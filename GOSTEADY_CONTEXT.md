# GoSteady — Firmware Context

> **Audience:** future Claude sessions starting cold. Optimized for low-token-cost orientation.
> Per-commit detail lives in git log; per-day cross-team notes live in the coord doc (link at end).

---

## What is GoSteady?

GoSteady is a smart walker cap — a small device that attaches to a walker leg and estimates distance traveled using onboard IMU sensors. Targets elderly users and PT patients who need mobility tracking without wearing anything on their body.

Prototype platform: **Nordic Thingy:91 X**, board target `thingy91x/nrf9151/ns` (non-secure, TF-M in secure partition). Production will move to a custom PCB.

Firmware version: **`0.16.1-gait-wakewindow`** (gait speed + decoupled step counter, 2026-06-18; coord §C46 / spec `gosteady-portal/docs/specs/2026-06-07-gait-speed.md`). Prior pilot lineage: **`0.13.1-pilot`** (battery pilot, 2026-05-31; see coord §C38). Two-step battery-life work for the **1350 mAh AA-class pilot cell / 30-day target**: `0.12.0-psm` requests LTE-M **PSM** at attach (root cause was `CONFIG_LTE_PSM_REQ` never set → modem idled in registered I-DRX at ~mA 24/7 → ~20-day life; **fixed + validated on `GS9999999998`**, carrier granted tau=3 h / active=2 s), and `0.13.x-pilot` adds the `prj_pilot.conf` overlay (snippets OFF + `CONFIG_GOSTEADY_LOW_POWER`; **dark at rest but green session LED on during recording** via `CONFIG_GOSTEADY_SESSION_LED`, added 0.13.1 — the idle purple blink stays off). Bench model projects ~90–210 days optimized; per-connection TLS energy is the remaining swing, to be pinned by an on-battery SoC-slope discharge run. Deferred (capture hot-path risk): idle-sampler gating + gyro-disable. Prior shipping version **`0.11.0-wipe-cmd`** (as of 2026-05-17; commit `5d73684`; flashed to `GS9999999998` 2026-05-17 21:55 UTC; **bench-validated end-to-end 2026-05-18** — full wipe-ack auto-recycle roundtrip per coord §C22, including memo W-R2 closure via `tools/pull_sessions.py --list-only` returning 0 files post-wipe). Three structural findings surfaced at bench (coord §C22.3) — most important is AWS IoT MQTT 3.1.1 persistent_session 1h timer expires before firmware's 1h heartbeat, making downlink cmds operationally unreliable without intervention; §C23 design memo for cloud-side connection-coordinator Lambda is the production fix. Previous versions: `0.10.0-at-timeout` (bounded-timeout AT wrapper in `cellular.c`, §C11.5 / commit `95f87e6`); `0.9.0-hardening` (M14.5-shakedown candidate from 2026-05-10).
Repo: `https://github.com/Jabl1629/gosteady-firmware` — direct-to-`main` workflow (no PRs).
Cowork pattern: single Claude operates both this firmware repo and `gosteady-portal` (cloud). They were separate sessions through 2026-05-06; merged 2026-05-10. The shared coord doc is now a "shared engineering log" rather than cross-team broadcast.

---

## Hardware (Thingy:91 X)

| IC | Function | Bus / Pins | Role |
|---|---|---|---|
| **nRF9151** | App processor + LTE-M / NB-IoT | — | Runs `src/*`. TF-M secure partition. No USB, no 2.4 GHz radio. |
| **nRF5340 (app)** | USB + BLE host | — | Runs vendored `bridge_fw/`. Exposes USB CDC + BLE NUS. |
| **nRF5340 (net)** | BLE controller | IPC to app core | NCS `ipc_radio` sample, sysbuild-managed. |
| **nRF7002** | Wi-Fi 6 | spi3 | **Not used in v1.** |
| **BMI270** | 6-axis IMU (accel+gyro) | spi3 (CS P0.10, IRQ P0.06) | Primary sensor — step detection + stride. 4g / 500dps / 100Hz. |
| **ADXL367** | Ultra-low-power 3-axis accel | i2c2 @ 0x1d (INT1 P0.11) | Wake-on-motion trigger. Z-sign opposite of BMI270 (different orientations). |
| **GD25LE255E** | 32 MB SPI NOR flash | spi3 (8 MHz) | LittleFS partitions (sessions / snippets / etc.) |
| **nPM1300** | PMIC + fuel gauge | I²C | Charging + battery_pct/mv via `nrf_fuel_gauge` lib. |

### Debug interface

- No onboard debugger. **External SEGGER J-Link EDU Mini V2** (S/N `802006700`) via 10-pin SWD Tag-Connect.
- **SW2 switch:** `nRF91` (default — app flashing) or `nRF53` (bridge reflashing). **Always verify before any `nrfjprog --recover`** — wrong-position recover can cascade-corrupt the unintended chip (lost ~3 hr to this 2026-05-06).
- USB CDC enumerates from the nRF5340 bridge:
  - `/dev/cu.usbmodem*102` → nRF9151 **uart0** @ 115200 (app console / `LOG_INF`)
  - `/dev/cu.usbmodem*105` → nRF9151 **uart1** @ 1 Mbaud (dump/control protocol; also BLE NUS tunnel target)
  - Mass-storage volume `/Volumes/NO NAME` (bridge `Config.txt` lives here)
- USB CDC digit count varies between replugs (`*1102` ↔ `*11102`). Always `ls /dev/cu.usbmodem*` after a physical replug.

---

## Dev environment

- **NCS v3.2.4** at `/opt/nordic/ncs/v3.2.4`
- Toolchain at `/opt/nordic/ncs/toolchains/185bb0e3b6/`
- Toolchain Python: `/opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3` (has `intelhex`, `west`, etc.)

### Critical PATH issue

`~/.zshrc` PREPENDS Homebrew Python 3.14 + `~/.local/bin` to PATH → `west` picks the wrong Python which lacks `intelhex`. **Fix:** change prepends to appends in `~/.zshrc`.

Manual env activation (when not using nRF Connect terminal):
```bash
export PATH="/opt/nordic/ncs/toolchains/185bb0e3b6/bin:/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk/arm-zephyr-eabi/bin:$PATH"
export ZEPHYR_BASE="/opt/nordic/ncs/v3.2.4/zephyr"
# Required for PRISTINE (-p always) builds — incremental builds reuse the cached
# SDK path, but a fresh build dir fails at the mcuboot sub-image configure with
# "Could not find a package configuration file provided by Zephyr-sdk" without these:
export ZEPHYR_SDK_INSTALL_DIR="/opt/nordic/ncs/toolchains/185bb0e3b6/opt/zephyr-sdk"
export ZEPHYR_TOOLCHAIN_VARIANT="zephyr"
```

### Build configs (3 overlays)

| Build dir | Overlay | Use case |
|---|---|---|
| `build/` | none (default `prj.conf`) | Bench data collection (M8 captures, no cellular). Default. |
| `build_cloud/` | `prj_cloud.conf` | Bench + cloud (cellular MQTT). Iterating on M12.1+. |
| `build_field/` | `prj_field.conf` | Deployment image. `CONFIG_GOSTEADY_FIELD_MODE=y` gates off SW0 / dump UART / heartbeat LEDs. |
| `build_pilot/` | `prj_pilot.conf` (self-contained) | **Battery pilot image (0.13.1-pilot).** = field + snippets OFF + `CONFIG_GOSTEADY_LOW_POWER` + `CONFIG_GOSTEADY_SESSION_LED` (green LED during recording, dark at rest). Build single-overlay (sysbuild only forwards one `EXTRA_CONF_FILE` cleanly): `-DEXTRA_CONF_FILE=prj_pilot.conf`. |

### Build + flash commands

