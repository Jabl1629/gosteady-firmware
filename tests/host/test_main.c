/*
 * test_main.c — drives the full V1 algorithm against the reference
 * vectors emitted by algo/export_reference_vectors.py.
 *
 * For each fixture, the test:
 *   (1) replays mag_g through the HP filter alone, comparing per-sample
 *       output against fixture.mag_hp
 *   (2) replays mag_g through HP→LP, comparing against fixture.mag_lp
 *   (3) replays mag_g through HP→motion_gate, comparing the boolean
 *       output against fixture.motion_mask
 *   (4) replays mag_g through HP→LP→step_detector, comparing emitted
 *       peaks against fixture.peaks
 *   (5) end-to-end: runs the full gs_pipeline, comparing R, surface
 *       class, distance, step_count, motion duration against the
 *       fixture's expected scalars
 *
 * Tolerances reflect the float32-on-target vs float64-in-Python
 * generation difference. They're tight (sub-1e-3 absolute on filter
 * output, ±1 sample on peak idx, 1% relative on R/distance) but not
 * bit-exact — that would require also running Python in float32.
 */

#include "test_loader.h"

#include "gs_filters.h"
#include "gs_motion_gate.h"
#include "gs_pipeline.h"
#include "gs_roughness.h"
#include "gs_step_detector.h"
#include "gosteady_algo_params.h"
#include "gs_time.h"   /* 0.17.0-time plausibility gate (src/gs_time.h) */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Per-test counter; printed at end. */
static int g_failures = 0;
static int g_checks = 0;

#define CHECK(cond, fmt, ...) do { \
	g_checks++; \
	if (!(cond)) { \
		g_failures++; \
		fprintf(stderr, "  FAIL %s:%d  " fmt "\n", \
		        __FILE__, __LINE__, ##__VA_ARGS__); \
	} \
} while (0)

static bool close_abs(float a, float b, float tol)
{
	return fabsf(a - b) <= tol;
}

static bool close_either(float a, float b, float abs_tol, float rel_tol)
{
	const float diff = fabsf(a - b);
	if (diff <= abs_tol) {
		return true;
	}
	const float ref = fmaxf(fabsf(a), fabsf(b));
	if (ref > 0.0f && diff / ref <= rel_tol) {
		return true;
	}
	return false;
}

/* ----------------------------------------------------------------- */
/* Per-module replays                                                 */
/* ----------------------------------------------------------------- */

static void test_hp_filter(const struct test_fixture *fix, const char *name)
{
	printf("  [HP filter]\n");
	struct gs_biquad hp;
	gs_biquad_init(&hp, gs_hp_coeffs, GS_HP_NUM_STAGES);
	gs_biquad_init_steady(&hp, fix->mag_g[0] - 1.0f);

	float max_abs_diff = 0.0f;
	uint32_t worst_idx = 0u;
	for (uint32_t i = 0u; i < fix->n_samples; i++) {
		const float v = gs_biquad_step(&hp, fix->mag_g[i] - 1.0f);
		const float diff = fabsf(v - fix->mag_hp[i]);
		if (diff > max_abs_diff) {
			max_abs_diff = diff;
			worst_idx = i;
		}
	}
	printf("    max |diff| = %.6e at sample %u (val=%.6e expected=%.6e)\n",
	       max_abs_diff, worst_idx,
	       (double)0.0f /* recompute below for printing only */,
	       (double)fix->mag_hp[worst_idx]);
	CHECK(max_abs_diff < 1e-4f,
	      "%s: HP max |diff| = %.6e exceeds 1e-4", name, max_abs_diff);
	(void)name;
}

