#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "audio.h"
#include "imu_sampler.h"
#include "sd_log.h"
#include "sd_writer.h"

LOG_MODULE_REGISTER(sd_writer, LOG_LEVEL_INF);

/* ---------- Tunables ---------- */
#define AUDIO_QUEUE_SLOTS    16              /* matches PDM_BLOCK_COUNT */
#define IMU_QUEUE_SLOTS      128             /* ~2.5 s @ 52 Hz */
#define IMU_BATCH_MAX        26              /* one fs_write every ~0.5 s */
#define WRITER_STACK_SZ      4096
#define WRITER_PRIO          K_PRIO_PREEMPT(4)   /* above producers */
#define WRITER_TICK_MS       25
#define SYNC_INTERVAL_MS     5000

/* #30 resilience knobs. fs_*_retry catches transient SD timeouts (the
 * card busy doing internal GC / wear-levelling — typical for -116/-EIO at
 * rotate burst) and sleeps RETRY_BACKOFF_MS between attempts. Total worst-
 * case stall per fs op = RETRY_MAX × RETRY_BACKOFF_MS ≈ 600 ms, well inside
 * the PDM mem-slab slack (~1.6 s) and IMU msgq slack (~2.5 s).
 */
#define RETRY_MAX            3
#define RETRY_BACKOFF_MS     200
#define ROTATE_FAIL_LIMIT    3   /* consecutive rotate fails → abort session */
#define ROTATE_SYNC_TIMEOUT  K_SECONDS(10)

#define AUDIO_BLOCK_BYTES    (AUDIO_PDM_RATE_HZ * 100 / 1000   /* per 100 ms */ \
			      * AUDIO_PDM_CHANNELS * (AUDIO_PDM_BIT_WIDTH / 8))
/* = 16000 * 0.1 * 2 * 2 = 6400 — but production PDM_BLOCK_MS is 100 ms
 * yielding 1600 sample-pairs * 2 bytes = 3200 B per block. The DMIC API
 * gives us the actual size at runtime, so this constant is informational.
 */

/* ---------- FIFOs ---------- */
K_MSGQ_DEFINE(audio_msgq, sizeof(void *),
	      AUDIO_QUEUE_SLOTS, 4);
K_MSGQ_DEFINE(imu_msgq, sizeof(struct sd_writer_imu_sample),
	      IMU_QUEUE_SLOTS, 4);

/* ---------- Module state ---------- */
static K_THREAD_STACK_DEFINE(writer_stack, WRITER_STACK_SZ);
static struct k_thread     writer_thread;

static struct fs_file_t    audio_file;
static struct fs_file_t    csv_file;
/* #30: opened during do_rotate Phase 1, swapped into audio_file/csv_file at
 * Phase 2 commit. Holding both old + new handles open lets us abort the
 * rotate (close pending only, keep old active) if any new-side fs op fails.
 */
static struct fs_file_t    audio_file_pending;
static struct fs_file_t    csv_file_pending;

/* #19.3 split semantics:
 *   - thread_alive: writer thread spawned by sd_writer_init, runs cho đến
 *     shutdown. Phục vụ touch_file/write_file ngay cả khi không có session.
 *   - running: session active (audio.wav + imu.csv mở, producers chạy).
 *     Cleared on sd_writer_stop, thread vẫn ở. Tên giữ nguyên để session.c
 *     monitor backward-compat.
 */
static atomic_t            thread_alive  = ATOMIC_INIT(0);
static atomic_t            running       = ATOMIC_INIT(0);
static atomic_t            stop_req      = ATOMIC_INIT(0);
static atomic_t            rotate_req    = ATOMIC_INIT(0);
static atomic_t            failed        = ATOMIC_INIT(0);

/* #30: synchronous rotate API. rotate_full() blocks on rotate_done until
 * writer thread reports rotate_result. session.c needs the real outcome
 * to decide whether to commit current_id / next_id.
 */
static struct k_sem        rotate_done;
static int                 rotate_result;
static uint32_t            rotate_consecutive_fails;
static uint32_t            rotate_deferred_total;     /* monotonic stat */

/* #32: synchronous touch-file API. session.c monitor needs to create the
 * `.unsynced` marker; trước #32 nó gọi fs_open/close trực tiếp từ
 * system_work_queue → SD card -116 timeout sau ~1-40 phút. Giờ chuyển
 * request qua sd_writer thread, ngăn 2 thread đụng FATFS đồng thời.
 */
