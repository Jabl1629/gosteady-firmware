/*
 * gs_pipeline.h — V1 distance-estimator orchestrator (auto-surface).
 *
 * Composes the streaming building blocks (filters → motion gate → step
 * detector → roughness → surface classifier → stride sum) into the full
 * session pipeline. Mirrors algo/distance_estimator.py +
 * algo/run_auto_surface.py exactly.
 *
 * Lifecycle:
 *   gs_pipeline_init(p);                  // once at boot
 *   gs_pipeline_session_start(p, mag0);   // at session open; mag0 is |a|_g of the first sample
 *   for each sample:
 *       gs_pipeline_step(p, mag_g);       // call at the sampling rate
 *   gs_pipeline_finalize(p, &outputs);    // at session close — produces the activity payload values
 *
 * Inputs: |a|_g per sample (the caller computes
 * sqrt(ax² + ay² + az²) / 9.80665 from the BMI270 reading).
 *
 * Outputs (gs_pipeline_outputs): distance_ft, step_count (raw peaks),
 * steps_merged (reported, de-satellited), R, surface_class,
 * motion_duration_s, total_duration_s, motion_fraction, walking_time_s,
 * gait_speed_fts (+ gait_valid guard).
 *
 * Memory: dominated by the per-sample buffer used for batch inter-peak
 * RMS. Sized for GS_PIPELINE_MAX_BUFFERED_SAMPLES samples (default
 * 12000 = 120s @ 100Hz). Sessions exceeding this still record + emit
 * distance/step_count/active_minutes correctly; only R becomes NaN (and
 * buffer_overflowed is set in the outputs). 60 KB at the default size.
 */

#ifndef GOSTEADY_GS_PIPELINE_H_
#define GOSTEADY_GS_PIPELINE_H_

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include "gs_filters.h"
#include "gs_motion_gate.h"
#include "gs_step_detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Tunables — sized for first deployment. Bump if longer sessions
 * become typical. */
#ifndef GS_PIPELINE_MAX_BUFFERED_SAMPLES
#define GS_PIPELINE_MAX_BUFFERED_SAMPLES   12000u   /* 120s @ 100Hz */
#endif

#ifndef GS_PIPELINE_MAX_PEAKS
#define GS_PIPELINE_MAX_PEAKS              512u     /* worst case ~5/s */
#endif

struct gs_pipeline_outputs {
	float    distance_ft;
	float    roughness_R;            /* NaN if < 10 samples passed the gate */
	uint8_t  surface_class;          /* gs_surface_t value (0=indoor, 1=outdoor) */
	uint8_t  _pad[3];
	uint32_t step_count;             /* raw emitted peak count (distance feature N) */
	float    motion_duration_s;
	float    total_duration_s;
	float    motion_fraction;
	bool     buffer_overflowed;      /* true if session exceeded buffer; R is over the buffered prefix only */
	uint8_t  _pad2[3];

	/* Gait speed + decoupled step count — post-processed from the emitted
	 * peak train at finalize; the distance/roughness/surface pipeline is
	 * unchanged. See gosteady_algo_params.h GS_STEP_MERGE_* / GS_STRIDE_GAP_*
	 * / GS_GAIT_*. */
	uint32_t steps_merged;           /* reported step count: peaks after the refractory merge */
	float    walking_time_s;         /* gait denominator: gated peak-train walking time (s); 0 if < 2 gated gaps */
	float    gait_speed_fts;         /* distance_ft / walking_time_s, ft/s; 0 unless gait_valid */
	bool     gait_valid;             /* true iff the gait guards passed (emit gait_speed_fts) */
	uint8_t  _pad3[3];
};

struct gs_pipeline {
	/* DSP blocks (configured from gosteady_algo_params.h at init) */
	struct gs_biquad        hp;
	struct gs_biquad        lp;
	struct gs_motion_gate   gate;
	struct gs_step_detector det;

	/* Buffers (used for batch inter-peak RMS at finalize) */
	float    mag_lp_buf[GS_PIPELINE_MAX_BUFFERED_SAMPLES];
	uint8_t  motion_mask_buf[GS_PIPELINE_MAX_BUFFERED_SAMPLES];
	uint32_t peak_indices[GS_PIPELINE_MAX_PEAKS];

	/* Streaming accumulators (always correct regardless of overflow) */
	uint32_t n_peaks;
	uint32_t n_samples_processed;
	uint32_t n_samples_buffered;     /* min(processed, MAX_BUFFERED) */
	double   sum_amp_g;              /* Σ amplitude_g over emitted peaks */
};

/* One-time initialization. Binds filter state to the const coefficient
 * arrays from gosteady_algo_params.h and configures detector + gate
 * with the V1 thresholds. Idempotent — safe to call multiple times.
 *
 * Returns 0 on success, negative errno if any block fails to init
 * (would indicate a build-time configuration mismatch). */
int gs_pipeline_init(struct gs_pipeline *p);

/* Reset session-scoped state and prime the filters to a steady-state
 * response of (first_mag_g - 1.0). Call at session open with the |a|_g
 * value of the first sample. */
void gs_pipeline_session_start(struct gs_pipeline *p, float first_mag_g);

/* Process one sample. The caller passes |a|_g (NOT raw m/s²) — the
 * pipeline subtracts 1.0 g internally to feed gravity-removed input
 * into the HP filter. */
void gs_pipeline_step(struct gs_pipeline *p, float mag_g);

/* Compute final session outputs. Safe to call multiple times (idempotent
 * given the same accumulated state). */
void gs_pipeline_finalize(const struct gs_pipeline *p,
			  struct gs_pipeline_outputs *out);

/* Reported (de-satellited) step count from an ascending peak sample-index
 * array: a peak counts as a step only if it is >= merge_gap_samples from the
 * last KEPT step. Decoupled from distance (which uses every peak). Exposed
 * for unit testing + reuse. Mirrors algo/gait.py::merge_step_count. */
uint32_t gs_merge_step_count(const uint32_t *peak_indices, uint32_t n_peaks,
			     uint32_t merge_gap_samples);

/* Peak-train walking time (seconds) — the gait denominator. Sums inter-peak
 * gaps <= cap_samples (longer gaps are between-bout pauses) + one mean gated
 * gap for the leading partial stride. 0 if < 2 peaks or all gaps exceed the
 * cap. Mirrors algo/gait.py::walking_time_s. */
float gs_walking_time_from_peaks(const uint32_t *peak_indices, uint32_t n_peaks,
				 uint32_t cap_samples, float fs_hz);

#ifdef __cplusplus
}
#endif

#endif  /* GOSTEADY_GS_PIPELINE_H_ */
