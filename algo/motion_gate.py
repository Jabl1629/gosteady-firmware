"""Motion gate — short-window energy threshold with hysteresis.

Purpose: emit a boolean "is the walker moving right now?" stream
suitable for

  (a) aggregating into `motion_duration_s` / `active_minutes`, and
  (b) future power gating on-device (skip full step detection when
      parked).

Implementation is a running variance over a fixed-length window, with
separate enter/exit thresholds and a minimum dwell below the exit
threshold to avoid chatter around the boundary.

All state is O(1):
- `sum` and `sum_sq` updated incrementally via add-new, subtract-old
- `ring[W]` holds the last W samples
- A couple of small counters and a single state bit

Total footprint on device (float32): 8·W + ~32 bytes. For W=50 samples
(500 ms @ 100 Hz), that's ~232 bytes — trivial.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import numpy as np


@dataclass
class MotionGate:
    """Running-sigma gate with Schmitt hysteresis.

    Fires True when the rolling σ of the input (over `window_samples`)
    first exceeds `enter_threshold`. Stays True until σ has been below
    `exit_threshold` for `exit_hold_samples` consecutive samples.
    """

    fs: float
    window_samples: int = 50          # 500 ms @ 100 Hz
    enter_threshold: float = 0.01     # g (from Phase 2: 11× stationary σ)
    exit_threshold: float = 0.005     # g (hysteresis gap)
    exit_hold_samples: int = 200      # 2 s @ 100 Hz

    # State (populated in __post_init__)
    _ring: np.ndarray = None
    _ring_pos: int = 0
    _ring_filled: bool = False
    _sum: float = 0.0
    _sum_sq: float = 0.0
    _below_count: int = 0
    _in_motion: bool = False
    _motion_sample_count: int = 0
    _total_sample_count: int = 0

    def __post_init__(self) -> None:
        if self.exit_threshold > self.enter_threshold:
            raise ValueError(
                f"exit_threshold ({self.exit_threshold}) must be ≤ "
                f"enter_threshold ({self.enter_threshold}) for hysteresis"
            )
        self.reset()

    def reset(self) -> None:
        self._ring = np.zeros(self.window_samples, dtype=np.float64)
        self._ring_pos = 0
        self._ring_filled = False
        self._sum = 0.0
        self._sum_sq = 0.0
        self._below_count = 0
        self._in_motion = False
        self._motion_sample_count = 0
        self._total_sample_count = 0

    @property
    def motion_duration_s(self) -> float:
        return self._motion_sample_count / self.fs

    @property
    def total_duration_s(self) -> float:
        return self._total_sample_count / self.fs

    @property
    def motion_fraction(self) -> float:
        return (self._motion_sample_count / self._total_sample_count
                if self._total_sample_count > 0 else 0.0)

    def step(self, x: float) -> bool:
        """Process one sample of the input signal. Returns current
        in_motion state (post-update). Expects `x` to be a
        gravity-removed channel such as |a|_HP in g — the thresholds
        assume that unit."""
        x = float(x)
        old = self._ring[self._ring_pos]

        # Incremental sums.
        if self._ring_filled:
            self._sum += x - old
            self._sum_sq += x * x - old * old
        else:
            self._sum += x
            self._sum_sq += x * x

        self._ring[self._ring_pos] = x
        self._ring_pos = (self._ring_pos + 1) % self.window_samples
        if self._ring_pos == 0:
            self._ring_filled = True

        # Compute window std. Use population variance (N) to match the
        # trivial C port — the difference vs. sample variance for
        # W=50 is negligible for our thresholds.
        n = self.window_samples if self._ring_filled else self._ring_pos
        if n < 2:
            self._total_sample_count += 1
            return self._in_motion
        mean = self._sum / n
        var = max(self._sum_sq / n - mean * mean, 0.0)
        sigma = np.sqrt(var)

        if self._in_motion:
            if sigma < self.exit_threshold:
                self._below_count += 1
                if self._below_count >= self.exit_hold_samples:
                    self._in_motion = False
                    self._below_count = 0
            else:
                self._below_count = 0
        else:
            if sigma >= self.enter_threshold:
                self._in_motion = True
                self._below_count = 0

        self._total_sample_count += 1
        if self._in_motion:
            self._motion_sample_count += 1
        return self._in_motion

    def apply(self, x: Iterable[float]) -> np.ndarray:
        x = np.asarray(x, dtype=np.float64)
        mask = np.empty(x.shape, dtype=bool)
        for i, xi in enumerate(x):
            mask[i] = self.step(xi)
        return mask
