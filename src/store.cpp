#include "store.h"

#include <dirent.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <system_error>

namespace cachedb {
namespace {

constexpr const char* kTableSuffix = ".sst";
constexpr int kSequenceDigits = 6;

// Returns the sequence number of an SSTable file name, or nullopt if the name
// is not one of ours. Anything else in the directory -- wal.log, an editor's
// backup, a half-copied file -- is simply not a table and is left alone.
std::optional<uint64_t> sequence_of(const std::string& name) {
  const size_t suffix = std::string(kTableSuffix).size();
  if (name.size() <= suffix) return std::nullopt;
  if (name.compare(name.size() - suffix, suffix, kTableSuffix) != 0) {
    return std::nullopt;
  }
  const std::string digits = name.substr(0, name.size() - suffix);
  if (digits.empty()) return std::nullopt;
  for (const char c : digits) {
    if (c < '0' || c > '9') return std::nullopt;
  }
  return std::strtoull(digits.c_str(), nullptr, 10);
}

}  // namespace

Store::Store(StoreOptions options) : options_(std::move(options)) {
  if (options_.dir.empty()) return;

  DIR* dir = ::opendir(options_.dir.c_str());
  if (dir == nullptr) {
    throw std::system_error(errno, std::generic_category(),
                            "opendir " + options_.dir);
  }

  std::vector<uint64_t> sequences;
  while (const dirent* entry = ::readdir(dir)) {
    if (const auto seq = sequence_of(entry->d_name)) sequences.push_back(*seq);
  }
  ::closedir(dir);

  // Newest first. The sequence number is the only thing that establishes
  // recency -- mtime would not, since a compaction at M4 rewrites old data
  // into a new file.
  std::sort(sequences.rbegin(), sequences.rend());
  for (const uint64_t seq : sequences) {
    sstables_.push_back(
        std::make_unique<Sstable>(table_path(seq), options_.use_bloom));
  }
  if (!sequences.empty()) next_sequence_ = sequences.front() + 1;
}

std::string Store::table_path(uint64_t sequence) const {
  char name[32];
  std::snprintf(name, sizeof(name), "%0*llu%s", kSequenceDigits,
                static_cast<unsigned long long>(sequence), kTableSuffix);
  return options_.dir + "/" + name;
}

std::optional<Found> Store::lookup(std::string_view key) const {
  // The clock is read at most once per lookup, and not at all unless a stamp
  // is actually met. Most keys have no expiry, and a GET that never sees one
  // should not pay for a clock read on the hottest path in the program.
  //
  // Once, rather than per layer, for a second reason: two reads of the clock
  // inside one lookup could straddle a millisecond and judge two layers
  // against different values of "now".
  int64_t now = 0;  // 0 means "not read yet", not "the epoch"
  const auto expired = [&now](int64_t stamp) {
    if (stamp == 0) return false;  // no expiry, so nothing to compare
    if (now == 0) now = now_ms();
    return stamp <= now;
  };

  if (const Memtable::Entry* entry = memtable_.find(key)) {
    // Found in the newest layer, and that ends the search either way. A
    // tombstone means the key was deleted and no older file may be consulted
    // -- an older file is exactly where the deleted value still sits.
    if (entry->tombstone) return std::nullopt;
    // An expired entry ends the search in exactly the same way, and for
    // exactly the same reason. Falling through to an older table here would
    // uncover the value this entry was hiding: SET k v1, flush, SET k v2 EX
    // 10, wait -- and a GET would answer v1, a value the client replaced.
    if (expired(entry->expires_at_ms)) return std::nullopt;
    return Found{Value::borrowed(entry->value), entry->expires_at_ms};
  }

  // Newest table first. The first one with anything to say about this key
  // settles it, for the same reason: a tombstone here hides whatever an older
  // table still holds.
  for (const auto& table : sstables_) {
    Sstable::Lookup found = table->get(key);
    if (!found.found) continue;
    if (found.tombstone) return std::nullopt;
    if (expired(found.expires_at_ms)) return std::nullopt;
    return Found{Value::owned(std::move(found.value)), found.expires_at_ms};
  }
  return std::nullopt;
}

std::optional<Value> Store::get(std::string_view key) const {
  std::optional<Found> found = lookup(key);
  if (!found) return std::nullopt;
  return std::move(found->value);
}

TtlResult Store::ttl(std::string_view key) const {
  const std::optional<Found> found = lookup(key);
  if (!found) return {};  // exists = false, which TTL reports as -2
  if (found->expires_at_ms == 0) return {true, false, 0};

  // lookup() already refused to return anything whose stamp had passed, so
  // this cannot be negative by the time it is read. Clamped at zero anyway:
  // the clock is read twice across the two calls and could step between them.
  const int64_t remaining = found->expires_at_ms - now_ms();
  return {true, true, remaining > 0 ? remaining : 0};
}

ExpireResult Store::expire(std::string_view key,
                                  int64_t expires_at_ms) {
  const std::optional<Found> found = lookup(key);
  if (!found) return {true, false};  // nothing to expire; EXPIRE replies :0

  // Copied out before the write, not viewed across it. On a memtable hit the
  // value borrows the entry's own std::string, and set() assigns over that
  // same string -- which may reallocate, leaving the view dangling. The copy
  // is the whole reason this is not a two-line function.
  const std::string value(found->value.get());

  if (!set(key, value, expires_at_ms)) return {false, false};
  return {true, true};
}

bool Store::set(std::string_view key, std::string_view value,
                int64_t expires_at_ms) {
  // The log entry goes down before the table changes, and that ordering is
  // the entire durability guarantee. Crash between the two and replay puts
  // the write back. Do it the other way round and a crash leaves a write that
  // was acknowledged and is gone -- the one outcome a database may not have.
  if (wal_ && !wal_->append(Record::Op::kSet, key, value, expires_at_ms)) {
    return false;
  }
  memtable_.set(key, value, expires_at_ms);
  maybe_flush();
  return true;
}

DelResult Store::del(std::string_view key) {
  // Asked before anything changes, because the answer is about the state the
  // client last saw. Only a full layered read knows it: the memtable alone
  // cannot see a key that lives solely in an SSTable, and cannot tell a live
  // entry from one whose expiry has passed.
  //
  // This makes DEL cost what GET costs -- a filter check per table, and a
  // block read from the first table that claims the key. That price buys one
  // number, and it is worth knowing that RocksDB declines to pay it at all:
  // its Delete returns nothing, because in a log-structured store "did that
  // key exist" is not something you know, it is something you go and find out.
  const bool was_live = get(key).has_value();

  // A tombstone is a log record like any other, for the same reason it is a
  // memtable entry like any other.
  if (wal_ && !wal_->append(Record::Op::kDelete, key, "")) return {};
  memtable_.del(key);
  maybe_flush();
  return {true, was_live};
}

DelBatchResult Store::del_many(const std::vector<std::string_view>& keys) {
  if (wal_) {
    std::vector<Wal::Mutation> batch;
    batch.reserve(keys.size());
    for (const std::string_view key : keys) {
      batch.push_back({Record::Op::kDelete, key, {}});
    }
    // Every tombstone or none. Nothing in memory is touched until the log has
    // accepted the lot.
    if (!wal_->append_batch(batch)) return {};
  }

  DelBatchResult result;
  result.durable = true;
  for (const std::string_view key : keys) {
    // Same question as the single-key path, and the same reason it cannot be
    // answered from the memtable. Counted before the tombstone goes in, so a
    // key named twice in one command is counted once -- the second look
    // already finds the tombstone the first one left.
    if (get(key).has_value()) ++result.removed;
    memtable_.del(key);
  }
  maybe_flush();
  return result;
}

void Store::apply(const Record& record) {
  switch (record.op) {
    case Record::Op::kSet:
      // The stamp is replayed as it was written, never recomputed. An expiry
      // is a point in time, so a key that ran out while the process was down
      // must come back already expired rather than getting a fresh lease.
      memtable_.set(record.key, record.value, record.expires_at_ms);
      break;
    case Record::Op::kDelete:
      // The return is DEL's reply count, which recovery has nobody to tell.
      memtable_.del(record.key);
      break;
  }
  // Deliberately no maybe_flush(). Replay runs before the Wal is opened, so
  // there would be no log to truncate, and a flush partway through recovery
  // would write a table from a half-rebuilt memtable.
}

void Store::maybe_flush() {
  if (options_.dir.empty()) return;
  if (memtable_.bytes() < options_.memtable_limit_bytes) return;
  flush_failed_ = !flush();
}

bool Store::flush() {
  if (options_.dir.empty() || memtable_.empty()) return true;

  const uint64_t sequence = next_sequence_;
  const std::string path = table_path(sequence);

  {
    SstableWriter writer(path, options_.bloom_bits_per_key);
    // One pass in key order, which is what the memtable being sorted buys.
    // Tombstones go out too: a table that dropped them would lose the deletes
    // and the keys they hide would come back from an older table.
    // Entries that have already expired are written out too, stamp and all.
    // Dropping them here would be the tombstone mistake wearing a different
    // hat: the expired entry may be the only thing hiding a live copy in an
    // older table, and leaving it out would let that copy resurface. Only a
    // compaction of the oldest level can discard them, which is the same rule
    // tombstones follow (PROJECT.md 6.7).
    memtable_.for_each(
        [&writer](std::string_view key, const Memtable::Entry& entry) {
          writer.add(key, entry.value, entry.tombstone, entry.expires_at_ms);
        });
    if (!writer.finish()) return false;
  }

  // The table is fsynced, so the data exists in two places. Only now may the
  // log be cut. Truncating first would leave a window where a crash loses
  // everything the memtable held; doing it in this order, a crash between the
  // two simply replays records that are already in the table -- they go back
  // into the memtable and are flushed again, which is wasteful and harmless.
  if (wal_ && !wal_->truncate()) return false;

  sstables_.insert(sstables_.begin(),
                   std::make_unique<Sstable>(path, options_.use_bloom));
  next_sequence_ = sequence + 1;
  memtable_.clear();
  return true;
}

}  // namespace cachedb
