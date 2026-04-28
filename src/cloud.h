/*
 * GoSteady firmware — Milestone 12.1c.1: cloud bring-up (MQTT/TLS to AWS IoT).
 *
 * Owns the AWS IoT MQTT/TLS lifecycle via the NCS aws_iot lib. For M12.1c.1
 * this is a one-shot publisher that:
 *
 *   1. Waits for cellular registration + network time (via cellular.h).
 *   2. Connects to AWS IoT Core (TLS handshake + MQTT CONNECT).
 *   3. Publishes a single heartbeat to gs/{client_id}/heartbeat with
 *      placeholder battery_pct=0.5 and real RSRP/SNR/ts from M12.1a.
 *   4. Disconnects and exits.
 *
 * The whole flow runs on an internal worker thread so main() can return
 * promptly. Logs every event verbosely on the app console (uart0) so a
 * first-time TLS handshake failure is diagnosable from logs/uart0_*.log
 * alone.
 *
 * M12.1c.2 will add hourly cadence + all locked optional extras
 * (last_cmd_id, reset_reason, fault_counters, etc.) + real battery_pct
 * from M10.7.2 nPM1300 fuel gauge.
 */

#ifndef GOSTEADY_CLOUD_H_
#define GOSTEADY_CLOUD_H_

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * M12.1d activity payload — built by session.c at session close (after
 * the M9 algo finishes), submitted to the cloud worker via
 * gosteady_cloud_publish_activity().
 *
 * String fields are inline char arrays (not pointers) so the struct can
 * be copied into the msgq and outlive the caller's stack frame.
 * Optional fields use sentinel values:
 *   - roughness_R:    NaN means "not applicable" (stationary baseline,
 *                     buffer overflow, etc.) — omitted from JSON.
 *   - surface_class:  0xFF means "unknown" — omitted from JSON.
 *                     0 = indoor, 1 = outdoor (matches gs_surface_t).
 *   - firmware_version: empty string means "omit" — but in practice the
 *                       caller always populates it from the build's
 *                       compile-time constant.
 */
#define GOSTEADY_ACTIVITY_SURFACE_INDOOR   0
#define GOSTEADY_ACTIVITY_SURFACE_OUTDOOR  1
#define GOSTEADY_ACTIVITY_SURFACE_UNKNOWN  0xFF

struct gosteady_activity {
	char     session_start_utc_iso[24];   /* "YYYY-MM-DDTHH:MM:SSZ" */
	char     session_end_utc_iso[24];
	uint32_t steps;
	float    distance_ft;
	uint32_t active_min;
	float    roughness_R;                 /* NaN → omit */
	uint8_t  surface_class;               /* 0xFF → omit */
	char     firmware_version[16];        /* "" → omit */
};

/*
 * Initialize the AWS IoT lib + spawn the cloud publisher worker thread.
 * Idempotent: only the first call registers the lib + spawns the worker;
 * subsequent calls return 0 without re-initializing.
 *
 * The worker handles both M12.1c heartbeats (one-shot at boot) and M12.1d
 * activity uplinks (one per session close). Connect/publish/disconnect is
 * serialized — concurrent publishes queue behind each other.
 *
 * Returns 0 on success, negative errno on aws_iot_init failure.
 */
int gosteady_cloud_init(void);

/*
 * Submit an activity record for asynchronous publish to gs/{client_id}/activity.
 * Returns immediately after copying the record into the cloud worker's queue;
 * the actual cellular round-trip happens on the worker's own thread.
 *
 * Returns 0 on successful enqueue, -EAGAIN if the queue is full (current cap
 * is 4 pending activities — far more than realistic back-to-back session
 * closure rate). Caller should log + drop on -EAGAIN; the cellular path will
 * catch up.
 *
 * Safe to call from any context (including session.c's writer thread) as long
 * as the caller doesn't expect the publish to complete before the call returns.
 */
int gosteady_cloud_publish_activity(const struct gosteady_activity *a);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_CLOUD_H_ */