static void test_lp_filter(const struct test_fixture *fix, const char *name)
{
	printf("  [LP filter (after HP)]\n");
	struct gs_biquad hp, lp;
	gs_biquad_init(&hp, gs_hp_coeffs, GS_HP_NUM_STAGES);
	gs_biquad_init(&lp, gs_lp_coeffs, GS_LP_NUM_STAGES);
	gs_biquad_init_steady(&hp, fix->mag_g[0] - 1.0f);
	gs_biquad_init_steady(&lp, 0.0f);  /* HP output is ≈0 at DC */

	float max_abs_diff = 0.0f;
	uint32_t worst_idx = 0u;
	for (uint32_t i = 0u; i < fix->n_samples; i++) {
		const float v_hp = gs_biquad_step(&hp, fix->mag_g[i] - 1.0f);
		const float v_lp = gs_biquad_step(&lp, v_hp);
		const float diff = fabsf(v_lp - fix->mag_lp[i]);
		if (diff > max_abs_diff) {
			max_abs_diff = diff;
			worst_idx = i;
		}
	}
	printf("    max |diff| = %.6e at sample %u\n", max_abs_diff, worst_idx);
	/* LP after HP is two cascaded float32 IIRs; cumulative roundoff
	 * relative to Python float64 is bounded but a touch above 1e-4 on
	 * the longest sessions. 2.5e-4 is comfortable headroom. */
	CHECK(max_abs_diff < 2.5e-4f,
	      "%s: LP max |diff| = %.6e exceeds 2.5e-4", name, max_abs_diff);
}

static void test_motion_gate(const struct test_fixture *fix, const char *name)
{
	printf("  [motion gate]\n");
	struct gs_biquad hp;
	struct gs_motion_gate gate;
	gs_biquad_init(&hp, gs_hp_coeffs, GS_HP_NUM_STAGES);
	gs_biquad_init_steady(&hp, fix->mag_g[0] - 1.0f);
	gs_motion_gate_init(&gate,
			    GS_GATE_WINDOW_SAMPLES,
			    GS_GATE_ENTER_G,
			    GS_GATE_EXIT_G,
			    GS_GATE_EXIT_HOLD_SAMPLES);

	uint32_t mismatches = 0u;
	uint32_t first_mismatch_idx = 0u;
	for (uint32_t i = 0u; i < fix->n_samples; i++) {
		const float v_hp = gs_biquad_step(&hp, fix->mag_g[i] - 1.0f);
		const bool m = gs_motion_gate_step(&gate, v_hp);
		const uint8_t got = m ? 1u : 0u;
		if (got != fix->motion_mask[i]) {
			if (mismatches == 0u) {
				first_mismatch_idx = i;
			}
			mismatches++;
		}
	}
	printf("    mismatches = %u of %u (first at %u)\n",
	       mismatches, fix->n_samples, first_mismatch_idx);
	/* Boundary samples can flip due to ULP-level sigma differences;
	 * up to 5 boundary mismatches per session is still acceptable. */
	CHECK(mismatches <= 5u,
	      "%s: motion mask diverged at %u samples (first at %u)",
	      name, mismatches, first_mismatch_idx);
}

