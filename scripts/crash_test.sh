#!/usr/bin/env bash
#
# PROJECT.md 8, M2 acceptance: write keys, kill -9 the server mid-write,
# restart, and verify that every key the client was told was written is still
# there. Ten consecutive runs.
#
# The invariant under test is one-directional and that matters. We assert
#
#     acknowledged  =>  present after recovery
#
# and NOT the reverse. A key can legitimately be durable without having been
# acknowledged: the server can fsync the record and die before the reply
# reaches the client. Losing a reply is a disappointment; losing an
# acknowledged write is a broken database.
#
# What this proves, and what it does not: kill -9 kills the process, not the
# kernel, and the page cache belongs to the kernel -- so bytes that reached
# write() survive under every fsync policy, "no" included. This test therefore
# validates the ORDERING (log before acknowledgement), which is the thing most
# likely to be got wrong. Only power loss separates the policies, and nothing
# here simulates that honestly. See PROJECT.md 6.4.
#
# Phase 2 exists because of something phase 1 cannot do. A record here is
# about 35 bytes, and write() copies a buffer that small into the page cache in
# one step that a signal cannot interrupt -- so kill -9 essentially never
# leaves a half-written record, and in practice phase 1 never exercises the
# truncation path at all. Only a power cut tears a record. Phase 2 models that
# by cutting a few bytes off the log after the kill, which is the state a power
# cut would leave, and then checks recovery keeps everything before the damage.
#
#   ./scripts/crash_test.sh                 10 runs, 10k keys, fsync=always
#   RUNS=3 KEYS=2000 ./scripts/crash_test.sh
#   FSYNC=everysec ./scripts/crash_test.sh  expected to still pass: see above
#
# Linux only, and it needs a built server -- run it inside the container.

set -uo pipefail

RUNS=${RUNS:-10}
KEYS=${KEYS:-10000}
PORT=${PORT:-7379}
FSYNC=${FSYNC:-always}
TEAR_RUNS=${TEAR_RUNS:-3}
BIN=${BIN:-./build-linux/cachedb}

if [ ! -x "$BIN" ]; then
  echo "crash_test: no server at $BIN -- build it first, or set BIN=" >&2
  exit 1
fi
if ! command -v redis-cli > /dev/null; then
  echo "crash_test: redis-cli not found" >&2
  exit 1
fi

SERVER_PID=""
WORK=""
cleanup() {
  [ -n "$SERVER_PID" ] && kill -9 "$SERVER_PID" 2> /dev/null
  [ -n "$WORK" ] && rm -rf "$WORK"
  return 0
}
trap cleanup EXIT

# Waits for the listener rather than sleeping a guessed interval, so a slow
# recovery does not read as a failure.
wait_ready() {
  for _ in $(seq 1 100); do
    [ "$(redis-cli -p "$PORT" ping 2> /dev/null)" = "PONG" ] && return 0
    sleep 0.1
  done
  return 1
}

start_server() {  # $1 = data dir, $2 = log file
  "$BIN" --dir "$1" --fsync "$FSYNC" --port "$PORT" > "$2" 2>&1 &
  SERVER_PID=$!
  wait_ready
}

# Sets `acked` and `dir`; returns 1 if the run is unusable as a test.
acked=0
dir=""
run_to_crash() {
  dir="$WORK/data"
  if ! start_server "$dir" "$WORK/first.log"; then
    echo "  server did not start"; cat "$WORK/first.log"; return 1
  fi

  # One connection, one command per line, one reply line each -- so line N of
  # acks.txt is the reply to key N and the acknowledged set is a prefix.
  seq 1 "$KEYS" | awk '{print "set key"$1" val"$1}' \
    | redis-cli -p "$PORT" > "$WORK/acks.txt" 2>&1 &
  local writer=$!

  # Land the kill inside the write stream rather than before or after it.
  sleep "$(awk -v seed="$RANDOM" 'BEGIN { srand(seed); printf "%.2f", 0.3 + rand() * 2.2 }')"
  kill -9 "$SERVER_PID" 2> /dev/null
  wait "$writer" 2> /dev/null
  wait "$SERVER_PID" 2> /dev/null
  SERVER_PID=""

  acked=$(grep -c '^OK$' "$WORK/acks.txt")

  # The acknowledged keys must be a contiguous prefix. If they are not, the
  # count is checking the wrong keys and every later assertion is meaningless.
  if [ "$acked" -gt 0 ] && [ "$(head -n "$acked" "$WORK/acks.txt" | grep -cv '^OK$')" -ne 0 ]; then
    echo "  acknowledgements are not a contiguous prefix"; return 1
  fi
  # A kill that landed before any write proves nothing; do not let it pass.
  if [ "$acked" -eq 0 ]; then
    echo "  killed before any write was acknowledged"; return 1
  fi
  return 0
}