static atomic_t            touch_req     = ATOMIC_INIT(0);
static char                touch_path_buf[64];
static struct k_sem        touch_done;
static int                 touch_result;

/* #19.3: synchronous write-file API (small config files only). */
static atomic_t            write_req     = ATOMIC_INIT(0);
static char                write_path_buf[64];
static uint8_t             write_body_buf[SD_WRITER_MAX_SMALL_FILE_BYTES];
static size_t              write_body_len;
static struct k_sem        write_done;
static int                 write_result;

/* #19.4: synchronous list-sessions API. */
static atomic_t            list_req      = ATOMIC_INIT(0);
static struct sd_writer_session_info *list_out;
static uint32_t            list_max;
static uint32_t            list_count;
static struct k_sem        list_done;
static int                 list_result;

static volatile uint32_t   audio_bytes;
static volatile uint32_t   imu_samples;
static volatile uint32_t   audio_dropped;
static volatile uint32_t   imu_dropped;

static char                audio_path_buf[64];
static char                csv_path_buf[64];

/* #27: rotate request payload — consumer thread reads these atomically
 * with path_mtx held. session.c stages everything *before* setting the
 * rotate_req atomic flag.
 */
static char                next_folder_buf[64];
static char                next_audio_path[64];
static char                next_csv_path[64];
static char                next_meta_body[384];
static uint32_t            next_meta_len;
static struct k_mutex      path_mtx;

static const char          csv_header_str[] = "t_us,ax,ay,az,gx,gy,gz\n";

/* ---------- #30 Retry + verify helpers ---------- */
static inline bool is_transient_err(int ret)
{
	return ret == -EIO || ret == -ENXIO ||
	       ret == -ETIMEDOUT || ret == -EAGAIN;
}

static int fs_open_retry(struct fs_file_t *f, const char *path, fs_mode_t flags)
{
	int ret = 0;
	for (int i = 0; i < RETRY_MAX; i++) {
		fs_file_t_init(f);
		ret = fs_open(f, path, flags);
		if (ret == 0 || !is_transient_err(ret)) return ret;
		LOG_WRN("fs_open(%s) try %d/%d: %d — backoff %d ms",
			path, i + 1, RETRY_MAX, ret, RETRY_BACKOFF_MS);
		k_msleep(RETRY_BACKOFF_MS);
	}
	return ret;
}

static int fs_write_retry(struct fs_file_t *f, const void *data, size_t len)
{
	int ret = 0;
	for (int i = 0; i < RETRY_MAX; i++) {
		ret = fs_write(f, data, len);
		if (ret >= 0 || !is_transient_err(ret)) return ret;
		LOG_WRN("fs_write(%zu B) try %d/%d: %d — backoff %d ms",
			len, i + 1, RETRY_MAX, ret, RETRY_BACKOFF_MS);
		k_msleep(RETRY_BACKOFF_MS);
	}
	return ret;
}

static int fs_close_retry(struct fs_file_t *f)
{
	int ret = 0;
	for (int i = 0; i < RETRY_MAX; i++) {
		ret = fs_close(f);
		if (ret == 0 || !is_transient_err(ret)) return ret;
		LOG_WRN("fs_close try %d/%d: %d — backoff %d ms",
			i + 1, RETRY_MAX, ret, RETRY_BACKOFF_MS);
		k_msleep(RETRY_BACKOFF_MS);
	}
	return ret;
}

static int fs_seek_retry(struct fs_file_t *f, off_t off, int whence)
{
	int ret = 0;
	for (int i = 0; i < RETRY_MAX; i++) {
		ret = fs_seek(f, off, whence);
		if (ret == 0 || !is_transient_err(ret)) return ret;
		LOG_WRN("fs_seek try %d/%d: %d — backoff %d ms",
			i + 1, RETRY_MAX, ret, RETRY_BACKOFF_MS);
		k_msleep(RETRY_BACKOFF_MS);
	}
	return ret;
}

/* Catch the silent FAT corruption seen in #30 run 1 (SESSION_00007 came out
 * as a 0-byte regular file even though mkdir + open + write returned success).
 * fs_stat after mkdir confirms the entry on disk is genuinely a directory.
 */
