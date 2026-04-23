/*
 * GoSteady session file writer (Milestones 4 + 5).
 *
 * Milestone 4 implemented a mutex-protected `fs_write` on the sampler
 * thread, which stalled the sampler whenever a flash page erase landed
 * on a flush boundary. The net capture rate dropped to ~87 % of the
 * configured 100 Hz.
 *
 * Milestone 5 decouples the sampler from flash I/O:
 *
 *   sampler thread  --k_msgq_put(K_NO_WAIT)-->  [ sample_q ]
 *                                                     |
 *                                                     v
 *                                             writer thread
 *                                                     |
 *                                                     v
 *                                           /lfs/sessions/<uuid>.dat
 *
 * The sampler never blocks on flash — if the queue is full it drops the
 * sample and bumps `dropped_samples` (visible as `flash_errors` in the
 * header; semantics: anything that prevented a sample from reaching
 * disk cleanly). Writer thread drains the queue in batches and flushes
 * on a batch boundary. On session_stop(), the caller signals the writer
 * via a k_poll signal, the writer finishes draining, rewrites the
 * header with final stats, closes the file, and hands back control.
 *
 * Public API stays the same — callers use start / append / stop and
 * don't see the thread plumbing.
 */

#include "session.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <string.h>
#include <stdio.h>

LOG_MODULE_REGISTER(gs_session, LOG_LEVEL_INF);

#define SESSION_DIR         "/lfs/sessions"
#define SAMPLE_QUEUE_DEPTH  256   /* ~2.56 s of headroom at 100 Hz */
#define SAMPLE_BATCH_COUNT  64    /* 64 * 28 = 1792 B per fs_write call */

/* Inter-thread plumbing. */
K_MSGQ_DEFINE(sample_q, sizeof(struct gosteady_sample), SAMPLE_QUEUE_DEPTH, 4);
static struct k_poll_signal stop_signal  = K_POLL_SIGNAL_INITIALIZER(stop_signal);
static struct k_poll_signal start_signal = K_POLL_SIGNAL_INITIALIZER(start_signal);
static K_SEM_DEFINE(stop_done_sem,  0, 1);
static K_SEM_DEFINE(start_done_sem, 0, 1);

/* Writer thread stack + thread object. */
K_THREAD_STACK_DEFINE(writer_stack, 3072);
static struct k_thread writer_thread;

/* Session state — only mutated by either the main-thread control path
 * (start/stop) or the writer thread while the other is quiesced. */
static struct fs_file_t  s_file;
static bool              s_active;
static struct gosteady_session_header s_header;
static uint32_t          s_session_start_uptime_ms;
static uint32_t          s_sample_count;
static uint16_t          s_dropped_samples;

/* --- Helpers --- */

static int gen_uuid_v4(uint8_t out[16])
{
	const struct device *entropy = DEVICE_DT_GET(DT_CHOSEN(zephyr_entropy));
	if (!device_is_ready(entropy)) {
		LOG_ERR("entropy device not ready");
		return -ENODEV;
	}
	int ret = entropy_get_entropy(entropy, out, 16);
	if (ret < 0) {
		LOG_ERR("entropy_get_entropy failed (%d)", ret);
		return ret;
	}
	out[6] = (out[6] & 0x0f) | 0x40;
	out[8] = (out[8] & 0x3f) | 0x80;
	return 0;
}

