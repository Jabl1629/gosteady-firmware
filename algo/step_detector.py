"""Streaming step detector — Schmitt-trigger peak FSM with per-peak
feature extraction.

Operates on a gravity-removed, LP-smoothed accelerometer magnitude
channel (|a|_HP_LP, in g). Emits one `Peak` object each time a
candidate is confirmed and survives a minimum-gap check against the
previous emission.

FSM states:
    IDLE    — waiting for the signal to exceed `enter_threshold`
    ACTIVE  — tracking the running max + accumulating features; exits
              when the signal falls back below `exit_threshold`

Per-peak features are accumulated in one pass during the ACTIVE state:
    amplitude_g  — max(x) during the active window
    duration_s   — time spent above `exit_threshold` (≈ time the walker
                   leg is exerting force on the cap)
    energy_g2s   — ∫ x² dt over the active window (sum·dt)

Features chosen to be:
    1. Physically meaningful (amplitude = impulse strength, duration =
       contact time, energy = total work in the impulse).
    2. Trivially computed in streaming mode (one running max, one
       counter, one running sum of squares).
    3. Not collinear-by-construction — duration and amplitude can vary
       independently (short-sharp vs. long-soft impulses), which is
       what the multi-feature regression exploits.

The FSM uses a Schmitt trigger (two thresholds) rather than a single
threshold because impulses have noisy shoulders — a single-threshold
detector would emit two peaks per impulse when the signal dithers
around the threshold. With Phase-2 thresholds (0.05 g enter, 0.01 g
exit), any impulse amplitude >= 0.05 g is caught cleanly and re-arms
only after the signal has unambiguously returned to baseline.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Optional

import numpy as np


@dataclass
class Peak:
    """One confirmed peak. `sample_idx` is the sample where the max
    was recorded (not where the state transitioned — FSM emission is
    slightly delayed but the reported time is the max itself)."""
    sample_idx: int
    time_s: float
    amplitude_g: float
    duration_s: float
    energy_g2s: float

    def as_feature_vector(self) -> np.ndarray:
        """(amplitude_g, duration_s, energy_g2s) — the row that feeds
        the stride regression's design matrix."""
        return np.array([self.amplitude_g, self.duration_s, self.energy_g2s],
                        dtype=np.float64)


FEATURE_NAMES = ("amplitude_g", "duration_s", "energy_g2s")


@dataclass
class StepDetector:
    """Streaming peak FSM. One instance per signal stream; call
    `.reset()` between runs."""

    fs: float
    enter_threshold_g: float = 0.05   # Phase 2 proposal: ~50× stationary σ
    exit_threshold_g: float = 0.01    # hysteresis gap, still > noise
    min_gap_s: float = 1.0            # ~1.5× max observed cadence
    # Guards against pathological "infinite ACTIVE" if the signal
    # never drops below exit_threshold (shouldn't happen with our
    # signal but we don't want unbounded state).
    max_active_s: float = 5.0

    # State ----------------------------------------------------------
    _sample_idx: int = 0
    _active: bool = False
    _active_start_idx: int = -1
    _candidate_max: float = 0.0
    _candidate_max_idx: int = -1
    _energy_sum_sq: float = 0.0
    _last_emit_idx: int = -10**9     # "-inf" so the first peak always
                                     # clears min_gap

    def __post_init__(self) -> None:
        if self.exit_threshold_g >= self.enter_threshold_g:
            raise ValueError("exit_threshold must be < enter_threshold")
        self.reset()

    def reset(self) -> None:
        self._sample_idx = 0
        self._active = False
        self._active_start_idx = -1
        self._candidate_max = 0.0
        self._candidate_max_idx = -1
        self._energy_sum_sq = 0.0
        self._last_emit_idx = -10**9

    def _emit_if_armed(self) -> Optional[Peak]:
        """Finalize the current ACTIVE window into a Peak (may be
        filtered out by min_gap)."""
        if self._active_start_idx < 0 or self._candidate_max_idx < 0:
            return None
        gap_samples = self._candidate_max_idx - self._last_emit_idx
        if gap_samples / self.fs < self.min_gap_s:
            return None
        duration_s = ((self._sample_idx - self._active_start_idx) / self.fs)
        energy_g2s = self._energy_sum_sq / self.fs
        peak = Peak(
            sample_idx=self._candidate_max_idx,
            time_s=self._candidate_max_idx / self.fs,
            amplitude_g=self._candidate_max,
            duration_s=duration_s,
            energy_g2s=energy_g2s,
        )
        self._last_emit_idx = self._candidate_max_idx
        return peak

    def step(self, x: float) -> Optional[Peak]:
        """Process one |a|_HP_LP sample. Returns a `Peak` on the sample
        that confirms emission, else None."""
        x = float(x)
        emitted: Optional[Peak] = None

        if not self._active:
            if x >= self.enter_threshold_g:
                self._active = True
                self._active_start_idx = self._sample_idx
                self._candidate_max = x
                self._candidate_max_idx = self._sample_idx
                self._energy_sum_sq = x * x
        else:
            if x > self._candidate_max:
                self._candidate_max = x
                self._candidate_max_idx = self._sample_idx
            self._energy_sum_sq += x * x

            active_duration_s = (
                (self._sample_idx - self._active_start_idx + 1) / self.fs
            )
            # Normal exit: signal returned below exit threshold.
            if x < self.exit_threshold_g:
                emitted = self._emit_if_armed()
                self._active = False
                self._active_start_idx = -1
                self._candidate_max = 0.0
                self._candidate_max_idx = -1
                self._energy_sum_sq = 0.0
            # Safety exit: clamp unbounded active windows.
            elif active_duration_s >= self.max_active_s:
                emitted = self._emit_if_armed()
                self._active = False
                self._active_start_idx = -1
                self._candidate_max = 0.0
                self._candidate_max_idx = -1
                self._energy_sum_sq = 0.0

        self._sample_idx += 1
        return emitted

    def apply(self, x: Iterable[float]) -> list[Peak]:
        """Batch helper — one pass through `step()`."""
        peaks: list[Peak] = []
        for xi in x:
            p = self.step(xi)
            if p is not None:
                peaks.append(p)
        # Do NOT force-close any in-progress ACTIVE state at end of
        # stream: an unclosed window means the run ended while the
        # sensor was still in the middle of an impulse (e.g., the
        # operator hit STOP on the down-swing). Emitting would give
        # that peak a clipped duration/energy and bias the regression.
        return peaks
