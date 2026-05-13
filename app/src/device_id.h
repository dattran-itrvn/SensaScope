/*
 * SensaPulse — device identity (name + chip id).
 *
 * On boot:
 *   1. Compute chip_id = lower-case hex of NRF_FICR->DEVICEID[1..0]
 *      (16 chars, no prefix).
 *   2. Read /SD/device.name (max 32 UTF-8 bytes, trailing CR/LF and
 *      surrounding whitespace stripped). If the file is absent, empty,
 *      or whitespace-only, fall back to name = "chip_<chip_id>".
 *
 * Cached for the lifetime of the program; meta.json (#11) and the
 * BLE Device Info characteristic (#19) read it through the getters.
 *
 * SD volume must already be mounted (sdlog_init) before identity_init.
 */
#pragma once

int         identity_init(void);
const char *identity_get_name(void);
const char *identity_get_chip_id(void);

/* #19.3: re-read /SD/device.name and update cached name. Called by
 * ble_sync.c name_write_cb after Set Name persists the new value, so the
 * change takes effect immediately (next meta.json + BLE Device Info read).
 */
int         identity_reload(void);
