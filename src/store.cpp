#include "store.h"

#include <utility>

namespace cachedb {

std::optional<std::string_view> Store::get(std::string_view key) const {
  const auto it = entries_.find(key);
  if (it == entries_.end() || it->second.tombstone) return std::nullopt;
  return std::string_view(it->second.value);
}

void Store::set(std::string_view key, std::string_view value) {
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

bool Store::del(std::string_view key) {
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
