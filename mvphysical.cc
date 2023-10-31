#include <sys/types.h>
#include <dirent.h>
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <thread>

#include "common.h"
#include "map_scanner.h"
#include "sample.h"
#include "util.h"

#include "region.h"

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
      if (errno != ENXIO) ERROR(2, "unexpected errno %d\n", errno);

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
  if (ptr == MAP_FAILED) ERROR(3, "mmap failed errno %d\n", errno);

  return ptr;
}

class PageTable {
 public:
  PageTable(size_t max_pfn, void *ptr, size_t size) {
    ptr_ = ptr;
    size_ = size;
    max_pfn_ = max_pfn;
    info_ = reinterpret_cast<std::atomic_uint64_t *>(ptr);
    hits_ = reinterpret_cast<int *>((uintptr_t)ptr + max_pfn * sizeof(uint64_t));
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
    
    /* Increment in hit-array */
    ++hits_[pfn];
  }

  int GetHits(uintptr_t pfn) {
    if (pfn > max_pfn_) return -1;

    return hits_[pfn];
  }

  size_t GetMaxPfn() {
    return max_pfn_;
  }

  void Reset() {
    memset(ptr_, 0, size_);
  }
 
 private:
  void *ptr_ = nullptr;
  size_t size_ = 0;

  /* Page information, updated by map scanner */
  std::atomic_uint64_t *info_ = nullptr;
  /* Hits, updated by PEBS thread */
  int *hits_ = nullptr;

  size_t max_pfn_ = 0;

  union page_info {
    struct {
      uint64_t vma : 32;
      uint64_t pid : 23;
      uint64_t pebs : 1; /* Has this page ever been hit by pebs? */
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

      /*
       * Update the page table.
       * To reduce contention, DON'T touch the owner/vma bytes.
       * Just increment the hit counter.
       */
      uintptr_t pfn = sample->phys_addr >> 12;
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
  if (!proc) ERROR(6, "errno %d opendir /proc/\n", errno);

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

  if (errno != 0) ERROR(7, "errno %d readdir /proc/\n", errno);

  /* Wait for all jobs to finish */
  for (size_t w = 0; w < kWorkers; ++w) {
    /* Join all threads */
    if (workers[w].joinable()) workers[w].join();
  }

