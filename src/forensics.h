/*
 * GoSteady firmware — Milestone 10.7.3: crash forensics + watchdog.
 *
 * Persisted-across-reset record of:
 *   - boot counter
 *   - reset cause (last and previous boot)
 *   - fatal-error frame from the most recent fault (PC / LR / PSR / reason
 *     / thread name / uptime when it fired)
 *   - cumulative counters: fault_count, watchdog_hits
 *
 * Persistence target is the dedicated `crash_forensics` partition carved out
 * by M10.7.1 — separate from LittleFS so a corrupted /lfs sessions FS can't
 * take the postmortem down with it. We only use the first 4 KB erase block
 * of the 64 KB partition; the rest is reserved for future per-fault history.
 *
 * Watchdog is a single-channel hardware WDT (~60 s timeout) kicked from a
 * dedicated supervisor thread. The watchdog is non-pausable, so a kernel
 * hang or panic that the system can't recover from will trip a reset within
 * the timeout window — captured in the next-boot reset cause and surfaced
 * in the next heartbeat. v1 doesn't do per-thread liveness checking; the
 * supervisor's mere existence + kick is the canary.
 *
 * After a reset, the previous boot's reset_reason / fault frame can be read
 * via the public getters and surfaced in the next heartbeat (M12.1c.2 extras).
 */

#ifndef GOSTEADY_FORENSICS_H_
#define GOSTEADY_FORENSICS_H_

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize forensics: load the persisted record from flash, capture
 * this boot's reset cause via hwinfo, increment boot_count, persist back.
 * Then start the watchdog + supervisor thread.
 *
 * Idempotent. Safe to call once early in main(). Failure is non-fatal —
 * the public getters return zero/empty defaults if init didn't run.
 */
int gosteady_forensics_init(void);

/*
 * Reset cause of THIS boot, formatted per the portal contract examples
 * ("POWER_ON", "PIN", "SOFTWARE", "WATCHDOG", "FAULT_AT_<pc_hex>", or
 * comma-joined for multi-bit). Always NUL-terminates `out`.
 *
 * Returns 0 on success, -EINVAL if `out_sz < 32` (need room for the
 * longest format).
 */
int gosteady_forensics_get_reset_reason(char *out, size_t out_sz);

/*
 * Cumulative counters across all boots since flash erase. All return 0
 * if forensics has never been initialized. Used by the M12.1c.2 production
 * heartbeat to populate the `fault_counters`, `watchdog_hits` portal-spec
 * optional fields.
 */
uint32_t gosteady_forensics_get_fault_count(void);
uint32_t gosteady_forensics_get_watchdog_hits(void);
uint32_t gosteady_forensics_get_assert_count(void);

/*
 * Build a small JSON object of fault counters suitable for direct embed
 * into the heartbeat payload as the `fault_counters` field. Format:
 *   {"fatal":N,"asserts":N,"watchdog":N}
 * Always NUL-terminates `buf`. Writes the byte count (excluding NUL) to
 * `*out_len`. Returns 0 on success, -ENOMEM on truncation.
 */
int gosteady_forensics_fault_counters_json(char *buf, size_t buflen,
					   size_t *out_len);

/*
 * Uptime since the most recent boot, in seconds. Wraps at ~136 years.
 */
uint32_t gosteady_forensics_get_uptime_s(void);

#if defined(CONFIG_GOSTEADY_FORENSICS_STRESS)
/*
 * Deliberate fault for stress-testing the forensics path. Performs a NULL
 * dereference which triggers a BUSFAULT / MEMFAULT and routes through our
 * k_sys_fatal_error_handler → flash persist → reboot. Next boot's heartbeat
 * should report fault_count incremented + last_fault populated.
 *
 * Compiled in only when CONFIG_GOSTEADY_FORENSICS_STRESS=y (must be a
 * deliberate temporary opt-in — stress-test surface should never ship to a
 * real deployment build).
 */
__attribute__((noreturn)) void gosteady_forensics_stress_fault(void);

/*
 * Deliberate WDT hit for stress-testing. Sets a flag that causes the
 * supervisor thread to stop feeding the watchdog; ~60 s later the hardware
 * WDT fires a SoC reset. Next boot's heartbeat should report
 * reset_reason="WATCHDOG" (joined with whatever else hwinfo reports) and
 * watchdog_hits incremented.
 */
void gosteady_forensics_stress_stall_wdt(void);
#endif /* CONFIG_GOSTEADY_FORENSICS_STRESS */

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_FORENSICS_H_ */
