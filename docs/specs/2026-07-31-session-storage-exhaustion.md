# Session-storage exhaustion → LittleFS fault — spec + fix options

> **Status:** diagnosed, unfixed. Found 2026-07-31 on `GS0002000002` (prod
> rollator, Dorothy Iupert's unit) after a 3 day 20 hr blackout.
> **Severity: HIGH.** A cellular outage is currently sufficient to crash a
> deployed unit and destroy the data it collected while offline. This is not
> specific to one device — any unit in marginal coverage walks the same path.
> **Coord:** portal `docs/firmware-coordination/2026-04-17-cloud-contracts.md` §C62.

---

## 1. The fault (evidence, not inference)

Read live from the device's in-RAM forensics record via
`nrfutil device read --address 0x2001cfcc` (the persisted record lives in the
external-flash `crash_forensics` partition; `forensics_init` loads it into the
static `s_record` at every boot, so SWD can read it without touching flash):

```
PC        0x0005F2F2  →  lfs_file_write_ +0x6
LR        0x00059FB6  →  littlefs_write  +0x20
thread    "auto_start"
reason    41 = K_ERR_ARM_SECURE_ATTRIBUTION_UNIT
uptime    35,030,251 ms  (9 h 43 m)
boot_count 9   fault_count 2   watchdog_hits 0
```

**The device crashed inside LittleFS while writing session samples.**
`SECURE_ATTRIBUTION_UNIT` means a non-secure access hit an address the SAU
attributes to the secure world — on nRF9151 + TF-M a wild pointer surfaces as a
security violation rather than a plain bus fault.

**Timing cross-check.** The last successful heartbeat reported `uptime_s: 30424`
(8 h 27 m). The fault fired at 9 h 43 m — **~77 minutes after the device last
reached the cloud.** This frame is the blackout event, not a stale historical
fault. `watchdog_hits = 0` rules out a hang; it faulted.

State at recovery: `/lfs` **93% full** (589,824 B free of 8,388,608), and 26
`.dat` files swept at the next boot.

---

## 2. Why it filled up — the scaling law

```c
struct __attribute__((packed)) gosteady_sample {   /* session.h:168 */
        uint32_t t_ms;
        float    ax, ay, az;    /* accel, m/s^2 */
        float    gx, gy, gz;    /* gyro,  rad/s */
};                              /* _Static_assert == 28 bytes */
```

| | |
|---|---|
| Sample rate | 100 Hz |
| Per-sample | 28 B |
| **Raw rate** | **2,800 B/s** |
| `/lfs` sessions partition | 8,388,608 B (2048 × 4 KB blocks) |
| **Cumulative recording before full** | **≈ 2,995 s ≈ 50 minutes** |

**Fifty minutes of total recorded motion fills the partition** — not 50 minutes
per day, 50 minutes *ever*, if nothing is draining it. At an observed ~28 s per
session that is ~107 sessions.

### What drains it, and why nothing did

There are exactly two deletion paths:

1. **`gosteady_session_prune()` after activity PUBACK** (`session.c:768`) — the
   only routine drain, and **it requires a successful cloud publish.**
2. **`gosteady_session_orphan_sweep()` at boot** (`session.c:837`) — see §2.1.

So with no uplink there is **no drain at all**. Storage grows at 2,800 B/s of
motion until the partition is full. Dorothy's unit was at −127 dBm / SNR −10;
uploads were failing; sessions accumulated; ~50 minutes of cumulative walking
later, LittleFS faulted.

**The retention policy is the uplink.** That is the design defect.

### 2.1 `orphan_sweep` is not selective — a second, independent bug

Despite the name, the sweep deletes **every** `.dat` in `/lfs/sessions`
unconditionally — it matches the `.dat` suffix and calls `fs_unlink`. There is
no staleness, completeness, or published check:

```c
if (nlen <= 4 || strcmp(ent.name + nlen - 4, ".dat") != 0) { continue; }
...
int urc = fs_unlink(path);
```

Consequences:

- **Retention-until-PUBACK is worthless across a reboot.** Any session not
  published before the next boot is destroyed.
- Nothing ever re-reads a `.dat` to regenerate an activity payload, so the
  files have **no durability value** after the algo has run at session end.
- It destroys forensic evidence. The 26 files deleted on Dorothy's unit were
  the sessions she walked during the blackout. That data is unrecoverable.

### 2.2 The in-flight backlog is RAM-only and 4 deep

```c
#define ACTIVITY_MSGQ_DEPTH 4        /* cloud.c:177 */
```

Derived activity records queue in RAM, four deep, and do not survive reset. The
`telemetry_queue` partition (256 KB at `0xce2000`) is **carved in
`pm_static.yml` but never implemented** — `cloud.c:188` reads "survival waits on
FMEA 6.3 telemetry_queue partition implementation."

**So the offline story today is: 78 KB of raw samples retained per session that
nothing will ever read, and ~256 B of derived output that is the only thing
anyone actually wants — held in volatile RAM, four deep.** That is exactly
backwards.

---

## 3. Fix options

Options 1–4 attack storage; **Option 5 is required regardless** — a full or
corrupt filesystem must return an error, never fault.

### Option 1 — Implement `telemetry_queue`; delete `.dat` at session end ★ recommended

The algo already runs at session end (`ALGO_V1A/B/C` + `ROLL_DIST` all compute
in the auto-stop path, before any publish). Once it has run, the raw file's job
is done.

1. On session end: run the algo (unchanged), write the ~256 B derived record to
   the `telemetry_queue` partition, `fs_unlink` the `.dat` **immediately**.
2. Publish from `telemetry_queue`; delete the record on PUBACK.

| | Today | Option 1 |
|---|---|---|
| Offline cost per session | ~78 KB | ~256 B |
| Backlog capacity | 50 min of motion | 256 KB / 256 B ≈ **1,024 sessions** |
| At ~5 sessions/day | — | **~200 days offline** |
| `/lfs/sessions` peak | grows unbounded | one in-flight session (~78 KB) |
| Survives reboot | ✗ (swept at boot) | ✓ (flash-backed queue) |

**This changes the scaling law** — from O(recording time) to O(session count),
with a ~300× constant-factor win on top. It is the already-designed path
(FMEA 6.3), and it fixes the RAM-only backlog (§2.2) at the same time.

Cost: the largest change here — a flash-backed circular queue with wear
levelling, plus rework of the publish path to source from the queue.

### Option 2 — High-water retention on `/lfs/sessions` (immediate mitigation)

Before opening a session, `statvfs` the partition; if free space is below a
floor (say 25%), delete the oldest `.dat` until it isn't.

- **Pro:** small, self-contained, no format change; prevents the crash today.
- **Con:** still 2,800 B/s; still loses data, just chooses *which* (oldest)
  instead of crashing. A mitigation, not a fix.
- Pairs naturally with Option 5 and buys time for Option 1.

### Option 3 — Don't retain raw samples in field builds

Gate raw `.dat` retention behind a Kconfig (`GOSTEADY_SESSION_RAW_RETAIN`,
default `y`) and set it `n` in `prj_*_pilot` / `prj_*_field`. Bench and capture
builds keep raw for algorithm development; deployed units keep only derived
output.

- **Pro:** near-eliminates the problem in exactly the builds that ship; no new
  storage machinery.
- **Con:** forfeits post-hoc raw analysis from the field — which has already
  proven valuable (the §C52 distance re-validation used captured corpora).
  Also **depends on every algo being streaming/online**; needs confirmation
  (`ROLL_DIST` is described as a continuous vibration odometer, no FFT, no
  classifier — likely fine; `ALGO_V1A/B/C` unverified). See Q2.

### Option 4 — Condense the sample record (the "more days per MB" lever)

The 28-byte record is unpacked floats over a BMI270 whose native output is
`int16`. Fixed-point storage plus delta timestamps:

| Change | Per-sample | Rate | Time to full |
|---|---|---|---|
| Today | 28 B | 2,800 B/s | 50 min |
| 6 × `int16` + `uint16` Δt | 14 B | 1,400 B/s | 100 min |
| …plus 50 Hz | 14 B | 700 B/s | 200 min |
| …plus drop gyro (**if unused** — Q2) | 8 B | 400 B/s | 350 min |

- **Pro:** cheap, orthogonal, stacks with everything else.
- **Con:** **a multiplier, not a fix.** 4× more headroom still exhausts in
  hours-of-motion. It changes the constant, not the scaling law, and it breaks
  the `.dat` format contract with `tools/ingest_capture.py` and the annotation
  spreadsheet (`session.h` header comment) — a version bump plus host-side work.
- Precision loss is likely acceptable (int16 at the sensor's native resolution)
  but wants a parity check against the golden reference.

### Option 5 — Fail gracefully ★ required regardless of the above

The SAU fault is its own defect. Even with perfect retention, LittleFS must not
fault on a full or damaged filesystem.

- **Pre-flight guard:** `statvfs` before `session_start`; refuse to open a
  session below a hard floor and log/telemeter it, rather than writing into a
  nearly-full FS.
- **Root-cause the wild pointer.** `session.c` already handles `-ENOSPC`
  correctly (flags it, stops the session cleanly — the 2026-05-05 Phase 1.6
  follow-up), so this is *not* unhandled ENOSPC. The fault is inside LittleFS
  internals, which points at cache/buffer corruption. Needs a reproduction
  (fill the partition on the bench and write) before we can call it fixed.
- **Make `orphan_sweep` selective** (§2.1): delete only files that are
  incomplete (no valid trailer) or already published — never a wholesale
  `*.dat` unlink.
- **Surface it:** `gosteady_session_flash_full()` exists but nothing publishes
  it. Add a `storage_pct` field to the heartbeat so the fleet dashboard can see
  a unit approaching the cliff instead of discovering it after the crash.

### Recommended sequencing

1. **Option 5 guard + Option 2 high-water** — small, ships fast, stops the
   bleeding for units already in the field.
2. **Option 1** — the actual fix; retire Option 2's pruning once it lands.
3. **Option 4** — opportunistic, when the `.dat` format is next revised.
4. **Option 3** — decide as policy once Q2 is answered.

---

## 4. Open questions

| # | Question | Notes |
|---|---|---|
| Q1 | What corrupted the pointer inside `lfs_file_write_`? | Not established. Near-full is the suspect; unproven. Needs a bench repro: fill `/lfs` to ~95% and write. |
| Q2 | Are `ALGO_V1A/B/C` fully streaming, or do they need the whole session buffered? | Gates Option 3, and the "drop gyro" row of Option 4. |
| Q3 | Is gyro used by any shipping algo? | If not, 28 B → 8 B is available cheaply. |
| Q4 | Should `telemetry_queue` also carry heartbeats? | `pm_static.yml` sized it for both (24 hb/day + 5 activity/day × 30 days ≈ 210 KB). |
| Q5 | Retention floor for Option 2 — what %? | Must exceed one max-length session. Session length cap is not currently enforced. |
| Q6 | Is there a max session length? | A pathological never-stationary session could fill the FS on its own regardless of retention. |

---

## 5. Related

- **Coord §C62** — full incident writeup, portal repo.
- **§C61** — signal alerting disabled the day before; `signal_lost` was this
  unit's last transmission. Unrelated cause, adjacent symptom.
- **Cloud-side gap:** `device_silent` fired correctly at +24 h and reached
  nobody — SNS/SES delivery is still the Phase 2C stub
  (`infra/lib/stacks/notification-stack.ts` is 59 lines of TODO). The device
  was dark 3 days 20 hr before a human noticed. Fixing storage does not fix
  detection→notification.

## Changelog

| Date | Change |
|---|---|
| 2026-07-31 | Diagnosed on `GS0002000002` from the live forensics record; spec + 5 fix options drafted. Unfixed. |
