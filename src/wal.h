#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cachedb
{

  // CRC-32 as used by zlib, gzip and PNG (IEEE 802.3, reflected, polynomial
  // 0xEDB88320). Hand-rolled because the standard library has no checksum and
  // PROJECT.md 3 allows no other dependency.
  //
  // It detects accidental damage -- a torn write, a bad sector -- and nothing
  // more. It is not a hash and is trivial to forge deliberately, which is
  // fine: the WAL is our own file, not something a client sends us.
  uint32_t crc32(std::string_view data);

  // One mutation, as it appears in the log. PROJECT.md 6.4:
  //
  //   [crc32: 4][timestamp_ms: 8][op: 1][klen: 4][vlen: 4][key][value]
  //
  // Little-endian, written byte by byte rather than copied out of a struct, so
  // the format does not change with the host's byte order or padding rules.
  //
  // The CRC sits first because the header has to be read before the record's
  // length is known, so it needs a fixed position. It covers everything after
  // itself: timestamp through value.
  struct Record
  {
    enum class Op : uint8_t
    {
      kSet = 0,
      kDelete = 1,
    };

    Op op = Op::kSet;
    int64_t timestamp_ms = 0;
    // Views into the buffer passed to decode_record, valid only as long as it
    // is. A replay holds the file contents alive across the whole scan.
    std::string_view key;
    std::string_view value;
  };

  // Header is crc(4) + timestamp(8) + op(1) + klen(4) + vlen(4).
  inline constexpr size_t kRecordHeaderSize = 21;

  void encode_record(std::string &out, Record::Op op, int64_t timestamp_ms,
                     std::string_view key, std::string_view value);

  enum class DecodeStatus
  {
    Ok,
    // The buffer ends part way through a record. During replay this is the
    // normal signature of a crash mid-append, not an error.
    Incomplete,
    // The bytes are all present but do not agree with themselves: a failed
    // checksum, an unknown op, a length that cannot be right.
    Corrupt,
  };

  struct DecodeResult
  {
    DecodeStatus status = DecodeStatus::Incomplete;
    size_t consumed = 0; // bytes the record occupied; 0 unless status is Ok
    Record record;       // meaningful only when status is Ok
  };

  // Decodes the record at the front of `in`. Does not trust the lengths it
  // reads: a claim that does not fit in the buffer is reported rather than
  // acted on.
  DecodeResult decode_record(std::string_view in);

} // namespace cachedb
