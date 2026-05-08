/*
 * SensaPulse — IMU CSV sampler.
 *
 * Streams LSM6DSL accel + gyro at 52 Hz to a CSV file. Buffered: ~1 s of
 * samples accumulate in RAM and flush in a single fs_write to keep FATFS
 * overhead low while audio recorder runs in parallel.
 *
 * CSV format:  t_us,ax,ay,az,gx,gy,gz
 *   - t_us = k_uptime in microseconds (uint64)
 *   - ax..gz = raw int16 LSB. NO calibration in firmware (per spec).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define IMU_SAMPLER_RATE_HZ  52

int      imu_sampler_start(const char *csv_path);
int      imu_sampler_stop(void);
bool     imu_sampler_is_running(void);
uint32_t imu_sampler_samples_written(void);
