/*
 * GoSteady firmware — DFR0534 (JQ8400-class) MP3/voice module driver.
 * See audio_dfr0534.h for the wiring/protocol summary and the portal spec
 * family-assistance-alert.md §4 for the hardware rationale.
 *
 * Transport: uart1 re-pinned onto the P1 expansion connector
 * (boards/assist_audio_uart1.overlay). TX is polled (frames are ≤ 6 bytes at
 * 9600 baud ≈ 6 ms); RX is interrupt-driven into a small ring buffer, same
 * style as src/dump.c.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/drivers/regulator.h>
#include <zephyr/sys/ring_buffer.h>
#include <zephyr/logging/log.h>
#include <string.h>

#include "audio_dfr0534.h"

LOG_MODULE_REGISTER(gs_audio, LOG_LEVEL_INF);

#if defined(CONFIG_GOSTEADY_ASSIST_AUDIO_XPORT_UART1)
#define AUDIO_UART_NODE DT_NODELABEL(uart1)
#else
#error "No audio transport selected (CONFIG_GOSTEADY_ASSIST_AUDIO_XPORT_*)"
#endif

#if !DT_NODE_HAS_STATUS(AUDIO_UART_NODE, okay)
#error "audio UART node not enabled — build with -DEXTRA_DTC_OVERLAY_FILE=boards/assist_audio_uart1.overlay"
#endif

#define EXP_PWR_NODE DT_NODELABEL(exp_board_enable)
#if !DT_NODE_HAS_STATUS(EXP_PWR_NODE, okay)
#error "exp_board_enable (P0.03 load switch) missing from the board DTS"
#endif

static const struct device *const audio_uart = DEVICE_DT_GET(AUDIO_UART_NODE);
static const struct device *const exp_pwr    = DEVICE_DT_GET(EXP_PWR_NODE);

/* ---- protocol constants ---- */
#define FRAME_START        0xAA
#define CMD_STATUS         0x01
#define CMD_PLAY           0x02
#define CMD_PAUSE          0x03
#define CMD_STOP           0x04
#define CMD_PLAY_TRACK     0x07
#define CMD_TRACK_COUNT    0x0C
#define CMD_END            0x10
#define CMD_VOLUME         0x13
#define CMD_LOOP_MODE      0x18
#define CMD_SHORT_NAME     0x1E
#define CMD_SELECT_NOPLAY  0x1F
#define LOOP_MODE_SINGLE_STOP 0x03   /* play once, then stop (datasheet example AA 18 01 03 C6) */

#define MAX_FRAME          32        /* longest reply we care about (short name ≤ 12 + 4) */
#define RX_RING_SZ         128
#define QUERY_TIMEOUT      K_MSEC(250)

static uint8_t          rx_ring_buf[RX_RING_SZ];
static struct ring_buf  rx_ring;
static K_SEM_DEFINE(rx_sem, 0, 1);
static K_MUTEX_DEFINE(s_lock);
static bool s_inited;
static bool s_powered;

/* ---- UART RX ISR → ring buffer ---- */

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	uint8_t chunk[16];

	while (uart_irq_update(dev) && uart_irq_rx_ready(dev)) {
		int n = uart_fifo_read(dev, chunk, sizeof(chunk));
		if (n <= 0) {
			break;
		}
		(void)ring_buf_put(&rx_ring, chunk, n);
		k_sem_give(&rx_sem);
	}
}

static void rx_flush(void)
{
	ring_buf_reset(&rx_ring);
	k_sem_reset(&rx_sem);
}

/* ---- framing ---- */

static void send_frame(uint8_t cmd, const uint8_t *data, uint8_t len)
{
	uint8_t f[4 + 16];
	uint32_t sum = 0;

	if (len > 16) {
		len = 16;
	}
	f[0] = FRAME_START;
	f[1] = cmd;
	f[2] = len;
	if (len) {
		memcpy(&f[3], data, len);
	}
	for (int i = 0; i < 3 + len; i++) {
		sum += f[i];
	}
	f[3 + len] = (uint8_t)(sum & 0xFF);
	for (int i = 0; i < 4 + len; i++) {
		uart_poll_out(audio_uart, f[i]);
	}
}

/* Wait for a reply frame whose cmd byte == want_cmd. Copies its data bytes
 * into `data` (cap `*len`), sets *len to the data length. Frames for other
 * commands are discarded. Returns 0, -ETIMEDOUT, or -EBADMSG (checksum). */
