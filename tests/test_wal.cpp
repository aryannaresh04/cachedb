#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "wal.h"

#include <string>

using cachedb::crc32;
using cachedb::decode_record;
using cachedb::DecodeStatus;
using cachedb::encode_record;
using cachedb::kRecordHeaderSize;
using cachedb::Record;

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
