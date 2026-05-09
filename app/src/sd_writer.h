/*
 * SensaPulse — single-thread SD writer with 2 producer FIFOs.
 *
 * Architecture (#25, supersedes the per-stream writer threads in #23):
 *
 *   audio_producer  ──→ audio_msgq (16 × ptr, ~1.6 s slack)  ─┐
 *                                                               ├──→ sd_writer_thread
 *   imu_producer    ──→ imu_msgq   (128 × 20 B, ~2.5 s slack) ─┘     │
 *                                                                       └─→ FATFS:
 *                                                                          audio.wav
 *                                                                          imu.csv
 *
 * Why: the per-stream writer design (audio.c + imu_sampler.c each calling
 * fs_write directly) shared one global FATFS mutex. When IMU did fs_sync
 * holding the mutex 100-300 ms, audio's mid-cluster fs_write timed out
 * and returned -EIO. Stress-test phase 6 reproduced this 3/5 runs.
 *
 * In this design only sd_writer_thread ever touches FATFS, so the
 * contention vanishes by construction. Producers do nothing but push
 * raw data into the FIFO and free slabs on backpressure.
 *
 * Public API: session.c calls sd_writer_start / sd_writer_stop /
 *             sd_writer_rotate. Producers are kicked off internally.
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

/* Raw IMU sample pushed by imu_producer. Formatted to CSV by sd_writer. */
struct sd_writer_imu_sample {
	uint64_t t_us;
	int16_t  ax, ay, az;
	int16_t  gx, gy, gz;
};

/* ---------- Lifecycle (called from session.c) ---------- */

/* Open audio.wav (placeholder 44 B header) + imu.csv (CSV header), spawn
 * the consumer thread, then start audio + imu producer threads. Returns 0
 * on success, negative errno on failure (files not opened, threads not
 * spawned).
 */
int sd_writer_start(const char *audio_path, const char *csv_path);

/* Signal producers + consumer to stop, drain remaining queue items, finalize
 * the WAV header with the actual data byte count, close both files.
 * Blocking — returns when files are closed.
 */
int sd_writer_stop(void);

/* Mid-stream file swap, FATFS-atomic edition (#27).
 *
 * Producers keep running. Consumer thread does the entire transition in
 * one pass: drain pending → finalize current WAV/CSV → fs_mkdir new
 * session folder → write meta.json → open new audio + csv → resume.
 *
 * No other thread touches FATFS during these steps — so there's no lock
 * contention with system_work_queue or app threads, which is what made
 * the pre-#27 prepare_folder-in-session.c approach race against the
 * writer's fs_writes mid-rotate.
 *
 * Caller passes everything by value (copied into module-private buffers)
 * before the request returns. Audio mem-slab gives ~1.6 s of slack to
 * cover the mkdir + 4 fs_opens + meta write the consumer has to do.
 */
int sd_writer_rotate_full(const char *new_folder,
			  const char *new_audio_path,
			  const char *new_csv_path,
			  const char *meta_body,
			  uint32_t    meta_len);

bool sd_writer_is_running(void);

/* True if the consumer thread exited because of an error (FATFS open /
 * fs_write returning < 0) rather than a clean stop request. Cleared on
 * the next sd_writer_start. session.c watchdog uses this to detect a
 * silently-dead writer.
 */
bool sd_writer_failed(void);

/* ---------- Producer push (used by audio.c + imu_sampler.c) ---------- */

/* Push one PDM slab pointer into the audio FIFO. The slab is freed by
 * sd_writer when the data has been written. On FIFO full (consumer can't
 * keep up), the function returns -ENOSPC and the caller MUST free the
 * slab itself — drop-newest backpressure policy.
 */
int sd_writer_push_audio(void *slab_buf);

/* Push one IMU sample (raw int16 axes + timestamp). Drop-newest on full;
 * the caller doesn't own anything that needs freeing.
 */
int sd_writer_push_imu(const struct sd_writer_imu_sample *sample);

/* ---------- Stats (used by session.c watchdog + meta.json) ---------- */

uint32_t sd_writer_audio_bytes_written(void);
uint32_t sd_writer_imu_samples_written(void);
uint32_t sd_writer_audio_dropped(void);   /* PDM blocks dropped at producer */
uint32_t sd_writer_imu_dropped(void);     /* IMU samples dropped at producer */
