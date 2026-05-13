/*
 * SensaPulse v1.0 firmware — main entry.
 *
 * Task #14: real application FSM.
 *
 *   APP_STATE_IDLE              — waiting for double-tap. LED idle blink.
 *   APP_STATE_RECORDING         — session running. LED solid.
 *                                 Battery polled every 30 s; <3300 mV →
 *                                 stop session, LOW_BATT_HOLDOFF.
 *                                 Watchdog abort → ERROR.
 *   APP_STATE_LOW_BATT_HOLDOFF  — taps ignored, LED 5 Hz. Recovers to
 *                                 IDLE when battery climbs back ≥3500 mV
 *                                 (200 mV hysteresis).
 *   APP_STATE_ERROR             — LED SOS Morse. SD full or watchdog
 *                                 abort. Permanent until reboot.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>

#include "led.h"
#include "battery.h"
#include "device_id.h"
#include "imu.h"
#include "imu_sampler.h"
#include "audio.h"
#include "sd_log.h"
#include "sd_writer.h"
#include "session.h"
#include "ble_sync.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define SMOKE_WAV_PATH    SD_MOUNT_POINT "/PDM_TEST.WAV"
#define SMOKE_RECORD_S    3

#define BATT_LOW_MV       3300   /* spec: stop recording below this */
#define BATT_RECOVERY_MV  3500   /* hysteresis to leave LOW_BATT_HOLDOFF */
#define MAIN_TICK_MS      500
#define BATT_POLL_TICKS   (30 * 1000 / MAIN_TICK_MS)   /* 60 ticks = 30 s */

/* ---------- App FSM ---------- */
typedef enum {
	APP_STATE_IDLE,
	APP_STATE_RECORDING,
	APP_STATE_LOW_BATT_HOLDOFF,
	APP_STATE_SYNC,                  /* #20: BLE-connected, draining data */
	APP_STATE_ERROR,
} app_state_t;

static app_state_t app_state = APP_STATE_IDLE;
/* #20: state lúc trước khi vào SYNC — restore khi BLE disconnect. Cho phép
 * `ERROR → SYNC → ERROR` (user vớt data từ thẻ SD đầy mà không clear lỗi). */
static app_state_t pre_sync_state = APP_STATE_IDLE;
static unsigned    batt_tick = 0;

static const led_state_t state_to_led[] = {
	[APP_STATE_IDLE]              = LED_STATE_IDLE,
	[APP_STATE_RECORDING]         = LED_STATE_RECORDING,
	[APP_STATE_LOW_BATT_HOLDOFF]  = LED_STATE_LOW_BATT,
	[APP_STATE_SYNC]              = LED_STATE_SYNC,
	[APP_STATE_ERROR]             = LED_STATE_ERROR,
};

static const char *state_name(app_state_t s)
{
	switch (s) {
	case APP_STATE_IDLE:             return "IDLE";
	case APP_STATE_RECORDING:        return "RECORDING";
	case APP_STATE_LOW_BATT_HOLDOFF: return "LOW_BATT_HOLDOFF";
	case APP_STATE_SYNC:             return "SYNC";
	case APP_STATE_ERROR:            return "ERROR";
	}
	return "?";
}

/* #19.2: lower-case state name cho Device Info JSON (theo SYNC_PROTOCOL.md). */
const char *app_state_lc(void)
{
	switch (app_state) {
	case APP_STATE_IDLE:             return "idle";
	case APP_STATE_RECORDING:        return "recording";
	case APP_STATE_LOW_BATT_HOLDOFF: return "low_batt";
	case APP_STATE_SYNC:             return "sync";
	case APP_STATE_ERROR:            return "error";
	}
	return "unknown";
}

/* Forward declarations for BLE radio control. */
static void ble_adv_start(void);
static void ble_adv_stop(void);

static void transition(app_state_t new_state)
{
	if (new_state == app_state) return;
	LOG_INF("FSM: %s → %s", state_name(app_state), state_name(new_state));

	/* #20: BLE radio control by state.
	 * - RECORDING tắt advertising (giải phóng radio + tiết kiệm điện).
	 * - Rời RECORDING → bật lại để PC có thể connect.
	 * - SYNC giữ nguyên (đang trong connection, no advertising needed).
	 */
	if (app_state == APP_STATE_RECORDING && new_state != APP_STATE_RECORDING) {
		ble_adv_start();
	}
	if (new_state == APP_STATE_RECORDING && app_state != APP_STATE_RECORDING) {
		ble_adv_stop();
	}

	app_state = new_state;
	led_set_state(state_to_led[new_state]);
	batt_tick = 0;
}

