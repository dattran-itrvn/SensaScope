# Playbooks

Each `*.json` file describes one overnight test scenario. `scripts/run_loop.py` reads a playbook, drives build → flash → RTT capture → parse → check, writes results to `runs/<timestamp>_<name>/`.

## Existing playbooks

| File | Duration | Purpose |
|---|---|---|
| `short_smoke_x10.json` | ~10 × 30 s | Validate the loop scripts themselves before trusting overnight runs. |
| `long_recording_stability.json` | 30 min × 1 | Idle-running stability — board boots, runs, doesn't crash. |

## Schema

```json
{
    "name": "string — used in run folder name",
    "description": "human-readable",
    "iterations": "int — how many build+flash+capture cycles",
    "per_iter_seconds": "int — RTT capture duration per iter",
    "pristine_first": "bool — pass --pristine to build_flash.sh on first iter",
    "stop_on_error": "bool — abort the loop on first failure",
    "expect": {
        "booted": "bool",
        "battery_mv_min": "int",
        "errors_max": "int",
        "recorder_bytes_min": "int"
    }
}
```

## Adding a new playbook

1. Copy an existing JSON, change name + duration + expectations.
2. Run once with short duration first (`per_iter_seconds: 30`) to verify expectations are not too strict.
3. Then ramp up duration.

## Output

Each run produces `runs/<timestamp>_<name>/`:
- `iter_NNN.rtt.log` — raw RTT log per iteration
- `report.json` — all metrics + per-iter pass/fail
- `report.md` — human-readable summary table

The morning workflow: open `report.md`, scan the pass/fail column, dig into the first failed iteration's `iter_NNN.rtt.log` if any.
