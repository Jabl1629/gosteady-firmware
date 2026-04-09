/*
 * GoSteady firmware - blinky bring-up target
 *
 * First milestone: toggles led0 (defined by the Thingy:91 X board DTS)
 * at 1 Hz and prints a heartbeat message over the UART console.
 *
 * Success criterion: an LED on the Thingy:91 X blinks visibly and a
 * "heartbeat" log line appears on the serial console once per second.
 *
 * This file is intentionally minimal. Real application code lives in
 * modules added as the firmware arc progresses; main.c exists here
 * only to prove the toolchain and the board.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(gosteady, LOG_LEVEL_INF);

#define BLINK_PERIOD_MS 1000

/* led0 alias is defined in the Thingy:91 X board DTS */
#define LED0_NODE DT_ALIAS(led0)

#if !DT_NODE_HAS_STATUS(LED0_NODE, okay)
#error "Unsupported board: led0 devicetree alias is not defined"
#endif

static const struct gpio_dt_spec led0 = GPIO_DT_SPEC_GET(LED0_NODE, gpios);

int main(void)
{
	int ret;
	uint32_t tick = 0;

	LOG_INF("GoSteady firmware starting (build %s %s)", __DATE__, __TIME__);

	if (!gpio_is_ready_dt(&led0)) {
		LOG_ERR("LED device not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&led0, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure LED pin (%d)", ret);
		return ret;
	}

	LOG_INF("Bring-up complete. Entering blink loop.");

	while (1) {
		gpio_pin_toggle_dt(&led0);
		LOG_INF("heartbeat tick=%u", tick++);
		k_msleep(BLINK_PERIOD_MS);
	}

	return 0;
}
