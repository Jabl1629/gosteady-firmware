/*
 * Session control command parser (Milestone 6a).
 *
 * One line of text in → one line of text out. The parser is transport-
 * agnostic: today it's driven by the uart1 `gs_dump` thread, and in M6b
 * the nRF5340 BLE bridge will route NUS writes through this same entry
 * point. Commands:
 *
 *   START {"subject_id":"S00","walker_type":"two_wheel", ... }
 *     → OK uuid=<uuid-string>
 *   STOP
 *     → OK uuid=<uuid-string> samples=<n> dropped=<n> duration_ms=<n>
 *   STATUS
 *     → STATUS active=1 uuid=<uuid-string> samples=<n>
 *     → STATUS active=0
 *
 * All errors reply as `ERR <reason>`. Callers are expected to newline-
 * terminate when relaying the response to the wire.
 *
 * The JSON payload keys mirror the annotation-spreadsheet columns one-
 * for-one (see GOSTEADY_CONTEXT.md "Data Collection Plan"), with the
 * one irritant that `operator` is a C++ reserved word, so the JSON key
 * stays `operator` but the struct member is `operator_id`.
 */

#ifndef GOSTEADY_CONTROL_H_
#define GOSTEADY_CONTROL_H_

#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Returns true if `line` begins with a control command verb (START,
 * STOP, or STATUS). Lets dump.c decide whether to forward an unknown
 * line here or reply with the existing "ERR unknown" itself. */
bool gosteady_control_recognises(const char *line);

/* Parse and execute one command line (not NUL-terminated? — must be).
 * The response is written into `out` as a NUL-terminated string, no
 * trailing newline (the caller adds the line terminator appropriate
 * for its transport). Return value is the byte length of the response
 * written (excluding the terminator), or -1 on out-buffer overflow. */
int gosteady_control_execute(const char *line, char *out, size_t out_sz);

#ifdef __cplusplus
}
#endif

#endif
