/*
 * Session dump channel — uart1 line protocol. See dump.h for the wire
 * protocol.
 *
 * Threading model:
 *   - One dedicated thread `gs_dump` runs `dump_entry`.
 *   - UART RX is interrupt-driven into a small ring buffer; the thread
 *     pulls lines off the ring and dispatches commands.
 *   - UART TX uses `uart_poll_out` (blocking per byte). At 1 Mbaud, a
 *     40 KB session file takes ~400 ms. Good enough for M5.
 *
 * The dump thread never touches session state mutated by the writer
 * thread. It only reads files from /lfs/sessions (open/read/close) and
 * lists/unlinks directory entries — the session writer never keeps a
 * handle on old session files after stop, so there's no overlap.
 */

#include "dump.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/ring_buffer.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

LOG_MODULE_REGISTER(gs_dump, LOG_LEVEL_INF);

#define DUMP_UART_NODE  DT_NODELABEL(uart1)
#if !DT_NODE_HAS_STATUS(DUMP_UART_NODE, okay)
#error "dump uart1 not enabled — check boards/thingy91x_nrf9151_ns.overlay"
#endif

#define SESSION_DIR     "/lfs/sessions"
#define MAX_LINE_LEN    128
#define UART_RX_BUFSZ   256
#define FILE_CHUNK_SZ   512  /* bytes per fs_read during DUMP */

static const struct device *const dump_uart = DEVICE_DT_GET(DUMP_UART_NODE);

/* UART RX ring buffer — fed by ISR, drained by the dump thread. */
static uint8_t  rx_ring_buf[UART_RX_BUFSZ];
static struct   ring_buf rx_ring;

K_THREAD_STACK_DEFINE(dump_stack, 4096);
static struct k_thread dump_thread;

/* Keep the file-chunk buffer off the thread stack — fs_dirent is
 * already ~260 B, the path buffer is another ~80 B, and we were
 * overflowing 2 KB once fs_read/fs_write's internal frames stacked on
 * top of a chunk[512]. Static storage is fine because cmd_dump is only
 * ever called from the single dump thread. */
static uint8_t file_chunk[FILE_CHUNK_SZ];

/* --- UART plumbing --- */

static void dump_write(const char *buf, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		uart_poll_out(dump_uart, (unsigned char)buf[i]);
	}
}

static void dump_writef(const char *fmt, ...)
{
	char line[MAX_LINE_LEN];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintk(line, sizeof(line), fmt, ap);
	va_end(ap);
	if (n < 0) { return; }
	if (n > (int)sizeof(line)) { n = sizeof(line); }
	dump_write(line, n);
}

static void uart_isr(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);
	if (!uart_irq_update(dev)) { return; }

	while (uart_irq_rx_ready(dev)) {
		uint8_t chunk[32];
		int read = uart_fifo_read(dev, chunk, sizeof(chunk));
		if (read > 0) {
			/* If the ring buffer is full we silently drop bytes.
			 * The dump protocol is ASCII and line-terminated, so
			 * worst case the next newline resynchronizes us. */
			(void)ring_buf_put(&rx_ring, chunk, read);
		}
	}
}

/* Blocking read of one line (terminated by \n). Strips trailing \r.
 * Returns the length of the line (excluding the terminator), or -ETIME
 * on inactivity timeout. */
static int read_line(char *buf, size_t buf_sz, k_timeout_t byte_timeout)
{
	size_t len = 0;
	while (len < buf_sz - 1) {
		uint8_t b;
		uint32_t got = ring_buf_get(&rx_ring, &b, 1);
		if (got == 0) {
			/* No data — sleep briefly and retry. Inactivity is
			 * the normal state (no host connected yet), so this
			 * isn't an error unless a caller wants a timeout. */
			k_sleep(K_MSEC(5));
			continue;
		}
		if (b == '\r') { continue; }
		if (b == '\n') {
			buf[len] = '\0';
			return (int)len;
		}
		buf[len++] = (char)b;
	}
	/* Overflow — discard and treat as no line. */
	buf[0] = '\0';
	return -EMSGSIZE;
}

/* --- Command handlers --- */

