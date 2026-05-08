#include <zephyr/kernel.h>
#include <zephyr/audio/dmic.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <string.h>
#include "audio.h"

LOG_MODULE_REGISTER(audio, LOG_LEVEL_INF);

#define PDM_NODE          DT_NODELABEL(pdm0)
#define PDM_BLOCK_MS      100
#define PDM_BLOCK_SAMPLES (AUDIO_PDM_RATE_HZ * PDM_BLOCK_MS / 1000)
#define PDM_BLOCK_BYTES   (PDM_BLOCK_SAMPLES * AUDIO_PDM_CHANNELS * sizeof(int16_t))
#define PDM_BLOCK_COUNT   4

K_MEM_SLAB_DEFINE_STATIC(pdm_slab, PDM_BLOCK_BYTES, PDM_BLOCK_COUNT, 4);

static const struct device *const dmic_dev = DEVICE_DT_GET(PDM_NODE);

/* ---------- DMIC configure helper ---------- */
static int configure_dmic(void)
{
	struct pcm_stream_cfg stream = {
		.pcm_width = AUDIO_PDM_BIT_WIDTH,
		.mem_slab  = &pdm_slab,
	};
	struct dmic_cfg cfg = {
		.io = {
			.min_pdm_clk_freq = 1000000,
			.max_pdm_clk_freq = 3500000,
			.min_pdm_clk_dc   = 40,
			.max_pdm_clk_dc   = 60,
		},
		.streams = &stream,
		.channel = {
			.req_num_streams = 1,
			.req_num_chan    = AUDIO_PDM_CHANNELS,
			.req_chan_map_lo =
				dmic_build_channel_map(0, 0, PDM_CHAN_LEFT) |
				dmic_build_channel_map(1, 0, PDM_CHAN_RIGHT),
		},
	};
	cfg.streams[0].pcm_rate   = AUDIO_PDM_RATE_HZ;
	cfg.streams[0].block_size = PDM_BLOCK_BYTES;
	return dmic_configure(dmic_dev, &cfg);
}

int audio_init(void)
{
	if (!device_is_ready(dmic_dev)) {
		LOG_ERR("PDM device not ready");
		return -ENODEV;
	}
	return 0;
}

/* ---------- WAV header (canonical 44-byte PCM) ---------- */
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
	return fs_write(f, hdr, sizeof(hdr));
}

/* ---------- One-shot capture (kept for smoke test) ---------- */
int audio_record_to_wav(const char *path, int seconds,
			int32_t *peak_l, int32_t *peak_r,
			int32_t *mean_l, int32_t *mean_r)
{
	int ret = configure_dmic();
	if (ret < 0) {
		LOG_ERR("dmic_configure failed: %d", ret);
		return ret;
	}

	struct fs_file_t f;
	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("open %s: %d", path, ret);
		return ret;
	}

	uint8_t pad[44] = {0};
	fs_write(&f, pad, sizeof(pad));

	LOG_INF("PDM start: %d s stereo @ %d kHz → %s",
		seconds, AUDIO_PDM_RATE_HZ / 1000, path);
	ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("dmic_trigger START failed: %d", ret);
		fs_close(&f);
		return ret;
	}

	const int blocks = seconds * 1000 / PDM_BLOCK_MS;
	uint32_t total_bytes = 0;
	int32_t pl = 0, pr = 0;
	int64_t sl = 0, sr = 0;
	uint32_t n_pairs = 0;

	for (int i = 0; i < blocks; i++) {
		void *buf;
		uint32_t size;

		ret = dmic_read(dmic_dev, 0, &buf, &size, 500);
		if (ret < 0) {
			LOG_ERR("dmic_read[%d] failed: %d", i, ret);
			break;
		}

		int16_t *s = (int16_t *)buf;
		uint32_t n = size / sizeof(int16_t);
		for (uint32_t k = 0; k < n; k += 2) {
			int16_t l = s[k];
			int16_t r = s[k + 1];
			int32_t al = l < 0 ? -l : l;
			int32_t ar = r < 0 ? -r : r;
			if (al > pl) pl = al;
			if (ar > pr) pr = ar;
			sl += l;
			sr += r;
		}
		n_pairs += n / 2;

		fs_write(&f, buf, size);
		total_bytes += size;
		k_mem_slab_free(&pdm_slab, buf);
	}

	dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);
	write_wav_header(&f, total_bytes);
	fs_close(&f);

	if (peak_l) *peak_l = pl;
	if (peak_r) *peak_r = pr;
	if (mean_l) *mean_l = n_pairs ? (int32_t)(sl / (int64_t)n_pairs) : 0;
	if (mean_r) *mean_r = n_pairs ? (int32_t)(sr / (int64_t)n_pairs) : 0;

	LOG_INF("PDM stop: %u B written, %u sample pairs", total_bytes, n_pairs);
	return 0;
}

