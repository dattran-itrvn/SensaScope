# Playbook — SD subsystem isolation stress test (#24)

**Audience**: Claude Code on Dat's Mac M4. **Purpose**: stop chasing symptoms.
Run a stand-alone firmware that does only SD R/W (no PDM, no IMU, no FSM, no
BLE) and stress-test it for 10 iterations to answer one question:

> **Is the empty-session / -EIO bug in the SD subsystem alone, or only
> when SD interacts with the rest of the app?**

If 10/10 iterations pass with zero errors and bounded latency, the bug is
**interaction-layer** (PDM mem-slab pressure, watchdog tripping mid-write,
shared resource starvation, etc.). We then revert / rewrite the production
patches under that lens.

If errors appear in stress runs, the bug is **inside the SD subsystem**
(FATFS retry budget, sdhc_spi driver, SD card cells, signal integrity).
Different fix path — driver tuning, hardware changes, card spec.

Do NOT add more retry-band-aids to the production code until this answer is
in. Stop the iterate-fix loop.

---

## Files Cowork prepared

```
app/sd_stress/CMakeLists.txt
app/sd_stress/prj.conf
app/sd_stress/boards/sensapulse_v1.overlay   # SPI3+SDHC only, default 8 MHz
app/sd_stress/src/main.c                     # 4-phase auto-run on boot
scripts/sd_stress_loop.sh                    # build once, flash+capture×N
scripts/parse_sd_stress.py                   # per-run verdict + aggregate
```

