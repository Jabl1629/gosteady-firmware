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

#include <stdio.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/timeutil.h>

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

/* ---- Bounded-timeout AT command wrapper (firmware-coord §C11.5) ----
 *
 * Background:
 *   Under sustained modem contention (e.g. an exhausted SIM driving a
 *   tight PDN-reject reattach loop), `nrf_modem_at_scanf` / `_at_cmd`
 *   block synchronously past the 60 s WDT envelope, killing the caller
 *   thread. The 2026-05-11/12 conference produced this failure mode
 *   end-to-end (1 watchdog + 1 fatal + 3 reboots; coord §C11.2/§C11.4).
 *   §C10.5 had predicted it six days earlier as the "M14.5 watch item".
 *
 * Design:
 *   A dedicated worker thread runs the (blocking) `nrf_modem_at_cmd`.
 *   Callers post the command via `at_request_sem` and wait for the
 *   response on `at_response_sem` with a bounded timeout. If the
 *   timeout fires, the caller returns `-ETIMEDOUT`; the worker may
 *   continue blocking until the modem eventually responds, but the
 *   caller's thread (often main.c's auto-start coordinator) is freed.
 *
 *   `session_start`'s callers translate `-ETIMEDOUT` to `-EAGAIN` so
 *   the existing FMEA 1.1 retro-stamp path picks up cleanly — session
 *   opens with `start_utc=unavailable`, the timestamp is filled in at
 *   `session_stop` (or by the activity worker's republish path).
 *
 * Race handling:
 *   A monotonic request sequence number is stamped per call. The
 *   worker writes the sequence back into `at_response_seq` after the
 *   AT call returns. Callers compare on wake: if the sequence doesn't
 *   match, they got a stale response from a prior timed-out caller and
 *   return `-ETIMEDOUT` themselves. The caller-side mutex serializes
 *   wrapper invocations; the `at_request_sem` has max=1 so concurrent
 *   `give()` calls collapse safely (the worker will service the most-
 *   recent in-flight cmd, and stale responses are filtered by seq).
 *
 * Timeout choice:
 *   2000 ms — far below the 60 s WDT envelope, comfortably above the
 *   typical AT round-trip time (<100 ms per §C10.5 bench observation).
 */

#define AT_DEFAULT_TIMEOUT_MS     2000
#define AT_RESPONSE_BUF_SIZE      128

/* Dispatch slots — the at worker runs INSIDE the reporter thread to avoid
 * adding a second 2 KB stack (RAM was already at 99.76% pre-patch per
 * coord §C10.6). The reporter's main loop polls at_request_sem with a
 * timeout: on sem fire, run the AT cmd; on timeout, do the periodic
 * signal+time poll. AT cmds get priority — periodic poll runs whenever
 * the reporter is otherwise idle. */
static K_SEM_DEFINE(at_request_sem, 0, 1);
static K_SEM_DEFINE(at_response_sem, 0, 1);
static K_MUTEX_DEFINE(at_caller_mutex);

static atomic_t at_request_seq  = ATOMIC_INIT(0);
static atomic_t at_response_seq = ATOMIC_INIT(0);

static const char *at_pending_cmd;
static int         at_pending_result;
static char        at_pending_response[AT_RESPONSE_BUF_SIZE];

/* Called by reporter_entry when at_request_sem fires. Runs in the reporter
 * thread context (its 2 KB stack is plenty for nrf_modem_at_cmd). */
static void at_worker_service_one(void)
{
	atomic_val_t my_seq = atomic_get(&at_request_seq);
	int64_t t0 = k_uptime_get();
	at_pending_result = nrf_modem_at_cmd(at_pending_response,
					      sizeof(at_pending_response),
					      "%s", at_pending_cmd);
	int64_t elapsed = k_uptime_get() - t0;
	atomic_set(&at_response_seq, my_seq);
	if (elapsed > AT_DEFAULT_TIMEOUT_MS) {
		LOG_WRN("at cmd '%s' took %lld ms (>timeout); caller may have given up",
			at_pending_cmd, elapsed);
	}
	k_sem_give(&at_response_sem);
}

/* Synchronous AT command with bounded caller-side wait.
 * Returns nrf_modem_at_cmd's result on success; -ETIMEDOUT if the modem
 * doesn't respond within timeout_ms; -EINVAL on bad args.
 */
