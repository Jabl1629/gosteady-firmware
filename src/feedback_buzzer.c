/*
 * GoSteady firmware — Family Assistance feedback backend: SparkFun Qwiic
 * Buzzer (BOB-24474, ATtiny84 + magnetic buzzer) over I²C.
 *
 * Hardware (portal spec family-assistance-alert.md §4, v0.4):
 *   - Plugs into the Thingy:91 X P1 Qwiic connector with a plain Qwiic cable.
 *     On an unmodified board that is the sensor I²C bus (i2c2, P0.08/P0.09,
 *     SB8/SB9 intact) through the TXS0102 level shifter —
 *     boards/assist_buzzer_i2c2.overlay. On the 2026-09-18 bench unit whose
 *     SB8/SB9 were cut for the speaker experiment, P1 pins 3/4 are P0.18/P0.19
 *     and a bit-banged `gpio-i2c` bus drives the same node —
 *     boards/assist_buzzer_bitbang.overlay. Either way the driver just uses
 *     the `qwiic_buzzer` devicetree node.
 *   - Power: VDD_EXP_BRD (nPM1300 BUCK2 3.3 V via load switch U14, enable
 *     P0.03 = exp_board_enable). The ATtiny only IDLE-sleeps, so the board
 *     stays unpowered between incidents (spec L8). ~95 mA while sounding at
 *     volume 4 (SparkFun).
 *
 * Register map (SparkFun_Qwiic_Buzzer_Arduino_Library sfDevBuzzerRegisters.h):
 *   0x00 ID (=0x5E)  0x01/0x02 FW minor/major  0x03/0x04 TONE_FREQUENCY MSB/LSB
 *   0x05 VOLUME (0 off … 4 max)  0x06/0x07 DURATION MSB/LSB (ms; 0 = until off)
 *   0x08 ACTIVE (1 start; self-clears when the duration elapses)
 *   0x09 SAVE_SETTINGS  0x0A I2C_ADDRESS (default 0x34)
 * Resonant (loudest) frequency 2730 Hz.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/logging/log.h>

#include "feedback.h"

LOG_MODULE_REGISTER(gs_buzzer, LOG_LEVEL_INF);

#define BUZZER_NODE DT_NODELABEL(qwiic_buzzer)
#if !DT_NODE_HAS_STATUS(BUZZER_NODE, okay)
#error "qwiic_buzzer node missing — build with boards/assist_buzzer_i2c2.overlay (or _bitbang on a cut board)"
#endif
#define EXP_PWR_NODE DT_NODELABEL(exp_board_enable)
#if !DT_NODE_HAS_STATUS(EXP_PWR_NODE, okay)
#error "exp_board_enable (P0.03 load switch) missing from the board DTS"
#endif

static const struct i2c_dt_spec buzzer = I2C_DT_SPEC_GET(BUZZER_NODE);
static const struct device *const exp_pwr = DEVICE_DT_GET(EXP_PWR_NODE);

#define REG_ID          0x00
#define REG_FW_MINOR    0x01
#define REG_FW_MAJOR    0x02
#define REG_TONE_MSB    0x03
#define REG_ACTIVE      0x08
#define DEVICE_ID       0x5E

#define VOL             CONFIG_GOSTEADY_ASSIST_BUZZER_VOLUME
#define F_RES           CONFIG_GOSTEADY_ASSIST_BUZZER_FREQ_HZ   /* 2730 = loudest */
#define F_MID           2200
#define F_LOW           1500
#define F_DEEP          1000
#define BOOT_TIMEOUT_MS 400

static bool s_inited;
static bool s_powered;
static bool s_present;    /* answered the ID query this power cycle */

/* ---- low-level ---- */

static int reg_write_u8(uint8_t reg, uint8_t val)
{
	uint8_t buf[2] = { reg, val };
	return i2c_write_dt(&buzzer, buf, sizeof(buf));
}

static int reg_read_u8(uint8_t reg, uint8_t *val)
{
	return i2c_write_read_dt(&buzzer, &reg, 1, val, 1);
}

/* Bench diagnostic: which addresses ACK on the buzzer's bus right now. */
static void bus_scan(const char *tag)
{
	char found[96];
	int n = 0;
	for (uint8_t a = 0x08; a < 0x78; a++) {
		uint8_t b;
		if (i2c_read(buzzer.bus, &b, 1, a) == 0) {
			n += snprintk(found + n, sizeof(found) - n, " 0x%02x", a);
			if (n >= (int)sizeof(found) - 6) {
				break;
			}
		}
	}
	LOG_INF("i2c scan (%s):%s", tag, n ? found : " no ACKs");
}

/* One burst: freq MSB/LSB, volume, duration MSB/LSB, then ACTIVE=1.
 * duration_ms == 0 keeps sounding until note_off(). */
static int note_on(uint16_t freq_hz, uint16_t duration_ms, uint8_t volume)
{
	uint8_t cfg[6] = { REG_TONE_MSB,
			   (uint8_t)(freq_hz >> 8), (uint8_t)freq_hz,
			   volume,
			   (uint8_t)(duration_ms >> 8), (uint8_t)duration_ms };
	int rc = i2c_write_dt(&buzzer, cfg, sizeof(cfg));
	if (rc < 0) {
		return rc;
	}
	return reg_write_u8(REG_ACTIVE, 1);
}

static int note_off(void)
{
	return reg_write_u8(REG_ACTIVE, 0);
}

/* Blocking note: sound for `ms`, then a `gap_ms` rest. The buzzer times the
 * note itself; we just wait so the next note doesn't cut it. */
