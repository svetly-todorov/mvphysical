#include "common.h"
#include "map_scanner.h"
#include "sample.h"
#include "util.h"

#include <unordered_map>

struct Args {
  /* pid of the process to reverse-map */
  pid_t pid;
};

void Usage(char **argv) {
  printf("\t%s [PID]:\n\tperform and time a reverse pfn-to-vma mapping for PID\n", argv[0]);
  exit(1);
}

/*
 * Time how long it takes to perform a reverse mapping
 * from phys memory to virtual.
 */
int main(int argc, char **argv) {
  struct Args args;

  if (argc < 2) Usage(argv);

  args.pid = atoi(argv[1]);

  PRINT("Performing reverse mapping on %d:\n", args.pid);

  /* 4. reverse-map, saving the entries in an unordered_map */
  std::unordered_map<uintptr_t, uintptr_t> map;
  
  auto rev = [&](uintptr_t vma, uint64_t info, bool last) {
    /* Get the physical pfn, bits 0-54 */
    uint64_t pfn = (info & 0x003fffffffffffffull);

    if (!pfn) return;

    map[pfn] = vma;
  };

  MapScanner map_scanner;

  TIME(map_scanner.MapScan(rev, args.pid));

  const size_t kMb = 1024 * 1024;

  PRINT(
    "Statistics:\n\t%lu addresses\n\tmap size %lu bytes (%lu mb)\n\tpid %d memusage %lu mb\n",
    map.size(), 
    map.size() * 16ull,
    map.size() * 16ull / kMb,
    args.pid,
    map.size() * 4096ull / kMb
  );
}