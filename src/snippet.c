/*
 * M12.1f — snippet capture + opportunistic upload.
 *
 * Two responsibilities:
 *   1. Per-session capture of the first 30 s of raw BMI270 samples,
 *      written to /snippets/<uuid>.bin on the snippet_storage partition.
 *   2. Heartbeat-piggyback upload of one queued snippet per cellular
 *      wake, oldest-first by mtime.
 *
 * Layout per snippet (see snippet.h header for full contract):
 *   /snippets/<uuid>.bin   16-byte binary header + N × 28-byte samples
 *   /snippets/<uuid>.json  small JSON sidecar with snippet_id +
 *                          window_start_ts (and v1.5 anomaly_trigger)
 *   /snippets/<uuid>.up    zero-byte marker file → uploaded
 *
 * The 16-byte binary header (written at capture_start with a placeholder
 * sample_count_n=0, rewritten at capture_finish with the final count)
 * matches §F.4 verbatim:
 *
 *   uint8  format_version  = 1
 *   uint8  sensor_id       = 1   (BMI270)
 *   uint16 sample_rate_hz  = 100
 *   uint32 sample_count_n
 *   uint64 window_start_uptime_ms
 *
 * Upload framing per §F.3:
 *   [4-byte BE uint32 header_len][header_len JSON][.bin contents]
 */

#include "snippet.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/timeutil.h>

#include "cellular.h"  /* FMEA 6.1: stale-cutoff needs current cellular UTC */

LOG_MODULE_REGISTER(gs_snippet, LOG_LEVEL_INF);

#define SNIPPET_MNT       "/snippets"
#define BIN_HEADER_BYTES  16
#define SAMPLE_BYTES      28
#define MAX_SAMPLES       (GOSTEADY_SNIPPET_WINDOW_MS / 10)  /* 100 Hz × 30 s */
#define MAX_BIN_BYTES     (BIN_HEADER_BYTES + MAX_SAMPLES * SAMPLE_BYTES)
#define MAX_JSON_BYTES    256
#define MAX_PAYLOAD_BYTES (4 + MAX_JSON_BYTES + MAX_BIN_BYTES)

/* On-disk binary header — packed, little-endian. Mirrors §F.4 verbatim. */
struct __attribute__((packed)) snippet_bin_header {
	uint8_t  format_version;
	uint8_t  sensor_id;
	uint16_t sample_rate_hz;
	uint32_t sample_count_n;
	uint64_t window_start_uptime_ms;
};
_Static_assert(sizeof(struct snippet_bin_header) == BIN_HEADER_BYTES,
	"snippet_bin_header must be 16 bytes per §F.4");

/* On-disk sample record — packed, matches gosteady_sample minus session
 * fields. */
struct __attribute__((packed)) snippet_sample {
	uint32_t t_ms;
	float    ax, ay, az;
	float    gx, gy, gz;
};
_Static_assert(sizeof(struct snippet_sample) == SAMPLE_BYTES,
	"snippet_sample must be 28 bytes per §F.4");

/* Mount config — own littlefs instance on snippet_storage partition. */
FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(snippet_lfs_data);
static struct fs_mount_t snippet_mnt = {
	.type        = FS_LITTLEFS,
	.fs_data     = &snippet_lfs_data,
	.storage_dev = (void *)FIXED_PARTITION_ID(snippet_storage),
	.mnt_point   = SNIPPET_MNT,
};

static atomic_t s_initialized = ATOMIC_INIT(0);

/* Capture-state guard. Single in-flight capture at a time (matches the
 * single-active-session invariant in session.c). All access goes through
 * s_capture_lock.
 *
 * Samples accumulate in `batch` (in-RAM ring) and flush to flash either
 * when full (BATCH_SIZE samples) or at capture finish. Per-sample
 * fs_write would otherwise hit ~3000 writes per 30 s window, which
 * (a) hammers the SPI flash and (b) deepens the writer-thread call
 * stack on every sample with LittleFS's per-write metadata churn. The
 * batch-flush pattern is borrowed from session.c::writer_entry's own
 * 64-sample pattern for /lfs/sessions/<uuid>.dat.
 *
 * BATCH_SIZE = 64 → 1.8 KB per flush, ~47 flushes per 30 s window.
 * Comfortable both for stack-pressure and for SPI write throughput. */
