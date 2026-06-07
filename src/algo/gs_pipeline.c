/*
 * gs_pipeline.c — see gs_pipeline.h.
 *
 * Locked-V1 wiring per algo/run_auto_surface.py: HP→LP→detector +
 * HP→motion_gate, then surface classification by hard threshold on
 * inter-peak RMS, then per-peak stride sum. Deviations from the Python
 * reference will diverge from the test_vectors fixtures — keep this in
 * lock-step with algo/export_c_header.py and algo/export_reference_vectors.py.
 */

#include "gs_pipeline.h"

#include <errno.h>
#include <math.h>
#include <string.h>

#include "gosteady_algo_params.h"
#include "gs_roughness.h"

int gs_pipeline_init(struct gs_pipeline *p)
{
	if (p == NULL) {
		return -EINVAL;
	}

	int rc;
	rc = gs_biquad_init(&p->hp, gs_hp_coeffs, GS_HP_NUM_STAGES);
	if (rc != 0) {
		return rc;
	}
	rc = gs_biquad_init(&p->lp, gs_lp_coeffs, GS_LP_NUM_STAGES);
	if (rc != 0) {
		return rc;
	}
	rc = gs_motion_gate_init(&p->gate,
				 GS_GATE_WINDOW_SAMPLES,
				 GS_GATE_ENTER_G,
				 GS_GATE_EXIT_G,
				 GS_GATE_EXIT_HOLD_SAMPLES);
	if (rc != 0) {
		return rc;
	}
	rc = gs_step_detector_init(&p->det,
				   GS_FS_HZ,
				   GS_PEAK_ENTER_G,
				   GS_PEAK_EXIT_G,
				   GS_PEAK_MIN_GAP_SAMPLES,
				   GS_PEAK_MAX_ACTIVE_SAMPLES);
	if (rc != 0) {
		return rc;
	}

	p->n_peaks = 0u;
	p->n_samples_processed = 0u;
	p->n_samples_buffered = 0u;
	p->sum_amp_g = 0.0;
	return 0;
}

void gs_pipeline_session_start(struct gs_pipeline *p, float first_mag_g)
{
	/* Re-init the DSP blocks' state (preserve config) */
	gs_biquad_reset(&p->hp);
	gs_biquad_reset(&p->lp);
	gs_motion_gate_reset(&p->gate);
	gs_step_detector_reset(&p->det);

	/* Prime HP to steady-state at (first_mag_g - 1.0). Matches Python:
	 *   hp.init_steady(float(mag_g[0]) - 1.0)
	 * This kills the filter's startup transient — without it, the
	 * first ~30 samples have artificially large |a|_HP and would
	 * spuriously trip both the motion gate and the step detector. */
	gs_biquad_init_steady(&p->hp, first_mag_g - 1.0f);

	p->n_peaks = 0u;
	p->n_samples_processed = 0u;
	p->n_samples_buffered = 0u;
	p->sum_amp_g = 0.0;
}

void gs_pipeline_step(struct gs_pipeline *p, float mag_g)
{
	const float v_hp = gs_biquad_step(&p->hp, mag_g - 1.0f);
	const float v_lp = gs_biquad_step(&p->lp, v_hp);
	const bool in_motion = gs_motion_gate_step(&p->gate, v_hp);

	struct gs_peak peak;
	const bool emitted = gs_step_detector_step(&p->det, v_lp, &peak);
	if (emitted) {
		if (p->n_peaks < GS_PIPELINE_MAX_PEAKS) {
			p->peak_indices[p->n_peaks] = peak.sample_idx;
			p->n_peaks++;
			p->sum_amp_g += (double)peak.amplitude_g;
		}
		/* Else: peak count exceeded buffer cap. distance + sum_amp
		 * stop accumulating; very long sessions will under-report
		 * distance. The buffered cap is sized for ~5 peaks/s × 100s,
		 * which is well above any plausible cadence. */
	}

	/* Buffer for batch roughness. If we've overflowed, just stop
	 * appending — R will be computed over the prefix only and the
	 * outputs will flag buffer_overflowed=true. */
	if (p->n_samples_buffered < GS_PIPELINE_MAX_BUFFERED_SAMPLES) {
		p->mag_lp_buf[p->n_samples_buffered] = v_lp;
		p->motion_mask_buf[p->n_samples_buffered] = in_motion ? 1u : 0u;
		p->n_samples_buffered++;
	}
	p->n_samples_processed++;
}

/* Reported step count: walk the emitted peak indices once, counting a peak
 * as a step only if it is >= merge_gap_samples from the last KEPT step. This
 * de-satellites the loose impulse detector (which fires ~2 impulses/step) for
 * the *count only* — the distance regression still consumes every peak. Peak
 * indices are monotonically increasing, so the unsigned subtraction is safe. */