The stress firmware on boot:
1. Mounts FATFS.
2. **Phase 1 — seq_bulk** (synthetic): 1 MB blocks × 20 = 20 MB, peak rate.
3. **Phase 2 — sustained** (synthetic): 4 KB / 25 ms (160 KB/s, above
   audio's 64 KB/s) for 60 s.
4. **Phase 3 — dual_audio + dual_imu** (synthetic): 4 KB/25 ms + 256 B/200 ms
   in parallel for 60 s. Mimics the audio + imu writer pair *without* real
   PDM/IMU peripherals.
5. **Phase 4 — sustained_synced** (synthetic): same as phase 2, with
   `fs_sync` every 5 s (matches the #23 patch).
6. **Phase 5 — pdm_only** (production data path through sd_writer #25):
   real PDM hardware at 16 kHz / 16-bit stereo, fed via audio_producer →
   audio_msgq → sd_writer (single FATFS thread). IMU producer is stopped
   immediately after start so only the audio path is exercised. 60 s.
   Expected audio.wav size: 16000 × 2 × 2 × 60 = 3,840,000 B.
7. **Phase 6 — pdm_imu** (production data path through sd_writer #25):
   PDM + LSM6DSL @ 52 Hz running concurrently. Both producers push into
   their FIFOs; sd_writer drains both queues from one thread. 60 s.
   Expected: audio.wav 3.84 MB, imu samples ≥ 3120.
8. Emits one `STRESS_SUMMARY: {...}` JSON line and halts.

Phases 1-4 already passed in the previous bench run (100 K writes, 0 errors,
max latency 380 ms). Phases 5-6 with the per-stream-FATFS design failed
3/5 on phase 6 (audio writer -EIO at FAT cluster boundary while IMU held
the lock during fs_sync). The #25 refactor makes sd_writer the sole FATFS
owner and replaces the per-stream writers with FIFO producers, so phases
5-6 should now pass 5/5.

If 5/6 pass
across all iterations, the production -EIO is in the FSM / watchdog / BLE
layer, not the data path. If 5 fails, PDM IRQ ↔ SPI contention is real. If
5 passes but 6 fails, it's the PDM + IMU + SD triple interaction (heap,
I²C, FATFS lock).

LED blinks 1 Hz throughout (only liveness). No tap input, nothing to
double-press. Total run time ≈ 5 minutes plus mount overhead.

---

## Step 1 — Run the loop (one command, fully automatic)

```bash
cd /Users/trandat/Project/SensaScope
# 5 iterations × 420 s = ~35 min. Each run does phases 1-6 (≈5 min on-chip).
bash scripts/sd_stress_loop.sh 5 420
# All logs land in logs/sd_stress_YYYYMMDD_HHMM/.
```

The script builds the stress firmware **once** then loops:
flash → wait 240 s while RTT capture runs → next.

If `west flash` itself fails on iteration N, the script keeps going — record
the iteration number that failed and surface it at the end. A flash failure
is a J-Link issue, not an SD issue, and should be re-tried separately.

---

## Step 2 — Read the verdict

`scripts/parse_sd_stress.py` runs at the end of the loop and prints:

```
## logs/sd_stress_*/run_01.log
**Result: PASS**
- writes: 8123, errors: 0
- total bytes: 33,275,392
- max latency: 27.4 ms
| phase | writes | errors | bytes | first_err |
| seq_bulk | 5120 | 0 | 20,971,520 | 0 |
| sustained | 2400 | 0 | 9,830,400 | 0 |
| ...

# Aggregate
- runs: 10
- pass: 10, fail: 0
- total writes across runs: 81,230
- total errors: 0 (0.000%)
```

The aggregate is the answer:

| Aggregate result                        | Bug location                | Next action                                                                                        |
|-----------------------------------------|-----------------------------|----------------------------------------------------------------------------------------------------|
| Phases 1-6 all pass on every iteration  | FSM / watchdog / BLE layer  | Revert #23 fs_sync; simplify session.c watchdog; investigate FSM interaction with writer threads.  |
| Phases 1-4 pass, phase 5 fails          | PDM IRQ ↔ SPI contention    | Try lowering PDM IRQ priority, or running SPI3 on a dedicated thread. PCB-level if not solvable.   |
| Phases 1-5 pass, phase 6 fails          | PDM + IMU + SD triple       | Heap pressure or FATFS lock contention. Bump heap to 32 K, profile. Or serialise audio+imu writes. |
| Any synthetic phase (1-4) fails         | SD subsystem                | Inconsistent with the previous 100 K-writes-clean run — re-run with same hardware to reproduce.    |
| Phase 5 / 6 pass but throughput < 95 %  | Buffer underrun             | PDM mem-slab too small (8 blocks → 16) or IMU buffer over-budgeted.                                |
| Latency > 500 ms anywhere               | SD subsystem (latent)       | Slack will run out under combined load even if no errors yet.                                      |

Surface the table cell that matched to the user with the actual numbers.

---

## Step 3 — Optional: SPI clock sweep

If aggregate is borderline (1–3 % error), re-run the loop at three SPI
clocks to see if the rate is clock-dependent:

```bash
for HZ in 24000000 16000000 8000000 4000000; do
    cd /Users/trandat/Project/SensaScope/app/sd_stress
    west build -b sensapulse_v1/nrf52840 -p always -- \
        -DBOARD_ROOT=/Users/trandat/Project/SensaScope \
        -DSD_STRESS_SPI_HZ=$HZ
    cd /Users/trandat/Project/SensaScope
    bash scripts/sd_stress_loop.sh 5 240
    mv logs/sd_stress_*  logs/sd_stress_${HZ}/
done
```

(Requires the `SD_STRESS_SPI_HZ` macro to actually flow through to the
overlay's `spi-max-frequency` — Cowork wired that already; verify by
grepping the build's generated DT for `spi-max-frequency`.)

---

## Step 4 — Reconnect with production firmware

Once the answer is in, switch back to the main app build:

```bash
cd /Users/trandat/Project/SensaScope/app
west build -b sensapulse_v1/nrf52840 -p always -- \
    -DBOARD_ROOT=/Users/trandat/Project/SensaScope
west flash
```

(Use `-p always` because the build directory is shared with the stress app.
A `-p auto` would re-use stress objects which gives a confused link.)

Then either:
- **Interaction-class bug** → tell user to revert `feat/22-led-tap-tuning` +
  `fix/sd-reliability+led-tap` patches against #23 (keep #22 LED+tap, drop
  #23 fs_sync). Investigate why writer threads die under PDM load — likely
  mem-slab pressure, FATFS write latency outlasting 800 ms slack, or
  watchdog racing against in-flight writes.
- **SD-subsystem bug** → tell user the SD path itself is unreliable on this
  hardware. Options: bigger PDM slack (16 blocks), lower SPI clock with
  retry, or PCB v1.1 with proper SI termination.

---

## Honest framing for the user

The user's read is correct — adding fs_sync + retry + atomic markers each
moved the failure rather than fixing it. The patches are not wrong, but
they're treating layers we haven't proven are the cause. This stress test
gives us evidence to pick a direction with confidence instead of trying
another patch.

Report the run as a **diagnostic milestone**, not a fix. The fix decision
follows from the verdict.
