#include "server.h"

#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
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

[[noreturn]] void throw_errno(const char* what) {
  throw std::system_error(errno, std::generic_category(), what);
}

}  // namespace

Server::Server(Store& store, uint16_t port) : store_(store) {
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

void Server::run() {
  std::vector<epoll_event> events(kMaxEvents);
  for (;;) {
    const int n =
        ::epoll_wait(epoll_.get(), events.data(), kMaxEvents, /*timeout=*/-1);
    if (n < 0) {
      // A signal arriving during the wait is routine, not a failure.
      if (errno == EINTR) continue;
      throw_errno("epoll_wait");
    }
    for (int i = 0; i < n; ++i) {
      if (events[i].data.fd == listener_.get()) {
        accept_ready();
      } else {
        service(events[i].data.fd, events[i].events);
      }
    }
  }
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

void Server::service(int fd, uint32_t events) {
  const auto it = conns_.find(fd);
  if (it == conns_.end()) return;
  ConnState& state = it->second;

  // EPOLLHUP or EPOLLERR means the socket is finished; there is nobody left to
  // send a reply to.
  if (events & (EPOLLERR | EPOLLHUP)) {
    close_connection(fd);
    return;
  }

  if ((events & EPOLLIN) && !read_available(state)) {
    close_connection(fd);
    return;
  }

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
