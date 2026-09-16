#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "fd.h"

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


  // How hard the log is pushed toward the platter. PROJECT.md 6.4.
  //
  // The distinction that matters, and that question 4 in section 12 is really
  // asking about: a write() only hands bytes to the kernel. It survives the
  // process being killed, because the page cache belongs to the kernel and
  // not to us. It does not survive the power going out. Only fsync makes that
  // second claim, and only as far as the drive is honest about its own cache.
  enum class SyncPolicy
  {
    // fsync before the write is acknowledged. An acked write survives a power
    // cut. Safest and by a wide margin the slowest.
    kAlways,
    // fsync at most once a second, driven by the event loop's tick. Up to a
    // second of acknowledged writes can be lost to a power cut.
    kEverySec,
    // Never fsync explicitly. Survives kill -9, not power loss.
    kNo,
  };

  // The append-only log. One instance owns the file for the life of the
  // process.
  //
  // Deliberately does not buffer: every append is one write() syscall. Under
  // the default kAlways policy the fsync dominates so completely that a
  // userspace buffer would save nothing measurable, and a buffer is a second
  // place records can be lost from -- one that the CRC cannot detect, because
  // bytes that never reached the kernel leave no torn record behind, just a
  // shorter log. Buffering is the optimization to add and measure once the
  // simple path is proven (6.4).
  class Wal
  {
  public:
    // Opens or creates the log and positions at the end. Throws on failure:
    // a log that will not open is a startup error with nothing to degrade to
    // (PROJECT.md 11).
    //
    // Call replay() on the path FIRST if this is recovery. Opening for append
    // does not inspect what is already there.
    Wal(const std::string &path, SyncPolicy policy);

    // Appends one mutation. Returns false if the bytes did not reach the
    // kernel -- or, under kAlways, the disk. A false here must stop the caller
    // acknowledging the write, which is the whole point of the log.
    bool append(Record::Op op, std::string_view key, std::string_view value);

    // Driven from the event loop tick. Does nothing unless the policy is
    // kEverySec and a second has actually elapsed, so it is cheap to call
    // often.
    bool maybe_sync();

    bool sync();

    SyncPolicy policy() const { return policy_; }

    // Bytes in the log, as of the last append. Used from M3 to decide when the
    // memtable should be flushed and the log truncated.
    uint64_t size() const { return size_; }

  private:
    Fd fd_;
    SyncPolicy policy_;
    // Reused across appends so a write costs no allocation.
    std::string buf_;
    // steady_clock, not system_clock: this measures an interval, and a wall
    // clock that steps backwards over an NTP correction would stall syncing
    // for as long as the jump. The timestamp *inside* a record is the opposite
    // case and uses system_clock -- expiry is an absolute point in time.
    std::chrono::steady_clock::time_point last_sync_;
    uint64_t size_ = 0;
  };

  struct ReplayResult
  {
    size_t records = 0;      // records handed to the callback
    uint64_t good_bytes = 0; // length of the intact prefix
    // A partial or damaged tail was found and cut off. After a kill -9 this is
    // the expected outcome, not a failure.
    bool truncated = false;
  };

  // Applies every intact record at the front of `path`, in order, then cuts
  // the file back to that intact prefix.
  //
  // Stopping at the first bad record is not merely convenient, it is required.
  // The log is a sequence of events whose meaning depends on order: if record
  // 5 is a torn DELETE and record 6 is a SET of the same key, replaying 6
  // while skipping 5 resurrects data the client was told was deleted. Past the
  // first damaged record nothing can be trusted, so the tail is discarded
  // wholesale -- which is also why truncating matters, so the next run cannot
  // reinterpret those bytes.
  //
  // A missing file is not an error; a first run has nothing to recover.
  ReplayResult replay(const std::string &path,
                      const std::function<void(const Record &)> &apply);

} // namespace cachedb
