#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "wal.h"

#include <unistd.h>

#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using cachedb::crc32;
using cachedb::decode_record;
using cachedb::DecodeStatus;
using cachedb::encode_record;
using cachedb::kRecordHeaderSize;
using cachedb::Record;
using cachedb::replay;
using cachedb::ReplayResult;
using cachedb::SyncPolicy;
using cachedb::Wal;

namespace {

// Each test gets its own log in the test binary's working directory, which is
// under build*/ and therefore gitignored, and removes it on the way out
// whether the test passed or not.
struct TempLog {
  std::string path;
  explicit TempLog(const std::string& name) : path("wal_test_" + name + ".log") {
    ::unlink(path.c_str());
  }
  ~TempLog() { ::unlink(path.c_str()); }
};

// A Record's key and value are views into a buffer that dies inside replay(),
// so anything kept past the callback has to be copied. Doing that here rather
// than in the library keeps the replay path allocation-free.
struct Entry {
  Record::Op op;
  std::string key;
  std::string value;
};

std::vector<Entry> drain(const std::string& path, ReplayResult* out = nullptr) {
  std::vector<Entry> got;
  const ReplayResult r = replay(path, [&](const Record& rec) {
    got.push_back({rec.op, std::string(rec.key), std::string(rec.value)});
  });
  if (out) *out = r;
  return got;
}

std::string read_file(const std::string& path) {
  std::ifstream in(path, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(in),
                     std::istreambuf_iterator<char>());
}

void write_file(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::trunc);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

void append_raw(const std::string& path, const std::string& bytes) {
  std::ofstream out(path, std::ios::binary | std::ios::app);
  out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
}

}  // namespace

TEST_CASE("crc32 matches the published values") {
  // The check value every CRC-32 implementation is expected to agree on. If
  // this passes, the file format is readable by anything else that speaks
  // zlib's CRC.
  CHECK(crc32("123456789") == 0xCBF43926u);
  CHECK(crc32("") == 0u);
  CHECK(crc32("a") == 0xE8B7BE43u);
  // A single changed bit has to change the checksum, which is the entire job.
  CHECK(crc32("hello") != crc32("hellp"));
}

TEST_CASE("a record survives the round trip") {
  SUBCASE("a set") {
    std::string buf;
    encode_record(buf, Record::Op::kSet, 1700000000123, "username", "aryan");

    const auto r = decode_record(buf);
    REQUIRE(r.status == DecodeStatus::Ok);
    CHECK(r.consumed == buf.size());
    CHECK(r.record.op == Record::Op::kSet);
    CHECK(r.record.timestamp_ms == 1700000000123);
    CHECK(r.record.key == "username");
    CHECK(r.record.value == "aryan");
  }

  SUBCASE("a delete carries no value") {
    std::string buf;
    encode_record(buf, Record::Op::kDelete, 42, "username", "");

    const auto r = decode_record(buf);
    REQUIRE(r.status == DecodeStatus::Ok);
    CHECK(r.record.op == Record::Op::kDelete);
    CHECK(r.record.key == "username");
    CHECK(r.record.value == "");
  }

  SUBCASE("keys and values are binary safe") {
    const std::string key("a\0b", 3);
    const std::string value("x\0\r\ny", 5);
    std::string buf;
    encode_record(buf, Record::Op::kSet, 7, key, value);

    const auto r = decode_record(buf);
    REQUIRE(r.status == DecodeStatus::Ok);
    CHECK(r.record.key == key);
    CHECK(r.record.value == value);
  }

  SUBCASE("records read back one after another") {
    std::string buf;
    encode_record(buf, Record::Op::kSet, 1, "a", "1");
    encode_record(buf, Record::Op::kSet, 2, "b", "2");
    encode_record(buf, Record::Op::kDelete, 3, "a", "");

    std::string_view rest(buf);
    for (const char* expected : {"a", "b", "a"}) {
      const auto r = decode_record(rest);
      REQUIRE(r.status == DecodeStatus::Ok);
      CHECK(r.record.key == expected);
      rest.remove_prefix(r.consumed);
    }
    CHECK(rest.empty());
  }
}

TEST_CASE("the encoding does not depend on the host's byte order") {
  std::string buf;
  encode_record(buf, Record::Op::kSet, 0, "ab", "cde");

  REQUIRE(buf.size() == kRecordHeaderSize + 5);
  CHECK(buf[12] == 0);  // op: set
  // klen = 2 and vlen = 3, little-endian, at fixed offsets.
  CHECK(std::string(buf, 13, 4) == std::string("\x02\x00\x00\x00", 4));
  CHECK(std::string(buf, 17, 4) == std::string("\x03\x00\x00\x00", 4));
  CHECK(std::string(buf, kRecordHeaderSize, 5) == "abcde");
}

