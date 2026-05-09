#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "imu.h"
#include "imu_sampler.h"

LOG_MODULE_REGISTER(imu_sampler, LOG_LEVEL_INF);

#define SAMPLER_STACK_SZ  3072
#define SAMPLER_PRIO      K_PRIO_PREEMPT(6)

#define BATCH_SAMPLES     IMU_SAMPLER_RATE_HZ           /* 52 ≈ 1 s */
#define BUF_SAMPLES       (BATCH_SAMPLES + 16)          /* margin */
#define LINE_MAX          80
#define FLUSH_BUF_SIZE    (BATCH_SAMPLES * LINE_MAX)    /* 4160 B */

struct imu_sample {
	uint64_t t_us;
	int16_t  ax, ay, az;
	int16_t  gx, gy, gz;
};

static K_THREAD_STACK_DEFINE(samp_stack, SAMPLER_STACK_SZ);
static struct k_thread samp_thread;

static struct fs_file_t  csv_file;
static atomic_t          samp_running    = ATOMIC_INIT(0);
static atomic_t          samp_stop_req   = ATOMIC_INIT(0);
static atomic_t          samp_rotate_req = ATOMIC_INIT(0);
static volatile uint32_t samp_count;
static uint32_t          samp_dropped;

static struct imu_sample buf[BUF_SAMPLES];
static int               buf_n;
static char              flush_text[FLUSH_BUF_SIZE];
static char              path_buf[64];
static char              next_path_buf[64];
static struct k_mutex    path_mtx;

static const char        csv_header[] = "t_us,ax,ay,az,gx,gy,gz\n";

static int swap_file_locked(const char *new_path)
{
	fs_close(&csv_file);

	fs_file_t_init(&csv_file);
	int ret = fs_open(&csv_file, new_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("sampler: rotate open %s: %d", new_path, ret);
		return ret;
	}
	fs_write(&csv_file, csv_header, sizeof(csv_header) - 1);

	strncpy(path_buf, new_path, sizeof(path_buf) - 1);
	path_buf[sizeof(path_buf) - 1] = '\0';
	LOG_INF("sampler: rotated → %s", new_path);
	return 0;
}

static int flush_to_file(void)
{
	if (buf_n == 0) return 0;

	int total = 0;
	for (int i = 0; i < buf_n; i++) {
		int n = snprintf(flush_text + total, FLUSH_BUF_SIZE - total,
				 "%llu,%d,%d,%d,%d,%d,%d\n",
				 (unsigned long long)buf[i].t_us,
				 buf[i].ax, buf[i].ay, buf[i].az,
				 buf[i].gx, buf[i].gy, buf[i].gz);
		if (n < 0 || total + n >= FLUSH_BUF_SIZE) {
			LOG_WRN("flush buf full at i=%d", i);
			break;
		}
		total += n;
	}

	int n_flushed = buf_n;
	int ret = fs_write(&csv_file, flush_text, total);
	buf_n = 0;
	if (ret < 0) {
		LOG_ERR("sampler: fs_write failed: %d", ret);
		return ret;
	}

	/* #23 SD reliability fix: force the dirty FATFS sector cache down to
	 * the card every flush (~1 Hz). Without this, a writer-thread death
	 * or chip reset wipes everything cached — exactly the failure mode
	 * we see as imu.csv = 0 B in SESSION_00001/3/7.
	 */
	int sret = fs_sync(&csv_file);
	if (sret < 0) {
		LOG_ERR("sampler: fs_sync failed: %d", sret);
		return sret;
	}
	LOG_INF("sampler: flushed %d samples (total=%u, sync OK)",
		n_flushed, samp_count);
	return ret;
}

