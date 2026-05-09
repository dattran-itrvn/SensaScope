/*
 * SensaPulse — SD subsystem stress test (#23 isolation).
 *
 * Why this exists: the production app's empty-session bug looks like an
 * SD R/W issue, but the production firmware also runs PDM, IMU sampling,
 * a session FSM, and a watchdog. We can't tell whether the bug is in SD
 * code itself or in something interacting with SD. This standalone
 * firmware mounts FATFS and only writes/reads, so any error here is
 * unambiguously in the SD path (driver / FATFS / hardware).
 *
 * Phases (all auto, no human input):
 *   1. Sequential bulk write — 1 MB blocks × 20 = 20 MB at peak rate.
 *   2. Sustained PDM-rate emulation — 4 KB / 25 ms ≈ 160 KB/s for 60 s
 *      (above the 64 KB/s of real audio so we have headroom).
 *   3. Dual-file interleave — 4 KB/25 ms to audio.dat + 256 B/200 ms
 *      to imu.dat in parallel, mimicking the production writer pair.
 *   4. With periodic fs_sync — phase 2 again but fs_sync every 5 s.
 *
 * Output: one line per phase summary plus a final
 *   STRESS_SUMMARY: { ... JSON ... }
 * line that scripts/parse_sd_stress.py grep'es for.
 *
 * LED: 1 Hz blink throughout so we can see the firmware is alive on the
 * bench. Solid OFF when the test is done.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sd_log.h"
#include "audio.h"
#include "imu.h"
#include "imu_sampler.h"
#include "sd_writer.h"

LOG_MODULE_REGISTER(stress, LOG_LEVEL_INF);

/* ---------- LED liveness blinker ---------- */
static const struct gpio_dt_spec led_spec =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

static void led_tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	static int on;
	on = !on;
	gpio_pin_set_dt(&led_spec, on);
}
K_TIMER_DEFINE(led_timer, led_tick, NULL);

/* ---------- Per-phase metrics ---------- */
struct phase_stats {
	const char *name;
	uint32_t writes;
	uint32_t errors;
	uint32_t total_bytes;
	uint64_t total_us;
	uint32_t max_latency_us;
	int      first_err_code;
	int64_t  first_err_byte;
};

static void stats_record(struct phase_stats *s, int ret, uint32_t bytes,
			 uint64_t lat_us)
{
	s->writes++;
	if (ret < 0) {
		s->errors++;
		if (s->first_err_code == 0) {
			s->first_err_code = ret;
			s->first_err_byte = s->total_bytes;
		}
		LOG_ERR("write[%s] err=%d at offset=%u (write #%u)",
			s->name, ret, s->total_bytes, s->writes);
		return;
	}
	s->total_bytes += bytes;
	s->total_us    += lat_us;
	if (lat_us > s->max_latency_us) s->max_latency_us = (uint32_t)lat_us;
}

static void stats_log(const struct phase_stats *s)
{
	uint32_t avg_us = s->writes ? (uint32_t)(s->total_us / s->writes) : 0;
	uint32_t kbps   = s->total_us
		? (uint32_t)((uint64_t)s->total_bytes * 1000 / s->total_us)
		: 0;
	LOG_INF("[%s] writes=%u errors=%u total=%u B avg=%u us "
		"max=%u us throughput=%u kB/s",
		s->name, s->writes, s->errors, s->total_bytes,
		avg_us, s->max_latency_us, kbps);
	if (s->errors) {
		LOG_ERR("[%s] first error: code=%d at byte=%lld",
			s->name, s->first_err_code, (long long)s->first_err_byte);
	}
}

/* ---------- Phase helpers ---------- */
static int open_w(struct fs_file_t *f, const char *path)
{
	fs_file_t_init(f);
	int ret = fs_open(f, path, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC);
	if (ret) LOG_ERR("open %s: %d", path, ret);
	return ret;
}

static uint64_t now_us(void)
{
	return k_ticks_to_us_floor64(k_uptime_ticks());
}

/* Buffer reused across phases — kept small so the stack stays sane. */
static uint8_t buf[4096];

static void fill_pattern(uint32_t seed)
{
	for (size_t i = 0; i < sizeof(buf); i++) {
		seed = seed * 1103515245u + 12345u;
		buf[i] = (uint8_t)(seed >> 16);
	}
}