TEST_CASE("a crash mid-append leaves an incomplete record, not a corrupt one") {
  std::string buf;
  encode_record(buf, Record::Op::kSet, 99, "username", "aryan");

  // Every possible truncation point, which is every way a write can be cut
  // short. None may be mistaken for a valid record or read out of bounds.
  for (size_t n = 0; n < buf.size(); ++n) {
    const auto r = decode_record(std::string_view(buf).substr(0, n));
    CHECK(r.status == DecodeStatus::Incomplete);
    CHECK(r.consumed == 0);
  }
  CHECK(decode_record(buf).status == DecodeStatus::Ok);
}

TEST_CASE("damage is detected") {
  std::string buf;
  encode_record(buf, Record::Op::kSet, 99, "username", "aryan");

  SUBCASE("no single flipped bit can produce a valid record") {
    // Damage in a length field is caught by the length check rather than the
    // checksum, and so reports Incomplete. Either answer is fine; the
    // invariant that matters is that nothing damaged ever comes back Ok.
    for (size_t i = 0; i < buf.size(); ++i) {
      for (int bit = 0; bit < 8; ++bit) {
        std::string damaged = buf;
        damaged[i] = static_cast<char>(damaged[i] ^ (1 << bit));
        CHECK(decode_record(damaged).status != DecodeStatus::Ok);
      }
    }
  }

  SUBCASE("damage to the payload is caught by the checksum specifically") {
    // The lengths are untouched here, so the record is the size it claims and
    // only the checksum can tell that it is wrong.
    for (size_t i = kRecordHeaderSize; i < buf.size(); ++i) {
      std::string damaged = buf;
      damaged[i] = static_cast<char>(damaged[i] ^ 0x01);
      CHECK(decode_record(damaged).status == DecodeStatus::Corrupt);
    }
  }

  SUBCASE("a flipped bit in the checksum itself") {
    std::string damaged = buf;
    damaged[0] = static_cast<char>(damaged[0] ^ 0x01);
    CHECK(decode_record(damaged).status == DecodeStatus::Corrupt);
  }

  SUBCASE("an op byte from a format we do not know") {
    std::string damaged = buf;
    damaged[12] = 9;
    // Caught by the checksum first, which is the point: the op does not have
    // to be validated separately to be safe.
    CHECK(decode_record(damaged).status == DecodeStatus::Corrupt);
  }
}

TEST_CASE("a length field is not believed on sight") {
  std::string buf;
  encode_record(buf, Record::Op::kSet, 1, "k", "v");

  SUBCASE("a key length larger than the whole file") {
    std::string damaged = buf;
    damaged[13] = static_cast<char>(0xFF);
    damaged[14] = static_cast<char>(0xFF);
    damaged[15] = static_cast<char>(0xFF);
    damaged[16] = static_cast<char>(0xFF);
    // Reported as incomplete rather than acted on. Nothing allocates four
    // gigabytes and nothing reads past the end.
    CHECK(decode_record(damaged).status == DecodeStatus::Incomplete);
  }

  SUBCASE("lengths that would overflow if added in 32 bits") {
    std::string damaged = buf;
    for (size_t i = 13; i < 21; ++i) damaged[i] = static_cast<char>(0xFF);
    CHECK(decode_record(damaged).status == DecodeStatus::Incomplete);
  }
}

// --------------------------------------------------------------------
// The log as a file: append, fsync, replay.
// --------------------------------------------------------------------

TEST_CASE("a log that has never been written replays to nothing") {
  ReplayResult r;
  const auto got = drain("wal_test_definitely_absent.log", &r);
  // A first run is not a failure and must not need special-casing at startup.
  CHECK(got.empty());
  CHECK(r.records == 0);
  CHECK(r.good_bytes == 0);
  CHECK_FALSE(r.truncated);
}

TEST_CASE("an empty log replays to nothing and is left alone") {
  TempLog log("empty");
  { Wal wal(log.path, SyncPolicy::kNo); }  // creating it is enough

  ReplayResult r;
  drain(log.path, &r);
  CHECK(r.records == 0);
  CHECK(r.good_bytes == 0);
  CHECK_FALSE(r.truncated);
}

TEST_CASE("records survive a close and a reopen, in order") {
  TempLog log("roundtrip");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    REQUIRE(wal.append(Record::Op::kSet, "alpha", "one"));
    REQUIRE(wal.append(Record::Op::kSet, "beta", "two"));
    REQUIRE(wal.append(Record::Op::kDelete, "alpha", ""));
  }

  ReplayResult r;
  const auto got = drain(log.path, &r);
  CHECK(r.records == 3);
  CHECK_FALSE(r.truncated);
  REQUIRE(got.size() == 3);

  CHECK(got[0].op == Record::Op::kSet);
  CHECK(got[0].key == "alpha");
  CHECK(got[0].value == "one");
  CHECK(got[1].key == "beta");
  // Order is the whole point: the delete has to arrive after the set, or
  // replay rebuilds a state that never existed.
  CHECK(got[2].op == Record::Op::kDelete);
  CHECK(got[2].key == "alpha");
  CHECK(got[2].value.empty());
}

