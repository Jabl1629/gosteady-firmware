/*
 * gs_step_detector.h — Schmitt-trigger peak FSM with per-peak features.
 *
 * Mirrors algo/step_detector.py::StepDetector. Operates on the
 * gravity-removed, LP-smoothed accelerometer magnitude (|a|_HP_LP, in g).
 * Emits one gs_peak each time a candidate is confirmed and survives the
 * minimum-gap check against the previous emission.
 *
 * Per-peak features:
 *   amplitude_g   — max(x) during the active window
 *   duration_s    — time spent above exit_threshold
 *   energy_g2s    — Σ x²·dt over the active window
 *
 * The features map directly to the V1 stride regression
 * (algo/stride_model.py); each peak's amplitude_g is multiplied by the
 * surface-selected coefficient and summed over the session to produce
 * total distance.
 *
 * The FSM has two states (IDLE, ACTIVE). State variables are bounded
 * by the longest possible active window (max_active_samples) — there's
 * no unbounded buffering even if the input pathologically stays above
 * exit_threshold forever.
 */

#ifndef GOSTEADY_GS_STEP_DETECTOR_H_
#define GOSTEADY_GS_STEP_DETECTOR_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct gs_peak {
	uint32_t sample_idx;     /* sample index where the max was recorded */
	float    time_s;         /* sample_idx / fs_hz */
	float    amplitude_g;    /* max |a|_HP_LP during the active window */
	float    duration_s;     /* time spent above exit_threshold */
	float    energy_g2s;     /* Σ x²·dt over active window */
};

struct gs_step_detector {
	/* Config */
	float    fs_hz;
	float    enter_threshold_g;
	float    exit_threshold_g;
	uint32_t min_gap_samples;     /* derived from min_gap_s × fs_hz */
	uint32_t max_active_samples;  /* safety clamp */

	/* FSM state */
	uint32_t sample_idx;
	bool     active;
	uint32_t active_start_idx;
	float    candidate_max;
	uint32_t candidate_max_idx;
	float    energy_sum_sq;
	int64_t  last_emit_idx;       /* signed so first-peak gap math works */
};

/* Init. enter_threshold must be > exit_threshold (hysteresis). All
 * other config is consumed verbatim. Returns 0 on success, negative
 * errno on bad arguments. */
int gs_step_detector_init(struct gs_step_detector *d,
			  float fs_hz,
			  float enter_threshold_g,
			  float exit_threshold_g,
			  uint32_t min_gap_samples,
			  uint32_t max_active_samples);

/* Reset FSM state — preserves config. */
void gs_step_detector_reset(struct gs_step_detector *d);

/* Process one sample. If a peak is emitted on this sample, fills *out
 * and returns true; otherwise returns false. `out` may be NULL only if
 * the caller never wants to receive peaks (testing / draining). */
bool gs_step_detector_step(struct gs_step_detector *d, float x, struct gs_peak *out);

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_GS_STEP_DETECTOR_H_ */
