#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <unordered_map>

#include "connection.h"
#include "store.h"

namespace cachedb
{

  // Owns a file descriptor and closes it exactly once. PROJECT.md 11: no bare
  // close() calls scattered about, so a path that returns early or throws
  // cannot leak one.
  class Fd
  {
  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(Fd &&other) noexcept : fd_(other.release()) {}
    Fd &operator=(Fd &&other) noexcept
    {
      reset(other.release());
      return *this;
    }
    Fd(const Fd &) = delete;
    Fd &operator=(const Fd &) = delete;

    int get() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    int release()
    {
      const int fd = fd_;
      fd_ = -1;
      return fd;
    }
    void reset(int fd = -1);

  private:
    int fd_ = -1;
  };

  // The event loop: single threaded and level triggered, per PROJECT.md 3 and
  // 6.1. Level triggered because a missed readiness notification under edge
  // triggering is a hang that reproduces once a week, and correctness comes
  // before the syscall it would save.
  class Server
  {
  public:
    // Throws on any setup failure. A port already in use is a startup problem
    // for the operator to fix, not a condition to recover from.
    Server(Store &store, uint16_t port);

    // Runs until the process is killed.
    void run();

  private:
    struct ConnState
    {
      Fd fd;
      Connection conn;
      // What we last told epoll we cared about, kept so an unchanged interest
      // does not cost an epoll_ctl on every event.
      uint32_t interest = EPOLLIN;
    };

    void accept_ready();
    void service(int fd, uint32_t events);
    bool read_available(ConnState &state);
    bool write_pending(ConnState &state);
    void update_interest(int fd, ConnState &state);
    void close_connection(int fd);

    Store &store_;
    Fd epoll_;
    Fd listener_;
    std::unordered_map<int, ConnState> conns_;
  };

} // namespace cachedb
