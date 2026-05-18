/*
 * GoSteady firmware — wipe routine implementation.
 *
 * See wipe.h for the contract + decisions log pointer. This file just
 * implements gosteady_wipe_now() by orchestrating the existing module
 * APIs.
 */

#include "wipe.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <net/aws_iot.h>

#include "activation.h"
#include "battery.h"
#include "cellular.h"
#include "session.h"

#if defined(CONFIG_GOSTEADY_SNIPPET_ENABLE)
#include "snippet.h"
#endif

/* gosteady_cloud_set_last_cmd_id lives in cloud.h. We avoid a full
 * cloud.h include here to keep the dependency surface minimal — the
 * single function declaration is reproduced. */
extern void gosteady_cloud_set_last_cmd_id(const char *cmd_id);

LOG_MODULE_REGISTER(gs_wipe, LOG_LEVEL_INF);

#define MAX_WIPE_ID_LEN          48
#define MAX_COMPLETED_AT_LEN     24

/* Build the Shadow `reported.wipe_complete` JSON body and publish via
 * the AWS IoT lib's SHADOW_UPDATE channel. Mirrors cloud.c::
 * write_reported_activated_at — kept here (not factored into cloud.c)
 * because the wipe surface is otherwise self-contained, and adding a
 * shared "write_reported_*" helper would push cloud.c's API surface
 * outward for a one-extra-caller win.
 *
 * Body shape:
 *   {"state":{"reported":{"wipe_complete":"<wipe_id>",
 *                          "wipe_completed_at":"<iso>"}}}
 */
static int write_reported_wipe_complete(const char *wipe_id,
					const char *completed_at_iso)
{
	char body[192];
	int n = snprintf(body, sizeof(body),
		"{\"state\":{\"reported\":{"
		"\"wipe_complete\":\"%s\","
		"\"wipe_completed_at\":\"%s\""
		"}}}",
		wipe_id, completed_at_iso);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		LOG_ERR("wipe_complete body truncated (n=%d)", n);
		return -ENOMEM;
	}
	struct aws_iot_data tx = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic = { .type = AWS_IOT_SHADOW_TOPIC_UPDATE },
		.ptr = body,
		.len = (size_t)n,
	};
	int rc = aws_iot_send(&tx);
	if (rc) {
		LOG_ERR("wipe_complete shadow send failed: %d", rc);
		return -ECOMM;
	}
	LOG_INF("wrote reported.wipe_complete=%s completed_at=%s to Shadow",
		wipe_id, completed_at_iso);
	return 0;
}

/* Best-effort ISO 8601 wall-clock from the cellular UTC source. Returns
 * empty string + nonzero rc on failure; caller falls back to publishing
 * an empty completed_at (cloud-side accepts via accept-extra-fields
 * contract; the wipe_id echo in heartbeat last_cmd_id is the canonical
 * ack signal anyway).
 */
static int build_completed_at_iso(char *out, size_t out_sz)
{
	if (out_sz < MAX_COMPLETED_AT_LEN) { return -EINVAL; }
	out[0] = '\0';
	int rc = gosteady_cellular_get_network_time(out, out_sz);
	if (rc != 0) {
		LOG_WRN("wipe: cellular UTC unavailable (%d) — completed_at will be empty",
			rc);
		out[0] = '\0';
		return rc;
	}
	return 0;
}

