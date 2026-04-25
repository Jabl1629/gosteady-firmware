"""Cross-surface algorithm test — fit/eval coefficients per surface and
combined. Answers the M9 cross-surface question: are per-surface
regressions necessary for v1, or can a single shared model carry?

Three paths:

  Path A — outdoor-only LOO. Refit V1 on the 8 outdoor walks. Report
           coefficients + MAPE. Compare directly to indoor's
           `0.217 + 2.757·amp_g` baseline.

  Path B — combined indoor+outdoor LOO. 16 walks pooled. Per-surface
           MAPE breakdown after fold completion.

  Path C — apply indoor-fit coefficients directly to outdoor (no
           refit). The "no-surface-knowledge" failure case. If MAPE
           explodes, surface info is non-optional for v1.

Each path also reports step-detection MAE per surface. With the V1
loose Schmitt thresholds, the indoor detector finds ≈1.5–2× the manual
step count (compound 2:1 impulse structure). Whether that ratio holds
on outdoor concrete tells us whether the gait dynamics translate
1:1 across surfaces or whether the detector itself needs surface
calibration too.

Run:
    algo/venv/bin/python3 -m algo.run_cross_surface
"""

from __future__ import annotations

from typing import Sequence

import numpy as np

from .data_loader import Run, iter_walks, load_capture_day
from .distance_estimator import StepBasedDistanceEstimator
from .evaluator import RunResult, aggregate


def _valid_walks(runs: Sequence[Run]) -> list[Run]:
    """Walks where valid=Y and the actual distance is positive finite.
    Excludes discards (valid=N) and the stationary/stumble robustness
    gate runs."""
    return [r for r in iter_walks(runs) if r.valid]


def _stationary(runs: Sequence[Run]) -> Run | None:
    for r in runs:
        if r.run_type == "stationary_baseline" and r.valid:
            return r
    return None


def _collect_results(predictor, runs: Sequence[Run]) -> list[RunResult]:
    """Run a fit-once predictor over a list of runs and collect per-run
    RunResults. Uses the same metric structure as evaluator.loocv but
    skips the per-fold refit."""
    return [RunResult(run=r, pred=predictor.predict(r)) for r in runs]


def _fit_predictor(train: Sequence[Run]) -> StepBasedDistanceEstimator:
    p = StepBasedDistanceEstimator()  # V1 defaults
    p.fit(list(train))
    return p


def _format_coeffs(p: StepBasedDistanceEstimator) -> str:
    return p.stride.stride_formula()