/* ---------- Phase 1: sequential bulk write ---------- */
static int phase_seq_bulk(struct phase_stats *s)
{
	*s = (struct phase_stats){.name = "seq_bulk"};
	struct fs_file_t f;
	if (open_w(&f, SD_MOUNT_POINT "/STRESS_SEQ.DAT") < 0) return -EIO;

	fill_pattern(0xA5A5A5A5);
	const uint32_t target = 20u * 1024u * 1024u;       /* 20 MB */
	uint32_t written = 0;

	while (written < target) {
		uint64_t t0 = now_us();
		int ret = fs_write(&f, buf, sizeof(buf));
		uint64_t lat = now_us() - t0;
		stats_record(s, ret, ret > 0 ? ret : 0, lat);
		if (ret < 0) break;          /* abort phase on hard error */
		written += ret;
	}

	fs_sync(&f);
	fs_close(&f);
	stats_log(s);
	fs_unlink(SD_MOUNT_POINT "/STRESS_SEQ.DAT");
	return 0;
}

/* ---------- Phase 2: sustained rate ---------- */
static int phase_sustained(struct phase_stats *s, int seconds, bool with_sync)
{
	s->name = with_sync ? "sustained_synced" : "sustained";
	s->writes = s->errors = s->total_bytes = 0;
	s->total_us = 0;
	s->max_latency_us = 0;
	s->first_err_code = 0;
	s->first_err_byte = 0;

	struct fs_file_t f;
	if (open_w(&f, with_sync
		   ? SD_MOUNT_POINT "/STRESS_SYN.DAT"
		   : SD_MOUNT_POINT "/STRESS_SUS.DAT") < 0)
		return -EIO;

	fill_pattern(with_sync ? 0xC3C3C3C3 : 0x5A5A5A5A);
	const int     period_ms   = 25;            /* 4 KB / 25 ms = 160 KB/s */
	const int     write_size  = 4096;
	const int64_t deadline_ms = k_uptime_get() + seconds * 1000;
	int64_t       last_sync   = k_uptime_get();

	while (k_uptime_get() < deadline_ms) {
		int64_t t_loop = k_uptime_get();

		uint64_t t0 = now_us();
		int ret = fs_write(&f, buf, write_size);
		uint64_t lat = now_us() - t0;
		stats_record(s, ret, ret > 0 ? ret : 0, lat);
		if (ret < 0) break;

		if (with_sync && (k_uptime_get() - last_sync) >= 5000) {
			fs_sync(&f);
			last_sync = k_uptime_get();
		}

		int64_t elapsed = k_uptime_get() - t_loop;
		if (elapsed < period_ms) k_msleep(period_ms - elapsed);
	}

	fs_sync(&f);
	fs_close(&f);
	stats_log(s);
	fs_unlink(with_sync
		  ? SD_MOUNT_POINT "/STRESS_SYN.DAT"
		  : SD_MOUNT_POINT "/STRESS_SUS.DAT");
	return 0;
}

/* ---------- Phase 3: dual-file interleave ---------- */
static int phase_dual(struct phase_stats *sa, struct phase_stats *si,
		      int seconds)
{
	*sa = (struct phase_stats){.name = "dual_audio"};
	*si = (struct phase_stats){.name = "dual_imu"};

	struct fs_file_t fa, fi;
	if (open_w(&fa, SD_MOUNT_POINT "/STRESS_AUD.DAT") < 0) return -EIO;
	if (open_w(&fi, SD_MOUNT_POINT "/STRESS_IMU.DAT") < 0) {
		fs_close(&fa);
		return -EIO;
	}

	const int period_ms = 25;
	const int aud_size  = 4096;        /* like audio block */
	const int imu_size  = 256;         /* like imu flush */
	const int imu_every = 8;           /* once every 200 ms */
	int       imu_tick  = 0;

	const int64_t deadline_ms = k_uptime_get() + seconds * 1000;
	int64_t       last_sync   = k_uptime_get();

	fill_pattern(0xDEADBEEF);
	while (k_uptime_get() < deadline_ms) {
		int64_t t_loop = k_uptime_get();

		uint64_t t0 = now_us();
		int ret = fs_write(&fa, buf, aud_size);
		uint64_t lat = now_us() - t0;
		stats_record(sa, ret, ret > 0 ? ret : 0, lat);
		if (ret < 0) break;

		if (++imu_tick >= imu_every) {
			imu_tick = 0;
			t0 = now_us();
			ret = fs_write(&fi, buf, imu_size);
			lat = now_us() - t0;
			stats_record(si, ret, ret > 0 ? ret : 0, lat);
			if (ret < 0) break;
		}

		if ((k_uptime_get() - last_sync) >= 5000) {
			fs_sync(&fa);
			fs_sync(&fi);
			last_sync = k_uptime_get();
		}

		int64_t elapsed = k_uptime_get() - t_loop;
		if (elapsed < period_ms) k_msleep(period_ms - elapsed);
	}

	fs_sync(&fa); fs_sync(&fi);
	fs_close(&fa); fs_close(&fi);
	stats_log(sa);
	stats_log(si);
	fs_unlink(SD_MOUNT_POINT "/STRESS_AUD.DAT");
	fs_unlink(SD_MOUNT_POINT "/STRESS_IMU.DAT");
	return 0;
}

