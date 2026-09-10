#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest.h>

#include "resp.h"

#include <string>
#include <string_view>
#include <vector>

using cachedb::Command;
using cachedb::RespParser;
using Status = cachedb::RespParser::Status;

using Args = std::vector<std::string>;
using Commands = std::vector<Args>;

namespace {

struct Outcome {
  Commands commands;
  Status last = Status::NeedMoreData;
  std::string error;
};

// Drain every command the parser can produce from a single whole-buffer feed.
Outcome feed_whole(std::string_view in) {
  Outcome o;
  RespParser p;
  Command c;
  size_t off = 0;
  for (;;) {
    auto r = p.parse(in.substr(off), c);
    off += r.consumed;
    o.last = r.status;
    o.error = std::string(r.error);
    if (r.status != Status::Complete) return o;
    o.commands.push_back(c.args);
    c.clear();
  }
}

// The same input delivered one byte at a time, which is what actually
// exercises resumability: every suspend/resume boundary in the state machine
// gets hit at least once.
Outcome feed_bytewise(std::string_view in) {
  Outcome o;
  RespParser p;
  Command c;
  std::string win;
  for (char ch : in) {
    win.push_back(ch);
    for (;;) {
      auto r = p.parse(win, c);
      win.erase(0, r.consumed);
      o.last = r.status;
      o.error = std::string(r.error);
      if (r.status != Status::Complete) break;
      o.commands.push_back(c.args);
      c.clear();
    }
    if (o.last == Status::ProtocolError) break;
  }
  return o;
}

// PROJECT.md 9: "feed it byte-by-byte and assert it produces the same result
// as feeding it whole". Every case below goes through here, so that property
// is checked on every input rather than in one dedicated test.
Outcome parse(std::string_view in) {
  Outcome whole = feed_whole(in);
  Outcome bytewise = feed_bytewise(in);
  REQUIRE(whole.commands == bytewise.commands);
  REQUIRE(whole.last == bytewise.last);
  return whole;
}

}  // namespace

TEST_CASE("multibulk requests") {
  SUBCASE("a complete SET") {
    auto o = parse("*3\r\n$3\r\nSET\r\n$5\r\nmykey\r\n$7\r\nmyvalue\r\n");
    CHECK(o.commands == Commands{{"SET", "mykey", "myvalue"}});
    CHECK(o.last == Status::NeedMoreData);
  }

  SUBCASE("two commands in one buffer are returned one at a time") {
    auto o = parse("*1\r\n$4\r\nPING\r\n*1\r\n$4\r\nPING\r\n");
    CHECK(o.commands == Commands{{"PING"}, {"PING"}});
  }

  SUBCASE("an empty bulk string is a valid value") {
    auto o = parse("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$0\r\n\r\n");
    CHECK(o.commands == Commands{{"SET", "k", ""}});
  }

  SUBCASE("payloads are binary safe") {
    // Embedded CR, LF and NUL must survive: the parser is driven by the
    // declared length, never by scanning for a delimiter.
    const std::string payload("a\r\n\0b", 5);
    auto o = parse("*2\r\n$3\r\nGET\r\n$" + std::to_string(payload.size()) +
                   "\r\n" + payload + "\r\n");
    CHECK(o.commands == Commands{{"GET", payload}});
  }

  SUBCASE("an incomplete command yields nothing") {
    auto o = parse("*2\r\n$3\r\nGET\r\n$3\r\nfo");
    CHECK(o.commands.empty());
    CHECK(o.last == Status::NeedMoreData);
  }

  SUBCASE("*0 carries no command and is skipped") {
    // Must not surface as an empty Command, or the dispatcher has to defend
    // against args[0] on an empty vector.
    auto o = parse("*0\r\n*1\r\n$4\r\nPING\r\n");
    CHECK(o.commands == Commands{{"PING"}});
  }

  SUBCASE("a value larger than the reserve cap") {
    const std::string val(70000, 'z');
    auto o = parse("*3\r\n$3\r\nSET\r\n$1\r\nk\r\n$" + std::to_string(val.size()) +
                   "\r\n" + val + "\r\n");
    CHECK(o.commands == Commands{{"SET", "k", val}});
  }
}

