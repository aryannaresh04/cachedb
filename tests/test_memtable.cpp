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

TEST_CASE("an expiry is stored, and never interpreted") {
  // The memtable holds the stamp and nothing more. Comparing it against a
  // clock is Store's job, so that the memtable and the SSTables underneath it
  // cannot disagree about what time it is -- which is how a key expires in
  // one layer while the layer it was hiding is still serving the old value.
  Memtable m;
  m.set("k", "v", 1700000000000);

  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK(e->expires_at_ms == 1700000000000);
  // Long past, and the memtable still reports it live. Nothing here reads a
  // clock, so nothing here can decide otherwise.
  CHECK(m.live_count() == 1);
}

TEST_CASE("a key written with no expiry has none") {
  Memtable m;
  m.set("k", "v");
  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK(e->expires_at_ms == 0);
}

TEST_CASE("a plain overwrite drops an existing expiry") {
  // Redis's rule, and a behaviour rather than an accident: SET without EX
  // clears the TTL. Carrying the old stamp forward would look conservative
  // and would in fact be wrong -- a client that rewrites a key to keep it
  // would watch it vanish on the old schedule.
  Memtable m;
  m.set("k", "v", 1700000000000);
  m.set("k", "v2");

  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK(e->value == "v2");
  CHECK(e->expires_at_ms == 0);
}

TEST_CASE("an overwrite can replace one expiry with another") {
  Memtable m;
  m.set("k", "v", 1000);
  m.set("k", "v", 2000);
  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK(e->expires_at_ms == 2000);
}

TEST_CASE("a tombstone carries no expiry") {
  // "Absent, until it stops being absent" is not a state this engine has. If
  // a stale stamp survived on a tombstone, a later reader that checked the
  // expiry before the flag would resurrect a deleted key.
  Memtable m;
  m.set("k", "v", 1700000000000);
  CHECK(m.del("k"));

  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK(e->tombstone);
  CHECK(e->expires_at_ms == 0);
}

TEST_CASE("writing over a tombstone can set an expiry") {
  Memtable m;
  CHECK_FALSE(m.del("k"));
  m.set("k", "v", 4242);

  const auto* e = m.find("k");
  REQUIRE(e != nullptr);
  CHECK_FALSE(e->tombstone);
  CHECK(e->expires_at_ms == 4242);
  CHECK(m.live_count() == 1);
}

TEST_CASE("the sweep turns expired entries into tombstones") {
  Memtable m;
  m.set("expired", "v", 1000);
  m.set("alive", "v", 9000);
  m.set("forever", "v");

  CHECK(m.sweep_expired(/*now=*/5000, /*budget=*/100) == 1);

  const auto* e = m.find("expired");
  REQUIRE(e != nullptr);
  CHECK(e->tombstone);
  CHECK(e->value.empty());
  CHECK(e->expires_at_ms == 0);  // a tombstone has no lifetime

  CHECK_FALSE(m.find("alive")->tombstone);
  CHECK_FALSE(m.find("forever")->tombstone);
  CHECK(m.live_count() == 2);
}

TEST_CASE("the sweep marks rather than erases") {
  // The rule that keeps expiry from resurrecting data. The key may still sit
  // in an older SSTable, and this row is the only thing that will hide it.
  // Erasing would let the older copy come back -- a value returning from the
  // dead because it expired.
  Memtable m;
  m.set("k", "v", 1000);
  CHECK(m.sweep_expired(5000, 100) == 1);

  CHECK(m.entry_count() == 1);       // the row is still there
  CHECK(m.find("k") != nullptr);     // and still says something
  CHECK(m.find("k")->tombstone);     // namely: absent, stop looking
}

TEST_CASE("the sweep examines no more than its budget") {
  Memtable m;
  for (int i = 0; i < 10; ++i) {
    m.set("key" + std::to_string(i), "v", 1000);
  }
  // Three entries examined, so at most three swept, however many have run out.
  CHECK(m.sweep_expired(5000, 3) == 3);
  CHECK(m.live_count() == 7);
}

TEST_CASE("the sweep resumes where it stopped") {
  Memtable m;
  for (int i = 0; i < 6; ++i) {
    m.set("key" + std::to_string(i), "v", 1000);
  }
  // One at a time. Without a cursor every call would re-examine key0 and the
  // other five would never be reached.
  size_t total = 0;
  for (int i = 0; i < 6; ++i) total += m.sweep_expired(5000, 1);

  CHECK(total == 6);
  CHECK(m.live_count() == 0);
}

TEST_CASE("the sweep wraps round to the start") {
  Memtable m;
  m.set("a", "v");              // no expiry, never swept
  m.set("b", "v");
  // Walk past the end, which resets the cursor.
  CHECK(m.sweep_expired(5000, 100) == 0);

  // A key that expires after the cursor has already passed the end still gets
  // swept on a later pass, rather than being stranded behind it.
  m.set("a", "v", 1000);
  CHECK(m.sweep_expired(5000, 100) == 1);
  CHECK(m.find("a")->tombstone);
}

TEST_CASE("the sweep leaves tombstones and unexpired keys alone") {
  Memtable m;
  CHECK_FALSE(m.del("already_dead"));
  m.set("forever", "v");
  m.set("later", "v", 9000);

  CHECK(m.sweep_expired(5000, 100) == 0);
  CHECK(m.live_count() == 2);
}

TEST_CASE("sweeping an empty memtable does nothing and does not crash") {
  Memtable m;
  CHECK(m.sweep_expired(5000, 100) == 0);
  m.set("k", "v", 1000);
  CHECK(m.sweep_expired(5000, 100) == 1);
}

TEST_CASE("clearing resets the sweep cursor") {
  Memtable m;
  for (int i = 0; i < 4; ++i) m.set("key" + std::to_string(i), "v", 1000);
  CHECK(m.sweep_expired(5000, 2) == 2);  // cursor now part way through
  m.clear();

  // If the cursor had survived the clear, this key -- sorting before where
  // the cursor stopped -- would be skipped until the walk wrapped round.
  m.set("aaa", "v", 1000);
  CHECK(m.sweep_expired(5000, 1) == 1);
}
