#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include "led.h"

LOG_MODULE_REGISTER(led, LOG_LEVEL_INF);

static const struct gpio_dt_spec led_spec =
	GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);

int led_init(void)
{
	if (!gpio_is_ready_dt(&led_spec)) {
		LOG_ERR("LED GPIO not ready");
		return -ENODEV;
	}
	return gpio_pin_configure_dt(&led_spec, GPIO_OUTPUT_INACTIVE);
}

void led_on(void)     { gpio_pin_set_dt(&led_spec, 1); }
void led_off(void)    { gpio_pin_set_dt(&led_spec, 0); }
void led_toggle(void) { gpio_pin_toggle_dt(&led_spec); }
