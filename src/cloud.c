/*
 * M12.1c + M12.1d — AWS IoT MQTT/TLS publisher.
 *
 * Two outbound paths share one aws_iot lib instance via a mutex:
 *
 *   1. M12.1c.2 heartbeat (HOURLY). Worker thread waits for cellular
 *      registration + network time + signal stats on first iteration,
 *      then loops: build payload, connect+publish+disconnect, sleep one
 *      heartbeat interval. Payload is the 5-field required block
 *      (serial / ts / battery_pct / rsrp_dbm / snr_db) plus all locked
 *      portal-spec optional extras (battery_mv / firmware / uptime_s /
 *      last_cmd_id / reset_reason / fault_counters / watchdog_hits).
 *      M12.1c.1 was the one-shot bring-up; this is the production shape.
 *
 *   2. M12.1d activity (one per session close). session.c calls
 *      gosteady_cloud_publish_activity() which copies the activity record
 *      into a msgq; the activity worker thread drains and publishes each
 *      to gs/{client_id}/activity with all 6 required fields + optional
 *      roughness_R / surface_class / firmware_version. Schema per coord
 *      doc §C.7 + §F.3.
 *
 * Both publishes use QoS 1 and wait for PUBACK before disconnect — without
 * that, on NB-IoT the modem tears down TCP before the PUBLISH bytes hit
 * the wire (proven empirically during M12.1c.1 closure debug).
 *
 * Heartbeat retry policy: on connect/publish failure, sleep with linear
 * backoff (1 / 5 / 15 min) up to 3 attempts, then give up on this hour's
 * heartbeat and wait for the next scheduled tick. Bounds the energy cost
 * of a sustained cellular outage (offline-detection cloud-side will mark
 * device as offline at 2 hr without heartbeat anyway, so trying harder
 * than ~21 min into the hour buys us nothing).
 *
 * Verbose event logging on uart0 so first-time TLS / MQTT issues are
 * diagnosable from logs/uart0_*.log alone.
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <net/aws_iot.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cellular.h"
#include "cloud.h"
#include "version.h"
#include "activation.h"

#include <zephyr/data/json.h>
#include <zephyr/net/mqtt.h>

#if defined(CONFIG_NRF_FUEL_GAUGE)
#include "battery.h"
#endif

#if defined(CONFIG_GOSTEADY_FORENSICS_ENABLE)
#include "forensics.h"
#endif

LOG_MODULE_REGISTER(gs_cloud, LOG_LEVEL_INF);

#define HEARTBEAT_TOPIC_FMT   "gs/%s/heartbeat"
#define ACTIVITY_TOPIC_FMT    "gs/%s/activity"
#define CMD_TOPIC_FMT         "gs/%s/cmd"
#define TOPIC_MAX             64

/* Backing store for the application-topic subscription (gs/{serial}/cmd).
 * aws_iot_application_topics_set takes a list of mqtt_topics and the lib
 * holds pointers into the backing memory across the connect cycle. Sized
 * for one entry; bump if M12.1f adds more app topics. */
static char s_cmd_topic[TOPIC_MAX];
static struct mqtt_topic s_app_topics[1];
/* Production heartbeat payload sized for: 5 required fields + battery_mv +
 * firmware + uptime_s + last_cmd_id (≤32) + reset_reason (≤64) +
 * fault_counters object + watchdog_hits + structural overhead. ~360 B max
 * observed; 512 B leaves comfortable headroom for future schema additions. */
#define HEARTBEAT_PAYLOAD_MAX 512
#define ACTIVITY_PAYLOAD_MAX  384

/* M12.1c.2 hourly cadence + linear-backoff retry on the same wake. */
#define HEARTBEAT_INTERVAL    K_SECONDS(60 * 60)
#define HEARTBEAT_RETRY_1ST   K_SECONDS(60)
#define HEARTBEAT_RETRY_2ND   K_SECONDS(60 * 5)
#define HEARTBEAT_RETRY_3RD   K_SECONDS(60 * 15)
#define HEARTBEAT_MAX_ATTEMPTS 3

/* Wait windows. aws_iot_connect is documented as "synchronous and only returns
 * success when the client has connected" but in practice the CONNECTED event
 * is delivered to our handler asynchronously after CONNACK. Sem-based wait
 * mirrors what the upstream sample does. */
#define CONNECT_WAIT      K_SECONDS(60)
#define DISCONNECT_WAIT   K_SECONDS(10)
#define PUBACK_WAIT       K_SECONDS(30)  /* QoS 1 PUBACK from AWS broker */

/* Activity msgq — caps at 4 pending activities. Realistic back-to-back
 * session-closure rate is well below 1/min; 4 slots covers any plausible
 * burst without failing the publish (caller drops on -EAGAIN, cellular
 * path catches up). */
#define ACTIVITY_MSGQ_DEPTH 4
K_MSGQ_DEFINE(s_activity_msgq, sizeof(struct gosteady_activity),
	      ACTIVITY_MSGQ_DEPTH, 4);

/* Serializes aws_iot lib operations between heartbeat + activity workers.
 * aws_iot_init/connect/send/disconnect are not safe to interleave from
 * multiple threads. */
static K_MUTEX_DEFINE(s_aws_mutex);

static K_SEM_DEFINE(s_connected,    0, 1);
static K_SEM_DEFINE(s_disconnected, 0, 1);
static K_SEM_DEFINE(s_puback,       0, 1);

