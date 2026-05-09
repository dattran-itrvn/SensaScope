# SensaPulse — Task List

This is the canonical, version-controlled source of truth for what's done and what's next. Cowork (planner) and Claude Code (executor) both read it. When you complete or modify a task, edit this file and commit.

Tasks #8 was descoped (USB CDC shell — RTT is sufficient for debug). Numbering otherwise sequential.

## Status legend
- ✅ done
- 🚧 in progress
- ⏳ pending
- 🗑️ descoped

---

## v1.0 — local recording firmware (no BLE sync)

### Bring-up phase (peripheral verification)

**#1 ✅ Custom board definition `sensapulse_v1`**
Create `boards/itrvn/sensapulse_v1/` with `board.yml`, `Kconfig.sensapulse_v1`, `Kconfig.defconfig`, `board.cmake`, `sensapulse_v1_nrf52840.dts`, `sensapulse_v1_nrf52840.yaml`, `sensapulse_v1_nrf52840_defconfig`, `sensapulse_v1_nrf52840-pinctrl.dtsi`. Pin map per `CLAUDE.md` GPIO table.

**#2 ✅ App skeleton**
`app/CMakeLists.txt` (BOARD_ROOT trick), `app/prj.conf` (GPIO + LOG + RTT), `app/src/main.c` doing only LED blink to verify board definition compiles + flashes.

**#3 ✅ LED blink smoke test on real PCB v1.0**
`west build -b sensapulse_v1/nrf52840 -p auto -- -DBOARD_ROOT=...` then `west flash`. Verify LED on P0.03 toggles 1 Hz.

**#4 ✅ I2C + LSM6DSL WHO_AM_I**
Config I2C0 on P0.04 (SCL) / P0.12 (SDA). Read register `0x0F` of LSM6DSL at I²C addr `0x6A`. Expect `0x6A`. Verified — required `bias-pull-up` in pinctrl because PCB v1.0 has no external pull-ups, and forced `I2C_BITRATE_STANDARD` (100 kHz) since internal pull-ups are weak.

**#5 ✅ SPI + micro-SD + FATFS mount**
SPI3 on `P0.17/P0.15/P0.20`, CS `P0.22`, card-detect `P0.26`. Mount FATFS, list root, append boot stamp to `/SD/SP_BOOTS.TXT`. Verified with 30 GB SanDisk card.

**#6 ✅ PDM stereo 16 kHz / 16-bit capture**
`nrfx_pdm` via Zephyr DMIC API. CLK=P1.09, DIN=P0.05. Stereo: ch0=body (MP23DB01HP, falling edge), ch1=ambient (IMP34DT05, rising edge). Save to WAV with canonical 44-byte header. Verified — peaks ~9000, no clipping at 0 dB gain.

**#7 ✅ SAADC battery monitor**
ADC channel on AIN4 (P0.28). VBATT divided 1:2 by R12/R15 (100K/100K). Read in mV, multiply by 2 for VBATT. Classify into full / ok / low / warn / critical. Verified at 4112 mV (Li-ion full).

**#8 🗑️ USB CDC ACM test shell (descoped — RTT is enough)**

**#16 ✅ BLE smoke advertise**
Enable nRF SoftDevice controller. Advertise as `"SensaPulse v1"` (connectable, no GATT services yet). Verify discoverable from nRF Connect for Mobile. Phone connect/disconnect logged via callbacks.

### Feature phase (Milestone A — recording engine)

**#9 ✅ LSM6DSL double-tap detection via INT1**
Configure `TAP_CFG`/`TAP_THS_6D`/`INT_DUR2`/`WAKE_UP_THS`/`MD1_CFG` per AN5040 recipe. Route `DOUBLE_TAP` event to `INT1` pin (P0.06, active high). GPIO interrupt callback → `k_work` → user callback. Currently used to log `*** DOUBLE TAP #N ***`; will toggle record state in #14.

**#10 ✅ Streaming WAV writer + start/stop API**
`audio_recorder_start(path)` / `audio_recorder_stop()` async API. Writer thread (4 KB stack, prio 5) reads PDM blocks via DMIC, writes to file, accumulates byte count. On stop: finalize 44-byte WAV header at offset 0, close file. Verified streaming 14 s = 896000 B exactly (no drops, ~62 kB/s).