```bash
# Build (toolchain env sourced; -p auto incremental, -p always pristine)
west build -b thingy91x/nrf9151/ns -d build_cloud -p auto -- -DEXTRA_CONF_FILE=prj_cloud.conf

# Flash via external J-Link Mini (SW2 = nRF91). recover = unprotect+chip-erase + program in one shot.
nrfjprog -f NRF91 --recover --program build_cloud/merged.hex --verify --reset --snr 802006700

# Console (uart0): use screen/minicom/cu/pyserial — NOT cat (no DTR assert).
screen /dev/cu.usbmodem*102 115200
```

Chip-erase preserves external flash (LittleFS partitions, snippets, activation.bin) and modem cert (sec_tag 201) — only nRF9151 application flash is wiped.

---

## Repo layout

```
gosteady-firmware/
├── CMakeLists.txt, Kconfig, prj*.conf, sysbuild.conf, pm_static.yml
├── boards/thingy91x_nrf9151_ns.overlay
├── src/                          # nRF9151 app
│   ├── main.c                    # LED, heartbeat, sampler thread, auto-start coordinator
│   ├── session.h/c               # Session file format + writer thread + auto-prune + orphan sweep
│   ├── dump.h/c                  # uart1 line protocol (LIST/DUMP/DEL/PING)
│   ├── control.h/c               # Transport-agnostic START/STOP/STATUS JSON parser
│   ├── cellular.h/c              # nrf_modem_lib + LTE attach + AT+CCLK?/AT+CESQ + ISO 8601 helpers
│   ├── cloud.h/c                 # AWS IoT MQTT/TLS — heartbeat + activity workers + activate cmd dispatch
│   ├── snippet.h/c               # 30 s raw-sample capture + opportunistic upload + rotation policy
│   ├── battery.h/c               # nPM1300 fuel gauge worker
│   ├── forensics.h/c             # Crash forensics + WDT supervisor + CRASH/STALL stress cmds
│   ├── activation.h/c            # /lfs/activation.bin persistence + pre-activation gate
│   ├── version.h                 # GS_FIRMWARE_VERSION_STR (single source of truth)
│   └── algo/                     # M10 V1 distance estimator (C port)
│       ├── gosteady_algo_params.h    # Auto-generated from algo/export_c_header.py — DO NOT EDIT
│       ├── gs_filters.h/c             # DF-II-T biquad cascade
│       ├── gs_motion_gate.h/c         # Running-σ + Schmitt FSM
│       ├── gs_step_detector.h/c       # Schmitt peak FSM + per-peak features
│       ├── gs_roughness.h/c           # Motion-gated inter-peak RMS
│       └── gs_pipeline.h/c            # Orchestrator + surface classifier + stride sum
├── tests/host/                   # Host-side algo regression suite (no Zephyr dep, 31/31 passing)
├── bridge_fw/                    # Vendored nRF5340 bridge fork (uart1 retargeted from uart0)
│   ├── PATCHES.md                # What we changed and why
│   └── boards/thingy91x_nrf5340_cpuapp.overlay  # uart1 default 1 Mbaud (load-bearing!)
├── tools/
│   ├── capture.html              # Web BLE operator page (Chrome on Mac; M3 popup; 3-tier log persistence)
│   ├── control.py                # USB-uart1 START/STOP/STATUS CLI with run-matrix presets
│   ├── pull_sessions.py          # USB-uart1 LIST/DUMP/DEL puller (noise-tolerant)
│   ├── cleanup_device.py         # Wipe /lfs/sessions before a fresh capture
│   ├── ingest_capture.py         # Join .dat headers + POST-WALK notes JSON → CSV
│   ├── read_session.py           # Parse .dat file, validate against v1 vocabulary
│   ├── log_console.py            # Continuous uart0 logger → logs/uart0_YYYY-MM-DD.log
│   └── flash_cert.py             # Nordic nrfcredstore wrapper for sec_tag 201 cert flashing
├── algo/                         # Python distance estimator + eval harness (~25 files)
├── logs/                         # uart0 daily logs (gitignored)
├── raw_sessions/                 # Per-capture-date archive (gitignored)
└── data collection and protocols/ # v1 capture protocol + annotation spreadsheet
```

---

## Current state (2026-05-17)

**Done end-to-end:** capture pipeline (M1–M7) → Python algo + C port (M9 / M10) → deployment requirements (M10.5) → power architecture (M14-prep Phase 1a–5: ADXL367 wake-on-motion, BMI270 PM, auto-start coordinator, auto-stop, FIELD_MODE Kconfig) → cellular (M12.1a) → all 4 cloud uplinks bench-validated (heartbeat/activity/activate-cmd/snippet) → M10.7 production telemetry (storage repartition, nPM1300, crash forensics + WDT) → **M14.5 hardening sprint (2026-05-10) shipped 6 of 9 punch-list items** → **2026-05-11/12 informal conference site-survey** produced 1 WDT + 1 fatal + 3 reboots (Onomondo SIM exhausted → tight PDN-reject loop → §C10.5 AT-serialization watch-item triggered; full forensics in coord §C11) → **§C11.5 firmware fix shipped 2026-05-16 in 0.10.0-at-timeout** (bounded-timeout AT wrapper in `cellular.c`; commit `95f87e6`; bench-validated on `GS9999999998` per coord §C12).

**Dev units:**

| Serial | Firmware | J-Link | SIM | Status |
|---|---|---|---|---|
| `GS9999999999` | `0.9.0-hardening` | Broken pads (2 J-Link pins broke off mid-May) — cannot reflash | Onomondo (exhausted, EMM cause 19 on every PDN request) | Original bench unit. Held at 0.9.0 because no flash path. Can still publish if SIM is reactivated. |
| `GS9999999998` | `0.15.2-wakewindow` (flashed 2026-06-04, coord §C43 — **OCV SoC fix**; also PSM tau=3 h/active=2 s + green session LED. Was `0.13.1-pilot` during the discharge run) | Working | iBasis trial (Nordic-shipped, 10 MB lifetime — fresh) | Provisioned 2026-05-16 per new-dev-unit-bringup playbook. **Both the §C11.5 AT-timeout patch and §C20 AA-recycle patch live + validated on this unit.** Boot 9, faults 0/0/0, hourly heartbeat publishing all extras. Cloud state as of 2026-05-18T15:58:16Z (post-§C22 bench-validated wipe-ack auto-recycle): `ready_to_provision`, no outstanding wipe cmds, Shadow desired empty. Filesystem post-wipe: `/lfs/sessions/` empty (LIST verified), `/lfs/activation.bin` removed, 24 `/snippets/*` tuples purged. Discharge run (3.4 days off-charger) surfaced the **fuel-gauge divergence** (coord §C43): `nrf_fuel_gauge` read 0% / `battery_critical` at 4.086 V (~86% actual) — battery fine, gauge broken (FMEA 3.1 at sub-mA idle). **Fixed: voltage-based OCV SoC** (`battery.c`, verified 4.213 V→100% / Shadow `battery_pct=1.0`). Battery barely drained (4.2→4.09 V in 3.4 days) → low-power firmware validated; a precise life number still needs a fresh OCV-firmware discharge run. Earlier GS98 `battery_critical` alarms were false. |
| `GS0000000001` | `0.16.1-gait-wakewindow` (flashed 2026-06-18, coord §C46/§C47 — gait + step-counter + time-guard work; was `0.15.3-wakewindow` flashed 2026-06-04, coord §C45 — **wipe→shake reactivation hang fix**; was `0.15.1-wakewindow` observed live 2026-06-03. client_id baked + cert sec_tag 201. Superseded the `0.14.0-shipmode` of coord §C39/§C40 via the 0.15.0/0.15.1 wake-window commits) | Working (external J-Link) | iBasis eSIM (fresh) | **First shipping unit. As of 2026-06-03 (coord §C41): facility-activated end-to-end through the live portal.** Provisioned + activated via the facility build (`dev.portal.gosteady.co`) under `client_dev_pilot`/`fac_dev_pilot_a` (cmd `act_353d21c0…`, delivered on a shake via the 0.15.0 wake-window). **D2C QR-claim staging was overwritten by this** — the `walkerId 37b2e250-…` attribute persists but the unit is now facility-owned (not a claimable D2C cap); re-stage for D2C per coord §C41.3 if that test is wanted back. **As of 2026-06-04 (coord §C42) it ran the full device lifecycle:** Pilot CapTest (42-step walk) → **End Monitoring** → wipe + auto-recycle to `ready_to_provision` (on a shake; `reported.wipe_complete` matched) → re-provisioned + re-activated to a NEW resident (Rosa Delgado, `pat_8ce10709…`, 16-step walk). Currently **`active_monitoring` for Rosa**, battery ~94%. **2026-06-04 (coord §C45):** flashed `0.15.3-wakewindow` + console-validated the wipe→shake reactivation fix — re-ran End-Monitoring → wipe → Resume → **shake reactivated with NO reboot** (`preact: ACTIVATED during wake window → green confirm`; boot_count steady, 3.1 h continuous uptime). The §C44.5 "reboot needed post-wipe" trap is fixed; the §C44.5 PSM/rate-limit hypothesis was wrong (real cause: heartbeat thread parked in the activated `k_sleep` across the wipe). **2026-06-18 (coord §C46/§C47):** flashed `0.16.0`→`0.16.1-gait` (decoupled merge-0.8 step counter + `gait_speed_fts` + long-session guard relax); step count + gait validated end-to-end (active_monitoring for Rosa; gait 0.55–1.23 ft/s rendering in the portal). Investigating "missing" Jun 15–16 portal data revealed the device clock read **2080** on Jun 14–16 (roaming iBasis SIM gave no NITZ → modem 1980 default; parser accepts it; ~60 walks mis-dated but **not lost** — confirmed online via heartbeats + real `ingestedAt`). Time-reliability fix **designed, not implemented** (SNTP via `date_time` lib + cloud-ingest anchoring + AT+CCLK sanity gate; spec `gosteady-portal/docs/specs/2026-06-18-device-time-reliability.md`). |

