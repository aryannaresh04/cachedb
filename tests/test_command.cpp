#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "command.h"

#include <unistd.h>

#include <string>
#include <utility>
#include <vector>

using cachedb::Command;
using cachedb::Store;
using cachedb::SyncPolicy;
using cachedb::Wal;

namespace {

// Runs one command and returns the bytes that would go out on the wire.
std::string run(Store& store, std::vector<std::string> args) {
  Command cmd;
  cmd.args = std::move(args);
  std::string out;
  cachedb::execute(cmd, store, out);
  return out;
}

}  // namespace

TEST_CASE("the M1 command set") {
  Store s;

  SUBCASE("PING") {
    CHECK(run(s, {"PING"}) == "+PONG\r\n");
    CHECK(run(s, {"PING", "hello"}) == "$5\r\nhello\r\n");
  }

  SUBCASE("SET then GET") {
    CHECK(run(s, {"SET", "username", "aryan"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "username"}) == "$5\r\naryan\r\n");
  }

  SUBCASE("GET of a key that is not there") {
    CHECK(run(s, {"GET", "nothing"}) == "$-1\r\n");
  }

  SUBCASE("GET of an empty value is not the same as a miss") {
    run(s, {"SET", "k", ""});
    CHECK(run(s, {"GET", "k"}) == "$0\r\n\r\n");
  }

  SUBCASE("DEL reports how many keys it removed") {
    run(s, {"SET", "a", "1"});
    run(s, {"SET", "b", "2"});
    CHECK(run(s, {"DEL", "a", "b", "missing"}) == ":2\r\n");
    CHECK(run(s, {"GET", "a"}) == "$-1\r\n");
  }

  SUBCASE("EXISTS counts, and counts repeats") {
    run(s, {"SET", "a", "1"});
    CHECK(run(s, {"EXISTS", "a"}) == ":1\r\n");
    CHECK(run(s, {"EXISTS", "gone"}) == ":0\r\n");
    CHECK(run(s, {"EXISTS", "a", "a"}) == ":2\r\n");
  }

  SUBCASE("COMMAND is stubbed so redis-cli gets a prompt") {
    CHECK(run(s, {"COMMAND"}) == "*0\r\n");
    CHECK(run(s, {"COMMAND", "DOCS"}) == "*0\r\n");
  }

  SUBCASE("values are binary safe end to end") {
    const std::string value("a\r\n+INJECTED\r\nb", 15);
    run(s, {"SET", "k", value});
    // Length-prefixed, so the value cannot masquerade as extra replies.
    CHECK(run(s, {"GET", "k"}) == "$15\r\n" + value + "\r\n");
  }
}

TEST_CASE("command names are case insensitive") {
  Store s;
  CHECK(run(s, {"set", "k", "v"}) == "+OK\r\n");
  CHECK(run(s, {"GeT", "k"}) == "$1\r\nv\r\n");
  CHECK(run(s, {"PiNg"}) == "+PONG\r\n");
}

TEST_CASE("errors that leave the connection usable") {
  Store s;

  SUBCASE("unknown command") {
    CHECK(run(s, {"FLUSHALL"}) == "-ERR unknown command 'FLUSHALL'\r\n");
  }

  SUBCASE("a name too long to be any command") {
    const std::string long_name(200, 'x');
    const std::string reply = run(s, {long_name});
    CHECK(reply.rfind("-ERR unknown command '", 0) == 0);
  }

  SUBCASE("wrong arity names the command in lower case") {
    CHECK(run(s, {"GET"}) ==
          "-ERR wrong number of arguments for 'get' command\r\n");
    CHECK(run(s, {"GET", "a", "b"}) ==
          "-ERR wrong number of arguments for 'get' command\r\n");
    CHECK(run(s, {"DEL"}) ==
          "-ERR wrong number of arguments for 'del' command\r\n");
  }

  SUBCASE("SET options we do not implement are refused, not dropped") {
    // EX and PX are implemented; the conditional ones are not. Refusing beats
    // accepting and ignoring: a client that asks for a conditional write and
    // silently gets an unconditional one has a bug it cannot see.
    CHECK(run(s, {"SET", "k", "v", "NX"}) == "-ERR syntax error\r\n");
    CHECK(run(s, {"SET", "k", "v", "XX"}) == "-ERR syntax error\r\n");
    CHECK(run(s, {"SET", "k", "v", "KEEPTTL"}) == "-ERR syntax error\r\n");
    CHECK(run(s, {"SET", "k", "v", "EX"}) == "-ERR syntax error\r\n");
    CHECK(run(s, {"GET", "k"}) == "$-1\r\n");  // nothing was written
  }

  SUBCASE("the store still works after an error") {
    run(s, {"NOPE"});
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "k"}) == "$1\r\nv\r\n");
  }
}

