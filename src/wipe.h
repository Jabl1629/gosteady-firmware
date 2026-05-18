/*
 * GoSteady firmware — wipe routine for the AA-battery-recycle design.
 *
 * Cloud (`device-api` Lambda) issues `cmd: "wipe"` on `gs/{serial}/cmd`
 * immediately after a successful `end-assignment` API call. The hardware
 * shift to replaceable AAs (6-12 mo life) invalidated the original
 * charger-gated reset design (ARCH DL6, since rewritten 2026-05-17 — see
 * portal `docs/specs/2026-05-17-aa-battery-recycle.md`). The new design
 * is software-orchestrated: cloud commands, firmware acts, cloud auto-
 * recycles `discontinued → ready_to_provision` on ack.
 *
 * This module owns the local-wipe routine. The cmd dispatch lives in
 * `cloud.c::handle_wipe_cmd`; that function calls `gosteady_wipe_now`
 * after parsing the cmd. The wipe is deliberately not gated on
 * `gosteady_activation_is_activated()` — cloud is the authority on
 * whether a wipe should fire, and the dispatch path is reachable only
 * when cloud has sent the cmd.
 *
 * Wipe scope (per portal memo §3 D4):
 *   WIPE:  /lfs/activation.bin (via gosteady_activation_clear)
 *          /lfs/sessions/*.dat (via gosteady_session_orphan_sweep —
 *                              the function unconditionally fs_unlinks
 *                              every .dat, exactly what we want here)
 *          /snippets/* (via new gosteady_snippet_purge_all)
 *   KEEP:  firmware image, device cert (sec_tag 201), modem cert,
 *          /lfs/boot_count, crash_forensics partition (cross-deployment
 *          forensic continuity), in-RAM algo state (session-scoped;
 *          reset at next session_start)
 *
 * Battery floor (memo §3 D3): refuse + return -EAGAIN if
 * battery_pct < 0.10. Cloud retries on next heartbeat once battery
 * recovers (e.g., post-AA-swap). Caller may also retry via the cmd
 * being re-published.
 */

#ifndef GOSTEADY_WIPE_H_
#define GOSTEADY_WIPE_H_

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Battery percentage floor below which the wipe is refused. Defined
 * here so cloud.c (cmd dispatcher) and tests can reason about it.
 * Matches portal memo §3 D3.
 */
#define GOSTEADY_WIPE_BATTERY_FLOOR_PCT  0.10f

/*
 * Perform the local-wipe routine and emit the cloud ack.
 *
 * Steps (in order):
 *   1. Read battery_pct via gosteady_battery_get; refuse with -EAGAIN
 *      if < GOSTEADY_WIPE_BATTERY_FLOOR_PCT. Caller (cmd dispatcher)
 *      logs and returns; cloud retries on next wake.
 *   2. If a session is currently active, gosteady_session_stop(NULL)
 *      to release writer-thread state cleanly before unlinking .dat
 *      files. Drops any in-flight sample buffer — intentional, the
 *      device is recycling and this session's data is moot.
 *   3. gosteady_activation_clear() — fs_unlink /lfs/activation.bin
 *      + reset in-RAM activated atomic. Device is now in pre-
 *      activation state; session_start gate refuses capture.
 *   4. gosteady_session_orphan_sweep() — fs_unlink all /lfs/sessions/*.dat.
 *   5. gosteady_snippet_purge_all() — fs_unlink all /snippets/*.{bin,json,up}.
 *   6. Update s_last_cmd_id (via gosteady_cloud_set_last_cmd_id) so the
 *      next heartbeat carries the wipe_id echo for cloud's ack-matching.
 *   7. Publish Shadow `reported.wipe_complete = <wipe_id>` and
 *      `reported.wipe_completed_at = <ts>` via aws_iot_send. Caller
 *      controls connection lifecycle; this function assumes connected.
 *
 * Returns 0 on full success, negative errno on any of the above failing.
 * Specific failure modes:
 *   -EAGAIN  — battery floor not met; retry after AA swap / charge
 *   -ENODEV  — battery readout failed; can't verify floor
 *   -EIO     — flash failure (unlink errors aggregated)
 *   -ECOMM   — Shadow ack publish failed (other steps succeeded; cloud
 *              will see no wipe_complete in reported state but may still
 *              see last_cmd_id echo in next heartbeat — both ack paths
 *              exist by design per portal memo §5)
 *
 * Idempotent: a repeated wipe with the same wipe_id is harmless — already-
 * unlinked files return -ENOENT (treated as success), and Shadow reported
 * just gets re-written with the same value.
 *
 * Thread-safety: called from the cloud worker thread (AWS IoT event
 * handler context) only. session_stop is internally thread-safe.
 */
int gosteady_wipe_now(const char *wipe_id);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_WIPE_H_ */
