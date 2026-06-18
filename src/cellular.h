/*
 * GoSteady firmware — Milestone 12.1a: cellular bring-up.
 *
 * Async LTE-M attach via lte_lc_connect_async + a small reporter that
 * logs registration state changes and RSRP/SNR once the modem is
 * registered. No MQTT, no sockets — those land in M12.1c.
 *
 * Absolute time (0.17.0-time): sourced from the NCS `date_time` library
 * (NITZ via the modem %XTIME push -> NTP/SNTP -> app-set) rather than a
 * bespoke AT+CCLK? read, with a §4.1 plausibility gate. See the accessors
 * below and src/gs_time.h.
 *
 * All logging goes to uart0 (the app console at /dev/cu.usbmodem*102),
 * so tools/log_console.py captures it across reboots.
 */

#ifndef GOSTEADY_CELLULAR_H_
#define GOSTEADY_CELLULAR_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the modem library (idempotent if already auto-init'd by
 * NCS) and start an async LTE-M attach. Returns 0 on successful kick-off,
 * negative errno on failure. Non-blocking: registration completes
 * asynchronously; subscribe via the LOG output or poll
 * gosteady_cellular_is_registered().
 */
int gosteady_cellular_start(void);

/*
 * True once the network has reported REGISTERED_HOME or REGISTERED_ROAMING.
 */
bool gosteady_cellular_is_registered(void);

/*
 * Latest cached signal quality. Returns 0 on success and fills the
 * out-parameters; returns -EAGAIN if no measurement is available yet
 * (typically because we haven't registered).
 *
 * RSRP is reported in dBm (e.g. -95). SNR is reported in dB (e.g. 10).
 */
int gosteady_cellular_get_signal(int16_t *rsrp_dbm, int8_t *snr_db);

/*
 * Read current UTC time as an ISO-8601 string ("YYYY-MM-DDTHH:MM:SSZ",
 * null-terminated). Returns 0 on success.
 *
 * As of 0.17.0-time this reads the NCS `date_time` library's cached value
 * (sourced NITZ -> NTP -> app-set), NOT a live AT+CCLK? round-trip — so it
 * never blocks on the modem and is safe in the session-start hot path. The
 * value is passed through the §4.1 plausibility gate (gs_time.h): a time
 * outside [2024,2050] is treated as "unsynced".
 *
 * Returns -EAGAIN if the library has no valid (plausible) time yet (no NITZ
 * and NTP not yet obtained), -EIO on a formatting failure.
 *
 * Caller-supplied buffer must be at least 24 bytes.
 */
int gosteady_cellular_get_network_time(char *buf, size_t buflen);

/*
 * Current UTC time as a Unix-epoch milliseconds value, from the `date_time`
 * library cache + §4.1 plausibility gate (same source/semantics as the ISO
 * accessor above). Used by session.c to populate the .dat header's
 * session_start_utc_ms / session_end_utc_ms fields.
 *
 * Returns 0 on success, -EAGAIN if no valid/plausible time is available,
 * -EINVAL on a NULL pointer.
 */
int gosteady_cellular_get_network_time_unix_ms(int64_t *out_ms);

/*
 * True iff the device currently holds a valid, plausible absolute time
 * (date_time_is_valid() AND the value passes the §4.1 year gate). This is the
 * `clock_synced` signal threaded into the activity/heartbeat payloads: when
 * false, the cloud reconstructs timestamps from upload-receive time + the
 * monotonic uptimes (device-time-reliability spec §4.3).
 */
bool gosteady_cellular_clock_is_synced(void);

/*
 * Convert a monotonic k_uptime millisecond value (captured earlier this boot
 * via k_uptime_get_32 / k_uptime_get) to absolute Unix-epoch milliseconds,
 * anchored to the `date_time` library's current sync (NTP when NITZ is
 * absent). This is the spec §4.2 retro-stamp: absolute = trusted-anchor +
 * uptime-delta, done with one consistent reference for both session ends.
 *
 * Returns 0 on success, -EAGAIN if the clock is not synced, -EINVAL on a NULL
 * pointer.
 */
int gosteady_cellular_uptime_to_unix_ms(uint32_t uptime_ms, int64_t *out_ms);

/*
 * The source of the most-recently obtained absolute time, as a short stable
 * string for the `time_source` observability field: "nitz" (modem %XTIME push),
 * "ntp" (SNTP), or "unsynced" (no valid time obtained yet). Never NULL.
 */
const char *gosteady_cellular_time_source(void);

/*
 * Format a Unix-epoch milliseconds value as ISO 8601 UTC string in the
 * canonical "YYYY-MM-DDTHH:MM:SSZ" form (23 chars + NUL = 24 bytes).
 *
 * Pure formatter — does not touch the modem; safe to call without
 * cellular registration. Used by cloud.c's activity worker to retro-
 * stamp session_start / session_end ISO strings from uptime deltas
 * captured at session_start/stop, when cellular UTC was unavailable
 * at capture time but is available at publish time (FMEA 1.1, 1.2).
 *
 * Returns 0 on success, -EINVAL if `out_sz < 24` or `unix_ms` is
 * negative, -EIO if gmtime_r fails.
 */
int gosteady_cellular_format_unix_ms_iso8601(int64_t unix_ms,
					      char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_CELLULAR_H_ */
