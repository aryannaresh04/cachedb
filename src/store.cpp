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

std::optional<Value> Store::get(std::string_view key) const {
  if (const Memtable::Entry* entry = memtable_.find(key)) {
    // Found in the newest layer, and that ends the search either way. A
    // tombstone means the key was deleted and no older file may be consulted
    // -- an older file is exactly where the deleted value still sits.
    if (entry->tombstone) return std::nullopt;
    return Value::borrowed(entry->value);
  }

  // Newest table first. The first one with anything to say about this key
  // settles it, for the same reason: a tombstone here hides whatever an older
  // table still holds.
  for (const auto& table : sstables_) {
    Sstable::Lookup found = table->get(key);
    if (!found.found) continue;
    if (found.tombstone) return std::nullopt;
    return Value::owned(std::move(found.value));
  }
  return std::nullopt;
}

bool Store::set(std::string_view key, std::string_view value) {
  // The log entry goes down before the table changes, and that ordering is
  // the entire durability guarantee. Crash between the two and replay puts
  // the write back. Do it the other way round and a crash leaves a write that
  // was acknowledged and is gone -- the one outcome a database may not have.
  if (wal_ && !wal_->append(Record::Op::kSet, key, value)) return false;
  memtable_.set(key, value);
  maybe_flush();
  return true;
}

DelResult Store::del(std::string_view key) {
  // A tombstone is a log record like any other, for the same reason it is a
  // memtable entry like any other.
  if (wal_ && !wal_->append(Record::Op::kDelete, key, "")) return {};
  const DelResult result{true, memtable_.del(key)};
  maybe_flush();
  return result;
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
    if (memtable_.del(key)) ++result.removed;
  }
  maybe_flush();
  return result;
}

void Store::apply(const Record& record) {
  switch (record.op) {
    case Record::Op::kSet:
      memtable_.set(record.key, record.value);
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
    memtable_.for_each(
        [&writer](std::string_view key, const Memtable::Entry& entry) {
          writer.add(key, entry.value, entry.tombstone);
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
