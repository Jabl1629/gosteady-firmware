/*
 * GoSteady firmware — Family Assistance Alert incident state machine.
 * See assist.h and gosteady-portal/docs/specs/family-assistance-alert.md §5.
 *
 * Timeline (t = 0 at the debounced press):
 *   0.0  red LED on, power the speaker module, say PRESSED_20S (~1.5 s after
 *        press because the module has to boot), kick the cloud connect.
 *  10.0  TONE, then CONTACTING_10S.
 *  0–20  a second deliberate press → CANCELLED prompt, nothing is sent.
 *  20.0  publish the request; wait ≤ ACK_WAIT_S for `assist_ack`; retry.
 *  ack   say CONTACTED (or TEST_OK); done.
 *  none  say RETRYING after the last attempt (FA-1 adds persistence +
 *        background retry; the bench stub never fails).
 *
 * The incident never touches session state (spec §5.2).
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/atomic.h>
#include <stdio.h>
#include <string.h>

#include "assist.h"
#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO)
#include "audio_dfr0534.h"
#endif

LOG_MODULE_REGISTER(gs_assist, LOG_LEVEL_INF);

#define COUNTDOWN_MS   (CONFIG_GOSTEADY_ASSIST_COUNTDOWN_S * 1000)
#define MIDPROMPT_MS   (CONFIG_GOSTEADY_ASSIST_MIDPROMPT_S * 1000)
#define ACK_WAIT       K_SECONDS(CONFIG_GOSTEADY_ASSIST_ACK_WAIT_S)
#define MAX_ATTEMPTS   3
#define DEBOUNCE_MS    50
#define REPRESS_GUARD_MS 300   /* ignore edges this soon after the first press */
#define PROMPT_WAIT    K_SECONDS(8)

/* ---- button + LEDs (own specs; main.c configures the pins at boot) ---- */
static const struct gpio_dt_spec button    = GPIO_DT_SPEC_GET(DT_ALIAS(sw0), gpios);
static const struct gpio_dt_spec led_red   = GPIO_DT_SPEC_GET(DT_ALIAS(led0), gpios);
static const struct gpio_dt_spec led_green = GPIO_DT_SPEC_GET(DT_ALIAS(led1), gpios);
static const struct gpio_dt_spec led_blue  = GPIO_DT_SPEC_GET(DT_ALIAS(led2), gpios);

static void led(bool r, bool g, bool b)
{
	(void)gpio_pin_set_dt(&led_red, r);
	(void)gpio_pin_set_dt(&led_green, g);
	(void)gpio_pin_set_dt(&led_blue, b);
}

/* ---- state ---- */
static K_SEM_DEFINE(press_sem, 0, 1);
static K_SEM_DEFINE(ack_sem, 0, 1);
static atomic_t s_active;
static bool s_armed = IS_ENABLED(CONFIG_GOSTEADY_ASSIST_STUB_CLOUD);

struct incident {
	char     id[37];
	uint8_t  seq;
	int64_t  pressed_uptime_ms;
	int64_t  sent_uptime_ms;
};
static struct incident s_inc;

/* ack payload copied in from ISR/timer context */
static struct {
	char id[37];
	bool test_mode;
	int  status;
} s_ack;

K_THREAD_STACK_DEFINE(assist_stack, 2048);
static struct k_thread assist_thread;

/* ---- public ISR-safe entry points ---- */

void gs_assist_button_isr(void)
{
	k_sem_give(&press_sem);
}

bool gs_assist_is_active(void)
{
	return atomic_get(&s_active) != 0;
}

void gs_assist_set_armed(bool armed)
{
	s_armed = armed;
	LOG_INF("assist: %s", armed ? "ARMED" : "disarmed");
}

bool gs_assist_is_armed(void)
{
	return s_armed;
}

void gs_assist_ack_received(const char *incident_id, bool test_mode, int status)
{
	if (!incident_id) {
		return;
	}
	strncpy(s_ack.id, incident_id, sizeof(s_ack.id) - 1);
	s_ack.id[sizeof(s_ack.id) - 1] = '\0';
	s_ack.test_mode = test_mode;
	s_ack.status = status;
	k_sem_give(&ack_sem);
}

