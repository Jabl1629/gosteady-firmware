"""Evaluation harness for GoSteady distance estimators.

Separates the *how we measure it* from the *how we compute it*. Any
distance estimator is just a `Predictor` — a `.fit(train_runs)` hook
(no-op for hand-tuned models) and a `.predict(run) -> Prediction` call.

Metrics (per-run and aggregate):

- Distance error
    MAE_ft = mean |pred - truth|
    MAPE   = mean |pred - truth| / truth
    Worst  = max |pred - truth| / truth (single worst run)
- Step-count error (on the subset with manual_step_count)
    MAE_steps = mean |pred_steps - truth_steps|

Robustness gates (GOSTEADY_CONTEXT.md §M9 answer 3):

- stationary_baseline: predicted distance must be small (default <1 ft).
- stumble: predicted distance must not blow up (default <3× actual).
  Fails logged loudly, don't raise — a model can be "wrong but useful"
  and we want the number in front of us, not swallowed.

LOO cross-validation is the primary validation protocol for v1. With
~7 training walks it's the only honest split; k-fold would leak across
near-identical repeated normal runs. `loocv()` iterates every walk as
the held-out fold, refits from scratch on the remaining (N-1), and
returns one aggregate report plus per-fold predictions.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from typing import Callable, Protocol

import numpy as np

from .data_loader import Run, iter_walks


# ---------------------------------------------------------------------
# Predictor protocol + Prediction result type
# ---------------------------------------------------------------------


@dataclass
class Prediction:
    """What every estimator returns. Add fields here, not in subclasses,
    so downstream reports can rely on a single shape."""

    distance_ft: float
    step_count: int | None = None
    # Free-form per-prediction diagnostics (peak timings, filter state,
    # intermediate energies, etc.) — consumed by notebooks/plots, not
    # by the aggregate metrics.
    diagnostics: dict = field(default_factory=dict)


class Predictor(Protocol):
    """Every GoSteady estimator implements this two-method interface.

    - `fit(train_runs)` must be idempotent and stateless w.r.t. prior
      calls (LOO refits from scratch every fold).
    - `predict(run)` must never look at `run.actual_distance_ft` or
      `run.manual_step_count` (prevent self-peeking during LOO).
    """

    def fit(self, train_runs: list[Run]) -> None: ...

    def predict(self, run: Run) -> Prediction: ...


PredictorFactory = Callable[[], Predictor]


# ---------------------------------------------------------------------
# Metrics
# ---------------------------------------------------------------------


@dataclass
class RunResult:
    run: Run
    pred: Prediction

    @property
    def truth_ft(self) -> float:
        return self.run.actual_distance_ft

    @property
    def abs_err_ft(self) -> float:
        return abs(self.pred.distance_ft - self.truth_ft)

    @property
    def pct_err(self) -> float:
        """Signed percent error. NaN if truth is 0 (stationary) — use
        `abs_err_ft` for those."""
        t = self.truth_ft
        if t == 0:
            return float("nan")
        return (self.pred.distance_ft - t) / t

    @property
    def abs_pct_err(self) -> float:
        return abs(self.pct_err)

    @property
    def step_abs_err(self) -> float:
        """NaN if the run has no manual_step_count or the predictor
        returned no step_count."""
        if self.pred.step_count is None:
            return float("nan")
        truth = self.run.manual_step_count
        if not np.isfinite(truth):
            return float("nan")
        return abs(float(self.pred.step_count) - truth)


@dataclass
class AggregateMetrics:
    n_runs: int
    distance_mae_ft: float
    distance_mape: float          # mean absolute percent error, fraction (0.124 = 12.4%)
    distance_worst_pct: float
    distance_rmse_ft: float
    step_mae: float               # NaN if no runs had step counts
    n_with_steps: int


def aggregate(results: list[RunResult]) -> AggregateMetrics:
    """Reduce a list of per-run results to aggregate metrics. Ignores
    runs whose truth distance is 0 for MAPE, which would otherwise
    divide by zero."""
    abs_err = np.array([r.abs_err_ft for r in results], dtype=np.float64)

    pct_err = np.array([r.abs_pct_err for r in results], dtype=np.float64)
    pct_err_valid = pct_err[np.isfinite(pct_err)]

    step_err = np.array([r.step_abs_err for r in results], dtype=np.float64)
    step_err_valid = step_err[np.isfinite(step_err)]

    return AggregateMetrics(
        n_runs=len(results),
        distance_mae_ft=float(np.mean(abs_err)) if len(abs_err) else float("nan"),
        distance_mape=(
            float(np.mean(pct_err_valid)) if len(pct_err_valid) else float("nan")
        ),
        distance_worst_pct=(
            float(np.max(pct_err_valid)) if len(pct_err_valid) else float("nan")
        ),
        distance_rmse_ft=(
            float(np.sqrt(np.mean(abs_err ** 2))) if len(abs_err) else float("nan")
        ),
        step_mae=(
            float(np.mean(step_err_valid)) if len(step_err_valid) else float("nan")
        ),
        n_with_steps=int(len(step_err_valid)),
    )


# ---------------------------------------------------------------------
# Robustness gates
# ---------------------------------------------------------------------


@dataclass
class GateResult:
    run: Run
    pred: Prediction
    passed: bool
    limit_ft: float
    reason: str


def check_robustness_gates(
    runs: list[Run],
    predictor: Predictor,
    stationary_limit_ft: float = 1.0,
    stumble_multiplier: float = 3.0,
) -> list[GateResult]:
    """Run the non-walk runs through the final predictor and check
    behavior is sane. Stationary → predicted distance near zero.
    Stumble → predicted distance not wildly over the actual distance.

    Returns one GateResult per gate run (skipped silently if none exist).
    """
    out: list[GateResult] = []
    for r in runs:
        if r.is_walk:
            continue
        p = predictor.predict(r)
        if r.run_type == "stationary_baseline":
            passed = p.distance_ft <= stationary_limit_ft
            out.append(GateResult(
                run=r, pred=p, passed=passed,
                limit_ft=stationary_limit_ft,
                reason=f"stationary predicted {p.distance_ft:.2f} ft "
                       f"(limit {stationary_limit_ft:.1f} ft)",
            ))
        elif r.run_type == "stumble":
            limit = r.actual_distance_ft * stumble_multiplier
            passed = p.distance_ft <= limit
            out.append(GateResult(
                run=r, pred=p, passed=passed,
                limit_ft=limit,
                reason=f"stumble predicted {p.distance_ft:.2f} ft "
                       f"vs actual {r.actual_distance_ft:.1f} "
                       f"(limit {limit:.1f} ft @ {stumble_multiplier}×)",
            ))
    return out


# ---------------------------------------------------------------------
# LOO harness
# ---------------------------------------------------------------------


@dataclass
class EvalReport:
    per_fold: list[RunResult]          # one per held-out walk
    aggregate: AggregateMetrics
    gates: list[GateResult]

    def to_text(self) -> str:
        lines = []
        lines.append("Per-fold predictions (LOO):")
        lines.append(
            f"  {'run':>3}  {'type':<10}  {'dir':<10}  "
            f"{'truth_ft':>8}  {'pred_ft':>8}  {'err_ft':>7}  "
            f"{'err%':>6}  {'step_true':>9}  {'step_pred':>9}"
        )
        for rr in self.per_fold:
            st = (f"{int(rr.run.manual_step_count)}"
                  if np.isfinite(rr.run.manual_step_count) else "-")
            sp = (f"{rr.pred.step_count}" if rr.pred.step_count is not None else "-")
            err_pct = (f"{100 * rr.pct_err:+.1f}"
                       if np.isfinite(rr.pct_err) else "-")
            lines.append(
                f"  {rr.run.run_idx:>3}  {rr.run.run_type:<10}  "
                f"{rr.run.direction:<10}  "
                f"{rr.truth_ft:>8.2f}  {rr.pred.distance_ft:>8.2f}  "
                f"{rr.pred.distance_ft - rr.truth_ft:>+7.2f}  "
                f"{err_pct:>6}  {st:>9}  {sp:>9}"
            )
        a = self.aggregate
        lines.append("")
        lines.append(f"Aggregate across {a.n_runs} LOO folds:")
        lines.append(f"  distance MAE   = {a.distance_mae_ft:.2f} ft")
        lines.append(f"  distance RMSE  = {a.distance_rmse_ft:.2f} ft")
        lines.append(f"  distance MAPE  = {100 * a.distance_mape:.1f}%")
        lines.append(f"  distance worst = {100 * a.distance_worst_pct:.1f}%")
        if a.n_with_steps > 0:
            lines.append(
                f"  step MAE       = {a.step_mae:.2f} steps "
                f"({a.n_with_steps}/{a.n_runs} folds had ground truth)"
            )
        if self.gates:
            lines.append("")
            lines.append("Robustness gates:")
            for g in self.gates:
                mark = "PASS" if g.passed else "FAIL"
                lines.append(f"  [{mark}] {g.run.run_type:<20} — {g.reason}")
        return "\n".join(lines)


def loocv(
    runs: list[Run],
    predictor_factory: PredictorFactory,
) -> EvalReport:
    """Leave-one-out cross-validation across all *walk* runs with
    finite positive `actual_distance_ft`. Robustness-gate runs
    (stationary / stumble) pass through a final predictor fit on all
    walks — they're never held out, because there's nothing about them
    to train against.
    """
    walks = list(iter_walks(runs))
    if len(walks) < 2:
        raise ValueError(f"need >=2 walk runs for LOO; got {len(walks)}")

    per_fold: list[RunResult] = []
    for i, held_out in enumerate(walks):
        train = [r for j, r in enumerate(walks) if j != i]
        model = predictor_factory()
        model.fit(train)
        pred = model.predict(held_out)
        per_fold.append(RunResult(run=held_out, pred=pred))

    final = predictor_factory()
    final.fit(walks)
    gates = check_robustness_gates(runs, final)

    return EvalReport(
        per_fold=per_fold,
        aggregate=aggregate(per_fold),
        gates=gates,
    )
