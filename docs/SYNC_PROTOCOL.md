# SensaPulse v1.1 — BLE Sync Protocol

This document is the contract between the device firmware and the PC sync tool. Implement to this spec on both sides.

## State machine (v1.1.2)

The device is always in exactly one of three primary states (IDLE, RECORDING, SYNC) plus two error/hold states (LOW_BATT_HOLDOFF, ERROR). A separate `rec_owner` flag tracks who started the current recording — `TAP` or `BLE` — and enforces "start by X → stop by X" symmetry.

```
        ┌────────┐  double-tap (start)     ┌─────────────────┐
   ┌──→ │  IDLE  │ ──────────────────────→ │ RECORDING       │
   │    └────────┘  ←────────────────────  │ rec_owner=TAP   │
   │       │       double-tap (stop)       └─────────────────┘
   │       │ BLE connect (PC tool)
   │       │
   │       ↓                  OP_START_RECORD     ┌─────────────────┐
   │    ┌────────┐ ────────────────────────────→ │ RECORDING       │
   │    │  SYNC  │ ←──────────────────────────── │ rec_owner=BLE   │
   └─── └────────┘ self-disconnect after STOP    └─────────────────┘
        ↑                                                  ↑    │
        │                                                  │    │
        └─── PC brief reconnect for opcode exchange ───────┘    │
             (BUSY for non-STOP / non-RESET, then self-disconnect)
```

