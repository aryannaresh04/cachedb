#include "connection.h"

namespace cachedb {

bool Connection::on_bytes(std::string_view bytes, Store& store) {
  if (closing_) return false;

  if (in_.size() + bytes.size() > max_read_buffer_) {
    append_error(out_, "ERR Protocol error: request too large");
    closing_ = true;
    return false;
  }
  in_.append(bytes);

  // The parser stops at each completed command so it can be executed before
  // the next is read. Its replies go to out_, never to in_, so the view below
  // stays valid across the whole loop.
  size_t consumed = 0;
  Command cmd;
  for (;;) {
    const auto result = parser_.parse(std::string_view(in_).substr(consumed),
                                      cmd);
    consumed += result.consumed;

    if (result.status == RespParser::Status::Complete) {
      execute(cmd, store, out_);
      cmd.clear();
      continue;
    }

    if (result.status == RespParser::Status::ProtocolError) {
      // Tell the client why before hanging up. A byte stream cannot be
      // resynchronized once framing is lost, so there is nothing to recover
      // to -- the connection has to go.
      std::string msg = "ERR ";
      msg += result.error;
      append_error(out_, msg);
      closing_ = true;
    }
    break;  // NeedMoreData, or the error above
  }

  // One memmove per read() rather than one per command: the leftover is
  // whatever partial command is still waiting for its remaining bytes.
  in_.erase(0, consumed);
  return !closing_;
}

std::string_view Connection::pending_output() const {
  return std::string_view(out_).substr(out_pos_);
}

void Connection::consume_output(size_t n) {
  out_pos_ += n;
  if (out_pos_ == out_.size()) {
    out_.clear();
    out_pos_ = 0;
  } else if (out_pos_ >= kCompactThreshold && out_pos_ * 2 >= out_.size()) {
    out_.erase(0, out_pos_);
    out_pos_ = 0;
  }
}

}  // namespace cachedb