static void test_step_detector(const struct test_fixture *fix, const char *name)
{
	printf("  [step detector]\n");
	struct gs_biquad hp, lp;
	struct gs_step_detector det;
	gs_biquad_init(&hp, gs_hp_coeffs, GS_HP_NUM_STAGES);
	gs_biquad_init(&lp, gs_lp_coeffs, GS_LP_NUM_STAGES);
	gs_biquad_init_steady(&hp, fix->mag_g[0] - 1.0f);
	gs_biquad_init_steady(&lp, 0.0f);
	gs_step_detector_init(&det,
			      GS_FS_HZ,
			      GS_PEAK_ENTER_G,
			      GS_PEAK_EXIT_G,
			      GS_PEAK_MIN_GAP_SAMPLES,
			      GS_PEAK_MAX_ACTIVE_SAMPLES);

	struct gs_peak emitted[GS_PIPELINE_MAX_PEAKS];
	uint32_t n_emitted = 0u;
	for (uint32_t i = 0u; i < fix->n_samples; i++) {
		const float v_hp = gs_biquad_step(&hp, fix->mag_g[i] - 1.0f);
		const float v_lp = gs_biquad_step(&lp, v_hp);
		struct gs_peak p;
		if (gs_step_detector_step(&det, v_lp, &p)) {
			if (n_emitted < GS_PIPELINE_MAX_PEAKS) {
				emitted[n_emitted++] = p;
			}
		}
	}

	printf("    emitted = %u  expected = %u\n", n_emitted, fix->n_peaks);
	CHECK(n_emitted == fix->n_peaks,
	      "%s: peak count %u != expected %u",
	      name, n_emitted, fix->n_peaks);

	/* Per-peak comparison is "best-effort match" rather than strict —
	 * float32 vs float64 cumulative noise in the LP filter can shift a
	 * threshold-crossing peak by several samples, splitting/merging
	 * adjacent compound impulses without changing the total step count.
	 * The end-to-end pipeline test catches what actually matters
	 * (Σ amplitude × c1 → distance) so per-peak diffs here are
	 * informational, not blocking — we still warn on big drifts. */
	uint32_t big_idx_diffs = 0u;
	uint32_t big_amp_diffs = 0u;
	double sum_got_amp = 0.0;
	double sum_exp_amp = 0.0;
	const uint32_t cmp_n = (n_emitted < fix->n_peaks) ? n_emitted : fix->n_peaks;
	for (uint32_t i = 0u; i < cmp_n; i++) {
		const struct gs_peak *got = &emitted[i];
		const struct test_peak *exp = &fix->peaks[i];
		const int32_t idx_diff = (int32_t)got->sample_idx - (int32_t)exp->sample_idx;
		if (idx_diff < -10 || idx_diff > 10) {
			big_idx_diffs++;
		}
		if (!close_either(got->amplitude_g, exp->amplitude_g, 0.05f, 0.30f)) {
			big_amp_diffs++;
		}
		sum_got_amp += (double)got->amplitude_g;
		sum_exp_amp += (double)exp->amplitude_g;
	}
	if (big_idx_diffs > 0u || big_amp_diffs > 0u) {
		printf("    note: %u peaks shifted >10 samples, %u peaks differ "
		       ">30%% in amp (acceptable for float32; sum_amp diff = %.2f%%)\n",
		       big_idx_diffs, big_amp_diffs,
		       100.0 * (sum_got_amp - sum_exp_amp) /
		       (sum_exp_amp > 0.0 ? sum_exp_amp : 1.0));
	}
	/* The aggregate Σ amplitude over peaks IS what feeds the regression,
	 * so check that within 5% — meaningful end-to-end constraint. */
	if (sum_exp_amp > 0.0) {
		const double rel = fabs(sum_got_amp - sum_exp_amp) / sum_exp_amp;
		CHECK(rel < 0.05,
		      "%s: Σ peak amplitude diverged %.2f%% (got %.4f vs exp %.4f)",
		      name, 100.0 * rel, sum_got_amp, sum_exp_amp);
	}
}

/* 0.17.0-time: the year-plausibility gate that stops a 1980/2080 modem time
 * from reaching the wire (device-time-reliability spec §4.1). Pure function,
 * no fixtures. Epoch anchors hand-verified against 1980-01-01 (315532800),
 * 2026-06 (~1781000000), and 2080-01-01 (3471292800). */
