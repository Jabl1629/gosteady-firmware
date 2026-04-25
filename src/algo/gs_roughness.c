/*
 * gs_roughness.c — see gs_roughness.h.
 */

#include "gs_roughness.h"

#include <math.h>
#include <string.h>

float gs_inter_peak_rms_g(const float    *mag_lp,
			  const uint8_t  *motion_mask,
			  uint32_t        n_samples,
			  const uint32_t *peak_indices,
			  uint32_t        n_peaks,
			  uint32_t        half_window)
{
	if (mag_lp == NULL || motion_mask == NULL || n_samples == 0u) {
		return NAN;
	}

	/* Walk the signal once. For each sample, decide:
	 *   in_motion AND not in any peak's exclusion zone -> contribute.
	 *
	 * "In any peak's exclusion zone" is checked by linear scan over
	 * peaks; for our typical n_peaks (<= 60 per session) and n_samples
	 * (<= 30000), this is <2M ops — sub-millisecond on the nRF9151.
	 *
	 * We exit the inner loop early as soon as we find ONE peak that
	 * claims the sample, keeping the average cost much lower than the
	 * worst case. */
	double sum_sq = 0.0;
	uint32_t count = 0u;

	for (uint32_t i = 0u; i < n_samples; i++) {
		if (motion_mask[i] == 0u) {
			continue;
		}
		bool excluded = false;
		for (uint32_t k = 0u; k < n_peaks; k++) {
			const uint32_t p = peak_indices[k];
			/* |i - p| <= half_window, computed without signed arith */
			const uint32_t lo = (p > half_window) ? (p - half_window) : 0u;
			const uint32_t hi = p + half_window;  /* may exceed n_samples; ok */
			if (i >= lo && i <= hi) {
				excluded = true;
				break;
			}
		}
		if (excluded) {
			continue;
		}
		const double v = (double)mag_lp[i];
		sum_sq += v * v;
		count++;
	}

	if (count < 10u) {
		return NAN;
	}
	return (float)sqrt(sum_sq / (double)count);
}
