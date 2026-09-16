#include "sstable.h"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <system_error>

namespace cachedb {
namespace {

using sstable::kFooterSize;
using sstable::kIndexInterval;
using sstable::kMagic;
using sstable::kMagicV1;

void put_u32(std::string& out, uint32_t v) {
  for (int shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((v >> shift) & 0xFFu));
  }
}

void put_u64(std::string& out, uint64_t v) {
  for (int shift = 0; shift < 64; shift += 8) {
    out.push_back(static_cast<char>((v >> shift) & 0xFFu));
  }
}

uint32_t load_u32(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  uint32_t v = 0;
  for (int i = 3; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

uint64_t load_u64(const char* p) {
  const auto* b = reinterpret_cast<const unsigned char*>(p);
  uint64_t v = 0;
  for (int i = 7; i >= 0; --i) v = (v << 8) | b[i];
  return v;
}

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

// pread rather than lseek+read: it carries its own offset, so a lookup does
// not disturb any other reader's position and the reader stays const.
bool read_exact(int fd, uint64_t offset, size_t length, std::string* out) {
  out->resize(length);
  size_t done = 0;
  while (done < length) {
    const ssize_t n = ::pread(fd, out->data() + done, length - done,
                              static_cast<off_t>(offset + done));
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (n == 0) return false;  // file is shorter than the footer claimed
    done += static_cast<size_t>(n);
  }
  return true;
}

[[noreturn]] void throw_errno(const char* what, const std::string& path) {
  throw std::system_error(errno, std::generic_category(), what + (" " + path));
}

[[noreturn]] void throw_format(const char* why, const std::string& path) {
  throw std::runtime_error(std::string(why) + ": " + path);
}

constexpr unsigned char kTombstoneFlag = 0x01;

// Bit 1: an 8-byte expiry sits between the flags byte and the key. Spending a
// spare bit rather than eight bytes on every entry keeps a key with no TTL --
// which is most of them -- costing exactly what it cost before.
constexpr unsigned char kExpiryFlag = 0x02;

}  // namespace

SstableWriter::SstableWriter(const std::string& path, int bits_per_key)
    : bits_per_key_(bits_per_key) {
  fd_.reset(::open(path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644));
  if (!fd_.valid()) throw_errno("open", path);
}

void SstableWriter::add(std::string_view key, std::string_view value,
                        bool tombstone, int64_t expires_at_ms) {
  if (failed_) return;  // nothing to gain by writing past a failure

  // Same rule as the log: a tombstone with a lifetime would be a deleted key
  // scheduled to come back. Dropped here rather than trusted not to arrive.
  const bool carries_expiry = !tombstone && expires_at_ms != 0;

  buf_.clear();
  put_u32(buf_, static_cast<uint32_t>(key.size()));
  put_u32(buf_, static_cast<uint32_t>(value.size()));
  unsigned char flags = tombstone ? kTombstoneFlag : 0;
  if (carries_expiry) flags |= kExpiryFlag;
  buf_.push_back(static_cast<char>(flags));
  if (carries_expiry) put_u64(buf_, static_cast<uint64_t>(expires_at_ms));
  buf_.append(key);
  buf_.append(value);

  // The index entry goes in before the data it points at, so its offset is
  // where this entry begins rather than where it ends. The first entry always
  // gets one, which is what makes a lookup for a key below every index key
  // still have somewhere to start.
  if (index_.empty() || bytes_since_index_ >= kIndexInterval) {
    index_.push_back({std::string(key), offset_});
    bytes_since_index_ = 0;
  }

  if (!write_all(fd_.get(), buf_)) {
    failed_ = true;
    return;
  }
  offset_ += buf_.size();
  bytes_since_index_ += buf_.size();
  ++entry_count_;
  bloom_keys_.emplace_back(key);
}

bool SstableWriter::finish() {
  if (failed_ || finished_) return false;
  finished_ = true;

  const uint64_t index_offset = offset_;

  std::string block;
  put_u32(block, static_cast<uint32_t>(index_.size()));
  for (const IndexEntry& e : index_) {
    put_u32(block, static_cast<uint32_t>(e.key.size()));
    block.append(e.key);
    put_u64(block, e.offset);
  }
  if (!write_all(fd_.get(), block)) return false;
  const uint64_t bloom_offset = index_offset + block.size();

  std::vector<std::string_view> keys(bloom_keys_.begin(), bloom_keys_.end());
  const std::string filter = bloom::build(keys, bits_per_key_);
  if (!write_all(fd_.get(), filter)) return false;

  std::string footer;
  put_u64(footer, index_offset);
  put_u64(footer, bloom_offset);
  put_u64(footer, static_cast<uint64_t>(entry_count_));
  footer.append(kMagic, sizeof(kMagic));
  if (!write_all(fd_.get(), footer)) return false;

  // Durable before anyone is told this table exists. A flush may only
  // truncate the WAL after this returns true -- truncating first would leave
  // a window where the data is in neither place.
  for (;;) {
    if (::fsync(fd_.get()) == 0) break;
    if (errno == EINTR) continue;
    return false;
  }
  return true;
}

Sstable::Sstable(const std::string& path, bool use_bloom)
    : path_(path), use_bloom_(use_bloom) {
  fd_.reset(::open(path.c_str(), O_RDONLY));
  if (!fd_.valid()) throw_errno("open", path);

  const off_t size = ::lseek(fd_.get(), 0, SEEK_END);
  if (size < 0) throw_errno("lseek", path);
  if (static_cast<uint64_t>(size) < kFooterSize) {
    throw_format("sstable is too short to hold a footer", path);
  }

  // The footer is read first because nothing else in the file can be located
  // without it -- that is the whole reason it sits at the end, where a writer
  // that streams the data block can still reach it.
  std::string footer;
  if (!read_exact(fd_.get(), static_cast<uint64_t>(size) - kFooterSize,
                  kFooterSize, &footer)) {
    throw_errno("read footer", path);
  }
  // v1 is still read, so a table written before entries could expire keeps
  // serving rather than a format change costing a wipe. Only v2 is written.
  if (std::memcmp(footer.data() + 24, kMagic, sizeof(kMagic)) != 0 &&
      std::memcmp(footer.data() + 24, kMagicV1, sizeof(kMagicV1)) != 0) {
    throw_format("not a cachedb sstable, or a version we do not know", path);
  }

  const uint64_t index_offset = load_u64(footer.data());
  const uint64_t bloom_offset = load_u64(footer.data() + 8);
  entry_count_ = static_cast<size_t>(load_u64(footer.data() + 16));

  // Offsets from the file are claims, and a file may be damaged or forged.
  // Check they are ordered and inside the file before seeking anywhere.
  const uint64_t footer_offset = static_cast<uint64_t>(size) - kFooterSize;
  if (index_offset > bloom_offset || bloom_offset > footer_offset) {
    throw_format("sstable footer offsets are not in order", path);
  }
  data_end_ = index_offset;

  std::string block;
  if (!read_exact(fd_.get(), index_offset,
                  static_cast<size_t>(bloom_offset - index_offset), &block)) {
    throw_errno("read index", path);
  }
  if (block.size() < 4) throw_format("sstable index is truncated", path);

  const uint32_t count = load_u32(block.data());
  size_t pos = 4;
  index_.reserve(count);
  for (uint32_t i = 0; i < count; ++i) {
    if (pos + 4 > block.size()) throw_format("sstable index is truncated", path);
    const uint32_t klen = load_u32(block.data() + pos);
    pos += 4;
    if (pos + klen + 8 > block.size()) {
      throw_format("sstable index is truncated", path);
    }
    IndexEntry e;
    e.key.assign(block, pos, klen);
    pos += klen;
    e.offset = load_u64(block.data() + pos);
    pos += 8;
    if (e.offset > data_end_) {
      throw_format("sstable index points past its data", path);
    }
    index_.push_back(std::move(e));
  }

  if (!read_exact(fd_.get(), bloom_offset,
                  static_cast<size_t>(footer_offset - bloom_offset), &bloom_)) {
    throw_errno("read bloom", path);
  }
}

Sstable::Lookup Sstable::get(std::string_view key) const {
  // The filter first, because the whole point of it is to answer without
  // touching the disk at all. Skipping it is a benchmark-only setting: the
  // lookup below then reads a block from every table that could hold the key,
  // which is precisely the cost the filter exists to avoid.
  if (use_bloom_ && !bloom::may_contain(bloom_, key)) return {};
  if (index_.empty()) return {};

  // The last index entry whose key is <= the one we want. upper_bound finds
  // the first strictly greater, so the one before it is the block that could
  // contain the key. If even the first index key is greater, no block can.
  const auto it = std::upper_bound(
      index_.begin(), index_.end(), key,
      [](std::string_view k, const IndexEntry& e) { return k < e.key; });
  if (it == index_.begin()) return {};
  const size_t slot = static_cast<size_t>(it - index_.begin()) - 1;

  const uint64_t from = index_[slot].offset;
  const uint64_t to = (slot + 1 < index_.size()) ? index_[slot + 1].offset
                                                 : data_end_;
  if (to <= from) return {};

  // One read for the whole block, bounded by the index interval rather than
  // by how the keys happened to be sized.
  std::string block;
  if (!read_exact(fd_.get(), from, static_cast<size_t>(to - from), &block)) {
    return {};
  }

  size_t pos = 0;
  while (pos + 9 <= block.size()) {
    const uint32_t klen = load_u32(block.data() + pos);
    const uint32_t vlen = load_u32(block.data() + pos + 4);
    const unsigned char flags =
        static_cast<unsigned char>(block[pos + 8]);
    // The expiry, when present, sits between the flags byte and the key, so
    // where the key starts depends on a bit we have only just read.
    const bool has_expiry = (flags & kExpiryFlag) != 0;
    const size_t stamp = pos + 9;
    const size_t body = stamp + (has_expiry ? 8 : 0);
    // Checked before the stamp is loaded, not after: an entry claiming an
    // expiry it does not have room for would otherwise read eight bytes off
    // the end of the block.
    if (body + klen + vlen > block.size()) break;  // truncated, stop believing

    const std::string_view entry_key(block.data() + body, klen);
    if (entry_key == key) {
      Lookup found;
      found.found = true;
      found.tombstone = (flags & kTombstoneFlag) != 0;
      if (has_expiry) {
        found.expires_at_ms =
            static_cast<int64_t>(load_u64(block.data() + stamp));
      }
      if (!found.tombstone) found.value.assign(block, body + klen, vlen);
      return found;
    }
    // Sorted, so the first key past the target settles it: scanning further
    // cannot find it, and this is what keeps the scan bounded by the block.
    if (entry_key > key) break;

    pos = body + klen + vlen;
  }
  return {};
}

}  // namespace cachedb
