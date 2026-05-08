/*
 * SensaPulse v1.0 firmware — main entry.
 *
 * Task #13: LED driven by state-engine. The legacy on/off/toggle calls
 *           are replaced with led_set_state(LED_STATE_*); the pattern
 *           timer in led.c owns the GPIO writes.
 * (Task #14 will replace this main-loop polling with a real FSM that
 *  also drives LOW_BATT_HOLDOFF and ERROR states.)
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gap.h>

#include "led.h"
#include "battery.h"
#include "imu.h"
#include "imu_sampler.h"
#include "audio.h"
#include "sd_log.h"
#include "session.h"

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

#define SMOKE_WAV_PATH    SD_MOUNT_POINT "/PDM_TEST.WAV"
#define SMOKE_RECORD_S    3

/* ---------- BLE smoke advertise ---------- */
static const struct bt_data adv_data[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_NAME_COMPLETE, CONFIG_BT_DEVICE_NAME,
		sizeof(CONFIG_BT_DEVICE_NAME) - 1),
};
static void on_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) LOG_ERR("BLE connect failed: 0x%02x", err);
	else     LOG_INF("BLE connected");
}
static void on_disconnected(struct bt_conn *conn, uint8_t reason)
{
	LOG_INF("BLE disconnected (reason 0x%02x)", reason);
}
BT_CONN_CB_DEFINE(conn_cb) = {
	.connected = on_connected,
	.disconnected = on_disconnected,
};
static int start_ble(void)
{
	int err = bt_enable(NULL);
	if (err) { LOG_ERR("bt_enable: %d", err); return err; }
	err = bt_le_adv_start(BT_LE_ADV_CONN, adv_data, ARRAY_SIZE(adv_data),
			      NULL, 0);
	if (err) { LOG_ERR("bt_le_adv_start: %d", err); return err; }
	LOG_INF("Advertising as '%s'", CONFIG_BT_DEVICE_NAME);
	return 0;
}

/* ---------- Double-tap → toggle record ---------- */
static void on_double_tap(void)
{
	if (session_is_active()) {
		LOG_INF(">>> DOUBLE TAP — stopping session");
		session_stop();
		return;
	}

	LOG_INF(">>> DOUBLE TAP — starting session");
	int batt_mv = battery_read_mv();
	int ret = session_start(batt_mv);
	if (ret) {
		LOG_ERR("session_start failed: %d", ret);
		return;
	}
	led_set_state(LED_STATE_RECORDING);
}

/* ---------- main ---------- */
int main(void)
{
	LOG_INF("SensaPulse v1.0 — phase 6 (streaming recorder)");
	LOG_INF("Build: %s %s", __DATE__, __TIME__);

	if (led_init() == 0)     LOG_INF("LED ready");

	if (battery_init() == 0) {
		int mv = battery_read_mv();
		if (mv >= 0) {
			LOG_INF("Battery: %d mV (%s)", mv, battery_state_str(mv));
		}
	}

	if (imu_init() == 0) {
		imu_enable_double_tap(on_double_tap);
	}

	if (audio_init() == 0 && sdlog_init() == 0) {
		sdlog_append_boot_stamp();
		int32_t pl, pr, ml, mr;
		if (audio_record_to_wav(SMOKE_WAV_PATH, SMOKE_RECORD_S,
					&pl, &pr, &ml, &mr) == 0) {
			LOG_INF("smoke ch0 (body, L)    peak=%d  mean=%d", pl, ml);
			LOG_INF("smoke ch1 (ambient, R) peak=%d  mean=%d", pr, mr);
		}
		session_init();
	}

	start_ble();

	led_set_state(LED_STATE_IDLE);
	LOG_INF("Idle. Double-tap to start/stop recording.");

	bool prev_recording = false;
	while (1) {
		bool now_recording = session_is_active() ||
			audio_recorder_is_running() ||
			imu_sampler_is_running();
		if (now_recording != prev_recording) {
			if (now_recording) {
				led_set_state(LED_STATE_RECORDING);
			} else {
				LOG_INF("Stopped: audio_last=%u B, imu_last=%u samples",
					audio_recorder_bytes_written(),
					imu_sampler_samples_written());
				led_set_state(LED_STATE_IDLE);
			}
		}
		prev_recording = now_recording;
		k_msleep(500);
	}
	return 0;
}
