#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "store.h"

#include <unistd.h>

#include <filesystem>

#include <string>

using cachedb::DelBatchResult;
using cachedb::DelResult;
using cachedb::Record;
using cachedb::replay;
using cachedb::ReplayResult;
using cachedb::Store;
using cachedb::StoreOptions;
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

struct TempDir {
  std::string path;
  TempDir() {
    char tmpl[] = "store_test_dir_XXXXXX";
    path = ::mkdtemp(tmpl);
  }
  ~TempDir() {
    std::error_code ignored;
    std::filesystem::remove_all(path, ignored);
  }
};

StoreOptions opts(const TempDir& d, size_t limit = 4u * 1024 * 1024) {
  StoreOptions o;
  o.dir = d.path;
  o.memtable_limit_bytes = limit;
  return o;
}

}  // namespace

TEST_CASE("set and get") {
  Store s;

  SUBCASE("a value comes back") {
    REQUIRE(s.set("username", "aryan"));
    REQUIRE(s.get("username").has_value());
    CHECK(s.get("username")->get() == "aryan");
    CHECK(s.exists("username"));
    CHECK(s.memtable_keys() == 1);
  }

  SUBCASE("a key that was never written is absent") {
    CHECK_FALSE(s.get("nothing").has_value());
    CHECK_FALSE(s.exists("nothing"));
    CHECK(s.memtable_keys() == 0);
  }

  SUBCASE("writing the same key twice overwrites") {
    REQUIRE(s.set("k", "first"));
    REQUIRE(s.set("k", "second"));
    CHECK(s.get("k")->get() == "second");
    CHECK(s.memtable_keys() == 1);
  }

  SUBCASE("an empty value is stored, not treated as missing") {
    // GET has to distinguish these: an empty value replies "$0", a missing
    // key replies "$-1".
    REQUIRE(s.set("k", ""));
    REQUIRE(s.get("k").has_value());
    CHECK(s.get("k")->get() == "");
    CHECK(s.exists("k"));
  }

  SUBCASE("keys and values are binary safe") {
    const std::string key("a\0b", 3);
    const std::string value("x\0\r\ny", 5);
    REQUIRE(s.set(key, value));
    REQUIRE(s.get(key).has_value());
    CHECK(s.get(key)->get() == value);
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
    CHECK(s.memtable_keys() == 0);
  }

  SUBCASE("deleting a key that is not there removes nothing") {
    CHECK_FALSE(s.del("absent").was_live);
    CHECK_FALSE(s.get("absent").has_value());
    CHECK(s.memtable_keys() == 1);  // "k" is untouched
  }

  SUBCASE("deleting twice only counts the first time") {
    CHECK(s.del("k").was_live);
    CHECK_FALSE(s.del("k").was_live);
    CHECK(s.memtable_keys() == 0);
  }

  SUBCASE("a deleted key can be written again") {
    REQUIRE(s.del("k").durable);
    REQUIRE(s.set("k", "again"));
    REQUIRE(s.get("k").has_value());
    CHECK(s.get("k")->get() == "again");
    CHECK(s.memtable_keys() == 1);
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

  CHECK(view->get() == "value");
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
  CHECK(recovered.get("a")->get() == "1-updated");
  // The delete has to survive recovery. Replaying only the sets would bring
  // "b" back from the dead, which is the failure the tombstone exists for.
  CHECK_FALSE(recovered.get("b").has_value());
  REQUIRE(recovered.get("c").has_value());
  CHECK(recovered.get("c")->get() == "3");
  CHECK(recovered.memtable_keys() == 2);
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
    CHECK(s.memtable_keys() == 2);
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
  CHECK(s.memtable_keys() == 0);

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

TEST_CASE("a multi-key delete is all or nothing when the log refuses") {
  // The gap this closes: DEL a b c used to delete whatever came before the
  // failure and then report an error that could not say how far it got.
  if (::access("/dev/full", W_OK) != 0) return;

  TempLog log("batch");
  Wal good(log.path, SyncPolicy::kNo);
  Store s(&good);
  REQUIRE(s.set("a", "1"));
  REQUIRE(s.set("b", "2"));
  REQUIRE(s.set("c", "3"));

  // Swap in a log that fails every write, without disturbing what is already
  // in memory.
  Wal full("/dev/full", SyncPolicy::kNo);
  s.set_wal(&full);

  const DelBatchResult r = s.del_many({"a", "b", "c"});
  CHECK_FALSE(r.durable);
  CHECK(r.removed == 0);

  // Not one of them went. Previously "a" would be gone and the client would
  // have no way to learn that but to re-read every key it named.
  CHECK(s.get("a").has_value());
  CHECK(s.get("b").has_value());
  CHECK(s.get("c").has_value());
  CHECK(s.memtable_keys() == 3);
}

TEST_CASE("a batched delete that succeeds counts and logs like the single one") {
  TempLog log("batch_ok");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    Store s(&wal);
    REQUIRE(s.set("a", "1"));
    REQUIRE(s.set("b", "2"));

    const DelBatchResult r = s.del_many({"a", "b", "missing"});
    CHECK(r.durable);
    CHECK(r.removed == 2);  // "missing" was never live
    CHECK(s.memtable_keys() == 0);
  }

  // All three tombstones are in the log, including the one for a key that was
  // never there -- from M3 that marker is what hides the key in an SSTable.
  int tombstones = 0;
  const ReplayResult rr = replay(log.path, [&](const Record& rec) {
    if (rec.op == Record::Op::kDelete) ++tombstones;
  });
  CHECK(rr.records == 5);  // 2 sets + 3 deletes
  CHECK(tombstones == 3);
}

TEST_CASE("deleting the same key twice in one command counts it once") {
  Store s;
  REQUIRE(s.set("k", "v"));
  const DelBatchResult r = s.del_many({"k", "k"});
  CHECK(r.durable);
  CHECK(r.removed == 1);  // as real Redis counts it
}

// --------------------------------------------------------------------
// The store across a memtable and the SSTables under it.
// --------------------------------------------------------------------

TEST_CASE("a flushed key is still readable, now from disk") {
  TempDir d;
  Store s(opts(d));
  REQUIRE(s.set("k", "v"));
  CHECK(s.memtable_keys() == 1);

  REQUIRE(s.flush());
  // The memtable is empty, so a hit here can only have come off disk.
  CHECK(s.memtable_keys() == 0);
  CHECK(s.sstable_count() == 1);
  REQUIRE(s.get("k").has_value());
  CHECK(s.get("k")->get() == "v");
}

TEST_CASE("a tombstone hides a value that is still in an older table") {
  // The failure this whole design exists to prevent. If a delete did not
  // shadow the older file, the read would fall through, find the flushed
  // value, and hand a client back data it deleted.
  TempDir d;
  StoreOptions o = opts(d);
  Store s(o);

  REQUIRE(s.set("k", "v"));
  REQUIRE(s.flush());  // "k" = "v" is now on disk
  REQUIRE(s.get("k").has_value());

  REQUIRE(s.del("k").durable);  // tombstone lives in the memtable
  CHECK_FALSE(s.get("k").has_value());

  REQUIRE(s.flush());  // and now the tombstone is on disk too
  CHECK(s.sstable_count() == 2);
  CHECK_FALSE(s.get("k").has_value());

  // Still gone after a restart, which reads the tables back newest first.
  const Store reopened(o);
  CHECK(reopened.sstable_count() == 2);
  CHECK_FALSE(reopened.get("k").has_value());
}

TEST_CASE("a newer table wins over an older one") {
  TempDir d;
  StoreOptions o = opts(d);
  Store s(o);

  REQUIRE(s.set("k", "first"));
  REQUIRE(s.flush());
  REQUIRE(s.set("k", "second"));
  REQUIRE(s.flush());
  CHECK(s.sstable_count() == 2);
  CHECK(s.get("k")->get() == "second");

  const Store reopened(o);
  CHECK(reopened.get("k")->get() == "second");
}

TEST_CASE("reads fall through the layers in order") {
  TempDir d;
  Store s(opts(d));

  REQUIRE(s.set("old", "1"));
  REQUIRE(s.flush());
  REQUIRE(s.set("middle", "2"));
  REQUIRE(s.flush());
  REQUIRE(s.set("fresh", "3"));  // still in the memtable

  CHECK(s.get("fresh")->get() == "3");
  CHECK(s.get("middle")->get() == "2");
  CHECK(s.get("old")->get() == "1");
  CHECK_FALSE(s.get("never").has_value());
  CHECK(s.sstable_count() == 2);
}

TEST_CASE("crossing the threshold flushes without being asked") {
  TempDir d;
  // Small enough that a handful of keys crosses it.
  Store s(opts(d, 8 * 1024));

  for (int i = 0; i < 400; ++i) {
    REQUIRE(s.set("key" + std::to_string(i), std::string(64, 'v')));
  }
  CHECK(s.sstable_count() > 0);
  CHECK_FALSE(s.flush_failed());
  // The memtable shed its bulk rather than growing without bound.
  CHECK(s.memtable().bytes() < 8 * 1024);

  // Every key is still there, wherever it ended up.
  for (int i = 0; i < 400; ++i) {
    const auto got = s.get("key" + std::to_string(i));
    REQUIRE_MESSAGE(got.has_value(), "lost key" << i);
    CHECK(got->get() == std::string(64, 'v'));
  }
}

TEST_CASE("a flush truncates the log it made redundant") {
  TempDir d;
  TempLog log("flush_truncates");
  StoreOptions o = opts(d);
  Store s(o);
  Wal wal(log.path, SyncPolicy::kAlways);
  s.set_wal(&wal);

  REQUIRE(s.set("a", "1"));
  REQUIRE(s.set("b", "2"));
  CHECK(wal.size() > 0);

  REQUIRE(s.flush());
  // Every record in the log is now in a table that has been fsynced, so the
  // log has nothing left to protect.
  CHECK(wal.size() == 0);
  CHECK(Wal(log.path, SyncPolicy::kNo).size() == 0);

  // And the data is still there.
  CHECK(s.get("a")->get() == "1");
  CHECK(s.get("b")->get() == "2");
}

TEST_CASE("a restart sees tables and the log together") {
  TempDir d;
  TempLog log("restart");
  StoreOptions o = opts(d);
  {
    Store s(o);
    Wal wal(log.path, SyncPolicy::kAlways);
    s.set_wal(&wal);
    REQUIRE(s.set("flushed", "on-disk"));
    REQUIRE(s.flush());
    // Written after the flush, so this one lives only in the log.
    REQUIRE(s.set("pending", "in-log"));
  }

  // The real startup sequence: adopt the tables, then replay what the log
  // still holds on top of them.
  Store reopened(o);
  CHECK(reopened.sstable_count() == 1);
  const ReplayResult r = replay(log.path, [&](const Record& rec) {
    reopened.apply(rec);
  });
  CHECK(r.records == 1);  // the flush cut the first write out of the log

  CHECK(reopened.get("flushed")->get() == "on-disk");
  CHECK(reopened.get("pending")->get() == "in-log");
}

TEST_CASE("an empty memtable flushes to nothing rather than an empty table") {
  TempDir d;
  Store s(opts(d));
  REQUIRE(s.flush());
  CHECK(s.sstable_count() == 0);  // no file for no data
}

TEST_CASE("a store with no directory never flushes") {
  // What the rest of these tests use, and what M1 and M2 were.
  Store s;
  for (int i = 0; i < 200; ++i) {
    REQUIRE(s.set("key" + std::to_string(i), "v"));
  }
  CHECK(s.sstable_count() == 0);
  CHECK(s.memtable_keys() == 200);
}

TEST_CASE("a key whose expiry has passed reads as absent") {
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("gone", "v", cachedb::now_ms() - 1000));
  CHECK(store.set("staying", "v", cachedb::now_ms() + 3600'000));
  CHECK(store.set("forever", "v"));

  CHECK_FALSE(store.get("gone").has_value());
  CHECK_FALSE(store.exists("gone"));
  REQUIRE(store.get("staying").has_value());
  CHECK(store.get("staying")->get() == "v");
  REQUIRE(store.get("forever").has_value());
  CHECK(store.get("forever")->get() == "v");
}

TEST_CASE("an expired entry hides an older value instead of uncovering it") {
  // The reason an expired entry stops the search rather than being skipped.
  // If the memtable's expired entry were treated as "nothing to say here",
  // the read would fall through to the table below and serve v1 -- a value
  // the client replaced. Same failure as a dropped tombstone, different door.
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("k", "v1"));
  CHECK(store.flush());
  CHECK(store.set("k", "v2", cachedb::now_ms() - 1000));

  CHECK_FALSE(store.get("k").has_value());
}

TEST_CASE("an expiry survives a flush") {
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("gone", "v", cachedb::now_ms() - 1000));
  CHECK(store.set("staying", "v", cachedb::now_ms() + 3600'000));
  CHECK(store.flush());
  // Both are now on disk and the memtable is empty, so these answers come
  // from the SSTable's own stamps.
  CHECK(store.memtable_keys() == 0);
  CHECK_FALSE(store.get("gone").has_value());
  REQUIRE(store.get("staying").has_value());
  CHECK(store.get("staying")->get() == "v");
}

TEST_CASE("an expiry survives a restart") {
  TempLog log("expiry_replay");
  const int64_t past = cachedb::now_ms() - 1000;
  const int64_t future = cachedb::now_ms() + 3600'000;
  {
    Wal wal(log.path, SyncPolicy::kNo);
    Store store(&wal);
    CHECK(store.set("gone", "v", past));
    CHECK(store.set("staying", "v", future));
  }

  Store rebuilt;
  const ReplayResult r =
      replay(log.path, [&](const Record& rec) { rebuilt.apply(rec); });
  CHECK(r.records == 2);
  // Replayed as an absolute point in time, not a fresh lease. A key that ran
  // out while the process was down has to come back already gone.
  CHECK_FALSE(rebuilt.get("gone").has_value());
  CHECK(rebuilt.get("staying").has_value());
}

TEST_CASE("a plain overwrite clears an expiry through the store") {
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("k", "v", cachedb::now_ms() - 1000));
  CHECK_FALSE(store.get("k").has_value());
  CHECK(store.set("k", "v2"));
  REQUIRE(store.get("k").has_value());
  CHECK(store.get("k")->get() == "v2");
}

TEST_CASE("DEL counts a key that lives only in an SSTable") {
  // Was :0 before, which disagreed with EXISTS about the same key. The count
  // is "was this visible a moment ago", and the memtable alone cannot answer
  // that once a flush has moved the key out from under it.
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("k", "v"));
  CHECK(store.flush());
  REQUIRE(store.exists("k"));

  const DelResult r = store.del("k");
  CHECK(r.durable);
  CHECK(r.was_live);
  CHECK_FALSE(store.exists("k"));
}

