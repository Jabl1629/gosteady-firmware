# GoSteady Firmware Hardening — 6-Axis FMEA

**Date:** 2026-05-10
**Author:** firmware-Claude (independent pass)
**Target milestone:** M14.5 site-survey shakedown → M15 clinic deployment
**Scope:** firmware only (cloud-side gaps flagged but not enumerated)

---

## Rating Scales

**Severity (S, 1–5)** — what hurts if it happens:

| S | Meaning |
|---|---|
| 5 | Permanent brick / total silent data loss / no recovery without physical access |
| 4 | Multi-day data loss / patient timeline corruption / device "alive but mute" / recoverable only via portal action |
| 3 | Single session lost / single false alert / battery deployment cut by days |
| 2 | Per-event minor degradation / observable in dashboards / operator confusion |
| 1 | Cosmetic / verbose logs |

**Probability (P, 1–5)** — likelihood during a 30-day clinic deployment:

| P | Meaning |
|---|---|
| 5 | Will happen multiple times in every deployment |
| 4 | Will likely happen at least once during 30-day deployment |
| 3 | Possible during 30-day deployment, depends on environment / cellular / patient |
| 2 | Edge case requiring specific conditions |
| 1 | Theoretical / requires firmware bug not yet observed |

**Effort to close (E, S/M/L/XL):**

| E | Meaning |
|---|---|
| S | < 2 hours, single file, isolated change |
| M | half-day, 2–3 files, may need bench validation |
| L | 1–2 days, new module / partition policy / test infrastructure |
| XL | > 2 days, architectural change |

**Risk = S × P** (1–25). Roughly: 16+ = critical for M14.5 / 9–15 = should fix / 5–8 = consider / ≤4 = accept.

---

## Axis 1 — Silent Data Loss

*Single-event failures where data the device captured doesn't make it into the patient timeline, with no operator-visible signal.*

### 1.1 Empty `session_start` when motion fires before LTE-M attach ⚠️ **FOUND 2026-05-10**

**Scenario:** Cold boot → walker is moved within ~700 ms (auto-start fires) → cellular still attaching → `gosteady_cellular_get_network_time()` returns `-EAGAIN` → `s_session_start_utc_iso[0] = '\0'` → activity payload publishes `"session_start":""` → cloud activity-processor rejects `bad_timestamp:Invalid isoformat string: ''` → row never lands in DDB → "Recent activity sessions" widget empty → no alarm fires (cloud `activity_reject_count` has no alarm subscriber, separate cloud-side gap).

**Where:** [src/session.c:496-512](src/session.c) (capture at start), [src/cloud.c:776-783](src/cloud.c) (publish empty string).

**Detection on firmware side:** WRN at session_start: `cellular UTC unavailable (-11) — activity will publish without session_start`. Visible in uart0 log but not surfaced to cloud.

**Detection on cloud side:** activity-processor logs `activity_reject` at WARNING level. Lambda returns 200 OK → no alarm.

**S=4** (every cold-boot mid-motion loses that session permanently from patient record)
**P=5** (every cold boot or battery swap; every boot-on-bench gives motion event from being plugged in)
**E=S** (in `session_stop` body, if `s_session_start_utc_iso[0] == '\0'`, recompute from `cellular_get_network_time_unix_ms() - (k_uptime_get_32() - s_session_start_uptime_ms)` which uses the stop-time UTC minus the session duration to back-calculate start)
**Risk = 20** ← **highest in axis**

### 1.2 Empty `session_end` if cellular drops between session_start and stop

**Scenario:** Cellular registers at session_start (UTC stamped fine), then drops mid-session (PSM cycle, RRC release into deep idle, brief coverage glitch), then at `session_stop()` call `gosteady_cellular_get_network_time()` returns `-EAGAIN` again → activity publishes `"session_end":""` → same cloud rejection.

**Where:** [src/session.c:663-668](src/session.c).

**S=4** (same as 1.1 — silent drop)
**P=3** (less common than 1.1; most sessions stay attached for tens of seconds, but possible during deep PSM)
**E=S** (same fix shape — if cellular_get returns error at stop, fallback to a synthesized `session_end = session_start + session_duration_uptime_ms`)
**Risk = 12**

### 1.3 PUBACK timeout silently drops activity, no retry

**Scenario:** Activity payload built and `aws_iot_send` succeeds, but PUBACK doesn't arrive within `PUBACK_WAIT=30s` (cellular flap, broker glitch, TLS reauth issue) → `connect_publish_disconnect` returns `-ETIMEDOUT` → activity worker logs WRN "session file still on flash; will not retry" → activity row dropped permanently → .dat file is NOT pruned (since prune is gated on PUBACK success), so it's preserved on flash for offline analysis but never lands in DDB.

**Where:** [src/cloud.c:861-866](src/cloud.c). Single-shot per session_stop.

**S=4** (lost session in cloud; .dat survives so v1.5 retrain corpus is intact)
**P=3** (cellular instability during the 30 s PUBACK window)
**E=M** (re-enqueue to s_activity_msgq on PUBACK timeout, OR on next boot scan /lfs/sessions for orphan .dats and republish; either approach needs a dedup tag — `session_uuid` works)
**Risk = 12**

### 1.4 Activity msgq overflow drops session

**Scenario:** `ACTIVITY_MSGQ_DEPTH=4`. If 5 sessions close back-to-back without successful publish (cellular outage), the 5th `k_msgq_put` returns `-ENOMSG` → session.c logs WRN, activity dropped. The .dat survives but nothing triggers a republish.

**Where:** [src/cloud.c:107](src/cloud.c) (queue depth), [src/session.c:695-697](src/session.c) (drop site).

**S=3** (single session lost; clinic walks not usually back-to-back)
**P=2** (would need cellular outage + ≥5 sessions in flight; possible but uncommon)
**E=M** (boot-time orphan-.dat sweep that re-enqueues from disk would also fix 1.3)
**Risk = 6**

### 1.5 Snippet capture orphan-bin loss when sidecar missing

**Scenario:** Boot-during-snippet-capture (HardFault, WDT, power glitch during a session) → `.bin` exists but `.json` sidecar never written → next-boot snippet upload finds `.bin` without sidecar and `fs_unlink`s it (per the M12.1f orphan-cleanup fix). 30 s of raw IMU lost.

**Where:** [src/snippet.c:399-410](src/snippet.c).

**S=2** (one snippet lost; v1.5 retrain corpus shrinks by a 30 s window per fault)
**P=3** (faults are rare but a snippet capture is in progress for ~30 s of every session, so the *window* is ~30/session-duration)
**E=M** (synthesize sidecar from `.bin`'s `window_start_uptime_ms` + best-effort wall-clock estimate using boot-time UTC minus uptime delta — adds ~50 lines; OR just accept and document as v1 limitation)
**Risk = 6**

### 1.6 LittleFS journal corruption from physical events

**Scenario:** Cap is dropped, banged, or detached mid-write → power glitch on the GD25LE255E SPI NOR → LittleFS journal gets corrupted at a metadata block → on next mount, fs_mount returns error → main.c logs "session logging disabled" → no /lfs/sessions writes ever, no boot_count, no activation.bin.

**Where:** [src/main.c:200-204](src/main.c). LittleFS resilience is high but not bulletproof against journal-block hardware failure.

**S=5** (could lose `/lfs/activation.bin` → device falls back to pre-activation forever; effectively bricked from product perspective)
**P=2** (LittleFS is robust; physical hardware events on a sealed cap should be rare)
**E=L** (move activation.bin to a separate non-LittleFS region; OR mirror to crash_forensics partition reserved bytes — see Axis 6.8)
**Risk = 10**

### 1.7 Activity rejected if cloud schema later tightens

**Scenario:** Cloud team adds a required field or tightens a range validation in the activity-processor (e.g., requires `surface_class` to be present). Firmware doesn't send → silent rejection, no firmware-side observability.

**Where:** Outside firmware, but firmware is the affected surface.

**S=3** (firmware-side invisible until cloud team flags it)
**P=2** (cloud team coordinates well, but precedent exists for under-tested schema changes)
**E=0** (depends on the contract change; firmware does have all current optional fields populated today)
**Risk = 6**

---

## Axis 2 — Permanent Brick

*Failures where the device cannot recover without physical access (USB, J-Link, charger, RMA).*

### 2.1 Activate cmd never received → permanent pre-activation lock

**Scenario:** `CONFIG_MQTT_CLEAN_SESSION=n` lets AWS IoT broker hold queued QoS 1 cmds across hourly heartbeats. BUT per AWS IoT docs, broker session state expires after ~1 hour of idle. If cloud team manually publishes activate cmd while device is between hourly heartbeats AND session expires before next CONNECT, the cmd is lost. Cloud's `outstandingActivationCmds` map (which would let heartbeat-processor confirm-on-ack) is currently DORMANT (Phase 2A device-api Lambda doesn't populate it). Recovery: cloud-side reissue OR firmware-side Shadow `desired.activated_at` re-check (also deferred per coord §F7.5).

