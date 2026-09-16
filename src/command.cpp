#include "command.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <functional>
#include <map>
#include <string_view>
#include <vector>

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

// Redis's two integer errors, spelled the way a real server spells them so a
// client library's error matching keeps working.
constexpr const char* kNotAnInteger =
    "ERR value is not an integer or out of range";

// Strict on purpose, and strict in the same way Redis is.
//
// strtoll would accept "10abc", leading whitespace and a locale's idea of
// digits; a client that sent any of those has a bug it should be told about
// rather than a silently different number. No partial parse, and the overflow
// check happens before the multiply rather than after it -- signed overflow is
// undefined behaviour, so detecting it afterwards is detecting nothing.
//
// It also demands the *canonical* spelling: no leading zeros, no leading '+',
// and no "-0". That is not fussiness copied for its own sake. Redis's
// string2ll requires a number to round-trip to exactly the bytes it came from,
// because an integer it stores has to read back identical; "007" and "7" would
// be the same number and different values. Checked against a real server,
// which rejects 007 for INCR, for SET's EX and for EXPIRE alike -- so this
// lives in the parser rather than in one command.
bool parse_int64(std::string_view text, int64_t* out) {
  if (text.empty() || text.size() > 20) return false;
  size_t i = 0;
  bool negative = false;
  if (text[0] == '-') {
    negative = true;
    i = 1;
    if (text.size() == 1) return false;
  }
  // "0" is the one number that may start with a zero, and only alone.
  if (text[i] == '0' && (text.size() - i > 1 || negative)) return false;

  int64_t value = 0;
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c < '0' || c > '9') return false;
    const int digit = c - '0';
    if (value > (kMax - digit) / 10) return false;
    value = value * 10 + digit;
  }
  *out = negative ? -value : value;
  return true;
}

// Turns a lifetime into a deadline, refusing anything that would wrap.
//
// Without the check, EXPIRE k 9999999999999 overflows into a stamp in the
// past and deletes the key instead of keeping it for three hundred years --
// the exact opposite of what was asked, reported as success.
bool deadline_from(int64_t amount, int64_t unit_ms, int64_t* out) {
  constexpr int64_t kMax = std::numeric_limits<int64_t>::max();
  if (amount > kMax / unit_ms) return false;
  const int64_t delta = amount * unit_ms;
  const int64_t now = now_ms();
  if (delta > kMax - now) return false;
  *out = now + delta;
  // 0 is the sentinel for "no expiry", so a deadline that lands exactly on
  // the epoch has to become something else. One millisecond earlier is
  // already fifty years past and expires just the same.
  if (*out == 0) *out = -1;
  return true;
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
  // 0 means no expiry, which is also what a plain SET writes: overwriting a
  // key without EX or PX drops whatever TTL it had, as real Redis does.
  int64_t expires_at_ms = 0;

  if (c.args.size() == 5) {
    char unit[kMaxNameLen];
    std::string_view option;
    if (!normalize_name(c.args[3], unit, option) ||
        (option != "ex" && option != "px")) {
      // NX, XX and KEEPTTL land here. Still refused rather than ignored: a
      // client that asks for a conditional write and silently gets an
      // unconditional one has a bug it cannot see from the outside.
      append_error(out, "ERR syntax error");
      return;
    }

    int64_t amount = 0;
    if (!parse_int64(c.args[4], &amount)) {
      append_error(out, kNotAnInteger);
      return;
    }
    // Redis refuses a non-positive expiry on SET rather than treating it as
    // "delete immediately", which is what it does for EXPIRE. The asymmetry
    // is theirs; matching it is cheaper than explaining a difference.
    if (amount <= 0) {
      append_error(out, "ERR invalid expire time in 'set' command");
      return;
    }
    if (!deadline_from(amount, option == "ex" ? 1000 : 1, &expires_at_ms)) {
      append_error(out, "ERR invalid expire time in 'set' command");
      return;
    }
  } else if (c.args.size() != 3) {
    append_error(out, "ERR syntax error");
    return;
  }

  if (!store.set(c.args[1], c.args[2], expires_at_ms)) {
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
    append_bulk_string(out, value->get());
  } else {
    append_null_bulk(out);
  }
}

