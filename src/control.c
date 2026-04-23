/*
 * Session control parser — see control.h for the wire protocol.
 *
 * Implementation notes:
 *   - Zephyr's json_obj_parse modifies the input buffer in place (it
 *     writes NUL terminators into it), so we always copy the incoming
 *     line into a local mutable buffer before parsing.
 *   - Controlled-vocabulary fields (walker_type, cap_type, ...) are
 *     accepted as their canonical string names from the spreadsheet's
 *     Vocabularies sheet, then translated to the small-integer enum
 *     values that land in the session-file header.
 *   - Free-form fields (subject_id, walker_model, course_id, operator)
 *     are copied verbatim into the fixed-size ASCII arrays, truncated
 *     on overflow rather than rejected (matches the spreadsheet
 *     behaviour — long free-form strings get truncated in ingest too).
 */

#include "control.h"
#include "session.h"

#include <zephyr/kernel.h>
#include <zephyr/data/json.h>
#include <zephyr/logging/log.h>
#include <stdio.h>
#include <string.h>

LOG_MODULE_REGISTER(gs_control, LOG_LEVEL_INF);

#define MAX_JSON_BYTES 512

/* --- JSON schema mirroring struct gosteady_prewalk ---------------------- */

struct prewalk_json {
	const char *subject_id;
	const char *walker_type;
	const char *cap_type;
	const char *walker_model;
	const char *mount_config;
	const char *course_id;
	int intended_distance_ft;
	const char *surface;
	const char *intended_speed;
	const char *direction;
	const char *run_type;
	const char *operator_id;  /* JSON key: "operator" (reserved word in C++) */
};

static const struct json_obj_descr prewalk_descr[] = {
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, subject_id,           JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, walker_type,          JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, cap_type,             JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, walker_model,         JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, mount_config,         JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, course_id,            JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, intended_distance_ft, JSON_TOK_NUMBER),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, surface,              JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, intended_speed,       JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, direction,            JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM(struct prewalk_json, run_type,             JSON_TOK_STRING),
	JSON_OBJ_DESCR_PRIM_NAMED(struct prewalk_json, "operator", operator_id, JSON_TOK_STRING),
};

/* --- Controlled-vocabulary tables (match session.h enum values) --------- */

static const char *const walker_types[]  = {"standard", "two_wheel"};
static const char *const cap_types[]     = {"tacky", "glide"};
static const char *const surfaces[]      = {
	"polished_concrete", "low_pile_carpet", "high_pile_carpet",
	"hardwood", "tile", "linoleum", "vinyl",
	"outdoor_concrete", "outdoor_asphalt",
};
static const char *const speeds[]        = {"slow", "normal", "fast"};
static const char *const directions[]    = {"straight", "turn_left", "turn_right", "s_curve", "pivot"};
static const char *const run_types[]     = {
	"normal", "stumble", "pickup", "setdown", "stationary_baseline",
	"car_transport", "chair_transfer", "turn_test", "obstacle",
	"walker_type_transition", "surface_transition",
};
static const char *const mount_configs[] = {
	"front_left_leg", "front_right_leg", "rear_left_leg",
	"rear_right_leg", "front_crossbar",
};

static int lookup_enum(const char *s, const char *const *tbl, size_t n)
{
	if (s == NULL) { return -1; }
	for (size_t i = 0; i < n; i++) {
		if (strcmp(s, tbl[i]) == 0) { return (int)i; }
	}
	return -1;
}

static void copy_ascii(char *dst, size_t dst_sz, const char *src)
{
	memset(dst, 0, dst_sz);
	if (src != NULL) {
		strncpy(dst, src, dst_sz);
	}
}

/* --- Command handlers --------------------------------------------------- */

/* Build a `struct gosteady_prewalk` from a parsed JSON object. Writes
 * an error reason into err_out on failure. Returns 0 on success. */
static int build_prewalk(const struct prewalk_json *j,
			 struct gosteady_prewalk *p,
			 char *err_out, size_t err_sz)
{
	int v;

#define VALIDATE_ENUM(field, tbl, out_member, label)                           \
	do {                                                                   \
		if (j->field == NULL) {                                        \
			snprintk(err_out, err_sz, "missing field: %s", label); \
			return -EINVAL;                                        \
		}                                                              \
		v = lookup_enum(j->field, tbl, ARRAY_SIZE(tbl));               \
		if (v < 0) {                                                   \
			snprintk(err_out, err_sz, "invalid %s: %s", label,     \
				 j->field);                                    \
			return -EINVAL;                                        \
		}                                                              \
		p->out_member = (uint8_t)v;                                    \
	} while (0)

	VALIDATE_ENUM(walker_type,    walker_types,   walker_type,    "walker_type");
	VALIDATE_ENUM(cap_type,       cap_types,      cap_type,       "cap_type");
	VALIDATE_ENUM(mount_config,   mount_configs,  mount_config,   "mount_config");
	VALIDATE_ENUM(surface,        surfaces,       surface,        "surface");
	VALIDATE_ENUM(intended_speed, speeds,         intended_speed, "intended_speed");
	VALIDATE_ENUM(direction,      directions,     direction,      "direction");
	VALIDATE_ENUM(run_type,       run_types,      run_type,       "run_type");

#undef VALIDATE_ENUM

	/* Free-form strings get truncated-copied; no error if too long. */
	copy_ascii(p->subject_id,   sizeof(p->subject_id),   j->subject_id);
	copy_ascii(p->walker_model, sizeof(p->walker_model), j->walker_model);
	copy_ascii(p->course_id,    sizeof(p->course_id),    j->course_id);
	copy_ascii(p->operator_id,  sizeof(p->operator_id),  j->operator_id);

	if (j->intended_distance_ft < 0 || j->intended_distance_ft > UINT16_MAX) {
		snprintk(err_out, err_sz, "intended_distance_ft out of range");
		return -EINVAL;
	}
	p->intended_distance_ft = (uint16_t)j->intended_distance_ft;

	return 0;
}

