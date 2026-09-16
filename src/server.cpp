#include "server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <system_error>
#include <utility>
#include <vector>

namespace cachedb {
namespace {

// One event batch. Larger costs nothing but memory; smaller just means more
// trips round the loop.
constexpr int kMaxEvents = 256;

// Reused for every read, so a connection costs no per-read allocation.
constexpr size_t kReadChunk = 16 * 1024;

// How long epoll_wait may block with nothing happening. 100 ms is the same
// 10 Hz real Redis runs its cron at.
//
// It has to be finite. Under the everysec policy this tick is the only thing
// that forces the log down, and from M4 it is also what drives active expiry;
// with an infinite wait neither would fire on a server nobody is talking to --
// exactly the quiet period where the power is most likely to go out. Ten idle
// wakeups a second costs nothing measurable.
constexpr int kTickMs = 100;

// Entries the active expiry sweep examines per tick (PROJECT.md 6.8). At the
// 10 Hz tick above that is 1,000 a second, so a memtable holding 15,000 keys
// is walked end to end in about fifteen seconds.
//
// A count and not a time budget. The work per tick is then a constant rather
// than something that varies with how many keys happen to have expired, which
// is what makes its effect on tail latency a number that can be measured once
// instead of a distribution that moves with the workload.
constexpr size_t kSweepBudget = 100;

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

// Set from a signal handler, so it is volatile sig_atomic_t and nothing else.
// Assigning to one of these is the only thing the standard promises is safe to
// do from a handler -- no allocation, no locks, no I/O, no calling back into
// the Server.
volatile std::sig_atomic_t g_shutdown = 0;

extern "C" void on_shutdown_signal(int) { g_shutdown = 1; }

}  // namespace

void install_shutdown_handlers() {
  struct sigaction sa {};
  sa.sa_handler = on_shutdown_signal;
  ::sigemptyset(&sa.sa_mask);
  // Deliberately not SA_RESTART. We want epoll_wait to come back with EINTR
  // so the loop reaches its flag check promptly rather than sitting out the
  // rest of its timeout.
  sa.sa_flags = 0;
  ::sigaction(SIGTERM, &sa, nullptr);
  ::sigaction(SIGINT, &sa, nullptr);
  // SIGPIPE needs no handler: every send() already passes MSG_NOSIGNAL.
}

Server::Server(Store& store, Wal& wal, uint16_t port)
    : store_(store), wal_(wal) {
  listener_.reset(::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0));
  if (!listener_.valid()) throw_errno("socket");

  // Without SO_REUSEADDR a restart fails for a minute or so while the old
  // socket sits in TIME_WAIT, which makes the edit-build-run loop miserable.
  const int one = 1;
  if (::setsockopt(listener_.get(), SOL_SOCKET, SO_REUSEADDR, &one,
                   sizeof(one)) < 0) {
    throw_errno("setsockopt(SO_REUSEADDR)");
  }

  sockaddr_in addr{};
  addr.sin_family = AF_INET;
  addr.sin_port = ::htons(port);
  // Loopback only. There is no authentication and no TLS -- both are explicit
  // non-goals -- so binding every interface would put an open database on the
  // network. Change this to INADDR_ANY deliberately, not by accident.
  addr.sin_addr.s_addr = ::htonl(INADDR_LOOPBACK);

  if (::bind(listener_.get(), reinterpret_cast<const sockaddr*>(&addr),
             sizeof(addr)) < 0) {
    throw_errno("bind");
  }
  if (::listen(listener_.get(), SOMAXCONN) < 0) throw_errno("listen");

  epoll_.reset(::epoll_create1(0));
  if (!epoll_.valid()) throw_errno("epoll_create1");

  epoll_event ev{};
  ev.events = EPOLLIN;
  ev.data.fd = listener_.get();
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, listener_.get(), &ev) < 0) {
    throw_errno("epoll_ctl(listener)");
  }
}

