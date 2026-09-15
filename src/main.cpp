#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>

#include "server.h"
#include "store.h"

namespace {

constexpr uint16_t kDefaultPort = 6379;

void usage(const char* argv0) {
  std::fprintf(stderr, "usage: %s [--port N]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = kDefaultPort;

  for (int i = 1; i < argc; ++i) {
    if (std::strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
      const long value = std::strtol(argv[++i], nullptr, 10);
      if (value <= 0 || value > 65535) {
        std::fprintf(stderr, "cachedb: port out of range: %s\n", argv[i]);
        return 1;
      }
      port = static_cast<uint16_t>(value);
      continue;
    }
    usage(argv[0]);
    return 1;
  }

  try {
    cachedb::Store store;
    cachedb::Server server(store, port);
    std::fprintf(stderr, "cachedb listening on 127.0.0.1:%u\n", port);
    server.run();
  } catch (const std::exception& e) {
    // Startup failures are the exceptional case PROJECT.md 11 allows
    // exceptions for: there is nothing to degrade to if the port will not bind.
    std::fprintf(stderr, "cachedb: %s\n", e.what());
    return 1;
  }
  return 0;
}
