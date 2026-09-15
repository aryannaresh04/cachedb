#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "connection.h"

#include <string>

using cachedb::Connection;
using cachedb::Store;

namespace {

// Drains everything queued, as a socket that accepts all of it would.
std::string drain(Connection& c) {
  std::string out(c.pending_output());
  c.consume_output(out.size());
  return out;
}

}  // namespace

TEST_CASE("bytes in, replies out") {
  Store store;
  Connection c;

  SUBCASE("one command") {
    CHECK(c.on_bytes("*1\r\n$4\r\nPING\r\n", store));
    CHECK(drain(c) == "+PONG\r\n");
    CHECK_FALSE(c.has_pending_output());
  }

  SUBCASE("three commands in a single read produce three replies") {
    // Exactly what a pipelining client does, and what redis-benchmark does
    // by default.
    CHECK(c.on_bytes(
        "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$1\r\nv\r\n"
        "*2\r\n$3\r\nGET\r\n$1\r\nk\r\n"
        "*1\r\n$4\r\nPING\r\n",
        store));
    CHECK(drain(c) == "+OK\r\n$1\r\nv\r\n+PONG\r\n");
  }

  SUBCASE("a command split across two reads") {
    CHECK(c.on_bytes("*2\r\n$3\r\nGE", store));
    CHECK_FALSE(c.has_pending_output());  // nothing complete yet
    CHECK(c.on_bytes("T\r\n$1\r\nk\r\n", store));
    CHECK(drain(c) == "$-1\r\n");
  }

  SUBCASE("one byte at a time still works") {
    const std::string wire = "*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$2\r\nhi\r\n";
    for (char ch : wire) CHECK(c.on_bytes(std::string_view(&ch, 1), store));
    CHECK(drain(c) == "+OK\r\n");
  }

  SUBCASE("inline commands, as netcat sends them") {
    CHECK(c.on_bytes("PING\n", store));
    CHECK(drain(c) == "+PONG\r\n");
  }
}

TEST_CASE("partial writes") {
  Store store;
  Connection c;
  REQUIRE(c.on_bytes("*1\r\n$4\r\nPING\r\n", store));

  SUBCASE("a socket that takes only part of the reply") {
    CHECK(c.pending_output() == "+PONG\r\n");
    c.consume_output(3);  // "+PO" went out, the rest did not
    CHECK(c.pending_output() == "NG\r\n");
    c.consume_output(4);
    CHECK_FALSE(c.has_pending_output());
  }

  SUBCASE("a reply queued while an earlier one is still draining") {
    c.consume_output(2);  // "+P" went out
    REQUIRE(c.on_bytes("*1\r\n$4\r\nPING\r\n", store));
    // The new reply queues behind the remains of the old one, in order.
    CHECK(c.pending_output() == "ONG\r\n+PONG\r\n");
  }
}

TEST_CASE("the connection closes itself when it has to") {
  Store store;

  SUBCASE("a protocol error explains itself, then ends the connection") {
    Connection c;
    CHECK_FALSE(c.on_bytes("*abc\r\n", store));
    const std::string reply = drain(c);
    CHECK(reply == "-ERR Protocol error: invalid multibulk length\r\n");
    CHECK(c.should_close());
  }

  SUBCASE("nothing is parsed after framing is lost") {
    Connection c;
    CHECK_FALSE(c.on_bytes("*abc\r\n*1\r\n$4\r\nPING\r\n", store));
    CHECK(drain(c).find("PONG") == std::string::npos);
    CHECK_FALSE(c.on_bytes("*1\r\n$4\r\nPING\r\n", store));
  }

  SUBCASE("an unfinished command cannot buffer without bound") {
    Connection c(64);  // tiny cap so the limit is reachable in a test
    CHECK_FALSE(c.on_bytes(std::string(100, 'x'), store));
    CHECK(drain(c) == "-ERR Protocol error: request too large\r\n");
    CHECK(c.should_close());
  }

  SUBCASE("an unknown command is not a reason to close") {
    Connection c;
    CHECK(c.on_bytes("*1\r\n$4\r\nNOPE\r\n", store));
    CHECK(drain(c) == "-ERR unknown command 'NOPE'\r\n");
    CHECK_FALSE(c.should_close());
    CHECK(c.on_bytes("*1\r\n$4\r\nPING\r\n", store));
    CHECK(drain(c) == "+PONG\r\n");
  }
}

TEST_CASE("a large value arriving in fragments") {
  // The read buffer grows across many reads here, which is exactly the case
  // that would dangle a string_view into it.
  Store store;
  Connection c;
  const std::string value(200000, 'z');

  c.on_bytes("*3\r\n$3\r\nSET\r\n$3\r\nbig\r\n$200000\r\n", store);
  for (size_t i = 0; i < value.size(); i += 4096) {
    CHECK(c.on_bytes(std::string_view(value).substr(i, 4096), store));
  }
  CHECK(c.on_bytes("\r\n", store));
  CHECK(drain(c) == "+OK\r\n");

  REQUIRE(c.on_bytes("*2\r\n$3\r\nGET\r\n$3\r\nbig\r\n", store));
  CHECK(drain(c) == "$200000\r\n" + value + "\r\n");
}
