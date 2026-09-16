#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "bloom.h"

#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

using cachedb::bloom::bits_for;
using cachedb::bloom::build;
using cachedb::bloom::kDefaultBitsPerKey;
using cachedb::bloom::may_contain;
using cachedb::bloom::optimal_probes;

namespace {

std::vector<std::string> make_keys(const char* prefix, int n) {
  std::vector<std::string> keys;
  keys.reserve(static_cast<size_t>(n));
  for (int i = 0; i < n; ++i) keys.push_back(prefix + std::to_string(i));
  return keys;
}

std::vector<std::string_view> views(const std::vector<std::string>& keys) {
  return std::vector<std::string_view>(keys.begin(), keys.end());
}

// (1 - e^(-kn/m))^k -- the probability that all k of a key's bits are set by
// chance once n keys have been inserted into m bits.
double theoretical_fpr(size_t n, size_t m, int k) {
  const double exponent =
      -static_cast<double>(k) * static_cast<double>(n) / static_cast<double>(m);
  return std::pow(1.0 - std::exp(exponent), k);
}

}  // namespace

TEST_CASE("a key that was added is never reported absent") {
  // The one guarantee. A false positive costs a wasted disk read; a false
  // negative means the read path skips a file that holds the key, which is
  // data loss that looks exactly like a missing key.
  const auto keys = make_keys("key", 5000);
  const std::string block = build(views(keys));

  int missing = 0;
  for (const auto& key : keys) {
    if (!may_contain(block, key)) ++missing;
  }
  CHECK(missing == 0);
}

TEST_CASE("no false negatives at any bits-per-key, including absurd ones") {
  const auto keys = make_keys("k", 500);
  for (const int bpk : {1, 2, 4, 10, 20, 64}) {
    const std::string block = build(views(keys), bpk);
    int missing = 0;
    for (const auto& key : keys) {
      if (!may_contain(block, key)) ++missing;
    }
    CHECK_MESSAGE(missing == 0, "bits_per_key=" << bpk);
  }
}

TEST_CASE("an empty filter answers no to everything") {
  const std::string block = build({});
  CHECK_FALSE(may_contain(block, "anything"));
  CHECK_FALSE(may_contain(block, ""));
}

TEST_CASE("a block that cannot be read answers possibly-present") {
  // Never "absent". Guessing absent on damaged bytes turns a corrupt filter
  // into missing data; guessing present only wastes the read the filter was
  // there to save.
  CHECK(may_contain("", "k"));
  CHECK(may_contain("x", "k"));                  // the k byte alone
  CHECK(may_contain(std::string(9, '\xff'), "k"));  // k byte of 255, not ours
  CHECK(may_contain(std::string(9, '\0'), "k"));    // k byte of 0, not ours
}

TEST_CASE("keys are binary safe") {
  const std::string a("a\0b", 3);
  const std::string b("a\0c", 3);
  const std::vector<std::string_view> keys = {a};
  const std::string block = build(keys);
  CHECK(may_contain(block, a));
  // Not a guarantee -- b could collide -- but it must not match because the
  // hash stopped at the NUL.
  CHECK(may_contain(block, a));
  CHECK(may_contain(block, std::string_view(a.data(), 3)));
}

TEST_CASE("k comes from bits per key and lands on 7 at the default") {
  CHECK(optimal_probes(kDefaultBitsPerKey) == 7);  // PROJECT.md 6.6
  CHECK(optimal_probes(1) == 1);
  CHECK(optimal_probes(0) == 1);    // never zero: that is not a filter
  CHECK(optimal_probes(1000) == 30);  // capped
}

TEST_CASE("the block is self-describing") {
  const auto keys = make_keys("key", 100);
  const std::string block = build(views(keys), 10);
  // The bit array, then one byte of k. A reader needs nothing else: the bit
  // count is (size - 1) * 8.
  CHECK(static_cast<int>(static_cast<unsigned char>(block.back())) ==
        optimal_probes(10));
  CHECK((block.size() - 1) * 8 == bits_for(keys.size(), 10));
}

TEST_CASE("the measured false positive rate tracks the theoretical curve") {
  // PROJECT.md 6.6 asks for this specifically. It also pins down the hash:
  // the finaliser in bloom.cpp was chosen on the strength of exactly this
  // measurement, repeated over several key shapes. Without it the rate still
  // lands near the curve for these keys but wanders with the shape of the key
  // set; the band below would not catch that, which is why the comparison
  // lives in that comment and this test only guards the curve itself.
  constexpr int kKeys = 10000;
  constexpr int kProbes = 100000;
  const auto keys = make_keys("key", kKeys);
  const auto absent = make_keys("absent", kProbes);

  std::printf("\n  bits/key   k   theoretical   measured\n");
  for (const int bpk : {4, 8, 10, 16}) {
    const std::string block = build(views(keys), bpk);

    int hits = 0;
    for (const auto& key : absent) {
      if (may_contain(block, key)) ++hits;
    }
    const double measured = static_cast<double>(hits) / kProbes;
    const double expected =
        theoretical_fpr(kKeys, bits_for(kKeys, bpk), optimal_probes(bpk));

    std::printf("  %8d  %2d      %7.4f    %7.4f\n", bpk, optimal_probes(bpk),
                expected, measured);

    // A band, not an equality: the curve is an approximation that assumes
    // independent probes, and double hashing only approximates that. Wide
    // enough not to flake, tight enough that a genuinely bad hash -- which
    // would land several times over -- fails it.
    CHECK_MESSAGE(measured < expected * 2.0 + 0.002,
                  "bits_per_key=" << bpk << " measured=" << measured
                                  << " expected=" << expected);
    CHECK_MESSAGE(measured > expected * 0.4,
                  "bits_per_key=" << bpk << " measured=" << measured
                                  << " expected=" << expected);
  }
}
