/*
 * gs_motion_gate.c — see gs_motion_gate.h.
 */

#include "gs_motion_gate.h"

#include <errno.h>
#include <math.h>
#include <string.h>

int gs_motion_gate_init(struct gs_motion_gate *g,
			uint32_t window_samples,
			float enter_threshold,
			float exit_threshold,
			uint32_t exit_hold_samples)
{
	if (g == NULL) {
		return -EINVAL;
	}
	if (window_samples == 0u || window_samples > GS_MOTION_GATE_MAX_WINDOW) {
		return -EINVAL;
	}
	if (exit_threshold > enter_threshold) {
		return -EINVAL;
	}

	g->window_samples = window_samples;
	g->enter_threshold = enter_threshold;
	g->exit_threshold = exit_threshold;
	g->exit_hold_samples = exit_hold_samples;
	gs_motion_gate_reset(g);
	return 0;
}

void gs_motion_gate_reset(struct gs_motion_gate *g)
{
	memset(g->ring, 0, sizeof(g->ring));
	g->ring_pos = 0u;
	g->ring_filled = false;
	g->sum = 0.0f;
	g->sum_sq = 0.0f;
	g->below_count = 0u;
	g->in_motion = false;
	g->motion_sample_count = 0u;
	g->total_sample_count = 0u;
}

bool gs_motion_gate_step(struct gs_motion_gate *g, float x)
{
	const float old = g->ring[g->ring_pos];

	/* Incremental sums. Once the ring has wrapped at least once, every
	 * incoming sample replaces a known stored sample, so we add-new and
	 * subtract-old. Before that, we're still filling. */
	if (g->ring_filled) {
		g->sum += x - old;
		g->sum_sq += x * x - old * old;
	} else {
		g->sum += x;
		g->sum_sq += x * x;
	}

	g->ring[g->ring_pos] = x;
	g->ring_pos++;
	if (g->ring_pos >= g->window_samples) {
		g->ring_pos = 0u;
		g->ring_filled = true;
	}

	const uint32_t n = g->ring_filled ? g->window_samples : g->ring_pos;
	g->total_sample_count++;

	if (n < 2u) {
		/* Not enough samples to compute σ. Hold previous in_motion
		 * (which is initially false). */
		if (g->in_motion) {
			g->motion_sample_count++;
		}
		return g->in_motion;
	}

	const float inv_n = 1.0f / (float)n;
	const float mean = g->sum * inv_n;
	float var = g->sum_sq * inv_n - mean * mean;
	if (var < 0.0f) {
		var = 0.0f;  /* numerical safety: catastrophic cancellation */
	}
	const float sigma = sqrtf(var);

	if (g->in_motion) {
		if (sigma < g->exit_threshold) {
			g->below_count++;
			if (g->below_count >= g->exit_hold_samples) {
				g->in_motion = false;
				g->below_count = 0u;
			}
		} else {
			g->below_count = 0u;
		}
	} else {
		if (sigma >= g->enter_threshold) {
			g->in_motion = true;
			g->below_count = 0u;
		}
	}

	if (g->in_motion) {
		g->motion_sample_count++;
	}
	return g->in_motion;
}