  closedir(proc);
}

void scan_page_table(PageTable &page_table) {
  /* Like reverse_map, scan over the page table using 16 worker threads. */
  const size_t kWorkers = 16;

  std::thread workers[kWorkers];

  /* Statistics need to be per-thread. Otherwise mad slowdown. */
  struct page_stat {
    int32_t hits = 0;
    int32_t populated = 0;
    uint64_t hit_at_all = 0;
  };
  struct page_stat stats[kWorkers];

  /* Number of pfns handled by each worker */
  size_t chunk = page_table.GetMaxPfn() / kWorkers;

  /* Spinning off the worker threads */
  for (size_t w = 0; w < kWorkers; ++w)
    workers[w] = std::thread([](size_t pfn, size_t count, PageTable *pt, page_stat *st) {
      /* Thread local page stat structure is so much faster */
      struct page_stat ls;

      for (; count > 0; ++pfn, --count) {
        int h = pt->GetHits(pfn); 
        if (h == -1) break;
        if (h >= 1) ++ls.hit_at_all;
        ls.hits += h;
        if (pt->GetPid(pfn) != 0) ++ls.populated;
      }

      /* Update the parent's page_stat array */
      st->hits = ls.hits;
      st->populated = ls.populated;
      st->hit_at_all = ls.hit_at_all;
    }, chunk * w, chunk, &page_table, &stats[w]);

  /* Join the jobs */
  for (std::thread &t : workers) t.join();

  /* Aggregate stats for printing */
  struct page_stat total;
  for (size_t w = 0; w < kWorkers; ++w) {
    total.hits += stats[w].hits;
    total.populated += stats[w].populated;
    total.hit_at_all += stats[w].hit_at_all;
  }

  PRINT("%lu of %lu pages (%lu%) are allocated\n", total.populated, page_table.GetMaxPfn(), total.populated * 100ull / page_table.GetMaxPfn());
  PRINT("%lu of %lu pages (%lu%) have at least /one/ PEBS hit\n", total.hit_at_all, page_table.GetMaxPfn(), total.hit_at_all * 100ull / page_table.GetMaxPfn());
  PRINT("%lu PEBS hits in the sample period\n", total.hits);
}

/*
 * Set the idle bitmap in cacheline sized chunks
 */
void set_idle(uintptr_t start_pfn, uintptr_t end_pfn) {
  if (end_pfn <= start_pfn) return;

  const size_t kBufSize = 16 * sizeof(uint64_t);

  int fd = open("/sys/kernel/mm/page_idle/bitmap", O_WRONLY);

  uint8_t buf[kBufSize];
  memset(buf, 0xff, kBufSize);

  uint64_t off = start_pfn / 64ull * 8ull;

  int rc = 0;
  do {
    off += rc;  
    if (off >= end_pfn / 64ull * 8ull) break;
    rc = pwrite(fd, buf, kBufSize, off);
  } while (rc > 0);

  if (rc <= 0) PRINT("pwrite to idle bitmap rc %d errno %d\n", rc, errno);

  PRINT("wrote %lu bytes to idle bitmap\n", off - start_pfn / 64ull * 8ull);

  close(fd);
}

/*
 * Read the idle bitmap and call the callback with memory regions -> start, end
 */
template <class CALLBACK>
void scan_idle(const CALLBACK &callback) {
  const size_t kBufSize = 8;
  uint64_t buf[kBufSize];
  const size_t kBufBytes = 8 * sizeof(uint64_t);

  /* Going to need a handle into kpageflags for thp */
  int fd = open("/sys/kernel/mm/page_idle/bitmap", O_RDONLY);
  int pf = open("/proc/kpageflags", O_RDONLY);
  if (fd < 0 || pf < 0) ERROR(1, "Couldn't open kpageflags (%d) or page_idle/bitmap (%d)\n", pf, fd);

  bool seek_end = false;
  bool huge_page = false;
  size_t start_pfn = 0, end_pfn = 0;

  size_t off = 0;
  size_t thp = 0;
  int rc = 0;
  do {
    off += rc;
    rc = pread(fd, buf, kBufBytes, off);

    size_t check = buf[0] | buf[1] | buf[2] | buf[3] | buf[4] | buf[5] | buf[6] | buf[7];
    if (!check) {
      if (seek_end) {
        end_pfn = (off * 8ull) - 1ull;
        seek_end = false;
        callback(start_pfn, end_pfn, huge_page);
      }
      continue;
    }

    /* state 1 find the first set bit */
    size_t j = 0, i = 0;
    if (!seek_end) {
      for (; j < kBufBytes; ++j) {
        for (; i < 64; ++i) {
          if ((buf[j] & (1ull << i)) == 0 /* 0 is not-idle */) {
            start_pfn = (off + j) * 8ull + i;
            
            /*
             * check if this pfn is the head of a huge page,
             * in which case we have to mark the remaining pages as not-idle.
             */
            uint64_t flags;
            rc = pread(pf, &flags, sizeof(uint64_t), start_pfn * sizeof(uint64_t));
            if (rc < 0) ERROR(1, "kpageflags read errno %d\n", errno);
            if (((flags >> 22) & 1) && ((flags >> 15) & 1)) {
              huge_page = true;
              thp = 512;
            } else {
              thp = 0;
              huge_page = false;
            }
            
            end_pfn = start_pfn;
            seek_end = true;
            break;
          }
        }
        if (seek_end) break;
      }
    }

    /* state 2 find the last contiguous 0 bit */
    if (seek_end) {
      for (; j < kBufBytes; ++j) {
        for (; i < 64; ++i) {
          if ((buf[j] & (1ull << i)) == 1 /* 1 is idle */) {
            if (thp && thp--) continue;
            end_pfn = ((off + j) * 8ull + i) - 1ull;
            seek_end = false;
            callback(start_pfn, end_pfn, huge_page);
            break;
          }
        }
        if (!seek_end) break;
      }
    }

  } while (rc > 0);
  /* wowza that's ugly !! */

  close(fd);
  close(pf);
}

int main(int argc, char **argv) {
  const size_t kGb = 1024ull * 1024ull * 1024ull;
  const size_t kMb = 1024ull * 1024ull;

  int idlefd = open_idle();
  
  /* 0 -> check if IDLE tracking is supported */
  if (idlefd == -1)
    ERROR(1, "IDLE tracking unsupported\n");

  /* 1 -> Get size of phys mem */
  size_t max_pfn;
  PRINT("getting max pfn\n");
  TIME(max_pfn = get_max_pfn(idlefd));
  PRINT("max_pfn is 0x%lx\n", max_pfn);
  PRINT("memory size is %lu GB\n", max_pfn * 4096ull / kGb);

  /* 1 -> allocate the page table */
  size_t size = max_pfn * (sizeof(uint64_t) + sizeof(int32_t));

  PRINT("allocating a page table, size %lu MB\n", size / kMb);

  PageTable page_table(max_pfn, mmap_table(size), size);

  TIME(page_table.Reset());

  /* 2 -> Set up PEBS */
  PRINT("setting up PEBS\n");

  sample::Event pebs_event;
  std::atomic_uint64_t sample_period = 5000;
  pebs_event.period_atomic = &sample_period;
  pebs_event.config = 0x20D1;
  pebs_event.dev = "cpu";
  pebs_event.name = "pebs";

  PRINT(".config: 0x%lx .sample_period: %lu\n", pebs_event.config, pebs_event.period_atomic->load());

  sample::Perf pebs(pebs_event);
  if (pebs.Open()) ERROR(5, "setup PEBS\n");
  
  PRINT("starting up PEBS thread\n");

  std::thread pebs_thread([&]() { read_pebs(&pebs, &page_table); });

  /* 2 -> Memory regions */
  Regions<Region> regions;

  /* 2 -> Set IDLE bit */
  PRINT("setting all IDLE bits to 1. May take some time...\n");
  
  TIME(set_idle(0, max_pfn));

  /* MapScanner for getting the numa of each phys-addr */
  MapScanner map_scanner;

  /* Some statistics */
  struct {
    /* regions per node */
    uint64_t regions[8] = {0};
    /* usage per node */
    uint64_t usage[8] = {0};
  } stats;

  /* 2 -> Scan IDLE map and define memory regions */
  scan_idle([&](uintptr_t start, uintptr_t end, bool huge_page) {
    uint64_t node = map_scanner.GetNumaOfPfn(start);
    PRINT("[0x%lx - 0x%lx] N%lu %s\n", start, end, node, huge_page ? "HUGE PAGE" : "");
    stats.regions[node] += 1;
    stats.usage[node] += (end - start) * 4096ull;
  });
  // scan_idle([&](uintptr_t start, uintptr_t end) { regions.Insert(start, {start, end, 0}); });
  
  /* print stats */
  for (size_t i = 0; i < 8; ++i) {
    if (!stats.regions[i]) continue;
    PRINT("Node %lu:\n\tusage %lu mb\n\t%lu regions\n", i, stats.usage[i] / kMb, stats.regions[i]);
  }

  /* 3 -> main loop */
  PRINT("setup done. starting main loop\n");
  for (;;) {
    PRINT("do reverse mapping\n");
    TIME(reverse_map(page_table));
    
    PRINT("scan page table\n");
    TIME(scan_page_table(page_table));

    // PRINT("resetting page table\n");
    // TIME(page_table.Reset());
  }
  return 0;
}