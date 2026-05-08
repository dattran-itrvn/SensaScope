/*
 * SensaPulse — LED indicator (P0.03 active-high via N-MOSFET).
 *
 * Phase 4: just a thin GPIO wrapper. The full state machine
 * (idle blink / recording solid / low-batt / error SOS) goes in task #13.
 */
#pragma once

int  led_init(void);
void led_on(void);
void led_off(void);
void led_toggle(void);
