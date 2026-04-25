/*
 * gs_step_detector.c — see gs_step_detector.h.
 *
 * Translation of algo/step_detector.py — same FSM, same edge cases, same
 * reported peak fields. The duration_s and energy_g2s arithmetic exactly
 * mirrors the Python: duration_s is computed BEFORE sample_idx is
 * incremented, energy is accumulated INCLUDING the exit-trigger sample.
 * Don't "fix" these without also updating the Python — the reference
 * vectors will diverge.
 */

#include "gs_step_detector.h"

#include <errno.h>
#include <stddef.h>
#include <string.h>

int gs_step_detector_init(struct gs_step_detector *d,
			  float fs_hz,
			  float enter_threshold_g,
			  float exit_threshold_g,
			  uint32_t min_gap_samples,
			  uint32_t max_active_samples)
{
	if (d == NULL) {
		return -EINVAL;
	}
	if (fs_hz <= 0.0f) {
		return -EINVAL;
	}
	if (exit_threshold_g >= enter_threshold_g) {
		return -EINVAL;
	}
	d->fs_hz = fs_hz;
	d->enter_threshold_g = enter_threshold_g;
	d->exit_threshold_g = exit_threshold_g;
	d->min_gap_samples = min_gap_samples;
	d->max_active_samples = max_active_samples;
	gs_step_detector_reset(d);
	return 0;
}

void gs_step_detector_reset(struct gs_step_detector *d)
{
	d->sample_idx = 0u;
	d->active = false;
	d->active_start_idx = 0u;
	d->candidate_max = 0.0f;
	d->candidate_max_idx = 0u;
	d->energy_sum_sq = 0.0f;
	/* "Negative infinity" so the first peak always clears the gap.
	 * Using a signed 64-bit so subtractions like (uint32 - last_emit)
	 * never underflow. */
	d->last_emit_idx = INT64_MIN / 2;
}

/* Internal: try to emit the currently-armed candidate. Returns true if
 * a peak was emitted and *out filled. May fail the min-gap check
 * (returns false; caller still resets active state in the same
 * transition). */
static bool emit_if_armed(struct gs_step_detector *d, struct gs_peak *out)
{
	if (out == NULL) {
		/* Caller doesn't want peaks; just consume + report success
		 * to keep FSM behavior identical from the FSM's perspective.
		 * (No production caller does this — it's a testing convenience.) */
		const int64_t gap = (int64_t)d->candidate_max_idx - d->last_emit_idx;
		if (gap < (int64_t)d->min_gap_samples) {
			return false;
		}
		d->last_emit_idx = (int64_t)d->candidate_max_idx;
		return true;
	}

	const int64_t gap = (int64_t)d->candidate_max_idx - d->last_emit_idx;
	if (gap < (int64_t)d->min_gap_samples) {
		return false;
	}

	const float duration_s = (float)(d->sample_idx - d->active_start_idx) / d->fs_hz;
	const float energy_g2s = d->energy_sum_sq / d->fs_hz;
	out->sample_idx = d->candidate_max_idx;
	out->time_s = (float)d->candidate_max_idx / d->fs_hz;
	out->amplitude_g = d->candidate_max;
	out->duration_s = duration_s;
	out->energy_g2s = energy_g2s;
	d->last_emit_idx = (int64_t)d->candidate_max_idx;
	return true;
}

bool gs_step_detector_step(struct gs_step_detector *d, float x, struct gs_peak *out)
{
	bool emitted = false;

	if (!d->active) {
		if (x >= d->enter_threshold_g) {
			d->active = true;
			d->active_start_idx = d->sample_idx;
			d->candidate_max = x;
			d->candidate_max_idx = d->sample_idx;
			d->energy_sum_sq = x * x;
		}
	} else {
		if (x > d->candidate_max) {
			d->candidate_max = x;
			d->candidate_max_idx = d->sample_idx;
		}
		d->energy_sum_sq += x * x;

		const uint32_t active_len_samples =
			d->sample_idx - d->active_start_idx + 1u;

		/* Normal exit: signal returned below exit threshold. */
		if (x < d->exit_threshold_g) {
			emitted = emit_if_armed(d, out);
			d->active = false;
			d->active_start_idx = 0u;
			d->candidate_max = 0.0f;
			d->candidate_max_idx = 0u;
			d->energy_sum_sq = 0.0f;
		} else if (active_len_samples >= d->max_active_samples) {
			/* Safety exit: clamp pathologically long active windows. */
			emitted = emit_if_armed(d, out);
			d->active = false;
			d->active_start_idx = 0u;
			d->candidate_max = 0.0f;
			d->candidate_max_idx = 0u;
			d->energy_sum_sq = 0.0f;
		}
	}

	d->sample_idx++;
	return emitted;
}
