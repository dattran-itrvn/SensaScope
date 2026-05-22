# Project Map — SensaPulse

> **Single entry point for new Claude sessions. Read this file FIRST before any other action.**
>
> Maintained by Claude. §1 (LIVE STATE) is rewritten end-of-session when domain state changes. §2-4 drift slowly; updated only when convention changes. §5 (ARCHIVE) is append-only.
>
> If §1 is older than 7 days, treat live state as stale — re-derive from `git log --since=7.days` + read recent `TASKS.md` diff + check `docs/PCB_v2_SPEC.md` §13 before acting.

**Last updated:** 2026-05-22 — PROJECT_MAP introduced (ecg_project pattern). Last domain state change: 2026-05-16 PCB v2 SPEC Draft v2.0 published; 2026-05-14 v1.1.1 + v2 algo bench scripts landed.

---

## 1. LIVE STATE (current week)

### 1.1 Active goal

Phase 2 enablement: prove on-device noise-cancellation is feasible on v1.0 bench data BEFORE committing PCB v2 to a particular algorithm path. Three parallel tracks:

- **Algorithm bench (PC-side)**: NLMS adaptive filter is the leading candidate (cheapest CPU, ports to CMSIS-DSP `arm_lms_norm_f32` on Cortex-M4F). First working pipeline shipped 2026-05-14 (commit `ee75a1b`). Subsequent commits added posture robustness, ambient-mic ablation, doctor-playback export, lung-pipeline preliminary scan. Next step per `playbooks/denoise_test_v1.md`: bench listening test to judge clinical quality. Listening verdict drives whether v2 ships with classical DSP (NLMS) on nRF52840 or escalates to neural denoise / bigger MCU.

- **PCB v2 spec**: `docs/PCB_v2_SPEC.md` Draft v2.0 (2026-05-16) — pending Dat's review of 6 open decisions in §13 (single vs dual mic, battery 300 vs 500 mAh, substrate hybrid vs full-flex, PPG drop confirmed, adhesive Silbione vs Easyderm, antenna integrated vs u.FL). Em (Cowork) defaulted to dual omni + 300 mAh + hybrid rigid+flex + drop PPG + Silbione + integrated antenna. **No tape-out until §13 confirmed.**

- **PCB v1.1 incremental proposal**: `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` (2026-05-16) — separate path for an incremental fix on v1.0 (better body mic + I²C pull-ups + USB-C + brown-out) if v2 full-redesign slips. Review meeting with PCB team pending.

Production v1.1.1 firmware is **locked** for bench data collection while Phase 2 work proceeds.

### 1.2 In-flight work items

| Item | Owner | Status | Where |
|---|---|---|---|
| Bench listening test on NLMS-cleaned audio | Dat (by ear) | ⏳ pending — protocol ready | `playbooks/denoise_test_v1.md` |
| PCB v2 §13 review (6 decisions) | Dat | ⏳ pending | `docs/PCB_v2_SPEC.md` §13 |
| PCB v1.1 review meeting with PCB team | Dat + PCB team | ⏳ pending | `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` |
| NLMS port to CMSIS-DSP firmware | Claude Code | ⏳ blocked on listening verdict | — |
| Lung-pipeline iteration (voice rejection) | Cowork + Dat (notebooks) | 🚧 preliminary scan done | `notebooks/v2_lung_pipeline.py`, `v2_lung_phases.py`, `v2_lung_scan.py`, `v2_lung_zoom.py` |
| Heart S1/S2 fundamental capture | blocked on v2 mic | ⛔ HW-limited | v1.0 body mic MP23DB01HP cuts <130 Hz; IM73D122 (28 Hz) on v2 BOM |

### 1.3 Production firmware (locked)