static void sampler_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	const int64_t period_us = 1000000 / IMU_SAMPLER_RATE_HZ;  /* 19230 */
	int64_t next_us = k_ticks_to_us_floor64(k_uptime_ticks());

	LOG_INF("sampler: streaming @ %d Hz → %s",
		IMU_SAMPLER_RATE_HZ, path_buf);

	while (!atomic_get(&samp_stop_req)) {
		/* Honor a pending rotation: flush remaining samples, close old
		 * file, open new file, write header. The sampler period is
		 * 19 ms — adding ~50–150 ms of FATFS swap latency may delay
		 * one or two samples but they aren't dropped (they sit in the
		 * RAM buffer until the new file is open).
		 */
		if (atomic_get(&samp_rotate_req)) {
			flush_to_file();
			char path[64];
			k_mutex_lock(&path_mtx, K_FOREVER);
			strncpy(path, next_path_buf, sizeof(path) - 1);
			path[sizeof(path) - 1] = '\0';
			k_mutex_unlock(&path_mtx);

			if (swap_file_locked(path) < 0) {
				LOG_ERR("sampler: swap failed → stopping");
				atomic_clear(&samp_rotate_req);
				break;
			}
			atomic_clear(&samp_rotate_req);
		}

		int16_t a[3], g[3];
		int ret = imu_read_xlg(a, g);
		if (ret == 0) {
			if (buf_n < BUF_SAMPLES) {
				struct imu_sample *s = &buf[buf_n++];
				s->t_us = k_ticks_to_us_floor64(k_uptime_ticks());
				s->ax = a[0]; s->ay = a[1]; s->az = a[2];
				s->gx = g[0]; s->gy = g[1]; s->gz = g[2];
				samp_count++;
			} else {
				samp_dropped++;
			}
		}

		if (buf_n >= BATCH_SAMPLES) {
			int w = flush_to_file();
			if (w < 0) {
				LOG_ERR("imu csv fs_write: %d", w);
			}
		}

		next_us += period_us;
		int64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());
		int64_t delay  = next_us - now_us;
		if (delay > 0 && delay < 100000) {
			k_usleep(delay);
		} else if (delay <= 0) {
			/* fell behind > 1 period — reset baseline, log occasionally */
			next_us = now_us;
		}
	}

	flush_to_file();
	fs_close(&csv_file);
	atomic_clear(&samp_running);
	LOG_INF("sampler: stop, %u samples, %u dropped",
		samp_count, samp_dropped);
}

int imu_sampler_start(const char *csv_path)
{
	if (atomic_get(&samp_running)) {
		return -EALREADY;
	}

	k_mutex_init(&path_mtx);

	strncpy(path_buf, csv_path, sizeof(path_buf) - 1);
	path_buf[sizeof(path_buf) - 1] = '\0';

	fs_file_t_init(&csv_file);
	int ret = fs_open(&csv_file, path_buf, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("sampler: open %s: %d", path_buf, ret);
		return ret;
	}

	fs_write(&csv_file, csv_header, sizeof(csv_header) - 1);

	samp_count   = 0;
	samp_dropped = 0;
	buf_n        = 0;
	atomic_clear(&samp_stop_req);
	atomic_clear(&samp_rotate_req);
	atomic_set(&samp_running, 1);

	k_tid_t tid = k_thread_create(&samp_thread, samp_stack,
				      K_THREAD_STACK_SIZEOF(samp_stack),
				      sampler_thread_fn, NULL, NULL, NULL,
				      SAMPLER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(tid, "imu_samp");
	return 0;
}

int imu_sampler_stop(void)
{
	if (!atomic_get(&samp_running)) {
		return -ENOENT;
	}
	atomic_set(&samp_stop_req, 1);
	return 0;
}

int imu_sampler_rotate(const char *new_csv_path)
{
	if (!atomic_get(&samp_running)) {
		return -ENOENT;
	}
	k_mutex_lock(&path_mtx, K_FOREVER);
	strncpy(next_path_buf, new_csv_path, sizeof(next_path_buf) - 1);
	next_path_buf[sizeof(next_path_buf) - 1] = '\0';
	k_mutex_unlock(&path_mtx);
	atomic_set(&samp_rotate_req, 1);
	return 0;
}

bool imu_sampler_is_running(void)
{
	return atomic_get(&samp_running) != 0;
}

uint32_t imu_sampler_samples_written(void)
{
	return samp_count;
}
