#!/usr/bin/env python3
"""SensaPulse v1.1 — BLE sync CLI tool.

Implements the host side of `docs/SYNC_PROTOCOL.md`. Flow:

    scan → connect → fetch device info → LIST →
    for each unsynced session:
        READ audio.wav / imu.csv / meta.json (resumable)
        rename .tmp → final
        ACK   (removes .unsynced marker on device)

On-disk layout (PC):
    <output>/<device_label>/SESSION_NNNNN/
        ├── audio.wav
        ├── imu.csv
        └── meta.json

`<device_label>` = device.name if set, else "device_<chip_id>".

Usage:
    pip install bleak
    python3 tools/sync.py --output ~/SensaScope_data
    python3 tools/sync.py --output ~/SensaScope_data --device "Dat-chest-01"
    python3 tools/sync.py --list-devices
    python3 tools/sync.py --output ~/SensaScope_data --resume
"""
from __future__ import annotations

import argparse
import asyncio
import json
import logging
import os
import sys
from pathlib import Path

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

OP_LIST          = 0x01
OP_READ          = 0x02
OP_ACK           = 0x03
OP_ABORT         = 0x04
OP_DEL           = 0x05
OP_START_RECORD  = 0x06  # v1.1.1
OP_STOP_RECORD   = 0x07  # v1.1.1
OP_RESET         = 0xFF

ST_OK             = 0x00
ST_BUSY           = 0x01
ST_NOT_FOUND      = 0x02
ST_ALREADY_SYNCED = 0x03
ST_IO_ERR         = 0x04
ST_INVALID        = 0x05

STATUS_NAMES = {
    ST_OK: "ok",
    ST_BUSY: "busy",
    ST_NOT_FOUND: "not_found",
    ST_ALREADY_SYNCED: "already_synced",
    ST_IO_ERR: "io_err",
    ST_INVALID: "invalid",
}

FILE_NAMES = ["audio.wav", "imu.csv", "meta.json"]

log = logging.getLogger("sync")


