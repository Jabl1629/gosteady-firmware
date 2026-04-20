/*
 * GoSteady session file writer (Milestone 4).
 *
 * See session.h for the file format and the contract with the ingest
 * script. This .c just implements the lifecycle:
 *
 *   start  -> generate UUIDv4, stamp header, create /lfs/sessions/<uuid>.dat,
 *             reserve 256 bytes for the header (written blank first, then
 *             rewritten at close with the final stats)
 *   append -> add sample to a RAM ring buffer, flush a batch to disk when
 *             full. Keeps per-sample work off the flash path.
 *   stop   -> flush remaining samples, rewrite header with sample_count /
 *             session_end_utc_ms / battery_mv_end / flash_errors, close file,
 *             and base64-log the final header to UART for M4 ingest testing.
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
#define SAMPLE_BATCH_COUNT  32    /* 32 * 28 = 896 bytes per flush — close to one flash page */

/* --- Module state --- */

static struct fs_file_t s_file;
static bool             s_active;
static struct gosteady_session_header s_header;
static uint32_t         s_session_start_uptime_ms;
static uint32_t         s_sample_count;
static uint16_t         s_flash_errors;

static struct gosteady_sample s_batch[SAMPLE_BATCH_COUNT];
static size_t           s_batch_fill;

/* Mutex so start/append/stop from different threads don't step on each
 * other's fs_* calls. The batch buffer is protected by it too. */
K_MUTEX_DEFINE(s_lock);

/* --- Helpers --- */

/* Build a RFC 4122 version-4 (random) UUID into `out`. Pulls 16 bytes
 * from the DTS-chosen entropy source (zephyr,entropy → psa_rng on the
 * nRF91 NS), then fixes up the version and variant nibbles. */
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
	out[6] = (out[6] & 0x0f) | 0x40;  /* version 4 */
	out[8] = (out[8] & 0x3f) | 0x80;  /* variant RFC 4122 */
	return 0;
}

/* Format 16 UUID bytes as the canonical 36-char hex string. */
static void uuid_to_string(const uint8_t u[16], char out[37])
{
	snprintk(out, 37,
		 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-"
		 "%02x%02x%02x%02x%02x%02x",
		 u[0], u[1], u[2], u[3], u[4], u[5], u[6], u[7],
		 u[8], u[9], u[10], u[11], u[12], u[13], u[14], u[15]);
}

/* Copy a zero-padded ASCII field. Source need not be NUL-terminated. */
static void copy_ascii_field(char *dst, size_t dst_size, const char *src)
{
	memset(dst, 0, dst_size);
	if (src != NULL) {
		strncpy(dst, src, dst_size);
	}
}

/* Flush the in-memory batch to the open file. Caller holds s_lock. */
static int flush_batch_locked(void)
{
	if (s_batch_fill == 0) {
		return 0;
	}
	ssize_t want = (ssize_t)(s_batch_fill * sizeof(struct gosteady_sample));
	ssize_t got  = fs_write(&s_file, s_batch, want);
	if (got != want) {
		s_flash_errors++;
		LOG_ERR("fs_write %zd/%zd (errno=%d)", got, want, (int)got);
		s_batch_fill = 0;
		return got < 0 ? (int)got : -EIO;
	}
	s_sample_count += s_batch_fill;
	s_batch_fill = 0;
	return 0;
}

/* Rewrite the 256-byte header at the top of the file with the final
 * close-time stats. Caller holds s_lock. */
static int rewrite_header_locked(void)
{
	s_header.sample_count       = s_sample_count;
	s_header.session_end_utc_ms = 0;  /* TODO(M12): stamp real UTC once RTC sync lands */
	s_header.battery_mv_end     = 0;  /* TODO: pending nPM1300 fuel-gauge wiring */
	s_header.flash_errors       = s_flash_errors;

	int ret = fs_seek(&s_file, 0, FS_SEEK_SET);
	if (ret < 0) { return ret; }
	ssize_t got = fs_write(&s_file, &s_header, sizeof(s_header));
	if (got != (ssize_t)sizeof(s_header)) {
		return got < 0 ? (int)got : -EIO;
	}
	return fs_sync(&s_file);
}

/* Dump the header as base64 to the log so the ingest script can grab it
 * off the console during M4 testing. USB mass-storage pull path lands in
 * Milestone 5; this is the bridge. */
static void log_header_base64(void)
{
	/* base64 of 256 bytes is ceil(256/3)*4 = 344 chars + NUL */
	char b64[360];
	size_t outlen = 0;
	int ret = base64_encode((uint8_t *)b64, sizeof(b64), &outlen,
				(const uint8_t *)&s_header, sizeof(s_header));
	if (ret < 0) {
		LOG_ERR("base64_encode failed (%d)", ret);
		return;
	}
	/* Z-terminate; base64_encode doesn't guarantee it. */
	b64[MIN(outlen, sizeof(b64) - 1)] = '\0';
	LOG_INF("SESSION_HEADER_B64 %s", b64);
}