static int cmd_start(const char *json_start, size_t json_len,
		     char *out, size_t out_sz)
{
	if (gosteady_session_is_active()) {
		return snprintk(out, out_sz, "ERR already active");
	}
	if (json_len == 0 || json_len >= MAX_JSON_BYTES) {
		return snprintk(out, out_sz, "ERR bad json length %zu", json_len);
	}

	/* json_obj_parse mutates — copy into a private buffer. */
	char mutable_buf[MAX_JSON_BYTES];
	memcpy(mutable_buf, json_start, json_len);
	mutable_buf[json_len] = '\0';

	struct prewalk_json parsed;
	memset(&parsed, 0, sizeof(parsed));

	int ret = json_obj_parse(mutable_buf, json_len,
				 prewalk_descr, ARRAY_SIZE(prewalk_descr),
				 &parsed);
	if (ret < 0) {
		return snprintk(out, out_sz, "ERR bad json (%d)", ret);
	}
	/* json_obj_parse returns a bitmask of fields found — every slot
	 * is required, so all 12 bits must be set. */
	uint32_t required = (1u << ARRAY_SIZE(prewalk_descr)) - 1u;
	if (((uint32_t)ret & required) != required) {
		return snprintk(out, out_sz, "ERR incomplete json mask=0x%x", ret);
	}

	struct gosteady_prewalk prewalk;
	memset(&prewalk, 0, sizeof(prewalk));
	char err_buf[64];
	ret = build_prewalk(&parsed, &prewalk, err_buf, sizeof(err_buf));
	if (ret < 0) {
		return snprintk(out, out_sz, "ERR %s", err_buf);
	}

	ret = gosteady_session_start(&prewalk);
	if (ret < 0) {
		return snprintk(out, out_sz, "ERR start failed (%d)", ret);
	}
	/* Echo the UUID back so transport-side clients (capture.html in
	 * particular) can key their POST-WALK notes against the same
	 * primary key the session file header carries. Falls back to a
	 * bare "OK started" if the UUID somehow can't be fetched; the
	 * protocol stays forward-compatible either way. */
	char uuid_str[37];
	int uret = gosteady_session_get_uuid_str(uuid_str, sizeof(uuid_str));
	if (uret < 0) {
		return snprintk(out, out_sz, "OK started");
	}
	return snprintk(out, out_sz, "OK started %s", uuid_str);
}

static int cmd_stop(char *out, size_t out_sz)
{
	if (!gosteady_session_is_active()) {
		return snprintk(out, out_sz, "ERR not active");
	}
	uint32_t samples = 0;
	int ret = gosteady_session_stop(&samples);
	if (ret < 0) {
		return snprintk(out, out_sz, "ERR stop failed (%d)", ret);
	}
	return snprintk(out, out_sz, "OK samples=%u", (unsigned)samples);
}

static int cmd_status(char *out, size_t out_sz)
{
	if (!gosteady_session_is_active()) {
		return snprintk(out, out_sz, "STATUS active=0");
	}
	return snprintk(out, out_sz, "STATUS active=1");
}

/* --- Public entry points ------------------------------------------------ */

bool gosteady_control_recognises(const char *line)
{
	return strncmp(line, "START", 5) == 0 ||
	       strncmp(line, "STOP",  4) == 0 ||
	       strncmp(line, "STATUS", 6) == 0;
}

int gosteady_control_execute(const char *line, char *out, size_t out_sz)
{
	if (out == NULL || out_sz == 0) { return -1; }

	int n;
	if (strncmp(line, "START", 5) == 0) {
		/* Skip past "START" and any leading whitespace before the JSON. */
		const char *p = line + 5;
		while (*p == ' ' || *p == '\t') { p++; }
		n = cmd_start(p, strlen(p), out, out_sz);
	} else if (strcmp(line, "STOP") == 0) {
		n = cmd_stop(out, out_sz);
	} else if (strcmp(line, "STATUS") == 0) {
		n = cmd_status(out, out_sz);
	} else {
		n = snprintk(out, out_sz, "ERR unknown");
	}
	if (n < 0 || (size_t)n >= out_sz) { return -1; }
	return n;
}
