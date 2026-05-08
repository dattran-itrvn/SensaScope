/*
 * SensaPulse — LSM6DSL IMU driver (raw I2C; doesn't use Zephyr sensor API
 * because we want explicit control of tap detection registers).
 *
 *  WHO_AM_I = 0x6A   on PCB v1.0 (SDO/SA0 strapped to GND).
 *
 *  Task #9: double-tap detection routed to INT1 pin (P0.06, active high).
 *           Caller registers a callback that fires from a Zephyr work item.
 */
#pragma once

#include <stdint.h>

typedef void (*imu_tap_cb_t)(void);

/* Probe LSM6DSL on I2C and configure XL (416 Hz / ±2 g) + G (52 Hz / ±245 dps)
 * + BDU. Returns 0 on success.
 */
int     imu_init(void);
uint8_t imu_who_am_i(void);

/* Burst-read 6 axes (raw int16 LSB).
 * Output order: accel_xyz[ax,ay,az], gyro_xyz[gx,gy,gz].
 */
int     imu_read_xlg(int16_t accel_xyz[3], int16_t gyro_xyz[3]);

/* Configure double-tap on X/Y/Z and route to INT1.
 * cb is called from system work-queue context (k_work) — don't sleep there.
 */
int     imu_enable_double_tap(imu_tap_cb_t cb);