static int at_cmd_with_timeout(const char *cmd, char *out, size_t outlen,
			       int timeout_ms)
{
	if (!cmd || !out || outlen == 0) {
		return -EINVAL;
	}

	k_mutex_lock(&at_caller_mutex, K_FOREVER);

	atomic_val_t my_seq = atomic_inc(&at_request_seq) + 1;
	at_pending_cmd = cmd;
	k_sem_reset(&at_response_sem);   /* drop any stale give from a prior caller */
	k_sem_give(&at_request_sem);     /* wake worker */

	int err = k_sem_take(&at_response_sem, K_MSEC(timeout_ms));
	if (err == -EAGAIN) {
		LOG_WRN("at cmd timed out after %d ms (modem contention?): %s",
			timeout_ms, cmd);
		k_mutex_unlock(&at_caller_mutex);
		return -ETIMEDOUT;
	}

	if (atomic_get(&at_response_seq) != my_seq) {
		LOG_WRN("at cmd: stale response (got seq %ld, wanted %ld); treating as timeout: %s",
			(long)atomic_get(&at_response_seq), (long)my_seq, cmd);
		k_mutex_unlock(&at_caller_mutex);
		return -ETIMEDOUT;
	}

	int rc = at_pending_result;
	if (rc >= 0) {
		strncpy(out, at_pending_response, outlen - 1);
		out[outlen - 1] = '\0';
	}
	k_mutex_unlock(&at_caller_mutex);
	return rc;
}

/* Read AT+CESQ for RSRP/SNR. AT+CESQ returns:
 *   +CESQ: <rxlev>,<ber>,<rscp>,<ecno>,<rsrq>,<rsrp>
 * where rsrp is mapped per 3GPP TS 36.133:
 *   0     => RSRP < -140 dBm
 *   1..96 => -141 + N dBm (i.e. N=1 -> -140, N=96 -> -45)
 *   97    => RSRP >= -44 dBm
 *   255   => not known
 *
 * Note: read_signal runs on the cellular reporter thread (line ~240),
 * not in session_start's critical path, so the synchronous bare
 * nrf_modem_at_scanf here is acceptable. session_start's CCLK calls
 * use at_cmd_with_timeout() — see read_network_time_iso8601() and
 * gosteady_cellular_get_network_time_unix_ms() below.
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

/* Bare AT+CCLK? read — no wrapper, used by the reporter thread itself
 * (calling at_cmd_with_timeout from the reporter would self-deadlock:
 * the wrapper gives at_request_sem, which only the reporter consumes,
 * but the reporter is blocked waiting on at_response_sem). */
static int read_network_time_iso8601_bare(char *out, size_t outlen)
{
	if (outlen < 24) {
		return -EINVAL;
	}

	int year, month, day, hour, minute, second, tz;
	int err = nrf_modem_at_scanf("AT+CCLK?",
		"+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
		&year, &month, &day, &hour, &minute, &second, &tz);
	if (err < 0) {
		return -EIO;
	}
	if (err < 7) {
		return -EAGAIN;
	}

	(void)snprintf(out, outlen, "20%02d-%02d-%02dT%02d:%02d:%02dZ",
		year, month, day, hour, minute, second);
	return 0;
}

