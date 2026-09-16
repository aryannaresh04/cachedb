#pragma once

#include <cstddef>
#include <optional>
#include <string_view>
#include <vector>

#include "memtable.h"
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

  // What a multi-key delete reports. Same split as DelResult: whether the
  // command may be acknowledged, and what it should reply.
  struct DelBatchResult
  {
    bool durable = false;
    // How many of the keys were live. Meaningless unless durable.
    int64_t removed = 0;
  };

  // The storage engine's front door. It owns durability and the order the
  // layers are consulted in; it does not own the layers' internals.
  //
  // Today there is one layer, the memtable. From M3 it grows an immutable
  // memtable while a flush is running and a list of SSTables behind that, and
  // get() walks them newest first -- which is why get() below already has the
  // shape of a search that can continue past the memtable rather than one that
  // assumes the memtable is the whole database.
  class Store
  {
  public:
    // A Store with no log applies writes to memory and never fails one. That
    // is what the unit tests use, and what M1 was in its entirety.
    Store() = default;

    // Borrows the log; the Wal must outlive the Store.
    explicit Store(Wal *wal) : wal_(wal) {}

    // nullopt means the key is absent as far as a client is concerned, which
    // covers both "nowhere to be found" and "deleted".
    //
    // The returned view stays valid until that key is written again; the
    // memtable holds its entries at stable addresses.
    std::optional<std::string_view> get(std::string_view key) const;

    // Logs the write, then applies it. Returns false if the log write failed,
    // in which case nothing was applied and the caller must not acknowledge.
    //
    // [[nodiscard]] deliberately: under -Werror, ignoring the durability
    // answer is a compile error rather than a write that silently is not one.
    [[nodiscard]] bool set(std::string_view key, std::string_view value);

    [[nodiscard]] DelResult del(std::string_view key);

    // Deletes several keys as one unit. The tombstones go down as a single
    // log write, so a log failure leaves none of them applied rather than
    // some -- which is what DEL k1 k2 k3 used to do, deleting the keys before
    // the failure and reporting an error that did not say which.
    //
    // Duplicate keys are counted once, as real Redis counts them: the second
    // delete of a key finds it already dead.
    [[nodiscard]] DelBatchResult del_many(
        const std::vector<std::string_view> &keys);

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

    // Live keys, tombstones excluded. From M3 this stops being the whole
    // answer: keys living only in an SSTable are not counted here, and a true
    // count would mean a merge across every level.
    size_t size() const { return memtable_.live_count(); }

    // For the flush path at M3, which walks the memtable in key order and
    // needs to know how big it has grown.
    const Memtable &memtable() const { return memtable_; }

  private:
    Memtable memtable_;
    // Null means no durability, which is a legitimate configuration for a
    // test and not a state to guard against everywhere.
    Wal *wal_ = nullptr;
  };

} // namespace cachedb
