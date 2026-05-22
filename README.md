# SensaPulse — Wearable Continuous-Wear Stethoscope

A wearable stethoscope worn on the chest 24/7, recording two-channel PDM audio (body + ambient mic) plus 6-axis IMU to micro-SD, with offline BLE sync to PC. Designed to enable on-device audio source separation (heart/lung from environmental noise) so a doctor can listen as if using a traditional stethoscope — without raw recordings leaving the device.

This repo contains the device firmware (Zephyr / nRF Connect SDK), a custom-board definition for PCB v1.0, the PCB v2 design spec under draft, Python tools for offline data + algorithm work, and overnight test playbooks.

---

## Overview

The product has two functional phases:

1. **Phase 1 — Data collection** (PCB v1.0, firmware v1.x): wearable records raw 2-ch PDM audio (16 kHz / 16-bit, body + ambient) + IMU (52 Hz) to micro-SD as 10-min session folders. Double-tap to start/stop. BLE sync (v1.1) pulls control + meta + IMU + small audio samples to PC. Bulk raw audio is moved by physically removing the SD card, NOT over BLE.
2. **Phase 2 — On-device DSP** (PCB v2, future firmware): MCU runs noise-cancellation in real time; only filtered heart+lung audio leaves the device over BLE. Privacy by design (voice/ambient stripped on-chip) + naturally fits Apple's BLE bandwidth ceiling.

Currently the Phase 1 device is production-ready (v1.0 + v1.1 firmware CLOSED). Algorithm work for Phase 2 is in flight on PC against bench data captured by Phase 1. PCB v2 spec is under draft and pending product-owner review on §13 open decisions.

---

## Status

| Area | State | Notes |
|---|---|---|
| **Firmware v1.0** (local recording) | ✅ CLOSED 2026-05-13 | All acceptance criteria in `docs/REQUIREMENTS.md` pass. |
| **Firmware v1.1** (BLE sync) | ✅ CLOSED 2026-05-14 | Bulk READ deadlock fixed (commit `9b5fc1b`). Throughput ~17 KB/s ceiling vs Apple host (DLE refused — see CLAUDE.md Discovered note). |
| **Firmware v1.1.1** (BLE-driven start/stop) | ✅ shipped | Commit `e629edf` — symmetric start-method=stop-method. |
| **Algorithm v2 — NLMS denoise** | 🚧 prototype proven on SESSION_00002 | `tools/nlms_denoise.py` + 6 notebooks. Heart-band fundamental still limited by v1.0 body mic 130 Hz HPF — v2 mic (IM73D122, 28 Hz) needed for full S1/S2 capture. |
| **Algorithm v2 — Lung pipeline** | 🚧 preliminary scan | `notebooks/v2_lung_pipeline.py` + `v2_lung_phases.py` + `v2_lung_scan.py` + `v2_lung_zoom.py`. |
| **PCB v1.1 proposal** | 📋 review draft | `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` — incremental fixes from v1.0 retrospective. |
| **PCB v2 spec** | 📋 Draft v2.0 (2026-05-16) | `docs/PCB_v2_SPEC.md` §13 = 6 open decisions pending Dat (mic single vs dual, battery cap, substrate, PPG drop, adhesive, antenna). |
| **CMSIS-DSP firmware port** of NLMS | ⏳ not started | Blocked on algorithm lock-in via bench listening tests. |
| **Production hardening** | 📋 tracked | `docs/PRODUCTION_TODO.md` — BLE pairing/bonding, brown-out detector, MCUboot OTA, FCC/CE, etc. |

See `CLAUDE.md` for the canonical operational context (hardware pin map, PCB v1.0 quirks, locked-in firmware spec, build/flash/RTT workflow). See `PROJECT_MAP.md` for the living single-entry-point with current week state + glossary + by-concern index.

---

## Repository layout

