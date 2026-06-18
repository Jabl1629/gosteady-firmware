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
#include "gs_time.h"

#include <stdio.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <date_time.h>
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
 * thereafter. Keeps the bring-up log informative without flooding.
 *
 * Pilot low-power (CONFIG_GOSTEADY_LOW_POWER): relax to 30 min. The 60 s
 * poll is bench diagnostics that also wakes the app core (and pokes the
 * modem AT interface) every minute, which works against PSM sleep between
 * the hourly heartbeats. The heartbeat publishes the last cached RSRP/SNR,
 * which for a (stationary) fielded cap is fine at 30-min freshness — the
 * first post-registration poll still primes a valid reading before the
 * first heartbeat. */
#if defined(CONFIG_GOSTEADY_LOW_POWER)
#define REPORTER_PERIOD_MS  (30 * 60 * 1000)
#else
#define REPORTER_PERIOD_MS  (60 * 1000)
#endif

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

/* ---- Absolute time via NCS date_time library (0.17.0-time) ----
 *
 * Replaces the bespoke AT+CCLK? read (and the §C11.5 bounded-timeout AT
 * wrapper that guarded it) with the NCS `date_time` lib, which sources UTC
 * in priority NITZ -> NTP -> app-set and maintains it against the monotonic
 * clock. Two properties drive the rewrite:
 *
 *   1. date_time's modem source is the %XTIME *NITZ push* notification (via
 *      AT_MONITOR), NOT a poll of the free-running modem RTC. So on a roaming
 *      SIM that broadcasts no NITZ it never emits the 1980/2080 default — it
 *      falls through to NTP. This is the actual fix for the Jun 14-16 2080
 *      incident (coord §C47).
 *   2. date_time_now() / date_time_uptime_to_unix_time_ms() are cached RAM
 *      reads — no modem round-trip — so the session-start hot path no longer
 *      needs the bounded-timeout AT wrapper at all. Deleting that apparatus
 *      (two sems + mutex + seq atomics + the reporter-thread dispatch loop)
 *      both reclaims RAM to offset date_time's ~1.3 KB thread + SNTP socket
 *      and *eliminates* (not just bounds) the §C11.5 AT-serialization lockup
 *      for the time path.
 *
 * read_signal() (AT+CESQ / AT%XSNRSQ) still uses the bare synchronous
 * nrf_modem_at_scanf — it runs on the reporter thread, never in session_start.
 */

enum gs_time_src { GS_TIME_SRC_UNSYNCED = 0, GS_TIME_SRC_NITZ, GS_TIME_SRC_NTP };
static volatile enum gs_time_src s_time_source = GS_TIME_SRC_UNSYNCED;

static void date_time_evt_handler(const struct date_time_evt *evt)
{
	switch (evt->type) {
	case DATE_TIME_OBTAINED_MODEM:
		s_time_source = GS_TIME_SRC_NITZ;
		LOG_INF("date_time: obtained from modem (NITZ)");
		break;
	case DATE_TIME_OBTAINED_NTP:
		s_time_source = GS_TIME_SRC_NTP;
		LOG_INF("date_time: obtained from NTP");
		break;
	case DATE_TIME_OBTAINED_EXT:
		LOG_INF("date_time: obtained from external source");
		break;
	case DATE_TIME_NOT_OBTAINED:
		LOG_WRN("date_time: update did not obtain a valid time");
		break;
	default:
		break;
	}
}

/* Read AT+CESQ for RSRP/SNR. AT+CESQ returns:
 *   +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
 * where rsrp is mapped per 3GPP TS 36.133:
 *   0     => RSRP < -140 dBm
 *   1..96 => -141 + N dBm (i.e. N=1 -> -140, N=96 -> -45)
 *   97    => RSRP >= -44 dBm
 *   255   => not known
 *
 * Note: read_signal runs on the cellular reporter thread, not in
 * session_start's critical path, so the synchronous bare
 * nrf_modem_at_scanf here is acceptable. Absolute time no longer touches
 * the modem at all (it comes from the date_time lib cache — see the time
 * accessors below).
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

	/* Informational time log from the date_time cache (no modem round-trip). */
	int64_t now_ms = 0;
	if (gosteady_cellular_get_network_time_unix_ms(&now_ms) == 0) {
		char ts[32];
		if (gosteady_cellular_format_unix_ms_iso8601(now_ms, ts,
							      sizeof(ts)) == 0) {
			LOG_INF("network_time=%s (src=%s)", ts,
				gosteady_cellular_time_source());
		}
	} else {
		LOG_INF("network_time=unsynced (no NITZ/NTP yet)");
	}
}

