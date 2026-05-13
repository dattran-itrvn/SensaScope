/*
 * SensaPulse — BLE Sync Service (task #19).
 *
 * Custom GATT service cho PC sync. Spec đầy đủ trong
 * `docs/SYNC_PROTOCOL.md`. Service UUID base
 * `7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e`, 4 characteristic:
 *   0002 Device Info   — Read (JSON)
 *   0003 Control       — Write + Notify (opcode framing)
 *   0004 Data          — Notify only (bulk file stream)
 *   0005 Set Name      — Write (UTF-8 ≤32 byte)
 *
 * Module này CHỈ chịu trách nhiệm GATT layer. FSM transitions (IDLE ↔
 * SYNC) đã xử lý ở main.c qua `on_connected` / `on_disconnected` callbacks
 * của #20. File I/O cho LIST/READ/ACK/DEL phải route qua sd_writer thread
 * (giữ invariant single-FATFS-owner của #32).
 */
#pragma once

/* Đăng ký service với BT host. Gọi sau bt_enable() trong start_ble(). */
int ble_sync_init(void);