static int verify_is_dir(const char *path)
{
	struct fs_dirent ent;
	int ret = fs_stat(path, &ent);
	if (ret) {
		LOG_ERR("verify_is_dir(%s) stat: %d", path, ret);
		return ret;
	}
	if (ent.type != FS_DIR_ENTRY_DIR) {
		LOG_ERR("verify_is_dir(%s) entry is FILE not DIR — "
			"silent FAT corruption, aborting rotate", path);
		return -EBADF;
	}
	return 0;
}

static int verify_file_nonempty(const char *path)
{
	struct fs_dirent ent;
	int ret = fs_stat(path, &ent);
	if (ret) {
		LOG_ERR("verify_file(%s) stat: %d", path, ret);
		return ret;
	}
	if (ent.type != FS_DIR_ENTRY_FILE) {
		LOG_ERR("verify_file(%s) type=%d not FILE", path, ent.type);
		return -EBADF;
	}
	if (ent.size == 0) {
		LOG_ERR("verify_file(%s) size=0 — write did not commit", path);
		return -EBADF;
	}
	return 0;
}

/* ---------- WAV header (44 B canonical PCM) ---------- */
static int write_wav_header(struct fs_file_t *f, uint32_t data_bytes)
{
	uint8_t hdr[44];
	uint32_t sr  = AUDIO_PDM_RATE_HZ;
	uint16_t ch  = AUDIO_PDM_CHANNELS;
	uint16_t bps = AUDIO_PDM_BIT_WIDTH;
	uint32_t br  = sr * ch * (uint32_t)bps / 8;
	uint16_t ba  = ch * bps / 8;
	uint32_t fsize = data_bytes + 36;
	uint32_t fmt_size = 16;
	uint16_t fmt = 1;

	memcpy(&hdr[0],  "RIFF",      4);
	memcpy(&hdr[4],  &fsize,      4);
	memcpy(&hdr[8],  "WAVE",      4);
	memcpy(&hdr[12], "fmt ",      4);
	memcpy(&hdr[16], &fmt_size,   4);
	memcpy(&hdr[20], &fmt,        2);
	memcpy(&hdr[22], &ch,         2);
	memcpy(&hdr[24], &sr,         4);
	memcpy(&hdr[28], &br,         4);
	memcpy(&hdr[32], &ba,         2);
	memcpy(&hdr[34], &bps,        2);
	memcpy(&hdr[36], "data",      4);
	memcpy(&hdr[40], &data_bytes, 4);

	int ret = fs_seek_retry(f, 0, FS_SEEK_SET);
	if (ret) return ret;
	ret = fs_write_retry(f, hdr, sizeof(hdr));
	return ret < 0 ? ret : 0;
}

/* ---------- Helpers ---------- */
/* Open audio + csv pair into the given fs_file_t structs and write the
 * placeholder WAV header + CSV header. Used by sd_writer_start (initial
 * open into audio_file / csv_file) AND by do_rotate (open into the
 * pending pair before swap).
 */
static int open_pair_into(struct fs_file_t *af, struct fs_file_t *cf,
			  const char *audio_path, const char *csv_path)
{
	uint8_t pad[44] = {0};
	int ret;

	ret = fs_open_retry(af, audio_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) { LOG_ERR("open audio %s: %d", audio_path, ret); return ret; }
	ret = fs_write_retry(af, pad, sizeof(pad));
	if (ret < 0) {
		LOG_ERR("audio header placeholder: %d", ret);
		fs_close_retry(af);
		return ret;
	}

	ret = fs_open_retry(cf, csv_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("open csv %s: %d", csv_path, ret);
		fs_close_retry(af);
		return ret;
	}
	ret = fs_write_retry(cf, csv_header_str, sizeof(csv_header_str) - 1);
	if (ret < 0) {
		LOG_ERR("csv header: %d", ret);
		fs_close_retry(cf);
		fs_close_retry(af);
		return ret;
	}
	return 0;
}

static int open_pair(const char *audio_path, const char *csv_path)
{
	return open_pair_into(&audio_file, &csv_file, audio_path, csv_path);
}

static int finalize_pair(uint32_t audio_data_bytes)
{
	int ret = write_wav_header(&audio_file, audio_data_bytes);
	if (ret) LOG_WRN("finalize: WAV header rewrite failed: %d", ret);
	fs_close_retry(&audio_file);
	fs_close_retry(&csv_file);
	return ret;
}

/* Drain audio FIFO (NO_WAIT, while not empty) and write to file.
 * Returns -EIO on first fs_write failure; partial writes possible.
 */
