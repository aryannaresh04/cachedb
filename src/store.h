#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bloom.h"
#include "memtable.h"
#include "sstable.h"
#include "wal.h"

namespace cachedb
{

  // A value on its way out to a client, from wherever it was found.
  //
  // A memtable hit borrows: the entry sits at a stable address and copying it
  // would put a memcpy on the hottest path in the program. An SSTable hit owns:
  // the bytes were read off disk into a buffer that has to outlive the call.
  // Both answer get() the same way, so the read path does not branch on where
  // the value came from and no caller has a lifetime rule to remember.
  //
  // Move-only on purpose. Copying one is a silent copy of however many bytes
  // the value holds, and nothing needs it.
  class Value
  {
  public:
    static Value borrowed(std::string_view view)
    {
      Value v;
      v.view_ = view;
      return v;
    }

    static Value owned(std::string &&bytes)
    {
      Value v;
      v.owns_ = true;
      v.owned_ = std::move(bytes);
      return v;
    }

    Value(Value &&) = default;
    Value &operator=(Value &&) = default;
    Value(const Value &) = delete;
    Value &operator=(const Value &) = delete;

    // Recomputed rather than cached, so a move cannot leave the view pointing
    // at the old object's small-string buffer.
    std::string_view get() const
    {
      return owns_ ? std::string_view(owned_) : view_;
    }

  private:
    Value() = default;
    bool owns_ = false;
    std::string owned_;
    std::string_view view_;
  };

  // What a delete reports. Two separate answers that used to be one: whether
  // the mutation is safe to acknowledge, and what DEL should reply.
  struct DelResult
  {
    bool durable = false;
    bool was_live = false;
  };

  // The same split for a multi-key delete.
  struct DelBatchResult
  {
    bool durable = false;
    int64_t removed = 0;
  };

  struct StoreOptions
  {
    // Where SSTables live. Empty means no disk at all: writes stay in the
    // memtable for ever and nothing is flushed. That is what the unit tests
    // use, and what M1 and M2 were.
    std::string dir;
    // PROJECT.md 8 flushes at 4 MB. The memtable measures itself against this
    // (6.3), and that estimate is deliberately a slight overestimate.
    size_t memtable_limit_bytes = 4u * 1024 * 1024;
    int bloom_bits_per_key = bloom::kDefaultBitsPerKey;
    // Benchmark-only. See the note on Sstable's constructor.
    bool use_bloom = true;
  };

  // The storage engine's front door: it owns durability, and the order the
  // layers are searched in.
  class Store
  {
  public:
    Store() = default;

    // Borrows the log; the Wal must outlive the Store. No disk, so no flush.
    explicit Store(Wal *wal) : wal_(wal) {}

    // Adopts any SSTables already in the directory, newest first, so a restart
    // sees the data a previous run flushed. Throws if the directory cannot be
    // read or a table in it will not open -- at startup there is nothing to
    // degrade to, and quietly ignoring an unreadable table would silently
    // lose every key that only lives in it.
    explicit Store(StoreOptions options);

    // nullopt means absent as far as a client is concerned, covering both
    // "nowhere to be found" and "deleted".
    std::optional<Value> get(std::string_view key) const;

    [[nodiscard]] bool set(std::string_view key, std::string_view value);
    [[nodiscard]] DelResult del(std::string_view key);
    [[nodiscard]] DelBatchResult del_many(
        const std::vector<std::string_view> &keys);

    // Recovery only: applies a record already in the log without writing it
    // back, and never triggers a flush -- replay runs before the Wal is even
    // open, so there would be no log to truncate afterwards.
    void apply(const Record &record);

    void set_wal(Wal *wal) { wal_ = wal; }

    bool exists(std::string_view key) const { return get(key).has_value(); }

    // Writes the memtable out as a new SSTable and truncates the log. Normally
    // driven by the threshold; exposed so a test does not have to write four
    // megabytes to see one happen.
    [[nodiscard]] bool flush();

    // Live keys in the memtable only. Keys that live solely in an SSTable are
    // not counted: a true total would mean merging every level, which is what
    // a compaction does and not what a status line should.
    size_t memtable_keys() const { return memtable_.live_count(); }
    size_t sstable_count() const { return sstables_.size(); }
    const Memtable &memtable() const { return memtable_; }

    // True if a flush has failed since the last successful one. The writes
    // themselves are still durable in the log; what has stopped is the
    // memtable being able to shed them.
    bool flush_failed() const { return flush_failed_; }

  private:
    // Called after every mutation. A failure here does not fail the write:
    // the record is already in the log, so nothing has been lost. What it
    // means is that the memtable can no longer shed memory, which is a
    // capacity problem for an operator rather than a correctness one for the
    // client that happened to be writing at the time.
    void maybe_flush();

    std::string table_path(uint64_t sequence) const;

    Memtable memtable_;
    // Newest first, which is the order a read must consult them in: an older
    // table may hold a value that a newer table's tombstone is hiding.
    std::vector<std::unique_ptr<Sstable>> sstables_;
    StoreOptions options_;
    uint64_t next_sequence_ = 1;
    bool flush_failed_ = false;
    Wal *wal_ = nullptr;
  };

} // namespace cachedb