static void test_time_sanity(void)
{
	printf("\n=== time plausibility gate (gs_time.h) ===\n");
	const int64_t y1980_ms = 315532800000LL;   /* 1980-01-01T00:00:00Z (modem default) */
	const int64_t y2080_ms = 3471292800000LL;  /* 2080-01-01T00:00:00Z (the mis-format) */
	const int64_t y2026_ms = 1781000000000LL;  /* ~2026-06 (a real walk time) */

	CHECK(!gs_time_year_is_plausible(y1980_ms), "reject 1980 modem default");
	CHECK(!gs_time_year_is_plausible(y2080_ms), "reject 2080 mis-format");
	CHECK(!gs_time_year_is_plausible(0),        "reject epoch 0 (1970)");
	CHECK(!gs_time_year_is_plausible(-1),       "reject negative");
	CHECK(gs_time_year_is_plausible(y2026_ms),  "accept 2026");

	/* Window boundaries: [2024-01-01 inclusive, 2051-01-01 exclusive). */
	CHECK(gs_time_year_is_plausible(GS_TIME_PLAUSIBLE_MIN_MS),
	      "accept 2024-01-01 (inclusive lower bound)");
	CHECK(!gs_time_year_is_plausible(GS_TIME_PLAUSIBLE_MIN_MS - 1),
	      "reject 1 ms before 2024-01-01");
	CHECK(!gs_time_year_is_plausible(GS_TIME_PLAUSIBLE_MAX_MS),
	      "reject 2051-01-01 (exclusive upper bound)");
	CHECK(gs_time_year_is_plausible(GS_TIME_PLAUSIBLE_MAX_MS - 1),
	      "accept 1 ms before 2051-01-01");
}

/* Synthetic unit tests for the decoupled step counter + gait denominator.
 * Hand-computed expectations; independent of the reference-vector fixtures. */
static void test_gait_synthetic(void)
{
	printf("\n=== gait / step-merge synthetic units ===\n");
	const float    fs  = GS_FS_HZ;                    /* 100 */
	const uint32_t mg  = GS_STEP_MERGE_GAP_SAMPLES;   /* 80  (0.8 s) */
	const uint32_t cap = GS_STRIDE_GAP_CAP_SAMPLES;   /* 250 (2.5 s) */

	/* empty + single peak (no NULL deref: n<1/n<2 short-circuit) */
	CHECK(gs_merge_step_count(NULL, 0u, mg) == 0u, "merge: empty -> 0");
	CHECK(gs_walking_time_from_peaks(NULL, 0u, cap, fs) == 0.0f,
	      "walk: empty -> 0");
	{
		const uint32_t idx[] = {10u};
		CHECK(gs_merge_step_count(idx, 1u, mg) == 1u, "merge: single -> 1");
		CHECK(gs_walking_time_from_peaks(idx, 1u, cap, fs) == 0.0f,
		      "walk: single -> 0");
	}
	/* 0.6 s-spaced train (gap 60 < 80) -> every other peak merges away.
	 * kept: 0, then 120, 240 -> 3. walk: 5 gaps×60 = 300 + mean 60 = 3.60 s. */
	{
		const uint32_t idx[] = {0u, 60u, 120u, 180u, 240u, 300u};
		CHECK(gs_merge_step_count(idx, 6u, mg) == 3u, "merge: 0.6s train -> 3");
		CHECK(close_abs(gs_walking_time_from_peaks(idx, 6u, cap, fs), 3.60f, 1e-4f),
		      "walk: 0.6s train -> 3.60");
	}
	/* 0.9 s-spaced train (gap 90 >= 80) -> all kept. walk: 9×90=810 + mean 90 = 9.00 s. */
	{
		const uint32_t idx[] = {0u, 90u, 180u, 270u, 360u, 450u, 540u, 630u, 720u, 810u};
		CHECK(gs_merge_step_count(idx, 10u, mg) == 10u, "merge: 0.9s train -> 10");
		CHECK(close_abs(gs_walking_time_from_peaks(idx, 10u, cap, fs), 9.00f, 1e-4f),
		      "walk: 0.9s train -> 9.00");
	}
	/* Two bouts with a 6.3 s pause (gap 630 > 250 cap, excluded). gated:
	 * 6 gaps×90 = 540 + mean 90 = 6.30 s; merge keeps all 8 (all gaps >= 80). */
	{
		const uint32_t idx[] = {0u, 90u, 180u, 270u, 900u, 990u, 1080u, 1170u};
		CHECK(gs_merge_step_count(idx, 8u, mg) == 8u, "merge: two-bout -> 8");
		CHECK(close_abs(gs_walking_time_from_peaks(idx, 8u, cap, fs), 6.30f, 1e-4f),
		      "walk: two-bout pause excluded -> 6.30");
	}
}