static int recv_frame(uint8_t want_cmd, uint8_t *data, uint8_t *len, k_timeout_t timeout)
{
	uint8_t acc[MAX_FRAME];
	size_t have = 0;
	k_timepoint_t end = sys_timepoint_calc(timeout);

	for (;;) {
		/* Drain whatever has arrived so far. */
		while (have < sizeof(acc)) {
			uint8_t b;
			if (ring_buf_get(&rx_ring, &b, 1) != 1) {
				break;
			}
			acc[have++] = b;
		}
		/* Scan for a complete frame. */
		size_t i = 0;
		while (i < have && acc[i] != FRAME_START) {
			i++;
		}
		if (i > 0) {
			memmove(acc, &acc[i], have - i);
			have -= i;
		}
		if (have >= 3) {
			uint8_t flen = acc[2];
			size_t need = 4 + (size_t)flen;
			if (need > sizeof(acc)) {
				/* implausible length — resync */
				memmove(acc, &acc[1], have - 1);
				have -= 1;
				continue;
			}
			if (have >= need) {
				uint32_t sum = 0;
				for (size_t k = 0; k < need - 1; k++) {
					sum += acc[k];
				}
				bool ok = ((sum & 0xFF) == acc[need - 1]);
				uint8_t cmd = acc[1];
				if (ok && cmd == want_cmd) {
					uint8_t n = flen;
					if (data && len) {
						if (n > *len) {
							n = *len;
						}
						memcpy(data, &acc[3], n);
						*len = n;
					}
					memmove(acc, &acc[need], have - need);
					have -= need;
					return 0;
				}
				if (!ok) {
					LOG_DBG("bad checksum on cmd 0x%02x", cmd);
				}
				memmove(acc, &acc[need], have - need);
				have -= need;
				continue;
			}
		}
		/* Need more bytes. */
		if (sys_timepoint_expired(end)) {
			return -ETIMEDOUT;
		}
		(void)k_sem_take(&rx_sem, sys_timepoint_timeout(end));
	}
}

/* ---- public API ---- */

int gs_audio_init(void)
{
	if (s_inited) {
		return 0;
	}
	if (!device_is_ready(audio_uart)) {
		LOG_ERR("audio uart not ready");
		return -ENODEV;
	}
	if (!device_is_ready(exp_pwr)) {
		LOG_ERR("exp_board_enable regulator not ready");
		return -ENODEV;
	}
	ring_buf_init(&rx_ring, sizeof(rx_ring_buf), rx_ring_buf);
	uart_irq_callback_set(audio_uart, uart_isr);
	uart_irq_rx_enable(audio_uart);
	s_inited = true;
	LOG_INF("dfr0534 driver ready (uart1 → P1: TX P0.19, RX P0.18 @ 9600; VDD_EXP_BRD via P0.03, off)");
	return 0;
}

static int query_status_locked(uint8_t *status, k_timeout_t timeout)
{
	uint8_t d[4];
	uint8_t n = sizeof(d);

	rx_flush();
	send_frame(CMD_STATUS, NULL, 0);
	int rc = recv_frame(CMD_STATUS, d, &n, timeout);
	if (rc == 0) {
		*status = (n >= 1) ? d[0] : 0xFF;
	}
	return rc;
}

int gs_audio_power_on(void)
{
	if (!s_inited) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (s_powered) {
		k_mutex_unlock(&s_lock);
		return 0;
	}
	rx_flush();
	int rc = regulator_enable(exp_pwr);
	if (rc < 0) {
		LOG_ERR("VDD_EXP_BRD enable failed (%d)", rc);
		k_mutex_unlock(&s_lock);
		return rc;
	}
	s_powered = true;

	/* Module boot: poll the status query until it answers. */
	int64_t t0 = k_uptime_get();
	bool ready = false;
	uint8_t st = 0xFF;

	k_msleep(CONFIG_GOSTEADY_ASSIST_AUDIO_BOOT_MIN_MS);
	while ((k_uptime_get() - t0) < CONFIG_GOSTEADY_ASSIST_AUDIO_BOOT_TIMEOUT_MS) {
		if (query_status_locked(&st, K_MSEC(150)) == 0) {
			ready = true;
			break;
		}
		k_msleep(100);
	}
	int64_t boot_ms = k_uptime_get() - t0;

	/* Volume + single-play mode every power-up (the module's persistence of
	 * either across power cycles is undocumented). */
	uint8_t vol = CONFIG_GOSTEADY_ASSIST_AUDIO_VOLUME;
	send_frame(CMD_VOLUME, &vol, 1);
	k_msleep(20);
	uint8_t mode = LOOP_MODE_SINGLE_STOP;
	send_frame(CMD_LOOP_MODE, &mode, 1);
	k_msleep(20);

	if (ready) {
		LOG_INF("audio: powered, module answered after %lld ms (status=%u), volume=%u",
			boot_ms, st, vol);
	} else {
		LOG_WRN("audio: powered but NO reply after %lld ms — check RX wiring (P0.18 ← module T) / SB9 cut; trying TX-only",
			boot_ms);
	}
	k_mutex_unlock(&s_lock);
	return ready ? 0 : -EIO;
}