#define SNIPPET_BATCH_SIZE 64

static K_MUTEX_DEFINE(s_capture_lock);
static struct {
	bool          active;
	char          uuid[40];
	char          window_start_ts[24];
	uint64_t      window_start_uptime_ms;
	uint32_t      sample_count;
	uint32_t      batch_fill;
	struct snippet_sample batch[SNIPPET_BATCH_SIZE];
	struct fs_file_t file;
} s_cap;

/* Path helpers — stack buffers, no malloc on this firmware. */
static void path_for(const char *uuid, const char *suffix, char *out, size_t out_sz)
{
	(void)snprintf(out, out_sz, SNIPPET_MNT "/%s%s", uuid, suffix);
}

/* ---- FMEA 6.1: rotation policy helpers ---- */

/* Parse "YYYY-MM-DDTHH:MM:SSZ" to Unix-epoch milliseconds. Returns 0
 * on success. Strict parser — only accepts the exact 20-char canonical
 * form the firmware writes (cellular_format_unix_ms_iso8601). */
static int parse_iso8601_unix_ms(const char *iso, int64_t *out_ms)
{
	int year, month, day, hour, minute, second;
	int n = sscanf(iso, "%4d-%2d-%2dT%2d:%2d:%2dZ",
		       &year, &month, &day, &hour, &minute, &second);
	if (n != 6) {
		return -EINVAL;
	}
	struct tm tm = {
		.tm_year = year - 1900,
		.tm_mon  = month - 1,
		.tm_mday = day,
		.tm_hour = hour,
		.tm_min  = minute,
		.tm_sec  = second,
	};
	int64_t epoch_s = timeutil_timegm64(&tm);
	if (epoch_s == (int64_t)-1) {
		return -EIO;
	}
	*out_ms = epoch_s * 1000;
	return 0;
}

/* Read the JSON sidecar for a snippet UUID and extract window_start_ts.
 * Sidecar format is fixed:
 *   {"snippet_id":"<uuid>","window_start_ts":"YYYY-MM-DDTHH:MM:SSZ"}
 * We use strstr (no Zephyr json lib dep) since the format is firmware-
 * controlled. Returns 0 + fills out_ts (must be >= 24 bytes), -ENOENT
 * if sidecar doesn't exist, -EINVAL if format is unparseable. */
static int read_window_start_ts(const char *uuid, char *out_ts, size_t out_sz)
{
	if (!uuid || !out_ts || out_sz < 24) { return -EINVAL; }
	char path[80];
	path_for(uuid, ".json", path, sizeof(path));
	struct fs_file_t f;
	fs_file_t_init(&f);
	int rc = fs_open(&f, path, FS_O_READ);
	if (rc < 0) { return rc; }
	char buf[MAX_JSON_BYTES + 1];
	ssize_t n = fs_read(&f, buf, sizeof(buf) - 1);
	(void)fs_close(&f);
	if (n <= 0) { return -EIO; }
	buf[n] = '\0';
	const char *key = "\"window_start_ts\":\"";
	const char *p = strstr(buf, key);
	if (p == NULL) { return -EINVAL; }
	p += strlen(key);
	if ((size_t)(p - buf) + 20 + 1 > (size_t)n) { return -EINVAL; }
	if (p[20] != '"') { return -EINVAL; }
	memcpy(out_ts, p, 20);
	out_ts[20] = '\0';
	return 0;
}

/* Delete .bin + .json + .up for a snippet UUID. -ENOENT on the components
 * is non-fatal (some may not exist). Returns the count of files actually
 * unlinked. */
static int delete_snippet_files(const char *uuid)
{
	int count = 0;
	const char *suffixes[] = { ".bin", ".json", ".up" };
	for (size_t i = 0; i < ARRAY_SIZE(suffixes); i++) {
		char path[80];
		path_for(uuid, suffixes[i], path, sizeof(path));
		int rc = fs_unlink(path);
		if (rc == 0) {
			count++;
		} else if (rc != -ENOENT) {
			LOG_WRN("rotate: fs_unlink %s failed (%d)", path, rc);
		}
	}
	return count;
}

