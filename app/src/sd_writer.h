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

/* ---------- Lifecycle ---------- */

/* #19.3: spawn the writer thread once at boot, AFTER sdlog_init. Thread
 * remains alive for the rest of run-time, mediates FATFS ops via the
 * sd_writer_touch_file / sd_writer_write_file APIs even khi không có
 * session đang ghi. Gọi từ main.c trước khi start_ble() / session_init().
 */
int sd_writer_init(void);

/* Open audio.wav (placeholder 44 B header) + imu.csv (CSV header), then
 * start audio + imu producer threads. Writer thread phải đã chạy từ
 * sd_writer_init(). Returns 0 on success, negative errno on failure.
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

/* True from the moment sd_writer_rotate_full() is called until the
 * consumer thread has finished mkdir + meta + open new pair. Used by
 * session.c monitor (#28) to suppress touch_unsynced(new_id) until the
 * folder actually exists on disk.
 */
bool sd_writer_is_rotating(void);

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

/* #30: number of times do_rotate() returned an error and was deferred
 * (caller kept writing to the old folder, retried on next timer tick).
 * Monotonic since sd_writer_start. Useful diagnostic in monitor heartbeat.
 */
uint32_t sd_writer_rotate_deferred_total(void);

/* #32: synchronously execute fs_open(CREATE|WRITE) + fs_close on `path`
 * from the sd_writer thread. Used by session.c monitor để tạo `.unsynced`
 * marker mà KHÔNG vi phạm "single-FATFS-owner" invariant của #25.
 *
 * Lý do tồn tại: trước #32, monitor_work_handler gọi fs_open/close trực
 * tiếp từ system_work_queue trong khi sd_writer thread đang fs_write →
 * SD card bị stress ngược và trả `-116 ETIME` sau ~12-41 phút. Đã
 * reproduce trong Test 7 (xem `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md`).
 *
 * Block trên semaphore tới khi writer thread xử lý xong; trả về 0 hoặc
 * negative errno. Trả `-ENOENT` nếu writer chưa start.
 */
int sd_writer_touch_file(const char *path);

/* #19.3: synchronously open(CREATE|WRITE|TRUNC) + write + close `path`
 * với `body` (`len` byte) từ sd_writer thread. Dùng cho config file nhỏ
 * (Set Name, future save_counter). len=0 → tạo file rỗng (truncate).
 * KHÔNG cho bulk data — bulk ghi qua producer FIFO. Cùng pattern k_sem
 * như sd_writer_touch_file. Trả `-ENOENT` nếu writer chưa start.
 */
#define SD_WRITER_MAX_SMALL_FILE_BYTES 64
int sd_writer_write_file(const char *path, const void *body, size_t len);

/* #19.4: list SESSION_NNNNN folders on SD. Scan từ sd_writer thread (giữ
 * single-FATFS-owner). Caller cấp out array; thread fill `count_out` entry
 * theo readdir order (typically ascending session id). Mỗi entry chứa
 * id (1..65535), tổng bytes của audio.wav + imu.csv + meta.json, và flag
 * is_unsynced (true = folder có `.unsynced` marker, chưa được PC ACK).
 *
 * Trả `-ENOENT` nếu thread chưa init. `max` ≤ SD_WRITER_LIST_MAX.
 */
#define SD_WRITER_LIST_MAX 40

struct sd_writer_session_info {
	uint16_t session_id;
	uint32_t size_bytes;
	bool     is_unsynced;
};

int sd_writer_list_sessions(struct sd_writer_session_info *out,
			    uint32_t max, uint32_t *count_out);

/* #19.5: streaming file read for BLE Data notify. Callback `cb` được gọi
 * mỗi chunk từ sd_writer thread; ble_sync truyền cb thực hiện bt_gatt_notify.
 * cb trả `< 0` để abort (vd ABORT opcode hoặc disconnect). `length=0` =
 * đọc đến EOF. `total_out` (nullable) trả tổng số byte sẽ đọc trước khi
 * stream bắt đầu. Trả `-ENOENT` nếu file không tồn tại hoặc thread chưa init.
 */
typedef int (*sd_writer_read_cb)(const uint8_t *chunk, uint32_t len, void *user);

int sd_writer_read_file(const char *path, uint32_t offset, uint32_t length,
			sd_writer_read_cb cb, void *user,
			uint32_t *total_out);