void gs_audio_power_off(void)
{
	if (!s_inited) {
		return;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (s_powered) {
		send_frame(CMD_STOP, NULL, 0);
		k_msleep(30);
		int rc = regulator_disable(exp_pwr);
		if (rc < 0) {
			LOG_ERR("VDD_EXP_BRD disable failed (%d)", rc);
		}
		s_powered = false;
		rx_flush();
		LOG_INF("audio: powered off");
	}
	k_mutex_unlock(&s_lock);
}

bool gs_audio_is_powered(void)
{
	return s_powered;
}

int gs_audio_play(uint16_t track)
{
	if (!s_inited) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_powered) {
		k_mutex_unlock(&s_lock);
		return -ENOTCONN;
	}
	uint8_t d[2] = { (uint8_t)(track >> 8), (uint8_t)(track & 0xFF) };
	rx_flush();
	send_frame(CMD_PLAY_TRACK, d, 2);
	LOG_INF("audio: play track %u", track);
	k_mutex_unlock(&s_lock);
	return 0;
}

int gs_audio_stop(void)
{
	if (!s_inited) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (s_powered) {
		send_frame(CMD_STOP, NULL, 0);
	}
	k_mutex_unlock(&s_lock);
	return 0;
}

int gs_audio_set_volume(uint8_t volume)
{
	if (!s_inited) {
		return -EINVAL;
	}
	if (volume > 30) {
		volume = 30;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_powered) {
		k_mutex_unlock(&s_lock);
		return -ENOTCONN;
	}
	send_frame(CMD_VOLUME, &volume, 1);
	k_mutex_unlock(&s_lock);
	return 0;
}

int gs_audio_query_status(uint8_t *status, k_timeout_t timeout)
{
	if (!s_inited || !status) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_powered) {
		k_mutex_unlock(&s_lock);
		return -ENOTCONN;
	}
	int rc = query_status_locked(status, timeout);
	k_mutex_unlock(&s_lock);
	return rc;
}

int gs_audio_query_track_count(uint16_t *count, k_timeout_t timeout)
{
	if (!s_inited || !count) {
		return -EINVAL;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_powered) {
		k_mutex_unlock(&s_lock);
		return -ENOTCONN;
	}
	uint8_t d[4];
	uint8_t n = sizeof(d);
	rx_flush();
	send_frame(CMD_TRACK_COUNT, NULL, 0);
	int rc = recv_frame(CMD_TRACK_COUNT, d, &n, timeout);
	if (rc == 0) {
		*count = (n >= 2) ? (uint16_t)((d[0] << 8) | d[1]) : 0;
	}
	k_mutex_unlock(&s_lock);
	return rc;
}

int gs_audio_wait_done(k_timeout_t timeout)
{
	if (!s_inited) {
		return -EINVAL;
	}
	k_timepoint_t end = sys_timepoint_calc(timeout);
	k_timepoint_t start_deadline = sys_timepoint_calc(K_MSEC(1200));
	bool seen_playing = false;
	int silent = 0;

	for (;;) {
		uint8_t st = 0xFF;
		int rc = gs_audio_query_status(&st, QUERY_TIMEOUT);
		if (rc == -ENOTCONN) {
			return rc;
		}
		if (rc < 0) {
			if (++silent >= 8) {
				return -EIO;   /* module not answering at all */
			}
		} else {
			silent = 0;
			if (st == GS_AUDIO_STATUS_PLAYING) {
				seen_playing = true;
			} else if (st == GS_AUDIO_STATUS_STOPPED) {
				if (seen_playing) {
					return 0;
				}
				/* Not started yet (or already over for a very short
				 * track): give it a little time to start. */
				if (sys_timepoint_expired(start_deadline)) {
					return 0;
				}
			}
		}
		if (sys_timepoint_expired(end)) {
			return -ETIMEDOUT;
		}
		k_msleep(100);
	}
}

int gs_audio_selftest(void)
{
	if (!s_inited) {
		return -EINVAL;
	}
	uint16_t count = 0;
	int rc = gs_audio_query_track_count(&count, K_MSEC(400));
	if (rc < 0) {
		LOG_WRN("audio selftest: track count query failed (%d)", rc);
		return rc;
	}
	LOG_INF("audio selftest: module reports %u track(s) (manifest expects %u)",
		count, (unsigned)GS_PROMPT_COUNT);

	k_mutex_lock(&s_lock, K_FOREVER);
	for (uint16_t t = 1; t <= count && t <= 20; t++) {
		uint8_t sel[2] = { (uint8_t)(t >> 8), (uint8_t)(t & 0xFF) };
		uint8_t name[16] = { 0 };
		uint8_t n = sizeof(name) - 1;

		rx_flush();
		send_frame(CMD_SELECT_NOPLAY, sel, 2);
		k_msleep(40);
		rx_flush();
		send_frame(CMD_SHORT_NAME, NULL, 0);
		if (recv_frame(CMD_SHORT_NAME, name, &n, K_MSEC(300)) == 0) {
			name[n] = '\0';
			LOG_INF("  track %2u → %s", t, (const char *)name);
		} else {
			LOG_INF("  track %2u → (no name reply)", t);
		}
	}
	k_mutex_unlock(&s_lock);
	return count;
}