/* Snapshot the currently-active capture's UUID under s_capture_lock. */
static bool snapshot_active_uuid(char *out_uuid, size_t out_sz)
{
	if (!out_uuid || out_sz < 40) {
		if (out_uuid && out_sz > 0) { out_uuid[0] = '\0'; }
		return false;
	}
	bool active;
	k_mutex_lock(&s_capture_lock, K_FOREVER);
	active = s_cap.active;
	if (active) {
		strncpy(out_uuid, s_cap.uuid, out_sz - 1);
		out_uuid[out_sz - 1] = '\0';
	} else {
		out_uuid[0] = '\0';
	}
	k_mutex_unlock(&s_capture_lock);
	return active;
}

/* Pass 1: stale-cutoff. Iterate /snippets, for each .bin entry read its
 * sidecar, parse window_start_ts, delete if older than threshold. */
static int rotate_stale_pass(uint32_t stale_age_seconds, const char *active_uuid)
{
	int64_t now_ms = 0;
	int t_err = gosteady_cellular_get_network_time_unix_ms(&now_ms);
	if (t_err != 0) {
		LOG_DBG("rotate: stale-pass skipped — cellular UTC unavailable (%d)",
			t_err);
		return 0;
	}
	int64_t threshold_ms = now_ms - (int64_t)stale_age_seconds * 1000;

	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int ret = fs_opendir(&dir, SNIPPET_MNT);
	if (ret < 0) { return ret; }

	int deleted = 0;
	struct fs_dirent ent;
	while (1) {
		int rc = fs_readdir(&dir, &ent);
		if (rc < 0 || ent.name[0] == '\0') { break; }
		size_t nlen = strlen(ent.name);
		if (nlen <= 4 || strcmp(ent.name + nlen - 4, ".bin") != 0) {
			continue;
		}
		char uuid[40];
		size_t ulen = nlen - 4;
		if (ulen >= sizeof(uuid)) { continue; }
		memcpy(uuid, ent.name, ulen);
		uuid[ulen] = '\0';
		if (active_uuid && active_uuid[0] != '\0' &&
		    strcmp(uuid, active_uuid) == 0) {
			continue;
		}
		char ts[24];
		int srt = read_window_start_ts(uuid, ts, sizeof(ts));
		if (srt < 0) {
			/* Missing/malformed sidecar — orphan path in
			 * upload_one handles that case; don't delete here
			 * based on uncertain age. */
			continue;
		}
		int64_t snip_ms = 0;
		if (parse_iso8601_unix_ms(ts, &snip_ms) < 0) {
			continue;
		}
		if (snip_ms < threshold_ms) {
			LOG_INF("rotate: stale snippet %s (ts=%s, age=%lld s) — deleting",
				uuid, ts,
				(long long)((now_ms - snip_ms) / 1000));
			(void)delete_snippet_files(uuid);
			deleted++;
		}
	}
	(void)fs_closedir(&dir);
	return deleted;
}

/* Pass 2: free-space rotation. If utilization >= threshold_pct, delete
 * the OLDEST .up-marked snippet (by readdir order). Loops until below
 * threshold OR no .up marker remains. */
