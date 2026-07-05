/*
 * gs_rollator_distance.c — see gs_rollator_distance.h.
 * Mirrors the golden reference algo/rollator_distance_ref.py exactly (causal
 * biquads; per-window features; motion gate + flat-norm resolved at finalize).
 */

#include "gs_rollator_distance.h"
#include "gs_rollator_params.h"     /* GS_ROLL_* defines + SOS coeff arrays */

#include <math.h>
#include <string.h>

#define FLAT_EPS  1e-15

void gs_roll_distance_reset(struct gs_roll_distance *d)
{
	gs_biquad_reset(&d->wheel);
	gs_biquad_reset(&d->hp);
	for (uint32_t k = 0; k < GS_ROLL_NUM_BANDS; k++) {
		gs_biquad_reset(&d->band[k]);
	}
	d->win_n = 0u;
	d->wheel_sumsq = 0.0;
	d->wheel_sum = 0.0;
	d->hp_sumsq = 0.0;
	d->hp_sum = 0.0;
	for (uint32_t k = 0; k < GS_ROLL_NUM_BANDS; k++) {
		d->band_sumsq[k] = 0.0;
	}
	d->flat_win_gw = 0u;
	d->n_gate_windows = 0u;
	d->n_flat_windows = 0u;
	d->overflowed = false;
	d->total_samples = 0u;
}

void gs_roll_distance_init(struct gs_roll_distance *d)
{
	/* Coeffs start from zero state (matches the reference's sosfilt zero-IC). */
	gs_biquad_init(&d->wheel, &gs_roll_wheel_sos[0][0], 2u);
	gs_biquad_init(&d->hp, &gs_roll_hp_sos[0][0], 1u);
	for (uint32_t k = 0; k < GS_ROLL_NUM_BANDS; k++) {
		gs_biquad_init(&d->band[k], &gs_roll_band_sos[k][0][0], 2u);
	}
	gs_roll_distance_reset(d);
}

void gs_roll_distance_step(struct gs_roll_distance *d, float mag_g)
{
	d->total_samples++;

	const float wv = gs_biquad_step(&d->wheel, mag_g);
	const float hv = gs_biquad_step(&d->hp, mag_g);
	d->wheel_sumsq += (double)wv * wv;
	d->wheel_sum += (double)wv;
	d->hp_sumsq += (double)hv * hv;
	d->hp_sum += (double)hv;
	for (uint32_t k = 0; k < GS_ROLL_NUM_BANDS; k++) {
		const float bv = gs_biquad_step(&d->band[k], mag_g);
		d->band_sumsq[k] += (double)bv * bv;
	}

	d->win_n++;
	if (d->win_n < GS_ROLL_GATE_WINDOW_SMPL) {
		return;
	}

	/* --- close a 500 ms gate window: window RMS = population std (mean-subtracted,
	 * = numpy np.std), var = E[x^2] - E[x]^2 --- */
	const double n = (double)GS_ROLL_GATE_WINDOW_SMPL;
	const double wmean = d->wheel_sum / n;
	const double hmean = d->hp_sum / n;
	double wvar = d->wheel_sumsq / n - wmean * wmean;
	double hvar = d->hp_sumsq / n - hmean * hmean;
	if (wvar < 0.0) { wvar = 0.0; }
	if (hvar < 0.0) { hvar = 0.0; }
	if (d->n_gate_windows < GS_ROLL_MAX_GATE_WINDOWS) {
		d->wheel_rms[d->n_gate_windows] = sqrtf((float)wvar);
		d->hp_rms[d->n_gate_windows] = sqrtf((float)hvar);
		d->n_gate_windows++;
	} else {
		d->overflowed = true;
	}
	d->wheel_sumsq = 0.0;
	d->wheel_sum = 0.0;
	d->hp_sumsq = 0.0;
	d->hp_sum = 0.0;
	d->win_n = 0u;

	/* --- close a 2 s flatness window every FLAT_WINDOW_GW gate windows --- */
	d->flat_win_gw++;
	if (d->flat_win_gw >= GS_ROLL_FLAT_WINDOW_GW) {
		/* flatness = geomean(band energy) / mean(band energy); the per-window
		 * 1/N sample-count cancels in the ratio, so raw sum-of-squares works. */
		double sum_log = 0.0, sum_lin = 0.0;
		for (uint32_t k = 0; k < GS_ROLL_NUM_BANDS; k++) {
			const double e = d->band_sumsq[k] + FLAT_EPS;
			sum_log += log(e);
			sum_lin += e;
			d->band_sumsq[k] = 0.0;
		}
		const double gm = exp(sum_log / (double)GS_ROLL_NUM_BANDS);
		const double am = sum_lin / (double)GS_ROLL_NUM_BANDS;
		if (d->n_flat_windows < GS_ROLL_MAX_FLAT_WINDOWS) {
			d->flat_val[d->n_flat_windows] = (float)(gm / am);
			d->n_flat_windows++;
		}
		d->flat_win_gw = 0u;
	}
}

