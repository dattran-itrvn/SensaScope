#!/usr/bin/env python3
"""Parse SD-stress firmware RTT log and emit a verdict.

Usage:
    python3 scripts/parse_sd_stress.py LOG_FILE [LOG_FILE ...]

If multiple logs are passed (e.g. from N iterations of the stress firmware),
prints per-iteration verdicts and an aggregate at the end.

Emits JSON-readable summary and exits 0 on PASS, 1 on any FAIL.
"""
from __future__ import annotations

import json
import re
import sys
from pathlib import Path


SUMMARY_RE = re.compile(r"STRESS_SUMMARY:\s*(\{.+\})\s*$")
ERR_RE     = re.compile(r"<err>\s+(\w+):\s*(.+)$")
BOOT_RE    = re.compile(r"\*\*\* Booting nRF Connect SDK")
BUILD_RE   = re.compile(r"<inf> stress: Build:\s*(.+)$")
PHASE_RE   = re.compile(r"\[(\w+)\] writes=(\d+) errors=(\d+) total=(\d+) B "
                        r"avg=(\d+) us max=(\d+) us throughput=(\d+) kB/s")


def parse_one(log_path: Path) -> dict:
    text = log_path.read_text(errors="replace")
    summary = None
    booted = False
    build = None
    errs: list[str] = []
    phases: dict[str, dict] = {}

    for line in text.splitlines():
        if BOOT_RE.search(line):
            booted = True
            continue
        m = BUILD_RE.search(line)
        if m:
            build = m.group(1)
            continue
        m = SUMMARY_RE.search(line)
        if m:
            try:
                summary = json.loads(m.group(1))
            except json.JSONDecodeError as e:
                errs.append(f"summary JSON parse error: {e}")
            continue
        m = PHASE_RE.search(line)
        if m:
            phases[m.group(1)] = {
                "writes":     int(m.group(2)),
                "errors":     int(m.group(3)),
                "bytes":      int(m.group(4)),
                "avg_us":     int(m.group(5)),
                "max_us":     int(m.group(6)),
                "kbps":       int(m.group(7)),
            }
            continue
        m = ERR_RE.search(line)
        if m:
            errs.append(f"{m.group(1)}: {m.group(2)}")

    return {
        "log_path": str(log_path),
        "booted":   booted,
        "build":    build,
        "summary":  summary,
        "phases":   phases,
        "errors":   errs,
    }


def grade(rec: dict) -> tuple[str, list[str]]:
    reasons: list[str] = []
    if not rec["booted"]:
        return "FAIL", ["Chip never booted — no boot banner."]
    if rec["summary"] is None:
        # Did some phases run?
        if rec["phases"]:
            ran = sorted(rec["phases"].keys())
            return "FAIL", [
                f"Stress aborted before final summary. "
                f"Last phases logged: {', '.join(ran)}."
            ]
        return "FAIL", ["Stress aborted with no phase summary — sdlog_init likely failed."]

    s = rec["summary"]
    if s["errors"] > 0:
        reasons.append(f"{s['errors']} write error(s) across {s['writes']} writes "
                       f"({s['errors']/s['writes']*100:.2f}%).")
        for name, p in s.get("phases", {}).items():
            if "e" in p and p["e"] > 0:
                reasons.append(f"  · {name}: {p['e']} err / {p['w']} writes "
                               f"(first code {p['first_err']})")
    if s["max_latency_us"] > 500_000:
        reasons.append(f"Max write latency {s['max_latency_us']/1000:.1f} ms — "
                       f"FATFS stalls long enough to break PDM in production.")

    # ---- Phase 5 / 6 (production data path through sd_writer #25) ----
    phases = s.get("phases", {})
    p5 = phases.get("pdm_only")
    if p5 is not None:
        if p5.get("writer_failed"):
            reasons.append(f"phase 5 (pdm_only): sd_writer reported failed.")
        elif p5.get("audio_b", 0) < int(p5.get("audio_expected_b", 0) * 0.95):
            reasons.append(
                f"phase 5 (pdm_only): audio bytes {p5['audio_b']:,} < 95 % of "
                f"expected {p5['audio_expected_b']:,} — buffer underrun, FIFO "
                f"backpressure, or writer stalled before stop "
                f"(audio_dropped={p5.get('audio_dropped',0)}).")
    else:
        reasons.append("phase 5 (pdm_only) missing from summary — "
                       "PDM data path never tested.")

    p6 = phases.get("pdm_imu")
    if p6 is not None:
        if p6.get("writer_failed"):
            reasons.append(f"phase 6 (pdm_imu): sd_writer reported failed "
                           f"(dropped: audio={p6.get('audio_dropped',0)}, "
                           f"imu={p6.get('imu_dropped',0)}).")
        if p6.get("audio_b", 0) < int(p6.get("audio_expected_b", 0) * 0.95):
            reasons.append(
                f"phase 6 (pdm_imu): audio bytes {p6['audio_b']:,} < 95 % "
                f"of expected {p6['audio_expected_b']:,} "
                f"(audio_dropped={p6.get('audio_dropped',0)}).")
        if p6.get("imu_samples", 0) < int(p6.get("imu_expected", 0) * 0.90):
            reasons.append(
                f"phase 6 (pdm_imu): IMU samples {p6['imu_samples']} < 90 % "
                f"of expected {p6['imu_expected']} "
                f"(imu_dropped={p6.get('imu_dropped',0)}).")
    else:
        reasons.append("phase 6 (pdm_imu) missing from summary — "
                       "production data path never tested.")

    if reasons:
        return "FAIL", reasons
    return "PASS", []


