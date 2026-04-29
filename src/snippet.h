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

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_SNIPPET_H_ */