- **IDLE** — LED 1 pulse/3s (sparse heartbeat per #22). BLE advertising. SD has space (or has sync'd folders to evict). Listening for double-tap or BLE connect.
- **RECORDING** — LED 2 pulses/5s. **BLE advertising stays on** (v1.1.2). PDM + IMU + SD writers running. New session folder open with `.unsynced` marker.
- **SYNC** — LED 2 Hz blink. PDM + IMU stopped. BLE connection active. PC tool drives LIST/READ/ACK/DEL transfers. Returns to pre_sync_state on disconnect.

### v1.1.2 — BLE policy during RECORDING

The CLAUDE.md hard rule "RECORDING turns BLE controller off" applies to the **persistent link**, not advertising. Specifically:

- **Advertising**: always on, in every FSM state. Restarted in `on_disconnected()` regardless of state.
- **Persistent connection**: only valid when device was IDLE/LOW_BATT/ERROR before connect (transitions to SYNC). During RECORDING the FSM does NOT transition on connect — the device stays in RECORDING and the opcode handler self-disconnects in ~150 ms after replying. This is the "kết nối ngắn, không giữ kết nối" model.
- **Recording survives disconnect**: PC disconnect mid-record is NOT an implicit stop (was in v1.1.1; removed in v1.1.2 due to BLE-link-during-RECORDING crashing the SD writer at ~64s — see TASKS.md #36).

### Opcode behaviour matrix

| FSM state | rec_owner | OP_LIST/READ/ACK/DEL/ABORT | OP_START_RECORD | OP_STOP_RECORD | OP_RESET_CTL |
|---|---|---|---|---|---|
| SYNC (pre=IDLE) | NONE | OK | OK → RECORDING (owner=BLE), self-disconnect | (no record to stop) | OK |
| RECORDING | TAP | BUSY + self-disconnect | BUSY + self-disconnect | BUSY + self-disconnect (owner mismatch) | OK (no disconnect) |
| RECORDING | BLE | BUSY + self-disconnect | BUSY + self-disconnect (already recording) | OK → IDLE, self-disconnect | OK (no disconnect) |
| SYNC (pre=ERROR / LOW_BATT) | NONE | OK (drain data) | BUSY + self-disconnect (pre != IDLE) | (no record to stop) | OK |

### Stopping a recording

- **rec_owner=TAP**: only by double-tap, by `batt < BATT_LOW_MV` (LOW_BATT_HOLDOFF auto-stop), by SD full (ERROR), or by SW1 power cut.
- **rec_owner=BLE**: only by `OP_STOP_RECORD` over a fresh BLE connection, by batt low, by SD full, or by SW1 power cut.
- In all cases `rec_owner` is reset to NONE on FSM exit from RECORDING.

## On-SD layout

```
/SD/
├── device.name                   # optional, 1 line plain text, max 32 chars
├── SESSION_00001/
│   ├── audio.wav
│   ├── imu.csv
│   ├── meta.json
│   └── .unsynced                 # 0-byte marker; absent = already sync'd
├── SESSION_00002/
│   └── ...
├── SP_BOOTS.TXT                  # legacy boot stamp log; ignored by sync
```

Session id is sequential, monotonic from a 4-byte counter persisted in `/SD/sync_state.json`. On boot, firmware scans existing folders to find max id and seeds the counter. (No reuse of ids even after deletion.)

## Free-space management

Triggered just before opening a new session folder (every 10 minutes):

```python
free_mb = sd_free_space_mb()
if free_mb >= 100:                  # comfortable, keep going
    open_new_session()
elif have_at_least_one_synced():
    delete_oldest_synced_session()  # frees ~38 MB
    open_new_session()
else:
    enter_error_state()             # SD full of unsync'd data
    led_pattern = SOS
    log_err("SD full, no synced folders to evict — sync required")
```

`delete_oldest_synced_session()` removes the lowest-numbered folder whose `.unsynced` marker is absent. `meta.json`, `audio.wav`, `imu.csv` get unlinked, then the folder.

## BLE GATT service

### Service UUID
```
7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e
```

(Random UUID base — fine for development; PC tool hardcodes this.)

### Characteristics

| UUID suffix | Name           | Properties        | Direction       |
|-------------|----------------|-------------------|-----------------|
| `0002`      | Device Info    | Read              | device → host   |
| `0003`      | Control        | Write, Notify     | host ↔ device   |
| `0004`      | Data           | Notify            | device → host   |
| `0005`      | Set Name       | Write             | host → device   |

Replace `0001` with `0002`...`0005` in the full UUID.

#### `Device Info` (read)

Returns a CBOR-encoded map (or simple JSON string for v1.1, ~256 bytes max):

```json
{
    "name": "Dat-chest-01",
    "chip_id": "9A9E783c4f4b8e8a",
    "fw": "v1.1.0+abc123",
    "state": "idle",          // "idle" | "recording" | "sync"
    "sd_total_mb": 30436,
    "sd_free_mb":  18020,
    "unsynced":    23,
    "synced":      147
}
```

#### `Control` (write + notify)

Frame format (binary, little-endian):

```
| 1 byte | 1 byte | N bytes        |
| OPCODE | STATUS | params/payload |
```

Host writes `(OPCODE, 0, params)`. Device replies via notify with `(OPCODE, status_code, response_payload)`.

| OPCODE | Name      | Host params                                  | Device response                  |
|--------|-----------|---------------------------------------------|----------------------------------|
| `0x01` | LIST      | (no params)                                  | n_unsynced (u16) + array of (u16 session_id, u32 size_bytes) |
| `0x02` | READ      | session_id (u16) + file_index (u8) + offset (u32) + length (u32; 0 = to end) | (immediately): total_bytes (u32). Then `Data` notifications stream the bytes. |
| `0x03` | ACK       | session_id (u16)                             | success/fail status              |
| `0x04` | ABORT     | (no params)                                  | success — cancels in-flight READ |
| `0x05` | DEL       | session_id (u16)                             | success/fail (refuse if not synced) |
| `0x06` | START_RECORD | (no params)                               | success / busy. Only valid in SYNC state. |
| `0x07` | STOP_RECORD  | (no params)                               | success / busy. Only stops a BLE-initiated session; tap-initiated sessions return BUSY. |
| `0xFF` | RESET_CTL | (no params)                                  | clears any partial state         |

`file_index`: 0=audio.wav, 1=imu.csv, 2=meta.json. (Order is well-known so host doesn't need to discover it.)

Status codes:
- `0x00` OK
- `0x01` busy (device is recording)
- `0x02` not_found (session_id doesn't exist)
- `0x03` already_synced (DEL refused because no `.unsynced` marker — guard against accidental delete; ACK already removed marker)
- `0x04` io_err (FATFS read failed)
- `0x05` invalid (malformed frame)

#### `Data` (notify only)

Each notification carries up to MTU − 3 bytes of raw file content. No header — host counts bytes received against the total announced in the READ response. Last chunk can be shorter than MTU − 3.

When all bytes for current READ are streamed, device sends a final notify of length 0 (terminator).

#### `Set Name` (write)

Plain UTF-8 string, max 32 bytes. Device persists to `/SD/device.name`. Empty write deletes the name file (revert to chip-id naming).

## Connection parameters (device side)

In `prj.conf`:

```
CONFIG_BT_CTLR_PHY_2M=y
CONFIG_BT_USER_PHY_UPDATE=y
CONFIG_BT_USER_DATA_LEN_UPDATE=y
CONFIG_BT_BUF_ACL_RX_SIZE=251
CONFIG_BT_BUF_ACL_TX_SIZE=251
CONFIG_BT_L2CAP_TX_MTU=247
CONFIG_BT_GATT_NOTIFY_MULTIPLE=y    # batch notifies per interval
```

After connection:
1. `bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M)` — switch to 2 Mbit/s
2. `bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX)` — request 251-byte LL packets
3. `bt_gatt_exchange_mtu(conn, 247)` — request larger MTU
4. `bt_conn_le_param_update(conn, &BT_LE_CONN_PARAM(6, 12, 0, 400))` — request interval 7.5–15 ms (host can deny; we accept whatever it grants)

Throughput math at 7.5 ms interval, 244-byte data payload, 6 packets/interval:
- 244 × 6 / 0.0075 ≈ 195 kB/s theoretical, ~80 kB/s realistic after stack overhead.
- 38 MB session at 80 kB/s ≈ 8 minutes. Fits user's 10-min target.

**Realistic fallback if 7.5 ms is denied.** macOS and iOS BLE stacks routinely refuse intervals < 11.25 ms; the request returns `bt_conn_le_param_update` accept with a coerced larger value. At 15 ms the throughput falls to ~40 kB/s realistic → 38 MB ≈ 16 minutes. **Acceptable.** The protocol does not depend on hitting 7.5 ms; tune for "best the host gives us, no app retries on slow path".

**Power budget reality check (post-overnight stress test).**
The hardware draws ~26 mA in `RECORDING` state (LED solid 13 mA + recording 13 mA). A small ~50 mAh wearable cell gives ~110 minutes runtime under recording — already a constraint. During `SYNC` state we replace the recording load with BLE radio + SD reads:
- LED in SYNC pattern (2 Hz blink, 50 % duty) ≈ 6.5 mA average.
- BLE 5.0 PHY 2M continuous notify, low-margin connection: ~10 mA average.
- SD reads (sustained block reads at 8 MHz SPI): ~5 mA.
- **Total ≈ 22 mA in SYNC.**

Per session sync at 80 kB/s = 8 min × 22 mA = 2.9 mAh. At 40 kB/s (15 ms interval) = 16 min × 22 mA = 5.9 mAh. With a 50 mAh cell and budget for 1 hour record/day (~26 mAh), sync of 6 sessions leaves enough headroom for the next day's recording — assuming user docks for charging between sync sessions. Tighter cells need smaller BLE duty.

If throughput tuning fights us in the firmware (#19), the fallback is **slower sync, not protocol redesign**. Don't sacrifice protocol simplicity to chase the last 20 % of throughput.

## PC sync flow (Python)

```python
# Pseudocode
async with BleakClient(ADDR) as client:
    info = json.loads(await read_char(DEVICE_INFO_UUID))
    if info["state"] != "idle":
        raise RuntimeError(f"device busy: {info['state']}")

    sessions = await ctrl_list(client)
    for sid, size in sessions:
        out_dir = ROOT / device_folder(info) / f"SESSION_{sid:05d}.tmp"
        out_dir.mkdir(parents=True, exist_ok=True)

        for fname, idx in [("audio.wav", 0), ("imu.csv", 1), ("meta.json", 2)]:
            with (out_dir / fname).open("wb") as f:
                async for chunk in ctrl_read(client, sid, idx):
                    f.write(chunk)

        # Atomic publish: rename .tmp → final
        out_dir.rename(out_dir.with_suffix(""))
        # Confirm to device
        await ctrl_ack(client, sid)
```

Resume after disconnect: PC checks for `*.tmp` folders; for each, ask device for size of each file, compare to local size, resume from where it left off using READ with `offset` parameter. Sessions that are fully transferred but never ACKed → re-send ACK.

## Error handling

- **Device in RECORDING state when host connects** → device replies to LIST/READ/ACK with `0x01 busy`. PC tool prints "device is recording, sync skipped" and disconnects.
- **SD full + no synced folders** → device's `Device Info` shows `state: "error"` and `sd_free_mb < 100`. PC tool warns the user; sync proceeds normally to clear unsynced data, then user reboots device to clear error state.
- **PC disconnects mid-READ** → device aborts in-flight transfer on disconnect. Marker `.unsynced` stays — next reconnect resumes via offset.

### Skipping empty / corrupt sessions

Overnight stability data showed ~40 % of sessions can land empty (audio.wav and imu.csv = 0 bytes) and ~17 % can have folder→file FATFS corruption when the chip resets mid-session. The sync protocol must not treat these as transfer failures:

- **LIST** filters out folders whose `audio.wav` is 0 bytes *or* missing — they have no payload worth downloading. The marker `.unsynced` stays, but firmware logs once and ignores. (Optional: a separate cleanup pass can `DEL` them when unambiguous.)
- **LIST** also skips entries that are not directories at all (the `SESSION_NNNNN` file corruption case). Don't crash, log and move on.
- **PC tool** treats a zero-byte audio file from a normal session (which can happen if user double-tapped stop too fast — < 100 ms after start) as a sync target with `total_bytes=0` for that file; READ returns 0 bytes immediately, ACK still removes the marker.

These rules keep the protocol forward-compatible with the v1.0 reliability hardening (`docs/PRODUCTION_TODO.md` § Reliability — `fs_sync()` periodic, atomic session create order). When that hardening lands, the empty-session rate should drop toward zero, but the LIST filter stays for forward safety.

## Spec versioning

Bump the major version number in `Device Info.fw` if any wire format changes. PC tool refuses to talk to mismatched major versions.
