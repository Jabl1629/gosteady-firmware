/*
 * GoSteady firmware — the cap's status light for the user-facing flows:
 * waiting-to-activate (blue pulse, main.c) and Family Assistance (assist.c).
 *
 * The light has to make it through the bottom cap, so these flows light every
 * LED the nRF9151 can reach in the same colour:
 *   - LED1  RGB on nRF9151 GPIOs (P0.29 R / P0.31 G / P0.30 B) via MOSFETs,
 *           ~10 mA per channel from VDD_LED (3.3 V).
 *   - LED3  the "charge LED": RGB on the nPM1300 LED sinks, 5 mA each —
 *           R on LED0 (charger-error mode), G on LED1 (charging mode), B on
 *           LED2 (unused). The schematic nets CHG_LED/ERR_LED are the other
 *           way round; the modes above are what the PMIC runs. Borrowed in
 *           host mode while lit and handed back to the charger on release.
 * Out of reach: LED2 (wired to the nRF5340 bridge MCU — needs a custom
 * connectivity-bridge build) and the Qwiic buzzer's LEDs (red PWR on its rail,
 * blue STAT only while it sounds, ~0.3 mA each). Session green and the bench
 * purple blink stay LED1-only in main.c.
 */

#ifndef GOSTEADY_LIGHT_H_
#define GOSTEADY_LIGHT_H_

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read the charger's LED modes (main.c has already configured the LED1 pins).
 * Hands LED3 back to the charger if a reset left it borrowed. */
int  gs_light_init(void);

/* LED1 + LED3 to this colour (borrows LED3 on first use). */
void gs_light_set(bool r, bool g, bool b);

/* Beat mode (assistance countdown): the light shows this colour only while
 * a beep sounds and is dark in between — feedback_buzzer.c brackets every
 * note with gs_light_flash(true/false), a no-op outside beat mode. Full
 * brightness is the hardware's: LED1 ~13 mA red (100 Ω), LED3 5 mA (fixed
 * PMIC sink). gs_light_set()/release() end it. */
void gs_light_beat(bool r, bool g, bool b);
void gs_light_flash(bool on);

/* Everything dark; LED3 back under charger control. Idempotent. */
void gs_light_release(void);

/* Bench (control channel "LED <colour>"): hold a colour so it can be judged
 * through the cap; "off" releases. Returns false for an unknown colour. */
bool gs_light_bench(const char *colour);
bool gs_light_bench_active(void);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_LIGHT_H_ */
