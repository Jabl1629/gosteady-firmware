# GoSteady firmware — build configurations (canonical reference)

> **This is the single source of truth for "which overlay sets which config, and
> which image is on which device."** It exists because config knowledge was
> scattered across `Kconfig`, six `prj*.conf` files, the `GOSTEADY_CONTEXT.md`
> build table, and `version.h` — and the seams caused a real diagnosis miss
> (a device running the capture image looked like it was missing basic features).
>
> **Layered ownership — do not duplicate, point here:**
> - **`Kconfig`** owns *what each `CONFIG_GOSTEADY_*` symbol does* (help text). Load-bearing; it can't silently rot because the build reads it.
> - **`prj*.conf`** overlays own *what each build enables* — they ARE the config.
> - **This doc** owns the *overlay × symbol matrix* + the *build-dir → overlay → device* map + the *version-string* mapping. Everything else links here.

---

## 1. The three layers (read this first)

A GoSteady build is determined by three stacked layers. Confusion almost always
comes from conflating them:

| Layer | Artifact | "Where do I look?" |
|---|---|---|
| **1. Symbols** | `Kconfig` — the `CONFIG_GOSTEADY_*` definitions + help | What a knob *does* |
| **2. Overlay** | `prj*.conf` (+ any `-D` flags on the west line) | What a given build *turns on* |
| **3. Image / device** | `build_*/merged.hex` flashed to a serial | What's *actually running* |

The same source tree produces every image — walker and rollator are one app with
a `CONFIG_GOSTEADY_PRODUCT_*` Kconfig choice (walker = default). Nothing is
forked; every platform mechanism is shared and inherited by construction. What
differs between builds is **only** which overlay symbols are set.

---

## 2. Kconfig symbol reference

Behaviour-defining project symbols (full help in [`Kconfig`](../Kconfig)):

| Symbol | Default | What it does |
|---|---|---|
| `GOSTEADY_PRODUCT_WALKER_CAP` / `_ROLLATOR` | walker | Product choice → version line, heartbeat `device_type`, activity metric set |
| `GOSTEADY_CLOUD_ENABLE` | n | Compiles `cloud.c`; enables AWS IoT heartbeat/activity/cmd path |
| `GOSTEADY_FIELD_MODE` | n | Deployment posture: SW0 / uart1 dump / idle+recording LEDs / uart0 heartbeat line all **off** (device runs dark; pre-activation blue LED remains) |
| `GOSTEADY_MOTION_AUTOSTART` | **y** | ADXL367 wake-on-motion → BMI270 confirm → auto session start (the autowake + green-LED-on-motion behaviour). Set **n** only on operator-driven capture images |
| `GOSTEADY_SNIPPET_ENABLE` | n | 30 s raw-IMU snippet capture + opportunistic upload (needs `CLOUD_ENABLE`) |
| `GOSTEADY_FORENSICS_ENABLE` | n | Crash forensics + 60 s hardware watchdog + supervisor thread |
| `GOSTEADY_LOW_POWER` | n | Battery cadence relax: fuel-gauge 5 s→60 s, cellular reporter poll 60 s→30 min |
| `GOSTEADY_SESSION_LED` | n | Green LED solid while recording, even under `FIELD_MODE` (operator visibility) |
| `GOSTEADY_PREACT_LOWPOWER` | n | Pre-activation shipping mode: shake→blue wake-window→activate, 24 h safety-net heartbeat, transit back-off |
| `LTE_PSM_REQ` | n (Zephyr) | Request LTE-M PSM at attach — modem deep-sleeps between heartbeats (biggest battery win) |
| `DATE_TIME` | n (Zephyr) | NCS `date_time` lib: NITZ→NTP→app-set (the 2080-timestamp fix) |
| `PM` | y (in `prj.conf`) | Zephyr system power management (System-ON idle) — base, all builds |

**Always-on, not overlay-gated** (shared code, present in *every* build — don't
hunt for a flag): the gated idle sampler, voltage/OCV battery SoC, boot-time
orphan sweep. Auto-prune-on-PUBACK is shared but only fires when `CLOUD_ENABLE`.

---

## 3. Overlay × symbol matrix

✅ = set `y` · — = unset/default-off · Rollator overlays mirror their walker
sibling **except snippets** (rollator = corpus-over-uart1, and RAM).

### Walker overlays

