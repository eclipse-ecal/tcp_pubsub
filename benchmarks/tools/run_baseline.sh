#!/bin/bash
# Records a baseline with the end-to-end harness and the micro-benchmarks (Linux / macOS).
#
#   benchmarks/tools/run_baseline.sh <build-dir> <out-dir> <platform-tag> ["platform description"]
#
#   build-dir      CMake build directory (single-config generator, Release) that contains
#                  benchmarks/tcp_pubsub_benchmark/tcp_pubsub_benchmark and
#                  benchmarks/tcp_pubsub_micro_benchmark/tcp_pubsub_micro_benchmark
#   out-dir        where the JSON files are written
#   platform-tag   used in the file names, e.g. linux-native, linux-wsl2, macos-arm64
#
# Environment (all optional):
#   BENCH_ADAPTER_TAG  adapter name used in file names             (default: v1)
#   BENCH_DURATION     measured seconds per scenario               (default: 10)
#   BENCH_WARMUP       warm-up seconds                             (default: 1)
#   BENCH_REPEAT       repetitions per scenario / micro-benchmark  (default: 3)
#   BENCH_MAXTRY       attempts per scenario after a crash or a noisy run (default: 3)
#   BENCH_SKIP_MICRO   1 = run only the end-to-end part            (default: 0)
#   BENCH_COMMIT       commit id stored in the label               (default: git HEAD)
#
# Why one process per scenario with retries: the current implementation can crash
# (SIGSEGV in asio's epoll_reactor::deregister_descriptor) when it is torn down repeatedly,
# and a run that was disturbed (laptop going to sleep, other load) is flagged by the harness
# ("pacing overruns"). Every crash and every noisy attempt is logged and the scenario is
# repeated. Keep the machine quiet and awake while this runs (about 20 minutes with defaults).
set -u

if [ $# -lt 3 ]; then
  sed -n '2,/^set -u/p' "$0" | sed '$d' | sed 's/^# \{0,1\}//'
  exit 2
fi

BUILD=$1
OUT=$2
TAG=$3
DESC=${4:-$TAG}
ADAPTER=${BENCH_ADAPTER_TAG:-v1}
DURATION=${BENCH_DURATION:-10}
WARMUP=${BENCH_WARMUP:-1}
REPEAT=${BENCH_REPEAT:-3}
MAXTRY=${BENCH_MAXTRY:-3}
SKIP_MICRO=${BENCH_SKIP_MICRO:-0}

E2E=$BUILD/benchmarks/tcp_pubsub_benchmark/tcp_pubsub_benchmark
MICRO=$BUILD/benchmarks/tcp_pubsub_micro_benchmark/tcp_pubsub_micro_benchmark
[ -x "$E2E" ] || { echo "not found: $E2E"; exit 2; }
[ "$SKIP_MICRO" = 1 ] || [ -x "$MICRO" ] || { echo "not found: $MICRO"; exit 2; }
mkdir -p "$OUT"
W=$(mktemp -d)
trap 'rm -rf "$W"' EXIT
EVENTS=$W/events.txt; : > "$EVENTS"
COMMIT=${BENCH_COMMIT:-$(git -C "$(dirname "$0")" rev-parse --short HEAD 2>/dev/null || echo unknown)}

log() { echo "[$(date +%T)] $*"; }

check_json() {   # exit 1 if any run failed or the pacing thread was stalled
python3 - "$1" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
bad = [r for r in d["results"] if r.get("error") or r.get("pacing_overruns", 0) > 0]
sys.exit(1 if bad else 0)
PY
}

e2e_one() {      # $1 delivery (pooled|inline)  $2 scenario
  local delivery=$1 sc=$2 attempt=1 f
  f=$W/e2e_${delivery}_${sc}.json
  while [ $attempt -le "$MAXTRY" ]; do
    rm -f "$f"
    "$E2E" --scenario "$sc" --duration "$DURATION" --warmup "$WARMUP" --repeat "$REPEAT" --io-threads 2 \
           --delivery "$delivery" --label x --json "$f" > "$W/e2e_${delivery}_${sc}.log" 2>&1 < /dev/null
    local code=$?
    if [ $code -ne 0 ] || [ ! -s "$f" ]; then
      echo "CRASH e2e $delivery $sc attempt $attempt exit=$code" >> "$EVENTS"; log "CRASH $delivery/$sc attempt $attempt (exit $code)"
    elif ! check_json "$f"; then
      echo "NOISY e2e $delivery $sc attempt $attempt (pacing overruns or error)" >> "$EVENTS"; log "NOISY $delivery/$sc attempt $attempt"
      [ $attempt -ge "$MAXTRY" ] && return 0     # keep the last noisy result, it is logged
    else
      log "ok   $delivery/$sc"; return 0
    fi
    attempt=$((attempt+1))
  done
  [ -s "$f" ] || echo "MISSING e2e $delivery $sc" >> "$EVENTS"
  return 0
}

merge_e2e() {    # $1 delivery  $2 output file  $3 label  $4... scenarios (in order)
  local delivery=$1 out=$2 label=$3; shift 3
  python3 - "$out" "$label" "$delivery" "$W" "$@" <<'PY'
import json, os, sys
out, label, delivery, w, scenarios = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5:]
meta, results = None, []
for sc in scenarios:
    p = os.path.join(w, "e2e_%s_%s.json" % (delivery, sc))
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        continue
    d = json.load(open(p))
    meta = meta or d["meta"]
    results.extend(d["results"])
meta["label"] = label
json.dump({"meta": meta, "results": results}, open(out, "w"), indent=1)
print("wrote", out, len(results), "results")
PY
}

