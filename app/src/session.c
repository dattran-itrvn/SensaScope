#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "device_id.h"
#include "imu_sampler.h"
#include "sd_log.h"
#include "sd_writer.h"
#include "session.h"

LOG_MODULE_REGISTER(session, LOG_LEVEL_INF);

#ifndef FW_VERSION
#define FW_VERSION "v1.0.0-dev"
#endif
#ifndef FW_BUILD_HASH
#define FW_BUILD_HASH "unknown"
#endif

#define SYNC_STATE_PATH    SD_MOUNT_POINT "/sync_state.json"
#define MIN_FREE_MB        100
#define SESSION_PREFIX     "SESSION_"
#define FOLDER_FMT         SD_MOUNT_POINT "/" SESSION_PREFIX "%05u"
#define AUDIO_FMT          SD_MOUNT_POINT "/" SESSION_PREFIX "%05u/audio.wav"
#define CSV_FMT            SD_MOUNT_POINT "/" SESSION_PREFIX "%05u/imu.csv"
#define META_FMT           SD_MOUNT_POINT "/" SESSION_PREFIX "%05u/meta.json"
#define UNSYNCED_FMT       SD_MOUNT_POINT "/" SESSION_PREFIX "%05u/.unsynced"

/* ---------- Module state ---------- */
static uint32_t      next_id      = 1;
static uint32_t      current_id   = 0;
static int           batt_at_start;
static bool          active;
static bool          aborted;       /* set by monitor; cleared by session_start */
static uint32_t      rotate_sec   = SESSION_ROTATE_SEC_DEFAULT;
static struct k_timer rotate_timer;
static struct k_work  rotate_work;

/* Monitor watchdog. Fires once per second while a session is active and
 * aborts if either writer thread has silently exited (e.g. audio recorder
 * hit dmic_read/fs_write error and left rec_running=0 while the IMU
 * sampler kept going — the bug Cowork caught in #11 testing).
 *
 * #23/#25: monitor also owns the .unsynced marker now. The marker is
 * touched only after sd_writer_audio_bytes_written() > 0 for the current
 * session, i.e. only once we know real data exists on the card. Crashed-
 * empty sessions never get a marker, so BLE LIST naturally skips them.
 */
#define MONITOR_INTERVAL_SEC  1
static struct k_timer monitor_timer;
static struct k_work  monitor_work;
static uint32_t       unsynced_marker_id;   /* 0 = not yet set this session */
static uint32_t       monitor_ticks;

/* ---------- Helpers ---------- */

/* #17: scan sessions via sd_writer (holds single-FATFS-owner invariant);
 * find lowest session_id whose `.unsynced` marker is absent (= PC has
 * ACKed). Returns 0 if no synced folder available to evict. */
static uint16_t find_oldest_synced(void)
{
	static struct sd_writer_session_info infos[SD_WRITER_LIST_MAX];
	uint32_t count = 0;
	int ret = sd_writer_list_sessions(infos, SD_WRITER_LIST_MAX, &count);
	if (ret != 0) {
		LOG_WRN("eviction: list_sessions: %d", ret);
		return 0;
	}
	uint16_t oldest = 0;
	for (uint32_t i = 0; i < count; i++) {
		if (infos[i].is_unsynced) continue;
		if (oldest == 0 || infos[i].session_id < oldest) {
			oldest = infos[i].session_id;
		}
	}
	return oldest;
}

/* #17: unlink known files in SESSION_NNNNN, then the folder itself.
 * Routes every fs_unlink through sd_writer thread (single-FATFS-owner). */
static int remove_session_folder(uint16_t sid)
{
	static const char *const files[] = {
		".unsynced", "audio.wav", "imu.csv", "meta.json",
	};
	int io_err = 0;
	for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
		char p[80];
		snprintf(p, sizeof(p), "%s/" SESSION_PREFIX "%05u/%s",
			 SD_MOUNT_POINT, (unsigned)sid, files[i]);
		int r = sd_writer_unlink(p);
		if (r != 0 && r != -ENOENT) {
			LOG_WRN("evict: unlink %s: %d", p, r);
			io_err = r;
		}
	}
	char folder[80];
	snprintf(folder, sizeof(folder), FOLDER_FMT, (unsigned)sid);
	int fr = sd_writer_unlink(folder);
	if (fr != 0 && fr != -ENOENT) {
		LOG_ERR("evict: unlink folder %s: %d", folder, fr);
		return fr;
	}
	if (io_err) return io_err;
	LOG_INF("evict: removed SESSION_%05u", (unsigned)sid);
	return 0;
}

