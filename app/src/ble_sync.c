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
#include <zephyr/bluetooth/l2cap.h>
#include <zephyr/net_buf.h>
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
extern int  app_request_start_record_via_ble(void);
extern int  app_request_stop_record_via_ble(void);
extern bool app_is_recording(void);

/* v1.1.2: queue a self-disconnect after an opcode reply so the link is
 * not held during RECORDING. 150 ms gives the controller multiple
 * connection events (7.5-15 ms interval) to drain the notify before
 * we tear the link down — enough margin even on macOS' coerced 30 ms. */
static struct k_work_delayable disconnect_work;
static void disconnect_handler(struct k_work *w);
static void schedule_self_disconnect(void)
{
	k_work_schedule(&disconnect_work, K_MSEC(150));
}

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

/* v1.1.2: actual implementation of self-disconnect (forward decl above). */
static void disconnect_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	if (current_conn) {
		LOG_INF("self-disconnect (post opcode, FSM=%s)", app_state_lc());
		bt_conn_disconnect(current_conn,
				   BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
}

/* #34: forward decl for data-notify state (used by track_disconnected). */
#define N_NOTIFY_SLOTS  16
struct notify_slot {
	struct bt_gatt_notify_params params;
	uint8_t                      buf[244];
	atomic_t                     in_use;
};
static struct notify_slot notify_slots[N_NOTIFY_SLOTS];
static struct k_sem       data_notify_credit;
static bool               data_notify_inited;

/* #19.7 + #34: negotiate high-throughput link parameters after connect.
 *
 * Observation (bench 2026-05-14): macOS Core Bluetooth coerced the
 * connection interval *up* to 45 ms when we didn't request one
 * explicitly. At 45 ms interval the controller can't fill its link
 * even with multi-credit notify flow → throughput stalled around
 * 5 KB/s. Request 7.5-15 ms now; macOS may still coerce, but at least
 * we start with a faster ask.
 *
 * Also retry data-length update — macOS sometimes refuses the first
 * request silently. */
static void negotiate_link(struct bt_conn *conn)
{
	int ret = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (ret) LOG_WRN("phy_update: %d", ret);

	ret = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (ret) LOG_WRN("data_len_update: %d", ret);

	/* min=6 (7.5 ms), max=12 (15 ms), latency=0, timeout=400 (4 s).
	 * macOS Core Bluetooth typically grants 15 ms; if it coerces wider
	 * we still benefit vs the previous 45 ms it picked unilaterally. */
	/* Range request: min=6 (7.5 ms), max=12 (15 ms). macOS commonly
	 * grants 15 ms; forcing min=max=6 caused it to reject entirely and
	 * stay at 30 ms (bench 2026-05-14). Range gives the host room to
	 * negotiate but ensures we don't accept the macOS default 45 ms. */
	struct bt_le_conn_param param = BT_LE_CONN_PARAM_INIT(6, 12, 0, 400);
	ret = bt_conn_le_param_update(conn, &param);
	if (ret) LOG_WRN("param_update: %d", ret);
}

static void log_link_info(struct bt_conn *conn)
{
	struct bt_conn_info info;
	if (bt_conn_get_info(conn, &info) == 0 && info.type == BT_CONN_TYPE_LE) {
		LOG_INF("link: interval=%u (%u.%02u ms) latency=%u timeout=%u",
			info.le.interval,
			(info.le.interval * 125) / 100,
			((info.le.interval * 125) % 100),
			info.le.latency, info.le.timeout);
	}
}

static void on_le_param_updated(struct bt_conn *conn, uint16_t interval,
				uint16_t latency, uint16_t timeout)
{
	ARG_UNUSED(latency); ARG_UNUSED(timeout);
	LOG_INF("le_param_updated: interval=%u (%u.%02u ms)", interval,
		(interval * 125) / 100, (interval * 125) % 100);
	ARG_UNUSED(conn);
}

static void on_le_phy_updated(struct bt_conn *conn,
			      struct bt_conn_le_phy_info *param)
{
	LOG_INF("le_phy_updated: tx=%u rx=%u (1=1M 2=2M)",
		param->tx_phy, param->rx_phy);
	/* Retry DLE now that PHY is on 2M — some hosts (macOS) accept DLE
	 * only after PHY settles, not at the connect moment. */
	int ret = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (ret) LOG_WRN("data_len_update (post-PHY): %d", ret);
}

static void on_le_data_len_updated(struct bt_conn *conn,
				   struct bt_conn_le_data_len_info *info)
{
	ARG_UNUSED(conn);
	LOG_INF("le_data_len: tx_max=%u rx_max=%u",
		info->tx_max_len, info->rx_max_len);
}

static void track_connected(struct bt_conn *conn, uint8_t err)
{
	if (err) return;
	if (current_conn) bt_conn_unref(current_conn);
	current_conn = bt_conn_ref(conn);
	log_link_info(conn);
	negotiate_link(conn);
}

static void track_disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn); ARG_UNUSED(reason);
	if (current_conn) {
		bt_conn_unref(current_conn);
		current_conn = NULL;
	}
	/* #34: hard-reset multi-credit notify state. Abrupt disconnect can
	 * leave slots `in_use=1` with their callbacks never invoked (Zephyr
	 * usually still fires them, but be defensive). Without this the next
	 * session would run with fewer credits or stall on the missing slot. */
	if (data_notify_inited) {
		for (int i = 0; i < N_NOTIFY_SLOTS; i++) {
			atomic_set(&notify_slots[i].in_use, 0);
		}
		k_sem_reset(&data_notify_credit);
		for (int i = 0; i < N_NOTIFY_SLOTS; i++) {
			k_sem_give(&data_notify_credit);
		}
	}
}

