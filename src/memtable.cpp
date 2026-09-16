#include "memtable.h"

#include <string_view>

namespace cachedb {
namespace {

// What one entry costs regardless of how long its key and value are: the
// red-black tree node (a colour and three pointers), the two std::string
// objects living inside it, and the allocator's header on the node itself.
constexpr size_t kEntryOverhead = 120;

// Up to this length a std::string keeps its bytes inside the object under the
// small-string optimisation, so they are already counted in kEntryOverhead and
// cost nothing extra. Both libstdc++ and libc++ draw the line at 15.
constexpr size_t kSsoCapacity = 15;

// What a string's bytes actually cost on the heap, which is not what
// size() says. The allocator writes a header before every chunk and rounds the
// chunk to a multiple of its alignment, so a 64-byte value occupies closer to
// 96 bytes.
//
// Counting size() alone was the first attempt and it was wrong in the
// dangerous direction: measured against real RSS it came in at 0.88 of the
// truth for 16-byte keys with 64-byte values, meaning a 4 MB memtable really
// held 4.54 MB. Under-reading a flush threshold is exactly how a process ends
// up holding more than PROJECT.md 8 says it may.
size_t heap_cost(size_t length) {
  if (length <= kSsoCapacity) return 0;
  constexpr size_t kAlign = 16;
  constexpr size_t kMallocHeader = 16;
  return (length + 1 + kAlign - 1) / kAlign * kAlign + kMallocHeader;
}

size_t entry_cost(std::string_view key, std::string_view value) {
  return kEntryOverhead + heap_cost(key.size()) + heap_cost(value.size());
}

}  // namespace

const Memtable::Entry* Memtable::find(std::string_view key) const {
  const auto it = entries_.find(key);
  return it == entries_.end() ? nullptr : &it->second;
}

void Memtable::set(std::string_view key, std::string_view value) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    entries_.emplace(std::string(key), Entry{std::string(value), false});
    ++live_count_;
    bytes_ += entry_cost(key, value);
    return;
  }

  if (it->second.tombstone) {
    // Writing over a tombstone revives the key: the marker is replaced by the
    // value rather than kept alongside it.
    it->second.tombstone = false;
    ++live_count_;
  }

  // The estimate never goes down, because the memory does not either: neither
  // assign() nor clear() returns a string's capacity to the allocator, and the
  // map holds its nodes until the whole table is cleared. Subtracting on a
  // shrink would drift the estimate below the truth, and a flush threshold
  // that reads low is the one failure mode worth avoiding here.
  const size_t old_cost = heap_cost(it->second.value.size());
  it->second.value.assign(value);
  const size_t new_cost = heap_cost(value.size());
  if (new_cost > old_cost) bytes_ += new_cost - old_cost;
}

bool Memtable::del(std::string_view key) {
  const auto it = entries_.find(key);
  const bool was_live = it != entries_.end() && !it->second.tombstone;
  if (was_live) --live_count_;

  if (it == entries_.end()) {
    // The tombstone goes in even though the key is not here, and from M3 that
    // is the entire point: the key may still sit in an SSTable this table has
    // never seen, and this marker is what will hide it from a read.
    entries_.emplace(std::string(key), Entry{std::string(), true});
    bytes_ += entry_cost(key, "");
    return was_live;
  }

  // No subtraction, per the note in set(): clearing the value does not hand
  // its buffer back, so those bytes are still held until the table is cleared.
  it->second.value.clear();
  it->second.tombstone = true;
  return was_live;
}

void Memtable::for_each(
    const std::function<void(std::string_view, const Entry&)>& fn) const {
  for (const auto& [key, entry] : entries_) fn(key, entry);
}

void Memtable::clear() {
  entries_.clear();
  live_count_ = 0;
  bytes_ = 0;
}

}  // namespace cachedb
