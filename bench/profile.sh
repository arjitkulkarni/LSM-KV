#!/usr/bin/env bash
# Sampling profile of YCSB workload A at 8 threads (Linux).
#
#   bench/profile.sh                    # after the fix
#   ADAPTIVE_WAIT=0 bench/profile.sh    # the pre-fix baseline
#
# Produces results/perf/flame-aw<0|1>.svg. Needs `perf` and a clone of
# https://github.com/brendangregg/FlameGraph (set FLAMEGRAPH=/path/to/it).
# Build with frame pointers so perf can walk the stacks:
#   cmake -S . -B build/prof -DCMAKE_BUILD_TYPE=RelWithDebInfo \
#         -DCMAKE_CXX_FLAGS="-fno-omit-frame-pointer"
set -euo pipefail
BUILD="${BUILD:-build/prof}"
FLAMEGRAPH="${FLAMEGRAPH:-$HOME/FlameGraph}"
AW="${ADAPTIVE_WAIT:-1}"
OUT=results/perf
mkdir -p "$OUT"

perf record -F 999 -g --call-graph fp -o "$OUT/perf-aw$AW.data" -- \
  "$BUILD/lsmkv-bench" --workload=A --threads=8 --records=1000000 \
  --warmup=5 --duration=30 --reps=1 --db=/tmp/lsmkv-prof --adaptive_wait="$AW"

perf script -i "$OUT/perf-aw$AW.data" \
  | "$FLAMEGRAPH/stackcollapse-perf.pl" \
  | "$FLAMEGRAPH/flamegraph.pl" --title "LSM-KV YCSB-A 8 threads (adaptive_wait=$AW)" \
  > "$OUT/flame-aw$AW.svg"
echo "wrote $OUT/flame-aw$AW.svg"

# Off-CPU view: where threads *sleep* (the pre-fix bottleneck is time spent
# blocked in the commit queue, which an on-CPU profile under-reports).
if command -v offcputime-bpfcc >/dev/null 2>&1; then
  echo "tip: sudo offcputime-bpfcc -df -p \$(pgrep lsmkv-bench) 30 > $OUT/offcpu.stacks"
fi
