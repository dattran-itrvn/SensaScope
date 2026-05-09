# SensaPulse — Production / Hardening TODO

Items deferred from v1.1 development for security, reliability, and compliance reasons. Address before any device leaves the bench.

## Security (BLE)

The current sync protocol is **unauthenticated and unencrypted** at the application layer. Anyone within BLE range with the service UUID can list and download all unsynced sessions. This is fine for bench development but **unacceptable in production** because:

- Audio captured by SensaPulse is PHI (heart/lung sounds, possibly speech in ambient track).
- 24/7 wear means data accumulates over days; an attacker with brief access to a charging device can exfiltrate everything.

### Required before shipping
- [ ] Enable **BLE LE Secure Connections** pairing (`CONFIG_BT_SMP=y`, `CONFIG_BT_SMP_SC_ONLY=y`). Require pairing before any GATT request.
- [ ] **Bonded** persistent keys stored in nRF52840 internal flash (Zephyr settings subsystem).
- [ ] **Passkey display + confirmation** on first pair (passkey shown via PC tool, user confirms on device — but device has no display, so use `passkey just-works` with explicit user confirmation via tap-on-device gesture).
- [ ] Whitelist: device only accepts connections from **bonded** peers; new pairings require physical user action (e.g., hold-tap for 5s during boot).

### Optional / belt-and-suspenders
- [ ] AES-GCM payload encryption with per-device key burned at provisioning. Defends against compromised BLE link layer (theoretical).
- [ ] Per-session signing — hash audio.wav with HMAC-SHA256 in `meta.json`, signed with device key. Tamper detection downstream.

## Provisioning

- [ ] Factory provisioning script: writes a unique device key to UICR or settings partition before first power-on.
- [ ] Device serial number tracking in inventory system (link MAC ↔ chip_id ↔ owner).

## Reliability

- [ ] **Brown-out detector** — nRF52840 BOR/POF. If VBATT drops below 2.5V mid-write, FATFS may corrupt. Configure POF threshold ≥ 2.7V, force shutdown.
- [ ] **Watchdog** — `CONFIG_WDT=y` + 10s timeout. Force reboot if main loop hangs.
- [ ] **FATFS power-fail recovery** — switch to journaled FS or do `f_sync()` after each WAV block (cost: more SD wear, but safer).
- [ ] **SD card health monitoring** — track write errors over time, disable card and warn user via BLE if error rate spikes.
- [ ] **SD card CRC error rate monitor** — log error count per session, abort + LED warn if >0 in production. Worn cards corrupt data silently and FATFS retries can mask it long enough that hours of audio look fine in metadata but contain NUL runs or truncated tails. Diagnosed during #11/#12 bring-up: a worn card produced 2.4 s of audio versus 50 s of IMU before the watchdog caught the divergence.
- [ ] **Battery sag protection** — when VBATT drops below 3.3V *during recording*, finalize current session cleanly before LDO drops out.
- [ ] **Empty-session prevention via fs_sync()** — after `fs_open` + placeholder WAV header in `audio_recorder_start` and after the CSV column header in `imu_sampler_start`, call `fs_sync()` so the directory entry + first block hits the card before the writer thread does any real work. Then call `fs_sync()` periodically inside both writer threads (e.g. every 10 PDM blocks ≈ 1 s) so a power-loss / hardfault loses at most ~1 s of audio rather than the entire session. Discovered in overnight test: 2 of 5 session folders contained 44-byte WAV with `data_bytes=0` and zero IMU rows because the writer thread crashed before any flush.
- [ ] **Atomic session creation order** — write meta.json + placeholder audio header + `fs_sync()` *before* creating the `.unsynced` marker. Then the marker semantically means "session has at least the placeholder data on disk". Sync analyzer / PC tool can skip any folder *missing* `.unsynced` (already implied to be synced) AND any folder *with* `.unsynced` but `audio.wav < 44 B` (failed before the placeholder hit the card). Cuts the post-mortem cost of the empty-session failure mode in half.
- [ ] **Crash diagnostic at boot** — enable `CONFIG_RESET_ON_FATAL_ERROR=y` so a hardfault reboots cleanly, and on every boot read `NRF_POWER->RESETREAS`, log the bit set (`RESETPIN`, `DOG`, `SREQ`, `LOCKUP`, `OFF`, `LPCOMP`, `DIF`, `NFC`, `VBUS`), and clear it. Without this we can't tell whether the s3→s4 jump in the overnight log (uptime 260 s → 8 s) was a hardfault, brownout, watchdog, or external reset — three of those would point at very different root causes.

## Hardware / PCB v1.1+

(Bring these to next PCB respin.)

- [ ] Add 4.7K pull-ups on I2C SDA/SCL (R12'/R13'). Internal 13K is borderline; future MEMS may need 400 kHz.
- [ ] Add 100K pull-up on LSM6DSL INT1 (cheap insurance against floating line if firmware bug disables push-pull).
- [ ] Add hardware test point for PDM_DATA + PDM_CLK for scope debug.
- [ ] Consider USB connector for fast bulk file dump (alternative to BLE) — would massively speed up sync.

## Compliance

- [ ] FCC/CE: BLE module is pre-certified (Ebyte E73 has FCC ID), but final product also needs full radiated emissions test in actual enclosure.
- [ ] Medical device classification: as a "wellness" device, lower bar than diagnostic. Confirm regulatory class with legal before any clinical claims.
- [ ] Privacy notice on packaging + companion app: discloses always-on recording, retention, and sync mechanism.

## User experience

- [ ] Battery level indication outside of just LED — exposed in `Device Info` GATT for PC tool to show, and BLE GAP appearance includes battery service (BAS).
- [ ] Tamper detect: log + flag in meta.json if device was unplugged from skin (large IMU spike + audio dropout).
- [ ] Time sync: when PC connects, push UTC time to device → device updates its software RTC → all subsequent `meta.json.start_rtc_ms` are real wall-clock. Currently they're since-boot uptime.

## Software / tools

- [ ] **GUI** for sync tool (PyQt or Electron) — non-technical users can't use CLI.
- [ ] **Mobile app** for sync (companion to existing SensaHub mention in original requirements deck).
- [ ] OTA firmware update via MCUboot + BLE SMP. Currently flashing requires physical SWD access.
- [ ] Logging analytics: optional opt-in upload of anonymized error logs / battery curves to help iterate firmware.
