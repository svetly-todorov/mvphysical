#include "common.h"
#include "map_scanner.h"
#include "sample.h"
#include "util.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <unordered_set>

struct Args {
  /* mask of cxl nodes */
  uint32_t far_node_mask = 0;
  bool IsInMask(uint32_t bit) {
    return far_node_mask & (1 << bit);
  }
};

void Usage(char **argv) {
  printf("\t%s [-c] FAR_NODE_0 ... FAR_NODE_N:\n\tperform and time a reverse pfn-to-vma mapping for PID\n\t-c: output timed values in csv format\n", argv[0]);
  exit(1);
}

struct Vma {
  uint64_t vma : 32;
  uint64_t pid : 23;
};

struct Mapping {
  uintptr_t pfn;
  struct Vma vm;
};

void reverse_map_job(std::vector<Mapping> &out, MapScanner &map_scanner, uint64_t pid) {
  auto task = [&out, pid](uintptr_t vma, uint64_t info, bool last) {
    /* Get the physical pfn, bits 0-54 */
    uint64_t pfn = (info & 0x003fffffffffffffULL);

    if (!pfn) return;

    out.push_back({pfn, {vma, pid}});
  };

  map_scanner.MapScan(task, pid);
}

void reverse_map(std::vector<pid_t> &pids, std::unordered_map<uintptr_t, Vma> &out) {
  /* For some reason it seems that using 1 worker is just about as fast as 8. 
   * Maybe my setup is wrong, or there's something wrong with my caching scheme. */
  const size_t kWorkers = 1;

  std::atomic_bool done[kWorkers];
  
  for (std::atomic_bool &b : done) b.store(true);

  std::thread workers[kWorkers];

  MapScanner map_scanner;

  /*
   * reserve enough slots for the mappings s/t we don't get real time
   * reallocations, seg faults
   */
  std::vector<std::vector<Mapping>> mappings;
  mappings.reserve(pids.size());

  for (size_t p = 0; p < pids.size(); ++p) mappings.push_back({});

  std::atomic_size_t idx = {0};

  for (pid_t pid : pids) {
    /* Scan over our jobs to see if any are available */
    for (size_t w = 0;;) {
      if (done[w].load()) {
        /* Set bool to active */
        done[w].store(false);

        /* Join the finished thread */
        if (workers[w].joinable()) workers[w].join();
        
        /* Boot up a new one */
        workers[w] = std::thread([&]() {
          std::vector<Mapping> mapping;
          reverse_map_job(mapping, map_scanner, pid);
          mappings[idx++] = std::move(mapping);
          done[w].store(true);
        });

        /* Move on to next entry of proc */
        break;
      }

      w = (w + 1) % kWorkers;

      /* Yield thread on loop to avoid busy wait */
      if (!w) sched_yield();
    }
  }

  /* Wait for all jobs to finish */
  for (size_t w = 0; w < kWorkers; ++w) {
    /* Join all threads */
    if (workers[w].joinable()) workers[w].join();
  }

  PRINT("merging %lu vectors into a single map:\n", mappings.size());

  /* Merge all vectors into the unordered_map */
  TIME(
    for (std::vector<Mapping> &mapping : mappings)
      for (Mapping &map : mapping)
        out[map.pfn] = map.vm
  );
}

/*
 * Parse over all of the leaf cgroups, reverse-mapping only those that contain
 * allocate a certain amount of space on N2 / N3.
 */
int main(int argc, char **argv) {
  struct Args args;

  const size_t kMb = 1024 * 1024;

  if (argc < 2) Usage(argv);

  /* Build mask of far nodes */
  for (int i = 1; i < argc; ++i) {
    int n = atoi(argv[i]);
    if (!n) Usage(argv);
    args.far_node_mask |= 1 << n;
  }

  /* 1. parse to find the leaf cgroups */
  
  std::unordered_set<std::string> leaves;

  for (const auto &entry : std::filesystem::recursive_directory_iterator("/sys/fs/cgroup")) {
    /* Add leaf cgroups when we encounter them in the parent dir */
    if (entry.is_directory()) {
      /* If current dir contains a directory, then it's not a leaf cgroup */
      leaves.erase(entry.path().parent_path());
      /* Add dir if it has a numa_stat */
      if (std::filesystem::exists(entry.path() / "memory.numa_stat")) leaves.emplace(entry.path());
    }
  }

  /* 2. get memory usage of cgroups. enqueue only pids in leaf groups with cxl usage */

  std::vector<pid_t> cxl_pids;

  for (const auto &dir : leaves) {
    std::ifstream cg(dir + "/memory.numa_stat");

    if (!cg.is_open()) ERROR(1, "ifstream open %s/memory.numa_stat\n", dir.c_str());

    std::string line;

    size_t bytes = 0;
    
    if (std::getline(cg, line)) {
      std::istringstream iss(line);
      std::string token, node;

      // PRINT("%s/memory.numa_stat:\n", dir.c_str());

      while (iss >> token) {
        node = "";
        
        if (token[0] != 'N') continue;

        // PRINT("\t%s\n", token.c_str());

        size_t i = 1;
        for (; i < token.size(); ++i) {
          if (token[i] < '0' || token[i] > '9') break;
          node += token[i];
        }
        
        if (args.IsInMask(strtoull(node.c_str(), nullptr, 10))) {
          node = token.substr(i + 1);
          size_t b = strtoull(node.c_str(), nullptr, 10);
          // PRINT(
          //   "\t+ %lu bytes (%lu mb)\n",
          //   b,
          //   b / kMb
          // );
          bytes += b;
        }
      }
    }

    /* Don't proceed if no far mem usage */
    if (bytes == 0) continue;

    cg.close();
    cg.open(dir + "/cgroup.procs");
    if (!cg.is_open()) ERROR(1, "ifstream open %s/cgroup.procs\n", dir.c_str());

    // BREAK();
    // PRINT("From %s (usage %lu b) add:\n", dir.c_str(), bytes);

    while (std::getline(cg, line)) {
      std::istringstream iss(line);
      std::string token;
      if (iss >> token) {
        cxl_pids.push_back(atoi(token.c_str()));
        // PRINT("\t%s\n", token.c_str());
      }
    }

    // BREAK();
  }

  BREAK();

  /* 3. reverse map the cxl PIDs */

  std::unordered_map<uintptr_t, Vma> map;
  
  PRINT("reverse mapping %lu pids:\n", cxl_pids.size());
  
  TIME(reverse_map(cxl_pids, map));

  PRINT(
    "Statistics:\n\t%lu addresses\n\tmap size %lu bytes (%lu mb)\n\ttotal farnode usage %lu mb\n",
    map.size(), 
    map.size() * 16ull,
    map.size() * 16ull / kMb,
    map.size() * 4096ull / kMb
  );
}