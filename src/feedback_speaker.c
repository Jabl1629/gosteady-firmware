/*
 * GoSteady firmware — Family Assistance feedback backend: DFR0534 MP3 speaker
 * with spoken prompts (ARCHIVED path; bench-proven 2026-09-18, coord §C63.6).
 *
 * The product moved to a Qwiic buzzer the same day (size + BOM, family-only
 * notification — see feedback_buzzer.c and spec v0.4). This wrapper keeps the
 * validated speaker path selectable (GOSTEADY_ASSIST_FEEDBACK_SPEAKER) so it
 * can return as an option without re-deriving the wiring.
 *
 * Mapping: countdown_start → PRESSED_20S, countdown_mid → TONE + CONTACTING_10S,
 * ticks → nothing (speech carries the countdown), outcomes → their prompts.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include "feedback.h"
#include "audio_dfr0534.h"

LOG_MODULE_REGISTER(gs_fb_speaker, LOG_LEVEL_INF);

#define PROMPT_WAIT K_SECONDS(8)

static void say_wait(int prompt)
{
	(void)gs_audio_play((uint16_t)prompt);
	(void)gs_audio_wait_done(PROMPT_WAIT);
}

int gs_feedback_init(void)
{
	return gs_audio_init();
}

int gs_feedback_begin(void)
{
	return gs_audio_power_on();
}

void gs_feedback_end(void)
{
	gs_audio_power_off();
}

void gs_feedback_countdown_start(void)
{
	(void)gs_audio_play(GS_PROMPT_PRESSED_20S);
}

void gs_feedback_countdown_mid(void)
{
	say_wait(GS_PROMPT_TONE);
	(void)gs_audio_play(GS_PROMPT_CONTACTING_10S);
}

void gs_feedback_tick(enum gs_fb_phase phase)
{
	ARG_UNUSED(phase);   /* speech carries the countdown */
}

void gs_feedback_hold_tone(bool on)
{
	if (on) {
		(void)gs_audio_play(GS_PROMPT_TONE);
	}
}

void gs_feedback_cancelled(void)
{
	say_wait(GS_PROMPT_CANCELLED);
}

void gs_feedback_confirmed(bool test_mode)
{
	say_wait(test_mode ? GS_PROMPT_TEST_OK : GS_PROMPT_CONTACTED);
}

void gs_feedback_failed(void)
{
	say_wait(GS_PROMPT_RETRYING);
}

void gs_feedback_not_setup(void)
{
	say_wait(GS_PROMPT_NOT_SETUP);
}

void gs_feedback_fault(void)
{
	say_wait(GS_PROMPT_FAULT);
}

int gs_feedback_selftest(void)
{
	int rc = gs_audio_power_on();
	if (rc == 0) {
		(void)gs_audio_selftest();
	}
	gs_audio_power_off();
	return rc;
}
