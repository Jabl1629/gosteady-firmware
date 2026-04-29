/*
 * GoSteady firmware — Milestone 12.1e.2: pre-activation gate + activation
 * state persistence.
 *
 * Until a device receives its first `activate` cmd from the cloud, it must
 * not capture sessions. The activation state is persisted to flash so it
 * survives reboot — once activated, the device stays activated until
 * cloud-side de-provisioning (a future Phase 2A flow that writes
 * `desired.activated_at = null` to Shadow; we re-enter pre-activation on
 * the next cellular wake when we observe the null).
 *
 * State persisted:
 *   - activated_at_iso: the ISO 8601 timestamp from the last successful
 *     activate cmd (also written back to Shadow.reported.activated_at as
 *     the device-side ack).
 *   - last_cmd_id: most-recent activate cmd_id; echoed in next heartbeat
 *     via gosteady_cloud_set_last_cmd_id() per portal §1.2 ack contract.
 *
 * The cloud-side `device-api` Lambda issues activate cmds via
 * `gs/{serial}/cmd` (Phase 2A). For the M12.1e.2 acceptance probe we
 * synthesize one with `aws iot publish` against the bench Thing.
 */

#ifndef GOSTEADY_ACTIVATION_H_
#define GOSTEADY_ACTIVATION_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Load persisted activation state from flash. Idempotent. Safe to call
 * after LittleFS is mounted. Returns 0 on success, negative errno on
 * read/parse failure (state stays in the "not activated" default — the
 * device behaves correctly even when the persisted record is absent
 * or corrupt).
 */
int gosteady_activation_init(void);

/*
 * Returns true if the device has received and persisted an activate cmd.
 * Fast-path read (atomic, no I/O) — safe to call from any context.
 */
bool gosteady_activation_is_activated(void);

/*
 * Copy the current activated_at ISO 8601 timestamp into `out`. `out_sz`
 * must be ≥24 ("YYYY-MM-DDTHH:MM:SSZ" + NUL = 21 actual; leave headroom).
 * Writes empty string + returns -ENODATA if not yet activated.
 * Returns 0 on success, -EINVAL if `out_sz < 24`.
 */
int gosteady_activation_get_at(char *out, size_t out_sz);

/*
 * Copy the persisted last_cmd_id (the activate cmd_id of the last
 * successful activation) into `out`. `out_sz` must be ≥40. Used by
 * main.c at boot to mirror the cmd_id back into cloud.c's
 * gosteady_cloud_set_last_cmd_id so the post-reboot heartbeat still
 * carries the ack-echo per portal §C.2 (cloud's 24 h matching window).
 *
 * Writes empty string + returns -ENODATA if not yet activated.
 * Returns 0 on success, -EINVAL if `out_sz` < 40.
 */
int gosteady_activation_get_last_cmd_id(char *out, size_t out_sz);

/*
 * Apply a successfully-parsed activate cmd: persist activated_at and
 * cmd_id to flash, mark device as activated. Idempotent — applying the
 * same cmd_id twice is a no-op (cloud retries are safe).
 *
 * The next heartbeat will include `last_cmd_id` (M12.1c.2 plumbing) and
 * a separate cloud helper writes `reported.activated_at` to Shadow.
 *
 * Returns 0 on success, negative errno on flash write failure.
 */
int gosteady_activation_apply(const char *cmd_id, const char *activated_at_iso);

/*
 * Clear persisted activation state. Triggered when cloud writes
 * `desired.activated_at = null` to Shadow (de-provisioned device). Next
 * boot or subsequent session_start refuses session capture until a fresh
 * activate cmd arrives.
 *
 * Returns 0 on success, negative errno on flash unlink failure.
 */
int gosteady_activation_clear(void);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_ACTIVATION_H_ */