TEST_CASE("DEL does not count a key that had already expired") {
  // Was :1 before, reporting the removal of something no client could see.
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("k", "v", cachedb::now_ms() - 1000));
  REQUIRE_FALSE(store.exists("k"));

  const DelResult r = store.del("k");
  CHECK(r.durable);
  CHECK_FALSE(r.was_live);
}

TEST_CASE("a batched delete counts disk and expired keys the same way") {
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("on_disk", "v"));
  CHECK(store.flush());
  CHECK(store.set("in_memory", "v"));
  CHECK(store.set("expired", "v", cachedb::now_ms() - 1000));

  const DelBatchResult r =
      store.del_many({"on_disk", "in_memory", "expired", "never_existed"});
  CHECK(r.durable);
  CHECK(r.removed == 2);  // the two that were visible
}

TEST_CASE("the sweep reclaims a key nobody read") {
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("gone", "v", cachedb::now_ms() - 1000));
  CHECK(store.set("staying", "v"));
  // Nothing has read "gone", so the lazy half of expiry has never seen it.
  CHECK(store.memtable_keys() == 2);

  CHECK(store.sweep_expired(100) == 1);
  CHECK(store.memtable_keys() == 1);
  CHECK_FALSE(store.get("gone").has_value());
  CHECK(store.get("staying").has_value());
}

