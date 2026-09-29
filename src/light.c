/*
 * GoSteady firmware — the cap's status light: LED1 plus the borrowed charge
 * LED (LED3). See light.h.
 *
 * LED3 hangs off the nPM1300's three LED sinks, which normally run in charger
 * modes (charging / error indication). Lighting it means switching each sink
 * to host mode and driving the SET/CLR latches over I²C; releasing clears the
 * latches and restores the modes read at boot. The PMIC keeps its registers
 * across an nRF9151 reset, so init also recovers a charge LED that a reset
 * caught borrowed (otherwise it could stay lit, 5 mA per channel, forever).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>
#include <string.h>

#include "light.h"

LOG_MODULE_REGISTER(gs_light, LOG_LEVEL_INF);

static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

#define PMIC_NODE DT_NODELABEL(pmic_main)
#if DT_NODE_HAS_STATUS(PMIC_NODE, okay) && defined(CONFIG_MFD_NPM13XX)
#define HAVE_CHARGE_LED 1
#include <zephyr/drivers/mfd/npm13xx.h>

static const struct device *const pmic = DEVICE_DT_GET(PMIC_NODE);

/* nPM1300 LEDDRV block — same offsets as zephyr drivers/led/led_npm13xx.c. */
#define LED_BASE      0x0AU
#define LED_MODE(n)   (0x00U + (n))
#define LED_SET(n)    (0x03U + 2U * (n))
#define LED_CLR(n)    (0x04U + 2U * (n))
#define MODE_ERROR    0U
#define MODE_CHARGING 1U
#define MODE_HOST     2U
#define N_CH          3U

/* The charger modes this board runs with (error / charging / host), read off
 * GS0002000003 on 2026-09-29. Only used when a reset caught the LED borrowed. */
static const uint8_t board_mode[N_CH] = { MODE_ERROR, MODE_CHARGING, MODE_HOST };

static uint8_t s_mode[N_CH];   /* charger's modes, restored on release */
static bool    s_ready;
static bool    s_borrowed;
static uint8_t s_lit;          /* LED3 channels currently lit (bit n = sink n) */

static void ch_write(uint8_t reg)
{
	(void)mfd_npm13xx_reg_write(pmic, LED_BASE, reg, 1U);
}

static void borrow(void)
{
	for (uint8_t n = 0; n < N_CH; n++) {
		ch_write(LED_CLR(n));            /* latch off before the mode flips */
	}
	for (uint8_t n = 0; n < N_CH; n++) {
		(void)mfd_npm13xx_reg_write(pmic, LED_BASE, LED_MODE(n), MODE_HOST);
	}
	s_borrowed = true;
	s_lit = 0;
}

static void hand_back(void)
{
	for (uint8_t n = 0; n < N_CH; n++) {
		ch_write(LED_CLR(n));
	}
	for (uint8_t n = 0; n < N_CH; n++) {
		(void)mfd_npm13xx_reg_write(pmic, LED_BASE, LED_MODE(n), s_mode[n]);
	}
	s_borrowed = false;
	s_lit = 0;
}
#endif /* HAVE_CHARGE_LED */

static K_MUTEX_DEFINE(s_lock);
static atomic_t s_bench;

int gs_light_init(void)
{
#if defined(HAVE_CHARGE_LED)
	if (!device_is_ready(pmic)) {
		LOG_WRN("nPM1300 not ready — status light is LED1 only");
		return -ENODEV;
	}
	for (uint8_t n = 0; n < N_CH; n++) {
		int rc = mfd_npm13xx_reg_read(pmic, LED_BASE, LED_MODE(n), &s_mode[n]);
		if (rc < 0) {
			LOG_WRN("charge LED mode read failed (%d) — status light is LED1 only", rc);
			return rc;
		}
	}
	bool left_borrowed = false;
	for (uint8_t n = 0; n < N_CH; n++) {
		if (s_mode[n] == MODE_HOST && board_mode[n] != MODE_HOST) {
			left_borrowed = true;
		}
	}
	if (left_borrowed) {
		LOG_WRN("charge LED left borrowed by a reset (modes %u/%u/%u) — handing it back",
			s_mode[0], s_mode[1], s_mode[2]);
		memcpy(s_mode, board_mode, sizeof(s_mode));
	}
	hand_back();   /* also clears a latch a reset left lit */
	s_ready = true;
	LOG_INF("status light: LED1 + charge LED (nPM1300 LED modes %u/%u/%u)",
		s_mode[0], s_mode[1], s_mode[2]);
#else
	LOG_INF("status light: LED1 only (no nPM1300)");
#endif
	return 0;
}

void gs_light_set(bool r, bool g, bool b)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	(void)gpio_pin_set_dt(&led_red, r);
	(void)gpio_pin_set_dt(&led_green, g);
	(void)gpio_pin_set_dt(&led_blue, b);
#if defined(HAVE_CHARGE_LED)
	uint8_t want = (r ? BIT(0) : 0) | (g ? BIT(1) : 0) | (b ? BIT(2) : 0);

	/* Dark keeps the LED borrowed (a pulsing flow shouldn't flash the
	 * charger's colour between pulses); release hands it back. */
	if (s_ready && want && !s_borrowed) {
		borrow();
	}
	if (s_borrowed) {
		for (uint8_t n = 0; n < N_CH; n++) {
			if ((want ^ s_lit) & BIT(n)) {
				ch_write((want & BIT(n)) ? LED_SET(n) : LED_CLR(n));
			}
		}
		s_lit = want;
	}
#endif
	k_mutex_unlock(&s_lock);
}

void gs_light_release(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	(void)gpio_pin_set_dt(&led_red, 0);
	(void)gpio_pin_set_dt(&led_green, 0);
	(void)gpio_pin_set_dt(&led_blue, 0);
#if defined(HAVE_CHARGE_LED)
	if (s_borrowed) {
		hand_back();
	}
#endif
	atomic_set(&s_bench, 0);
	k_mutex_unlock(&s_lock);
}

bool gs_light_bench(const char *colour)
{
	static const struct {
		const char *name;
		bool r, g, b;
	} colours[] = {
		{ "red", 1, 0, 0 }, { "green", 0, 1, 0 }, { "blue", 0, 0, 1 },
		{ "magenta", 1, 0, 1 }, { "cyan", 0, 1, 1 }, { "yellow", 1, 1, 0 },
		{ "white", 1, 1, 1 },
	};

	if (strcmp(colour, "off") == 0) {
		gs_light_release();
		return true;
	}
	for (size_t i = 0; i < ARRAY_SIZE(colours); i++) {
		if (strcmp(colour, colours[i].name) == 0) {
			atomic_set(&s_bench, 1);   /* pauses main.c's bench blink */
			gs_light_set(colours[i].r, colours[i].g, colours[i].b);
			return true;
		}
	}
	return false;
}

bool gs_light_bench_active(void)
{
	return atomic_get(&s_bench) != 0;
}