**Bench fleet snippet:** keep snippets enabled on `GS9999999998` for full pipeline validation; if iBasis trial nears exhaustion (snippets dominate ~300× heartbeats per the playbook sizing table), rebuild with `-DCONFIG_GOSTEADY_SNIPPET_ENABLE=n` for longevity.

### 15-step firmware arc — status

| # | Milestone | Status |
|---|---|---|
| 1 | Dev env (heartbeat blinky) | ✅ done |
| 2 | Sensor bring-up (BMI270 + ADXL367) | ✅ done |
| 3 | External flash (LittleFS) | ✅ done |
| 4 | Session logging | ✅ done |
| 5 | USB dump | ✅ done |
| 6 | BLE control (NUS, capture.html) | ✅ done |
| 7 | Python CLI / POST-WALK popup | ✅ done |
| 8 | Dataset collection | ⏸ paused at 19/30 (carpet runs deferred to v1.5) |
| 9 | Python algorithm (Phase 1–4) | ✅ done — auto-surface roughness adjustment |
| 10 | C port + on-device validation | ✅ done — `src/algo/`, 31/31 host tests passing |
| 10.5 | Field deployment requirements | ✅ spec done; M14-prep Phase 1a–5 implemented |
| 10.7.1 | Storage repartition | ✅ done (`pm_static.yml`) |
| 10.7.2 | nPM1300 fuel gauge | ✅ done (battery.c) |
| 10.7.3 | Crash forensics + WDT | ✅ done (forensics.c, stress-tested) |
| 11.1 | Algo-side validation | ✅ host suite passing; M14.5 includes confirmation walk against shipping FW |
| 11.2 | Deployment-side validation | 🔲 outcome of M15 |
| 12.1a | Modem attach + UTC | ✅ done |
| 12.1c.1/.2 | Heartbeat (one-shot → hourly + extras) | ✅ done |
| 12.1d | Activity uplink | ✅ done |
| 12.1e.1 | NCS Shadow lib bench check | ✅ PASS |
| 12.1e.2 | Activate cmd ingest + persistence | ✅ done |
| 12.1f | Snippet capture + opportunistic upload | ✅ done |
| 13 | Cloud backend | ✅ Phase 0/1A/1B/1.5/1.6 deployed; firmware-side complete |
| 14 | OTA / unit-4+ telemetry | 🔲 future-work bucket |
| 14.5 | Site-survey unit shakedown | ⏭ NEXT — flash `GS0000000001` with shipping cert |
| 15 | Field testing (clinic) | 🔲 after M14.5 |

---

## Algorithm v1 — auto-surface roughness adjustment

**Locked 2026-04-25.** Operator never picks a surface; firmware computes a session-level roughness metric from the IMU stream and selects coefficients automatically.

```
IMU 100 Hz  →  |a|/g  →  HP 0.2 Hz  →  |a|_HP
                              │
                ┌─────────────┤
                ▼             ▼
            LP 5 Hz      500 ms running σ
                │             │
                ▼             ▼
        Schmitt peak FSM   motion gate (Schmitt hysteresis)
                │             │
                ▼             │
          per-peak amp        │
                └─────┬───────┘
                      ▼
            inter-peak RMS over motion-gated samples
                      ▼
                roughness R
                      ▼
            R-thresholded coefficient lookup
                      ▼
        stride_ft = a_surface + b_surface · peak_amp_g     (per peak)
                      ▼
        distance_ft = Σ stride_ft     (per session)
```

| Block | Param | Value |
|---|---|---|
| HP filter | Butterworth, order 2, cutoff | 0.2 Hz |
| LP filter | Butterworth, order 2, cutoff | 5 Hz |
| Peak detector | Schmitt enter / exit / min-gap | 0.02 g / 0.005 g / 0.5 s |
| Motion gate | window / enter σ / exit σ / exit hold | 500 ms / 0.01 g / 0.005 g / 2 s |
| Surface classifier | hard threshold on R | τ = 0.245 g |
| Stride coeffs (R < τ) | indoor | `+0.217 + 2.757 · amp_g` |
| Stride coeffs (R ≥ τ) | outdoor | `−0.024 + 1.705 · amp_g` |
| Stride coeffs | low-pile carpet | TBD — runs 21–30 not captured; v1 fallback = nearest-R indoor |

**LOO performance (n=16, indoor + outdoor):** 22.4 % pooled MAPE (indoor 17.2 %, outdoor 27.6 %). 15/16 walks correctly auto-classified. v1.5 retrain expected to close the gap with carpet captures + larger n per surface.

**Why step-based, not dead reckoning?** Dead reckoning gives 73 % MAPE — even 0.001 g of bias integrates to feet of phantom displacement. Fundamental MEMS limitation.

**Output:** logged at session_stop on uart0 as `ALGO_V1A` + `ALGO_V1B` + `ALGO_V1C` lines, also published to cloud as activity uplink (`distance_ft`, `steps`, `roughness_R`, `surface_class`, `active_min`, `gait_speed_fts`).