**#11 ✅ IMU CSV writer + meta.json**
- New module `imu_sampler.c/h`. Sample LSM6DSL accel + gyro at **52 Hz** via blocking read or hardware FIFO.
- CSV header: `t_us,ax,ay,az,gx,gy,gz`. Raw LSB units, NO calibration in firmware.
- Buffer ~1 s of samples in RAM, then flush in a single `fs_write` call (reduces FATFS overhead).
- `meta.json` written once per session with: `start_rtc_ms` (currently `k_uptime_get()` since RTC isn't wired), `fs_audio=16000`, `fs_imu=52`, `fw_version`, `batt_mv_start`, `build_hash` (commit short SHA), `device_name`, `chip_id`.
- Hook into existing `audio_recorder_start/stop` so all three writers come up/tear down together.
- Smoke test: 30 s recording → check audio.wav + imu.csv + meta.json all on SD, all valid.
- **Verified** with healthy SD card: 0 CRC errors, audio↔IMU sync drift 73 ms, IMU effective rate 51.97 Hz, no embedded NUL bytes. Initial test failed with a worn card masking writes silently — see `docs/TIMING.md` Tooling lessons.

**#12 ✅ Session manager + 10-min rotation**
- New module `session.c/h` owning the lifecycle of a session folder.
- On `session_start()`: pick next sequential id (counter persisted in `/SD/sync_state.json`), `mkdir /SD/SESSION_NNNNN/`, open `audio.wav` + `imu.csv` + `meta.json`, touch `.unsynced` marker.
- Internal timer fires at +10 min: `session_rotate()` — close current, open next. Must be **seamless** (no audio gap > 1 PDM block ≈ 100 ms).
- On `session_stop()` (double-tap): close files, finalize WAV header, write final meta.json fields.
- Free-space management (overlaps with #17 below): pre-flight check before opening new folder. If <100 MB free *and* synced folders exist → evict oldest. If <100 MB *and no* synced folders → enter ERROR state (LED SOS, refuse new recording).
- Tests: trigger rotation manually (shorten timer to 30 s temporarily), verify 2-3 consecutive folders all valid.
- **Verified** with healthy SD card alongside #11. Also added a 1 Hz watchdog (`monitor_work_handler`) that aborts the session if either writer thread dies silently — caught the worn-SD failure mode where audio recorder exited mid-stream but IMU sampler kept running.

**#13 ✅ LED state machine**
- Refactor `led.c/h` from current toggle helpers to `led_set_state(led_state_t)` API.
- States: `IDLE` (1 Hz blink, ~10 % duty), `RECORDING` (solid on), `SYNC` (2 Hz blink, ~50 % duty — preview for v1.1), `LOW_BATT` (5 Hz blink), `ERROR` (SOS Morse — `... --- ...`).
- Implementation via `k_timer` callback firing every 100 ms, walking a state-specific pattern.
- Main thread or peripheral modules call `led_set_state()` to transition; LED module owns actual GPIO writes.
- Smoke test: drive each state from a small test harness, eyeball pattern correctness.
- **Verified** end-to-end alongside #14 on charged battery: LED follows IDLE / RECORDING / LOW_BATT / ERROR transitions with the right cadence by eyeball.

**#14 ✅ Main state machine + integration**
- Replace the current "boot tests then idle blink" main flow with a real state machine:
  - `STATE_IDLE`: LED idle pattern. Listening for double-tap (→ `RECORDING`) and (v1.1) BLE connect (→ `SYNC`).
  - `STATE_RECORDING`: LED solid. Session writers running. Double-tap stops → returns to IDLE. Battery monitor every 30 s; if `< 3300 mV` → auto-stop session and transition to `STATE_LOW_BATT_HOLDOFF` (LED low_batt pattern, ignore taps).
  - `STATE_ERROR`: LED SOS. Reached when SD full + no synced folders, or unrecoverable FATFS error.
- Wire #11/#12/#13 modules together. Verify start/stop via double-tap, rotation seamless, LED follows state.
- **Verified** with charged battery (4098 mV at start). 37.1 s recording: audio↔IMU drift 73 ms, IMU `fs_eff=51.96 Hz`, max IMU sample gap 32 ms, 0 NUL bytes in WAV. Session counter persisted across reboot (`session_id=3`). Build hash from feat branch tip stamped into meta.json correctly.

**#15 ✅ Python session loader (PC tool)**
- `tools/load_session.py` — `load_session(path: Path) -> dict`:
  - `audio: np.ndarray of shape (N, 2), dtype=int16`
  - `fs_audio: 16000`
  - `imu: pd.DataFrame with columns t_us, ax, ay, az, gx, gy, gz`
  - `fs_imu: 52`
  - `meta: dict` (parsed JSON)
- Uses stdlib `wave` + numpy instead of `scipy.io.wavfile` (scipy is a heavy dep we don't need for one read; the macOS dev env doesn't ship it). `pandas.read_csv` for imu.csv.
- `python -m tools.load_session <path>` prints a summary: duration, peak/mean per channel, IMU effective rate, audio↔IMU drift.
- **Verified** against an uploaded test session (SESSION_00002): 46.60 s audio, IMU `fs_eff=51.97 Hz`, drift `+72.6 ms` — matches Cowork's bench-side analysis (73 ms / 51.97 Hz).

---

## v1.1 — BLE sync (full spec in `docs/SYNC_PROTOCOL.md`)

**#17 ⏳ Session marker + persistent counter + free-space eviction**
- Implement `.unsynced` 0-byte marker file written when a new session folder is created (overlaps with #12 — keep them in lockstep).
- Persist next-session-id counter in `/SD/sync_state.json` (read on boot, increment on session create).
- Free-space eviction logic: scan unsynced/synced folders, delete oldest synced when free < 100 MB.
- ERROR-state entry path: SD full + no synced folders → set `state = ERROR`, LED SOS, refuse `session_start`.

**#18 ✅ Device name file + chip-id fallback**
- Read `/SD/device.name` (max 32 bytes UTF-8) on boot. Strip newline.
- If file absent or empty: format chip ID from `NRF_FICR->DEVICEID[0..1]` (two `uint32_t` → 16 hex chars) into `"chip_<hex>"`.
- Expose name + chip_id in BLE `Device Info` characteristic. (BLE wiring deferred to #19; identity_init result is consumed by meta.json now and by Device Info GATT later.)
- BLE `Set Name` characteristic (write): persists to file, takes effect immediately. (Deferred to #19 along with the rest of the GATT.)
- **Verified** end-to-end: scenario 1 (no file → fallback `chip_361e30a6dd726309`) via RTT log; scenario 2 (custom file `Dat-test`) by reading `meta.json` from a real session (`session_id=7`, `device_name="Dat-test"`) — confirms `identity_init()` reads SD and propagates the value into the session metadata.

**#19 ⏳ BLE Sync Service GATT (firmware)**
- Implement custom service per `docs/SYNC_PROTOCOL.md`. Service UUID `7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e`.
- Characteristics: Device Info (read JSON), Control (write+notify, opcodes `LIST/READ/ACK/ABORT/DEL/RESET`), Data (notify only, bulk file stream), Set Name (write).
- On connect: negotiate PHY 2M + MTU 247 + connection interval 7.5 ms.
- Refuse Control ops with status `0x01 busy` if device is `RECORDING`.
- New module `ble_sync.c/h` plus tweaks to `prj.conf` for the BT macros.

**#20 ⏳ State machine: IDLE / RECORDING / SYNC mutual exclusion**
- Update `STATE_*` enum to include `STATE_SYNC`.
- Transitions:
  - `IDLE → RECORDING` via double-tap (existing).
  - `RECORDING → IDLE` via double-tap (existing).
  - `IDLE → SYNC` on BLE connect (new).
  - `SYNC → IDLE` on BLE disconnect (new).
  - `RECORDING → SYNC` forbidden — reject BLE connect with disconnect reason 0x13 or similar.
  - `SYNC → RECORDING` forbidden — drop double-tap events while in SYNC.
- BLE controller off in `RECORDING` (saves power + frees nRF radio for hypothetical noise audit), on in `IDLE`.
- ERROR state still allows `SYNC` (so user can drain unsynced data even when SD is full).

**#24 🚧 SD subsystem isolation stress firmware (diagnostic, not a fix)**

Standalone firmware in `app/sd_stress/` that mounts FATFS and runs 4 phases of synthetic write tests with no PDM, no IMU, no FSM, no BLE. Goal: distinguish whether the empty-session / -EIO bug lives in the SD subsystem alone or only when SD interacts with the rest of the production app.

Phases (auto on boot, no human input):
- Phase 1 `seq_bulk` — 1 MB blocks × 20 = 20 MB at peak rate.
- Phase 2 `sustained` — 4 KB / 25 ms ≈ 160 KB/s for 60 s.
- Phase 3 `dual_audio` + `dual_imu` — 4 KB/25 ms + 256 B/200 ms in parallel for 60 s, mimicking the production writer pair.
- Phase 4 `sustained_synced` — phase 2 with `fs_sync` every 5 s (matches #23).

Final RTT line is a JSON `STRESS_SUMMARY: {...}` consumed by `scripts/parse_sd_stress.py`. `scripts/sd_stress_loop.sh` builds once, then flashes + RTT-captures + parses 10 iterations in a row — fully automatic, ~40 min wall clock.

Decision matrix (handled by `playbooks/sd_stress_isolation.md`):
- 10/10 pass with 0 errors → bug is interaction-class (PDM ↔ FATFS ↔ watchdog). Revert #23 fs_sync; investigate writer-thread race instead.
- ≥1 fail or >1 % write errors → bug is in SD subsystem (driver / FATFS / hardware). Different fix path.
- Latency > 500 ms → SD subsystem stalls long enough to break PDM in production even without errors.

**Why this exists**: 5 iterations of fs_sync / retry / clock-tuning patches on the production firmware moved the failure mode instead of eliminating it. We need an answer to "where" before we try another fix.

**#23 ✅ SD write reliability — empty session bug** (closed by architectural fix #25 — fs_sync band-aid abandoned. Production 25-min 3-rotate test PASS.)

**#24 ✅ SD subsystem isolation stress firmware** (verified SD subsystem clean in isolation; isolated bug to triple-interaction layer; informed #25 redesign.)

**#25 ✅ SD writer refactor — single-thread + 2 FIFO**
- New module `sd_writer.c/h` (~290 lines): one consumer thread owning FATFS, two producer FIFOs (audio_msgq 16 × ptr ≈ 1.6 s slack via PDM mem-slab; imu_msgq 128 × 20 B ≈ 2.5 s slack).
- `audio.c` + `imu_sampler.c` simplified to producers (push slab/struct, never touch FATFS). API renamed `audio_recorder_*` → `audio_producer_*`, `imu_sampler_*` → `imu_producer_*`.
- session.c orchestrates via single `sd_writer_start` / `sd_writer_stop`. Watchdog reads counters via `sd_writer_*_bytes_written` / `*_dropped`.
- PDM_BLOCK_COUNT bumped 8 → 16 (1.6 s slack). Drop-newest backpressure on FIFO full.
- **Stress phase 6**: pre-#25 was 2/5 PASS, post-#25 is 2/2 PASS with max latency drop 380 ms → 92 ms.
- **Production 25-min**: see #27 results. Bundled with #22 (LED + tap tuning).

**#27 ✅ Rotation continuity — folder-prep moved into sd_writer thread**
- `sd_writer_rotate_full(folder, audio_path, csv_path, meta_body, meta_len)` atomically does drain → finalize old → fs_mkdir → write meta.json → open new pair, all from sd_writer thread.
- session.c rotate_work_handler now only computes paths + meta string and queues — no FATFS access on system_work_queue.
- save_counter() removed from rotate path (boot scan_max_session_id() handles recovery).
- **Verified** production 25-min session: SESSION_00003 (38.4 MB), SESSION_00004 (38.39 MB), SESSION_00005 (15.68 MB ongoing). 2/2 rotates gap-free, 0 audio dropped, 0 IMU dropped, FSM never → ERROR.

**#22 ✅ LED + double-tap tuning** (bundled into the data-path fix branch; production-verified during 25-min session)

**#26 ⏳ SD cold-boot init flakiness — CMD0 fail**
- ~60 % of fresh boots fail SD init at CMD0/CMD8. Pre-existing, unrelated to data path. Fix candidates: 100 ms VDD settle delay before sdlog_init, 80 idle SCLK pre-CMD0 per SD spec.

**#28 ⏳ Marker-touch race during rotate — cosmetic cleanup**
- Monitor's `touch_unsynced(new_id)` sometimes fires before sd_writer's `fs_mkdir(folder)` lands — gets `-ENOENT`, self-heals on next 1 Hz tick. Strict effect: 1 `<err>` log line per rotate. Functionally clean.
- Fix: add `sd_writer_is_rotating()` predicate; monitor skips marker check while rotate pending. ~20 lines.

[OBSOLETE — superseded by #25/#27 above; kept for history]


Symptom: ~50 % of sessions land with `audio.wav = 0 B, imu.csv = 0 B, meta.json = 234 B`. Confirmed across both old worn SD card (overnight, sessions 1 + 3) and **new SD card** (today, session 7). Rules out hardware silent corruption — bug is in firmware reliability path.

Three patches landed on this branch:

1. **`audio.c`** — periodic `fs_sync(&rec_file)` every 50 blocks (~5 s), with heartbeat `LOG_INF("recorder: %u blocks, %u B (sync OK)")`. Without sync, FATFS RAM-cached PCM is wiped on writer-thread death or chip reset. Each sync is one SPI burst (~5–20 ms), well under the 800 ms mem-slab slack.
2. **`imu_sampler.c`** — `fs_sync(&csv_file)` after every flush (~1 Hz), with `LOG_INF("sampler: flushed %d samples (total=%u, sync OK)")`. Same rationale.
3. **`session.c`** — atomic `.unsynced` marker. The marker is now touched by the monitor watchdog only after `audio_recorder_bytes_written() > 0` for the current session. Empty sessions never get a marker → BLE LIST naturally skips them. Bonus: monitor logs a tick heartbeat every 5 s with current `audio` + `imu` sample counts so RTT pinpoints exactly when a writer dies.

Verify: build, flash, capture 90 s RTT around a tap-start. Two outcomes: (a) heartbeat logs appear and audio.wav is non-empty after stop → sync was the cause, fix works. (b) heartbeat missing or ends abruptly → writer is dying for a different reason; the new error logs will name the failing call.

**#22 🚧 LED + double-tap tuning (in flight)**
- **LED**: redesign IDLE + RECORDING patterns to "sparse heartbeat" so the LED no longer dominates current draw during 24/7 wear.
  - IDLE: single 100 ms pulse every 3 s (~3.3 % duty, ~0.43 mA avg).
  - RECORDING: double 100 ms pulse with 200 ms gap, then 4.7 s dark (~4 % duty, ~0.52 mA avg). Distinguishable from IDLE at a glance — single vs double pulse.
  - LOW_BATT / ERROR / SYNC unchanged (those states want to be visible/alarming, are short-lived, or only used during BLE sync).
  - Saving: ~25× lower LED current in RECORDING vs old solid-on (13 mA → 0.5 mA). Doubles record-only runtime on a small wearable cell.
- **Double-tap**: raise sensitivity threshold so worn-on-chest motion (walking, clothing rub, body roll) doesn't trip false starts.
  - `TAP_THS_6D` 0x09 → **0x14** (~0.56 g → ~1.25 g, "nấc trung").
  - `INT_DUR2` 0x7F → **0x4E** (DUR=4 → ~307 ms gap, SHOCK=2 → ~19 ms max impulse, QUIET=3 unchanged).
  - Effect: requires deliberate fingertip tap on the case; rejects accidental jolts. Confirm by overnight wear test — if tap still misses, drop to 0x10 (~1.0 g).
- v1.1.1 (planned, not in this task): add BLE Control opcodes `START_RECORD` / `STOP_RECORD` so PC tool can start/stop sessions. Requires BLE controller to stay on during RECORDING — spec change vs current SYNC_PROTOCOL.md, will be addressed when v1.1.1 kicks off.
- **Status**: code changes landed on this branch; awaiting build + flash + RTT verify on PCB v1.0.

**#21 ⏳ PC sync CLI (Python + bleak)**
- Fill in `tools/sync.py` stubs: `cmd_list`, `cmd_read`, `cmd_ack`.
- Atomic transfer: write to `<root>/<device_label>/SESSION_NNNNN.tmp/`, rename to `SESSION_NNNNN/` only when all three files present and ACK succeeded on device.
- `--resume` mode: scan `.tmp/` directories, query device for current file size, resume `READ` from `offset`.
- Logging: structured INFO output, suitable for parsing by the run_loop driver.

---

## Production hardening (from `docs/PRODUCTION_TODO.md`)

Not assigned task numbers yet because they're milestones for after v1.1 ships. Don't start without explicit user direction.

- BLE pairing + bonding (LE Secure Connections)
- Per-device factory key + AES-GCM payload encryption (optional)
- Brown-out detector + watchdog
- FATFS power-fail recovery (`f_sync` after each block)
- Real-time clock initialization (PC sync sets RTC on connect)
- MCUboot + BLE OTA
- GUI sync app (PyQt or Electron)
- Mobile companion (BLE 5.0 Phone — 2M PHY support varies)
- FCC/CE radiated emissions test in final enclosure

---

## How to update this file

When a task moves status:
1. Edit this file inline (change emoji + add a short note about what was verified).
2. If new tasks emerge, append at the bottom of the appropriate section with the next free number.
3. Commit with message `tasks: #N status → done` or similar.

Cowork's in-memory task list is the *historical* source — this file (`TASKS.md`) is now the *living* source.
