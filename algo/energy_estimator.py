"""Path B — distance from integrated signal energy.

Tests the hypothesis that the two-wheel/glide walker is better modeled
as a continuous work-input process than as discrete step events:

    distance_ft  ≈  a · ∫ |a|_HP_LP² dt   +   (b · T_motion  if enabled)  +  c

Integration runs over motion-gated samples only. No peak detection —
the motion gate's Schmitt-trigger output defines which samples
contribute. On a gliding walker where each stride produces a compound
impulse structure that my peak detector was catching inconsistently,
total energy should be a more stable primitive than counted peaks.

Three configurable variants, tested against the same LOO harness:

    features=('E',)            → D = a·E + c                  (1 DOF)
    features=('E', 'T')        → D = a·E + b·T + c            (2 DOF)
    features=('T',)            → D = b·T + c                  (degenerate
                                                                baseline — if
                                                                this wins,
                                                                speed is so
                                                                uniform that
                                                                just knowing
                                                                duration is
                                                                enough)

Uses the same streaming filter + motion-gate blocks as the peak
estimator, so a production C port shares the same parameter file.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Sequence

import numpy as np

from .data_loader import Run
from .evaluator import Prediction
from .filters import butterworth_hp, butterworth_lp
from .motion_gate import MotionGate


ENERGY_FEATURE_NAMES = ("E_motion_g2s", "T_motion_s")


@dataclass
class EnergyDistanceEstimator:
    """Energy-based distance estimator implementing the `Predictor`
    protocol."""

    # Filter design ---------------------------------------------------
    hp_cutoff_hz: float = 0.2
    lp_cutoff_hz: float | None = 5.0   # None disables LP smoothing
    filter_order: int = 2

    # Motion gate -----------------------------------------------------
    motion_window_s: float = 0.5
    motion_enter_g: float = 0.01
    motion_exit_g: float = 0.005
    motion_exit_hold_s: float = 2.0
    use_motion_gate: bool = True

    # Model features --------------------------------------------------
    features: tuple[str, ...] = ("E_motion_g2s",)
    include_intercept: bool = True
    ridge_alpha: float = 1e-3

    # Fitted state ----------------------------------------------------
    _coeffs: np.ndarray | None = field(default=None, init=False, repr=False)
    _feature_idx: tuple[int, ...] = field(init=False, default=())
    _fit_diag: dict = field(default_factory=dict, init=False, repr=False)

    def __post_init__(self) -> None:
        name_to_idx = {n: i for i, n in enumerate(ENERGY_FEATURE_NAMES)}
        unknown = [f for f in self.features if f not in name_to_idx]
        if unknown:
            raise ValueError(
                f"unknown feature(s): {unknown}. "
                f"Available: {ENERGY_FEATURE_NAMES}"
            )
        self._feature_idx = tuple(name_to_idx[f] for f in self.features)

    # ---- Per-run processing ----------------------------------------

    def _process_run(self, run: Run) -> dict:
        fs = run.derived_rate_hz or run.header.sample_rate_hz or 100.0
        hp = butterworth_hp(self.hp_cutoff_hz, fs=fs, order=self.filter_order)
        lp = (butterworth_lp(self.lp_cutoff_hz, fs=fs, order=self.filter_order)
              if self.lp_cutoff_hz is not None else None)
        gate = MotionGate(
            fs=fs,
            window_samples=max(2, int(round(self.motion_window_s * fs))),
            enter_threshold=self.motion_enter_g,
            exit_threshold=self.motion_exit_g,
            exit_hold_samples=max(1, int(round(self.motion_exit_hold_s * fs))),
        )
        mag_g = run.accel_mag_g
        hp.init_steady(float(mag_g[0]) - 1.0)
        dt = 1.0 / fs
        energy_total = 0.0
        energy_motion = 0.0
        for i in range(len(mag_g)):
            v_hp = hp.step(mag_g[i] - 1.0)
            v_filt = lp.step(v_hp) if lp is not None else v_hp
            in_motion = gate.step(v_hp)
            sq = v_filt * v_filt * dt
            energy_total += sq
            if in_motion:
                energy_motion += sq
        return {
            "fs": fs,
            "energy_total_g2s": energy_total,
            "energy_motion_g2s": energy_motion,
            "motion_duration_s": gate.motion_duration_s,
            "motion_fraction": gate.motion_fraction,
            "total_duration_s": gate.total_duration_s,
        }

    def _features_from(self, res: dict) -> np.ndarray:
        E = (res["energy_motion_g2s"] if self.use_motion_gate
             else res["energy_total_g2s"])
        T = res["motion_duration_s"]
        full = np.array([E, T], dtype=np.float64)
        return full[list(self._feature_idx)]

    def _row(self, feats: np.ndarray) -> np.ndarray:
        return (np.concatenate([[1.0], feats]) if self.include_intercept
                else feats)

    # ---- Predictor interface ---------------------------------------

    def fit(self, train_runs: Sequence[Run]) -> None:
        X_rows = []
        y = []
        for r in train_runs:
            res = self._process_run(r)
            X_rows.append(self._row(self._features_from(res)))
            y.append(r.actual_distance_ft)
        X = np.asarray(X_rows)
        y = np.asarray(y, dtype=np.float64)
        A = X.T @ X + self.ridge_alpha * np.eye(X.shape[1])
        b = X.T @ y
        self._coeffs = np.linalg.solve(A, b)

        residuals = y - X @ self._coeffs
        ss_res = float(np.sum(residuals ** 2))
        ss_tot = float(np.sum((y - y.mean()) ** 2))
        self._fit_diag = {
            "n_runs": int(len(y)),
            "r_squared_train": (1.0 - ss_res / ss_tot
                                if ss_tot > 0 else float("nan")),
            "cond_number": float(np.linalg.cond(X)),
            "residuals_ft": residuals.tolist(),
            "coeffs": self._coeffs.tolist(),
        }

    def predict(self, run: Run) -> Prediction:
        if self._coeffs is None:
            raise RuntimeError("EnergyDistanceEstimator.predict() before fit()")
        res = self._process_run(run)
        feats = self._features_from(res)
        row = self._row(feats)
        distance_ft = max(float(row @ self._coeffs), 0.0)
        return Prediction(
            distance_ft=distance_ft,
            step_count=None,
            diagnostics={
                "fs_hz": res["fs"],
                "energy_total_g2s": res["energy_total_g2s"],
                "energy_motion_g2s": res["energy_motion_g2s"],
                "motion_duration_s": res["motion_duration_s"],
                "motion_fraction": res["motion_fraction"],
                "total_duration_s": res["total_duration_s"],
                "formula": self.formula,
            },
        )

    # ---- Introspection ---------------------------------------------

    @property
    def formula(self) -> str:
        if self._coeffs is None:
            return "<not fit>"
        c = self._coeffs
        parts: list[str] = []
        offset = 0
        if self.include_intercept:
            parts.append(f"{c[0]:+.3f}")
            offset = 1
        for i, name in enumerate(self.features):
            parts.append(f"{c[offset + i]:+.5f}·{name}")
        return "distance_ft = " + " ".join(parts)

    def fit_diagnostics(self) -> dict:
        return dict(self._fit_diag) if self._coeffs is not None else {}
