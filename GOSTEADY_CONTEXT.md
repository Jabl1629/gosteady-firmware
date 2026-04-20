# GoSteady — Firmware Development Context

## What Is GoSteady?

GoSteady is a smart walker cap — a small device that attaches to a walker leg and estimates distance traveled using onboard IMU sensors. The product targets elderly users and physical therapy patients who need to track mobility without wearing anything on their body.

The prototype development platform is the **Nordic Thingy:91 X** dev board, which will eventually be replaced by a custom PCB for production.

---

## Hardware: Thingy:91 X

### Key ICs and Their Roles

| IC | Function | Interface | GoSteady Relevance |
|---|---|---|---|
| **nRF9151** | Main application processor + LTE-M/NB-IoT cellular | — | Runs our firmware, has TF-M in secure partition |
| **nRF5340** | BLE host + network core | — | BLE connectivity to phone app (future) |
| **nRF7002** | Wi-Fi 6 companion | — | Not used in v1 |
| **BMI270** | 6-axis IMU (accel + gyro) | SPI (`spi3` @ CS `P0.10`, 10 MHz max, IRQ `P0.06` active-low) | **Primary sensor** — step detection + stride estimation |
| **ADXL367** | Ultra-low-power 3-axis accelerometer | I²C (`i2c2` @ `0x1d`, INT1 `P0.11` active-high) | Secondary/backup accel, wake-on-motion trigger |
| **64 MB QSPI Flash** | External storage | QSPI | IMU data logging for offline analysis |

### Board Target

```
thingy91x/nrf9151/ns
```

- `ns` = non-secure — our application runs in the non-secure partition, TF-M handles the secure side
- This is the correct target for all `west build` commands

### Debug/Flash Interface

The Thingy:91 X has **no onboard debugger**. Flashing requires an external SWD probe.

- **Debugger:** external **SEGGER J-Link EDU Mini V2** (S/N `802006700`) connected via 10-pin SWD Tag-Connect to the Thingy:91 X debug header.
- **SW2 switch:** selects which chip SWD targets. `SW2 = nRF91` for application flashing (our default). Flip to `nRF53` only when reflashing the connectivity bridge, then flip back. Wrong position = SWD talks to the wrong chip and silently wastes time.

**USB enumeration:** comes from the **nRF5340 connectivity bridge** (not a J-Link OB, not direct nRF9151 USB). The bridge runs `thingy91x_nrf53_connectivity_bridge_v3.0.1` and exposes:
1. Two CDC-ACM endpoints (`/dev/cu.usbmodem*` on macOS) — one carries the nRF9151 app console, the other is for bridge/modem traces.
2. A Zephyr mass-storage endpoint for DFU drag-and-drop.

The hex-format serial (e.g. `DAE5B718C2209445`) is the bridge's USB serial, not a J-Link OB serial. `802006700` is the external J-Link Mini — these are two separate devices, not two formats of the same ID.

---

## Development Environment

### Toolchain

- **nRF Connect SDK (NCS) v3.2.4** installed at `/opt/nordic/ncs/v3.2.4`
- **Toolchain** at `/opt/nordic/ncs/toolchains/185bb0e3b6/`
- **NCS Python 3.12** at `/opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3` (has intelhex, west, and all dependencies)
- **Zephyr RTOS** is the underlying OS (part of NCS)
- **VS Code + nRF Connect Extension Pack** is the IDE

### Critical PATH Issue (macOS)

Jace's `~/.zshrc` prepends Homebrew Python 3.14 and `~/.local/bin` to PATH. This causes `west` (which is Python-based) to use the wrong Python interpreter, which lacks `intelhex` and other NCS dependencies.

**Fix needed:** Change PATH prepends to appends in `~/.zshrc`:
```bash
# WRONG (current):
export PATH="/opt/homebrew/opt/python@3.14/bin:$PATH"
export PATH="/Users/jaceblackburn/.local/bin:$PATH"

# CORRECT:
export PATH="$PATH:/opt/homebrew/opt/python@3.14/bin"
export PATH="$PATH:/Users/jaceblackburn/.local/bin"
```

**Alternative:** Always use the nRF Connect terminal (VS Code extension → "Open Terminal" or Toolchain Manager → "Open Terminal") which sources the correct NCS environment automatically.

