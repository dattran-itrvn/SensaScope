#!/usr/bin/env python3
"""Parse a #23 SD reliability test RTT log and emit a PASS / FAIL verdict.

Usage:
    python3 scripts/parse_sd_reliability.py LOG_FILE

Designed to be called by the playbooks/sd_reliability_test.md driver and
optionally by run_loop.py.

Verdict criteria (all must hold for PASS):
  * Build banner present                            → chip booted
  * "session_start: SESSION_NNNNN" appeared         → tap-start fired
  * "audio: recorder: streaming → ..." appeared     → audio writer entered loop
  * "imu_sampler: streaming @ ..."   appeared       → imu writer entered loop
  * ≥ 5  audio "(sync OK)" heartbeats               → ≥ 25 s of audio committed
  * ≥ 30 imu  "(sync OK)" flushes                   → ≥ 30 s of imu committed
  * Exactly one ".unsynced marker set" per session  → atomic-marker logic OK
  * No <err> from audio/imu_sampler/session/fs/fatfs

Anything else is a graded FAIL with the most informative reason printed first.
"""
from __future__ import annotations

import re
import sys
from collections import Counter
from pathlib import Path


PATTERNS = {
    "boot":        re.compile(r"\*\*\* Booting nRF Connect SDK"),
    "build":       re.compile(r"<inf> main: Build:\s*(.+)$"),
    "sess_start":  re.compile(r"<inf> session: session_start: SESSION_(\d{5})"),
    "sess_stop":   re.compile(r"<inf> session: session_stop: closed SESSION_(\d{5})"),
    "rec_stream":  re.compile(r"<inf> audio: recorder: streaming"),
    "rec_sync":    re.compile(r"<inf> audio: recorder: (\d+) blocks, (\d+) B \(sync OK\)"),
    "rec_stop":    re.compile(r"<inf> audio: recorder: stop,\s*(\d+) B"),
    "rec_err":     re.compile(r"<err> audio: recorder: (.+)$"),
    "samp_stream": re.compile(r"<inf> imu_sampler: sampler: streaming @"),
    "samp_sync":   re.compile(r"<inf> imu_sampler: sampler: flushed (\d+) samples \(total=(\d+),\s*sync OK\)"),
    "samp_stop":   re.compile(r"<inf> imu_sampler: sampler: stop, (\d+) samples"),
    "samp_err":    re.compile(r"<err> imu_sampler:"),
    "marker_set":  re.compile(r"<inf> session: monitor: SESSION_(\d{5}) \.unsynced marker set \(audio=(\d+) B\)"),
    "monitor_hb":  re.compile(r"<inf> session: monitor: SESSION_(\d{5}) tick=\d+, audio=(\d+) B, imu=(\d+) samples"),
    "monitor_die": re.compile(r"<err> session: monitor: writer died"),
    "fs_err":      re.compile(r"<err> (fs|fatfs):"),
    "any_err":     re.compile(r"<err>\s+(\w+):"),
    "tap":         re.compile(r"<inf> main: >>> DOUBLE TAP — (\w+)"),
}


def parse(log_path: Path) -> dict:
    text = log_path.read_text(errors="replace")
    counts: Counter = Counter()
    matches: dict = {k: [] for k in PATTERNS}

    for line in text.splitlines():
        for name, rx in PATTERNS.items():
            m = rx.search(line)
            if m:
                counts[name] += 1
                matches[name].append(m.groups() if m.groups() else line.strip())

    return {"counts": counts, "matches": matches, "raw_lines": text.count("\n")}


def fmt_count(name: str, n: int) -> str:
    return f"  {name:<14} ×{n}"


