"""Streaming DF-II-T biquad cascade filters.

Implemented sample-by-sample to match CMSIS-DSP's
`arm_biquad_cascade_df2T_f32` exactly. The `.apply()` batch method is a
convenience for offline eval — it's a bare loop over `.step()` so the
output is bit-identical to what the streaming form produces.

Coefficient layout follows scipy's SOS format: one row per biquad with
`[b0, b1, b2, a0, a1, a2]`; we assume `a0 == 1` (scipy normalizes).

DF-II-T recurrence (per biquad):
    y[n]     = b0 * x[n] + z1[n-1]
    z1[n]    = b1 * x[n] - a1 * y[n] + z2[n-1]
    z2[n]    = b2 * x[n] - a2 * y[n]

Two state variables per biquad. State initialization via `.init_steady(x0)`
primes each biquad as if it had been seeing `x0` for infinite time —
eliminates the startup transient that would otherwise bias the first
few seconds of every session.
"""

from __future__ import annotations

from dataclasses import dataclass
from typing import Iterable

import numpy as np
from scipy import signal as _scipy_signal


@dataclass
class BiquadSOS:
    """Cascade of 2nd-order biquads in DF-II-T form.

    State is two scalars per biquad stage (`z1`, `z2`). Thread-unsafe
    by design — one instance per signal stream. Reset between runs with
    `.reset()` or `.init_steady(x0)`.
    """

    sos: np.ndarray   # shape (n_stages, 6); scipy SOS layout
    z1: np.ndarray = None
    z2: np.ndarray = None

    def __post_init__(self) -> None:
        self.sos = np.asarray(self.sos, dtype=np.float64)
        if self.sos.ndim != 2 or self.sos.shape[1] != 6:
            raise ValueError(f"sos must be shape (n, 6); got {self.sos.shape}")
        if not np.allclose(self.sos[:, 3], 1.0):
            raise ValueError("biquad a0 must be 1 (use scipy SOS normalization)")
        self.reset()

    @property
    def n_stages(self) -> int:
        return int(self.sos.shape[0])

    def reset(self) -> None:
        """Zero the state. Causes a startup transient until the filter
        has seen ~(3 / cutoff_hz) samples — prefer `init_steady(x0)`."""
        self.z1 = np.zeros(self.n_stages, dtype=np.float64)
        self.z2 = np.zeros(self.n_stages, dtype=np.float64)

    def init_steady(self, x0: float) -> None:
        """Initialize the state to the steady-state response of a DC
        input x0. Eliminates startup transient.

        For a DF-II-T biquad in steady state with input x=y=k (DC gain
        determines k): z1 = b1*x - a1*y and z2 = b2*x - a2*y. Apply
        stage-by-stage, cascading the DC gain."""
        x = float(x0)
        for i in range(self.n_stages):
            b0, b1, b2, _a0, a1, a2 = self.sos[i]
            # DC gain of this stage:
            dc_gain = (b0 + b1 + b2) / (1.0 + a1 + a2)
            y = dc_gain * x
            self.z1[i] = b1 * x - a1 * y + (b2 * x - a2 * y)
            self.z2[i] = b2 * x - a2 * y
            x = y   # cascade

    def step(self, x: float) -> float:
        """Process one sample. Returns one sample."""
        v = float(x)
        for i in range(self.n_stages):
            b0, b1, b2, _a0, a1, a2 = self.sos[i]
            y = b0 * v + self.z1[i]
            self.z1[i] = b1 * v - a1 * y + self.z2[i]
            self.z2[i] = b2 * v - a2 * y
            v = y
        return v

    def apply(self, x: Iterable[float]) -> np.ndarray:
        """Batch helper. Equivalent to a bare loop over `.step()`."""
        x = np.asarray(x, dtype=np.float64)
        y = np.empty_like(x)
        for i, xi in enumerate(x):
            y[i] = self.step(xi)
        return y


# ---------------------------------------------------------------------
# Butterworth constructors
# ---------------------------------------------------------------------


def butterworth_hp(cutoff_hz: float, fs: float, order: int = 2) -> BiquadSOS:
    sos = _scipy_signal.butter(order, cutoff_hz, btype="high", fs=fs, output="sos")
    return BiquadSOS(sos=sos)


def butterworth_lp(cutoff_hz: float, fs: float, order: int = 2) -> BiquadSOS:
    sos = _scipy_signal.butter(order, cutoff_hz, btype="low", fs=fs, output="sos")
    return BiquadSOS(sos=sos)
