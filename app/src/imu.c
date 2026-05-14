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
#define LSM6DSL_REG_CTRL2_G     0x11
#define LSM6DSL_REG_CTRL3_C     0x12
#define LSM6DSL_REG_OUTX_L_G    0x22  /* gyro X low; +12 covers gyro+accel */
#define LSM6DSL_REG_TAP_CFG     0x58
#define LSM6DSL_REG_TAP_THS_6D  0x59
#define LSM6DSL_REG_INT_DUR2    0x5A
#define LSM6DSL_REG_WAKE_UP_THS 0x5B
#define LSM6DSL_REG_MD1_CFG     0x5E

#define LSM6DSL_WHO_AM_I_VAL    0x6A

/* Sensor stack config (kept in sync with #11 sampler).
 *  CTRL1_XL = 0x60: XL ODR=416 Hz, FS=±2 g.
 *               XL stays at 416 Hz so hardware tap detect still works
 *               (AN5040 wants ≥208 Hz). The 52 Hz CSV sampler just polls
 *               the registers at its own rate — slight aliasing is fine
 *               for activity classification.
 *  CTRL2_G  = 0x30: GYRO ODR=52 Hz, FS=±245 dps.
 *  CTRL3_C  = 0x44: BDU=1 (block-data-update — output regs are frozen
 *               between MSB/LSB read pairs), IF_INC=1 (auto-increment
 *               on multi-byte burst reads).
 */
#define LSM6DSL_CTRL1_XL_VAL    0x60
#define LSM6DSL_CTRL2_G_VAL     0x30
#define LSM6DSL_CTRL3_C_VAL     0x44

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
	k_msleep(50);  /* LSM6DSL POR is ~35 ms typical */

	/* #31: on cold boot the I²C read of WHO_AM_I sometimes returns 0xFF
	 * (chip still in POR or LDO ramp not settled). 50 ms alone is on the
	 * edge; retry up to 5× with 50 ms gap so a slow rail or cold LSM6DSL
	 * gets up to ~300 ms total before we declare init failed. */
	uint8_t who = 0xFF;
	for (int attempt = 1; attempt <= 5; attempt++) {
		who = imu_who_am_i();
		if (who == LSM6DSL_WHO_AM_I_VAL) {
			if (attempt > 1) {
				LOG_INF("WHO_AM_I OK on attempt %d (%d ms warm-up)",
					attempt, (attempt - 1) * 50);
			}
			break;
		}
		LOG_WRN("WHO_AM_I=0x%02X (expected 0x%02X) try %d/5",
			who, LSM6DSL_WHO_AM_I_VAL, attempt);
		k_msleep(50);
	}
	if (who != LSM6DSL_WHO_AM_I_VAL) {
		LOG_ERR("IMU not responsive after retries: WHO_AM_I=0x%02X", who);
		return -EIO;
	}

	int ret;
	ret = reg_write(LSM6DSL_REG_CTRL3_C,  LSM6DSL_CTRL3_C_VAL);  if (ret) goto fail;
	ret = reg_write(LSM6DSL_REG_CTRL1_XL, LSM6DSL_CTRL1_XL_VAL); if (ret) goto fail;
	ret = reg_write(LSM6DSL_REG_CTRL2_G,  LSM6DSL_CTRL2_G_VAL);  if (ret) goto fail;

	LOG_INF("LSM6DSL @ 0x%02X: WHO_AM_I=0x%02X, XL=416Hz/±2g, G=52Hz/±245dps",
		imu_i2c.addr, who);
	return 0;

fail:
	LOG_ERR("imu_init: reg config failed: %d", ret);
	return ret;
}

int imu_read_xlg(int16_t accel_xyz[3], int16_t gyro_xyz[3])
{
	/* Burst-read 12 bytes from OUTX_L_G. Chip layout: gx,gy,gz then ax,ay,az
	 * (little-endian int16 each). BDU=1 in CTRL3_C ensures consistent pairs.
	 */
	int16_t raw[6];
	int ret = i2c_burst_read_dt(&imu_i2c, LSM6DSL_REG_OUTX_L_G,
				    (uint8_t *)raw, sizeof(raw));
	if (ret) return ret;

	gyro_xyz[0]  = raw[0];
	gyro_xyz[1]  = raw[1];
	gyro_xyz[2]  = raw[2];
	accel_xyz[0] = raw[3];
	accel_xyz[1] = raw[4];
	accel_xyz[2] = raw[5];
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

	/* CTRL1_XL is already configured at 416 Hz / ±2 g by imu_init().
	 * Tap detection works best at ≥208 Hz; this is also the rate the CSV
	 * sampler polls (at 52 Hz, with implicit downsampling).
	 */

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

	/* TAP_THS_6D: 5-bit threshold, 1 LSB = FS/32 = 62.5 mg at ±2 g.
	 *
	 * Task #22 — raise to "nấc trung" so worn-on-chest motion (walking
	 * shocks ~0.3–0.5 g, clothing rub ~0.4–0.6 g, body roll on bed) no
	 * longer trips double-tap. 0x14 (20 LSB) ≈ 1.25 g → still easy with
	 * a deliberate fingertip tap on the case, rejects accidental jolts.
	 *
	 * Earlier value 0x09 (~0.56 g, AN5040 demo) was too hot for 24/7 wear.
	 */
	ret = reg_write(LSM6DSL_REG_TAP_THS_6D, 0x14);
	if (ret) goto err;

	/* INT_DUR2 = 0x4E:
	 *   DUR  (bit7..4)=4 → max gap between 2 taps ≈ 307 ms (was 538 ms)
	 *                      Forces a quick double-tap; two random jolts
	 *                      ≥ 350 ms apart no longer count.
	 *   QUIET(bit3..2)=3 → quiet ~57 ms after a tap (unchanged — filters
	 *                      mechanical ringing on the case after impact)
	 *   SHOCK(bit1..0)=2 → max tap duration ~19 ms (was 28 ms)
	 *                      Slow rubs/swipes filtered as non-tap.
	 *
	 * Packed: (4 << 4) | (3 << 2) | 2 = 0x4E.
	 */
	ret = reg_write(LSM6DSL_REG_INT_DUR2, 0x4E);
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
