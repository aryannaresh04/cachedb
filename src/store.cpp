#include "store.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

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

// Makes a directory's own contents durable -- the names in it, not the data
// in the files it lists.
//
// Creating or removing a file changes the directory, and fsync(2) is explicit
// that fsyncing the file does not carry that change with it: "Calling fsync()
// does not necessarily ensure that the entry in the directory containing the
// file has also reached disk. For that an explicit fsync() on a file
// descriptor for the directory is also needed."
//
// Without this a flush could fsync a whole SSTable, truncate the log, and
// lose the table to a power cut anyway, because the name pointing at it never
// landed -- acknowledged writes gone, with the log already emptied on the
// strength of a file that is no longer reachable. crash_test.sh cannot see
// this: kill -9 leaves the page cache with the kernel, so the entry survives.
// Only losing power loses it, which is exactly the distinction 6.4 draws.
bool fsync_dir(const std::string& path) {
  const int fd = ::open(path.c_str(), O_RDONLY | O_DIRECTORY);
  if (fd < 0) return false;
  bool ok = true;
  for (;;) {
    if (::fsync(fd) == 0) break;
    if (errno == EINTR) continue;
    // Same rule as the log's fsync: a writeback error is reported once and
    // then cleared, so retrying could return success over lost data.
    ok = false;
    break;
  }
  ::close(fd);
  return ok;
}

// A run of this many similarly sized tables is worth merging. Four is
// PROJECT.md 6.7's figure and the usual size-tiered default: small enough
// that reads never walk far, large enough that each byte is not rewritten on
// every flush.
constexpr size_t kCompactionTrigger = 4;

// Two tables belong to the same tier if neither is more than this many times
// the size of the other. A merged table comes out roughly four times its
// inputs, so it falls out of their tier by itself -- which is the whole idea
// of size tiering, and why no level has to be written down anywhere.
constexpr uint64_t kTierRatio = 2;

// Where a merge is assembled before it is given its real name. Deliberately
// not NNNNNN.sst, so a crash mid-merge leaves a file the directory scan
// ignores rather than a table it would try to read.
constexpr const char* kCompactTemp = "compact.tmp";

