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
#include <zephyr/sys/atomic.h>
#include <zephyr/drivers/i2c.h>
#include <math.h>
#include <stdlib.h>

#include "session.h"
#include "dump.h"
#include "cellular.h"

LOG_MODULE_REGISTER(gosteady, LOG_LEVEL_INF);

#define HEARTBEAT_PERIOD_MS  1000
#define SAMPLE_PERIOD_MS     10   /* 100 Hz */

/* Phase 3: how many seconds of M9-motion-gate stillness before the
 * main loop auto-closes an open session. M10.5 spec calls for a
 * default of 30 s; using 15 s for bench testing so cycles are quick.
 * TODO: move to Kconfig with a deployment-mode override (30 s) when
 * Phase 5 (FIELD_MODE) lands. */
#define AUTO_STOP_STATIONARY_S  15

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

/* Phase 5: deployment-mode pre-walk metadata. Per the M10.5 spec
 * patient attribution is post-hoc cloud-side via DeviceAssignment
 * lookup on `serial`, so the firmware never knows or stamps a
 * patient identity. The PRE-WALK fields the bench operator fills
 * via capture.html become deployment placeholders here, and the
 * cloud-side ingestion treats them as constants. */
static const struct gosteady_prewalk FIELD_PREWALK = {
	.subject_id           = "deployed",
	.walker_type          = GS_WALKER_TWO_WHEEL,
	.cap_type             = GS_CAP_GLIDE,
	.walker_model         = "unspecified",
	.mount_config         = GS_MOUNT_FRONT_LEFT_LEG,
	.course_id            = "field",
	.intended_distance_ft = 0,
	.surface              = GS_SURFACE_POLISHED_CONCRETE,  /* placeholder; auto-surface classifier overrides */
	.intended_speed       = GS_SPEED_NORMAL,
	.direction            = GS_DIR_STRAIGHT,
	.run_type             = GS_RUN_NORMAL,
	.operator_id          = "auto",
};

#define ACTIVE_PREWALK \
	(IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE) ? &FIELD_PREWALK : &BENCH_PREWALK)

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
	/* Phase 5: deployment-mode firmware shows no LEDs in normal
	 * operation per the M10.5 anti-features list. Only the M12.1e
	 * pre-activation blue LED is allowed. Skip the recording LED
	 * entirely in field builds — the chip stays dark while
	 * sessions auto-capture. */
	if (IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE)) {
		return;
	}
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

/* ---- Phase 1b: BMI270 power management (suspend between sessions) ----
 *
 * The BMI270 is the algorithm's primary sensor and runs at 100 Hz when a
 * session is active. Between sessions the chip has nothing useful to
 * sample, but at normal-mode current draw (~325 µA continuous per
 * datasheet) it's the dominant idle term in the deployment power
 * budget. Suspended (PWR_CTRL.ACC_EN=0, GYR_EN=0) it draws ~0.5 µA —
 * roughly 600× less.
 *
 * `sensor_attr_set(SAMPLING_FREQUENCY=0)` is the documented Zephyr way
 * to suspend on this driver: see drivers/sensor/bosch/bmi270/bmi270.c
 * `set_accel_odr_osr` (and the gyro equivalent) — `odr_bits == 0` clears
 * the corresponding ENABLE bit in PWR_CTRL. Setting it back to 100 Hz
 * re-enables the chip (the ODR/range/oversampling registers retain
 * their values across the disable, so we don't have to re-run the full
 * configure_bmi270() sequence).
 *
 * Resume timing: per BMI270 datasheet, the accelerometer needs a few ms
 * to produce valid data after PWR_CTRL.ACC_EN goes low → high. The
 * sampler thread's natural 10 ms tick gives some headroom, but we
 * explicitly settle and discard the first sample after resume to avoid
 * any first-sample staleness regression in M5/M6/M10 paths. */
