#include "store.h"

#include <utility>

namespace cachedb {

std::optional<std::string_view> Store::get(std::string_view key) const {
  const auto it = entries_.find(key);
  if (it == entries_.end() || it->second.tombstone) return std::nullopt;
  return std::string_view(it->second.value);
}

bool Store::set(std::string_view key, std::string_view value) {
  // The log entry goes down before the table changes, and that ordering is
  // the entire durability guarantee. Crash between the two and replay puts
  // the write back. Do it the other way round and a crash leaves a write that
  // was acknowledged and is gone -- which is the one outcome a database is
  // not allowed to have.
  if (wal_ && !wal_->append(Record::Op::kSet, key, value)) return false;
  apply_set(key, value);
  return true;
}

DelResult Store::del(std::string_view key) {
  // A tombstone is a log record like any other, for the same reason it is a
  // table entry like any other: from M3 the key may still live in an SSTable,
  // and the tombstone is the only thing that will hide it.
  if (wal_ && !wal_->append(Record::Op::kDelete, key, "")) return {};
  return {true, apply_del(key)};
}

void Store::apply(const Record& record) {
  switch (record.op) {
    case Record::Op::kSet:
      apply_set(record.key, record.value);
      break;
    case Record::Op::kDelete:
      // The return is the DEL reply count, which recovery has nobody to tell.
      apply_del(record.key);
      break;
  }
}

void Store::apply_set(std::string_view key, std::string_view value) {
  const auto it = entries_.find(key);
  if (it == entries_.end()) {
    entries_.emplace(std::string(key), Entry{std::string(value), false});
    ++live_count_;
    return;
  }
  if (it->second.tombstone) {
    // Writing over a tombstone revives the key: the marker is replaced by the
    // value rather than kept alongside it.
    it->second.tombstone = false;
    ++live_count_;
  }
  it->second.value.assign(value);
}

bool Store::apply_del(std::string_view key) {
  const auto it = entries_.find(key);
  const bool was_live = it != entries_.end() && !it->second.tombstone;
  if (was_live) --live_count_;

  // The tombstone goes in whether or not the key was here. Today that is
  // redundant: this table is the whole database, so a miss means the key does
  // not exist anywhere. From M3 it stops being redundant -- the key may still
  // sit in an SSTable this table knows nothing about, and the tombstone is the
  // only thing that will hide it from a read. Writing it unconditionally now
  // means the delete path does not have to change when the disk layers land.
  if (it == entries_.end()) {
    entries_.emplace(std::string(key), Entry{std::string(), true});
  } else {
    it->second.value = std::string();  // a tombstone keeps no value
    it->second.tombstone = true;
  }
  return was_live;
}

}  // namespace cachedb