#if defined(CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK)
/* M12.1e.1 bench-check sems. Given on the corresponding shadow event
 * type. Used by the bench thread to gate get → update sequencing. */
static K_SEM_DEFINE(s_shadow_get_accepted,    0, 1);
static K_SEM_DEFINE(s_shadow_update_accepted, 0, 1);
#endif

static atomic_t s_initialized = ATOMIC_INIT(0);

/* last_cmd_id: most-recent downlink cmd_id received from gs/{serial}/cmd.
 * Echoed in heartbeat as ack-on-uplink per portal contract. M12.1c.2 ships
 * the field plumbing; M12.1e.2 will populate it when the activate cmd
 * arrives. Empty string means "no cmd seen yet" → field omitted from
 * payload (cloud accepts missing optional). */
static K_MUTEX_DEFINE(s_last_cmd_lock);
static char s_last_cmd_id[40] = { 0 };  /* "act_<uuid>" plausibly fits in 40 */

static const char *client_id(void)
{
	return CONFIG_AWS_IOT_CLIENT_ID_STATIC;
}

/* ---- M12.1e.2 activate cmd parser ----
 *
 * Schema (locked 2026-04-17 in coord doc):
 *   {"cmd":"activate","cmd_id":"act_<uuid>","ts":"<ISO8601>","session_id":"..."}
 *
 * Zephyr json lib needs a json_obj_descr table. session_id is captured
 * but currently unused (audit-only on cloud side); kept in the parse
 * struct so we don't fail-validate on payloads that include it. */
struct activate_cmd_json {
	const char *cmd;
	const char *cmd_id;
	const char *ts;
	const char *session_id;
};

static const struct json_obj_descr activate_cmd_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct activate_cmd_json, cmd,        JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct activate_cmd_json, cmd_id,     JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct activate_cmd_json, ts,         JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct activate_cmd_json, session_id, JSON_TOK_STRING),
};

/* Best-effort write of state.reported.activated_at as the device-side
 * ack of an applied activate cmd. Per coord §C.4.4 invariant: cloud-side
 * device-shadow-handler (Phase 2A) eventually consumes this and flips
 * Device Registry.activated_at; for v1 we just need to write it
 * correctly so the round-trip is observable in the Shadow doc. Not
 * gated on success — the heartbeat last_cmd_id echo is the canonical
 * ack per coord §C.2 / D14a. */
static int write_reported_activated_at(const char *iso)
{
	char body[160];
	int n = snprintf(body, sizeof(body),
		"{\"state\":{\"reported\":{\"activated_at\":\"%s\"}}}", iso);
	if (n < 0 || (size_t)n >= sizeof(body)) {
		LOG_ERR("reported.activated_at body truncated");
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
		LOG_ERR("reported.activated_at send failed: %d", rc);
		return rc;
	}
	LOG_INF("wrote reported.activated_at=%s to Shadow", iso);
	return 0;
}

/* Handle an inbound `activate` cmd.
 *  - Persist via gosteady_activation_apply (idempotent on cmd_id retry).
 *  - Echo cmd_id in next heartbeat via the M12.1c.2 plumbing.
 *  - Best-effort ack-write to Shadow.reported.activated_at.
 *
 * Per coord §C.4.4 ("stray activate cmd handling"): treat any received
 * activate as authoritative — cloud is canonical issuance authority,
 * firmware shouldn't second-guess.
 */
static void handle_activate_cmd(const struct activate_cmd_json *c)
{
	if (!c->cmd_id || !c->ts) {
		LOG_ERR("activate cmd missing required fields (cmd_id=%p ts=%p) — ignored",
			c->cmd_id, c->ts);
		return;
	}
	LOG_INF("activate cmd received: cmd_id=%s ts=%s session_id=%s",
		c->cmd_id, c->ts, c->session_id ? c->session_id : "<none>");

	int ret = gosteady_activation_apply(c->cmd_id, c->ts);
	if (ret < 0) {
		LOG_ERR("activation_apply failed (%d) — cmd_id NOT acked", ret);
		return;
	}
	gosteady_cloud_set_last_cmd_id(c->cmd_id);
	(void)write_reported_activated_at(c->ts);
}

/* Dispatch an inbound app-specific topic message. We only handle the
 * `activate` cmd today; future cmds (e.g., remote-stop, snippet-arm)
 * land here too. */
static void dispatch_app_message(const struct aws_iot_data *msg)
{
	if (!msg || !msg->ptr || msg->len == 0) { return; }
	LOG_INF("app msg on '%.*s' (len=%u): %.*s",
		(int)msg->topic.len, msg->topic.str,
		(unsigned)msg->len,
		(int)msg->len, msg->ptr);

	struct activate_cmd_json parsed = { 0 };
	int rc = json_obj_parse((char *)msg->ptr, msg->len,
				 activate_cmd_descr, ARRAY_SIZE(activate_cmd_descr),
				 &parsed);
	if (rc < 0) {
		LOG_ERR("app msg JSON parse failed: %d", rc);
		return;
	}
	if (!parsed.cmd) {
		LOG_ERR("app msg has no cmd field — ignored");
		return;
	}
	if (strcmp(parsed.cmd, "activate") == 0) {
		handle_activate_cmd(&parsed);
	} else {
		LOG_WRN("unknown cmd '%s' — ignored (forward-compat)", parsed.cmd);
	}
}

