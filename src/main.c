/*
 * GoSteady firmware - Milestone 2 sensor bring-up
 *
 * Keeps the Milestone 1 behaviour (purple LED blink + heartbeat log) and
 * adds a 1 Hz poll of the two motion sensors:
 *
 *   - BMI270 (high-performance 6-axis IMU, SPI):
 *       accel XYZ in m/s^2, gyro XYZ in rad/s
 *   - ADXL367 (ultra-low-power 3-axis accel, I2C):
 *       accel XYZ in m/s^2
 *
 * Each tick logs one line per sensor. This is the "raw reads to console"
 * bring-up target for Milestone 2 — no filtering, no session logging,
 * no sleep. All of that lands in later milestones.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
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

/*
 * Both sensor nodes live in the Thingy:91 X board DTS and are enabled via
 * our boards/thingy91x_nrf9151_ns.overlay. Labels come straight from the
 * upstream DTS: `accelerometer_hp` for the BMI270 on SPI, `accel` for the
 * ADXL367 on I2C.
 */
#define BMI270_NODE  DT_NODELABEL(accelerometer_hp)
#define ADXL367_NODE DT_NODELABEL(accel)

#if !DT_NODE_HAS_STATUS(BMI270_NODE, okay)
#error "BMI270 node is not enabled — check boards/thingy91x_nrf9151_ns.overlay"
#endif
#if !DT_NODE_HAS_STATUS(ADXL367_NODE, okay)
#error "ADXL367 node is not enabled — check boards/thingy91x_nrf9151_ns.overlay"
#endif

static const struct device *const bmi270  = DEVICE_DT_GET(BMI270_NODE);
static const struct device *const adxl367 = DEVICE_DT_GET(ADXL367_NODE);

/*
 * Bring the BMI270 out of its power-on suspend state. The driver uploads
 * the sensor config file during init, but accel and gyro stay powered off
 * until we set SAMPLING_FREQUENCY to a non-zero rate (this is what picks
 * the internal power mode). Match the target config captured in the v1
 * annotation schema: 4G / 500 dps / 100 Hz, normal oversampling. The
 * Python-side .dat files ran at ~99.3 Hz, which is this exact config.
 */
static int configure_bmi270(void)
{
	struct sensor_value full_scale, sampling_freq, oversampling;
	int ret;

	full_scale.val1 = 4;  full_scale.val2 = 0;           /* +/- 4 g */
	oversampling.val1 = 1; oversampling.val2 = 0;        /* normal mode */
	sampling_freq.val1 = 100; sampling_freq.val2 = 0;    /* 100 Hz */

	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE, &full_scale);
	if (ret < 0) { LOG_ERR("bmi270 accel FULL_SCALE (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
	if (ret < 0) { LOG_ERR("bmi270 accel OVERSAMPLING (%d)", ret); return ret; }
	/* sampling freq last — this is the write that powers the sensor on. */
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
	if (ret < 0) { LOG_ERR("bmi270 accel SAMPLING_FREQUENCY (%d)", ret); return ret; }

	full_scale.val1 = 500; full_scale.val2 = 0;          /* +/- 500 dps */
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_FULL_SCALE, &full_scale);
	if (ret < 0) { LOG_ERR("bmi270 gyro FULL_SCALE (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
	if (ret < 0) { LOG_ERR("bmi270 gyro OVERSAMPLING (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
	if (ret < 0) { LOG_ERR("bmi270 gyro SAMPLING_FREQUENCY (%d)", ret); return ret; }

	return 0;
}

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

static void log_bmi270_sample(void)
{
	struct sensor_value accel[3];
	struct sensor_value gyro[3];
	int ret;

	ret = sensor_sample_fetch(bmi270);
	if (ret < 0) {
		LOG_WRN("bmi270 sample_fetch failed (%d)", ret);
		return;
	}

	ret = sensor_channel_get(bmi270, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (ret < 0) {
		LOG_WRN("bmi270 accel channel_get failed (%d)", ret);
		return;
	}

	ret = sensor_channel_get(bmi270, SENSOR_CHAN_GYRO_XYZ, gyro);
	if (ret < 0) {
		LOG_WRN("bmi270 gyro channel_get failed (%d)", ret);
		return;
	}

	LOG_INF("bmi270 accel=[%d.%06d, %d.%06d, %d.%06d] m/s^2  gyro=[%d.%06d, %d.%06d, %d.%06d] rad/s",
		accel[0].val1, abs(accel[0].val2),
		accel[1].val1, abs(accel[1].val2),
		accel[2].val1, abs(accel[2].val2),
		gyro[0].val1, abs(gyro[0].val2),
		gyro[1].val1, abs(gyro[1].val2),
		gyro[2].val1, abs(gyro[2].val2));
}

static void log_adxl367_sample(void)
{
	struct sensor_value accel[3];
	int ret;

	ret = sensor_sample_fetch(adxl367);
	if (ret < 0) {
		LOG_WRN("adxl367 sample_fetch failed (%d)", ret);
		return;
	}

	ret = sensor_channel_get(adxl367, SENSOR_CHAN_ACCEL_XYZ, accel);
	if (ret < 0) {
		LOG_WRN("adxl367 accel channel_get failed (%d)", ret);
		return;
	}

	LOG_INF("adxl367 accel=[%d.%06d, %d.%06d, %d.%06d] m/s^2",
		accel[0].val1, abs(accel[0].val2),
		accel[1].val1, abs(accel[1].val2),
		accel[2].val1, abs(accel[2].val2));
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

	if (!device_is_ready(bmi270)) {
		LOG_ERR("bmi270 device not ready");
		return -ENODEV;
	}
	if (!device_is_ready(adxl367)) {
		LOG_ERR("adxl367 device not ready");
		return -ENODEV;
	}

	ret = configure_bmi270();
	if (ret < 0) {
		return ret;
	}

	LOG_INF("Bring-up complete. Entering purple blink + sensor poll loop.");

	while (1) {
		gpio_pin_toggle_dt(&led_red);
		gpio_pin_toggle_dt(&led_blue);
		LOG_INF("heartbeat tick=%u", tick++);
		log_bmi270_sample();
		log_adxl367_sample();
		k_msleep(BLINK_PERIOD_MS);
	}

	return 0;
}