static void uuid_to_string(const uint8_t u[16], char out[37])
{
	snprintk(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

static void copy_ascii_field(char *dst, size_t dst_size, const char *src)
{
	memset(dst, 0, dst_size);
	if (src != NULL) {
		strncpy(dst, src, dst_size);
	}
}

/* Flush the writer's local batch to the currently-open file. Returns 0
 * on success, negative errno on failure. */
static int writer_flush(struct gosteady_sample *batch, size_t *fill)
{
	if (*fill == 0) { return 0; }
	ssize_t want = (ssize_t)(*fill * sizeof(struct gosteady_sample));
	ssize_t got  = fs_write(&s_file, batch, want);
	if (got != want) {
		LOG_ERR("writer fs_write %zd/%zd", got, want);
		*fill = 0;
		return got < 0 ? (int)got : -EIO;
	}
	s_sample_count += *fill;
	*fill = 0;
	return 0;
}

static int rewrite_header(void)
{
	s_header.sample_count       = s_sample_count;
	s_header.session_end_utc_ms = 0;  /* TODO(M12): real UTC once RTC sync lands */
	s_header.battery_mv_end     = 0;  /* TODO: pending nPM1300 fuel-gauge wiring */
	s_header.flash_errors       = s_dropped_samples;

	int ret = fs_seek(&s_file, 0, FS_SEEK_SET);
	if (ret < 0) { return ret; }
	ssize_t got = fs_write(&s_file, &s_header, sizeof(s_header));
	if (got != (ssize_t)sizeof(s_header)) {
		return got < 0 ? (int)got : -EIO;
	}
	return fs_sync(&s_file);
}

static void log_header_base64(void)
{
	char b64[360];
	size_t outlen = 0;
	int ret = base64_encode((uint8_t *)b64, sizeof(b64), &outlen,
				(const uint8_t *)&s_header, sizeof(s_header));
	if (ret < 0) { LOG_ERR("base64_encode failed (%d)", ret); return; }
	b64[MIN(outlen, sizeof(b64) - 1)] = '\0';
	LOG_INF("SESSION_HEADER_B64 %s", b64);
}

/* --- Writer thread: drain queue, batch-write, close on stop --- */

static void writer_entry(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	struct gosteady_sample local_batch[SAMPLE_BATCH_COUNT];
	size_t batch_fill = 0;

	while (1) {
		/* Block until a sample is ready, a stop has been requested,
		 * or a start handshake has been raised. The start signal is
		 * used by session_start() to synchronously flush any stale
		 * state (stragglers enqueued during the previous session's
		 * close window) before the new session opens its file. */
		struct k_poll_event events[] = {
			K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_MSGQ_DATA_AVAILABLE,
							K_POLL_MODE_NOTIFY_ONLY, &sample_q, 0),
			K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SIGNAL,
							K_POLL_MODE_NOTIFY_ONLY, &stop_signal, 0),
			K_POLL_EVENT_STATIC_INITIALIZER(K_POLL_TYPE_SIGNAL,
							K_POLL_MODE_NOTIFY_ONLY, &start_signal, 0),
		};
		int ret = k_poll(events, ARRAY_SIZE(events), K_FOREVER);
		if (ret < 0) {
			LOG_ERR("k_poll failed (%d)", ret);
			continue;
		}

		/* Start-of-session handshake — handled FIRST so we clear any
		 * stragglers left in local_batch or the msgq from the previous
		 * stop's close window before the new session gets going. This
		 * eliminates a race that otherwise lets 30-40 samples with the
		 * PREVIOUS session's t_ms base leak into the new session's
		 * file (see GOSTEADY_CONTEXT.md "Session recording" gotchas). */
		if (events[2].state == K_POLL_STATE_SIGNALED) {
			batch_fill = 0;
			struct gosteady_sample discard;
			while (k_msgq_get(&sample_q, &discard, K_NO_WAIT) == 0) {
				/* drop any residual samples */
			}
			k_poll_signal_reset(&start_signal);
			events[2].state = K_POLL_STATE_NOT_READY;
			k_sem_give(&start_done_sem);
		}

		/* Drain samples — capture anything that arrived before (or
		 * slightly after) a stop signal so we don't lose tail-end
		 * samples. Post-stop stragglers that manage to arrive after
		 * s_active has been cleared are additionally rejected by the
		 * s_active guard below, and any that slip through are
		 * cleaned up by the start-handshake on the next session. */
		if (events[0].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct gosteady_sample s;
			while (k_msgq_get(&sample_q, &s, K_NO_WAIT) == 0) {
				if (!s_active) { continue; }
				local_batch[batch_fill++] = s;
				if (batch_fill == SAMPLE_BATCH_COUNT) {
					(void)writer_flush(local_batch, &batch_fill);
				}
			}
			events[0].state = K_POLL_STATE_NOT_READY;
		}

		/* Stop requested: flush remainder, rewrite header, close. */
		if (events[1].state == K_POLL_STATE_SIGNALED) {
			(void)writer_flush(local_batch, &batch_fill);
			int hdr_ret   = rewrite_header();
			int close_ret = fs_close(&s_file);
			if (hdr_ret < 0 || close_ret < 0) {
				LOG_ERR("stop close errors: hdr=%d close=%d",
					hdr_ret, close_ret);
			}
			/* Belt-and-suspenders: writer_flush already resets this,
			 * but an explicit reset here documents the contract that
			 * local_batch owns no samples across a stop boundary. */
			batch_fill = 0;
			k_poll_signal_reset(&stop_signal);
			events[1].state = K_POLL_STATE_NOT_READY;
			k_sem_give(&stop_done_sem);
		}
	}
}

/* --- Public API --- */