static void aws_iot_event_handler(const struct aws_iot_evt *evt)
{
	switch (evt->type) {
	case AWS_IOT_EVT_CONNECTING:
		LOG_INF("evt: CONNECTING");
		break;
	case AWS_IOT_EVT_CONNECTED:
		LOG_INF("evt: CONNECTED (persistent_session=%d)",
			(int)evt->data.persistent_session);
		k_sem_give(&s_connected);
		break;
	case AWS_IOT_EVT_DISCONNECTED:
		LOG_INF("evt: DISCONNECTED");
		k_sem_give(&s_disconnected);
		break;
	case AWS_IOT_EVT_DATA_RECEIVED:
		LOG_INF("evt: DATA_RECEIVED len=%u type_received=%d",
			(unsigned)evt->data.msg.len,
			(int)evt->data.msg.topic.type_received);
		switch (evt->data.msg.topic.type_received) {
		case AWS_IOT_SHADOW_TOPIC_APPLICATION_SPECIFIC:
			/* M12.1e.2: gs/{serial}/cmd — activate / future commands. */
			dispatch_app_message(&evt->data.msg);
			break;
#if defined(CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK)
		case AWS_IOT_SHADOW_TOPIC_GET_ACCEPTED:
			LOG_INF("shadow GET ACCEPTED — payload (%u B): %.*s",
				(unsigned)evt->data.msg.len,
				(int)evt->data.msg.len, evt->data.msg.ptr);
			k_sem_give(&s_shadow_get_accepted);
			break;
		case AWS_IOT_SHADOW_TOPIC_GET_REJECTED:
			LOG_ERR("shadow GET REJECTED — payload (%u B): %.*s",
				(unsigned)evt->data.msg.len,
				(int)evt->data.msg.len, evt->data.msg.ptr);
			break;
		case AWS_IOT_SHADOW_TOPIC_UPDATE_ACCEPTED:
			LOG_INF("shadow UPDATE ACCEPTED — payload (%u B): %.*s",
				(unsigned)evt->data.msg.len,
				(int)evt->data.msg.len, evt->data.msg.ptr);
			k_sem_give(&s_shadow_update_accepted);
			break;
		case AWS_IOT_SHADOW_TOPIC_UPDATE_REJECTED:
			LOG_ERR("shadow UPDATE REJECTED — payload (%u B): %.*s",
				(unsigned)evt->data.msg.len,
				(int)evt->data.msg.len, evt->data.msg.ptr);
			break;
#endif
		default:
			break;
		}
		break;
	case AWS_IOT_EVT_PUBACK:
		LOG_INF("evt: PUBACK msg_id=%u", (unsigned)evt->data.message_id);
		k_sem_give(&s_puback);
		break;
	case AWS_IOT_EVT_PINGRESP:
		LOG_DBG("evt: PINGRESP");
		break;
	case AWS_IOT_EVT_ERROR:
		LOG_ERR("evt: ERROR err=%d", evt->data.err);
		break;
	default:
		LOG_INF("evt: type=%d (unhandled)", (int)evt->type);
		break;
	}
}

static int wait_for_cellular_ready(void)
{
	LOG_INF("waiting for cellular registration...");
	while (!gosteady_cellular_is_registered()) {
		k_sleep(K_SECONDS(2));
	}
	LOG_INF("cellular registered");

	/* Network time arrives via NITZ shortly after registration. Required
	 * because the heartbeat payload must include a `ts` field per portal
	 * contract, and we have no RTC fallback. */
	char ts[32];
	int err;
	for (int i = 0; i < 30; i++) {
		err = gosteady_cellular_get_network_time(ts, sizeof(ts));
		if (err == 0) {
			LOG_INF("network time available: %s", ts);
			break;
		}
		k_sleep(K_SECONDS(1));
	}
	if (err != 0) {
		LOG_ERR("network time still unavailable after 30 s; giving up");
		return -ETIMEDOUT;
	}

	/* Signal stats also lag registration — the cellular reporter polls
	 * AT+CESQ on its own ~60 s schedule and doesn't fire the first poll
	 * for a few seconds after registration completes. Poll our cached
	 * accessor until it returns non-EAGAIN, same as the network-time
	 * wait above. */
	int16_t rsrp_dbm = 0;
	int8_t  snr_db   = 0;
	for (int i = 0; i < 60; i++) {
		err = gosteady_cellular_get_signal(&rsrp_dbm, &snr_db);
		if (err == 0) {
			LOG_INF("signal available: rsrp=%d dBm snr=%d dB",
				(int)rsrp_dbm, (int)snr_db);
			return 0;
		}
		k_sleep(K_SECONDS(1));
	}
	LOG_ERR("signal stats still unavailable after 60 s; giving up");
	return -ETIMEDOUT;
}

/*
 * Generic connect → publish → wait-for-PUBACK → disconnect path.
 * Holds s_aws_mutex for the duration so heartbeat + activity don't race.
 * Topic + payload are caller-built; this function just wraps the lifecycle.
 */
