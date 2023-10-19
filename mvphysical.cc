#include <sys/types.h>
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <memory>

#include "common.h"
#include "map_scanner.h"
#include "sample.h"
#include "util.h"

void Usage(char **argv) {
  exit(1);
}

int open_idle() {
  return open("/sys/kernel/mm/page_idle/bitmap", O_RDWR);
}

/*
 * Binary search across the pfn space to find the highest
 * pfn for which /page_idle/bitmap doesn't return ENXIO.
 */
size_t get_max_pfn(int idlefd) {
  /* Start increment at 4GB intervals */
  size_t add = 4ull * 1024ull * 1024ull * 1024ull / 4096ull / 64ull;
  size_t off = 0;

  for (;;) {
    uint64_t buf = ~0ull;

    int rc = pwrite(idlefd, &buf, sizeof(buf), off * sizeof(uint64_t));

    if ((size_t)rc == sizeof(buf)) {
      off += add;
      continue;
    }

    if (rc == -1) {
      if (errno != ENXIO) error(2, "%s: unexpected errno %d\n", __func__, errno);

      /* If add == 1 then we're done with the search */
      off -= add;
      if (add == 1) return off * 64ull; 
      
      add = add >> 1;
      continue;
    }
  }
}

void *mmap_table(size_t bytes) {
  void *ptr = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
  if (ptr == MAP_FAILED) error(3, "%s: mmap failed errno %d\n", __func__, errno);

  return ptr;
}

class PageTable {
 public:
  PageTable(void *ptr, size_t size) {
    ptr_ = ptr;
    info_ = reinterpret_cast<std::atomic_uint64_t *>(ptr);
    max_pfn_ = size / sizeof(uint64_t);
    size_ = size;
  }

  void SetPid(uintptr_t pfn, pid_t pid) {
    if (pfn > max_pfn_) return;

    page_info info;
    info.pid = pid;

    page_info mask;
    mask.data = ~0ull;
    mask.pid = 0;
    info_[pfn] &= mask.data;

    info_[pfn] |= info.data;
  }

  pid_t GetPid(uintptr_t pfn) {
    if (pfn > max_pfn_) return -1;

    return page_info(info_[pfn]).pid;
  }

  void SetVirtualAddr(uintptr_t pfn, uintptr_t vma) {
    if (pfn > max_pfn_) return;

    page_info info;
    info.vma = (vma >> 12);

    page_info mask;
    mask.data = ~0ull;
    mask.vma = 0;
    info_[pfn] &= mask.data;

    info_[pfn] |= info.data;
  }

  uintptr_t GetVirtualAddr(uintptr_t pfn) {
    if (pfn > max_pfn_) return -1;

    return page_info(info_[pfn]).vma;
  }

  void PageHit(uintptr_t pfn) {
    if (pfn > max_pfn_) return;

    page_info info;
    info.count = page_info(info_[pfn]).count;
    if (info.count < 255) ++info.count;

    page_info mask;
    mask.data = ~0ull;
    mask.count = 0;
    info_[pfn] &= mask.data;

    info_[pfn] |= info.data;
  }

  ssize_t GetHits(uintptr_t pfn) {
    if (pfn > max_pfn_) return -1;

    return page_info(info_[pfn]).count;
  }

  void Reset() {
    memset(ptr_, 0, size_);
  }
 
 private:
  void *ptr_ = nullptr;
  size_t size_ = 0;

  std::atomic_uint64_t *info_ = nullptr;
  size_t max_pfn_ = 0;

  union page_info {
    struct {
      uint64_t vma : 32;
      uint64_t pid : 23;
      uint64_t thp : 1;
      uint64_t count : 8;
    };
    uint64_t data;

    page_info() { data = 0ull; }
    page_info(uint64_t val) { data = val; }
  };
};

void read_pebs(sample::Perf *handler, PageTable *page_table) {
  auto reader = [&](
    uintptr_t off, 
    uintptr_t size,
    uintptr_t head, 
    unsigned long long &tail
  ) {
    while (tail < head) {
      uintptr_t addr = off + (tail % size);

      perf_event_header *header = reinterpret_cast<perf_event_header *>(addr);
      tail += header->size;

      if (header->type != PERF_RECORD_SAMPLE) continue;

      if (addr + sizeof(sample::perf_record) > off + size) continue;

      sample::perf_record *sample = reinterpret_cast<sample::perf_record *>(addr);

      /* Update the page table */
      uintptr_t pfn = sample->phys_addr >> 12;
      page_table->SetPid(pfn, sample->pid);
      page_table->SetVirtualAddr(pfn, sample->addr);
      page_table->PageHit(pfn);
    }

    return false;
  };
  
  auto breaker = [](){
    return false;
  };

  for (;;) 
    if (handler->Read(0, reader, breaker) == ~0ull)
      sched_yield();
}


void reverse_map_job(PageTable &page_table, MapScanner &map_scanner, pid_t pid) {
  auto task = [&page_table, pid](uintptr_t vma, uint64_t info, bool last) {
    /* Get the physical pfn, bits 0-54 */
    uint64_t pfn = (info & 0x003fffffffffffffULL);

    if (!pfn) return;

    page_table.SetPid(pfn, pid);
    page_table.SetVirtualAddr(pfn, vma);
  };

  map_scanner.MapScan(task, pid);
}

