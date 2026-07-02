# GoSteady Rollator Capture Protocol v1 (DT-1)

> **Status:** ready for DT-2 data collection · **Created:** 2026-07-01 (Phase DT-1,
> portal spec `phase-dt1-rollator-bench-bringup.md`, coord §C49)
> **Companion assets:** `GoSteady_Rollator_Annotations_v1.xlsx` (ground truth),
> `tools/capture_rollator.html` (operator page, 39-run matrix baked in),
> `tools/control.py` (rollator presets, USB fallback), `tools/pull_sessions.py` +
> `tools/ingest_capture.py` (unchanged from walker v1).
> Mirrors the structure of `GoSteady_Capture_Protocol_v1.docx` (walker v1);
> written in Markdown so it diffs/versions in-repo.

## 1. Purpose & scope

Collect the ground-truth IMU dataset for the **rollator wheeled-motion
algorithms** (DT-2): distance, step detection through frame vibration, and
gait speed. The walker cap's lift-and-place impulse detector does not
transfer to rolling motion — this dataset is what the new pipeline trains
and validates against, targeting walker-cap metric parity
(`steps`, `distance_ft`, `active_min`, `gait_speed_fts`) by DT-2 exit.

Wheeled-motion additions vs the walker protocol: explicit **90° turns**,
**brake events** (full stop; drag), **frame loading** (heavy lean), and a
**seated-rest baseline** (parking brake set, user seated on the rollator) —
the seated case is a realistic rollator use pattern with no walker analog.

## 2. Prerequisites

- Bench unit `GS9999999981` flashed with the **capture-day image**
  (`build_rollator_bench`: no cloud, motion auto-start OFF, manual
  sessions exempt from stillness auto-stop — coord §C50). Do NOT run the
  protocol on the cloud build: it auto-prunes each `.dat` seconds after
  publish, and motion auto-start squats sessions between runs.
- A real 4-wheel rollator with functional hand brakes + parking brake.
- Device mounted on the **accessory platform position** (cupholder mount
  point, frame midline). Mounting method for the bare Thingy:91 X is an
  open item (spec Open Question) — whatever is used, keep it rigid
  (no foam/dampening) and IDENTICAL across all 39 runs; record it in
  `walker_model` free-text if it changes.
- Course: tape measure, painter's tape start/end lines, 2 cones for
  s-curve, clear 40 ft straight for the long runs.
- Operator page `tools/capture_rollator.html` via Chrome Web-Bluetooth
  (BLE_ENABLED=1 in the bridge Config.txt), or `tools/control.py` over
  USB uart1 as fallback.

## 3. Annotation schema

Identical 34-column schema to walker v1 (13 FIRMWARE + 12 PRE-WALK +
7 POST-WALK + 2 DERIVED) — see the workbook's Legend sheet. Rollator
specifics:

- `walker_type=rollator_4wheel`, `cap_type=frame_mount`,
  `mount_config=accessory_platform` (constant across all runs).
- `manual_step_count`: **leave blank** — no ground-truth step count in v0.
  (If a helper counts steps on video later, backfill.)
- `actual_distance_ft`: measure to where the **front wheels** stop.
- `events` free text: `brake@mm:ss`, `sit@mm:ss` join the walker vocab
  (`stumble`, `pickup`, `setdown`, `obstacle`, `interruption`).

## 4. Run matrix — 39 runs (3 surfaces × 13)

Surfaces: **indoor polished concrete** (runs 1–13), **outdoor concrete
sidewalk** (14–26), **indoor low-pile carpet** (27–39). Per-surface
template (distances: long = 40 ft outdoor/concrete, 30 ft carpet; s-curve
= 20 ft concrete/sidewalk, 15 ft carpet):

