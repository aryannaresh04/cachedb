#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <string>
#include <string_view>

namespace cachedb
{

  // The in-memory half of the storage engine: every write lands here first and
  // every read consults it before touching a disk.
  //
  // Sorted, and that is not a detail. A flush turns this into an SSTable, and
  // an SSTable is sorted by key -- keeping the order here means a flush walks
  // the table once, in order, with no sort step and no second copy of a
  // structure that is deliberately sized to be large. PROJECT.md 6.3.
  class Memtable
  {
  public:
    struct Entry
    {
      std::string value;
      // A delete records a tombstone rather than erasing the row, because the
      // key may still exist in an older SSTable this table knows nothing
      // about, and the marker is the only thing that will hide it.
      bool tombstone = false;
    };

    // Three answers, not two, and the distinction is the whole reason this
    // returns a pointer:
    //
    //   nullptr                  not in this memtable -- keep looking below
    //   entry, tombstone = false the value, and the search is over
    //   entry, tombstone = true  deleted -- the search is over, answer absent
    //
    // Collapsing the first and third into one "absent" is the classic LSM
    // bug: the read falls through to an older file, finds the key the
    // tombstone was hiding, and resurrects data the client deleted.
    //
    // The pointer stays valid until this key is written again. std::map holds
    // each node at a stable address, the same property that lets a value view
    // outlive the lookup that produced it.
    const Entry *find(std::string_view key) const;

    void set(std::string_view key, std::string_view value);

    // Reports whether the key was live beforehand, which is DEL's reply count.
    bool del(std::string_view key);

    // Walks every entry in key order, tombstones included -- a flush has to
    // write those too, or the SSTable it produces would silently drop the
    // deletes and the keys would come back.
    //
    // A callback rather than begin()/end() so that no caller names the backing
    // container. PROJECT.md 6.3 wants a skip list measured against std::map;
    // with this shape that swap touches this file and nothing else.
    void for_each(
        const std::function<void(std::string_view, const Entry &)> &fn) const;

    bool empty() const { return entries_.empty(); }

    // Entries including tombstones: what a flush would write.
    size_t entry_count() const { return entries_.size(); }

    // Live keys only, which is what a client asking for a key count means.
    size_t live_count() const { return live_count_; }

    // Approximate bytes held, used to decide when to flush. Accounts for
    // allocator overhead, not just key and value lengths -- see memtable.cpp,
    // where the difference was measured against real RSS.
    size_t bytes() const { return bytes_; }

    void clear();

  private:
    // std::less<> instead of the default makes the comparator transparent,
    // which lets find() accept a string_view as-is. Without it every lookup
    // would construct a temporary std::string from the key: an allocation on
    // the hot path of every single GET.
    std::map<std::string, Entry, std::less<>> entries_;
    size_t live_count_ = 0;
    size_t bytes_ = 0;
  };

} // namespace cachedb
