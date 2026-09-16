#include "memtable.h"

#include <string_view>

namespace cachedb {
namespace {

// What one entry costs regardless of how long its key and value are: the
// red-black tree node (a colour and three pointers), the two std::string
// objects living inside it, and the allocator's header on the node itself.
//
// 120 was measured against real RSS. The expiry stamp added 8 to it and not
// zero: an Entry had a bool followed by padding, but the padding sat at the
// end of the object rather than inside it, so the int64 extended the node
// rather than fitting in a hole. Measured, not assumed -- sizeof the tree
// node on Linux/libstdc++ went from 104 to 112.
constexpr size_t kEntryOverhead = 128;

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

void Memtable::set(std::string_view key, std::string_view value,
                   int64_t expires_at_ms) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    entries_.emplace(std::string(key),
                     Entry{std::string(value), false, expires_at_ms});
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
  it->second.expires_at_ms = expires_at_ms;
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
    entries_.emplace(std::string(key), Entry{std::string(), true, 0});
    bytes_ += entry_cost(key, "");
    return was_live;
  }

  // swap with an empty string, not clear(). clear() sets the length to zero
  // and keeps the buffer, so a tombstone would go on holding every byte of
  // the value it replaced -- and the active sweep, which turns expired
  // entries into tombstones, would reclaim precisely nothing.
  //
  // Measured on 50,000 emptied values of 200 bytes, then writing 50,000 fresh
  // keys over them: 18.37 MB of growth with clear(), 8.07 MB with swap.
  //
  // bytes_ still does not go down, and that is not caution -- it stays
  // accurate. The allocator keeps the freed chunk rather than returning it to
  // the kernel, so RSS does not fall either, and RSS is what this estimate
  // was calibrated against. What the swap buys is the next key written into
  // this memtable being able to use those bytes instead of asking for more.
  std::string().swap(it->second.value);
  it->second.tombstone = true;
  // A tombstone with an expiry is a contradiction -- it says "absent, until
  // it stops being absent". Clearing it keeps the flag the only thing a
  // reader has to consult once the marker is set.
  it->second.expires_at_ms = 0;
  return was_live;
}

void Memtable::for_each(
    const std::function<void(std::string_view, const Entry&)>& fn) const {
  for (const auto& [key, entry] : entries_) fn(key, entry);
}

size_t Memtable::sweep_expired(int64_t now, size_t budget) {
  if (entries_.empty()) {
    sweep_cursor_.clear();
    return 0;
  }

  // lower_bound rather than find: the cursor names where to resume, and that
  // key may have been swept into a tombstone or may never have existed at
  // all. The nearest key at or after it is the right place either way.
  auto it = entries_.lower_bound(sweep_cursor_);

  size_t swept = 0;
  for (size_t examined = 0; examined < budget; ++examined) {
    if (it == entries_.end()) break;

    Entry& entry = it->second;
    // A tombstone has neither a value to release nor an expiry to check.
    if (!entry.tombstone && entry.expires_at_ms != 0 &&
        entry.expires_at_ms <= now) {
      // A tombstone and not an erase, which is the same rule a DEL follows:
      // the key may still sit in an older SSTable, and this marker is the
      // only thing that will hide it. Erasing here would let that older copy
      // resurface -- the value would come back from the dead because it
      // expired, which is an absurd sentence and an easy bug.
      entry.tombstone = true;
      entry.expires_at_ms = 0;
      std::string().swap(entry.value);
      --live_count_;
      ++swept;
    }
    ++it;
  }

  // Where to pick up. Running off the end resets to empty, so the next tick
  // starts from the first key again rather than stalling at the end for ever.
  sweep_cursor_ = (it == entries_.end()) ? std::string() : it->first;
  return swept;
}

void Memtable::clear() {
  entries_.clear();
  sweep_cursor_.clear();
  live_count_ = 0;
  bytes_ = 0;
}

}  // namespace cachedb