```
.
├── CLAUDE.md                          # Operational instructions (auto-loaded each session)
├── PROJECT_MAP.md                     # Living map — start here for current state
├── TASKS.md                           # Canonical task list (numbered, sequential)
├── README.md                          # ← you are here
├── MIGRATION.md                       # Cowork ↔ Claude Code workflow notes
├── LICENSE
│
├── app/                               # Firmware (Zephyr / nRF Connect SDK v2.9.3)
│   ├── CMakeLists.txt                 # BOARD_ROOT-aware
│   ├── prj.conf                       # Kconfig
│   ├── boards/sensapulse_v1.overlay   # Peripheral DT enables
│   ├── src/                           # main.c + per-module .c/h
│   └── sd_stress/                     # Standalone SD subsystem isolation firmware (#24)
│
├── boards/itrvn/sensapulse_v1/        # Zephyr custom board definition (HWMv2)
│
├── docs/
│   ├── REQUIREMENTS.md                # v1.0 acceptance criteria
│   ├── SCOPE.md                       # One-page non-technical brief
│   ├── TIMING.md                      # Per-task estimate vs actual (calibration data)
│   ├── SYNC_PROTOCOL.md               # v1.1 BLE GATT spec
│   ├── PRODUCTION_TODO.md             # Pre-ship hardening checklist
│   ├── POSTMORTEM_SD_WRITE_RELIABILITY.md  # #32 root-cause analysis
│   ├── PCB_v1_BOM.csv                 # v1.0 BOM
│   ├── PCB_v1_COST_ANALYSIS.md        # v1.0 cost breakdown
│   ├── PCB_v1_REVIEW_v1.1_PROPOSAL.md # v1.0 retrospective + incremental v1.1 fixes
│   ├── PCB_v2_BOM.csv                 # v2 BOM (DigiKey snapshot 2026-05-16)
│   ├── PCB_v2_SPEC.md                 # v2 design spec (Draft v2.0)
│   ├── SETUP_MACOS.md                 # Toolchain install
│   └── SETUP_VSCODE_CLAUDE.md         # IDE + Claude Code workflow setup
│
├── tools/                             # Python — runs on PC against SD-captured data
│   ├── load_session.py                # Session folder → (audio, imu, meta)
│   ├── nlms_denoise.py                # NLMS / spectral / Wiener prototypes
│   ├── noise_cancel.py                # Shared denoise primitives
│   ├── sync.py                        # PC-side BLE sync CLI (bleak)
│   ├── concat_audio.py                # Concatenate WAVs across sessions
│   ├── analyze_overnight.py           # Overnight-run report builder
│   ├── l2cap_test.py                  # L2CAP CoC throughput probe
│   └── test_ble_sync_state.py
│
├── notebooks/                         # v2 algorithm development scripts
│   ├── v2_noise_cancel_v0.py          # First-pass NLMS vs spectral
│   ├── v2_noise_cancel_v1.py          # Hyperparam grid + quality metrics
│   ├── v2_lung_scan.py / _zoom.py / _phases.py / _pipeline.py   # Lung-band exploration
│   ├── v2_postures.py                 # Posture-robust HR estimator
│   ├── v2_ablation_ambient_mic.py     # Quantify ambient-mic contribution
│   ├── debug_hr_estimate.py
│   ├── export_cleaned_for_doctor.py   # Doctor-playback WAV export
│   ├── session_02_explore.py
│   ├── doctor_audio/                  # 10 positional WAVs (4 + 16 kHz × 5 postures)
│   └── figures/                       # PNG outputs (gitignored)
│
├── playbooks/                         # Test scenarios + decision matrices
│   ├── README.md
│   ├── v1_overnight_stability.md
│   ├── sd_reliability_test.md
│   ├── sd_stress_isolation.md         # #24 decision matrix
│   ├── denoise_test_v1.md             # Bench listening protocol for NLMS
│   ├── short_smoke_x10.json           # Playbook driver input
│   └── long_recording_stability.json
│
├── scripts/                           # Bash + Python harness (build/flash/parse)
│   ├── build_flash.sh                 # Atomic build + flash + RC
│   ├── rtt_capture.sh                 # JLinkRTTLogger wrapper, fixed duration
│   ├── parse_rtt.py                   # RTT log → metrics JSON
│   ├── parse_sd_reliability.py
│   ├── parse_sd_stress.py
│   ├── sd_stress_loop.sh              # Build-once flash-then-capture × 10
│   ├── run_loop.py                    # Overnight playbook driver
│   ├── digikey_bom_cost.py            # BOM costing via DigiKey API
│   └── claude_settings.json
│
├── logs/                              # RTT capture logs (gitignored)
└── runs/                              # Overnight run reports (gitignored)
```

---

## Hardware

