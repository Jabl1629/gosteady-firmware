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

#include "algo/gs_pipeline.h"
#include "cellular.h"
#include "cloud.h"

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/base64.h>
#include <math.h>
#include <string.h>
#include <stdio.h>

/* Single source of truth for firmware version string; mirrored into the
 * session header (FIRMWARE layer) and the M12.1d activity uplink payload
 * so cloud-side queries can correlate the two. Bumped 2026-04-27 from
 * "0.6.0-algo" to "0.7.0-cloud" with the M12.1c cloud bring-up. */
#define GS_FIRMWARE_VERSION_STR "0.7.0-cloud"

LOG_MODULE_REGISTER(gs_session, LOG_LEVEL_INF);

/* Inverse of g (m/s²) — converts BMI270 m/s² readings to g for the
 * V1 algorithm pipeline (mag_g input). 9.80665 is the CODATA standard
 * value used by both algo/data_loader.py and src/algo/gs_pipeline.h. */
#define INV_G_MS2  (1.0f / 9.80665f)

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

/* M12.1d: cellular UTC at session start, captured for inclusion in the
 * activity uplink payload (session_start field per coord §C.7). Empty
 * string if cellular wasn't ready at start time — activity uplink uses
 * empty string and cloud-side handler treats it as missing. */
static char              s_session_start_utc_iso[24];

/* M10 V1 distance estimator. Lives in .bss (~60 KB for the buffered
 * sample window). Owned exclusively by the writer thread between
 * session_start and session_stop; the start path resets it via
 * gs_pipeline_session_start() on the first post-handshake sample. */
static struct gs_pipeline s_pipeline;
static bool               s_pipeline_seeded;
static struct gs_pipeline_outputs s_pipeline_outputs;
static bool               s_pipeline_outputs_valid;

/* Phase 3 auto-stop: count of consecutive non-motion samples seen by the
 * writer thread since the last motion-gate-active sample. Reset to 0 in
 * the start handshake and on any motion-active sample. Reads from the
 * main thread (heartbeat tick) are deliberately unsynchronized — this is
 * a single-writer single-reader counter, the writer increments
 * monotonically while still, and a slightly stale read just means the
 * auto-stop fires at most one heartbeat tick (1 s) later than its
 * threshold. */