# Compares keys 1..$1 against the values they were written with.
check_keys() {  # $1 = how many
  seq 1 "$1" | awk '{print "get key"$1}' | redis-cli -p "$PORT" > "$WORK/got.txt" 2>&1
  seq 1 "$1" | awk '{print "val"$1}' > "$WORK/want.txt"
  diff -q "$WORK/want.txt" "$WORK/got.txt" > /dev/null 2>&1
}

torn=0   # runs in which recovery actually had to discard a partial record
fails=0

echo "phase 1: kill -9 mid-write, every acknowledged key must survive"
for run in $(seq 1 "$RUNS"); do
  WORK=$(mktemp -d)

  if ! run_to_crash; then
    echo "run $run: FAIL"; fails=$((fails + 1)); rm -rf "$WORK"; WORK=""; continue
  fi

  if ! start_server "$dir" "$WORK/second.log"; then
    echo "run $run: FAIL -- server did not restart after the kill"
    cat "$WORK/second.log"
    fails=$((fails + 1)); rm -rf "$WORK"; WORK=""; continue
  fi
  grep -q 'discarded an incomplete tail' "$WORK/second.log" && torn=$((torn + 1))

  if check_keys "$acked"; then
    printf 'run %2d: PASS  %6d acknowledged, all recovered\n' "$run" "$acked"
  else
    lost=$(diff "$WORK/want.txt" "$WORK/got.txt" | grep -c '^<')
    echo "run $run: FAIL -- $lost of $acked acknowledged keys are missing or wrong"
    diff "$WORK/want.txt" "$WORK/got.txt" | head -5
    fails=$((fails + 1))
  fi

  kill -9 "$SERVER_PID" 2> /dev/null; wait "$SERVER_PID" 2> /dev/null; SERVER_PID=""
  rm -rf "$WORK"; WORK=""
done

echo
echo "phase 2: a torn record, as a power cut would leave it"
tear_fails=0
for run in $(seq 1 "$TEAR_RUNS"); do
  WORK=$(mktemp -d)

  if ! run_to_crash; then
    echo "tear $run: FAIL"; tear_fails=$((tear_fails + 1)); rm -rf "$WORK"; WORK=""; continue
  fi

  # Cut less than one record off the end. That damages exactly the last record
  # and nothing before it, so recovery must keep every key but the last.
  before=$(stat -c%s "$dir/wal.log")
  cut=$((RANDOM % 19 + 1))
  truncate -s $((before - cut)) "$dir/wal.log"

  if ! start_server "$dir" "$WORK/second.log"; then
    echo "tear $run: FAIL -- server did not start on a torn log"
    cat "$WORK/second.log"
    tear_fails=$((tear_fails + 1)); rm -rf "$WORK"; WORK=""; continue
  fi

  if ! grep -q 'discarded an incomplete tail' "$WORK/second.log"; then
    echo "tear $run: FAIL -- cut $cut bytes mid-record and recovery said nothing"
    tear_fails=$((tear_fails + 1))
  elif check_keys $((acked - 1)); then
    printf 'tear %d: PASS  cut %2d bytes, %6d keys kept, damaged tail dropped\n' \
      "$run" "$cut" "$((acked - 1))"
  else
    lost=$(diff "$WORK/want.txt" "$WORK/got.txt" | grep -c '^<')
    echo "tear $run: FAIL -- $lost keys before the damage were lost too"
    diff "$WORK/want.txt" "$WORK/got.txt" | head -5
    tear_fails=$((tear_fails + 1))
  fi

  kill -9 "$SERVER_PID" 2> /dev/null; wait "$SERVER_PID" 2> /dev/null; SERVER_PID=""
  rm -rf "$WORK"; WORK=""
done
fails=$((fails + tear_fails))

echo
echo "fsync=$FSYNC, up to $KEYS keys per run"
echo "phase 1: $((RUNS - (fails - tear_fails)))/$RUNS passed"
echo "phase 2: $((TEAR_RUNS - tear_fails))/$TEAR_RUNS passed"
if [ "$torn" -eq 0 ]; then
  # Expected, and the reason phase 2 exists. See the header.
  echo "note: no phase 1 run tore a record on its own -- kill -9 cannot"
fi
[ "$fails" -eq 0 ] || exit 1