def grade(parsed: dict) -> tuple[str, list[str]]:
    c = parsed["counts"]
    m = parsed["matches"]
    reasons: list[str] = []

    if not c["boot"]:
        return "FAIL", ["Chip never booted — no `*** Booting` banner. Check J-Link halt state."]

    if not c["sess_start"]:
        return "FAIL", ["No `session_start` logged — tap-start was never recognised. "
                        "Either user didn't double-tap, or #22 tap threshold is too high."]

    if not c["rec_stream"]:
        return "FAIL", ["Audio writer never entered streaming loop. "
                        "dmic_configure or fs_open(audio.wav) failed."]
    if not c["samp_stream"]:
        return "FAIL", ["IMU sampler never entered loop — fs_open(imu.csv) failed."]

    n_rec_sync = c["rec_sync"]
    n_samp_sync = c["samp_sync"]

    if n_rec_sync == 0:
        # Look at the last error before any rec_err line
        last_err = m["rec_err"][-1] if m["rec_err"] else "(none in log)"
        reasons.append(f"Audio writer died before first sync (no `(sync OK)` heartbeat). "
                       f"Last audio err: {last_err}")
    elif n_rec_sync < 5:
        reasons.append(f"Only {n_rec_sync} audio sync heartbeats (expected ≥5 for 60 s). "
                       f"Writer may have died mid-recording.")

    if n_samp_sync == 0:
        last_err = m["samp_err"][-1] if m["samp_err"] else "(none in log)"
        reasons.append(f"IMU sampler died before first flush (no `(sync OK)`). "
                       f"Last imu err: {last_err}")
    elif n_samp_sync < 30:
        reasons.append(f"Only {n_samp_sync} imu sync heartbeats (expected ≥30 for 60 s).")

    if c["marker_set"] == 0 and n_rec_sync > 0:
        reasons.append("Audio synced but `.unsynced` marker was never touched — "
                       "atomic-marker logic regression in session.c monitor.")
    if c["marker_set"] > c["sess_start"]:
        reasons.append(f"Marker set {c['marker_set']} times for {c['sess_start']} sessions — "
                       f"should be at most one per session.")

    if c["monitor_die"]:
        reasons.append(f"Monitor watchdog reported writer death {c['monitor_die']} time(s).")

    if c["fs_err"]:
        reasons.append(f"FATFS error(s): {c['fs_err']} hits.")

    if c["any_err"] and not c["monitor_die"] and not c["fs_err"] \
            and not c["rec_err"] and not c["samp_err"]:
        # Some other module errored — call it out
        reasons.append(f"Errors in other modules: {c['any_err']} `<err>` line(s) "
                       f"not from audio/imu/session/fs.")

    if reasons:
        return "FAIL", reasons
    return "PASS", []


def report(log_path: Path) -> int:
    parsed = parse(log_path)
    c = parsed["counts"]
    verdict, reasons = grade(parsed)

    print(f"# SD reliability test verdict — {log_path.name}")
    print()
    print(f"**Result: {verdict}**")
    print()
    if reasons:
        print("## Reasons")
        for r in reasons:
            print(f"- {r}")
        print()

    print("## Counts")
    for name in ["boot", "sess_start", "sess_stop", "rec_stream", "rec_sync",
                 "rec_stop", "rec_err", "samp_stream", "samp_sync", "samp_stop",
                 "samp_err", "marker_set", "monitor_hb", "monitor_die",
                 "fs_err", "tap"]:
        print(fmt_count(name, c.get(name, 0)))

    if c["rec_sync"]:
        last_blocks, last_bytes = parsed["matches"]["rec_sync"][-1]
        print()
        print(f"## Audio progress (last sync log)")
        print(f"  blocks: {last_blocks}")
        print(f"  bytes:  {last_bytes}  (~{int(last_bytes)/64000:.1f} s)")
    if c["samp_sync"]:
        last_n, last_total = parsed["matches"]["samp_sync"][-1]
        print()
        print(f"## IMU progress (last flush log)")
        print(f"  total samples: {last_total}  (~{int(last_total)/52:.1f} s @ 52 Hz)")
    if c["marker_set"]:
        for sess, audio_b in parsed["matches"]["marker_set"]:
            print()
            print(f"## .unsynced marker — SESSION_{sess}")
            print(f"  set after {audio_b} B audio")

    print()
    print(f"_log lines parsed: {parsed['raw_lines']}_")
    return 0 if verdict == "PASS" else 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        sys.exit(2)
    sys.exit(report(Path(sys.argv[1])))