| # | Course | Dist (ft) | Speed | Direction | run_type | Notes |
|---|--------|-----------|-------|-----------|----------|-------|
| 1 | straight | 10 | normal | straight | normal | warmup |
| 2 | straight | 20 | slow | straight | normal | |
| 3 | straight | 20 | normal | straight | normal | |
| 4 | straight | 20 | fast | straight | normal | |
| 5 | straight | 20 | normal | straight | normal | variance repeat |
| 6 | long | 40/30 | normal | straight | normal | long distance |
| 7 | curve | 20/15 | normal | s_curve | normal | weave 2 cones |
| 8 | turn | 20 | normal | turn_left | turn_test | 90° left turn mid-course |
| 9 | turn | 20 | normal | turn_right | turn_test | 90° right turn mid-course |
| 10 | straight | 20 | normal | straight | brake_stop | full brake squeeze to stop ~mid-course, then resume; note `brake@mm:ss` |
| 11 | straight | 20 | normal | straight | heavy_lean | max comfortable body weight on the frame throughout |
| 12 | straight | 0 | slow | straight | stationary_baseline | 30 s parked, untouched, parking brake OFF |
| 13 | straight | 0 | slow | straight | park_brake_seated | parking brake ON, user seated on the rollator seat 30 s; note `sit@mm:ss` |

`brake_drag` (partial-brake walking) is in the vocabulary but not in the
v1 matrix — add ad-hoc runs if the brake_stop data shows the signature
matters.

## 5. Per-run procedure (mirrors walker v1 §7)

1. Identify the run in the matrix (the capture page tracks completion).
2. Tape-measure the course; painter's tape at start/end; cones for
   s-curve; mark the turn apex for runs 8–9.
3. Position the rollator **front wheels behind the start line** (constant
   convention — walker v1 used front feet).
4. Start the session from the capture page (sends the 12-field START) or
   `control.py start-preset …`. Wait for ack + 1 s stationary settling.
5. Execute the run. Adversarial runs: one event ~halfway; note the
   timestamp for the `events` column. Baselines: hands off (run 12) /
   seated (run 13) for the full 30 s.
6. Stop at the end line + 1 s settling → STOP.
7. POST-WALK popup: Good / Discard / Notes. Fill `actual_distance_ft`
   (front-wheel position), `events`, `subjective_speed`.
8. ~30 s idle between runs. Mishap → `valid=N` + `discard_reason`, re-run
   the same row.

## 6. Post-session procedure (per surface block; mirrors walker v1 §8)

1. USB-pull the block's sessions:
   `python3 tools/pull_sessions.py --out raw_sessions/<date>/`.
2. Verify file count == completed run count.
3. Export notes JSON from the capture page
   (`gosteady_rollator_capture_notes_<date>.json`).
4. Ingest: `python3 tools/ingest_capture.py raw_sessions/<date> <notes.json>`
   → CSV; paste/append into the `Captures` sheet of
   `GoSteady_Rollator_Annotations_v1.xlsx`.
5. Sanity: non-zero `sample_count`, zero `flash_errors`, `duration_s`
   plausible vs run length, header enums decode (`read_session.py` exits 0).
6. Commit `raw_sessions/<date>` + the workbook.

**Cloud-build caveat (differs from walker bench history):** on the
cloud-enabled `rol-0.1.0-bench` build, the `.dat` is auto-pruned after the
activity uplink PUBACKs, and a boot-time orphan sweep deletes all `.dat`
at every boot. **Pull sessions over uart1 before rebooting**, and pull
promptly after each block. If a full capture day is planned away from
USB, consider a snippets-off, cloud-off bench build (plain `prj.conf` +
product flag) — no prune, no uplinks, no SIM spend.

## 7. Deferred (v1.5+)

- High-pile carpet / asphalt / ramps + thresholds (vocab already covers
  asphalt).
- `brake_drag` matrix runs (see §4 note).
- Multiple subjects + real user weights (S00 operator-only in v1, same as
  walker v1).
- Video-synced ground-truth step counts (backfill `manual_step_count`).
- Second mount position (frame side-rail) if accessory-platform placement
  proves noisy.
