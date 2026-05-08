#!/usr/bin/env bash
# SensaPulse — capture RTT to a log file using the reliable
# two-process pattern that works on macOS Apple Silicon:
#
#   1. JLinkExe stays alive as a gateway on localhost:19021. We feed it
#      commands via pipe: reset, go (release halt), sleep, quit.
#   2. JLinkRTTClient connects to that gateway and streams the RTT
#      buffer to stdout, which we redirect to the output log.
#
# This replaces an earlier JLinkRTTLogger-only version which silently
# failed to find the RTT control block on macOS Apple Silicon.
#
# Usage: scripts/rtt_capture.sh DURATION_SEC OUTPUT_LOG

set -euo pipefail

DURATION="${1:-30}"
OUT="${2:-/Users/trandat/Project/SensaScope/logs/rtt_$(date +%Y%m%d_%H%M%S).log}"
mkdir -p "$(dirname "$OUT")"

# Cleanup any straggler J-Link processes from a previous run.
pkill -f JLinkExe       2>/dev/null || true
pkill -f JLinkRTTClient 2>/dev/null || true
pkill -f JLinkRTTLogger 2>/dev/null || true
sleep 1

# 1. Spawn JLinkExe in background. We feed commands on stdin via subshell:
#       r  → reset target
#       g  → release halt (firmware starts running)
#       sleep DURATION+3 → keep stdin open + JLinkExe alive long enough
#                          for the RTT client to capture
#       q  → clean quit
#
# JLinkExe inherits the controlling-tty heuristic; piping stdin works
# because the SEGGER CLI reads commands line-by-line from stdin in
# `-autoconnect 1` mode.
echo "[rtt_capture] starting J-Link gateway (port 19021)..."
{
	echo "r"
	sleep 0.5
	echo "g"
	sleep "$((DURATION + 3))"
	echo "q"
} | JLinkExe \
	-device NRF52840_XXAA \
	-if SWD \
	-speed 4000 \
	-autoconnect 1 \
	-RTTTelnetPort 19021 \
	>/dev/null 2>&1 &
JLINK_PID=$!

# 2. Give the gateway time to connect + reset the chip + release halt.
sleep 3

# 3. Connect RTT client and capture for DURATION seconds.
#    macOS doesn't ship GNU `timeout`, so we use background + sleep + kill.
echo "[rtt_capture] capturing RTT for ${DURATION}s → $OUT"
JLinkRTTClient > "$OUT" 2>&1 &
RTT_PID=$!
sleep "$DURATION"
kill "$RTT_PID" 2>/dev/null || true
wait "$RTT_PID" 2>/dev/null || true

# 4. Cleanup. JLinkExe's `q` command will arrive after its inner sleep,
#    but kill anyway to be safe in case timing slipped.
kill "$JLINK_PID" 2>/dev/null || true
wait "$JLINK_PID" 2>/dev/null || true
pkill -f JLinkRTTClient 2>/dev/null || true

if [[ ! -s "$OUT" ]]; then
	echo "[rtt_capture] OUTPUT EMPTY — chip may have crashed pre-RTT-init or J-Link gateway never came up"
	exit 1
fi

# Check for actual firmware output (RTTClient header alone doesn't count).
if ! grep -q "Booting nRF Connect SDK\|<inf>\|<err>\|<wrn>" "$OUT" 2>/dev/null; then
	echo "[rtt_capture] WARN: log has $(wc -c <"$OUT" | tr -d ' ') B but no firmware log lines — RTT client connected but chip silent"
	exit 2
fi

bytes=$(wc -c <"$OUT" | tr -d ' ')
lines=$(wc -l <"$OUT" | tr -d ' ')
echo "[rtt_capture] OK: $bytes B, $lines lines"
exit 0
