#!/usr/bin/env bash
# Regenerates every number in BENCHMARKS.md, results/summary.json and the
# website's data file from scratch. Nothing in the write-up is typed by hand.
#
#   bench/run_all.sh              # full methodology (~2.5 h on a laptop)
#   QUICK=1 bench/run_all.sh      # smoke run: short durations, 1 rep (~10 min)
#   SKIP_LEVELDB=1 bench/run_all.sh
#
# Methodology (full mode): fresh load per rep, WaitForCompactions() before the
# measured phase, 10 s warmup, 60 s steady state, 3 reps, median reported.
# On Linux the script also pins the CPU governor to `performance` and drops
# the page cache between runs when it has sudo; elsewhere it records that it
# could not.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
BUILD="${BUILD:-build/rel}"
LDB_BUILD="${LDB_BUILD:-build/leveldb}"
OUT="${OUT:-results}"
QUICK="${QUICK:-0}"
SCRATCH="${SCRATCH:-$ROOT/build/bench-scratch}"
EXE=""
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) EXE=".exe" ;; esac

if [ "$QUICK" = "1" ]; then
  WARMUP=2; DURATION=8; REPS=1; RECORDS=300000; CRASH_ITERS=25
else
  WARMUP=10; DURATION=60; REPS=3; RECORDS=1000000; CRASH_ITERS=200
fi
THREADS=8

log() { printf '\n=== %s ===\n' "$*"; }

# Windows: keep the machine from idle-sleeping for the whole run (a suspend
# mid-benchmark silently corrupts every percentile). Released on exit.
if [ -n "$EXE" ]; then
  PY=$(command -v python3 || command -v python)
  "$PY" -c "import ctypes,time; ctypes.windll.kernel32.SetThreadExecutionState(0x80000001); time.sleep(10**7)" &
  AWAKE_PID=$!
  trap 'kill $AWAKE_PID 2>/dev/null || true' EXIT
fi

# ---- Build ----------------------------------------------------------------
log "build ($BUILD)"
cmake -S . -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$BUILD" --parallel
if [ "${SKIP_LEVELDB:-0}" != "1" ]; then
  cmake -S . -B "$LDB_BUILD" -G Ninja -DCMAKE_BUILD_TYPE=Release \
        -DLSMKV_WITH_LEVELDB=ON -DLSMKV_BUILD_TESTS=OFF -DLSMKV_BUILD_TOOLS=OFF >/dev/null
  cmake --build "$LDB_BUILD" --target lsmkv-bench --parallel
fi
BENCH="$BUILD/lsmkv-bench$EXE"

mkdir -p "$OUT"/{ycsb,scaling,shards,openloop,stall,leveldb,perf} "$SCRATCH"
PY=$(command -v python3 || command -v python)

# The test suite must pass before anything is measured; record its size.
log "tests"
ctest --test-dir "$BUILD" --output-on-failure -j 8 --timeout 600 | tail -3
ctest --test-dir "$BUILD" -N | tail -1 > "$OUT/tests.txt"

# ---- Environment prep (best effort) ---------------------------------------
prep_linux() {
  if [ "$(uname -s)" = "Linux" ] && sudo -n true 2>/dev/null; then
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
      echo performance | sudo tee "$g" >/dev/null || true
    done
    sync; echo 3 | sudo tee /proc/sys/vm/drop_caches >/dev/null || true
    echo "governor=performance, page cache dropped"
  else
    echo "no sudo / not Linux: governor and page cache left as-is (recorded in results)"
  fi
}
prep_linux | tee "$OUT/env.txt"
if command -v powercfg.exe >/dev/null 2>&1; then
  powercfg.exe //getactivescheme >> "$OUT/env.txt" 2>&1 || true
fi

run_bench() {  # run_bench <out.json> [flags...]
  local out="$1"; shift
  prep_linux >/dev/null
  "$BENCH" --db="$SCRATCH/db" --warmup=$WARMUP --duration=$DURATION --reps=$REPS \
           --records=$RECORDS --out="$out" "$@"
}

# ---- 1. Component micro-benchmarks (Day 1 baseline) -------------------------
log "micro-bench"
"$BUILD/micro-bench$EXE" --dir="$SCRATCH/micro" --out="$OUT/micro.json"

# ---- 2. Bloom filter sweep ----------------------------------------------------
log "bloom sweep"
"$BUILD/bloom-sweep$EXE" --db="$SCRATCH/bloom" --out="$OUT/bloom_sweep.json"

