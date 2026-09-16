#include "bloom.h"

#include <algorithm>
#include <cmath>

namespace cachedb {
namespace bloom {
namespace {

// FNV-1a, 64-bit. Hand-rolled because PROJECT.md 3 allows no dependency, and
// it is four lines.
uint64_t fnv1a(std::string_view data) {
  uint64_t h = 0xcbf29ce484222325ull;
  for (const char c : data) {
    h ^= static_cast<unsigned char>(c);
    h *= 0x100000001b3ull;
  }
  return h;
}

// The finaliser from splitmix64: three shifts and two multiplies that spread
// every input bit across the whole word.
//
// Measured rather than assumed, and the result was not what it looked like it
// would be. FNV-1a on its own is not worse here -- it is *unpredictable*.
// Across four key shapes (decimal strings, sequential 8-byte integers,
// integers differing only in their high bytes, and long shared prefixes) it
// ranged from 0.110 to 0.153 at 4 bits per key, landing either side of the
// 0.147 the formula predicts; on structured keys it sometimes beat the curve,
// because the structure happened to spread the probes more evenly than chance
// would. With the finaliser every one of those sets came in at 0.146 to 0.149.
//
// That predictability is the whole reason it is here. The probe positions are
// derived by splitting one hash into two halves, so any structure left in
// those halves becomes structure in the probes, and the error rate starts
// depending on what the keys happen to look like. Five instructions buy a
// filter whose measured rate follows the curve whatever it is fed -- which is
// what lets bits_per_key mean what 6.6 says it means, instead of something to
// be re-measured for every workload.
uint64_t mix(uint64_t z) {
  z ^= z >> 30;
  z *= 0xbf58476d1ce4e5b9ull;
  z ^= z >> 27;
  z *= 0x94d049bb133111ebull;
  z ^= z >> 31;
  return z;
}

struct Probe {
  uint32_t h1;
  uint32_t h2;
};

Probe probe_for(std::string_view key) {
  const uint64_t h = mix(fnv1a(key));
  Probe p;
  p.h1 = static_cast<uint32_t>(h);
  // Double hashing (Kirsch-Mitzenmacher): the k probe positions are
  // h1 + i*h2, which costs one hash instead of k independent ones and is
  // provably no worse asymptotically.
  //
  // h2 is forced odd. It is the stride, and a stride sharing a factor with the
  // bit count can only ever reach a fraction of the array -- an even stride
  // over a power-of-two sized filter visits half the bits, which would quietly
  // halve the usable filter. Odd is coprime with every power of two, so the
  // walk can reach everything.
  p.h2 = static_cast<uint32_t>(h >> 32) | 1u;
  return p;
}

}  // namespace

int optimal_probes(int bits_per_key) {
  // ln 2 = 0.693. Clamped: k must be at least 1 to be a filter at all, and
  // past ~30 the probes cost more than the shrinking error rate saves.
  const int k = static_cast<int>(static_cast<double>(bits_per_key) * 0.69 + 0.5);
  return std::min(30, std::max(1, k));
}

size_t bits_for(size_t key_count, int bits_per_key) {
  const size_t bits = key_count * static_cast<size_t>(std::max(1, bits_per_key));
  // A floor keeps a table with one or two keys from producing a filter so
  // small that every query collides, and keeps the modulo below off zero.
  return std::max<size_t>(64, (bits + 7) / 8 * 8);
}

std::string build(const std::vector<std::string_view>& keys, int bits_per_key) {
  const size_t bits = bits_for(keys.size(), bits_per_key);
  const int k = optimal_probes(bits_per_key);

  std::string block(bits / 8, '\0');
  for (const std::string_view key : keys) {
    const Probe p = probe_for(key);
    uint32_t pos = p.h1;
    for (int i = 0; i < k; ++i) {
      const size_t bit = pos % bits;
      block[bit / 8] = static_cast<char>(
          static_cast<unsigned char>(block[bit / 8]) | (1u << (bit % 8)));
      pos += p.h2;
    }
  }

  // k last, so the block carries the one parameter a reader cannot infer from
  // its length.
  block.push_back(static_cast<char>(k));
  return block;
}

bool may_contain(std::string_view block, std::string_view key) {
  // Nothing to read, or nothing but the k byte. Cannot answer, so answer the
  // way that cannot lose data.
  if (block.size() < 2) return true;

  const int k = static_cast<unsigned char>(block.back());
  if (k < 1 || k > 30) return true;  // not a block we wrote

  const size_t bits = (block.size() - 1) * 8;
  const Probe p = probe_for(key);
  uint32_t pos = p.h1;
  for (int i = 0; i < k; ++i) {
    const size_t bit = pos % bits;
    // One clear bit is proof of absence: every key in the set set all k of
    // its bits, so a key missing any one of them was never added.
    if ((static_cast<unsigned char>(block[bit / 8]) & (1u << (bit % 8))) == 0) {
      return false;
    }
    pos += p.h2;
  }
  return true;
}

}  // namespace bloom
}  // namespace cachedb
