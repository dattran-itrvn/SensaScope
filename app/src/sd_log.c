#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/storage/disk_access.h>
#include <zephyr/logging/log.h>
#include <ff.h>
#include <stdio.h>
#include "sd_log.h"

LOG_MODULE_REGISTER(sd_log, LOG_LEVEL_INF);

static FATFS fat_fs;
static struct fs_mount_t sd_mp = {
	.type      = FS_FATFS,
	.fs_data   = &fat_fs,
	.mnt_point = SD_MOUNT_POINT,
};

int sdlog_print_info(void)
{
	uint32_t bc = 0, bs = 0;

	if (disk_access_init(SD_DRIVE) != 0) {
		LOG_ERR("disk_access_init failed");
		return -EIO;
	}
	if (disk_access_ioctl(SD_DRIVE, DISK_IOCTL_GET_SECTOR_COUNT, &bc) ||
	    disk_access_ioctl(SD_DRIVE, DISK_IOCTL_GET_SECTOR_SIZE, &bs)) {
		LOG_ERR("disk ioctl failed");
		return -EIO;
	}
	uint64_t bytes = (uint64_t)bc * bs;
	LOG_INF("SD: %u sectors x %u B = %u MB",
		bc, bs, (uint32_t)(bytes / (1024 * 1024)));
	return 0;
}

/* #26 + #33: SD cold-boot init can fail for two distinct reasons:
 *
 *  (a) Power/clock settle on fresh boot (the original #26 problem).
 *      The SD spec requires ≥ 1 ms after Vcc rise + ≥ 74 SCLK idle
 *      cycles before CMD0; the sdhc_spi driver does the SCLK part but
 *      not always with enough margin against J-Link Vcc transients.
 *      Mitigation: 100 ms initial settle.
 *
 *  (b) Card-internal init mid-startup. Mitigation: a small retry budget.
 *      For ~60 % cold-boot failures on this PCB, 3 × 200 ms was enough.
 *
 *  (c) #33 stuck-state recovery: after a production crash that returned
 *      `-116` mid-write, the card can enter a "stuck" state where CMD0
 *      cold-init keeps failing for a few seconds until the card's
 *      internal supervisor times out and accepts a fresh CMD0. Extended
 *      retry budget (8 × 500 ms = 4 s total, on top of the 100 ms
 *      settle) covers this without lengthening normal cold boot
 *      meaningfully — first attempt succeeds on healthy cards. The
 *      proper hardware-side fix is a GPIO line that can power-cycle
 *      the SD slot; tracked in `docs/PRODUCTION_TODO.md § Hardware`.
 */
int sdlog_init(void)
{
	k_msleep(100);  /* (a) settle */

	int ret = -EIO;
	const int max_attempts = 8;
	const int retry_gap_ms = 500;
	for (int attempt = 1; attempt <= max_attempts; attempt++) {
		ret = sdlog_print_info();
		if (ret == 0) {
			if (attempt > 1) {
				LOG_INF("SD init OK on attempt %d (after %d ms warm-up)",
					attempt, (attempt - 1) * retry_gap_ms);
			}
			break;
		}
		LOG_WRN("SD init attempt %d/%d failed: %d — retrying in %d ms",
			attempt, max_attempts, ret, retry_gap_ms);
		k_msleep(retry_gap_ms);
	}
	if (ret) return ret;

	ret = fs_mount(&sd_mp);
	if (ret) {
		LOG_ERR("fs_mount failed: %d", ret);
		return ret;
	}
	LOG_INF("SD mounted at %s", SD_MOUNT_POINT);
	return 0;
}

int sdlog_list_root(int max_show)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, SD_MOUNT_POINT);
	if (ret) {
		LOG_ERR("opendir %s: %d", SD_MOUNT_POINT, ret);
		return ret;
	}

	LOG_INF("Listing %s:", SD_MOUNT_POINT);
	int n = 0;
	for (;;) {
		struct fs_dirent ent;
		ret = fs_readdir(&dir, &ent);
		if (ret || ent.name[0] == '\0') break;
		if (n < max_show) {
			LOG_INF("  [%c] %-32s %u B",
				ent.type == FS_DIR_ENTRY_DIR ? 'D' : 'F',
				ent.name, ent.size);
		}
		n++;
		k_msleep(2);
	}
	fs_closedir(&dir);
	if (n > max_show) {
		LOG_INF("  ...(%d more)", n - max_show);
	}
	LOG_INF("  total %d entries", n);
	return 0;
}

int sdlog_append_boot_stamp(void)
{
	struct fs_file_t f;
	char line[64];

	fs_file_t_init(&f);
	int ret = fs_open(&f, SD_BOOT_LOG, FS_O_CREATE | FS_O_WRITE | FS_O_APPEND);
	if (ret) {
		LOG_ERR("open %s: %d", SD_BOOT_LOG, ret);
		return ret;
	}
	int len = snprintf(line, sizeof(line),
			   "boot uptime=%lld ms build=%s %s\n",
			   k_uptime_get(), __DATE__, __TIME__);
	ret = fs_write(&f, line, len);
	fs_close(&f);
	if (ret >= 0) {
		LOG_INF("Boot stamp appended (%d B)", ret);
	}
	return ret;
}
