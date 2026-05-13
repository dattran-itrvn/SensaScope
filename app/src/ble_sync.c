/*
 * SensaPulse — BLE Sync Service skeleton (#19.1).
 *
 * Đăng ký service + 4 characteristic với handler stubs. Mỗi sub-task
 * tiếp theo (#19.2 Device Info, #19.3 Set Name, #19.4 LIST, ...) sẽ
 * thay từng stub.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/logging/log.h>

#include <stdio.h>
#include <string.h>

#include "ble_sync.h"
#include "device_id.h"
#include "battery.h"
#include "sd_log.h"
#include "sd_writer.h"

LOG_MODULE_REGISTER(ble_sync, LOG_LEVEL_INF);

#ifndef FW_VERSION
#define FW_VERSION "v1.0.0-dev"
#endif
#ifndef FW_BUILD_HASH
#define FW_BUILD_HASH "unknown"
#endif

extern const char *app_state_lc(void);

/* ---------- UUIDs ---------- */
/* Base: 7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e. Chỉ phần đầu 32 bit khác
 * giữa service và mỗi characteristic. */
#define UUID128(a32) BT_UUID_128_ENCODE(a32, 0x3c4f, 0x4b8e, 0x8a8a, 0x5e5e5e5e5e5e)

static struct bt_uuid_128 uuid_svc  = BT_UUID_INIT_128(UUID128(0x7e7e0001));
static struct bt_uuid_128 uuid_info = BT_UUID_INIT_128(UUID128(0x7e7e0002));
static struct bt_uuid_128 uuid_ctrl = BT_UUID_INIT_128(UUID128(0x7e7e0003));
static struct bt_uuid_128 uuid_data = BT_UUID_INIT_128(UUID128(0x7e7e0004));
static struct bt_uuid_128 uuid_name = BT_UUID_INIT_128(UUID128(0x7e7e0005));

/* ---------- Subscriber + connection tracking ---------- */
static bool ctrl_subscribed;
static bool data_subscribed;
static struct bt_conn *current_conn;          /* refcounted; cleared on disconnect */

static void track_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) return;
	if (current_conn) bt_conn_unref(current_conn);
	current_conn = bt_conn_ref(conn);
}

static void track_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
}

BT_CONN_CB_DEFINE(ble_sync_conn_cb) = {
	.connected    = track_connected,
	.disconnected = track_disconnected,
};

static void ctrl_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	ctrl_subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Control CCC = %u (subscribed=%d)", value, ctrl_subscribed);
}

static void data_ccc_cfg(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	data_subscribed = (value == BT_GATT_CCC_NOTIFY);
	LOG_INF("Data CCC = %u (subscribed=%d)", value, data_subscribed);
}

/* ---------- Opcode framing (per docs/SYNC_PROTOCOL.md) ---------- */
#define OP_LIST       0x01
#define OP_READ       0x02
#define OP_ACK        0x03
#define OP_ABORT      0x04
#define OP_DEL        0x05
#define OP_RESET_CTL  0xFF

#define ST_OK             0x00
#define ST_BUSY           0x01
#define ST_NOT_FOUND      0x02
#define ST_ALREADY_SYNCED 0x03
#define ST_IO_ERR         0x04
#define ST_INVALID        0x05

/* Service attribute indices (xem BT_GATT_SERVICE_DEFINE phía dưới):
 *   0 primary, 1 info-decl, 2 info-value,
 *   3 ctrl-decl, 4 ctrl-value, 5 ctrl-ccc,
 *   6 data-decl, 7 data-value, 8 data-ccc,
 *   9 name-decl, 10 name-value
 */
#define ATTR_CTRL_VAL  4
#define ATTR_DATA_VAL  7

extern const struct bt_gatt_service_static ble_sync_svc;