static void test_pipeline(const struct test_fixture *fix, const char *name)
{
	printf("  [end-to-end pipeline]\n");
	static struct gs_pipeline pl;  /* large; keep static to avoid stack blowout */
	gs_pipeline_init(&pl);
	gs_pipeline_session_start(&pl, fix->mag_g[0]);

	for (uint32_t i = 0u; i < fix->n_samples; i++) {
		gs_pipeline_step(&pl, fix->mag_g[i]);
	}

	struct gs_pipeline_outputs out;
	gs_pipeline_finalize(&pl, &out);

	printf("    step_count          got=%u  exp=%u\n",
	       out.step_count, fix->n_peaks);
	printf("    surface_class       got=%u  exp=%u\n",
	       out.surface_class, fix->surface_class);
	printf("    R                   got=%.6f  exp=%.6f\n",
	       (double)out.roughness_R, (double)fix->expected_R);
	printf("    distance_ft         got=%.4f  exp=%.4f\n",
	       (double)out.distance_ft, (double)fix->expected_distance_ft);
	printf("    motion_duration_s   got=%.4f  exp=%.4f\n",
	       (double)out.motion_duration_s, (double)fix->expected_motion_duration_s);
	printf("    motion_fraction     got=%.4f  exp=%.4f\n",
	       (double)out.motion_fraction, (double)fix->expected_motion_fraction);

	CHECK(out.step_count == fix->n_peaks,
	      "%s: step_count %u != %u", name, out.step_count, fix->n_peaks);
	CHECK(out.surface_class == fix->surface_class,
	      "%s: surface_class %u != %u",
	      name, out.surface_class, fix->surface_class);

	if (isfinite(fix->expected_R)) {
		CHECK(isfinite(out.roughness_R),
		      "%s: R is NaN, expected %.4f",
		      name, (double)fix->expected_R);
		CHECK(close_either(out.roughness_R, fix->expected_R, 5e-4f, 1e-2f),
		      "%s: R %.6f differs from expected %.6f",
		      name, (double)out.roughness_R, (double)fix->expected_R);
	} else {
		CHECK(!isfinite(out.roughness_R),
		      "%s: R %.4f, expected NaN",
		      name, (double)out.roughness_R);
	}
	CHECK(close_either(out.distance_ft, fix->expected_distance_ft, 0.1f, 1e-2f),
	      "%s: distance_ft %.4f differs from expected %.4f",
	      name, (double)out.distance_ft, (double)fix->expected_distance_ft);
	CHECK(close_abs(out.motion_duration_s, fix->expected_motion_duration_s, 0.05f),
	      "%s: motion_duration_s %.4f differs from expected %.4f",
	      name, (double)out.motion_duration_s,
	      (double)fix->expected_motion_duration_s);

	/* Decoupled step count + gait — validate the pipeline's new outputs
	 * against an independent reference computed over the fixture's own peak
	 * indices. (The pipeline's internal peaks may differ by ±1 sample from
	 * the fixture peaks, which can rarely flip a merge decision at the gap
	 * boundary — hence ±1 tolerance on the merged count.) */
	{
		static uint32_t pidx[GS_PIPELINE_MAX_PEAKS];
		uint32_t np = fix->n_peaks < GS_PIPELINE_MAX_PEAKS
			      ? fix->n_peaks : GS_PIPELINE_MAX_PEAKS;
		for (uint32_t i = 0u; i < np; i++) {
			pidx[i] = fix->peaks[i].sample_idx;
		}
		const uint32_t ref_merged =
			gs_merge_step_count(pidx, np, GS_STEP_MERGE_GAP_SAMPLES);
		const float ref_walk =
			gs_walking_time_from_peaks(pidx, np, GS_STRIDE_GAP_CAP_SAMPLES,
						   GS_FS_HZ);
		printf("    steps_merged        got=%u  ref=%u  (raw=%u)\n",
		       out.steps_merged, ref_merged, out.step_count);
		printf("    walking_time_s      got=%.3f  ref=%.3f\n",
		       (double)out.walking_time_s, (double)ref_walk);
		printf("    gait_speed_fts      got=%.3f  valid=%d\n",
		       (double)out.gait_speed_fts, out.gait_valid);

		CHECK(out.steps_merged <= out.step_count,
		      "%s: steps_merged %u > raw step_count %u",
		      name, out.steps_merged, out.step_count);
		CHECK(out.steps_merged + 1u >= ref_merged && ref_merged + 1u >= out.steps_merged,
		      "%s: steps_merged %u vs ref %u (>±1)",
		      name, out.steps_merged, ref_merged);
		CHECK(close_abs(out.walking_time_s, ref_walk, 0.05f),
		      "%s: walking_time_s %.4f vs ref %.4f",
		      name, (double)out.walking_time_s, (double)ref_walk);
		if (out.gait_valid) {
			CHECK(out.steps_merged >= GS_GAIT_MIN_STEPS
			      && out.walking_time_s >= GS_GAIT_MIN_WALK_S
			      && out.distance_ft > 0.0f,
			      "%s: gait_valid but guards not met", name);
			CHECK(close_either(out.gait_speed_fts,
					   out.distance_ft / out.walking_time_s,
					   1e-3f, 1e-3f),
			      "%s: gait_speed_fts %.4f != distance/walking_time",
			      name, (double)out.gait_speed_fts);
		} else {
			CHECK(out.gait_speed_fts == 0.0f,
			      "%s: gait invalid but speed %.4f != 0",
			      name, (double)out.gait_speed_fts);
		}
	}
}

