/*
 * M10.7.2 — nPM1300 fuel gauge wiring.
 *
 * Pattern lifted from the upstream NCS sample at
 * nrf/samples/pmic/native/npm13xx_fuel_gauge/src/{main,fuel_gauge}.c with
 * three GoSteady-specific simplifications:
 *
 *   1. No interactive shell / charger-event GPIO — we infer vbus_connected
 *      from the charger sensor's VBUS_STATUS channel each update tick
 *      instead of subscribing to mfd_npm13xx events. Saves the GPIO + IRQ
 *      callback infrastructure for a non-interactive fielded device that
 *      doesn't care about millisecond-latency vbus state changes.
 *
 *   2. No TTE/TTF reporting — heartbeat schema doesn't carry those, and
 *      they're not actionable from the cloud side. SoC alone is enough.
 *
 *   3. Public getter returns cached snapshot under a mutex so the cloud
 *      worker (heartbeat publish) doesn't block on a charger I²C round-trip
 *      every time it builds a payload.
 *
 * Battery model: src/battery_model.inc (verbatim from the upstream sample's
 * "Example" 1100 mAh LiPol). v1.5 should swap in an LP803448-tuned model
 * once we have field discharge curves.
 */

#include "battery.h"

#include <stdlib.h>
#include <stdbool.h>

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/drivers/sensor/npm13xx_charger.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

#include <nrf_fuel_gauge.h>

LOG_MODULE_REGISTER(gs_battery, LOG_LEVEL_INF);

/* nPM1300 CHARGER.BCHGCHARGESTATUS register bitmasks (matches sample). */
#define NPM13XX_CHG_STATUS_COMPLETE_MASK BIT(1)
#define NPM13XX_CHG_STATUS_TRICKLE_MASK  BIT(2)
#define NPM13XX_CHG_STATUS_CC_MASK       BIT(3)
#define NPM13XX_CHG_STATUS_CV_MASK       BIT(4)

#define BATTERY_NODE  DT_NODELABEL(npm1300_charger)
#if !DT_NODE_HAS_STATUS(BATTERY_NODE, okay)
#error "npm1300_charger devicetree node not enabled — verify board overlay"
#endif

static const struct device *const s_charger = DEVICE_DT_GET(BATTERY_NODE);

/* Update cadence. The fuel gauge lib accepts arbitrary `delta` between calls;
 * 5 s gives reasonable SoC tracking without hammering the I²C bus. The
 * heartbeat publishes hourly post-M12.1c.2, so we get ~720 fuel-gauge ticks
 * between heartbeats — plenty of resolution. */
#define UPDATE_PERIOD K_SECONDS(5)

static atomic_t s_initialized = ATOMIC_INIT(0);
static atomic_t s_first_reading_landed = ATOMIC_INIT(0);

/* Cached snapshot, protected by s_lock. Reader path
 * (gosteady_battery_get) is fast — no I²C, no fuel-gauge lib calls. */
static K_MUTEX_DEFINE(s_lock);
static uint16_t s_cached_mv = 0;
static float    s_cached_pct = 0.5f;  /* placeholder until first real reading */

static int64_t  s_ref_time;
static int32_t  s_chg_status_prev;

static const struct battery_model s_battery_model = {
#include "battery_model.inc"
};

static int read_sensors(float *voltage, float *current, float *temp,
			 int32_t *chg_status, bool *vbus_connected)
{
	struct sensor_value v;
	int ret;

	ret = sensor_sample_fetch(s_charger);
	if (ret < 0) { return ret; }

	sensor_channel_get(s_charger, SENSOR_CHAN_GAUGE_VOLTAGE, &v);
	*voltage = (float)v.val1 + ((float)v.val2 / 1000000.0f);

	sensor_channel_get(s_charger, SENSOR_CHAN_GAUGE_TEMP, &v);
	*temp = (float)v.val1 + ((float)v.val2 / 1000000.0f);

	sensor_channel_get(s_charger, SENSOR_CHAN_GAUGE_AVG_CURRENT, &v);
	*current = (float)v.val1 + ((float)v.val2 / 1000000.0f);

	sensor_channel_get(s_charger, SENSOR_CHAN_NPM13XX_CHARGER_STATUS, &v);
	*chg_status = v.val1;

	/* VBUS_STATUS channel returns 1 when USB is plugged. We use it instead of
	 * the upstream sample's mfd_npm13xx GPIO callback path — no need for
	 * sub-second latency on this signal in field operation. */
	sensor_channel_get(s_charger, SENSOR_CHAN_NPM13XX_CHARGER_VBUS_STATUS, &v);
	*vbus_connected = (v.val1 != 0);

	return 0;
}