class SyncSession:
    """Wraps a connected BleakClient with notify dispatch for Control + Data.

    Bleak's start_notify fires from the asyncio event loop, so per-opcode
    asyncio.Queue is safe without locks. We hand each Control reply to a
    queue keyed by opcode, and bulk Data chunks to a single queue consumed
    by cmd_read.
    """

    def __init__(self, client: BleakClient):
        self.client = client
        self.ctrl_replies: dict[int, asyncio.Queue[bytes]] = {}
        self.data_q: asyncio.Queue[bytes] = asyncio.Queue()

    async def setup(self) -> None:
        await self.client.start_notify(CHR_CONTROL, self._on_ctrl)
        await self.client.start_notify(CHR_DATA, self._on_data)
        # Reset device-side READ state in case the previous run left it dirty.
        await self._raw_write(bytes([OP_RESET, 0]))
        try:
            await asyncio.wait_for(self._ctrl_queue(OP_RESET).get(), timeout=2.0)
        except asyncio.TimeoutError:
            log.debug("RESET_CTL: no reply (older firmware?)")

    def _ctrl_queue(self, op: int) -> asyncio.Queue[bytes]:
        return self.ctrl_replies.setdefault(op, asyncio.Queue())

    def _on_ctrl(self, _char, data: bytearray) -> None:
        if len(data) < 2:
            log.warning("ctrl notify too short: %d byte", len(data))
            return
        op = data[0]
        self._ctrl_queue(op).put_nowait(bytes(data))

    def _on_data(self, _char, data: bytearray) -> None:
        self.data_q.put_nowait(bytes(data))

    async def _raw_write(self, frame: bytes) -> None:
        await self.client.write_gatt_char(CHR_CONTROL, frame, response=True)

    async def _await_ctrl(self, op: int, timeout: float) -> bytes:
        return await asyncio.wait_for(self._ctrl_queue(op).get(), timeout=timeout)

    async def fetch_info(self) -> dict:
        raw = await self.client.read_gatt_char(CHR_DEVICE_INFO)
        return json.loads(raw.decode("utf-8"))

    async def cmd_list(self) -> list[tuple[int, int]]:
        """Return [(session_id, total_bytes), ...] for unsynced sessions."""
        await self._raw_write(bytes([OP_LIST, 0]))
        reply = await self._await_ctrl(OP_LIST, timeout=30.0)
        if reply[1] != ST_OK:
            raise RuntimeError(f"LIST failed: status={STATUS_NAMES.get(reply[1], reply[1])}")
        if len(reply) < 4:
            raise RuntimeError(f"LIST reply too short: {len(reply)} byte")
        n = reply[2] | (reply[3] << 8)
        out: list[tuple[int, int]] = []
        pos = 4
        for _ in range(n):
            if pos + 6 > len(reply):
                raise RuntimeError(
                    f"LIST reply truncated: expected {n} entries, got {len(out)}")
            sid = reply[pos] | (reply[pos + 1] << 8)
            sz = (reply[pos + 2] | (reply[pos + 3] << 8)
                  | (reply[pos + 4] << 16) | (reply[pos + 5] << 24))
            out.append((sid, sz))
            pos += 6
        return out

    async def cmd_read(self, sid: int, file_idx: int, dest: Path,
                       offset: int = 0, length: int = 0) -> tuple[int, int]:
        """Stream one file. Returns (bytes_written_this_call, total_reported).

        If `offset` > 0, dest is opened in append mode (resume).
        `length=0` means "to EOF"; non-zero caps the device-side stream.
        """
        # Drain any leftover data chunks from a previous READ.
        while not self.data_q.empty():
            try:
                self.data_q.get_nowait()
            except asyncio.QueueEmpty:
                break

        frame = bytearray([OP_READ, 0])
        frame += sid.to_bytes(2, "little")
        frame += bytes([file_idx])
        frame += offset.to_bytes(4, "little")
        frame += length.to_bytes(4, "little")
        await self._raw_write(bytes(frame))

        ctrl_q = self._ctrl_queue(OP_READ)
        mode = "ab" if offset > 0 else "wb"
        bytes_written = 0
        ctrl_reply: bytes | None = None

        with dest.open(mode) as f:
            while ctrl_reply is None:
                data_task = asyncio.create_task(self.data_q.get())
                ctrl_task = asyncio.create_task(ctrl_q.get())
                done, pending = await asyncio.wait(
                    [data_task, ctrl_task],
                    return_when=asyncio.FIRST_COMPLETED,
                    timeout=15.0,
                )
                if not done:
                    for t in pending:
                        t.cancel()
                    if not self.client.is_connected:
                        raise ConnectionError(
                            f"link dropped during READ "
                            f"(sess={sid} file={file_idx} bytes={bytes_written})")
                    raise TimeoutError(
                        f"READ stalled: no data/ctrl for 15s "
                        f"(sess={sid} file={file_idx} bytes={bytes_written})")
                if ctrl_task in done:
                    ctrl_reply = ctrl_task.result()
                    # Drain any data chunks already queued before ctrl arrived.
                    while not self.data_q.empty():
                        chunk = self.data_q.get_nowait()
                        if chunk:
                            f.write(chunk)
                            bytes_written += len(chunk)
                    if not data_task.done():
                        data_task.cancel()
                    # Brief grace window for the last in-flight chunks:
                    # device sends data chunks → 0-byte EOF → ctrl reply, but
                    # the BLE driver can re-order delivery so a 40-200 byte
                    # tail chunk may still be in transit when ctrl arrives.
                    grace_deadline = asyncio.get_event_loop().time() + 0.5
                    while True:
                        remaining = grace_deadline - asyncio.get_event_loop().time()
                        if remaining <= 0:
                            break
                        try:
                            chunk = await asyncio.wait_for(self.data_q.get(),
                                                            timeout=remaining)
                        except asyncio.TimeoutError:
                            break
                        if chunk:
                            f.write(chunk)
                            bytes_written += len(chunk)
                else:
                    chunk = data_task.result()
                    if chunk:
                        f.write(chunk)
                        bytes_written += len(chunk)
                    if not ctrl_task.done():
                        ctrl_task.cancel()

        status = ctrl_reply[1]
        if len(ctrl_reply) >= 6:
            total = (ctrl_reply[2] | (ctrl_reply[3] << 8)
                     | (ctrl_reply[4] << 16) | (ctrl_reply[5] << 24))
        else:
            total = bytes_written

        if status != ST_OK:
            raise RuntimeError(
                f"READ failed sess={sid} file={file_idx}: "
                f"status={STATUS_NAMES.get(status, status)}")
        return bytes_written, total

    async def cmd_ack(self, sid: int) -> None:
        frame = bytearray([OP_ACK, 0])
        frame += sid.to_bytes(2, "little")
        await self._raw_write(bytes(frame))
        reply = await self._await_ctrl(OP_ACK, timeout=10.0)
        if reply[1] != ST_OK:
            raise RuntimeError(
                f"ACK sess={sid} failed: status={STATUS_NAMES.get(reply[1], reply[1])}")

    async def cmd_start_record(self) -> None:
        """v1.1.1 — start a recording session over BLE. Must be in SYNC state."""
        await self._raw_write(bytes([OP_START_RECORD, 0]))
        reply = await self._await_ctrl(OP_START_RECORD, timeout=5.0)
        if reply[1] != ST_OK:
            raise RuntimeError(
                f"START_RECORD failed: status={STATUS_NAMES.get(reply[1], reply[1])}")

    async def cmd_stop_record(self) -> None:
        """v1.1.1 — stop a BLE-initiated recording. Tap-initiated records
        can only be stopped by the wearer; this returns BUSY for those."""
        await self._raw_write(bytes([OP_STOP_RECORD, 0]))
        reply = await self._await_ctrl(OP_STOP_RECORD, timeout=5.0)
        if reply[1] != ST_OK:
            raise RuntimeError(
                f"STOP_RECORD failed: status={STATUS_NAMES.get(reply[1], reply[1])}")