micro_one() {    # $1 index  $2 benchmark name
  local i=$1 name=$2 attempt=1
  while [ $attempt -le "$MAXTRY" ]; do
    local f=$W/micro_$i.json
    rm -f "$f"
    "$MICRO" --benchmark_filter="^${name}\$" --benchmark_repetitions="$REPEAT" --benchmark_min_time=0.3s \
             --benchmark_out="$f" --benchmark_out_format=json > "$W/micro_$i.log" 2>&1 < /dev/null
    local code=$?
    if [ $code -eq 0 ] && [ -s "$f" ]; then return 0; fi
    echo "CRASH micro $name attempt $attempt exit=$code" >> "$EVENTS"; log "CRASH micro $name attempt $attempt (exit $code)"
    attempt=$((attempt+1))
  done
  echo "MISSING micro $name" >> "$EVENTS"
  return 0
}

log "start: tag=$TAG commit=$COMMIT duration=${DURATION}s warmup=${WARMUP}s repeat=$REPEAT maxtry=$MAXTRY"

SCENARIOS=$("$E2E" --list < /dev/null | awk '{print $1}')
log "=== end-to-end, pooled delivery"
for sc in $SCENARIOS; do e2e_one pooled "$sc"; done
merge_e2e pooled "$OUT/baseline-$ADAPTER-$TAG-pooled.json" \
  "baseline $ADAPTER, commit $COMMIT, $DESC, pooled delivery, one process per scenario" $SCENARIOS

log "=== end-to-end, inline delivery (subset)"
INLINE="small-64B small-1KiB small-64KiB large-1MiB fanout-10"
for sc in $INLINE; do e2e_one inline "$sc"; done
merge_e2e inline "$OUT/baseline-$ADAPTER-$TAG-inline.json" \
  "baseline $ADAPTER, commit $COMMIT, $DESC, inline delivery, one process per scenario" $INLINE

if [ "$SKIP_MICRO" != 1 ]; then
  log "=== micro-benchmarks"
  i=0
  NAMES=$("$MICRO" --benchmark_list_tests=true < /dev/null 2>/dev/null)
  for name in $NAMES; do i=$((i+1)); micro_one $i "$name"; done
  python3 - "$OUT/micro-$ADAPTER-$TAG.json" "$W" "$i" <<'PY'
import json, os, sys
out, w, n = sys.argv[1], sys.argv[2], int(sys.argv[3])
ctx, benches = None, []
for i in range(1, n + 1):
    p = os.path.join(w, "micro_%d.json" % i)
    if os.path.exists(p) and os.path.getsize(p) > 0:
        d = json.load(open(p))
        ctx = ctx or d["context"]
        benches.extend(d["benchmarks"])
json.dump({"context": ctx, "benchmarks": benches}, open(out, "w"), indent=1)
print("wrote", out, len(benches), "entries")
PY
fi

log "=== events (crashes / noisy runs / missing results)"
if [ -s "$EVENTS" ]; then cat "$EVENTS"; else echo "none"; fi
log "done"
exit 0