static volatile uint32_t  s_stationary_samples;

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
			/* Mark the V1 pipeline as needing re-seed on the next
			 * sample. We don't call gs_pipeline_session_start here
			 * because we don't have a sample yet — the seed needs
			 * the first sample's mag_g to prime the HP filter to
			 * steady state. The writer's drain loop seeds on the
			 * first sample after this handshake. */
			s_pipeline_seeded = false;
			s_pipeline_outputs_valid = false;
			memset(&s_pipeline_outputs, 0, sizeof(s_pipeline_outputs));
			s_stationary_samples = 0;
			k_poll_signal_reset(&start_signal);
			events[2].state = K_POLL_STATE_NOT_READY;
			k_sem_give(&start_done_sem);
		}

		/* Drain samples — capture anything that arrived before (or
		 * slightly after) a stop signal so we don't lose tail-end
		 * samples. Post-stop stragglers that manage to arrive after
		 * s_active has been cleared are additionally rejected by the
		 * s_active guard below, and any that slip through are
		 * cleaned up by the start-handshake on the next session.
		 * Each sample is also fed to the V1 distance pipeline so the
		 * algo state stays in lock-step with what's persisted. */
		if (events[0].state == K_POLL_STATE_MSGQ_DATA_AVAILABLE) {
			struct gosteady_sample s;
			while (k_msgq_get(&sample_q, &s, K_NO_WAIT) == 0) {
				if (!s_active) { continue; }
				const float mag_g = sqrtf(s.ax * s.ax + s.ay * s.ay
							  + s.az * s.az) * INV_G_MS2;
				if (!s_pipeline_seeded) {
					gs_pipeline_session_start(&s_pipeline, mag_g);
					s_pipeline_seeded = true;
				}
				gs_pipeline_step(&s_pipeline, mag_g);
				/* Phase 3 auto-stop input: track consecutive
				 * non-motion samples. The motion gate's
				 * `in_motion` flag has its own 500 ms running
				 * window + Schmitt hysteresis (exit_hold = 2 s),
				 * so this counter increments only after the gate
				 * has confirmed sustained stillness — brief
				 * mid-walk pauses don't drag it up. Reset to
				 * zero on every motion-active sample. Read by
				 * gosteady_session_stationary_samples() from the
				 * main thread's heartbeat tick. */
				if (s_pipeline.gate.in_motion) {
					s_stationary_samples = 0;
				} else {
					s_stationary_samples++;
				}
				local_batch[batch_fill++] = s;
				if (batch_fill == SAMPLE_BATCH_COUNT) {
					(void)writer_flush(local_batch, &batch_fill);
				}
			}
			events[0].state = K_POLL_STATE_NOT_READY;
		}

		/* Stop requested: flush remainder, finalize algo, rewrite
		 * header, close. */
		if (events[1].state == K_POLL_STATE_SIGNALED) {
			(void)writer_flush(local_batch, &batch_fill);
			if (s_pipeline_seeded) {
				gs_pipeline_finalize(&s_pipeline, &s_pipeline_outputs);
				s_pipeline_outputs_valid = true;
			} else {
				memset(&s_pipeline_outputs, 0,
				       sizeof(s_pipeline_outputs));
				s_pipeline_outputs_valid = false;
			}
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
	copy_ascii_field(s_header.firmware_version, sizeof(s_header.firmware_version), GS_FIRMWARE_VERSION_STR);
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

	/* M12.1d: capture cellular UTC for the activity uplink's session_start
	 * field. Best-effort — if cellular isn't ready (modem still attaching,
	 * NITZ not yet received), leave the buffer empty and the activity
	 * payload will publish with session_start="" (cloud handler treats
	 * empty as missing per accept-all contract). M12.1c.1 normally has
	 * cellular up by the time any session opens, so this is mostly
	 * defensive against early-boot session_start before modem registers. */
	s_session_start_utc_iso[0] = '\0';
	int t_err = gosteady_cellular_get_network_time(s_session_start_utc_iso,
						       sizeof(s_session_start_utc_iso));
	if (t_err) {
		LOG_WRN("session_start: cellular UTC unavailable (%d) — activity will publish without session_start", t_err);
	}

	LOG_INF("session start uuid=%s file=%s start_utc=%s",
		uuid_str, path,
		s_session_start_utc_iso[0] ? s_session_start_utc_iso : "(unavailable)");
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

	/* Clear s_active FIRST so the sampler stops enqueueing immediately
	 * and the writer's per-sample s_active guard discards anything that
	 * was already in flight in the msgq. This closes the race that
	 * otherwise lets a batch fill up between the writer's fs_close and
	 * this function setting s_active=false, which produced the
	 * `writer fs_write -9/1792` (-EBADF) errors and — in the worst case
	 * — landed inside a use-after-free path in LittleFS as a HardFault
	 * on STOP. Mirrors session_start which sets s_active=true only
	 * AFTER the file is open + handshake acked; this sets it false
	 * BEFORE the close path runs. k_poll_signal_raise has the memory
	 * barriers needed for the writer to observe the flag change before
	 * processing the stop event. */
	s_active = false;

	/* Signal the writer to finish draining, rewrite the header, and
	 * close. Wait for the ack — after this returns, the file is safe. */
	k_poll_signal_raise(&stop_signal, 0);
	int ret = k_sem_take(&stop_done_sem, K_SECONDS(5));
	if (ret < 0) {
		LOG_ERR("writer stop ack timeout (%d)", ret);
		/* Best-effort cleanup */
	}

	char uuid_str[37];
	uuid_to_string(s_header.session_uuid, uuid_str);
	LOG_INF("session stop uuid=%s samples=%u dropped=%u duration_ms=%u",
		uuid_str, s_sample_count, s_dropped_samples,
		k_uptime_get_32() - s_session_start_uptime_ms);
	log_header_base64();

	/* M10 V1 distance estimator outputs. Logged on TWO lines to fit
	 * Zephyr's per-message buffer (the combined string overflows
	 * CONFIG_LOG_BUFFER_SIZE's 128-byte deferred-message limit and
	 * gets truncated mid-UUID). Tag both lines with the same UUID so
	 * the log parser can join them.
	 *   ALGO_V1A uuid=<u> distance_ft=<f> R=<f|nan> surface=<n> steps=<u>
	 *   ALGO_V1B uuid=<u> motion_s=<f> total_s=<f> motion_frac=<f> overflow=<0|1>
	 * surface: 0=indoor, 1=outdoor (per gs_surface_t).
	 * R is 'nan' when the session had no walking motion (stationary
	 * baseline) or when it exceeded the buffered-samples cap. */
	if (s_pipeline_outputs_valid) {
		const struct gs_pipeline_outputs *o = &s_pipeline_outputs;
		char r_buf[16];
		if (isfinite((double)o->roughness_R)) {
			snprintk(r_buf, sizeof(r_buf), "%.4f", (double)o->roughness_R);
		} else {
			snprintk(r_buf, sizeof(r_buf), "nan");
		}
		LOG_INF("ALGO_V1A uuid=%s distance_ft=%.2f R=%s surface=%u steps=%u",
			uuid_str, (double)o->distance_ft, r_buf,
			o->surface_class, o->step_count);
		LOG_INF("ALGO_V1B uuid=%s motion_s=%.2f total_s=%.2f motion_frac=%.3f overflow=%u",
			uuid_str,
			(double)o->motion_duration_s,
			(double)o->total_duration_s,
			(double)o->motion_fraction,
			o->buffer_overflowed ? 1u : 0u);
	} else {
		LOG_WRN("ALGO_V1 uuid=%s no outputs (pipeline not seeded)", uuid_str);
	}

	/* M12.1d: enqueue activity uplink to cloud worker. Build the payload
	 * struct from the M9 algo outputs + cellular UTC stamps. The publish
	 * is fully async — gosteady_cloud_publish_activity returns after the
	 * record is copied into the worker's msgq, never blocks on cellular.
	 *
	 * Skipped if:
	 *   - CONFIG_GOSTEADY_CLOUD_ENABLE=n (default bench builds for M8 data
	 *     collection — IS_ENABLED gates dead-code elimination)
	 *   - algo outputs not valid (pipeline never seeded — typically a
	 *     session that closed before any sample was processed)
	 *
	 * Empty session_start_utc / session_end_utc is OK at the schema layer
	 * — cloud handler accepts and treats as missing per §C.7 accept-all
	 * contract; firmware-side this just means cellular wasn't ready when
	 * the corresponding event happened. */
	if (IS_ENABLED(CONFIG_GOSTEADY_CLOUD_ENABLE) && s_pipeline_outputs_valid) {
		const struct gs_pipeline_outputs *o = &s_pipeline_outputs;
		struct gosteady_activity a = {0};
		(void)snprintk(a.session_start_utc_iso, sizeof(a.session_start_utc_iso),
			       "%s", s_session_start_utc_iso);
		int end_err = gosteady_cellular_get_network_time(
			a.session_end_utc_iso, sizeof(a.session_end_utc_iso));
		if (end_err) {
			LOG_WRN("session_stop: cellular UTC unavailable (%d) — activity session_end will be empty", end_err);
			a.session_end_utc_iso[0] = '\0';
		}
		a.steps        = o->step_count;
		a.distance_ft  = o->distance_ft;
		/* active_min: round half-up from motion_duration_s/60. Cap at the
		 * portal-contract max (1440 = 24 h) defensively; sessions are
		 * normally minutes long so this only matters if something pinned
		 * a session open across a day. */
		{
			double active_min_d = (double)o->motion_duration_s / 60.0 + 0.5;
			if (active_min_d < 0.0)        { active_min_d = 0.0; }
			if (active_min_d > 1440.0)     { active_min_d = 1440.0; }
			a.active_min = (uint32_t)active_min_d;
		}
		a.roughness_R = o->roughness_R;          /* may be NaN — JSON omits */
		a.surface_class =                         /* gs_surface_t passes through */
			(o->step_count > 0) ? o->surface_class
					    : GOSTEADY_ACTIVITY_SURFACE_UNKNOWN;
		(void)snprintk(a.firmware_version, sizeof(a.firmware_version),
			       "%s", GS_FIRMWARE_VERSION_STR);

		int rc = gosteady_cloud_publish_activity(&a);
		if (rc) {
			LOG_WRN("activity enqueue failed: %d (session file still on flash)", rc);
		}
	}

	if (out_sample_count != NULL) {
		*out_sample_count = s_sample_count;
	}
	return ret;
}

