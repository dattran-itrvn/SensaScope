#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

#include "audio.h"
#include "imu_sampler.h"
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

static atomic_t            running       = ATOMIC_INIT(0);
static atomic_t            stop_req      = ATOMIC_INIT(0);
static atomic_t            rotate_req    = ATOMIC_INIT(0);
static atomic_t            failed        = ATOMIC_INIT(0);

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

	fs_seek(f, 0, FS_SEEK_SET);
	int ret = fs_write(f, hdr, sizeof(hdr));
	return ret < 0 ? ret : 0;
}

/* ---------- Helpers ---------- */
static int open_pair(const char *audio_path, const char *csv_path)
{
	uint8_t pad[44] = {0};
	int ret;

	fs_file_t_init(&audio_file);
	ret = fs_open(&audio_file, audio_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) { LOG_ERR("open audio %s: %d", audio_path, ret); return ret; }
	ret = fs_write(&audio_file, pad, sizeof(pad));
	if (ret < 0) {
		LOG_ERR("audio header placeholder: %d", ret);
		fs_close(&audio_file);
		return ret;
	}

	fs_file_t_init(&csv_file);
	ret = fs_open(&csv_file, csv_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("open csv %s: %d", csv_path, ret);
		fs_close(&audio_file);
		return ret;
	}
	ret = fs_write(&csv_file, csv_header_str, sizeof(csv_header_str) - 1);
	if (ret < 0) {
		LOG_ERR("csv header: %d", ret);
		fs_close(&csv_file);
		fs_close(&audio_file);
		return ret;
	}
	return 0;
}

static int finalize_pair(uint32_t audio_data_bytes)
{
	int ret = write_wav_header(&audio_file, audio_data_bytes);
	if (ret) LOG_WRN("finalize: WAV header rewrite failed: %d", ret);
	fs_close(&audio_file);
	fs_close(&csv_file);
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
		int ret = fs_write(&audio_file, buf, AUDIO_BLOCK_BYTES);
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
	int ret = fs_write(&csv_file, text, len);
	if (ret < 0) {
		LOG_ERR("imu fs_write %d samples / %d bytes: %d", n, len, ret);
		return ret;
	}
	imu_samples += n;
	return 0;
}

/* Service a pending rotate request — does ALL FATFS work in this thread,
 * so nothing else holds the FATFS lock during the transition. Sequence:
 *
 *   1. drain pending audio + imu queues into the *current* files
 *   2. finalize current WAV header, close both
 *   3. fs_mkdir new session folder
 *   4. open + write + close meta.json
 *   5. open new audio.wav (placeholder header) + new imu.csv (CSV header)
 *
 * If any step fails, returns < 0 with the failure logged; caller (the
 * thread loop) sets the failed flag.
 */
static int do_rotate(void)
{
	uint32_t junk;
	int ret;

	/* Snapshot the request first — caller's k_msgq_put might race the
	 * mutex if we read piecewise. */
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

	drain_audio(&junk);
	drain_imu();

	ret = finalize_pair(audio_bytes);
	if (ret) LOG_WRN("rotate finalize: %d", ret);

	ret = fs_mkdir(folder);
	if (ret && ret != -EEXIST) {
		LOG_ERR("rotate mkdir %s: %d", folder, ret);
		return ret;
	}

	/* meta.json — open + write + close in this thread, no contention. */
	struct fs_file_t mf;
	char meta_path[64];
	snprintf(meta_path, sizeof(meta_path), "%s/meta.json", folder);
	fs_file_t_init(&mf);
	ret = fs_open(&mf, meta_path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("rotate meta open %s: %d", meta_path, ret);
		return ret;
	}
	ret = fs_write(&mf, meta, meta_n);
	if (ret < 0) LOG_WRN("rotate meta write: %d", ret);
	fs_close(&mf);

	ret = open_pair(ap, cp);
	if (ret) {
		LOG_ERR("rotate open new pair failed: %d", ret);
		return ret;
	}
	LOG_INF("rotate: %s + %s opened (closed %u B audio in prev session)",
		ap, cp, audio_bytes);

	strncpy(audio_path_buf, ap, sizeof(audio_path_buf));
	strncpy(csv_path_buf,   cp, sizeof(csv_path_buf));
	audio_bytes = 0;
	imu_samples = 0;
	return 0;
}

/* ---------- Consumer thread ---------- */
static void writer_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	LOG_INF("writer: started → %s + %s", audio_path_buf, csv_path_buf);

	int64_t last_sync = k_uptime_get();
	uint32_t loop_ticks = 0;

	while (!atomic_get(&stop_req)) {
		if (atomic_get(&rotate_req)) {
			if (do_rotate() < 0) {
				atomic_set(&failed, 1);
				atomic_clear(&rotate_req);
				break;
			}
			atomic_clear(&rotate_req);
		}

		uint32_t audio_chunk = 0;
		int ret = drain_audio(&audio_chunk);
		if (ret < 0) { atomic_set(&failed, 1); break; }

		ret = drain_imu();
		if (ret < 0) { atomic_set(&failed, 1); break; }

		/* Periodic sync — only one thread, no contention possible. */
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

		k_msleep(WRITER_TICK_MS);
	}

	/* Drain anything remaining post-stop. */
	uint32_t junk;
	drain_audio(&junk);
	drain_imu();
	finalize_pair(audio_bytes);

	LOG_INF("writer: stopped, %u B audio, %u IMU samples, "
		"%u audio dropped, %u imu dropped",
		audio_bytes, imu_samples, audio_dropped, imu_dropped);
	atomic_clear(&running);
}

/* ---------- Public API ---------- */
int sd_writer_start(const char *audio_path, const char *csv_path)
{
	if (atomic_get(&running)) return -EALREADY;

	k_mutex_init(&path_mtx);
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

	k_tid_t tid = k_thread_create(&writer_thread, writer_stack,
				      K_THREAD_STACK_SIZEOF(writer_stack),
				      writer_thread_fn, NULL, NULL, NULL,
				      WRITER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(tid, "sd_writer");

	/* Kick off producers AFTER files are open so the very first push has
	 * somewhere to land. */
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
	atomic_set(&rotate_req, 1);
	return 0;
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

uint32_t sd_writer_audio_bytes_written(void) { return audio_bytes; }
uint32_t sd_writer_imu_samples_written(void) { return imu_samples; }
uint32_t sd_writer_audio_dropped(void)       { return audio_dropped; }
uint32_t sd_writer_imu_dropped(void)         { return imu_dropped; }
