# SensaPulse v1.0 — Project Context for Claude Code

You are Claude Code working on SensaPulse v1.0 firmware. This file is your starting context every session. Read it carefully before answering.

## Who you collaborate with

The user (Dat Tran, ITRVN) wears two Anthropic-AI hats in parallel:
- **You (Claude Code)** — runs natively on his Mac M4 with full shell access. You execute build / flash / RTT loops, edit code, run overnight test scripts.
- **Cowork agent** (in the Claude desktop app) — acts as architect/reviewer. Sandboxed bash, no NCS toolchain access, but can read PDFs, analyze WAV files, do high-level planning.

When the user asks for things needing image analysis, schematic reading, or strategy he is **probably better off** taking it back to Cowork. Don't pretend you can read PDFs you can't reach.

## Project summary

Wearable stethoscope worn 24/7. Two PDM mics (body + ambient), 6-axis IMU, micro-SD logging, no BLE in v1 firmware. Goal: collect clean labelable data on SD for offline AI training (heart/lung/cough separation, activity classification).

User profile: biomedical signal-processing engineer, Python-first, has built ECG/PPG models. Firmware (Zephyr/C) is outside his daily work — frame embedded explanations in terms of signal pipelines (DMA = streaming buffer, PDM decimation = front-end filter, etc.). User communicates in Vietnamese. Reply in Vietnamese unless he switches.

## Hardware (PCB v1.0, schematic dated 2026-04-09)

| Block | Part | Connection |
|---|---|---|
| MCU | Ebyte E73-2G4M08S1C (nRF52840) | — |
| Body mic | ST MP23DB01HPTR (PDM, L/R=GND → LEFT, falling edge) | shares PDM_CLK + PDM_DATA with ambient |
| Ambient mic | ST IMP34DT05TR (PDM, L/R=VDD → RIGHT, rising edge) | same |
| IMU | ST LSM6DSLTR (I2C addr 0x6A, SDO=GND) | I2C0 |
| Storage | micro-SD (SPI mode, up to 24 MHz) | SPI3 |
| Power | Li-ion → SW1 slide → TPS7A0333 LDO 3V3 | — |
| LED | active-high via SSM3K44 N-MOSFET | P0.03 |

GPIO assignments (from schematic sheet 5):
| nRF52840 GPIO | Function |
|---|---|
| `P0.03/AIN1` | LED_DRV |
| `P0.04/AIN2` | I2C SCL |
| `P0.05/AIN3` | PDM_DATA |
| `P0.06` | LSM6DSL INT1 (active high; PCB has no external pull-up — use internal) |
| `P0.08` | LSM6DSL INT2 |
| `P0.12` | I2C SDA |
| `P0.15` | SPI MISO |
| `P0.17` | SPI SCK |
| `P0.20` | SPI MOSI |
| `P0.22` | SPI CS |
| `P0.26` | SD CARD_DETECT (active low, 2M pull-up) |
| `P0.28/AIN4` | BATT_LEVEL (VBATT/2 from R12/R15 100K/100K divider) |
| `P1.09` | PDM_CLK |
| `P0.18/RESET` | reset |
| `P0.00`/`P0.01` | XL1/XL2 (32.768 kHz LFXTAL) |

PCB v1.0 known quirks (already worked around in code):
- **No external pull-ups** on I2C SDA/SCL. Pinctrl uses `bias-pull-up`.
- **No external pull-ups** for tap detect either; LSM6DSL drives INT1 push-pull.
- I2C frequency forced to `I2C_BITRATE_STANDARD` (100 kHz) because the internal 13K pull-ups are weak.

## Firmware spec (locked in, do not re-litigate)

- Audio: **16 kHz / 16-bit stereo** via `nrfx_pdm`. nRF52840's PDM peripheral can't do 32 kHz / 24-bit natively; user accepted this tradeoff. Mic SNR is ~64 dB (~10–11 effective bits) so 16-bit is enough.
- IMU: **52 Hz** accel + gyro for activity classification (sit/stand/walk/run/lying/stairs). Not used for respiration.
- SD layout (per session):
  - `audio.wav` (canonical 44-byte header, ch0=body, ch1=ambient, fs=16000, 16-bit)
  - `imu.csv` (header `t_us,ax,ay,az,gx,gy,gz`, raw LSB units, no FW-side calibration)
  - `meta.json` (start RTC ms, fs_audio, fs_imu, FW version, batt_mv_start)
