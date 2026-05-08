#!/usr/bin/env bash
# SensaPulse — atomic build + flash. Returns 0 on success.
# Usage:
#   scripts/build_flash.sh                 # build + flash
#   scripts/build_flash.sh --build-only    # build only
#   scripts/build_flash.sh --pristine      # rm -rf build first
set -euo pipefail

PROJECT="/Users/trandat/Project/SensaScope"
APP="$PROJECT/app"
BOARD="sensapulse_v1/nrf52840"
LOG_DIR="$PROJECT/logs"
mkdir -p "$LOG_DIR"

BUILD_ONLY=0
PRISTINE=0
for a in "$@"; do
	case "$a" in
		--build-only) BUILD_ONLY=1 ;;
		--pristine)   PRISTINE=1 ;;
	esac
done

cd "$APP"

if (( PRISTINE )); then
	echo "[build_flash] pristine: removing build/"
	rm -rf build
fi

ts=$(date +%Y%m%d_%H%M%S)
build_log="$LOG_DIR/build_${ts}.log"
flash_log="$LOG_DIR/flash_${ts}.log"

echo "[build_flash] west build → $build_log"
if ! west build -b "$BOARD" -p auto \
		-- -DBOARD_ROOT="$PROJECT" \
		>"$build_log" 2>&1; then
	echo "[build_flash] BUILD FAILED — last 40 lines:"
	tail -40 "$build_log"
	exit 1
fi
tail -6 "$build_log" | sed 's/^/[build_flash] /'

if (( BUILD_ONLY )); then
	echo "[build_flash] build-only mode: skipping flash"
	exit 0
fi

echo "[build_flash] west flash → $flash_log"
if ! west flash >"$flash_log" 2>&1; then
	echo "[build_flash] FLASH FAILED — last 40 lines:"
	tail -40 "$flash_log"
	exit 2
fi

# `west flash` exits 0 on success even with the noisy SeggerBackend errors.
if grep -q "flashed successfully" "$flash_log"; then
	echo "[build_flash] OK"
	exit 0
fi
echo "[build_flash] flash log did not contain 'flashed successfully' — check $flash_log"
exit 3
