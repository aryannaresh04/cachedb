#pragma once

#include <sys/epoll.h>

#include <cstdint>
#include <unordered_map>

#include "connection.h"
#include "fd.h"
#include "store.h"
#include "wal.h"

namespace cachedb
{

  // Installs SIGTERM and SIGINT handlers that ask the event loop to stop.
  // Call once, from main, before run().
  //
  // Process-wide state, so it is a free function rather than something a
  // Server constructor does behind your back.
  //
  // Without this the process could only ever be killed by signal, and a signal
  // does not run the atexit hooks -- which is where ASan's leak check lives.
  // Every leak in the connection lifecycle was invisible until this existed.
  void install_shutdown_handlers();

  // The event loop: single threaded and level triggered, per PROJECT.md 3 and
  // 6.1. Level triggered because a missed readiness notification under edge
  // triggering is a hang that reproduces once a week, and correctness comes
  // before the syscall it would save.
  class Server
  {
  public:
    // Throws on any setup failure. A port already in use is a startup problem
    // for the operator to fix, not a condition to recover from.
    //
    // Borrows both; each must outlive the Server. The log is here only so the
    // loop can drive maybe_sync() -- writes reach it through the Store.
    Server(Store &store, Wal &wal, uint16_t port);

    // Runs until SIGTERM or SIGINT arrives, then returns -- having forced the
    // log down, so a clean stop costs nothing even under everysec or no.
    // False means a fatal durability failure: the log could not be fsynced
    // while replies were being held for it. See run() in server.cpp.
    [[nodiscard]] bool run();

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
    // Reads and executes, but does not write the reply out: under group commit
    // no reply may leave before the iteration's fsync. Returns false if the
    // connection was closed.
    bool service(int fd, uint32_t events);

    // The other half, run after that fsync: writes whatever the command left
    // queued and re-arms the socket's interest.
    void release(int fd);
    bool read_available(ConnState &state);
    bool write_pending(ConnState &state);
    void update_interest(int fd, ConnState &state);
    void close_connection(int fd);

    Store &store_;
    Wal &wal_;
    Fd epoll_;
    Fd listener_;
    std::unordered_map<int, ConnState> conns_;
  };

} // namespace cachedb