/* ---------- Streaming recorder ---------- */
#define RECORDER_STACK_SZ  4096
#define RECORDER_PRIO      K_PRIO_PREEMPT(5)

static K_THREAD_STACK_DEFINE(rec_stack, RECORDER_STACK_SZ);
static struct k_thread rec_thread;
static k_tid_t         rec_tid;

static struct fs_file_t rec_file;
static atomic_t         rec_running = ATOMIC_INIT(0);
static atomic_t         rec_stop_req = ATOMIC_INIT(0);
static volatile uint32_t rec_bytes;
static char             rec_path_buf[64];

static void recorder_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);
	int ret;

	ret = configure_dmic();
	if (ret < 0) {
		LOG_ERR("recorder: dmic_configure failed: %d", ret);
		goto out_close;
	}

	ret = dmic_trigger(dmic_dev, DMIC_TRIGGER_START);
	if (ret < 0) {
		LOG_ERR("recorder: trigger START failed: %d", ret);
		goto out_close;
	}
	LOG_INF("recorder: streaming → %s", rec_path_buf);

	uint64_t t0 = k_uptime_get();
	uint32_t blocks = 0;

	while (!atomic_get(&rec_stop_req)) {
		void *buf;
		uint32_t size;

		ret = dmic_read(dmic_dev, 0, &buf, &size, 500);
		if (ret < 0) {
			LOG_ERR("recorder: dmic_read failed: %d", ret);
			break;
		}
		ret = fs_write(&rec_file, buf, size);
		if (ret < 0) {
			LOG_ERR("recorder: fs_write failed: %d", ret);
			k_mem_slab_free(&pdm_slab, buf);
			break;
		}
		rec_bytes += size;
		blocks++;
		k_mem_slab_free(&pdm_slab, buf);
	}

	dmic_trigger(dmic_dev, DMIC_TRIGGER_STOP);

	uint64_t dt_ms = k_uptime_get() - t0;
	LOG_INF("recorder: stop, %u B written in %llu ms (~%u kB/s)",
		rec_bytes, dt_ms,
		dt_ms ? (uint32_t)((uint64_t)rec_bytes * 1000 / dt_ms / 1024) : 0);

out_close:
	write_wav_header(&rec_file, rec_bytes);
	fs_close(&rec_file);
	atomic_clear(&rec_running);
}

int audio_recorder_start(const char *path)
{
	if (atomic_get(&rec_running)) {
		return -EALREADY;
	}

	strncpy(rec_path_buf, path, sizeof(rec_path_buf) - 1);
	rec_path_buf[sizeof(rec_path_buf) - 1] = '\0';

	fs_file_t_init(&rec_file);
	int ret = fs_open(&rec_file, rec_path_buf, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("recorder: open %s: %d", rec_path_buf, ret);
		return ret;
	}
	uint8_t pad[44] = {0};
	fs_write(&rec_file, pad, sizeof(pad));

	rec_bytes = 0;
	atomic_clear(&rec_stop_req);
	atomic_set(&rec_running, 1);

	rec_tid = k_thread_create(&rec_thread, rec_stack,
				  K_THREAD_STACK_SIZEOF(rec_stack),
				  recorder_thread_fn, NULL, NULL, NULL,
				  RECORDER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(rec_tid, "audio_rec");
	return 0;
}

int audio_recorder_stop(void)
{
	if (!atomic_get(&rec_running)) {
		return -ENOENT;
	}
	atomic_set(&rec_stop_req, 1);
	return 0;
}

bool audio_recorder_is_running(void)
{
	return atomic_get(&rec_running) != 0;
}

uint32_t audio_recorder_bytes_written(void)
{
	return rec_bytes;
}