async def scan_for_devices(timeout: float = 5.0) -> list[tuple[str, str]]:
    found: list[tuple[str, str]] = []
    devices = await BleakScanner.discover(timeout=timeout)
    for d in devices:
        if d.name and "SensaPulse" in d.name:
            found.append((d.address, d.name))
    return found


def device_label(info: dict) -> str:
    name = (info.get("name") or "").strip()
    if name:
        return name
    return f"device_{info.get('chip_id', 'unknown')}"


def existing_partial_size(path: Path) -> int:
    """For --resume: return current size on disk, or 0 if absent."""
    try:
        return path.stat().st_size
    except FileNotFoundError:
        return 0


async def transfer_session(sess: SyncSession, sid: int, total_hint: int,
                           device_dir: Path, resume: bool, no_ack: bool,
                           only_file_idx: int | None = None,
                           max_bytes: int = 0) -> bool:
    tmp = device_dir / f"SESSION_{sid:05d}.tmp"
    final = device_dir / f"SESSION_{sid:05d}"
    if final.exists():
        if not resume:
            log.info("session %d: already on disk → skipping (use --resume to redo)", sid)
            return True
        log.warning("session %d: final exists but device still has marker — re-ACK only", sid)
        if not no_ack:
            await sess.cmd_ack(sid)
        return True

    tmp.mkdir(parents=True, exist_ok=True)
    log.info("session %d (~%d B): downloading to %s", sid, total_hint, tmp)

    for fname, idx in zip(FILE_NAMES, range(len(FILE_NAMES))):
        if only_file_idx is not None and idx != only_file_idx:
            continue
        dest = tmp / fname
        offset = existing_partial_size(dest) if resume else 0
        if offset > 0:
            log.info("  %s: resuming from offset %d", fname, offset)
        written, total = await sess.cmd_read(sid, idx, dest, offset=offset,
                                              length=max_bytes)
        log.info("  %s: +%d byte (file total %d)", fname, written, offset + written)
        if total != offset + written:
            log.warning("  %s: device reported total=%d, on-disk=%d",
                        fname, total, offset + written)

    tmp.rename(final)
    if no_ack:
        log.info("session %d: --no-ack, device marker preserved", sid)
    else:
        await sess.cmd_ack(sid)
        log.info("session %d: ACK ok (marker removed on device)", sid)
    return True


async def sync_one_device(addr: str, output_root: Path, resume: bool,
                          no_ack: bool, only_session: int | None,
                          only_file_idx: int | None, max_bytes: int) -> int:
    async with BleakClient(addr) as client:
        sess = SyncSession(client)
        await sess.setup()

        info = await sess.fetch_info()
        log.info("device info: %s", info)
        state = info.get("state", "?")
        # Per #20, IDLE → SYNC happens on BLE connect, so "sync" here is the
        # expected post-connect state. RECORDING connect is rejected by the
        # firmware before we get here, but guard anyway.
        if state == "recording":
            log.error("device is recording — sync refused")
            return 1
        if state not in ("sync", "idle"):
            log.warning("unexpected device state=%s — proceeding cautiously", state)

        label = device_label(info)
        device_dir = output_root / label
        device_dir.mkdir(parents=True, exist_ok=True)
        log.info("output dir: %s", device_dir)

        sessions = await sess.cmd_list()
        log.info("LIST → %d unsynced session(s)", len(sessions))
        if only_session is not None:
            sessions = [s for s in sessions if s[0] == only_session]
            log.info("--only-session %d → %d match", only_session, len(sessions))
        if not sessions:
            return 0

        failures = 0
        for sid, total_hint in sessions:
            if not client.is_connected:
                log.error("link dropped — bailing out of remaining sessions")
                failures += 1
                break
            try:
                await transfer_session(sess, sid, total_hint, device_dir,
                                       resume=resume, no_ack=no_ack,
                                       only_file_idx=only_file_idx,
                                       max_bytes=max_bytes)
            except Exception as e:
                log.error("session %d failed: %r", sid, e)
                failures += 1
                if not client.is_connected:
                    log.error("link dropped — bailing out of remaining sessions")
                    break
        return 0 if failures == 0 else 2