static int connect_publish_disconnect(const char *topic, size_t topic_len,
				      const char *payload, size_t payload_len)
{
	int rc;

	k_mutex_lock(&s_aws_mutex, K_FOREVER);

	rc = wait_for_cellular_ready();
	if (rc) {
		goto out_unlock;
	}

	LOG_INF("aws_iot_connect host=%s client_id=%s sec_tag=%d",
		CONFIG_AWS_IOT_BROKER_HOST_NAME,
		CONFIG_AWS_IOT_CLIENT_ID_STATIC,
		CONFIG_MQTT_HELPER_SEC_TAG);

	k_sem_reset(&s_connected);
	k_sem_reset(&s_disconnected);
	rc = aws_iot_connect(NULL); /* NULL → use Kconfig statics */
	if (rc) {
		LOG_ERR("aws_iot_connect: %d", rc);
		goto out_unlock;
	}

	rc = k_sem_take(&s_connected, CONNECT_WAIT);
	if (rc) {
		LOG_ERR("CONNECTED event not received within %d s",
			(int)k_ticks_to_ms_floor32(CONNECT_WAIT.ticks) / 1000);
		goto out_disconnect;
	}

	LOG_INF("publish %s -> %s (len=%u)", topic, payload, (unsigned)payload_len);

	struct aws_iot_data tx = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic = {
			.type = AWS_IOT_SHADOW_TOPIC_NONE,
			.str  = topic,
			.len  = topic_len,
		},
		.ptr = (char *)payload,
		.len = payload_len,
	};

	k_sem_reset(&s_puback);
	rc = aws_iot_send(&tx);
	if (rc) {
		LOG_ERR("aws_iot_send: %d", rc);
		goto out_disconnect;
	}
	LOG_INF("aws_iot_send queued; waiting for PUBACK (up to %d s)",
		(int)k_ticks_to_ms_floor32(PUBACK_WAIT.ticks) / 1000);

	rc = k_sem_take(&s_puback, PUBACK_WAIT);
	if (rc) {
		LOG_ERR("PUBACK not received within timeout — broker did not ack");
		rc = -ETIMEDOUT;
		goto out_disconnect;
	}
	LOG_INF("PUBACK received — broker confirmed");
	rc = 0;

	/* M12.1e.2: linger briefly after PUBACK so any pending application-
	 * topic messages (e.g., activate cmd queued by cloud while we were
	 * disconnected) have time to flow in before the disconnect.
	 *
	 * Without this, a cloud activate cmd published just-before-our-
	 * connect lands in our session's MQTT buffer but doesn't get
	 * processed before aws_iot_disconnect tears down the connection.
	 * 1.5 s is empirically generous: the broker's per-session queue
	 * delivery happens within ~hundreds of ms after SUBACK, so by
	 * PUBACK + 1.5 s any held messages have arrived. dispatch_app_message
	 * runs synchronously from the event handler, so by the time we
	 * disconnect the cmd has already been parsed + applied. */
	k_sleep(K_MSEC(1500));

out_disconnect:
	LOG_INF("aws_iot_disconnect");
	int derr = aws_iot_disconnect();
	if (derr && derr != -ENOTCONN) {
		LOG_WRN("aws_iot_disconnect: %d", derr);
	} else {
		(void)k_sem_take(&s_disconnected, DISCONNECT_WAIT);
	}

out_unlock:
	k_mutex_unlock(&s_aws_mutex);
	return rc;
}

/* ---- M12.1c.2 production heartbeat path ----
 *
 * Required-fields block: serial / ts / battery_pct / rsrp_dbm / snr_db.
 * Optional extras (per portal contract — all "accept-all" cloud-side):
 *   battery_mv      — only when fuel gauge has produced a reading
 *   firmware        — semver string from version.h
 *   uptime_s        — seconds since this boot
 *   last_cmd_id     — only when activate cmd has been received
 *   reset_reason    — formatted reset cause from M10.7.3 forensics
 *   fault_counters  — JSON object from M10.7.3 forensics
 *   watchdog_hits   — cumulative WDT-triggered resets
 *
 * Each optional is independently gated so a missing prereq (forensics
 * not enabled, fuel gauge not warm, etc.) just omits that field instead
 * of failing the publish.
 */

