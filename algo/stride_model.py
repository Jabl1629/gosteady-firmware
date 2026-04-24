"""Multi-feature stride regression.

Fits a linear model `stride_ft = c0 + c·features` that, when summed
over the detected peaks of a run, reproduces the run's total actual
distance. The training target is per-run total distance (not per-step
stride), which lets us train on all 8 walk runs rather than only the
7 with manual step counts.

Mathematical setup
------------------
Per run r with peaks {p_{r,1}, ..., p_{r,N_r}}, feature vectors
f_{r,i} ∈ R^K, the estimated distance is

    D̂_r = Σ_i (c0 + c · f_{r,i})
         = c0 · N_r + c · Σ_i f_{r,i}

Define per-run aggregate X_r = (N_r, Σ f_{r,1}, Σ f_{r,2}, ...)
∈ R^(K+1). Then D̂_r = X_r · [c0, c1, ..., cK], and we fit against
measured D_r by OLS (with a small L2 ridge term for conditioning):

    [c0, c] = (XᵀX + α I)⁻¹ Xᵀ D

Why per-run rather than per-step
--------------------------------
- `actual_distance_ft` is filled for every walk; `manual_step_count`
  isn't (missing on s_curve + stumble + stationary).
- The model sees sums, so detector miscounts are absorbed as stride
  error rather than silently breaking the regression.
- Production inference has no access to manual step counts — training
  on the same information the field algorithm sees is more faithful.

Feature set
-----------
Default: (amplitude_g, duration_s, energy_g2s) + intercept. Can be
overridden via `features=` to reproduce the prior work's single-feature
baseline (amplitude only) for A/B comparison.

Production port
---------------
After fit, the model is 4 float32 coefficients (K=3 features + intercept).
Inference is K+1 multiply-adds per peak. `export_c_params()` emits a
ready-to-paste C struct initializer for the firmware's algorithm header.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Iterable, Sequence

import numpy as np

from .step_detector import FEATURE_NAMES, Peak


DEFAULT_FEATURES: tuple[str, ...] = FEATURE_NAMES


@dataclass
class StrideRegression:
    """Linear regression of per-run aggregate peak features onto
    actual distance. Produces per-peak stride estimates at inference."""

    features: tuple[str, ...] = DEFAULT_FEATURES
    include_intercept: bool = True
    ridge_alpha: float = 1e-3

    # Fitted state ---------------------------------------------------
    coeffs: np.ndarray | None = field(default=None, init=False)
    feature_idx: tuple[int, ...] = field(init=False, default=())
    _fit_diag: dict = field(default_factory=dict, init=False)

    def __post_init__(self) -> None:
        name_to_idx = {n: i for i, n in enumerate(FEATURE_NAMES)}
        unknown = [f for f in self.features if f not in name_to_idx]
        if unknown:
            raise ValueError(
                f"unknown feature(s): {unknown}. "
                f"Available: {FEATURE_NAMES}"
            )
        self.feature_idx = tuple(name_to_idx[f] for f in self.features)

    # ---- Design matrix construction --------------------------------

    def _run_row(self, peaks: Sequence[Peak]) -> np.ndarray:
        """Per-run row for the design matrix: `[n_peaks, Σ f_1, ...]`
        if intercept, else `[Σ f_1, ...]`."""
        k = len(self.features)
        if not peaks:
            row_feats = np.zeros(k, dtype=np.float64)
            n = 0
        else:
            fmat = np.stack([p.as_feature_vector() for p in peaks], axis=0)
            row_feats = fmat[:, list(self.feature_idx)].sum(axis=0)
            n = len(peaks)
        if self.include_intercept:
            return np.concatenate([[float(n)], row_feats])
        return row_feats

    def _design_matrix(
        self, run_peaks: Sequence[Sequence[Peak]]
    ) -> np.ndarray:
        return np.stack([self._run_row(p) for p in run_peaks], axis=0)

    # ---- Fit / predict ---------------------------------------------

    def fit(
        self,
        run_peaks: Sequence[Sequence[Peak]],
        distances_ft: Sequence[float],
    ) -> None:
        """Fit coefficients. `run_peaks[i]` are the peaks detected in
        run i; `distances_ft[i]` is that run's `actual_distance_ft`."""
        if len(run_peaks) != len(distances_ft):
            raise ValueError(
                f"run_peaks ({len(run_peaks)}) and distances_ft "
                f"({len(distances_ft)}) length mismatch"
            )
        X = self._design_matrix(run_peaks)
        y = np.asarray(distances_ft, dtype=np.float64)
        # Ridge-regularized normal equations.
        n_cols = X.shape[1]
        A = X.T @ X + self.ridge_alpha * np.eye(n_cols)
        b = X.T @ y
        self.coeffs = np.linalg.solve(A, b)

        # Diagnostics for the report.
        residuals = y - X @ self.coeffs
        ss_res = float(np.sum(residuals ** 2))
        ss_tot = float(np.sum((y - y.mean()) ** 2))
        r_squared = 1.0 - ss_res / ss_tot if ss_tot > 0 else float("nan")
        self._fit_diag = {
            "n_runs": int(len(y)),
            "r_squared_train": r_squared,
            "residuals_ft": residuals.tolist(),
            "cond_number": float(np.linalg.cond(X)),
            "distance_mean": float(y.mean()),
        }

    def _require_fit(self) -> np.ndarray:
        if self.coeffs is None:
            raise RuntimeError("StrideRegression not fit yet")
        return self.coeffs

    def predict_stride(self, peak: Peak) -> float:
        c = self._require_fit()
        feats = peak.as_feature_vector()[list(self.feature_idx)]
        if self.include_intercept:
            return float(c[0] + np.dot(c[1:], feats))
        return float(np.dot(c, feats))

    def predict_distance(self, peaks: Iterable[Peak]) -> float:
        c = self._require_fit()
        peaks = list(peaks)
        if not peaks:
            return 0.0
        fmat = np.stack([p.as_feature_vector() for p in peaks], axis=0)
        fmat = fmat[:, list(self.feature_idx)]
        if self.include_intercept:
            return float(c[0] * len(peaks) + (fmat @ c[1:]).sum())
        return float((fmat @ c).sum())

    # ---- Reporting / export ----------------------------------------

    def coefficients_as_dict(self) -> dict[str, float]:
        c = self._require_fit()
        out: dict[str, float] = {}
        offset = 0
        if self.include_intercept:
            out["intercept_ft"] = float(c[0])
            offset = 1
        for i, name in enumerate(self.features):
            out[f"{name}_coeff"] = float(c[offset + i])
        return out

    def stride_formula(self) -> str:
        """Human-readable formula, e.g.
        `stride = -0.47 + 1.82·amplitude_g + 0.31·duration_s + 0.08·energy_g2s`."""
        c = self._require_fit()
        parts: list[str] = []
        offset = 0
        if self.include_intercept:
            parts.append(f"{c[0]:+.3f}")
            offset = 1
        for i, name in enumerate(self.features):
            parts.append(f"{c[offset + i]:+.3f}·{name}")
        return "stride_ft = " + " ".join(parts)

    def fit_diagnostics(self) -> dict:
        if self.coeffs is None:
            return {}
        return dict(self._fit_diag)