int gosteady_session_start(const struct gosteady_prewalk *prewalk)
{
	if (prewalk == NULL) { return -EINVAL; }
	if (s_active) { return -EALREADY; }

	int ret = fs_mkdir(SESSION_DIR);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("mkdir %s failed (%d)", SESSION_DIR, ret);
		return ret;
	}

	/* Reset the writer's state (local_batch) AND purge any stale
	 * samples left in the msgq from the previous session's close
	 * window. Done via a synchronous signal/ack handshake so that by
	 * the time this returns, the writer is guaranteed to be back in
	 * its k_poll wait with batch_fill=0 — no race possible with a
	 * sampler enqueue from this session. Double-purge (we also purge
	 * below after fs_open) is deliberate belt-and-suspenders. */
	k_msgq_purge(&sample_q);
	k_poll_signal_raise(&start_signal, 0);
	int sem_ret = k_sem_take(&start_done_sem, K_SECONDS(2));
	if (sem_ret < 0) {
		LOG_ERR("writer start-ack timeout (%d) — proceeding anyway", sem_ret);
	}
	k_msgq_purge(&sample_q);

	memset(&s_header, 0, sizeof(s_header));
	s_header.magic       = GOSTEADY_SESSION_MAGIC;
	s_header.version     = GOSTEADY_SESSION_VERSION;
	s_header.header_size = GOSTEADY_HEADER_BYTES;
	ret = gen_uuid_v4(s_header.session_uuid);
	if (ret < 0) { return ret; }
	copy_ascii_field(s_header.device_serial,    sizeof(s_header.device_serial),    "TH91X-0001");
	copy_ascii_field(s_header.firmware_version, sizeof(s_header.firmware_version), "0.5.0-dev");
	copy_ascii_field(s_header.sensor_model,     sizeof(s_header.sensor_model),     "BMI270");
	s_header.sample_rate_hz        = 100;
	s_header.accel_range_g         = 4;
	s_header.gyro_range_dps        = 500;
	s_header.session_start_utc_ms  = 0;
	s_header.battery_mv_start      = 0;
	memcpy(&s_header.prewalk, prewalk, sizeof(*prewalk));

	char uuid_str[37];
	uuid_to_string(s_header.session_uuid, uuid_str);
	char path[sizeof(SESSION_DIR) + 1 + 36 + 4 + 1];
	snprintk(path, sizeof(path), "%s/%s.dat", SESSION_DIR, uuid_str);

	fs_file_t_init(&s_file);
	ret = fs_open(&s_file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("fs_open %s failed (%d)", path, ret);
		return ret;
	}
	ssize_t wrote = fs_write(&s_file, &s_header, sizeof(s_header));
	if (wrote != (ssize_t)sizeof(s_header)) {
		LOG_ERR("header write short (%zd)", wrote);
		fs_close(&s_file);
		return wrote < 0 ? (int)wrote : -EIO;
	}

	s_session_start_uptime_ms = k_uptime_get_32();
	s_sample_count            = 0;
	s_dropped_samples         = 0;
	s_active                  = true;

	LOG_INF("session start uuid=%s file=%s", uuid_str, path);
	return 0;
}

int gosteady_session_append(const struct gosteady_sample *s)
{
	if (s == NULL) { return -EINVAL; }
	if (!s_active) { return -ENODEV; }

	/* Non-blocking: if the writer hasn't caught up, drop the sample and
	 * note it in the header. We prefer consistent timing in the sampler
	 * thread over a guaranteed sample-count match. */
	int ret = k_msgq_put(&sample_q, s, K_NO_WAIT);
	if (ret < 0) {
		s_dropped_samples++;
		return ret;
	}
	return 0;
}

int gosteady_session_stop(uint32_t *out_sample_count)
{
	if (!s_active) { return -ENODEV; }

	/* Signal the writer to finish draining, rewrite the header, and
	 * close. Wait for the ack — after this returns, the file is safe. */
	k_poll_signal_raise(&stop_signal, 0);
	int ret = k_sem_take(&stop_done_sem, K_SECONDS(5));
	if (ret < 0) {
		LOG_ERR("writer stop ack timeout (%d)", ret);
		/* Best-effort cleanup */
	}

	s_active = false;

	char uuid_str[37];
	uuid_to_string(s_header.session_uuid, uuid_str);
	LOG_INF("session stop uuid=%s samples=%u dropped=%u duration_ms=%u",
		uuid_str, s_sample_count, s_dropped_samples,
		k_uptime_get_32() - s_session_start_uptime_ms);
	log_header_base64();

	if (out_sample_count != NULL) {
		*out_sample_count = s_sample_count;
	}
	return ret;
}

bool gosteady_session_is_active(void)
{
	return s_active;
}

/* --- One-time writer init, runs at boot via SYS_INIT --- */

static int session_writer_init(void)
{
	k_thread_create(&writer_thread, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack),
			writer_entry, NULL, NULL, NULL,
			/* priority: lower than sampler's 5, above main loop */
			6, 0, K_NO_WAIT);
	k_thread_name_set(&writer_thread, "gs_writer");
	return 0;
}

SYS_INIT(session_writer_init, APPLICATION, 90);
