/*
 * M12.1c.1 — AWS IoT MQTT/TLS minimum-viable bring-up.
 *
 * One-shot publisher: waits for cellular registration + network time, opens a
 * single AWS IoT connection using the per-device cert at sec_tag 201
 * (provisioned via tools/flash_cert.py + nrfcredstore), publishes one heartbeat
 * to gs/{client_id}/heartbeat with required portal-contract fields, then
 * disconnects and the worker thread exits.
 *
 * Verbose logging on every aws_iot event so first-time TLS / MQTT issues
 * are diagnosable from uart0 logs alone (no debugger needed).
 *
 * Heartbeat payload (M12.1c.1 — required fields only, optional extras land
 * in M12.1c.2):
 *   {
 *     "serial":      <CONFIG_AWS_IOT_CLIENT_ID_STATIC>,
 *     "ts":          <ISO 8601 UTC from AT+CCLK?>,
 *     "battery_pct": 0.5,                              <-- placeholder until M10.7.2
 *     "rsrp_dbm":    <int from AT+CESQ>,
 *     "snr_db":      <int from AT%XSNRSQ?>
 *   }
 */

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <net/aws_iot.h>

#include <stdio.h>
#include <string.h>

#include "cellular.h"
#include "cloud.h"

LOG_MODULE_REGISTER(gs_cloud, LOG_LEVEL_INF);

#define HEARTBEAT_TOPIC_FMT   "gs/%s/heartbeat"
#define HEARTBEAT_TOPIC_MAX   64
#define HEARTBEAT_PAYLOAD_MAX 256

/* Wait windows. aws_iot_connect is documented as "synchronous and only returns
 * success when the client has connected" but in practice the CONNECTED event
 * is delivered to our handler asynchronously after CONNACK. Sem-based wait
 * mirrors what the upstream sample does. */
#define CONNECT_WAIT      K_SECONDS(60)
#define DISCONNECT_WAIT   K_SECONDS(10)
#define POST_PUB_DRAIN    K_MSEC(500)   /* let QoS-0 PUBLISH hit the wire */

static K_SEM_DEFINE(s_connected,    0, 1);
static K_SEM_DEFINE(s_disconnected, 0, 1);

static atomic_t s_initialized = ATOMIC_INIT(0);

static const char *client_id(void)
{
	return CONFIG_AWS_IOT_CLIENT_ID_STATIC;
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
		LOG_INF("evt: DATA_RECEIVED len=%u", (unsigned)evt->data.msg.len);
		break;
	case AWS_IOT_EVT_PUBACK:
		LOG_INF("evt: PUBACK msg_id=%u", (unsigned)evt->data.message_id);
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
			return 0;
		}
		k_sleep(K_SECONDS(1));
	}
	LOG_ERR("network time still unavailable after 30 s; giving up");
	return -ETIMEDOUT;
}

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

	int n = snprintf(buf, buflen,
		"{\"serial\":\"%s\","
		 "\"ts\":\"%s\","
		 "\"battery_pct\":0.5,"
		 "\"rsrp_dbm\":%d,"
		 "\"snr_db\":%d}",
		client_id(), ts, (int)rsrp_dbm, (int)snr_db);

	if (n < 0 || (size_t)n >= buflen) {
		LOG_ERR("payload format truncated/error: n=%d buflen=%u",
			n, (unsigned)buflen);
		return -ENOMEM;
	}
	*out_len = (size_t)n;
	return 0;
}

static int publish_one_heartbeat(void)
{
	char topic[HEARTBEAT_TOPIC_MAX];
	int t = snprintf(topic, sizeof(topic), HEARTBEAT_TOPIC_FMT, client_id());
	if (t < 0 || (size_t)t >= sizeof(topic)) {
		LOG_ERR("topic truncated/error: t=%d", t);
		return -ENOMEM;
	}

	char payload[HEARTBEAT_PAYLOAD_MAX];
	size_t payload_len = 0;
	int err = build_heartbeat_payload(payload, sizeof(payload), &payload_len);
	if (err) {
		return err;
	}

	LOG_INF("publish %s -> %s (len=%u)", topic, payload, (unsigned)payload_len);

	struct aws_iot_data tx = {
		.qos = MQTT_QOS_0_AT_MOST_ONCE,
		.topic = {
			.type = AWS_IOT_SHADOW_TOPIC_NONE,
			.str  = topic,
			.len  = (size_t)t,
		},
		.ptr = payload,
		.len = payload_len,
	};

	err = aws_iot_send(&tx);
	if (err) {
		LOG_ERR("aws_iot_send: %d", err);
		return err;
	}
	LOG_INF("aws_iot_send returned 0 (QoS 0 — no PUBACK to wait for)");
	return 0;
}

static void publisher_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1); ARG_UNUSED(p2); ARG_UNUSED(p3);

	int err = wait_for_cellular_ready();
	if (err) {
		LOG_ERR("cellular not ready; cloud worker exiting (err=%d)", err);
		return;
	}

	LOG_INF("aws_iot_connect host=%s client_id=%s sec_tag=%d",
		CONFIG_AWS_IOT_BROKER_HOST_NAME,
		CONFIG_AWS_IOT_CLIENT_ID_STATIC,
		CONFIG_MQTT_HELPER_SEC_TAG);

	err = aws_iot_connect(NULL); /* NULL → use Kconfig statics */
	if (err) {
		LOG_ERR("aws_iot_connect: %d", err);
		return;
	}

	err = k_sem_take(&s_connected, CONNECT_WAIT);
	if (err) {
		LOG_ERR("CONNECTED event not received within %d s",
			(int)k_ticks_to_ms_floor32(CONNECT_WAIT.ticks) / 1000);
		return;
	}

	(void)publish_one_heartbeat();
	/* Continue to disconnect even on publish failure — leaves the modem in
	 * a clean state for the next attach attempt. */

	k_sleep(POST_PUB_DRAIN);

	LOG_INF("aws_iot_disconnect");
	err = aws_iot_disconnect();
	if (err) {
		LOG_ERR("aws_iot_disconnect: %d", err);
		return;
	}
	err = k_sem_take(&s_disconnected, DISCONNECT_WAIT);
	if (err) {
		LOG_WRN("DISCONNECTED event not received within %d s",
			(int)k_ticks_to_ms_floor32(DISCONNECT_WAIT.ticks) / 1000);
	}

	LOG_INF("M12.1c.1 one-shot heartbeat sequence complete");
}

#define PUBLISHER_STACK_SIZE 4096
#define PUBLISHER_PRIORITY   7
K_THREAD_STACK_DEFINE(s_publisher_stack, PUBLISHER_STACK_SIZE);
static struct k_thread s_publisher_thread;

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

	(void)k_thread_create(&s_publisher_thread, s_publisher_stack,
			      PUBLISHER_STACK_SIZE,
			      publisher_thread_fn, NULL, NULL, NULL,
			      PUBLISHER_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&s_publisher_thread, "gs_cloud_pub");

	LOG_INF("cloud_init OK; publisher thread spawned");
	return 0;
}
