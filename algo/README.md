# GoSteady algorithm dev (M9)

Python development of the step-based distance estimator for the v1 dataset.
Streaming-friendly by construction so the M10 C port is a mechanical
translation, not a redesign.

## Setup

```bash
cd algo
./venv/bin/pip install -r requirements.lock.txt   # reproducible
# or
python3 -m venv venv
./venv/bin/pip install -r requirements.txt        # latest compatible
```

Python 3.14 is what we used to build the venv. Any 3.12+ should work.

## Running

From the repo root (`gosteady-firmware/`):

```bash
algo/venv/bin/python3 -m algo.sanity_check           # smoke-test loader
algo/venv/bin/jupyter lab --notebook-dir=algo/notebooks
```

## Directory layout

```
algo/
├── data_loader.py         # parse .dat + join capture CSV → per-run objects
├── evaluator.py           # metrics + LOO cross-validation harness
├── (later) filters.py step_detector.py stride_model.py distance_estimator.py
├── notebooks/
├── requirements.txt       # loose pins, latest compatible
└── requirements.lock.txt  # exact pip freeze output, for reproducibility
```

## Data source

`raw_sessions/<YYYY-MM-DD>/` — `.dat` session files + `capture_<date>.csv`
annotations + `gosteady_capture_notes_<date>.json` POST-WALK sidecar.
The loader takes the capture date as input and returns one run object
per row of the CSV.