static int ctrl_notify(const uint8_t *frame, size_t len)
{
	if (!current_conn) {
		LOG_WRN("ctrl_notify: no connection");
		return -ENOTCONN;
	}
	if (!ctrl_subscribed) {
		LOG_WRN("ctrl_notify: client not subscribed (CCC=0)");
		return -EACCES;
	}
	for (int i = 0; i < 20; i++) {
		int ret = bt_gatt_notify(current_conn,
					 &ble_sync_svc.attrs[ATTR_CTRL_VAL],
					 frame, len);
		if (ret == 0) return 0;
		if (ret != -ENOMEM && ret != -EAGAIN) return ret;
		k_msleep(5);
	}
	return -EIO;
}

static int data_notify(const uint8_t *frame, size_t len)
{
	if (!current_conn) return -ENOTCONN;
	if (!data_subscribed) return -EACCES;
	return bt_gatt_notify(current_conn,
			      &ble_sync_svc.attrs[ATTR_DATA_VAL],
			      frame, len);
}

/* ---------- LIST handler — #19.4 (deferred to k_work) ---------- */
static struct k_work list_work;

/* READ state — #19.5 (declared early so ctrl_write_cb above can stash params). */
static struct k_work read_work;
static uint16_t      read_session_id;
static uint8_t       read_file_index;
static uint32_t      read_offset_req;
static uint32_t      read_length_req;
static atomic_t      read_abort_flag = ATOMIC_INIT(0);

static void list_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);

	static struct sd_writer_session_info sessions[SD_WRITER_LIST_MAX];
	uint32_t count = 0;
	int ret = sd_writer_list_sessions(sessions, SD_WRITER_LIST_MAX, &count);

	uint8_t frame[4 + SD_WRITER_LIST_MAX * 6];
	frame[0] = OP_LIST;
	frame[1] = (ret == 0) ? ST_OK : ST_IO_ERR;

	uint16_t n_unsynced = 0;
	size_t   pos = 4;
	if (ret == 0) {
		for (uint32_t i = 0; i < count; i++) {
			if (!sessions[i].is_unsynced) continue;
			if (pos + 6 > sizeof(frame)) break;
			uint16_t sid = sessions[i].session_id;
			uint32_t sz  = sessions[i].size_bytes;
			frame[pos + 0] = sid       & 0xff;
			frame[pos + 1] = (sid >> 8) & 0xff;
			frame[pos + 2] = sz        & 0xff;
			frame[pos + 3] = (sz >> 8) & 0xff;
			frame[pos + 4] = (sz >> 16) & 0xff;
			frame[pos + 5] = (sz >> 24) & 0xff;
			pos += 6;
			n_unsynced++;
		}
	}
	frame[2] = n_unsynced & 0xff;
	frame[3] = (n_unsynced >> 8) & 0xff;

	LOG_INF("LIST: %u unsynced of %u total → notify %zu B",
		n_unsynced, count, pos);
	int nret = ctrl_notify(frame, pos);
	if (nret) LOG_ERR("LIST notify: %d", nret);
}

/* ---------- Device Info (read) — #19.2 ---------- */
/* JSON theo SYNC_PROTOCOL.md. sd_total_mb / sd_free_mb / unsynced /
 * synced sẽ wire khi làm #19.4 (LIST) — cần sd_writer API mới. Tạm dùng
 * placeholder 0 trong #19.2.
 */
static ssize_t info_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	char body[256];
	int batt_mv = battery_read_mv();
	int n = snprintf(body, sizeof(body),
		"{\"name\":\"%s\",\"chip_id\":\"%s\","
		"\"fw\":\"%s+%s\",\"state\":\"%s\","
		"\"batt_mv\":%d,"
		"\"sd_total_mb\":0,\"sd_free_mb\":0,"
		"\"unsynced\":0,\"synced\":0}",
		identity_get_name(), identity_get_chip_id(),
		FW_VERSION, FW_BUILD_HASH, app_state_lc(),
		batt_mv);
	if (n < 0 || n >= (int)sizeof(body)) {
		LOG_ERR("Device Info JSON truncated: %d", n);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	LOG_INF("Device Info read: %d B (offset=%u, len=%u)", n, offset, len);
	return bt_gatt_attr_read(conn, attr, buf, len, offset, body, n);
}