| Symbol | `prj.conf` | `prj_cloud` ⚠ | `prj_field` | `prj_pilot` ◄ships |
|---|:--:|:--:|:--:|:--:|
| `PRODUCT_ROLLATOR` | — | — | — | — |
| `CLOUD_ENABLE` | — | ✅ | ✅ | ✅ |
| `FIELD_MODE` | — | — | ✅ | ✅ |
| `MOTION_AUTOSTART` | ✅ | ✅ | ✅ | ✅ |
| `SNIPPET_ENABLE` | — | ✅ | ✅ | — |
| `FORENSICS_ENABLE` | — | ✅ | ✅ | ✅ |
| `LTE_PSM_REQ` | — | ✅ | ✅ | ✅ |
| `DATE_TIME` | — | ✅ | ✅ | ✅ |
| `LOW_POWER` | — | — | — | ✅ |
| `SESSION_LED` | — | — | — | ✅ |
| `PREACT_LOWPOWER` | — | — | — | ✅ |
| → version suffix | `-psm`¹ | `-psm`¹ | `-psm`¹ | `-wakewindow` |

⚠ `prj_cloud.conf` **does not link at 0.17.0** — RAM overflows by 1296 B
(snippets ON + DBG log). Pre-existing; recent walker units all ship `prj_pilot`
(snippets off). See coord §C49.

### Rollator overlays

| Symbol | `prj_rollator_cloud` | `prj_rollator_field` ✨new | `prj_rollator_pilot` ✨new | capture¹ (on GS…81) |
|---|:--:|:--:|:--:|:--:|
| `PRODUCT_ROLLATOR` | ✅ | ✅ | ✅ | ✅ |
| `CLOUD_ENABLE` | ✅ | ✅ | ✅ | — |
| `FIELD_MODE` | — | ✅ | ✅ | — |
| `MOTION_AUTOSTART` | ✅ | ✅ | ✅ | **— (off)** |
| `SNIPPET_ENABLE` | — | — | — | — |
| `FORENSICS_ENABLE` | ✅ | ✅ | ✅ | — |
| `LTE_PSM_REQ` | ✅ | ✅ | ✅ | — |
| `DATE_TIME` | ✅ | ✅ | ✅ | ✅ (NITZ-only) |
| `LOW_POWER` | — | — | ✅ | — |
| `SESSION_LED` | — | — | ✅ | — |
| `PREACT_LOWPOWER` | — | — | ✅ | — |
| → version suffix | `-bench`¹ | `-bench`¹ | `-ww` | `-bench`¹ |

`prj_rollator_field` / `prj_rollator_pilot` were added 2026-07-06 to carry the
walker's deployment + battery/shipping posture forward. **Status: authored; see
§6 for build/validation state.** The capture image (`build_rollator_dist`) is a
data-collection build (base `prj.conf` + `-D` flags), **not** an overlay — it
deliberately disables `MOTION_AUTOSTART` and cloud so operator captures aren't
hijacked and `.dat`s aren't auto-pruned (coord §C50).

---

## 4. Build-dir → overlay → device → purpose

The build the west command produces, and where it goes. (Per-device *live* state
is tracked in [`GOSTEADY_CONTEXT.md`](../GOSTEADY_CONTEXT.md) → Dev units — that
table is authoritative for "what's flashed right now.")

| Build dir | Overlay / flags | Product | Purpose |
|---|---|---|---|
| `build/` | `prj.conf` | walker | M8 bench data-collection (no cloud). Autowake on |
| `build_cloud/` | `prj_cloud.conf` | walker | Bench + cloud. ⚠ broken at 0.17.0 (RAM) |
| `build_field/` | `prj_field.conf` | walker | Deployment image (dark, corpus snippets on) |
| `build_pilot/` | `prj_pilot.conf` | walker | **Walker ships this** — battery pilot (GS0000000001/98) |
| `build_rollator_gs81/` | `prj_rollator_cloud.conf` | rollator | Bench + cloud product build (autowake + green LED on) |
| `build_rollator_field/` | `prj_rollator_field.conf` ✨ | rollator | Rollator deployment baseline (dark) |
| `build_rollator_pilot/` | `prj_rollator_pilot.conf` ✨ | rollator | **Rollator deployment target** — battery + shipping mode |
| `build_rollator_bench/` | `prj.conf` + `-DPRODUCT_ROLLATOR -DDATE_TIME -DDATE_TIME_NTP=n -DMOTION_AUTOSTART=n` | rollator | DT-2 capture day (no cloud, no autoprune) |
| `build_rollator_dist/` | as bench + `gs_rollator_distance` | rollator | **Currently on GS9999999981** — distance-capture image |

