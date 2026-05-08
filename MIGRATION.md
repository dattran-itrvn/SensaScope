# Migration: Cowork → Claude Code

This document is the handoff so you can run iteration loops (build / flash / RTT / analyze) on your Mac without supervising every command.

## Mental model

| | Cowork (old) | Claude Code (new) |
|---|---|---|
| Where it runs | Sandbox in Anthropic infra | Native on your Mac M4 |
| Can call `west` / `JLink*` | ❌ | ✅ |
| Can read PDFs / WAVs / images | ✅ | partial (text-only files) |
| Persistent project context | own memory store | `CLAUDE.md` in repo |
| Best for | Architecture, schematic reading, audio analysis | Iteration loops, overnight runs |

You'll keep using Cowork (this assistant) for things like reading new datasheets, analyzing recorded WAV files, planning new features. Switch to Claude Code for hands-on iteration.

## One-time setup

### 1. Install Claude Code

Two flavors. Pick one:

**A. CLI** (run in any terminal — recommended for overnight loops):
```bash
npm install -g @anthropic-ai/claude-code
# Login on first run
claude
```

**B. VS Code extension** (chat panel inside the IDE):
- Open VS Code → Extensions → search "Claude Code" (publisher: Anthropic) → Install.
- Sign in via the prompt.

Both share the same `~/.claude/` config and the project's `.claude/settings.json`.

### 2. Open the project from a Toolchain-Manager terminal

Claude Code inherits the shell environment of where it was launched. To pick up `west`, `arm-zephyr-eabi-gcc`, and `nrfjprog`, launch from a Nordic-aware terminal:

- Open `nRF Connect for Desktop` → `Toolchain Manager` → ▾ next to `nRF Connect SDK v2.9.3` → `Open Terminal`.
- In that terminal: `cd /Users/trandat/Project/SensaScope`.
- Then `claude` (CLI) or open VS Code from this terminal (`code .`) so the extension inherits env.

To verify Claude Code sees the toolchain, ask it: *"Run `west --version` and `nrfjprog --version`."*

### 3. Approve the project's permissions file

Claude Code reads `.claude/settings.json` from the project root the first time it sees them. Allowed patterns (already filled in for you):

- All `west *` commands
- `JLinkExe`, `JLinkRTTClient`, `JLinkRTTLogger`, `pkill -f JLink*`
- `python3 scripts/*` and `bash scripts/*`
- File reads inside the project + `/opt/nordic/ncs/v2.9.3/`
- File writes only inside `logs/`, `runs/`, `scripts/`, `playbooks/`

Denied:
- `sudo`, `git push`, `curl`, `wget`, anything that mutates `/Users/trandat/` outside the project.

If something is needed but blocked, Claude Code will surface a permission prompt — accept once or edit `settings.json`.

### 4. Verify Claude Code reads your context

Start a session and ask: *"Đọc CLAUDE.md, tóm tắt 5 dòng các quyết định FW đã chốt."*

If it answers correctly (16 kHz / 16-bit, IMU 52 Hz, double-tap toggle, 10-min rotation, raw data on SD), it has the project context loaded. If not, point it at `CLAUDE.md` explicitly.

## Daily workflow

### Quick iteration (interactive)

```
You:        Add a 1 Hz heartbeat log line that prints free heap.
Claude Code: <edits main.c, runs build_flash.sh, tells you what to expect in RTT>
You:        OK go.
Claude Code: <runs rtt_capture.sh 10s, parses, reports>
```

### Overnight loop (unattended)

```
You:        Run playbooks/long_recording_stability.json. Wake me with a summary.
Claude Code: <runs scripts/run_loop.py — produces runs/<ts>/report.md>
... 30 min later ...
You:        (next morning) cat the report.
```

### Add a new playbook

```
You:        Make a playbook that flashes once, then captures RTT for 8h, expects
            >0 recorder bytes and battery_mv_min=3300.
Claude Code: <creates playbooks/8h_stability.json, dry-run validates>
```

## Cowork ↔ Claude Code handoff

When you want me (Cowork) to take over for analysis:

```
You (to Claude Code):  Generate a short summary I can paste to Cowork — what was tested,
                       what passed, what failed.
You (to Cowork):       <paste summary, attach any WAV/log files you want analyzed>
```

When you want Claude Code to take over from me:

```
You (to Cowork):    Write a brief for Claude Code on what to try next.
                    (I'll write a paragraph or two specifically for it.)
You (to Claude Code): Read CLAUDE.md, then read this brief, then start.
```

## When something goes wrong

- **Build fails repeatedly** — ask Claude Code to diff the working `runs/<old_ts>/` against the broken iter to find what changed.
- **Loop hangs** — `pkill -f JLink` and `pkill -f run_loop.py`. The flash session probably wedged.
- **Board doesn't boot after flash** — physical: pin J-Link reset, gate slide-switch off-on, replug battery. SW: check VTref voltage; could be sub-3V (battery dead).
- **Claude Code refuses a command you expected to be allowed** — edit `.claude/settings.json` and add the exact pattern (with `*` for variables). Restart the session.

## Repo layout for this workflow

```
SensaScope/
├── CLAUDE.md            ← Claude Code reads this first
├── MIGRATION.md         ← (this file)
├── .claude/
│   └── settings.json    ← permission allowlist
├── app/                 ← firmware (Zephyr)
├── boards/              ← custom board definition
├── docs/
├── scripts/
│   ├── build_flash.sh   ← atomic build + flash
│   ├── rtt_capture.sh   ← reset chip + log RTT for N seconds
│   ├── parse_rtt.py     ← extract metrics from RTT log
│   └── run_loop.py      ← overnight loop driver
├── playbooks/
│   ├── README.md
│   ├── short_smoke_x10.json
│   └── long_recording_stability.json
├── logs/                ← per-build / per-flash / per-capture logs
└── runs/                ← per-overnight-run results (report.md + per-iter logs)
```
