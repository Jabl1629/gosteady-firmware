/*
 * GoSteady firmware — Milestones 1–4.
 *
 * Behavior:
 *   - LED: green solid while a session is recording; purple blink @ 1 Hz when idle.
 *   - Heartbeat: once per second on the idle main loop.
 *   - Sampling thread: 100 Hz BMI270 poll. When a session is active, each
 *     sample is appended to /lfs/sessions/<uuid>.dat; otherwise the sample
 *     is discarded. ADXL367 is polled at 1 Hz on the idle loop as a
 *     sanity print — not recorded into session files (see GOSTEADY_CONTEXT.md
 *     for why: BMI270 is the primary sensor for the algorithm, ADXL367 is
 *     the wake-on-motion helper).
 *   - SW0 (button 1) short-press toggles session state (start/stop). When
 *     BLE control lands in M6, it supersedes this trigger path.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>
#include <stdlib.h>

#include "session.h"
#include "dump.h"
#include "cellular.h"

LOG_MODULE_REGISTER(gosteady, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS  1000
#define SAMPLE_PERIOD_MS     10   /* 100 Hz */

/* ---- RGB LED (three GPIO channels; purple = red + blue, green only while recording) ---- */
#define LED_RED_NODE   DT_ALIAS(led0)
#define LED_GREEN_NODE DT_ALIAS(led1)
#define LED_BLUE_NODE  DT_ALIAS(led2)

#if !DT_NODE_HAS_STATUS(LED_RED_NODE, okay) || \
    !DT_NODE_HAS_STATUS(LED_GREEN_NODE, okay) || \
    !DT_NODE_HAS_STATUS(LED_BLUE_NODE, okay)
#error "Unsupported board: led0/led1/led2 devicetree aliases are not all defined"
#endif

