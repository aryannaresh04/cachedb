#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cachedb
{

  // A bloom filter over the keys of one SSTable, in the form it is stored in:
  // a flat block of bytes.
  //
  // It answers "definitely not present" or "possibly present". False positives
  // cost a wasted disk read; false negatives would lose data, because the read
  // path would skip a file that really does hold the key. Everything here is
  // arranged around never producing one -- see may_contain on a block it
  // cannot make sense of.
  //
  // PROJECT.md 6.5 puts this block inside each SSTable, so the block is
  // self-describing: nothing outside it needs to remember how it was built.
  namespace bloom
  {

    // 10 bits per key with the matching k gives roughly a 1% false positive
    // rate, which is the usual place to sit: the next 1% costs another 10 bits
    // per key for every key in the table.
    inline constexpr int kDefaultBitsPerKey = 10;

    // The number of probes that minimises the false positive rate for a given
    // bits-per-key is (m/n) * ln 2. At 10 bits that is 6.93, hence the k = 7 in
    // PROJECT.md 6.6. Deriving it rather than hard-coding 7 keeps the filter
    // optimal when bits_per_key is changed to measure the curve.
    int optimal_probes(int bits_per_key);

    // Builds the block for `keys`. Duplicates are harmless. An empty key set
    // still produces a valid block, one that answers "not present" to
    // everything.
    //
    // Layout: the bit array, then a single trailing byte holding k. The number
    // of bits is implied by the length, so a reader needs no separate header.
    std::string build(const std::vector<std::string_view> &keys,
                      int bits_per_key = kDefaultBitsPerKey);

    // False means the key is definitely not in the set the block was built
    // from. True means it might be.
    //
    // A block too short to be meaningful answers true, not false. Guessing
    // "absent" on damaged bytes would turn a corrupt filter into missing data;
    // guessing "present" only costs the read the filter was there to avoid.
    bool may_contain(std::string_view block, std::string_view key);

    // Exposed for the SSTable, which sizes its own block, and for the tests
    // that measure the false positive rate against the theoretical curve.
    size_t bits_for(size_t key_count, int bits_per_key);

  } // namespace bloom

} // namespace cachedb
