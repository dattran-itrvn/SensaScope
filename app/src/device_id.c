#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <soc.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "sd_log.h"
#include "device_id.h"

LOG_MODULE_REGISTER(identity, LOG_LEVEL_INF);

#define NAME_FILE_PATH    SD_MOUNT_POINT "/device.name"
#define NAME_MAX_BYTES    32                 /* spec cap on user input */
#define CHIP_HEX_LEN      16                 /* 2 × uint32_t hex chars */
#define NAME_BUF_SZ       (5 + NAME_MAX_BYTES + 1)  /* "chip_" + 32 + NUL is safe upper bound */

static struct {
	char name[NAME_BUF_SZ];
	char chip_id[CHIP_HEX_LEN + 1];
} state;

static void compute_chip_id(void)
{
	/* Match the byte order session.c was using before this module
	 * existed: DEVICEID[1] (high) then DEVICEID[0] (low). Keeping it
	 * stable so existing meta.json files retain a consistent chip_id
	 * across firmware rebuilds.
	 */
	snprintf(state.chip_id, sizeof(state.chip_id), "%08x%08x",
		 (unsigned)NRF_FICR->DEVICEID[1],
		 (unsigned)NRF_FICR->DEVICEID[0]);
}

/* Read /SD/device.name into `out`. Returns 0 on success with a non-empty
 * trimmed string, or a negative error if the file is missing / empty /
 * whitespace-only.
 */
static int read_device_name_file(char *out, size_t out_size)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, NAME_FILE_PATH, FS_O_READ);
	if (ret) return ret;  /* typically -ENOENT if absent */

	char buf[NAME_MAX_BYTES + 1] = {0};
	int n = fs_read(&f, buf, NAME_MAX_BYTES);
	fs_close(&f);
	if (n <= 0) return -EIO;

	/* Trim trailing CR / LF / SP / TAB. */
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r' ||
			 buf[n - 1] == ' '  || buf[n - 1] == '\t')) {
		buf[--n] = '\0';
	}
	/* Skip leading whitespace. */
	const char *p = buf;
	while (*p == ' ' || *p == '\t') p++;
	if (*p == '\0') return -EINVAL;  /* whitespace-only */

	strncpy(out, p, out_size - 1);
	out[out_size - 1] = '\0';
	return 0;
}

int identity_init(void)
{
	compute_chip_id();

	char user_name[NAME_MAX_BYTES + 1];
	if (read_device_name_file(user_name, sizeof(user_name)) == 0) {
		strncpy(state.name, user_name, sizeof(state.name) - 1);
		state.name[sizeof(state.name) - 1] = '\0';
		LOG_INF("device.name='%s' chip_id=%s", state.name, state.chip_id);
	} else {
		snprintf(state.name, sizeof(state.name), "chip_%s", state.chip_id);
		LOG_INF("device.name absent → fallback '%s'", state.name);
	}
	return 0;
}

const char *identity_get_name(void)    { return state.name; }
const char *identity_get_chip_id(void) { return state.chip_id; }
