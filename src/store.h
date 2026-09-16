#pragma once

#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "wal.h"

namespace cachedb
{

  // What a delete reports. Two separate answers that used to be one: whether
  // the mutation is safe to acknowledge, and what DEL should reply.
  struct DelResult
  {
    // False when the log write failed. Nothing was mutated, and the client
    // must not be told anything was deleted.
    bool durable = false;
    // Whether the key was live beforehand, which is the number DEL counts.
    // Meaningless unless durable.
    bool was_live = false;
  };

  // The in-memory table every write lands in and every read consults first.
  //
  // In M1 there is no disk, so this is the entire database. From M3 it becomes
  // the memtable: the same structure, flushed out to an SSTable once it grows
  // past a threshold, with reads falling through to those files on a miss.
  class Store
  {
  public:
    // A Store with no log applies writes to memory and never fails one. That
    // is what the unit tests use, and what M1 effectively was.
    Store() = default;

    // Borrows the log; the Wal must outlive the Store. Not owned, because
    // startup replays the log before the Store exists and the same file has
    // to stay open across both.
    explicit Store(Wal *wal) : wal_(wal) {}

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

    // Logs the write, then applies it. Returns false if the log write failed,
    // in which case nothing was applied and the caller must not acknowledge.
    //
    // [[nodiscard]] deliberately: under -Werror, ignoring the durability
    // answer is a compile error rather than a write that silently is not one.
    [[nodiscard]] bool set(std::string_view key, std::string_view value);

    [[nodiscard]] DelResult del(std::string_view key);

    // Recovery only: applies a record already in the log, without writing it
    // back. Replaying through set()/del() would append every record a second
    // time and the log would double on every restart.
    void apply(const Record &record);

    // Startup ordering: the log has to be replayed -- which truncates any torn
    // tail -- before it is opened for appending, or the Wal caches a size that
    // the truncation then invalidates. So the Store outlives its Wal's
    // construction and is handed the log afterwards. Recovery still goes
    // through apply(), never through set().
    void set_wal(Wal *wal) { wal_ = wal; }

    bool exists(std::string_view key) const { return get(key).has_value(); }

    // Live keys only. Tombstones are not counted.
    size_t size() const { return live_count_; }

  private:
    // The mutation itself, with no log involved. set()/del() are these plus a
    // log append in front; recovery is these on their own. One code path
    // changes the table, whichever way in you came.
    void apply_set(std::string_view key, std::string_view value);
    bool apply_del(std::string_view key); // returns whether the key was live

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
    // Null means no durability, which is a legitimate configuration for a
    // test and not a state to guard against everywhere.
    Wal *wal_ = nullptr;
  };

} // namespace cachedb
