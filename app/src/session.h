/*
 * SensaPulse — session metadata.
 *
 * Skeleton for task #11: just writes meta.json once per recording session.
 * Task #12 extends this module to own folder lifecycle, persistent counter,
 * 10-min rotation, .unsynced marker, and free-space management.
 */
#pragma once

int session_write_meta(const char *path, int batt_mv_start);