static int read_network_time_iso8601(char *out, size_t outlen)
{
	if (outlen < 24) {
		return -EINVAL;
	}

	/* §C11.5: bounded-timeout AT call. -ETIMEDOUT under modem contention
	 * becomes -EAGAIN to the caller (session.c treats -EAGAIN as
	 * "cellular UTC unavailable, FMEA 1.1 retro-stamp will re-attempt"). */
	char resp[64];
	int err = at_cmd_with_timeout("AT+CCLK?", resp, sizeof(resp),
				       AT_DEFAULT_TIMEOUT_MS);
	if (err == -ETIMEDOUT) {
		return -EAGAIN;
	}
	if (err < 0) {
		return -EIO;
	}

	/* +CCLK: "yy/MM/dd,hh:mm:ss±zz"
	 *   yy/MM/dd  : 2-digit year, month, day (UTC after timezone correction)
	 *   ±zz       : timezone in 15-minute units; +00 => already UTC
	 * Modem normally reports UTC with +00 once it receives NITZ from the
	 * network, which is what we'll see at the bench.
	 */
	int year, month, day, hour, minute, second, tz;
	int n = sscanf(resp, "+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
		       &year, &month, &day, &hour, &minute, &second, &tz);
	if (n < 7) {
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
	/* Reporter calls bare (no wrapper) — see read_network_time_iso8601_bare. */
	err = read_network_time_iso8601_bare(ts, sizeof(ts));
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

	LOG_INF("at_worker servicing on reporter thread (default timeout=%d ms)",
		AT_DEFAULT_TIMEOUT_MS);

	/* Block until first registration. Callers of at_cmd_with_timeout
	 * gate on s_registered, so no AT requests are expected before this
	 * point — and if any sneak in, they'll just time out (safe). */
	(void)k_sem_take(&registered_sem, K_FOREVER);
	LOG_INF("registered — first signal/time poll in 5 s "
		"(servicing AT requests during settle)");
	/* Settle period for NITZ arrival before our own time poll. Service
	 * AT requests during the wait — otherwise gs_cloud's immediate
	 * post-registration AT+CCLK? gets stuck for ~2.5 s and the
	 * at_cmd_with_timeout wrapper logs a spurious "modem contention?"
	 * warning even on healthy cellular. */
	int64_t settle_deadline = k_uptime_get() + 5000;
	while (k_uptime_get() < settle_deadline) {
		int wait_ms = (int)(settle_deadline - k_uptime_get());
		if (wait_ms <= 0) break;
		int err = k_sem_take(&at_request_sem, K_MSEC(wait_ms));
		if (err == 0) at_worker_service_one();
	}

	/* Main loop: interleave periodic signal+time poll with AT-cmd
	 * dispatch. Poll-due check happens at the TOP of every iteration so
	 * continuous AT request traffic (which gs_cloud generates during
	 * post-registration setup) cannot starve the periodic poll. Without
	 * this, log_signal_and_time() never runs, gs_cloud's signal-stats
	 * wait never succeeds, and the heartbeat thread gives up after 60 s
	 * with "cellular never became ready". */
	int64_t next_poll_ms = k_uptime_get();   /* fire poll immediately on entry */
	while (1) {
		int64_t now = k_uptime_get();

		/* (a) Fire periodic poll if due. */
		if (now >= next_poll_ms && s_registered) {
			log_signal_and_time();
			next_poll_ms = k_uptime_get() + REPORTER_PERIOD_MS;
			continue;
		}

		/* (b) Wait for either an AT request OR until the next poll
		 * is due, whichever fires first. */
		int wait_ms = (int)(next_poll_ms - now);
		if (wait_ms < 0) wait_ms = 0;
		int err = k_sem_take(&at_request_sem, K_MSEC(wait_ms));
		if (err == 0) {
			at_worker_service_one();
			/* loop back to (a) — may now be poll-due */
		}
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
	/* §C11.5: the reporter thread now also services bounded-timeout AT
	 * cmd dispatch from at_cmd_with_timeout(). One thread does both
	 * jobs because RAM was already at 99.76% pre-patch (coord §C10.6),
	 * so adding a second dedicated AT worker thread overflowed the
	 * region by ~2 KB. Folding into reporter saves the second 2 KB
	 * stack + k_thread struct. AT cmds are prioritized over the
	 * informational signal/time poll. */

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

int gosteady_cellular_get_network_time_unix_ms(int64_t *out_ms)
{
	if (!out_ms) {
		return -EINVAL;
	}
	if (!s_registered) {
		return -EAGAIN;
	}

	/* §C11.5: bounded-timeout AT call. -ETIMEDOUT → -EAGAIN so the
	 * caller's existing FMEA 1.1 retro-stamp path handles it. */
	char resp[64];
	int err = at_cmd_with_timeout("AT+CCLK?", resp, sizeof(resp),
				       AT_DEFAULT_TIMEOUT_MS);
	if (err == -ETIMEDOUT) {
		return -EAGAIN;
	}
	if (err < 0) {
		return -EIO;
	}

	int year, month, day, hour, minute, second, tz;
	int n = sscanf(resp, "+CCLK: \"%d/%d/%d,%d:%d:%d%d\"",
		       &year, &month, &day, &hour, &minute, &second, &tz);
	if (n < 7) {
		/* Modem hasn't received NITZ yet. */
		return -EAGAIN;
	}

	/* Convert via Zephyr's UTC-aware timegm helper. struct tm fields:
	 *   tm_year: years since 1900
	 *   tm_mon:  0..11
	 *   tm_mday: 1..31
	 *   year from AT+CCLK? is 2-digit (00..99) → 2000..2099. */
	struct tm tm = {
		.tm_year = (2000 + year) - 1900,
		.tm_mon  = month - 1,
		.tm_mday = day,
		.tm_hour = hour,
		.tm_min  = minute,
		.tm_sec  = second,
	};
	int64_t epoch_s = timeutil_timegm64(&tm);
	if (epoch_s == (int64_t)-1) {
		return -EIO;
	}
	*out_ms = epoch_s * 1000;
	return 0;
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