bool Server::run() {
  std::vector<epoll_event> events(kMaxEvents);
  // Sockets serviced this iteration, whose replies are held until the fsync.
  // Reused across iterations so a busy loop costs no allocation.
  std::vector<int> touched;
  touched.reserve(kMaxEvents);
  // Checking a flag once per iteration rather than waking the loop through a
  // self-pipe. That is only safe because the wait has a finite timeout: the
  // signal can land just after this check, and the worst case is noticing it
  // one tick -- 100 ms -- later. With the infinite wait this loop used to have,
  // a flag would have been the wrong answer and a self-pipe or signalfd the
  // right one.
  while (!g_shutdown) {
    const int n = ::epoll_wait(epoll_.get(), events.data(), kMaxEvents,
                               /*timeout=*/kTickMs);
    if (n < 0) {
      // A signal arriving during the wait is routine, not a failure -- and is
      // in fact how a shutdown usually arrives, so go round and re-test the
      // flag rather than treating it as an error.
      if (errno == EINTR) continue;
      throw_errno("epoll_wait");
    }
    // Every ready socket is read and its commands executed before anything is
    // written back. That is what makes one fsync cover the whole iteration.
    touched.clear();
    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == listener_.get()) {
        accept_ready();
      } else if (service(events[i].data.fd, events[i].events)) {
        touched.push_back(events[i].data.fd);
      }
    }

    // Group commit (PROJECT.md 6.4). One fsync for everything this iteration
    // produced, and no reply leaves before it returns.
    //
    // This is the whole of the 20x gap against real Redis, which does exactly
    // this: measured with strace, cachedb issued 1.00 fsyncs per write and
    // Redis 0.02. Redis is not less durable -- it holds the replies too.
    //
    // A read-only iteration syncs nothing, because needs_sync() is false.
    if (wal_.needs_sync() && !wal_.sync()) {
      // Nothing has been acknowledged: every reply for this iteration is
      // still sitting in a buffer, unsent. Exiting is what real Redis does
      // when an AOF write fails under appendfsync=always, and for the same
      // reason -- there is no honest reply to send and no honest way to
      // continue. Every client sees a dropped connection and must treat its
      // write as indeterminate, which is exactly what it is.
      //
      // Carrying on would be worse than it looks: Linux reports a writeback
      // error once and then clears it, so the next fsync could return success
      // over data that is already gone, laundering the failure into an ack.
      std::fprintf(stderr,
                   "cachedb: WAL fsync failed with %zu replies held; nothing "
                   "was acknowledged. Exiting rather than answering.\n",
                   touched.size());
      return false;
    }

    for (const int fd : touched) release(fd);

    // Expired keys nobody has asked for, reclaimed a bounded slice at a time.
    // The lazy half of expiry handles anything a client reads; this handles
    // everything a client never reads, which would otherwise sit in the
    // memtable until a flush happened to sweep it out.
    store_.sweep_expired(kSweepBudget);

    // Compaction runs here rather than straight after a flush, so the two
    // stalls stay separable. The flush's cost is already measured (10); this
    // one is the measurement 6.7 asks for, and folding them together would
    // make neither attributable.
    store_.maybe_compact();

    // Once per tick regardless of whether anything happened, including when
    // epoll_wait returned on the timeout with n == 0. Under always and no this
    // returns immediately; under everysec it is the entire policy.
    if (!wal_.maybe_sync()) {
      // These writes were acknowledged up to a second ago and cannot be made
      // durable after the fact -- which is precisely the bargain everysec
      // offers. Saying so loudly is all that is left. A production system
      // would stop accepting writes here; see PROJECT.md 6.1.
      std::fprintf(stderr,
                   "cachedb: WAL fsync failed, acknowledged writes may be "
                   "lost\n");
    }
  }

  // Stopping cleanly, so force the log down whatever the policy says. Under
  // everysec or no there may be up to a second of writes sitting in the page
  // cache: they would survive this process exiting, since the page cache is
  // the kernel's, but not the machine losing power a moment later. A clean
  // shutdown is the one chance to close that window for free.
  std::fprintf(stderr, "cachedb: shutting down, syncing the log\n");
  if (!wal_.sync()) {
    std::fprintf(stderr, "cachedb: final WAL fsync failed\n");
  }

  // Returning rather than exiting matters: connections and their descriptors
  // unwind through their destructors, and main returns normally, which is what
  // lets ASan's leak check actually run.
  return true;
}

