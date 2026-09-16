#pragma once

#include <unistd.h>

namespace cachedb
{

  // Owns a file descriptor and closes it exactly once. PROJECT.md 11: no bare
  // close() calls scattered about, so a path that returns early or throws
  // cannot leak one.
  //
  // Lives in its own header rather than inside server.h because that header
  // includes <sys/epoll.h> and is therefore Linux-only, while the WAL needs
  // the same ownership on every host its tests run on -- including the macOS
  // box where the server itself is not built at all.
  class Fd
  {
  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    ~Fd() { reset(); }

    Fd(Fd &&other) noexcept : fd_(other.release()) {}
    Fd &operator=(Fd &&other) noexcept
    {
      // Safe against self-assignment: release() clears fd_ before reset()
      // looks at it, so the descriptor is never closed and then kept.
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

    void reset(int fd = -1)
    {
      if (fd_ >= 0) ::close(fd_);
      fd_ = fd;
    }

  private:
    int fd_ = -1;
  };

} // namespace cachedb