**Manual environment activation** (for any terminal):
```bash
export PATH="/opt/nordic/ncs/toolchains/185bb0e3b6/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_BASE="/opt/nordic/ncs/v3.2.4/zephyr"
```

### Build & Flash Commands

```bash
# Build (with NCS env sourced)
west build -b thingy91x/nrf9151/ns -p auto

# Flash via the external J-Link Mini (SW2 must be in the nRF91 position)
west flash

# Console output: one of the two /dev/cu.usbmodem* CDC endpoints
# exposed by the nRF5340 connectivity bridge. Use screen, minicom,
# or the nRF Terminal in VS Code.
```

### Known Gotchas

Things that have already bitten us at least once. Read before the next hardware bring-up.

- **VS Code build config board must be `thingy91x/nrf9151/ns`.** Easy to accidentally create a build config pointing at `nrf9151dk` — it will build cleanly but produce an image with completely different pin assignments. Symptom: LEDs do the wrong thing, UART is garbage, peripherals don't respond. Sanity check: `build/gosteady-firmware/zephyr/.config` should contain `CONFIG_BOARD="thingy91x"`.
- **`west flash` with the nrfutil runner hits `UICR data is not erasable` on nearly every flash.** The runner only erases sectors the new image touches, and our b0 + MCUboot + TF-M + app layout writes UICR on every rebuild. Fix: `nrfutil device erase --serial-number 802006700` before each `west flash`, or use the nrfjprog runner with `--eraseall` (slower but single-command).
- **APPROTECT + ERASEPROTECT lockout.** If a flashed image turned on readback protection, `nrfjprog --recover` alone fails. On the nRF9151 (single core), chain recover + program in one invocation so no reset re-asserts the lock between them: `nrfjprog -f NRF91 --recover --program build/merged.hex --verify --reset`. (On multi-core nRF5340 targets, recover the network core first with `--coprocessor CP_NETWORK --recover` to clear the interlock.)
- **Board stops enumerating on USB-C → bridge firmware is probably corrupt.** Reflash the connectivity bridge from the local Nordic bundle: `nordic resources/thingy91x_mfw-2.0.4_sdk-3.2.1/img_app_bl/thingy91x_nrf53_connectivity_bridge_v3.0.1.hex`, with **SW2 flipped to nRF53**. This recovery is independent of anything wrong on the nRF9151 side.
- **`cat /dev/cu.usbmodem*` shows a dead console even when the app is logging.** The bridge holds UART RX back until DTR is asserted; plain `cat` doesn't. Use `screen`, `minicom`, `cu`, pyserial, or nRF Terminal — all assert DTR on open. See the pyserial one-liner in the *Build & Flash Commands* section.

> **Serial console status (verified 2026-04-19):**
> - `/dev/cu.usbmodem102` → nRF9151 **uart0** = app console @ 115200 (`<inf> gosteady: heartbeat tick=N` lands here).
> - `/dev/cu.usbmodem105` → nRF9151 **uart1** = modem trace @ 1 Mbaud (silent unless modem trace is enabled).
>
> **DTR gotcha:** the bridge firmware holds UART RX data back on the USB side until the host asserts DTR. Plain `cat /dev/cu.usbmodem102` on macOS does **not** assert DTR and will appear to show the console as dead (a few bytes of noise per minute). Any proper terminal works: `screen /dev/cu.usbmodem102 115200`, `minicom`, `cu -l /dev/cu.usbmodem102 -s 115200`, the nRF Terminal in VS Code, or a pyserial script. Quick pyserial one-liner for scripted checks:
>
> ```bash
> /opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3 -c "
> import serial, time
> s = serial.Serial('/dev/cu.usbmodem102', 115200, timeout=0.5)
> s.dtr = True; time.sleep(0.2); s.reset_input_buffer()
> t0 = time.time()
> while time.time()-t0 < 6:
>     d = s.read(512)
>     if d: print(d.decode('utf-8', errors='replace'), end='')
> "
> ```

---

## Firmware Repository: gosteady-firmware

### Current State

The repo contains a **bring-up target** — a minimal Zephyr app that blinks the RGB LED purple at 1 Hz, prints a heartbeat log, and polls both motion sensors once per tick. As of **2026-04-19**, **Milestones 1 and 2 are complete**: toolchain, SDK, J-Link path, build+flash cycle, serial console, and both sensor drivers are all verified against real hardware.