TEST_CASE("every sync policy writes the same records") {
  // The policy decides when bytes are forced to the platter, not what goes
  // into the file. Nothing about the format may depend on it.
  for (const SyncPolicy policy :
       {SyncPolicy::kAlways, SyncPolicy::kEverySec, SyncPolicy::kNo}) {
    TempLog log("policy");
    {
      Wal wal(log.path, policy);
      REQUIRE(wal.append(Record::Op::kSet, "k", "v"));
      REQUIRE(wal.maybe_sync());
    }
    const auto got = drain(log.path);
    REQUIRE(got.size() == 1);
    CHECK(got[0].key == "k");
    CHECK(got[0].value == "v");
  }
}

TEST_CASE("size() tracks the file and survives reopening") {
  TempLog log("size");
  uint64_t after_two = 0;
  {
    Wal wal(log.path, SyncPolicy::kNo);
    CHECK(wal.size() == 0);
    REQUIRE(wal.append(Record::Op::kSet, "a", "1"));
    REQUIRE(wal.append(Record::Op::kSet, "b", "2"));
    after_two = wal.size();
    CHECK(after_two == 2 * (kRecordHeaderSize + 2));
  }
  {
    // Reopening must not restart the count, or M3 would flush the memtable on
    // a threshold it had already passed.
    Wal wal(log.path, SyncPolicy::kNo);
    CHECK(wal.size() == after_two);
  }
}

TEST_CASE("keys and values containing NUL and CRLF survive the file") {
  TempLog log("binary");
  const std::string key("k\0\r\n", 4);
  const std::string value("v\0\xff\r\n", 5);
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    REQUIRE(wal.append(Record::Op::kSet, key, value));
  }
  const auto got = drain(log.path);
  REQUIRE(got.size() == 1);
  CHECK(got[0].key == key);
  CHECK(got[0].value == value);
}

TEST_CASE("a torn tail is dropped and cut off the file") {
  TempLog log("torn");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    REQUIRE(wal.append(Record::Op::kSet, "a", "1"));
    REQUIRE(wal.append(Record::Op::kSet, "b", "2"));
  }
  const size_t intact = read_file(log.path).size();

  // What a kill -9 partway through a third append leaves behind.
  std::string third;
  encode_record(third, Record::Op::kSet, 123, "c", "3");
  append_raw(log.path, third.substr(0, third.size() / 2));

  ReplayResult r;
  auto got = drain(log.path, &r);
  CHECK(r.records == 2);
  CHECK(got.size() == 2);
  CHECK(r.truncated);
  CHECK(r.good_bytes == intact);
  // Cut, not merely ignored -- otherwise the next append would sit behind the
  // wreckage and every later replay would stop at it.
  CHECK(read_file(log.path).size() == intact);

  ReplayResult again;
  got = drain(log.path, &again);
  CHECK(again.records == 2);
  CHECK_FALSE(again.truncated);
}

TEST_CASE("a damaged record hides every record after it") {
  TempLog log("corrupt");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    REQUIRE(wal.append(Record::Op::kSet, "a", "1"));
    REQUIRE(wal.append(Record::Op::kSet, "b", "2"));
    REQUIRE(wal.append(Record::Op::kSet, "c", "3"));
  }

  const size_t record_len = kRecordHeaderSize + 2;  // 1-byte key, 1-byte value
  std::string bytes = read_file(log.path);
  REQUIRE(bytes.size() == 3 * record_len);
  bytes[record_len + kRecordHeaderSize] ^= 0x01;  // the second record's key
  write_file(log.path, bytes);

  ReplayResult r;
  const auto got = drain(log.path, &r);
  // "c" was intact on disk and is still discarded. That is deliberate: the
  // log is ordered, so a record whose predecessor cannot be read cannot be
  // applied safely either -- replaying it could resurrect a deleted key.
  REQUIRE(got.size() == 1);
  CHECK(got[0].key == "a");
  CHECK(r.records == 1);
  CHECK(r.truncated);
  CHECK(read_file(log.path).size() == record_len);
}

TEST_CASE("appending after recovery continues from the intact prefix") {
  TempLog log("resume");
  {
    Wal wal(log.path, SyncPolicy::kAlways);
    REQUIRE(wal.append(Record::Op::kSet, "a", "1"));
  }
  std::string partial;
  encode_record(partial, Record::Op::kSet, 1, "b", "2");
  append_raw(log.path, partial.substr(0, 7));

  ReplayResult r;
  drain(log.path, &r);
  REQUIRE(r.truncated);

  {
    Wal wal(log.path, SyncPolicy::kAlways);
    // Opened at the clean boundary the truncation left, not past the garbage.
    CHECK(wal.size() == r.good_bytes);
    REQUIRE(wal.append(Record::Op::kSet, "c", "3"));
  }

  const auto got = drain(log.path);
  REQUIRE(got.size() == 2);
  CHECK(got[0].key == "a");
  CHECK(got[1].key == "c");
}