/* ---------- Phases 5 / 6 — production data path through sd_writer (#25) ---
 *
 * Architecture under test:
 *   audio_producer  ──→ audio_msgq ─┐
 *                                    ├─→ sd_writer_thread ─→ FATFS
 *   imu_producer    ──→ imu_msgq   ─┘
 *
 * Phase 5 stops imu_producer right after start so audio path is exercised
 * alone through sd_writer (single FATFS thread, no IMU contention).
 * Phase 6 lets both producers run for the full 60 s — this is the test
 * that previously failed 3/5 with the per-stream-FATFS design.
 */
struct dpath_stats {
	const char *name;
	uint32_t    audio_bytes;
	uint32_t    imu_samples;
	bool        writer_failed;
	bool        writer_alive_at_stop;
	uint32_t    audio_dropped;
	uint32_t    imu_dropped;
	uint32_t    duration_s;
};

static int run_data_path(struct dpath_stats *s, int seconds,
			 const char *audio_path, const char *csv_path,
			 bool stop_imu_immediately)
{
	if (audio_init() != 0) {
		LOG_ERR("[%s] audio_init failed", s->name);
		s->writer_failed = true;
		return -1;
	}
	if (!stop_imu_immediately && imu_init() != 0) {
		LOG_ERR("[%s] imu_init failed", s->name);
		s->writer_failed = true;
		return -1;
	}

	if (sd_writer_start(audio_path, csv_path) != 0) {
		LOG_ERR("[%s] sd_writer_start failed", s->name);
		s->writer_failed = true;
		return -1;
	}
	if (stop_imu_immediately) {
		imu_producer_stop();
		while (imu_producer_is_running()) k_msleep(20);
	}

	const int64_t deadline = k_uptime_get() + (int64_t)seconds * 1000;
	while (k_uptime_get() < deadline) {
		k_msleep(5000);
		LOG_INF("[%s] heartbeat: audio=%u B imu=%u samples "
			"(writer_alive=%d failed=%d, dropped a=%u i=%u)",
			s->name,
			sd_writer_audio_bytes_written(),
			sd_writer_imu_samples_written(),
			sd_writer_is_running(),
			sd_writer_failed(),
			sd_writer_audio_dropped(),
			sd_writer_imu_dropped());
		if (sd_writer_failed()) {
			LOG_ERR("[%s] sd_writer reports FAILED — early stop",
				s->name);
			break;
		}
	}

	s->writer_alive_at_stop = sd_writer_is_running();
	sd_writer_stop();

	s->audio_bytes    = sd_writer_audio_bytes_written();
	s->imu_samples    = sd_writer_imu_samples_written();
	s->writer_failed  = sd_writer_failed();
	s->audio_dropped  = sd_writer_audio_dropped();
	s->imu_dropped    = sd_writer_imu_dropped();

	LOG_INF("[%s] DONE audio=%u B (failed=%d, dropped=%u) "
		"imu=%u samples (dropped=%u) — expected audio ~%u B, "
		"imu ~%u samples",
		s->name, s->audio_bytes, s->writer_failed, s->audio_dropped,
		s->imu_samples, s->imu_dropped,
		AUDIO_PDM_RATE_HZ * AUDIO_PDM_CHANNELS * 2 * seconds,
		stop_imu_immediately ? 0 : IMU_SAMPLER_RATE_HZ * seconds);

	fs_unlink(audio_path);
	fs_unlink(csv_path);
	return s->writer_failed ? -1 : 0;
}

static int phase_pdm_only(struct dpath_stats *s, int seconds)
{
	*s = (struct dpath_stats){.name = "pdm_only", .duration_s = seconds};
	return run_data_path(s, seconds,
			     SD_MOUNT_POINT "/STRESS_PDM5.WAV",
			     SD_MOUNT_POINT "/STRESS_IMU5.CSV",
			     true);
}

static int phase_pdm_imu(struct dpath_stats *s, int seconds)
{
	*s = (struct dpath_stats){.name = "pdm_imu", .duration_s = seconds};
	return run_data_path(s, seconds,
			     SD_MOUNT_POINT "/STRESS_PDM6.WAV",
			     SD_MOUNT_POINT "/STRESS_IMU6.CSV",
			     false);
}