- **M1 (dev env):** `<inf> gosteady: heartbeat tick=N` prints at 1 Hz on `/dev/cu.usbmodem102`.
- **M2 (sensor bring-up):** BMI270 (SPI) and ADXL367 (I²C) both produce sane readings — ~±1 g on the gravity axis at rest, noise-floor-level signal on the other axes. BMI270 is configured at 4 g / 500 dps / 100 Hz per the v1 annotation schema. **Note:** Z-axis signs are opposite between BMI270 (+1 g) and ADXL367 (-1 g) because the two chips are mounted in different orientations on the PCB — handle this at the algorithm fusion layer.

The repo is pushed to GitHub at `https://github.com/Jabl1629/gosteady-firmware` — though as of this update, local has substantial uncommitted work beyond the initial scaffold commit.

### File Structure

```
gosteady-firmware/
├── CMakeLists.txt          # Zephyr CMake config
├── prj.conf                # Kconfig: GPIO, LOG, UART console, PRINTK
├── src/
│   └── main.c              # LED toggle every 1000ms, logs "heartbeat tick=N"
├── .gitignore              # build/, .west/, *.hex/bin/elf, .DS_Store, venvs
├── README.md               # Project orientation + 15-step firmware arc
└── GOSTEADY_CONTEXT.md     # This document
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.20.0)
find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(gosteady_firmware LANGUAGES C)
target_sources(app PRIVATE src/main.c)
```

### prj.conf

```
CONFIG_GPIO=y
CONFIG_LOG=y
CONFIG_LOG_DEFAULT_LEVEL=3
CONFIG_PRINTK=y
CONFIG_THREAD_NAME=y
CONFIG_MAIN_STACK_SIZE=4096
```

### src/main.c (blinky scaffold)

```c
// Uses DT_ALIAS(led0), struct gpio_dt_spec
// Toggles LED every 1000ms
// Logs "heartbeat tick=N" via LOG_INF
```

### 15-Step Firmware Arc (from README.md)

1. **Dev environment setup** — blinky on the board. *(done 2026-04-19; serial console verification still pending)*
2. **Sensor bring-up** — BMI270 / ADXL367 reads over SPI / I²C. *(← next)*
3. **External flash** — LittleFS on the 64 MB QSPI.
4. **Session logging** — binary session files on flash with versioned header.
5. **USB dump** — mass storage / CDC-ACM path for host-side extraction.
6. **BLE control** — start/stop session commands over GATT (NUS).
7. **Python CLI** — host tool for session control and data dump.
8. **Dataset collection** — run the capture protocol end-to-end.
9. **Python algorithm** — distance estimator trained on the dataset.
10. **C port** — move the estimator on-device.
11. **Validation** — hold-out error characterization.
12. **Cellular** — LTE-M / NB-IoT link up on nRF9151.
13. **Cloud backend** — session telemetry upload.
14. **Production telemetry** — battery, errors, OTA hooks.
15. **Field testing.**

---

## Algorithm: Distance Estimation

### Current State (Python, validated offline)

A step-based distance estimator developed in Python using data from a prior prototype. Code lives in `GoSteady/null_old_fw/`.

**Pipeline:**
```
IMU data → gravity removal (Butterworth LP 0.5 Hz) → accel magnitude → peak detection → stride regression from peak accel → distance (ft)
```

**Performance (9 calibration runs, polished concrete):**
- Step detection MAE: 0.67 steps
- Distance MAPE: 12.4%
- Distance MAE: 1.21 ft
- All runs within 25% error

**Calibrated parameters:**
- Step detection prominence: 0.15 g
- Step detection min distance: 70 samples (~0.7s at 99.3 Hz)
- Stride regression: `stride_ft = 1.257 × peak_accel_g − 1.052` (R² = 0.84)

### Why Step-Based, Not Dead Reckoning?

Dead reckoning (double-integrating accelerometer) was tested and gives 73% MAPE — unusable. Even 0.001g of bias integrates to feet of phantom displacement. This is a fundamental limitation of low-cost MEMS IMUs.

### Dual-Algorithm Architecture (Production Goal)

