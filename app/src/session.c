#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <soc.h>
#include <stdio.h>

#include "audio.h"
#include "imu_sampler.h"
#include "session.h"

LOG_MODULE_REGISTER(session, LOG_LEVEL_INF);

#ifndef FW_VERSION
#define FW_VERSION "v1.0.0-dev"
#endif
#ifndef FW_BUILD_HASH
#define FW_BUILD_HASH "unknown"
#endif

static void chip_id_hex(char *out, size_t n)
{
	uint32_t hi = NRF_FICR->DEVICEID[1];
	uint32_t lo = NRF_FICR->DEVICEID[0];
	snprintf(out, n, "%08x%08x", (unsigned)hi, (unsigned)lo);
}

int session_write_meta(const char *path, int batt_mv_start)
{
	struct fs_file_t f;
	fs_file_t_init(&f);

	int ret = fs_open(&f, path, FS_O_CREATE | FS_O_WRITE);
	if (ret) {
		LOG_ERR("meta open %s: %d", path, ret);
		return ret;
	}

	char chip[24];
	chip_id_hex(chip, sizeof(chip));

	char body[320];
	int n = snprintf(body, sizeof(body),
		"{\n"
		"  \"start_uptime_ms\": %lld,\n"
		"  \"fs_audio\": %d,\n"
		"  \"fs_imu\": %d,\n"
		"  \"fw_version\": \"%s\",\n"
		"  \"fw_build_hash\": \"%s\",\n"
		"  \"batt_mv_start\": %d,\n"
		"  \"chip_id\": \"%s\",\n"
		"  \"device_name\": \"%s\"\n"
		"}\n",
		(long long)k_uptime_get(),
		AUDIO_PDM_RATE_HZ,
		IMU_SAMPLER_RATE_HZ,
		FW_VERSION,
		FW_BUILD_HASH,
		batt_mv_start,
		chip,
		chip);

	ret = fs_write(&f, body, n);
	fs_close(&f);
	if (ret < 0) {
		LOG_ERR("meta write: %d", ret);
		return ret;
	}
	LOG_INF("meta: %s (%d B, chip=%s, batt=%d mV)",
		path, n, chip, batt_mv_start);
	return 0;
}