/* --- Public API --- */

int gosteady_session_start(const struct gosteady_prewalk *prewalk)
{
	if (prewalk == NULL) { return -EINVAL; }

	int ret;
	k_mutex_lock(&s_lock, K_FOREVER);

	if (s_active) {
		k_mutex_unlock(&s_lock);
		return -EALREADY;
	}

	/* fs_mkdir returns -EEXIST if the directory is already there, which
	 * is fine on every boot after the first. */
	ret = fs_mkdir(SESSION_DIR);
	if (ret < 0 && ret != -EEXIST) {
		LOG_ERR("mkdir %s failed (%d)", SESSION_DIR, ret);
		k_mutex_unlock(&s_lock);
		return ret;
	}

	/* Header — firmware fields first. */
	memset(&s_header, 0, sizeof(s_header));
	s_header.magic       = GOSTEADY_SESSION_MAGIC;
	s_header.version     = GOSTEADY_SESSION_VERSION;
	s_header.header_size = GOSTEADY_HEADER_BYTES;
	ret = gen_uuid_v4(s_header.session_uuid);
	if (ret < 0) {
		k_mutex_unlock(&s_lock);
		return ret;
	}
	copy_ascii_field(s_header.device_serial,    sizeof(s_header.device_serial),    "TH91X-0001");
	copy_ascii_field(s_header.firmware_version, sizeof(s_header.firmware_version), "0.4.0-dev");
	copy_ascii_field(s_header.sensor_model,     sizeof(s_header.sensor_model),     "BMI270");
	s_header.sample_rate_hz        = 100;
	s_header.accel_range_g         = 4;
	s_header.gyro_range_dps        = 500;
	s_header.session_start_utc_ms  = 0;  /* TODO(M12): real UTC once RTC sync lands */
	s_header.battery_mv_start      = 0;  /* TODO: pending nPM1300 fuel-gauge wiring */

	/* Pre-walk layer — the host-provided part. */
	memcpy(&s_header.prewalk, prewalk, sizeof(*prewalk));

	/* File path: /lfs/sessions/<uuid>.dat */
	char uuid_str[37];
	uuid_to_string(s_header.session_uuid, uuid_str);
	char path[sizeof(SESSION_DIR) + 1 + 36 + 4 + 1];
	snprintk(path, sizeof(path), "%s/%s.dat", SESSION_DIR, uuid_str);

	fs_file_t_init(&s_file);
	ret = fs_open(&s_file, path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) {
		LOG_ERR("fs_open %s failed (%d)", path, ret);
		k_mutex_unlock(&s_lock);
		return ret;
	}

	/* Reserve the header region up front. We write a zeroed-out-but-correctly-
	 * magiced header now, and rewrite it at stop() with the final stats. */
	ssize_t wrote = fs_write(&s_file, &s_header, sizeof(s_header));
	if (wrote != (ssize_t)sizeof(s_header)) {
		LOG_ERR("header write short (%zd)", wrote);
		fs_close(&s_file);
		k_mutex_unlock(&s_lock);
		return wrote < 0 ? (int)wrote : -EIO;
	}

	s_session_start_uptime_ms = k_uptime_get_32();
	s_sample_count            = 0;
	s_flash_errors            = 0;
	s_batch_fill              = 0;
	s_active                  = true;

	LOG_INF("session start uuid=%s file=%s", uuid_str, path);

	k_mutex_unlock(&s_lock);
	return 0;
}

int gosteady_session_append(const struct gosteady_sample *s)
{
	if (s == NULL) { return -EINVAL; }

	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_active) {
		k_mutex_unlock(&s_lock);
		return -ENODEV;
	}

	s_batch[s_batch_fill++] = *s;

	int ret = 0;
	if (s_batch_fill == SAMPLE_BATCH_COUNT) {
		ret = flush_batch_locked();
	}

	k_mutex_unlock(&s_lock);
	return ret;
}

int gosteady_session_stop(uint32_t *out_sample_count)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	if (!s_active) {
		k_mutex_unlock(&s_lock);
		return -ENODEV;
	}

	int flush_ret = flush_batch_locked();
	int hdr_ret   = rewrite_header_locked();
	int close_ret = fs_close(&s_file);
	s_active = false;

	/* Prefer the first non-zero failure code. */
	int ret = flush_ret ? flush_ret : (hdr_ret ? hdr_ret : close_ret);

	char uuid_str[37];
	uuid_to_string(s_header.session_uuid, uuid_str);
	LOG_INF("session stop uuid=%s samples=%u flash_errors=%u duration_ms=%u",
		uuid_str, s_sample_count, s_flash_errors,
		k_uptime_get_32() - s_session_start_uptime_ms);
	log_header_base64();

	if (out_sample_count != NULL) {
		*out_sample_count = s_sample_count;
	}
	k_mutex_unlock(&s_lock);
	return ret;
}

bool gosteady_session_is_active(void)
{
	/* Read is atomic on 32-bit ARM; no mutex needed for a bool probe. */
	return s_active;
}
