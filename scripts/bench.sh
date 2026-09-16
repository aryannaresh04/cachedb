#!/usr/bin/env bash
#
# PROJECT.md 10. Runs identical workloads against cachedb and, where it is
# installed, real Redis on the same machine -- because a throughput number
# compared against someone else's hardware means nothing at all.
#
# Being slower than Redis is expected. Redis has had fifteen years of
# optimisation and this has had four weeks. The number that is worth anything
# is the explanation of *why*, which is what the per-section breakdowns below
# are for.
#
#   ./scripts/bench.sh              everything
#   SECTIONS="fsync redis" ./scripts/bench.sh
#
# Sections: fsync, redis, amplification, bloom, flush
#
# Linux only, and it needs an UNSANITIZED build -- see the guard below.

set -uo pipefail

BIN=${BIN:-./build-rel/cachedb}
PORT=${PORT:-7500}
REDIS_PORT=${REDIS_PORT:-7501}
REQUESTS=${REQUESTS:-100000}
CLIENTS=${CLIENTS:-50}
KEYSPACE=${KEYSPACE:-100000}
SECTIONS=${SECTIONS:-"fsync redis amplification bloom flush"}

if [ ! -x "$BIN" ]; then
  echo "bench: no server at $BIN" >&2
  echo "  build one with: cmake -S . -B build-rel -DCACHEDB_SANITIZE=OFF \\" >&2
  echo "                        -DCMAKE_BUILD_TYPE=Release && cmake --build build-rel -j" >&2
  exit 1
fi

# The 50x guard. The M3 acceptance run first reported 427 MB of RSS against a
# 114 MB dataset and looked like a design failure; it was the default sanitized
# build, and the same workload holds 8 MB without it. Every number below would
# be similarly worthless, so refuse rather than produce fiction.
if ldd "$BIN" 2> /dev/null | grep -qi 'asan'; then
  echo "bench: $BIN is built with sanitizers, every number here would be fiction" >&2
  echo "  rebuild with -DCACHEDB_SANITIZE=OFF (PROJECT.md 3 and 10)" >&2
  exit 1
fi

WORK=$(mktemp -d)
CACHEDB_PID=""
REDIS_PID=""
cleanup() {
  [ -n "$CACHEDB_PID" ] && kill -9 "$CACHEDB_PID" 2> /dev/null
  [ -n "$REDIS_PID" ] && kill -9 "$REDIS_PID" 2> /dev/null
  rm -rf "$WORK"
  return 0
}
trap cleanup EXIT

# Reads are served from the OS page cache unless it is emptied first, and a
# warm cache hides the disk entirely -- the first run of the read-amplification
# section below showed no difference at all between 0 tables and 15 because of
# it. Needs a privileged container; without one the numbers stay warm-cache and
# are labelled as such rather than quietly meaning something else.
CAN_DROP_CACHE=no
if [ -w /proc/sys/vm/drop_caches ]; then CAN_DROP_CACHE=yes; fi

drop_cache() {
  [ "$CAN_DROP_CACHE" = yes ] || return 0
  sync
  echo 3 > /proc/sys/vm/drop_caches 2> /dev/null
}

wait_ready() {  # $1 = port
  for _ in $(seq 1 100); do
    [ "$(redis-cli -p "$1" ping 2> /dev/null)" = "PONG" ] && return 0
    sleep 0.1
  done
  return 1
}

start_cachedb() {  # $1 = data dir, rest = extra flags
  local dir=$1; shift
  "$BIN" --dir "$dir" --port "$PORT" "$@" > "$WORK/server.log" 2>&1 &
  CACHEDB_PID=$!
  wait_ready "$PORT"
}

stop_cachedb() {
  [ -n "$CACHEDB_PID" ] && kill -TERM "$CACHEDB_PID" 2> /dev/null
  wait "$CACHEDB_PID" 2> /dev/null
  CACHEDB_PID=""
}

