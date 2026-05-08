#!/usr/bin/env python3
"""SensaPulse overnight loop driver.

Reads a playbook (JSON), runs N iterations of (build → flash → capture RTT
→ parse → check), writes per-iteration logs + a final report.

Playbook schema (minimal, see playbooks/*.json for examples):
{
    "name": "long_recording_8h",
    "iterations": 1,
    "per_iter_seconds": 28800,
    "pristine_first": true,
    "stop_on_error": true,
    "expect": {
        "booted": true,
        "battery_mv_min": 3500,
        "errors_max": 0
    }
}

Usage:
    python3 scripts/run_loop.py playbooks/long_recording_8h.json
"""
from __future__ import annotations

import json
import subprocess
import sys
import time
from datetime import datetime
from pathlib import Path

ROOT = Path("/Users/trandat/Project/SensaScope")
SCRIPTS = ROOT / "scripts"
RUNS = ROOT / "runs"


def shell(cmd: list[str], cwd: Path | None = None, check: bool = True) -> int:
    print(f"  $ {' '.join(cmd)}", flush=True)
    res = subprocess.run(cmd, cwd=cwd)
    if check and res.returncode != 0:
        raise RuntimeError(f"command failed: {' '.join(cmd)} (rc={res.returncode})")
    return res.returncode


def parse_log(log_path: Path) -> dict:
    out = subprocess.check_output(
        ["python3", str(SCRIPTS / "parse_rtt.py"), str(log_path)],
        text=True,
    )
    return json.loads(out)


def check_expectations(metrics: dict, expect: dict) -> tuple[bool, list[str]]:
    failures = []
    if expect.get("booted") is True and not metrics.get("booted"):
        failures.append("did not boot")
    if "battery_mv_min" in expect:
        bm = metrics.get("battery_mv")
        if bm is None or bm < expect["battery_mv_min"]:
            failures.append(f"battery_mv={bm} below {expect['battery_mv_min']}")
    if "errors_max" in expect:
        n = len(metrics.get("errors", []))
        if n > expect["errors_max"]:
            failures.append(f"errors={n} exceeds {expect['errors_max']}")
    if "recorder_bytes_min" in expect:
        rb = metrics.get("recorder_bytes_total", 0)
        if rb < expect["recorder_bytes_min"]:
            failures.append(f"recorder_bytes={rb} below {expect['recorder_bytes_min']}")
    return (not failures), failures


def main() -> int:
    if len(sys.argv) != 2:
        print(__doc__, file=sys.stderr)
        return 2

    playbook = json.loads(Path(sys.argv[1]).read_text())
    name = playbook["name"]
    iters = int(playbook.get("iterations", 1))
    duration = int(playbook.get("per_iter_seconds", 60))
    pristine = bool(playbook.get("pristine_first", False))
    stop_on_error = bool(playbook.get("stop_on_error", True))
    expect = playbook.get("expect", {})

    run_id = datetime.now().strftime("%Y%m%d_%H%M%S") + "_" + name
    run_dir = RUNS / run_id
    run_dir.mkdir(parents=True, exist_ok=True)
    print(f"[run_loop] starting {run_id} → {run_dir}")

    results = []
    failed_at = None

    for i in range(iters):
        print(f"\n[run_loop] iteration {i + 1}/{iters}")
        try:
            args = [str(SCRIPTS / "build_flash.sh")]
            if pristine and i == 0:
                args.append("--pristine")
            shell(args)

            log_path = run_dir / f"iter_{i:03d}.rtt.log"
            shell(
                [
                    "bash",
                    str(SCRIPTS / "rtt_capture.sh"),
                    str(duration),
                    str(log_path),
                ]
            )
            metrics = parse_log(log_path)
            ok, failures = check_expectations(metrics, expect)
            metrics["_iteration"] = i
            metrics["_passed"] = ok
            metrics["_failures"] = failures
            results.append(metrics)
            print(f"  → passed={ok}  taps={metrics['double_taps']}  "
                  f"rec_bytes={metrics['recorder_bytes_total']}  "
                  f"errors={len(metrics['errors'])}")
            if not ok and stop_on_error:
                failed_at = i
                break
        except Exception as e:
            print(f"  ! iteration crashed: {e}")
            results.append({"_iteration": i, "_passed": False, "_exception": str(e)})
            if stop_on_error:
                failed_at = i
                break

    report = {
        "run_id": run_id,
        "playbook": playbook,
        "n_iterations_run": len(results),
        "all_passed": all(r.get("_passed") for r in results),
        "failed_at": failed_at,
        "results": results,
    }
    (run_dir / "report.json").write_text(json.dumps(report, indent=2))
    md_lines = [
        f"# SensaPulse run report: {run_id}",
        "",
        f"- Playbook: `{playbook['name']}`",
        f"- Iterations run: {len(results)} of {iters}",
        f"- All passed: {'✅' if report['all_passed'] else '❌'}",
        "",
        "| iter | passed | battery_mv | rec_bytes | errors | failures |",
        "|---|---|---|---|---|---|",
    ]
    for r in results:
        md_lines.append(
            f"| {r['_iteration']} | "
            f"{'✅' if r.get('_passed') else '❌'} | "
            f"{r.get('battery_mv', '-')} | "
            f"{r.get('recorder_bytes_total', '-')} | "
            f"{len(r.get('errors', []))} | "
            f"{', '.join(r.get('_failures', []))} |"
        )
    (run_dir / "report.md").write_text("\n".join(md_lines))
    print(f"\n[run_loop] report → {run_dir}/report.md")
    return 0 if report["all_passed"] else 1


if __name__ == "__main__":
    sys.exit(main())
