# Overnight stability test — v1.0 recording engine

**Purpose:** confirm v1.0 firmware survives 8 hours of continuous recording (~48 sessions × 10 min rotation) with no SD corruption, no IMU rate drift, no FATFS errors. This is the integration gate before kicking off v1.1 (BLE sync) work.

This is a **manual procedure** rather than `run_loop.py` because v1.0 requires a physical double-tap to start a session — `run_loop.py` can only flash + reboot, it can't tap. The post-mortem analyzer (`tools/analyze_overnight.py`) walks the SD card and produces a report.

## Pass criteria

| Check | Threshold |
|---|---|
| Number of sessions | 48 ± 2 (8 h ÷ 10 min, last one may be partial) |
| Per-session audio duration | 600 s ± 5 % (last session can be shorter) |
| Per-session IMU effective rate | 52 ± 0.5 Hz |
| audio↔IMU drift per session | < 200 ms |
| NUL bytes in any audio.wav or imu.csv | 0 |
| `<err> sdhc_spi: Bad data CRC` count in RTT | 0 |
| `<err> recorder: ...` or `<err> imu_sampler: ...` | 0 |
| Battery drop | < 80 % (informational, not pass/fail) |

## Procedure (J-Link unplugged for the long run)

**Important context:** the J-Link probe sources VTref current to the target during normal operation, which means as long as J-Link is plugged in, the board is powered by the host USB rail and the battery does not drain. To measure real battery life we have to **unplug J-Link** for the bulk of the test. This costs us live RTT visibility during those hours, but `meta.json` per session + SD-side file integrity is enough to verify pass/fail post-mortem.

### Tonight (kick-off)

1. **Charge battery to ≥ 95 %** (4150 mV+ at TP11 with voltmeter, or check the boot RTT line `Battery: NNNN mV (full)`).
2. **Format the SD card** to FAT32 on Mac so the session counter resets cleanly (Disk Utility → Erase → MS-DOS FAT). Optional but makes the analyzer's session list start at 1.
3. **Plug battery**, gạt **SW1 → ON**, J-Link cắm.
4. **Reset board**, verify boot via short RTT capture:

   ```bash
   cd /Users/trandat/Project/SensaScope
   bash scripts/rtt_capture.sh 5 logs/preflight_$(date +%Y%m%d).log
   grep -E "Idle\. Double-tap|Battery:" logs/preflight_*.log
   ```

   Confirm both lines appear and battery reading is sensible.
5. **Double-tap on the body** to start session 1.
6. Verify the session is healthy with a 60 s capture:

   ```bash
   bash scripts/rtt_capture.sh 60 logs/preflight_sess1_$(date +%Y%m%d).log
   grep -E "recorder: streaming|sampler: streaming|session_start|<err>" \
        logs/preflight_sess1_*.log
   ```

   You should see all three streaming lines and **no `<err>` entries**. If any error appears, abort and diagnose before committing to the long run.
7. **Unplug J-Link USB from the Mac.** The board keeps running on battery; the active session continues seamlessly.
8. Walk away. The session manager rotates every 10 min until either you double-tap to stop, or battery hits the 3.3 V threshold and `STATE_LOW_BATT_HOLDOFF` auto-stops the session.

### Tomorrow morning

1. **Plug J-Link back in.**
2. Read board state and final battery via a 5 s RTT capture:

   ```bash
   bash scripts/rtt_capture.sh 5 logs/postflight_$(date +%Y%m%d).log
   grep -E "Battery:|FSM|Stopped" logs/postflight_*.log
   ```

   Three possibilities for what you see:
   - LED was solid + you double-tap stop now → board was still RECORDING. **Excellent — battery survived 8 h.**
   - LED was 5 Hz blink → board entered `LOW_BATT_HOLDOFF`. Battery hit ≤ 3.3 V at some point. The last completed session is the cutoff time.
   - LED off / no RTT → battery fully dead. SD content tells you when it stopped.
3. **Double-tap to stop** if still RECORDING.
4. **Pull SD card**, mount on Mac, run analyzer:

   ```bash
   python -m tools.analyze_overnight /Volumes/SENSAPULSE \
       --rtt logs/preflight_sess1_*.log \
       --out runs/overnight_$(date +%Y%m%d)/report.md
   ```

   Pass the **preflight RTT log** to `--rtt`, not a missing overnight one — analyzer cross-references CRC / watchdog patterns from whatever RTT we have.
5. Compute battery drain manually:
   - `batt_mv_start` from `SESSION_00001/meta.json` (first session of the night).
   - Final battery from postflight log (or last session's `meta.json` if it auto-stopped).
   - Drain = start − final.
6. Open `report.md`. Pass/fail per session, aggregate stats, RTT error counts.

## What "pass" looks like

Headline metrics on a clean run:

```
SESSIONS: 48 / 48 PASS
AUDIO   : 48 × 600.0 s ± 0.1 s, 0 NUL
IMU     : 48 × 51.96–52.04 Hz, drift 60–90 ms
SD CRC  : 0 errors
WATCHDOG: 0 trips
BATTERY : 4098 mV → ~3700 mV (~390 mV drain over 8 h)
VERDICT : ✅ v1.0 stable for 8 h continuous recording
```

If any session fails its individual checks, the report drops a section with the specific diagnostic (file size, embedded NULs, IMU rate). Cross-reference with the RTT log timestamp at the rotation boundary to find the cause.

## If something fails

- **SD CRC errors > 0** → swap card, re-run. Worn cards fail silently; we already saw this in the #11/#12 verification.
- **Some session.wav is short (e.g., 250 s instead of 600 s)** → rotation logic dropped a swap. Check RTT for `recorder: rotated` event count = 47, plus `recorder: rotate open ...: <errno>` lines.
- **All sessions short by the same ratio (e.g., 580 s)** → PDM clock drift. Compare `samples_per_pair / fs_audio` against wall-clock. Probably benign.
- **IMU rate < 51.5 Hz on later sessions** → SD write latency growing as card fills. Add `f_sync()` after each block in `imu_sampler.c` (cost: more SD wear).
- **Watchdog trip** → `session.c` already aborts the session and logs `monitor: writer X stopped unexpectedly`. Find the trigger line, file as a bug.
