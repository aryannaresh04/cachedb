# Build and test environment for cachedb.
#
# The engine is Linux-only by design: epoll has no macOS equivalent worth
# abstracting over (PROJECT.md 3). On a Mac host CMake builds the core library
# and the unit tests but skips server.cpp entirely -- so everything that
# involves a real socket (the M1 acceptance test, crash_test.sh,
# redis-benchmark) can only run in here.
#
# This is a toolchain image, not a shipping image. The source is bind-mounted
# rather than COPYed: the whole value of the container is the edit-build-test
# loop, and rebuilding an image on every source edit destroys it. It also lets
# the M2 crash test kill -9 the server and restart it against a WAL that
# outlives the container.
#
#   Build:  docker build -t cachedb-dev .
#   Shell:  docker run --rm -it -v "$PWD":/cachedb cachedb-dev
#
# Then, inside:
#   cmake -S . -B build-linux && cmake --build build-linux -j
#   ctest --test-dir build-linux --output-on-failure
#   ./build-linux/cachedb &
#   redis-cli -p 6379 set foo bar && redis-cli -p 6379 get foo
#
# Use build-linux/, not build/: the latter holds a macOS CMakeCache.txt with
# absolute host paths baked in, and CMake will refuse or misbehave if a Linux
# toolchain is pointed at it. .gitignore's build*/ already covers both.

# Native to the host architecture -- arm64 on Apple Silicon. Forcing
# linux/amd64 would put every build and every benchmark under qemu emulation,
# slow enough to make the numbers PROJECT.md 10 asks for meaningless. epoll,
# fsync and CRC32 are not architecture-sensitive.
FROM debian:bookworm-slim

# build-essential   g++ 12 (full C++17), make, and the libasan/libubsan
#                   runtimes the default sanitized build links against
# cmake             build system
# redis-tools       redis-cli and redis-benchmark -- these ARE the M1 and M4
#                   acceptance tests, not conveniences
# redis-server      real Redis, to benchmark against on this same machine.
#                   PROJECT.md 10 wants identical workloads run side by side,
#                   because a throughput number compared against someone
#                   else's hardware means nothing at all
# netcat-openbsd    raw inline-command testing, which PROJECT.md 6.2 calls out
#                   as the reason inline commands are supported at all
# procps            pgrep/ps for the M2 crash test
#
# Deliberately absent: gdb. Add it when there is something to debug rather
# than carrying ~50 MB on the chance.
RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      cmake \
      redis-tools \
      redis-server \
      netcat-openbsd \
      procps \
 && rm -rf /var/lib/apt/lists/*

# No EXPOSE, and publishing a port will not do what you expect: the server
# binds INADDR_LOOPBACK (server.cpp), so it listens on the container's own
# loopback, not on the interface a -p mapping forwards to. Connect from inside
# the container -- docker exec, or the shell this image drops you into. That is
# the intended posture anyway: there is no auth and no TLS, and the container
# boundary is doing the job the loopback bind does on a normal host.

WORKDIR /cachedb
CMD ["/bin/bash"]
