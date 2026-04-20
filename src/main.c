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

/*
 * The Thingy:91 X has an RGB LED exposed as three separate GPIO-controlled
 * channels via board DTS aliases: led0 = red, led1 = green, led2 = blue.
 * Purple = red + blue together with green off.
 */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE  DT_ALIAS(led2)

#if !DT_NODE_HAS_STATUS(LED_RED_NODE, okay) || \
    !DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay) || \
    !DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay)
#error "Unsupported board: led0/led1/led2 devicetree aliases are not all defined"
#endif

static const struct gpio_dt_spec led_red =
	GPIO_DT_SPEC_GET(LED_RED_NODE, gpios);
static const struct gpio_dt_spec led_green =
	GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);
static const struct gpio_dt_spec led_blue =
	GPIO_DT_SPEC_GET(LED_BLUE_NODE, gpios);

static int configure_led(const struct gpio_dt_spec *led, const char *name)
{
	int ret;

	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("%s LED device not ready", name);
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s LED pin (%d)", name, ret);
		return ret;
	}

	return 0;
}

int main(void)
{
	int ret;
	uint32_t tick = 0;

	LOG_INF("GoSteady firmware starting (build %s %s)", __DATE__, __TIME__);

	ret = configure_led(&led_red, "red");
	if (ret < 0) {
		return ret;
	}
	ret = configure_led(&led_green, "green");
	if (ret < 0) {
		return ret;
	}
	ret = configure_led(&led_blue, "blue");
	if (ret < 0) {
		return ret;
	}

	/* Hold green off for the life of this firmware: we only want red + blue
	 * to produce purple. */
	(void)gpio_pin_set_dt(&led_green, 0);

	LOG_INF("Bring-up complete. Entering purple blink loop.");

	while (1) {
		gpio_pin_toggle_dt(&led_red);
		gpio_pin_toggle_dt(&led_blue);
		LOG_INF("heartbeat tick=%u", tick++);
		k_msleep(BLINK_PERIOD_MS);
	}

	return 0;
}
