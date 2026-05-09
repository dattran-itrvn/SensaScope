/*
 * SensaPulse — IMU sampler producer (#25 refactor).
 *
 * Reads LSM6DSL accel + gyro at 52 Hz and pushes raw int16 samples (with
 * timestamp) into sd_writer's IMU FIFO. Does NOT open or write any file.
 * sd_writer formats each sample as a CSV line and writes the imu.csv on
 * its own thread, eliminating the FATFS lock contention that the old
 * fs_write-from-here design caused with the audio writer.
 *
 * Sample rate: 52 Hz hardware (see imu.c CTRL2_G config).
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#define IMU_SAMPLER_RATE_HZ  52

int      imu_producer_start(void);
int      imu_producer_stop(void);
bool     imu_producer_is_running(void);

/* Counters maintained by the producer for diagnostics. The actually-
 * written-to-disk count lives in sd_writer (since the producer only pushes
 * to a queue, the write may happen later or be dropped under backpressure).
 */
uint32_t imu_producer_pushed(void);
uint32_t imu_producer_read_errors(void);