def report_one(rec: dict) -> tuple[str, list[str]]:
    verdict, reasons = grade(rec)
    print(f"## {rec['log_path']}")
    print(f"**Result: {verdict}**")
    if rec["build"]:
        print(f"_build: {rec['build']}_")
    print()
    if rec["summary"]:
        s = rec["summary"]
        print(f"- writes: {s['writes']}, errors: {s['errors']}")
        print(f"- total bytes: {s['bytes']:,}")
        print(f"- max latency: {s['max_latency_us']/1000:.1f} ms")
        print()
        print("| phase | writes | errors | bytes | first_err |")
        print("|---|---|---|---|---|")
        for name, p in s.get("phases", {}).items():
            if "w" in p:  # synthetic phases 1-4
                print(f"| {name} | {p['w']} | {p['e']} | {p['b']:,} | {p['first_err']} |")
        # Production data-path phases (5 / 6) — different shape
        p5 = s.get("phases", {}).get("pdm_only")
        if p5:
            pct = 100 * p5.get("audio_b", 0) / max(p5.get("audio_expected_b", 1), 1)
            print(f"| pdm_only (sd_writer + audio) | audio={p5.get('audio_b',0):,} B "
                  f"({pct:.1f}% of expected) | "
                  f"writer_failed={p5.get('writer_failed',0)} "
                  f"dropped={p5.get('audio_dropped',0)} | — | — |")
        p6 = s.get("phases", {}).get("pdm_imu")
        if p6:
            apct = 100 * p6.get("audio_b", 0) / max(p6.get("audio_expected_b", 1), 1)
            ipct = 100 * p6.get("imu_samples", 0) / max(p6.get("imu_expected", 1), 1)
            print(f"| pdm_imu (sd_writer + audio + imu) | "
                  f"audio={p6.get('audio_b',0):,} B ({apct:.1f}%) "
                  f"imu={p6.get('imu_samples',0)} ({ipct:.1f}%) | "
                  f"writer_failed={p6.get('writer_failed',0)} "
                  f"dropped a={p6.get('audio_dropped',0)} "
                  f"i={p6.get('imu_dropped',0)} | — | — |")
    if reasons:
        print()
        print("### reasons")
        for r in reasons:
            print(f"- {r}")
    print()
    return verdict, reasons


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__, file=sys.stderr)
        return 2
    paths = [Path(p) for p in sys.argv[1:]]
    overall_pass = True
    aggregate = {"runs": 0, "pass": 0, "fail": 0, "total_writes": 0, "total_errors": 0}

    for p in paths:
        rec = parse_one(p)
        v, _ = report_one(rec)
        aggregate["runs"] += 1
        if v == "PASS":
            aggregate["pass"] += 1
        else:
            aggregate["fail"] += 1
            overall_pass = False
        if rec["summary"]:
            aggregate["total_writes"] += rec["summary"]["writes"]
            aggregate["total_errors"] += rec["summary"]["errors"]

    if len(paths) > 1:
        print("---")
        print("# Aggregate")
        print(f"- runs: {aggregate['runs']}")
        print(f"- pass: {aggregate['pass']}, fail: {aggregate['fail']}")
        if aggregate["total_writes"]:
            rate = aggregate["total_errors"] / aggregate["total_writes"] * 100
            print(f"- total writes across runs: {aggregate['total_writes']:,}")
            print(f"- total errors: {aggregate['total_errors']} "
                  f"({rate:.3f}%)")

    return 0 if overall_pass else 1


if __name__ == "__main__":
    sys.exit(main())