static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(LED_RED_NODE,   gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(LED_GREEN_NODE, gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(LED_BLUE_NODE,  gpios);

/* ---- Sensors ---- */
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

/* ---- Button (SW0 / button0, P0.26, active-low pull-up per board DTS) ---- */
#define BUTTON_NODE DT_ALIAS(sw0)
#if !DT_NODE_HAS_STATUS(BUTTON_NODE, okay)
#error "Button alias sw0 is not defined on this board"
#endif
static const struct gpio_dt_spec button = GPIO_DT_SPEC_GET(BUTTON_NODE, gpios);
static struct gpio_callback button_cb;

/* The button ISR just signals the session-controller thread; we never
 * call filesystem code from interrupt context. */
static K_SEM_DEFINE(button_press_sem, 0, 1);

/* Bench-default pre-walk metadata for M4. In M6 these fields will be
 * pushed over BLE by the host's start_session command. */
static const struct gosteady_prewalk BENCH_PREWALK = {
	.subject_id           = "S00",
	.walker_type          = GS_WALKER_TWO_WHEEL,
	.cap_type             = GS_CAP_GLIDE,
	.walker_model         = "unspecified",
	.mount_config         = GS_MOUNT_FRONT_LEFT_LEG,
	.course_id            = "bench_m4",
	.intended_distance_ft = 0,
	.surface              = GS_SURFACE_POLISHED_CONCRETE,
	.intended_speed       = GS_SPEED_NORMAL,
	.direction            = GS_DIR_STRAIGHT,
	.run_type             = GS_RUN_NORMAL,
	.operator_id          = "bench",
};

/* ---- LittleFS mount (unchanged from M3) ---- */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(lfs_data);
static struct fs_mount_t lfs_mnt = {
	.type         = FS_LITTLEFS,
	.fs_data      = &lfs_data,
	.storage_dev  = (void *)FIXED_PARTITION_ID(littlefs_storage),
	.mnt_point    = "/lfs",
};

/* ---- LED state helpers ---- */

static int configure_led(const struct gpio_dt_spec *led, const char *name)
{
	if (!gpio_is_ready_dt(led)) {
		LOG_ERR("%s LED device not ready", name);
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(led, GPIO_OUTPUT_INACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure %s LED pin (%d)", name, ret);
	}
	return ret;
}

/* Drive the RGB LED to show session state. Recording: solid green. Idle:
 * caller handles the purple blink via toggles in the main loop. */
static void led_set_recording(bool recording)
{
	if (recording) {
		(void)gpio_pin_set_dt(&led_red,   0);
		(void)gpio_pin_set_dt(&led_green, 1);
		(void)gpio_pin_set_dt(&led_blue,  0);
	} else {
		(void)gpio_pin_set_dt(&led_green, 0);
		/* red+blue left to the main loop's blink toggling */
	}
}

/* ---- LittleFS + boot counter (M3 regression check) ---- */

static int mount_lfs_and_bump_boot_count(void)
{
	struct fs_statvfs vfs;
	struct fs_file_t f;
	uint32_t count = 0;
	ssize_t n;
	int ret;

	ret = fs_mount(&lfs_mnt);
	if (ret < 0) {
		LOG_ERR("fs_mount(/lfs) failed (%d)", ret);
		return ret;
	}
	ret = fs_statvfs("/lfs", &vfs);
	if (ret == 0) {
		LOG_INF("lfs mounted: block=%lu total=%lu bytes free=%lu bytes",
			(unsigned long)vfs.f_bsize,
			(unsigned long)(vfs.f_blocks * vfs.f_frsize),
			(unsigned long)(vfs.f_bfree  * vfs.f_frsize));
	}

	fs_file_t_init(&f);
	ret = fs_open(&f, "/lfs/boot_count", FS_O_CREATE | FS_O_RDWR);
	if (ret < 0) {
		LOG_ERR("fs_open(/lfs/boot_count) failed (%d)", ret);
		return ret;
	}
	n = fs_read(&f, &count, sizeof(count));
	if (n < 0) {
		LOG_WRN("fs_read boot_count failed (%d); starting from 0", (int)n);
		count = 0;
	} else if (n != sizeof(count)) {
		count = 0;
	}
	count++;
	(void)fs_seek(&f, 0, FS_SEEK_SET);
	if (fs_write(&f, &count, sizeof(count)) < 0) {
		LOG_ERR("fs_write boot_count failed");
	}
	fs_close(&f);
	LOG_INF("boot_count = %u (persisted to /lfs/boot_count)", count);
	return 0;
}

/* ---- Sensor configuration (M2) ---- */

static int configure_bmi270(void)
{
	struct sensor_value full_scale, sampling_freq, oversampling;
	int ret;

	full_scale.val1 = 4;     full_scale.val2 = 0;
	oversampling.val1 = 1;   oversampling.val2 = 0;
	sampling_freq.val1 = 100; sampling_freq.val2 = 0;

	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_FULL_SCALE,   &full_scale);
	if (ret < 0) { LOG_ERR("bmi270 accel FULL_SCALE (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
	if (ret < 0) { LOG_ERR("bmi270 accel OVERSAMPLING (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
	if (ret < 0) { LOG_ERR("bmi270 accel SAMPLING_FREQUENCY (%d)", ret); return ret; }

	full_scale.val1 = 500;   full_scale.val2 = 0;
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_FULL_SCALE,   &full_scale);
	if (ret < 0) { LOG_ERR("bmi270 gyro FULL_SCALE (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_OVERSAMPLING, &oversampling);
	if (ret < 0) { LOG_ERR("bmi270 gyro OVERSAMPLING (%d)", ret); return ret; }
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ, SENSOR_ATTR_SAMPLING_FREQUENCY, &sampling_freq);
	if (ret < 0) { LOG_ERR("bmi270 gyro SAMPLING_FREQUENCY (%d)", ret); return ret; }

	return 0;
}

/* ---- Button plumbing ---- */

static void button_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
	ARG_UNUSED(port);
	ARG_UNUSED(cb);
	ARG_UNUSED(pins);
	/* Non-blocking give; ignore the "semaphore already given" case. */
	k_sem_give(&button_press_sem);
}

static int configure_button(void)
{
	if (!gpio_is_ready_dt(&button)) {
		LOG_ERR("button device not ready");
		return -ENODEV;
	}
	int ret = gpio_pin_configure_dt(&button, GPIO_INPUT);
	if (ret < 0) { LOG_ERR("button configure (%d)", ret); return ret; }

	/* GPIO_INT_EDGE_TO_ACTIVE fires on press (button pulls the line low
	 * through GPIO_ACTIVE_LOW in DTS, so "to active" = press down). */
	ret = gpio_pin_interrupt_configure_dt(&button, GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) { LOG_ERR("button interrupt configure (%d)", ret); return ret; }

	gpio_init_callback(&button_cb, button_isr, BIT(button.pin));
	ret = gpio_add_callback(button.port, &button_cb);
	if (ret < 0) { LOG_ERR("button add callback (%d)", ret); return ret; }

	return 0;
}

/* ---- 100 Hz sampling thread ---- */

/* Generous stack: 2 KB. Sampling thread touches sensor API + fs_write. */
K_THREAD_STACK_DEFINE(sampler_stack, 2048);
static struct k_thread sampler_thread;

static void sampler_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct sensor_value accel[3], gyro[3];
	uint32_t next = k_uptime_get_32();
	uint32_t session_base_ms = 0;

	/* Baseline: what "session_active" was on the previous iteration, so
	 * we can capture the start timestamp exactly once at transition. */
	bool was_active = false;

	while (1) {
		bool active = gosteady_session_is_active();

		if (active && !was_active) {
			session_base_ms = k_uptime_get_32();
		}
		was_active = active;

		if (sensor_sample_fetch(bmi270) == 0 &&
		    sensor_channel_get(bmi270, SENSOR_CHAN_ACCEL_XYZ, accel) == 0 &&
		    sensor_channel_get(bmi270, SENSOR_CHAN_GYRO_XYZ, gyro) == 0 && active) {

			struct gosteady_sample s = {
				.t_ms = k_uptime_get_32() - session_base_ms,
				.ax = (float)sensor_value_to_double(&accel[0]),
				.ay = (float)sensor_value_to_double(&accel[1]),
				.az = (float)sensor_value_to_double(&accel[2]),
				.gx = (float)sensor_value_to_double(&gyro[0]),
				.gy = (float)sensor_value_to_double(&gyro[1]),
				.gz = (float)sensor_value_to_double(&gyro[2]),
			};
			int ret = gosteady_session_append(&s);
			if (ret < 0 && ret != -ENODEV) {
				LOG_WRN("session_append failed (%d)", ret);
			}
		}

		next += SAMPLE_PERIOD_MS;
		int32_t delay = (int32_t)(next - k_uptime_get_32());
		if (delay > 0) {
			k_msleep(delay);
		} else {
			/* Fell behind (flash write stall?) — resync and move on. */
			next = k_uptime_get_32();
			k_yield();
		}
	}
}

/* ---- Button-press session toggle (runs on main context, not ISR) ---- */

static void handle_button_press(void)
{
	if (gosteady_session_is_active()) {
		uint32_t count = 0;
		int ret = gosteady_session_stop(&count);
		led_set_recording(false);
		if (ret < 0) {
			LOG_ERR("session_stop failed (%d)", ret);
		}
	} else {
		int ret = gosteady_session_start(&BENCH_PREWALK);
		if (ret < 0) {
			LOG_ERR("session_start failed (%d)", ret);
			return;
		}
		led_set_recording(true);
	}
}

/* ---- ADXL367 sanity log (unchanged from M2) ---- */

static void log_adxl367_sample(void)
{
	struct sensor_value accel[3];
	if (sensor_sample_fetch(adxl367) < 0) { return; }
	if (sensor_channel_get(adxl367, SENSOR_CHAN_ACCEL_XYZ, accel) < 0) { return; }
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

	if ((ret = configure_led(&led_red,   "red"))   < 0) return ret;
	if ((ret = configure_led(&led_green, "green")) < 0) return ret;
	if ((ret = configure_led(&led_blue,  "blue"))  < 0) return ret;

	if (!device_is_ready(bmi270))  { LOG_ERR("bmi270 device not ready");  return -ENODEV; }
	if (!device_is_ready(adxl367)) { LOG_ERR("adxl367 device not ready"); return -ENODEV; }

	if ((ret = configure_bmi270()) < 0) { return ret; }
	if ((ret = configure_button()) < 0) { return ret; }

	if (mount_lfs_and_bump_boot_count() < 0) {
		LOG_WRN("LittleFS bring-up failed — session logging disabled");
	}

	/* Start the 100 Hz sampler after all config is done. */
	k_thread_create(&sampler_thread, sampler_stack, K_THREAD_STACK_SIZEOF(sampler_stack),
			sampler_entry, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&sampler_thread, "sampler");

	/* Bring up the host-facing dump channel on uart1. */
	if (gosteady_dump_start() < 0) {
		LOG_WRN("dump channel failed to start — file pull disabled");
	}

	/* M12.1a: kick off async LTE-M attach. Non-blocking — registration
	 * completes asynchronously; cellular.c logs state changes + signal
	 * stats + network UTC. No MQTT yet. */
	if (gosteady_cellular_start() < 0) {
		LOG_WRN("cellular bring-up failed — proceeding without modem");
	}

	LOG_INF("Bring-up complete. Press SW0 to start/stop a session.");

	while (1) {
		/* Consume any pending button presses non-blockingly. */
		if (k_sem_take(&button_press_sem, K_NO_WAIT) == 0) {
			handle_button_press();
		}

		if (!gosteady_session_is_active()) {
			/* Idle: purple blink (red+blue toggle), 1 Hz heartbeat log. */
			gpio_pin_toggle_dt(&led_red);
			gpio_pin_toggle_dt(&led_blue);
			LOG_INF("heartbeat tick=%u", tick++);
			log_adxl367_sample();
		}
		k_msleep(HEARTBEAT_PERIOD_MS);
	}
	return 0;
}