BT_CONN_CB_DEFINE(ble_sync_conn_cb) = {
	.connected        = track_connected,
	.disconnected     = track_disconnected,
	.le_param_updated = on_le_param_updated,
	.le_phy_updated   = on_le_phy_updated,
	.le_data_len_updated = on_le_data_len_updated,
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
#define OP_LIST          0x01
#define OP_READ          0x02
#define OP_ACK           0x03
#define OP_ABORT         0x04
#define OP_DEL           0x05
#define OP_START_RECORD  0x06  /* v1.1.1: PC starts a session over BLE */
#define OP_STOP_RECORD   0x07  /* v1.1.1: PC stops the BLE-started session */
#define OP_RESET_CTL     0xFF

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

/* ---------- L2CAP CoC server for high-throughput bulk transfer — #34 ----- *
 * macOS Core Bluetooth caps GATT-notify throughput around 14 kB/s
 * (no DLE, ~8 packets per connection event). L2CAP Connection-Oriented
 * Channel has its own credit-based flow that macOS schedules more
 * generously — bench reports show 30-60 kB/s realistic.
 *
 * PSM 0x0080 is in the LE dynamic range (0x0080-0x00FF). PC side opens
 * the channel after GATT connect, then issues OP_READ via Control; the
 * READ stream goes through this CoC instead of GATT Data notify.
 */
#define SP_L2CAP_PSM       0x0080
#define SP_L2CAP_MTU       1024   /* SDU MTU: gets segmented at LL layer */

static struct bt_l2cap_le_chan sp_l2cap_le_chan;
static struct bt_l2cap_chan   *sp_l2cap_chan;     /* non-NULL while open */

NET_BUF_POOL_DEFINE(sp_l2cap_tx_pool, 16,
		    BT_L2CAP_SDU_BUF_SIZE(SP_L2CAP_MTU),
		    CONFIG_BT_CONN_TX_USER_DATA_SIZE, NULL);

static void sp_l2cap_connected(struct bt_l2cap_chan *chan)
{
	struct bt_l2cap_le_chan *le = BT_L2CAP_LE_CHAN(chan);
	sp_l2cap_chan = chan;
	LOG_INF("L2CAP CoC connected: tx_mtu=%u tx_mps=%u rx_mtu=%u rx_mps=%u",
		le->tx.mtu, le->tx.mps, le->rx.mtu, le->rx.mps);
}

static void sp_l2cap_disconnected(struct bt_l2cap_chan *chan)
{
	ARG_UNUSED(chan);
	sp_l2cap_chan = NULL;
	LOG_INF("L2CAP CoC disconnected");
}

static int sp_l2cap_recv(struct bt_l2cap_chan *chan, struct net_buf *buf)
{
	/* Peripheral only sends; ignore any unexpected RX. */
	ARG_UNUSED(chan);
	LOG_INF("L2CAP CoC RX (ignored): %u byte", buf->len);
	return 0;
}

static const struct bt_l2cap_chan_ops sp_l2cap_ops = {
	.connected    = sp_l2cap_connected,
	.disconnected = sp_l2cap_disconnected,
	.recv         = sp_l2cap_recv,
};

static int sp_l2cap_accept(struct bt_conn *conn,
			   struct bt_l2cap_server *server,
			   struct bt_l2cap_chan **chan)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(server);
	if (sp_l2cap_chan) {
		LOG_WRN("L2CAP CoC accept: already open");
		return -ENOMEM;
	}
	memset(&sp_l2cap_le_chan, 0, sizeof(sp_l2cap_le_chan));
	sp_l2cap_le_chan.chan.ops = &sp_l2cap_ops;
	sp_l2cap_le_chan.rx.mtu   = SP_L2CAP_MTU;
	*chan = &sp_l2cap_le_chan.chan;
	LOG_INF("L2CAP CoC accept (PSM 0x%04x)", SP_L2CAP_PSM);
	return 0;
}

