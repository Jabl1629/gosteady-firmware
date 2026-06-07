"""Reference implementation of the decoupled step counter + gait speed.

Mirrors `gs_pipeline_finalize()` (src/algo/gs_pipeline.c) and the params in
src/algo/gosteady_algo_params.h. Operates on the emitted peak sample-index
list — the SAME peaks the distance regression consumes — so step count and
gait are decoupled from distance (distance is unchanged; it still uses every
peak). Integer-sample math here matches the C bit-for-bit.

Spec: gosteady-portal/docs/specs/2026-06-07-gait-speed.md (coord §C46).
"""
from __future__ import annotations

from typing import Sequence

# Defaults mirror gosteady_algo_params.h (FS_HZ = 100).
STEP_MERGE_GAP_SAMPLES = 80      # 0.8 s @ 100 Hz
STRIDE_GAP_CAP_SAMPLES = 250     # 2.5 s @ 100 Hz
GAIT_MIN_STEPS = 5
GAIT_MIN_WALK_S = 3.0


def merge_step_count(peak_sample_indices: Sequence[int],
                     merge_gap_samples: int = STEP_MERGE_GAP_SAMPLES) -> int:
    """Reported step count: keep a peak only if it is >= merge_gap_samples from
    the last KEPT step (a refractory merge applied to the count only)."""
    idx = list(peak_sample_indices)
    if not idx:
        return 0
    kept = 1
    last = idx[0]
    for s in idx[1:]:
        if s - last >= merge_gap_samples:
            kept += 1
            last = s
    return kept


def walking_time_s(peak_sample_indices: Sequence[int],
                   fs_hz: float,
                   cap_samples: int = STRIDE_GAP_CAP_SAMPLES) -> float:
    """Gait denominator: sum inter-peak gaps <= cap_samples (longer gaps are
    between-bout pauses) + one mean gated gap for the leading partial stride."""
    idx = list(peak_sample_indices)
    if len(idx) < 2:
        return 0.0
    sum_gated = 0
    count = 0
    for a, b in zip(idx[:-1], idx[1:]):
        gap = b - a
        if gap <= cap_samples:
            sum_gated += gap
            count += 1
    if count == 0:
        return 0.0
    return (sum_gated + sum_gated / count) / fs_hz


def gait_speed_fts(distance_ft: float,
                   peak_sample_indices: Sequence[int],
                   fs_hz: float,
                   *,
                   buffer_overflowed: bool = False,
                   n_peaks_at_cap: bool = False,
                   merge_gap_samples: int = STEP_MERGE_GAP_SAMPLES,
                   cap_samples: int = STRIDE_GAP_CAP_SAMPLES,
                   min_steps: int = GAIT_MIN_STEPS,
                   min_walk_s: float = GAIT_MIN_WALK_S) -> float | None:
    """Session-average gait speed (ft/s), or None when the guards fail
    (too few steps / too little walking time / saturated session)."""
    steps = merge_step_count(peak_sample_indices, merge_gap_samples)
    wt = walking_time_s(peak_sample_indices, fs_hz, cap_samples)
    if (buffer_overflowed or n_peaks_at_cap
            or steps < min_steps or wt < min_walk_s or distance_ft <= 0.0):
        return None
    return distance_ft / wt
