# SensaPulse Firmware — Scope & Estimates

Use this as a one-page brief for non-technical reviewers or new developers picking up the project. The detailed task list is in `TASKS.md`; the architectural decisions are in `CLAUDE.md`.

## v1.0 — local recording firmware

### What it does
A wearable stethoscope (PCB v1.0, nRF52840) records two-channel PDM audio (16 kHz / 16-bit, body + ambient mic) plus 6-axis IMU (52 Hz) to a micro-SD card, organized as 10-minute session folders. The user double-taps the device to start/stop a recording session. No BLE, no cloud — all data stays on the SD card.

### Output on SD per session

```
/SD/SESSION_NNNNN/
    audio.wav          stereo 16 kHz / 16-bit PCM, ~38 MB per 10 min
    imu.csv            t_us,ax,ay,az,gx,gy,gz at 52 Hz, ~50 KB per 10 min
    meta.json          fs_audio, fs_imu, fw_version, batt_mv_start, etc.
    .unsynced          0-byte marker (kept until v1.1 PC sync)
```

### Hardware verified working
- LED indicator, ADC battery monitor, I²C IMU (LSM6DSL), SPI micro-SD (FATFS), PDM stereo audio (DMIC + 16 kHz/16-bit), BLE LL controller (advertise + connect from phone).

### Components remaining for v1.0
| # | Component | What's left | Estimate |
|---|---|---|---|
| 11 | IMU CSV writer | 52 Hz sampler thread + buffered CSV writer + meta.json output | 2–4 hours |
| 12 | Session manager + 10-min rotation | Folder lifecycle, persistent session counter, seamless rotation, ERROR state on SD full | 0.5–1 day |
| 13 | LED state machine | k_timer-driven patterns: idle / recording / low-batt / error | 1–2 hours |
| 14 | Main state machine + integration | Wire double-tap ↔ session start/stop, battery auto-stop, error handling | 2–4 hours |
| 15 | Python session loader | `tools/load_session.py` returning numpy/pandas for ML pipeline | ~30 min |

**v1.0 effort: ~2–3 working days.** Foundation modules (audio recorder with start/stop API, IMU probe, SD mount, LED helper, battery monitor) are already in place from the bring-up phase, so each remaining task is composition + tests rather than from-scratch work.

---

## v1.1 — BLE PC sync

### What it adds
A PC-side Python tool connects to the device over BLE and downloads unsynced session folders to the developer's laptop. Each device shows up as a folder named after the user-set device name (or chip ID fallback). Once a session is fully downloaded and ACKed back to the device, its `.unsynced` marker is removed. If the SD card fills up and there are still unsynced folders, the device refuses to record any more — protecting the data already captured.

### Concurrency rules
- Device states are mutually exclusive: `IDLE` / `RECORDING` / `SYNC`.
- BLE advertises only in `IDLE`. `RECORDING` turns BLE off (saves power, frees the radio).
- PC can only sync when device is idle. Sync attempts during recording are rejected with `busy`.

### Throughput goal
A 10-minute session (~38 MB) syncs in ≤ 10 minutes. This requires BLE 5.0 PHY 2M + 247-byte MTU + 7.5 ms connection interval. PC-side support is reliable; phones often refuse intervals < 15 ms, so v1.1 targets PC only.

### Security in v1.1
**Open / unauthenticated** for development. Production hardening checklist (BLE pairing, AES-GCM payload, etc.) is captured in `docs/PRODUCTION_TODO.md` and must be done before any unit ships.

### Components for v1.1
| # | Component | What it does | Estimate |
|---|---|---|---|
| 17 | Session marker + persistent counter + free-space eviction | `.unsynced` marker, `sync_state.json` next-id counter, evict oldest synced folder on SD-full | 2–4 hours |
| 18 | Device name + chip-ID fallback | Read `/SD/device.name`, fall back to FICR.DEVICEID hex | 1–2 hours |
| 19 | BLE GATT Sync Service (firmware) | Custom GATT service: Device Info / Control / Data / Set Name. PHY 2M + MTU 247 + 7.5 ms negotiation. LIST/READ/ACK/ABORT/DEL opcodes. | **2–4 days** (single biggest risk) |
| 20 | State machine: IDLE / RECORDING / SYNC | Mutual-exclusion transitions, BLE on/off per state, reject conflicting requests | 1–2 hours |
| 21 | PC sync CLI (Python + bleak) | Scan, connect, list, download with atomic rename, resume mode | 0.5–1 day |

**v1.1 effort: ~3–5 working days.** Task #19 dominates because BLE bulk transfer optimization is genuinely hard work — connection-interval negotiation has device-specific quirks (host can refuse 7.5 ms), notify backpressure tuning is empirical, and reassembly/offset logic must handle disconnects. Other v1.1 tasks are short.

---

## Combined estimate

| Phase | Lower bound | Upper bound (with debugging surprises) |
|---|---|---|
| v1.0 remaining work | 2 days | 3 days |
| v1.1 | 3 days | 5 days |
| Integration / overnight stress / fixes | 1 day | 2 days |
| **Total** | **6 working days (~1.5 weeks)** | **10 working days (~2 weeks)** |

This assumes the current development model: Claude Code executes the build/flash/test loop on the user's Mac, Cowork (this assistant) supplies architectural decisions and reviews artifacts, and the user is available to plug/unplug the board between iterations.

If the developer is new to Zephyr / nRF Connect SDK and works without AI assistance, multiply by ~3× — the ramp-up alone takes a week, and each peripheral integration takes a day instead of an hour.

Major risks that could blow up estimates:
- **BLE throughput not hitting 64 KB/s on real hardware** → may need protocol tweaks (larger MTU, batched notify, different framing) or accept slower sync. Adds 1–2 days to #19.
- **FATFS power-fail corruption** discovered under stress test → need `f_sync` after each block, possibly journaled FS. Adds 1–2 days.
- **LSM6DSL polling can't sustain 52 Hz** under audio I/O load → switch to FIFO + interrupt-driven read. Adds 0.5 day.

---

## Out of scope (explicitly)

- Real-time audio streaming over BLE to a mobile app (deferred to v2 — needs custom BLE 5.0 audio service, complex).
- On-device cough detection / counting (deferred to v3 — needs trained AI model + on-device inference).
- OTA firmware update via BLE (deferred — see `docs/PRODUCTION_TODO.md`).
- Mobile companion app (deferred — PC sync is sufficient for the data-collection phase).
- Real-time clock sync from a connected device (deferred — `meta.json` uses uptime for now).
- Activity classification on device (deferred — done offline in Python from `imu.csv`).

---

## Hardware status

- PCB v1.0 verified end-to-end (May 2026 batch). All peripherals working with known firmware-level workarounds documented in `CLAUDE.md`.
- PCB v1.1 (next respin) needs: external pull-ups on I²C SDA/SCL, external pull-up on LSM6DSL INT1, optional USB connector for fast bulk dump. See `docs/PRODUCTION_TODO.md` § Hardware.
