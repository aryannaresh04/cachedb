#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "compaction.h"

#include <unistd.h>

#include <memory>
#include <string>
#include <vector>

using cachedb::compact;
using cachedb::CompactionResult;
using cachedb::Sstable;
using cachedb::SstableWriter;

namespace {

struct TempFile {
  std::string path;
  explicit TempFile(const std::string& name)
      : path("compact_test_" + name + ".sst") {
    ::unlink(path.c_str());
  }
  ~TempFile() { ::unlink(path.c_str()); }
};

struct Row {
  std::string key;
  std::string value;
  bool tombstone = false;
  int64_t expires_at_ms = 0;
};

void write_table(const std::string& path, const std::vector<Row>& rows) {
  SstableWriter w(path);
  for (const Row& r : rows) {
    w.add(r.key, r.value, r.tombstone, r.expires_at_ms);
  }
  REQUIRE(w.finish());
}

std::vector<Row> read_table(const std::string& path) {
  std::vector<Row> rows;
  const Sstable t(path);
  for (auto c = t.cursor(); c.valid(); c.next()) {
    rows.push_back({std::string(c.key()), std::string(c.value()),
                    c.tombstone(), c.expires_at_ms()});
  }
  return rows;
}

}  // namespace

TEST_CASE("a merge keeps the newest version of a duplicated key") {
  TempFile newer("newer"), older("older"), out("out");
  write_table(newer.path, {{"a", "new"}, {"c", "new"}});
  write_table(older.path, {{"a", "old"}, {"b", "old"}});

  const Sstable n(newer.path), o(older.path);
  // Newest first, the order Store keeps and a read consults. The merge never
  // compares timestamps -- position is the answer, which is why it cannot
  // disagree with the read path.
  const CompactionResult r = compact({&n, &o}, out.path, 10, false, 0);
  REQUIRE(r.ok);
  CHECK(r.written == 3);
  CHECK(r.dropped == 1);  // the older "a"

  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 3);
  CHECK(rows[0].key == "a");
  CHECK(rows[0].value == "new");
  CHECK(rows[1].key == "b");
  CHECK(rows[1].value == "old");
  CHECK(rows[2].key == "c");
  CHECK(rows[2].value == "new");
}

TEST_CASE("output is sorted even when inputs interleave") {
  TempFile a("i_a"), b("i_b"), c("i_c"), out("i_out");
  write_table(a.path, {{"a", "1"}, {"d", "1"}, {"g", "1"}});
  write_table(b.path, {{"b", "2"}, {"e", "2"}, {"h", "2"}});
  write_table(c.path, {{"c", "3"}, {"f", "3"}, {"i", "3"}});

  const Sstable ta(a.path), tb(b.path), tc(c.path);
  REQUIRE(compact({&ta, &tb, &tc}, out.path, 10, false, 0).ok);

  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 9);
  std::string previous;
  for (const Row& r : rows) {
    CHECK(previous < r.key);
    previous = r.key;
  }
  CHECK(rows.front().key == "a");
  CHECK(rows.back().key == "i");
}

TEST_CASE("a tombstone survives a merge that is not the last one") {
  // The rule that keeps deletes deleted. An older table outside this merge
  // may still hold the key, and the tombstone is the only thing hiding it.
  TempFile newer("t_new"), older("t_old"), out("t_out");
  write_table(newer.path, {{"gone", "", true}});
  write_table(older.path, {{"gone", "still here"}});

  const Sstable n(newer.path), o(older.path);
  const CompactionResult r = compact({&n, &o}, out.path, 10,
                                     /*drop_obsolete=*/false, 0);
  REQUIRE(r.ok);

  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].key == "gone");
  CHECK(rows[0].tombstone);
}

TEST_CASE("a tombstone is dropped once nothing older is left") {
  TempFile newer("d_new"), older("d_old"), out("d_out");
  write_table(newer.path, {{"gone", "", true}, {"kept", "v"}});
  write_table(older.path, {{"gone", "was here"}});

  const Sstable n(newer.path), o(older.path);
  const CompactionResult r = compact({&n, &o}, out.path, 10,
                                     /*drop_obsolete=*/true, 0);
  REQUIRE(r.ok);
  CHECK(r.written == 1);
  CHECK(r.dropped == 2);  // the tombstone, and the value it was hiding

  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].key == "kept");
}

TEST_CASE("an expired entry follows exactly the tombstone rule") {
  TempFile newer("e_new"), older("e_old"), keep("e_keep"), drop("e_drop");
  write_table(newer.path, {{"a", "v", false, 1000}, {"b", "v", false, 9000}});
  write_table(older.path, {{"a", "older value"}});
  const Sstable n(newer.path), o(older.path);

  SUBCASE("carried forward while an older table survives") {
    // Dropping it here would uncover "older value" -- a value resurrected by
    // its replacement expiring.
    REQUIRE(compact({&n, &o}, keep.path, 10, false, /*now=*/5000).ok);
    const auto rows = read_table(keep.path);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "a");
    CHECK(rows[0].expires_at_ms == 1000);
    CHECK(rows[1].key == "b");
  }

  SUBCASE("discarded once nothing older is left") {
    REQUIRE(compact({&n, &o}, drop.path, 10, true, /*now=*/5000).ok);
    const auto rows = read_table(drop.path);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].key == "b");  // not yet expired at now=5000
  }
}

TEST_CASE("merging empty and single tables") {
  TempFile empty("m_empty"), one("m_one"), out("m_out");
  write_table(empty.path, {});
  write_table(one.path, {{"k", "v"}});

  const Sstable e(empty.path), o(one.path);
  REQUIRE(compact({&e, &o}, out.path, 10, false, 0).ok);
  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].key == "k");
}

TEST_CASE("a key present in every input is written once") {
  TempFile a("s_a"), b("s_b"), c("s_c"), d("s_d"), out("s_out");
  write_table(a.path, {{"k", "1"}});
  write_table(b.path, {{"k", "2"}});
  write_table(c.path, {{"k", "3"}});
  write_table(d.path, {{"k", "4"}});

  const Sstable ta(a.path), tb(b.path), tc(c.path), td(d.path);
  const CompactionResult r = compact({&ta, &tb, &tc, &td}, out.path, 10, true, 0);
  REQUIRE(r.ok);
  CHECK(r.written == 1);
  CHECK(r.dropped == 3);

  const auto rows = read_table(out.path);
  REQUIRE(rows.size() == 1);
  CHECK(rows[0].value == "1");  // the newest input
}

TEST_CASE("the merged table answers point lookups through its filter") {
  TempFile a("f_a"), b("f_b"), out("f_out");
  std::vector<Row> rows_a, rows_b;
  for (int i = 0; i < 500; i += 2) {
    rows_a.push_back({"key" + std::to_string(1000 + i), "a"});
  }
  for (int i = 1; i < 500; i += 2) {
    rows_b.push_back({"key" + std::to_string(1000 + i), "b"});
  }
  write_table(a.path, rows_a);
  write_table(b.path, rows_b);

  const Sstable ta(a.path), tb(b.path);
  REQUIRE(compact({&ta, &tb}, out.path, 10, true, 0).ok);

  // A rebuilt filter, not a copied one: a merged table's key set is the union
  // of its inputs' and neither input's filter describes it.
  const Sstable merged(out.path);
  for (int i = 0; i < 500; ++i) {
    const auto got = merged.get("key" + std::to_string(1000 + i));
    REQUIRE(got.found);
    CHECK(got.value == (i % 2 == 0 ? "a" : "b"));
  }
  CHECK_FALSE(merged.get("key9999").found);
}