int gosteady_wipe_now(const char *wipe_id)
{
	if (!wipe_id || wipe_id[0] == '\0') {
		LOG_ERR("wipe: missing wipe_id");
		return -EINVAL;
	}

	/* Defensive bound on wipe_id length — cloud uses prefix `wipe_` +
	 * a 36-char UUID, total 41 chars. Cap at MAX_WIPE_ID_LEN-1 = 47
	 * so the s_last_cmd_id [48] buffer in cloud.c never truncates
	 * (the §C18 buffer-truncation lesson). */
	size_t id_len = strnlen(wipe_id, MAX_WIPE_ID_LEN);
	if (id_len >= MAX_WIPE_ID_LEN) {
		LOG_ERR("wipe: wipe_id too long (len>=%d) — ignoring",
			MAX_WIPE_ID_LEN);
		return -EINVAL;
	}

	/* Step 1: battery floor check.
	 * gosteady_battery_get returns -EAGAIN if the cache is still
	 * warming up (placeholder 0.5); treat that as "wait" rather than
	 * proceeding on a placeholder. The cloud will retry on the next
	 * heartbeat once the cache is real. */
	float battery_pct = 0.0f;
	uint16_t battery_mv = 0;
	int berr = gosteady_battery_get(&battery_mv, &battery_pct);
	if (berr == -EAGAIN) {
		LOG_WRN("wipe: battery cache warming up — defer to next wake");
		return -EAGAIN;
	}
	if (berr < 0 && berr != -EAGAIN) {
		LOG_ERR("wipe: battery_get failed (%d)", berr);
		return -ENODEV;
	}
	if (battery_pct < GOSTEADY_WIPE_BATTERY_FLOOR_PCT) {
		LOG_WRN("wipe: battery_pct=%.3f below floor %.2f — defer (cloud retries on next wake post-AA-swap)",
			(double)battery_pct,
			(double)GOSTEADY_WIPE_BATTERY_FLOOR_PCT);
		return -EAGAIN;
	}
	LOG_INF("wipe: battery_pct=%.3f mv=%u — proceeding", (double)battery_pct,
		(unsigned)battery_mv);

	/* Step 2: stop any active session cleanly before unlinking .dat
	 * files. session_stop is a no-op if no session is active. */
	if (gosteady_session_is_active()) {
		LOG_WRN("wipe: active session — stopping it (any in-flight sample buffer is dropped)");
		uint32_t dropped = 0;
		int srt = gosteady_session_stop(&dropped);
		if (srt < 0) {
			LOG_ERR("wipe: session_stop failed (%d) — proceeding anyway, .dat file will be swept",
				srt);
		} else {
			LOG_INF("wipe: session stopped (samples dropped=%u)",
				(unsigned)dropped);
		}
	}

	/* Step 3: clear /lfs/activation.bin + in-RAM activated atomic. */
	int arc = gosteady_activation_clear();
	if (arc < 0) {
		LOG_ERR("wipe: activation_clear failed (%d) — partial wipe", arc);
		/* Don't bail — keep going. Even if activation.bin lingers
		 * due to flash error, the snippet+session purges still
		 * remove patient-derived data, and the next boot's
		 * activation_init will reload the stale record which is
		 * the existing tolerated behavior (force_reset admin path
		 * cleans up). */
	}

	/* Step 4: purge /lfs/sessions/*.dat. orphan_sweep is the right
	 * tool: it unconditionally fs_unlinks every .dat file in the
	 * directory (FMEA 6.2 semantics — see session.h:243). The name
	 * is historical; the operation is "purge all .dat". */
	int swept = gosteady_session_orphan_sweep();
	if (swept < 0) {
		LOG_ERR("wipe: session_orphan_sweep failed (%d) — partial wipe",
			swept);
	} else {
		LOG_INF("wipe: session purge removed %d .dat file(s)", swept);
	}

	/* Step 5: purge /snippets/* — only if snippet capture compiled in.
	 * snippet_purge_all is defined in this batch (snippet.c new
	 * function) and iterates the whole partition. */
#if defined(CONFIG_GOSTEADY_SNIPPET_ENABLE)
	int spurged = gosteady_snippet_purge_all();
	if (spurged < 0) {
		LOG_ERR("wipe: snippet_purge_all failed (%d) — partial wipe",
			spurged);
	} else {
		LOG_INF("wipe: snippet purge removed %d snippet file-set(s)",
			spurged);
	}
#else
	LOG_INF("wipe: snippet capture not compiled in — skipping snippet purge");
#endif

	/* Step 6: record wipe_id so the next heartbeat echoes it in
	 * last_cmd_id (cloud's primary ack-matching path). */
	gosteady_cloud_set_last_cmd_id(wipe_id);

	/* Step 7: best-effort Shadow ack write. Cloud has TWO ack paths:
	 *   (a) heartbeat last_cmd_id echo (step 6 above; guaranteed
	 *       to fire on next heartbeat in this connection or the
	 *       next one)
	 *   (b) Shadow reported.wipe_complete (this step; sent over the
	 *       same MQTT connection that delivered the wipe cmd)
	 * Either is sufficient — both firing is idempotent on the cloud
	 * side. So we soft-fail this step: log + return success on the
	 * happy path, log + return -ECOMM if shadow send fails (callers
	 * may want to treat that as non-fatal too). */
	char completed_at[MAX_COMPLETED_AT_LEN] = { 0 };
	(void)build_completed_at_iso(completed_at, sizeof(completed_at));

	int src = write_reported_wipe_complete(wipe_id, completed_at);
	if (src < 0) {
		LOG_WRN("wipe: shadow ack failed (%d) — heartbeat last_cmd_id echo is fallback",
			src);
		return src;
	}

	LOG_INF("wipe complete: wipe_id=%s completed_at=%s sessions_swept=%d",
		wipe_id, completed_at[0] ? completed_at : "<unavailable>",
		swept >= 0 ? swept : 0);
	return 0;
}