/* ---------- BLE advertise + connect/disconnect FSM hook (#20) ---------- */
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static bool ble_advertising;

static void ble_adv_start(void)
{
	if (ble_advertising) return;
	int err = bt_le_adv_start(BT_LE_ADV_CONN, adv_data, ARRAY_SIZE(adv_data),
				  NULL, 0);
	if (err) {
		LOG_ERR("bt_le_adv_start: %d", err);
		return;
	}
	ble_advertising = true;
	LOG_INF("BLE: advertising as '%s'", CONFIG_BT_DEVICE_NAME);
}

static void ble_adv_stop(void)
{
	if (!ble_advertising) return;
	int err = bt_le_adv_stop();
	if (err) LOG_WRN("bt_le_adv_stop: %d", err);
	ble_advertising = false;
	LOG_INF("BLE: advertising stopped");
}

static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) {
		LOG_ERR("BLE connect failed: 0x%02x", err);
		return;
	}
	LOG_INF("BLE connected (current FSM=%s)", state_name(app_state));

	/* Connectable advertising auto-stops at the BLE controller side when a
	 * connection completes. Reflect that in our local flag. */
	ble_advertising = false;

	/* #20: gate state-transition by current FSM.
	 * - IDLE / LOW_BATT_HOLDOFF / ERROR → SYNC (allow draining data).
	 * - RECORDING → reject (data integrity > convenience). With adv stop
	 *   when entering RECORDING, this branch should be unreachable except
	 *   for a race window; defense in depth.
	 * - SYNC → impossible (no advertising while already in SYNC).
	 */
	switch (app_state) {
	case APP_STATE_RECORDING:
		LOG_WRN("BLE: reject connect (RECORDING); disconnecting");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	case APP_STATE_SYNC:
		LOG_WRN("BLE: unexpected connect while already in SYNC");
		bt_conn_disconnect(conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
		return;
	case APP_STATE_IDLE:
	case APP_STATE_LOW_BATT_HOLDOFF:
	case APP_STATE_ERROR:
		pre_sync_state = app_state;
		transition(APP_STATE_SYNC);
		return;
	}
}

static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	LOG_INF("BLE disconnected (reason 0x%02x, FSM=%s)",
		reason, state_name(app_state));

	/* #20: rời SYNC → restore state trước SYNC. Trong các state khác
	 * (e.g., disconnect xảy ra do reject ở on_connected), không transition.
	 * Restart advertising (trừ khi đang RECORDING — advertising sẽ tự bật
	 * lại lúc transition rời RECORDING). */
	if (app_state == APP_STATE_SYNC) {
		transition(pre_sync_state);
	}
	if (app_state != APP_STATE_RECORDING) {
		ble_adv_start();
	}
}

BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};

static int start_ble(void)
{
	int err = bt_enable(NULL);
	if (err) { LOG_ERR("bt_enable: %d", err); return err; }
	ble_sync_init();
	ble_adv_start();
	return 0;
}

/* ---------- Double-tap callback (system work-queue context) ---------- */
static void on_double_tap(void)
{
	switch (app_state) {
	case APP_STATE_LOW_BATT_HOLDOFF:
	case APP_STATE_ERROR:
	case APP_STATE_SYNC:        /* #20: SYNC ↔ RECORDING bị cấm */
		LOG_INF("tap ignored in %s", state_name(app_state));
		return;

	case APP_STATE_RECORDING:
		LOG_INF(">>> DOUBLE TAP — stopping session");
		session_stop();
		transition(APP_STATE_IDLE);
		return;

	case APP_STATE_IDLE: {
		int batt_mv = battery_read_mv();
		if (batt_mv >= 0 && batt_mv < BATT_LOW_MV) {
			LOG_WRN("tap: batt %d mV < %d, refusing → LOW_BATT_HOLDOFF",
				batt_mv, BATT_LOW_MV);
			transition(APP_STATE_LOW_BATT_HOLDOFF);
			return;
		}
		LOG_INF(">>> DOUBLE TAP — starting session");
		int ret = session_start(batt_mv);
		if (ret == SESSION_ERR_NO_SPACE) {
			LOG_ERR("session_start: SD full → ERROR");
			transition(APP_STATE_ERROR);
			return;
		}
		if (ret) {
			LOG_ERR("session_start: %d (staying in IDLE)", ret);
			return;
		}
		transition(APP_STATE_RECORDING);
		return;
	}
	}
}

