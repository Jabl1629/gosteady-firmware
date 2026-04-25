/*
 * gosteady_algo_params.h — V1 distance-estimator coefficients (auto-generated).
 *
 * DO NOT EDIT BY HAND. Regenerate with:
 *     algo/venv/bin/python3 -m algo.export_c_header
 *
 * Generator:        algo/export_c_header.py
 * Generated (UTC):  2026-04-25T22:42:16+00:00
 * Git commit:       8008c5021cfa (DIRTY WORKING TREE)
 * Algorithm:        V1 — auto-surface roughness adjustment
 * Algo version:     0.6.0-algo-v1
 *
 * Coefficient provenance:
 *   indoor stride fit on 8 valid walks from raw_sessions/2026-04-23/
 *   outdoor stride fit on 8 valid walks from raw_sessions/2026-04-25/
 *
 * Filter coefficients are Butterworth SOS in CMSIS-DSP
 * arm_biquad_cascade_df2T_f32 storage layout: per stage
 * { b0, b1, b2, -a1, -a2 } (a0 implicit = 1; feedback-term signs
 * negated per CMSIS convention).
 */

#ifndef GOSTEADY_ALGO_PARAMS_H
#define GOSTEADY_ALGO_PARAMS_H

#include <stdint.h>

/* === Sampling === */
#define GS_FS_HZ                          100.0f
#define GS_FS_HZ_INT                      100

/* === HP filter (gravity removal) === */
#define GS_HP_CUTOFF_HZ                   0.200f
#define GS_HP_NUM_STAGES                  1
static const float gs_hp_coeffs[5] = {
     9.911535978e-01f, -1.982307196e+00f,  9.911535978e-01f,  1.982228875e+00f, -9.823854566e-01f
};

/* === LP filter (step shaping) === */
#define GS_LP_CUTOFF_HZ                   5.000f
#define GS_LP_NUM_STAGES                  1
static const float gs_lp_coeffs[5] = {
     2.008336596e-02f,  4.016673192e-02f,  2.008336596e-02f,  1.561018109e+00f, -6.413515210e-01f
};

/* === Step detector (Schmitt-trigger peak FSM) === */
#define GS_PEAK_ENTER_G                   0.0200f
#define GS_PEAK_EXIT_G                    0.0050f
#define GS_PEAK_MIN_GAP_S                 0.500f
#define GS_PEAK_MIN_GAP_SAMPLES           50
#define GS_PEAK_MAX_ACTIVE_S              5.000f
#define GS_PEAK_MAX_ACTIVE_SAMPLES        500

/* === Motion gate (running-σ with hysteresis) === */
#define GS_GATE_WINDOW_S                  0.500f
#define GS_GATE_WINDOW_SAMPLES            50
#define GS_GATE_ENTER_G                   0.0100f
#define GS_GATE_EXIT_G                    0.0050f
#define GS_GATE_EXIT_HOLD_S               2.000f
#define GS_GATE_EXIT_HOLD_SAMPLES         200

/* === Roughness metric (motion-gated inter-peak RMS) === */
#define GS_ROUGH_HALF_WINDOW_S            0.200f
#define GS_ROUGH_HALF_WINDOW_SAMPLES      20

/* === Surface classifier === */
#define GS_R_THRESHOLD                    0.2450f

typedef enum {
    GS_SURFACE_INDOOR  = 0,
    GS_SURFACE_OUTDOOR = 1,
    GS_NUM_SURFACES    = 2,
} gs_surface_t;

/* === Per-surface stride coefficients ===
 * Inference per peak:
 *     stride_ft = gs_stride_intercept_ft[surface]
 *               + gs_stride_amp_coeff[surface] * peak_amp_g
 * Per-session distance is the sum over emitted peaks, clamped to >= 0.
 */
static const float gs_stride_intercept_ft[GS_NUM_SURFACES] = {
    +2.270902338e-01f,  /* GS_SURFACE_INDOOR  */
    -7.434298964e-02f,  /* GS_SURFACE_OUTDOOR */
};

static const float gs_stride_amp_coeff[GS_NUM_SURFACES] = {
    +2.804284688e+00f,  /* GS_SURFACE_INDOOR  */
    +1.820037386e+00f,  /* GS_SURFACE_OUTDOOR */
};

/* === Provenance strings (also include in telemetry / logs) === */
#define GS_ALGO_VERSION_STR               "0.6.0-algo-v1"
#define GS_ALGO_PARAMS_GIT_SHA            "8008c5021cfa (DIRTY WORKING TREE)"
#define GS_ALGO_PARAMS_GEN_UTC            "2026-04-25T22:42:16+00:00"

#endif /* GOSTEADY_ALGO_PARAMS_H */
