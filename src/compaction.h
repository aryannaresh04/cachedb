#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "bloom.h"
#include "sstable.h"

namespace cachedb
{

  // Merges several SSTables into one. PROJECT.md 6.7.
  //
  // `inputs` is newest first, the same order Store keeps its tables in and the
  // same order a read consults them. That order is what resolves a duplicate:
  // the first table holding a key is the one whose version survives, so the
  // merge never has to compare timestamps and cannot disagree with the read
  // path about which value is current.
  //
  // `drop_obsolete` says the inputs reach all the way down -- there is no
  // older table left once this merge lands. Only then may a tombstone or an
  // expired entry be discarded. Dropping one while an older table survives is
  // the resurrection bug in its original form: the marker disappears and the
  // value it was hiding comes back.
  struct CompactionResult
  {
    bool ok = false;
    size_t written = 0;
    // Superseded duplicates, plus tombstones and expired entries when
    // drop_obsolete allowed them to go. The point of the whole exercise.
    size_t dropped = 0;
  };

  CompactionResult compact(const std::vector<const Sstable *> &inputs,
                           const std::string &out_path, int bits_per_key,
                           bool drop_obsolete, int64_t now_ms);

} // namespace cachedb