TEST_CASE("an error reply cannot be forged by the command name") {
  // The name is echoed back into a line-terminated reply, so a client that
  // names its command "BOGUS\r\n+INJECTED" would otherwise get two replies
  // where the server sent one.
  Store s;
  const std::string evil("BOGUS\r\n+INJECTED", 16);
  const std::string reply = run(s, {evil});

  // "+INJECTED" still appears as text, which is harmless -- it is characters
  // inside a quoted name. What would not be harmless is a CRLF in front of it,
  // because that is what would end the error and start a second reply.
  CHECK(reply.find("\r\n+INJECTED") == std::string::npos);
  // Exactly one terminator, and it sits at the end: one reply, not two.
  CHECK(reply.find("\r\n") == reply.size() - 2);
  CHECK(reply == "-ERR unknown command 'BOGUS..+INJECTED'\r\n");
}

TEST_CASE("a mutation that cannot be logged replies with an error") {
  // See test_store.cpp for why /dev/full: it fails every write with ENOSPC,
  // so the durability path can be exercised without a fake. Linux only.
  if (::access("/dev/full", W_OK) != 0) return;

  Wal wal("/dev/full", SyncPolicy::kNo);
  Store s(&wal);

  CHECK(run(s, {"SET", "k", "v"}) ==
        "-ERR the write could not be logged and was not applied\r\n");
  // Not ":0". A delete that was not logged has not happened, and reporting a
  // count would say it had.
  CHECK(run(s, {"DEL", "k"}) ==
        "-ERR the delete could not be logged and was not applied\r\n");
  // The multi-key form takes the batched path and gives the same answer,
  // which it can now do honestly: nothing was applied, not "some of it was".
  CHECK(run(s, {"DEL", "k1", "k2", "k3"}) ==
        "-ERR the delete could not be logged and was not applied\r\n");
  // Reads are unaffected: nothing was applied, so the key is simply absent.
  CHECK(run(s, {"GET", "k"}) == "$-1\r\n");
  CHECK(run(s, {"EXISTS", "k"}) == ":0\r\n");
}