static int drain_audio(uint32_t *bytes_written_out)
{
	void *buf;
	uint32_t bytes_total = 0;
	while (k_msgq_get(&audio_msgq, &buf, K_NO_WAIT) == 0) {
		int ret = fs_write_retry(&audio_file, buf, AUDIO_BLOCK_BYTES);
		audio_producer_release_slab(buf);   /* always free, even on err */
		if (ret < 0) {
			LOG_ERR("audio fs_write at %u B: %d", audio_bytes, ret);
			return ret;
		}
		audio_bytes += ret;
		bytes_total += ret;
	}
	if (bytes_written_out) *bytes_written_out = bytes_total;
	return 0;
}

static int drain_imu(void)
{
	struct sd_writer_imu_sample batch[IMU_BATCH_MAX];
	int n = 0;
	while (n < IMU_BATCH_MAX &&
	       k_msgq_get(&imu_msgq, &batch[n], K_NO_WAIT) == 0) {
		n++;
	}
	if (n == 0) return 0;

	char text[1024];
	int len = 0;
	for (int i = 0; i < n; i++) {
		int adv = snprintf(text + len, sizeof(text) - len,
				   "%llu,%d,%d,%d,%d,%d,%d\n",
				   (unsigned long long)batch[i].t_us,
				   batch[i].ax, batch[i].ay, batch[i].az,
				   batch[i].gx, batch[i].gy, batch[i].gz);
		if (adv < 0 || len + adv >= (int)sizeof(text)) {
			LOG_WRN("imu text buf full at i=%d (n=%d)", i, n);
			break;
		}
		len += adv;
	}
	int ret = fs_write_retry(&csv_file, text, len);
	if (ret < 0) {
		LOG_ERR("imu fs_write %d samples / %d bytes: %d", n, len, ret);
		return ret;
	}
	imu_samples += n;
	return 0;
}

/* #30 do_rotate — two-phase to allow defer-on-failure.
 *
 * Phase 1: prepare new folder + meta + open new audio/csv into PENDING
 *          handles. Old handles still active. All fs ops are retry-wrapped
 *          and verified (post-mkdir fs_stat for DIR attr, post-meta fs_stat
 *          for size > 0). On any failure, close any partial pending handles
 *          and return error — old session keeps writing to the same folder.
 *
 * Phase 2: commit. Drain pending audio+imu into OLD handles, finalize
 *          (rewrite WAV header, close OLD), swap pending → active. Reset
 *          counters. The drain is moved AFTER the commit-point so an error
 *          in Phase 1 doesn't lose pending data — those bytes stay in the
 *          FIFOs and land in the old folder on the next loop iteration.
 *
 * Caller (writer thread) treats < 0 as deferred (after ROTATE_FAIL_LIMIT
 * consecutive defers, escalates to failed=1 → FSM ERROR).
 */