> **Why the bench unit shows "no green LED on motion":** GS9999999981 runs
> `build_rollator_dist` (capture), where `MOTION_AUTOSTART=n` by design — motion
> does not open a session, so no green. The rollator *product* builds
> (`prj_rollator_cloud/field/pilot`) autowake + light green normally. (Separately,
> the uart1/BLE operator-START path has never driven the recording LED — only the
> motion-autostart and SW0 paths in `main.c` do.)

---

## 5. Version-string ambiguity (known issue + proposed fix)

`src/version.h` keys the suffix on **power-mode only**, so it does **not**
distinguish cloud / field / capture builds. Two collisions today:

- `rol-0.1.0-bench` = `prj_rollator_cloud` **and** `prj_rollator_field` **and**
  the capture image — three very different images, one string. You cannot tell
  from telemetry which one a unit runs.
- `0.17.0-time-psm` = walker `prj.conf` **and** `prj_cloud` **and** `prj_field`
  (and "psm" is misleading for the no-PSM base build).

**Proposed minimal fix** (not yet applied — `firmware_version` feeds cloud cohort
dashboards, so it's a cross-repo surface worth an explicit sign-off): give the
no-cloud capture image its own tag, since `!CLOUD_ENABLE` is its defining trait
and it's the image that caused the confusion —

```c
#if !defined(CONFIG_GOSTEADY_CLOUD_ENABLE)
  #define ..._SUFFIX "cap"     // rol-0.1.0-cap / 0.17.0-time-cap
#elif defined(CONFIG_GOSTEADY_PREACT_LOWPOWER) ...
```

A fuller scheme could also fold `FIELD_MODE` into the suffix (bench vs field),
but mind the length caps: `firmware_version[32]` (cloud.h) and the `.dat`
header's `firmware_version[16]` (session.h) silently truncate.

---

## 6. Build & validation state

| Overlay | Built? | RAM | Notes |
|---|---|---|---|
| `prj_rollator_cloud.conf` | ✅ 2026-07-02 (coord §C49) | 62.6 % | Snippets off; DBG on |
| `prj_rollator_field.conf` ✨ | ⚙ by implication | < 63 % | Strict subset of pilot (no `LOW_POWER`/`SESSION_LED`/`PREACT`); not separately built, but fits since the superset does |
| `prj_rollator_pilot.conf` ✨ | ✅ 2026-07-06 | **63.05 %** (143,760 / 227,992 B) | Links clean; flash 26.16 %; ELF carries `rol-0.1.0-ww` + `rollator_platform`. +1.1 KB RAM vs cloud (the battery/shipping code). `PREACT` motion thresholds walker-tuned → runtime bench validation still needed (DT-3, spec A3) |

`prj_rollator_pilot.conf` **compiles, links, and fits** (verified 2026-07-06;
RAM 63.05 %, `rol-0.1.0-ww` baked). `prj_rollator_field.conf` is a strict subset
(pilot minus three flags) so it fits by implication. What's **not** yet validated
is *runtime behavior on hardware*: neither overlay has been flashed, and
`PREACT_LOWPOWER`'s wake-on-motion sensitivity is tuned for walker lift-and-place,
not rollator wheel vibration — quantify at bench before any real rollator
deployment (memo Q3 / spec A3, a DT-3 item).

---

## 7. Mirror-discipline liability

sysbuild forwards only one `EXTRA_CONF_FILE` cleanly, so each overlay is a
**self-contained copy** of the shared cloud/PSM/fuel-gauge/forensics block. That
block now lives in **six** files (`prj_cloud`, `prj_field`, `prj_pilot`,
`prj_rollator_cloud`, `prj_rollator_field`, `prj_rollator_pilot`). Change one →
mirror the others, and update the §3 matrix. If this duplication keeps growing,
consider a generator that renders the matrix from the `.conf` files (so it can't
drift) or a build-system change to layer fragments — both were deferred as
larger changes than the doc.

---

*Related: [`Kconfig`](../Kconfig) · overlays `prj*.conf` · [`GOSTEADY_CONTEXT.md`](../GOSTEADY_CONTEXT.md) (dev-unit live state) · `src/version.h` (cascade + changelog).*