| Block | PCB v1.0 part | PCB v2 candidate (Draft) |
|---|---|---|
| MCU | Ebyte E73-2G4M08S1C (nRF52840) | Raytac MDBT50Q-1MV2 (same SoC) |
| Body mic | ST MP23DB01HPTR (130 Hz HPF — cuts heart fundamental) | Infineon IM73D122V01XTMA1 (28 Hz HPF) ⭐ |
| Ambient mic | ST IMP34DT05TR | TDK ICS-41351 |
| IMU | ST LSM6DSLTR | ST LSM6DSV16XTR (with ML core) |
| Storage | micro-SD (SPI mode) | micro-SD (TBD industrial vs eMMC) |
| Power | Li-ion 50 mAh + TPS7A0333 LDO | LiPo 300 mAh + TPS62840 buck + BQ25618 charger + TPS3839 supervisor |
| Charging | none (bench cable) | USB-C |
| Schematic | PCB v1.0 dated 2026-04-09 ✅ produced | Draft v2.0 dated 2026-05-16 — review pending |

Full pin map, PCB v1.0 quirks already worked around in firmware (no external I²C pull-ups, etc.), and the locked-in firmware spec are in `CLAUDE.md`. PCB v2 rationale + DigiKey BOM snapshot in `docs/PCB_v2_SPEC.md`.

---

## Quick start

### Firmware build / flash / debug

```bash
# Build (requires nRF Connect SDK v2.9.3 toolchain in PATH — see docs/SETUP_MACOS.md)
cd app
west build -b sensapulse_v1/nrf52840 -p auto -- -DBOARD_ROOT=$(pwd)/..

# Flash (requires J-Link)
west flash

# Capture RTT log (single command, writes to file, Ctrl+C to stop)
JLinkRTTLogger -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 logs/rtt.log

# Helper scripts (preferred — handle env quirks)
scripts/build_flash.sh
scripts/rtt_capture.sh 60 logs/run.log
python scripts/parse_rtt.py logs/run.log
```

### Python — read a SD session + run NLMS denoise

```bash
# Load a session folder (returns dict of audio, imu, meta)
python -m tools.load_session /Volumes/SENSAPULSE/SESSION_00002

# Run NLMS adaptive denoise on body + ambient channels
python -m tools.nlms_denoise /Volumes/SENSAPULSE/SESSION_00002 \
    --mode nlms --filter-len 128 --mu 0.05 --out cleaned.wav --plot
```

### PC-side BLE sync (v1.1)

```bash
# Discover, list unsynced sessions, pull all
python tools/sync.py --device-name "Dat-test"

# Single-file debug (skip ACK, cap bytes)
python tools/sync.py --only-session 14 --only-file-idx 1 --max-bytes 1024 --no-ack
```

---

## Key documents

| Question | Read |
|---|---|
| What is the project trying to achieve? | This README + `docs/SCOPE.md` |
| What's the current state right now? | `PROJECT_MAP.md` §1 LIVE STATE |
| What tasks are open vs closed? | `TASKS.md` |
| Hardware pin map / SoC quirks / build trick | `CLAUDE.md` |
| BLE v1.1 protocol details | `docs/SYNC_PROTOCOL.md` |
| SD reliability postmortem (the long story behind #32) | `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md` |
| PCB v2 design rationale + BOM | `docs/PCB_v2_SPEC.md` |
| PCB v1.0 retrospective + v1.1 incremental proposal | `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` |
| What's needed before shipping a real unit | `docs/PRODUCTION_TODO.md` |
| Per-task estimate vs actual (calibration history) | `docs/TIMING.md` |

---

## Conventions

- **Raw data first**: no firmware-side filtering or feature extraction. All DSP runs on PC against SD-captured WAV+CSV. Don't propose adding on-board filtering without explicit user approval.
- **Single-FATFS-owner invariant** (strict): all FATFS calls run on the `sd_writer` thread. Anything outside must route via the writer's public sync API (e.g. `sd_writer_touch_file`). Violating this even once per second crashes the SD card under sustained write — see #32 + `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md`.
- **BLE bulk audio is not a workload**: raw 2-ch audio moves via SD card pull, not BLE. Don't optimize BLE as if it had to stream stereo 16 kHz.
- **Vietnamese-first communication**: user (Dat Tran, ITRVN) replies in Vietnamese unless he switches. Frame embedded explanations in terms of signal pipelines (DMA = streaming buffer, PDM decimation = front-end filter, etc.) — he is a biomedical signal-processing engineer, firmware/C is outside his daily work.
- **TASKS.md numbering is stable**: never renumber. When closing a task also append actual hours in `docs/TIMING.md` (estimate, actual, variance, brief note).
- **Build hash in meta.json**: every session records `build_hash` (commit short SHA) so an SD file can be traced back to the exact firmware build.

---

## License

See `LICENSE`.

## Maintainer

Dat Tran, ITRVN — dattran@itrvn.com
