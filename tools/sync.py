#!/usr/bin/env python3
"""SensaPulse v1.1 — BLE sync CLI tool.

This is a STUB. Claude Code should fill in the BLE bits per
docs/SYNC_PROTOCOL.md. The CLI shape and on-disk layout are final.

Usage:
    pip install bleak
    python3 tools/sync.py --output ~/SensaScope_data
    python3 tools/sync.py --output ~/SensaScope_data --device "Dat-chest-01"
    python3 tools/sync.py --list-devices
    python3 tools/sync.py --output ~/SensaScope_data --resume

On-disk layout (PC):
    <output>/<device_label>/SESSION_NNNNN/
        ├── audio.wav
        ├── imu.csv
        └── meta.json

`<device_label>` = device.name if set, else "device_<chip_id>".
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import sys
from pathlib import Path

# pip install bleak
try:
    from bleak import BleakClient, BleakScanner
except ImportError:
    print("missing dependency: pip install bleak", file=sys.stderr)
    sys.exit(2)


SVC_UUID         = "7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e"
CHR_DEVICE_INFO  = "7e7e0002-3c4f-4b8e-8a8a-5e5e5e5e5e5e"
CHR_CONTROL      = "7e7e0003-3c4f-4b8e-8a8a-5e5e5e5e5e5e"
CHR_DATA         = "7e7e0004-3c4f-4b8e-8a8a-5e5e5e5e5e5e"
CHR_SET_NAME     = "7e7e0005-3c4f-4b8e-8a8a-5e5e5e5e5e5e"

OP_LIST   = 0x01
OP_READ   = 0x02
OP_ACK    = 0x03
OP_ABORT  = 0x04
OP_DEL    = 0x05
OP_RESET  = 0xFF

FILE_NAMES = ["audio.wav", "imu.csv", "meta.json"]

log = logging.getLogger("sync")


async def scan_for_devices(timeout: float = 5.0) -> list[tuple[str, str]]:
    """Return list of (address, advertised_name) tuples for SensaPulse devices."""
    found: list[tuple[str, str]] = []
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name and "SensaPulse" in d.name:
            found.append((d.address, d.name))
    return found


async def fetch_device_info(client: BleakClient) -> dict:
    raw = await client.read_gatt_char(CHR_DEVICE_INFO)
    return json.loads(raw.decode("utf-8"))


async def cmd_list(client: BleakClient) -> list[tuple[int, int]]:
    """Return [(session_id, total_bytes), ...] for unsynced sessions."""
    raise NotImplementedError("Claude Code: implement per SYNC_PROTOCOL.md")


async def cmd_read(client: BleakClient, sid: int, file_idx: int, dest: Path) -> None:
    """Stream one file via Data notifications, write to dest."""
    raise NotImplementedError("Claude Code: implement per SYNC_PROTOCOL.md")


async def cmd_ack(client: BleakClient, sid: int) -> None:
    raise NotImplementedError("Claude Code: implement per SYNC_PROTOCOL.md")


def device_label(info: dict) -> str:
    name = info.get("name", "").strip()
    if name:
        return name
    return f"device_{info.get('chip_id', 'unknown')}"


async def sync_one_device(addr: str, output_root: Path, resume: bool) -> int:
    async with BleakClient(addr) as client:
        # 1. Negotiate connection params for throughput (PHY 2M, MTU 247).
        # 2. Read device info, refuse if state != "idle".
        # 3. List unsynced sessions, iterate.
        # 4. For each session: download to <root>/<label>/SESSION_NNNNN.tmp/,
        #    rename to final on success, then ACK.
        info = await fetch_device_info(client)
        log.info("connected: %s", info)
        if info.get("state") != "idle":
            log.error("device busy (state=%s) — sync aborted", info.get("state"))
            return 1

        label = device_label(info)
        device_dir = output_root / label
        device_dir.mkdir(parents=True, exist_ok=True)

        sessions = await cmd_list(client)
        log.info("%d unsynced session(s)", len(sessions))

        for sid, total_bytes in sessions:
            tmp = device_dir / f"SESSION_{sid:05d}.tmp"
            final = device_dir / f"SESSION_{sid:05d}"
            if final.exists() and not resume:
                log.info("skip session %d (already on disk, no --resume)", sid)
                continue
            tmp.mkdir(parents=True, exist_ok=True)
            for fname, idx in zip(FILE_NAMES, range(len(FILE_NAMES))):
                await cmd_read(client, sid, idx, tmp / fname)
            tmp.rename(final)
            await cmd_ack(client, sid)
            log.info("session %d synced", sid)
        return 0


def main() -> int:
    p = argparse.ArgumentParser(description="SensaPulse BLE sync")
    p.add_argument("--output", type=Path, default=Path.home() / "SensaScope_data",
                   help="root folder on PC for synced data")
    p.add_argument("--device", help="match device by advertised name (default: any)")
    p.add_argument("--list-devices", action="store_true",
                   help="scan and print SensaPulse devices, then exit")
    p.add_argument("--resume", action="store_true",
                   help="re-attempt interrupted transfers")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    async def run():
        if args.list_devices:
            devs = await scan_for_devices()
            for addr, name in devs:
                print(f"{addr}\t{name}")
            return 0

        devs = await scan_for_devices()
        if not devs:
            log.error("no SensaPulse devices found")
            return 1
        if args.device:
            devs = [(a, n) for a, n in devs if args.device in (n or "")]
            if not devs:
                log.error("no device matched name=%s", args.device)
                return 1

        rc = 0
        for addr, name in devs:
            log.info("syncing %s (%s)", name, addr)
            rc |= await sync_one_device(addr, args.output, args.resume)
        return rc

    return asyncio.run(run())


if __name__ == "__main__":
    sys.exit(main())
