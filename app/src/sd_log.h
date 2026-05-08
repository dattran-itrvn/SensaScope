/*
 * SensaPulse — micro-SD card mount + simple boot-stamp log.
 *
 * Functions are prefixed `sdlog_` to avoid clash with Zephyr's subsys/sd
 * which exports a global symbol `sd_init`.
 *
 * Phase 4 only mounts the card. The session manager (task #12) will own
 * folder creation and per-session WAV/CSV/meta writing.
 */
#pragma once

#define SD_DRIVE        "SD"
#define SD_MOUNT_POINT  "/" SD_DRIVE ":"
#define SD_BOOT_LOG     SD_MOUNT_POINT "/SP_BOOTS.TXT"

int  sdlog_init(void);                /* disk_access + fs_mount */
int  sdlog_print_info(void);          /* logs sectors / size */
int  sdlog_list_root(int max_show);   /* logs first N entries; logs total */
int  sdlog_append_boot_stamp(void);   /* one line: build + uptime */
