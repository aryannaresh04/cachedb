#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "store.h"

#include <unistd.h>

#include <string>

using cachedb::DelResult;
using cachedb::Record;
using cachedb::replay;
using cachedb::ReplayResult;
using cachedb::Store;
using cachedb::SyncPolicy;
using cachedb::Wal;

namespace {

struct TempLog {
  std::string path;
  explicit TempLog(const std::string& name)
      : path("store_test_" + name + ".log") {
    ::unlink(path.c_str());
  }
  ~TempLog() { ::unlink(path.c_str()); }
};

}  // namespace

TEST_CASE("set and get") {
  Store s;

  SUBCASE("a value comes back") {
    REQUIRE(s.set("username", "aryan"));
    REQUIRE(s.get("username").has_value());
    CHECK(*s.get("username") == "aryan");
    CHECK(s.exists("username"));
    CHECK(s.size() == 1);
  }

  SUBCASE("a key that was never written is absent") {
    CHECK_FALSE(s.get("nothing").has_value());
    CHECK_FALSE(s.exists("nothing"));
    CHECK(s.size() == 0);
  }

  SUBCASE("writing the same key twice overwrites") {
    REQUIRE(s.set("k", "first"));
    REQUIRE(s.set("k", "second"));
    CHECK(*s.get("k") == "second");
    CHECK(s.size() == 1);
  }

  SUBCASE("an empty value is stored, not treated as missing") {
    // GET has to distinguish these: an empty value replies "$0", a missing
    // key replies "$-1".
    REQUIRE(s.set("k", ""));
    REQUIRE(s.get("k").has_value());
    CHECK(*s.get("k") == "");
    CHECK(s.exists("k"));
  }

  SUBCASE("keys and values are binary safe") {
    const std::string key("a\0b", 3);
    const std::string value("x\0\r\ny", 5);
    REQUIRE(s.set(key, value));
    REQUIRE(s.get(key).has_value());
    CHECK(*s.get(key) == value);
    // The key is the whole three bytes, not the part before the NUL.
    CHECK_FALSE(s.get("a").has_value());
  }
}

TEST_CASE("delete") {
  Store s;
  REQUIRE(s.set("k", "v"));

  SUBCASE("deleting a live key reports it and hides it") {
    CHECK(s.del("k").was_live);
    CHECK_FALSE(s.get("k").has_value());
    CHECK_FALSE(s.exists("k"));
    CHECK(s.size() == 0);
  }

  SUBCASE("deleting a key that is not there removes nothing") {
    CHECK_FALSE(s.del("absent").was_live);
    CHECK_FALSE(s.get("absent").has_value());
    CHECK(s.size() == 1);  // "k" is untouched
  }

  SUBCASE("deleting twice only counts the first time") {
    CHECK(s.del("k").was_live);
    CHECK_FALSE(s.del("k").was_live);
    CHECK(s.size() == 0);
  }

  SUBCASE("a deleted key can be written again") {
    REQUIRE(s.del("k").durable);
    REQUIRE(s.set("k", "again"));
    REQUIRE(s.get("k").has_value());
    CHECK(*s.get("k") == "again");
    CHECK(s.size() == 1);
  }
}

TEST_CASE("a value handed out stays valid while other keys are written") {
  // std::map entries do not move, which is what makes it safe for get() to
  // return a view rather than a copy. Under ASan a mistake here is a
  // use-after-free, not a silent wrong answer.
  Store s;
  REQUIRE(s.set("k", "value"));
  const auto view = s.get("k");
  REQUIRE(view.has_value());

  for (int i = 0; i < 1000; ++i) {
    REQUIRE(s.set("filler" + std::to_string(i), "x"));
  }

  CHECK(*view == "value");
}

// --------------------------------------------------------------------
// The store on top of a log.
// --------------------------------------------------------------------

TEST_CASE("a store rebuilt from its log matches the one that wrote it") {
  TempLog log("recovery");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    Store s(&wal);
    REQUIRE(s.set("a", "1"));
    REQUIRE(s.set("b", "2"));
    REQUIRE(s.set("a", "1-updated"));  // an overwrite, replayed in order
    REQUIRE(s.del("b").durable);       // a delete that must stay deleted
    REQUIRE(s.set("c", "3"));
  }

  Store recovered;
  ReplayResult r = replay(log.path, [&](const Record& rec) {
    recovered.apply(rec);
  });
  CHECK(r.records == 5);
  CHECK_FALSE(r.truncated);

  REQUIRE(recovered.get("a").has_value());
  CHECK(*recovered.get("a") == "1-updated");
  // The delete has to survive recovery. Replaying only the sets would bring
  // "b" back from the dead, which is the failure the tombstone exists for.
  CHECK_FALSE(recovered.get("b").has_value());
  REQUIRE(recovered.get("c").has_value());
  CHECK(*recovered.get("c") == "3");
  CHECK(recovered.size() == 2);
}

TEST_CASE("recovery does not write the log back into itself") {
  TempLog log("no_rewrite");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    Store s(&wal);
    REQUIRE(s.set("a", "1"));
    REQUIRE(s.set("b", "2"));
  }

  uint64_t after_first_run = 0;
  { after_first_run = Wal(log.path, SyncPolicy::kNo).size(); }

  // The real startup sequence: replay into the store, then open the log for
  // appending and hand it over. Going through set() during replay would append
  // every record a second time and the log would double on every restart.
  {
    Store s;
    replay(log.path, [&](const Record& rec) { s.apply(rec); });
    Wal wal(log.path, SyncPolicy::kAlways);
    s.set_wal(&wal);
    CHECK(s.size() == 2);
    CHECK(wal.size() == after_first_run);
  }
  CHECK(Wal(log.path, SyncPolicy::kNo).size() == after_first_run);
}

TEST_CASE("a write that cannot be logged is refused and changes nothing") {
  // /dev/full accepts a write() and then fails it with ENOSPC, which
  // exercises a real log failure without a fake Wal or a virtual call on the
  // write path. Linux only; on macOS this case does not run.
  if (::access("/dev/full", W_OK) != 0) return;

  Wal wal("/dev/full", SyncPolicy::kNo);
  Store s(&wal);

  CHECK_FALSE(s.set("k", "v"));
  // Refusing is only half of it. If the table had been changed anyway, a
  // client that saw the error would still find the value on the next GET --
  // and a restart would then lose it, which is the divergence the ordering
  // in Store::set exists to prevent.
  CHECK_FALSE(s.get("k").has_value());
  CHECK(s.size() == 0);

  const DelResult r = s.del("k");
  CHECK_FALSE(r.durable);
  CHECK_FALSE(r.was_live);
}

TEST_CASE("a store with no log still applies every write") {
  // Not a degraded mode to guard against: it is what the unit tests use and
  // what M1 was in its entirety.
  Store s;
  CHECK(s.set("k", "v"));
  CHECK(s.del("k").durable);
}
