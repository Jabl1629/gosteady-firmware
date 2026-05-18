/*
 * GoSteady firmware — Milestone 12.1f: snippet capture + opportunistic
 * upload.
 *
 * Captures a 30 s window of raw 100 Hz BMI270 IMU samples per session
 * (v1: always-on; v1.5 will add scheduled / anomaly triggers per the
 * M10.5 snippet upload policy). Stores each snippet in the dedicated
 * snippet_storage flash partition (M10.7.1, 16 MB at 0xd22000) on its
 * own LittleFS instance, separate from /lfs/sessions, so an FS
 * corruption on either side stays contained.
 *
 * On-device layout per snippet (path-prefix `/snippets/`):
 *   <snippet_id>.bin   16-byte binary header + N × 28-byte sample records
 *                      (matches §F.4 inner binary; little-endian)
 *   <snippet_id>.json  metadata sidecar: snippet_id, window_start_ts,
 *                      and (when v1.5 anomaly path lands) anomaly_trigger
 *   <snippet_id>.up    zero-byte marker file = "uploaded to cloud"
 *                      (presence-driven; absence = pending upload)
 *
 * Upload path (M12.1f) — drain ONE oldest pending snippet per heartbeat
 * cellular wake. Outer wire framing per §F.3:
 *   [4-byte BE uint32 hdr_len][hdr_len bytes JSON][.bin contents]
 *
 * v1 retain-forever once uploaded (mark + leave); v1.5 will add the
 * 14-day stale cutoff + 90 % full storage rotation per the M10.5
 * policy. For the deployment-readiness floor, never-deleting is fine —
 * the snippet partition holds ~190 max-sized snippets and clinic walks
 * are typically much shorter, so capacity is months at the 8/day
 * upper bound (which we don't actually enforce in v1; relying on FS
 * write failures + storage pressure as the implicit backstop).
 */

#ifndef GOSTEADY_SNIPPET_H_
#define GOSTEADY_SNIPPET_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Snippet capture window in milliseconds — 30 s @ 100 Hz = 3000 samples.
 * Per §F.4 max payload size is 16 + 3000 × 28 = 84,016 bytes, well
 * under the 100 KB MQTT cap.
 */
#define GOSTEADY_SNIPPET_WINDOW_MS  30000

/* FMEA 6.1 (2026-05-10): snippet rotation policy parameters per the
 * M10.5 spec. Bench-default values; can be overridden via Kconfig in a
 * follow-up if field data shows the defaults are wrong.
 *
 * STALE_AGE_S:  any snippet whose window_start_ts is older than this
 *               many seconds ago is unconditionally deleted (.bin +
 *               .json + .up). 14 days aligns with the 90-day S3 hot-
 *               storage lifecycle in the cloud spec — anything we held
 *               on-device for 2 weeks isn't useful for v1.5 retrain.
 *
 * ROTATE_THRESHOLD_PCT: when partition utilization >= this %, delete
 *               oldest .up-marked snippets until below threshold.
 *               Never deletes un-uploaded snippets — those wait for
 *               STALE_AGE_S to make them eligible. */
#define GOSTEADY_SNIPPET_STALE_AGE_S       (14u * 24u * 3600u)
#define GOSTEADY_SNIPPET_ROTATE_THRESHOLD  90u

/*
 * Mount the snippet_storage LittleFS partition. Idempotent. Safe to call
 * once at boot, after the main /lfs is up. Returns 0 on success,
 * negative errno on mount failure (snippet capture/upload skipped on
 * failure; no fail-loud since /lfs/sessions is the canonical raw-data
 * source the deployment-readiness floor depends on).
 */
int gosteady_snippet_init(void);

/*
 * Open a new snippet capture for the current session. Called from
 * session.c::gosteady_session_start when capture is "armed" (v1: always
 * armed; v1.5 will gate on scheduled / anomaly triggers).
 *
 * `session_uuid_str` is the 36-char hyphenated UUID the session header
 * already stamps; reused as the snippet_id for cross-correlation.
 * `window_start_ts` is the ISO 8601 wall-clock time of the first
 * sample, copied verbatim into the JSON sidecar.
 *
 * Idempotent if already capturing for the same session_uuid (no-ops);
 * returns -EALREADY if a different session's capture is in progress.
 */