static int do_rotate(void)
{
	int ret;

	/* Snapshot args under mutex (paths + meta body). */
	char folder[64], ap[64], cp[64];
	char meta[sizeof(next_meta_body)];
	uint32_t meta_n;
	k_mutex_lock(&path_mtx, K_FOREVER);
	strncpy(folder, next_folder_buf, sizeof(folder) - 1); folder[sizeof(folder)-1] = '\0';
	strncpy(ap,     next_audio_path, sizeof(ap)     - 1); ap[sizeof(ap)-1]         = '\0';
	strncpy(cp,     next_csv_path,   sizeof(cp)     - 1); cp[sizeof(cp)-1]         = '\0';
	memcpy(meta, next_meta_body, sizeof(meta));
	meta_n = next_meta_len;
	k_mutex_unlock(&path_mtx);

	/* ===== Phase 1: prepare new (old handles still active) ===== */

	/* 1a. mkdir + verify DIR attribute (catches the silent-FAT-corruption
	 *     case from run 1: mkdir returned 0 but on-disk entry came out as
	 *     a regular file). EEXIST is fine as long as verify passes.
	 */
	ret = fs_mkdir(folder);
	if (ret && ret != -EEXIST) {
		LOG_ERR("rotate mkdir %s: %d", folder, ret);
		return ret;
	}
	ret = verify_is_dir(folder);
	if (ret) return ret;

	/* 1b. meta.json: open+write+close with retry, then stat-verify size>0. */
	struct fs_file_t mf;
	char meta_path[64];
	snprintf(meta_path, sizeof(meta_path), "%s/meta.json", folder);
	ret = fs_open_retry(&mf, meta_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("rotate meta open %s: %d", meta_path, ret);
		return ret;
	}
	int wret = fs_write_retry(&mf, meta, meta_n);
	int cret = fs_close_retry(&mf);
	if (wret < 0) {
		LOG_ERR("rotate meta write: %d", wret);
		return wret;
	}
	if (cret) {
		LOG_ERR("rotate meta close: %d", cret);
		return cret;
	}
	ret = verify_file_nonempty(meta_path);
	if (ret) return ret;

	/* 1c. Open new audio + csv into PENDING handles, write placeholder
	 *     headers. open_pair_into closes any opened-then-failed handle
	 *     internally so we don't leak on partial failure.
	 */
	ret = open_pair_into(&audio_file_pending, &csv_file_pending, ap, cp);
	if (ret) {
		LOG_ERR("rotate open new pair: %d", ret);
		return ret;
	}

	/* ===== Phase 2: commit. From here on we MUST swap to keep state
	 *      consistent — drain into OLD, finalize OLD, swap.
	 *      Errors past this point are logged but not propagated. =====
	 */
	uint32_t junk;
	drain_audio(&junk);
	drain_imu();

	int fret = finalize_pair(audio_bytes);
	if (fret) LOG_WRN("rotate finalize old: %d", fret);

	audio_file = audio_file_pending;
	csv_file   = csv_file_pending;
	fs_file_t_init(&audio_file_pending);
	fs_file_t_init(&csv_file_pending);

	LOG_INF("rotate: %s + %s opened (closed %u B audio in prev session)",
		ap, cp, audio_bytes);

	strncpy(audio_path_buf, ap, sizeof(audio_path_buf));
	strncpy(csv_path_buf,   cp, sizeof(csv_path_buf));
	audio_bytes = 0;
	imu_samples = 0;
	return 0;
}

/* ---------- #19.4 SD session enumeration (runs in writer thread) ---------- */
static int do_list_sessions(struct sd_writer_session_info *out,
			    uint32_t max, uint32_t *count_out)
{
	*count_out = 0;
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int ret = fs_opendir(&dir, SD_MOUNT_POINT);
	if (ret) {
		LOG_ERR("list: opendir: %d", ret);
		return ret;
	}

	static const char *const files[] = { "audio.wav", "imu.csv", "meta.json" };
	const size_t prefix = sizeof("SESSION_") - 1;

	while (*count_out < max) {
		struct fs_dirent ent;
		if (fs_readdir(&dir, &ent) || ent.name[0] == '\0') break;
		if (ent.type != FS_DIR_ENTRY_DIR) continue;
		if (strncmp(ent.name, "SESSION_", prefix) != 0) continue;
		uint32_t id = strtoul(ent.name + prefix, NULL, 10);
		if (id == 0 || id > UINT16_MAX) continue;

		uint32_t total = 0;
		for (size_t f = 0; f < ARRAY_SIZE(files); f++) {
			char p[80];
			snprintf(p, sizeof(p), "%s/%s/%s",
				 SD_MOUNT_POINT, ent.name, files[f]);
			struct fs_dirent s;
			if (fs_stat(p, &s) == 0 && s.type == FS_DIR_ENTRY_FILE) {
				total += (uint32_t)s.size;
			}
		}

		char marker[80];
		snprintf(marker, sizeof(marker), "%s/%s/.unsynced",
			 SD_MOUNT_POINT, ent.name);
		struct fs_dirent ms;
		bool unsynced = (fs_stat(marker, &ms) == 0);

		out[*count_out].session_id  = (uint16_t)id;
		out[*count_out].size_bytes  = total;
		out[*count_out].is_unsynced = unsynced;
		(*count_out)++;
	}
	fs_closedir(&dir);
	LOG_INF("list_sessions: %u entries scanned", *count_out);
	return 0;
}

