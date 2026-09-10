#include "resp.h"

#include <algorithm>
#include <charconv>
#include <utility>

namespace cachedb {
namespace {

constexpr std::string_view kErrMultibulkLen =
    "Protocol error: invalid multibulk length";
constexpr std::string_view kErrBulkLen = "Protocol error: invalid bulk length";
constexpr std::string_view kErrExpectedDollar = "Protocol error: expected '$'";
constexpr std::string_view kErrUnterminatedBulk =
    "Protocol error: bulk string not terminated by CRLF";
constexpr std::string_view kErrLineTooLong =
    "Protocol error: too big inline request";
// Reported if the caller keeps parsing after an error instead of closing.
constexpr std::string_view kErrAlreadyFailed = "Protocol error: framing lost";

// Reserve caps. A declared length is a client's claim, not a fact: honouring
// "$536870912" before a single payload byte arrives hands out 512 MB to a
// client that may then send nothing. Reserve a sane amount and let the vector
// or string grow if the bytes actually show up.
constexpr size_t kArgReserveCap = 16;
constexpr size_t kBulkReserveCap = 64 * 1024;

// Strict integer parse: no leading whitespace, no '+', no trailing junk, and
// overflow is a failure rather than a wrap. std::stoll would accept the first
// two, and throws.
bool parse_i64(std::string_view s, int64_t& out) {
  if (s.empty()) return false;
  const char* end = s.data() + s.size();
  auto [ptr, ec] = std::from_chars(s.data(), end, out);
  return ec == std::errc() && ptr == end;
}

bool is_inline_space(char c) { return c == ' ' || c == '\t'; }

}  // namespace

void RespParser::reset() {
  if (state_ == State::Failed) return;
  state_ = State::ReadingType;
  pending_.clear();
  multibulk_remaining_ = 0;
  bulk_remaining_ = 0;
}

RespParser::Result RespParser::parse(std::string_view in, Command& out) {
  size_t pos = 0;

  auto need_more = [&]() { return Result{Status::NeedMoreData, pos, {}}; };
  auto fail = [&](std::string_view msg) {
    state_ = State::Failed;
    return Result{Status::ProtocolError, pos, msg};
  };

  for (;;) {
    switch (state_) {
      case State::Failed:
        return Result{Status::ProtocolError, pos, kErrAlreadyFailed};

      case State::ReadingType: {
        if (pos == in.size()) return need_more();
        if (in[pos] == '*') {
          ++pos;  // the type byte is consumed even if the length line is not
                  // yet complete; state_ remembers where we are.
          state_ = State::ReadingMultibulkLen;
        } else {
          state_ = State::ReadingInline;
        }
        break;
      }

      case State::ReadingMultibulkLen: {
        // Protocol lines are consumed all-or-nothing: until the terminator is
        // present we absorb none of them, so there is no partial-line scratch
        // buffer to maintain.
        const size_t crlf = in.find("\r\n", pos);
        if (crlf == std::string_view::npos) {
          if (in.size() - pos > kMaxLineLen) return fail(kErrLineTooLong);
          return need_more();
        }
        int64_t n = 0;
        if (!parse_i64(in.substr(pos, crlf - pos), n) || n > kMaxMultibulkLen) {
          return fail(kErrMultibulkLen);
        }
        pos = crlf + 2;
        if (n <= 0) {
          // "*0\r\n" is a well-formed request carrying no command. Skip it and
          // look for the next one rather than handing the dispatcher an empty
          // Command.
          reset();
          break;
        }
        multibulk_remaining_ = static_cast<size_t>(n);
        pending_.args.reserve(std::min<size_t>(multibulk_remaining_,
                                               kArgReserveCap));
        state_ = State::ReadingBulkLen;
        break;
      }

      case State::ReadingBulkLen: {
        if (pos == in.size()) return need_more();
        if (in[pos] != '$') return fail(kErrExpectedDollar);
        const size_t crlf = in.find("\r\n", pos + 1);
        if (crlf == std::string_view::npos) {
          if (in.size() - pos > kMaxLineLen) return fail(kErrLineTooLong);
          return need_more();
        }
        int64_t n = 0;
        if (!parse_i64(in.substr(pos + 1, crlf - pos - 1), n) || n < 0 ||
            n > kMaxBulkLen) {
          // Unlike a multibulk count, a negative bulk length is an error
          // rather than a skip: "$-1" is a null *reply*, never a request.
          return fail(kErrBulkLen);
        }
        pos = crlf + 2;
        bulk_remaining_ = static_cast<size_t>(n);
        pending_.args.emplace_back();
        pending_.args.back().reserve(
            std::min<size_t>(bulk_remaining_, kBulkReserveCap));
        state_ = State::ReadingBulkBody;
        break;
      }

      case State::ReadingBulkBody: {
        // The payload, unlike a protocol line, is consumed greedily: a large
        // value arriving in fragments is appended as it lands so the
        // connection's buffer can be drained instead of holding the whole
        // value before we touch it.
        const size_t take = std::min(in.size() - pos, bulk_remaining_);
        pending_.args.back().append(in.data() + pos, take);
        pos += take;
        bulk_remaining_ -= take;
        if (bulk_remaining_ > 0) return need_more();
        state_ = State::ReadingBulkCrlf;
        break;
      }

      case State::ReadingBulkCrlf: {
        if (in.size() - pos < 2) return need_more();
        // Redis skips these two bytes unchecked. We validate them: a payload
        // not followed by CRLF means the declared length disagrees with the
        // stream, and continuing would silently parse garbage as commands.
        if (in[pos] != '\r' || in[pos + 1] != '\n') {
          return fail(kErrUnterminatedBulk);
        }
        pos += 2;
        --multibulk_remaining_;
        if (multibulk_remaining_ == 0) {
          out = std::move(pending_);
          reset();
          return Result{Status::Complete, pos, {}};
        }
        state_ = State::ReadingBulkLen;
        break;
      }

      case State::ReadingInline: {
        // Inline terminates on '\n', not on CRLF: this mode exists so a raw
        // `nc` session is usable for debugging, and nc sends a bare '\n'.
        const size_t nl = in.find('\n', pos);
        if (nl == std::string_view::npos) {
          if (in.size() - pos > kMaxLineLen) return fail(kErrLineTooLong);
          return need_more();
        }
        size_t end = nl;
        if (end > pos && in[end - 1] == '\r') --end;  // tolerate CRLF too

        // Whitespace split only; quotes are not honoured, so an argument
        // containing a space cannot be sent inline. Use redis-cli for those.
        std::string_view line = in.substr(pos, end - pos);
        pending_.clear();
        size_t i = 0;
        while (i < line.size()) {
          while (i < line.size() && is_inline_space(line[i])) ++i;
          if (i == line.size()) break;
          const size_t start = i;
          while (i < line.size() && !is_inline_space(line[i])) ++i;
          pending_.args.emplace_back(line.substr(start, i - start));
        }
        pos = nl + 1;
        if (pending_.empty()) {
          // A blank line is how a human using nc produces nothing at all.
          reset();
          break;
        }
        out = std::move(pending_);
        reset();
        return Result{Status::Complete, pos, {}};
      }
    }
  }
}

}  // namespace cachedb
