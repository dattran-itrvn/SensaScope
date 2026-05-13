#!/usr/bin/env python3
"""
Automated #20 verification — exercise IDLE / SYNC / disconnect transitions.

Loop: scan → connect → hold → disconnect → wait → repeat.

Pair with `scripts/rtt_capture.sh` (run in parallel) to confirm FSM logs
on the device side: BLE connected → FSM: IDLE → SYNC, BLE disconnected
→ FSM: SYNC → IDLE.

Requires `bleak`. Install: `pip3 install --user bleak`.

Usage:
    python3 tools/test_ble_sync_state.py [--name "SensaPulse v1"] [--rounds 3] [--hold 5]
"""
from __future__ import annotations

import argparse
import asyncio
import sys
import time

from bleak import BleakClient, BleakScanner


def stamp() -> str:
	return time.strftime("%H:%M:%S", time.localtime())


async def find_device(name: str, scan_timeout: float):
	print(f"[{stamp()}] scanning for '{name}' (timeout {scan_timeout}s)…")
	device = await BleakScanner.find_device_by_name(name, timeout=scan_timeout)
	if device is None:
		print(f"[{stamp()}] not found — is the board advertising? (check RTT log)")
	return device


async def one_round(name: str, hold: float, scan_timeout: float) -> bool:
	device = await find_device(name, scan_timeout)
	if device is None:
		return False
	print(f"[{stamp()}] found {device.address} — connecting…")
	try:
		async with BleakClient(device) as client:
			if not client.is_connected:
				print(f"[{stamp()}] connect returned but not connected")
				return False
			print(f"[{stamp()}] connected — holding {hold}s")
			await asyncio.sleep(hold)
			print(f"[{stamp()}] disconnecting…")
		print(f"[{stamp()}] disconnected (context manager closed)")
		return True
	except Exception as e:
		print(f"[{stamp()}] connect/hold error: {e!r}")
		return False


async def main():
	ap = argparse.ArgumentParser(description=__doc__,
	                             formatter_class=argparse.RawDescriptionHelpFormatter)
	ap.add_argument("--name", default="SensaPulse v1",
	                help="advertise name to scan for (default: SensaPulse v1)")
	ap.add_argument("--rounds", type=int, default=3,
	                help="number of connect/disconnect cycles (default: 3)")
	ap.add_argument("--hold", type=float, default=5.0,
	                help="seconds to stay connected per round (default: 5)")
	ap.add_argument("--gap", type=float, default=3.0,
	                help="seconds between rounds (default: 3)")
	ap.add_argument("--scan-timeout", type=float, default=10.0,
	                help="scan timeout per round (default: 10)")
	args = ap.parse_args()

	passes = 0
	for i in range(args.rounds):
		print(f"\n=== round {i + 1}/{args.rounds} ===")
		ok = await one_round(args.name, args.hold, args.scan_timeout)
		if ok:
			passes += 1
		else:
			print(f"[{stamp()}] round {i + 1} FAILED")
		if i + 1 < args.rounds:
			await asyncio.sleep(args.gap)

	print(f"\n=== summary: {passes}/{args.rounds} rounds passed ===")
	sys.exit(0 if passes == args.rounds else 1)


if __name__ == "__main__":
	asyncio.run(main())