- Time sync: simple — both started from same `k_uptime_get()` tick; audio inferred from PDM clock, IMU from `i*Ts`.
- Trigger: **double-tap** detected by LSM6DSL hardware tap detector → toggles record start/stop. Slide switch SW1 is hardware ON/OFF only.
- Mode: **single firmware image** (no separate test build). Test routines accessible via USB CDC shell when USB connected at boot — but USB CDC was descoped (task #8) since RTT works.
- LED: idle slow blink 1 Hz, recording solid on, low-batt fast blink, error SOS.
- File rotation: new session folder + new audio.wav + imu.csv every 10 minutes.

User explicitly wants **raw** data on SD — no FW filtering or feature extraction. Processing in Python.

## Build / flash / debug

**Toolchain:** nRF Connect SDK v2.9.3 at `/opt/nordic/ncs/v2.9.3/`. Toolchain at `/opt/nordic/ncs/toolchains/b8efef2ad5/`. To get `west` and `arm-zephyr-eabi-gcc` in PATH, source the SDK env or open a terminal from Nordic Toolchain Manager.

**Build (always include the trailing `-DBOARD_ROOT`!):**
```bash
cd /Users/trandat/Project/SensaScope/app
west build -b sensapulse_v1/nrf52840 -p auto -- -DBOARD_ROOT=/Users/trandat/Project/SensaScope
```

Without `-DBOARD_ROOT`, sysbuild's outer cmake reports `Invalid BOARD`. Alternative is `--no-sysbuild` (skips sysbuild wrapper, fine for v1 since no MCUboot).

**Flash:** `west flash` (uses J-Link via nrfjprog by default). The macOS log shows many `JLinkARM.dll reported error -256` lines — these are harmless noise from Nordic-bundled J-Link DLL on Apple Silicon. Look for "flashed successfully" at the bottom.

**RTT logging (capture to file, single command):**
```bash
JLinkRTTLogger -Device NRF52840_XXAA -If SWD -Speed 4000 -RTTChannel 0 logs/rtt.log
```

Stop with Ctrl+C or `pkill -f JLinkRTTLogger`.

**RTT logging (interactive, two terminals):**
```bash
# Tab 1
JLinkExe -device NRF52840_XXAA -if SWD -speed 4000 -autoconnect 1
# inside J-Link prompt: `r` reset, `g` go (release halt), `q` quit
# Tab 2
JLinkRTTClient
```

If RTT shows `V0.0, SN=0` and no log streams: chip is halted, type `g` in Tab 1.

**Helper scripts** (use these, don't reinvent):
- `scripts/build_flash.sh` — atomic build + flash + return code
- `scripts/rtt_capture.sh DURATION_SEC OUTPUT_LOG` — start chip, capture RTT for N seconds
- `scripts/parse_rtt.py LOG_FILE` — extract metrics (boot, battery, PDM peak/mean, recorder bytes, taps, errors)
- `scripts/run_loop.py PLAYBOOK.json` — overnight test driver (loops build → flash → capture → parse → decide)

## Code structure (current state)

```
app/
├── CMakeLists.txt                    # adds all .c modules + BOARD_ROOT
├── prj.conf                          # Kconfig (don't add CONFIG_BOARD_*/CONFIG_SOC_* — auto-set in HWMv2)
├── boards/sensapulse_v1.overlay      # peripheral DT additions (I2C, SPI/SDHC, PDM, ADC)
└── src/
    ├── main.c        orchestrator (state machine — Milestone B onward)
    ├── led.c/h       GPIO wrapper; LED FSM is task #13
    ├── battery.c/h   ADC + state classifier (full/ok/low/warn/critical)
    ├── imu.c/h       LSM6DSL: WHO_AM_I + double-tap on INT1 + (TBD) 52 Hz sampling
    ├── audio.c/h     PDM 16 kHz stereo + canonical 44-byte WAV writer + async streaming recorder
    └── sd_log.c/h    FATFS mount + boot stamp (function prefix `sdlog_` to avoid clashing with Zephyr `sd_init`)

boards/itrvn/sensapulse_v1/
├── board.yml
├── Kconfig.sensapulse_v1
├── Kconfig.defconfig
├── board.cmake                       # required! without it, runners.yaml never generates
├── sensapulse_v1_nrf52840.dts
├── sensapulse_v1_nrf52840.yaml
├── sensapulse_v1_nrf52840_defconfig
└── sensapulse_v1_nrf52840-pinctrl.dtsi
```

## Custom-board gotchas (NCS v2.9 / Zephyr 3.7 HWMv2)

1. `CONFIG_BOARD_<NAME>` is auto-set from `board.yml`. Never put it in defconfig — Kconfig will error "not directly user-configurable".
2. `CONFIG_SOC_*` is also auto-set from `board.yml`'s `socs:`. Don't set in defconfig.
3. `board.cmake` is mandatory — registers `west flash` runners, generates `runners.yaml`. Without it, sysbuild's post-step reads a non-existent file and fails.
4. App-level overlay must be at `app/boards/<board>.overlay` (without the `_<soc>` suffix in v2.9). Confirmed working: `app/boards/sensapulse_v1.overlay`.
5. When build error is wrapped by sysbuild's "FATAL ERROR ... Zephyr project: app", scroll the log up to find the **first** `error:` or `CMake Error:` — that's the real cause.
6. `disk-name` is NOT a property of `zephyr,sdmmc-disk` in NCS v2.9. The driver hardcodes the disk name to `"SD"`.
7. Zephyr `subsys/sd/sd.c` exports a global `sd_init` symbol — name your own SD module functions with a unique prefix (we used `sdlog_`).

## Communicating with the user

- Reply in Vietnamese unless he switches.
- Prefer prose to bullet lists when explaining.
- Be honest about what's risky vs safe (esp. for overnight loops). If a test could leave the board in a bad state, say so.
- For overnight runs, summarize the morning report at the top: did it pass, what's the headline metric, what failed (if anything). Then details.
- The user has a **strong "raw data first"** preference — never propose adding DSP / filtering / on-board feature extraction without checking with him.

## Task list

The canonical, in-tree task list lives in `TASKS.md`. Cowork's in-memory task list is the *historical* source; `TASKS.md` is the *living* source you should read before starting work and edit when you finish or modify a task. Numbering is stable; never renumber.

## Memory

The Cowork agent maintains a longer-form memory across sessions in a separate location. You don't share that memory directly, but this CLAUDE.md is your snapshot of it.

If during a session you discover something stable and surprising (e.g., a hardware quirk, a working register magic value), append a one-paragraph "Discovered:" entry to the bottom of this file before declaring the task done.

## v1.1 — BLE sync feature (planned, not yet implemented)

Adds: PC tool downloads unsynced session folders from device over BLE. Detailed protocol in `docs/SYNC_PROTOCOL.md`. Production hardening checklist in `docs/PRODUCTION_TODO.md` (must do before any unit leaves the bench).

Hard rules:
- Device states are mutually exclusive: IDLE / RECORDING / SYNC.
- BLE only advertises in IDLE. RECORDING turns BLE controller off.
- Each session folder gets a `.unsynced` marker on creation; PC tool sends ACK after full download → marker removed.
- Free-space management: before opening a new session folder, evict oldest *synced* folder if free space low. If all folders are unsynced and SD is full → enter ERROR state (LED SOS), refuse new recording.
- Throughput target: 10 min audio (~38 MB) syncs in ≤ 10 min → require BLE 5.0 PHY 2M + MTU 247 + 7.5 ms connection interval. PC-side only for now (phones often deny ≤15 ms interval).
- Device name: optional, stored in `/SD/device.name`. Falls back to FICR.DEVICEID if absent. PC tool labels folders by name or chip_id accordingly.
- Security: **OPEN for v1.1 dev**. See `docs/PRODUCTION_TODO.md` for the must-do list before shipping.

## Discovered

(Append findings here. Format: ` - YYYY-MM-DD: short fact + one line of context.`)
