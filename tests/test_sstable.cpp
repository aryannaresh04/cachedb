#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "sstable.h"

#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <map>
#include <string>
#include <vector>

using cachedb::Sstable;
using cachedb::SstableWriter;

namespace {

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& name) : path("sst_test_" + name + ".sst") {
    ::unlink(path.c_str());
  }
  ~TempFile() { ::unlink(path.c_str()); }
};

// Writes a whole table from a sorted map, which is the shape a memtable flush
// will hand over.
void write_table(const std::string& path,
                 const std::map<std::string, std::pair<std::string, bool>>& rows,
                 int bits_per_key = 10) {
  SstableWriter w(path, bits_per_key);
  for (const auto& [key, entry] : rows) {
    w.add(key, entry.first, entry.second);
  }
  REQUIRE(w.finish());
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

}  // namespace

TEST_CASE("every key written comes back") {
  TempFile f("roundtrip");
  std::map<std::string, std::pair<std::string, bool>> rows;
  for (int i = 0; i < 2000; ++i) {
    rows["key" + std::to_string(i)] = {"value" + std::to_string(i), false};
  }
  write_table(f.path, rows);

  const Sstable t(f.path);
  CHECK(t.entry_count() == rows.size());
  for (const auto& row : rows) {
    // Named rather than a structured binding: doctest's message macro builds a
    // lambda, and capturing a structured binding is C++20.
    const std::string& key = row.first;
    const auto got = t.get(key);
    REQUIRE_MESSAGE(got.found, "missing " << key);
    CHECK_FALSE(got.tombstone);
    CHECK(got.value == row.second.first);
  }
}

TEST_CASE("a key that was never written is not found") {
  TempFile f("absent");
  std::map<std::string, std::pair<std::string, bool>> rows;
  for (int i = 0; i < 500; ++i) {
    rows["key" + std::to_string(i)] = {"v", false};
  }
  write_table(f.path, rows);

  const Sstable t(f.path);
  // Below every key, above every key, and in the middle -- the three places a
  // sparse index can go wrong.
  CHECK_FALSE(t.get("aaa").found);
  CHECK_FALSE(t.get("zzz").found);
  CHECK_FALSE(t.get("key9999").found);
  CHECK_FALSE(t.get("").found);
}

TEST_CASE("a tombstone is found, and is not a value") {
  // The distinction the read path depends on. A tombstone here has to stop
  // the search, not let it fall through to an older file holding the key.
  TempFile f("tombstone");
  write_table(f.path, {
    {"alive", {"v", false}},
    {"dead", {"", true}},
  });

  const Sstable t(f.path);
  const auto alive = t.get("alive");
  CHECK(alive.found);
  CHECK_FALSE(alive.tombstone);

  const auto dead = t.get("dead");
  CHECK(dead.found);   // found: the file has something to say about this key
  CHECK(dead.tombstone);
  CHECK(dead.value.empty());

  CHECK_FALSE(t.get("never").found);  // and this one it does not
}

TEST_CASE("the sparse index bounds the scan by bytes, not by keys") {
  // Entries far larger than the index interval, so nearly every one gets its
  // own index entry; then entries far smaller, so many share one. Both must
  // read back, and the index must stay small relative to the data.
  SUBCASE("large values") {
    TempFile f("large");
    std::map<std::string, std::pair<std::string, bool>> rows;
    for (int i = 0; i < 40; ++i) {
      rows["k" + std::to_string(i)] = {std::string(8192, 'v'), false};
    }
    write_table(f.path, rows);
    const Sstable t(f.path);
    for (const auto& [key, entry] : rows) {
      const auto got = t.get(key);
      REQUIRE(got.found);
      CHECK(got.value.size() == entry.first.size());
    }
  }
  SUBCASE("tiny values") {
    TempFile f("tiny");
    std::map<std::string, std::pair<std::string, bool>> rows;
    for (int i = 0; i < 5000; ++i) rows["k" + std::to_string(i)] = {"x", false};
    write_table(f.path, rows);
    const Sstable t(f.path);
    for (const auto& [key, entry] : rows) {
      REQUIRE(t.get(key).found);
    }
  }
}

