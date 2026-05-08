/*
 * SensaPulse — Li-ion battery monitor.
 *
 * AIN4 (P0.28) reads VBATT divided 1:2 by R12/R15 (100K/100K).
 * Returned voltage is whole-battery mV (already scaled ×2).
 */
#pragma once

int         battery_init(void);
int         battery_read_mv(void);
const char *battery_state_str(int mv);
