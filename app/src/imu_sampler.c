#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <stdint.h>

#include "imu.h"
#include "imu_sampler.h"
#include "sd_writer.h"

LOG_MODULE_REGISTER(imu_sampler, LOG_LEVEL_INF);

#define PRODUCER_STACK_SZ  2048
#define PRODUCER_PRIO      K_PRIO_PREEMPT(6)

static K_THREAD_STACK_DEFINE(prod_stack, PRODUCER_STACK_SZ);
static struct k_thread     prod_thread;

static atomic_t            prod_running   = ATOMIC_INIT(0);
static atomic_t            prod_stop_req  = ATOMIC_INIT(0);
static volatile uint32_t   pushed;
static volatile uint32_t   read_errors;

static void producer_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	const int64_t period_us = 1000000 / IMU_SAMPLER_RATE_HZ;  /* 19230 */
	int64_t next_us = k_ticks_to_us_floor64(k_uptime_ticks());

	LOG_INF("imu producer: streaming @ %d Hz", IMU_SAMPLER_RATE_HZ);

	while (!atomic_get(&prod_stop_req)) {
		int16_t a[3], g[3];
		int ret = imu_read_xlg(a, g);
		if (ret == 0) {
			struct sd_writer_imu_sample s = {
				.t_us = k_ticks_to_us_floor64(k_uptime_ticks()),
				.ax = a[0], .ay = a[1], .az = a[2],
				.gx = g[0], .gy = g[1], .gz = g[2],
			};
			sd_writer_push_imu(&s);   /* drop-newest if full */
			pushed++;
		} else {
			read_errors++;
		}

		next_us += period_us;
		int64_t now_us = k_ticks_to_us_floor64(k_uptime_ticks());
		int64_t delay  = next_us - now_us;
		if (delay > 0 && delay < 100000) {
			k_usleep(delay);
		} else if (delay <= 0) {
			next_us = now_us;
		}
	}

	LOG_INF("imu producer: stopped, pushed=%u read_errors=%u",
		pushed, read_errors);
	atomic_clear(&prod_running);
}

int imu_producer_start(void)
{
	if (atomic_get(&prod_running)) return -EALREADY;
	pushed       = 0;
	read_errors  = 0;
	atomic_clear(&prod_stop_req);
	atomic_set(&prod_running, 1);

	k_tid_t tid = k_thread_create(&prod_thread, prod_stack,
				      K_THREAD_STACK_SIZEOF(prod_stack),
				      producer_thread_fn, NULL, NULL, NULL,
				      PRODUCER_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(tid, "imu_prod");
	return 0;
}

int imu_producer_stop(void)
{
	if (!atomic_get(&prod_running)) return -ENOENT;
	atomic_set(&prod_stop_req, 1);
	return 0;
}

bool     imu_producer_is_running(void) { return atomic_get(&prod_running) != 0; }
uint32_t imu_producer_pushed(void)     { return pushed; }
uint32_t imu_producer_read_errors(void){ return read_errors; }