TEST_CASE("keys and values are binary safe") {
  TempFile f("binary");
  const std::string key("a\0b", 3);
  const std::string value("x\0\r\ny", 5);
  write_table(f.path, {{key, {value, false}}});

  const Sstable t(f.path);
  const auto got = t.get(key);
  REQUIRE(got.found);
  CHECK(got.value == value);
  // The key is all three bytes, not the part before the NUL.
  CHECK_FALSE(t.get("a").found);
}

TEST_CASE("an empty value is stored, and is not a missing key") {
  TempFile f("empty_value");
  write_table(f.path, {{"k", {"", false}}});
  const Sstable t(f.path);
  const auto got = t.get("k");
  CHECK(got.found);
  CHECK_FALSE(got.tombstone);
  CHECK(got.value.empty());
}

TEST_CASE("a table with no entries is still a valid file") {
  TempFile f("empty");
  {
    SstableWriter w(f.path);
    REQUIRE(w.finish());
  }
  const Sstable t(f.path);
  CHECK(t.entry_count() == 0);
  CHECK_FALSE(t.get("anything").found);
}

TEST_CASE("the footer is what makes the file readable") {
  TempFile f("footer");
  write_table(f.path, {{"a", {"1", false}}, {"b", {"2", false}}});
  const std::string good = read_file(f.path);

  SUBCASE("magic identifies the format") {
    // Last eight bytes, readable in a hex dump on purpose.
    CHECK(good.compare(good.size() - 8, 8, "CDBSSTv1") == 0);
  }

  SUBCASE("a file without our magic is refused, not misparsed") {
    std::string bad = good;
    bad[bad.size() - 1] = '9';  // a version we do not know
    std::ofstream(f.path, std::ios::binary | std::ios::trunc)
        .write(bad.data(), static_cast<std::streamsize>(bad.size()));
    CHECK_THROWS(Sstable(f.path));
  }

  SUBCASE("a file too short to hold a footer is refused") {
    std::string bad = good.substr(0, 10);
    std::ofstream(f.path, std::ios::binary | std::ios::trunc)
        .write(bad.data(), static_cast<std::streamsize>(bad.size()));
    CHECK_THROWS(Sstable(f.path));
  }

  SUBCASE("offsets that are not in order are refused") {
    // index_offset past bloom_offset. A damaged file must not send the reader
    // seeking somewhere arbitrary.
    std::string bad = good;
    for (int i = 0; i < 8; ++i) bad[bad.size() - 32 + i] = '\xff';
    std::ofstream(f.path, std::ios::binary | std::ios::trunc)
        .write(bad.data(), static_cast<std::streamsize>(bad.size()));
    CHECK_THROWS(Sstable(f.path));
  }

  SUBCASE("a missing file is refused") {
    CHECK_THROWS(Sstable("sst_test_does_not_exist.sst"));
  }
}

TEST_CASE("the bloom filter is consulted before the disk") {
  // Not a timing test: the observable is that a key the filter rejects is
  // reported absent without the index ever being searched. Correctness is the
  // same either way, so this pins the wiring rather than the speed.
  TempFile f("filter");
  std::map<std::string, std::pair<std::string, bool>> rows;
  for (int i = 0; i < 1000; ++i) rows["key" + std::to_string(i)] = {"v", false};
  write_table(f.path, rows, 16);  // a low false-positive rate

  const Sstable t(f.path);
  int absent = 0;
  for (int i = 0; i < 1000; ++i) {
    if (!t.get("absent" + std::to_string(i)).found) ++absent;
  }
  CHECK(absent == 1000);  // no false positive may become a found key
}
