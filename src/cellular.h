/*
 * GoSteady firmware — Milestone 12.1a: cellular bring-up.
 *
 * Async LTE-M attach via lte_lc_connect_async + a small reporter that
 * logs registration state changes, RSRP/SNR, and network UTC time
 * (AT+CCLK?) once the modem is registered. No MQTT, no sockets — those
 * land in M12.1c.
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
 * Read network UTC time via AT+CCLK?. Returns 0 on success and fills
 * `buf` with an ISO-8601 string ("YYYY-MM-DDTHH:MM:SSZ", null-terminated).
 * Returns -EAGAIN if the network has not yet provided time (typical
 * before registration completes), or a negative errno on AT failure.
 *
 * Caller-supplied buffer must be at least 24 bytes.
 */
int gosteady_cellular_get_network_time(char *buf, size_t buflen);

/*
 * Read network UTC time via AT+CCLK? as a Unix-epoch milliseconds value.
 *
 * Used by session.c to populate the .dat header's session_start_utc_ms /
 * session_end_utc_ms fields (the activity-uplink path uses the ISO string
 * accessor above instead, since cloud schemas expect ISO 8601 strings).
 *
 * Returns 0 on success, -EAGAIN if the modem hasn't yet received NITZ,
 * or -EIO on AT failure. Same caching semantics as the ISO accessor —
 * each call hits AT+CCLK?, no internal cache.
 */
int gosteady_cellular_get_network_time_unix_ms(int64_t *out_ms);

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