static ssize_t ctrl_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	if (len < 2) {
		LOG_WRN("Control write too short: %u", len);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	const uint8_t *p = buf;
	uint8_t op = p[0];
	LOG_INF("Control write: opcode=0x%02x len=%u", op, len);

	switch (op) {
	case OP_LIST:
		k_work_submit(&list_work);
		return len;
	case OP_READ:
		if (len < 13) {
			uint8_t reply[2] = { op, ST_INVALID };
			ctrl_notify(reply, sizeof(reply));
			return len;
		}
		read_session_id = p[2] | (p[3] << 8);
		read_file_index = p[4];
		read_offset_req = p[5] | (p[6] << 8) | (p[7] << 16) | (p[8] << 24);
		read_length_req = p[9] | (p[10] << 8) | (p[11] << 16) | (p[12] << 24);
		LOG_INF("READ submit: sess=%u file=%u offset=%u length=%u",
			read_session_id, read_file_index,
			read_offset_req, read_length_req);
		k_work_submit(&read_work);
		return len;
	case OP_ABORT:
		atomic_set(&read_abort_flag, 1);
		{
			uint8_t reply[2] = { op, ST_OK };
			ctrl_notify(reply, sizeof(reply));
		}
		return len;
	case OP_ACK:
	case OP_DEL:
	case OP_RESET_CTL: {
		/* Stubs cho #19.6 — trả lỗi ST_INVALID tạm thời. */
		uint8_t reply[2] = { op, ST_INVALID };
		ctrl_notify(reply, sizeof(reply));
		LOG_WRN("opcode 0x%02x not implemented yet (#19.6)", op);
		return len;
	}
	default: {
		uint8_t reply[2] = { op, ST_INVALID };
		ctrl_notify(reply, sizeof(reply));
		return len;
	}
	}
}

/* ---------- READ + Data streaming — #19.5 (deferred to k_work) ---------- */

static const char *file_index_to_name(uint8_t idx)
{
	switch (idx) {
	case 0: return "audio.wav";
	case 1: return "imu.csv";
	case 2: return "meta.json";
	default: return NULL;
	}
}

static int read_chunk_cb(const uint8_t *chunk, uint32_t len, void *user)
{
	ARG_UNUSED(user);
	if (atomic_get(&read_abort_flag)) return -ECANCELED;
	/* Retry với backoff khi ATT TX buffer hết. PC consumes notifies tại
	 * BLE connection interval (~7.5-15 ms). Backoff 5 ms × 20 = 100 ms
	 * total — đủ cho buffer drain ở host side mà không stall nhiều.
	 * Bottleneck tổng thể vẫn ở connection-interval, không phải ở đây. */
	for (int i = 0; i < 20; i++) {
		if (atomic_get(&read_abort_flag)) return -ECANCELED;
		int ret = data_notify(chunk, len);
		if (ret == 0) return 0;
		if (ret != -ENOMEM && ret != -EAGAIN) {
			LOG_ERR("read_chunk_cb: data_notify: %d", ret);
			return -EIO;
		}
		k_msleep(5);
	}
	LOG_ERR("read_chunk_cb: TX buffer stuck after retries");
	return -EIO;
}