The production firmware will support two walker types with different motion models:

**Standard walker (no wheels):** Lift → shift → plant → shuffle. Each step produces a distinct impact spike. The current step detection algorithm targets this pattern.

**2-wheeled walker:** Glide → shuffle. The front wheels stay on the ground, creating a continuous rolling motion with less distinct step boundaries. Needs a different detection approach — likely continuous acceleration integration over glide phases rather than discrete step counting.

**Online walker-type classifier:** Production firmware will include a classifier with hysteresis that detects which walker type is in use and switches between the two distance estimation algorithms. For v1 data collection, we are fixing walker type to 2-wheeled only.

### Surface Effects

Surface friction affects both step/stride dynamics and sensor signatures:
- **Polished concrete:** Low friction, clean glide for wheeled walkers
- **Carpet:** High friction, shorter strides, dampened acceleration peaks
- **Outdoor concrete/sidewalk:** Variable texture, cracks create impulse noise

The IMU's high-frequency content contains surface friction signatures that could enable automatic surface detection. We decided NOT to use a microphone for surface detection — it adds hardware cost, battery drain, and the IMU already captures the needed friction signal.

---

## Data Collection Plan

The canonical sources for this section are the two files in `data collection and protocols/`:

- **`GoSteady_Capture_Protocol_v1.docx`** — defines the 30-run v1 dataset, prerequisites, run matrix, per-run procedure, and post-session ingest flow.
- **`GoSteady_Capture_Annotations_v1.xlsx`** — the ground-truth spreadsheet. Three sheets: `Captures` (one row per run), `Legend` (column meanings), `Vocabularies` (controlled values). The `session_uuid` column is the primary key tying session files on flash to spreadsheet rows.

> The spreadsheet already has ~15 sample rows with `walker_type=standard`, `cap_type=tacky`, and surfaces outside v1 scope (hardwood, tile, outdoor_concrete). Those are **schema-test fixtures**, not v1 data — they exist to validate the ingest pipeline. The real v1 dataset will be 30 rows with `walker_type=two_wheel` + `cap_type=glide`.

### Scope (v1)

- **Subject:** 1 (S00 = Jace).
- **Walker type:** `two_wheel` only.
- **Cap type:** `glide` only (single physical cap).
- **Surfaces:** `polished_concrete` (indoor), `outdoor_concrete`, `low_pile_carpet` (indoor). 10 runs each = **30 total**.
- **Out of scope (deferred to v2):** standard/tacky dataset, additional subjects, walker-type / surface transition runs, other surfaces (tile, hardwood, vinyl, asphalt), per-subject calibration.

### Prerequisites (before capture can start)

These are hard blockers — capturing data the ingest pipeline can't handle is more expensive than waiting. Each item is a distinct firmware-arc milestone:

- **Session logging to QSPI flash** working and verified on a throwaway session (Milestone 4).
- **BLE `start_session` / `stop_session` commands** on the host laptop, able to send the full pre-walk metadata payload (Milestone 6 + 7).
- **USB mass-storage / CDC dump path** to extract all session files after the capture session (Milestone 5 + 7).
- **Batch-ingest script** that reads each session file's header, creates a matching row in the annotation spreadsheet keyed by `session_uuid`, and fills every FIRMWARE-owned column automatically.

### Run Matrix

All 30 runs, pulled from the canonical protocol. `course_id` is the named physical path; `run_type`, `intended_speed`, and `direction` are separate pre-walk fields (not conflated like they were in the old draft).

**Indoor polished concrete (runs 1–10):**

| # | course_id | run_type | dist_ft | speed | direction | notes |
|---|---|---|---|---|---|---|
| 1 | concrete_A_straight | normal | 10 | normal | straight | warmup |
| 2 | concrete_A_straight | normal | 20 | slow | straight | |
| 3 | concrete_A_straight | normal | 20 | normal | straight | |
| 4 | concrete_A_straight | normal | 20 | fast | straight | |
| 5 | concrete_A_straight | normal | 20 | normal | straight | repeat for variance |
| 6 | concrete_A_long | normal | 40 | normal | straight | long-distance |
| 7 | concrete_A_short | normal | 10 | normal | straight | |
| 8 | concrete_A_curve | normal | 20 | normal | s_curve | weave 2 cones |
| 9 | concrete_A_straight | stationary_baseline | 0 | slow | straight | 30 s parked |
| 10 | concrete_A_straight | stumble | 20 | normal | straight | simulated near-trip |