static int rotate_freespace_pass(uint8_t threshold_pct, const char *active_uuid)
{
	int deleted = 0;
	const int MAX_ITERATIONS = 64;
	for (int iter = 0; iter < MAX_ITERATIONS; iter++) {
		struct fs_statvfs vfs;
		int srt = fs_statvfs(SNIPPET_MNT, &vfs);
		if (srt < 0 || vfs.f_blocks == 0) {
			LOG_WRN("rotate: fs_statvfs failed (%d) — skipping freespace pass",
				srt);
			return deleted;
		}
		uint64_t total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
		uint64_t free  = (uint64_t)vfs.f_bfree  * vfs.f_frsize;
		uint64_t used  = total - free;
		uint8_t  used_pct = (total > 0) ? (uint8_t)((used * 100) / total) : 0;
		if (used_pct < threshold_pct) {
			if (deleted > 0) {
				LOG_INF("rotate: freespace settled at %u%% used (deleted %d)",
					(unsigned)used_pct, deleted);
			}
			return deleted;
		}
		struct fs_dir_t dir;
		fs_dir_t_init(&dir);
		int ret = fs_opendir(&dir, SNIPPET_MNT);
		if (ret < 0) { return deleted; }
		char victim[40];
		victim[0] = '\0';
		struct fs_dirent ent;
		while (1) {
			int rc = fs_readdir(&dir, &ent);
			if (rc < 0 || ent.name[0] == '\0') { break; }
			size_t nlen = strlen(ent.name);
			if (nlen <= 3 || strcmp(ent.name + nlen - 3, ".up") != 0) {
				continue;
			}
			size_t ulen = nlen - 3;
			if (ulen >= sizeof(victim)) { continue; }
			memcpy(victim, ent.name, ulen);
			victim[ulen] = '\0';
			if (active_uuid && active_uuid[0] != '\0' &&
			    strcmp(victim, active_uuid) == 0) {
				victim[0] = '\0';
				continue;
			}
			break;
		}
		(void)fs_closedir(&dir);
		if (victim[0] == '\0') {
			LOG_WRN("rotate: %u%% used >= %u%% threshold but no .up-marked snippet to evict — partition pressure with all snippets pending upload",
				(unsigned)used_pct, (unsigned)threshold_pct);
			return deleted;
		}
		LOG_INF("rotate: freespace at %u%% — evicting %s",
			(unsigned)used_pct, victim);
		(void)delete_snippet_files(victim);
		deleted++;
	}
	LOG_WRN("rotate: freespace pass hit MAX_ITERATIONS=%d — bailing", MAX_ITERATIONS);
	return deleted;
}

int gosteady_snippet_rotate(uint32_t stale_age_seconds,
			    uint8_t rotate_threshold_pct)
{
	if (!atomic_get(&s_initialized)) {
		return -ENODEV;
	}
	char active_uuid[40];
	(void)snapshot_active_uuid(active_uuid, sizeof(active_uuid));
	int s = rotate_stale_pass(stale_age_seconds, active_uuid);
	if (s < 0) { return s; }
	int f = rotate_freespace_pass(rotate_threshold_pct, active_uuid);
	if (f < 0) { return f; }
	return s + f;
}

int gosteady_snippet_init(void)
{
	if (atomic_set(&s_initialized, 1) == 1) {
		return 0;
	}
	int ret = fs_mount(&snippet_mnt);
	if (ret < 0) {
		LOG_ERR("fs_mount(%s) failed (%d) — snippet capture disabled",
			SNIPPET_MNT, ret);
		atomic_set(&s_initialized, 0);
		return ret;
	}
	struct fs_statvfs vfs;
	if (fs_statvfs(SNIPPET_MNT, &vfs) == 0) {
		LOG_INF("snippet fs mounted: total=%lu B free=%lu B",
			(unsigned long)(vfs.f_blocks * vfs.f_frsize),
			(unsigned long)(vfs.f_bfree  * vfs.f_frsize));
	}
	(void)fs_mkdir(SNIPPET_MNT);  /* harmless if already exists */
	memset(&s_cap, 0, sizeof(s_cap));

	/* FMEA 6.1 (2026-05-10): boot-time rotation pass. Stale-cutoff is
	 * skipped if cellular UTC isn't available yet (typical at boot —
	 * cellular attaches a few seconds in). Free-space pass runs
	 * regardless. The heartbeat-thread call below covers the steady-
	 * state case where cellular IS up and we want stale-pass to run
	 * periodically. */
	int swept = gosteady_snippet_rotate(GOSTEADY_SNIPPET_STALE_AGE_S,
					     GOSTEADY_SNIPPET_ROTATE_THRESHOLD);
	if (swept > 0) {
		LOG_INF("boot-time snippet rotation: %d snippet(s) deleted",
			swept);
	}
	return 0;
}

/* Helper: write the 16-byte binary header at offset 0 of the open .bin
 * file. Caller holds s_capture_lock and has the file already open. */
static int write_bin_header_locked(uint32_t sample_count_n)
{
	struct snippet_bin_header h = {
		.format_version        = 1,
		.sensor_id             = 1,
		.sample_rate_hz        = 100,
		.sample_count_n        = sample_count_n,
		.window_start_uptime_ms = s_cap.window_start_uptime_ms,
	};
	int ret = fs_seek(&s_cap.file, 0, FS_SEEK_SET);
	if (ret < 0) { return ret; }
	ssize_t n = fs_write(&s_cap.file, &h, sizeof(h));
	if (n < 0) { return (int)n; }
	if (n != sizeof(h)) { return -EIO; }
	return 0;
}

