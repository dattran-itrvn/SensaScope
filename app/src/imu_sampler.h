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

/* Mid-stream file swap. Sampler thread flushes pending samples to the
 * old file, closes it, opens new_path, writes the CSV header, and
 * resumes. Returns 0 on request accepted (swap happens at next iteration).
 */
int      imu_sampler_rotate(const char *new_csv_path);
