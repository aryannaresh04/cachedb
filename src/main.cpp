#include <sys/stat.h>

#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <string>
#include <system_error>

#include "server.h"
#include "store.h"
#include "wal.h"

namespace {

constexpr uint16_t kDefaultPort = 6379;
constexpr const char* kDefaultDir = "data";

void usage(const char* argv0) {
  std::fprintf(
      stderr,
      "usage: %s [--port N] [--dir PATH] [--fsync always|everysec|no]\n",
      argv0);
}

bool parse_fsync(const char* name, cachedb::SyncPolicy* out) {
  if (std::strcmp(name, "always") == 0) {
    *out = cachedb::SyncPolicy::kAlways;
  } else if (std::strcmp(name, "everysec") == 0) {
    *out = cachedb::SyncPolicy::kEverySec;
  } else if (std::strcmp(name, "no") == 0) {
    *out = cachedb::SyncPolicy::kNo;
  } else {
    return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  uint16_t port = kDefaultPort;
  std::string dir = kDefaultDir;
  // PROJECT.md 6.4: the safe policy is the default. Losing writes should take
  // a deliberate flag, never an omission.
  cachedb::SyncPolicy policy = cachedb::SyncPolicy::kAlways;

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
    if (std::strcmp(argv[i], "--dir") == 0 && i + 1 < argc) {
      dir = argv[++i];
      continue;
    }
    if (std::strcmp(argv[i], "--fsync") == 0 && i + 1 < argc) {
      if (!parse_fsync(argv[++i], &policy)) {
        std::fprintf(stderr, "cachedb: unknown fsync policy: %s\n", argv[i]);
        usage(argv[0]);
        return 1;
      }
      continue;
    }
    usage(argv[0]);
    return 1;
  }

  try {
    // One directory for the log now and the SSTables from M3, so recency and
    // ownership of the data set live in one place a person can delete.
    if (::mkdir(dir.c_str(), 0755) != 0 && errno != EEXIST) {
      throw std::system_error(errno, std::generic_category(), "mkdir " + dir);
    }
    const std::string wal_path = dir + "/wal.log";

    cachedb::Store store;

    // Recovery runs before the listener exists, so no client can read a state
    // that is still being rebuilt.
    //
    // It also runs before the log is opened for appending, and that order is
    // required rather than tidy: replay() cuts a torn tail off the file, and
    // Wal records the file's length when it opens. Opening first would cache a
    // length the truncation then invalidates.
    const cachedb::ReplayResult recovered = cachedb::replay(
        wal_path,
        [&store](const cachedb::Record& record) { store.apply(record); });

    if (recovered.records > 0 || recovered.truncated) {
      std::fprintf(stderr, "cachedb: replayed %zu records from %s\n",
                   recovered.records, wal_path.c_str());
    }
    if (recovered.truncated) {
      // Expected after a kill -9, not a fault. Said out loud anyway, because
      // "we threw away the end of your log" should never be silent.
      std::fprintf(stderr,
                   "cachedb: discarded an incomplete tail; log cut to %llu "
                   "bytes\n",
                   static_cast<unsigned long long>(recovered.good_bytes));
    }

    cachedb::Wal wal(wal_path, policy);
    store.set_wal(&wal);

    cachedb::Server server(store, wal, port);
    std::fprintf(stderr, "cachedb listening on 127.0.0.1:%u, %zu keys, log %s\n",
                 port, store.size(), wal_path.c_str());
    server.run();
  } catch (const std::exception& e) {
    // Startup failures are the exceptional case PROJECT.md 11 allows
    // exceptions for: there is nothing to degrade to if the port will not bind
    // or the log will not open.
    std::fprintf(stderr, "cachedb: %s\n", e.what());
    return 1;
  }
  return 0;
}
