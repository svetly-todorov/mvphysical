#ifndef MAP_SCANNER_H
#define MAP_SCANNER_H

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>  // C-style file IO is faster than C++ style
#include <sys/mman.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>

class MapScanner {
 public:
  MapScanner();
  ~MapScanner();
  int InitPfnMap();

  int GetNumaMax();
  int GetNumaOfPfn(uintptr_t pfn);
  int GetNumaOfPfn(uintptr_t *pfns, int *nodes, size_t len);

  template <class CALLBACK>
  void MapScan(const CALLBACK &process_u64, int pid, bool ignore_swapped = true);

 private:
  static const int kMaxNodes = 16;
  struct NumaRange {
    uintptr_t start = ~0ULL;
    uintptr_t end = 0ULL;
  };
  NumaRange numa_[kMaxNodes];

  uintptr_t start_pfn_[kMaxNodes];
  int max_node_ = -1;
};

MapScanner::MapScanner() {
  InitPfnMap();
}

MapScanner::~MapScanner() {}

int MapScanner::GetNumaMax() { return max_node_; }

template <class CALLBACK>
void MapScanner::MapScan(const CALLBACK &process_u64, int pid, bool ignore_swapped) {
  const uint64_t kPageSize = 4096;
  const uint64_t kBufSize = 1024 * 1024 * 64;
  const uint64_t kBuf64 = kBufSize / sizeof(uint64_t);

  void *buf = mmap(nullptr, kBufSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (buf == MAP_FAILED) return;

  uint64_t *page_map_buf = reinterpret_cast<uint64_t *>(buf);

  /* Does this work? */
  std::unique_ptr<void *, void (*)(void **)> del(&buf, [](void **p){ munmap(*p, 1024 * 1024 * 64); });

  /* line will hold name of file, but later will also be used for parsing */
  char line[256];
  snprintf(line, sizeof(line), "/proc/%d/maps", pid);

  /* mp == "map pointer" */
  FILE *mp = fopen(line, "r");
  if (!mp) return;

  /* pp == "pagemap pointer" */
  snprintf(line, sizeof(line), "/proc/%d/pagemap", pid);
  FILE *pp = fopen(line, "r");
  if (!pp) return;

  /* while not EOF... */
  while (fgets(line, sizeof(line), mp)) {
    /* maps gives us VMAs; we parse over these to get subranges on each NUMA */
    uintptr_t vma_start, vma_end;
    /* Each line in maps is of form 55cca000-55dca000; sscanf to get the addrs */
    sscanf(line, "%lx-%lx", &vma_start, &vma_end);
    /* Seek to the corresponding entry in proc/pid/pagemap */
    fseek(pp, vma_start / kPageSize * sizeof(uint64_t), SEEK_SET);
    uint64_t remain = (vma_end - vma_start) / kPageSize;
    uint64_t count = 0;

    while (remain > 0) {
      /* Read from pagemap. Gives us a bunch of _u64's */
      uint64_t am = (remain > kBuf64 ? kBuf64 : remain);
      uint64_t el = fread(page_map_buf, sizeof(uint64_t), am, pp);

      /* Now parse over each _u64 in this range. */
      for (am = 0; am < el; ++am) {
        uint64_t info = page_map_buf[am];
        uintptr_t vaddr = vma_start + (count++) * kPageSize;
        /* Ignore this u64 if its page is swapped out or not present. */
        bool present = (info >> 63) & 1ull; 
        bool swapped = (info >> 62) & 1ull;
        if (ignore_swapped && (!present || swapped)) continue;
        process_u64(vaddr, info, vaddr == (vma_end - kPageSize));
      }
      if (el == 0) break;
      remain -= el;
    }
  }

  fclose(mp);
  fclose(pp);
}

int MapScanner::InitPfnMap() {
  char line[256];
  int node = -1;
  char type[64];

  max_node_ = -1;

  unsigned long spanned;
  unsigned long start_pfn;
  unsigned long lengths[16] = {0};
  bool ignore = false;

  FILE *zi = fopen("/proc/zoneinfo", "r");
  if (!zi) return -1;

  while (fgets(line, sizeof(line), zi)) {
    int rc = sscanf(line, "Node %d, zone %s", &node, type);
    if (rc == 2) {
      ignore = false;
      if (!strcmp(type, "Device")) ignore = true;
      if (node > max_node_) max_node_ = node;
      continue;
    }
    /* Do not count pages in ZONE_DEVICE */
    if (ignore) continue;
    rc = sscanf(line, " spanned %lu", &spanned);
    if (rc == 1) {
      lengths[node] += spanned;
      continue;
    }
    rc = sscanf(line, " start_pfn: %lu", &start_pfn);
    if (rc == 1) {
      if (start_pfn < numa_[node].start) numa_[node].start = start_pfn;
      continue;
    }
  }

  for (int n = 0; n <= max_node_; ++n)
    numa_[n].end = numa_[n].start + lengths[n];

  fclose(zi);
  return max_node_;
}

inline int MapScanner::GetNumaOfPfn(uintptr_t pfn) {
  int res = 0;
  int max = (max_node_ + 3) & ~0x03;
  for (int n = 0; n < max; n += 4) {
    int n0 = ((int64_t)((pfn < numa_[n + 0].end) && (numa_[n + 0].start < pfn)) << 63) >> 63 & (n + 0);
    int n1 = ((int64_t)((pfn < numa_[n + 1].end) && (numa_[n + 1].start < pfn)) << 63) >> 63 & (n + 1);
    int n2 = ((int64_t)((pfn < numa_[n + 2].end) && (numa_[n + 2].start < pfn)) << 63) >> 63 & (n + 2);
    int n3 = ((int64_t)((pfn < numa_[n + 3].end) && (numa_[n + 3].start < pfn)) << 63) >> 63 & (n + 3);
    res |= n0 | n1 | n2 | n3;
  }
  return res;
}

int MapScanner::GetNumaOfPfn(uintptr_t *pfns, int *nodes, size_t len) {
  for (size_t l = 0; l < len; ++l) nodes[l] = GetNumaOfPfn(pfns[l]);
  return len;
}

#endif