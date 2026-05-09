#!/usr/bin/env bash
# SD-stress isolation loop. Builds the stress firmware once, then flashes
# + RTT-captures + parses N times in a row. No human input needed once
# kicked off — there's no tap detection in the stress firmware, it auto-runs
# the full phase sequence on boot.
#
# Usage:
#   bash scripts/sd_stress_loop.sh [N_ITERATIONS] [SECONDS_PER_RUN]
# Defaults:
#   N_ITERATIONS    = 10
#   SECONDS_PER_RUN = 240   (covers ~3.5 min of test + 15 s slack)
set -euo pipefail

ITER="${1:-10}"
DUR="${2:-240}"

REPO="/Users/trandat/Project/SensaScope"
APPDIR="$REPO/app/sd_stress"
LOGDIR="$REPO/logs/sd_stress_$(date +%Y%m%d_%H%M)"
mkdir -p "$LOGDIR"

echo "=========================================="
echo "SD stress loop"
echo "  iterations:    $ITER"
echo "  per-run secs:  $DUR"
echo "  log dir:       $LOGDIR"
echo "=========================================="

# Build once. The stress firmware doesn't change between iterations.
cd "$APPDIR"
echo ""
echo "--- Building stress firmware ---"
west build -b sensapulse_v1/nrf52840 -p auto -- \
    -DBOARD_ROOT="$REPO" 2>&1 | tee "$LOGDIR/build.log"
test ${PIPESTATUS[0]} -eq 0 || { echo "BUILD FAILED — see $LOGDIR/build.log"; exit 1; }

cd "$REPO"

# Flash + capture loop.
for i in $(seq 1 "$ITER"); do
    LOG="$LOGDIR/run_$(printf '%02d' "$i").log"
    echo ""
    echo "=========================================="
    echo "Iteration $i / $ITER → $LOG"
    echo "=========================================="
    cd "$APPDIR"
    west flash 2>&1 | tail -20
    cd "$REPO"
    bash scripts/rtt_capture.sh "$DUR" "$LOG" || {
        echo "RTT capture failed for iteration $i"
    }
done

echo ""
echo "=========================================="
echo "All iterations done. Parsing results..."
echo "=========================================="
python3 scripts/parse_sd_stress.py "$LOGDIR"/run_*.log \
    | tee "$LOGDIR/verdict.md"

echo ""
echo "Aggregate verdict written to $LOGDIR/verdict.md"
