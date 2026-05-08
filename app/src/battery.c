#include <zephyr/drivers/adc.h>
#include <zephyr/logging/log.h>
#include "battery.h"

LOG_MODULE_REGISTER(battery, LOG_LEVEL_INF);

static const struct adc_dt_spec batt_adc =
	ADC_DT_SPEC_GET_BY_IDX(DT_NODELABEL(vbatt), 0);

int battery_init(void)
{
	if (!adc_is_ready_dt(&batt_adc)) {
		LOG_ERR("ADC not ready");
		return -ENODEV;
	}
	int ret = adc_channel_setup_dt(&batt_adc);
	if (ret < 0) {
		LOG_ERR("adc_channel_setup_dt failed: %d", ret);
	}
	return ret;
}

int battery_read_mv(void)
{
	int16_t raw = 0;
	struct adc_sequence seq = {
		.buffer = &raw,
		.buffer_size = sizeof(raw),
	};
	int ret = adc_sequence_init_dt(&batt_adc, &seq);
	if (ret < 0) return ret;

	ret = adc_read(batt_adc.dev, &seq);
	if (ret < 0) return ret;

	int32_t mv = raw;
	ret = adc_raw_to_millivolts_dt(&batt_adc, &mv);
	if (ret < 0) return ret;

	/* Voltage divider 100k/100k → ×2. */
	return (int)(mv * 2);
}

const char *battery_state_str(int mv)
{
	if (mv >= 4100) return "full";
	if (mv >= 3700) return "ok";
	if (mv >= 3500) return "low";
	if (mv >= 3300) return "warn";
	return "critical";
}
