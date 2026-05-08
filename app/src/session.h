/*
 * SensaPulse — session manager.
 *
 * Owns the lifecycle of a recording session:
 *   /SD/SESSION_NNNNN/
 *       audio.wav       (canonical 44-byte header, stereo 16 kHz/16-bit)
 *       imu.csv         (t_us,ax,ay,az,gx,gy,gz @ 52 Hz)
 *       meta.json
 *       .unsynced       0-byte marker (removed by PC sync ACK in v1.1)
 *
 *   - session_start():   pick next sequential id (counter persisted in
 *                        /SD/sync_state.json), mkdir, open all writers,
 *                        schedule the 10-min rotation timer.
 *   - rotation timer:    seamless mid-stream swap to the next folder.
 *                        Audio mem-slab and IMU sample buffer absorb
 *                        the FATFS swap latency; no audio gap > one
 *                        PDM block (~100 ms).
 *   - session_stop():    cancel timer, stop both writers (they flush
 *                        and close their files), session enters idle.
 *
 * Free-space management is split between this module and #17:
 *   - Pre-flight check before opening a folder. <100 MB free → bail.
 *   - Eviction of oldest synced folder is the job of #17.
 *   - SD full + no synced folders → caller should drive ERROR state.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SESSION_ROTATE_SEC_DEFAULT  600   /* 10 min — production cadence */

int      session_init(void);              /* read counter / scan SD on boot */
int      session_start(int batt_mv);      /* open new folder + start writers */
int      session_stop(void);              /* close current session */
bool     session_is_active(void);
uint32_t session_current_id(void);        /* 0 if none active */

/* Rotation period override — call before session_start() to test fast
 * rotation (e.g. 30 s) without recompiling. Default 600 s.
 */
void     session_set_rotate_sec(uint32_t seconds);

/* Status returned by session_start when SD is too full to open a new
 * folder. Caller (main FSM) should transition to ERROR state.
 */
#define SESSION_ERR_NO_SPACE  (-ENOSPC)