void cmd_del(const Command& c, Store& store, std::string& out) {
  constexpr const char* kFailed =
      "ERR the delete could not be logged and was not applied";

  // One key is the common case and needs no vector: it is a single record
  // either way, so it takes the direct path and stays allocation-free.
  if (c.args.size() == 2) {
    const DelResult r = store.del(c.args[1]);
    if (!r.durable) {
      append_error(out, kFailed);
      return;
    }
    append_integer(out, r.was_live ? 1 : 0);
    return;
  }

  // Several keys go down as one log write, so a failure leaves the command
  // wholly unapplied rather than half done. Before this, a failure partway
  // along deleted the keys before it and reported an error that could not say
  // which -- and the client had no way to find out but to re-read every key.
  const std::vector<std::string_view> keys(c.args.begin() + 1, c.args.end());
  const DelBatchResult r = store.del_many(keys);
  if (!r.durable) {
    append_error(out, kFailed);
    return;
  }
  append_integer(out, r.removed);
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

void cmd_expire(const Command& c, Store& store, std::string& out) {
  int64_t seconds = 0;
  if (!parse_int64(c.args[2], &seconds)) {
    append_error(out, kNotAnInteger);
    return;
  }

  int64_t deadline = 0;
  if (seconds <= 0) {
    // Redis treats a non-positive expiry on EXPIRE as "gone now", unlike SET
    // where it is an error. A stamp in the past does that here without a
    // separate delete path: the key stops being visible immediately and the
    // sweep reclaims it like any other expired entry.
    deadline = -1;
  } else if (!deadline_from(seconds, 1000, &deadline)) {
    append_error(out, "ERR invalid expire time in 'expire' command");
    return;
  }

  const ExpireResult r = store.expire(c.args[1], deadline);
  if (!r.durable) {
    append_error(out, "ERR the expiry could not be logged and was not applied");
    return;
  }
  append_integer(out, r.applied ? 1 : 0);
}

void cmd_ttl(const Command& c, Store& store, std::string& out) {
  const TtlResult r = store.ttl(c.args[1]);
  if (!r.exists) {
    append_integer(out, -2);  // no such key
    return;
  }
  if (!r.has_expiry) {
    append_integer(out, -1);  // lives here for ever
    return;
  }
  // Rounded up, as Redis does: a key with 1 ms left has 1 second of TTL and
  // not 0, because 0 would read as "expiring now" to anything watching.
  append_integer(out, (r.remaining_ms + 999) / 1000);
}

void cmd_incr(const Command& c, Store& store, std::string& out) {
  // A missing key counts as 0, so INCR on nothing replies :1. Redis's rule,
  // and the reason this is one command rather than a GET and a SET.
  int64_t value = 0;
  int64_t expires_at_ms = 0;

  if (const std::optional<Found> found = store.lookup(c.args[1])) {
    // Parsed out before anything is written. The view borrows the memtable's
    // own string on a hit, and set() assigns over that same string -- holding
    // the view across the write is the trap EXPIRE has to copy around. Here
    // the bytes become an integer first, so there is nothing left to dangle.
    if (!parse_int64(found->value.get(), &value)) {
      append_error(out, kNotAnInteger);
      return;
    }
    expires_at_ms = found->expires_at_ms;
  }

  if (value == std::numeric_limits<int64_t>::max()) {
    append_error(out, "ERR increment or decrement would overflow");
    return;
  }
  ++value;

  // The expiry is carried across deliberately. A plain SET drops a TTL, and
  // INCR is a SET underneath, so without this an incremented counter would
  // quietly lose its lifetime -- and a key that was supposed to disappear
  // would live for ever because something counted it.
  if (!store.set(c.args[1], std::to_string(value), expires_at_ms)) {
    append_error(out, "ERR the write could not be logged and was not applied");
    return;
  }
  append_integer(out, value);
}

void cmd_flushall(const Command&, Store& store, std::string& out) {
  if (!store.flush_all()) {
    append_error(out, "ERR the flush could not be completed");
    return;
  }
  append_simple_string(out, "OK");
}

// Captured at static-init time, so it really is when the process started
// rather than when something first asked. Calling a function during static
// initialisation is safe; it is cross-TU *objects* that have no defined order.
const int64_t kStartedMs = now_ms();

const char* policy_name(SyncPolicy policy) {
  switch (policy) {
    case SyncPolicy::kAlways: return "always";
    case SyncPolicy::kEverySec: return "everysec";
    case SyncPolicy::kNo: return "no";
  }
  return "unknown";
}

void cmd_info(const Command&, Store& store, std::string& out) {
  // Redis's shape: "# Section" lines and "field:value" lines, CRLF separated,
  // the whole thing returned as one bulk string. Sections are what a client
  // splits on, so the names matter more than the order.
  std::string info;
  const auto line = [&info](const char* key, unsigned long long value) {
    info += key;
    info += ':';
    info += std::to_string(value);
    info += "\r\n";
  };

  info += "# Server\r\n";
  line("uptime_in_seconds",
       static_cast<unsigned long long>((now_ms() - kStartedMs) / 1000));

  info += "\r\n# Memtable\r\n";
  // Keys in the memtable only. A true total would mean merging every level,
  // which is what a compaction does and not what a status line should.
  line("memtable_keys", store.memtable_keys());
  line("memtable_bytes", store.memtable_bytes());
  line("memtable_limit_bytes", store.memtable_limit_bytes());

  info += "\r\n# Persistence\r\n";
  line("sstable_count", store.sstable_count());
  line("wal_bytes", store.wal_bytes());
  // The one number that makes the fsync policies distinguishable from
  // outside the process. It took strace to see this difference the first
  // time; a skipped-sync optimisation is exactly what silently stops working.
  line("wal_fsyncs", store.wal_syncs());
  info += "wal_fsync_policy:";
  info += store.has_wal() ? policy_name(store.wal_policy()) : "none";
  info += "\r\n";
  // Writes are still durable when this is 1 -- they are in the log. What has
  // stopped is the memtable being able to shed them.
  line("flush_failed", store.flush_failed() ? 1 : 0);

  info += "\r\n# Expiry\r\n";
  // The active sweep is invisible otherwise: its whole job is making keys
  // disappear that were already unreachable.
  line("expired_keys_swept", store.swept_keys());

  append_bulk_string(out, info);
}

void cmd_config(const Command& c, Store& store, std::string& out) {
  char buf[kMaxNameLen];
  std::string_view sub;
  if (normalize_name(c.args[1], buf, sub) && sub == "get") {
    // redis-benchmark asks for `save` and `appendonly` before it starts and
    // prints "WARNING: Could not fetch server CONFIG" unless it gets a
    // two-element array back. An empty array was the first attempt and did
    // not satisfy it -- the warning is about the shape of the reply, not
    // about whether any parameter is configurable.
    //
    // Nothing here is invented to please the client. Each answer is a fact
    // about this server stated in Redis's vocabulary: cachedb never
    // snapshots, every write really does go to an append-only log, and that
    // log's fsync policy is the one --fsync selected.
    if (c.args.size() != 3) {
      append_array_header(out, 0);
      return;
    }
    char pbuf[kMaxNameLen];
    std::string_view param;
    std::string_view value;
    bool known = false;
    if (normalize_name(c.args[2], pbuf, param)) {
      if (param == "save") {
        value = "";  // no RDB-style snapshots, and none planned
        known = true;
      } else if (param == "appendonly") {
        value = "yes";  // the WAL, which is not optional
        known = true;
      } else if (param == "appendfsync") {
        value = store.has_wal() ? policy_name(store.wal_policy()) : "no";
        known = true;
      } else if (param == "maxmemory") {
        value = "0";  // unbounded; the memtable limit is not a memory cap
        known = true;
      }
    }
    if (!known) {
      append_array_header(out, 0);
      return;
    }
    append_array_header(out, 2);
    append_bulk_string(out, c.args[2]);
    append_bulk_string(out, value);
    return;
  }
  std::string msg = "ERR Unknown CONFIG subcommand or wrong number of ";
  msg += "arguments for '";
  append_printable(msg, c.args[1]);
  msg += '\'';
  append_error(out, msg);
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
      {"expire", {cmd_expire, 3, 3}},
      {"ttl", {cmd_ttl, 2, 2}},
      {"incr", {cmd_incr, 2, 2}},
      {"flushall", {cmd_flushall, 1, -1}},
      {"info", {cmd_info, 1, 2}},
      {"config", {cmd_config, 2, -1}},
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
