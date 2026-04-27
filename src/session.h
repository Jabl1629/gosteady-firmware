/*
 * GoSteady session file format (Milestone 4).
 *
 * A session file is the on-device record of one run of the v1 capture
 * protocol. Format:
 *   - 256-byte packed header (this file's struct gosteady_session_header)
 *   - body: packed sequence of 28-byte struct gosteady_sample records
 *
 * Field names mirror the annotation spreadsheet one-to-one so the
 * host-side ingest script can map header bytes directly into spreadsheet
 * rows. Controlled-vocabulary fields are stored as small unsigned enums;
 * free-form identifiers are fixed-size ASCII strings, zero-padded, not
 * necessarily NUL-terminated.
 *
 * All multi-byte fields are little-endian (the nRF9151 is little-endian,
 * and the ingest script runs on x86/ARM laptops that are also LE — no
 * byte-swapping needed).
 *
 * Rev history:
 *   v1 (2026-04-19): initial M4 bring-up format.
 */

#ifndef GOSTEADY_SESSION_H_
#define GOSTEADY_SESSION_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GOSTEADY_SESSION_MAGIC    0x53533647u  /* 'G','6','S','S' LE */
#define GOSTEADY_SESSION_VERSION  1u
#define GOSTEADY_HEADER_BYTES     256u

/* Controlled vocabularies. Stable numeric values — do NOT renumber these
 * once a session file has been written in the wild, or existing files
 * will decode wrong. To add a value, append at the end. */
enum gosteady_walker_type {
	GS_WALKER_STANDARD  = 0,
	GS_WALKER_TWO_WHEEL = 1,
};

enum gosteady_cap_type {
	GS_CAP_TACKY = 0,
	GS_CAP_GLIDE = 1,
};

enum gosteady_surface {
	GS_SURFACE_POLISHED_CONCRETE = 0,
	GS_SURFACE_LOW_PILE_CARPET   = 1,
	GS_SURFACE_HIGH_PILE_CARPET  = 2,
	GS_SURFACE_HARDWOOD          = 3,
	GS_SURFACE_TILE              = 4,
	GS_SURFACE_LINOLEUM          = 5,
	GS_SURFACE_VINYL             = 6,
	GS_SURFACE_OUTDOOR_CONCRETE  = 7,
	GS_SURFACE_OUTDOOR_ASPHALT   = 8,
};

enum gosteady_speed {
	GS_SPEED_SLOW   = 0,
	GS_SPEED_NORMAL = 1,
	GS_SPEED_FAST   = 2,
};

enum gosteady_direction {
	GS_DIR_STRAIGHT    = 0,
	GS_DIR_TURN_LEFT   = 1,
	GS_DIR_TURN_RIGHT  = 2,
	GS_DIR_S_CURVE     = 3,
	GS_DIR_PIVOT       = 4,
};

enum gosteady_run_type {
	GS_RUN_NORMAL                  = 0,
	GS_RUN_STUMBLE                 = 1,
	GS_RUN_PICKUP                  = 2,
	GS_RUN_SETDOWN                 = 3,
	GS_RUN_STATIONARY_BASELINE     = 4,
	GS_RUN_CAR_TRANSPORT           = 5,
	GS_RUN_CHAIR_TRANSFER          = 6,
	GS_RUN_TURN_TEST               = 7,
	GS_RUN_OBSTACLE                = 8,
	GS_RUN_WALKER_TYPE_TRANSITION  = 9,
	GS_RUN_SURFACE_TRANSITION      = 10,
};

enum gosteady_mount_config {
	GS_MOUNT_FRONT_LEFT_LEG  = 0,
	GS_MOUNT_FRONT_RIGHT_LEG = 1,
	GS_MOUNT_REAR_LEFT_LEG   = 2,
	GS_MOUNT_REAR_RIGHT_LEG  = 3,
	GS_MOUNT_FRONT_CROSSBAR  = 4,
};

/* Pre-walk metadata that the host will eventually push over BLE
 * (Milestone 6). For M4 we stamp a hard-coded bench default.
 * Packed so the on-disk layout matches the Python ingest script's
 * byte-packed struct format exactly. */
struct __attribute__((packed)) gosteady_prewalk {
	char     subject_id[8];        /* e.g. "S00" */
	uint8_t  walker_type;          /* enum gosteady_walker_type */
	uint8_t  cap_type;             /* enum gosteady_cap_type   */
	char     walker_model[16];     /* free form */
	uint8_t  mount_config;         /* enum gosteady_mount_config */
	char     course_id[32];        /* free form */
	uint16_t intended_distance_ft;
	uint8_t  surface;              /* enum gosteady_surface */
	uint8_t  intended_speed;       /* enum gosteady_speed */
	uint8_t  direction;            /* enum gosteady_direction */
	uint8_t  run_type;             /* enum gosteady_run_type */
	char     operator_id[16];      /* free form */
};

