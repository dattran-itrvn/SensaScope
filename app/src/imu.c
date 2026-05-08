#include <zephyr/kernel.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "imu.h"

LOG_MODULE_REGISTER(imu, LOG_LEVEL_INF);

#define LSM6DSL_NODE  DT_NODELABEL(lsm6dsl)

/* Register map (subset, see ST DS11990 §9 / AN5040). */
#define LSM6DSL_REG_WHO_AM_I    0x0F
#define LSM6DSL_REG_CTRL1_XL    0x10
#define LSM6DSL_REG_CTRL3_C     0x12
#define LSM6DSL_REG_TAP_CFG     0x58
#define LSM6DSL_REG_TAP_THS_6D  0x59
#define LSM6DSL_REG_INT_DUR2    0x5A
#define LSM6DSL_REG_WAKE_UP_THS 0x5B
#define LSM6DSL_REG_MD1_CFG     0x5E

#define LSM6DSL_WHO_AM_I_VAL    0x6A

static const struct i2c_dt_spec imu_i2c = I2C_DT_SPEC_GET(LSM6DSL_NODE);
static const struct gpio_dt_spec int1 =
	GPIO_DT_SPEC_GET(LSM6DSL_NODE, irq_gpios);

static struct gpio_callback int1_cb;
static struct k_work        tap_work;
static imu_tap_cb_t         user_tap_cb;

static int reg_write(uint8_t reg, uint8_t val)
{
	return i2c_reg_write_byte_dt(&imu_i2c, reg, val);
}

static int reg_read(uint8_t reg, uint8_t *val)
{
	return i2c_reg_read_byte_dt(&imu_i2c, reg, val);
}

uint8_t imu_who_am_i(void)
{
	uint8_t v = 0;
	if (reg_read(LSM6DSL_REG_WHO_AM_I, &v) < 0) {
		return 0xFF;
	}
	return v;
}

int imu_init(void)
{
	if (!device_is_ready(imu_i2c.bus)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}
	k_msleep(50);  /* LSM6DSL boot ~35ms */

	uint8_t who = imu_who_am_i();
	if (who != LSM6DSL_WHO_AM_I_VAL) {
		LOG_ERR("Unexpected WHO_AM_I = 0x%02X", who);
		return -EIO;
	}
	LOG_INF("LSM6DSL @ 0x%02X: WHO_AM_I = 0x%02X (OK)",
		imu_i2c.addr, who);
	return 0;
}

/* ---------- Double-tap ---------- */
static void tap_work_handler(struct k_work *w)
{
	if (user_tap_cb) {
		user_tap_cb();
	}
}

static void int1_isr(const struct device *port, struct gpio_callback *cb,
		     uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	k_work_submit(&tap_work);
}

int imu_enable_double_tap(imu_tap_cb_t cb)
{
	int ret;

	user_tap_cb = cb;
	k_work_init(&tap_work, tap_work_handler);

	/* CTRL1_XL = 0x60: XL ODR=416 Hz, FS=±2 g, no LPF1 modification.
	 * Tap detection works best at ≥208 Hz.
	 */
	ret = reg_write(LSM6DSL_REG_CTRL1_XL, 0x60);
	if (ret) goto err;

	/* TAP_CFG = 0x8E: latch interrupt; enable Z, Y, X tap recognition.
	 *   bit7 INTERRUPTS_ENABLE
	 *   bit3 SLOPE_FDS=1 (slope filter — useful for tap)
	 *   bit2 TAP_X_EN, bit1 TAP_Y_EN — actually layout is:
	 *   bit7=INTERRUPTS_ENABLE bit6=INACT_EN[1] bit5=INACT_EN[0]
	 *   bit3=SLOPE_FDS bit2=TAP_X_EN bit1=TAP_Y_EN bit0=TAP_Z_EN
	 *   We set: 0x8E = 1000 1110 → INTERRUPTS_ENABLE | SLOPE_FDS |
	 *                              TAP_X_EN | TAP_Y_EN | TAP_Z_EN
	 */
	ret = reg_write(LSM6DSL_REG_TAP_CFG, 0x8E);
	if (ret) goto err;

	/* TAP_THS_6D: tap threshold 5 LSB = 5 × 0.0625 g ≈ 0.31 g.
	 * Lower → more sensitive. 0x09 ≈ 0.56 g (default for AN5040 demo).
	 */
	ret = reg_write(LSM6DSL_REG_TAP_THS_6D, 0x09);
	if (ret) goto err;

	/* INT_DUR2 = 0x7F:
	 *   DUR  (bit7..4)=7  → max gap between 2 taps ≈ 538 ms
	 *   QUIET(bit3..2)=3  → quiet ~57 ms after a tap
	 *   SHOCK(bit1..0)=3  → max tap duration ~28 ms
	 */
	ret = reg_write(LSM6DSL_REG_INT_DUR2, 0x7F);
	if (ret) goto err;

	/* WAKE_UP_THS bit7 SINGLE_DOUBLE_TAP=1 → enable single+double-tap
	 * (we route only double-tap to INT1 below).
	 */
	ret = reg_write(LSM6DSL_REG_WAKE_UP_THS, 0x80);
	if (ret) goto err;

	/* MD1_CFG bit3 = INT1_DOUBLE_TAP. */
	ret = reg_write(LSM6DSL_REG_MD1_CFG, 0x08);
	if (ret) goto err;

	/* GPIO int line (P0.06, active high). */
	if (!gpio_is_ready_dt(&int1)) {
		LOG_ERR("INT1 GPIO not ready");
		return -ENODEV;
	}
	ret = gpio_pin_configure_dt(&int1, GPIO_INPUT);
	if (ret) goto err;
	ret = gpio_pin_interrupt_configure_dt(&int1, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret) goto err;

	gpio_init_callback(&int1_cb, int1_isr, BIT(int1.pin));
	ret = gpio_add_callback(int1.port, &int1_cb);
	if (ret) goto err;

	LOG_INF("Double-tap detection enabled (INT1 = P0.%d)", int1.pin);
	return 0;

err:
	LOG_ERR("imu_enable_double_tap failed at register write: %d", ret);
	return ret;
}
