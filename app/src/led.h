/*
 * SensaPulse — LED indicator (P0.03 active-high via N-MOSFET).
 *
 * State-driven blink engine. A k_timer ticks every 100 ms and walks the
 * pattern array assigned to the current state. Callers do not poke the
 * GPIO; they call led_set_state() and the engine plays the pattern.
 *
 * State semantics (set by main FSM in #14, BLE sync in #20):
 *   OFF        — pin low, no blink. Used at boot before init.
 *   IDLE       — 1 Hz, ~10 % duty. Heart-beat while waiting for double-tap.
 *   RECORDING  — solid on. Recording session in progress.
 *   SYNC       — ~2 Hz, ~40 % duty. v1.1 BLE PC-sync in progress.
 *   LOW_BATT   — 5 Hz, 50 % duty. Battery sag below threshold.
 *   ERROR      — SOS Morse (... --- ...). SD full / unrecoverable FATFS.
 */
#pragma once

typedef enum {
	LED_STATE_OFF,
	LED_STATE_IDLE,
	LED_STATE_RECORDING,
	LED_STATE_SYNC,
	LED_STATE_LOW_BATT,
	LED_STATE_ERROR,
	LED_STATE__MAX,
} led_state_t;

int          led_init(void);
void         led_set_state(led_state_t state);
led_state_t  led_get_state(void);