/* ---- audio helpers (no-ops without the audio driver) ---- */

static void audio_on(void)
{
#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO)
	(void)gs_audio_power_on();
#endif
}

static void audio_off(void)
{
#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO)
	gs_audio_power_off();
#endif
}

static void say(int prompt)
{
#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO)
	(void)gs_audio_play((uint16_t)prompt);
#else
	LOG_INF("assist: [prompt %d] (no audio driver)", prompt);
#endif
}

static void say_wait(int prompt)
{
	say(prompt);
#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO)
	(void)gs_audio_wait_done(PROMPT_WAIT);
#else
	k_msleep(800);
#endif
}

/* ---- transport ---- */

#if defined(CONFIG_GOSTEADY_ASSIST_STUB_CLOUD)
static void stub_ack_expiry(struct k_timer *t)
{
	ARG_UNUSED(t);
	gs_assist_ack_received(s_inc.id, false, 0);
}
static K_TIMER_DEFINE(stub_ack_timer, stub_ack_expiry, NULL);
#endif

static int transport_send(struct incident *inc)
{
	inc->sent_uptime_ms = k_uptime_get();
#if defined(CONFIG_GOSTEADY_ASSIST_STUB_CLOUD)
	LOG_INF("assist: [STUB] publish gs/<serial>/assist event=request incident=%s seq=%u "
		"pressed_uptime=%lld sent_uptime=%lld — simulated ack in %d ms",
		inc->id, inc->seq, inc->pressed_uptime_ms, inc->sent_uptime_ms,
		CONFIG_GOSTEADY_ASSIST_STUB_ACK_MS);
	k_timer_start(&stub_ack_timer, K_MSEC(CONFIG_GOSTEADY_ASSIST_STUB_ACK_MS), K_NO_WAIT);
	return 0;
#else
	/* FA-2: gosteady_cloud_assist_publish(inc) — priority-0 connect + publish
	 * (spec §5.4/§5.10). Not wired yet. */
	LOG_ERR("assist: no cloud transport compiled in");
	return -ENOSYS;
#endif
}

/* ---- helpers ---- */

static void new_incident(struct incident *inc, int64_t pressed_ms)
{
	uint8_t r[16];

	sys_rand_get(r, sizeof(r));
	r[6] = (r[6] & 0x0F) | 0x40;   /* UUID v4 */
	r[8] = (r[8] & 0x3F) | 0x80;
	snprintf(inc->id, sizeof(inc->id),
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
		 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7],
		 r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
	inc->seq = 0;
	inc->pressed_uptime_ms = pressed_ms;
	inc->sent_uptime_ms = 0;
}

/* Re-read the pin after the bounce window; true iff still pressed. */
static bool debounced_press(void)
{
	k_msleep(DEBOUNCE_MS);
	return gpio_pin_get_dt(&button) == 1;
}

static void finish(void)
{
	audio_off();
	led(0, 0, 0);
	k_sem_reset(&press_sem);
	k_sem_reset(&ack_sem);
	atomic_set(&s_active, 0);
	LOG_INF("assist: idle");
}

/* ---- the incident thread ---- */