static void reporter_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* Block until first registration. */
	(void)k_sem_take(&registered_sem, K_FOREVER);

	/* Settle briefly for the modem %XTIME (NITZ) push, then explicitly kick
	 * a date_time update. DATE_TIME_AUTO_UPDATE also fires on the LTE link
	 * event, but the explicit kick guarantees an NTP attempt now that the
	 * data link is up — this is what saves the no-NITZ roaming case (the
	 * %XTIME notification may never arrive). Runs on the date_time lib's own
	 * thread; results arrive via date_time_evt_handler. */
	LOG_INF("registered — settling 5 s, then date_time update + first signal poll");
	k_sleep(K_MSEC(5000));
	int derr = date_time_update_async(date_time_evt_handler);
	if (derr) {
		LOG_WRN("date_time_update_async kick failed (%d) — auto-update still active",
			derr);
	}

	/* Periodic signal+time poll for the bring-up/diagnostics log. No AT-cmd
	 * dispatch any more — absolute time comes from the date_time cache and
	 * read_signal's AT+CESQ is a bare synchronous call on this thread. */
	while (1) {
		if (s_registered) {
			log_signal_and_time();
		}
		k_sleep(K_MSEC(REPORTER_PERIOD_MS));
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

	/* 0.17.0-time: register the date_time handler so we learn which source
	 * (NITZ/NTP) supplied the clock. DATE_TIME_AUTO_UPDATE drives the first
	 * update when LTE attaches; the reporter thread also kicks an explicit
	 * update once registered (see reporter_entry). */
	date_time_register_handler(date_time_evt_handler);

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

int gosteady_cellular_get_network_time_unix_ms(int64_t *out_ms)
{
	if (!out_ms) {
		return -EINVAL;
	}

	int64_t now_ms = 0;
	/* Cached read from the date_time lib — no modem round-trip. -ENODATA
	 * when no valid time has been obtained yet. */
	if (date_time_now(&now_ms) != 0) {
		return -EAGAIN;
	}
	/* §4.1 hard floor: reject anything outside [2024,2050]. date_time
	 * shouldn't ever produce such a value (its modem source is the NITZ
	 * push, not the 1980 RTC), but this guarantees the no-2080-on-wire
	 * invariant regardless of a bogus NITZ/NTP response. */
	if (!gs_time_year_is_plausible(now_ms)) {
		LOG_WRN("date_time returned implausible time (%lld ms) — treating as unsynced",
			(long long)now_ms);
		return -EAGAIN;
	}
	*out_ms = now_ms;
	return 0;
}

int gosteady_cellular_get_network_time(char *buf, size_t buflen)
{
	if (!buf || buflen < 24) {
		return -EINVAL;
	}
	int64_t now_ms = 0;
	int err = gosteady_cellular_get_network_time_unix_ms(&now_ms);
	if (err) {
		return err;   /* -EAGAIN if unsynced/implausible */
	}
	return gosteady_cellular_format_unix_ms_iso8601(now_ms, buf, buflen);
}

bool gosteady_cellular_clock_is_synced(void)
{
	int64_t now_ms = 0;
	if (date_time_now(&now_ms) != 0) {
		return false;
	}
	return gs_time_year_is_plausible(now_ms);
}

int gosteady_cellular_uptime_to_unix_ms(uint32_t uptime_ms, int64_t *out_ms)
{
	if (!out_ms) {
		return -EINVAL;
	}
	/* date_time_uptime_to_unix_time_ms() converts in place: pass the stored
	 * k_uptime value in, receive absolute Unix-ms out, anchored to the
	 * library's current sync (NTP when NITZ is absent). This is the §4.2
	 * retro-stamp. -ENODATA when the clock isn't synced. */
	int64_t v = (int64_t)uptime_ms;
	if (date_time_uptime_to_unix_time_ms(&v) != 0) {
		return -EAGAIN;
	}
	if (!gs_time_year_is_plausible(v)) {
		return -EAGAIN;
	}
	*out_ms = v;
	return 0;
}

const char *gosteady_cellular_time_source(void)
{
	switch (s_time_source) {
	case GS_TIME_SRC_NITZ: return "nitz";
	case GS_TIME_SRC_NTP:  return "ntp";
	default:               return "unsynced";
	}
}

int gosteady_cellular_format_unix_ms_iso8601(int64_t unix_ms,
					      char *out, size_t out_sz)
{
	/* Pure formatter — does not touch the modem. Used by cloud.c's
	 * activity worker (FMEA 1.1, 1.2) to retro-stamp session_start /
	 * session_end ISO strings from uptime deltas captured at
	 * session_start/stop, when cellular UTC was unavailable at capture
	 * time but is available at publish time. */
	if (!out || out_sz < 24 || unix_ms < 0) {
		return -EINVAL;
	}
	time_t epoch_s = (time_t)(unix_ms / 1000);
	struct tm tm;
	if (gmtime_r(&epoch_s, &tm) == NULL) {
		return -EIO;
	}
	int n = snprintf(out, out_sz, "%04d-%02d-%02dT%02d:%02d:%02dZ",
			 tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
			 tm.tm_hour, tm.tm_min, tm.tm_sec);
	if (n < 0 || (size_t)n >= out_sz) {
		return -ENOMEM;
	}
	return 0;
}