void Server::accept_ready() {
  // One readable notification can cover several pending connections, so accept
  // until the queue is drained.
  for (;;) {
    const int raw = ::accept4(listener_.get(), nullptr, nullptr,
                              SOCK_NONBLOCK);
    if (raw < 0) {
      if (errno == EINTR) continue;
      // The client gave up between its SYN and our accept. Common under load
      // and not our problem; the next one may still be queued.
      if (errno == ECONNABORTED) continue;
      if (errno == EAGAIN || errno == EWOULDBLOCK) return;  // queue drained
      // Out of descriptors. Level-triggered epoll will report the listener
      // readable again immediately, so this spins until a connection closes.
      // The usual fix is to hold a spare descriptor in reserve and give it up
      // to accept-and-close the excess client; worth doing, but not in M1.
      return;
    }

    Fd fd(raw);

    // Request-response traffic is exactly the pattern Nagle's algorithm hurts:
    // the kernel sits on a small reply waiting for more data to coalesce with,
    // and combined with the client's delayed ACK that becomes a tens-of-
    // milliseconds stall in the P99 with no cause visible in our code.
    const int one = 1;
    ::setsockopt(fd.get(), IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

    epoll_event ev{};
    ev.events = EPOLLIN;
    ev.data.fd = fd.get();
    if (::epoll_ctl(epoll_.get(), EPOLL_CTL_ADD, fd.get(), &ev) < 0) {
      continue;  // fd closes as it goes out of scope
    }

    const int key = fd.get();
    conns_.emplace(key, ConnState{std::move(fd), Connection{}, EPOLLIN});
  }
}

bool Server::service(int fd, uint32_t events) {
  const auto it = conns_.find(fd);
  if (it == conns_.end()) return false;
  ConnState& state = it->second;

  // EPOLLHUP or EPOLLERR means the socket is finished; there is nobody left to
  // send a reply to.
  if (events & (EPOLLERR | EPOLLHUP)) {
    close_connection(fd);
    return false;
  }

  if ((events & EPOLLIN) && !read_available(state)) {
    close_connection(fd);
    return false;
  }

  // Deliberately no write here. Whatever the commands queued stays queued
  // until run() has fsynced the log, because a reply is an acknowledgement
  // and an acknowledgement before the fsync is the one thing kAlways exists
  // to prevent.
  return true;
}

void Server::release(int fd) {
  const auto it = conns_.find(fd);
  if (it == conns_.end()) return;  // closed while this iteration ran
  ConnState& state = it->second;

  if (state.conn.has_pending_output() && !write_pending(state)) {
    close_connection(fd);
    return;
  }

  // A protocol error queues its explanation first and closes once that has
  // actually gone out.
  if (state.conn.should_close()) {
    close_connection(fd);
    return;
  }

  update_interest(fd, state);
}

bool Server::read_available(ConnState& state) {
  char buf[kReadChunk];
  for (;;) {
    const ssize_t n = ::read(state.fd.get(), buf, sizeof(buf));
    if (n > 0) {
      state.conn.on_bytes(std::string_view(buf, static_cast<size_t>(n)),
                          store_);
      // One read per event, then back to the loop. Level-triggered epoll will
      // report the socket readable again if more is waiting, so nothing is
      // lost -- and reading until EAGAIN instead would let a single busy
      // client monopolise the loop while everyone else waits.
      return true;
    }
    if (n == 0) return false;  // the peer closed cleanly
    if (errno == EINTR) continue;
    if (errno == EAGAIN || errno == EWOULDBLOCK) return true;
    return false;
  }
}

bool Server::write_pending(ConnState& state) {
  while (state.conn.has_pending_output()) {
    const std::string_view out = state.conn.pending_output();
    // send with MSG_NOSIGNAL rather than write: writing to a socket the client
    // has already closed raises SIGPIPE, whose default action is to kill the
    // process. A client pressing Ctrl-C would otherwise take the server down.
    const ssize_t n =
        ::send(state.fd.get(), out.data(), out.size(), MSG_NOSIGNAL);
    if (n > 0) {
      state.conn.consume_output(static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
      // The kernel's send buffer is full. Whatever is left stays queued and
      // goes out when epoll reports the socket writable.
      return true;
    }
    return false;
  }
  return true;
}

void Server::update_interest(int fd, ConnState& state) {
  const uint32_t want =
      EPOLLIN | (state.conn.has_pending_output() ? EPOLLOUT : 0u);
  if (want == state.interest) return;  // nothing changed, skip the syscall

  epoll_event ev{};
  ev.events = want;
  ev.data.fd = fd;
  if (::epoll_ctl(epoll_.get(), EPOLL_CTL_MOD, fd, &ev) == 0) {
    state.interest = want;
  }
}

void Server::close_connection(int fd) {
  // Erasing destroys the Fd, which closes the descriptor, which removes it
  // from the epoll set. No explicit EPOLL_CTL_DEL needed.
  conns_.erase(fd);
}

}  // namespace cachedb
