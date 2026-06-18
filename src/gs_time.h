/*
 * GoSteady firmware — absolute-time plausibility gate (device-time-reliability
 * spec §4.1; portal docs/specs/2026-06-18-device-time-reliability.md).
 *
 * Pure, dependency-free (only <stdint.h>) so the host regression suite in
 * tests/host/ can include and unit-test it without a Zephyr/modem build.
 *
 * Why this exists: before 0.17.0-time the firmware's only absolute-time source
 * was carrier NITZ via AT+CCLK?. On a roaming SIM with no NITZ the nRF9151
 * modem RTC sits at its 1980 default and AT+CCLK? returns "80/01/06,..."; the
 * old parser only rejected a *malformed* reply, so "20%02d" formatted that as
 * year 2080 and ~60 of GS0000000001's Jun 14-16 walks were mis-dated (coord
 * §C47). 0.17.0-time moves time acquisition to the NCS `date_time` lib
 * (NITZ -> NTP -> app-set), which structurally cannot emit the 1980 default
 * (its modem source is the %XTIME NITZ push, not the free-running RTC). This
 * gate is the belt-and-suspenders hard floor enforcing the invariant:
 *
 *     No timestamp with year < 2024 (or > 2050) ever reaches the wire.
 *
 * Anything outside the window is treated as "unsynced" by the cellular time
 * accessors (they return -EAGAIN), so a bad NTP/NITZ value can never become a
 * published timestamp.
 */

#ifndef GOSTEADY_GS_TIME_H_
#define GOSTEADY_GS_TIME_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Unix-epoch millisecond bounds for the plausible-year window [2024, 2050].
 *   2024-01-01T00:00:00Z = 1704067200 s  (inclusive lower bound)
 *   2051-01-01T00:00:00Z = 2556144000 s  (exclusive upper bound => year <= 2050)
 * Both verified against the 2000-01-01 (946684800) and 2020-01-01 (1577836800)
 * epoch anchors. */
#define GS_TIME_PLAUSIBLE_MIN_MS  1704067200000LL
#define GS_TIME_PLAUSIBLE_MAX_MS  2556144000000LL

/*
 * True iff `unix_ms` falls in the plausible-year window [2024-01-01,
 * 2051-01-01). Used by the cellular time accessors to reject the modem's
 * 1980/2080 default (or any other implausible anchor) before it can be
 * formatted into a wire timestamp.
 */
static inline bool gs_time_year_is_plausible(int64_t unix_ms)
{
	return (unix_ms >= GS_TIME_PLAUSIBLE_MIN_MS) &&
	       (unix_ms <  GS_TIME_PLAUSIBLE_MAX_MS);
}

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_GS_TIME_H_ */
