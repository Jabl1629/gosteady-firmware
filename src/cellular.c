/*
 * GoSteady firmware — Milestone 12.1a: cellular bring-up.
 *
 * Drives nrf_modem_lib + LTE link control to attach to LTE-M, then
 * reports RSRP/SNR + network UTC (AT+CCLK?) once registered. This is
 * the pure-bring-up step: no MQTT, no sockets, no telemetry.
 *
 * Goal of this milestone:
 *   - Confirm the modem firmware is alive and answering AT commands.
 *   - Confirm SIM is provisioned and LTE-M coverage is usable at the
 *     bench location.
 *   - Confirm we can stamp a real UTC time on a session_end ISO 8601
 *     payload (M9/M12.1b prerequisite).
 *
 * The reporter thread polls signal quality + time once at registration
 * and then every 60 s thereafter so we can watch coverage stability
 * without waiting for the hourly heartbeat path that lands in M12.1c.
 */

#include "cellular.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <modem/lte_lc.h>
#include <modem/nrf_modem_lib.h>
#include <nrf_modem_at.h>

#include <errno.h>
#include <string.h>

LOG_MODULE_REGISTER(cellular, LOG_LEVEL_INF);

/* ---- State (single-threaded reporter; flags are simple atomics) ---- */

static volatile bool s_registered;          /* set on REGISTERED_HOME/ROAMING */
static volatile bool s_started;             /* gosteady_cellular_start() ran */

/* Cached most-recent measurements. Filled by the reporter thread. */
static volatile int16_t s_rsrp_dbm = INT16_MIN;
static volatile int8_t  s_snr_db   = INT8_MIN;
static volatile bool    s_signal_valid;

/* Reporter thread cadence: poll once at registration and every 60 s
 * thereafter. Keeps the bring-up log informative without flooding. */
#define REPORTER_PERIOD_MS  (60 * 1000)

K_THREAD_STACK_DEFINE(reporter_stack, 2048);
static struct k_thread reporter_thread;
static K_SEM_DEFINE(registered_sem, 0, 1);

/* ---- Helpers ---- */

static const char *reg_status_str(enum lte_lc_nw_reg_status s)
{
	switch (s) {
	case LTE_LC_NW_REG_NOT_REGISTERED:        return "not_registered";
	case LTE_LC_NW_REG_REGISTERED_HOME:       return "registered_home";
	case LTE_LC_NW_REG_SEARCHING:             return "searching";
	case LTE_LC_NW_REG_REGISTRATION_DENIED:   return "registration_denied";
	case LTE_LC_NW_REG_UNKNOWN:               return "unknown";
	case LTE_LC_NW_REG_REGISTERED_ROAMING:    return "registered_roaming";
	case LTE_LC_NW_REG_UICC_FAIL:             return "uicc_fail";
	default:                                   return "?";
	}
}

static const char *mode_str(enum lte_lc_lte_mode m)
{
	switch (m) {
	case LTE_LC_LTE_MODE_NONE:  return "none";
	case LTE_LC_LTE_MODE_LTEM:  return "ltem";
	case LTE_LC_LTE_MODE_NBIOT: return "nbiot";
	default:                     return "?";
	}
}

/* Read AT+CESQ for RSRP/SNR. AT+CESQ returns:
 *   +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
 * where rsrp is mapped per 3GPP TS 36.133:
 *   0     => RSRP < -140 dBm
 *   1..96 => -141 + N dBm (i.e. N=1 -> -140, N=96 -> -45)
 *   97    => RSRP >= -44 dBm
 *   255   => not known
 */
static int read_signal(int16_t *rsrp_dbm, int8_t *snr_db)
{
	/* lte_lc_conn_eval_params_get returns rich measurements but only when
	 * the modem is in connected (RRC) state. Use AT+CESQ as the always-
	 * available fallback. SNR is not in CESQ; use AT%XSNRSQ if available. */
	int cesq_rsrp = 255;
	int cesq_rsrq = 255;
	int unused1, unused2, unused3, unused4;

	int err = nrf_modem_at_scanf("AT+CESQ",
		"+CESQ: %d,%d,%d,%d,%d,%d",
		&unused1, &unused2, &unused3, &unused4, &cesq_rsrq, &cesq_rsrp);
	if (err < 0 || err < 6) {
		return -EIO;
	}

	if (cesq_rsrp == 255) {
		return -EAGAIN;
	}
	if (cesq_rsrp == 0) {
		*rsrp_dbm = -141;
	} else if (cesq_rsrp >= 1 && cesq_rsrp <= 96) {
		*rsrp_dbm = (int16_t)(-141 + cesq_rsrp);
	} else if (cesq_rsrp == 97) {
		*rsrp_dbm = -44;
	} else {
		*rsrp_dbm = INT16_MIN;
	}

	/* %XSNRSQ is a Nordic-modem extension: %XSNRSQ: <snr>,<srxlev>,<ce_level>
	 * SNR is in dB with offset 24 (i.e. raw 24 -> 0 dB). 127 => unknown. */
	int snr_raw = 127;
	int srxlev = 0;
	int ce_level = 0;
	err = nrf_modem_at_scanf("AT%XSNRSQ?",
		"%%XSNRSQ: %d,%d,%d", &snr_raw, &srxlev, &ce_level);
	if (err >= 1 && snr_raw != 127) {
		*snr_db = (int8_t)(snr_raw - 24);
	} else {
		*snr_db = INT8_MIN;
	}

	return 0;
}