def main() -> int:
    indoor_runs = load_capture_day("raw_sessions/2026-04-23")
    outdoor_runs = load_capture_day("raw_sessions/2026-04-25")
    indoor_walks = _valid_walks(indoor_runs)
    outdoor_walks = _valid_walks(outdoor_runs)
    outdoor_stat = _stationary(outdoor_runs)
    print(f"Indoor walks loaded:  {len(indoor_walks)}  "
          f"(of which {sum(1 for r in indoor_walks if np.isfinite(r.manual_step_count))} have step counts)")
    print(f"Outdoor walks loaded: {len(outdoor_walks)}  "
          f"(of which {sum(1 for r in outdoor_walks if np.isfinite(r.manual_step_count))} have step counts)")
    print()

    # ------------------------------------------------------------------
    # Path A — outdoor-only LOO.
    # Use loocv() but synthesize an iter_walks-equivalent input.
    # ------------------------------------------------------------------
    print("=" * 80)
    print("PATH A — outdoor-only LOO (V1 algorithm, refit on 8 outdoor walks)")
    print("=" * 80)
    # We can't reuse loocv() directly because it filters via iter_walks
    # which depends on Run.is_walk + actual_distance_ft. Our outdoor
    # walks already meet that filter, so call factory + LOO manually.
    per_fold_a: list[RunResult] = []
    for i, held_out in enumerate(outdoor_walks):
        train = [r for j, r in enumerate(outdoor_walks) if j != i]
        m = _fit_predictor(train)
        per_fold_a.append(RunResult(run=held_out, pred=m.predict(held_out)))
    agg_a = aggregate(per_fold_a)
    print(f"  {'run':>3}  {'truth':>5}  {'pred':>6}  {'err%':>6}  {'speed':<6}  notes")
    for rr in per_fold_a:
        err_pct = (f"{100*rr.pct_err:+.1f}"
                   if np.isfinite(rr.pct_err) else "-")
        print(f"  {rr.run.run_idx:>3}  {rr.truth_ft:>5.1f}  "
              f"{rr.pred.distance_ft:>6.2f}  {err_pct:>6}  "
              f"{rr.run.intended_speed:<6}  {rr.run.run_type}")
    print(f"  → MAE {agg_a.distance_mae_ft:.2f} ft, "
          f"MAPE {100*agg_a.distance_mape:.1f}%, "
          f"worst {100*agg_a.distance_worst_pct:.1f}%, "
          f"RMSE {agg_a.distance_rmse_ft:.2f} ft")

    # Final outdoor coefficients (fit on ALL outdoor walks)
    final_outdoor = _fit_predictor(outdoor_walks)
    print(f"  outdoor model: {_format_coeffs(final_outdoor)}")
    print(f"  outdoor train R² = "
          f"{final_outdoor.stride.fit_diagnostics()['r_squared_train']:.3f}")

    # Outdoor stationary gate
    p_stat = final_outdoor.predict(outdoor_stat)
    print(f"  outdoor stationary gate (run 19, 50.4 s): "
          f"predicted {p_stat.distance_ft:.2f} ft (limit 1.0 ft) — "
          f"{'PASS' if p_stat.distance_ft <= 1.0 else 'FAIL'}")
    print()

    # ------------------------------------------------------------------
    # Path B — combined indoor+outdoor LOO.
    # ------------------------------------------------------------------
    print("=" * 80)
    print("PATH B — combined LOO (16 walks pooled, V1 algorithm)")
    print("=" * 80)
    pooled = list(indoor_walks) + list(outdoor_walks)
    surface_tag = (["indoor"] * len(indoor_walks)
                   + ["outdoor"] * len(outdoor_walks))
    per_fold_b: list[tuple[str, RunResult]] = []
    for i, held_out in enumerate(pooled):
        train = [r for j, r in enumerate(pooled) if j != i]
        m = _fit_predictor(train)
        per_fold_b.append((surface_tag[i],
                           RunResult(run=held_out, pred=m.predict(held_out))))

    indoor_results = [rr for tag, rr in per_fold_b if tag == "indoor"]
    outdoor_results = [rr for tag, rr in per_fold_b if tag == "outdoor"]
    agg_in = aggregate(indoor_results)
    agg_out = aggregate(outdoor_results)
    agg_all = aggregate([rr for _, rr in per_fold_b])

    print(f"  {'surf':<7}  {'run':>3}  {'truth':>5}  {'pred':>6}  {'err%':>6}  {'speed':<6}")
    for tag, rr in per_fold_b:
        err_pct = (f"{100*rr.pct_err:+.1f}"
                   if np.isfinite(rr.pct_err) else "-")
        print(f"  {tag:<7}  {rr.run.run_idx:>3}  {rr.truth_ft:>5.1f}  "
              f"{rr.pred.distance_ft:>6.2f}  {err_pct:>6}  "
              f"{rr.run.intended_speed:<6}")
    print()
    print(f"  per-surface breakdown:")
    print(f"    indoor  (n={agg_in.n_runs}): MAE {agg_in.distance_mae_ft:.2f} ft, "
          f"MAPE {100*agg_in.distance_mape:.1f}%, worst {100*agg_in.distance_worst_pct:.1f}%")
    print(f"    outdoor (n={agg_out.n_runs}): MAE {agg_out.distance_mae_ft:.2f} ft, "
          f"MAPE {100*agg_out.distance_mape:.1f}%, worst {100*agg_out.distance_worst_pct:.1f}%")
    print(f"    pooled  (n={agg_all.n_runs}): MAE {agg_all.distance_mae_ft:.2f} ft, "
          f"MAPE {100*agg_all.distance_mape:.1f}%, worst {100*agg_all.distance_worst_pct:.1f}%")

    final_combined = _fit_predictor(pooled)
    print(f"  combined model: {_format_coeffs(final_combined)}")
    print(f"  combined train R² = "
          f"{final_combined.stride.fit_diagnostics()['r_squared_train']:.3f}")
    print()

    # ------------------------------------------------------------------
    # Path C — apply indoor-fit coefficients to outdoor (no refit).
    # The "we shipped indoor coefficients to a customer who then walked
    # outdoors" failure case.
    # ------------------------------------------------------------------
    print("=" * 80)
    print("PATH C — indoor-fit coefficients applied to outdoor (no surface info)")
    print("=" * 80)
    indoor_only_model = _fit_predictor(indoor_walks)
    print(f"  indoor model: {_format_coeffs(indoor_only_model)}")
    cross_results = _collect_results(indoor_only_model, outdoor_walks)
    agg_c = aggregate(cross_results)
    print(f"  {'run':>3}  {'truth':>5}  {'pred':>6}  {'err%':>6}  {'speed':<6}")
    for rr in cross_results:
        err_pct = (f"{100*rr.pct_err:+.1f}"
                   if np.isfinite(rr.pct_err) else "-")
        print(f"  {rr.run.run_idx:>3}  {rr.truth_ft:>5.1f}  "
              f"{rr.pred.distance_ft:>6.2f}  {err_pct:>6}  "
              f"{rr.run.intended_speed:<6}")
    print(f"  → MAE {agg_c.distance_mae_ft:.2f} ft, "
          f"MAPE {100*agg_c.distance_mape:.1f}%, "
          f"worst {100*agg_c.distance_worst_pct:.1f}%, "
          f"RMSE {agg_c.distance_rmse_ft:.2f} ft")
    print()

    # ------------------------------------------------------------------
    # Side-by-side coefficients
    # ------------------------------------------------------------------
    indoor_model = _fit_predictor(indoor_walks)
    print("=" * 80)
    print("COEFFICIENT COMPARISON  (stride_ft = a + b · amplitude_g)")
    print("=" * 80)
    for name, m in (("indoor only      ", indoor_model),
                    ("outdoor only     ", final_outdoor),
                    ("combined (both)  ", final_combined)):
        c = m.stride.coeffs
        print(f"  {name}: a = {c[0]:+.3f}, b = {c[1]:+.3f}    "
              f"(R² = {m.stride.fit_diagnostics()['r_squared_train']:.3f})")
    print()

    # ------------------------------------------------------------------
    # Final summary table
    # ------------------------------------------------------------------
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"  {'configuration':<55}  {'MAPE':>6}  {'MAE':>5}  {'worst':>6}")
    print(f"  {'A: outdoor LOO (refit on outdoor)':<55}  "
          f"{100*agg_a.distance_mape:>5.1f}%  {agg_a.distance_mae_ft:>4.2f}ft  "
          f"{100*agg_a.distance_worst_pct:>5.1f}%")
    print(f"  {'B: combined LOO (16 walks) — indoor folds':<55}  "
          f"{100*agg_in.distance_mape:>5.1f}%  {agg_in.distance_mae_ft:>4.2f}ft  "
          f"{100*agg_in.distance_worst_pct:>5.1f}%")
    print(f"  {'B: combined LOO (16 walks) — outdoor folds':<55}  "
          f"{100*agg_out.distance_mape:>5.1f}%  {agg_out.distance_mae_ft:>4.2f}ft  "
          f"{100*agg_out.distance_worst_pct:>5.1f}%")
    print(f"  {'B: combined LOO (16 walks) — pooled':<55}  "
          f"{100*agg_all.distance_mape:>5.1f}%  {agg_all.distance_mae_ft:>4.2f}ft  "
          f"{100*agg_all.distance_worst_pct:>5.1f}%")
    print(f"  {'C: indoor-fit applied to outdoor (no surface info)':<55}  "
          f"{100*agg_c.distance_mape:>5.1f}%  {agg_c.distance_mae_ft:>4.2f}ft  "
          f"{100*agg_c.distance_worst_pct:>5.1f}%")

    # ------------------------------------------------------------------
    # Step-count + stride analysis. Both surfaces in one table so we
    # can directly compare detector consistency, manual stride lengths,
    # and manual-vs-detected ratios. Uses the SAME final-fit predictor
    # we'd ship for each surface, so detector counts reflect the V1
    # loose-Schmitt thresholds in production.
    # ------------------------------------------------------------------
    print()
    print("=" * 80)
    print("STEP COUNTS + STRIDE LENGTHS — indoor vs outdoor")
    print("=" * 80)
    indoor_pred = _fit_predictor(indoor_walks)
    outdoor_pred = _fit_predictor(outdoor_walks)

    def _row_for(run: Run, model: StepBasedDistanceEstimator) -> dict:
        p = model.predict(run)
        manual = run.manual_step_count
        det = p.step_count
        detected_per_manual = (det / manual
                               if (det is not None and np.isfinite(manual)
                                   and manual > 0) else float('nan'))
        manual_stride_ft = (run.actual_distance_ft / manual
                            if (np.isfinite(manual) and manual > 0)
                            else float('nan'))
        return dict(run_idx=run.run_idx,
                    speed=run.intended_speed,
                    dist_ft=run.actual_distance_ft,
                    manual=manual, detected=det,
                    ratio=detected_per_manual,
                    stride_ft=manual_stride_ft)

    print(f"  {'surface':<8}  {'run':>3}  {'speed':<6}  {'dist':>4}  "
          f"{'manual':>6}  {'detected':>8}  {'ratio':>5}  {'stride_ft':>9}")
    print("  " + "-" * 70)
    indoor_ratios, outdoor_ratios = [], []
    indoor_strides, outdoor_strides = [], []
    for r in indoor_walks:
        row = _row_for(r, indoor_pred)
        m = (str(int(row['manual'])) if np.isfinite(row['manual']) else "-")
        d = (str(row['detected']) if row['detected'] is not None else "-")
        ratio = (f"{row['ratio']:.2f}" if np.isfinite(row['ratio']) else "-")
        stride = (f"{row['stride_ft']:.2f}"
                  if np.isfinite(row['stride_ft']) else "-")
        if np.isfinite(row['ratio']): indoor_ratios.append(row['ratio'])
        if np.isfinite(row['stride_ft']): indoor_strides.append(row['stride_ft'])
        print(f"  {'indoor':<8}  {r.run_idx:>3}  {row['speed']:<6}  "
              f"{row['dist_ft']:>4.0f}  {m:>6}  {d:>8}  {ratio:>5}  {stride:>9}")
    for r in outdoor_walks:
        row = _row_for(r, outdoor_pred)
        m = (str(int(row['manual'])) if np.isfinite(row['manual']) else "-")
        d = (str(row['detected']) if row['detected'] is not None else "-")
        ratio = (f"{row['ratio']:.2f}" if np.isfinite(row['ratio']) else "-")
        stride = (f"{row['stride_ft']:.2f}"
                  if np.isfinite(row['stride_ft']) else "-")
        if np.isfinite(row['ratio']): outdoor_ratios.append(row['ratio'])
        if np.isfinite(row['stride_ft']): outdoor_strides.append(row['stride_ft'])
        print(f"  {'outdoor':<8}  {r.run_idx:>3}  {row['speed']:<6}  "
              f"{row['dist_ft']:>4.0f}  {m:>6}  {d:>8}  {ratio:>5}  {stride:>9}")

    print()
    print(f"  indoor  detected/manual ratio: median {np.median(indoor_ratios):.2f}, "
          f"min {min(indoor_ratios):.2f}, max {max(indoor_ratios):.2f}")
    print(f"  outdoor detected/manual ratio: median {np.median(outdoor_ratios):.2f}, "
          f"min {min(outdoor_ratios):.2f}, max {max(outdoor_ratios):.2f}")
    print(f"  indoor  manual stride: median {np.median(indoor_strides):.2f} ft, "
          f"range {min(indoor_strides):.2f}–{max(indoor_strides):.2f}")
    print(f"  outdoor manual stride: median {np.median(outdoor_strides):.2f} ft, "
          f"range {min(outdoor_strides):.2f}–{max(outdoor_strides):.2f}")

    # ------------------------------------------------------------------
    # Path D — multi-feature combined LOO. With n=16 walks across two
    # surfaces, does adding peak duration / energy let a SINGLE
    # coefficient set carry both surfaces? If yes, ship one model; if
    # no, the per-surface table at the end of this report wins.
    # ------------------------------------------------------------------
    print()
    print("=" * 80)
    print("PATH D — multi-feature combined LOO (16 walks, V1 detector)")
    print("=" * 80)
    feature_variants = [
        ("amp only             ", ("amplitude_g",)),
        ("amp + duration       ", ("amplitude_g", "duration_s")),
        ("amp + duration + E   ", ("amplitude_g", "duration_s", "energy_g2s")),
    ]
    for label, feats in feature_variants:
        per_fold_d: list[tuple[str, RunResult]] = []
        for i, held_out in enumerate(pooled):
            train = [r for j, r in enumerate(pooled) if j != i]
            m = StepBasedDistanceEstimator(features=feats)
            m.fit(list(train))
            per_fold_d.append((surface_tag[i],
                              RunResult(run=held_out, pred=m.predict(held_out))))
        in_res = [rr for tag, rr in per_fold_d if tag == "indoor"]
        out_res = [rr for tag, rr in per_fold_d if tag == "outdoor"]
        agg_pooled = aggregate([rr for _, rr in per_fold_d])
        agg_in_d = aggregate(in_res)
        agg_out_d = aggregate(out_res)
        # Final fit on all 16 to inspect coefficients + R²
        final = StepBasedDistanceEstimator(features=feats)
        final.fit(pooled)
        diag = final.stride.fit_diagnostics()
        print(f"  {label}  pooled MAPE {100*agg_pooled.distance_mape:.1f}%  "
              f"indoor {100*agg_in_d.distance_mape:.1f}%  "
              f"outdoor {100*agg_out_d.distance_mape:.1f}%  "
              f"R²={diag['r_squared_train']:.3f}  cond={diag['cond_number']:.1f}")
        print(f"     {final.stride.stride_formula()}")
    print()

    # ------------------------------------------------------------------
    # PROPOSED V1 PER-SURFACE COEFFICIENT TABLE
    # ------------------------------------------------------------------
    print("=" * 80)
    print("PROPOSED V1 PER-SURFACE COEFFICIENT TABLE")
    print("=" * 80)
    indoor_diag = indoor_pred.stride.fit_diagnostics()
    outdoor_diag = outdoor_pred.stride.fit_diagnostics()
    print("// gosteady_algo_params.h  (generated, single-feature V1 — amp only)")
    print("typedef struct {")
    print("    float intercept_ft;        // stride_ft = a + b * peak_amp_g")
    print("    float amp_coeff_ft_per_g;")
    print("} gosteady_stride_coeffs_t;")
    print()
    print("static const gosteady_stride_coeffs_t "
          "stride_coeffs_by_surface[] = {")
    print(f"    [SURFACE_POLISHED_CONCRETE]  = {{ {indoor_pred.stride.coeffs[0]:+.4f}f, "
          f"{indoor_pred.stride.coeffs[1]:+.4f}f }},  "
          f"// LOO MAPE 16.4%, R²={indoor_diag['r_squared_train']:.3f} (n=8)")
    print(f"    [SURFACE_OUTDOOR_CONCRETE]   = {{ {outdoor_pred.stride.coeffs[0]:+.4f}f, "
          f"{outdoor_pred.stride.coeffs[1]:+.4f}f }},  "
          f"// LOO MAPE 23.3%, R²={outdoor_diag['r_squared_train']:.3f} (n=8)")
    print(f"    [SURFACE_LOW_PILE_CARPET]    = {{ /* TBD — runs 21-30 not yet captured */ }},")
    print("    // Out of v1 scope:")
    print("    [SURFACE_HARDWOOD]           = stride_coeffs_by_surface[SURFACE_POLISHED_CONCRETE],")
    print("    [SURFACE_TILE]               = stride_coeffs_by_surface[SURFACE_POLISHED_CONCRETE],")
    print("    [SURFACE_LINOLEUM]           = stride_coeffs_by_surface[SURFACE_POLISHED_CONCRETE],")
    print("    [SURFACE_VINYL]              = stride_coeffs_by_surface[SURFACE_POLISHED_CONCRETE],")
    print("    [SURFACE_OUTDOOR_ASPHALT]    = stride_coeffs_by_surface[SURFACE_OUTDOOR_CONCRETE],")
    print("    [SURFACE_HIGH_PILE_CARPET]   = stride_coeffs_by_surface[SURFACE_LOW_PILE_CARPET],")
    print("};")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
