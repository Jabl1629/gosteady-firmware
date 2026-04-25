/*
 * gs_roughness.h — motion-gated inter-peak RMS, batch form.
 *
 * Mirrors algo/roughness.py::inter_peak_rms_g exactly. Computes the RMS
 * of |a|_HP_LP over samples that are (a) flagged as in-motion AND (b)
 * NOT inside the ±half_window exclusion zone of any detected peak.
 *
 * Why batch and not streaming?
 *   The exclusion zone of a detected peak is centered on its
 *   candidate_max_idx, which can be up to max_active_samples (500 for
 *   the V1 config) BEFORE the emission sample. A streaming form would
 *   need to retroactively un-contribute samples already added to the
 *   running RMS — workable but ~3x more code than the batch version.
 *   The pipeline orchestrator buffers mag_lp + motion_mask in RAM for
 *   the session (~30 KB for a typical clinic walk) and calls this
 *   function at session close.
 *
 * Returns R in g (≥ 0) on success. Returns NaN if fewer than 10
 * samples survive the (in_motion & ~peak_excl) filter — matches the
 * Python guard against under-conditioned RMS computations.
 */

#ifndef GOSTEADY_GS_ROUGHNESS_H_
#define GOSTEADY_GS_ROUGHNESS_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Compute motion-gated inter-peak RMS.
 *
 * mag_lp           : per-sample LP-smoothed |a|_HP, length n_samples
 * motion_mask      : per-sample in-motion flag (0/1), length n_samples
 * n_samples        : length of mag_lp and motion_mask
 * peak_indices     : sample indices of detected peaks (candidate_max_idx
 *                    from gs_step_detector), length n_peaks
 * n_peaks          : length of peak_indices
 * half_window      : exclusion half-width in samples (±this around
 *                    each peak)
 *
 * Returns R or NaN if < 10 samples survive.
 */
float gs_inter_peak_rms_g(const float    *mag_lp,
			  const uint8_t  *motion_mask,
			  uint32_t        n_samples,
			  const uint32_t *peak_indices,
			  uint32_t        n_peaks,
			  uint32_t        half_window);

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_GS_ROUGHNESS_H_ */