static int build_heartbeat_payload(char *buf, size_t buflen, size_t *out_len)
{
	char ts[32];
	int err = gosteady_cellular_get_network_time(ts, sizeof(ts));
	if (err) {
		LOG_ERR("get_network_time: %d", err);
		return err;
	}

	int16_t rsrp_dbm = 0;
	int8_t  snr_db   = 0;
	err = gosteady_cellular_get_signal(&rsrp_dbm, &snr_db);
	if (err) {
		LOG_ERR("get_signal: %d", err);
		return err;
	}

	/* Battery readings: M10.7.2 fuel gauge if compiled in, else the
	 * placeholder from M12.1c.1. The getter returns 0/0.5 placeholder
	 * + -EAGAIN if the fuel-gauge worker hasn't produced its first
	 * reading yet, so we publish whatever it gives us — cloud schema
	 * is happy with 0.5 since the placeholder is mid-range. */
	uint16_t battery_mv  = 0;
	float    battery_pct = 0.5f;
#if defined(CONFIG_NRF_FUEL_GAUGE)
	int berr = gosteady_battery_get(&battery_mv, &battery_pct);
	if (berr == -EAGAIN) {
		LOG_DBG("battery cache not warm yet — placeholder 0.5");
	} else if (berr) {
		LOG_WRN("battery_get: %d — placeholder 0.5", berr);
	}
#endif

	int n = snprintf(buf, buflen,
		"{\"serial\":\"%s\","
		 "\"ts\":\"%s\","
		 "\"battery_pct\":%.3f,"
		 "\"rsrp_dbm\":%d,"
		 "\"snr_db\":%d",
		client_id(), ts, (double)battery_pct, (int)rsrp_dbm, (int)snr_db);
	if (n < 0 || (size_t)n >= buflen) {
		LOG_ERR("heartbeat truncated (required block): n=%d buflen=%u",
			n, (unsigned)buflen);
		return -ENOMEM;
	}

#define APPEND_OR_FAIL(...)                                          \
	do {                                                          \
		int m = snprintf(buf + n, buflen - (size_t)n, __VA_ARGS__); \
		if (m < 0 || (size_t)(n + m) >= buflen) {            \
			return -ENOMEM;                              \
		}                                                     \
		n += m;                                               \
	} while (0)

	/* battery_mv: only when fuel gauge produced a real reading. */
	if (battery_mv > 0) {
		APPEND_OR_FAIL(",\"battery_mv\":%u", (unsigned)battery_mv);
	}

	/* firmware: semver from version.h (single source of truth). Always
	 * publish — useful for cloud-side correlation regardless of feature gates. */
	APPEND_OR_FAIL(",\"firmware\":\"%s\"", GS_FIRMWARE_VERSION_STR);

	/* uptime_s: time since boot. Bounded by uint32_t cast (~136 years). */
	APPEND_OR_FAIL(",\"uptime_s\":%u",
		       (unsigned)(k_uptime_get() / 1000));

	/* last_cmd_id: snapshot under mutex. Empty → field omitted (no cmd
	 * received yet — cloud's matching handler treats absence as "device
	 * has nothing new to ack"). */
	char cmd_snapshot[sizeof(s_last_cmd_id)];
	k_mutex_lock(&s_last_cmd_lock, K_FOREVER);
	strncpy(cmd_snapshot, s_last_cmd_id, sizeof(cmd_snapshot));
	cmd_snapshot[sizeof(cmd_snapshot) - 1] = '\0';
	k_mutex_unlock(&s_last_cmd_lock);
	if (cmd_snapshot[0] != '\0') {
		APPEND_OR_FAIL(",\"last_cmd_id\":\"%s\"", cmd_snapshot);
	}

	/* M10.7.3 forensics extras: reset_reason / fault_counters / watchdog_hits. */
#if defined(CONFIG_GOSTEADY_FORENSICS_ENABLE)
	char reset_reason[64];
	if (gosteady_forensics_get_reset_reason(reset_reason,
						 sizeof(reset_reason)) == 0) {
		APPEND_OR_FAIL(",\"reset_reason\":\"%s\"", reset_reason);
	}

	/* fault_counters as a JSON object literal — built by forensics so
	 * the schema lives next to the field definitions. */
	char fc_json[64];
	size_t fc_len = 0;
	if (gosteady_forensics_fault_counters_json(
		    fc_json, sizeof(fc_json), &fc_len) == 0) {
		APPEND_OR_FAIL(",\"fault_counters\":%s", fc_json);
	}

	APPEND_OR_FAIL(",\"watchdog_hits\":%u",
		       (unsigned)gosteady_forensics_get_watchdog_hits());
#endif

#undef APPEND_OR_FAIL

	if ((size_t)(n + 1) >= buflen) { return -ENOMEM; }
	buf[n++] = '}';
	buf[n] = '\0';

	*out_len = (size_t)n;
	return 0;
}

/* Linear backoff schedule: 1 / 5 / 15 min. After the third failure the
 * worker gives up on this hour's heartbeat. */
static k_timeout_t heartbeat_retry_delay(int attempt)
{
	switch (attempt) {
	case 0: return HEARTBEAT_RETRY_1ST;
	case 1: return HEARTBEAT_RETRY_2ND;
	default: return HEARTBEAT_RETRY_3RD;
	}
}

/*
 * Try the heartbeat publish up to HEARTBEAT_MAX_ATTEMPTS times with linear
 * backoff between attempts. Returns 0 on first success, last-error if all
 * attempts failed. The mutex inside connect_publish_disconnect serializes
 * vs the activity worker, so an in-flight activity publish doesn't race
 * the heartbeat retry.
 */
static int heartbeat_publish_with_retry(const char *topic, size_t topic_len)
{
	int last_err = 0;
	for (int attempt = 0; attempt < HEARTBEAT_MAX_ATTEMPTS; attempt++) {
		char payload[HEARTBEAT_PAYLOAD_MAX];
		size_t payload_len = 0;

		/* Rebuild the payload on every attempt so ts / battery / RSRP
		 * are fresh — important after a 15 min wait. */
		int err = build_heartbeat_payload(payload, sizeof(payload),
						  &payload_len);
		if (err) {
			LOG_ERR("heartbeat payload build failed: %d", err);
			last_err = err;
			break;
		}

		err = connect_publish_disconnect(topic, topic_len,
						 payload, payload_len);
		if (err == 0) {
			if (attempt > 0) {
				LOG_INF("heartbeat succeeded on retry %d", attempt);
			}
			return 0;
		}

		last_err = err;
		LOG_WRN("heartbeat attempt %d/%d failed (%d)",
			attempt + 1, HEARTBEAT_MAX_ATTEMPTS, err);

		if (attempt + 1 < HEARTBEAT_MAX_ATTEMPTS) {
			k_timeout_t delay = heartbeat_retry_delay(attempt);
			LOG_INF("heartbeat backoff: sleeping %d s before next attempt",
				(int)k_ticks_to_ms_floor32(delay.ticks) / 1000);
			k_sleep(delay);
		}
	}
	return last_err;
}

