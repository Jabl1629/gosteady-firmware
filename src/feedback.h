/*
 * GoSteady firmware — Family Assistance Alert user-feedback abstraction.
 *
 * assist.c drives the incident timeline; this layer turns its events into
 * something the walker user can hear. Two backends, selected by the
 * GOSTEADY_ASSIST_FEEDBACK Kconfig choice:
 *
 *   - feedback_buzzer.c   SparkFun Qwiic Buzzer (BOB-24474) on the P1 Qwiic
 *                         connector — the product direction (2026-09-18).
 *                         Beep cadences; no speech.
 *   - feedback_speaker.c  DFR0534 MP3 module with spoken prompts — archived,
 *                         bench-proven 2026-09-18, kept selectable.
 *
 * Both are powered only while an incident runs (VDD_EXP_BRD load switch,
 * P0.03) — spec L8.
 *
 * All calls run on the assist thread. Pattern calls may block for the length
 * of the pattern (≤ ~1.5 s); ticks and the hold tone return immediately.
 */

#ifndef GOSTEADY_FEEDBACK_H_
#define GOSTEADY_FEEDBACK_H_

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Countdown phases (assist.c decides when each starts). */
enum gs_fb_phase {
	GS_FB_PHASE_EARLY = 0,   /* 0 … MIDPROMPT_S: one short beep per second */
	GS_FB_PHASE_LATE,        /* MIDPROMPT_S … last 3 s: double beep per second */
	GS_FB_PHASE_FINAL,       /* last 3 s: rapid beeps */
};

#if defined(CONFIG_GOSTEADY_ASSIST_FEEDBACK_BUZZER) || defined(CONFIG_GOSTEADY_ASSIST_FEEDBACK_SPEAKER)

/* Bind devices; feedback hardware stays unpowered. */
int  gs_feedback_init(void);

/* Power the feedback hardware and get it ready (buzzer ID check / speaker
 * boot). Returns 0 when the device answered, -EIO when it did not (the
 * incident continues LED-only), other negative errno on power failure. */
int  gs_feedback_begin(void);

/* Silence + power off. Idempotent. */
void gs_feedback_end(void);

/* Countdown events. */
void gs_feedback_countdown_start(void);          /* t = 0 (press accepted) */
void gs_feedback_countdown_mid(void);            /* t = MIDPROMPT_S */
void gs_feedback_tick(enum gs_fb_phase phase);   /* once per cadence slot */

/* Hold-to-cancel progress: steady low tone while the button is held past the
 * warn threshold; off on release. */
void gs_feedback_hold_tone(bool on);

/* Outcomes (blocking patterns). */
void gs_feedback_cancelled(void);
void gs_feedback_confirmed(bool test_mode);
void gs_feedback_failed(void);
void gs_feedback_not_setup(void);
void gs_feedback_fault(void);

/* Bench: power up, identify, make a noise, power down. Returns 0 on success. */
int  gs_feedback_selftest(void);

#else /* LED-only builds: no-ops so assist.c compiles unchanged */

static inline int  gs_feedback_init(void) { return 0; }
static inline int  gs_feedback_begin(void) { return 0; }
static inline void gs_feedback_end(void) {}
static inline void gs_feedback_countdown_start(void) {}
static inline void gs_feedback_countdown_mid(void) {}
static inline void gs_feedback_tick(enum gs_fb_phase phase) { (void)phase; }
static inline void gs_feedback_hold_tone(bool on) { (void)on; }
static inline void gs_feedback_cancelled(void) {}
static inline void gs_feedback_confirmed(bool test_mode) { (void)test_mode; }
static inline void gs_feedback_failed(void) {}
static inline void gs_feedback_not_setup(void) {}
static inline void gs_feedback_fault(void) {}
static inline int  gs_feedback_selftest(void) { return 0; }

#endif

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_FEEDBACK_H_ */
