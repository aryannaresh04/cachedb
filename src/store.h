#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace cachedb
{

  // The in-memory table every write lands in and every read consults first.
  //
  // In M1 there is no disk, so this is the entire database. From M3 it becomes
  // the memtable: the same structure, flushed out to an SSTable once it grows
  // past a threshold, with reads falling through to those files on a miss.
  class Store
  {
  public:
    // Returns a view of the stored value, not a copy. std::map keeps each
    // entry at a stable address, so the view stays valid until this key is
    // overwritten or deleted -- which a caller does not do while it is busy
    // serializing the reply.
    //
    // Command owns its bytes for the opposite reason: the read buffer it would
    // otherwise point into reallocates as it grows.
    //
    // nullopt means absent, which includes a key holding a tombstone.
    std::optional<std::string_view> get(std::string_view key) const;

    void set(std::string_view key, std::string_view value);

    // Reports whether the key was live, which is the number DEL returns to the
    // client. A tombstone is recorded either way -- see store.cpp.
    bool del(std::string_view key);

    bool exists(std::string_view key) const { return get(key).has_value(); }

    // Live keys only. Tombstones are not counted.
    size_t size() const { return live_count_; }

  private:
    struct Entry
    {
      std::string value;
      // A delete marks the row dead rather than removing it. Expiry
      // timestamps join this struct at M4.
      bool tombstone = false;
    };

    // std::less<> instead of the default std::less<std::string> makes the
    // comparator transparent, which lets find() accept a string_view as-is.
    // Without it every lookup would construct a temporary std::string from the
    // key: an allocation on the hot path of every single GET.
    std::map<std::string, Entry, std::less<>> entries_;
    size_t live_count_ = 0;
  };

} // namespace cachedb
