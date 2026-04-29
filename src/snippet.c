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

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/byteorder.h>

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