**Outdoor concrete sidewalk (runs 11–20):** same template. `course_id` prefix `sidewalk_`. Run 19 = `stationary_baseline` (0 ft / 30 s). Run 20 = `pickup` (walker lifted briefly mid-walk, 10 ft).

**Indoor low-pile carpet (runs 21–30):** same template. `course_id` prefix `carpet_A_`. Run 26 is only 30 ft (indoor space limit). Run 28 s-curve is 15 ft (furniture path). Run 29 = `stationary_baseline`. Run 30 = `stumble`.

### Annotation Schema (canonical field names)

These are the exact column names in `GoSteady_Capture_Annotations_v1.xlsx`. Use them verbatim in firmware and ingest code — no synonyms.

**FIRMWARE layer** (stamped into session file header by the device, populated at ingest):
`session_uuid`, `device_serial`, `firmware_version`, `sensor_model`, `sample_rate_hz`, `accel_range_g`, `gyro_range_dps`, `session_start_utc`, `session_end_utc`, `sample_count`, `battery_mv_start`, `battery_mv_end`, `flash_errors`.

**PRE-WALK layer** (sent via BLE `start_session` payload, also stamped into the session file header):
`subject_id`, `walker_type`, `cap_type`, `walker_model`, `mount_config`, `course_id`, `intended_distance_ft`, `surface`, `intended_speed`, `direction`, `run_type`, `operator`.

**POST-WALK layer** (operator fills after the run):
`valid` (Y/N), `manual_step_count`, `actual_distance_ft` (defaults to `intended_distance_ft` unless the subject deviated), `subjective_speed`, `events` (e.g. `stumble@0:07`, semicolon-separated), `discard_reason`, `free_notes`.

**DERIVED layer** (formula-only — do not overwrite):
`duration_s` = `session_end_utc − session_start_utc`, `avg_speed_ft_s` = `actual_distance_ft / duration_s`.

### Controlled Vocabularies

From the `Vocabularies` sheet. Firmware's BLE start-session payload handling and any host-side validation should reject values outside these lists.

| Field | Allowed values |
|---|---|
| `walker_type` | `standard`, `two_wheel` |
| `cap_type` | `tacky`, `glide` |
| `surface` | `polished_concrete`, `low_pile_carpet`, `high_pile_carpet`, `hardwood`, `tile`, `linoleum`, `vinyl`, `outdoor_concrete`, `outdoor_asphalt` |
| `intended_speed` | `slow`, `normal`, `fast` |
| `direction` | `straight`, `turn_left`, `turn_right`, `s_curve`, `pivot` |
| `run_type` | `normal`, `stumble`, `pickup`, `setdown`, `stationary_baseline`, `car_transport`, `chair_transfer`, `turn_test`, `obstacle`, `walker_type_transition`, `surface_transition` |
| `mount_config` | `front_left_leg`, `front_right_leg`, `rear_left_leg`, `rear_right_leg`, `front_crossbar` |
| `valid` | `Y`, `N` |
| `discard_reason` | `bad_mount`, `sensor_glitch`, `incomplete_run`, `wrong_course`, `interruption`, `operator_error` |

### Firmware Implications

The schema forces several concrete firmware requirements that shape Milestones 3–7:

- **RTC with UTC time** for `session_start_utc` / `session_end_utc`. A cellular time sync path is the cleanest way to get one.
- **UUIDv4 generation** at session start for `session_uuid` — the primary key across device and spreadsheet.
- **Battery voltage** in millivolts (not percent), measured at both session start and end.
- **Flash integrity counter** (`flash_errors`) bumped whenever a log write fails CRC / verify.
- **Session file header format** must serialize all 13 FIRMWARE fields + all 12 PRE-WALK fields. Versioned header is essential — v2 of the protocol will add fields.
- **BLE start_session command** needs to accept and validate the full 12-field PRE-WALK payload before firmware stamps it. Invalid payload → reject with error, don't silently drop to defaults.

### Per-Run Procedure (operator side, abbreviated)

