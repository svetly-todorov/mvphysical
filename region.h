#ifndef REGIONS_H
#define REGIONS_H

#include <map>
#include <vector>

/* region struct */
struct Region {
  uintptr_t start;
  uintptr_t end;
  size_t data;
};

/* class for managing non overlapping regions */
template <typename INFO>
class Regions {
 public:
  INFO *Find(size_t val);
  void Insert(size_t key, INFO info);
 private:
  /* underlying rb tree here... */
  std::map<size_t, INFO> regions_;
  
  /* Nasty little deletion queue */
  std::vector<size_t> del_;
  void Cleanup();
};

/*
 * Return the INFO for the key that this val falls under.
 * thanks wolfgang:
 * https://stackoverflow.com/questions/8561113/storing-set-of-non-overlapping-ranges-and-finding-whether-a-value-is-present-in
 */
template <typename INFO>
INFO *Regions<INFO>::Find(size_t val) {
  auto it = regions_.upper_bound(val);
  if (it == regions_.begin()) return nullptr;

  --it;
  if (val >= it->first && val < it->second.end) return &(it->second);
  return nullptr;
}

/*
 * Remove regions that overlap with the new one.
 */
template <typename INFO>
void Regions<INFO>::Insert(size_t key, INFO info) {
  /* Find first element with key bigger than this key */
  auto r = regions_.upper_bound(key);
  auto l = r;
  
  /* If no such thing, insert and leave */
  if (r == regions_.begin()) {
    regions_[key] = info;
    return;
  }

  /* mark this region for deletion if new key is greater than start addr */
  do {
    if (r == regions_.end()) break;
    if (info.end >= r->first) del_.push_back(r->first);
    ++r;
  } while (info.end >= r->first);

  /* Seek backwards to find the previous region */
  --l;

  /* If prev region overlaps with this one, mark for del */
  if (key <= l->second.end) del_.push_back(l->first);

  Cleanup();
}

template <typename INFO>
void Regions<INFO>::Cleanup() {
  for (auto d : del_) regions_.erase(d);
  del_.clear();
}

#endif