TEST_CASE("SET with an expiry") {
  Store s;

  SUBCASE("EX and PX both write, in their own units") {
    CHECK(run(s, {"SET", "a", "v", "EX", "100"}) == "+OK\r\n");
    CHECK(run(s, {"SET", "b", "v", "PX", "100000"}) == "+OK\r\n");
    CHECK(run(s, {"GET", "a"}) == "$1\r\nv\r\n");
    CHECK(run(s, {"GET", "b"}) == "$1\r\nv\r\n");
    // Same lifetime expressed two ways, so the two TTLs must agree.
    CHECK(run(s, {"TTL", "a"}) == run(s, {"TTL", "b"}));
  }

  SUBCASE("the option name is case-insensitive, as commands are") {
    CHECK(run(s, {"SET", "a", "v", "ex", "100"}) == "+OK\r\n");
    CHECK(run(s, {"SET", "b", "v", "Px", "100000"}) == "+OK\r\n");
  }

  SUBCASE("a non-integer expiry is an integer error, not a syntax one") {
    CHECK(run(s, {"SET", "k", "v", "EX", "abc"}) ==
          "-ERR value is not an integer or out of range\r\n");
    // Strict: a number with trailing rubbish is not a number. strtoll would
    // have taken the 10 and ignored the rest.
    CHECK(run(s, {"SET", "k", "v", "EX", "10abc"}) ==
          "-ERR value is not an integer or out of range\r\n");
    CHECK(run(s, {"SET", "k", "v", "EX", ""}) ==
          "-ERR value is not an integer or out of range\r\n");
    CHECK(run(s, {"GET", "k"}) == "$-1\r\n");  // nothing was written
  }

  SUBCASE("a non-positive expiry is refused on SET") {
    CHECK(run(s, {"SET", "k", "v", "EX", "0"}) ==
          "-ERR invalid expire time in 'set' command\r\n");
    CHECK(run(s, {"SET", "k", "v", "EX", "-1"}) ==
          "-ERR invalid expire time in 'set' command\r\n");
  }

  SUBCASE("an expiry that would overflow is refused, not wrapped") {
    // Without the guard this lands in the past and deletes the key -- the
    // exact opposite of what was asked, reported as +OK.
    CHECK(run(s, {"SET", "k", "v", "EX", "9223372036854775807"}) ==
          "-ERR invalid expire time in 'set' command\r\n");
    CHECK(run(s, {"GET", "k"}) == "$-1\r\n");
  }

  SUBCASE("a plain overwrite drops the TTL") {
    CHECK(run(s, {"SET", "k", "v", "EX", "100"}) == "+OK\r\n");
    CHECK(run(s, {"SET", "k", "v2"}) == "+OK\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":-1\r\n");
  }
}

TEST_CASE("EXPIRE and TTL") {
  Store s;

  SUBCASE("TTL reports the three cases Redis reports") {
    CHECK(run(s, {"TTL", "missing"}) == ":-2\r\n");
    CHECK(run(s, {"SET", "forever", "v"}) == "+OK\r\n");
    CHECK(run(s, {"TTL", "forever"}) == ":-1\r\n");
    CHECK(run(s, {"SET", "timed", "v", "EX", "100"}) == "+OK\r\n");
    CHECK(run(s, {"TTL", "timed"}) == ":100\r\n");
  }

  SUBCASE("EXPIRE attaches a TTL to a key that already exists") {
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"EXPIRE", "k", "50"}) == ":1\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":50\r\n");
    // The value is untouched by the read-modify-write.
    CHECK(run(s, {"GET", "k"}) == "$1\r\nv\r\n");
  }

  SUBCASE("EXPIRE on a key that is not there changes nothing") {
    CHECK(run(s, {"EXPIRE", "missing", "50"}) == ":0\r\n");
    CHECK(run(s, {"GET", "missing"}) == "$-1\r\n");
    CHECK(run(s, {"TTL", "missing"}) == ":-2\r\n");
  }

  SUBCASE("EXPIRE can replace an existing TTL") {
    CHECK(run(s, {"SET", "k", "v", "EX", "100"}) == "+OK\r\n");
    CHECK(run(s, {"EXPIRE", "k", "5"}) == ":1\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":5\r\n");
  }

  SUBCASE("EXPIRE with a non-positive time removes the key now") {
    // Redis's asymmetry: non-positive is an error on SET and means "gone" on
    // EXPIRE. Matched rather than explained away.
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"EXPIRE", "k", "0"}) == ":1\r\n");
    CHECK(run(s, {"GET", "k"}) == "$-1\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":-2\r\n");
    CHECK(run(s, {"EXISTS", "k"}) == ":0\r\n");
  }

  SUBCASE("EXPIRE rejects a non-integer and an overflowing time") {
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"EXPIRE", "k", "soon"}) ==
          "-ERR value is not an integer or out of range\r\n");
    CHECK(run(s, {"EXPIRE", "k", "9223372036854775807"}) ==
          "-ERR invalid expire time in 'expire' command\r\n");
    // Neither attempt touched the key.
    CHECK(run(s, {"TTL", "k"}) == ":-1\r\n");
  }

  SUBCASE("an expired key reads as absent everywhere") {
    CHECK(run(s, {"SET", "k", "v"}) == "+OK\r\n");
    CHECK(run(s, {"EXPIRE", "k", "-1"}) == ":1\r\n");
    CHECK(run(s, {"GET", "k"}) == "$-1\r\n");
    CHECK(run(s, {"EXISTS", "k"}) == ":0\r\n");
    CHECK(run(s, {"TTL", "k"}) == ":-2\r\n");
    // And DEL agrees it was already gone, rather than claiming a removal.
    CHECK(run(s, {"DEL", "k"}) == ":0\r\n");
  }

  SUBCASE("arity is checked") {
    CHECK(run(s, {"EXPIRE", "k"}) ==
          "-ERR wrong number of arguments for 'expire' command\r\n");
    CHECK(run(s, {"TTL"}) ==
          "-ERR wrong number of arguments for 'ttl' command\r\n");
    CHECK(run(s, {"TTL", "a", "b"}) ==
          "-ERR wrong number of arguments for 'ttl' command\r\n");
  }
}
