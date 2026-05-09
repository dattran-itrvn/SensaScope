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
 */
#define MONITOR_INTERVAL_SEC  1
static struct k_timer monitor_timer;
static struct k_work  monitor_work;

/* ---------- Helpers ---------- */
static int statvfs_free_mb(uint32_t *mb_out)
{
	struct fs_statvfs s;
	int ret = fs_statvfs(SD_MOUNT_POINT, &s);
	if (ret) {
		LOG_ERR("fs_statvfs: %d", ret);
		return ret;
	}
	uint64_t bytes = (uint64_t)s.f_bsize * s.f_bfree;
	*mb_out = (uint32_t)(bytes / (1024 * 1024));
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
	int n = snprintf(body, sizeof(body),
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

	fs_write(&f, body, n);
	fs_close(&f);
	return 0;
}

static int touch_unsynced(uint32_t id)
{
	char path[64];
	snprintf(path, sizeof(path), UNSYNCED_FMT, id);

	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("unsynced open %s: %d", path, ret);
		return ret;
	}
	fs_close(&f);
	return 0;
}

/* Open folder #id: mkdir, write meta, touch .unsynced. Caller owns audio
 * + imu writers. Returns 0 on success.
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
	ret = touch_unsynced(id);
	if (ret) return ret;
	return 0;
}

/* ---------- Rotation ---------- */
static void rotate_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (!active) return;

	uint32_t free_mb = 0;
	if (statvfs_free_mb(&free_mb) == 0 && free_mb < MIN_FREE_MB) {
		LOG_WRN("rotate: only %u MB free — eviction is task #17, "
			"continuing into next folder anyway", free_mb);
		/* TODO #17: evict oldest synced folder; if none, stop session
		 * and have main FSM transition to ERROR.
		 */
	}

	uint32_t new_id = next_id++;
	if (prepare_folder(new_id) < 0) {
		LOG_ERR("rotate: prepare_folder %u failed — keeping current",
			new_id);
		next_id--;
		return;
	}

	char audio_path[64], csv_path[64];
	snprintf(audio_path, sizeof(audio_path), AUDIO_FMT, new_id);
	snprintf(csv_path,   sizeof(csv_path),   CSV_FMT,   new_id);

	int ra = audio_recorder_rotate(audio_path);
	int ri = imu_sampler_rotate(csv_path);
	if (ra || ri) {
		LOG_ERR("rotate: writer rotate failed (audio=%d imu=%d)", ra, ri);
		return;
	}

	save_counter(next_id);
	uint32_t old_id = current_id;
	current_id = new_id;
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

	bool audio_alive  = audio_recorder_is_running();
	bool audio_failed = audio_recorder_failed();
	bool imu_alive    = imu_sampler_is_running();

	if (audio_alive && imu_alive) {
		return;  /* healthy */
	}

	LOG_ERR("monitor: writer died — aborting SESSION_%05u "
		"(audio_alive=%d audio_failed=%d imu_alive=%d, "
		"audio=%u B, imu=%u samples)",
		current_id, audio_alive, audio_failed, imu_alive,
		audio_recorder_bytes_written(),
		imu_sampler_samples_written());

	/* Inline the cleanup — we can't call session_stop() because that
	 * would re-stop our own timers, and we're already running on the
	 * system work queue.
	 */
	aborted = true;
	active  = false;
	k_timer_stop(&rotate_timer);
	k_timer_stop(&monitor_timer);
	if (audio_alive) audio_recorder_stop();
	if (imu_alive)   imu_sampler_stop();
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

	if (load_counter(&next_id) == 0) {
		LOG_INF("counter loaded, next_id=%u", next_id);
		return 0;
	}
	uint32_t scanned = scan_max_session_id();
	next_id = scanned + 1;
	LOG_INF("counter rebuilt by scan, max=%u → next_id=%u",
		scanned, next_id);
	save_counter(next_id);
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

	uint32_t free_mb = 0;
	if (statvfs_free_mb(&free_mb) == 0 && free_mb < MIN_FREE_MB) {
		LOG_ERR("session_start: only %u MB free, refusing (need eviction "
			"or sync)", free_mb);
		return SESSION_ERR_NO_SPACE;
	}

	batt_at_start = batt_mv;
	uint32_t id = next_id;
	int ret = prepare_folder(id);
	if (ret) return ret;

	char audio_path[64], csv_path[64];
	snprintf(audio_path, sizeof(audio_path), AUDIO_FMT, id);
	snprintf(csv_path,   sizeof(csv_path),   CSV_FMT,   id);

	ret = audio_recorder_start(audio_path);
	if (ret) {
		LOG_ERR("audio_recorder_start failed: %d", ret);
		return ret;
	}
	ret = imu_sampler_start(csv_path);
	if (ret) {
		LOG_ERR("imu_sampler_start failed: %d", ret);
		audio_recorder_stop();
		return ret;
	}

	current_id = id;
	next_id    = id + 1;
	save_counter(next_id);
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

	int ra = audio_recorder_stop();
	int ri = imu_sampler_stop();
	if (ra && ra != -ENOENT) LOG_WRN("stop: audio_recorder_stop: %d", ra);
	if (ri && ri != -ENOENT) LOG_WRN("stop: imu_sampler_stop: %d", ri);

	uint32_t closed = current_id;
	current_id = 0;
	LOG_INF("session_stop: closed SESSION_%05u", closed);
	return 0;
}

bool     session_is_active(void)    { return active; }
bool     session_was_aborted(void)  { return aborted; }
uint32_t session_current_id(void)   { return current_id; }
