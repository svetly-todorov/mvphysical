#ifndef SAMPLES_H
#define SAMPLES_H

#include <errno.h>
#include <numa.h>
#include <linux/hw_breakpoint.h>
#include <linux/perf_event.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h> /* Definition of SYS_* constants */
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <memory>
#include <vector>

#include "common.h"
#include "util.h"

namespace sample {

static struct perf_event_attr pebs_attr = {
    .type = PERF_TYPE_RAW,  // get type from documentation
    .size = sizeof(struct perf_event_attr),
    .config = 0x20D1,
    .sample_period = 125,  // record one out of every sample_period_ events
    .sample_type = PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU | PERF_SAMPLE_WEIGHT,
    .disabled = 0,                  // 0 means enable the event
    .exclude_kernel = 1,            // don't count kernel events
    .exclude_hv = 1,                // don't count hypervisor
    .freq = 0,                      // use period, not frequency
    .precise_ip = 1,                // if this is 0, we don't get addr tracing.
    .exclude_callchain_kernel = 1,  // don't record kernel stack trace
    .exclude_callchain_user = 1     // don't record user stack trace
};
static struct perf_event_attr ibs_attr = {
  .type = 0xff,
  .size = sizeof(struct perf_event_attr),
    // Setting bit 19 in .config enables sampling every sample_period ops.
    // Leaving it unset will take an IBS sample every sample_period cycles
  .config        = (1ULL << 19),
  .sample_period = 2500000, // Can be any value > 0x10
  .sample_type   = PERF_SAMPLE_TID | PERF_SAMPLE_ADDR | PERF_SAMPLE_PHYS_ADDR | PERF_SAMPLE_TIME | PERF_SAMPLE_CPU,
  .disabled      = 0,
  .inherit       = 1,
  .precise_ip    = 2,
  .sample_id_all = 1,
};
struct perf_record {
  perf_event_header header;
  uint32_t pid, tid;
  uint64_t time;
  uint64_t addr;
  uint32_t cpu, res;
  uint64_t weight;
  uint64_t phys_addr;
};

/* Abstraction of a perf_event_attr */
struct Event {
  std::atomic_uint64_t *period_atomic = nullptr;
  uint64_t period_actual = 0;
  uint64_t config = 0;
  uint64_t config1 = 0;
  const char *dev = nullptr;
  const char *name = nullptr;
};

/* Responsible for tracking sample-event across the entire system */
class Perf {
 public:
  Perf(Event event);
  Perf(Perf&& o) noexcept :
    mmaps_(std::move(o.mmaps_)),
    fds_(std::move(o.fds_))
  {
    print("perf: move constructor\n");
    snprintf(name_, sizeof(name_), "%s", o.name_);
  }
  ~Perf();

  int Open();
  void Close();

  template <typename CALLBACK, typename BREAKOUT>
  size_t Read(size_t start, const CALLBACK &reader, const BREAKOUT &breakout);
 private:
  Event event_;
  char name_[64];

  struct Map {
    static const size_t kSize = (1 + (1 << 6)) * 4096;
    void *ptr = nullptr;
    size_t size = kSize;
  };
  std::vector<Map> mmaps_;

