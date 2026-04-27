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

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Initialize the AWS IoT lib + spawn the one-shot publisher worker.
 * Idempotent: only the first call registers the lib + spawns the thread;
 * subsequent calls return 0 without re-initializing.
 *
 * Returns 0 on success, negative errno on aws_iot_init failure.
 */
int gosteady_cloud_init(void);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_CLOUD_H_ */