/* Scan /SD: for SESSION_NNNNN folders, return max id seen (0 if none). */
static uint32_t scan_max_session_id(void)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int ret = fs_opendir(&dir, SD_MOUNT_POINT);
	if (ret) {
		LOG_WRN("scan: opendir failed: %d", ret);
		return 0;
	}

	uint32_t max_id = 0;
	const size_t prefix_len = sizeof(SESSION_PREFIX) - 1;

	for (;;) {
		struct fs_dirent ent;
		ret = fs_readdir(&dir, &ent);
		if (ret || ent.name[0] == '\0') break;
		if (ent.type != FS_DIR_ENTRY_DIR) continue;
		if (strncmp(ent.name, SESSION_PREFIX, prefix_len) != 0) continue;

		const char *digits = ent.name + prefix_len;
		uint32_t id = (uint32_t)strtoul(digits, NULL, 10);
		if (id > max_id) max_id = id;
	}
	fs_closedir(&dir);
	return max_id;
}

static int load_counter(uint32_t *next_out)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, SYNC_STATE_PATH, FS_O_READ);
	if (ret) return ret;

	char body[128] = {0};
	int n = fs_read(&f, body, sizeof(body) - 1);
	fs_close(&f);
	if (n <= 0) return -EIO;

	/* Tiny ad-hoc parse: look for "next_session_id":<digits>. Avoid pulling
	 * a JSON parser into firmware for one field.
	 */
	const char *key = "\"next_session_id\"";
	char *p = strstr(body, key);
	if (!p) return -EINVAL;
	p += strlen(key);
	while (*p == ' ' || *p == ':' || *p == '\t') p++;
	if (*p < '0' || *p > '9') return -EINVAL;
	*next_out = (uint32_t)strtoul(p, NULL, 10);
	return 0;
}

static int save_counter(uint32_t next)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, SYNC_STATE_PATH,
			  FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret) {
		LOG_ERR("save_counter open: %d", ret);
		return ret;
	}

	char body[64];
	int len = snprintf(body, sizeof(body),
			   "{\"next_session_id\":%u}\n", next);
	ret = fs_write(&f, body, len);
	fs_close(&f);
	return ret < 0 ? ret : 0;
}

/* Pure formatter — no FATFS access. Used by both session_start (path A:
 * write to disk directly) and rotate (path B: hand the body to sd_writer
 * so the writer thread does the fs_open + fs_write atomically with the
 * other rotate steps).
 */
static int compute_meta_body(uint32_t id, char *out, size_t out_sz)
{
	return snprintf(out, out_sz,
		"{\n"
		"  \"session_id\": %u,\n"
		"  \"start_uptime_ms\": %lld,\n"
		"  \"fs_audio\": %d,\n"
		"  \"fs_imu\": %d,\n"
		"  \"fw_version\": \"%s\",\n"
		"  \"fw_build_hash\": \"%s\",\n"
		"  \"batt_mv_start\": %d,\n"
		"  \"chip_id\": \"%s\",\n"
		"  \"device_name\": \"%s\"\n"
		"}\n",
		id,
		(long long)k_uptime_get(),
		AUDIO_PDM_RATE_HZ,
		IMU_SAMPLER_RATE_HZ,
		FW_VERSION,
		FW_BUILD_HASH,
		batt_at_start,
		identity_get_chip_id(),
		identity_get_name());
}

static int write_meta(uint32_t id)
{
	char path[64];
	snprintf(path, sizeof(path), META_FMT, id);

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("meta open %s: %d", path, ret);
		return ret;
	}

	char body[384];
	int n = compute_meta_body(id, body, sizeof(body));

	fs_write(&f, body, n);
	fs_close(&f);
	return 0;
}