  std::vector<int> fds_;
};

Perf::Perf(Event event) {
  event_ = event;
  snprintf(name_, sizeof(name_), "%s", event.name);  
}
Perf::~Perf() { Close(); }

void Perf::Close() {
  print("perf: %s closing %lu fds, %lu unmaps\n", event_.name, fds_.size(), mmaps_.size());
  for (int &f : fds_) close(f);
  for (Map &m : mmaps_) munmap(m.ptr, m.size);
  fds_.clear();
  mmaps_.clear();
}

int Perf::Open() {
  perf_event_attr attr = pebs_attr;

  /* Use different template when the event is ibs */
  if (event_.dev && !strcmp(event_.dev, "ibs_op")) attr = ibs_attr;

  attr.sample_period = event_.period_actual = event_.period_atomic->load();
  attr.config = event_.config;
  attr.config1 = event_.config1;

  /* if device, get the type */
  int type = 4;
  if (event_.dev) util::GetDevicesUncore(event_.dev, 0, type, [](util::SysDev dev) {});

  int rc = 0, fd = -1;

  /* open on all cpus */
  for (int node = numa_max_node(); node >= 0; --node) {
    if (util::GetNodeCpu(node) == -1) continue;
    util::GetDevicesCore(
      node,
      [&](util::SysDev d) {
        if (rc) return;

        attr.type = type;

        fd = util::perf_event_open(&attr, -1, d.cpu, -1, 0);
        if (fd == -1) {
          print("precise: open %s cpu %d type %d errno %d\n", event_.name, d.cpu, type, errno);
          Close();
          rc = errno;
          return;
        }

        fds_.push_back(fd);
      }
    );
  }

  if (rc) return rc;

  /* mmap sample buffers */
  for (int f : fds_) {
    Map m;
    m.ptr = mmap(nullptr, m.size, PROT_READ | PROT_WRITE, MAP_SHARED, f, 0);
    if (m.ptr == MAP_FAILED) {
      print("precise: map %s fd %d errno %d\n", event_.name, f, errno);
      Close();
      return errno;
    }
    mmaps_.push_back(m);
  }

  return 0;
}

template <typename CALLBACK, typename BREAKOUT>
size_t Perf::Read(size_t start, const CALLBACK &reader, const BREAKOUT &breakout) {
  for (; start < mmaps_.size(); ++start) {
    const Map &m = mmaps_[start];

    /* ioctl sinks performance, update only if necessary */
    if (event_.period_actual != event_.period_atomic->load()) {
      event_.period_actual = event_.period_atomic->load();
      ioctl(fds_[start], PERF_EVENT_IOC_PERIOD, &event_.period_actual);
    }

    perf_event_mmap_page *metadata;

    uintptr_t offset;

    metadata = reinterpret_cast<perf_event_mmap_page *>(m.ptr);
    offset = (uintptr_t)m.ptr + metadata->data_offset;

    while (metadata->data_head != metadata->data_tail) {
      bool rb = reader(offset, metadata->data_size, metadata->data_head, metadata->data_tail);
      if (rb) return start;
    }

    /* producer explicitly wants us to exit */
    if (breakout()) return start;
  }

  return ~0ull; 
  /* return ~0 -> internal buffer not full, Collection calls next handler */
}

class Collection {
 public:
  Collection(std::vector<Event> *events);
  ~Collection();

  Collection(Collection&& o) noexcept :
    events_(o.events_),
    monitors_(std::move(o.monitors_))
  {
    print("samples: move constructor\n");
  }

  int Open();
  template <typename CALLBACK, typename BREAKOUT>
  void Read(const CALLBACK &callback, const BREAKOUT &breakout);
 private:
  std::vector<Event> *events_ = nullptr;
  std::vector<std::shared_ptr<Perf>> monitors_;
  size_t outer_ = 0, inner_ = 0;
};

Collection::Collection(std::vector<Event> *events) { events_ = events; }
Collection::~Collection() { delete events_; }

int Collection::Open() {
  if (!events_) {
    print("samples: open() no events\n");
    return ENOENT;
  }

  for (auto &event : *events_) {
    std::shared_ptr<Perf> perf(new Perf(event));

    int rc = perf->Open(); 
    if (rc) {
      print("samples: perf->open() errno %d\n", rc);
      continue;
    }

    monitors_.push_back(std::move(perf));
  }

  return 0;
}

template <typename CALLBACK, typename BREAKOUT>
void Collection::Read(const CALLBACK &callback, const BREAKOUT &breakout) {
  for (;;) {
    for (; outer_ < monitors_.size(); ++outer_) {
      inner_ = monitors_[outer_]->Read(inner_, callback, breakout);
      if (inner_ != ~0ull) {
        /* Producer needs us to return -- honor the request. */
        return;
      } else {
        /* We consumed all samples in this monitor's ring buffer. 
         * So yield the thread for a while. */
        inner_ = 0;
        std::this_thread::sleep_for(std::chrono::microseconds(1000));
      }
    }
    outer_ = 0;
  }
}

} // namespace sample

#endif