static void heartbeat_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	char topic[TOPIC_MAX];
	int t = snprintf(topic, sizeof(topic), HEARTBEAT_TOPIC_FMT, client_id());
	if (t < 0 || (size_t)t >= sizeof(topic)) {
		LOG_ERR("heartbeat topic truncated: t=%d", t);
		return;
	}

	/* Wait for cellular ready ONCE on first iteration. After that the
	 * connect_publish_disconnect path waits internally; no need to
	 * re-block at the top of every hourly tick. */
	if (wait_for_cellular_ready() != 0) {
		LOG_ERR("heartbeat thread giving up — cellular never became ready");
		return;
	}

	/* Production hourly cadence: publish, sleep one interval, repeat.
	 * The cloud's offline-detection threshold is 2 hr without a heartbeat,
	 * so a single missed publish (e.g., after retry exhaustion) doesn't
	 * trip an alert; two in a row will. */
	int iter = 0;
	while (1) {
		LOG_INF("heartbeat tick #%d (cadence=%d s)", iter++,
			(int)k_ticks_to_ms_floor32(HEARTBEAT_INTERVAL.ticks) / 1000);
		int rc = heartbeat_publish_with_retry(topic, (size_t)t);
		if (rc != 0) {
			LOG_ERR("heartbeat tick #%d FAILED after %d attempts (%d) — waiting for next interval",
				iter - 1, HEARTBEAT_MAX_ATTEMPTS, rc);
		}
		k_sleep(HEARTBEAT_INTERVAL);
	}
}

/* ---- M12.1d activity path ---- */

/*
 * Build activity JSON payload from struct.
 *   Required: serial, session_start, session_end, steps, distance_ft, active_min
 *   Optional: roughness_R, surface_class, firmware_version
 *
 * Optional fields with sentinel values are omitted from the JSON entirely
 * (per coord §C.7 accept-all + the desire to avoid a "0 = not measured"
 * sentinel race in the cloud-side schema).
 */
static int build_activity_payload(const struct gosteady_activity *a,
				  char *buf, size_t buflen, size_t *out_len)
{
	int n = snprintf(buf, buflen,
		"{\"serial\":\"%s\","
		 "\"session_start\":\"%s\","
		 "\"session_end\":\"%s\","
		 "\"steps\":%u,"
		 "\"distance_ft\":%.2f,"
		 "\"active_min\":%u",
		client_id(),
		a->session_start_utc_iso,
		a->session_end_utc_iso,
		(unsigned)a->steps,
		(double)a->distance_ft,
		(unsigned)a->active_min);
	if (n < 0 || (size_t)n >= buflen) {
		return -ENOMEM;
	}

	if (isfinite((double)a->roughness_R)) {
		int m = snprintf(buf + n, buflen - (size_t)n,
				 ",\"roughness_R\":%.4f", (double)a->roughness_R);
		if (m < 0 || (size_t)(n + m) >= buflen) { return -ENOMEM; }
		n += m;
	}

	if (a->surface_class == GOSTEADY_ACTIVITY_SURFACE_INDOOR) {
		int m = snprintf(buf + n, buflen - (size_t)n,
				 ",\"surface_class\":\"indoor\"");
		if (m < 0 || (size_t)(n + m) >= buflen) { return -ENOMEM; }
		n += m;
	} else if (a->surface_class == GOSTEADY_ACTIVITY_SURFACE_OUTDOOR) {
		int m = snprintf(buf + n, buflen - (size_t)n,
				 ",\"surface_class\":\"outdoor\"");
		if (m < 0 || (size_t)(n + m) >= buflen) { return -ENOMEM; }
		n += m;
	}

	if (a->firmware_version[0] != '\0') {
		int m = snprintf(buf + n, buflen - (size_t)n,
				 ",\"firmware_version\":\"%s\"",
				 a->firmware_version);
		if (m < 0 || (size_t)(n + m) >= buflen) { return -ENOMEM; }
		n += m;
	}

	if ((size_t)(n + 1) >= buflen) { return -ENOMEM; }
	buf[n++] = '}';
	buf[n] = '\0';
	*out_len = (size_t)n;
	return 0;
}

static void activity_worker_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	char topic[TOPIC_MAX];
	int t = snprintf(topic, sizeof(topic), ACTIVITY_TOPIC_FMT, client_id());
	if (t < 0 || (size_t)t >= sizeof(topic)) {
		LOG_ERR("activity topic truncated: t=%d", t);
		return;
	}

	struct gosteady_activity a;
	while (1) {
		int rc = k_msgq_get(&s_activity_msgq, &a, K_FOREVER);
		if (rc) {
			LOG_WRN("activity msgq_get: %d", rc);
			continue;
		}

		LOG_INF("activity worker: dequeued session_end=%s steps=%u distance_ft=%.2f",
			a.session_end_utc_iso, (unsigned)a.steps, (double)a.distance_ft);

		char payload[ACTIVITY_PAYLOAD_MAX];
		size_t payload_len = 0;
		rc = build_activity_payload(&a, payload, sizeof(payload), &payload_len);
		if (rc) {
			LOG_ERR("activity payload build failed: %d (skipping)", rc);
			continue;
		}

		(void)connect_publish_disconnect(topic, (size_t)t, payload, payload_len);
		LOG_INF("M12.1d activity uplink sequence complete");
	}
}

