#include "command.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <map>
#include <string_view>

namespace cachedb {
namespace {

using Handler = void (*)(const Command&, Store&, std::string&);

struct Spec {
  Handler handler;
  // Argument counts include the command name itself, so GET's two are "get"
  // and the key. A max of -1 means variadic.
  int min_args;
  int max_args;
};

// Longest name dispatched here is "command" (7). Normalising into a buffer
// this size keeps lookup allocation-free; a longer name cannot match anything
// in the table, so it goes straight to "unknown command".
constexpr size_t kMaxNameLen = 16;

bool normalize_name(std::string_view name, char (&buf)[kMaxNameLen],
                    std::string_view& out) {
  if (name.size() > kMaxNameLen) return false;
  for (size_t i = 0; i < name.size(); ++i) {
    const unsigned char c = static_cast<unsigned char>(name[i]);
    buf[i] = static_cast<char>(std::tolower(c));
  }
  out = std::string_view(buf, name.size());
  return true;
}

// The name is client-supplied and is about to be embedded in an error reply,
// which is terminated by the next CRLF. Echoed raw, a command named
// "X\r\n+OK" would forge a second reply out of nothing but data. Unprintable
// bytes become '.' and the echo is truncated: an error message is a
// diagnostic, not a faithful reproduction of what arrived.
void append_printable(std::string& out, std::string_view s) {
  constexpr size_t kMaxEcho = 64;
  const size_t n = std::min(s.size(), kMaxEcho);
  for (size_t i = 0; i < n; ++i) {
    const unsigned char c = static_cast<unsigned char>(s[i]);
    out += std::isprint(c) ? static_cast<char>(c) : '.';
  }
}

void cmd_ping(const Command& c, Store&, std::string& out) {
  // Bare PING answers with a simple string; PING <msg> echoes the message as
  // a bulk string, because the message is client bytes and only bulk framing
  // is safe for those.
  if (c.args.size() == 1) {
    append_simple_string(out, "PONG");
  } else {
    append_bulk_string(out, c.args[1]);
  }
}

void cmd_set(const Command& c, Store& store, std::string& out) {
  if (c.args.size() > 3) {
    // SET's options -- EX, PX, NX, XX -- arrive with expiry in M4. Rejecting
    // them beats accepting and ignoring them: a client that asks for a TTL
    // and silently does not get one has a bug it cannot see.
    append_error(out, "ERR syntax error");
    return;
  }
  if (!store.set(c.args[1], c.args[2])) {
    // The log write failed, so the store was not touched. Saying so is the
    // point of the whole exercise: an acknowledged write that did not happen
    // is the one failure a database may not have.
    append_error(out, "ERR the write could not be logged and was not applied");
    return;
  }
  append_simple_string(out, "OK");
}

void cmd_get(const Command& c, Store& store, std::string& out) {
  const auto value = store.get(c.args[1]);
  if (value) {
    append_bulk_string(out, *value);
  } else {
    append_null_bulk(out);
  }
}

void cmd_del(const Command& c, Store& store, std::string& out) {
  int64_t removed = 0;
  for (size_t i = 1; i < c.args.size(); ++i) {
    const DelResult r = store.del(c.args[i]);
    if (!r.durable) {
      // Multi-key DEL is not atomic across a log failure. Keys earlier in the
      // argument list are already deleted and already logged; this one is not,
      // and the remaining ones were never attempted. Replying with the count
      // so far would read as "these succeeded and the rest did not exist",
      // which is a different and false statement -- so the command reports an
      // error and the client re-reads to find out where it stopped.
      //
      // Making it atomic needs the log to accept a batch that replays all or
      // nothing, which is a real feature and not one M2 promises.
      append_error(out,
                   "ERR the delete could not be logged; this command was "
                   "applied only in part");
      return;
    }
    if (r.was_live) ++removed;
  }
  append_integer(out, removed);
}

void cmd_exists(const Command& c, Store& store, std::string& out) {
  // Variadic and counting, as real Redis is: EXISTS k k replies 2 when k is
  // present. PROJECT.md 7 describes the single-key case, which this covers.
  int64_t found = 0;
  for (size_t i = 1; i < c.args.size(); ++i) {
    if (store.exists(c.args[i])) ++found;
  }
  append_integer(out, found);
}

void cmd_command(const Command&, Store&, std::string& out) {
  // redis-cli sends COMMAND DOCS on connect and waits for an answer before
  // showing a prompt. An empty array is a truthful "no command table to
  // describe" and is enough to get the client running.
  append_array_header(out, 0);
}

const std::map<std::string, Spec, std::less<>>& dispatch_table() {
  // Function-local static: built once on first use, with no dependence on
  // initialisation order between translation units. std::less<> keeps lookup
  // transparent so the normalised name is used as-is, with no allocation.
  static const std::map<std::string, Spec, std::less<>> table = {
      {"ping", {cmd_ping, 1, 2}},
      // SET is declared variadic and rejects extra arguments itself, so an
      // unsupported option reports "syntax error" rather than bad arity,
      // matching what a real client expects to see.
      {"set", {cmd_set, 3, -1}},
      {"get", {cmd_get, 2, 2}},
      {"del", {cmd_del, 2, -1}},
      {"exists", {cmd_exists, 2, -1}},
      {"command", {cmd_command, 1, -1}},
  };
  return table;
}

}  // namespace

void execute(const Command& cmd, Store& store, std::string& out) {
  // The parser never emits an empty command, so there is nothing to answer.
  if (cmd.args.empty()) return;

  char buf[kMaxNameLen];
  std::string_view name;
  const auto& table = dispatch_table();
  auto it = table.end();
  if (normalize_name(cmd.args[0], buf, name)) it = table.find(name);

  if (it == table.end()) {
    std::string msg = "ERR unknown command '";
    append_printable(msg, cmd.args[0]);
    msg += '\'';
    append_error(out, msg);
    return;
  }

  const Spec& spec = it->second;
  const int argc = static_cast<int>(cmd.args.size());
  if (argc < spec.min_args || (spec.max_args >= 0 && argc > spec.max_args)) {
    // The normalised name is safe to embed: it matched a table entry, so it
    // is one of our own literals.
    std::string msg = "ERR wrong number of arguments for '";
    msg += name;
    msg += "' command";
    append_error(out, msg);
    return;
  }

  spec.handler(cmd, store, out);
}

}  // namespace cachedb