# ---- 3. YCSB matrix at 8 threads -----------------------------------------------
for W in A B C D F; do
  for D in zipfian uniform; do
    [ "$W" = "D" ] && [ "$D" = "zipfian" ] && D=latest
    log "YCSB $W / $D"
    run_bench "$OUT/ycsb/lsmkv-$W-$D.json" --workload=$W --distribution=$D --threads=$THREADS
  done
done
log "YCSB load"
run_bench "$OUT/ycsb/lsmkv-load.json" --workload=load --threads=$THREADS

# ---- 4. Performance pass: scaling before/after the one fix ---------------------
# Interleaved (baseline, fix, fix, baseline, ...) so machine drift over the run
# cannot favour either side. See bench/ab.py.
PY=$(command -v python3 || command -v python)
log "scaling A/B (interleaved)"
"$PY" bench/ab.py scaling --build "$BUILD" --out "$OUT" --reps $REPS \
      --records $RECORDS --warmup $WARMUP --duration $DURATION
# Memtable sharding: 1 lock vs 8 locks at 8 threads (interleaved as well).
log "shards A/B (interleaved)"
"$PY" bench/ab.py shards --build "$BUILD" --out "$OUT" --reps $REPS \
      --records $RECORDS --warmup $WARMUP --duration $DURATION

# ---- 5. Coordinated omission: closed loop vs open loop at fixed rates -----------
# Calibrate the maximum *now*, not from an earlier run: absolute throughput on
# a laptop drifts, and a rate derived from a quieter moment can already be an
# overload. 110% is included on purpose, to show what overload looks like.
log "open loop: calibrate max (closed loop, measured immediately before)"
"$BENCH" --db="$SCRATCH/db" --workload=A --threads=$THREADS --records=$RECORDS \
         --warmup=$WARMUP --duration=$DURATION --reps=1 --out="$OUT/openloop/A-calibration.json"
MAX=$("$PY" -c "import json;print(int(json.load(open('$OUT/openloop/A-calibration.json'))['median']['throughput']))")
for PCT in 50 80 95 110; do
  RATE=$((MAX * PCT / 100))
  log "open loop A @ ${PCT}% of calibrated max ($RATE ops/s)"
  "$BENCH" --db="$SCRATCH/db" --workload=A --threads=$THREADS --records=$RECORDS \
           --warmup=$WARMUP --duration=$DURATION --reps=1 --rate=$RATE \
           --out="$OUT/openloop/A-rate$PCT.json"
done

# ---- 6. Write-stall regression (tiny memtable) with a metrics time series --------
log "stall: normal memtable (4 MiB)"
"$BENCH" --db="$SCRATCH/db" --workload=A --threads=$THREADS --records=$RECORDS \
         --warmup=5 --duration=$DURATION --reps=1 --write_buffer=4194304 \
         --out="$OUT/stall/normal.json" --timeseries="$OUT/stall/normal-timeseries.json"
log "stall: regression (64 KiB memtable)"
"$BENCH" --db="$SCRATCH/db" --workload=A --threads=$THREADS --records=$RECORDS \
         --warmup=5 --duration=$DURATION --reps=1 --write_buffer=65536 \
         --out="$OUT/stall/tiny.json" --timeseries="$OUT/stall/tiny-timeseries.json"

# ---- 7. Crash matrix --------------------------------------------------------------
log "crash matrix ($CRASH_ITERS iterations)"
python3 tools/crash_matrix.py --build "$BUILD" --iterations $CRASH_ITERS \
        --out "$OUT/crash_matrix.json" 2>/dev/null || \
python tools/crash_matrix.py --build "$BUILD" --iterations $CRASH_ITERS --out "$OUT/crash_matrix.json"

# ---- 8. Credibility anchor: identical harness against LevelDB 1.23 ---------------
if [ "${SKIP_LEVELDB:-0}" != "1" ]; then
  log "LSM-KV vs LevelDB (interleaved)"
  "$PY" bench/ab.py leveldb --build "$LDB_BUILD" --out "$OUT" --reps $REPS \
        --records $RECORDS --warmup $WARMUP --duration $DURATION
fi

# ---- 9. Tables, summary, website data -----------------------------------------------
log "report"
python3 bench/report.py --results "$OUT" 2>/dev/null || python bench/report.py --results "$OUT"
echo "done: $OUT/summary.json, BENCHMARKS.md tables, site/data/results.js"
