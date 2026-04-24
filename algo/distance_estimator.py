"""Step-based distance estimator — Phase 3 baseline for GoSteady v1.

Wires the streaming filter → step detector → stride regression →
distance pipeline into a `Predictor` consumable by `algo.evaluator`.
Also runs the motion gate in parallel and exposes motion duration
as a first-class diagnostic.

Pipeline (per run):

    IMU samples  →  |a|_native (m/s²)  →  ÷ g  →  |a|_g
                                                    │
                              ┌─────────────────────┤
                              ▼                     ▼
                         HP 0.2 Hz              (gate input uses the
                         (gravity               same HP channel so its
                          removal)              thresholds live in the
                              │                  same unit system)
                              ▼
                    LP 5 Hz (step-shaping)
                              │
                              ▼
                       StepDetector  →  list[Peak]
                              │
                              ▼
                    StrideRegression  →  Σ stride_ft  =  distance_ft

The same `_extract_peaks_and_features` method is used for both fit
(on training runs) and predict (on held-out runs). The only branch is
whether coefficients already exist: fit accumulates the design matrix
across training runs then solves; predict applies the fitted
coefficients.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Sequence

import numpy as np

from .data_loader import Run
from .evaluator import Prediction, Predictor
from .filters import BiquadSOS, butterworth_hp, butterworth_lp
from .motion_gate import MotionGate
from .step_detector import Peak, StepDetector
from .stride_model import DEFAULT_FEATURES, StrideRegression


_G = 9.80665  # m/s² → g


@dataclass
class StepBasedDistanceEstimator:
    """Phase 3 baseline predictor.

    Instantiate, call `.fit(train_runs)`, then `.predict(held_out_run)`.
    `.fit` is stateless re: prior calls — LOO refits from scratch.
    """

    # Filter design ---------------------------------------------------
    hp_cutoff_hz: float = 0.2
    lp_cutoff_hz: float = 5.0
    filter_order: int = 2

    # Peak detection --------------------------------------------------
    # V1 (Phase 3) defaults: loose Schmitt thresholds capture the
    # compound two-wheel/glide impulse structure (≈2 accel impulses per
    # operator-perceived step). The stride regression absorbs the ratio,
    # and the tighter exit + shorter min-gap pass the stationary
    # robustness gate (1 ft limit) that A1's 0.05 g enter threshold
    # narrowly fails. See run_path_comparison.py for the A/B.
    peak_enter_g: float = 0.02
    peak_exit_g: float = 0.005
    peak_min_gap_s: float = 0.5
    peak_max_active_s: float = 5.0

    # Motion gate ----------------------------------------------------
    motion_window_s: float = 0.5
    motion_enter_g: float = 0.01
    motion_exit_g: float = 0.005
    motion_exit_hold_s: float = 2.0

    # Stride regression ----------------------------------------------
    # V1 (Phase 3) default: single-feature (amplitude only), matching
    # the prior-work structure. Multi-feature overfits badly on 8
    # training runs (cond(X)≈200, generalization MAPE 7 pt worse).
    # Revisit at 30 runs.
    features: tuple[str, ...] = ("amplitude_g",)
    include_intercept: bool = True
    ridge_alpha: float = 1e-3

    # Fitted model ---------------------------------------------------
    _stride: StrideRegression = field(init=False, repr=False,
                                      default_factory=StrideRegression)

    def __post_init__(self) -> None:
        self._stride = StrideRegression(
            features=self.features,
            include_intercept=self.include_intercept,
            ridge_alpha=self.ridge_alpha,
        )

    # ---- Pipeline application (per run) ----------------------------

    def _build_pipeline(self, fs: float) -> tuple[BiquadSOS, BiquadSOS,
                                                   StepDetector, MotionGate]:
        hp = butterworth_hp(self.hp_cutoff_hz, fs=fs, order=self.filter_order)
        lp = butterworth_lp(self.lp_cutoff_hz, fs=fs, order=self.filter_order)
        det = StepDetector(
            fs=fs,
            enter_threshold_g=self.peak_enter_g,
            exit_threshold_g=self.peak_exit_g,
            min_gap_s=self.peak_min_gap_s,
            max_active_s=self.peak_max_active_s,
        )
        gate = MotionGate(
            fs=fs,
            window_samples=max(2, int(round(self.motion_window_s * fs))),
            enter_threshold=self.motion_enter_g,
            exit_threshold=self.motion_exit_g,
            exit_hold_samples=max(1, int(round(self.motion_exit_hold_s * fs))),
        )
        return hp, lp, det, gate

    def _process_run(self, run: Run) -> dict:
        """Run the streaming pipeline over one session. Returns a dict
        of (peaks, motion fraction/duration, intermediate signals)."""
        fs = run.derived_rate_hz or run.header.sample_rate_hz or 100.0
        hp, lp, det, gate = self._build_pipeline(fs)

        mag_g = run.accel_mag_g
        # Prime HP filter to steady state at |a|=1 g so gravity
        # subtracts cleanly from sample 0. (The resting value of
        # |a| is g, and the HP at DC should give 0.)
        hp.init_steady(float(mag_g[0]))

        peaks: list[Peak] = []
        n = len(mag_g)
        mag_hp = np.empty(n, dtype=np.float64)
        mag_lp = np.empty(n, dtype=np.float64)
        motion_mask = np.empty(n, dtype=bool)

        for i in range(n):
            v_hp = hp.step(mag_g[i] - 1.0)   # subtract 1g so the HP
                                              # converges faster (residual
                                              # DC offset gets killed
                                              # anyway, but this skips
                                              # the transient)
            mag_hp[i] = v_hp
            v_lp = lp.step(v_hp)
            mag_lp[i] = v_lp
            motion_mask[i] = gate.step(v_hp)
            peak = det.step(v_lp)
            if peak is not None:
                peaks.append(peak)

        return {
            "fs": fs,
            "peaks": peaks,
            "mag_g": mag_g,
            "mag_hp": mag_hp,
            "mag_lp": mag_lp,
            "motion_mask": motion_mask,
            "motion_duration_s": gate.motion_duration_s,
            "motion_fraction": gate.motion_fraction,
            "total_duration_s": gate.total_duration_s,
        }

    # ---- Predictor interface ---------------------------------------

    def fit(self, train_runs: Sequence[Run]) -> None:
        run_peaks: list[list[Peak]] = []
        distances: list[float] = []
        for r in train_runs:
            res = self._process_run(r)
            run_peaks.append(res["peaks"])
            distances.append(r.actual_distance_ft)
        self._stride.fit(run_peaks, distances)

    def predict(self, run: Run) -> Prediction:
        res = self._process_run(run)
        peaks = res["peaks"]
        if self._stride.coeffs is None:
            # Allowed in `predict`-only contexts (e.g. robustness gates
            # using a just-fit predictor — in that case fit() has been
            # called). Guard against misuse.
            raise RuntimeError("predict() called before fit()")
        distance_ft = self._stride.predict_distance(peaks)
        distance_ft = max(distance_ft, 0.0)  # stride regression can
                                              # produce tiny negative
                                              # distances on edge-case
                                              # inputs (e.g. stationary
                                              # with zero peaks + neg
                                              # intercept). Clamp.
        return Prediction(
            distance_ft=distance_ft,
            step_count=len(peaks),
            diagnostics={
                "fs_hz": res["fs"],
                "motion_duration_s": res["motion_duration_s"],
                "motion_fraction": res["motion_fraction"],
                "total_duration_s": res["total_duration_s"],
                "peaks": peaks,
                "stride_formula": (
                    self._stride.stride_formula()
                    if self._stride.coeffs is not None else None
                ),
            },
        )

    # ---- Introspection ---------------------------------------------

    @property
    def stride(self) -> StrideRegression:
        return self._stride