- **Branch**: `main` at commit `693ef88` (v2 algo top of tree, v1.1.1 firmware locked underneath at `e629edf`).
- **Firmware version exposed via BLE Device Info**: v1.1.1 (build hash from feat branch tip stamped into `meta.json` per session).
- **Hardware**: PCB v1.0 dated 2026-04-09 (single board produced; bench-only). All known PCB v1.0 quirks worked around in code — see CLAUDE.md.
- **Operational gotchas to remember**:
  - J-Link debug session with SW1 OFF: chip runs off SWD 3.27 V, but `battery_read_mv()` returns noise → FSM trips to `LOW_BATT_HOLDOFF`. Set `BATT_LOW_MV = 0` in `main.c` for J-Link sessions; revert before commit.
  - Empty session folders (no `.unsynced` marker, or `audio.wav < 44 B`) = crashed before first write → PC sync skips them.
  - Apple Core Bluetooth refuses DLE upgrade → throughput hard-capped ~17 KB/s. NOT a firmware bug. Live with it; raw bulk audio goes via SD card pull anyway.
  - 30 GB SanDisk card is EOL (mid-session `-116 ETIME` even post-#32). Bench-trusted card = 122 GB SanDisk. Vet new cards with 5 × 420 s stress before production use.

### 1.4 Bench data assets

| Asset | Path | Notes |
|---|---|---|
| Bench session corpus | `/Volumes/SENSAPULSE/SESSION_*` (external SD) | SESSION_00002 (~46.6 s) is the canonical test session — used by every v2 notebook. |
| Doctor-playback WAVs | `notebooks/doctor_audio/01..05_*_{4,16}kHz.wav` | 5 postures × 2 sample rates × 1 NLMS-cleaned export. Listenable on phone/headphones. |
| RTT logs from overnight runs | `logs/` (gitignored) | Used by `scripts/parse_rtt.py` to extract metrics. |
| SD stress run summaries | `logs/sd_stress_*/` | 3 historical stress runs (2026-05-09 phases 1-6). |
| Overnight run reports | `runs/` (gitignored) | One report per playbook execution. |

### 1.5 Pending decisions (need user input)

- **PCB v2 §13 (6 decisions)** — see §1.1. Top priority.
- **Algorithm path lock-in**: after bench listening test, choose `classical NLMS on nRF52840` vs `escalate to bigger MCU` vs `neural denoise on quantized M4F`. Drives v2 firmware roadmap.
- **PCB v1.1 vs jump straight to v2**: depends on PCB team capacity + lead-time vs schedule. If v1.1 chosen, Phase 2 algo dev gets new bench hardware sooner with corrected body mic.
- **Production hardening sequencing** (`docs/PRODUCTION_TODO.md`): user has not explicitly directed Claude Code to start any of the post-v1.1 hardening items (pairing/bonding, brown-out detector, MCUboot OTA, etc.). Don't start without explicit direction.

---

## 2. GLOSSARY

### 2.1 Firmware versions

| Name | Scope | Status |
|---|---|---|
| **v1.0** | Local recording only: PDM → SD as 10-min session folders, double-tap start/stop, no BLE, IMU+meta+WAV. | ✅ CLOSED 2026-05-13 |
| **v1.1** | + BLE GATT Sync Service for PC pull (LIST/READ/ACK/DEL/RESET), `.unsynced` marker, free-space eviction, FSM SYNC state mutual exclusion. | ✅ CLOSED 2026-05-14 |
| **v1.1.1** | + BLE Control opcodes `START_RECORD` / `STOP_RECORD` with start-method=stop-method symmetry (so a session started by tap must be stopped by tap, and vice versa). | ✅ shipped — commit `e629edf` |
| **v2 firmware** | On-device noise-cancellation (Phase 2). Algorithm path not locked yet — depends on bench verdict. | ⏳ not started |

### 2.2 PCB versions

| Name | Status | Key change |
|---|---|---|
| **PCB v1.0** | ✅ produced single bench unit, schematic 2026-04-09 | Baseline. 8 known issues (body mic HPF, no external pull-ups, weak switch, no charger, no BOD, etc.). |
| **PCB v1.1** (proposal) | 📋 review draft `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` | Incremental fixes: better body mic, I²C pull-ups, USB-C charger, brown-out detector. Cheaper iteration if v2 slips. |
| **PCB v2** (Draft v2.0) | 📋 spec `docs/PCB_v2_SPEC.md` 2026-05-16, §13 6 open decisions | Full redesign: IM73D122 + ICS-41351 mics, LSM6DSV16X IMU, LiPo 300 mAh, USB-C, hybrid rigid+flex. |

### 2.3 Algorithm methods (notebooks/v2_*)

| Method | Code | Trade-off |
|---|---|---|
| **NLMS** (Normalized LMS) | `tools/nlms_denoise.py --mode nlms` + `notebooks/v2_noise_cancel_v0.py` / `_v1.py` | Cheapest CPU; portable to CMSIS-DSP `arm_lms_norm_f32` on M4F. Time-domain, 1-frame look-ahead. Current leader. |
| **Spectral subtraction** | `--mode spectral` | Classical STFT-based; needs ambient noise spectrum estimate. Mid CPU. Worse than NLMS on v0 bench. |
| **Wiener cross-spectral** | `--mode wiener` | Best quality, batch-mode, not real-time-portable. Reference ceiling, not deploy candidate. |
| **Lung pipeline** | `notebooks/v2_lung_pipeline.py` + variants | NLMS configured for 60-2000 Hz band; voice (300-3000 Hz) overlap — ambient mic is the key here, ablation predicted +10 dB lung-band SNR. |
| **Posture-robust HR** | `notebooks/v2_postures.py` + `debug_hr_estimate.py` | Time-domain peak HR estimator that survives posture changes. Validated against 5 postures in `doctor_audio/`. |
| **Ambient-mic ablation** | `notebooks/v2_ablation_ambient_mic.py` | Quantifies the ambient mic's contribution. Result drives PCB v2 §13 D1 (single vs dual mic). |

### 2.4 Key terms / invariants

- **Single-FATFS-owner invariant** (strict, do NOT violate): all FATFS calls run on the `sd_writer` thread. Violated by `session.c monitor.touch_unsynced` originally; #32 routes everything through `sd_writer_touch_file` / `sd_writer_get_free_mb` / `sd_writer_rotate_full`. Even 1 fs_open/close per second from another thread crashes the card with `-116 ETIME` in 12-41 min.
- **`.unsynced` marker as integrity signal**: 0-byte file created by watchdog only after `audio_recorder_bytes_written > 0`, gated by `!sd_writer_is_rotating()`. Folder without marker = crashed before first write = PC sync skips.
- **Apple BLE GATT throughput ceiling = 15.5 KB/s sustained** (bench-measured 2026-05-22 via 25-sample on-disk size sweep, see CLAUDE.md Discovered note). Both macOS and iOS Core Bluetooth send `LL_LENGTH_RSP 27/27` (refuses DLE upgrade) — that part is HCI-DBG observation. The ceiling 14-17 KB/s is **derived math** from LL=27 × packets/event × events/sec, **NOT from any Apple-published document**. "Bluetooth Accessory Design Guidelines for Apple Products" (Apple's public BLE doc) does not state DLE behaviour or throughput caps.
- **PDM mem-slab slack**: `K_MEM_SLAB_DEFINE_STATIC(pdm_slab, ..., 16, 4)` — 1.6 s slack is the floor for safe operation with #25 sd_writer. 8 blocks (0.8 s) was on the edge.
- **Bench-trusted card**: 122 GB SanDisk only. 30 GB SanDisk was EOL'd 2026-05-09 (`-116` even post-fix). Vet new cards with 5 × 420 s stress before production.
- **Build hash**: every session's `meta.json` stamps the firmware commit short SHA. Trace any SD file back to its exact firmware build.
- **Cowork vs Claude Code split**: Cowork is the architect/reviewer agent (Claude desktop app, sandboxed bash, reads PDFs/schematics). Claude Code is the executor (native Mac M4, runs build/flash/RTT, edits code). When user asks for things needing image analysis or schematic reading, suggest taking it back to Cowork.

### 2.5 Hardware quick-recall

| Pin | Function | Pin | Function |
|---|---|---|---|
| P0.03 | LED (active-high via SSM3K44) | P0.04 | I²C SCL |
| P0.12 | I²C SDA | P0.05 | PDM_DATA |
| P1.09 | PDM_CLK | P0.06 | LSM6DSL INT1 (active high) |
| P0.08 | LSM6DSL INT2 | P0.15 | SPI MISO |
| P0.17 | SPI SCK | P0.20 | SPI MOSI |
| P0.22 | SPI CS | P0.26 | SD CARD_DETECT (active low) |
| P0.28 | BATT_LEVEL (VBATT/2) | — | — |

Full schematic refs + PCB v1.0 quirks: `CLAUDE.md`.

---

## 3. BY CONCERN

Each subsection: canonical reference files + production pointer + recipe to find recent activity.

### 3.1 Firmware — recording engine

- **Modules**: `app/src/main.c` (FSM) + `audio.c` (PDM producer) + `imu_sampler.c` (IMU producer) + `sd_writer.c` (single FATFS owner, 2-FIFO consumer) + `session.c` (lifecycle + 10-min rotation) + `led.c` (state machine) + `battery.c` (ADC + classifier) + `sdlog.c` (mount + boot stamp).
- **Build**: `cd app && west build -b sensapulse_v1/nrf52840 -p auto -- -DBOARD_ROOT=$(pwd)/..` — the `-DBOARD_ROOT` is mandatory (sysbuild outer cmake fails without it).
- **Flash**: `west flash` (J-Link via nrfjprog). Apple Silicon noise like `JLinkARM.dll reported error -256` is harmless; look for "flashed successfully" at the bottom.
- **Closed tasks**: #1-#16, #18, #22-#28, #30, #32 (v1.0); #17, #19, #20, #21 (v1.1); #31, #33, #34, #35 (hardening).
- **Open items**: none for the production stack — locked.
- **Find recent**: `git log --oneline app/src/`

### 3.2 BLE sync (v1.1)

- **Spec**: `docs/SYNC_PROTOCOL.md`. Service UUID `7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e`. 4 characteristics: Device Info / Control / Data / Set Name.
- **Firmware**: `app/src/ble_sync.c` — credit-based flow control (`bt_gatt_notify_cb`, 1-permit semaphore) + dedicated `read_wq` (NOT system_work_queue, see Discovered 2026-05-14).
- **PC tool**: `tools/sync.py` (bleak). Supports LIST/READ/ACK/RESET + atomic `.tmp/` → final rename + `--resume` + per-file debug flags.
- **Throughput ceiling**: ~17 KB/s vs Apple host (DLE refused). Non-Apple host (Linux/Android) ~80 KB/s — L2CAP CoC code preserved on main (PSM `0x0080`) for that path.
- **Closed tasks**: #19, #20, #21, #17, #34, #35.
- **Memory**: `[[project_34_bulk_read_bisect]]`, `[[project_sync_scope]]` — scope clarification that BLE is for control + meta + IMU + small audio samples, NOT raw bulk.

### 3.3 SD reliability

- **Postmortem**: `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md` — full diagnosis of #23/#25/#27/#30/#32 chain.
- **Standalone diagnostic firmware**: `app/sd_stress/` (no PDM/IMU/FSM/BLE; 4 synthetic phases). Driver: `scripts/sd_stress_loop.sh` builds once + flashes/captures/parses 10× (~40 min wall).
- **Decision matrix for new failures**: `playbooks/sd_stress_isolation.md` — 10/10 pass = interaction-class; ≥1 fail = SD-subsystem-class; >500 ms latency = stalls long enough to break PDM.
- **Closed tasks**: #23, #24, #25, #26, #27, #28, #30, #32.
- **CLAUDE.md Discovered notes**: 2026-05-09 (mem-slab 16-block floor, cold-boot 100 ms settle, 122 GB bench card), 2026-05-12 (single-FATFS-owner = invariant), 2026-05-14 (BLE notify deadlock root cause).

### 3.4 Audio denoising — v2 algorithm (current focus)

- **Primary CLI tool**: `tools/nlms_denoise.py` — `python -m tools.nlms_denoise SESSION_PATH [--mode nlms|spectral|wiener] [--filter-len N] [--mu F] [--out cleaned.wav] [--plot]`.
- **Shared primitives**: `tools/noise_cancel.py` — `nlms_subtract(body, ambient, taps, mu) -> cleaned`.
- **Notebook progression** (chronological):
  1. `notebooks/v2_noise_cancel_v0.py` — first-pass NLMS vs spectral comparison.
  2. `notebooks/v2_noise_cancel_v1.py` — hyperparam grid (taps, μ) + 3 quality metrics (heart-band SNR, envelope kurtosis, autocorrelation peak strength) + WAV exports.
  3. `notebooks/v2_postures.py` + `debug_hr_estimate.py` — posture-robust time-domain peak HR estimator (5 postures).
  4. `notebooks/export_cleaned_for_doctor.py` — doctor-playback + BLE-production format exports.
  5. `notebooks/v2_ablation_ambient_mic.py` — does the ambient mic actually contribute? (drives PCB v2 §13 D1).
  6. `notebooks/v2_lung_scan.py` / `_zoom.py` / `_phases.py` / `_pipeline.py` — lung-band exploration (60-2000 Hz, voice overlap problem).
- **Listening protocol**: `playbooks/denoise_test_v1.md` — bench listening test before committing to algorithm path. Decision tree: NLMS clean → ship v2 with `arm_lms_norm_f32`; spectral/Wiener only → may need nRF54L15 v3; none work → deep-learning denoise (U-Net / Conv-TasNet quantized).
- **Hardware caveat for v1.0 bench data**: body mic MP23DB01HP 130 Hz HPF cuts S1/S2 fundamental (25-150 Hz). Lung sounds (100-2500 Hz) intact and are the **primary validation target** on v1.0 hardware. Full heart capture requires PCB v2 with IM73D122 (28 Hz HPF).
- **Bench data source**: `/Volumes/SENSAPULSE/SESSION_00002` (~46.6 s, the canonical test session) + `notebooks/doctor_audio/` (5 postures × 2 sample rates).

### 3.5 Hardware / PCB

- **PCB v1.0 (current bench)**: schematic 2026-04-09. BOM `docs/PCB_v1_BOM.csv`. Cost breakdown `docs/PCB_v1_COST_ANALYSIS.md`. Quirks worked around in firmware — see CLAUDE.md.
- **PCB v1.1 (proposal)**: `docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md` — review draft for PCB team. Incremental fix path if v2 slips.
- **PCB v2 (Draft v2.0)**: `docs/PCB_v2_SPEC.md` 2026-05-16. BOM `docs/PCB_v2_BOM.csv`. §13 open decisions = blocking before tape-out.
- **Costing tool**: `scripts/digikey_bom_cost.py BOM.csv --qty N` — needs DigiKey developer API key.
- **Production-grade hardening checklist**: `docs/PRODUCTION_TODO.md` — BLE pairing/bonding, brown-out detector, FATFS power-fail recovery, MCUboot OTA, FCC/CE, etc.

### 3.6 Python tools (PC-side)

- **Session loader**: `tools/load_session.py` — `load_session(path) -> {audio, fs_audio, imu, fs_imu, meta}`. Uses stdlib `wave` + numpy + pandas (no scipy dep for the read path).
- **NLMS denoise**: `tools/nlms_denoise.py` — see §3.4.
- **BLE sync CLI**: `tools/sync.py` — see §3.2.
- **L2CAP CoC probe**: `tools/l2cap_test.py` — used during #34 deep-dive.
- **Audio concat**: `tools/concat_audio.py` — concatenate WAVs across sessions.
- **Overnight analyzer**: `tools/analyze_overnight.py` — build report from `runs/` artifacts.
- **BLE state self-test**: `tools/test_ble_sync_state.py`.

### 3.7 Test playbooks + harness

- **Playbook driver**: `scripts/run_loop.py PLAYBOOK.json` — loops build → flash → capture → parse → decide.
- **RTT parser**: `scripts/parse_rtt.py LOG_FILE` — extracts boot, battery, PDM peak/mean, recorder bytes, taps, errors.
- **Playbooks** (see `playbooks/README.md` for full list):
  - `v1_overnight_stability.md` — long-run stability protocol.
  - `sd_reliability_test.md` — empty-session bug repro (historical, fixed by #32).
  - `sd_stress_isolation.md` — #24 decision matrix.
  - `denoise_test_v1.md` — bench listening protocol for v2 algo (CURRENT priority).
  - `short_smoke_x10.json` / `long_recording_stability.json` — driver inputs.

---

## 4. HOW TO FIND THINGS

| If you need... | Read |
|---|---|
| What's the project trying to achieve? | `README.md` Overview |
| Hardware pin map, build trick, SoC quirks | `CLAUDE.md` |
| Numbered task list (what's done vs open) | `TASKS.md` |
| Per-task hours estimate vs actual | `docs/TIMING.md` |
| Living state, current focus, who's blocked | §1 (this file) |
| Term I don't recognize | §2 GLOSSARY (this file) |
| Where a feature lives in the tree | §3 BY CONCERN (this file) |
| Recent commits | `git log --oneline -20` |
| What broke last time + how it was fixed | CLAUDE.md "Discovered" notes (bottom) |
| BLE v1.1 protocol bytes-on-wire | `docs/SYNC_PROTOCOL.md` |
| SD reliability deep-dive | `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md` |
| PCB v2 rationale + open decisions | `docs/PCB_v2_SPEC.md` (§13 = blocking) |
| Production checklist | `docs/PRODUCTION_TODO.md` |
| User preferences / corrections / memory | `/Users/trandat/.claude/projects/-Users-trandat-Project-SensaScope/memory/MEMORY.md` |

### 4.1 Hard rules — must respect

- **Single-FATFS-owner**: every FATFS call from any thread goes through `sd_writer` public sync API. Violations crash the SD card in minutes-to-hours.
- **Build always includes `-DBOARD_ROOT`**: otherwise sysbuild outer cmake fails with "Invalid BOARD".
- **Reply in Vietnamese**: user communicates in Vietnamese; switch language only when he does. Frame embedded explanations in signal-pipeline terms (he's a biomedical SP engineer, not a firmware engineer by day).
- **Raw data first**: never propose on-device DSP / filtering / feature extraction without explicit user direction.
- **No on-device noise-cancel firmware until Phase 2** — algorithm path not locked. Don't pre-port NLMS to firmware until bench listening verdict is in.
- **No AskUserQuestion 4-choice pickers**: user wants free-text chat (memory `feedback_no_askuserquestion`).
- **`/compact` workflow**: before user runs `/compact`, give an explicit state snapshot listing persistent vs in-session-only items (memory `feedback_compact_workflow`).
- **LED pattern semantics**: IDLE = 1 pulse, RECORDING = 2 pulses. Don't assume "solid = recording" — old pattern (memory `project_led_patterns`).

---

## 5. ARCHIVE — major milestones (append-only)

One-line summary per milestone. Detailed reports + commits referenced inline.

- **2026-04-09** — PCB v1.0 schematic finalized. Single bench unit produced.
- **2026-05-04 → 2026-05-08** — Bring-up (#1-#8, #16): board definition, LED, I²C+IMU WHO_AM_I, SPI+SD+FATFS, PDM stereo capture, SAADC battery, BLE smoke advertise. All on PCB v1.0 hardware. USB CDC shell (#8) descoped — RTT sufficient.
- **2026-05-08 → 2026-05-09** — Milestone A: streaming WAV writer (#10), double-tap detection (#9). Session manager + IMU CSV + meta.json + LED FSM + main FSM (#11-#14) integrated. Python session loader (#15) verified against bench session (drift 73 ms, IMU fs_eff 51.97 Hz).
- **2026-05-09** — SD reliability investigation begins: empty session bug (#23 fs_sync band-aid abandoned), single-thread + 2-FIFO redesign (#25), atomic rotate (#27), cold-boot retry (#26), marker race (#28). Single-FATFS-owner invariant established. 30 GB SanDisk EOL'd; 122 GB SanDisk = bench-trusted.
- **2026-05-12** — SD POSTMORTEM published (`docs/POSTMORTEM_SD_WRITE_RELIABILITY.md`). Single-FATFS-owner upgraded from guideline to strict invariant. Test 7 reproduces production crash in 86 s by adding 1 fs_open/close 1Hz from system_work_queue.
- **2026-05-13** — **v1.0 CLOSED**. All `docs/REQUIREMENTS.md` criteria pass. 1-hour production stack verify with #32 fix: 5 rotations clean, ~230 MB audio + ~31000 IMU, 0 drop, 0 FSM ERROR. v1.1 work begins: free-space eviction (#17), session counter validation, PC sync CLI (#21).
- **2026-05-14** — **v1.1 CLOSED**. BLE bulk notify deadlock root-caused as 2-bug stack: silent-drop past TX pool + system_work_queue ↔ notify callback deadlock (#34 commit `9b5fc1b`). Apple DLE refusal confirmed via HCI DBG — LL=27 hard ceiling ~17 KB/s. Scope clarified: BLE sync = control+meta+IMU+small audio; raw bulk via SD pull. L2CAP CoC code preserved (PSM `0x0080`) for non-Apple host path. Hardening: #31 boot timing + #33 sdlog stuck recovery + #35 firmware silent halt (same root cause as #34). **v1.1.1 BLE-driven record start/stop shipped** (commit `e629edf`). **v2 algorithm bench work begins**: NLMS first working algorithm (`ee75a1b`), posture-robust HR (`956cbae`), doctor-playback + BLE-production exports (`dba7643`), ambient-mic ablation (`406ee90`), lung-sound preliminary scan (`693ef88`).
- **2026-05-16** — **PCB v2 SPEC Draft v2.0** published (`docs/PCB_v2_SPEC.md`). Scope clarified: Phase 1 is audio source separation (denoise), NOT disease classification → PPG dropped from v2 BOM (saves $13.51 + ~600 µA). 6 open decisions in §13 (single vs dual mic, battery cap, substrate, PPG drop confirmed, adhesive, antenna) pending Dat's review before tape-out. PCB v1.1 incremental proposal also published (`docs/PCB_v1_REVIEW_v1.1_PROPOSAL.md`) as cheaper alternative path. DigiKey BOMs snapshot (PCB_v1_BOM.csv, PCB_v2_BOM.csv).
- **2026-05-22** — **PROJECT_MAP.md introduced** (this file), pattern from `~/Project/ecg_project/PROJECT_MAP.md`. README refreshed to reflect v1.0 + v1.1 CLOSED + Phase 2 in flight (the previous README still showed v1.0 components as "in progress").