/* ---------- Final summary line ---------- */
static void summary_json(const struct phase_stats *p1, const struct phase_stats *p2,
			 const struct phase_stats *p3a, const struct phase_stats *p3b,
			 const struct phase_stats *p4,
			 const struct dpath_stats *p5,
			 const struct dpath_stats *p6)
{
	const struct phase_stats *all[] = { p1, p2, p3a, p3b, p4 };
	uint32_t total_writes = 0, total_errors = 0;
	uint64_t total_bytes  = 0;
	uint32_t max_latency  = 0;
	for (size_t i = 0; i < ARRAY_SIZE(all); i++) {
		total_writes += all[i]->writes;
		total_errors += all[i]->errors;
		total_bytes  += all[i]->total_bytes;
		if (all[i]->max_latency_us > max_latency)
			max_latency = all[i]->max_latency_us;
	}

	uint32_t expected_audio_bytes =
		(uint32_t)AUDIO_PDM_RATE_HZ * AUDIO_PDM_CHANNELS * 2 * p5->duration_s;
	uint32_t expected_imu_samples =
		(uint32_t)IMU_SAMPLER_RATE_HZ * p6->duration_s;

	LOG_INF("STRESS_SUMMARY: {"
		"\"writes\":%u,"
		"\"errors\":%u,"
		"\"bytes\":%llu,"
		"\"max_latency_us\":%u,"
		"\"phases\":{"
			"\"seq_bulk\":{\"w\":%u,\"e\":%u,\"b\":%u,\"first_err\":%d},"
			"\"sustained\":{\"w\":%u,\"e\":%u,\"b\":%u,\"first_err\":%d},"
			"\"dual_audio\":{\"w\":%u,\"e\":%u,\"b\":%u,\"first_err\":%d},"
			"\"dual_imu\":{\"w\":%u,\"e\":%u,\"b\":%u,\"first_err\":%d},"
			"\"sustained_synced\":{\"w\":%u,\"e\":%u,\"b\":%u,\"first_err\":%d},"
			"\"pdm_only\":{\"audio_b\":%u,\"audio_expected_b\":%u,"
				      "\"writer_failed\":%d,"
				      "\"audio_dropped\":%u,\"sec\":%u},"
			"\"pdm_imu\":{\"audio_b\":%u,\"audio_expected_b\":%u,"
				     "\"writer_failed\":%d,"
				     "\"audio_dropped\":%u,"
				     "\"imu_samples\":%u,\"imu_expected\":%u,"
				     "\"imu_dropped\":%u,\"sec\":%u}"
		"}}",
		total_writes, total_errors,
		(unsigned long long)total_bytes, max_latency,
		p1->writes, p1->errors, p1->total_bytes, p1->first_err_code,
		p2->writes, p2->errors, p2->total_bytes, p2->first_err_code,
		p3a->writes, p3a->errors, p3a->total_bytes, p3a->first_err_code,
		p3b->writes, p3b->errors, p3b->total_bytes, p3b->first_err_code,
		p4->writes, p4->errors, p4->total_bytes, p4->first_err_code,
		p5->audio_bytes, expected_audio_bytes, (int)p5->writer_failed,
		p5->audio_dropped, p5->duration_s,
		p6->audio_bytes,
		(uint32_t)AUDIO_PDM_RATE_HZ * AUDIO_PDM_CHANNELS * 2 * p6->duration_s,
		(int)p6->writer_failed, p6->audio_dropped,
		p6->imu_samples, expected_imu_samples,
		p6->imu_dropped, p6->duration_s);
}

/* ---------- main ---------- */
int main(void)
{
	LOG_INF("SD STRESS v1.0 — start");
	LOG_INF("Build: %s %s", __DATE__, __TIME__);

	if (gpio_is_ready_dt(&led_spec)) {
		gpio_pin_configure_dt(&led_spec, GPIO_OUTPUT_INACTIVE);
		k_timer_start(&led_timer, K_MSEC(500), K_MSEC(500));
	}

	if (sdlog_init() != 0) {
		LOG_ERR("sdlog_init failed — aborting");
		return -1;
	}

	struct phase_stats p1 = {0}, p2 = {0}, p3a = {0}, p3b = {0}, p4 = {0};
	struct dpath_stats p5 = {0}, p6 = {0};

	LOG_INF("------- phase 1: seq_bulk -------");
	phase_seq_bulk(&p1);

	LOG_INF("------- phase 2: sustained 60 s -------");
	phase_sustained(&p2, 60, false);

	LOG_INF("------- phase 3: dual-file 60 s -------");
	phase_dual(&p3a, &p3b, 60);

	LOG_INF("------- phase 4: sustained_synced 60 s -------");
	phase_sustained(&p4, 60, true);

	LOG_INF("------- phase 5: pdm_only 60 s -------");
	phase_pdm_only(&p5, 60);

	LOG_INF("------- phase 6: pdm_imu 60 s -------");
	phase_pdm_imu(&p6, 60);

	LOG_INF("------- DONE -------");
	summary_json(&p1, &p2, &p3a, &p3b, &p4, &p5, &p6);
	LOG_INF("Stress test complete. LED off, halting.");

	k_timer_stop(&led_timer);
	if (gpio_is_ready_dt(&led_spec)) {
		gpio_pin_set_dt(&led_spec, 0);
	}

	while (1) k_sleep(K_FOREVER);
	return 0;
}
