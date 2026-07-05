/*
 * gs_rollator_distance.h — rollator wheeled-motion distance estimator.
 *
 * Continuous "flat-norm" vibration odometer (NO surface classifier, NO FFT):
 *
 *     distance_ft = m * SUM_active( hp_window_rms * dt ) / flatness
 *
 * flatness = spectral flatness of the wheel vibration from a 16-band biquad filter
 * bank (geomean(band energy)/mean(band energy)), which de-confounds surface
 * roughness from speed. Validated on-bench 2026-07-05 at ~31% MAPE across 3
 * surfaces (polished/sidewalk/asphalt), 0.55-3.08 ft/s, out-of-sample (causal
 * biquads = this port). Specs: docs/specs/2026-07-05-rollator-distance-firmware-port.md,
 * docs/specs/2026-07-04-rollator-algorithm-scope.md (§3e/§3f).
 *
 * ROLLATOR-ONLY: this is a separate path from the walker's step-detector/stride
 * distance (frame-mount rollator has no step impulses; steps were dropped). Coeffs
 * in gs_rollator_params.h (auto-gen by algo/rollator_distance_ref.py, the golden
 * reference the host test replays against). Compile-gated by
 * CONFIG_GOSTEADY_PRODUCT_ROLLATOR at the call sites — walker builds are untouched.
 *
 * Fed the same 100 Hz gravity-normalised accel magnitude as gs_pipeline. Fully
 * causal; buffers per-500ms-window features (a few KB) and resolves the motion
 * gate + flat-norm at finalize (no session-length sample buffer).
 */

#ifndef GOSTEADY_GS_ROLLATOR_DISTANCE_H_
#define GOSTEADY_GS_ROLLATOR_DISTANCE_H_

#include <stdbool.h>
#include <stdint.h>

#include "gs_filters.h"          /* gs_biquad */

#ifdef __cplusplus
extern "C" {
#endif

#define GS_ROLL_NUM_BANDS         16u
#define GS_ROLL_GATE_WINDOW_SMPL  50u    /* 500 ms @ 100 Hz */
#define GS_ROLL_FLAT_WINDOW_GW    4u     /* 2 s flatness window = 4 gate windows */
#define GS_ROLL_MAX_GATE_WINDOWS  600u   /* 300 s horizon; tail flags overflow */
#define GS_ROLL_MAX_FLAT_WINDOWS  150u

struct gs_roll_distance {
	/* streaming causal filters (coeffs from gs_rollator_params.h) */
	struct gs_biquad wheel;                       /* 10-44 Hz bandpass, 2 stages */
	struct gs_biquad hp;                          /* 0.2 Hz highpass, 1 stage */
	struct gs_biquad band[GS_ROLL_NUM_BANDS];     /* 1-49 Hz log bank, 2 stages each */

	/* current 500 ms gate-window accumulators (window RMS = std, mean-subtracted) */
	uint32_t win_n;
	double   wheel_sumsq, wheel_sum;
	double   hp_sumsq, hp_sum;

	/* current 2 s flatness-window band-energy accumulators */
	double   band_sumsq[GS_ROLL_NUM_BANDS];
	uint32_t flat_win_gw;                         /* gate windows into current 2 s window */

	/* per-window feature history (gate + flat resolved at finalize) */
	uint32_t n_gate_windows;
	float    wheel_rms[GS_ROLL_MAX_GATE_WINDOWS];
	float    hp_rms[GS_ROLL_MAX_GATE_WINDOWS];
	uint32_t n_flat_windows;
	float    flat_val[GS_ROLL_MAX_FLAT_WINDOWS];  /* per-2 s-window flatness */

	bool     overflowed;
	uint32_t total_samples;
};

struct gs_roll_outputs {
	bool     valid;              /* false -> caller emits active_min only, no distance */
	float    distance_ft;
	float    flatness;
	float    walk_t_s;           /* motion-gated active time */
	float    vib_int;
	uint32_t n_active_windows;
	bool     overflowed;
};

void gs_roll_distance_init(struct gs_roll_distance *d);   /* loads coeffs + resets */
void gs_roll_distance_reset(struct gs_roll_distance *d);
void gs_roll_distance_step(struct gs_roll_distance *d, float mag_g);   /* one 100 Hz sample */
void gs_roll_distance_finalize(struct gs_roll_distance *d, struct gs_roll_outputs *out);

#ifdef __cplusplus
}
#endif

#endif /* GOSTEADY_GS_ROLLATOR_DISTANCE_H_ */