void gs_roll_distance_finalize(struct gs_roll_distance *d, struct gs_roll_outputs *out)
{
	static bool active[GS_ROLL_MAX_GATE_WINDOWS];   /* single-threaded finalize */
	memset(out, 0, sizeof(*out));
	out->overflowed = d->overflowed;

	const uint32_t nw = d->n_gate_windows;

	/* motion gate: Schmitt hysteresis + exit debounce (with back-fill of the
	 * confirmed-still tail) — identical to rollator_distance_ref.gate(). */
	bool state = false;
	uint32_t still = 0u;
	for (uint32_t i = 0; i < nw; i++) {
		const float v = d->wheel_rms[i];
		if (state) {
			if (v < GS_ROLL_GATE_EXIT_G) {
				still++;
				if (still >= GS_ROLL_GATE_DEBOUNCE) {
					state = false;
					const uint32_t start = (i + 1u >= still) ? (i + 1u - still) : 0u;
					for (uint32_t j = start; j <= i; j++) {
						active[j] = false;
					}
				} else {
					active[i] = true;
				}
			} else {
				still = 0u;
				active[i] = true;
			}
		} else {
			if (v > GS_ROLL_GATE_ENTER_G) {
				state = true;
				still = 0u;
				active[i] = true;
			} else {
				active[i] = false;
			}
		}
	}

	/* vib_int + active time over gated windows */
	double vib = 0.0;
	uint32_t nact = 0u;
	for (uint32_t i = 0; i < nw; i++) {
		if (active[i]) {
			vib += (double)d->hp_rms[i] * 0.5;
			nact++;
		}
	}

	/* session flatness: geomean over 2 s windows that are >= FLAT_ACTIVE_FRAC active */
	double sum_log = 0.0;
	uint32_t nflat = 0u;
	for (uint32_t f = 0; f < d->n_flat_windows; f++) {
		const uint32_t gw0 = f * GS_ROLL_FLAT_WINDOW_GW;
		uint32_t a = 0u;
		for (uint32_t j = 0; j < GS_ROLL_FLAT_WINDOW_GW && (gw0 + j) < nw; j++) {
			if (active[gw0 + j]) {
				a++;
			}
		}
		if ((float)a / (float)GS_ROLL_FLAT_WINDOW_GW >= GS_ROLL_FLAT_ACTIVE_FRAC) {
			sum_log += log((double)d->flat_val[f]);
			nflat++;
		}
	}

	out->vib_int = (float)vib;
	out->walk_t_s = (float)((double)nact * 0.5);
	out->n_active_windows = nact;
	out->valid = (nact >= GS_ROLL_MIN_ACTIVE_WIN) && (nflat > 0u) && (vib > 0.0);
	if (out->valid) {
		const float flat = (float)exp(sum_log / (double)nflat);
		out->flatness = flat;
		out->distance_ft = (float)(GS_ROLL_DIST_M * vib / (double)flat);
	}
}
