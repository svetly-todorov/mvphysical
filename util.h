#ifndef UTIL_H
#define UTIL_H

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

#include <vector>

namespace util {

template <typename T>
bool PopVectorIndex(std::vector<T> &vec, size_t i) {
  if (!vec.size()) return false;
  if (i >= vec.size()) return false;
  vec[i] = vec[vec.size() - 1];
  vec.pop_back();
  return true;
}

template <typename T>
bool PopVectorElement(std::vector<T> &vec, T &t) {
  if (!vec.size()) return false;

  for (size_t s = 0; s < vec.size(); ++s)
    if (vec[s] == t)
      return PopVectorIndex(vec, s);
  
  return false;
}

template <typename T>
bool PopVectorPointer(std::vector<T> &vec, T &t) {
  if (!vec.size()) return false;

  for (size_t s = 0; s < vec.size(); ++s)
    if (*vec[s] == *t)
      return PopVectorIndex(vec, s);
  
  return false;
}

/* Utility functions for clock_gettime -> milliseconds */
inline uint64_t GetTimeMs(clockid_t clk_id = CLOCK_MONOTONIC) {
  struct timespec ts;
  clock_gettime(clk_id, &ts);
  return ts.tv_nsec / 1000000ull + ts.tv_sec * 1000ull;
}

inline uint64_t GetNumMs(uint64_t since) {
  return GetTimeMs() - since;
}

inline uint64_t GetNumMsSinceEpoch() {
  return GetTimeMs(CLOCK_REALTIME);
}

int perf_event_open(const struct perf_event_attr *event, pid_t pid, int cpu, int group_fd = -1, uint64_t flags = 0) {
  return syscall(SYS_perf_event_open, event, pid, cpu, group_fd, flags);
}

/* Round up u32 to nearest power of two.
 * see https://graphics.stanford.edu/%7Eseander/bithacks.html#RoundUpPowerOf2 */
uint32_t RoundUp(uint32_t v) {
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  return ++v;
}

long GetVendor() {
  /* Get vendor from the host bridge, device 0000:00... */
  int fd = open("/sys/devices/pci0000:00/0000:00:00.0/vendor", O_RDONLY);
  if (fd == -1) return -1;

  char v[8];
  int rc = read(fd, v, sizeof(v));
  close(fd);

  if (rc == -1) return -1;
  return strtol(v + 2, nullptr, 16);
}

long GetModel() {
  /* Get cpu model from /proc/cpuinfo */
  FILE *fp = fopen("/proc/cpuinfo", "r");
  if (!fp) return -1;

  /* model number */
  long model = -1;

  char skip[256];

  /* Scan until we hit "model : num" */
  int rc = EOF;
  for (;;) {
    rc = fscanf(fp, " model : %ld ", &model);
    if (rc == 0) rc = fscanf(fp, "%s", skip);
    if (rc == EOF) break;
    if (model != -1) break;
  }

  fclose(fp);
  return model;
}

int GetNodeCpu(long node) {
  char buf[128];
  int rc, cpu;

  snprintf(buf, sizeof(buf), "/sys/devices/system/node/node%ld/cpulist", node);
  FILE *fp = fopen(buf, "r");
  if (!fp) {
    return -1;
  }

  rc = fscanf(fp, "%d", &cpu);
  if (rc == EOF) {
    fclose(fp);
    return -1;
  }

  fclose(fp);

  return cpu;
}

/* Increases rlimit for # of fds; returns nonzero on fail */
int MoreFds() {
  struct rlimit rlim;

  /* Get current rlimit */
  int rc = getrlimit(RLIMIT_NOFILE, &rlim);
  if (!rc) return rc;

  /* Increase soft limit of fds to the max */
  rlim.rlim_cur = rlim.rlim_max;
  rc = setrlimit(RLIMIT_NOFILE, &rlim);
  if (!rc) return rc;

  /* Successfully increased the fd limit; return */
  return 0;
}

struct SysDev {
  uint64_t type = 4ull;
  uint64_t cpu =  ~0ull;
};

long GetNode(long cpu) {
  char buf[128];
  const long kMaxNode = 256;
  int fd;

  for (long n = 0; n < kMaxNode; ++n) {
    snprintf(buf, sizeof(buf), "/sys/devices/system/cpu/cpu%ld/node%ld", cpu, n);
    fd = open(buf, O_RDONLY);
    if (fd >= 0) close(fd);
    if (fd >= 0) return n;
  }

  return -1;  
}

template <typename ADD>
int GetDevicesUncore(const char *device, int node, int &type, const ADD& add) {
  if (!device) return -1;

  int cpu = -1;
  char devt[128], devc[128];
  const char *c = device, *p = device;
  while (*c != '\0') {
    p = c;
    ++c;
  }

  FILE *fp;

  for (int idx = 0;; ++idx) {
    /* `device` ends in underscore. Treat as wildcard, and we must parse over
     * /sys/devices to find all matches */
    if (*p == '_') {
      snprintf(devt, sizeof(devt), "/sys/devices/%s%d/type", device, idx);
      snprintf(devc, sizeof(devc), "/sys/devices/%s%d/cpumask", device, idx);
    } else {
      snprintf(devt, sizeof(devt), "/sys/devices/%s/type", device);
      snprintf(devc, sizeof(devc), "/sys/devices/%s/cpumask", device);
    }

    SysDev device_data;

    /* Parse /type to get the device type */
    fp = fopen(devt, "r");
    if (!fp) {
      if (idx == 0) return errno;
      if (errno == ENOENT) break;
    }

    int rc = fscanf(fp, "%d", &type);
    if (rc != 1) {
      fclose(fp);
      return errno;
    }
    fclose(fp);

    device_data.type = type;

    /* Parse /cpumask to find the CPU to pass to perf for this node */
    fp = fopen(devc, "r");
    if (!fp) {
      rc = errno;
      break;
    }

    int i;
    char delim;
    for (i = 0; i <= node; ++i) {
      /* Scan for next cpu */
      rc = fscanf(fp, "%d", &cpu);
      if (rc != 1) {
        rc = ENOENT;
        break;
      }
      
      if (i == node) break;
      
      /* Scan for delimiting character (in /cpumask, a comma) */
      rc = fscanf(fp, "%c", &delim);
      if (rc == 0 || rc == EOF) break;
      if (delim != ',') {
        rc = EINVAL;
        break;
      }
    }

    fclose(fp);

    if (i != node) rc = ENOENT;

    if (cpu != -1) device_data.cpu = cpu; 

    add(device_data);
    
    if (*p != '_') break;
  }

  return 0;
}

template <typename ADD>
int GetDevicesCore(int node, const ADD& add) {
  int max_cpu = sysconf(_SC_NPROCESSORS_CONF);

  for (int c = 0; c < max_cpu; ++c) {
    if (GetNode(c) == node) {
      SysDev dev;
      dev.type = 4;
      dev.cpu = c;
      add(dev);
    }
  }

  return 0;
}

int GetCurrentDirectory(char *buf, size_t size, char term = '\0') {
  ssize_t rc = readlink("/proc/self/cwd", buf, size);
  if (rc == -1) return errno;
  buf[rc] = term;
  return 0;
}

int GetHomeDirectory(char *buf, size_t size, char term = '\0') {
  size_t rc = snprintf(buf, size, "%s", getenv("HOME"));
  // while (*buf != '\n') ++buf;
  buf[rc] = term;
  return 0;
}

/* translate from relative path to full */
int GetAbsolutePath(char *out, size_t size, const char *path) {
  const size_t kFileMax = 256;
  const char *p = path;
  char name[kFileMax], buf[PATH_MAX];
  char *n = name, *b = buf, *e = buf + PATH_MAX;

  /* If the path is already absolute, then copy directly into output */
  if (*p == '/') {
    snprintf(out, size, "%s", path);
    return 0;
  }

  /* Initialize buf with the cwd */
  int rc = GetCurrentDirectory(buf, sizeof(buf), '\0');
  if (rc) return rc;

  /* Seek to last character, replace with delimiter */
  while (*b) ++b;
  *b = '/';
  
  /* Next part of path */
  ++b;

  /* Copy next portion of the filename into `name' */
  for (;;) {
    *n = *p;

    /* End of path component */
    if (*p == '/' || *p == '\0') {

      /* Replace end of name with \0 to allow strcmp */
      *n = '\0';

      /* '..' : Regress output cursor to parent */
      if (!strcmp(name, "..")) {
        --b;                    // move b back to the '/'
        if (b > buf) --b;       // if this isn't the root '/', go back one more time
        while (*b != '/') --b;  // loop till previous '/'
        ++b;
      }
      
      /* '~' : Copy home dir to start of buffer */
      else if (!strcmp(name, "~")) {
        if (p - 1 == path) {
          //  Overwrite buf with getcwd(HOME)
          rc = GetHomeDirectory(buf, sizeof(buf), '\0');
          if (rc) return rc;
          
          // Reset b as above
          b = buf;
          while (*b) ++b;
          *b = '/';
          ++b;
        }
      }
      
      /* Anything else but '.' : copy component name into buffer */
      else if (strcmp(name, ".")) {
        // Copy name into buffer at b
        snprintf(b, e - b, "%s", name);

        // Seek to end of name
        while (*b) ++b;
        *b = '/';
        ++b;
      }
      
      n = name;
      if (*p) {
        ++p;
        continue;
      }
    }

    /* end of path */
    if (*p == '\0') {
      --b;
      *b = '\0';
      snprintf(out, size, "%s", buf);
      return 0;
    }

    ++p;
    ++n;
  }

  return -1;
}

/* Gets the inode associated with a file */
int GetInode(ino_t &ino, const char *path, bool is_shm) {
  int fd;

  if (!is_shm) {
    fd = open(path, O_RDONLY);
    if (fd == -1) return errno;
  } else {
    fd = shm_open(path, O_RDONLY, S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
    if (fd == -1) return errno;
  }
  
  struct stat filestat;

  int rc = fstat(fd, &filestat);
  if (rc == -1) {
    close(fd);
    return errno;
  }

  close(fd);

  ino = filestat.st_ino;
  
  return 0;
}

/* Open and map an shm */
void *MapShm(const char *name, size_t size, int flags = 0) {
  int fd = shm_open(name, flags | O_RDWR,
                    S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);
  if (fd == -1) return nullptr;

  /* if newly created, ftruncate the file to allocate space */
  if (flags & (O_CREAT | O_TRUNC)) {
    if (ftruncate(fd, size)) {
      close(fd);
      return nullptr;
    }
  }

  /* Map-in the files to get void* for the ring buffers */
  void *ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (ptr == MAP_FAILED) {
    close(fd);
    return nullptr;
  }

  return ptr;
}

} // namespace util

#endif