#include "store.h"

namespace cachedb {

std::optional<std::string_view> Store::get(std::string_view key) const {
  if (const Memtable::Entry* entry = memtable_.find(key)) {
    // Found in the newest layer, and that ends the search either way. A live
    // value is the answer; a tombstone means the key was deleted and no older
    // file may be consulted, because an older file is exactly where the
    // deleted value still sits.
    if (entry->tombstone) return std::nullopt;
    return std::string_view(entry->value);
  }

  // Not in the memtable. From M3 the immutable memtable and then the SSTables
  // get consulted here, newest first. Today there are none, so absent in the
  // memtable is absent everywhere.
  return std::nullopt;
}

bool Store::set(std::string_view key, std::string_view value) {
  // The log entry goes down before the table changes, and that ordering is
  // the entire durability guarantee. Crash between the two and replay puts
  // the write back. Do it the other way round and a crash leaves a write that
  // was acknowledged and is gone -- which is the one outcome a database is
  // not allowed to have.
  if (wal_ && !wal_->append(Record::Op::kSet, key, value)) return false;
  memtable_.set(key, value);
  return true;
}

DelResult Store::del(std::string_view key) {
  // A tombstone is a log record like any other, for the same reason it is a
  // memtable entry like any other.
  if (wal_ && !wal_->append(Record::Op::kDelete, key, "")) return {};
  return {true, memtable_.del(key)};
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
}

}  // namespace cachedb