static struct bt_l2cap_server sp_l2cap_server = {
	.psm       = SP_L2CAP_PSM,
	.sec_level = BT_SECURITY_L1,    /* open for dev; production lock later */
	.accept    = sp_l2cap_accept,
};

/* Send `len` bytes over the CoC channel. Blocking on net_buf alloc with a
 * 2 s ceiling so the caller can detect a wedged link. Returns 0 on
 * success or negative errno. */
static int sp_l2cap_send(const uint8_t *data, size_t len)
{
	if (!sp_l2cap_chan) return -ENOTCONN;
	struct net_buf *buf = net_buf_alloc(&sp_l2cap_tx_pool, K_SECONDS(2));
	if (!buf) {
		LOG_ERR("L2CAP send: net_buf_alloc timeout");
		return -ETIMEDOUT;
	}
	/* Reserve headroom for L2CAP SDU header (2 byte length). */
	net_buf_reserve(buf, BT_L2CAP_SDU_CHAN_SEND_RESERVE);
	net_buf_add_mem(buf, data, len);
	int ret = bt_l2cap_chan_send(sp_l2cap_chan, buf);
	if (ret < 0) {
		net_buf_unref(buf);
		LOG_ERR("bt_l2cap_chan_send: %d", ret);
		return ret;
	}
	return 0;
}

/* ---------- Data notify with multi-credit flow control — #34 ---------- *
 * History (2026-05-14): pre-fix `bt_gatt_notify()` returned 0 even when
 * ACL TX pool was exhausted, silently dropping PDUs at exactly
 * CONFIG_BT_BUF_ACL_TX_COUNT chunks. First fix used `bt_gatt_notify_cb`
 * with a single in-flight slot — correct but ~5 KB/s. For 10-min
 * audio.wav (~38 MB) that's 2 h sync; target is ≤ recording time
 * (≤ 10 min) so we need ≥ 80 KB/s.
 *
 * Multi-credit version: N parallel slots, each with its own persistent
 * params + buf. Caller takes a free-slot permit, fills the slot, queues
 * `bt_gatt_notify_cb`; completion releases the slot. This lets the
 * controller keep its pipe full while we keep correct backpressure.
 *
 * N=8 chosen against CONFIG_BT_BUF_ACL_TX_COUNT=32 (lots of headroom
 * for ctrl notifies, EATT chatter, etc.). At 240 B/chunk × 8 in-flight,
 * the controller can saturate a 7.5-15 ms BLE connection interval
 * (~80-200 KB/s theoretical with PHY 2M). macOS typically grants 15 ms;
 * realistic target ~50-80 KB/s. */
static void data_notify_init_once(void)
{
	if (data_notify_inited) return;
	k_sem_init(&data_notify_credit, N_NOTIFY_SLOTS, N_NOTIFY_SLOTS);
	for (int i = 0; i < N_NOTIFY_SLOTS; i++) {
		atomic_set(&notify_slots[i].in_use, 0);
	}
	data_notify_inited = true;
}

static void on_data_notify_done(struct bt_conn *conn, void *user_data)
{
	ARG_UNUSED(conn);
	struct notify_slot *slot = user_data;
	if (slot) {
		atomic_set(&slot->in_use, 0);
	}
	k_sem_give(&data_notify_credit);
}

static int data_notify(const uint8_t *frame, size_t len)
{
	if (!current_conn) return -ENOTCONN;

	/* #34: prefer L2CAP CoC when the channel is open. PC tool opens it
	 * after GATT connect; firmware streams READ data through CoC at
	 * 30-60 kB/s (vs ~14 kB/s GATT-notify ceiling on macOS). The Data
	 * characteristic notify path remains for backward-compatible clients
	 * that don't open the CoC. */
	if (sp_l2cap_chan && len > 0) {
		return sp_l2cap_send(frame, len);
	}

	if (!data_subscribed) return -EACCES;

	/* 0-byte EOF terminator: send directly, no slot needed. */
	if (len == 0) {
		return bt_gatt_notify(current_conn,
				      &ble_sync_svc.attrs[ATTR_DATA_VAL],
				      frame, len);
	}
	if (len > sizeof(notify_slots[0].buf)) return -EINVAL;

	/* Block waiting for a free slot. 3 s ceiling — if the link is
	 * wedged (no callbacks firing), give up so the caller can report
	 * -EIO and the host can recover. */
	if (k_sem_take(&data_notify_credit, K_SECONDS(3))) {
		LOG_ERR("data_notify: credit timeout (link wedged?)");
		return -ETIMEDOUT;
	}

	/* The semaphore guarantees >=1 slot is free; find it. atomic_cas
	 * keeps us safe even if a callback races with this scan. */
	struct notify_slot *slot = NULL;
	for (int i = 0; i < N_NOTIFY_SLOTS; i++) {
		if (atomic_cas(&notify_slots[i].in_use, 0, 1)) {
			slot = &notify_slots[i];
			break;
		}
	}
	if (!slot) {
		/* Should not happen: sem accounting and slot bitmap diverged. */
		LOG_ERR("data_notify: sem held but no free slot");
		k_sem_give(&data_notify_credit);
		return -EIO;
	}

	memcpy(slot->buf, frame, len);
	slot->params = (struct bt_gatt_notify_params){
		.attr      = &ble_sync_svc.attrs[ATTR_DATA_VAL],
		.data      = slot->buf,
		.len       = len,
		.func      = on_data_notify_done,
		.user_data = slot,
	};
	int ret = bt_gatt_notify_cb(current_conn, &slot->params);
	if (ret) {
		atomic_set(&slot->in_use, 0);
		k_sem_give(&data_notify_credit);
		return ret;
	}
	return 0;
}