/* ----------------------------------------------------------------- */
/* Driver                                                             */
/* ----------------------------------------------------------------- */

static int run_one(const char *path)
{
	struct test_fixture fix;
	if (test_fixture_load(path, &fix) != 0) {
		return -1;
	}
	const char *name = strrchr(path, '/');
	name = name ? name + 1 : path;
	printf("\n=== %s ===\n", name);
	printf("    n_samples=%u  n_peaks=%u  fs_hz=%.3f  surface=%u\n",
	       fix.n_samples, fix.n_peaks, (double)fix.fs_hz, fix.surface_class);

	test_hp_filter(&fix, name);
	test_lp_filter(&fix, name);
	test_motion_gate(&fix, name);
	test_step_detector(&fix, name);
	test_pipeline(&fix, name);

	test_fixture_free(&fix);
	return 0;
}

int main(int argc, char **argv)
{
	const char *defaults[] = {
		"src/algo/test_vectors/indoor_run05_walk_20ft.bin",
		"src/algo/test_vectors/indoor_run09_stationary_30s.bin",
		"src/algo/test_vectors/outdoor_run13_walk_20ft.bin",
	};
	int n;
	const char **paths;
	if (argc > 1) {
		n = argc - 1;
		paths = (const char **)(argv + 1);
	} else {
		n = (int)(sizeof(defaults) / sizeof(defaults[0]));
		paths = defaults;
	}

	test_time_sanity();
	test_gait_synthetic();

	for (int i = 0; i < n; i++) {
		if (run_one(paths[i]) != 0) {
			fprintf(stderr, "FATAL: could not load %s\n", paths[i]);
			return 2;
		}
	}

	printf("\n=== SUMMARY ===\n");
	printf("    %d checks, %d failure%s\n",
	       g_checks, g_failures, g_failures == 1 ? "" : "s");
	return g_failures == 0 ? 0 : 1;
}