1. Confirm the next run's `intended_distance_ft`, `intended_speed`, `direction`, `run_type` from the matrix.
2. Tape-measure the course; mark start/end; place cones for `s_curve` runs.
3. Position the walker with a consistent convention (e.g. front wheels on start line).
4. From laptop, send BLE `start_session` with the full pre-walk payload.
5. Wait for confirmation + 1 s stationary settling period.
6. Walk the course. For `stationary_baseline` leave the walker parked 30 s. For `stumble` / `pickup` introduce the event at mid-walk and note the timestamp.
7. Stop at end line, 1 s settling, then BLE `stop_session`.
8. Log any `actual_distance_ft` deviation and `events` with timestamps in the run log.
9. ~30 s gap before next run (clean boundary for sensors + flash log).

### Post-Session Ingest

Run after completing all 10 runs on a surface, always before closing out the session:

1. Disassemble cap, connect Thingy:91 X via USB, mount flash (mass-storage) or use CDC dump.
2. Copy all new session files to `/raw_sessions/<capture_date>/`. Verify file count == runs executed. **Do not delete anything from the device if counts mismatch — investigate first.**
3. Run batch-ingest script → appends one row per `session_uuid` to the annotation spreadsheet, auto-filling FIRMWARE + PRE-WALK columns.
4. Operator fills POST-WALK columns from run-log notes.
5. Sanity check each row: non-zero `sample_count`, zero `flash_errors`, `duration_s` within a few seconds of intended (except `stationary_baseline`).
6. Commit raw_sessions + updated spreadsheet to the dataset archive — this is the v1 capture deliverable.

---

## Existing Algorithm Codebase (Python)

Located at `GoSteady/null_old_fw/`:

```
null_old_fw/
├── src/
│   ├── data_loader.py           # Parse .dat files and Run Annotations.xlsx
│   ├── preprocessing.py         # Butterworth filtering, gravity removal, magnitude
│   ├── step_detection.py        # Accel peak detection, gyro threshold, step metrics
│   ├── distance_estimation.py   # Accel regression, per-speed stride, fixed stride, dead reckoning
│   └── visualization.py         # IMU plots, step overlays, comparison charts
├── notebooks/
│   ├── 01_data_exploration.ipynb
│   ├── 02_preprocessing.ipynb
│   ├── 03_step_detection.ipynb
│   ├── 04_distance_estimation.ipynb
│   └── 05_validation_summary.ipynb
├── testdata/
│   ├── data000.dat – data011.dat    # 12 IMU recordings (7-col TSV)
│   └── Run Annotations.xlsx
└── requirements.txt
```

**Sensor data format (.dat files):** Tab-separated, 7 columns, no header, CRLF line endings:
```
timestamp_ms  accel_x_g  accel_y_g  accel_z_g  gyro_x_dps  gyro_y_dps  gyro_z_dps
```

- Accelerometer: units in g, Y-axis carries gravity (~0.96g at rest)
- Gyroscope: units in deg/s, quantized at 0.0625 dps steps
- Sample rate: 99.3 Hz (10.07 ms ± 0.29 ms intervals)

---

## Immediate Next Steps

Milestones 1 and 2 are both done. Next is **Milestone 3 — external flash**: bring up LittleFS on the 64 MB QSPI (Gigadevice GD25LE255E, already `status = "okay"` on `spi3` CS0 in the board DTS — we just need the Kconfig + mount point). That unlocks Milestone 4 (binary session logging with versioned header), which is where the firmware-owned fields from the v1 annotation schema (see *Data Collection Plan*) start getting stamped into real session files. 

---

## Key Design Decisions Made

- **Step-based, not dead reckoning** — dead reckoning gives 73% MAPE with MEMS IMUs, unusable
- **Accelerometer magnitude** for step detection — orientation-independent (`sqrt(x² + y² + z²)`)
- **Stride regression from peak acceleration** — no speed label needed, 12.4% MAPE
- **No microphone** for surface detection — IMU high-frequency content has the friction signal, mic adds cost/power/board complexity
- **v1 data collection is single-walker, single-cap, 3 surfaces** — complexity deferred to v2
- **Thingy:91 X as dev platform** — production will use custom PCB
- **thingy91x/nrf9151/ns** as build target — non-secure application core, TF-M in secure partition