/* #32: trước đây hàm này gọi fs_open + fs_close trực tiếp từ
 * monitor_work_handler (chạy trên system_work_queue) → đụng sd_writer
 * thread khi nó đang fs_write → SD card timeout `-116` sau 1-40 phút.
 * Giờ route qua sd_writer_touch_file để chỉ sd_writer thread chạm FATFS.
 * Xem `docs/POSTMORTEM_SD_WRITE_RELIABILITY.md`.
 */
static int touch_unsynced(uint32_t id)
{
	char path[64];
	snprintf(path, sizeof(path), UNSYNCED_FMT, id);
	int ret = sd_writer_touch_file(path);
	if (ret) {
		LOG_ERR("touch_unsynced %s via sd_writer: %d", path, ret);
	}
	return ret;
}

/* Open folder #id: mkdir, write meta. Caller owns audio + imu writers.
 * Returns 0 on success.
 *
 * #23: deliberately does NOT touch .unsynced anymore. The marker is set by
 * the monitor watchdog only after the audio writer reports >0 bytes
 * actually written, so empty sessions (writer crash before first write)
 * never appear as sync targets.
 */
static int prepare_folder(uint32_t id)
{
	char folder[64];
	snprintf(folder, sizeof(folder), FOLDER_FMT, id);
	int ret = fs_mkdir(folder);
	if (ret && ret != -EEXIST) {
		LOG_ERR("mkdir %s: %d", folder, ret);
		return ret;
	}
	ret = write_meta(id);
	if (ret) return ret;
	return 0;
}

/* ---------- Rotation ---------- */
static void rotate_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!active) return;

	/* #32: KHÔNG gọi statvfs_free_mb ở đây nữa. fs_statvfs đụng FATFS
	 * volume mutex từ system_work_queue đồng thời với sd_writer's
	 * fs_write → trigger -116 timeout (xem docs/POSTMORTEM_SD_WRITE_
	 * RELIABILITY.md). Khi task #17 (eviction) làm tới, route qua
	 * sd_writer_get_free_mb (API mới) thay vì gọi trực tiếp.
	 */

	/* #27: rotate no longer touches FATFS from the system_work_queue.
	 * Compute folder + paths + meta body only; hand everything to
	 * sd_writer which does mkdir + meta write + finalize old + open new
	 * inside its own thread → no FATFS lock contention with the
	 * concurrent audio + IMU writes.
	 */
	uint32_t new_id = next_id;

	char folder[64], audio_path[64], csv_path[64];
	snprintf(folder,     sizeof(folder),     FOLDER_FMT, new_id);
	snprintf(audio_path, sizeof(audio_path), AUDIO_FMT,  new_id);
	snprintf(csv_path,   sizeof(csv_path),   CSV_FMT,    new_id);

	char meta[384];
	int meta_n = compute_meta_body(new_id, meta, sizeof(meta));
	if (meta_n <= 0 || meta_n > (int)sizeof(meta)) {
		LOG_ERR("rotate: compute_meta_body failed: %d", meta_n);
		return;
	}

	int rr = sd_writer_rotate_full(folder, audio_path, csv_path,
				       meta, (uint32_t)meta_n);
	if (rr) {
		/* #30: sd_writer kept old handles alive (defer-on-fail). We
		 * stay on current_id; next rotate timer tick will retry into
		 * the SAME new_id. After ROTATE_FAIL_LIMIT consecutive defers
		 * sd_writer escalates to failed=1 → watchdog → ERROR. */
		LOG_WRN("rotate deferred (sd_writer: %d, keep SESSION_%05u, "
			"total deferred=%u)", rr, current_id,
			sd_writer_rotate_deferred_total());
		return;
	}

	next_id = new_id + 1;
	/* #27: do NOT save_counter() here — would call fs_write on
	 * /SD/sync_state.json from system_work_queue, racing sd_writer.
	 * On reboot mid-session, scan_max_session_id() rebuilds the counter
	 * from on-disk folders. Counter persistence at session_start +
	 * session_stop is sufficient. */
	uint32_t old_id = current_id;
	current_id = new_id;
	unsynced_marker_id = 0;          /* #23: marker for the new id only */
	LOG_INF("rotate: SESSION_%05u → SESSION_%05u", old_id, new_id);
}

static void rotate_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&rotate_work);
}