TEST_CASE("inline commands") {
  SUBCASE("bare LF, as netcat sends it") {
    auto o = parse("PING\n");
    CHECK(o.commands == Commands{{"PING"}});
  }

  SUBCASE("CRLF is tolerated too") {
    auto o = parse("SET foo bar\r\n");
    CHECK(o.commands == Commands{{"SET", "foo", "bar"}});
  }

  SUBCASE("runs of spaces and tabs collapse") {
    auto o = parse("  SET   foo\tbar  \n");
    CHECK(o.commands == Commands{{"SET", "foo", "bar"}});
  }

  SUBCASE("blank lines are skipped") {
    auto o = parse("\n\nPING\n");
    CHECK(o.commands == Commands{{"PING"}});
  }

  SUBCASE("quotes are not honoured") {
    // Documented limitation: inline mode splits on whitespace only. An
    // argument containing a space has to come from redis-cli.
    auto o = parse("SET k \"a b\"\n");
    CHECK(o.commands == Commands{{"SET", "k", "\"a", "b\""}});
  }
}

TEST_CASE("protocol errors") {
  SUBCASE("reply-only prefixes are rejected in a request") {
    CHECK(parse("*2\r\n+OK\r\n").last == Status::ProtocolError);
  }

  SUBCASE("a null bulk string is reply-only") {
    CHECK(parse("*2\r\n$-1\r\n").last == Status::ProtocolError);
  }

  SUBCASE("non-numeric length") {
    CHECK(parse("*x\r\n").last == Status::ProtocolError);
  }

  SUBCASE("trailing junk after a length is not ignored") {
    // Guards the `ptr == end` half of parse_i64: without it this parses as 3.
    CHECK(parse("*3 \r\n").last == Status::ProtocolError);
  }

  SUBCASE("a length that overflows int64 is not wrapped") {
    CHECK(parse("*99999999999999999999\r\n").last == Status::ProtocolError);
  }

  SUBCASE("an array longer than the cap") {
    CHECK(parse("*2000000\r\n").last == Status::ProtocolError);
  }

  SUBCASE("a payload whose declared length disagrees with the stream") {
    CHECK(parse("*1\r\n$1\r\nabc\r\n").last == Status::ProtocolError);
  }

  SUBCASE("commands parsed before the error still count") {
    auto o = parse("*1\r\n$4\r\nPING\r\n*x\r\n");
    CHECK(o.commands == Commands{{"PING"}});
    CHECK(o.last == Status::ProtocolError);
  }

  SUBCASE("the error is sticky once framing is lost") {
    RespParser p;
    Command c;
    CHECK(p.parse("*x\r\n", c).status == Status::ProtocolError);
    CHECK(p.parse("*1\r\n$4\r\nPING\r\n", c).status == Status::ProtocolError);
    p.reset();  // must not clear a Failed parser
    CHECK(p.parse("*1\r\n$4\r\nPING\r\n", c).status == Status::ProtocolError);
  }
}

TEST_CASE("unterminated lines cannot buffer without bound") {
  // A client that opens a line and never closes it must be cut off rather
  // than allowed to grow the connection buffer indefinitely.
  SUBCASE("multibulk header") {
    std::string in = "*";
    in.append(RespParser::kMaxLineLen + 8, '1');
    CHECK(feed_whole(in).last == Status::ProtocolError);
  }

  SUBCASE("inline command") {
    std::string in(RespParser::kMaxLineLen + 8, 'x');
    CHECK(feed_whole(in).last == Status::ProtocolError);
  }
}

TEST_CASE("consumed reports exactly one command") {
  // The connection relies on this to know how much to discard, and stopping
  // at one command lets it execute before parsing the rest.
  const std::string first = "*1\r\n$4\r\nPING\r\n";
  RespParser p;
  Command c;
  auto r = p.parse(first + "*1\r\n$4\r\nECHO\r\n", c);
  CHECK(r.status == Status::Complete);
  CHECK(r.consumed == first.size());
  CHECK(c.args == Args{"PING"});
}