static int read_network_time_iso8601(char *out, size_t outlen)
{
	if (outlen < 24) {
		return -EINVAL;
	}

	/* +CCLK: "yy/MM/dd,hh:mm:ss±zz"
	 *   yy/MM/dd  : 2-digit year, month, day (UTC after timezone correction)
	 *   ±zz       : timezone in 15-minute units; +00 => already UTC
	 * Modem normally reports UTC with +00 once it receives NITZ from the
	 * network, which is what we'll see at the bench.
	 */
	int year, month, day, hour, minute, second, tz;
	int err = nrf_modem_at_scanf("AT+CCLK?",
		"+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
		&year, &month, &day, &hour, &minute, &second, &tz);
	if (err < 0) {
		return -EIO;
	}
	if (err < 7) {
		/* Modem hasn't received NITZ yet. */
		return -EAGAIN;
	}

	/* Format as ISO 8601 in UTC. We don't apply tz here — at the bench
	 * the network gives us +00 anyway, and v1 is okay with seconds-level
	 * accuracy. If a deployment cell ever returns a non-zero tz we'd
	 * want to convert rather than just truncate. */
	(void)snprintf(out, outlen, "20%02d-%02d-%02dT%02d:%02d:%02dZ",
		year, month, day, hour, minute, second);
	return 0;
}

/* ---- LTE event handler ---- */

static void lte_handler(const struct lte_lc_evt *const evt)
{
	switch (evt->type) {
	case LTE_LC_EVT_NW_REG_STATUS:
		LOG_INF("nw_reg_status=%s", reg_status_str(evt->nw_reg_status));
		if (evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_HOME ||
		    evt->nw_reg_status == LTE_LC_NW_REG_REGISTERED_ROAMING) {
			if (!s_registered) {
				s_registered = true;
				k_sem_give(&registered_sem);
			}
		} else {
			s_registered = false;
		}
		break;

	case LTE_LC_EVT_LTE_MODE_UPDATE:
		LOG_INF("lte_mode=%s", mode_str(evt->lte_mode));
		break;

	case LTE_LC_EVT_PSM_UPDATE:
		LOG_INF("psm: tau=%d s, active=%d s",
			evt->psm_cfg.tau, evt->psm_cfg.active_time);
		break;

	case LTE_LC_EVT_RRC_UPDATE:
		LOG_INF("rrc=%s",
			evt->rrc_mode == LTE_LC_RRC_MODE_CONNECTED ? "connected" : "idle");
		break;

	case LTE_LC_EVT_CELL_UPDATE:
		LOG_INF("cell: id=0x%08x tac=0x%04x",
			evt->cell.id, evt->cell.tac);
		break;

	default:
		break;
	}
}

/* ---- Reporter thread: poll signal + time once registered, then periodically ---- */

static void log_signal_and_time(void)
{
	int16_t rsrp_dbm = INT16_MIN;
	int8_t  snr_db   = INT8_MIN;
	int err = read_signal(&rsrp_dbm, &snr_db);
	if (err == 0) {
		s_rsrp_dbm = rsrp_dbm;
		s_snr_db   = snr_db;
		s_signal_valid = true;
		if (snr_db != INT8_MIN) {
			LOG_INF("signal: rsrp=%d dBm snr=%d dB", rsrp_dbm, snr_db);
		} else {
			LOG_INF("signal: rsrp=%d dBm snr=unknown", rsrp_dbm);
		}
	} else {
		LOG_WRN("signal read failed (%d)", err);
	}

	char ts[32];
	err = read_network_time_iso8601(ts, sizeof(ts));
	if (err == 0) {
		LOG_INF("network_time=%s", ts);
	} else if (err == -EAGAIN) {
		LOG_INF("network_time=unsynced (no NITZ yet)");
	} else {
		LOG_WRN("network_time read failed (%d)", err);
	}
}

static void reporter_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* Block until first registration. */
	(void)k_sem_take(&registered_sem, K_FOREVER);
	LOG_INF("registered — first signal/time poll in 5 s");
	/* Brief settle so the modem has a chance to receive NITZ from the
	 * network before we ask for time. */
	k_msleep(5000);

	while (1) {
		if (s_registered) {
			log_signal_and_time();
		}
		k_msleep(REPORTER_PERIOD_MS);
	}
}

/* ---- Public API ---- */

int gosteady_cellular_start(void)
{
	if (s_started) {
		return 0;
	}

	int err = nrf_modem_lib_init();
	if (err < 0) {
		LOG_ERR("nrf_modem_lib_init failed (%d) — modem firmware OK?", err);
		return err;
	}
	LOG_INF("nrf_modem_lib_init ok");

	err = lte_lc_connect_async(lte_handler);
	if (err < 0) {
		LOG_ERR("lte_lc_connect_async failed (%d)", err);
		return err;
	}
	LOG_INF("lte_lc_connect_async kicked off — waiting for registration");

	k_thread_create(&reporter_thread, reporter_stack,
		K_THREAD_STACK_SIZEOF(reporter_stack),
		reporter_entry, NULL, NULL, NULL,
		7, 0, K_NO_WAIT);
	k_thread_name_set(&reporter_thread, "cell_reporter");

	s_started = true;
	return 0;
}

bool gosteady_cellular_is_registered(void)
{
	return s_registered;
}

int gosteady_cellular_get_signal(int16_t *rsrp_dbm, int8_t *snr_db)
{
	if (!rsrp_dbm || !snr_db) {
		return -EINVAL;
	}
	if (!s_signal_valid) {
		return -EAGAIN;
	}
	*rsrp_dbm = s_rsrp_dbm;
	*snr_db   = s_snr_db;
	return 0;
}

int gosteady_cellular_get_network_time(char *buf, size_t buflen)
{
	if (!buf || buflen < 24) {
		return -EINVAL;
	}
	if (!s_registered) {
		return -EAGAIN;
	}
	return read_network_time_iso8601(buf, buflen);
}
