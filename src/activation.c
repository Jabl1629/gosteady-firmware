/*
 * M12.1e.2 — activation state persistence.
 *
 * Persists to /lfs/activation.bin as a small packed struct (via LittleFS,
 * which is already mounted by the time this module is initialised). The
 * record format is versioned + magic-gated so a fresh / wiped LittleFS
 * starts in the "not activated" default. Writes are infrequent (once per
 * activate cmd ≈ once-per-device-lifetime in v1; possibly twice if a
 * Phase 2A re-provision happens), so per-write cost is irrelevant.
 *
 * Why a small file in /lfs rather than the crash_forensics partition:
 * the activation state is a normal app-data concern, not a postmortem.
 * Mixing it into crash_forensics would couple two independent invariants
 * (activation state survives FS corruption only if forensics writes do
 * too — fragile). Keeping it in /lfs alongside boot_count is the simpler
 * design; if /lfs corrupts, the device falls back to "not activated"
 * which is the correct fail-safe.
 */

#include "activation.h"

#include <string.h>
#include <stdio.h>

#include <zephyr/kernel.h>
#include <zephyr/fs/fs.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>

LOG_MODULE_REGISTER(gs_activation, LOG_LEVEL_INF);

#define ACTIVATION_PATH       "/lfs/activation.bin"
#define ACTIVATION_MAGIC      0x47535631u  /* 'GSV1' = GoSteady V1 activation */
#define ACTIVATION_VERSION    1u

#define MAX_CMD_ID_LEN        40   /* "act_<uuid>" + NUL */
#define MAX_ACTIVATED_AT_LEN  24   /* "YYYY-MM-DDTHH:MM:SSZ" + NUL */

struct __attribute__((packed)) gs_activation_record {
	uint32_t magic;
	uint32_t version;
	char     activated_at_iso[MAX_ACTIVATED_AT_LEN];
	char     last_cmd_id[MAX_CMD_ID_LEN];
	uint8_t  _reserved[16];
};
_Static_assert(sizeof(struct gs_activation_record) == 88,
	"gs_activation_record size drift");

static atomic_t s_activated = ATOMIC_INIT(0);
static struct gs_activation_record s_record;
static K_MUTEX_DEFINE(s_lock);

static int read_file(struct gs_activation_record *out)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, ACTIVATION_PATH, FS_O_READ);
	if (ret < 0) { return ret; }
	ssize_t n = fs_read(&f, out, sizeof(*out));
	fs_close(&f);
	if (n < 0) { return (int)n; }
	if ((size_t)n != sizeof(*out)) { return -EIO; }
	return 0;
}

static int write_file_locked(const struct gs_activation_record *r)
{
	struct fs_file_t f;
	fs_file_t_init(&f);
	int ret = fs_open(&f, ACTIVATION_PATH, FS_O_CREATE | FS_O_WRITE);
	if (ret < 0) { return ret; }
	ssize_t n = fs_write(&f, r, sizeof(*r));
	(void)fs_sync(&f);
	(void)fs_close(&f);
	if (n < 0) { return (int)n; }
	if ((size_t)n != sizeof(*r)) { return -EIO; }
	return 0;
}

int gosteady_activation_init(void)
{
	struct gs_activation_record tmp;
	int ret = read_file(&tmp);
	if (ret == 0 && tmp.magic == ACTIVATION_MAGIC &&
	    tmp.version == ACTIVATION_VERSION) {
		k_mutex_lock(&s_lock, K_FOREVER);
		s_record = tmp;
		k_mutex_unlock(&s_lock);
		atomic_set(&s_activated, 1);
		LOG_INF("activated: at=%s cmd_id=%s",
			s_record.activated_at_iso, s_record.last_cmd_id);
	} else {
		LOG_INF("not activated (read=%d magic=0x%08x) — pre-activation state",
			ret, (ret == 0) ? (unsigned)tmp.magic : 0u);
		memset(&s_record, 0, sizeof(s_record));
		atomic_set(&s_activated, 0);
	}
	return 0;
}

bool gosteady_activation_is_activated(void)
{
	return atomic_get(&s_activated) != 0;
}

int gosteady_activation_get_at(char *out, size_t out_sz)
{
	if (!out || out_sz < MAX_ACTIVATED_AT_LEN) { return -EINVAL; }
	if (!atomic_get(&s_activated)) {
		out[0] = '\0';
		return -ENODATA;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	strncpy(out, s_record.activated_at_iso, out_sz - 1);
	out[out_sz - 1] = '\0';
	k_mutex_unlock(&s_lock);
	return 0;
}

int gosteady_activation_get_last_cmd_id(char *out, size_t out_sz)
{
	if (!out || out_sz < MAX_CMD_ID_LEN) { return -EINVAL; }
	if (!atomic_get(&s_activated)) {
		out[0] = '\0';
		return -ENODATA;
	}
	k_mutex_lock(&s_lock, K_FOREVER);
	strncpy(out, s_record.last_cmd_id, out_sz - 1);
	out[out_sz - 1] = '\0';
	k_mutex_unlock(&s_lock);
	return 0;
}

int gosteady_activation_apply(const char *cmd_id, const char *activated_at_iso)
{
	if (!cmd_id || !activated_at_iso) { return -EINVAL; }

	k_mutex_lock(&s_lock, K_FOREVER);

	/* Idempotent: if the cmd_id matches the persisted one and we're
	 * already activated, this is a cloud retry — log + skip the flash
	 * write but still return 0 so the caller treats it as success. */
	if (atomic_get(&s_activated) &&
	    strncmp(s_record.last_cmd_id, cmd_id, MAX_CMD_ID_LEN) == 0) {
		k_mutex_unlock(&s_lock);
		LOG_INF("activation_apply: idempotent retry of cmd_id=%s — no-op", cmd_id);
		return 0;
	}

	memset(&s_record, 0, sizeof(s_record));
	s_record.magic   = ACTIVATION_MAGIC;
	s_record.version = ACTIVATION_VERSION;
	strncpy(s_record.activated_at_iso, activated_at_iso, MAX_ACTIVATED_AT_LEN - 1);
	strncpy(s_record.last_cmd_id, cmd_id, MAX_CMD_ID_LEN - 1);

	int ret = write_file_locked(&s_record);
	if (ret < 0) {
		LOG_ERR("activation_apply: write_file failed (%d) — state NOT persisted", ret);
		k_mutex_unlock(&s_lock);
		return ret;
	}
	k_mutex_unlock(&s_lock);

	atomic_set(&s_activated, 1);
	LOG_INF("activation applied: at=%s cmd_id=%s (persisted to %s)",
		activated_at_iso, cmd_id, ACTIVATION_PATH);
	return 0;
}

int gosteady_activation_clear(void)
{
	k_mutex_lock(&s_lock, K_FOREVER);
	memset(&s_record, 0, sizeof(s_record));
	int ret = fs_unlink(ACTIVATION_PATH);
	k_mutex_unlock(&s_lock);
	atomic_set(&s_activated, 0);
	if (ret == 0 || ret == -ENOENT) {
		LOG_WRN("activation cleared — device re-entered pre-activation state");
		return 0;
	}
	LOG_ERR("activation_clear: fs_unlink failed (%d)", ret);
	return ret;
}