**Decoupled step counter + gait speed (0.16.0-gait, 2026-06-07; spec `gosteady-portal/docs/specs/2026-06-07-gait-speed.md`, coord §C46).** The Schmitt detector is a loose *impulse* detector (~2 impulses/perceived step; "the regression absorbs the ratio" for distance), so its raw peak count over-counts steps ~1.5× (worst slow: live 53-vs-33). At finalize the reported `steps` is now the emitted peak train after a **0.8 s refractory merge** (`gs_merge_step_count`) — distance still consumes *every* peak, so it's untouched (still 22.4 % MAPE). `gait_speed_fts` = `distance_ft` / **peak-train walking time** (`gs_walking_time_from_peaks` — gated inter-peak gaps ≤ 2.5 s, not the exit-hold-inflated `motion_duration_s`), omitted (NaN) when merged steps < 5 / walking < 3 s / session saturated. `ALGO_V1A` now logs `steps=<merged> raw=<impulses>`; `ALGO_V1C` logs `walk_s`/`gait_fts`/`gait_valid`. Params in `gosteady_algo_params.h` (`GS_STEP_MERGE_GAP_*`, `GS_STRIDE_GAP_CAP_*`, `GS_GAIT_*`); algo version `0.7.0-algo-v1.1`. Host suite 55/55. Gait is a within-resident *trend*, not an absolute (inherits the distance MAPE floor).

C port lives in `src/algo/`. Auto-generated coefficients header `src/algo/gosteady_algo_params.h` (regenerate via `algo/export_c_header.py`). Host regression suite `tests/host/` (31/31 passing).

---

## Field deployment (M10.5) — first-clinic profile

| Item | Value |
|---|---|
| Units | 3 (`GS0000000001/2/3`) |
| Site | Single rehab clinic |
| Duration | ~1 month |
| Hardware | Thingy:91 X (no custom PCB) |
| Connectivity | LTE-M primary, NB-IoT fallback |
| Charging | None — battery must last on a single charge (1300 mAh) |
| OTA | None — firmware fixed for the deployment |
| User interaction | None — clinic observes via portal only |
| Mounting | Custom housing |

### Hard requirements (all shipped)

- **Auto-start session on motion** (ADXL367 INT1 → BMI270 confirm → session)
- **Auto-stop on stationarity** (15 s default, M10.5 spec calls for 30 s in field)
- **Sleep-when-idle** (`CONFIG_PM=y` → System ON Idle ~50 µA between motion events)
- **Hourly heartbeat** (`gs/{serial}/heartbeat`)
- **Per-session activity uplink** on session close
- **Pre-activation gate** (`CONFIG_GOSTEADY_FIELD_MODE` builds; `/lfs/activation.bin` persistence)
- **Snippet capture** (always-arm v1; opportunistic upload piggybacks on heartbeat wake)
- **Crash forensics** (`crash_forensics` partition; survives reset; surfaced in next heartbeat)
- **Hardware watchdog** (60 s timeout, kicked from supervisor thread @ 20 s)
- **Cellular UTC** via `AT+CCLK?`
- **Per-device cert + private key** flashed at build time, sec_tag 201 in CryptoCell-312
- **nPM1300 fuel gauge** for `battery_pct` / `battery_mv`

### Anti-features (NOT in v1)

- No fall / tipover / impact detection — algorithm has zero Python prototype, deferred
- No fleet provisioning / claim-cert flow (manual cert at flash time for 3 units)
- No OTA (MCUboot layout preserved for forward-compat)
- No downlink config (all knobs compile-time; only `cmd` topic message is `activate`)
- No active LEDs in normal operation (pre-activation blue LED only)
- No real-time streaming (all uplinks are summary-form)
- No on-device patient identity (cloud-side via DeviceAssignment lookup on `serial`)

---

## Storage layout (`pm_static.yml`)

| Partition | Size | Address | Purpose |
|---|---|---|---|
| `littlefs_storage` (`/lfs`) | 8 MB | 0x4d2000 | Sessions + boot_count + activation.bin |
| `crash_forensics` | 64 KB | 0xcd2000 | Single 4 KB block: boot_count + reset_reason + fault_counters + last_fault frame |
| `telemetry_queue` | 256 KB | 0xce2000 | Persistent Priority-1 queue (carved but **unimplemented** — FMEA 6.3) |
| `snippet_storage` (`/snippets`) | 16 MB | 0xd22000 | Per-session 30 s raw IMU snippets + JSON sidecars + `.up` markers |
| `external_flash` remainder | ~2.86 MB | 0x1d22000 | Unused (reserved for future) |

---

## Portal contracts (firmware-side cached view)

> **Authoritative source:** `gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md` (append-only). If discrepancy, the coord doc wins.

### Identity & topology

| Item | Value |
|---|---|
| Device ID format | `GS` + 10 digits |
| First-3-unit serials | `GS0000000001`, `GS0000000002`, `GS0000000003` |
| Reserved test/dev range | `GS9999999990–GS9999999999` (bench unit: `GS9999999999`) |
| AWS IoT endpoint (dev) | `a2dl73jkjzv6h5-ats.iot.us-east-1.amazonaws.com:8883` |
| Auth | Per-device cert + key in CryptoCell-312 sec_tag 201 |
| Root CA | Amazon Root CA 1 |
| MQTT version | 3.1.1 (NCS 3.2.4 limit; no MQTT 5 user properties) |
| `CLEAN_SESSION` | `n` (broker queues QoS 1 cmds across hourly heartbeats) |

### Topics

| Topic | Direction | Purpose |
|---|---|---|
| `gs/{serial}/heartbeat` | uplink | Hourly liveness + state |
| `gs/{serial}/activity` | uplink | Per-session algo outputs |
| `gs/{serial}/snippet` | uplink | Raw IMU window (length-prefix + JSON header + binary) |
| `gs/{serial}/cmd` | downlink | v1 = `activate` (M12.1e.2) + `wipe` (AA-battery-recycle 2026-05-17, coord §C20) |
| `gs/{serial}/alert` | uplink | Channel exists; firmware does NOT publish in v1 |
| `$aws/things/{serial}/shadow/*` | both | Shadow GET/UPDATE for activated_at re-check |

### Heartbeat schema

Required: `serial`, `ts` (ISO 8601), `battery_pct` (0.0–1.0), `rsrp_dbm` (−140 to 0), `snr_db` (−20 to 40).
Optional (firmware ships all): `battery_mv`, `firmware`, `uptime_s`, `last_cmd_id`, `reset_reason`, `fault_counters` (object), `watchdog_hits`, `boot_count`.
Cloud handling: accept-all (unknown fields land in Shadow `reported`).
Storage: AWS IoT Device Shadow `reported` state (no DDB row).

### Activity schema

Required: `serial`, `session_start` (ISO 8601), `session_end` (ISO 8601), `steps` (0–100k; **de-satellited merged count as of 0.16.0-gait**), `distance_ft` (0–50k), `active_min` (0–1440).
Optional: `roughness_R`, `surface_class` (`indoor`/`outdoor`), `firmware_version`, `gait_speed_fts` (ft/s, session-avg walking speed; omitted when on-device guards fail — 0.16.0-gait+).
Idempotency: `(serial, session_end)`. Patient attribution: cloud-side via DeviceAssignment.

### Snippet schema (binary)

Outer framing per §F.3:
```
[4-byte BE uint32 hdr_len][hdr_len bytes JSON header][N × 28-byte sample records]
```
Inner binary header (16 bytes, packed, LE): `format_version=1, sensor_id=1, sample_rate_hz=100, sample_count_n, window_start_uptime_ms`.
Sample record (28 bytes): `t_ms, ax, ay, az, gx, gy, gz` (last 6 are float32).
Max payload 100 KB. Lands at `s3://gosteady-{env}-snippets/{serial}/{date}/{snippet_id}.bin`.

### Activate cmd

```json
{"cmd":"activate","cmd_id":"act_<uuid>","ts":"<ISO8601>","session_id":"..."}
```
Firmware: persist `activated_at` to `/lfs/activation.bin`, exit pre-activation, echo `cmd_id` in next heartbeat as `last_cmd_id`. Cloud matches against any cmd issued within last 24 h.

