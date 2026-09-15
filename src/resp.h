#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace cachedb
{

  // A single client request. args[0] is the command name; the rest are its
  // arguments.
  //
  // Arguments own their bytes. The alternative, string_views into the
  // connection's read buffer, avoids a copy per argument, but binds every
  // Command to the lifetime and address of the buffer that produced it: any
  // growth of that buffer reallocates and dangles every view. We take the copy
  // and keep Command independent of its source.
  struct Command
  {
    std::vector<std::string> args;

    bool empty() const { return args.empty(); }
    void clear() { args.clear(); }
  };

  // Incremental parser for RESP *requests*.
  //
  // The request grammar is much narrower than the reply grammar. A client only
  // ever sends one of two things:
  //
  //   multibulk:  *3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n
  //   inline:     SET k v\n
  //
  // '+', '-' and ':' are reply-only prefixes and are rejected here, as are the
  // null forms ($-1, *-1) which are meaningful only in replies.
  //
  // The parser is resumable: bytes may arrive in any chunking, including one
  // byte at a time, and a single command may span any number of read() calls.
  // Feeding it N bytes one at a time must produce exactly the same result as
  // feeding it all N at once.
  class RespParser
  {
  public:
    // Caps exist to bound work an unauthenticated client can force us to do.
    // Without them "*999999999\r\n" reserves a billion strings, and a client
    // that opens a line and never terminates it buffers without limit.
    static constexpr int64_t kMaxMultibulkLen = 1024 * 1024;
    static constexpr int64_t kMaxBulkLen = 512LL * 1024 * 1024; // matches the
                                                                // read buffer
                                                                // cap
    // Applies both to inline commands and to protocol header lines ("*3",
    // "$5"), neither of which has any business being long.
    static constexpr size_t kMaxLineLen = 64 * 1024;

    enum class Status
    {
      NeedMoreData,  // no complete command yet; feed more bytes
      Complete,      // `out` holds exactly one command
      ProtocolError, // framing is lost; the caller must close the connection
    };

    struct Result
    {
      Status status = Status::NeedMoreData;
      // Bytes from the front of `in` the parser absorbed. The caller discards
      // exactly this many before calling again.
      size_t consumed = 0;
      // Client-facing message, non-empty only when status == ProtocolError.
      // Always points at a string literal, so it outlives the parser.
      std::string_view error;
    };

    // Parses at most one command from `in`, stopping as soon as a command
    // completes so the caller can execute it before parsing the next.
    //
    // A protocol error is unrecoverable: once framing is lost there is no way
    // to resynchronize a byte stream, so the connection must be closed rather
    // than reset. Unknown commands and wrong arity are *not* protocol errors --
    // those belong to the dispatcher and leave the connection open.
    Result parse(std::string_view in, Command &out);

    // Drops all partial state. Does not clear a Failed parser.
    void reset();

  private:
    enum class State
    {
      ReadingType,         // deciding multibulk vs inline
      ReadingMultibulkLen, // rest of "*<n>\r\n"
      ReadingBulkLen,      // "$<n>\r\n"
      ReadingBulkBody,     // <n> bytes of payload
      ReadingBulkCrlf,     // the CRLF that follows the payload
      ReadingInline,       // whitespace-separated args up to '\n'
      Failed,              // sticky; a protocol error was already reported
    };

    State state_ = State::ReadingType;
    Command pending_;
    size_t multibulk_remaining_ = 0; // arguments still to read
    size_t bulk_remaining_ = 0;      // payload bytes still to read
  };


  // --------------------------------------------------------------------
  // Reply serialization
  //
  // Every function appends to `out` instead of returning a string. The
  // destination is always the connection's write buffer, and when one read()
  // delivers several commands their replies must land there back to back --
  // appending does that without an intermediate allocation per reply.
  //
  // Note this is the opposite choice from Command, which owns its bytes. The
  // lifetimes differ: nothing here is retained past the call, so there is no
  // buffer that can move out from under us.
  // --------------------------------------------------------------------

  // Line-terminated replies: framing is "read to the next CRLF", so a CR or LF
  // inside the payload would forge a reply boundary. Use these only for
  // strings we control -- literals and status codes. Anything a client gave us
  // goes through append_bulk_string, which is length-prefixed and therefore
  // cannot be escaped out of.
  void append_simple_string(std::string &out, std::string_view s);

  // `msg` carries its own error code, e.g. "ERR unknown command 'foo'".
  void append_error(std::string &out, std::string_view msg);

  void append_integer(std::string &out, int64_t n);

  // Length-prefixed, so any byte sequence is safe: an embedded NUL, CRLF, or
  // even an entire well-formed reply is just payload.
  void append_bulk_string(std::string &out, std::string_view s);

  // "$-1", the reply for a key that does not exist. Deliberately distinct from
  // an empty bulk string ("$0"), which is a key that exists and holds "".
  // GET has to be able to tell those apart.
  void append_null_bulk(std::string &out);

  // Writes the "*<n>" header only; the caller appends the n elements itself.
  // Arrays nest, so building them incrementally avoids needing a tree type
  // just to describe a reply.
  void append_array_header(std::string &out, int64_t n);

} // namespace cachedb