uint64_t file_size(const std::string& path) {
  struct ::stat st {};
  if (::stat(path.c_str(), &st) != 0) return 0;
  return static_cast<uint64_t>(st.st_size);
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
  // A merge that died before its rename leaves this behind. The scan above
  // already ignores it -- it is not NNNNNN.sst -- but leaving it would let it
  // accumulate one per crash.
  ::unlink((options_.dir + "/" + kCompactTemp).c_str());

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

size_t Store::sweep_expired(size_t budget) {
  // The clock is read here and nowhere below, so the sweep and a concurrent
  // read cannot disagree about which entries have run out.
  //
  // No maybe_flush() afterwards: sweeping never grows the memtable, and the
  // estimate does not fall when an entry is swept -- the allocator keeps the
  // freed bytes, so the process still holds them.
  const size_t swept = memtable_.sweep_expired(now_ms(), budget);
  swept_keys_ += swept;
  return swept;
}

size_t Store::maybe_compact() {
  if (!options_.use_compaction) return 0;
  if (options_.dir.empty() || sstables_.size() < kCompactionTrigger) return 0;

  std::vector<uint64_t> sizes;
  sizes.reserve(sstables_.size());
  for (const auto& table : sstables_) sizes.push_back(file_size(table->path()));

  // sstables_ is newest first, so this walks from the oldest table towards
  // the newest, taking the first run long enough to bother with. Oldest first
  // on purpose: that is the end where `drop_obsolete` can be true, which is
  // the only place tombstones and expired entries actually go away.
  //
  // The run must be *contiguous* in this vector, and that is not tidiness.
  // The merged table inherits the sequence number of the newest table in the
  // run, so it keeps that table's place in the recency order. A gap would
  // mean some untouched table sat between the inputs in age, and the merged
  // result would jump over it -- serving a value that a newer table had
  // already replaced.
  size_t best_begin = 0, best_len = 0;
  size_t i = sstables_.size();
  while (i > 0) {
    size_t end = i;          // one past the oldest member of this run
    uint64_t smallest = sizes[i - 1];
    while (i > 0) {
      const uint64_t candidate = sizes[i - 1];
      const uint64_t low = std::min(candidate, smallest);
      const uint64_t high = std::max(candidate, smallest);
      if (low == 0 || high > low * kTierRatio) break;
      smallest = low;
      --i;
    }
    if (end - i >= kCompactionTrigger) {
      best_begin = i;
      best_len = end - i;
      break;
    }
    if (end == i) --i;  // a zero-length run would not terminate the walk
  }
  if (best_len < kCompactionTrigger) return 0;

  // Nothing older survives this merge, so tombstones and expired entries may
  // finally be discarded rather than carried forward for ever.
  const bool drop_obsolete = best_begin + best_len == sstables_.size();

  std::vector<const Sstable*> inputs;
  std::vector<std::string> paths;
  for (size_t n = best_begin; n < best_begin + best_len; ++n) {
    inputs.push_back(sstables_[n].get());
    paths.push_back(sstables_[n]->path());
  }
  // The newest input's name, which the result takes over.
  const std::string target = paths.front();
  const std::string temp = options_.dir + "/" + kCompactTemp;

  const CompactionResult merged = compact(inputs, temp, options_.bloom_bits_per_key,
                                          drop_obsolete, now_ms());
  if (!merged.ok) {
    ::unlink(temp.c_str());
    return 0;
  }

  // rename() before any unlink, and that order is the whole crash story. A
  // rename is atomic, so after this the merged table either is or is not in
  // place under a name a restart will read. Losing power here leaves the
  // merged table *and* its inputs: harmless, because the merged table holds
  // the newest version of every key they held and sits at the newest one's
  // place in the order, so it shadows them completely. Wasted space until the
  // next merge, and not a wrong answer. Unlinking first would instead leave a
  // gap with no table at all.
  if (::rename(temp.c_str(), target.c_str()) != 0) {
    ::unlink(temp.c_str());
    return 0;
  }
  if (!fsync_dir(options_.dir)) return 0;

  // Closed before the old files are unlinked, so the space actually comes
  // back rather than being held by an open descriptor on a deleted inode.
  sstables_.erase(sstables_.begin() + static_cast<long>(best_begin),
                  sstables_.begin() + static_cast<long>(best_begin + best_len));
  for (size_t n = 1; n < paths.size(); ++n) ::unlink(paths[n].c_str());
  fsync_dir(options_.dir);

  // A merge that dropped everything leaves nothing worth opening.
  if (merged.written > 0) {
    sstables_.insert(sstables_.begin() + static_cast<long>(best_begin),
                     std::make_unique<Sstable>(target, options_.use_bloom));
  } else {
    ::unlink(target.c_str());
    fsync_dir(options_.dir);
  }

  ++compactions_;
  return best_len - (merged.written > 0 ? 1 : 0);
}

bool Store::flush_all() {
  // Closed before they are unlinked. On Linux unlinking an open file is legal
  // and the space is only reclaimed when the last descriptor goes, so leaving
  // these open would delete the names and keep the bytes.
  std::vector<std::string> paths;
  paths.reserve(sstables_.size());
  for (const auto& table : sstables_) paths.push_back(table->path());
  sstables_.clear();

  bool ok = true;
  for (const std::string& path : paths) {
    // ENOENT is not a failure: the file is gone, which is the goal.
    if (::unlink(path.c_str()) != 0 && errno != ENOENT) ok = false;
  }

  // The unlinks have to be durable before the log is cut. See the note on
  // this function in store.h for what the other order costs.
  if (!options_.dir.empty() && !fsync_dir(options_.dir)) ok = false;
  if (!ok) return false;

  if (wal_ && !wal_->truncate()) return false;

  memtable_.clear();
  // Safe to start over: every table that could have claimed a number is gone,
  // and gone durably, so no name can collide with one a restart would find.
  next_sequence_ = 1;
  flush_failed_ = false;
  return true;
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

  // The table's bytes are fsynced, but the directory entry naming it is not
  // durable until the directory itself is synced -- see fsync_dir above. This
  // has to happen before the log is cut, or a power cut could take the name
  // away from a log that has already been emptied.
  if (!options_.dir.empty() && !fsync_dir(options_.dir)) return false;

  // The table is fsynced and reachable, so the data exists in two places.
  // Only now may the log be cut. Truncating first would leave a window where a crash loses
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