/* ---------- Watchdog ---------- */
static void monitor_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!active) return;

	bool writer_alive   = sd_writer_is_running();
	bool writer_failed  = sd_writer_failed();
	bool audio_prod     = audio_producer_is_running();
	bool imu_prod       = imu_producer_is_running();

	if (writer_alive && audio_prod && imu_prod) {
		uint32_t a_bytes  = sd_writer_audio_bytes_written();
		uint32_t i_samp   = sd_writer_imu_samples_written();

		/* #23: set the .unsynced marker exactly once, after the SD
		 * writer reports its first non-zero byte count for this
		 * session. Sessions whose writer crashes before any data lands
		 * never reach this branch and stay marker-less → BLE LIST will
		 * skip them, no manual cleanup needed.
		 *
		 * #28: skip while a rotate is pending — the new folder's mkdir
		 * may not have landed yet and touch_unsynced would log a
		 * spurious -ENOENT before retrying on the next 1 Hz tick.
		 */
		if (unsynced_marker_id != current_id && a_bytes > 0
		    && !sd_writer_is_rotating()) {
			if (touch_unsynced(current_id) == 0) {
				unsynced_marker_id = current_id;
				LOG_INF("monitor: SESSION_%05u .unsynced marker "
					"set (audio=%u B)", current_id, a_bytes);
			}
		}

		/* Heartbeat every 5 s. Includes drop + deferred-rotate
		 * counters so we can see back-pressure / rotate resilience in
		 * real time. */
		monitor_ticks++;
		if ((monitor_ticks % 5) == 0) {
			LOG_INF("monitor: SESSION_%05u tick=%u, "
				"audio=%u B, imu=%u samples, "
				"dropped audio=%u imu=%u, "
				"rotate_deferred=%u",
				current_id, monitor_ticks, a_bytes, i_samp,
				sd_writer_audio_dropped(),
				sd_writer_imu_dropped(),
				sd_writer_rotate_deferred_total());
		}
		return;  /* healthy */
	}

	LOG_ERR("monitor: writer trouble — aborting SESSION_%05u "
		"(writer_alive=%d failed=%d audio_prod=%d imu_prod=%d, "
		"audio=%u B, imu=%u samples)",
		current_id, writer_alive, writer_failed,
		audio_prod, imu_prod,
		sd_writer_audio_bytes_written(),
		sd_writer_imu_samples_written());

	/* Inline the cleanup — we can't call session_stop() because that
	 * would re-stop our own timers, and we're already running on the
	 * system work queue.
	 */
	aborted = true;
	active  = false;
	k_timer_stop(&rotate_timer);
	k_timer_stop(&monitor_timer);
	sd_writer_stop();
	current_id = 0;
}

static void monitor_timer_cb(struct k_timer *t)
{
	ARG_UNUSED(t);
	k_work_submit(&monitor_work);
}

/* ---------- Public API ---------- */
int session_init(void)
{
	k_work_init(&rotate_work, rotate_work_handler);
	k_timer_init(&rotate_timer, rotate_timer_cb, NULL);
	k_work_init(&monitor_work, monitor_work_handler);
	k_timer_init(&monitor_timer, monitor_timer_cb, NULL);

	/* Counter file is the source of truth IF it's consistent with on-disk
	 * folders. But rotation does NOT save_counter (#27 closure), so a crash
	 * mid-session can leave sync_state.json behind the actual max folder id.
	 * Take max(loaded, scan+1) so prepare_folder never hits -EEXIST.
	 */
	uint32_t loaded = 0;
	bool have_file = (load_counter(&loaded) == 0);
	uint32_t scanned = scan_max_session_id();
	uint32_t scan_next = scanned + 1;

	if (have_file && loaded >= scan_next) {
		next_id = loaded;
		LOG_INF("counter loaded, next_id=%u (scan max=%u)",
			next_id, scanned);
	} else {
		next_id = scan_next;
		if (have_file) {
			LOG_WRN("counter file stale (%u), scan max=%u "
				"→ corrected next_id=%u",
				loaded, scanned, next_id);
		} else {
			LOG_INF("counter rebuilt by scan, max=%u → next_id=%u",
				scanned, next_id);
		}
		save_counter(next_id);
	}
	return 0;
}