bool gosteady_session_is_active(void)
{
	return s_active;
}

uint32_t gosteady_session_stationary_samples(void)
{
	return s_stationary_samples;
}

int gosteady_session_get_uuid_str(char *out, size_t out_sz)
{
	if (out == NULL || out_sz < 37) { return -EINVAL; }
	if (!s_active) { return -ENODEV; }
	uuid_to_string(s_header.session_uuid, out);
	return 0;
}

/* --- One-time writer init, runs at boot via SYS_INIT --- */

static int session_writer_init(void)
{
	int ret = gs_pipeline_init(&s_pipeline);
	if (ret < 0) {
		LOG_ERR("gs_pipeline_init failed (%d)", ret);
		/* Non-fatal — sessions can still capture, but algo outputs
		 * will be skipped (s_pipeline_seeded never gets set true
		 * because gs_pipeline_session_start would fail too). */
	}
	k_thread_create(&writer_thread, writer_stack, K_THREAD_STACK_SIZEOF(writer_stack),
			writer_entry, NULL, NULL, NULL,
			/* priority: lower than sampler's 5, above main loop */
			6, 0, K_NO_WAIT);
	k_thread_name_set(&writer_thread, "gs_writer");
	return 0;
}

SYS_INIT(session_writer_init, APPLICATION, 90);