def main() -> int:
    p = argparse.ArgumentParser(description="SensaPulse BLE sync")
    p.add_argument("--output", type=Path, default=Path.home() / "SensaScope_data",
                   help="root folder on PC for synced data")
    p.add_argument("--device", help="match device by advertised name (default: any)")
    p.add_argument("--list-devices", action="store_true",
                   help="scan and print SensaPulse devices, then exit")
    p.add_argument("--resume", action="store_true",
                   help="resume partial transfers from .tmp/ folders")
    p.add_argument("--no-ack", action="store_true",
                   help="debug: preserve .unsynced markers on device after transfer")
    p.add_argument("--only-session", type=int, default=None,
                   help="debug: sync only the given session id (skip others)")
    p.add_argument("--only-file-idx", type=int, default=None,
                   choices=[0, 1, 2],
                   help="debug: only read this file index (0=audio, 1=imu, 2=meta)")
    p.add_argument("--max-bytes", type=int, default=0,
                   help="debug: cap READ length per file (0 = to EOF)")
    p.add_argument("--scan-timeout", type=float, default=5.0,
                   help="seconds to scan for devices (default: 5)")
    p.add_argument("--start-record", action="store_true",
                   help="v1.1.1: send OP_START_RECORD then hold the link until killed "
                        "or --hold seconds elapse (disconnect = firmware auto-stop)")
    p.add_argument("--stop-record", action="store_true",
                   help="v1.1.1: send OP_STOP_RECORD then exit")
    p.add_argument("--hold", type=float, default=0.0,
                   help="seconds to hold the link after START_RECORD (0 = until killed)")
    p.add_argument("-v", "--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s %(levelname)s %(message)s",
    )

    async def run() -> int:
        if args.list_devices:
            devs = await scan_for_devices(args.scan_timeout)
            if not devs:
                print("(no SensaPulse devices found)")
            for addr, name in devs:
                print(f"{addr}\t{name}")
            return 0

        devs = await scan_for_devices(args.scan_timeout)
        if not devs:
            log.error("no SensaPulse devices found")
            return 1
        if args.device:
            devs = [(a, n) for a, n in devs if args.device in (n or "")]
            if not devs:
                log.error("no device matched name=%s", args.device)
                return 1

        if args.start_record or args.stop_record:
            if len(devs) != 1:
                log.error("expected exactly 1 device, found %d — use --device to filter",
                          len(devs))
                return 1
            addr, name = devs[0]
            action = "start" if args.start_record else "stop"
            log.info("controlling %s (%s) → %s_record", name, addr, action)
            async with BleakClient(addr) as client:
                sess = SyncSession(client)
                await sess.setup()
                info = await sess.fetch_info()
                log.info("device info: %s", info)
                state = info.get("state", "?")
                if action == "start":
                    if state == "recording":
                        log.error("device is already recording — refusing")
                        return 1
                    await sess.cmd_start_record()
                    log.info("START_RECORD ok — device should now be RECORDING (LED 2-pulse)")
                    hold = args.hold if args.hold > 0 else float("inf")
                    log.info("holding link for %s — Ctrl+C / kill PID %d to stop "
                             "(disconnect = firmware auto-stop)",
                             "until killed" if hold == float("inf") else f"{hold:.0f}s",
                             os.getpid())
                    try:
                        elapsed = 0.0
                        while elapsed < hold and client.is_connected:
                            await asyncio.sleep(5.0)
                            elapsed += 5.0
                            log.info("recording… +%.0fs (still connected)", elapsed)
                    except asyncio.CancelledError:
                        log.info("cancelled — sending STOP_RECORD before disconnect")
                        try:
                            await asyncio.wait_for(sess.cmd_stop_record(), timeout=3.0)
                        except Exception as e:
                            log.warning("STOP_RECORD failed (%r) — disconnect will auto-stop", e)
                        raise
                    if client.is_connected:
                        log.info("hold elapsed — sending STOP_RECORD")
                        try:
                            await sess.cmd_stop_record()
                        except Exception as e:
                            log.warning("STOP_RECORD failed (%r) — disconnect will auto-stop", e)
                    else:
                        log.warning("link dropped during hold — firmware should auto-stop")
                else:
                    if state != "recording":
                        log.warning("device state=%s, not 'recording' — sending STOP anyway",
                                    state)
                    await sess.cmd_stop_record()
                    log.info("STOP_RECORD ok — device returned to SYNC")
            return 0

        rc = 0
        for addr, name in devs:
            log.info("syncing %s (%s)", name, addr)
            try:
                code = await sync_one_device(addr, args.output, args.resume,
                                             args.no_ack, args.only_session,
                                             args.only_file_idx, args.max_bytes)
            except Exception as e:
                log.error("device %s failed: %r", name, e)
                code = 1
            rc = code if code > rc else rc
        return rc

    return asyncio.run(run())


if __name__ == "__main__":
    sys.exit(main())