int gosteady_snippet_capture_start(const char *session_uuid_str,
				    const char *window_start_ts,
				    uint64_t window_start_uptime_ms)
{
	if (!atomic_get(&s_initialized) || !session_uuid_str || !window_start_ts) {
		return -EINVAL;
	}

	/* FMEA 6.1 (2026-05-10): pre-capture rotation pass. Bound the
	 * capture-skip cascade — if the partition is at threshold, evict
	 * one .up-marked snippet now so we have room. Run BEFORE acquiring
	 * s_capture_lock to avoid a self-deadlock on snapshot_active_uuid. */
	(void)gosteady_snippet_rotate(GOSTEADY_SNIPPET_STALE_AGE_S,
				       GOSTEADY_SNIPPET_ROTATE_THRESHOLD);

	k_mutex_lock(&s_capture_lock, K_FOREVER);

	if (s_cap.active) {
		bool same = strcmp(s_cap.uuid, session_uuid_str) == 0;
		k_mutex_unlock(&s_capture_lock);
		return same ? 0 : -EALREADY;
	}

	memset(&s_cap, 0, sizeof(s_cap));
	strncpy(s_cap.uuid, session_uuid_str, sizeof(s_cap.uuid) - 1);
	strncpy(s_cap.window_start_ts, window_start_ts,
		sizeof(s_cap.window_start_ts) - 1);
	s_cap.window_start_uptime_ms = window_start_uptime_ms;

	char bin_path[80];
	path_for(s_cap.uuid, ".bin", bin_path, sizeof(bin_path));
	fs_file_t_init(&s_cap.file);
	int ret = fs_open(&s_cap.file, bin_path, FS_O_CREATE | FS_O_RDWR);
	if (ret < 0) {
		LOG_ERR("snippet open %s failed (%d)", bin_path, ret);
		k_mutex_unlock(&s_capture_lock);
		return ret;
	}

	/* Stamp placeholder header so fs_write of samples lands at offset 16. */
	ret = write_bin_header_locked(0);
	if (ret < 0) {
		LOG_ERR("snippet header placeholder write failed (%d)", ret);
		(void)fs_close(&s_cap.file);
		k_mutex_unlock(&s_capture_lock);
		return ret;
	}
	(void)fs_seek(&s_cap.file, BIN_HEADER_BYTES, FS_SEEK_SET);

	s_cap.active = true;
	k_mutex_unlock(&s_capture_lock);
	LOG_INF("snippet capture start: %s window_start_ts=%s",
		s_cap.uuid, s_cap.window_start_ts);
	return 0;
}

/* Flush the in-RAM batch to /snippets/<uuid>.bin. Caller holds
 * s_capture_lock and has the file open in append/write mode. */
static int flush_batch_locked(void)
{
	if (s_cap.batch_fill == 0) { return 0; }
	size_t want = s_cap.batch_fill * sizeof(s_cap.batch[0]);
	ssize_t got = fs_write(&s_cap.file, s_cap.batch, want);
	if (got < 0) { return (int)got; }
	if ((size_t)got != want) { return -EIO; }
	s_cap.batch_fill = 0;
	return 0;
}

int gosteady_snippet_capture_append(uint32_t t_ms,
				     float ax, float ay, float az,
				     float gx, float gy, float gz)
{
	if (!atomic_get(&s_initialized)) { return -EINVAL; }

	k_mutex_lock(&s_capture_lock, K_FOREVER);
	if (!s_cap.active) {
		k_mutex_unlock(&s_capture_lock);
		return -ENODATA;
	}
	if (t_ms >= GOSTEADY_SNIPPET_WINDOW_MS ||
	    s_cap.sample_count >= MAX_SAMPLES) {
		k_mutex_unlock(&s_capture_lock);
		return -EOVERFLOW;
	}
	s_cap.batch[s_cap.batch_fill++] = (struct snippet_sample){
		.t_ms = t_ms, .ax = ax, .ay = ay, .az = az,
		.gx = gx, .gy = gy, .gz = gz,
	};
	s_cap.sample_count++;
	if (s_cap.batch_fill == SNIPPET_BATCH_SIZE) {
		int rc = flush_batch_locked();
		if (rc < 0) {
			k_mutex_unlock(&s_capture_lock);
			return rc;
		}
	}
	k_mutex_unlock(&s_capture_lock);
	return 0;
}