void reverse_map(PageTable &page_table) {
  /*
   * The plan: boot up 16 threads and scan over all of the subdirectories in /proc/.
   * For each, do a MapScan and update the page_table with the appropriate owner-pid
   */
  const size_t kWorkers = 16;

  std::atomic_bool done[kWorkers];
  
  for (std::atomic_bool &b : done) b.store(true);

  std::thread workers[kWorkers];

  DIR *proc = opendir("/proc/");
  if (!proc) error(6, "%s: errno %d opendir /proc/\n", __func__, errno);

  MapScanner map_scanner;
  
  errno = 0;
  
  struct dirent *dir = nullptr;
  
  while ((dir = readdir(proc))) {
    /* Skip non-numeric entries of proc */
    if (dir->d_name[0] < '0' || dir->d_name[0] > '9') continue;
    
    pid_t pid = atoi(dir->d_name);

    /* Scan over our jobs to see if any are available */
    for (size_t w = 0;;) {
      if (done[w].load()) {
        /* Set bool to active */
        done[w].store(false);

        /* Join the finished thread */
        if (workers[w].joinable()) workers[w].join();
        
        /* Boot up a new one */
        workers[w] = std::thread([&]() {
          reverse_map_job(page_table, map_scanner, pid);
          done[w].store(true);
        });

        /* Move on to next entry of proc */
        break;
      }

      w = (w + 1) % kWorkers;

      /* Yield thread on loop to avoid busy wait */
      if (!w) sched_yield();
    }

    /* Reset errno in case it was changed during reverse_map_job */
    errno = 0;
  }

  if (errno != 0) error(7, "%s: errno %d readdir /proc/\n", __func__, errno);

  /* Wait for all jobs to finish */
  for (size_t w = 0; w < kWorkers; ++w) {
    /* Join all threads */
    if (workers[w].joinable()) workers[w].join();
  }

  closedir(proc);
}

void scan_page_table(PageTable &page_table) {
  size_t populated = 0, total = 0;
  for (size_t pfn = 0;; ++pfn) {
    ++total;
    if (page_table.GetHits(pfn) == -1) break;
    if (page_table.GetPid(pfn) != 0) ++populated;
  }

  print("%s: ... ... %lu of %lu (%lu%) pages are populated\n", __func__, populated, total, populated * 100ull / total);
}

int main(int argc, char **argv) {
  const size_t kGb = 1024ull * 1024ull * 1024ull;
  const size_t kMb = 1024ull * 1024ull;

  int idlefd = open_idle();

  size_t time_ms = 0;
  
  /* 0 -> check if IDLE tracking is supported */
  if (idlefd == -1)
    error(1, "%s: IDLE tracking unsupported\n", __func__);

  print("%s: performing setup\n", __func__);

  /* 1 -> Get size of phys mem */
  time_ms = util::GetTimeMs();
  size_t max_pfn = get_max_pfn(idlefd);
  print("%s: ... max_pfn is 0x%lx\n", __func__, max_pfn);
  print("%s: ... ... [it took %lu ms to get max_pfn] \n", __func__, util::GetNumMs(time_ms));
  print("%s: ... ... memory size is %lu GB\n", __func__, max_pfn * 4096ull / kGb);

  /* 1 -> allocate the page table */
  size_t size = max_pfn * sizeof(uint64_t);

  print("%s: ... allocating a page table, size %lu MB\n", __func__, size / kMb);

  PageTable page_table(mmap_table(size), size);

  time_ms = util::GetTimeMs();
  page_table.Reset();
  print("%s: ... [it took %lu ms to clear the page table]\n", __func__, util::GetNumMs(time_ms));

  /* 2 -> Set up PEBS */
  print("%s: ... setting up PEBS\n", __func__);

  sample::Event pebs_event;
  std::atomic_uint64_t sample_period = 10000;
  pebs_event.period_atomic = &sample_period;
  pebs_event.config = 0x20D1;
  pebs_event.dev = "cpu";
  pebs_event.name = "pebs";

  sample::Perf pebs(pebs_event);
  if (pebs.Open()) error(5, "%s: setup PEBS\n", __func__);
  
  print("%s: ... starting up PEBS thread\n", __func__);

  // std::thread pebs_thread([&]() { read_pebs(&pebs, &page_table); });

  /* 3 -> main loop */
  print("%s: setup done. starting main loop\n", __func__);
  for (;;) {
    time_ms = util::GetTimeMs();
    print("%s: ... do reverse mapping\n", __func__);
    reverse_map(page_table);
    // std::this_thread::sleep_for(std::chrono::seconds(5));
    print("%s: ... [it took %lu ms to get a full reverse mapping]\n", __func__, util::GetNumMs(time_ms));

    /* Time how long it takes for us to scan over the page map */

    time_ms = util::GetTimeMs();
    print("%s: ... scan page table\n", __func__);
    scan_page_table(page_table);
    print("%s: ... [it took %lu ms to scan page table]\n", __func__, util::GetNumMs(time_ms));
  }
  return 0;
}