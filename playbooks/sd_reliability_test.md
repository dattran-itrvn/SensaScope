# Playbook — SD write reliability test (#23) + LED/tap tuning (#22)

**Audience**: Claude Code (running on Dat's Mac M4 with full west / J-Link / RTT access).
**Purpose**: One-shot test loop that builds, flashes, captures RTT, and reports
whether the empty-session bug is fixed. Bundles #22 LED + double-tap tuning
(no SD-path side effects) so we verify both in one cycle.

---

## What's already on the working tree (do NOT re-edit)

Cowork left these 6 files modified:

```
app/src/audio.c           # +fs_sync every 50 blocks, heartbeat log
app/src/imu_sampler.c     # +fs_sync after every flush, heartbeat log
app/src/session.c         # atomic .unsynced marker (only after audio>0 B)
app/src/led.c             # IDLE single-pulse 3 s, RECORDING double-pulse 5 s
app/src/imu.c             # TAP_THS=0x14, INT_DUR2=0x4E
TASKS.md                  # #22 + #23 entries
```

Verify before starting:

```bash
cd /Users/trandat/Project/SensaScope
git status -sb
# expect:  M app/src/audio.c
#          M app/src/imu_sampler.c
#          M app/src/session.c
#          M app/src/led.c
#          M app/src/imu.c
#          M TASKS.md
```

If anything else is modified, abort and ping the user — Cowork didn't expect it.

---

## Step 1. Branch + commit + push (auto)

```bash
cd /Users/trandat/Project/SensaScope
git checkout main
git pull --ff-only
git checkout -b fix/sd-reliability+led-tap

git add app/src/audio.c app/src/imu_sampler.c app/src/session.c \
        app/src/led.c app/src/imu.c TASKS.md
git commit -m "fix(#23) + tune(#22): SD reliability + LED/tap tuning

#23 SD reliability:
- audio.c: fs_sync every 50 blocks (~5s), heartbeat log on each sync
- imu_sampler.c: fs_sync after every flush (~1s), heartbeat log
- session.c: atomic .unsynced — touched by monitor only after first
  non-zero audio bytes; monitor heartbeat log every 5s

Empty sessions (writer crash before first write) no longer get a
.unsynced marker, so BLE LIST will skip them naturally. Periodic
sync limits cache loss to <5s of audio / <1s of IMU on any reset.

#22 LED + tap tuning:
- led.c: IDLE single-pulse 100 ms / 3 s; RECORDING double-pulse
  100 ms+200 ms+100 ms / 5 s. ~25x lower LED current vs solid.
- imu.c: TAP_THS_6D 0x09->0x14 (~1.25 g, nấc trung);
  INT_DUR2 0x7F->0x4E (DUR=4, QUIET=3, SHOCK=2)."

git push -u origin fix/sd-reliability+led-tap
```

If push fails (no remote configured / auth issue), report to user and stop.

---

## Step 2. Build + flash (auto)

```bash
cd /Users/trandat/Project/SensaScope/app
west build -b sensapulse_v1/nrf52840 -p auto -- \
    -DBOARD_ROOT=/Users/trandat/Project/SensaScope 2>&1 | tee /tmp/build.log
test ${PIPESTATUS[0]} -eq 0 || { echo "BUILD FAILED"; exit 1; }

west flash 2>&1 | tee /tmp/flash.log
# nrfjprog spits "JLinkARM.dll reported error -256" noise on Apple Silicon
# — ignore. Look for "Flashing... PROGRAM SUCCESSFUL" or similar at the end.
```

If build fails, paste the first `error:` line from `/tmp/build.log` to the user
and stop. Typical failures: missing `-DBOARD_ROOT`, dirty include from a stale
build dir (try `rm -rf build && west build -p always ...`).

---

## Step 3. RTT capture + manual interaction (semi-auto)

```bash
cd /Users/trandat/Project/SensaScope
mkdir -p logs
LOGFILE="logs/sd_reliability_$(date +%Y%m%d_%H%M).log"
echo "Capturing RTT to $LOGFILE for 90 s..."
echo "While capture is running, the user must:"
echo "  1. Wait ~5 s for boot to settle (LED single-pulse every 3 s = IDLE)"
echo "  2. Double-tap firmly to START recording (LED switches to double-pulse)"
echo "  3. Wait ~60 s"
echo "  4. Double-tap firmly again to STOP"
echo "Capture continues 30 s past stop so the recorder cleanup is logged."
echo ""
echo "Press Enter to start capture..."
read

bash scripts/rtt_capture.sh 90 "$LOGFILE"
echo ""
echo "Capture done: $LOGFILE"
```

Tell the user the capture is live and to do the double-tap sequence.

---

## Step 4. Parse log + verdict (auto)

```bash
python3 scripts/parse_sd_reliability.py "$LOGFILE"
```

The parser prints PASS / FAIL with reasons. Possible verdicts:

- **PASS**: all required heartbeats present, no `<err>` from
  `audio` / `imu_sampler` / `session` / `fs`, marker was set.
- **FAIL — writer never started**: `streaming →` never logged.
  Either dmic_configure or fs_open of audio.wav failed.
- **FAIL — writer died early**: `streaming →` logged, but no
  `recorder: 50 blocks` heartbeat ever fired (writer died within
  first 5 s). Look at the previous 10 lines for `<err>` reason.
- **FAIL — sampler never wrote**: same pattern for IMU.
- **FAIL — marker not set**: writer ran but `monitor: SESSION_*
  .unsynced marker set` never appeared. Means `audio_recorder_bytes_written()`
  stayed at 0 even though writer thread was alive — points at a regression in
  how `rec_bytes` is updated.
- **PARTIAL**: writers ran but stopped early (e.g. after 30 s instead of 60 s).
  Probably a real failure mid-recording — the new error logs should name it.

Save the verdict to a brief markdown note in the same `logs/` dir:

```bash
python3 scripts/parse_sd_reliability.py "$LOGFILE" > "${LOGFILE%.log}.verdict.md"
cat "${LOGFILE%.log}.verdict.md"
```

---

## Step 5. SD card hardware verify (manual + auto)

After the capture finishes, ask the user to:
1. Power-cycle the device (or wait for the natural session_stop above).
2. Pull the SD card out, plug into the Mac via reader.

Wait for user confirmation, then:

```bash
SDPATH=/Volumes/SENSASCOPE
# Find the latest session
LATEST=$(ls -1d $SDPATH/SESSION_* | sort | tail -1)
echo "Latest: $LATEST"
ls -la "$LATEST"
echo ""

# Sizes
AWAV=$(stat -f%z "$LATEST/audio.wav" 2>/dev/null || echo 0)
ICSV=$(stat -f%z "$LATEST/imu.csv" 2>/dev/null || echo 0)
MJSON=$(stat -f%z "$LATEST/meta.json" 2>/dev/null || echo 0)
UNSYNCED=$([ -f "$LATEST/.unsynced" ] && echo "yes" || echo "no")

echo "audio.wav: $AWAV B"
echo "imu.csv:   $ICSV B"
echo "meta.json: $MJSON B"
echo ".unsynced: $UNSYNCED"

# Verdict
if [ "$AWAV" -gt 100000 ] && [ "$ICSV" -gt 1000 ] && [ "$UNSYNCED" = "yes" ]; then
    echo ""
    echo "✅ SD verify PASS — non-empty session with marker"
elif [ "$AWAV" -lt 50 ] && [ "$ICSV" -lt 50 ] && [ "$UNSYNCED" = "no" ]; then
    echo ""
    echo "🟡 EMPTY session correctly skipped (no marker) — fix #23 part 2 OK,"
    echo "   but writer still failed. Check RTT log for crash reason."
else
    echo ""
    echo "❌ Unexpected combination — read RTT log for diagnosis"
fi

# Run the full Python loader for sanity
python3 -m tools.load_session "$LATEST" 2>&1 | head -20
```

---

## Step 6. Decide next action (manual prompt to user)

Report to the user:

- **PASS + SD verify PASS**: propose merging `fix/sd-reliability+led-tap` to main.
  Use a squash merge so #22 and #23 land as one commit on main.
- **PASS RTT but EMPTY SD**: writer thread didn't actually call fs_write.
  This is a different bug from what was hypothesized; show the user the
  last 30 lines of the RTT log and ask whether to dig further.
- **FAIL with crash reason in log**: paste the relevant `<err>` line(s)
  to the user and propose a follow-up patch. Do NOT merge yet.

Update `TASKS.md` accordingly:
- On PASS, change #23 `🚧` → `✅` and append "Verified on $(date +%F): N B audio,
  M samples IMU, marker set after K B" with actual numbers.
- Add corresponding row to `docs/TIMING.md` v1.1 table for #23.

Do **not** kick off #19 (BLE GATT) until #23 passes — empty sessions would
mask BLE bugs and we don't want compounded uncertainty.

---

## Notes for human-in-the-loop

- If the J-Link RTT capture stalls (`V0.0, SN=0, no log streams`), the chip is
  halted. Open J-Link Commander manually and type `g` to release halt — the
  `rtt_capture.sh` script handles this case but if the script reports it,
  surface to the user.
- macOS will mount `SENSASCOPE` read-only on first plug if FATFS marked the
  filesystem dirty (writer-thread death without close). It's safe to pull the
  card and re-plug; the OS auto-fixes minor FAT inconsistencies.
- If user wants to test multiple sessions in one capture (e.g. 3x record
  cycles to estimate empty rate), bump capture duration to 5 minutes and
  repeat tap-start / tap-stop 3 times during it. Parser handles N sessions.