**Where:** [src/cloud.c:1099-1109](src/cloud.c) (subscribe), [src/cloud.c:474-475](src/cloud.c) (1.5s linger).

**S=4** (recoverable but operational pain; multiple manual interventions per device possible)
**P=4** (today's bench unit has activated_at from earlier test, but first-time activation for `GS0000000001/2/3` is the high-risk window — every shipping unit goes through this once)
**E=M** (firmware-side: send Shadow.GET on every cellular wake, check `desired.activated_at`, treat as canonical truth — roughly the M12.1e.2 design that was deferred. Half-day to wire up, gated on cloud's device-shadow-handler being live which is Phase 2A)
**Risk = 16** ← **highest in axis**

### 2.2 LittleFS unrecoverable mount failure

**Scenario:** Per 1.6, /lfs fails to mount → "session logging disabled" → no boot_count, no sessions, no activation.bin. In FIELD_MODE the activation gate refuses session_start → device is alive (heartbeats, fault counters), but produces zero patient data.

**Where:** [src/main.c:200-204](src/main.c).

**S=5** (deployed device produces no patient data; cloud sees only heartbeats)
**P=2** (LittleFS corruption from power glitch, bad NOR sector, or physical event)
**E=L** (could attempt format-on-mount-fail — risky because format wipes activation.bin; better: mirror activation to crash_forensics partition so re-format is safe)
**Risk = 10**

### 2.3 Watchdog + fault handler infinite reboot loop

**Scenario:** Deterministic crash bug ships in firmware (novel fault, not caught in M10.7.3 stress-test). Each cycle: boot → fault → fault handler stamps noinit + reboot → boot → fault → repeat. Each cycle ~30 s. In 24h that's 2880 reboots = 2880 forensics flash writes (~0.4 year wear lifetime on the 4 KB block, vs designed ~270 years). Also fills cloud Shadow with `reset_reason=SOFTWARE` updates.

**Where:** [src/forensics.c:262-300](src/forensics.c) (handler).

**S=5** (deployed device in fault loop = bricked from value perspective, plus accelerated forensics partition wear)
**P=2** (only with deterministic crash; M10.7.3 stress-tested for known cases)
**E=M** (rate-limit fault handler — if `s_pending.fault_inc > 5` within some window, set a "safe mode" flag in noinit that disables the faulting subsystem on next boot. Half-day to design + bench validate)
**Risk = 10**

### 2.4 Cellular modem never re-attaches after extended outage

**Scenario:** Clinic loses LTE-M coverage for hours (construction, weather, carrier maintenance). Modem enters searching state. `lte_lc_connect_async` is called once at boot; no firmware-side periodic detach/reattach if modem is stuck. Could enter a state where modem is "alive at AT level" but never re-registers even after coverage returns.

**Where:** [src/cellular.c:261-289](src/cellular.c) — only one connect call in `gosteady_cellular_start()`.

**S=4** (alive but mute — same observable pattern as 2.1)
**P=2** (clinic coverage usually stable; long outages possible)
**E=M** (add periodic check in cellular reporter thread: if `nw_reg_status` has been "searching" for >N hours, force `lte_lc_offline()` + `lte_lc_normal()` to re-attempt. Half-day)
**Risk = 8**

### 2.5 Modem firmware fault on init → permanently no cellular

**Scenario:** `nrf_modem_lib_init` fails. Logs `"modem firmware OK?"` and returns. No retry. Device boots into "alive but no modem" state — heartbeat thread blocks forever in `wait_for_cellular_ready()`.

**Where:** [src/cellular.c:267-272](src/cellular.c).

**S=5** (no recovery without physical reflash; cloud sees nothing)
**P=1** (transient init failure rare; modem firmware was already validated in M12.1a bring-up)
**E=M** (add retry-with-backoff for `nrf_modem_lib_init` — try 3 times, exponential backoff, then give up and log)
**Risk = 5**

### 2.6 AWS IoT cert expiration

**Scenario:** Cert minted via `aws iot create-keys-and-certificate` is valid by default until 2049. But if cert is later revoked (e.g., security incident in shared infra) or if AWS short-cycles cert validity, device disconnects with no recovery without reflash.

**Where:** Out of firmware scope but firmware feels the impact.

**S=5** (no recovery without physical reflash)
**P=1** (within v1 30-day deployment; long-term concern)
**E=XL** (need cert rotation + OTA infra)
**Risk = 5**

### 2.7 nPM1300 PMIC fault triggers power-cycle loop

**Scenario:** I²C bus to nPM1300 glitches → PMIC enters fault state → device power-cycles. If recurrent (deterministic hardware issue) → boot loop without device-side observability beyond reset_reason=PIN/POR pattern.

**Where:** Hardware-level; firmware just sees reset_cause.

**S=4** (boot-loop similar to 2.3 but at hardware level)
**P=1** (nPM1300 hardware faults are rare on dev kit)
**E=XL** (hardware-level diagnosis; could need PCB rev for production)
**Risk = 4**

---

## Axis 3 — Battery Overrun

*Failures that cut the 30-day deployment short. Battery capacity is 1300 mAh on LP803448; projected average draw is ~3 mA = ~18 days. We have ~12 days margin against the 30-day target — anything that costs >1 mA average is significant.*

### 3.1 nPM1300 fuel gauge model mismatch — false battery_critical

**Scenario:** Battery model in [src/battery_model.inc](src/battery_model.inc) is the bundled "Example" 1100 mAh LiPol from upstream NCS sample. Real cell is LP803448 ~1300 mAh. ±5–10% absolute SoC error per code comments. Cloud's `battery_critical` alarm fires at `battery_pct < 0.05`. False-critical at 8–10% actual SoC is plausible. → Caregiver gets paged "battery critical" while device has 100+ mAh remaining = ~3 days of life. Erodes trust + may force premature retrieval.

**Where:** [src/battery.c:73](src/battery.c) (model load), [src/battery_model.inc](src/battery_model.inc).

**S=3** (false alert + premature swap; not an actual brick)
**P=5** (the model mismatch is structural; will affect every shipping unit)
**E=L** (capture LP803448 discharge curve over a 30-day bench discharge; build tuned model. ~2 days bench time + analysis)
**Risk = 15** ← **highest in axis**

### 3.2 Snippet upload backlog drains battery during cellular instability

**Scenario:** Clinic loses cellular for a few hours, snippet captures continue, backlog grows. When cellular returns, FIFO drain at 1-snippet-per-hourly-heartbeat: 24 backlogged snippets = 24 hours of catch-up. Each upload is ~84 KB at ~50 kbps LTE-M = ~14 s of transmit at peak ~100 mA = ~0.4 mAh per snippet. Sustained heavy use = ~10 mAh/day = ~7-day deployment cut over a 30-day window.

**Where:** [src/cloud.c:455-461](src/cloud.c) (drain hook in heartbeat path).

**S=3** (deployment cut by ~5-7 days under heavy backlog conditions)
**P=3** (depends on cellular reliability + capture rate; clinic walks generate snippets regardless of upload success)
**E=M** (implement battery_pct >= 30% gate per M10.5 spec — currently always-uploads. Half-day)
**Risk = 9**

### 3.3 BMI270 stuck-on after suspend failure

**Scenario:** `bmi270_set_active(false)` returns error (sensor_attr_set fails). Chip stays at ~325 µA continuous (vs ~0.5 µA suspended). 325 µA × 24h × 30d = 234 mAh = 18% of battery. Cuts deployment by ~5 days.

**Where:** [src/main.c:288-307](src/main.c) (suspend path), no retry.

**S=3** (~5-day deployment cut)
**P=1** (suspend rarely fails; chip is responsive at SPI level)
**E=S** (retry with backoff in `bmi270_set_active`; LOG_ERR if all retries fail so operator notices)
**Risk = 3**

### 3.4 ADXL367 false-positive chatter wakes BMI270 confirmation needlessly

**Scenario:** Walker on vibrating surface (washing machine nearby? HVAC?) → ADXL367 hits 50 mg threshold + 100 ms hold → BMI270 wakes for 500 ms confirmation → σ < 0.05 g rejection → suspend → repeat. Each false wake costs ~6 mC (12 mA × 500 ms). 100 false wakes/day = ~0.2 mAh/day. Negligible per event but accumulates if environment is consistently noisy.

**Where:** [src/main.c:551-643](src/main.c) (auto_start_entry), [prj.conf:158-161](prj.conf) (ADXL367 thresholds).

**S=1** (negligible battery cost)
**P=4** (likely in some clinic environments)
**E=M** (raise ACTIVITY_TIME to 200 ms or ACTIVITY_THRESHOLD to 100 mg if field data shows chatter)
**Risk = 4**

### 3.5 Hourly heartbeat retry burst on outage

**Scenario:** Cellular fails → heartbeat retries at 1/5/15 min → 3 full TLS handshakes + connect failures = ~3× single-heartbeat current. Single heartbeat ≈ 50 mC; 3 retries ≈ 150 mC = 0.04 mAh per failed hour. Over 30-day with 10% failure rate ≈ 1 mAh extra. Negligible.

**S=1**, **P=3**, **Risk = 3**

### 3.6 PSM not granting active timer (active=-1)

**Scenario:** Per cellular.c log today: `psm: tau=3240 s, active=-1 s` — network rejects active timer request. Modem transitions to PSM immediately after RRC release. Each tx requires re-attach handshake (~1-3 s at higher current). 24 heartbeats/day × 5 s extra × ~50 mA ≈ 0.07 mAh/day. Negligible.

**S=1**, **P=4**, **Risk = 4**

### 3.7 Cellular RRC active timer hold (post-PUBACK linger)

**Scenario:** [src/cloud.c:475](src/cloud.c) `k_sleep(K_MSEC(1500))` after PUBACK, before disconnect. Per coord §F7.4, costs ~0.5 mC additional per heartbeat = 0.012 mAh/day. Bounded by the 24 heartbeats. Negligible per coord doc estimate. Worth confirming after M14.5 measurements.

**S=1**, **P=5**, **E=S**, **Risk = 5** (low priority but trivially measurable)

---

## Axis 4 — False-Positive Cloud Event

*Wrong row in patient timeline, spurious alert, operator confusion.*

### 4.1 fault_counters carry stale stress-test values forever

**Scenario:** Today's heartbeat shows `{fatal:3, asserts:0, watchdog:5}` from M10.7.3 + M12.1e.2 testing on bench unit. These never reset (forensics persists across reset by design). The `device-watchdog-hits-rate` cloud alarm is rate-of-change so a static value won't fire. BUT operator looking at dashboard sees "5 watchdog hits" and infers current state is unhealthy. Also: cloud Shadow merge keeps these values forever; old fault counters from before activation are visible alongside current state.

**Where:** [src/forensics.c:464-470](src/forensics.c) (no reset path).

**S=2** (operator confusion; potentially incorrect triage)
**P=5** (every shipping unit will accumulate stress-test counters during M14.5 unit shakedown if we keep doing CRASH/STALL)
**E=S** (add `gosteady_forensics_reset_counters()` call from a one-shot path triggered by first activation, OR from a bench-only `cleanup_device.py --forensics-reset` that runs as part of M14.5 ship checklist)
**Risk = 10**

### 4.2 Activity row with `surface_class=indoor` for outdoor walks (or vice-versa)

**Scenario:** M9 algo classifier accuracy at n=16 cal data: 15/16 correct. 1 misclassification = +63.9% prediction error on that walk. Patient timeline shows wrong surface label; dashboard categorization slightly off. Already accepted per M10.5; documented for v1.5 retrain.

**Where:** [src/algo/gs_pipeline.c](src/algo/gs_pipeline.c).

**S=2** (single walk mislabeled; doesn't affect distance much)
**P=4** (n=16 cal data; deployment will see new surfaces)
**E=L** (deferred to v1.5 retrain per M10.5)
**Risk = 8** (accepted as v1 limitation)

### 4.3 Reset reason "POWER_ON" indistinguishable from suspicious reboot

**Scenario:** Forensics formats `RESET_POR` as "POWER_ON". Could be: legitimate first boot, OR cell battery removal, OR PMIC reset, OR JTAG reset. Cloud can't distinguish.

**Where:** [src/forensics.c:155-195](src/forensics.c) `format_reset_reason`.

**S=2** (operational signal noise; could mask real issues)
**P=4** (every battery swap or cell removal — happens at deployment start, possibly mid-deployment if battery low)
**E=S** (cloud-side: compare boot_count delta vs heartbeat_count delta to detect "device rebooted unexpectedly". Firmware-side: include boot_count as heartbeat extra so cloud has the signal)
**Risk = 8**

### 4.4 First-boot heartbeat has uptime_s < 60 — looks unstable

**Scenario:** Today's heartbeat: `uptime_s=12` because publish happened 12 s after boot. Cloud could (incorrectly) interpret as "device rebooted recently" if alarm rules later add this. Currently no alarm keys on uptime so safe today.

**Where:** [src/cloud.c:621-622](src/cloud.c).

**S=1** (informational pattern only)
**P=5** (every cold boot)
**E=0** (working as designed; cloud-side could interpret correctly)
**Risk = 5**

### 4.5 Algo overflow flag → distance=0 in activity row

**Scenario:** `gs_pipeline_finalize` sets `buffer_overflowed=1` if session exceeds buffered cap. Activity uplink publishes `distance_ft=0, steps=0`. Cloud row shows patient walked nothing.

**Where:** [src/session.c:622-641](src/session.c) (logs overflow flag), [src/cloud.c:794-797](src/cloud.c) (publishes the 0).

**S=3** (lost session data permanently in cloud; .dat is fine for offline reanalysis)
**P=2** (depends on actual buffer cap; need to check — typical clinic walks <60 s, cap is likely much higher)
**E=M** (handle overflow: publish best-effort partial outputs; OR document the limit)
**Risk = 6**

### 4.6 last_cmd_id stale echo after 24h matching window

**Scenario:** After successful activation, `last_cmd_id` remains in heartbeat extras forever. Cloud's matching window is 24 hours (D14a). After 24h, persisted cmd_id is "stale"; firmware keeps echoing → heartbeat-processor logs `heartbeat_with_unknown_cmd_id` (saw this today).

**Where:** [src/cloud.c:626-634](src/cloud.c).

**S=1** (logged WARNING only; no functional impact)
**P=5** (every device after activation)
**E=S** (clear `s_last_cmd_id` 24h after set; OR cloud-side ignore stale silently)
**Risk = 5**

### 4.7 Boot_count not in heartbeat → cloud can't correlate forensics

**Scenario:** boot_count is in /lfs/boot_count and forensics record but not in heartbeat extras. Cloud sees fault counter increments but can't correlate with boot events (e.g., "did fault_count change BETWEEN boots, or during this boot?").

**S=2** (operational triage harder)
**P=5** (every deployment)
**E=S** (add `boot_count` to heartbeat extras via existing `APPEND_OR_FAIL` macro in build_heartbeat_payload)
**Risk = 10**

---

## Axis 5 — False-Negative Cloud Event

*Real signal not captured. The product premise is "monitor patient mobility"; missed events erode that.*

### 5.1 No fall/tipover/impact detection in firmware

**Scenario:** M10.5 explicit decision — firmware doesn't ship safety-event detector in v1. Real safety events (patient falls while using walker) will be missed entirely.

**Where:** Cloud topic `gs/{serial}/alert` exists but firmware doesn't publish.

**S=4** (product premise depends on this eventually)
**P=5** (every deployment, every safety event)
**E=XL** (multi-month detector algorithm dev)
**Risk = 20** (BUT explicitly accepted per M10.5; "deferred" not "broken")

### 5.2 Auto-stop fires after 15 s stillness → splits walk-with-rest into two sessions

**Scenario:** Patient takes brief 20+ s rest mid-walk → auto-stop ends session → walk continues → new session starts → cloud sees TWO activity rows for what should be ONE walk. Distance totals are correct, but session boundaries don't match what a caregiver expects.

**Where:** [src/main.c:61](src/main.c) `AUTO_STOP_STATIONARY_S=15` (TODO comment says M10.5 spec is 30 s).

**S=2** (split walk; total distance correct, "active_min" semantics affected)
**P=4** (older patients pause more often than 15 s)
**E=S** (bump to 30 s for FIELD_MODE per existing TODO; trivial Kconfig)
**Risk = 8**

### 5.3 No offline detection — no cloud signal when device goes silent

**Scenario:** Cloud's offline detector (Phase 1C) isn't deployed yet. A device that stops publishing (battery dead, cellular outage, fault loop) doesn't fire any alert until Phase 1C ships. M10.5 expected this.

**Where:** Cloud-side gap.

**S=4** (silent device producing no patient data; no operator alert)
**P=3** (depends on the failure rate of the failure modes above — if 2.1, 2.2, etc. happen, nobody knows)
**E=L** (cloud-side; firmware can't fix)
**Risk = 12**

### 5.4 Time-skew across long PSM cycles not validated

**Scenario:** AT+CCLK? returns the modem's perception of UTC; modem syncs from network NITZ. If carrier is unreliable about NITZ during PSM, cellular UTC drifts during 54-min PSM cycles. Long cycles without re-sync = session_start_utc could be off by minutes. Cloud-side dedup by `(serial, session_end)` so minor skew tolerated.

**Where:** [src/cellular.c:134-165](src/cellular.c) `read_network_time_iso8601`.

**S=2** (cloud dedup tolerates minor skew; minutes-of-drift would matter for daily rollups in Phase 1C)
**P=3** (carrier-dependent NITZ behavior)
**E=M** (compare modem time vs k_uptime delta to detect skew, log when drift > threshold)
**Risk = 6**

### 5.5 NaN R for stationary baseline → surface_class omitted

**Scenario:** session.c:683 omits surface_class for zero-step sessions (stationary baselines). Cloud row's surface column blank. Working as designed but operator might wonder.

**S=1** (informational; correct behavior)
**P=3** (clinic patients may accidentally trigger zero-step sessions)
**Risk = 3**

---

## Axis 6 — Storage / Lifecycle

*Per-partition retention + rotation + headroom policy gaps. Failures here are gradient (utilization grows over weeks) rather than point-in-time.*

### 6.1 Snippets retain-forever (M12.1f §F8.5 deferred policy)

**Partition:** `snippet_storage` (16 MB LittleFS at 0xd22000)

**Current policy:** Always-arm capture; FIFO upload; mark `.up` after PUBACK; never delete (neither stale-cutoff nor full-rotation implemented).

**Growth model:** ~84 KB max per snippet, typical ~50 KB clinic walk. 8 sessions/day × 30 days = 240 snippets × ~50 KB = ~12 MB. With cellular outage + slow drain: backlog accumulates without bound.

**Worst-case scenario:** Patient is heavy walker (15 sessions/day) + cellular flaky for a week (no upload drain) → 105 captured snippets × ~50 KB = ~5 MB queued. After 4 weeks at this rate: partition full. New captures silently skipped (per M10.5 spec); v1.5 retrain corpus stops growing for the rest of deployment.

**Detection:** Eventually `fs_write -ENOSPC` on snippet partition. No firmware-side alarm to cloud.

**S=4** (silent capture-skip; v1.5 retrain corpus loss for the device for the rest of deployment)
**P=4** (likely with intermittent cellular over 30 days)
**E=M** (implement: a) on snippet partition fs_statvfs, if >90% AND `.up` exists for any snippet, delete oldest `.up`-marked; b) if any `.bin` is older than 14 days regardless of `.up`, delete. Half-day to wire up + bench validate)
**Risk = 16** ← **highest in axis, tied for highest in deck**

### 6.2 Sessions partition stale .dat from pre-bde3434 / pre-prune builds

**Partition:** `littlefs_storage` (8 MB LittleFS at 0x4d2000) — sessions live at `/lfs/sessions/`

**Current policy:** Auto-prune-on-PUBACK (bde3434, 2026-05-06) for NEW sessions only. No boot-time orphan sweep.

**Growth model:** New sessions auto-prune cleanly. Stale .dat files from earlier firmware versions persist forever.

**Worst-case scenario:** Bench unit at 87 stale files / 84% util (5/5 incident). Any unit that goes through M14.5 stress-testing inherits cruft.

**Detection:** Eventually fs_write -ENOSPC → graceful auto-stop fires (bde3434) but new sessions can't capture until prior PUBACK + prune frees space. LittleFS GC stalls writes well before full (5/5 incident).

**S=3** (transient writer stalls during clinic walks; eventual graceful auto-stop)
**P=4** (any bench-tested unit accumulates cruft unless cleaned)
**E=S** (boot-time orphan sweep: delete `/lfs/sessions/*.dat` older than 7 days; OR add `tools/cleanup_device.py --all --yes` to M14.5 ship checklist)
**Risk = 12**

### 6.3 Telemetry queue partition allocated but unimplemented

**Partition:** `telemetry_queue` (256 KB raw at 0xce2000)

**Current policy:** Carved by M10.7.1; not used by any code. Heartbeat retries are in-memory only; on power-loss, in-flight payloads vanish.

**Growth model:** N/A (unused).

**Worst-case scenario:** 3-day cellular outage = 72 missed heartbeats + ~15-30 missed activity rows (clinic active days). Cloud sees device "offline" for the duration; activity rows lost permanently after device reboots in-window.

**Detection:** Cloud sees gap (when offline detector ships); no firmware-side observability.

**S=3** (cloud sees device offline; activity rows lost on power-loss-during-outage)
**P=3** (cellular outage of multi-hours possible during 30-day deployment)
**E=L** (implement persistent ring-buffer in 256 KB partition: write payload + header on enqueue, drain oldest-first on next wake. ~1-2 days)
**Risk = 9**

### 6.4 Pre-activation Shadow.reported.lastPreactivationAuditAt accumulates

**Scenario:** Per coord §C.4.7, cloud heartbeat-processor writes `lastPreactivationAuditAt` to Shadow `reported` for pre-activation devices. Today's bench Shadow shows `lastPreactivationAuditAt: "2026-04-27T23:41:22Z"` from M12.1c.1 testing, AFTER subsequent activation. Cloud-side per-leaf accept-all merge means this stale field persists forever.

**Where:** Cloud-side accept-all merge behavior; firmware doesn't write/clear.

**S=1** (operator confusion only; cloud-side data hygiene)
**P=5** (every device that publishes pre-activation heartbeats)
**E=0** (cloud-side cleanup; could also be done firmware-side by writing `null` to Shadow `reported.lastPreactivationAuditAt` on first activation)
**Risk = 5** (cloud-side concern, just flagging)

### 6.5 Snippet capture-skip cascade silently invisible

**Scenario:** When 6.1 manifests (snippet partition full), NEW captures are skipped per M10.5 spec. Currently no skip-count signal sent to cloud. Operator has zero visibility into "this device's retrain corpus stopped growing."

**Where:** [src/snippet.c](src/snippet.c) — capture_start could increment a counter on any failure.

**S=2** (lost retrain data; product still works for patient)
**P=4** (will happen when 6.1 manifests)
**E=S** (add `s_snippet_skip_count` static; expose via `gosteady_snippet_get_skip_count()`; include in heartbeat extras as `snippet_skips`)
**Risk = 8**

### 6.6 LittleFS metadata pressure stalls writes well before "full"

**Scenario:** Per 5/5 incident: GC stall at 84% util on sessions partition. Auto-prune helps but doesn't eliminate. No "stay below X%" policy or soft alarm.

**Where:** [src/main.c:206-212](src/main.c) (logs vfs free at boot, no ongoing monitor).

**S=3** (transient writer stalls degrade data quality)
**P=2** (with auto-prune working, hitting 84% is much harder; possible during extended outage)
**E=M** (boot-time fs_statvfs check; if utilization >70%, log WRN and emit `fs_pressure` field in heartbeat extras)
**Risk = 6**

### 6.7 Crash forensics single-block wear under fault loop

**Scenario:** Per 2.3, if firmware reboot-loops every 30 s = 2880 writes/day on the single 4 KB block. Designed lifetime drops from ~270 years to ~0.4 years. Eventually block can't be erased reliably → forensics writes start failing → no postmortem context for deployed-and-bricked firmware.

**Where:** [src/forensics.c:140-151](src/forensics.c) `forensics_write_locked` — no rate limit.

**S=3** (post-deployment bricked firmware can't surface its own state)
**P=1** (only with deterministic fault-loop bug)
**E=S** (rate-limit fault-driven writes: if last_fault.uptime_ms < 60 s ago, increment in-RAM counter and skip persist. Persist next time gap > 1 minute)
**Risk = 3**

### 6.8 Activation.bin lives in LittleFS — single point of failure for "is device active"

**Scenario:** Per 1.6 / 2.2, /lfs corruption or LittleFS unmount fails → activation.bin lost → device falls back to pre-activation in FIELD_MODE → silent zero-activity-rows from cloud's perspective. Recovery requires cloud to detect + reissue activate cmd (Phase 2A device-shadow-handler is the consumer; currently dormant).

**Where:** [src/activation.c:13-17](src/activation.c) — comment explicitly addresses why it lives in /lfs but acknowledges fail-safe defaults to "not activated."

**S=4** (silent re-entry to pre-activation; cloud-side response requires Phase 2A)
**P=2** (LittleFS corruption from physical events)
**E=M** (mirror activation record to crash_forensics partition reserved bytes — small struct, ~88 bytes; on init, prefer /lfs read but fall back to crash_forensics if /lfs read fails)
**Risk = 8**

### 6.9 External_flash remainder (~2.86 MB) idle

**Partition:** `external_flash` catch-all at 0x1d22000–0x2000000 (2.86 MB)

**Current policy:** Unused.

**Note:** Available for snippet partition resize, OTA staging, additional log regions. Can't repartition post-deployment without reflash.

**S=1**, **P=5** (always unused), **E=L** (repartition pre-M14.5)
**Risk = 5** (worth thinking about pre-M14.5 ship if we want telemetry queue extension)

---

## Cross-Axis Summary — Top Risks Ordered

| Axis.# | Title | S | P | Risk | E | Open/Accepted |
|---|---|---|---|---|---|---|
| **1.1** | Empty `session_start` from cold-boot motion | 4 | 5 | **20** | S | Open — found today |
| **5.1** | No fall/tipover/impact detection | 4 | 5 | 20 | XL | Accepted (M10.5) |
| **2.1** | Activate cmd never received → permanent pre-activation | 4 | 4 | **16** | M | Open |
| **6.1** | Snippets retain-forever (no rotation, no stale cutoff) | 4 | 4 | **16** | M | Open |
| **3.1** | nPM1300 fuel gauge model mismatch → false battery_critical | 3 | 5 | **15** | L | Open |
| **1.2** | Empty `session_end` if cellular drops mid-session | 4 | 3 | 12 | S | Open |
| **1.3** | PUBACK timeout silently drops activity, no retry | 4 | 3 | 12 | M | Open |
| **6.2** | Sessions partition stale .dat from pre-bde3434 builds | 3 | 4 | 12 | S | Open |
| **5.3** | No offline detection — no cloud signal when silent | 4 | 3 | 12 | L | Open (cloud-side) |
| **1.6** | LittleFS journal corruption from physical events | 5 | 2 | 10 | L | Open |
| **2.2** | LittleFS unrecoverable mount failure | 5 | 2 | 10 | L | Open |
| **2.3** | Watchdog + fault handler infinite reboot loop | 5 | 2 | 10 | M | Open |
| **4.1** | fault_counters carry stale stress-test values forever | 2 | 5 | 10 | S | Open |
| **4.7** | boot_count not in heartbeat → cloud can't correlate forensics | 2 | 5 | 10 | S | Open |
| **3.2** | Snippet upload backlog drains battery during cellular instability | 3 | 3 | 9 | M | Open |
| **6.3** | Telemetry queue partition allocated but unimplemented | 3 | 3 | 9 | L | Open |
| **2.4** | Cellular modem never re-attaches after extended outage | 4 | 2 | 8 | M | Open |
| **4.2** | Activity row with wrong `surface_class` | 2 | 4 | 8 | L | Accepted (v1.5) |
| **4.3** | Reset reason "POWER_ON" indistinguishable from suspicious reboot | 2 | 4 | 8 | S | Open |
| **5.2** | Auto-stop fires at 15s → splits walk-with-rest | 2 | 4 | 8 | S | Open (M10.5 spec is 30s) |
| **6.5** | Snippet capture-skip cascade silently invisible | 2 | 4 | 8 | S | Open |
| **6.8** | Activation.bin single point of failure | 4 | 2 | 8 | M | Open |
| **1.4** | Activity msgq overflow drops session | 3 | 2 | 6 | M | Open |
| **1.5** | Snippet capture orphan-bin loss | 2 | 3 | 6 | M | Accepted (v1) |
| **1.7** | Activity rejected if cloud schema tightens | 3 | 2 | 6 | 0 | Cloud-side |
| **4.5** | Algo overflow flag → distance=0 | 3 | 2 | 6 | M | Open |
| **5.4** | Time-skew across PSM cycles not validated | 2 | 3 | 6 | M | Open |
| **6.6** | LittleFS metadata pressure stalls writes | 3 | 2 | 6 | M | Open |
| **2.5** | Modem firmware fault on init → permanently no cellular | 5 | 1 | 5 | M | Open |
| **2.6** | AWS IoT cert expiration | 5 | 1 | 5 | XL | Defer |
| **3.5** | Hourly heartbeat retry burst | 1 | 3 | 3 | — | Negligible |
| **3.6** | PSM not granting active timer | 1 | 4 | 4 | M | Negligible |
| **4.4** | ADXL367 false-positive chatter | 1 | 4 | 4 | M | Negligible |
| **4.6** | last_cmd_id stale echo after 24h | 1 | 5 | 5 | S | Open (cosmetic) |
| **6.4** | lastPreactivationAuditAt accumulates in Shadow | 1 | 5 | 5 | 0 | Cloud-side |
| **6.7** | Crash forensics single-block wear under fault loop | 3 | 1 | 3 | S | Open |
| **6.9** | external_flash remainder idle | 1 | 5 | 5 | L | Defer |

---

## M14.5-Blocker Punch-List — Implementation Specs

The 9 items below are **Risk ≥ 12 AND Effort ≤ M AND not Accepted**. Each gets a detailed spec for implementation in this hardening sprint. Items deferred from this sprint are documented at the end with rationale.

Build target for this sprint: **`build_cloud`** (`prj_cloud.conf` overlay) — bench cloud-test image with full tooling. Validation against bench unit `GS9999999999`. After all items land, also build `build_field` to confirm deployment image is clean.

### Spec 4.7 — Add `boot_count` to heartbeat extras

**Files modified:** [src/forensics.h](src/forensics.h), [src/forensics.c](src/forensics.c), [src/cloud.c](src/cloud.c)

**Implementation:**

1. New public getter in [forensics.h](src/forensics.h):
   ```c
   uint32_t gosteady_forensics_get_boot_count(void);
   ```
2. Implementation in [forensics.c](src/forensics.c) — return `s_record.boot_count` (0 if not initialized).
3. In [cloud.c::build_heartbeat_payload](src/cloud.c), after the existing `watchdog_hits` `APPEND_OR_FAIL` (line 653), add:
   ```c
   APPEND_OR_FAIL(",\"boot_count\":%u",
                  (unsigned)gosteady_forensics_get_boot_count());
   ```
   Gated under `#if defined(CONFIG_GOSTEADY_FORENSICS_ENABLE)` like the other forensics extras.

**Test plan:**
- Build, flash, observe first heartbeat publish in uart0 log: should contain `"boot_count":N` where N matches the boot_count log line at startup.
- Verify cloud Shadow.reported now contains `boot_count` field via `aws iot-data get-thing-shadow`.

**Estimated effort:** 30 min total (S).

---

### Spec 4.1 — Reset forensics counters on first activation

**Files modified:** [src/forensics.h](src/forensics.h), [src/forensics.c](src/forensics.c), [src/activation.c](src/activation.c)

**Implementation:**

1. New public function in [forensics.h](src/forensics.h):
   ```c
   /* Reset cumulative fault/watchdog/assert counters to zero. boot_count
    * and reset_reason fields are NOT reset — those track lifetime device
    * state, not deployment lifecycle. Called from activation_apply on
    * the transition from not-activated to activated, signaling "this
    * is a new deployment lifecycle; bench-test counters are no longer
    * relevant." Persists immediately to flash.
    *
    * Returns 0 on success, negative errno on flash error. */
   int gosteady_forensics_reset_counters(void);
   ```
2. Implementation in [forensics.c](src/forensics.c) — under s_lock: zero `fault_count`, `watchdog_hits`, `assert_count` in `s_record`, persist via `forensics_write_locked`. Log INFO.
3. In [activation.c::gosteady_activation_apply](src/activation.c), after the successful `write_file_locked` and `atomic_set(&s_activated, 1)` (line 165), check if this was a transition (was previously not-activated). If so, call `gosteady_forensics_reset_counters()`.

**Subtle:** The activation_apply already has an idempotent retry path that returns early. So reaching past the early-return guarantees this is a fresh activation. Capture `bool was_activated_before = atomic_get(&s_activated)` at the start of the function (before mutex lock) to know.

Actually simpler: the path is "successful first-time write_file_locked" — so the reset call goes right after that, gated on `was_activated_before == 0`. Since the idempotent retry returns earlier, by the time we reach the write, we're committed to a fresh apply.

```c
// in activation_apply, after write_file_locked succeeds:
bool was_activated_before = atomic_get(&s_activated);
atomic_set(&s_activated, 1);
LOG_INF("activation applied: at=%s cmd_id=%s (persisted to %s)", ...);
if (!was_activated_before) {
    int rc = gosteady_forensics_reset_counters();
    if (rc < 0) {
        LOG_WRN("forensics counter reset failed (%d) — continuing", rc);
    } else {
        LOG_INF("forensics counters reset on first activation");
    }
}
```

Gate the call under `#if defined(CONFIG_GOSTEADY_FORENSICS_ENABLE)` so bench-only builds without forensics still compile.

**Test plan:**
- Build, flash, current bench unit is already activated → no reset on this boot.
- To force the test path: clear /lfs/activation.bin (via dump.c DEL or `tools/cleanup_device.py`), reboot, send fresh activate cmd via `aws iot-data publish`.
- Verify uart0 log: `forensics counters reset on first activation`.
- Verify next heartbeat: `fault_counters: {fatal:0, asserts:0, watchdog:0}`, `watchdog_hits:0`.

**Estimated effort:** 1 hour (S).

---

### Spec 1.1 + 1.2 — Retro-stamp session_start/end UTC at activity-publish time

**Files modified:** [src/cellular.h](src/cellular.h), [src/cellular.c](src/cellular.c), [src/cloud.h](src/cloud.h), [src/session.c](src/session.c), [src/cloud.c](src/cloud.c)

**Approach:** Currently session.c stamps cellular UTC into the activity struct at session_start (for session_start_utc_iso) and at session_stop (for session_end_utc_iso). If cellular is unavailable at either of those moments, the field is empty string → cloud rejects.

The activity worker thread already waits for cellular ready before publishing (via `wait_for_cellular_ready` inside `connect_publish_disconnect`). At the moment of publish, cellular UTC IS available. So the fix: **defer ISO-8601 string formatting to the activity worker, just before publish, using uptime deltas captured at session_start/stop**.

**Implementation:**

1. New helper in [cellular.h](src/cellular.h) + [cellular.c](src/cellular.c):
   ```c
   /* Format a Unix-epoch milliseconds value as ISO 8601 UTC string
    * ("YYYY-MM-DDTHH:MM:SSZ"). Uses gmtime_r + strftime. Pure formatter
    * — does not touch the modem.
    *
    * out_sz must be >= 24 for the standard format including NUL.
    * Returns 0 on success, -EINVAL on bad args, -EIO on gmtime_r failure. */
   int gosteady_cellular_format_unix_ms_iso8601(int64_t unix_ms,
                                                 char *out, size_t out_sz);
   ```
2. Add two fields to [cloud.h::struct gosteady_activity](src/cloud.h):
   ```c
   /* Phase 1.6 follow-up 2026-05-10 (FMEA 1.1+1.2): uptime deltas captured
    * at session_start/stop. The activity worker resolves these to ISO 8601
    * UTC strings just before publish using current cellular UTC + the
    * uptime delta. Handles the cold-boot-mid-motion edge case where
    * cellular hadn't attached at session_start AND covers the case where
    * cellular drops between session_start and session_stop.
    *
    * If the worker can't get cellular UTC even at publish time, it falls
    * through with whatever was originally stamped (which may be empty);
    * activity-republish path (FMEA 1.3) handles the publish-failure case. */
   uint32_t session_start_uptime_ms;
   uint32_t session_end_uptime_ms;
   ```
3. In [session.c::gosteady_session_stop](src/session.c) activity-fill block (line 656-699), populate the new fields:
   ```c
   a.session_start_uptime_ms = s_session_start_uptime_ms;
   a.session_end_uptime_ms   = k_uptime_get_32();
   ```
4. In [cloud.c::activity_worker_thread_fn](src/cloud.c), AFTER the dequeue and BEFORE `build_activity_payload` (around line 845-849), add a UTC-resolution pass:
   ```c
   if (a.session_start_utc_iso[0] == '\0' || a.session_end_utc_iso[0] == '\0') {
       int64_t now_unix_ms = 0;
       int t_err = gosteady_cellular_get_network_time_unix_ms(&now_unix_ms);
       if (t_err == 0) {
           uint32_t now_uptime = k_uptime_get_32();
           if (a.session_start_utc_iso[0] == '\0' && a.session_start_uptime_ms != 0) {
               int64_t start_unix = now_unix_ms - (int64_t)(now_uptime - a.session_start_uptime_ms);
               int frc = gosteady_cellular_format_unix_ms_iso8601(
                   start_unix, a.session_start_utc_iso, sizeof(a.session_start_utc_iso));
               if (frc == 0) {
                   LOG_INF("activity: retro-stamped session_start from uptime → %s",
                           a.session_start_utc_iso);
               }
           }
           if (a.session_end_utc_iso[0] == '\0' && a.session_end_uptime_ms != 0) {
               int64_t end_unix = now_unix_ms - (int64_t)(now_uptime - a.session_end_uptime_ms);
               int frc = gosteady_cellular_format_unix_ms_iso8601(
                   end_unix, a.session_end_utc_iso, sizeof(a.session_end_utc_iso));
               if (frc == 0) {
                   LOG_INF("activity: retro-stamped session_end from uptime → %s",
                           a.session_end_utc_iso);
               }
           }
       } else {
           LOG_WRN("activity worker: cellular UTC still unavailable (%d) — publish will have empty timestamp(s)", t_err);
       }
   }
   ```

**Why this works:**
- Best case: both timestamps were stamped at capture time → no-op pass, original strings used.
- Cold-boot case: session_start was empty; session_end was stamped (cellular attached during the session). The worker waits for cellular before this code runs (in connect_publish_disconnect's wait_for_cellular_ready). So we have cellular UTC available. Back-compute session_start.
- Cellular-dropped-mid-session case: both could be empty. Worker waits for cellular at publish time. Back-compute both.
- Pathological case: cellular still unavailable at publish (worker `wait_for_cellular_ready` blocks until cellular attaches; it has no timeout). So this code path effectively can't fail unless cellular is permanently dead — in which case publish fails for other reasons.

**Test plan:**
- Power-cycle the bench unit while gently moving it (motion-fires-before-cellular). Wait for the activity uplink. Verify uart0 log shows `activity: retro-stamped session_start from uptime → ...`.
- Verify cloud-side: activity row lands in DDB with valid ISO 8601 timestamps. Use `aws logs filter-log-events --log-group-name /aws/lambda/gosteady-dev-activity-processor` to confirm no `activity_reject` for `bad_timestamp`.
- Per-Device dashboard "Recent activity sessions" widget should show the row.

**Estimated effort:** 2 hours (S).

---

### Spec 6.2 — Boot-time stale .dat orphan sweep

**Files modified:** [src/session.h](src/session.h), [src/session.c](src/session.c), [src/main.c](src/main.c)

**Approach:** Currently the sessions partition (8 MB) accumulates .dat files from any session that didn't successfully PUBACK. After the M14.5 stress-test cycle, units may ship with old .dat files cluttering the partition. We need a boot-time cleanup that's safe (doesn't delete in-progress data) and aggressive (frees space).

**Constraint:** LittleFS doesn't expose mtime, so age-based deletion isn't directly possible. Two options:

- **Option A (chosen for this fix):** Delete ALL .dat files at boot. Justification: at boot, no session is in flight (we run before sampler/writer threads start). Any .dat on disk is from a previous boot's failed PUBACK. Without 1.3 (republish queue), those .dats are unreachable for republish anyway — they're effectively dead data. Pruning them frees space for new captures.

- **Option B (would be cleaner if 1.3's republish was reboot-survivable):** Keep .dats, attempt republish at boot, prune after PUBACK. But our 1.3 spec is in-memory-only (telemetry_queue partition is unimplemented; that's 6.3 deferred). So we can't reach option B without implementing 6.3 first.

We adopt Option A explicitly, document the trade-off, and note that future v1.5 with 6.3 implemented can switch to Option B without changing the API.

**Implementation:**

1. New public function in [session.h](src/session.h):
   ```c
   /* FMEA 6.2: boot-time orphan sweep. Iterate /lfs/sessions/, fs_unlink
    * every .dat file found. Called from main() at boot, AFTER LittleFS
    * mount + activation_init succeed but BEFORE the sampler/writer
    * threads start (so no session is in flight).
    *
    * Trade-off: any session whose PUBACK didn't return before the
    * previous shutdown is lost permanently from the activity DDB. Without
    * the FMEA 6.3 telemetry_queue persistence, those .dat files are
    * unreachable for republish anyway. Bounding partition growth wins.
    *
    * Returns the number of files deleted, or negative errno on directory
    * iteration error. */
   int gosteady_session_orphan_sweep(void);
   ```

2. Implementation in [session.c](src/session.c) — fs_opendir SESSION_DIR, iterate entries, for each ending in ".dat" call fs_unlink. Tally + log: `orphan-sweep: deleted N stale .dat files`.

3. In [main.c::main](src/main.c), call after `mount_lfs_and_bump_boot_count()` succeeds (around line 970-972), BEFORE the cloud paths (since cloud will block on cellular and we want the sweep to be deterministic-fast).

   ```c
   int swept = gosteady_session_orphan_sweep();
   if (swept > 0) {
       LOG_INF("orphan-sweep: deleted %d stale .dat files at boot", swept);
   }
   ```

**Test plan:**
- Add a manual test: pull current bench unit's .dat list with `tools/pull_sessions.py --port /dev/cu.usbmodem*1105 --list-only` (hypothetical flag; actual: just `LIST` over uart1).
- After flash, observe boot log for sweep count. Expect 1 (the `1c778fdd-...dat` from prior testing per coord doc).
- After boot, `LIST` again should show empty.

**Estimated effort:** 1 hour (S).

---

### Spec 1.3 — Activity republish on PUBACK timeout (in-memory retry)

**Files modified:** [src/cloud.c](src/cloud.c)

**Scope:** In-memory retry only. Persistent retry across reboot (telemetry_queue partition, FMEA 6.3) is deferred to a separate effort.

**Approach:** The activity worker thread currently dequeues from `s_activity_msgq`, calls `connect_publish_disconnect`, logs WRN on failure, and moves on. We change it to: on publish failure (PUBACK timeout, connect failure, etc.), re-enqueue with a retry-count and sleep before the next attempt.

**Implementation:**

1. Add a retry-counter field to [cloud.h::struct gosteady_activity](src/cloud.h):
   ```c
   /* FMEA 1.3: in-memory retry counter for PUBACK timeout / publish
    * failures. Bumped on each failure; the activity worker re-enqueues
    * with a backoff sleep up to MAX retries before giving up. Reset to
    * zero by the original caller (session.c). */
   uint8_t retry_count;
   ```
2. In [session.c::gosteady_session_stop](src/session.c) activity-fill, ensure `a.retry_count = 0` (memset zero already covers it via `struct gosteady_activity a = {0}`).

3. In [cloud.c::activity_worker_thread_fn](src/cloud.c), wrap the publish call in a retry block:
   ```c
   #define ACTIVITY_MAX_RETRIES 3
   static const k_timeout_t ACTIVITY_RETRY_DELAYS[] = {
       K_SECONDS(60),       /* 1st retry: 1 min */
       K_SECONDS(60 * 5),   /* 2nd retry: 5 min */
       K_SECONDS(60 * 15),  /* 3rd retry: 15 min */
   };

   int prc = connect_publish_disconnect(topic, (size_t)t, payload, payload_len);
   if (prc) {
       a.retry_count++;
       if (a.retry_count <= ACTIVITY_MAX_RETRIES) {
           LOG_WRN("activity publish failed (%d), retry %u/%u in %d s",
                   prc, a.retry_count, ACTIVITY_MAX_RETRIES,
                   (int)k_ticks_to_ms_floor32(
                       ACTIVITY_RETRY_DELAYS[a.retry_count - 1].ticks) / 1000);
           k_sleep(ACTIVITY_RETRY_DELAYS[a.retry_count - 1]);
           int rc = k_msgq_put(&s_activity_msgq, &a, K_NO_WAIT);
           if (rc < 0) {
               LOG_ERR("activity re-enqueue failed (%d) — DROPPING session %s",
                       rc, a.session_uuid);
           }
       } else {
           LOG_ERR("activity publish failed (%d) after %u retries — DROPPING session %s",
                   prc, ACTIVITY_MAX_RETRIES, a.session_uuid);
       }
   } else {
       LOG_INF("M12.1d activity uplink sequence complete");
       /* (existing auto-prune block stays) */
   }
   ```

**Why backoff matches heartbeat retry pattern:** Same 1/5/15 min schedule as `heartbeat_retry_delay`. Familiar to operators reading logs. After 21 minutes total of retries, give up — by then a sustained outage means the retry cost outweighs the benefit, and the next session's activity (if any) will retry naturally on its own.

**Why the .dat is preserved:** The auto-prune-on-PUBACK path only fires on success, so the .dat survives all failure paths. If we ever ship 6.3 (telemetry_queue), boot-time scan can re-enqueue from there. Until then, after the 3rd retry exhausts, the .dat is on flash but not in any queue → effectively lost (this is the documented trade-off).

**Trade-off vs telemetry_queue:** The retry covers transient cellular flaps within the boot window. For multi-hour outages or PUBACK timeouts that stretch past the 21-minute retry budget, we still lose the activity. Telemetry_queue (6.3) would provide reboot-survival but is much higher effort.

**Test plan:**
- Force a PUBACK timeout: temporarily set `PUBACK_WAIT` to 1 s (or block egress via airplane mode briefly during a publish).
- Verify uart0 log: `activity publish failed ... retry 1/3 in 60 s`.
- Restore connectivity, verify retry succeeds.
- Verify activity row appears in cloud Activity Series DDB.

**Estimated effort:** 2 hours (M).

---

### Spec 6.1 — Snippet rotation policy (90% threshold rotation + 14-day stale cutoff)

**Files modified:** [src/snippet.h](src/snippet.h), [src/snippet.c](src/snippet.c), [src/cloud.c](src/cloud.c)

**Approach:** Per M10.5 spec, snippet partition needs:
1. **90% rotation:** When fs_statvfs reports < 10% free, delete the OLDEST `.up`-marked snippet (uploaded → safe to remove). If no `.up`-marked exist (worst case all queued), skip — don't delete unsent snippets.
2. **14-day stale cutoff:** Any snippet (`.bin` regardless of `.up` status) older than 14 days gets deleted. Older un-uploaded snippets are presumably stale-data not-worth-uploading. Older uploaded ones are unconditionally safe to remove.

**Age detection:** LittleFS doesn't expose mtime. We use the `window_start_ts` ISO 8601 string in the JSON sidecar as the age proxy. Parse, compare to current cellular UTC.

**Implementation:**

1. New public function in [snippet.h](src/snippet.h):
   ```c
   /* FMEA 6.1: snippet rotation pass.
    *
    * Two-phase cleanup:
    *   1. Stale-cutoff: delete any snippet (bin + json + up) whose JSON
    *      sidecar window_start_ts is older than `stale_age_seconds` ago.
    *      Requires cellular UTC to be available (uses
    *      gosteady_cellular_get_network_time_unix_ms); skips silently
    *      otherwise.
    *   2. Free-space rotation: if fs_statvfs reports utilization
    *      >= rotate_threshold_pct, delete the OLDEST .up-marked snippet
    *      (FIFO by filename order, which is roughly insertion order in
    *      LittleFS). If no .up-marked snippets exist, skip — never delete
    *      un-uploaded snippets.
    *
    * Returns number of snippets deleted (sum across both phases), or
    * negative errno on directory error. */
   int gosteady_snippet_rotate(uint32_t stale_age_seconds,
                               uint8_t rotate_threshold_pct);
   ```

2. Implementation in [snippet.c](src/snippet.c) — two passes over /snippets/:
   - **Pass 1 (stale cutoff):** Get current UTC ms via `gosteady_cellular_get_network_time_unix_ms`. If unavailable, skip stale pass. Iterate `.json` sidecars, parse `window_start_ts`, compute age, if `> stale_age_seconds` delete `.bin` + `.json` + `.up`.
   - **Pass 2 (free-space):** `fs_statvfs(SNIPPET_MNT)`, compute used%. If `>= rotate_threshold_pct`, iterate `.up` files (cheap — just stat exists), pick first encountered (LittleFS order is roughly insertion), delete its `.bin` + `.json` + `.up`. Loop until below threshold OR no `.up` left.

3. Compact JSON-parsing helper in snippet.c — we already have a tiny ISO 8601 → unix_ms helper need (this is the inverse of the cellular formatter from spec 1.1). We need to parse `"YYYY-MM-DDTHH:MM:SSZ"` to epoch_ms. Use `sscanf` + `mktime`/`timegm` (Zephyr has `timeutil_timegm64`).

4. Call sites:
   - In [snippet.c::gosteady_snippet_init](src/snippet.c) — after fs_mount + mkdir succeed, call `gosteady_snippet_rotate(14*24*3600, 90)` once at boot.
   - In [snippet.c::gosteady_snippet_capture_start](src/snippet.c) — at the very top, before fs_open, call `gosteady_snippet_rotate(14*24*3600, 90)` so a full partition gets rotated before we attempt to write. Bounded cost (typically 0 deletes).
   - Optional: in [cloud.c::heartbeat_thread_fn](src/cloud.c) — once per heartbeat tick (not per publish), call rotate. This means the partition is checked hourly even if no captures happen. **Implement this too** so a backlogged-then-paused unit still gets cleaned.

5. Add a constants block in [snippet.h](src/snippet.h):
   ```c
   #define GOSTEADY_SNIPPET_STALE_AGE_S       (14u * 24u * 3600u)  /* 14 days */
   #define GOSTEADY_SNIPPET_ROTATE_THRESHOLD  90u                   /* 90 % full */
   ```

**Defensive:** If `fs_statvfs` returns error or impossible values, skip rotation (don't delete on bad signal). If a JSON sidecar is malformed (parse fails), skip that snippet (don't delete based on bad data — that snippet is broken anyway and the orphan-cleanup path handles it).

**Test plan:**
- Build, flash. With current bench unit's snippet partition state (probably mostly empty), rotation pass should be a no-op.
- For full validation we'd need to populate the partition — bench-only deferred to M14.5 soak observation.
- For minimal validation: confirm `gosteady_snippet_rotate` is called and returns 0 cleanly via uart0 log.

**Estimated effort:** 3-4 hours (M).

---

## Deferred Items

These items were on the M14.5 punch-list candidate but require external dependencies or longer-cycle observation, so are deferred from this hardening sprint with documented rationale.

### 2.1 Shadow re-check on every cellular wake — DEFERRED to Phase 2A coord

**Why deferred:** The firmware-side fix (send Shadow.GET on every cellular wake, check `desired.activated_at`) requires the cloud-side `device-shadow-handler` Lambda (Phase 2A) to be live for full reliability. Per coord §F7.5 firmware deferred this same reason. Until Phase 2A ships:
- The current `CONFIG_MQTT_CLEAN_SESSION=n` + AWS broker queueing covers the common case (cmd published while device offline → delivered on next CONNECT).
- AWS broker session-state expiry (~1 hour idle per docs) is the failure window; with hourly heartbeat cadence we're at the edge of that boundary.
- For first-time activation of `GS0000000001/2/3` — coordinated manual `aws iot-data publish` while a heartbeat is in-progress works as documented in M12.1e.2.

**Mitigation in M14.5:** Coordinate activate-cmd publishes with heartbeat ticks observed in dashboard.

**Re-open trigger:** Phase 2A `device-shadow-handler` Lambda deployment in cloud-portal repo.

### 3.1 nPM1300 LP803448-tuned battery model — DEFERRED to bench discharge run

**Why deferred:** Building a tuned battery model requires capturing ~30 days of discharge curve under a known load profile. The work is mostly observation time + analysis, not firmware engineering. Cannot be completed within this hardening sprint.

**Mitigation in M14.5:** Site-survey unit `GS0000000001` runs the bundled "Example" model for now. Heartbeat reports both `battery_pct` (model output) and `battery_mv` (raw measurement). Cloud-side comparison of voltage drop vs SoC drop will surface model error during the M14.5 ≥7-day soak. If false-critical alerts fire at non-critical actual SoC, that's the trigger to (a) bump cloud-side `battery_critical` threshold from 0.05 → 0.02 as a temporary gate, and (b) start the bench discharge.

**Re-open trigger:** M14.5 site-survey reveals battery model error >5% absolute.

---

## Notes on Methodology

- Rows enumerated by walking each module from session.c → cloud.c → cellular.c → snippet.c → forensics.c → activation.c → battery.c, asking "what fails silently?" + "what wears over time?"
- Anything with `LOG_WRN` that returns to caller without surfacing to cloud is a candidate.
- Anything with a `TODO` comment in code or `deferred` status in coord doc is a candidate.
- I deliberately did NOT enumerate every M10.5-spec deferral as a separate row — those are tracked in the spec.
- Cross-axis duplicates are fine: 1.6 (LittleFS journal corruption) appears once, but its consequences flow into 2.2 (mount fail) and 6.8 (activation.bin loss) — the rows are different *manifestations* requiring different *fixes*.
- I avoided rows that would require cloud-side measurement to characterize (e.g., "how often does NITZ fail in this carrier"); those are M14.5 soak observations.
