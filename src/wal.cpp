#include "wal.h"

#include <array>

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

}  // namespace cachedb