int gosteady_snippet_capture_start(const char *session_uuid_str,
				    const char *window_start_ts,
				    uint64_t window_start_uptime_ms);

/*
 * Append one BMI270 sample to the in-progress snippet. Returns 0 on
 * success, -EOVERFLOW once we've hit GOSTEADY_SNIPPET_WINDOW_MS / 10
 * samples (capture saturates at the window; further appends are
 * silently dropped). Caller (writer thread in session.c) does not
 * need to track the window — the snippet module enforces it.
 */
int gosteady_snippet_capture_append(uint32_t t_ms,
				     float ax, float ay, float az,
				     float gx, float gy, float gz);

/*
 * Close the in-progress snippet: rewrite the 16-byte binary header
 * with the final sample_count, close the .bin file, write the .json
 * sidecar with the metadata. Called from session.c::gosteady_session_stop.
 *
 * Returns 0 on success. Idempotent if no capture is in progress.
 */
int gosteady_snippet_capture_finish(void);

/*
 * Upload one pending snippet via the cloud worker if any are queued.
 * Called from cloud.c::heartbeat path AFTER the heartbeat PUBACK and
 * BEFORE disconnect. Per M10.5 policy: 1 snippet per cellular wake.
 *
 * Picks the oldest non-uploaded snippet (FIFO by mtime), builds the
 * outer framing per §F.3 in a caller-supplied buffer, and invokes
 * the publish callback. On callback success (PUBACK received), marks
 * the snippet uploaded by writing a zero-byte `.up` marker file.
 *
 * Returns 0 if a snippet was uploaded, -ENOENT if none pending, or
 * negative errno on framing / publish error.
 */
typedef int (*gosteady_snippet_publish_fn)(const uint8_t *payload,
					    size_t payload_len);

int gosteady_snippet_upload_one(gosteady_snippet_publish_fn publish_fn);

/*
 * FMEA 6.1 (2026-05-10): snippet partition rotation pass.
 *
 * Two phases:
 *   1. Stale cutoff — delete (.bin + .json + .up) for any snippet whose
 *      JSON sidecar window_start_ts is older than `stale_age_seconds`
 *      ago. Requires cellular UTC to be available; silently skips this
 *      phase otherwise.
 *   2. Free-space rotation — if fs_statvfs reports utilization >=
 *      `rotate_threshold_pct`, delete the oldest .up-marked snippet
 *      (FIFO by readdir order, which is roughly insertion in LittleFS).
 *      Loop until below threshold OR no .up-marked snippet remains.
 *      Never deletes un-uploaded snippets.
 *
 * Defensive: skips deletion on bad signals (statvfs error, malformed
 * sidecar, etc.) — never deletes based on uncertain data.
 *
 * Currently-active capture's UUID (held under s_capture_lock) is also
 * skipped to avoid racing with capture_finish.
 *
 * Returns the total number of snippets deleted (across both phases),
 * or negative errno on directory iteration failure. Bench-callers can
 * use the return as a count for visibility logging.
 */
int gosteady_snippet_rotate(uint32_t stale_age_seconds,
			    uint8_t rotate_threshold_pct);

/*
 * Purge ALL snippets from /snippets/. Deletes every .bin / .json / .up
 * tuple unconditionally — including un-uploaded snippets (intentional;
 * the wipe path discards patient-derived data regardless of upload
 * state).
 *
 * Called by the wipe routine (src/wipe.c) when cloud issues a `wipe`
 * cmd post-end-assignment. Skips the currently-active capture's UUID
 * (held under s_capture_lock) defensively — though the wipe path
 * should have already stopped any active session via session_stop
 * before calling here.
 *
 * Returns the count of snippet "tuples" (.bin files) removed, or
 * negative errno on directory iteration failure. -ENOENT-class failures
 * on individual unlinks are non-fatal (logged + counted as zero).
 *
 * Added 2026-05-17 for the AA-battery-recycle design — see portal
 * `docs/specs/2026-05-17-aa-battery-recycle.md` D4 (wipe scope) and
 * firmware coord §C20.
 */
int gosteady_snippet_purge_all(void);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_SNIPPET_H_ */