TEST_CASE("a swept key still hides a value in an older table") {
  // The sweep marks instead of erasing, and this is why. If it erased, the
  // read would fall through to the table below and answer v1 -- a value the
  // client replaced, resurrected by its replacement expiring.
  TempDir d;
  Store store(opts(d));
  CHECK(store.set("k", "v1"));
  CHECK(store.flush());
  CHECK(store.set("k", "v2", cachedb::now_ms() - 1000));

  CHECK(store.sweep_expired(100) == 1);
  CHECK_FALSE(store.get("k").has_value());
}

TEST_CASE("the sweep writes nothing to the log") {
  // The claim that the sweep needs no log record. The expiry justifying it is
  // already durable, so a crash replays the entry back with the same stamp
  // and the read path judges it expired again -- the same visible state, for
  // no bytes written.
  TempLog log("sweep_no_log");
  Wal wal(log.path, SyncPolicy::kNo);
  Store store(&wal);
  CHECK(store.set("a", "v", cachedb::now_ms() - 1000));
  CHECK(store.set("b", "v", cachedb::now_ms() - 1000));

  const uint64_t before = wal.size();
  CHECK(store.sweep_expired(100) == 2);
  CHECK(wal.size() == before);
}

TEST_CASE("a swept memtable survives a restart as the same absent keys") {
  TempLog log("sweep_replay");
  const int64_t past = cachedb::now_ms() - 1000;
  {
    Wal wal(log.path, SyncPolicy::kNo);
    Store store(&wal);
    CHECK(store.set("gone", "v", past));
    CHECK(store.set("staying", "v"));
    CHECK(store.sweep_expired(100) == 1);
  }

  Store rebuilt;
  replay(log.path, [&](const Record& rec) { rebuilt.apply(rec); });
  // The sweep left no record, so replay rebuilds the expired entry rather
  // than a tombstone. Different in memory, identical to a client.
  CHECK_FALSE(rebuilt.get("gone").has_value());
  CHECK(rebuilt.get("staying").has_value());
}
