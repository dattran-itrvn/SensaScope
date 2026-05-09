#!/usr/bin/env python3
"""SensaPulse — overnight stability test analyzer.

Walks an SD card root, validates every SESSION_NNNNN/ folder against the
v1.0 recording engine's expected output, optionally cross-references with
an RTT log, and writes a markdown report.

Usage:
    python -m tools.analyze_overnight <SD_root> [--rtt LOG] [--out REPORT.md]

Exit code is 0 if all sessions pass, 1 otherwise.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from dataclasses import dataclass, field
from pathlib import Path

# Reuse the per-session loader.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
from tools.load_session import load_session  # noqa: E402


# Pass thresholds (matches playbooks/v1_overnight_stability.md).
EXPECTED_DURATION_S = 600.0
DURATION_TOL_PCT   = 5.0           # ±5 %, last session may be lower
IMU_RATE_TARGET_HZ = 52.0
IMU_RATE_TOL_HZ    = 0.5
DRIFT_MAX_MS       = 200
NUL_MAX            = 0


@dataclass
class SessionReport:
    sid: int
    path: Path
    duration_s: float | None = None
    audio_bytes: int | None = None
    audio_nuls: int = 0
    imu_rows: int | None = None
    imu_rate_hz: float | None = None
    imu_max_gap_us: int | None = None
    drift_ms: float | None = None
    batt_mv_start: int | None = None
    is_last: bool = False
    failures: list[str] = field(default_factory=list)

    @property
    def passed(self) -> bool:
        return not self.failures


@dataclass
class RTTReport:
    crc_errors: int = 0
    recorder_errors: int = 0
    sampler_errors: int = 0
    watchdog_trips: int = 0
    rotation_events: int = 0
    other_errors: int = 0


def analyze_session(folder: Path, *, is_last: bool) -> SessionReport:
    sid = int(folder.name.removeprefix("SESSION_"))
    rep = SessionReport(sid=sid, path=folder, is_last=is_last)

    try:
        s = load_session(folder)
    except Exception as e:
        rep.failures.append(f"load_session crashed: {e}")
        return rep

    audio = s["audio"]
    imu = s["imu"]
    meta = s["meta"]

    # Audio metrics
    rep.audio_bytes = audio.nbytes
    rep.duration_s = audio.shape[0] / s["fs_audio"]
    # Count zeros (rough corruption indicator — the PDM stream rarely produces clean 0)
    import numpy as np
    rep.audio_nuls = int((audio == 0).sum())

    # IMU metrics
    rep.imu_rows = len(imu)
    if rep.imu_rows >= 2:
        span_us = int(imu["t_us"].iloc[-1]) - int(imu["t_us"].iloc[0])
        rep.imu_rate_hz = (rep.imu_rows - 1) / (span_us / 1e6)
        deltas = imu["t_us"].diff().dropna().astype(int)
        rep.imu_max_gap_us = int(deltas.max())
        # Drift: audio duration vs IMU span, both should match the user's stop-time.
        rep.drift_ms = (rep.duration_s - span_us / 1e6) * 1000

    rep.batt_mv_start = meta.get("batt_mv_start")

    # Apply thresholds
    if rep.duration_s is None:
        rep.failures.append("audio duration unreadable")
    elif not is_last:
        lo = EXPECTED_DURATION_S * (1 - DURATION_TOL_PCT / 100)
        hi = EXPECTED_DURATION_S * (1 + DURATION_TOL_PCT / 100)
        if not (lo <= rep.duration_s <= hi):
            rep.failures.append(
                f"duration {rep.duration_s:.1f}s outside [{lo:.0f}, {hi:.0f}]s"
            )

    if rep.imu_rate_hz is not None:
        if abs(rep.imu_rate_hz - IMU_RATE_TARGET_HZ) > IMU_RATE_TOL_HZ:
            rep.failures.append(
                f"IMU rate {rep.imu_rate_hz:.2f} Hz outside "
                f"{IMU_RATE_TARGET_HZ:.1f}±{IMU_RATE_TOL_HZ}"
            )

    if rep.drift_ms is not None and abs(rep.drift_ms) > DRIFT_MAX_MS:
        rep.failures.append(f"drift {rep.drift_ms:+.0f} ms exceeds ±{DRIFT_MAX_MS}")

    if rep.audio_nuls > NUL_MAX:
        rep.failures.append(
            f"{rep.audio_nuls} zero samples in audio (corruption?)"
        )

    # CSV NUL check (ASCII file, any literal NUL is corruption)
    csv_path = folder / "imu.csv"
    if csv_path.exists():
        if b"\x00" in csv_path.read_bytes():
            rep.failures.append("imu.csv contains NUL bytes")

    return rep


def analyze_rtt(log_path: Path) -> RTTReport:
    """Quick scan of the RTT log for known error patterns."""
    rep = RTTReport()
    text = log_path.read_text(errors="replace")
    # Strip ANSI color codes.
    text = re.sub(r"\x1b\[[0-9;]*m", "", text)
    text = text.replace("\x00", "")

    rep.crc_errors      = len(re.findall(r"<err> sdhc_spi: Bad data CRC", text))
    rep.recorder_errors = len(re.findall(r"<err> audio: recorder:", text))
    rep.sampler_errors  = len(re.findall(r"<err> imu_sampler:", text))
    rep.watchdog_trips  = len(re.findall(r"monitor: writer .* stopped", text))
    rep.rotation_events = len(re.findall(r"recorder: rotated", text))
    # All other <err> lines, minus the categorized ones above.
    all_errs = len(re.findall(r"<err>", text))
    rep.other_errors = max(0, all_errs - rep.crc_errors - rep.recorder_errors
                           - rep.sampler_errors - rep.watchdog_trips)
    return rep


def render_report(sessions: list[SessionReport],
                  rtt: RTTReport | None,
                  sd_root: Path) -> str:
    n_total = len(sessions)
    n_pass  = sum(1 for s in sessions if s.passed)
    n_fail  = n_total - n_pass
    overall = "✅ PASS" if n_fail == 0 else f"❌ FAIL ({n_fail}/{n_total} sessions failed)"

    lines: list[str] = []
    lines.append(f"# SensaPulse overnight stability — {sd_root}")
    lines.append("")
    lines.append(f"**Overall: {overall}**")
    lines.append("")

    # Aggregate
    durations = [s.duration_s for s in sessions if s.duration_s is not None]
    rates     = [s.imu_rate_hz for s in sessions if s.imu_rate_hz is not None]
    drifts    = [s.drift_ms for s in sessions if s.drift_ms is not None]
    total_audio_mb = sum((s.audio_bytes or 0) for s in sessions) / (1024 * 1024)

    lines.append("## Aggregate")
    lines.append("")
    lines.append("| Metric | Value |")
    lines.append("|---|---|")
    lines.append(f"| Sessions found | {n_total} |")
    lines.append(f"| Sessions passed | {n_pass} |")
    if durations:
        lines.append(
            f"| Audio duration   | min {min(durations):.1f}s, "
            f"max {max(durations):.1f}s, mean {sum(durations)/len(durations):.1f}s |"
        )
    if rates:
        lines.append(
            f"| IMU rate (Hz)    | min {min(rates):.2f}, "
            f"max {max(rates):.2f}, mean {sum(rates)/len(rates):.2f} |"
        )
    if drifts:
        lines.append(
            f"| Drift (ms)       | min {min(drifts):+.0f}, "
            f"max {max(drifts):+.0f}, mean {sum(drifts)/len(drifts):+.0f} |"
        )
    lines.append(f"| Audio total      | {total_audio_mb:.1f} MB |")
    if sessions:
        lines.append(f"| Battery at start | {sessions[0].batt_mv_start} mV |")
    lines.append("")

    # RTT
    if rtt is not None:
        lines.append("## RTT log")
        lines.append("")
        lines.append("| Pattern | Count |")
        lines.append("|---|---|")
        lines.append(f"| `<err> sdhc_spi: Bad data CRC` | **{rtt.crc_errors}** |")
        lines.append(f"| `<err> audio: recorder:`        | {rtt.recorder_errors} |")
        lines.append(f"| `<err> imu_sampler:`            | {rtt.sampler_errors} |")
        lines.append(f"| watchdog trips                  | {rtt.watchdog_trips} |")
        lines.append(f"| `recorder: rotated` events      | {rtt.rotation_events} |")
        lines.append(f"| other `<err>` lines             | {rtt.other_errors} |")
        lines.append("")

    # Per-session table
    lines.append("## Per-session detail")
    lines.append("")
    lines.append("| sid | dur (s) | imu rate | drift (ms) | audio NUL | csv NUL | result |")
    lines.append("|---|---|---|---|---|---|---|")
    def _f(val, spec, na="N/A"):
        return format(val, spec) if val is not None else na

    for s in sessions:
        result = "✅" if s.passed else "❌ " + "; ".join(s.failures)
        csv_path = s.path / "imu.csv"
        csv_nul = csv_path.read_bytes().count(b"\x00") if csv_path.exists() else 0
        lines.append(
            f"| {s.sid:05d} "
            f"| {_f(s.duration_s, '.1f')} "
            f"| {_f(s.imu_rate_hz, '.2f')} "
            f"| {_f(s.drift_ms, '+.0f')} "
            f"| {s.audio_nuls} "
            f"| {csv_nul} "
            f"| {result} |"
        )
    return "\n".join(lines) + "\n"


def main() -> int:
    p = argparse.ArgumentParser(description="SensaPulse overnight analyzer")
    p.add_argument("sd_root", type=Path, help="SD card mount point or copy")
    p.add_argument("--rtt", type=Path, help="optional RTT log to cross-reference")
    p.add_argument("--out", type=Path, default=Path("overnight_report.md"))
    args = p.parse_args()

    folders = sorted(args.sd_root.glob("SESSION_*"))
    if not folders:
        print(f"no SESSION_* folders under {args.sd_root}", file=sys.stderr)
        return 2

    print(f"found {len(folders)} session(s)")
    sessions = []
    for i, folder in enumerate(folders):
        is_last = (i == len(folders) - 1)
        rep = analyze_session(folder, is_last=is_last)
        status = "OK" if rep.passed else "FAIL"

        def _fmt(val, spec, na="N/A"):
            return format(val, spec) if val is not None else na

        print(
            f"  {folder.name}: "
            f"{_fmt(rep.duration_s, '.1f')}s, "
            f"{_fmt(rep.imu_rate_hz, '.2f')} Hz, "
            f"drift {_fmt(rep.drift_ms, '+.0f')}ms — {status}"
        )
        if not rep.passed:
            for f in rep.failures:
                print(f"    - {f}")
        sessions.append(rep)

    rtt = analyze_rtt(args.rtt) if args.rtt and args.rtt.exists() else None

    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(render_report(sessions, rtt, args.sd_root))
    print(f"\nreport → {args.out}")

    return 0 if all(s.passed for s in sessions) else 1


if __name__ == "__main__":
    sys.exit(main())