void session_set_rotate_sec(uint32_t seconds)
{
	if (seconds < 5) seconds = 5;          /* sanity — too fast breaks SD */
	rotate_sec = seconds;
	LOG_INF("rotation period = %u s", rotate_sec);
}

int session_start(int batt_mv)
{
	if (active) return -EALREADY;

	/* #17: free-space + eviction loop.
	 * - Query free via sd_writer (keeps single-FATFS-owner invariant).
	 * - If below MIN_FREE_MB, try to evict the oldest synced folder. Loop
	 *   until free ≥ MIN_FREE_MB OR no synced folder left.
	 * - No synced folder + still below threshold → refuse, FSM goes ERROR.
	 */
	uint32_t free_mb = 0;
	int fret = sd_writer_get_free_mb(&free_mb);
	if (fret == 0 && free_mb < MIN_FREE_MB) {
		LOG_WRN("session_start: only %u MB free, trying eviction", free_mb);
		while (free_mb < MIN_FREE_MB) {
			uint16_t victim = find_oldest_synced();
			if (victim == 0) {
				LOG_ERR("session_start: %u MB free, no synced folder "
					"to evict — sync required before record",
					free_mb);
				return SESSION_ERR_NO_SPACE;
			}
			int rret = remove_session_folder(victim);
			if (rret != 0) {
				LOG_ERR("session_start: evict SESSION_%05u failed: %d",
					(unsigned)victim, rret);
				return SESSION_ERR_NO_SPACE;
			}
			if (sd_writer_get_free_mb(&free_mb) != 0) break;
		}
		LOG_INF("session_start: eviction OK, now %u MB free", free_mb);
	}

	batt_at_start = batt_mv;
	uint32_t id = next_id;
	int ret = prepare_folder(id);
	if (ret) return ret;

	char audio_path[64], csv_path[64];
	snprintf(audio_path, sizeof(audio_path), AUDIO_FMT, id);
	snprintf(csv_path,   sizeof(csv_path),   CSV_FMT,   id);

	/* #32: save_counter PHẢI gọi TRƯỚC sd_writer_start. Trước fix này,
	 * save_counter chạy SAU khi sd_writer thread đã khởi động → 2 thread
	 * cùng đụng FATFS → contend với volume mutex và stress card.
	 * Đặt trước → khi save_counter chạm FATFS, chưa có ai khác động vào.
	 * Nếu sd_writer_start fail sau đó, counter vẫn đã tăng — chỉ lãng phí
	 * 1 session id (folder orphan, không có .unsynced marker nên BLE
	 * LIST skip → vô hại).
	 */
	uint32_t new_next_id = id + 1;
	save_counter(new_next_id);

	ret = sd_writer_start(audio_path, csv_path);
	if (ret) {
		LOG_ERR("sd_writer_start failed: %d", ret);
		return ret;
	}

	current_id          = id;
	next_id             = new_next_id;
	unsynced_marker_id  = 0;        /* #23: reset, monitor will set it */
	monitor_ticks       = 0;
	aborted = false;
	active  = true;

	k_timer_start(&rotate_timer, K_SECONDS(rotate_sec),
		      K_SECONDS(rotate_sec));
	k_timer_start(&monitor_timer, K_SECONDS(MONITOR_INTERVAL_SEC),
		      K_SECONDS(MONITOR_INTERVAL_SEC));

	LOG_INF("session_start: SESSION_%05u, rotate every %u s, batt=%d mV",
		id, rotate_sec, batt_mv);
	return 0;
}

int session_stop(void)
{
	if (!active) return -ENOENT;

	/* Set inactive first so a monitor work item already queued sees it
	 * and skips immediately — avoids a spurious "writer died" abort
	 * while the writer threads are winding down on their stop_req.
	 */
	active = false;
	k_timer_stop(&rotate_timer);
	k_timer_stop(&monitor_timer);

	int rs = sd_writer_stop();
	if (rs && rs != -ENOENT) LOG_WRN("stop: sd_writer_stop: %d", rs);

	uint32_t closed = current_id;
	current_id = 0;
	LOG_INF("session_stop: closed SESSION_%05u", closed);
	return 0;
}

bool     session_is_active(void)    { return active; }
bool     session_was_aborted(void)  { return aborted; }
uint32_t session_current_id(void)   { return current_id; }
