/*
 * SensaPulse — BLE Sync Service skeleton (#19.1).
 *
 * Đăng ký service + 4 characteristic với handler stubs. Mỗi sub-task
 * tiếp theo (#19.2 Device Info, #19.3 Set Name, #19.4 LIST, ...) sẽ
 * thay từng stub.
 */

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
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

/* #34: forward decl for data-notify credit (used by track_disconnected). */
static struct k_sem data_notify_credit;
static bool         data_notify_inited;

/* #19.7: negotiate high-throughput link parameters after connect.
 * Peripheral chỉ request PHY + data-len + conn-interval. MTU exchange
 * thường do host (PC) initiate; Zephyr peripheral phản hồi với min(247,
 * host_max). bt_gatt_exchange_mtu (client-initiated) cần BT_GATT_CLIENT
 * config nên bỏ qua. */
static void negotiate_link(struct bt_conn *conn)
{
	int ret = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (ret) LOG_WRN("phy_update: %d", ret);

	ret = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (ret) LOG_WRN("data_len_update: %d", ret);

	/* Connection interval: KHÔNG explicit request — macOS coerce trên
	 * dynamic param_update + có thể disconnect connection nếu phía
	 * peripheral push notify rate quá nhanh trong khi đang re-negotiate.
	 * Host sẽ chọn interval của riêng nó. Throughput tuning ở task #34
	 * (callback-based flow control) sẽ hiệu quả hơn là cố ép interval. */
}

static void track_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) return;
	if (current_conn) bt_conn_unref(current_conn);
	current_conn = bt_conn_ref(conn);
	negotiate_link(conn);
}