# redis-benchmark prints a summary block; pull throughput and the percentiles
# out of it. Reported in the same shape for both servers so the comparison is
# like for like.
# csv columns: test, rps, avg, min, p50, p95, p99, max -- quoted, no embedded
# commas. Fields are unquoted one at a time rather than with a gsub over the
# whole record: the separator *is* a quote, so stripping quotes from $0 first
# destroys the field boundaries and every column reads back empty.
parse_csv() {  # $1 = label; reads a csv line on stdin
  awk -F, -v label="$1" '
    NF >= 7 {
      for (i = 1; i <= NF; i++) gsub(/"/, "", $i)
      printf "  %-30s %9.0f ops/sec   p50 %6.3f   p95 %6.3f   p99 %6.3f ms\n",
             label, $2, $5, $6, $7
    }'
}

run_bench() {  # $1 = port, $2 = test (set|get), $3 = label
  redis-benchmark -p "$1" -t "$2" -n "$REQUESTS" -c "$CLIENTS" \
                  -r "$KEYSPACE" --csv 2> /dev/null | tail -1 | parse_csv "$3"
}

echo "cachedb benchmarks -- $(date -u +%Y-%m-%dT%H:%M:%SZ)"
echo "$REQUESTS requests, $CLIENTS clients, $KEYSPACE-key keyspace"
echo

# --------------------------------------------------------------------
if [[ " $SECTIONS " == *" fsync "* ]]; then
  echo "1. The cost of durability: throughput by fsync policy"
  echo "   Only the policy changes, so the difference is the durability tax."
  for policy in always everysec no; do
    rm -rf "$WORK/fsync"; mkdir -p "$WORK/fsync"
    start_cachedb "$WORK/fsync" --fsync "$policy" || { echo "  failed to start"; continue; }
    run_bench "$PORT" set "SET, fsync=$policy"
    stop_cachedb
  done
  echo
fi

# --------------------------------------------------------------------
if [[ " $SECTIONS " == *" redis "* ]]; then
  echo "2. Against real Redis, same machine, same workload"
  if command -v redis-server > /dev/null; then
    # Redis with appendonly on and fsync every write is the closest thing to
    # our fsync=always; comparing that against our default is the honest
    # pairing, since comparing a durable store to an in-memory one is not a
    # comparison at all.
    redis-server --port "$REDIS_PORT" --save '' --appendonly yes \
                 --appendfsync always --dir "$WORK" \
                 --daemonize no > "$WORK/redis.log" 2>&1 &
    REDIS_PID=$!
    if wait_ready "$REDIS_PORT"; then
      rm -rf "$WORK/vs"; mkdir -p "$WORK/vs"
      start_cachedb "$WORK/vs" --fsync always
      run_bench "$PORT" set "cachedb SET (fsync=always)"
      run_bench "$REDIS_PORT" set "redis   SET (appendfsync always)"
      run_bench "$PORT" get "cachedb GET"
      run_bench "$REDIS_PORT" get "redis   GET"
      stop_cachedb
    else
      echo "  redis-server did not start; see $WORK/redis.log"
    fi
    kill -9 "$REDIS_PID" 2> /dev/null; REDIS_PID=""
  else
    echo "  redis-server not installed, skipping (see the Dockerfile)"
  fi
  echo
fi

# --------------------------------------------------------------------
if [[ " $SECTIONS " == *" amplification "* ]]; then
  echo "3. Read amplification: GET latency against the number of L0 tables"
  echo "   Nothing merges them yet, so a read that the filter does not reject"
  echo "   walks every table. This is the baseline compaction has to beat."
  [ "$CAN_DROP_CACHE" = yes ] \
    && echo "   Cold cache: the page cache is dropped before each read run." \
    || echo "   WARM CACHE ONLY -- run privileged to drop it, or these hide the disk."
  # Down to a quarter-megabyte limit on purpose. At 15 tables the effect is
  # invisible; it only becomes obvious past a hundred.
  for limit in 33554432 4194304 1048576 262144; do
    rm -rf "$WORK/amp"; mkdir -p "$WORK/amp"
    start_cachedb "$WORK/amp" --fsync no --memtable-limit "$limit" || continue
    redis-benchmark -p "$PORT" -t set -n "$REQUESTS" -c "$CLIENTS" \
                    -r "$KEYSPACE" -q > /dev/null 2>&1
    tables=$(ls "$WORK/amp"/*.sst 2> /dev/null | wc -l)
    drop_cache
    run_bench "$PORT" get "$(printf '%3d tables, %5d KB limit' "$tables" "$((limit/1024))")"
    stop_cachedb
  done
  echo
fi

# --------------------------------------------------------------------
if [[ " $SECTIONS " == *" bloom "* ]]; then
  echo "4. What the bloom filter is worth: the same reads with it switched off"
  echo "   --no-bloom exists only for this. Without the filter a lookup reads a"
  echo "   block from every table that could hold the key; with it, a table that"
  echo "   definitely does not hold the key costs a few hash probes instead."
  rm -rf "$WORK/bloom"; mkdir -p "$WORK/bloom"
  start_cachedb "$WORK/bloom" --fsync no --memtable-limit 262144
  redis-benchmark -p "$PORT" -t set -n "$REQUESTS" -c "$CLIENTS" \
                  -r "$KEYSPACE" -q > /dev/null 2>&1
  tables=$(ls "$WORK/bloom"/*.sst 2> /dev/null | wc -l)
  stop_cachedb
  echo "  across $tables tables:"
  for mode in "" "--no-bloom"; do
    label="filter on "
    [ -n "$mode" ] && label="filter off"
    # shellcheck disable=SC2086
    start_cachedb "$WORK/bloom" --fsync no $mode
    drop_cache
    run_bench "$PORT" get "$label, keys that exist"
    drop_cache
    redis-benchmark -p "$PORT" -t get -n "$REQUESTS" -c "$CLIENTS" \
                    -r 99999999 --csv 2> /dev/null | tail -1 \
      | parse_csv "$label, keys that never were"
    stop_cachedb
  done
  echo
fi

# --------------------------------------------------------------------
if [[ " $SECTIONS " == *" flush "* ]]; then
  echo "5. What the blocking flush costs the tail"
  echo "   The SET that crosses the threshold writes the whole memtable while"
  echo "   the loop waits. Compare a run that never flushes against one that"
  echo "   flushes constantly: the throughput gap is the stall, and the P99 is"
  echo "   where an interviewer will look first."
  rm -rf "$WORK/flush"; mkdir -p "$WORK/flush"
  start_cachedb "$WORK/flush" --fsync no --memtable-limit 1073741824
  run_bench "$PORT" set "never flushes (1 GB limit)"
  stop_cachedb
  rm -rf "$WORK/flush2"; mkdir -p "$WORK/flush2"
  start_cachedb "$WORK/flush2" --fsync no --memtable-limit 524288
  run_bench "$PORT" set "flushes constantly (512 KB)"
  echo "  flushes produced: $(ls "$WORK/flush2"/*.sst 2> /dev/null | wc -l) tables"
  stop_cachedb
  echo
fi

echo "Report these honestly. Being slower than Redis is expected;"
echo "explaining why, with these numbers, is the part that is worth anything."