/* ---------- v1.1.1: BLE-driven record control ---------- */
static struct k_work    record_start_work;
static struct k_work    record_stop_work;
static struct k_sem     record_done;
static int              record_result;

static void record_start_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	record_result = app_request_start_record_via_ble();
	k_sem_give(&record_done);
}

static void record_stop_handler(struct k_work *w)
{
	ARG_UNUSED(w);
	record_result = app_request_stop_record_via_ble();
	k_sem_give(&record_done);
}

/* Map FSM return → wire status code per docs/SYNC_PROTOCOL.md. */
static uint8_t record_status(int ret)
{
	if (ret == 0)        return ST_OK;
	if (ret == -ENOSPC)  return ST_IO_ERR;
	if (ret == -EBUSY)   return ST_BUSY;
	if (ret == -EAGAIN)  return ST_BUSY;          /* low batt */
	return ST_IO_ERR;
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

	/* v1.1.2: when RECORDING, only STOP_RECORD and RESET_CTL are
	 * honoured. STOP_RECORD goes through the FSM owner check below
	 * (rejects owner==TAP). RESET_CTL is allowed because it only
	 * clears local read_abort_flag — no FATFS access, no data path
	 * impact, and tools/sync.py issues it routinely as part of its
	 * setup() before any real op. Everything else (LIST/READ/ACK/
	 * DEL/ABORT) needs FATFS access and is refused with BUSY plus
	 * self-disconnect to keep the link off the recording's back. */
	if (app_is_recording() && op != OP_STOP_RECORD && op != OP_RESET_CTL) {
		uint8_t reply[2] = { op, ST_BUSY };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("Opcode 0x%02x refused: RECORDING in progress", op);
		schedule_self_disconnect();
		return len;
	}

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
	case OP_START_RECORD: {
		/* Dispatch FSM transition to system_work_queue so FATFS access
		 * doesn't block the BT thread. Wait up to 3 s — session_start
		 * does fs_mkdir + 3 × fs_open which is fast on healthy SD. */
		k_sem_reset(&record_done);
		record_result = -EINVAL;
		k_work_submit(&record_start_work);
		k_sem_take(&record_done, K_SECONDS(3));
		uint8_t st = record_status(record_result);
		uint8_t reply[2] = { op, st };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("START_RECORD: ret=%d status=0x%02x", record_result, st);
		/* v1.1.2: release link regardless of outcome. PC's intent on
		 * BLE-start is fire-and-disconnect; failure paths shouldn't
		 * leave the link sitting open either. */
		schedule_self_disconnect();
		return len;
	}
	case OP_STOP_RECORD: {
		k_sem_reset(&record_done);
		record_result = -EINVAL;
		k_work_submit(&record_stop_work);
		k_sem_take(&record_done, K_SECONDS(3));
		uint8_t st = record_status(record_result);
		uint8_t reply[2] = { op, st };
		ctrl_notify(reply, sizeof(reply));
		LOG_INF("STOP_RECORD: ret=%d status=0x%02x", record_result, st);
		/* v1.1.2: brief opcode-only attach; PC dropped here. */
		schedule_self_disconnect();
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
	k_work_init(&record_start_work, record_start_handler);
	k_work_init(&record_stop_work,  record_stop_handler);
	k_sem_init(&record_done, 0, 1);
	k_work_init_delayable(&disconnect_work, disconnect_handler);
	data_notify_init_once();

	int lret = bt_l2cap_server_register(&sp_l2cap_server);
	if (lret) {
		LOG_ERR("bt_l2cap_server_register: %d", lret);
	} else {
		LOG_INF("L2CAP CoC server registered (PSM 0x%04x)", SP_L2CAP_PSM);
	}

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