/* ---------- Consumer thread ---------- */
static void writer_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	LOG_INF("writer: thread alive — waiting for session_start / fs ops");

	int64_t last_sync = k_uptime_get();
	uint32_t loop_ticks = 0;

	while (atomic_get(&thread_alive)) {
		bool session = atomic_get(&running) != 0;

		if (session && atomic_get(&rotate_req)) {
			int rr = do_rotate();
			rotate_result = rr;
			if (rr < 0) {
				rotate_consecutive_fails++;
				rotate_deferred_total++;
				LOG_WRN("rotate deferred (%u/%u consecutive): "
					"keeping old folder, will retry next "
					"timer tick",
					rotate_consecutive_fails,
					(uint32_t)ROTATE_FAIL_LIMIT);
				if (rotate_consecutive_fails >= ROTATE_FAIL_LIMIT) {
					LOG_ERR("rotate failed %u times in a row "
						"→ aborting session",
						rotate_consecutive_fails);
					atomic_set(&failed, 1);
					atomic_clear(&rotate_req);
					k_sem_give(&rotate_done);
					break;
				}
			} else {
				rotate_consecutive_fails = 0;
			}
			atomic_clear(&rotate_req);
			k_sem_give(&rotate_done);
		}

		/* #32: service touch_file requests in-thread. session.c monitor
		 * dùng cái này để tạo .unsynced marker, KHÔNG gọi fs_open trực
		 * tiếp từ system_work_queue nữa. */
		if (atomic_get(&touch_req)) {
			struct fs_file_t f;
			char path[sizeof(touch_path_buf)];

			k_mutex_lock(&path_mtx, K_FOREVER);
			strncpy(path, touch_path_buf, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';
			k_mutex_unlock(&path_mtx);

			int tret = fs_open_retry(&f, path,
						 FS_O_CREATE | FS_O_WRITE);
			if (tret == 0) {
				int cret = fs_close_retry(&f);
				if (cret) tret = cret;
			}
			touch_result = tret;
			atomic_clear(&touch_req);
			k_sem_give(&touch_done);
		}

		/* #19.4: service list_sessions requests in-thread. */
		if (atomic_get(&list_req)) {
			list_result = do_list_sessions(list_out, list_max,
						       &list_count);
			atomic_clear(&list_req);
			k_sem_give(&list_done);
		}

		/* #19.3: service write_file requests in-thread (Set Name, etc.) */
		if (atomic_get(&write_req)) {
			struct fs_file_t f;
			char  path[sizeof(write_path_buf)];
			uint8_t body[sizeof(write_body_buf)];
			size_t blen;

			k_mutex_lock(&path_mtx, K_FOREVER);
			strncpy(path, write_path_buf, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';
			blen = write_body_len;
			if (blen > sizeof(body)) blen = sizeof(body);
			memcpy(body, write_body_buf, blen);
			k_mutex_unlock(&path_mtx);

			int wret = fs_open_retry(&f, path,
				FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
			if (wret == 0) {
				if (blen > 0) {
					int n = fs_write_retry(&f, body, blen);
					if (n < 0) wret = n;
				}
				int cret = fs_close_retry(&f);
				if (wret == 0 && cret) wret = cret;
			}
			write_result = wret;
			atomic_clear(&write_req);
			k_sem_give(&write_done);
		}

		if (session) {
			uint32_t audio_chunk = 0;
			int ret = drain_audio(&audio_chunk);
			if (ret < 0) {
				atomic_set(&failed, 1);
				atomic_clear(&running);
				/* Don't kill thread — let it serve fs ops + future
				 * sessions. session.c watchdog reads
				 * sd_writer_failed() to detect this. */
				continue;
			}
			ret = drain_imu();
			if (ret < 0) {
				atomic_set(&failed, 1);
				atomic_clear(&running);
				continue;
			}

			/* Periodic sync — only one thread, no contention. */
			if (k_uptime_get() - last_sync >= SYNC_INTERVAL_MS) {
				fs_sync(&audio_file);
				fs_sync(&csv_file);
				last_sync = k_uptime_get();
			}

			/* Heartbeat every 5 s for diagnostic. */
			loop_ticks++;
			if ((loop_ticks % (5000 / WRITER_TICK_MS)) == 0) {
				LOG_INF("writer: audio=%u B, imu=%u samples, "
					"dropped audio=%u imu=%u",
					audio_bytes, imu_samples,
					audio_dropped, imu_dropped);
			}
		}

		/* stop_req from sd_writer_stop: finalize current session, clear
		 * running, but keep thread alive for fs ops + next session. */
		if (atomic_get(&stop_req)) {
			uint32_t junk;
			drain_audio(&junk);
			drain_imu();
			if (atomic_get(&running)) {
				finalize_pair(audio_bytes);
				atomic_clear(&running);
			}
			LOG_INF("writer: session stopped, %u B audio, %u IMU samples, "
				"%u audio dropped, %u imu dropped",
				audio_bytes, imu_samples,
				audio_dropped, imu_dropped);
			atomic_clear(&stop_req);
			last_sync = k_uptime_get();
		}

		k_msleep(WRITER_TICK_MS);
	}

	LOG_INF("writer: thread exit");
}

/* ---------- Public API ---------- */
int sd_writer_init(void)
{
	if (atomic_get(&thread_alive)) return -EALREADY;

	k_mutex_init(&path_mtx);
	k_sem_init(&rotate_done, 0, 1);
	k_sem_init(&touch_done,  0, 1);
	k_sem_init(&write_done,  0, 1);
	k_sem_init(&list_done,   0, 1);

	atomic_clear(&running);
	atomic_clear(&stop_req);
	atomic_clear(&rotate_req);
	atomic_clear(&failed);
	atomic_clear(&touch_req);
	atomic_clear(&write_req);
	atomic_clear(&list_req);

	atomic_set(&thread_alive, 1);

	k_tid_t tid = k_thread_create(&writer_thread, writer_stack,
				      K_THREAD_STACK_SIZEOF(writer_stack),
				      writer_thread_fn, NULL, NULL, NULL,
				      WRITER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(tid, "sd_writer");
	return 0;
}

int sd_writer_start(const char *audio_path, const char *csv_path)
{
	if (!atomic_get(&thread_alive)) return -ENOENT;
	if (atomic_get(&running)) return -EALREADY;

	rotate_consecutive_fails = 0;
	rotate_deferred_total    = 0;
	rotate_result            = 0;
	k_msgq_purge(&audio_msgq);
	k_msgq_purge(&imu_msgq);

	strncpy(audio_path_buf, audio_path, sizeof(audio_path_buf) - 1);
	audio_path_buf[sizeof(audio_path_buf) - 1] = '\0';
	strncpy(csv_path_buf, csv_path, sizeof(csv_path_buf) - 1);
	csv_path_buf[sizeof(csv_path_buf) - 1] = '\0';

	int ret = open_pair(audio_path_buf, csv_path_buf);
	if (ret) return ret;

	audio_bytes   = 0;
	imu_samples   = 0;
	audio_dropped = 0;
	imu_dropped   = 0;
	atomic_clear(&stop_req);
	atomic_clear(&rotate_req);
	atomic_clear(&failed);
	atomic_set(&running, 1);

	/* Kick off producers AFTER files are open + thread is told to drain. */
	ret = audio_producer_start();
	if (ret) {
		LOG_ERR("audio_producer_start: %d", ret);
		atomic_set(&stop_req, 1);
		return ret;
	}
	ret = imu_producer_start();
	if (ret) {
		LOG_ERR("imu_producer_start: %d — stopping audio + writer", ret);
		audio_producer_stop();
		atomic_set(&stop_req, 1);
		return ret;
	}
	return 0;
}

int sd_writer_stop(void)
{
	if (!atomic_get(&running)) return -ENOENT;

	/* Stop producers first so they stop pushing into the queues. */
	audio_producer_stop();
	imu_producer_stop();
	while (audio_producer_is_running() || imu_producer_is_running()) {
		k_msleep(20);
	}

	atomic_set(&stop_req, 1);
	while (atomic_get(&running)) k_msleep(20);
	return 0;
}

int sd_writer_rotate_full(const char *new_folder,
			  const char *new_audio_path,
			  const char *new_csv_path,
			  const char *meta_body,
			  uint32_t    meta_len)
{
	if (!atomic_get(&running)) return -ENOENT;
	if (meta_len > sizeof(next_meta_body)) return -EMSGSIZE;

	/* #30: drain any stale give from a previous abort (shouldn't happen
	 * but defensive — k_sem_take with 0 timeout, returns -EAGAIN if empty.
	 */
	k_sem_reset(&rotate_done);

	k_mutex_lock(&path_mtx, K_FOREVER);
	strncpy(next_folder_buf, new_folder,    sizeof(next_folder_buf) - 1);
	next_folder_buf[sizeof(next_folder_buf) - 1] = '\0';
	strncpy(next_audio_path, new_audio_path, sizeof(next_audio_path) - 1);
	next_audio_path[sizeof(next_audio_path) - 1] = '\0';
	strncpy(next_csv_path, new_csv_path, sizeof(next_csv_path) - 1);
	next_csv_path[sizeof(next_csv_path) - 1] = '\0';
	memcpy(next_meta_body, meta_body, meta_len);
	next_meta_len = meta_len;
	k_mutex_unlock(&path_mtx);

	rotate_result = -EBUSY;     /* in case sem times out */
	atomic_set(&rotate_req, 1);

	int sret = k_sem_take(&rotate_done, ROTATE_SYNC_TIMEOUT);
	if (sret) {
		LOG_ERR("rotate: writer thread did not signal within timeout");
		return -ETIMEDOUT;
	}
	return rotate_result;
}

bool sd_writer_is_running(void)  { return atomic_get(&running)    != 0; }
bool sd_writer_failed(void)      { return atomic_get(&failed)     != 0; }
bool sd_writer_is_rotating(void) { return atomic_get(&rotate_req) != 0; }

int sd_writer_push_audio(void *slab_buf)
{
	int ret = k_msgq_put(&audio_msgq, &slab_buf, K_NO_WAIT);
	if (ret) {
		audio_dropped++;
		return -ENOSPC;
	}
	return 0;
}

int sd_writer_push_imu(const struct sd_writer_imu_sample *sample)
{
	int ret = k_msgq_put(&imu_msgq, sample, K_NO_WAIT);
	if (ret) {
		imu_dropped++;
		return -ENOSPC;
	}
	return 0;
}

int sd_writer_write_file(const char *path, const void *body, size_t len)
{
	if (!atomic_get(&thread_alive)) return -ENOENT;
	if (len > SD_WRITER_MAX_SMALL_FILE_BYTES) return -EMSGSIZE;

	k_sem_reset(&write_done);

	k_mutex_lock(&path_mtx, K_FOREVER);
	strncpy(write_path_buf, path, sizeof(write_path_buf) - 1);
	write_path_buf[sizeof(write_path_buf) - 1] = '\0';
	if (len > 0 && body) memcpy(write_body_buf, body, len);
	write_body_len = len;
	k_mutex_unlock(&path_mtx);

	write_result = -EBUSY;
	atomic_set(&write_req, 1);

	int sret = k_sem_take(&write_done, K_SECONDS(5));
	if (sret) {
		LOG_ERR("sd_writer_write_file: timeout");
		return -ETIMEDOUT;
	}
	return write_result;
}

int sd_writer_list_sessions(struct sd_writer_session_info *out,
			    uint32_t max, uint32_t *count_out)
{
	if (!atomic_get(&thread_alive)) return -ENOENT;
	if (!out || !count_out || max == 0) return -EINVAL;
	if (max > SD_WRITER_LIST_MAX) max = SD_WRITER_LIST_MAX;

	k_sem_reset(&list_done);

	k_mutex_lock(&path_mtx, K_FOREVER);
	list_out   = out;
	list_max   = max;
	list_count = 0;
	k_mutex_unlock(&path_mtx);

	list_result = -EBUSY;
	atomic_set(&list_req, 1);

	int sret = k_sem_take(&list_done, K_SECONDS(10));
	if (sret) {
		LOG_ERR("sd_writer_list_sessions: timeout");
		return -ETIMEDOUT;
	}
	*count_out = list_count;
	return list_result;
}

int sd_writer_touch_file(const char *path)
{
	if (!atomic_get(&thread_alive)) return -ENOENT;

	k_sem_reset(&touch_done);

	k_mutex_lock(&path_mtx, K_FOREVER);
	strncpy(touch_path_buf, path, sizeof(touch_path_buf) - 1);
	touch_path_buf[sizeof(touch_path_buf) - 1] = '\0';
	k_mutex_unlock(&path_mtx);

	touch_result = -EBUSY;
	atomic_set(&touch_req, 1);

	int sret = k_sem_take(&touch_done, K_SECONDS(5));
	if (sret) {
		LOG_ERR("sd_writer_touch_file: writer thread did not "
			"signal within timeout");
		return -ETIMEDOUT;
	}
	return touch_result;
}

uint32_t sd_writer_audio_bytes_written(void)    { return audio_bytes; }
uint32_t sd_writer_imu_samples_written(void)    { return imu_samples; }
uint32_t sd_writer_audio_dropped(void)          { return audio_dropped; }
uint32_t sd_writer_imu_dropped(void)            { return imu_dropped; }
uint32_t sd_writer_rotate_deferred_total(void)  { return rotate_deferred_total; }