static int bmi270_set_active(bool on)
{
	struct sensor_value freq = {
		.val1 = on ? 100 : 0,
		.val2 = 0,
	};
	int ret = sensor_attr_set(bmi270, SENSOR_CHAN_ACCEL_XYZ,
				   SENSOR_ATTR_SAMPLING_FREQUENCY, &freq);
	if (ret < 0) {
		LOG_ERR("bmi270 accel %s failed (%d)", on ? "resume" : "suspend", ret);
		return ret;
	}
	ret = sensor_attr_set(bmi270, SENSOR_CHAN_GYRO_XYZ,
			       SENSOR_ATTR_SAMPLING_FREQUENCY, &freq);
	if (ret < 0) {
		LOG_ERR("bmi270 gyro %s failed (%d)", on ? "resume" : "suspend", ret);
		return ret;
	}
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

	/* Phase 1b: count of leading samples to drop after BMI270 resume.
	 *
	 * Empirically determined on bench 2026-04-27: the chip needs
	 * ~4-5 ODR periods past the 25 ms settle window before its data
	 * registers report fresh values rather than register-reset zeros.
	 * Logging the dropped samples directly during debug showed:
	 *   drop iter 1 (~26 ms post-resume): zeros
	 *   drop iter 2 (~26 ms post-resume, catch-up): zeros
	 *   drop iter 3 (~37 ms post-resume): zeros
	 *   drop iter 4 (~46 ms post-resume): real gravity sample
	 *   drop iter 5 (~57 ms post-resume): real gravity sample
	 *
	 * Dropping 4 = guarantees we discard all zero-residue samples
	 * with one extra iteration of safety margin. First persisted
	 * sample lands ~50 ms post-resume with valid data. Cost: 4
	 * missing samples at start of every session = 40 ms of data,
	 * immaterial to the M9 algorithm (motion gate has 500 ms
	 * settling window before peaks count). */
	uint32_t samples_to_drop = 0;

	while (1) {
		bool active = gosteady_session_is_active();

		if (active && !was_active) {
			/* Phase 1b: idle→active transition. Resume BMI270
			 * (was suspended between sessions to save ~325 µA),
			 * settle for ≥2 ODR periods, discard fetches to
			 * clear any stale data left in the chip's data
			 * registers from before suspend, then start
			 * capturing.
			 *
			 * Settle width chosen empirically: at 100 Hz ODR
			 * one period is 10 ms. After PWR_CTRL.ACC_EN goes
			 * 0→1 the chip needs at least one full period
			 * before fresh samples are available. With only 5 ms
			 * settle + 1 prime fetch we observed the first
			 * captured sample of the very-first-after-boot
			 * session reading all zeros (residual register
			 * state). 25 ms + 2 prime fetches guarantees ≥2
			 * full ODR periods elapse and at least one fresh
			 * sample lands in the data registers before the
			 * "real" fetch in the body below.
			 *
			 * Cost: only the first iteration of every session
			 * pays this — subsequent iterations run at the
			 * normal 10 ms tick. Session timestamp is
			 * unaffected (session_base_ms captures AFTER the
			 * settle; first persisted sample is t_ms ≈ 0). */
			if (bmi270_set_active(true) == 0) {
				LOG_INF("bmi270: resumed for session");
			}
			k_msleep(25);
			(void)sensor_sample_fetch(bmi270); /* prime 1, discard */
			(void)sensor_sample_fetch(bmi270); /* prime 2, discard */
			session_base_ms = k_uptime_get_32();
			samples_to_drop = 4; /* see comment at samples_to_drop decl */
		} else if (!active && was_active) {
			/* Phase 1b: active→idle transition. Session is
			 * stopped and the writer thread has flushed and
			 * closed the file (gosteady_session_stop is
			 * synchronous on the writer's stop_done_sem per the
			 * 2026-04-25 race fix). Safe to suspend BMI270
			 * now — no in-flight reads. */
			if (bmi270_set_active(false) == 0) {
				LOG_INF("bmi270: suspended (idle)");
			}
		}
		was_active = active;

		/* Skip the SPI fetch entirely when not active — chip is
		 * suspended and the read would either fail or return
		 * stale data, and we'd discard it anyway. Saves SPI bus
		 * traffic + CPU cycles on every idle tick. */
		if (active &&
		    sensor_sample_fetch(bmi270) == 0 &&
		    sensor_channel_get(bmi270, SENSOR_CHAN_ACCEL_XYZ, accel) == 0 &&
		    sensor_channel_get(bmi270, SENSOR_CHAN_GYRO_XYZ, gyro) == 0) {

			/* Drop the leading post-resume samples that come back
			 * as all-zeros from the chip's data registers. */
			if (samples_to_drop > 0) {
				samples_to_drop--;
				goto schedule_next;
			}

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

schedule_next:
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

/* Forward declarations for state shared between Phase 1a (ADXL367 trigger
 * handler) and Phase 2 (auto-start coordinator). The trigger handler and
 * its counters live further down, near the rest of the ADXL367 plumbing. */
static atomic_t s_motion_count;
static atomic_t s_inactivity_count;
static K_SEM_DEFINE(motion_event_sem, 0, 1);

/* ---- Phase 2: ADXL367 motion → BMI270 confirmation → auto session start ----
 *
 * Connects Phase 1a (ADXL367 wake-on-motion) and Phase 1b (BMI270 PM)
 * into the deployment-mode auto-start state machine described in the
 * M10.5 spec:
 *
 *   "Auto-start session capture on motion. ADXL367 wake-on-motion via
 *    INT1 line wakes the nRF9151 from deep sleep; nRF9151 confirms
 *    sustained motion via short BMI270 sample window before opening
 *    a session."
 *
 * Why a confirmation window instead of opening on the raw ADXL367
 * trigger? Because chip-level activity detection at 50 mg threshold
 * fires on lots of things that aren't real walking — door slam, walker
 * bumping the wall, cap getting set down on a hard surface. Each false-
 * positive session-open + close cycle wastes a few hundred ms of
 * BMI270 power and writes a useless little file to flash. Spending
 * 500 ms confirming with the algorithm-grade sensor (BMI270, the same
 * chip the M9 motion gate runs on) before committing pays for itself
 * in deployment.
 *
 * State machine (per motion event):
 *   wait on motion_event_sem
 *   ├── if session already active (manual SW0/BLE start): ignore
 *   └── else:
 *         resume BMI270, settle + drop 4 (Phase 1b warm-up pattern)
 *         sample 500 ms / 50 ticks of |a|/g − 1 → compute σ
 *         if σ > 0.05 g → real walking → session_start(), sampler takes over
 *         if σ ≤ 0.05 g → false positive → suspend BMI270, return to idle
 *
 * Latency budget (real-motion → green LED):
 *   ~100 ms ADXL367 chip-level filter (act_time=10 samples @ 100Hz)
 * + ~50 ms  coordinator wakes + BMI270 resume + warm-up
 * + 500 ms  confirmation window
 * + ~few ms session_start handshake + LED set
 *   ────────
 *   ≈ 0.7 s
 */

#define AUTO_START_CONFIRM_MS         500
#define AUTO_START_CONFIRM_PERIOD_MS  10  /* 100 Hz, matches BMI270 ODR */
#define AUTO_START_CONFIRM_SAMPLES    (AUTO_START_CONFIRM_MS / AUTO_START_CONFIRM_PERIOD_MS)
#define AUTO_START_SIGMA_THR_G        0.05f
#define G_MS2                          9.80665f

K_THREAD_STACK_DEFINE(auto_start_stack, 2048);
static struct k_thread auto_start_thread;

static void auto_start_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct sensor_value accel[3];

	while (1) {
		/* Block until ADXL367 wake-on-motion fires. */
		(void)k_sem_take(&motion_event_sem, K_FOREVER);

		/* Drop the event if a session is already capturing — manual
		 * SW0/BLE START got in first, or the previous auto-start
		 * already promoted to a session and the sampler hasn't
		 * stopped yet. */
		if (gosteady_session_is_active()) {
			continue;
		}

		LOG_INF("auto-start: motion observed → confirming with BMI270");

		/* Resume BMI270 (idempotent if already on) and apply the
		 * Phase 1b warm-up pattern: 25 ms settle + 2 prime fetches
		 * + 4 dropped samples to clear register-reset zeros. */
		(void)bmi270_set_active(true);
		k_msleep(25);
		(void)sensor_sample_fetch(bmi270);
		(void)sensor_sample_fetch(bmi270);
		for (int i = 0; i < 4; i++) {
			k_msleep(AUTO_START_CONFIRM_PERIOD_MS);
			(void)sensor_sample_fetch(bmi270);
		}

		/* Sample 500 ms of |a|/g, compute σ of (|a|/g − 1) so
		 * gravity drops out and we're left with motion energy. */
		float sum = 0.0f;
		float sum_sq = 0.0f;
		int n = 0;
		float peak_dev = 0.0f;
		for (int i = 0; i < AUTO_START_CONFIRM_SAMPLES; i++) {
			k_msleep(AUTO_START_CONFIRM_PERIOD_MS);
			if (sensor_sample_fetch(bmi270) != 0) { continue; }
			if (sensor_channel_get(bmi270, SENSOR_CHAN_ACCEL_XYZ, accel) != 0) { continue; }
			float ax = (float)sensor_value_to_double(&accel[0]);
			float ay = (float)sensor_value_to_double(&accel[1]);
			float az = (float)sensor_value_to_double(&accel[2]);
			float mag_g = sqrtf(ax * ax + ay * ay + az * az) / G_MS2;
			float dev = mag_g - 1.0f;
			sum    += dev;
			sum_sq += dev * dev;
			if (fabsf(dev) > peak_dev) { peak_dev = fabsf(dev); }
			n++;
		}
		if (n == 0) {
			LOG_WRN("auto-start: no valid samples in confirmation window — abort");
			(void)bmi270_set_active(false);
			continue;
		}
		float mean  = sum / (float)n;
		float var   = sum_sq / (float)n - mean * mean;
		float sigma = (var > 0.0f) ? sqrtf(var) : 0.0f;

		if (sigma > AUTO_START_SIGMA_THR_G) {
			LOG_INF("auto-start: motion CONFIRMED (σ=%.4f g, peak=%.3f g, n=%d) → opening session",
				(double)sigma, (double)peak_dev, n);
			int ret = gosteady_session_start(ACTIVE_PREWALK);
			if (ret == 0) {
				led_set_recording(true);
				/* Session is now active — sampler thread's
				 * idle→active transition handler will take
				 * over BMI270 ownership on its next tick.
				 * The chip is currently on; no harm in the
				 * sampler's redundant resume call (the
				 * underlying register write is idempotent).
				 *
				 * Stay in the loop to ignore further motion
				 * events until the session ends. */
				continue;
			}
			LOG_WRN("auto-start: session_start failed (%d) → returning to idle", ret);
		} else {
			LOG_INF("auto-start: motion NOT confirmed (σ=%.4f g, peak=%.3f g, n=%d, threshold=%.2f g) → idle",
				(double)sigma, (double)peak_dev, n,
				(double)AUTO_START_SIGMA_THR_G);
		}

		/* Either confirmation failed or session_start failed — put
		 * BMI270 back to sleep so we're back in the cheap idle
		 * state. Drain any motion events that piled up during the
		 * 500 ms confirmation window so we don't immediately
		 * re-enter (debounce). */
		(void)bmi270_set_active(false);
		(void)k_sem_reset(&motion_event_sem);
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
		int ret = gosteady_session_start(ACTIVE_PREWALK);
		if (ret < 0) {
			LOG_ERR("session_start failed (%d)", ret);
			return;
		}
		led_set_recording(true);
	}
}

/* ---- ADXL367 wake-on-motion (Phase 1a, replaces M2 1 Hz sanity poll) ----
 *
 * The 1 Hz polling print served its purpose in M2 — confirmed the chip
 * was talking — but it's pure CPU/SPI noise from M3 onward. Phase 1a
 * replaces it with an interrupt-driven trigger: the ADXL367 raises
 * INT1 on sustained motion crossing the configured activity threshold,
 * the Zephyr driver fires this handler from the system workqueue, and
 * we log + post a semaphore. The same plumbing becomes the wake source
 * for nRF9151 deep sleep + the auto-start input for sessions in later
 * phases.
 *
 * Counters give bench visibility into how often the gate is firing
 * without flooding the log if motion lingers; the foreground main loop
 * reads them once a heartbeat and logs deltas.
 */

/* Counters + motion_event_sem are forward-declared earlier, near the
 * Phase 2 auto-start coordinator that consumes them. */

static void adxl367_trigger_handler(const struct device *dev,
				     const struct sensor_trigger *trig)
{
	ARG_UNUSED(dev);

	switch ((int)trig->type) {
	case SENSOR_TRIG_THRESHOLD:
		atomic_inc(&s_motion_count);
		/* Non-blocking give: if the foreground hasn't consumed a
		 * prior pulse yet, that's fine — one pulse per quiescent
		 * window is enough for an auto-start decision. */
		k_sem_give(&motion_event_sem);
		break;
	case SENSOR_TRIG_DELTA:
		atomic_inc(&s_inactivity_count);
		break;
	default:
		break;
	}
}

/* Direct I²C handle for the ADXL367. Used by the register-dump helper,
 * the LINKED-mode workaround, and the INT1 GPIO+STATUS probe — all
 * paths that bypass the Zephyr sensor abstraction. The bus phandle
 * and address come from the devicetree (`&accel` node). */
static const struct device *const adxl367_i2c_bus =
	DEVICE_DT_GET(DT_BUS(DT_NODELABEL(accel)));
#define ADXL367_I2C_ADDR 0x1d

/* Workaround for upstream Zephyr ADXL367 driver: it hardcodes LINKLOOP=01
 * (LINKED mode) when CONFIG_ADXL367_TRIGGER is enabled. In LINKED mode at
 * power-on the chip enters MEASURE → AWAKE=1 → the activity/inactivity
 * state machine starts in the "looking-for-inactivity" half of the loop.
 * Until INACT fires, ACT events get suppressed. With our INACT threshold
 * tuned for sensitive idle detection (well below 1 g), the chip can't
 * easily satisfy INACT either, so the chip stays stuck and never fires
 * anything on motion.
 *
 * ADI's documented workaround for this exact scenario (see EZ thread
 * "adxl367 loop mode") is to either move to LOOP mode or DEFAULT mode.
 * DEFAULT mode (LINKLOOP=00) makes ACT and INACT independent — both
 * fire on the underlying condition regardless of any chip-side state
 * machine — which is what we actually want here: we just want a wake
 * pulse on motion, not a state-machine-tracked active/inactive enum.
 *
 * Sequence per the chip datasheet:
 *   1. POWER_CTL → STANDBY (0x00) — required to safely modify control
 *      registers without races against the on-going measurement loop.
 *   2. ACT_INACT_CTL: clear LINKLOOP bits [5:4] to 00, keeping the
 *      ACT_EN / ACT_REF / INACT_EN / INACT_REF bits the Zephyr driver
 *      already set (0x0F).
 *   3. POWER_CTL → MEASURE (0x02). Re-entry forces a fresh reference
 *      sample capture, which we want to land while the cap is on the
 *      bench (still), so the referenced-mode comparison has a sane
 *      baseline.
 */
static int apply_adxl367_default_mode_workaround(void)
{
	if (!device_is_ready(adxl367_i2c_bus)) {
		return -ENODEV;
	}

	int err;
	/* 1. Enter STANDBY */
	err = i2c_reg_write_byte(adxl367_i2c_bus, ADXL367_I2C_ADDR, 0x2D, 0x00);
	if (err < 0) { LOG_ERR("workaround: write STANDBY failed (%d)", err); return err; }

	/* Brief settle so the chip is definitively quiesced before we touch
	 * ACT_INACT_CTL. Conservative — datasheet says state changes are
	 * effective immediately but there's no harm. */
	k_msleep(2);

	/* 2. Clear LINKLOOP bits [5:4] to 00 (DEFAULT). Mask preserves
	 * ACT_EN / ACT_REF / INACT_EN / INACT_REF that the driver wrote. */
	err = i2c_reg_update_byte(adxl367_i2c_bus, ADXL367_I2C_ADDR, 0x27,
				   0x30, 0x00);
	if (err < 0) { LOG_ERR("workaround: clear LINKLOOP failed (%d)", err); return err; }

	/* 3. Re-enter MEASURE. Reference samples for ACT/INACT get captured
	 * on this STANDBY→MEASURE transition; cap should be still here. */
	err = i2c_reg_write_byte(adxl367_i2c_bus, ADXL367_I2C_ADDR, 0x2D, 0x02);
	if (err < 0) { LOG_ERR("workaround: write MEASURE failed (%d)", err); return err; }

	/* Verify the new ACT_INACT_CTL state lands so the workaround is
	 * unambiguous in the boot log. */
	uint8_t verify = 0;
	if (i2c_reg_read_byte(adxl367_i2c_bus, ADXL367_I2C_ADDR, 0x27, &verify) == 0) {
		LOG_INF("workaround: ACT_INACT_CTL now 0x%02x (LINKLOOP %s)",
			verify,
			(verify & 0x30) == 0x00 ? "DEFAULT" :
			(verify & 0x30) == 0x10 ? "LINKED" :
			(verify & 0x30) == 0x30 ? "LOOPED" : "?");
	}

	return 0;
}

static int configure_adxl367_wake_on_motion(void)
{
	/*
	 * SINGLE trigger registration only — `SENSOR_TRIG_THRESHOLD` already
	 * covers both ACT and INACT in the upstream driver (see
	 * adxl367_trigger.c:adxl367_thread_cb — it dispatches to th_handler
	 * whenever STATUS.ACT OR STATUS.INACT is set).
	 *
	 * Do NOT also register `SENSOR_TRIG_DELTA`. The driver hits its
	 * `default:` case for DELTA and returns -ENOTSUP — but only AFTER
	 * disabling the GPIO interrupt at the top of `sensor_trigger_set`.
	 * The result: GPIO interrupt is left permanently disabled and the
	 * Zephyr work-queue handler never fires regardless of how many ACT
	 * pulses INT1 emits. (Verified via direct GPIO probe on the bench
	 * 2026-04-26: with both calls, INT1 line pulses fine but
	 * `th_handler` never fires; with single THRESHOLD call, handler
	 * dispatches normally.)
	 */
	static const struct sensor_trigger trig_motion = {
		.type = SENSOR_TRIG_THRESHOLD,
		.chan = SENSOR_CHAN_ACCEL_XYZ,
	};

	int ret = sensor_trigger_set(adxl367, &trig_motion,
				     adxl367_trigger_handler);
	if (ret < 0) {
		LOG_ERR("adxl367 motion trigger set (%d)", ret);
		return ret;
	}

	LOG_INF("adxl367 wake-on-motion armed (act_thr=%d, act_time=%d samples, inact_thr=%d, inact_time=%d samples)",
		CONFIG_ADXL367_ACTIVITY_THRESHOLD, CONFIG_ADXL367_ACTIVITY_TIME,
		CONFIG_ADXL367_INACTIVITY_THRESHOLD, CONFIG_ADXL367_INACTIVITY_TIME);
	return 0;
}

/* ---- DEBUG (kept, gated): direct ADXL367 register dump via I²C ----
 *
 * Bench-only diagnostic. Reads back the activity-detection registers
 * we believe the Zephyr driver wrote at init. Useful when the wake-on-
 * motion path stops working again (e.g., after a driver upgrade or a
 * Kconfig change that's not obvious). Currently not called from any
 * hot path; declare via a Kconfig (TBD) or call manually from a test
 * fixture.
 *
 * Marked __unused so the compiler doesn't warn while the call is
 * commented out.
 */

__unused static void dump_adxl367_registers(void)
{
	if (!device_is_ready(adxl367_i2c_bus)) {
		LOG_ERR("adxl367 I2C bus not ready for register dump");
		return;
	}

	struct {
		uint8_t reg;
		const char *name;
	} regs[] = {
		{0x00, "DEVID_AD"},
		{0x0B, "STATUS"},
		{0x20, "THRESH_ACT_H"},
		{0x21, "THRESH_ACT_L"},
		{0x22, "TIME_ACT"},
		{0x23, "THRESH_INACT_H"},
		{0x24, "THRESH_INACT_L"},
		{0x25, "TIME_INACT_H"},
		{0x26, "TIME_INACT_L"},
		{0x27, "ACT_INACT_CTL"},
		{0x28, "FIFO_CONTROL"},
		{0x2A, "INTMAP1_LOWER"},
		{0x2B, "INTMAP2_LOWER"},
		{0x2C, "FILTER_CTL"},
		{0x2D, "POWER_CTL"},
	};

	LOG_INF("=== ADXL367 register dump ===");
	for (size_t i = 0; i < ARRAY_SIZE(regs); i++) {
		uint8_t val = 0xff;
		int err = i2c_reg_read_byte(adxl367_i2c_bus,
					    ADXL367_I2C_ADDR, regs[i].reg, &val);
		if (err < 0) {
			LOG_ERR("  %s (0x%02x): read err %d", regs[i].name, regs[i].reg, err);
		} else {
			LOG_INF("  %s (0x%02x) = 0x%02x", regs[i].name, regs[i].reg, val);
		}
	}
	LOG_INF("=== end dump ===");
}

/* Periodic visibility log: print the running counters from the foreground
 * loop so a sealed-cap bench operator can see motion events accumulate
 * without parsing per-event log lines. Called at heartbeat cadence. */
static void log_adxl367_motion_counts(void)
{
	static atomic_val_t prev_motion;
	static atomic_val_t prev_inactivity;

	atomic_val_t m = atomic_get(&s_motion_count);
	atomic_val_t i = atomic_get(&s_inactivity_count);

	if (m != prev_motion || i != prev_inactivity) {
		LOG_INF("adxl367 events: motion=%ld (+%ld) inactivity=%ld (+%ld)",
			(long)m, (long)(m - prev_motion),
			(long)i, (long)(i - prev_inactivity));
		prev_motion = m;
		prev_inactivity = i;
	}
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
	/* Phase 1b: configure_bmi270() leaves the chip running at 100 Hz —
	 * suspend immediately so the chip is idle until a session opens.
	 * Saves ~325 µA between sessions. The suspend keeps all the
	 * config registers (range, OSR, ODR target) intact; the resume
	 * on session start just flips PWR_CTRL.{ACC_EN,GYR_EN} back on.
	 *
	 * BUT: must let the chip take at least one real sample BEFORE
	 * the boot-time suspend, otherwise its data registers go into
	 * suspend holding whatever-was-there-at-power-on (zeros) and
	 * the very-first-after-boot session's first sample reads as
	 * (0,0,0). 30 ms = 3 ODR periods at 100 Hz — generous margin
	 * to guarantee fresh gravity samples sit in the data registers
	 * before suspend takes over. Subsequent suspend/resume cycles
	 * preserve the last pre-suspend sample, so this only matters at
	 * cold boot. (One-time 30 ms cost at boot; doesn't recur.) */
	k_msleep(30);
	if (bmi270_set_active(false) == 0) {
		LOG_INF("bmi270: suspended at boot (idle until session start)");
	}
	if (!IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE)) {
		/* Phase 5: SW0 is bench-only. Field deployments capture
		 * sessions exclusively via Phase 2 motion-triggered
		 * auto-start, so don't bother wiring the button. */
		if ((ret = configure_button()) < 0) { return ret; }
	}
	/* Phase 1a: arm the ADXL367 wake-on-motion path. Failure is
	 * non-fatal — the M2-era 1Hz sanity poll is gone, but motion-
	 * triggered auto-start in later phases needs this. */
	if (configure_adxl367_wake_on_motion() < 0) {
		LOG_WRN("adxl367 wake-on-motion not armed — falling back to bench-button-only session start");
	}

	/* Apply the DEFAULT-mode workaround AFTER the driver has finished
	 * configuring everything, so we override the driver's hardcoded
	 * LINKED state-machine setting with DEFAULT mode (LINKLOOP=00) —
	 * where ACT and INACT events fire independently regardless of any
	 * chip-side state. See comments in apply_adxl367_default_mode_workaround.
	 *
	 * Without this override the chip boots into AWAKE state, gets
	 * stuck in the "looking-for-inactivity" half of the LINKED state
	 * machine, and never fires ACT on motion. Verified end-to-end on
	 * 2026-04-26 bench: with workaround, INT1 pulses fire reliably on
	 * any motion above the configured threshold; without it, INT1
	 * stays low forever no matter how vigorously the device is
	 * moved. */
	if (apply_adxl367_default_mode_workaround() < 0) {
		LOG_WRN("adxl367 DEFAULT-mode workaround failed — wake-on-motion may not fire");
	}

	if (mount_lfs_and_bump_boot_count() < 0) {
		LOG_WRN("LittleFS bring-up failed — session logging disabled");
	}

	/* Start the 100 Hz sampler after all config is done. */
	k_thread_create(&sampler_thread, sampler_stack, K_THREAD_STACK_SIZEOF(sampler_stack),
			sampler_entry, NULL, NULL, NULL,
			5, 0, K_NO_WAIT);
	k_thread_name_set(&sampler_thread, "sampler");

	/* Phase 2: kick off the auto-start coordinator. Lower priority
	 * than the sampler — it spends most of its life blocked on
	 * motion_event_sem, and even when it's running its 500 ms
	 * confirmation window it shouldn't preempt the 100 Hz sampler
	 * loop. */
	k_thread_create(&auto_start_thread, auto_start_stack,
			K_THREAD_STACK_SIZEOF(auto_start_stack),
			auto_start_entry, NULL, NULL, NULL,
			6, 0, K_NO_WAIT);
	k_thread_name_set(&auto_start_thread, "auto_start");

	if (!IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE)) {
		/* Phase 5: uart1 dump channel is bench-only. Skipping in
		 * field mode also gates off the BLE NUS START path, since
		 * the bridge tunnels NUS RX into the same uart1 stream
		 * our dump channel listens on. M12.1e activation cmd
		 * arrives via cellular MQTT, not BLE, so this is the
		 * right gate. */
		if (gosteady_dump_start() < 0) {
			LOG_WRN("dump channel failed to start — file pull disabled");
		}
	}

	/* M12.1a: kick off async LTE-M attach. Non-blocking — registration
	 * completes asynchronously; cellular.c logs state changes + signal
	 * stats + network UTC. No MQTT yet. */
	if (gosteady_cellular_start() < 0) {
		LOG_WRN("cellular bring-up failed — proceeding without modem");
	}

	LOG_INF("Bring-up complete. %s",
		IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE) ?
		"FIELD_MODE: motion-driven autonomous capture only." :
		"Press SW0 to start/stop a session.");

	while (1) {
		/* Consume any pending button presses non-blockingly.
		 * In FIELD_MODE the button isn't wired so the sem never
		 * gets given — the take returns -EAGAIN and we move on.
		 * (Cheaper to leave the no-op call than to gate the
		 * conditional, since k_sem_take with K_NO_WAIT is a
		 * fast path.) */
		if (k_sem_take(&button_press_sem, K_NO_WAIT) == 0) {
			handle_button_press();
		}

		if (!gosteady_session_is_active()) {
			if (!IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE)) {
				/* Bench: purple blink (red+blue toggle),
				 * 1 Hz heartbeat log + motion-counter
				 * delta. All silenced in FIELD_MODE per
				 * M10.5 "no LEDs / minimal log noise"
				 * deployment posture. */
				gpio_pin_toggle_dt(&led_red);
				gpio_pin_toggle_dt(&led_blue);
				LOG_INF("heartbeat tick=%u", tick++);
				log_adxl367_motion_counts();
			}
		} else {
			/* Phase 3 auto-stop: end the session after the M9
			 * motion gate has confirmed AUTO_STOP_STATIONARY_S
			 * seconds of sustained stillness. The gate's own
			 * 500 ms running window + Schmitt exit-hold (2 s)
			 * already debounce against brief mid-walk pauses,
			 * so the counter we read only climbs after the gate
			 * has truly settled into "still". 1 Hz polling here
			 * means the auto-stop fires within ~1 s of crossing
			 * the threshold — fine for a 15-30 s timeout. */
			uint32_t stationary_n = gosteady_session_stationary_samples();
			uint32_t stationary_s = stationary_n / 100;
			if (stationary_s >= AUTO_STOP_STATIONARY_S) {
				LOG_INF("auto-stop: %u s of stationary motion → ending session",
					(unsigned)stationary_s);
				uint32_t count = 0;
				int ret = gosteady_session_stop(&count);
				led_set_recording(false);
				if (ret < 0) {
					LOG_ERR("auto-stop session_stop failed (%d)", ret);
				}
			}
		}
		k_msleep(HEARTBEAT_PERIOD_MS);
	}
	return 0;
}
