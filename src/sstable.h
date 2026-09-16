#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "bloom.h"
#include "fd.h"

namespace cachedb
{

  // An immutable sorted file, produced by flushing a memtable. PROJECT.md 6.5.
  //
  //   ┌──────────────────────────────────────────┐
  //   │ Data block   [klen:4][vlen:4][flags:1]   │  sorted by key
  //   │              [key][value] ...            │
  //   ├──────────────────────────────────────────┤
  //   │ Index block  [count:4] then, per entry,  │  sparse: roughly one
  //   │              [klen:4][key][offset:8]     │  entry per 4 KB of data
  //   ├──────────────────────────────────────────┤
  //   │ Bloom block  exactly as bloom::build     │
  //   ├──────────────────────────────────────────┤
  //   │ Footer       [index_off:8][bloom_off:8]  │  fixed 32 bytes, last
  //   │              [entry_count:8][magic:8]    │  thing in the file
  //   └──────────────────────────────────────────┘
  //
  // Little-endian throughout, written byte by byte, for the same reason the
  // WAL is: a file format should not inherit the host's byte order or its
  // struct padding.
  //
  // No checksums, following 6.5. The magic still catches a truncated or
  // foreign file, which is the failure a bug produces; silent bit rot on disk
  // goes undetected, and is also not something this project could honestly
  // demonstrate.
  namespace sstable
  {

    // "CDBSSTv1" read straight out of a hex dump. A version lives in the last
    // byte so a future format change is a diagnosable error rather than a
    // misparse.
    inline constexpr char kMagic[8] = {'C', 'D', 'B', 'S', 'S', 'T', 'v', '1'};
    inline constexpr size_t kFooterSize = 32;

    // One index entry per this many bytes of data block, not per N keys. The
    // index exists to bound how far a lookup scans after seeking, and that
    // distance is bytes -- indexing every Nth key would give a table of tiny
    // values and a table of 4 KB values the same index density and wildly
    // different scan costs.
    inline constexpr size_t kIndexInterval = 4096;

    // One entry as the caller supplies it.
    struct Entry
    {
      std::string_view key;
      std::string_view value;
      bool tombstone = false;
    };

  } // namespace sstable

  // Builds one SSTable. Keys must be added in ascending order; the file is
  // sorted, and the index and the filter are built in the same single pass
  // that writes the data.
  class SstableWriter
  {
  public:
    // Creates (or truncates) the file. Throws if it cannot be opened, which
    // at flush time is a startup-class problem rather than something to
    // degrade around.
    explicit SstableWriter(const std::string &path,
                           int bits_per_key = bloom::kDefaultBitsPerKey);

    // Appends one entry. Tombstones are written like any other entry: a flush
    // that dropped them would lose the deletes, and the keys they hide would
    // come back from an older file.
    //
    // Cannot fail on its own. A write error is remembered and reported by
    // finish(), so the caller has one place to check rather than a test after
    // every key.
    void add(std::string_view key, std::string_view value, bool tombstone);

    // Writes the index, the filter and the footer, then fsyncs.
    //
    // The fsync is not optional and not tidiness: a flush is only allowed to
    // truncate the WAL once this file is genuinely on disk. Truncating first
    // would leave a crash window where the data is in neither place.
    [[nodiscard]] bool finish();

    size_t entry_count() const { return entry_count_; }

  private:
    Fd fd_;
    std::string buf_;  // reused per entry, so a key costs no allocation
    uint64_t offset_ = 0;
    size_t entry_count_ = 0;
    bool failed_ = false;
    bool finished_ = false;
    int bits_per_key_;

    // Keys are copied rather than viewed. A view would tie this writer's
    // lifetime to whatever storage the caller passed, for the whole run rather
    // than the length of one add(); copies cost around 240 KB for a 4 MB
    // table, which is not worth a lifetime trap.
    std::vector<std::string> bloom_keys_;

    struct IndexEntry
    {
      std::string key;
      uint64_t offset = 0;
    };
    std::vector<IndexEntry> index_;
    uint64_t bytes_since_index_ = 0;
  };

  // Opens an SSTable for reading. The footer, index and filter are loaded into
  // memory; the data block stays on disk and is read one block at a time.
  class Sstable
  {
  public:
    // Throws if the file is missing, too short, or does not carry our magic.
    //
    // use_bloom exists for one reason: PROJECT.md 10 asks for read latency
    // with the filter on against off, and there is no way to measure what a
    // filter saves without being able to switch it off. Never false in normal
    // operation -- skipping it costs a disk read per table per miss and buys
    // nothing.
    explicit Sstable(const std::string &path, bool use_bloom = true);

    // The same three answers as Memtable::find, for the same reason: absent
    // here means keep looking in an older file, a tombstone means stop.
    struct Lookup
    {
      bool found = false;
      bool tombstone = false;
      std::string value;
    };

    Lookup get(std::string_view key) const;

    size_t entry_count() const { return entry_count_; }
    const std::string &path() const { return path_; }

  private:
    std::string path_;
    Fd fd_;
    std::string bloom_;
    struct IndexEntry
    {
      std::string key;
      uint64_t offset = 0;
    };
    std::vector<IndexEntry> index_;
    uint64_t data_end_ = 0;
    size_t entry_count_ = 0;
    bool use_bloom_ = true;
  };

} // namespace cachedb
