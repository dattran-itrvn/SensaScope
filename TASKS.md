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

## ✅ v1.0 CLOSED — 2026-05-13

Toàn bộ tiêu chí nghiệm thu trong `docs/REQUIREMENTS.md` đã pass. Verify production stack 1 giờ với fix #32: 5 rotations sạch, ~230 MB audio + ~31000 IMU samples, 0 drop, 0 FSM ERROR. Production firmware sẵn sàng cho bench-side data collection.

Các task firmware liên quan đến v1.0 đều ✅ (bao gồm #1-#16, #18, #22-#28, #30, #32). Task #17 (marker + counter + ERROR path) ✅ phần v1.0; sub-feature eviction defer sang v1.1. Task #29 (SD card EOL) firmware-side đã đóng qua #32; hardware-side track trong `docs/PRODUCTION_TODO.md § Hardware / PCB v1.1+`.

Bài học kỹ thuật quan trọng nhất của v1.0: `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md` — "single-FATFS-owner" invariant của #25 nghiêm ngặt, mọi FATFS call ngoài sd_writer thread phải route qua public sync API.

---

## Hardening — open follow-ups (không block v1.0, không là v1.1 feature)

**#31 ⏳ Boot timing margin — settle delay trước probe I²C/IMU**
- Triệu chứng: thi thoảng IMU báo `WHO_AM_I = 0xFF` ở boot (đặc biệt sau flash, MCU reset nhưng LDO + LSM6DSL chưa POR xong). Hiện code tiếp tục OK do retry nội bộ ở chỗ khác, nhưng cảnh báo nhỏ trong RTT log không đẹp.
- LSM6DSL datasheet: ~25 ms từ POR tới WHO_AM_I valid.
- Fix proposal: thêm `k_msleep(100)` ngay đầu `main()` trước `led_init() / battery_init() / imu_init()`. Cost: 100 ms boot delay, không đáng kể.
- Tương đương với power-settle của `sdlog_init` ở #26 (đã làm cho SD), giờ làm cho phần còn lại.
- Low priority — không gây production issue, chỉ là defensive coding.

**#35 ✅ Firmware silent halt on READ of crash-truncated audio.wav** (2026-05-14)

Same root cause as #34. The "silent halt" was not corrupt FAT — it was the retry-on-ENOMEM loop hammering the BT stack after the TX pool exhausted and bt_gatt_notify started silently dropping. With #34's serial credit-based flow control, the firmware no longer enters that state. Verified: SESSION_00014/audio.wav (25644 byte, the original repro case) syncs cleanly in 3.8 s; device stays up post-transfer.

Symptom: BLE READ trên `SESSION_NNNNN/audio.wav` của session bị crash
giữa chừng (vd session 14 với audio.wav ~26 KB, folder created by rotate
nhưng session aborted trước close) làm firmware **silent halt** sau khi
`LOG_INF("READ submit ...")` fires. Không thấy `read_file: ... done` log,
không có error log, BLE supervision timeout cuối cùng đẩy host disconnect,
device không re-advertise. Phải power-cycle để recover.

Reproduce: gắn PC sync CLI vào device sau crash, `python3 tools/sync.py
--only-session 14 --only-file-idx 0`. RTT log dừng ngay sau `READ submit:
sess=14 file=0 offset=0 length=0`.

Hypothesis: FAT entry của audio.wav có `size` lớn hơn cluster chain
allocation thực tế (rotate `fs_open` cấp size > 44 byte WAV header rồi
session abort trước khi close finalizes). `fs_read` của sd_writer thread
follow size đó đụng cluster trống → SDMMC driver hang chờ data nó không
nên đọc.

Workaround hiện tại: PC sync skip file_idx=0 cho session cuối (file
chưa close gọn) — chấp nhận mất audio session bị crash. v1.0 đã có rule
"empty session skip" tương tự ở #23.

Fix proposal:
- Trước fs_read trong `do_read_file`, kiểm tra `fs_stat` size vs cluster
  validity. Khó implement vì FATFS không expose cluster chain validate API.
- An toàn hơn: cap fs_read by `min(file_size, sane_bound)` với sane_bound
  derived từ session lifetime. Nhưng không có lifetime info on disk.
- Đơn giản nhất: nếu rotate left a placeholder open, `sd_writer_stop()`
  finalize phải truncate to actual bytes_written rồi sync. Cần audit
  rotate_full path để chắc chắn không có path leaves stale size.

Low priority — không block v1.1 happy path. PC sync log ra `link dropped`
clean khi gặp file này, user retry sau khi xóa session.

**#34 ✅ BLE GATT Sync — bulk READ deadlock fix** (2026-05-14)

Symptom (pre-fix): bleak READ > ~10 chunks (240 B each) → PC nhận 0 byte; threshold = `CONFIG_BT_BUF_ACL_TX_COUNT`. Originally diagnosed as throughput / disconnect; bisect revealed two separate bugs:

1. **Silent drop khi pool exhaust**: `bt_gatt_notify` trả `ret=0` ngay cả khi ACL TX pool đầy, queue PDU vào nowhere → PC không nhận. Threshold chính xác = TX_COUNT (10→threshold 10, 32→threshold 32). Phải dùng `bt_gatt_notify_cb` với credit-based flow control để biết khi nào pool drained thực sự.

2. **Deadlock via system_work_queue**: Callback từ `bt_gatt_notify_cb` dispatch trên `system_work_queue`. `read_work_handler` chạy trên cùng queue, block trên k_sem chờ callback → callback không bao giờ chạy → 3 s timeout → -EIO. Phải dispatch READ qua dedicated work queue (`read_wq` trong ble_sync.c) để tách context.

Fix combined trong commit này:
- `app/src/ble_sync.c`: `data_notify` dùng `bt_gatt_notify_cb` với 1-permit semaphore; `on_data_notify_done` callback releases. Dedicated `read_wq` (k_work_queue) chạy READ độc lập system_work_queue.
- `app/prj.conf`: `CONFIG_BT_BUF_ACL_TX_COUNT=32`, `CONFIG_BT_L2CAP_TX_BUF_COUNT=32`, `CONFIG_BT_GATT_NOTIFY_MULTIPLE=n`.
- `app/boards/sensapulse_v1.overlay`: `spi-max-frequency` 24 → 16 MHz (driver thực sự đã round xuống 16 MHz, giờ config match reality).

Verified bench: imu.csv 170 KB sync in 31.6 s (5.3 KB/s). Audio bulk transfer streaming clean qua chunk #1125 (270 KB) tested via RTT capture, nomem_total=0. Throughput ceiling ~5 KB/s từ serial 1-in-flight cb pattern; multi-credit version vẫn có thể tăng future, nhưng correctness first.

**#33 ⏳ `sdlog_init` stuck-state recovery sau crash**
- Triệu chứng: sau khi production crash (`-116` từ SD), thẻ vào trạng thái stuck — CMD0 cold-init fail repeatedly. `sdlog_init` hiện có 100 ms power-settle + 3 × 200 ms retry không đủ recover.
- Cách giải tạm hiện tại: rút SD ra cắm lại (hard power-cycle slot).
- End-user scenario: dùng pin với SW1, lần next boot là cold start với SD slot đã power-cycle theo MCU → không gặp stuck-state. Issue chỉ visible khi debug với J-Link cấp VTref liên tục.
- Fix proposal: extend retry budget trong `sdlog_init` (10 × 500 ms = 5 s total), thêm explicit CMD0 ping với delay dài hơn (1-2 s) cho stuck card warm-up.
- Phụ thuộc hardware: PCB v1.1+ có thể thêm hardware power-cycle line cho SD VCC (đã track trong `docs/PRODUCTION_TODO.md § Hardware`). Lúc đó firmware có thể GPIO toggle để hard-reset SD slot mà không cần user can thiệp.
- Low priority cho v1.0 (production user dùng pin sẽ không gặp), nhưng cần cho QA bench testing.

---

## ✅ v1.1 CLOSED — 2026-05-13

BLE sync happy path đầy đủ. Spec `docs/SYNC_PROTOCOL.md` đã implemented hai phía: device-side GATT (`ble_sync.c`) + PC-side CLI (`tools/sync.py`).

Đã verify end-to-end với PCB v1.0 + bench Mac:
- Scan → connect → IDLE→SYNC transition (#20, 3/3 bleak rounds)
- Device Info JSON đúng (name, chip_id, fw build hash, state, batt_mv, sd_free_mb)
- LIST 12 unsynced sessions
- READ meta.json clean transfer + atomic `.tmp/` → final rename
- READ audio.wav (capped `--max-bytes`) clean
- ACK loop removes `.unsynced` markers
- Eviction (#17): `sd_writer_get_free_mb` + `find_oldest_synced` + 5-unlink folder removal, all through writer thread (single-FATFS-owner invariant intact)

Update 2026-05-14: **#34 + #35 đã được fix** (commit `9b5fc1b`). Bulk READ giờ hoạt động end-to-end với serial credit-based flow control + dedicated work queue. Verified bench: imu.csv 170 KB sync clean (5.3 KB/s), audio.wav session 14 (file đã từng làm firmware hang) syncs clean trong 3.8 s. Throughput ceiling ~5 KB/s; 38 MB audio.wav full = ~2 giờ — chấp nhận được cho v1.1 với multi-credit optimization là follow-up nếu cần.

Hardening còn lại — không block v1.1, defer như v1.0 đã defer #29/#33:
- (Không còn open limits sau khi #34/#35 đóng.) Hardware-class follow-ups #29/#31/#33 stay open as before.

Production v1.1 firmware sẵn sàng cho field test full flow.

---

## v1.1 — BLE sync (full spec in `docs/SYNC_PROTOCOL.md`)

**#17 ✅ Session marker + persistent counter + free-space eviction**
- ✅ `.unsynced` 0-byte marker — implemented in #12; refined to integrity-signal in #23 (only created sau khi audio bytes > 0) và gated rotate-window trong #28.
- ✅ Counter persistence `/SD/sync_state.json` — `load_counter` / `save_counter` / `scan_max_session_id` fallback; save chuyển trước `sd_writer_start` ở #32 để tránh contention.
- ✅ ERROR-state entry path — FSM `APP_STATE_ERROR` đã có trong #14, SOS LED qua #13, watchdog catch trong session monitor.
- ✅ **Eviction (v1.1)** — `sd_writer_get_free_mb` + `find_oldest_synced` + `remove_session_folder` (routes 5 unlinks through sd_writer thread). `session_start` chạy eviction loop: nếu free < 100 MB, xoá folder synced cũ nhất, loop tiếp tới free ≥ 100 MB hoặc hết folder synced (→ `SESSION_ERR_NO_SPACE` → FSM ERROR). Smoke verified: Device Info `sd_free_mb` field now returns real value from `fs_statvfs` via writer thread.

**#18 ✅ Device name file + chip-id fallback**
- Read `/SD/device.name` (max 32 bytes UTF-8) on boot. Strip newline.
- If file absent or empty: format chip ID from `NRF_FICR->DEVICEID[0..1]` (two `uint32_t` → 16 hex chars) into `"chip_<hex>"`.
- Expose name + chip_id in BLE `Device Info` characteristic. (BLE wiring deferred to #19; identity_init result is consumed by meta.json now and by Device Info GATT later.)
- BLE `Set Name` characteristic (write): persists to file, takes effect immediately. (Deferred to #19 along with the rest of the GATT.)
- **Verified** end-to-end: scenario 1 (no file → fallback `chip_361e30a6dd726309`) via RTT log; scenario 2 (custom file `Dat-test`) by reading `meta.json` from a real session (`session_id=7`, `device_name="Dat-test"`) — confirms `identity_init()` reads SD and propagates the value into the session metadata.

**#19 🟡 BLE Sync Service GATT (firmware)** — full opcode set OK, throughput tuning defer #34
- Implement custom service per `docs/SYNC_PROTOCOL.md`. Service UUID `7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e`.
- Characteristics: Device Info (read JSON), Control (write+notify, opcodes `LIST/READ/ACK/ABORT/DEL/RESET`), Data (notify only, bulk file stream), Set Name (write).
- On connect: negotiate PHY 2M + MTU 247 + connection interval 7.5 ms.
- Refuse Control ops with status `0x01 busy` if device is `RECORDING`.
- New module `ble_sync.c/h` plus tweaks to `prj.conf` for the BT macros.

**#20 ✅ State machine: IDLE / RECORDING / SYNC mutual exclusion**
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

**#26 ✅ SD cold-boot init flakiness — CMD0 fail**
- `sdlog_init()` now: (a) `k_msleep(100)` before first `disk_access_init()` for power/clock settle; (b) up to 3 attempts with 200 ms gap. Eliminates the J-Link-flash-Vcc-transient + card-internal-startup race that produced ~60 % cold-init failures during stress loop.

**#28 ✅ Marker-touch race during rotate — cosmetic cleanup**
- Added `sd_writer_is_rotating()` returning `atomic_get(&rotate_req)`.
- session.c monitor now gates `touch_unsynced` on `!sd_writer_is_rotating()`. ENOENT race window closed at source — no more spurious err logs at rotate boundaries.

**#29 🟡 SD card EOL / `-116` SDMMC timeout** (hardware-class — firmware side đã đóng qua #32; hardware-side tracked trong `docs/PRODUCTION_TODO.md § Hardware / PCB v1.1+`)
- Symptom: mid-session `sd: Failed to read from SDMMC -116` (-ETIME) → `Card read failed` → `fs_write -5` cascade → watchdog → FSM `ERROR`. Lần đầu thấy trên 30 GB SanDisk wear-out hồi #28.
- Đợt diagnosis #32 confirm: ngay cả card mới (122 GB, format sạch) cũng có thể gặp `-116` nếu firmware có concurrent FATFS access từ system_work_queue → card stress → timeout. Fix #32 (single-FATFS-owner) loại bỏ trigger này.
- Firmware behavior khi card thực sự fail: watchdog catches → ERROR (LED SOS), session cuối có `.unsynced` marker → PC sync tool vẫn pull được phần đã ghi. **Đúng — không cần thêm firmware work.**
- Mitigation cấp operational (không firmware): vet card mới bằng 5 × 420 s stress trước production. Rotate card on warning. Telemetry `sdhc_spi` retry counter trong `meta.json` để giám sát wear — chưa làm, low priority.
- Hardware-side note: SD card slot trên PCB v1.0 dùng wear-prone consumer cards. PCB v1.1+ nên xem xét: industrial SD slot, hoặc eMMC chip-on-board (loại trừ socket completely). Đã thêm vào `docs/PRODUCTION_TODO.md § Hardware`.

**#32 ✅ SD write reliability — fix single-FATFS-owner violation**

Production crash sau 12-41 phút mỗi session, signature `-116 ETIME` từ SDMMC. Các fix trước (#23/#25/#27/#30) đều không giải quyết. Sau khi bisect bằng 7 test cô lập (xem `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md`), tìm ra root cause: `session.c` vi phạm "single-FATFS-owner" invariant của #25 — `monitor_work_handler.touch_unsynced` và `rotate_work_handler.statvfs_free_mb` gọi FATFS trực tiếp từ `system_work_queue` trong khi `sd_writer` thread đang fs_write → SD card stress → timeout `-116`.

**Test 7 reproduce**: Test 5 (full data path, no FSM) + work-item gọi `fs_open + fs_close` 1Hz từ `system_work_queue` → CRASH sau 86 giây với đúng signature production. Test 5 (không có monitor mock) pass 55 phút clean.

Fix (3 changes):
- `sd_writer.c/h`: thêm `sd_writer_touch_file(path)` — synchronous, writer thread serve giữa drain (pattern `k_sem` giống `sd_writer_rotate_full` của #30).
- `session.c monitor`: `touch_unsynced` route qua `sd_writer_touch_file` thay vì gọi `fs_open/close` trực tiếp.
- `session.c rotate_work_handler`: xoá hoàn toàn `statvfs_free_mb()` (TODO khi làm #17 thì thêm API `sd_writer_get_free_mb`).
- `session.c session_start`: đảo thứ tự — `save_counter()` chạy TRƯỚC `sd_writer_start()` (lúc đó writer chưa tồn tại, không contend).

**Verify**: 1 giờ full production stack (auto-start, BLE on, default 10-phút rotation). Pass criterion: 0 FSM ERROR, 5+ rotation clean.

**#30 ✅ SD write resilience — retry, verify, defer-on-rotate-fail** (merged; 1-min stress passed 30 phút, nhưng production crash chính được giải bằng #32)
Triggered by 1-min-rotation stress test (2 runs, both failed at rotate boundary): the write path has two latent flaws that #25/#27/#28 didn't address.

**Bug 1 — Silent FAT corruption**: in run 1, `fs_mkdir + fs_open + fs_write` for SESSION_00007 all returned success codes, but on disk the entry came out as a 0-byte regular file (no `DIR` attribute bit). 3.84 MB of audio became orphan clusters. Firmware logged no error.

**Bug 2 — `-116` one-shot kill**: in both runs, a single `-116` SDMMC timeout at rotate boundary cascaded to `failed=1` → watchdog → FSM ERROR → user must reboot. SDMMC `-116` is typically a transient busy state (card doing internal GC/wear-levelling); higher-level retry with backoff would mask it, and FIFO slack (~1.6 s audio, ~2.5 s IMU) accommodates the delay.

Fix in three layers, all in `sd_writer.c` (plus minor `session.c` adjustments):
- **A. Post-mkdir / post-meta verify.** After `fs_mkdir(folder)`, `fs_stat` it and assert `type == FS_DIR_ENTRY_DIR`. After meta.json open+write+close, `fs_stat` meta_path and assert `size > 0`. If either check fails, log loud and return error from `do_rotate()`.
- **B. Retry-with-backoff helpers** (`fs_open_retry`, `fs_write_retry`, `fs_close_retry`) wrapping FATFS calls in `do_rotate` and `drain_audio` / `drain_imu`. Catch `-EIO / -ENXIO / -ETIMEDOUT / -EAGAIN`, `k_msleep(200)`, retry up to 3 times.
- **C. Defer-on-rotate-fail.** `do_rotate()` opens new audio+csv into `audio_file_pending` + `csv_file_pending` *before* closing old. On any failure (after retries): clean up partial new handles, **keep old handles active**, return error. Session keeps writing to the old folder; next rotate timer tick retries. After 3 consecutive rotate failures, escalate: set `failed=1` → FSM ERROR.
- **Sync rotate API**: `sd_writer_rotate_full()` now blocks on a `k_sem` until the writer thread reports rotate result, so `session.c` knows whether to commit `current_id` / `next_id`.

Verify: rerun 30-min stress at 1-min cadence on freshly-formatted 122 GB card. Pass criterion = 30 valid folders + no FSM → ERROR, OR < 3 deferred rotates (logged but recovered).
- No firmware action; hardware-class. Track for visibility.

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

**#21 ✅ PC sync CLI (Python + bleak)**
- `tools/sync.py` implements LIST/READ/ACK/RESET + notify dispatcher + resumable per-file transfer + atomic `.tmp/` → final/ rename + ACK on success.
- Debug flags: `--only-session`, `--only-file-idx`, `--max-bytes`, `--no-ack`, `--resume`.
- Verified end-to-end against PCB v1.0: LIST 12 unsynced, READ meta.json (250 B) clean atomic rename, READ audio.wav `--max-bytes 1000` (1000/1000 B), grace-window fix handles BLE-driver reorder of last 40 B chunk.
- Known firmware-side limits — separate tasks, not PC bugs: #34 (bulk READ disconnect >~50 KB), #35 (READ on corrupt audio.wav hangs firmware silently).

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