void gosteady_cloud_set_last_cmd_id(const char *cmd_id)
{
	if (cmd_id == NULL) { cmd_id = ""; }
	k_mutex_lock(&s_last_cmd_lock, K_FOREVER);
	strncpy(s_last_cmd_id, cmd_id, sizeof(s_last_cmd_id) - 1);
	s_last_cmd_id[sizeof(s_last_cmd_id) - 1] = '\0';
	k_mutex_unlock(&s_last_cmd_lock);
	LOG_INF("last_cmd_id updated to '%s'", s_last_cmd_id);
}

int gosteady_cloud_publish_activity(const struct gosteady_activity *a)
{
	if (a == NULL) { return -EINVAL; }

	int rc = k_msgq_put(&s_activity_msgq, a, K_NO_WAIT);
	if (rc == -ENOMSG) {
		LOG_WRN("activity msgq full (depth=%d) — dropping", ACTIVITY_MSGQ_DEPTH);
		return -EAGAIN;
	}
	if (rc) {
		LOG_ERR("activity msgq_put: %d", rc);
		return rc;
	}
	LOG_INF("activity enqueued: session_end=%s (msgq has %u/%u)",
		a->session_end_utc_iso,
		k_msgq_num_used_get(&s_activity_msgq), ACTIVITY_MSGQ_DEPTH);
	return 0;
}

/* ---- M12.1e.1 Shadow bench check ----
 *
 * One-shot Shadow GET + UPDATE round-trip against the bench Thing's
 * shadow document. Validates that NCS 3.2.4's aws_iot lib supports the
 * Shadow surface end-to-end on this firmware build, before M12.1e.2
 * commits to a Shadow-based pre-activation re-check (vs the MQTT-retained
 * `activate` cmd fallback per coord doc §C.4.4 / §C.5.1).
 *
 * Sequence (gated on CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK; default n):
 *   1. Wait for cellular ready.
 *   2. Take s_aws_mutex (serialize against heartbeat/activity workers).
 *   3. Connect to AWS IoT, wait for CONNECTED.
 *   4. Publish empty body to AWS_IOT_SHADOW_TOPIC_GET, wait up to 30 s
 *      for the corresponding GET_ACCEPTED event.
 *   5. Publish a small `{state:{reported:{shadow_bench_check_at:<ts>}}}`
 *      body to AWS_IOT_SHADOW_TOPIC_UPDATE, wait for UPDATE_ACCEPTED.
 *   6. Disconnect, log overall outcome, exit thread.
 *
 * Outcome of this run determines M12.1e.2's design path. Logged loudly
 * on uart0 so it shows up in logs/uart0_*.log and can be quoted into the
 * coord doc closure entry.
 */
#if defined(CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK)

#define SHADOW_OP_TIMEOUT  K_SECONDS(30)
#define SHADOW_PAYLOAD_MAX 256

static int shadow_send_locked(enum aws_iot_shadow_topic_type topic,
			       const char *body, size_t body_len)
{
	struct aws_iot_data tx = {
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
		.topic = { .type = topic },
		.ptr = (char *)body,
		.len = body_len,
	};
	return aws_iot_send(&tx);
}

static int run_shadow_bench_check(void)
{
	int rc;

	k_mutex_lock(&s_aws_mutex, K_FOREVER);

	if (wait_for_cellular_ready() != 0) {
		LOG_ERR("shadow bench: cellular never became ready");
		rc = -ETIMEDOUT;
		goto out_unlock;
	}

	LOG_INF("shadow bench: aws_iot_connect");
	k_sem_reset(&s_connected);
	k_sem_reset(&s_disconnected);
	k_sem_reset(&s_shadow_get_accepted);
	k_sem_reset(&s_shadow_update_accepted);

	rc = aws_iot_connect(NULL);
	if (rc) { LOG_ERR("shadow bench: connect: %d", rc); goto out_unlock; }
	rc = k_sem_take(&s_connected, CONNECT_WAIT);
	if (rc) { LOG_ERR("shadow bench: CONNECTED timeout"); goto out_disconnect; }

	/* Step 1 — Shadow GET. Empty body publishes to
	 * $aws/things/<thing>/shadow/get; broker responds on
	 * .../shadow/get/accepted (we're subscribed via Kconfig). */
	LOG_INF("shadow bench: sending GET");
	rc = shadow_send_locked(AWS_IOT_SHADOW_TOPIC_GET, "", 0);
	if (rc) { LOG_ERR("shadow bench: GET send: %d", rc); goto out_disconnect; }

	rc = k_sem_take(&s_shadow_get_accepted, SHADOW_OP_TIMEOUT);
	if (rc) {
		LOG_ERR("shadow bench: GET_ACCEPTED timeout — Shadow GET FAILED");
		rc = -ETIMEDOUT;
		goto out_disconnect;
	}
	LOG_INF("shadow bench: GET round-trip OK");

	/* Step 2 — Shadow UPDATE. Small reported delta with a timestamp so
	 * each run leaves a visible mark in the Shadow document. */
	char ts[32] = "<no-time>";
	(void)gosteady_cellular_get_network_time(ts, sizeof(ts));
	char body[SHADOW_PAYLOAD_MAX];
	int blen = snprintf(body, sizeof(body),
		"{\"state\":{\"reported\":{\"shadow_bench_check_at\":\"%s\"}}}", ts);
	if (blen < 0 || (size_t)blen >= sizeof(body)) {
		LOG_ERR("shadow bench: UPDATE body truncated");
		rc = -ENOMEM; goto out_disconnect;
	}
	LOG_INF("shadow bench: sending UPDATE — %s", body);
	rc = shadow_send_locked(AWS_IOT_SHADOW_TOPIC_UPDATE, body, (size_t)blen);
	if (rc) { LOG_ERR("shadow bench: UPDATE send: %d", rc); goto out_disconnect; }

	rc = k_sem_take(&s_shadow_update_accepted, SHADOW_OP_TIMEOUT);
	if (rc) {
		LOG_ERR("shadow bench: UPDATE_ACCEPTED timeout — Shadow UPDATE FAILED");
		rc = -ETIMEDOUT;
		goto out_disconnect;
	}
	LOG_INF("shadow bench: UPDATE round-trip OK");
	rc = 0;

out_disconnect:
	(void)aws_iot_disconnect();
	(void)k_sem_take(&s_disconnected, DISCONNECT_WAIT);
out_unlock:
	k_mutex_unlock(&s_aws_mutex);
	return rc;
}