static void assist_entry(void *a, void *b, void *c)
{
	ARG_UNUSED(a); ARG_UNUSED(b); ARG_UNUSED(c);

	LOG_INF("assist thread up (countdown %d s, mid-prompt %d s, ack wait %d s, %s)",
		CONFIG_GOSTEADY_ASSIST_COUNTDOWN_S, CONFIG_GOSTEADY_ASSIST_MIDPROMPT_S,
		CONFIG_GOSTEADY_ASSIST_ACK_WAIT_S,
		IS_ENABLED(CONFIG_GOSTEADY_ASSIST_STUB_CLOUD) ? "STUB cloud" : "live cloud");

	for (;;) {
		k_sem_take(&press_sem, K_FOREVER);
		if (!debounced_press()) {
			continue;
		}
		int64_t t0 = k_uptime_get();
		atomic_set(&s_active, 1);
		k_sem_reset(&press_sem);
		led(1, 0, 0);

		if (!s_armed) {
			LOG_WRN("assist: press while NOT armed — no request");
			audio_on();
			say_wait(GS_PROMPT_NOT_SETUP);
			finish();
			continue;
		}

		new_incident(&s_inc, t0);
		LOG_INF("assist: PENDING incident=%s", s_inc.id);

		/* Speaker boot + first prompt (~1.5 s), cloud connect would start
		 * here too (FA-2). */
		audio_on();
		LOG_INF("assist: t+%lld ms first prompt", k_uptime_get() - t0);
		say(GS_PROMPT_PRESSED_20S);

		/* Countdown — a second press cancels; mid-prompt at MIDPROMPT_MS. */
		bool cancelled = false;
		bool mid_done = false;

		for (;;) {
			int64_t now = k_uptime_get() - t0;
			if (now >= COUNTDOWN_MS) {
				break;
			}
			int64_t next = mid_done ? COUNTDOWN_MS : MIDPROMPT_MS;
			int64_t wait = next - now;
			if (wait < 0) {
				wait = 0;
			}
			if (k_sem_take(&press_sem, K_MSEC(wait)) == 0) {
				if ((k_uptime_get() - t0) > REPRESS_GUARD_MS && debounced_press()) {
					cancelled = true;
					break;
				}
				continue;
			}
			if (!mid_done) {
				mid_done = true;
				LOG_INF("assist: t+%lld ms mid-prompt", k_uptime_get() - t0);
				say_wait(GS_PROMPT_TONE);
				say(GS_PROMPT_CONTACTING_10S);
			}
		}

		if (cancelled) {
			LOG_INF("assist: CANCELLED at t+%lld ms", k_uptime_get() - t0);
			say_wait(GS_PROMPT_CANCELLED);
			finish();
			continue;
		}

		/* SENDING / AWAIT_ACK */
		led(1, 0, 1);
		k_sem_reset(&ack_sem);
		bool acked = false;
		for (int attempt = 1; attempt <= MAX_ATTEMPTS && !acked; attempt++) {
			s_inc.seq = (uint8_t)attempt;
			LOG_INF("assist: SENDING attempt %d at t+%lld ms", attempt, k_uptime_get() - t0);
			if (transport_send(&s_inc) < 0) {
				break;
			}
			if (k_sem_take(&ack_sem, ACK_WAIT) == 0) {
				if (strcmp(s_ack.id, s_inc.id) == 0) {
					acked = true;
				} else {
					LOG_WRN("assist: ack for a different incident (%s) — ignored", s_ack.id);
				}
			} else {
				LOG_WRN("assist: no ack within %d s", CONFIG_GOSTEADY_ASSIST_ACK_WAIT_S);
			}
		}

		if (acked && s_ack.status == 0) {
			LOG_INF("assist: CONFIRMED (%s) at t+%lld ms", s_ack.test_mode ? "test" : "live",
				k_uptime_get() - t0);
			led(0, 1, 0);
			say_wait(s_ack.test_mode ? GS_PROMPT_TEST_OK : GS_PROMPT_CONTACTED);
		} else if (acked) {
			LOG_WRN("assist: cloud says not_ready — disarming");
			s_armed = false;
			say_wait(GS_PROMPT_NOT_SETUP);
		} else {
			LOG_ERR("assist: FAILED to get an ack after %d attempts", MAX_ATTEMPTS);
			say_wait(GS_PROMPT_RETRYING);
			/* FA-1: persist /lfs/assist/pending.json + background retry. */
		}
		finish();
	}
}

int gs_assist_start(void)
{
	if (!gpio_is_ready_dt(&button)) {
		return -ENODEV;
	}
	k_thread_create(&assist_thread, assist_stack, K_THREAD_STACK_SIZEOF(assist_stack),
			assist_entry, NULL, NULL, NULL, 6, 0, K_NO_WAIT);
	k_thread_name_set(&assist_thread, "gs_assist");
	return 0;
}
