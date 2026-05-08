#!/usr/bin/env bash
# SensaPulse — capture RTT to a log file using JLinkRTTLogger.
# Usage: scripts/rtt_capture.sh DURATION_SEC OUTPUT_LOG
#
# Resets the chip first (J-Link), then logs RTT for DURATION_SEC seconds,
# then kills JLinkRTTLogger. Output file contains all logs from boot.
set -euo pipefail

DURATION="${1:-30}"
OUT="${2:-/Users/trandat/Project/SensaScope/logs/rtt_$(date +%Y%m%d_%H%M%S).log}"
mkdir -p "$(dirname "$OUT")"

# Reset target via JLinkExe in batch mode (script feeds 'r' then 'g' then 'q')
echo "[rtt_capture] resetting chip..."
{
	echo "r"
	echo "g"
	echo "q"
} | JLinkExe -device NRF52840_XXAA -if SWD -speed 4000 -autoconnect 1 \
		>/dev/null 2>&1 || {
	echo "[rtt_capture] JLinkExe reset failed (chip might still run)"
}

# Run RTT logger in background; auto-kill after DURATION
echo "[rtt_capture] capturing RTT for ${DURATION}s → $OUT"
JLinkRTTLogger -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 "$OUT" \
		>/dev/null 2>&1 &
LOGGER_PID=$!

# Race: kill logger after duration
( sleep "$DURATION" && kill "$LOGGER_PID" 2>/dev/null ) &
KILLER_PID=$!

# Wait for logger to exit
wait "$LOGGER_PID" 2>/dev/null || true
kill "$KILLER_PID" 2>/dev/null || true

# Cleanup any straggler J-Link processes
pkill -f JLinkRTTLogger 2>/dev/null || true

if [[ -s "$OUT" ]]; then
	bytes=$(wc -c <"$OUT" | tr -d ' ')
	lines=$(wc -l <"$OUT" | tr -d ' ')
	echo "[rtt_capture] OK: $bytes B, $lines lines"
	exit 0
fi
echo "[rtt_capture] OUTPUT EMPTY — chip may not have booted or RTT block not found"
exit 1
