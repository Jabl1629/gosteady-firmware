/*
 * gs_filters.h — DF-II-T biquad cascade for the on-device V1 distance estimator.
 *
 * Implements the same recurrence as ARM CMSIS-DSP arm_biquad_cascade_df2T_f32:
 *
 *     y[n]  = b0 * x[n] + d1
 *     d1    = b1 * x[n] + a1 * y[n] + d2
 *     d2    = b2 * x[n] + a2 * y[n]
 *
 * Coefficients are stored per-stage as { b0, b1, b2, a1, a2 } where a1, a2
 * are the NEGATION of scipy's standard form (so the recurrence above uses
 * '+' instead of '-'). The header generator algo/export_c_header.py emits
 * the coefficients in this layout already; pass it directly to gs_biquad_init.
 *
 * Why our own biquad rather than CMSIS-DSP?
 *   - Host tests run with no CMSIS dependency (portable, fast iteration)
 *   - On a single 2nd-order biquad processing one sample at a time, the
 *     ARM SIMD doesn't help; the inner loop is identical-cost either way
 *   - Avoids one more ifdef path between host and target builds
 *
 * State is `(2 * n_stages)` floats per filter instance. Thread-unsafe by
 * design — one instance per signal stream. Reset between sessions with
 * gs_biquad_reset() or prime to a steady-state DC value with
 * gs_biquad_init_steady() (recommended at session start to skip the
 * filter's startup transient).
 */

#ifndef GOSTEADY_GS_FILTERS_H_
#define GOSTEADY_GS_FILTERS_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Maximum number of biquad stages we expect to support. The V1 algorithm
 * uses 1 stage each for HP and LP; bump this if a future filter needs
 * higher order. */
#define GS_BIQUAD_MAX_STAGES  4u

struct gs_biquad {
	const float *coeffs;          /* [n_stages * 5], { b0,b1,b2,a1,a2 } */
	uint32_t     n_stages;
	float        d1[GS_BIQUAD_MAX_STAGES];  /* state */
	float        d2[GS_BIQUAD_MAX_STAGES];
};

/* Bind a biquad instance to a coefficient array. coeffs must outlive the
 * filter (typically a const array in flash from gosteady_algo_params.h).
 * Resets state to zero. */
int gs_biquad_init(struct gs_biquad *bq, const float *coeffs, uint32_t n_stages);

/* Zero the filter state. Causes a startup transient until ~3/cutoff_hz
 * samples have flushed through. Prefer gs_biquad_init_steady() for
 * session-aligned runs where the input is known to be near-DC at sample 0. */
void gs_biquad_reset(struct gs_biquad *bq);

/* Initialize the state to the steady-state response of a DC input x0,
 * eliminating the startup transient. Same algorithm as algo/filters.py
 * BiquadSOS.init_steady — propagates the DC value stage-by-stage,
 * setting d1, d2 to the values they would settle on after seeing x0
 * for infinite time. */
void gs_biquad_init_steady(struct gs_biquad *bq, float x0);

/* Process one sample. */
float gs_biquad_step(struct gs_biquad *bq, float x);

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_GS_FILTERS_H_ */
