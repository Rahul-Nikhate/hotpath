#!/usr/bin/env bash
# Runs every experiment and writes CSVs + a summary into results/<host>/.
# Meant for a Linux machine (ideally bare metal, x86-64 with invariant TSC);
# on macOS the Linux-only parts are skipped and noted.
#
#   ./scripts/run_experiments.sh                  # unpinned
#   CPUS=2,4 ./scripts/run_experiments.sh         # pin the two benchmark threads
#   ISOLATED=2 ./scripts/run_experiments.sh       # also run exp. 4 on an isolated core
set -euo pipefail
cd "$(dirname "$0")/.."
B=${BUILD:-build}
HOST=$(hostname -s)
OUT=results/$HOST
mkdir -p "$OUT"
PIN=""
[ -n "${CPUS:-}" ] && PIN="--cpus $CPUS"

{
  echo "# Hotpath results: $HOST"
  echo "date: $(date -u +%Y-%m-%dT%H:%MZ)"
  echo "kernel: $(uname -sr)"
  if [ -r /proc/cpuinfo ]; then
    echo "cpu: $(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2-)"
    echo "invariant tsc: $(grep -q constant_tsc /proc/cpuinfo && grep -q nonstop_tsc /proc/cpuinfo && echo yes || echo no)"
    echo "cmdline: $(cat /proc/cmdline)"
  else
    echo "cpu: $(sysctl -n machdep.cpu.brand_string 2>/dev/null || uname -m)"
  fi
  echo "pinning: ${CPUS:-none}"
} > "$OUT/machine.txt"
cat "$OUT/machine.txt"

make -j"$(getconf _NPROCESSORS_ONLN)" all >/dev/null

echo "== experiment 1b: queue design + hop cost";       $B/hp_bench queue    $PIN --csv "$OUT" | tee "$OUT/queue.txt"
echo "== experiments 1a/3: fast path + warming";        $B/hp_bench fastpath $PIN --csv "$OUT" | tee "$OUT/fastpath.txt"
echo "== experiment 2: page faults + page size";         $B/hp_bench pages         --csv "$OUT" | tee "$OUT/pages.txt"
echo "== template encoding";                             $B/hp_bench encode        --csv "$OUT" | tee "$OUT/encode.txt"
echo "== stretch 6: dispatch";                           $B/hp_bench dispatch      --csv "$OUT" | tee "$OUT/dispatch.txt"

echo "== end to end (sim + engine + control + replay), unpinned"
OUT="$OUT/e2e-unpinned" COUNT=${COUNT:-1000000} RATE=${RATE:-200000} ./scripts/run_demo.sh | tee "$OUT/e2e-unpinned.txt"

if [ -n "${ISOLATED:-}" ]; then
  echo "== experiment 4: pinned to isolated core $ISOLATED, with a noisy neighbour"
  NOISE=""
  if command -v stress-ng >/dev/null; then
    stress-ng --cpu "$(($(getconf _NPROCESSORS_ONLN) / 2))" --vm 2 --vm-bytes 1G --timeout 120s >/dev/null 2>&1 & NOISE=$!
  fi
  OUT="$OUT/e2e-isolated" ENGINE_ARGS="--cpu $ISOLATED ${HUGE:+--huge}" COUNT=${COUNT:-1000000} RATE=${RATE:-200000} \
    ./scripts/run_demo.sh | tee "$OUT/e2e-isolated.txt"
  [ -n "$NOISE" ] && kill $NOISE 2>/dev/null || true
fi

echo "results in $OUT (plot with: python3 scripts/plot.py $OUT)"