/* ---------- main ---------- */
int main(void)
{
	LOG_INF("SensaPulse v1.0 — task #14 main FSM");
	LOG_INF("Build: %s %s", __DATE__, __TIME__);

	if (led_init() == 0)     LOG_INF("LED ready");

	int boot_batt_mv = -1;
	if (battery_init() == 0) {
		boot_batt_mv = battery_read_mv();
		if (boot_batt_mv >= 0) {
			LOG_INF("Battery: %d mV (%s)",
				boot_batt_mv, battery_state_str(boot_batt_mv));
		}
	}

	if (imu_init() != 0) {
		LOG_ERR("IMU init failed");
	}

	if (audio_init() == 0 && sdlog_init() == 0) {
		sdlog_append_boot_stamp();
		identity_init();
		int32_t pl, pr, ml, mr;
		if (audio_record_to_wav(SMOKE_WAV_PATH, SMOKE_RECORD_S,
					&pl, &pr, &ml, &mr) == 0) {
			LOG_INF("smoke ch0 (body, L)    peak=%d  mean=%d", pl, ml);
			LOG_INF("smoke ch1 (ambient, R) peak=%d  mean=%d", pr, mr);
		}
		/* #19.3: bring up the writer thread now so BLE handlers can
		 * route FATFS ops through it (Set Name, future LIST/READ/ACK/
		 * DEL/etc.). Thread stays alive cho đến shutdown.
		 */
		sd_writer_init();
		session_init();
	}

	/* Register the tap callback only after the SD subsystem and session
	 * counter are up — avoids a tap during boot calling session_start
	 * with the FATFS volume not yet mounted.
	 */
	imu_enable_double_tap(on_double_tap);

	start_ble();

	/* Pick initial state from battery reading. If we're already below the
	 * threshold at boot we go straight to LOW_BATT_HOLDOFF so the user
	 * gets an obvious 5 Hz blink instead of normal idle.
	 */
	if (boot_batt_mv >= 0 && boot_batt_mv < BATT_LOW_MV) {
		LOG_WRN("boot batt %d mV < %d → LOW_BATT_HOLDOFF",
			boot_batt_mv, BATT_LOW_MV);
		transition(APP_STATE_LOW_BATT_HOLDOFF);
	} else {
		led_set_state(LED_STATE_IDLE);
		LOG_INF("Idle. Double-tap to start/stop recording.");
	}

	while (1) {
		switch (app_state) {
		case APP_STATE_IDLE:
			/* Tap callback drives IDLE → RECORDING. Nothing to poll. */
			break;

		case APP_STATE_RECORDING:
			if (!session_is_active()) {
				if (session_was_aborted()) {
					LOG_ERR("FSM: session aborted by watchdog → ERROR");
					transition(APP_STATE_ERROR);
				} else {
					LOG_INF("Stopped: audio_last=%u B, imu_last=%u samples",
						sd_writer_audio_bytes_written(),
						sd_writer_imu_samples_written());
					transition(APP_STATE_IDLE);
				}
				break;
			}
			if (++batt_tick >= BATT_POLL_TICKS) {
				batt_tick = 0;
				int mv = battery_read_mv();
				if (mv >= 0 && mv < BATT_LOW_MV) {
					LOG_WRN("FSM: batt %d mV during RECORDING → "
						"stop session, LOW_BATT_HOLDOFF", mv);
					session_stop();
					transition(APP_STATE_LOW_BATT_HOLDOFF);
				}
			}
			break;

		case APP_STATE_LOW_BATT_HOLDOFF:
			if (++batt_tick >= BATT_POLL_TICKS) {
				batt_tick = 0;
				int mv = battery_read_mv();
				if (mv >= 0 && mv >= BATT_RECOVERY_MV) {
					LOG_INF("FSM: batt recovered %d mV → IDLE", mv);
					transition(APP_STATE_IDLE);
				}
			}
			break;

		case APP_STATE_SYNC:
			/* #20: passive — wait for BLE disconnect callback to
			 * restore pre_sync_state. No polling needed. */
			break;

		case APP_STATE_ERROR:
			/* Permanent until reboot. LED already SOS. */
			break;
		}
		k_msleep(MAIN_TICK_MS);
	}
	return 0;
}
