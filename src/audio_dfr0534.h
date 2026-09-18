/*
 * GoSteady firmware — DFR0534 (JQ8400-class) Gravity MP3/voice module driver.
 *
 * Family Assistance Alert speaker prompts. Design + hardware facts live in
 * gosteady-portal/docs/specs/family-assistance-alert.md (§4.2–§4.5, §5.3):
 *
 *   - Wiring (prototype, R1): Thingy:91 X P1 expansion connector →
 *     SparkFun Qwiic-to-Gravity cable → DFR0534 Gravity header.
 *     P1 pin 4 (SCL side) = nRF9151 P0.19 = EXP_BOARD_PIN1 → module "R" → our TX
 *     P1 pin 3 (SDA side) = nRF9151 P0.18 = EXP_BOARD_PIN2 → module "T" → our RX
 *     (schematic PCA20065 v2.0.0, nRF9151 sheet). SB8 and SB9 MUST be cut
 *     so P0.19/P0.18 are no longer bridged onto the sensor I²C bus.
 *     uart1 is re-pinned to those GPIOs at 9600 8N1 by
 *     boards/assist_audio_uart1.overlay (the nRF9151 has no spare UARTE).
 *   - Power: the module hangs off VDD_EXP_BRD (nPM1300 BUCK2 3.3 V through
 *     load switch U14, enabled by P0.03 = `exp_board_enable`). It is powered
 *     ONLY while a prompt sequence runs — its idle current would otherwise
 *     dominate the battery (spec L8).
 *   - Protocol: 9600 8N1, frame `AA <cmd> <len> <data…> <SM>`, SM = low byte of
 *     the sum of all preceding bytes. Track index = copy order on the
 *     module's USB drive (see tools/load_dfr0534_prompts.sh).
 *
 * All calls are blocking and serialized by an internal mutex; call them from
 * the assist thread, never from ISR context.
 */

#ifndef GOSTEADY_AUDIO_DFR0534_H_
#define GOSTEADY_AUDIO_DFR0534_H_

#include <stdbool.h>
#include <stdint.h>
#include <zephyr/kernel.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Prompt track numbers — MUST match audio/prompts/MANIFEST.md (copy order). */
enum gs_prompt {
	GS_PROMPT_SILENCE        = 1,   /* 100 ms guard track (power-up auto-play) */
	GS_PROMPT_PRESSED_20S    = 2,   /* "Assistance button pressed, contacting care circle in 20 seconds." */
	GS_PROMPT_TONE           = 3,   /* two short beeps */
	GS_PROMPT_CONTACTING_10S = 4,   /* "Contacting care circle in 10 seconds." */
	GS_PROMPT_CANCELLED      = 5,   /* descending tone + "Cancelled." */
	GS_PROMPT_CONTACTED      = 6,   /* "Contacted care circle." */
	GS_PROMPT_RETRYING       = 7,   /* "Could not reach your care circle yet. Still trying." */
	GS_PROMPT_TEST_OK        = 8,   /* "Test complete. Care circle contacted." */
	GS_PROMPT_NOT_SETUP      = 9,   /* "Assistance is not set up yet." */
	GS_PROMPT_FAULT          = 10,  /* error tone */
	GS_PROMPT_COUNT          = 10,
};

/* Playback status byte returned by the module's status query. */
#define GS_AUDIO_STATUS_STOPPED  0
#define GS_AUDIO_STATUS_PLAYING  1
#define GS_AUDIO_STATUS_PAUSED   2

/* Bind the UART + load-switch devices, start RX. Module stays unpowered.
 * Returns 0 or negative errno (device not ready). */
int gs_audio_init(void);

/* Enable VDD_EXP_BRD, wait for the module to answer a status query (bounded
 * by CONFIG_GOSTEADY_ASSIST_AUDIO_BOOT_TIMEOUT_MS), then apply volume + loop
 * mode. Returns 0 when the module replied, -EIO when it stayed silent (power
 * is left ON so TX-only playback can still be attempted), other negative
 * errno on regulator failure. Idempotent. */
int gs_audio_power_on(void);

/* Stop playback and cut VDD_EXP_BRD. Idempotent. */
void gs_audio_power_off(void);

bool gs_audio_is_powered(void);

/* Fire-and-forget: play track `track` (1-based) — interrupts anything playing. */
int gs_audio_play(uint16_t track);

/* Stop playback. */
int gs_audio_stop(void);

/* Set volume 0..30. */
int gs_audio_set_volume(uint8_t volume);

/* Query the playback status byte (GS_AUDIO_STATUS_*). */
int gs_audio_query_status(uint8_t *status, k_timeout_t timeout);

/* Number of audio files the module sees on its drive. */
int gs_audio_query_track_count(uint16_t *count, k_timeout_t timeout);

/* Wait for a track started with gs_audio_play() to finish: first waits (≤ ~1 s)
 * for the module to report PLAYING, then for STOPPED. Returns 0 when playback
 * ended, -ETIMEDOUT if `timeout` elapsed, -EIO if the module never answered. */
int gs_audio_wait_done(k_timeout_t timeout);

/* Bench self-check: log track count and the short filename behind each index
 * (uses "select without play" + "query short name"). Returns the count seen. */
int gs_audio_selftest(void);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_AUDIO_DFR0534_H_ */
