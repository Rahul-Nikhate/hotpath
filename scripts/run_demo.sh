#!/usr/bin/env bash
# Full system over loopback: exchange simulator + engine + control process,
# with packet loss and an injected order-session disconnect, then replays the
# journal three times and checks every decision is reproduced exactly.
#
#   ./scripts/run_demo.sh                 defaults below
#   COUNT=2000000 RATE=500000 ./scripts/run_demo.sh
#   ENGINE_ARGS="--cpu 2 --huge" ./scripts/run_demo.sh      (Linux)
set -euo pipefail
cd "$(dirname "$0")/.."
B=${BUILD:-build}
OUT=${OUT:-results/demo}
COUNT=${COUNT:-500000}
RATE=${RATE:-200000}
DROP=${DROP:-0.005}
DISC=${DISC:-100}
NAME=${NAME:-demo$$}
mkdir -p "$OUT"

SIM=""; ENG=""; CTL=""
cleanup() { kill $SIM $ENG $CTL 2>/dev/null || true; }
trap cleanup EXIT

echo "== demo: $COUNT msgs @ $RATE/s, ${DROP} datagram loss, session drop after $DISC orders"
$B/hp_sim --count "$COUNT" --rate "$RATE" --drop "$DROP" --disconnect-after "$DISC" ${SIM_ARGS:-} \
    > "$OUT/sim.txt" 2>&1 & SIM=$!
$B/hp_engine --name "$NAME" --csv "$OUT/latency.csv" ${ENGINE_ARGS:-} > "$OUT/engine.txt" 2>&1 & ENG=$!
# The control process must look where the engine put the region (hugetlbfs vs /dev/shm).
HUGE_FLAG=""
case " ${ENGINE_ARGS:-} " in *" --huge "*) HUGE_FLAG="--huge" ;; esac
$B/hp_control --name "$NAME" --journal "$OUT/journal.bin" --quiet $HUGE_FLAG ${CONTROL_ARGS:-} > "$OUT/control.txt" 2>&1 & CTL=$!

rc=0
wait $ENG || rc=$?
wait $CTL || true
wait $SIM || true
trap - EXIT

cat "$OUT/engine.txt"
echo
tail -4 "$OUT/sim.txt"
cat "$OUT/control.txt"
echo
echo "== replay"
$B/hp_replay "$OUT/journal.bin" --runs 3
exit $rc
