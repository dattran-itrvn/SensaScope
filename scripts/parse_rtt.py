#!/usr/bin/env python3
"""Parse a SensaPulse RTT log and extract metrics.

Usage:
    python3 scripts/parse_rtt.py LOG_FILE [LOG_FILE ...]

Emits JSON to stdout. Designed to be piped into run_loop.py.
"""
from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass, field, asdict
from pathlib import Path


@dataclass
class Metrics:
    log_file: str
    booted: bool = False
    build_stamp: str | None = None
    battery_mv: int | None = None
    battery_state: str | None = None
    sd_mb: int | None = None
    sd_mounted: bool = False
    pdm_smoke_peak_l: int | None = None
    pdm_smoke_peak_r: int | None = None
    pdm_smoke_mean_l: int | None = None
    pdm_smoke_mean_r: int | None = None
    pdm_smoke_pairs: int | None = None
    recorder_starts: int = 0
    recorder_stops: int = 0
    recorder_bytes_total: int = 0
    recorder_kbps_avg: float | None = None
    double_taps: int = 0
    ble_connects: int = 0
    ble_disconnects: int = 0
    errors: list[str] = field(default_factory=list)
    warnings: list[str] = field(default_factory=list)
    last_uptime_ms: int | None = None


# (Pattern → callable that mutates the Metrics object)
PATTERNS: list[tuple[re.Pattern, str]] = [
    (re.compile(r"\*\*\* Booting nRF Connect SDK"), "booted"),
    (re.compile(r"<inf> main: Build:\s*(.+)$"), "build_stamp"),
    (re.compile(r"<inf> main: Battery:\s*(\d+)\s*mV\s*\((\w+)\)"), "battery"),
    (re.compile(r"<inf> sd_log: SD:.*=\s*(\d+)\s*MB"), "sd_mb"),
    (re.compile(r"<inf> sd_log: SD mounted"), "sd_mounted"),
    (re.compile(r"<inf> main: smoke ch0 \(body, L\)\s*peak=(-?\d+)\s*mean=(-?\d+)"), "smoke_l"),
    (re.compile(r"<inf> main: smoke ch1 \(ambient, R\)\s*peak=(-?\d+)\s*mean=(-?\d+)"), "smoke_r"),
    (re.compile(r"<inf> audio: PDM stop:.*?(\d+)\s*sample pairs"), "smoke_pairs"),
    (re.compile(r"<inf> main: >>> DOUBLE TAP — starting"), "rec_start"),
    (re.compile(r"<inf> main: >>> DOUBLE TAP — stopping"), "rec_stop"),
    (re.compile(r"<inf> audio: recorder: stop,\s*(\d+)\s*B written in\s*(\d+)\s*ms"), "rec_done"),
    (re.compile(r"<inf> main: \*\*\* DOUBLE TAP #(\d+)"), "tap"),
    (re.compile(r"<inf> main: BLE connected"), "ble_conn"),
    (re.compile(r"<inf> main: BLE disconnected"), "ble_disc"),
    (re.compile(r"<err>"), "error"),
    (re.compile(r"<wrn>"), "warning"),
    (re.compile(r"\[(\d+):(\d+):(\d+)\.(\d+)"), "uptime"),
]


def parse_one(path: Path) -> Metrics:
    m = Metrics(log_file=str(path))
    rec_kbps_samples: list[float] = []

    with path.open(encoding="utf-8", errors="replace") as f:
        for line in f:
            for pat, kind in PATTERNS:
                match = pat.search(line)
                if not match:
                    continue
                if kind == "booted":
                    m.booted = True
                elif kind == "build_stamp":
                    m.build_stamp = match.group(1).strip()
                elif kind == "battery":
                    m.battery_mv = int(match.group(1))
                    m.battery_state = match.group(2)
                elif kind == "sd_mb":
                    m.sd_mb = int(match.group(1))
                elif kind == "sd_mounted":
                    m.sd_mounted = True
                elif kind == "smoke_l":
                    m.pdm_smoke_peak_l = int(match.group(1))
                    m.pdm_smoke_mean_l = int(match.group(2))
                elif kind == "smoke_r":
                    m.pdm_smoke_peak_r = int(match.group(1))
                    m.pdm_smoke_mean_r = int(match.group(2))
                elif kind == "smoke_pairs":
                    m.pdm_smoke_pairs = int(match.group(1))
                elif kind == "rec_start":
                    m.recorder_starts += 1
                elif kind == "rec_stop":
                    m.recorder_stops += 1
                elif kind == "rec_done":
                    bytes_ = int(match.group(1))
                    ms = int(match.group(2))
                    m.recorder_bytes_total += bytes_
                    if ms > 0:
                        rec_kbps_samples.append(bytes_ * 1000 / ms / 1024)
                elif kind == "tap":
                    m.double_taps = max(m.double_taps, int(match.group(1)))
                elif kind == "ble_conn":
                    m.ble_connects += 1
                elif kind == "ble_disc":
                    m.ble_disconnects += 1
                elif kind == "error":
                    m.errors.append(line.rstrip())
                elif kind == "warning":
                    m.warnings.append(line.rstrip())
                elif kind == "uptime":
                    h, mm, s, frac = match.groups()
                    m.last_uptime_ms = (
                        int(h) * 3_600_000
                        + int(mm) * 60_000
                        + int(s) * 1_000
                        + int(frac[:3])  # first 3 digits = ms
                    )
                break  # only first matching pattern per line

    if rec_kbps_samples:
        m.recorder_kbps_avg = sum(rec_kbps_samples) / len(rec_kbps_samples)
    return m


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    out = [asdict(parse_one(Path(p))) for p in sys.argv[1:]]
    json.dump(out if len(out) > 1 else out[0], sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
