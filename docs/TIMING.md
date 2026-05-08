# SensaPulse — Estimate vs Actual Time Tracking

The point of this file: every time we close a task, record how long it actually took versus the estimate. Over time we calibrate Cowork's planning so future estimates get tighter.

## How to log

When a task moves to ✅ in `TASKS.md`:

1. Add a row in the table below.
2. **Estimate** = whatever was last in `docs/SCOPE.md` *before* starting (don't back-fill from after).
3. **Actual** = sum of focused work time (code + debug + verify). Don't count time spent waiting on physical board operations or unrelated discussions. Round to nearest 0.25 h.
4. **Notes** = what caused variance. Especially valuable when actual ≫ estimate.

Units: hours. 1 working day = 8 h. Use decimals (e.g. 0.5 h, 2.5 h).

---

## Bring-up phase (already done — actuals are approximate, reconstructed from session logs)

| # | Task | Estimate (h) | Actual (h) | Variance | Notes |
|---|---|---|---|---|---|
| 1 | Board definition `sensapulse_v1` | 1.0 | ~2.0 | +100 % | Sysbuild BOARD_ROOT propagation, defconfig auto-set rules, missing board.cmake — 4 build/error iterations. Memory file project_sensapulse_build.md captures the gotchas now. |
| 2 | App skeleton (CMakeLists + prj.conf + main.c) | 0.25 | ~0.25 | 0 % | Smooth. |
| 3 | Smoke test on real PCB (LED blink) | 0.5 | ~0.5 | 0 % | J-Link connection on Mac M4 had spurious "DLL error -256" noise but actually works. Documented. |
| 4 | I²C + LSM6DSL WHO_AM_I | 0.5 | ~0.75 | +50 % | PCB v1.0 missing external I²C pull-ups → had to enable `bias-pull-up` + drop to 100 kHz. Worth knowing for v1.1 PCB respin. |
| 5 | SPI + micro-SD + FATFS mount | 0.5 | ~0.75 | +50 % | DTS binding rejected `disk-name` (driver hardcodes `"SD"` in NCS v2.9). RTT buffer overflow on long directory listing → bumped buffer to 4 KB. |
| 6 | PDM stereo capture 16 kHz | 0.5 | ~1.0 | +100 % | Initial WAV header had byte-offset bug (off by 2) — Audacity refused. Fixed inline + patched the captured file. |
| 7 | SAADC battery monitor | 0.5 | ~0.75 | +50 % | Forgot `adc_channel_setup_dt()`. Then user noticed SW1 OFF → reading was leakage on floating VBATT. Real reading after switch ON: 4112 mV ✓. |
| 16 | BLE smoke advertise | 0.5 | ~0.75 | +50 % | `BT_DEVICE_APPEARANCE` Kconfig refused inline comment; `BT_LE_ADV_CONN_FAST_1` doesn't exist in NCS v2.9 (used `BT_LE_ADV_CONN`). |
| 9 | LSM6DSL double-tap detect | 0.5 | ~0.5 | 0 % | Bundled into the module refactor turn. AN5040 register recipe worked first try. |
| 10 | Streaming WAV writer + start/stop API | 1.0 | ~0.75 | -25 % | Refactor went smooth. One linker conflict (`sd_init` collides with Zephyr `subsys/sd/sd.c`) — renamed to `sdlog_*`. |
| **Bring-up subtotal** | | **5.75 h** | **8.0 h** | **+39 %** | Mostly hardware/SDK quirks unknown until they bite. Now captured in CLAUDE.md so v1.0 remaining shouldn't pay them again. |

---

## v1.0 remaining (estimates only — fill in actuals as work completes)

| # | Task | Estimate (h) | Actual (h) | Variance | Notes |
|---|---|---|---|---|---|
| 11 | IMU CSV + meta.json | 3 | 3 | 0 % | Implementation clean. The 73 ms audio↔IMU sync drift and 51.97 Hz effective rate were only visible from post-mortem analysis of the CSV against `audio.wav` — not from RTT logs. Build the verification step into the workflow, not into the firmware. |
| 12 | Session manager + 10-min rotation | 6 | 3 | -50 % | Rotation timer + persistent counter + watchdog all fit in one batch — simpler than the "rotation seamlessness" framing implied because the existing audio + imu writer threads already had the right hooks (mid-loop atomic check, ring-buffered samples). The slack came from #11 having shown the writer-coordination pattern already. |
| 13 | LED state machine | 1.5 | 1.0 | -33 % | Pattern table + 100 ms tick engine simpler than expected. SOS Morse fit in a 32-byte array, no per-state state machine needed. |
| 14 | Main state machine + integration | 3 | 2.0 | -33 % | State enum + transitions wired into existing module APIs (session.c watchdog from #12 already detected aborts; battery_read_mv from #7 already classified). FSM was mostly composition. |
| 15 | Python session loader | 0.5 | 0.5 | 0 % | Stdlib `wave` + numpy + pandas; spec said scipy but scipy is heavy and not in the dev env. Smoke against uploaded fixture matched Cowork's drift / fs_eff numbers exactly. |
| **v1.0 remaining subtotal** | | **14 h** | **9.5 h** | -32 % | |

---

## v1.1 — BLE sync (estimates only)

| # | Task | Estimate (h) | Actual (h) | Variance | Notes |
|---|---|---|---|---|---|
| 17 | Session marker + counter + eviction | 3 | — | — | Overlaps with #12. Some work moves between them depending on order. |
| 18 | Device name + chip-ID fallback | 1.5 | — | — | |
| 19 | BLE GATT Sync Service (firmware) | 24 | — | — | **Highest-risk task.** BLE bulk transfer tuning (PHY 2M + MTU + interval + notify backpressure) is empirical. Could be 16 h if first try succeeds, 32 h if throughput tuning fights us. |
| 20 | State machine SYNC | 1.5 | — | — | |
| 21 | PC sync CLI (Python + bleak) | 6 | — | — | |
| **v1.1 subtotal** | | **36 h** | | | |

---

## Aggregate

| Phase | Estimate (h) | Actual (h) | Days est. | Days actual |
|---|---|---|---|---|
| Bring-up | 5.75 | 8.0 | 0.7 | 1.0 |
| v1.0 remaining | 14 | — | 1.75 | — |
| v1.1 | 36 | — | 4.5 | — |
| Integration + stress + fixes | 8 | — | 1.0 | — |
| **Project total** | **~64 h** | — | **~8 working days** | — |

(8 working days = 1.5–2 calendar weeks, accounting for non-coding time.)

---

## Calibration log

After each phase closes, update this section with what we learned:

### Bring-up phase (closed)
- **Estimate accuracy**: ~ +40 % under-estimated. Each task averaged ~1.4× the planned time.
- **Source of variance**: SDK / hardware quirks not documented anywhere, only discoverable by hitting the error. After capturing them in `CLAUDE.md`, future tasks should pay this cost less often.
- **Action item**: For v1.0 remaining and v1.1, multiply Cowork's first-pass estimate by 1.3× as a default safety margin until we accumulate ≥ 5 closed tasks of new data.

### Tooling lessons (cumulative — read before authoring playbooks)

- **JLinkRTTLogger standalone is not reliable on macOS Apple Silicon.** It connects to the J-Link, but its auto-search for the SEGGER RTT control block silently fails (reports `RTT Control Block not found`) even when the block is present at a normal SRAM address (verified: `_SEGGER_RTT` at `0x20001010`, magic `"SEGGER RTT"` correct, WrOff advancing). Symptom looks like firmware crash but isn't. Cost so far: ~0.75 h on task #11 chasing a ghost.
- **Use the 2-process pattern instead** (`scripts/rtt_capture.sh`): keep `JLinkExe` alive in the background as a gateway (`-RTTTelnetPort 19021`), then attach `JLinkRTTClient` to that gateway. The client reads from the gateway's already-discovered RTT block and just works.
- **macOS ships no GNU `timeout`** by default. For bounded captures use the background-`&` + `sleep` + `kill` pattern, not `timeout DURATION cmd`. Either install `coreutils` (`gtimeout`) or stick with the manual pattern — `rtt_capture.sh` does the latter.
- **Implication for overnight playbooks**: any `run_loop.py` step that captures RTT must go through `scripts/rtt_capture.sh`, never call `JLinkRTTLogger` directly. Document this in `playbooks/README.md` if a contributor reaches for the lower-level tool.
- **SD card hardware failure modes are diagnosable only by RTT log + post-mortem file analysis.** During #11/#12 a worn card produced 2.4 s of `audio.wav` while `imu.csv` ran 50 s — symptom looked like a firmware race, was actually `dmic_read`/`fs_write` failing under FATFS retries with the card silently corrupting writes. Always check (1) CRC error count exposed by the card, (2) embedded NUL runs in the captured WAV, (3) audio↔IMU sync drift, when verifying long recordings. Old/worn cards can fail without surfacing any error code from FATFS. Mitigations now in firmware: writer-death watchdog (session.c) + bigger PDM mem-slab (audio.c, ~800 ms slack). Production-side mitigation lives in `docs/PRODUCTION_TODO.md` § Reliability.

### v1.0 remaining (closed)

- **All 5 tasks closed**: #11 = 0 %, #12 = -50 %, #13 = -33 %, #14 = -33 %, #15 = 0 %.
- **Phase variance**: -32 % (9.5 h actual vs 14 h estimate). Bring-up was +39 %; v1.0 remaining was -32 %. The flip makes sense: bring-up paid for SDK/hardware unknowns once; v1.0 remaining was almost pure composition over already-stable APIs.
- **Calibration update**: drop the 1.3× safety margin for tasks plugging into stable module interfaces. Keep it (and add a hardware-debug budget line) for tasks that introduce a new peripheral or a new SDK subsystem (PDM, BLE, USB).
- **For v1.1**: BLE GATT (#19, est. 24 h) is the only task that introduces new SDK surface area. Apply 1.3× to that one and leave the rest at 1.0×.
- **Hardware-debug iterations were a separate budget line.** Worn-SD chase cost ~1 h, J-Link RTT chase ~0.75 h. Both are now captured in `Tooling lessons` so future-us pays once.

### v1.1 (open)

(Fill in when closed.)

---

## Update protocol

- **Cowork** owns the estimates column (writes them when planning a task).
- **Whoever closes a task** (Claude Code or human) writes the actual + notes. The closer commits to git in the same commit as the code.
- Don't fudge actuals. If a task took 6 h instead of 2 h, that's the data we need.
- If estimate variance > 50 % three times in a row in the same task class (e.g. BLE work), update Cowork's planning by editing this file's "Calibration log" section so future estimates absorb the lesson.
