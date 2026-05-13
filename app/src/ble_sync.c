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

#include "ble_sync.h"

LOG_MODULE_REGISTER(ble_sync, LOG_LEVEL_INF);

/* ---------- UUIDs ---------- */
/* Base: 7e7e0001-3c4f-4b8e-8a8a-5e5e5e5e5e5e. Chỉ phần đầu 32 bit khác
 * giữa service và mỗi characteristic. */
#define UUID128(a32) BT_UUID_128_ENCODE(a32, 0x3c4f, 0x4b8e, 0x8a8a, 0x5e5e5e5e5e5e)

static struct bt_uuid_128 uuid_svc  = BT_UUID_INIT_128(UUID128(0x7e7e0001));
static struct bt_uuid_128 uuid_info = BT_UUID_INIT_128(UUID128(0x7e7e0002));
static struct bt_uuid_128 uuid_ctrl = BT_UUID_INIT_128(UUID128(0x7e7e0003));
static struct bt_uuid_128 uuid_data = BT_UUID_INIT_128(UUID128(0x7e7e0004));
static struct bt_uuid_128 uuid_name = BT_UUID_INIT_128(UUID128(0x7e7e0005));

/* ---------- Subscriber tracking ---------- */
static bool ctrl_subscribed;
static bool data_subscribed;

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

/* ---------- Handler stubs (will be filled by #19.2 - #19.6) ---------- */
static ssize_t info_read_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			    void *buf, uint16_t len, uint16_t offset)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(buf);
	ARG_UNUSED(len);
	ARG_UNUSED(offset);
	LOG_INF("Device Info read (stub — #19.2 will fill)");
	return bt_gatt_attr_read(conn, attr, buf, len, offset, "{}", 2);
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
	LOG_INF("Control write: opcode=0x%02x, status=0x%02x, len=%u (stub — #19.4+ fill)",
		p[0], p[1], len);
	return len;
}

static ssize_t name_write_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			     const void *buf, uint16_t len, uint16_t offset,
			     uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	ARG_UNUSED(flags);
	LOG_INF("Set Name write: len=%u (stub — #19.3 will fill)", len);
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
	LOG_INF("Sync Service registered (UUID base 7e7e0001-...).");
	return 0;
}