static void cmd_list(void)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);

	int ret = fs_opendir(&dir, SESSION_DIR);
	if (ret < 0) {
		if (ret == -ENOENT) {
			/* No session directory yet = nothing to list. */
			dump_write("END\n", 4);
			return;
		}
		dump_writef("ERR opendir %d\n", ret);
		return;
	}

	while (1) {
		struct fs_dirent entry;
		ret = fs_readdir(&dir, &entry);
		if (ret < 0) { break; }
		if (entry.name[0] == '\0') { break; }
		if (entry.type != FS_DIR_ENTRY_FILE) { continue; }
		dump_writef("%s %u\n", entry.name, (unsigned)entry.size);
	}
	fs_closedir(&dir);
	dump_write("END\n", 4);
}

static void cmd_dump(const char *name)
{
	char path[sizeof(SESSION_DIR) + 1 + 64];
	if (snprintk(path, sizeof(path), "%s/%s", SESSION_DIR, name) >= (int)sizeof(path)) {
		dump_write("ERR name too long\n", 18);
		return;
	}

	struct fs_dirent info;
	int ret = fs_stat(path, &info);
	if (ret < 0) {
		dump_writef("ERR stat %d\n", ret);
		return;
	}

	struct fs_file_t f;
	fs_file_t_init(&f);
	ret = fs_open(&f, path, FS_O_READ);
	if (ret < 0) {
		dump_writef("ERR open %d\n", ret);
		return;
	}

	dump_writef("SIZE %u\n", (unsigned)info.size);

	size_t left = info.size;
	while (left > 0) {
		size_t want = MIN(left, sizeof(file_chunk));
		ssize_t got = fs_read(&f, file_chunk, want);
		if (got <= 0) {
			fs_close(&f);
			dump_writef("\nERR read %zd\n", got);
			return;
		}
		dump_write((const char *)file_chunk, (size_t)got);
		left -= (size_t)got;
	}

	fs_close(&f);
	dump_write("\nOK\n", 4);
}

static void cmd_del(const char *name)
{
	char path[sizeof(SESSION_DIR) + 1 + 64];
	if (snprintk(path, sizeof(path), "%s/%s", SESSION_DIR, name) >= (int)sizeof(path)) {
		dump_write("ERR name too long\n", 18);
		return;
	}

	int ret = fs_unlink(path);
	if (ret < 0) {
		dump_writef("ERR unlink %d\n", ret);
		return;
	}
	dump_write("OK\n", 3);
}

/* --- Dispatch loop --- */

static void dump_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	char line[MAX_LINE_LEN];
	/* Greeting so the host can confirm the channel is alive. */
	dump_write("GOSTEADY-DUMP v1\n", 17);

	while (1) {
		int n = read_line(line, sizeof(line), K_FOREVER);
		if (n < 0) {
			dump_writef("ERR line %d\n", n);
			continue;
		}
		if (n == 0) { continue; }  /* blank line — ignore */

		if (strcmp(line, "LIST") == 0) {
			cmd_list();
		} else if (strncmp(line, "DUMP ", 5) == 0) {
			cmd_dump(line + 5);
		} else if (strncmp(line, "DEL ", 4) == 0) {
			cmd_del(line + 4);
		} else if (strcmp(line, "PING") == 0) {
			dump_write("PONG\n", 5);
		} else {
			dump_write("ERR unknown\n", 12);
		}
	}
}

/* --- Init --- */

int gosteady_dump_start(void)
{
	if (!device_is_ready(dump_uart)) {
		LOG_ERR("dump uart not ready");
		return -ENODEV;
	}

	ring_buf_init(&rx_ring, sizeof(rx_ring_buf), rx_ring_buf);

	uart_irq_callback_set(dump_uart, uart_isr);
	uart_irq_rx_enable(dump_uart);

	k_thread_create(&dump_thread, dump_stack, K_THREAD_STACK_SIZEOF(dump_stack),
			dump_entry, NULL, NULL, NULL,
			7, 0, K_NO_WAIT);
	k_thread_name_set(&dump_thread, "gs_dump");

	LOG_INF("dump channel ready on uart1 (/dev/cu.usbmodem105 @ 1Mbaud)");
	return 0;
}
