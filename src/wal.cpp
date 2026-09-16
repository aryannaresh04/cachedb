#include "wal.h"

#include <fcntl.h>
#include <unistd.h>

#include <array>
#include <cerrno>
#include <system_error>

namespace cachedb {
namespace {

std::array<uint32_t, 256> make_crc_table() {
  std::array<uint32_t, 256> table{};
  for (uint32_t i = 0; i < 256; ++i) {
    uint32_t c = i;
    for (int bit = 0; bit < 8; ++bit) {
      c = (c & 1u) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    }
    table[i] = c;
  }
  return table;
}

// Byte-at-a-time table lookup. Slice-by-8 is several times faster and is a
// candidate for the measured optimization PROJECT.md 6.4 asks for, once there
// are numbers showing the checksum actually costs anything.
const std::array<uint32_t, 256>& crc_table() {
  static const std::array<uint32_t, 256> table = make_crc_table();
  return table;
}

// Little-endian by hand. memcpy of a struct would be shorter and would also
// bake in this machine's byte order and padding, which is not something a
// file format should inherit.
void put_u32(std::string& out, uint32_t v) {
  out.push_back(static_cast<char>(v & 0xFFu));
  out.push_back(static_cast<char>((v >> 8) & 0xFFu));
  out.push_back(static_cast<char>((v >> 16) & 0xFFu));
  out.push_back(static_cast<char>((v >> 24) & 0xFFu));
}

void put_u64(std::string& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((v >> shift) & 0xFFu));
  }
}

uint32_t load_u32(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  return static_cast<uint32_t>(b[0]) | (static_cast<uint32_t>(b[1]) << 8) |
         (static_cast<uint32_t>(b[2]) << 16) |
         (static_cast<uint32_t>(b[3]) << 24);
}

uint64_t load_u64(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

int64_t now_ms() {
  // system_clock, not steady_clock: this timestamp outlives the process and
  // becomes an absolute expiry point at M4, so it has to mean something to the
  // next run. Interval measurement inside Wal uses steady_clock instead.
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

// write() is allowed to accept fewer bytes than offered, even for a regular
// file -- a signal or a full disk will do it. Looping is not optional: a
// single unchecked write() silently truncates a record. If we die partway
// through, the short prefix left behind is exactly the torn record replay
// expects to find.
bool write_all(int fd, std::string_view data) {
  const char* p = data.data();
  size_t left = data.size();
  while (left > 0) {
    const ssize_t n = ::write(fd, p, left);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += n;
    left -= static_cast<size_t>(n);
  }
  return true;
}

[[noreturn]] void throw_errno(const char* what, const std::string& path) {
  throw std::system_error(errno, std::generic_category(), what + (" " + path));
}

}  // namespace

uint32_t crc32(std::string_view data) {
  const auto& table = crc_table();
  uint32_t c = 0xFFFFFFFFu;
  for (char ch : data) {
    const auto byte = static_cast<unsigned char>(ch);
    c = table[(c ^ byte) & 0xFFu] ^ (c >> 8);
  }
  return c ^ 0xFFFFFFFFu;
}

void encode_record(std::string& out, Record::Op op, int64_t timestamp_ms,
                   std::string_view key, std::string_view value) {
  const size_t crc_pos = out.size();
  put_u32(out, 0);  // placeholder; the checksum is only known once the rest is

  const size_t covered_from = out.size();
  put_u64(out, static_cast<uint64_t>(timestamp_ms));
  out.push_back(static_cast<char>(op));
  put_u32(out, static_cast<uint32_t>(key.size()));
  put_u32(out, static_cast<uint32_t>(value.size()));
  out.append(key);
  out.append(value);

  const uint32_t crc = crc32(
      std::string_view(out).substr(covered_from, out.size() - covered_from));
  // Patch the placeholder in place rather than building the record twice.
  std::string encoded_crc;
  put_u32(encoded_crc, crc);
  out.replace(crc_pos, 4, encoded_crc);
}

DecodeResult decode_record(std::string_view in) {
  if (in.size() < kRecordHeaderSize) return {DecodeStatus::Incomplete, 0, {}};

  const uint32_t stored_crc = load_u32(in.data());
  const uint64_t timestamp = load_u64(in.data() + 4);
  const auto op_byte = static_cast<unsigned char>(in[12]);
  const uint32_t klen = load_u32(in.data() + 13);
  const uint32_t vlen = load_u32(in.data() + 17);

  // Lengths are claims made by bytes that may themselves be wreckage. Widen to
  // 64 bits so the sum cannot wrap, and compare before believing either one.
  const uint64_t total = static_cast<uint64_t>(kRecordHeaderSize) +
                         static_cast<uint64_t>(klen) +
                         static_cast<uint64_t>(vlen);
  if (in.size() < total) return {DecodeStatus::Incomplete, 0, {}};

  const size_t size = static_cast<size_t>(total);
  const std::string_view covered =
      in.substr(4, size - 4);  // timestamp through value
  if (crc32(covered) != stored_crc) return {DecodeStatus::Corrupt, 0, {}};

  // The checksum already passed, so an op we do not recognise means the file
  // was written by a different version, not that it was damaged. Either way
  // there is nothing sensible to replay.
  if (op_byte > static_cast<unsigned char>(Record::Op::kDelete)) {
    return {DecodeStatus::Corrupt, 0, {}};
  }

  DecodeResult result;
  result.status = DecodeStatus::Ok;
  result.consumed = size;
  result.record.op = static_cast<Record::Op>(op_byte);
  result.record.timestamp_ms = static_cast<int64_t>(timestamp);
  result.record.key = in.substr(kRecordHeaderSize, klen);
  result.record.value = in.substr(kRecordHeaderSize + klen, vlen);
  return result;
}

Wal::Wal(const std::string& path, SyncPolicy policy)
    : policy_(policy), last_sync_(std::chrono::steady_clock::now()) {
  // O_APPEND so every write lands at the true end of the file regardless of
  // where the offset was left. It costs nothing here and removes a whole class
  // of bug where a stale offset overwrites committed records.
  fd_.reset(::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0644));
  if (!fd_.valid()) throw_errno("open", path);

  const off_t end = ::lseek(fd_.get(), 0, SEEK_END);
  if (end < 0) throw_errno("lseek", path);
  size_ = static_cast<uint64_t>(end);
}

