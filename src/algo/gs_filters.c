/*
 * gs_filters.c — see gs_filters.h.
 */

#include "gs_filters.h"

#include <errno.h>
#include <string.h>

int gs_biquad_init(struct gs_biquad *bq, const float *coeffs, uint32_t n_stages)
{
	if (bq == NULL || coeffs == NULL) {
		return -EINVAL;
	}
	if (n_stages == 0u || n_stages > GS_BIQUAD_MAX_STAGES) {
		return -EINVAL;
	}
	bq->coeffs = coeffs;
	bq->n_stages = n_stages;
	gs_biquad_reset(bq);
	return 0;
}

void gs_biquad_reset(struct gs_biquad *bq)
{
	memset(bq->d1, 0, sizeof(bq->d1));
	memset(bq->d2, 0, sizeof(bq->d2));
}

void gs_biquad_init_steady(struct gs_biquad *bq, float x0)
{
	float x = x0;
	for (uint32_t i = 0u; i < bq->n_stages; i++) {
		const float *c = &bq->coeffs[i * 5u];
		const float b0 = c[0];
		const float b1 = c[1];
		const float b2 = c[2];
		const float a1 = c[3];   /* CMSIS-style: already negated vs scipy */
		const float a2 = c[4];

		/* DC gain of this stage. CMSIS recurrence is
		 *   y = b0*x + d1
		 *   d1 = b1*x + a1*y + d2
		 *   d2 = b2*x + a2*y
		 * In steady state with input x and output y:
		 *   y/x = (b0+b1+b2) / (1 - a1 - a2)
		 * (matches the standard scipy DC-gain formula since a1c = -a1_scipy.) */
		const float denom = 1.0f - a1 - a2;
		const float dc_gain = (b0 + b1 + b2) / denom;
		const float y = dc_gain * x;

		/* Steady-state delay line values */
		const float d2_ss = b2 * x + a2 * y;
		const float d1_ss = b1 * x + a1 * y + d2_ss;
		bq->d1[i] = d1_ss;
		bq->d2[i] = d2_ss;

		/* Cascade the steady DC output into the next stage. */
		x = y;
	}
}

float gs_biquad_step(struct gs_biquad *bq, float x)
{
	float v = x;
	for (uint32_t i = 0u; i < bq->n_stages; i++) {
		const float *c = &bq->coeffs[i * 5u];
		const float b0 = c[0];
		const float b1 = c[1];
		const float b2 = c[2];
		const float a1 = c[3];
		const float a2 = c[4];

		const float y = b0 * v + bq->d1[i];
		bq->d1[i] = b1 * v + a1 * y + bq->d2[i];
		bq->d2[i] = b2 * v + a2 * y;
		v = y;
	}
	return v;
}
