#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "command.h"
#include "resp.h"
#include "store.h"

namespace cachedb
{

  // Everything one client connection needs except the socket: the bytes read
  // so far, the replies not yet written, and the parser's position between
  // them.
  //
  // Deliberately contains no socket or epoll code. It is fed bytes and hands
  // back bytes, which is what lets the hard parts -- a command split across
  // two reads, three commands in one, a write() that only takes half -- be
  // tested without a network.
  class Connection
  {
  public:
    // PROJECT.md 6.1. A client must not be able to make us buffer without
    // bound by opening a command and never finishing it.
    static constexpr size_t kMaxReadBuffer = 512u * 1024 * 1024;

    explicit Connection(size_t max_read_buffer = kMaxReadBuffer)
        : max_read_buffer_(max_read_buffer) {}

    // Hands over bytes that just arrived, executing every command they
    // complete and queueing the replies.
    //
    // Returns false when the connection is finished: framing was lost, or the
    // client overran the read buffer. The explanatory error is already queued,
    // so the caller should still flush what is pending before closing --
    // see should_close().
    bool on_bytes(std::string_view bytes, Store &store);

    // Bytes still waiting to go out. Empty when there is nothing to write.
    std::string_view pending_output() const;

    // Reports how many bytes write() actually accepted, which on a busy socket
    // is routinely fewer than were offered.
    void consume_output(size_t n);

    bool has_pending_output() const { return !pending_output().empty(); }

    // True once the error reply has drained and the socket can be closed.
    bool should_close() const { return closing_ && !has_pending_output(); }

  private:
    // Draining the output a piece at a time is normal, so the written prefix
    // is tracked with an offset instead of being erased on every write. It is
    // compacted only when the dead prefix is both sizeable and most of the
    // buffer, which keeps repeated partial writes from turning into
    // quadratic copying.
    static constexpr size_t kCompactThreshold = 64 * 1024;

    RespParser parser_;
    std::string in_;
    std::string out_;
    size_t out_pos_ = 0;
    size_t max_read_buffer_;
    bool closing_ = false;
  };

} // namespace cachedb