static void shadow_bench_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	/* Let the heartbeat thread complete its first publish first — keeps
	 * boot logs sequential (heartbeat → bench check) for cleaner reading. */
	k_sleep(K_SECONDS(30));

	int rc = run_shadow_bench_check();
	if (rc == 0) {
		LOG_INF("==== M12.1e.1 SHADOW BENCH CHECK: PASS ====");
		LOG_INF("Shadow GET + UPDATE round-trip work in NCS 3.2.4 against this build.");
		LOG_INF("M12.1e.2 can use the Shadow path per coord doc §C.4.4.");
	} else {
		LOG_ERR("==== M12.1e.1 SHADOW BENCH CHECK: FAIL (%d) ====", rc);
		LOG_ERR("M12.1e.2 should fall back to MQTT-retained activate cmd.");
	}
}

#define SHADOW_BENCH_STACK_SIZE 4096
K_THREAD_STACK_DEFINE(s_shadow_bench_stack, SHADOW_BENCH_STACK_SIZE);
static struct k_thread s_shadow_bench_thread;

#endif /* CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK */

/* ---- Init ---- */

#define HEARTBEAT_STACK_SIZE 4096
#define ACTIVITY_STACK_SIZE  4096
#define WORKER_PRIORITY      7
K_THREAD_STACK_DEFINE(s_heartbeat_stack, HEARTBEAT_STACK_SIZE);
K_THREAD_STACK_DEFINE(s_activity_stack,  ACTIVITY_STACK_SIZE);
static struct k_thread s_heartbeat_thread;
static struct k_thread s_activity_thread;

int gosteady_cloud_init(void)
{
	if (atomic_set(&s_initialized, 1) == 1) {
		LOG_INF("cloud_init: already initialized");
		return 0;
	}

	int err = aws_iot_init(aws_iot_event_handler);
	if (err) {
		LOG_ERR("aws_iot_init: %d", err);
		atomic_set(&s_initialized, 0);
		return err;
	}

	/* M12.1e.2: register gs/{serial}/cmd as an application-specific
	 * subscription. The aws_iot lib subscribes to all entries on every
	 * CONNECT, so each cellular wake (heartbeat / activity) is also a
	 * cmd-reception window. Cloud-side `device-api` Lambda (Phase 2A)
	 * publishes activate cmds here; firmware echoes the cmd_id in the
	 * next heartbeat as ack per coord §C.2. */
	int t = snprintf(s_cmd_topic, sizeof(s_cmd_topic),
			 CMD_TOPIC_FMT, client_id());
	if (t < 0 || (size_t)t >= sizeof(s_cmd_topic)) {
		LOG_ERR("cmd topic truncated: t=%d", t);
		atomic_set(&s_initialized, 0);
		return -ENOMEM;
	}
	s_app_topics[0] = (struct mqtt_topic){
		.topic = {
			.utf8 = s_cmd_topic,
			.size = (size_t)t,
		},
		.qos = MQTT_QOS_1_AT_LEAST_ONCE,
	};
	err = aws_iot_application_topics_set(s_app_topics,
					     ARRAY_SIZE(s_app_topics));
	if (err) {
		LOG_ERR("aws_iot_application_topics_set(%s): %d",
			s_cmd_topic, err);
		/* Non-fatal: heartbeat / activity still publish; we just
		 * won't receive activate cmds. Logged loudly so an operator
		 * sees the misconfiguration. */
	} else {
		LOG_INF("registered app subscription: %s (QoS 1)", s_cmd_topic);
	}

	(void)k_thread_create(&s_heartbeat_thread, s_heartbeat_stack,
			      HEARTBEAT_STACK_SIZE,
			      heartbeat_thread_fn, NULL, NULL, NULL,
			      WORKER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&s_heartbeat_thread, "gs_cloud_hb");

	(void)k_thread_create(&s_activity_thread, s_activity_stack,
			      ACTIVITY_STACK_SIZE,
			      activity_worker_thread_fn, NULL, NULL, NULL,
			      WORKER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&s_activity_thread, "gs_cloud_act");

#if defined(CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK)
	(void)k_thread_create(&s_shadow_bench_thread, s_shadow_bench_stack,
			      SHADOW_BENCH_STACK_SIZE,
			      shadow_bench_thread_fn, NULL, NULL, NULL,
			      WORKER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&s_shadow_bench_thread, "gs_shadow_bench");
	LOG_INF("cloud_init OK; heartbeat + activity + shadow-bench workers spawned");
#else
	LOG_INF("cloud_init OK; heartbeat + activity workers spawned");
#endif
	return 0;
}