### Wipe cmd (AA-battery-recycle, 0.11.0-wipe-cmd+)

```json
{"cmd":"wipe","cmd_id":"wipe_<uuid>","ts":"<ISO8601>"}
```
Fired immediately by cloud-side `end-assignment` (`POST /api/v1/devices/{serial}/end-assignment`). Firmware (`src/wipe.c::gosteady_wipe_now`): refuse + retry if `battery_pct < 0.10`; else stop any active session, fs_unlink `/lfs/activation.bin` + `/lfs/sessions/*.dat` + `/snippets/*`, write Shadow `reported.wipe_complete=<cmd_id>` + `reported.wipe_completed_at`, echo `cmd_id` via `last_cmd_id` in next heartbeat. Cloud auto-recycles `discontinued → ready_to_provision` on ack-match within 24 h + battery floor. Replaces deprecated charger-gated reset (DL6 rewrite). Full design: `gosteady-portal/docs/specs/2026-05-17-aa-battery-recycle.md`.

### Threshold alerts (cloud-side, firmware does nothing)

`battery_pct < 0.05` → `battery_critical`; `< 0.10` → `battery_low`; `rsrp_dbm ≤ −120` → `signal_lost`; `≤ −110` → `signal_weak`. Suppressed pre-activation. **Note:** bundled fuel-gauge model is generic 1100 mAh LiPol; actual cell is LP803448 ~1300 mAh. ±5–10 % SoC error straddles 5 % alert boundary — see FMEA 3.1.

---

## Failure-mode reference (FMEA — condensed)

> Full 6-axis analysis with per-row implementation specs is preserved in git at commit **`4a36740`** (`HARDENING_FMEA.md` at that revision). What follows is the cross-axis summary + sprint status + watch items.

**Six axes** (rated Severity × Probability = Risk on 1–25 scale):

1. **Silent data loss** — single uplink rejected / row missing
2. **Permanent brick** — no recovery without physical access
3. **Battery overrun** — cuts deployment short
4. **False-positive cloud event** — wrong row / spurious alert
5. **False-negative cloud event** — missed real signal
6. **Storage / lifecycle** — gradient failures (per-partition retention/rotation policy)

### M14.5 hardening sprint — top punch-list items (Risk ≥ 12, Effort ≤ M)

| Item | Title | Risk | Status (2026-05-10) | Commit |
|---|---|---|---|---|
| 1.1 | Empty `session_start` from cold-boot motion | 20 | ✅ Shipped — retro-stamp at activity-publish time | `2e4c1e7` |
| 6.1 | Snippets retain-forever | 16 | 🟡 Boot+hourly rotation hooks shipped; capture_start hook reverted (WDT regression) | `2e4c1e7` + `5343100` |
| 1.2 | Empty `session_end` if cellular drops mid-session | 12 | ✅ Shipped (same fix as 1.1) | `2e4c1e7` |
| 1.3 | PUBACK timeout silently drops activity | 12 | ✅ Shipped — in-memory retry 1/5/15 min backoff | `2e4c1e7` |
| 6.2 | Sessions partition stale `.dat` orphans | 12 | ✅ Shipped — boot-time orphan sweep | `2e4c1e7` |
| 4.1 | Stale fault_counters from stress-testing | 10 | ✅ Shipped — reset on first activation | `2e4c1e7` |
| 4.7 | `boot_count` missing from heartbeat | 10 | ✅ Shipped + verified in Shadow | `2e4c1e7` |

### Deferred (with documented rationale)

| Item | Title | Risk | Why deferred |
|---|---|---|---|
| 2.1 | Shadow re-check on every cellular wake | 16 | Gated on cloud Phase 2A `device-shadow-handler` Lambda being live |
| 3.1 | nPM1300 LP803448-tuned battery model | 15 | Needs ~30-day bench discharge run; M14.5 soak validates if false-critical alerts fire |
| 5.1 | No fall/tipover/impact detection | 20 | Explicit M10.5 v1 anti-feature; multi-month detector dev |
| 6.3 | Telemetry queue partition implementation | 9 | Unallocated partition exists; persistent Priority-1 queue is L-effort |
| 5.3 | No cloud-side offline detection | 12 | Cloud-side (Phase 1C) — firmware can't fix |

### M14.5 watch items (validate during ≥7-day soak)

- ~~**Pre-existing 2 AT calls in `session_start`** STILL risk WDT lockup under sustained cellular contention.~~ **CLOSED 2026-05-16 in 0.10.0-at-timeout.** The conference exposure (coord §C11.2/§C11.4) reproduced the failure mode exactly as predicted: SIM exhaustion → tight PDN-reject loop starved `nrf_modem_at` → the 2 synchronous `AT+CCLK?` calls in `session_start` blocked past the 60 s WDT → 1 WDT + 1 fatal + 3 reboots. Firmware fix: bounded-timeout AT wrapper (`at_cmd_with_timeout`, 2 s) in `cellular.c`. `-ETIMEDOUT` → `-EAGAIN` → existing FMEA 1.1 retro-stamp path handles `session_start` proceeding without cellular UTC. Patch folded into existing reporter thread (RAM was 99.76 % pre-patch; dedicated worker overflowed by ~2 KB). Bench-validated on `GS9999999998` per coord §C12. Full deploy commit `95f87e6`. Real-world failure-path validation still opportunistic — will happen on next M14.5-style stress or deliberate SIM-exhaustion test.
- **nPM1300 fuel-gauge accuracy** on actual LP803448 cell. If false-critical alerts fire at non-critical SoC, kick off bench discharge (FMEA 3.1).
- **Snippet upload battery cost** under heavy cellular outage backlog (FMEA 3.2). Implement battery-floor gate if observed.
- **Cellular re-attach** behavior after long outages (FMEA 2.4). Add periodic `lte_lc_offline()`+`normal()` if modem stuck in `searching` for hours.

### Cloud-side OPEN follow-ups (not firmware to fix, but worth knowing)

