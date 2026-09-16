#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "memtable.h"

#include <string>
#include <utility>
#include <vector>

using cachedb::Memtable;

namespace {

std::vector<std::pair<std::string, bool>> walk(const Memtable& m) {
  std::vector<std::pair<std::string, bool>> seen;
  m.for_each([&](std::string_view key, const Memtable::Entry& e) {
    seen.emplace_back(std::string(key), e.tombstone);
  });
  return seen;
}

}  // namespace

TEST_CASE("find answers three different things") {
  Memtable m;
  m.set("live", "v");
  m.del("dead");

  SUBCASE("a key never written is null, meaning keep looking") {
    CHECK(m.find("absent") == nullptr);
  }

  SUBCASE("a live key is an entry carrying its value") {
    const auto* e = m.find("live");
    REQUIRE(e != nullptr);
    CHECK_FALSE(e->tombstone);
    CHECK(e->value == "v");
  }

  SUBCASE("a deleted key is an entry, not a null") {
    // The distinction this whole interface exists for. If a delete looked the
    // same as a key that was never here, the read path would fall through to
    // an older SSTable, find what the tombstone was hiding, and hand a client
    // back data it deleted.
    const auto* e = m.find("dead");
    REQUIRE(e != nullptr);
    CHECK(e->tombstone);
  }
}

TEST_CASE("deleting a key that is absent still records a tombstone") {
  Memtable m;
  CHECK_FALSE(m.del("never-existed"));  // nothing was live
  const auto* e = m.find("never-existed");
  REQUIRE(e != nullptr);
  CHECK(e->tombstone);
  CHECK(m.entry_count() == 1);  // it occupies a row
  CHECK(m.live_count() == 0);   // but is not a key
}

TEST_CASE("counts track live keys and stored entries separately") {
  Memtable m;
  m.set("a", "1");
  m.set("b", "2");
  CHECK(m.live_count() == 2);
  CHECK(m.entry_count() == 2);

  CHECK(m.del("a"));
  CHECK(m.live_count() == 1);
  // Still two rows: the tombstone has to be written by a flush.
  CHECK(m.entry_count() == 2);

  CHECK_FALSE(m.del("a"));  // already dead, nothing live to report
  CHECK(m.live_count() == 1);

  m.set("a", "revived");
  CHECK(m.live_count() == 2);
  CHECK(m.entry_count() == 2);
  CHECK_FALSE(m.find("a")->tombstone);
  CHECK(m.find("a")->value == "revived");
}

TEST_CASE("for_each walks in key order and includes tombstones") {
  Memtable m;
  m.set("delta", "4");
  m.set("alpha", "1");
  m.set("charlie", "3");
  m.set("bravo", "2");
  m.del("charlie");
  m.del("echo");  // never existed; still a row

  const auto seen = walk(m);
  REQUIRE(seen.size() == 5);
  // Sorted, which is what lets a flush produce an SSTable in one pass.
  CHECK(seen[0].first == "alpha");
  CHECK(seen[1].first == "bravo");
  CHECK(seen[2].first == "charlie");
  CHECK(seen[3].first == "delta");
  CHECK(seen[4].first == "echo");
  // A flush that skipped these would drop the deletes and the keys would come
  // back from an older file.
  CHECK(seen[2].second);
  CHECK(seen[4].second);
}

TEST_CASE("the footprint estimate grows and never lies low") {
  Memtable m;
  CHECK(m.bytes() == 0);

  m.set("key", "value");
  const size_t after_one = m.bytes();
  // Key and value bytes plus per-entry overhead, so comfortably more than the
  // eight bytes of payload.
  CHECK(after_one > std::string("key").size() + std::string("value").size());

  m.set("key2", "value2");
  CHECK(m.bytes() > after_one);

  SUBCASE("growing a value grows the estimate") {
    const size_t before = m.bytes();
    m.set("key", std::string(1000, 'x'));
    CHECK(m.bytes() >= before + 1000 - 5);
  }

  SUBCASE("shrinking a value does not shrink the estimate") {
    // std::string does not hand capacity back on assign, so the memory is
    // still held. An estimate that dropped here would read low, and a flush
    // threshold that reads low is a memtable bigger than its limit claims.
    m.set("key", std::string(1000, 'x'));
    const size_t big = m.bytes();
    m.set("key", "s");
    CHECK(m.bytes() == big);
  }

  SUBCASE("deleting does not shrink the estimate either") {
    m.set("key", std::string(1000, 'x'));
    const size_t big = m.bytes();
    m.del("key");
    CHECK(m.bytes() == big);
  }

  SUBCASE("clear resets it") {
    m.clear();
    CHECK(m.bytes() == 0);
    CHECK(m.entry_count() == 0);
    CHECK(m.live_count() == 0);
    CHECK(m.empty());
  }
}

TEST_CASE("a 4 MB budget holds a plausible number of keys") {
  // Sanity on the overhead constant rather than a hard promise. PROJECT.md 8
  // flushes at 4 MB; if the estimate were wildly off, that threshold would
  // mean something very different from what it looks like it means.
  Memtable m;
  int keys = 0;
  while (m.bytes() < 4u * 1024 * 1024) {
    m.set("user:session:" + std::to_string(keys), std::string(64, 'v'));
    ++keys;
  }
  CHECK(keys > 5000);
  CHECK(keys < 40000);
  MESSAGE("4 MB of memtable holds " << keys << " keys of this shape");
}

TEST_CASE("keys and values are binary safe") {
  Memtable m;
  const std::string key("a\0b", 3);
  const std::string value("x\0\r\ny", 5);
  m.set(key, value);
  const auto* e = m.find(key);
  REQUIRE(e != nullptr);
  CHECK(e->value == value);
  // The key is all three bytes, not the part before the NUL.
  CHECK(m.find("a") == nullptr);
}

TEST_CASE("an entry stays put while other keys are written") {
  // The pointer contract. std::map nodes do not move, which is what lets
  // find() hand back a pointer and Store hand back a view into it. Under ASan
  // a mistake here is a use-after-free rather than a silently wrong answer.
  Memtable m;
  m.set("k", "value");
  const auto* e = m.find("k");
  REQUIRE(e != nullptr);

  for (int i = 0; i < 1000; ++i) m.set("filler" + std::to_string(i), "x");

  CHECK(e->value == "value");
  CHECK(e == m.find("k"));
}
