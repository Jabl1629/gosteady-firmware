/*
 * GoSteady firmware — Family Assistance Alert: assistance-button incident
 * state machine (portal spec family-assistance-alert.md §5.2).
 *
 *   IDLE → PENDING (20 s countdown, spoken prompts, 2nd press cancels)
 *        → SENDING → AWAIT_ACK → CONFIRMED → IDLE
 *        ↘ CANCELLED / FAILED_RETRYING
 *
 * The button ISR only signals this module (gs_assist_button_isr); everything
 * else runs on the `gs_assist` thread. Audio goes through audio_dfr0534.c.
 *
 * Cloud transport is pluggable:
 *   - CONFIG_GOSTEADY_ASSIST_STUB_CLOUD (bench, no MQTT): "publish" is logged
 *     and a simulated `assist_ack` arrives after ASSIST_STUB_ACK_MS. This is the
 *     FA-0/FA-1 button→speaker harness.
 *   - real cloud (FA-2): cloud.c publishes gs/{serial}/assist and calls
 *     gs_assist_ack_received() when the `assist_ack` cmd lands.
 */

#ifndef GOSTEADY_ASSIST_H_
#define GOSTEADY_ASSIST_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Spawn the assist thread. Call after audio + LEDs are initialised. */
int gs_assist_start(void);

/* ISR-safe: called from the SW0 GPIO callback on every press edge. */
void gs_assist_button_isr(void);

/* Bench hook: inject a debounced press (control channel "PRESS"). */
void gs_assist_inject_press(void);

/* Bench hook: the button reads as held for ms (control channel "HOLD <ms>"). */
void gs_assist_inject_hold(uint32_t ms);

/* True while an incident owns the LED/speaker (IDLE == false). Used by main.c
 * to suppress the bench purple blink. */
bool gs_assist_is_active(void);

/* Arming = the cloud has confirmed ≥ 1 eligible Care Circle contact (spec
 * §6.5). Un-armed presses only play "Assistance is not set up yet". The stub
 * build boots armed; real builds boot un-armed until an `assist_arm` cmd. */
void gs_assist_set_armed(bool armed);
bool gs_assist_is_armed(void);

/* Deliver a cloud `assist_ack` for `incident_id`. ISR/timer-safe.
 * status: 0 = accepted, 1 = not_ready. test_mode selects the test prompt. */
void gs_assist_ack_received(const char *incident_id, bool test_mode, int status);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_ASSIST_H_ */