static void track_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	/* #34: force-release the data-notify credit in case the pending TX
	 * callback never fires (e.g., abrupt link drop mid-stream). Without
	 * this the next session's first notify would block 3 s waiting for
	 * the ghost callback. */
	if (data_notify_inited) {
		k_sem_reset(&data_notify_credit);
		k_sem_give(&data_notify_credit);
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

/* ---------- Data notify with serial flow control — #34 ---------- *
 * Pre-fix bug: bt_gatt_notify() returned 0 even when ACL TX pool was
 * exhausted, silently dropping the data. Bisect (2026-05-14) showed the
 * cliff is exactly CONFIG_BT_BUF_ACL_TX_COUNT chunks: with TX_COUNT=10,
 * 8-10 chunks deliver and the rest vanish; TX_COUNT=32 pushes cliff to
 * ~31 chunks.
 *
 * Real fix: bt_gatt_notify_cb with a 1-permit semaphore. The completion
 * callback runs when the controller has transmitted the PDU (slot in
 * ACL TX pool freed), so we never overrun.
 *
 * Tradeoff: 1 in-flight ≈ 1 notify per BLE connection interval (7.5-15
 * ms typical). At 240 B/chunk: 16-32 kB/s realistic — slow but correct.
 * Multi-credit version is a follow-up; correctness first. */
static struct bt_gatt_notify_params data_notify_params;
static uint8_t                      data_notify_buf[244];

static void data_notify_init_once(void)
{
	if (data_notify_inited) return;
	k_sem_init(&data_notify_credit, 1, 1);
	data_notify_inited = true;
}

static void on_data_notify_done(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(user_data);
	k_sem_give(&data_notify_credit);
}

static int data_notify(const uint8_t *frame, size_t len)
{
	if (!current_conn) return -ENOTCONN;
	if (!data_subscribed) return -EACCES;

	/* 0-byte EOF terminator: send directly, no credit needed. */
	if (len == 0) {
		return bt_gatt_notify(current_conn,
				      &ble_sync_svc.attrs[ATTR_DATA_VAL],
				      frame, len);
	}
	if (len > sizeof(data_notify_buf)) return -EINVAL;

	/* Block waiting for the previous chunk's controller-TX-complete
	 * callback. 3 s ceiling — if the link is wedged, give up so the
	 * caller can report -EIO and the host can recover. */
	if (k_sem_take(&data_notify_credit, K_SECONDS(3))) {
		LOG_ERR("data_notify: credit timeout (link wedged?)");
		return -ETIMEDOUT;
	}

	memcpy(data_notify_buf, frame, len);
	data_notify_params = (struct bt_gatt_notify_params){
		.attr = &ble_sync_svc.attrs[ATTR_DATA_VAL],
		.data = data_notify_buf,
		.len  = len,
		.func = on_data_notify_done,
	};
	int ret = bt_gatt_notify_cb(current_conn, &data_notify_params);
	if (ret) {
		k_sem_give(&data_notify_credit);
		return ret;
	}
	return 0;
}

/* ---------- LIST handler — #19.4 (deferred to k_work) ---------- */
static struct k_work list_work;

/* #34: dedicated work queue for READ. system_work_queue is also where
 * bt_gatt_notify_cb completion callbacks dispatch; if read_work_handler
 * blocks on a callback-signaled semaphore while running on
 * system_work_queue, the callback never gets to run → deadlock,
 * data_notify_credit times out after 3 s. Dedicated queue breaks the
 * cycle: read_work blocks on this queue's thread, callback runs on
 * system_work_queue independently.
 */
#define READ_WQ_STACK_SZ   2048
#define READ_WQ_PRIO       K_PRIO_PREEMPT(7)
K_THREAD_STACK_DEFINE(read_wq_stack, READ_WQ_STACK_SZ);
static struct k_work_q read_wq;
static struct k_work   read_work;
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
	uint32_t free_mb = 0;
	(void)sd_writer_get_free_mb(&free_mb);   /* #17: 0 if writer not init */
	int n = snprintf(body, sizeof(body),
		"{\"name\":\"%s\",\"chip_id\":\"%s\","
		"\"fw\":\"%s+%s\",\"state\":\"%s\","
		"\"batt_mv\":%d,"
		"\"sd_total_mb\":0,\"sd_free_mb\":%u,"
		"\"unsynced\":0,\"synced\":0}",
		identity_get_name(), identity_get_chip_id(),
		FW_VERSION, FW_BUILD_HASH, app_state_lc(),
		batt_mv, free_mb);
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
		k_work_submit_to_queue(&read_wq, &read_work);
		return len;
	case OP_ABORT:
		atomic_set(&read_abort_flag, 1);
		{
			uint8_t reply[2] = { op, ST_OK };
			ctrl_notify(reply, sizeof(reply));
		}
		return len;
	case OP_ACK: {
		if (len < 4) {
			uint8_t reply[2] = { op, ST_INVALID };
			ctrl_notify(reply, sizeof(reply));
			return len;
		}
		uint16_t sid = p[2] | (p[3] << 8);
		char path[80];
		snprintf(path, sizeof(path), SD_MOUNT_POINT "/SESSION_%05u/.unsynced", sid);
		int ret = sd_writer_unlink(path);
		uint8_t st = (ret == 0) ? ST_OK :
			     (ret == -ENOENT) ? ST_NOT_FOUND : ST_IO_ERR;
		uint8_t reply[2] = { op, st };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("ACK SESSION_%05u: ret=%d status=0x%02x", sid, ret, st);
		return len;
	}
	case OP_DEL: {
		if (len < 4) {
			uint8_t reply[2] = { op, ST_INVALID };
			ctrl_notify(reply, sizeof(reply));
			return len;
		}
		uint16_t sid = p[2] | (p[3] << 8);
		/* Spec: DEL refused if folder is "already synced" (no .unsynced
		 * marker). DEL only deletes unsynced folders. */
		char marker[80];
		snprintf(marker, sizeof(marker), SD_MOUNT_POINT "/SESSION_%05u/.unsynced", sid);
		uint8_t mtype = 0;
		int sret = sd_writer_stat(marker, &mtype, NULL);
		if (sret == -ENOENT) {
			uint8_t reply[2] = { op, ST_ALREADY_SYNCED };
			ctrl_notify(reply, sizeof(reply));
			LOG_INF("DEL SESSION_%05u refused: no .unsynced marker", sid);
			return len;
		}
		/* Unlink files then folder. */
		static const char *const files[] = { ".unsynced", "audio.wav",
						     "imu.csv", "meta.json" };
		bool any_io_err = false;
		for (size_t i = 0; i < ARRAY_SIZE(files); i++) {
			char p2[96];
			snprintf(p2, sizeof(p2), SD_MOUNT_POINT "/SESSION_%05u/%s",
				 sid, files[i]);
			int r = sd_writer_unlink(p2);
			if (r != 0 && r != -ENOENT) any_io_err = true;
		}
		char folder[80];
		snprintf(folder, sizeof(folder), SD_MOUNT_POINT "/SESSION_%05u", sid);
		int fr = sd_writer_unlink(folder);
		if (fr != 0 && fr != -ENOENT) any_io_err = true;
		uint8_t st = any_io_err ? ST_IO_ERR : ST_OK;
		uint8_t reply[2] = { op, st };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("DEL SESSION_%05u: status=0x%02x", sid, st);
		return len;
	}
	case OP_RESET_CTL: {
		atomic_clear(&read_abort_flag);
		uint8_t reply[2] = { op, ST_OK };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("RESET_CTL: state cleared");
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
	static uint32_t chunk_n;
	static uint32_t nomem_total;
	if (atomic_get(&read_abort_flag)) { chunk_n = nomem_total = 0; return -ECANCELED; }
	chunk_n++;
	/* Per-chunk logging is too chatty at multi-thousand-chunk transfers;
	 * keep one boundary log every 500 chunks (~2 min @ 5 KB/s) for sanity. */
	if (chunk_n == 1 || (chunk_n % 500) == 0) {
		LOG_INF("read_chunk_cb #%u: %u B sent so far, nomem_total=%u",
			chunk_n, chunk_n * 240, nomem_total);
	}
	for (int i = 0; i < 20; i++) {
		if (atomic_get(&read_abort_flag)) return -ECANCELED;
		int ret = data_notify(chunk, len);
		if (ret == 0) return 0;
		if (ret == -ENOMEM || ret == -EAGAIN) {
			nomem_total++;
			k_msleep(5);
			continue;
		}
		LOG_ERR("read_chunk_cb #%u: data_notify: %d", chunk_n, ret);
		return -EIO;
	}
	LOG_ERR("read_chunk_cb #%u: TX buffer stuck after 20×5ms retries "
		"(nomem_total=%u)", chunk_n, nomem_total);
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
	data_notify_init_once();

	/* #34: bring up the dedicated READ work queue. Must complete BEFORE
	 * any client can write OP_READ; ble_sync_init is called from main()
	 * before start_ble(), so this ordering is fine. */
	k_work_queue_start(&read_wq, read_wq_stack,
			   K_THREAD_STACK_SIZEOF(read_wq_stack),
			   READ_WQ_PRIO, NULL);
	k_thread_name_set(&read_wq.thread, "ble_read_wq");
	LOG_INF("Sync Service registered (UUID base 7e7e0001-...).");
	return 0;
}