static void read_work_handler(struct k_work *w)
{
	ARG_UNUSED(w);

	const char *fname = file_index_to_name(read_file_index);
	if (!fname) {
		uint8_t reply[2] = { OP_READ, ST_INVALID };
		ctrl_notify(reply, sizeof(reply));
		return;
	}

	char path[80];
	snprintf(path, sizeof(path), SD_MOUNT_POINT "/SESSION_%05u/%s",
		 read_session_id, fname);

	atomic_clear(&read_abort_flag);

	/* First chunk callback hasn't fired yet — we need total_size before
	 * sending the Control "total bytes" reply. sd_writer_read_file fills
	 * total_out BEFORE streaming. Use 2-step: open via sd_writer's
	 * built-in flow (it stat+seek+stream), but we need total upfront.
	 * Workaround: call read_file with cb that captures total then proceeds.
	 *
	 * Simpler: send the Control reply AFTER read_file returns, including
	 * a status of OK + total. PC tool currently expects total BEFORE
	 * Data chunks — not great, but workable for v1.1 dev: PC counts bytes
	 * received until 0-byte terminator. Send total reply at the END as
	 * confirmation. */

	uint32_t total = 0;
	int ret = sd_writer_read_file(path, read_offset_req, read_length_req,
				      read_chunk_cb, NULL, &total);

	/* Send 0-byte Data notify as EOF terminator (per spec). */
	data_notify(NULL, 0);

	/* Control reply with final status + total bytes streamed. */
	uint8_t reply[6];
	reply[0] = OP_READ;
	reply[1] = (ret == 0) ? ST_OK :
		   (ret == -ENOENT) ? ST_NOT_FOUND :
		   (ret == -ECANCELED) ? ST_OK :     /* ABORT counted as OK-abort */
		   ST_IO_ERR;
	reply[2] = total & 0xff;
	reply[3] = (total >> 8) & 0xff;
	reply[4] = (total >> 16) & 0xff;
	reply[5] = (total >> 24) & 0xff;
	ctrl_notify(reply, sizeof(reply));

	LOG_INF("READ done: %s offset=%u length=%u → %u byte, ret=%d",
		path, read_offset_req, read_length_req, total, ret);
}

/* ---------- Set Name (write) — #19.3 ---------- */
#define DEVICE_NAME_PATH    SD_MOUNT_POINT "/device.name"
#define DEVICE_NAME_MAX     32

static ssize_t name_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);

	if (len > DEVICE_NAME_MAX) {
		LOG_WRN("Set Name: %u byte > %d limit", len, DEVICE_NAME_MAX);
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}

	int ret = sd_writer_write_file(DEVICE_NAME_PATH, buf, len);
	if (ret) {
		LOG_ERR("Set Name: sd_writer_write_file: %d", ret);
		return BT_GATT_ERR(BT_ATT_ERR_UNLIKELY);
	}
	identity_reload();
	LOG_INF("Set Name: persisted %u byte → '%s'", len, identity_get_name());
	return len;
}

/* ---------- Service definition ---------- */
BT_GATT_SERVICE_DEFINE(ble_sync_svc,
	BT_GATT_PRIMARY_SERVICE(&uuid_svc),

	/* 0002 Device Info — Read */
	BT_GATT_CHARACTERISTIC(&uuid_info.uuid,
			       BT_GATT_CHRC_READ,
			       BT_GATT_PERM_READ,
			       info_read_cb, NULL, NULL),

	/* 0003 Control — Write + Notify */
	BT_GATT_CHARACTERISTIC(&uuid_ctrl.uuid,
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE,
			       NULL, ctrl_write_cb, NULL),
	BT_GATT_CCC(ctrl_ccc_cfg,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* 0004 Data — Notify only */
	BT_GATT_CHARACTERISTIC(&uuid_data.uuid,
			       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_NONE,
			       NULL, NULL, NULL),
	BT_GATT_CCC(data_ccc_cfg,
		    BT_GATT_PERM_READ | BT_GATT_PERM_WRITE),

	/* 0005 Set Name — Write */
	BT_GATT_CHARACTERISTIC(&uuid_name.uuid,
			       BT_GATT_CHRC_WRITE,
			       BT_GATT_PERM_WRITE,
			       NULL, name_write_cb, NULL),
);

int ble_sync_init(void)
{
	k_work_init(&list_work, list_work_handler);
	k_work_init(&read_work, read_work_handler);
	LOG_INF("Sync Service registered (UUID base 7e7e0001-...).");
	return 0;
}