/* 256-byte session file header. Byte offsets inside this struct are part
 * of the on-disk format — do not reorder or resize existing fields
 * without bumping GOSTEADY_SESSION_VERSION. */
struct __attribute__((packed)) gosteady_session_header {
	/* Framing */
	uint32_t magic;                   /* GOSTEADY_SESSION_MAGIC */
	uint16_t version;                 /* GOSTEADY_SESSION_VERSION */
	uint16_t header_size;             /* GOSTEADY_HEADER_BYTES */

	/* FIRMWARE layer (13 fields) — device-owned, stamped at open/close */
	uint8_t  session_uuid[16];        /* UUIDv4 binary */
	char     device_serial[16];       /* e.g. "TH91X-0001" */
	char     firmware_version[16];    /* semver, e.g. "0.4.0-dev" */
	char     sensor_model[16];        /* "BMI270" */
	uint16_t sample_rate_hz;          /* 100 */
	uint8_t  accel_range_g;           /* 4 */
	uint8_t  _pad0;
	uint16_t gyro_range_dps;          /* 500 */
	uint16_t _pad1;
	int64_t  session_start_utc_ms;    /* 0 until RTC sync (Milestone 12) */
	int64_t  session_end_utc_ms;      /* same */
	uint32_t sample_count;            /* written at close */
	uint16_t battery_mv_start;        /* 0 until PMIC readout lands */
	uint16_t battery_mv_end;          /* same */
	uint16_t flash_errors;            /* failed fs_write calls during session */
	uint16_t _pad2;

	/* PRE-WALK layer (12 fields) — operator-owned, pushed via BLE in M6 */
	struct gosteady_prewalk prewalk;

	/* Room to grow without a version bump for padding-compatible fields.
	 * Compile-time checked below. Math: framing(8) + firmware(100) +
	 * prewalk(81 packed) + reserved = 256. */
	uint8_t  _reserved[67];
};

/* 28-byte per-sample record. BMI270 only for M4. accel in m/s^2, gyro
 * in rad/s (Zephyr sensor_value -> float conversion on-device). */
struct __attribute__((packed)) gosteady_sample {
	uint32_t t_ms;        /* milliseconds since session start */
	float    ax, ay, az;  /* accel, m/s^2 */
	float    gx, gy, gz;  /* gyro,  rad/s */
};

/* Build-time sanity checks. These are the contract with the ingest
 * script — if they ever fail, the script will also break. */
_Static_assert(sizeof(struct gosteady_session_header) == GOSTEADY_HEADER_BYTES,
	"gosteady_session_header must be exactly 256 bytes");
_Static_assert(sizeof(struct gosteady_sample) == 28,
	"gosteady_sample must be exactly 28 bytes");

/* ---- Public session-lifecycle API ---- */

/* Open a new session file with the given pre-walk metadata. Generates a
 * fresh UUIDv4, stamps the firmware-layer fields, and leaves the file
 * open with the header reserved (will be rewritten at close with the
 * final sample_count / session_end / battery_mv_end / flash_errors).
 *
 * Only one session can be active at a time.
 *
 * Returns 0 on success, negative errno on failure. */
int gosteady_session_start(const struct gosteady_prewalk *prewalk);

/* Append one sample to the currently-open session. Buffered internally;
 * the caller does not need to flush. Safe to call at 100 Hz from a
 * dedicated sensor thread.
 *
 * Returns 0 on success, -ENODEV if no session is open, negative errno
 * on flush failure. */
int gosteady_session_append(const struct gosteady_sample *s);

/* Stop the current session: flush any buffered samples, rewrite the
 * header with the final stats, close the file. Also base64-encodes the
 * header to stdout/log (UART) so M4 testing can round-trip without USB
 * mass-storage (which lands in Milestone 5).
 *
 * Writes the final sample count to *out_sample_count if non-NULL.
 *
 * Returns 0 on success, negative errno on failure. */
int gosteady_session_stop(uint32_t *out_sample_count);

/* True if a session is currently open and accepting samples. */
bool gosteady_session_is_active(void);

/* Phase 3 auto-stop input: count of consecutive non-motion samples
 * the writer thread has seen since the last motion-gate-active sample.
 * Updated per-sample in the writer thread; read from the main thread's
 * heartbeat tick. Resets to 0 on session_start and on every
 * motion-gate-active sample. Sample rate is 100 Hz, so seconds = N/100.
 *
 * The motion gate already has its own 500 ms running window + Schmitt
 * hysteresis with a 2 s exit-hold, so this counter only climbs after
 * the gate confirms sustained stillness — brief mid-walk pauses don't
 * contribute. */
uint32_t gosteady_session_stationary_samples(void);

/* Format the currently-active session's UUIDv4 as canonical
 * 36-char hyphenated string plus NUL (37 bytes). `out_sz` must
 * be >= 37. Returns 0 on success, -ENODEV if no session is
 * active, -EINVAL if `out` is too small. */
int gosteady_session_get_uuid_str(char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_SESSION_H_ */