uint32_t gs_merge_step_count(const uint32_t *idx, uint32_t n,
			     uint32_t merge_gap_samples)
{
	if (n == 0u) {
		return 0u;
	}
	uint32_t kept = 1u;
	uint32_t last = idx[0];
	for (uint32_t i = 1u; i < n; i++) {
		if (idx[i] - last >= merge_gap_samples) {
			kept++;
			last = idx[i];
		}
	}
	return kept;
}

/* Gait denominator: sum the inter-peak gaps that are <= cap_samples (longer
 * gaps are between-bout pauses, excluded), then add one MEAN gated gap for the
 * leading partial stride the span omits. O(1) memory — no per-gap buffer.
 * Returns seconds; 0 if there are fewer than 2 peaks or every gap exceeds the
 * cap (isolated impulses → not walking). */
float gs_walking_time_from_peaks(const uint32_t *idx, uint32_t n,
				 uint32_t cap_samples, float fs_hz)
{
	if (n < 2u) {
		return 0.0f;
	}
	uint64_t sum_gated = 0u;
	uint32_t count = 0u;
	for (uint32_t i = 1u; i < n; i++) {
		const uint32_t gap = idx[i] - idx[i - 1u];
		if (gap <= cap_samples) {
			sum_gated += gap;
			count++;
		}
	}
	if (count == 0u) {
		return 0.0f;
	}
	/* (count+1) strides' worth of time at the mean gated cadence. */
	const double samples = (double)sum_gated + (double)sum_gated / (double)count;
	return (float)(samples / (double)fs_hz);
}

void gs_pipeline_finalize(const struct gs_pipeline *p,
			  struct gs_pipeline_outputs *out)
{
	memset(out, 0, sizeof(*out));

	/* Cumulative motion-gate counters are streaming — accurate
	 * regardless of buffer overflow. */
	out->step_count = p->n_peaks;
	out->motion_duration_s = gs_motion_gate_duration_s(&p->gate, GS_FS_HZ);
	out->total_duration_s = gs_motion_gate_total_s(&p->gate, GS_FS_HZ);
	out->motion_fraction = gs_motion_gate_fraction(&p->gate);
	out->buffer_overflowed = (p->n_samples_processed > p->n_samples_buffered);

	/* Roughness — batch over the buffered prefix. */
	const float R = gs_inter_peak_rms_g(p->mag_lp_buf,
					    p->motion_mask_buf,
					    p->n_samples_buffered,
					    p->peak_indices,
					    p->n_peaks,
					    GS_ROUGH_HALF_WINDOW_SAMPLES);
	out->roughness_R = R;

	/* Surface classification: hard threshold on R. NaN R (no walking
	 * motion to compute from) defaults to indoor — harmless because
	 * the stride sum will be ~0 when there are no peaks. */
	if (!isfinite(R) || R < GS_R_THRESHOLD) {
		out->surface_class = GS_SURFACE_INDOOR;
	} else {
		out->surface_class = GS_SURFACE_OUTDOOR;
	}

	const float c0 = gs_stride_intercept_ft[out->surface_class];
	const float c1 = gs_stride_amp_coeff[out->surface_class];

	/* Σ stride_ft = c0 * N + c1 * Σ amp_g. Clamp ≥ 0 — the regression
	 * can produce a small negative when n_peaks=0 + c0<0; physical
	 * distance is non-negative. */
	double dist = (double)c0 * (double)p->n_peaks + (double)c1 * p->sum_amp_g;
	if (dist < 0.0) {
		dist = 0.0;
	}
	out->distance_ft = (float)dist;

	/* Decoupled step count + gait speed (post-process the emitted peak
	 * train; distance above is untouched). peak_indices[] holds the first
	 * min(n_peaks, MAX_PEAKS) emission sample indices. */
	out->steps_merged = gs_merge_step_count(p->peak_indices, p->n_peaks,
						GS_STEP_MERGE_GAP_SAMPLES);
	out->walking_time_s = gs_walking_time_from_peaks(p->peak_indices, p->n_peaks,
							 GS_STRIDE_GAP_CAP_SAMPLES,
							 GS_FS_HZ);

	out->gait_speed_fts = 0.0f;
	out->gait_valid = false;
	/* Long-session guard: if distance saturated (peak cap) or roughness
	 * overflowed, the numerator is unreliable while walking-time keeps
	 * growing → gait would falsely collapse. Omit. */
	const bool not_saturated = !out->buffer_overflowed &&
				   (p->n_peaks < GS_PIPELINE_MAX_PEAKS);
	if (not_saturated &&
	    out->steps_merged >= GS_GAIT_MIN_STEPS &&
	    out->walking_time_s >= GS_GAIT_MIN_WALK_S &&
	    out->distance_ft > 0.0f) {
		out->gait_speed_fts = out->distance_ft / out->walking_time_s;
		out->gait_valid = true;
	}
}