int gosteady_snippet_capture_finish(void)
{
	if (!atomic_get(&s_initialized)) { return 0; }

	k_mutex_lock(&s_capture_lock, K_FOREVER);
	if (!s_cap.active) {
		k_mutex_unlock(&s_capture_lock);
		return 0;
	}

	/* Flush the trailing partial batch before rewriting the header
	 * (otherwise the batch's samples would be lost or land after the
	 * header rewrite). */
	(void)flush_batch_locked();

	/* Rewrite the binary header with the real sample_count_n. */
	int ret = write_bin_header_locked(s_cap.sample_count);
	if (ret < 0) {
		LOG_ERR("snippet header final-rewrite failed (%d)", ret);
	}
	(void)fs_sync(&s_cap.file);
	(void)fs_close(&s_cap.file);

	/* Write JSON sidecar. v1 fields: snippet_id, window_start_ts.
	 * v1.5 adds anomaly_trigger when the anomaly path lands. */
	char json_path[80];
	path_for(s_cap.uuid, ".json", json_path, sizeof(json_path));
	struct fs_file_t jf;
	fs_file_t_init(&jf);
	int jret = fs_open(&jf, json_path, FS_O_CREATE | FS_O_WRITE);
	if (jret < 0) {
		LOG_ERR("snippet json sidecar open %s failed (%d)", json_path, jret);
	} else {
		char body[MAX_JSON_BYTES];
		int n = snprintf(body, sizeof(body),
			"{\"snippet_id\":\"%s\",\"window_start_ts\":\"%s\"}",
			s_cap.uuid, s_cap.window_start_ts);
		if (n > 0 && (size_t)n < sizeof(body)) {
			(void)fs_write(&jf, body, (size_t)n);
		}
		(void)fs_sync(&jf);
		(void)fs_close(&jf);
	}

	uint32_t count = s_cap.sample_count;
	char uuid[40];
	strncpy(uuid, s_cap.uuid, sizeof(uuid));
	memset(&s_cap, 0, sizeof(s_cap));
	k_mutex_unlock(&s_capture_lock);

	LOG_INF("snippet capture finish: %s samples=%u (%u B body)",
		uuid, (unsigned)count,
		(unsigned)(BIN_HEADER_BYTES + count * SAMPLE_BYTES));
	return 0;
}

/* ---- Upload helpers ---- */

/* Iterate /snippets/, find the oldest <uuid>.bin file that does NOT
 * have a paired <uuid>.up marker. Strict FIFO by mtime would require
 * stat for every entry — overkill for a partition that holds at most a
 * few hundred files. We just take the FIRST not-yet-uploaded entry
 * fs_readdir gives us, which is good enough for v1 (LittleFS readdir
 * order is roughly insertion order). v1.5 can stat-and-sort if real
 * deployment data shows ordering bugs. */
static int find_pending_uuid(char *out_uuid, size_t out_sz)
{
	struct fs_dir_t dir;
	fs_dir_t_init(&dir);
	int ret = fs_opendir(&dir, SNIPPET_MNT);
	if (ret < 0) { return ret; }

	struct fs_dirent ent;
	int found = -ENOENT;
	while (1) {
		ret = fs_readdir(&dir, &ent);
		if (ret < 0 || ent.name[0] == '\0') { break; }
		size_t nlen = strlen(ent.name);
		if (nlen <= 4 || strcmp(ent.name + nlen - 4, ".bin") != 0) {
			continue;
		}
		/* Strip ".bin" → uuid candidate. */
		char uuid[40];
		size_t ulen = nlen - 4;
		if (ulen >= sizeof(uuid)) { continue; }
		memcpy(uuid, ent.name, ulen);
		uuid[ulen] = '\0';

		/* Check if <uuid>.up exists → already uploaded, skip. */
		char up_path[80];
		(void)snprintf(up_path, sizeof(up_path), SNIPPET_MNT "/%s.up", uuid);
		struct fs_dirent up_ent;
		if (fs_stat(up_path, &up_ent) == 0) { continue; }

		strncpy(out_uuid, uuid, out_sz - 1);
		out_uuid[out_sz - 1] = '\0';
		found = 0;
		break;
	}
	(void)fs_closedir(&dir);
	return found;
}