static void note(uint16_t freq_hz, uint16_t ms, uint16_t gap_ms)
{
	if (s_present) {
		(void)note_on(freq_hz, ms, VOL);
	}
	k_msleep(ms + gap_ms);
}

/* ---- public API ---- */

int gs_feedback_init(void)
{
	if (s_inited) {
		return 0;
	}
	if (!device_is_ready(buzzer.bus)) {
		LOG_ERR("buzzer I2C bus not ready");
		return -ENODEV;
	}
	if (!device_is_ready(exp_pwr)) {
		LOG_ERR("exp_board_enable regulator not ready");
		return -ENODEV;
	}
	s_inited = true;
	LOG_INF("qwiic buzzer bound: bus %s addr 0x%02x, VDD_EXP_BRD via P0.03 (off), vol %d, %d Hz",
		buzzer.bus->name, buzzer.addr, VOL, F_RES);
	return 0;
}

int gs_feedback_begin(void)
{
	if (!s_inited) {
		return -EINVAL;
	}
	if (s_powered) {
		return s_present ? 0 : -EIO;
	}
	int rc = regulator_enable(exp_pwr);
	if (rc < 0) {
		LOG_ERR("VDD_EXP_BRD enable failed (%d)", rc);
		return rc;
	}
	s_powered = true;
	s_present = false;

	/* ATtiny boot + EEPROM settings load: poll the ID register. */
	int64_t t0 = k_uptime_get();
	uint8_t id = 0;
	int rrc = -1;
	k_msleep(20);
	while ((k_uptime_get() - t0) < BOOT_TIMEOUT_MS) {
		rrc = reg_read_u8(REG_ID, &id);
		if (rrc == 0 && id == DEVICE_ID) {
			s_present = true;
			break;
		}
		k_msleep(20);
	}
	int64_t boot_ms = k_uptime_get() - t0;
	if (s_present) {
		uint8_t fw_major = 0, fw_minor = 0;
		(void)reg_read_u8(REG_FW_MAJOR, &fw_major);
		(void)reg_read_u8(REG_FW_MINOR, &fw_minor);
		(void)note_off();
		LOG_INF("buzzer: powered, ID 0x%02x fw %u.%u after %lld ms", id, fw_major, fw_minor, boot_ms);
		return 0;
	}
	LOG_WRN("buzzer: powered but no ID reply after %lld ms (rc %d, last 0x%02x) — LED-only incident", boot_ms, rrc, id);
	/* Diagnose: who answers with the rail on, then with it off (polarity check). */
	bus_scan("rail on");
	(void)regulator_disable(exp_pwr);
	k_msleep(150);
	bus_scan("rail off");
	(void)regulator_enable(exp_pwr);
	k_msleep(50);
	return -EIO;
}

void gs_feedback_end(void)
{
	if (!s_powered) {
		return;
	}
	if (s_present) {
		(void)note_off();
	}
	int rc = regulator_disable(exp_pwr);
	if (rc < 0) {
		LOG_ERR("VDD_EXP_BRD disable failed (%d)", rc);
	}
	s_powered = false;
	s_present = false;
	LOG_INF("buzzer: powered off");
}

/* ---- patterns ----
 * Design intent (older-adult legibility): one cadence per phase, always at
 * the loud resonant pitch; outcomes use pitch *movement* (rising = good,
 * falling = cancelled, deep + long = trouble) so they are distinguishable
 * without counting beeps. Spec §5.3 (v0.4). */

void gs_feedback_countdown_start(void)
{
	/* "Heard you": two quick beeps, then the 1 Hz cadence takes over. */
	note(F_RES, 80, 60);
	note(F_RES, 80, 0);
}

void gs_feedback_countdown_mid(void)
{
	/* Phase change marker: one longer beep. */
	note(F_RES, 250, 0);
}

void gs_feedback_tick(enum gs_fb_phase phase)
{
	switch (phase) {
	case GS_FB_PHASE_EARLY:
		note(F_RES, 120, 0);
		break;
	case GS_FB_PHASE_LATE:
		note(F_RES, 100, 80);
		note(F_RES, 100, 0);
		break;
	case GS_FB_PHASE_FINAL:
	default:
		note(F_RES, 70, 0);
		break;
	}
}

void gs_feedback_hold_tone(bool on)
{
	if (!s_present) {
		return;
	}
	if (on) {
		(void)note_on(F_LOW, 0, VOL);   /* continuous until off */
	} else {
		(void)note_off();
	}
}

void gs_feedback_cancelled(void)
{
	/* Falling two-note. */
	note(F_MID, 180, 40);
	note(F_DEEP, 450, 0);
}

void gs_feedback_confirmed(bool test_mode)
{
	/* Rising three-note; test adds a fourth top note. */
	note(F_LOW, 130, 40);
	note(F_MID, 130, 40);
	note(F_RES, 220, 60);
	if (test_mode) {
		note(F_RES, 220, 0);
	}
}

void gs_feedback_failed(void)
{
	/* Deep, long, twice. */
	note(F_DEEP, 600, 250);
	note(F_DEEP, 600, 0);
}

void gs_feedback_not_setup(void)
{
	/* Two low, unhurried beeps. */
	note(F_LOW, 250, 200);
	note(F_LOW, 250, 0);
}

void gs_feedback_fault(void)
{
	note(F_DEEP, 300, 100);
	note(F_DEEP, 300, 100);
	note(F_DEEP, 300, 0);
}

int gs_feedback_selftest(void)
{
	int rc = gs_feedback_begin();
	if (rc == 0) {
		LOG_INF("buzzer selftest: chirp");
		note(F_RES, 60, 60);
		note(F_RES, 60, 0);
		bus_scan("rail on, buzzer ok");
	}
	gs_feedback_end();
	return rc;
}
