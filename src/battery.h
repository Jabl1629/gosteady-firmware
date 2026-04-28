/*
 * GoSteady firmware — Milestone 10.7.2: nPM1300 fuel gauge.
 *
 * Owns the charger sensor + nrf_fuel_gauge lib lifecycle. A background
 * worker thread polls the charger ~every 5 s, runs nrf_fuel_gauge_process,
 * and caches the resulting voltage / SoC. Other modules (cloud heartbeat,
 * session header) read the cached values via gosteady_battery_get().
 *
 * Replaces the M12.1c.1 `battery_pct = 0.5` placeholder. With real values
 * landing in heartbeats, the cloud-side `battery_critical` synthetic alert
 * (Phase 1B revision Threshold Detector) starts producing meaningful
 * caregiver-facing notifications.
 *
 * Battery model: bundled "Example" 1100 mAh LiPol model from the NCS
 * fuel-gauge sample. The Thingy:91 X carries an LP803448 (~1300 mAh)
 * LiPol — close enough to the bundled model that voltage-based SoC
 * correction stays within ±5-10% absolute, which is fine for the v1
 * cloud telemetry. v1.5 should swap in a model tuned against an actual
 * LP803448 discharge curve once we have field data.
 */

#ifndef GOSTEADY_BATTERY_H_
#define GOSTEADY_BATTERY_H_

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up the nPM1300 charger sensor + nrf_fuel_gauge lib + the periodic
 * update worker. Idempotent: subsequent calls return 0 without re-initializing.
 *
 * Returns 0 on success. -ENODEV if the npm1300_charger devicetree node is
 * not ready. Negative errno from the underlying lib otherwise. On failure
 * the public getter still works but always returns the placeholder values
 * (battery_mv = 0, battery_pct = 0.5) — caller never has to special-case
 * a failed init for cloud-uplink purposes.
 */
int gosteady_battery_init(void);

/*
 * Latest cached battery readings. Both out-pointers are optional (NULL skips
 * that field).
 *
 *   *out_mv   = battery voltage in millivolts (e.g. 3850).
 *   *out_pct  = state-of-charge as 0.0–1.0 (e.g. 0.78 for 78%). The cloud
 *               heartbeat schema uses this 0.0–1.0 range; do NOT scale
 *               in callers.
 *
 * Returns 0 if cached values are real (worker thread has produced ≥1 reading
 * since boot), -EAGAIN if the cache is still warming up (returns the
 * placeholders 0 / 0.5 in that case so callers can still publish without
 * a special branch).
 */
int gosteady_battery_get(uint16_t *out_mv, float *out_pct);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_BATTERY_H_ */