/* Mark a snippet as uploaded by creating a zero-byte <uuid>.up marker. */
static int mark_uploaded(const char *uuid)
{
	char up_path[80];
	(void)snprintf(up_path, sizeof(up_path), SNIPPET_MNT "/%s.up", uuid);
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, up_path, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) { return ret; }
	(void)fs_close(&f);
	return 0;
}

int gosteady_snippet_upload_one(gosteady_snippet_publish_fn publish_fn)
{
	if (!atomic_get(&s_initialized) || !publish_fn) { return -EINVAL; }

	char uuid[40] = { 0 };
	int ret = find_pending_uuid(uuid, sizeof(uuid));
	if (ret < 0) {
		return ret;  /* -ENOENT = nothing to upload, normal */
	}

	char bin_path[80], json_path[80];
	path_for(uuid, ".bin", bin_path, sizeof(bin_path));
	path_for(uuid, ".json", json_path, sizeof(json_path));

	/* Single shared payload buffer — file-scope static to avoid blowing
	 * the worker thread's stack (84 KB is huge by stack standards) and
	 * to avoid double-buffering (we read each section directly into its
	 * final position in the outer framing). RAM budget: 84 KB BSS;
	 * checked against the build's RAM region size. */
	static uint8_t s_payload[MAX_PAYLOAD_BYTES];

	/* Read the JSON sidecar directly into its slot at offset 4. We
	 * don't yet know json_len, so reserve sizeof(s_payload) - 4 minus
	 * the binary header reservation as the upper bound; first-pass
	 * write here then we'll memcpy the binary on top of any tail. */
	struct fs_file_t jf;
	fs_file_t_init(&jf);
	ret = fs_open(&jf, json_path, FS_O_READ);
	if (ret < 0) {
		/* Orphaned .bin (sidecar missing → capture_finish never ran,
		 * e.g., previous boot HardFault / WDT). Without cleanup,
		 * find_pending_uuid keeps returning this same orphan every
		 * heartbeat → infinite "missing sidecar" errors and zero
		 * progress on real snippets behind it. Best-effort delete
		 * the .bin so the next upload tick gets the next entry. */
		LOG_WRN("snippet upload: json sidecar %s missing (%d) — deleting orphan .bin",
			json_path, ret);
		(void)fs_unlink(bin_path);
		return ret;
	}
	ssize_t json_len = fs_read(&jf, s_payload + 4, MAX_JSON_BYTES);
	(void)fs_close(&jf);
	if (json_len <= 0) { return -EIO; }

	/* Read the binary body directly into the slot AFTER the JSON. */
	struct fs_file_t bf;
	fs_file_t_init(&bf);
	ret = fs_open(&bf, bin_path, FS_O_READ);
	if (ret < 0) {
		LOG_ERR("snippet upload: bin %s open failed (%d)", bin_path, ret);
		return ret;
	}
	ssize_t bin_len = fs_read(&bf, s_payload + 4 + json_len,
				    sizeof(s_payload) - 4 - json_len);
	(void)fs_close(&bf);
	if (bin_len <= 0) { return -EIO; }

	/* Stamp the 4-byte BE length-prefix per §F.3. */
	sys_put_be32((uint32_t)json_len, s_payload);
	size_t payload_len = 4 + (size_t)json_len + (size_t)bin_len;

	LOG_INF("snippet upload: %s — %u B (json=%u, bin=%u)",
		uuid, (unsigned)payload_len,
		(unsigned)json_len, (unsigned)bin_len);

	ret = publish_fn(s_payload, payload_len);
	if (ret < 0) {
		LOG_ERR("snippet publish failed (%d) — will retry next wake", ret);
		return ret;
	}

	ret = mark_uploaded(uuid);
	if (ret < 0) {
		LOG_WRN("snippet upload: mark_uploaded(%s) failed (%d) — may re-upload",
			uuid, ret);
	}
	return 0;
}
