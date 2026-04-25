/*
 * gs_motion_gate.h — running-σ motion gate with Schmitt hysteresis.
 *
 * Mirrors algo/motion_gate.py::MotionGate. Maintains a fixed-size ring
 * buffer of the most recent samples + incremental sum and sum-of-squares
 * to compute population standard deviation in O(1) per sample. Schmitt
 * thresholds prevent boundary chatter; exit_hold prevents single-sample
 * dropouts from ending a motion run prematurely.
 *
 * Input: gravity-removed accelerometer magnitude (|a|_HP in g) — same
 * channel the step detector consumes the LP-smoothed version of.
 *
 * Output (per sample): boolean "is the walker moving right now?"
 * Output (cumulative): motion_sample_count, total_sample_count.
 *
 * Memory footprint: 4 * GS_MOTION_GATE_MAX_WINDOW + ~32 bytes per gate.
 * For the V1 default (50-sample window @ 100 Hz), that's ~232 bytes.
 */

#ifndef GOSTEADY_GS_MOTION_GATE_H_
#define GOSTEADY_GS_MOTION_GATE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Hard cap on ring buffer size — sized for the V1 500ms @ 100Hz default
 * with headroom. Bump if a future config needs a longer window. */
#define GS_MOTION_GATE_MAX_WINDOW  64u

struct gs_motion_gate {
	/* Config (set at init) */
	uint32_t window_samples;
	uint32_t exit_hold_samples;
	float    enter_threshold;
	float    exit_threshold;

	/* Ring buffer + incremental sums */
	float    ring[GS_MOTION_GATE_MAX_WINDOW];
	uint32_t ring_pos;
	bool     ring_filled;
	float    sum;
	float    sum_sq;

	/* FSM state */
	uint32_t below_count;
	bool     in_motion;

	/* Cumulative counters (for motion_duration_s, motion_fraction) */
	uint32_t motion_sample_count;
	uint32_t total_sample_count;
};

/* Initialize. enter_threshold must be >= exit_threshold (hysteresis).
 * window_samples must be 1..GS_MOTION_GATE_MAX_WINDOW.
 * exit_hold_samples is the number of consecutive below-exit samples
 * required to exit motion (debounce against single-sample dropouts).
 *
 * Returns 0 on success, negative errno on bad arguments. */
int gs_motion_gate_init(struct gs_motion_gate *g,
			uint32_t window_samples,
			float enter_threshold,
			float exit_threshold,
			uint32_t exit_hold_samples);

/* Reset only the FSM and counters — preserves config. Use between
 * sessions. */
void gs_motion_gate_reset(struct gs_motion_gate *g);

/* Process one sample. Returns the post-update in_motion state. */
bool gs_motion_gate_step(struct gs_motion_gate *g, float x);

/* Convenience: motion duration in seconds, given the configured fs. */
static inline float gs_motion_gate_duration_s(const struct gs_motion_gate *g,
					      float fs_hz)
{
	return (float)g->motion_sample_count / fs_hz;
}

/* Convenience: total processed duration in seconds. */
static inline float gs_motion_gate_total_s(const struct gs_motion_gate *g,
					   float fs_hz)
{
	return (float)g->total_sample_count / fs_hz;
}

/* Convenience: fraction of total time in-motion (0..1). */
static inline float gs_motion_gate_fraction(const struct gs_motion_gate *g)
{
	if (g->total_sample_count == 0u) {
		return 0.0f;
	}
	return (float)g->motion_sample_count / (float)g->total_sample_count;
}

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_GS_MOTION_GATE_H_ */
