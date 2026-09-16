#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "fd.h"

namespace cachedb
{

  // Milliseconds since the Unix epoch, on the wall clock.
  //
  // system_clock and not steady_clock, and the difference is the whole reason
  // this is the only clock the storage engine reads. A steady_clock's zero
  // point is whenever the machine last booted, so a stamp taken from it means
  // nothing after a restart and cannot be compared with a number a client
  // sent us. The cost is stated plainly rather than hidden: if the system
  // clock steps backwards, keys that had expired become visible again.
  //
  // Lives here because the log has always needed it to date a record, and a
  // second clock function elsewhere is how two layers start disagreeing about
  // what time it is.
  int64_t now_ms();

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
  // A SET carrying an expiry is written with a third op byte and eight more
  // bytes between the header and the key (PROJECT.md 6.8):
  //
  //   [crc32: 4][timestamp_ms: 8][op: 2][klen: 4][vlen: 4]
  //   [expires_at_ms: 8][key][value]
  //
  // That third op is a wire detail and nothing more -- it never reaches this
  // struct. A decoded record says kSet with a non-zero expires_at_ms, and the
  // encoder picks the op byte back out of that. Keeping it out of the enum
  // means no switch anywhere gains a third case duplicating kSet, and no
  // caller can hand in an expiry alongside the wrong op and watch it be
  // dropped without a word.
  //
  // A new op rather than a new field, because a field would be paid for by
  // every DELETE and every SET that has no expiry, and would make every log
  // written before today unreadable. decode_record already treats an op it
  // does not recognise as Corrupt, so an older reader rejects a newer record
  // loudly instead of misparsing it.
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
    // When the record was written. Distinct from expires_at_ms, which is when
    // the key it carries stops being visible.
    int64_t timestamp_ms = 0;
    // Wall-clock milliseconds after which the key is gone, or 0 for never.
    // Only ever set on a kSet: a tombstone that expired would be a deleted
    // key coming back.
    int64_t expires_at_ms = 0;
    // Views into the buffer passed to decode_record, valid only as long as it
    // is. A replay holds the file contents alive across the whole scan.
    std::string_view key;
    std::string_view value;
  };

  // Header is crc(4) + timestamp(8) + op(1) + klen(4) + vlen(4). This is both
  // the smallest a record can be and the amount that has to be in hand before
  // its true length is knowable, which is why the lengths live inside it.
  inline constexpr size_t kRecordHeaderSize = 21;

  // What a record carrying an expiry spends on top of that, sitting between
  // the header and the key.
  inline constexpr size_t kExpirySize = 8;

  // A non-zero expires_at_ms on a kSet selects the wire op that can carry it.
  // Passing one with kDelete is ignored: a tombstone has no lifetime of its
  // own, and giving it one would mean a deleted key reappearing.
  void encode_record(std::string &out, Record::Op op, int64_t timestamp_ms,
                     std::string_view key, std::string_view value,
                     int64_t expires_at_ms = 0);

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
    // The interval is a parameter so a test can drive kEverySec in
    // milliseconds instead of sleeping a real second. Production passes
    // nothing and gets the second the policy is named after.
    Wal(const std::string &path, SyncPolicy policy,
        std::chrono::milliseconds sync_interval = std::chrono::seconds(1));

    // Appends one mutation. Returns false if the bytes did not reach the
    // kernel -- or, under kAlways, the disk. A false here must stop the caller
    // acknowledging the write, which is the whole point of the log.
    bool append(Record::Op op, std::string_view key, std::string_view value,
                int64_t expires_at_ms = 0);

    // One mutation inside a batch.
    struct Mutation
    {
      Record::Op op = Record::Op::kSet;
      std::string_view key;
      std::string_view value;
      int64_t expires_at_ms = 0;
    };

    // Appends every mutation as a single write() and a single fsync.
    //
    // What this buys and what it does not. It makes a batch atomic against a
    // *detected* failure: the log write either succeeds or it does not, and
    // the caller learns which before touching anything in memory. It does not
    // make it atomic against a crash -- a torn write still leaves a prefix of
    // the batch on disk, and replay will apply the whole records in it.
    //
    // That asymmetry is acceptable rather than overlooked. A crash mid-write
    // means the client never got a reply, so nothing was acknowledged and the
    // invariant the crash test enforces still holds. Closing the second gap
    // too would mean one record carrying every key, which is what real Redis
    // does by logging the command rather than its per-key effects.
    bool append_batch(const std::vector<Mutation> &mutations);

    // Under kAlways, stop fsyncing inside append() and leave it to the caller
    // to do once per event loop iteration. PROJECT.md 6.4.
    //
    // This does not weaken kAlways. The guarantee is that no acknowledged
    // write is ever lost, and it survives because a reply is not allowed out
    // until the fsync covering it has returned -- the server holds every
    // reply, syncs once, and only then writes to any socket. What changes is
    // the number of fsyncs, not what they promise.
    //
    // It does change what append() returning true means: not "durable" any
    // more, but "in the kernel and covered by the next sync". The only caller
    // permitted to treat that as an acknowledgement is one that syncs first.
    void set_group_commit(bool on) { group_commit_ = on; }

    // Whether anything has been written since the last fsync. The event loop
    // uses this to skip the syscall entirely on a read-only iteration.
    bool needs_sync() const { return bytes_since_sync_ > 0; }

    // Driven from the event loop tick. Does nothing unless the policy is
    // kEverySec, a second has actually elapsed, AND something has been written
    // since the last sync -- so it is cheap to call often, and free on a
    // server nobody is talking to.
    bool maybe_sync();

    // Forces the log down unconditionally. Unlike maybe_sync() this does not
    // check whether anything changed: an explicit request to sync is an
    // explicit request, not a suggestion.
    bool sync();

    // Empties the log. Called after a flush has put the memtable's contents
    // safely into an SSTable, at which point every record in here is
    // redundant.
    //
    // Only ever safe in that order. Truncating before the table is durable
    // would leave a crash window where the data is in neither place.
    [[nodiscard]] bool truncate();

    SyncPolicy policy() const { return policy_; }

    // How many fsyncs have actually been issued. Exposed because the whole
    // difference between the three policies is invisible from the outside
    // otherwise -- it took strace to see it the first time -- and because a
    // skipped-sync optimisation is exactly the kind of thing that silently
    // stops working. INFO will want this at M4.
    uint64_t syncs() const { return syncs_; }

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
    std::chrono::milliseconds sync_interval_;
    uint64_t size_ = 0;
    // Bytes appended since the last fsync. Zero means the file on disk already
    // matches what we have written, so a timer firing has nothing to do.
    uint64_t bytes_since_sync_ = 0;
    uint64_t syncs_ = 0;
    bool group_commit_ = false;
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