static int charge_status_inform(int32_t chg_status)
{
	union nrf_fuel_gauge_ext_state_info_data state_info;

	if (chg_status & NPM13XX_CHG_STATUS_COMPLETE_MASK) {
		LOG_INF("charge complete");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_COMPLETE;
	} else if (chg_status & NPM13XX_CHG_STATUS_TRICKLE_MASK) {
		LOG_INF("trickle charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_TRICKLE;
	} else if (chg_status & NPM13XX_CHG_STATUS_CC_MASK) {
		LOG_INF("constant-current charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CC;
	} else if (chg_status & NPM13XX_CHG_STATUS_CV_MASK) {
		LOG_INF("constant-voltage charging");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_CV;
	} else {
		LOG_DBG("charger idle");
		state_info.charge_state = NRF_FUEL_GAUGE_CHARGE_STATE_IDLE;
	}

	return nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_STATE_CHANGE, &state_info);
}

static int fuel_gauge_init_locked(void)
{
	struct sensor_value value;
	struct nrf_fuel_gauge_init_parameters parameters = {
		.model = &s_battery_model,
		.opt_params = NULL,
		.state = NULL,
	};
	float v0, i0, t0, max_charge_current, term_charge_current;
	bool vbus = false;
	int32_t chg_status;
	int ret;

	LOG_INF("nrf_fuel_gauge version: %s", nrf_fuel_gauge_version);

	ret = read_sensors(&v0, &i0, &t0, &chg_status, &vbus);
	if (ret < 0) {
		LOG_ERR("initial charger read failed (%d)", ret);
		return ret;
	}
	parameters.v0 = v0;
	parameters.t0 = t0;
	/* Zephyr sensor convention: gauge current is negative=discharging.
	 * nrf_fuel_gauge expects opposite sign convention. */
	parameters.i0 = -i0;

	sensor_channel_get(s_charger, SENSOR_CHAN_GAUGE_DESIRED_CHARGING_CURRENT, &value);
	max_charge_current = (float)value.val1 + ((float)value.val2 / 1000000.0f);
	term_charge_current = max_charge_current / 10.0f;

	ret = nrf_fuel_gauge_init(&parameters, NULL);
	if (ret < 0) {
		LOG_ERR("nrf_fuel_gauge_init failed (%d)", ret);
		return ret;
	}

	ret = nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_CHARGE_CURRENT_LIMIT,
		&(union nrf_fuel_gauge_ext_state_info_data){
			.charge_current_limit = max_charge_current});
	if (ret < 0) { return ret; }

	ret = nrf_fuel_gauge_ext_state_update(
		NRF_FUEL_GAUGE_EXT_STATE_INFO_TERM_CURRENT,
		&(union nrf_fuel_gauge_ext_state_info_data){
			.charge_term_current = term_charge_current});
	if (ret < 0) { return ret; }

	(void)charge_status_inform(chg_status);
	s_chg_status_prev = chg_status;
	s_ref_time = k_uptime_get();

	LOG_INF("fuel gauge initialized: v0=%.3f V, t0=%.1f °C, i0=%.3f A, max_chg=%.3f A",
		(double)v0, (double)t0, (double)-parameters.i0, (double)max_charge_current);
	return 0;
}

/* Run one fuel-gauge update tick. Called from the worker thread; safe to
 * read+write s_cached_* under s_lock. */
static int fuel_gauge_update_locked(void)
{
	float voltage, current, temp, soc;
	int32_t chg_status;
	bool vbus = false;
	int ret;

	ret = read_sensors(&voltage, &current, &temp, &chg_status, &vbus);
	if (ret < 0) {
		return ret;
	}

	(void)nrf_fuel_gauge_ext_state_update(
		vbus ? NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_CONNECTED
		     : NRF_FUEL_GAUGE_EXT_STATE_INFO_VBUS_DISCONNECTED, NULL);

	if (chg_status != s_chg_status_prev) {
		s_chg_status_prev = chg_status;
		(void)charge_status_inform(chg_status);
	}

	float delta = (float)k_uptime_delta(&s_ref_time) / 1000.0f;
	soc = nrf_fuel_gauge_process(voltage, -current, temp, delta, NULL);

	/* SoC clamp: lib can return mildly out-of-range values during
	 * settling. Cloud schema requires 0.0–1.0. */
	if (soc < 0.0f) { soc = 0.0f; }
	if (soc > 100.0f) { soc = 100.0f; }

	s_cached_mv  = (uint16_t)(voltage * 1000.0f + 0.5f);
	s_cached_pct = soc / 100.0f;

	atomic_set(&s_first_reading_landed, 1);
	return 0;
}

static void worker_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	while (1) {
		k_mutex_lock(&s_lock, K_FOREVER);
		int ret = fuel_gauge_update_locked();
		uint16_t mv  = s_cached_mv;
		float    pct = s_cached_pct;
		k_mutex_unlock(&s_lock);

		if (ret < 0) {
			LOG_WRN("fuel-gauge update failed (%d)", ret);
		} else {
			LOG_DBG("battery: %u mV, SoC %.1f%%",
				(unsigned)mv, (double)(pct * 100.0f));
		}
		k_sleep(UPDATE_PERIOD);
	}
}

#define BATTERY_THREAD_STACK 2048
#define BATTERY_THREAD_PRIO  8
K_THREAD_STACK_DEFINE(s_battery_stack, BATTERY_THREAD_STACK);
static struct k_thread s_battery_thread;

int gosteady_battery_init(void)
{
	if (atomic_set(&s_initialized, 1) == 1) {
		return 0;
	}

	if (!device_is_ready(s_charger)) {
		LOG_ERR("npm1300 charger device not ready");
		atomic_set(&s_initialized, 0);
		return -ENODEV;
	}

	k_mutex_lock(&s_lock, K_FOREVER);
	int ret = fuel_gauge_init_locked();
	k_mutex_unlock(&s_lock);
	if (ret < 0) {
		atomic_set(&s_initialized, 0);
		return ret;
	}

	(void)k_thread_create(&s_battery_thread, s_battery_stack,
			      BATTERY_THREAD_STACK,
			      worker_thread_fn, NULL, NULL, NULL,
			      BATTERY_THREAD_PRIO, 0, K_NO_WAIT);
	k_thread_name_set(&s_battery_thread, "gs_battery");

	LOG_INF("battery init OK; worker thread spawned");
	return 0;
}

int gosteady_battery_get(uint16_t *out_mv, float *out_pct)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	if (out_mv)  { *out_mv  = s_cached_mv; }
	if (out_pct) { *out_pct = s_cached_pct; }
	k_mutex_unlock(&s_lock);

	return atomic_get(&s_first_reading_landed) ? 0 : -EAGAIN;
}