- ~~`activity_reject_count` alarm subscriber (sibling to FMEA 1.1 — published metric, no alarm).~~ **DONE 2026-05-17.** Cloud-side commit `3c47f0d` (added alarm in `handler-alarms.ts`), deployed 2026-05-17 to `GoSteady-Dev-Observability`. New alarm `gosteady-dev-activity-processor-activity-reject` watches the existing EMF metric; threshold > 0 in 5 min; routes to ops topic. Catalog now at 30 Observability alarms. Captured in coord §C13.
- ~~Phase 2A `device-shadow-handler` Lambda (gates FMEA 2.1 firmware completion).~~ **DONE 2026-05-17.** Shipped as part of Phase 2A-DL deploy (coord §C17). IoT Topic Rule on `$aws/things/+/shadow/update/documents` filters for `reported.reset_complete` (legacy) AND now `reported.wipe_complete` (AA-recycle, coord §C21). FMEA 2.1's wake-time Shadow re-check is now cloud-side unblocked.
- ~~1B-rev heartbeat-processor activation-ack gaps (Gap 1 stale `activated_at` skip + Gap 2 missing `provisioned → active_monitoring` transition).~~ **DONE 2026-05-17.** Surfaced at the §C18 bench test on `GS9999999998`; closed by rewrite of `_try_activation_ack` (idempotency moved from `attribute_not_exists(activated_at)` to `attribute_exists(outstandingActivationCmds.#cid)`; status transition + `firstHeartbeatAt` stamp + paired `device.first_heartbeat` audit folded into the same UpdateItem). Three-stage Option-A synthetic validation on `GS9999999998` passed all paths. Captured in coord §C19.
- ~~AA-battery-recycle: charger-gated reset (DL6) is invalid on AA hardware.~~ **FULLY VALIDATED END-TO-END 2026-05-18** (coord §C20 directional → §C21 implementation → §C22 bench validation). Software-orchestrated wipe-ack: `POST /end-assignment` → cloud publishes `wipe` cmd → firmware wipes + acks via heartbeat `last_cmd_id` echo (Shadow ack path failed in this bench cycle due to timing — see §C22 Finding 4 — but the redundant heartbeat-ack path absorbed it cleanly, validating the §3 D2 design choice) → cloud auto-recycles `discontinued → ready_to_provision` with `battery_pct ≥ 0.10` sanity floor. Firmware `0.11.0-wipe-cmd` + cloud B-series live in dev. Full design memo: `gosteady-portal/docs/specs/2026-05-17-aa-battery-recycle.md`.
- ~~connection-coordinator Lambda to address §C22 Finding 2 (AWS IoT MQTT 3.1.1 persistent_session 1h timer).~~ **DEPLOYED + LIVE-VALIDATED 2026-05-18** (coord §C23 design → §C24 implementation + live test). Stateless Lambda subscribed to AWS IoT lifecycle events (`$aws/events/presence/connected/+`); on each firmware connect, re-publishes any outstanding `outstandingActivationCmds` / `outstandingWipeCmds` within the 24h ack window, opportunistically sweeps stale entries past it. **End-to-end latency from `POST /end-assignment` to cloud auto-recycle dropped from ~57min (race-publish bench primitive) to ~10s (production-grade).** Live test confirmed: coordinator fires on lifecycle event within ~1s of CONNECTED; firmware receives cmd within active subscription window; full wipe-ack auto-recycle completes via Shadow path. Two minor follow-ups filed: §C24 Finding 4 (coordinator not state-aware — re-publishes activate cmds even to `ready_to_provision` devices; ~10-line predicate tightening) + §C24 Finding 6 (Processing-stack should pre-create Lambda LogGroup so Audit deploy doesn't need a synthetic invoke between; ~5-line CDK addition).
- Phase 1C offline detector Lambda (`lastSeen > 2 hr`) — surfaced as gap by the conference silent-failure (cap dark for 3 days 21 hrs, no alarm fired). Coord §C11.7 has the slim-scope sketch. Could fold the stale-cmd-map sweeper (§C22 Finding 7) into the same Lambda.

---

## Known gotchas (canonical "got bitten" list)

### Build / flash

- **Board target must be `thingy91x/nrf9151/ns`.** Easy to accidentally target `nrf9151dk` — builds clean but pin assignments are wrong. Sanity check: `build/gosteady-firmware/zephyr/.config` should contain `CONFIG_BOARD="thingy91x"`.
- **`west build -p auto` doesn't pristine-rebuild** when adding new overlay or board files. Use `-p always` after `boards/*.overlay` or partition-manager Kconfig changes.
- **`west flash` with nrfutil runner hits `UICR data is not erasable`** on nearly every flash. Use `nrfjprog -f NRF91 --recover --program ... --verify --reset` instead.
- **APPROTECT lockout on freshly-rescued bridges.** First flash to nRF5340 fails with "Application core access port is protected." Fix: `west flash --recover`. Wipes bridge settings — re-toggle `BLE_ENABLED=1` in `Config.txt` after.
- **SW2-misposition + nrfjprog with wrong family flag = cascade-corruption** (lost 3 hr to this 2026-05-06). Always verify SW2 position before any `nrfjprog --recover`. Chip family in nRF Connect Programmer is the truth.
- **Bridge stops enumerating on USB-C → bridge firmware is corrupt.** Reflash from `nordic resources/.../thingy91x_nrf53_connectivity_bridge_v3.0.1.hex` with SW2 = nRF53.

### Serial / USB

- **`cat /dev/cu.usbmodem*` shows a dead console** even when the app is logging. Bridge holds RX until DTR is asserted; `cat` doesn't. Use `screen` / `minicom` / `cu` / pyserial / nRF Terminal.
- **uart1 runs at 1 Mbaud, not 115200.** Always open `*1105` at `baud=1000000`.
- **uart1 is a SHARED channel** (USB ↔ BLE). Bridge forwards 91's uart1 TX to BOTH endpoints. Host tools must skip noise lines (`STATUS active=`, `PONG`, `GOSTEADY-DUMP`, `OK started`, `OK samples=`). Reuse `pull_sessions.py::_read_protocol_line()` in any new host tool.
- **USB CDC endpoints re-enumerate on replug.** Digit count varies (`*1102` ↔ `*11102`). Always `ls /dev/cu.usbmodem*` after a physical replug.

### BLE

- **BLE compiled in, disabled at runtime by default.** Toggle `BLE_ENABLED=0`→`1` in `Config.txt` on `/Volumes/NO NAME`, eject, replug. Wiped by `west flash --recover` of nRF5340.
- **iOS system-level Bluetooth grabs the NUS slot.** "Forget This Device" in iPhone Settings if a BLE client app spins on connect.
- **Bridge uart1 baud is load-bearing.** `bridge_fw/boards/thingy91x_nrf5340_cpuapp.overlay` pins it to 1 Mbaud. Don't remove.
- **BLE writes >~240 bytes silently dropped end-to-end.** `capture.html sendCommand` chunks at ≤180 B with `writeValueWithoutResponse`. Same pattern needed in any new Web BLE client.

### Session recording (key races, all fixed)

- **Cross-session sample leak** (fixed 2026-04-22 via synchronous start-signal handshake; mirror in `session_start`).
- **`-EBADF`/HardFault on STOP** (fixed 2026-04-25 by setting `s_active=false` BEFORE raising `stop_signal`, so writer's per-sample guard discards in-flight samples).
- **Stale `stop_done_sem` race silently dropping activity uplinks** (fixed 2026-05-05 via `k_sem_reset` before raising stop_signal + bumped take timeout 5s→15s).
- **Partition-fill ENOSPC fatal-fault mid-walk** (fixed 2026-05-06 via auto-prune-on-publish + graceful ENOSPC → main-thread-polled auto-stop).

---

## Tools (`tools/`)

| Tool | Transport | Purpose |
|---|---|---|
| `capture.html` | BLE NUS via Chrome on Mac | Operator UI for sealed-cap captures. 30 preset buttons + STOP. POST-WALK modal. localStorage persistence. Served from GitHub Pages: `https://jabl1629.github.io/gosteady-firmware/tools/capture.html` |
| `control.py` | USB uart1 @ 1 Mbaud | Bench equivalent of capture.html. `start-preset`, `stop`, `status`, `raw`. |
| `pull_sessions.py` | USB uart1 @ 1 Mbaud | Post-session file pull. LIST + DUMP + optional DEL. |
| `cleanup_device.py` | USB uart1 @ 1 Mbaud | Wipe `/lfs/sessions/` before fresh capture. |
| `ingest_capture.py` | offline | Join `.dat` headers + POST-WALK notes JSON → CSV column-for-column matching annotation spreadsheet. |
| `read_session.py` | offline | Parse `.dat` file or base64 header. Validates against v1 vocabulary; reports rate, monotonicity. |
| `log_console.py` | USB uart0 @ 115200 | Continuous console logger with auto-reconnect → `logs/uart0_YYYY-MM-DD.log`. **Run continuously during bench work.** |
| `flash_cert.py` | USB uart0 (AT) | Wraps `nrfcredstore` for sec_tag 201 cert flashing. Requires bench unit running NCS `at_client` sample. |

### uart1 line protocol (same on USB + BLE NUS)

```
PING\n         → PONG\n
STATUS\n       → STATUS active={0|1}\n
START <json>\n → OK started <uuid>\n  (or ERR <reason>\n)
STOP\n         → OK samples=N\n        (or ERR not active\n)
LIST\n         → <name> <size>\n ... END\n
DUMP <name>\n  → SIZE <n>\n <n raw bytes> \nOK\n
DEL <name>\n   → OK\n                  (or ERR <reason>\n)
```

All commands `\n`-terminated (literal `0x0A`, no escape parsing).

---

## Coordination + cloud-side context

- **Coord doc (append-only joint log):** `~/Documents/gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md` _(moved out of iCloud 2026-05-23 to fix tsc/npm slowness; iCloud copy at the old path may persist for a short confidence-window before deletion)_
  - GitHub: `https://github.com/Jabl1629/GoSteadyPortal/blob/feature/infra-scaffold/docs/firmware-coordination/2026-04-17-cloud-contracts.md`
  - **Latest entry: §C47 (2026-06-18)** — **Device time-reliability design** (SNTP via `date_time` lib + cloud-ingest anchoring + AT+CCLK sanity gate) for the Jun 14–16 **2080-timestamp** incident on `GS0000000001` (roaming SIM, no NITZ → 1980 modem default → ~60 walks mis-dated 2080; data NOT lost; **design only, not implemented** — spec `gosteady-portal/docs/specs/2026-06-18-device-time-reliability.md`). **§C46 (2026-06-07)** — **Gait speed, firmware→cloud→portal** (`0.16.0-gait`): decoupled merge-0.8 step counter (raw impulse over-count 47%→16% step MAPE, slow-walk 78%→7%, zero distance cost) + new optional `gait_speed_fts` activity field (`distance_ft` ÷ peak-train walking time) + long-session guard (relaxed to peak-cap only in `0.16.1`); shipped + live on `GS0000000001`. **(prior) §C45 (2026-06-04)** — **Firmware fix: wipe→shake reactivation hang (`0.15.3-wakewindow`).** A console-attached bench repro overturned §C44.5's guess (PSM/rate-limited motion-connect — never validated): the wake window opened but **never connected**, even on USB with a warm modem. Real cause: the heartbeat thread's activated branch blind-`k_sleep(HEARTBEAT_INTERVAL)` (1 h) doesn't re-check activation, so a wipe (activated→pre-activation) mid-sleep leaves it parked in the activated branch NOT serving wake windows → a shake's blue-pulse window has no connection → queued `activate` never collected (only a reboot recovered it). Fix: interruptible activated sleep (`k_sem_take(&heartbeat_wake_sem, …)`) + `gosteady_activation_clear()` gives the sem via `gosteady_cloud_notify_deactivated()` (`cloud.c`/`activation.c`/`cloud.h`). Console-validated on `GS0000000001`: End-Monitoring → wipe → Resume → **shake reactivated with no reboot** (`preact: ACTIVATED during wake window → green confirm`), wake-window motion timeout intact (un-queued first window ended at the 600 s hard cap → ship sleep). No cloud contract change. **(prior) §C43 (2026-06-04)** — **Fuel-gauge fix: voltage-based (OCV) SoC.** A `GS9999999998` discharge run exposed the `nrf_fuel_gauge` coulomb estimate diverging to 0% at sub-mA idle (read 0% / `battery_critical` at 4.086 V ≈ 86% actual; 12% at a full 4.213 V) — battery fine, gauge broken (FMEA 3.1). Replaced with a voltage→OCV lookup (`battery.c`, unconditional; `0.12.1-psm` / `0.13.2-pilot` / `0.15.2-wakewindow`); verified 4.213 V→100%, Shadow `battery_pct=1.0`. Earlier GS98 `battery_critical` alarms were false. No cloud change. **(prior) §C42 (2026-06-04)** — **Device-lifecycle UX consolidation ("End Monitoring") + discharge-cascade wipe fix; full single-cap lifecycle validated.** Dropped "Discontinue Device", renamed "Discharge Resident" → **"End Monitoring"** (no reason — just confirm + optional notes; backend discharge `reason` now optional). Fixed the `discharge-cascade` Lambda that never queued the wipe (caps released via discharge got stuck in `discontinued`, never recycling to the pool) — now mirrors the end-assignment AA-recycle write; the §C24 coordinator delivers the wipe on next connect. **Validated the ENTIRE device lifecycle on one physical cap** (`GS0000000001`, `0.15.1-wakewindow`, unmodified): Pilot CapTest provision→activate→walk 42 steps → End Monitoring → wipe + recycle (on a shake) → Rosa Delgado re-provision→re-activate→walk 16 steps. **Zero firmware change.** **(prior) §C41 (2026-06-03)** — **First physical-device activation through the FACILITY portal, end-to-end on the live site** (`dev.portal.gosteady.co`): caregiver sign-in → Add Resident + provision `GS0000000001` → activate cmd → Jace shook the cap (0.15.0 wake-window) → `active_monitoring` → walk (42 steps / 50.09 ft / outdoor) rendered on the caregiver's screen. Phase 2B umbrella exit, hardware in the loop. **3 live-mode portal bugs fixed + deployed** (Flutter Unit-dropdown value-equality; `patient-mgmt` `status_patientId` missing on create → residents invisible to roster; + not updated on discharge). **D2C coordination follow-ups logged** (`deploy-portal.sh --delete` clobbers `/d2c/` in the shared bucket; `d2c-claim` `status_patientId` uses `active#` vs canonical `active_`; `GS0000000001` D2C↔facility device contention). **Zero firmware change** — `0.15.1-wakewindow` ran unmodified. **(prior) §C40 (2026-05-31)** — Pre-activation **shipping mode** (`0.14.0-shipmode`): gated sampler (100 Hz sampler blocks when idle instead of spinning — **soak-validated**, 16 start/stop cycles, 0 faults) + blue-on-motion pre-activation indicator + 24 h safety-net heartbeat. Cuts pre-activation storage draw ~340-410 → ~48 mAh/month (months of shelf life, no pull-tab). Flashed to `GS0000000001`; D2C staging intact. Spec: `gosteady-firmware/docs/specs/preactivation-lowpower-mode.md`. **(prior) §C39 (2026-05-31)** — `GS0000000001` flashed with `0.13.1-pilot` (client_id baked + cert sec_tag 201) and staged for the first **D2C QR-claim test**: walkerId/QR assigned, public lookup `unclaimed`, device heartbeating in pre-activation (`ready_to_provision`, owner NULL, `activated_at` None → real activate flow). Blocked only on Twilio OTP (operator) + Jace's phone-side claim. **(prior) §C38 (2026-05-31)** — Firmware battery pilot: LTE-M PSM (`0.12.0-psm`) + low-power overlay (`0.13.1-pilot`, `prj_pilot.conf`; idle dark + green session LED on). Root cause of the 30-day-budget overrun was `CONFIG_LTE_PSM_REQ` never set (modem idled at ~mA 24/7). PSM validated on `GS9999999998` (carrier granted tau=3 h/active=2 s). No cloud contract change; downlink (activate/wipe) unaffected (still delivered at next connect via the §C24 coordinator). Next: on-battery SoC-slope discharge run to measure real life. **(prior) §C37 (2026-05-31)** — D2C Phase 1 backend (walker-user claim + activation) deployed to dev. New consumer/household product on a 5-phase plan (walker-claim → SMS → deactivation/reset → new-user-reuse → caregivers); specs `d2c.md` + `d2c-phase1-walker-activation.md`. New `GoSteady-Dev-D2C-Auth` stack: separate Cognito pool (`us-east-1_bhvtxuHwD`), SMS-OTP custom-auth (via **Twilio** — dev AWS account has no SNS SMS origination), D2C pre-token. `d2c-claim` Lambda: `POST /api/v1/claim` (2nd JWT authorizer) bootstraps household+Patient+RoleAssignment then provisions the device via the existing 2A-DL activate path; public `GET /api/v1/public/walkers/{walkerId}` drives the QR `/setup` landing; new `by-walker-id` GSI (opaque QR id → serial; GS serial never exposed). Synthetic e2e all-green (3 live-AWS bugs found+fixed: empty-set, error_response arity, status_patientId GSI key). **Zero firmware-facing impact** — reuses shipped activation/provision/Shadow-desired contracts verbatim. Blocked on operator: Twilio compliance review (~2 biz days) + populate `gosteady/dev/twilio` secret before OTP sends; real-device exit test pending (decouplable from SMS via admin-minted token). Prior: §C36 (2026-05-27) FAC-W follow-up bundle.
  - **(prior) §C36 (2026-05-27)** — FAC-W follow-up bundle. Four §C35.4-deferred items closed in a single commit + one Lambda code swap: (1) Discharge reason picker now surfaces the `DischargeReason` enum + actually sends the previously-collected notes field; (2) Pause reason picker surfaces the `PauseReason` enum (days input was already wired); (3) Replace / Discontinue Device wired through new `FacilityRepository.replaceDevice` + `discontinueDevice` facades backed by 2A-DL provision + end-assignment (replaces the `UnimplementedError` stubs in ApiClient); (4) **US-31 census-tier paused-bell** landed end-to-end — `_patient_row_view` (shared by `/me/patients` and census-roster) now includes the same `notificationsPaused` projection as the detail view, threaded through `MePatientSummary` → `Patient` → subdued bell icon in both tile + list-row Census surfaces. Bonus fix: pre-existing client bug where `NotificationsPaused.fromJson` used ISO-only `_parseTs` against DDB epoch-string values, silently falling back to `DateTime.now()` and making `isActive` always false — Pause Banner remaining-days now actually renders. **Zero firmware-facing impact** — single-resource code-asset swap on `gosteady-dev-patient-api`. Prior milestones: §C35 (same day) 2B-FAC-W deployed; §C34 (same day) CORS drift recovery; §C33 (2026-05-26) alert recurrence policy; §C32 L5 throttle
- **Cloud architecture:** `gosteady-portal/docs/specs/ARCHITECTURE.md` — single source of truth for cloud-side. Phase plan, Lambda inventory, MQTT contracts, threshold + alert policy.
- **AA-battery-recycle design memo:** `gosteady-portal/docs/specs/2026-05-17-aa-battery-recycle.md` — decisions log + new MQTT contract + invariants W1-W5 + FMEA for the wipe-ack auto-recycle path that supersedes DL6 charger-gated reset.
- **Cloud-side state (2026-05-18):** Phases 0A-rev / 0B-rev / 1A-rev / 1B-rev / 1.5 / 1.6 / 1.7 deployed; Phase 2A split into 6 subsets — 2A-0 foundation + 2A-DL device-lifecycle deployed; 2A-RD / 2A-AA / 2A-UM / 2A-INT planned. AA-battery-recycle cloud B-series deployed (coord §C21). Connection-coordinator Lambda deployed + live-validated (coord §C23/§C24) — production primitive for downlink cmd reliability. 1C (offline detector), 2B (portal integration), 2C (notifications), 3A/B (hosting + CI/CD) all 🔲 planned. No firmware action required for §C24; firmware queue is open (M14.5 site-survey shakedown still the named next milestone).

### Per-Device dashboard (M14.5 daily-watch surface)

`https://console.aws.amazon.com/cloudwatch/home?region=us-east-1#dashboards:name=gosteady-dev-per-device`

Variable `serial` (PATTERN, default `GS9999999999`). Switch via dashboard UI. Widgets: Battery%, Signal RSRP+SNR, Watchdog hits + fault counters, Recent activity sessions, Recent snippet uploads, Recent synthetic + device alerts. Widget period set to 1 min as of 2026-05-10 (was 60 min — caused "dashboard appears empty" perception immediately after a heartbeat publishes).

---

## Key design decisions (still relevant)

- **Step-based, not dead reckoning** — dead reckoning gives 73 % MAPE with MEMS IMUs.
- **Auto-surface roughness** over operator-supplied surface enum — IMU-derived classifier scales to deployment surfaces continuum.
- **Single-feature stride regression** (amplitude only) — multi-feature overfit at n=8/16; revisit at v1.5 with n=24+.
- **BLE not cellular for v1 operator control** — operator always in room during capture. Cellular is for production telemetry.
- **POST-WALK in browser, not session-file header** — operator judgment, not device state. JSON sidecar joined at ingest by `tools/ingest_capture.py`.
- **Browser-side BLE chunking** (`capture.html sendCommand` ≤180 B) — short-term workaround for NUS RX → uart1 reassembly limit. Lean-protocol rewrite deferred to M12+ when production-PCB constraints inform the redesign.
- **Bridge fork vendored wholesale** into `bridge_fw/` — 264 KB of Nordic source in repo is fine; build-system fragility of patch-on-build would be worse.
- **Auto-prune on activity PUBACK** + graceful ENOSPC handling — bounds sessions partition growth without losing summary data (algo outputs in cloud, snippet on separate partition).
- **Forensics in noinit RAM, drained at next-boot init** — flash writes from `k_sys_fatal_error_handler` don't survive `sys_reboot` timing on this nRF9151+TF-M platform. `sys_reboot(SYS_REBOOT_WARM)` retains SRAM; next-boot init drains noinit into the on-flash record where flash I/O is fully ready.
- **`CONFIG_MQTT_CLEAN_SESSION=n`** — broker queues QoS 1 cmds across hourly heartbeats so cloud `activate` cmds aren't dropped.
- **LTE-M PSM over eDRX for the battery pilot** (`0.12.0-psm`, coord §C38) — request PSM at attach (TAU 3 h backstop > heartbeat; active-time 2 s so the modem sleeps ~immediately after each RRC release). PSM, not eDRX, because downlink latency of ≤ one heartbeat interval is already acceptable (cmds delivered at the next connect via the §C24 connection-coordinator), and PSM's deep-sleep floor is far lower than eDRX paging. The modem is otherwise never powered down between the connect-publish-disconnect heartbeats, so without PSM it idled in registered I-DRX at ~mA 24/7 — the single largest power overrun. Carrier grant is observable on uart0 (`psm: tau=… active=…`); fall back to `CONFIG_LTE_EDRX_REQ=y` if a pilot SIM/carrier denies PSM.
- **1.5 s linger after PUBACK** before disconnect — gives queued app-topic deliveries time to flow in before tear-down. ~0.5 mC additional cost per heartbeat.
- **Bounded-timeout AT wrapper (`at_cmd_with_timeout`, 2 s) for `session_start` CCLK calls** (`cellular.c`, 0.10.0-at-timeout) — synchronous `nrf_modem_at_*` calls can block past the 60 s WDT envelope under sustained modem contention (e.g. SIM-exhausted PDN-reject loop, as observed 2026-05-11/12 conference). Wrapper dispatches to a worker that runs INSIDE the reporter thread (RAM consolidation; dedicated thread overflowed by ~2 KB). Caller gets `-ETIMEDOUT` after 2 s instead of an indefinite block; maps to `-EAGAIN` so existing FMEA 1.1 retro-stamp handles `session_start` proceeding without cellular UTC. **Reporter-thread AT calls MUST bypass the wrapper** (use `_bare` variants like `read_network_time_iso8601_bare()`) or they self-deadlock: the wrapper gives `at_request_sem`, but the reporter is the only consumer and is blocked inside the wrapper waiting for itself. The 2 s timeout saves from a true deadlock but produces spurious `at cmd timed out (modem contention?)` warnings every cycle. See coord §C11.5 / §C12.4 for full design + the bench-discovered self-deadlock that motivated the `_bare` pattern.

---

*Per-commit detail and design rationale: `git log --oneline`. Per-day cross-team log: coord doc above. Detailed FMEA with full per-row implementation specs: git commit `4a36740`.*
