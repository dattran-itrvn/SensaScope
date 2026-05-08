#include <zephyr/kernel.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "led.h"

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

#define TICK_MS  100   /* pattern resolution */

static const struct gpio_dt_spec led_spec =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

/* Each pattern is a sequence of 0/1 entries, one per TICK_MS. The engine
 * advances `tick` and writes pattern[tick % len]. Periods are sized so
 * `len * TICK_MS` is the visible cadence.
 */
static const uint8_t p_off[]       = { 0 };
static const uint8_t p_idle[]      = { 1, 0, 0, 0, 0, 0, 0, 0, 0, 0 };  /* 1 Hz, 10 % */
static const uint8_t p_recording[] = { 1 };                              /* solid */
static const uint8_t p_sync[]      = { 1, 1, 0, 0, 0 };                  /* 2 Hz, 40 % */
static const uint8_t p_low_batt[]  = { 1, 0 };                           /* 5 Hz, 50 % */
/* SOS Morse at 1 unit = 1 tick: ...---... with 3-tick letter gaps and a
 * 7-tick word gap. 34 ticks ≈ 3.4 s per cycle.
 */
static const uint8_t p_error[]     = {
	1, 0, 1, 0, 1,                /* S = . . .         */
	0, 0, 0,                      /* letter gap        */
	1, 1, 1, 0, 1, 1, 1, 0, 1, 1, 1, /* O = - - -      */
	0, 0, 0,                      /* letter gap        */
	1, 0, 1, 0, 1,                /* S = . . .         */
	0, 0, 0, 0, 0, 0, 0,          /* word gap          */
};

struct led_pattern {
	const uint8_t *bits;
	size_t         len;
};

static const struct led_pattern patterns[LED_STATE__MAX] = {
	[LED_STATE_OFF]       = { p_off,       ARRAY_SIZE(p_off) },
	[LED_STATE_IDLE]      = { p_idle,      ARRAY_SIZE(p_idle) },
	[LED_STATE_RECORDING] = { p_recording, ARRAY_SIZE(p_recording) },
	[LED_STATE_SYNC]      = { p_sync,      ARRAY_SIZE(p_sync) },
	[LED_STATE_LOW_BATT]  = { p_low_batt,  ARRAY_SIZE(p_low_batt) },
	[LED_STATE_ERROR]     = { p_error,     ARRAY_SIZE(p_error) },
};

static led_state_t   curr_state = LED_STATE_OFF;
static size_t        tick;
static struct k_timer led_timer;

static void led_tick(struct k_timer *t)
{
	ARG_UNUSED(t);
	const struct led_pattern *p = &patterns[curr_state];
	uint8_t on = p->bits[tick % p->len];
	gpio_pin_set_dt(&led_spec, on);
	tick++;
}

int led_init(void)
{
	if (!gpio_is_ready_dt(&led_spec)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&led_spec, GPIO_OUTPUT_INACTIVE);
	if (ret) return ret;

	k_timer_init(&led_timer, led_tick, NULL);
	k_timer_start(&led_timer, K_MSEC(TICK_MS), K_MSEC(TICK_MS));
	return 0;
}

void led_set_state(led_state_t state)
{
	if (state >= LED_STATE__MAX) {
		LOG_WRN("invalid state %d, ignoring", state);
		return;
	}
	if (state == curr_state) return;

	curr_state = state;
	tick = 0;  /* restart pattern from the top so transitions look snappy */
	LOG_INF("state → %d", state);
}

led_state_t led_get_state(void)
{
	return curr_state;
}