bool Wal::append(Record::Op op, std::string_view key, std::string_view value) {
  buf_.clear();
  encode_record(buf_, op, now_ms(), key, value);
  if (!write_all(fd_.get(), buf_)) return false;
  size_ += buf_.size();

  if (policy_ == SyncPolicy::kAlways) return sync();
  return true;
}

bool Wal::sync() {
  for (;;) {
    if (::fsync(fd_.get()) == 0) break;
    // EINTR is a signal arriving, not an I/O failure, and retrying is correct.
    if (errno == EINTR) continue;
    // Anything else is not safely retryable. Linux reports a writeback error
    // once and then clears it, so calling fsync again can return success while
    // the data is still gone -- the failure would be laundered into an ack.
    // There is nothing honest to do but refuse to acknowledge the write.
    return false;
  }
  last_sync_ = std::chrono::steady_clock::now();
  return true;
}

bool Wal::maybe_sync() {
  if (policy_ != SyncPolicy::kEverySec) return true;
  const auto now = std::chrono::steady_clock::now();
  if (now - last_sync_ < std::chrono::seconds(1)) return true;
  return sync();
}

ReplayResult replay(const std::string& path,
                    const std::function<void(const Record&)>& apply) {
  ReplayResult result;

  // O_RDWR because a damaged tail has to be cut, and reopening for write would
  // race with anything else touching the file between the two opens.
  Fd fd(::open(path.c_str(), O_RDWR));
  if (!fd.valid()) {
    if (errno == ENOENT) return result;  // first run, nothing to recover
    throw_errno("open", path);
  }

  // Read the log whole. It is bounded by the memtable flush threshold -- 4 MB
  // from M3, since a flush truncates it -- so this is megabytes, never the
  // dataset. If that stops being true the loop below already works on a
  // sliding window; only the buffering would change.
  std::string data;
  char chunk[64 * 1024];
  for (;;) {
    const ssize_t n = ::read(fd.get(), chunk, sizeof(chunk));
    if (n == 0) break;
    if (n < 0) {
      if (errno == EINTR) continue;
      throw_errno("read", path);
    }
    data.append(chunk, static_cast<size_t>(n));
  }

  const std::string_view in(data);
  size_t offset = 0;
  for (;;) {
    const DecodeResult r = decode_record(in.substr(offset));
    if (r.status != DecodeStatus::Ok) break;
    apply(r.record);
    ++result.records;
    offset += r.consumed;
  }

  result.good_bytes = offset;
  result.truncated = offset < data.size();

  if (result.truncated) {
    // Cut the wreckage off now rather than leaving it to be re-examined. The
    // next append then starts on a clean record boundary, and a second replay
    // is a no-op instead of rediscovering the same damage.
    if (::ftruncate(fd.get(), static_cast<off_t>(offset)) != 0) {
      throw_errno("ftruncate", path);
    }
  }
  return result;
}

}  // namespace cachedb
