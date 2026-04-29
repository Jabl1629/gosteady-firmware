# GoSteady — Firmware Development Context

## What Is GoSteady?

GoSteady is a smart walker cap — a small device that attaches to a walker leg and estimates distance traveled using onboard IMU sensors. The product targets elderly users and physical therapy patients who need to track mobility without wearing anything on their body.

The prototype development platform is the **Nordic Thingy:91 X** dev board, which will eventually be replaced by a custom PCB for production.

---

## Hardware: Thingy:91 X

### Key ICs and Their Roles

| IC | Function | Interface | GoSteady Relevance |
|---|---|---|---|
| **nRF9151** | Main application processor + LTE-M/NB-IoT cellular | — | Runs our firmware (`src/*`); TF-M in secure partition. No direct USB, no 2.4 GHz radio. |
| **nRF5340 (app core)** | USB + BLE host | — | Runs our vendored-and-patched connectivity bridge (`bridge_fw/`). Exposes USB CDC-ACM + mass-storage + BLE NUS. |
| **nRF5340 (net core)** | BLE controller | IPC to app core | Runs NCS `ipc_radio` sample, built and flashed automatically by bridge sysbuild. |
| **nRF7002** | Wi-Fi 6 companion | SPI on `spi3` | Not used in v1. |
| **BMI270** | 6-axis IMU (accel + gyro) | SPI (`spi3` @ CS `P0.10`, 10 MHz max, IRQ `P0.06` active-low) | **Primary sensor** — step detection + stride estimation |
| **ADXL367** | Ultra-low-power 3-axis accelerometer | I²C (`i2c2` @ `0x1d`, INT1 `P0.11` active-high) | Secondary/backup accel, wake-on-motion trigger |
| **GD25LE255E SPI NOR Flash (32 MB)** | External storage | SPI (shared `spi3` bus, 8 MHz max) | IMU session logs, MCUboot secondary, modem FOTA staging |

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

**USB enumeration:** comes from the **nRF5340 connectivity bridge** (not a J-Link OB, not direct nRF9151 USB). As of M6b we run our own fork (`bridge_fw/`) based on NCS v3.2.4's `applications/connectivity_bridge`. The bridge exposes three USB interfaces simultaneously:

1. **CDC-ACM on `/dev/cu.usbmodem*102`** → nRF9151 **uart0** @ 115200 (app console; `LOG_INF` output lands here).
2. **CDC-ACM on `/dev/cu.usbmodem*105`** → nRF9151 **uart1** @ 1 Mbaud (our dump / control channel; also the target of BLE NUS tunneling since M6b).
3. **Mass-storage volume** mounted on macOS as **`/Volumes/NO NAME`** (the volume label is literally `NO NAME`, not `THINGY91X` — easy to miss in Finder). Contains `Config.txt` for runtime options (notably `BLE_ENABLED`) and `README.txt`.

The hex-format serial (e.g. `DAE5B718C2209445`) is the bridge's USB serial, not a J-Link OB serial. `802006700` is the external J-Link Mini — these are two separate devices, not two formats of the same ID.

> **USB CDC endpoint digit-count varies.** macOS assigns these port numbers based on USB hub topology at enumeration time. Seen: `/dev/cu.usbmodem11102` / `11105` on first plug, dropping to `usbmodem1102` / `1105` after unplug-replug, or vice versa. **Always `ls /dev/cu.usbmodem*` after any physical replug** before invoking host-side tools. Any persistent script/glob should use a wildcard (`*1105`) rather than a hard-coded digit count.

### Physical UART wiring between the two SoCs

The nRF9151 and nRF5340 share two dedicated UART lines on the PCB:

- **uart0 line** ↔ nRF5340 `cdc_acm_uart0` → USB CDC endpoint `/dev/cu.usbmodem102`. Baud: 115200 on both sides. Purpose: nRF9151 app console / log output.
- **uart1 line** ↔ nRF5340 `cdc_acm_uart1` → USB CDC endpoint `/dev/cu.usbmodem105`. Baud: 1 Mbaud on both sides. Purpose: our M5 dump/command channel (LIST / DUMP / DEL / PING / START / STOP / STATUS). Also the target of BLE NUS tunneling after our M6b patch.

**The uart1 baud rate is load-bearing** — see Known Gotchas. Upstream Nordic bridge defaults uart1 to 115200, which produces garbage on the wire because nRF9151 uart1 is at 1 Mbaud. Our `bridge_fw/boards/thingy91x_nrf5340_cpuapp.overlay` pins the bridge-side default to 1 Mbaud.

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

**Build / flash:**

- **VS Code build config board must be `thingy91x/nrf9151/ns`.** Easy to accidentally create a build config pointing at `nrf9151dk` — it will build cleanly but produce an image with completely different pin assignments. Symptom: LEDs do the wrong thing, UART is garbage, peripherals don't respond. Sanity check: `build/gosteady-firmware/zephyr/.config` should contain `CONFIG_BOARD="thingy91x"`.
- **`west build -p auto` doesn't pristine-rebuild when you add a new overlay or board file.** CMake's incremental build misses them. Use `-p always` or `rm -rf build/` after adding `boards/*.overlay` or changing partition-manager Kconfig. Hit this once on M2 and once on M3.
- **`west flash` with the nrfutil runner hits `UICR data is not erasable` on nearly every flash.** The runner only erases sectors the new image touches, and our b0 + MCUboot + TF-M + app layout writes UICR on every rebuild. Fix: `nrfutil device erase --serial-number 802006700` before each `west flash`, or use the nrfjprog runner with `--eraseall` (slower but single-command).
- **APPROTECT + ERASEPROTECT lockout on freshly-rescued bridges.** The rescue hex ships with readback protection on; first `west flash` to the nRF5340 fails with `The Application core access port is protected`. Fix: `west flash --recover` wipes both nRF5340 cores and reflashes. Side effect: the bridge's settings-flash gets wiped, which resets `BLE_ENABLED=0` — you have to re-toggle in `Config.txt` after every recover flash. On the nRF9151 (single core), chain recover + program in one invocation so no reset re-asserts the lock between them: `nrfjprog -f NRF91 --recover --program build/merged.hex --verify --reset`.
- **Board stops enumerating on USB-C → bridge firmware is probably corrupt.** Reflash the connectivity bridge from the local Nordic bundle: `nordic resources/thingy91x_mfw-2.0.4_sdk-3.2.1/img_app_bl/thingy91x_nrf53_connectivity_bridge_v3.0.1.hex`, with **SW2 flipped to nRF53**. This recovery is independent of anything wrong on the nRF9151 side. Alternative: reflash our own `bridge_fw/` build.

**Serial / USB:**

- **`cat /dev/cu.usbmodem*` shows a dead console even when the app is logging.** The bridge holds UART RX back until DTR is asserted; plain `cat` doesn't. Use `screen`, `minicom`, `cu`, pyserial, or nRF Terminal — all assert DTR on open.
- **nRF9151 uart1 runs at 1 Mbaud, not 115200.** Always open `/dev/cu.usbmodem*1105` at `baud=1000000`. CDC host opens trigger bridge-side `SET_LINE_CODING` → bridge physical uart1 reconfigures to match. Opening at the wrong baud just gets you binary garbage, not errors.
- **Stale garbage in the nRF9151's uart1 parser ring-buffer** sticks around if a baud mismatch ever streamed random bytes into it. Symptom: clean commands come back as `ERR unknown`. Fix: send ~10 bare newlines (`\n` × 10) to flush the parser's line state, then retry. The parser treats blank lines as no-ops.
- **USB CDC endpoints re-enumerate on every replug.** Seen on macOS: `/dev/cu.usbmodem11102`/`11105` on first plug, dropping a digit to `/dev/cu.usbmodem1102`/`1105` after an unplug-replug, or vice versa. The leading-digit reflects the USB hub topology slot macOS picked at enumeration. Always `ls /dev/cu.usbmodem*` before any host-side tool invocation after a physical replug. Hard-coding the digit-count in scripts is fragile — glob or autodetect if you're writing anything persistent.
- **uart1 is a SHARED channel (USB ↔ BLE) — host tools must tolerate noise lines (fixed 2026-04-22).** The bridge forwards the 91's uart1 TX to BOTH the USB CDC endpoint AND the BLE NUS TX notify. When a BLE client is connected (e.g., capture.html's 5 s STATUS poll), its command responses bleed into the USB stream between our DUMP transactions. Symptom in `pull_sessions.py`: every file after the first few was a size-shifted copy of the next file's content, because the preceding DUMP's trailing `STATUS active=0\n` was being consumed as the next DUMP's `SIZE <n>\n`. The firmware's dispatch loop is serial so noise NEVER interleaves mid-body; it only appears between command transactions. Fix lives in `pull_sessions.py::_read_protocol_line()` — skips known noise prefixes (`STATUS active=`, `PONG`, `GOSTEADY-DUMP`, `OK started `, `OK samples=`). Reuse this helper in any new host tool that reads uart1.

**BLE (M6b):**

- **BLE compiled in, disabled at runtime.** The bridge's `CONFIG_BRIDGE_BLE_ENABLE=y` builds all the NUS code, but `BLE_ENABLED=0` is the runtime default. Enable by editing `Config.txt` on the `/Volumes/NO NAME` mass-storage drive (`BLE_ENABLED=0` → `BLE_ENABLED=1`), then **eject the drive in Finder** (that's what triggers the settings write), then unplug + replug USB. Gets wiped every time we `west flash --recover` the nRF5340.
- **iOS system-level Bluetooth grabs the NUS connection slot.** After iOS sees the Thingy:91 X UART advertising once, it may auto-connect at the OS level, which blocks nRF Connect / Bluefy from establishing their own connection (we configured `CONFIG_BT_MAX_CONN=1`). Symptom: the device shows as "connected" in iPhone Settings → Bluetooth but your BLE client app spins forever on connect. Fix: iPhone Settings → Bluetooth → tap (i) next to "Thingy:91 X UART" → **Forget This Device**. Sometimes need to toggle BT off/on too.
- **nRF Connect iOS UTF-8 text field does *not* interpret `\n` as an escape.** Typing `PING\n` sends the six literal characters `P I N G \ n`, not `P I N G <newline>`. The text field's Return key also doesn't produce a newline byte in practice. For manual testing of newline-terminated commands, use the **Byte Array** format and send hex explicitly (`PING\n` = `50 49 4E 47 0A`). For production use `tools/capture.html` which constructs bytes programmatically.
- **CCCD descriptor (UUID 2902) must be written before notifications fire.** Subscribing to the NUS TX characteristic in nRF Connect is a matter of tapping the triple-down-arrow / "enable notifications" button, which writes `01 00` to the 2902 descriptor on that characteristic. Without it, the device's responses go into the void. Web Bluetooth's `startNotifications()` does this automatically.
- **Bridge uart1 baud was the invisible M6b bug.** The bridge's upstream default for uart1 is 115200; our nRF9151 side is 1 Mbaud. USB CDC opens reconfigure baud dynamically via `SET_LINE_CODING`, but BLE peer-conn events pass `baudrate=0` ("don't change"), so without our `current-speed = <1000000>` in `bridge_fw/boards/thingy91x_nrf5340_cpuapp.overlay`, BLE tunneling would silently run at a baud mismatch. **Don't remove that DTS override.**
- **BLE writes >~240 bytes silently dropped end-to-end (fixed 2026-04-22).** A full 12-field `START` payload is ~270 bytes. Chrome's `writeValueWithoutResponse` enforces ATT_MTU−3 and throws on oversize; the `writeValueWithResponse` fallback uses GATT Prepare+Execute Write, but somewhere in the NUS RX handler → bridge ring-buffer → uart1 path the reassembly is broken, so the 91's command parser never sees a `\n`-terminated line. Symptom: `→ START {...}` logs cleanly on the browser side, no `← OK` or `← ERR` ever returns, `← STATUS active=0` forever, `STOP` → `← ERR not active`. Short commands (`STATUS`, `STOP`, `PING`) always worked because they fit in one ATT write. **Fix lives in `tools/capture.html` `sendCommand`** — chunk outbound writes to ≤180 bytes with `writeValueWithoutResponse`, one chunk per ATT packet. Same bug almost certainly exists if anyone ever writes their own Web BLE client, so the chunking pattern is the rule, not the exception. See the "Key Design Decisions" note on why this is a browser-side workaround rather than a firmware rearchitecture.

**Session recording:**

- **Cross-session sample leak into the next session's file (fixed 2026-04-22).** The writer thread's `local_batch` and the msgq retained samples captured during session N's close window (between `k_sem_give(stop_done_sem)` and `s_active = false` — a race where the sampler was still running but the file was already closed). On the next `session_start`, those stragglers got appended to the new file's front, stamped with session N's `t_ms` base. Signature: every session after the first-post-boot had a small prefix (~30–50 samples) of "still cap" motion with `t_ms` values roughly equal to the previous session's duration, followed by a backward jump to `t_ms ≈ 1` and the real motion. Also correlated with `-9/1792` (`-EBADF`) `writer fs_write` errors logged right before the stop log — the writer trying to flush a full batch into an already-closed file. Fix is a **synchronous start-signal handshake**: `session_start` now raises the previously-unused `start_signal` and blocks on a new `start_done_sem` until the writer acknowledges that it has reset `batch_fill = 0` and drained any msgq residue. Combined with the existing `!s_active` guard in the drain loop and an explicit `batch_fill = 0` in the stop branch (both belt-and-suspenders against any other race), the writer is guaranteed to be in a known-clean state before `s_active = true` fires for the new session. `tools/read_session.py` now reports `duration_ms` and `derived_rate_hz` from `max(t_ms) − min(t_ms)` rather than `last − first`, and flags `t_ms_monotonic=false` with the first backward jump location — so any pre-fix session files that survive on disk still parse cleanly and loudly self-identify as contaminated.
- **HardFault on STOP / `-EBADF` on writer's `fs_write` (fixed 2026-04-25).** The 2026-04-22 fix above closed the START race (samples leaking *into* the new session's file). The symmetric STOP race remained: `gosteady_session_stop()` raised `stop_signal`, took `stop_done_sem`, then set `s_active = false` — so between writer ack and the flag flip, the sampler kept enqueueing samples, the writer woke up, drained samples that passed the still-true `s_active` check, accumulated a 64-sample batch, and called `fs_write` against the closed file. Symptom: `<err> gs_session: writer fs_write -9/1792` logged on every clean close. Most of the time benign (file already had its header from `rewrite_header()`). On 2026-04-23 09:53:49 the same race landed in a use-after-free path inside LittleFS and HardFaulted. Fix: move `s_active = false` *before* raising `stop_signal`. The sampler stops enqueueing immediately, the writer's per-sample `s_active` check discards anything in flight, no batch can fill, no `fs_write` can race against `fs_close`. Mirrors `session_start` where `s_active = true` is set only AFTER the file is open and the handshake is acked. Verified across rapid back-to-back START/STOP cycles — zero `-EBADF`.

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

The repo is at a **BLE-controllable session-logging target with closed-loop POST-WALK capture + a v1 Python distance estimator with auto-surface roughness adjustment** — the walker cap can be sealed and the operator drives `START` / `STOP` wirelessly from Chrome on the Mac, with a per-run popup that collects the operator's validity + notes decision in the same UI. On boot (nRF9151 side): mount LittleFS, bump the boot counter, start a 100 Hz BMI270 sampling thread with a decoupled writer, wire SW0 as a bench-fallback toggle for session start/stop, bring up the uart1 dump/control channel. LED state tells the operator which mode they're in (solid green while recording, purple blink while idle). On the nRF5340 side: our patched connectivity bridge runs, exposing BLE NUS tunneled to uart1. As of **2026-04-25**, **Milestones 1–7 are complete and shakedown-validated**; **M8 is paused at 19/30 runs** (10 indoor polished concrete + 8 outdoor concrete sidewalk + 1 outdoor stationary baseline; carpet runs 21–30 deferred); **M9 Phase 1–4 are complete** with the algorithm architecture finalized as **auto-surface roughness adjustment** — operator never picks a surface; the firmware computes a session-level roughness metric from the IMU stream and selects coefficients automatically. Cross-surface LOO (n=16): **17.2% indoor MAPE / 27.6% outdoor MAPE / 22.4% pooled** with one specific R-overlap walk dominating the outdoor error; v1.5 retrain at n=24+ with carpet expected to close the gap. **HardFault on STOP fixed in firmware** (2026-04-25 commit `81ddb11` — writer/session_stop race in `s_active` ordering; full uart0 logger + capture.html three-tier log persistence shipped in commit `00e6ed4`). Streaming-first algorithm by construction so the M10 C port is a direct translation; the only new C primitive is the roughness computation + small classifier-table lookup at session close.

- **M1 (dev env):** `<inf> gosteady: heartbeat tick=N` prints at 1 Hz on `/dev/cu.usbmodem102`.
- **M2 (sensor bring-up):** BMI270 (SPI) and ADXL367 (I²C) both produce sane readings — ~±1 g on the gravity axis at rest, noise-floor-level signal on the other axes. BMI270 is configured at 4 g / 500 dps / 100 Hz per the v1 annotation schema. **Note:** Z-axis signs are opposite between BMI270 (+1 g) and ADXL367 (-1 g) because the two chips are mounted in different orientations on the PCB — handle this at the algorithm fusion layer.
- **M3 (external flash):** LittleFS mounted at `/lfs` on an 8 MB `littlefs_storage` partition carved out of `external_flash` by the NCS partition manager. Persistence verified — `/lfs/boot_count` increments across hardware resets. 4096-byte erase blocks, 512-cycle wear threshold. Partition range: `0x4d2000`–`0xcd2000` (8 MB); remainder of external flash (`0xcd2000`–`0x2000000`, ~19 MB) stays free for future use.
- **M4 (session logging):** 256-byte packed binary header (`src/session.h`) stamps the 13 FIRMWARE + 12 PRE-WALK fields from the v1 annotation schema; body is 28-byte packed sample records (uptime_ms + 3×float accel m/s² + 3×float gyro rad/s) from BMI270 at 100 Hz. UUIDv4 from the TF-M PSA RNG. Files land at `/lfs/sessions/<uuid>.dat`. SW0 short-press toggles start/stop. On close, the header is base64-logged to UART so the host ingest script can round-trip without needing USB mass-storage. Every controlled-vocab enum decodes cleanly through `tools/read_session.py`.
- **M5 (USB dump + sampler decoupling):** `src/dump.c` runs a tiny line protocol on nRF9151 uart1 (routed by the nRF5340 connectivity bridge to `/dev/cu.usbmodem105 @ 1 Mbaud`): `PING`, `LIST`, `DUMP <name>`, `DEL <name>`. `tools/pull_sessions.py` drives the host side — round-trips session files to local disk and cross-validates each with `read_session.py`. Under the hood, `src/session.c` was refactored: sampler thread now only does a non-blocking `k_msgq_put`; a dedicated writer thread drains the queue, batches into `fs_write`, and rewrites the header on close. Rate went from ~96 Hz → **100.08 Hz** (1,300 samples / 12.989 s body duration on a fresh bench session). `flash_errors` now means `dropped_samples` (queue-full events); stays 0 when headroom is adequate. `battery_mv_{start,end}` and `session_*_utc_ms` still stamp 0 until M12 / nPM1300 fuel gauge.
- **M6a (transport-agnostic session control):** `src/control.c` parses `START <json>` / `STOP` / `STATUS` commands using Zephyr's JSON library against a schema mirroring `struct gosteady_prewalk`. All 7 controlled-vocab fields are string-validated against the v1 vocabulary; free-form strings are truncated-copied. The parser writes its response into a caller-provided buffer — transport-agnostic by design, so the M6b BLE bridge can tunnel the same commands from a NUS write without the nRF9151 firmware caring. Wired into `src/dump.c` as an additional dispatch target, so the existing uart1 channel now carries both file ops and session control. `tools/control.py` drives it from the host (supports named presets like `concrete-normal-20` so the operator doesn't hand-type JSON 30 times). End-to-end test: a preset-driven `start → sample → stop → pull` cycle produced a session file whose `course_id`, `intended_distance_ft`, and `operator_id` came through exactly as sent (and the other 9 fields all validate against the canonical vocabulary). The M4 button handler is still wired up as a fallback for bench work while the cap is open. Rate held at ~100 Hz through the JSON path.
- **M6b (BLE control over NUS):** `bridge_fw/` is a vendored fork of Nordic's `applications/connectivity_bridge` with four surgical edits (see `bridge_fw/PATCHES.md`) that retarget the Nordic UART Service from uart0 to uart1 — the latter being where our M5/M6a command parser listens. The nRF5340 net core gets built as `ipc_radio` automatically via sysbuild and programmed alongside the app image. BLE is compiled in upstream but **gated off at runtime by default**; the operator enables it by editing `BLE_ENABLED=0` → `1` in `Config.txt` on the bridge's USB mass-storage endpoint and ejecting. `bridge_fw/boards/thingy91x_nrf5340_cpuapp.overlay` pins the bridge's uart1 default to 1 Mbaud so the BLE path's `baudrate=0` ("don't change") peer-conn event doesn't leave a 115200/1Mbaud mismatch against the nRF9151. **Operator-facing UI:** `tools/capture.html` is a single-file Web Bluetooth page with a preset button per row of the v1 30-run capture matrix and a persistent STOP button. As of 2026-04-23 we drive it from **Chrome on the Mac** (not Bluefy on iOS — Bluefy's App Store build doesn't opt into `WKWebView.isInspectable` so Safari Web Inspector can't attach, blocking debug; Chrome gives us full DevTools + Claude-in-Chrome extension integration for end-to-end debugging).
- **M7 (POST-WALK capture loop):** `capture.html` gained a post-STOP modal that fires on every `OK samples=N` notification with three fast-path actions — **✓ Good** (writes `valid=Y` with defaults), **✗ Discard** (writes `valid=N` + `discard_reason` dropdown), and **📝 Notes** (full 7-field form covering the POST-WALK annotation schema). Entries are keyed by `session_uuid` in `localStorage` under `gosteady_post_walk_notes_v1`; an **Export capture notes (JSON)** button downloads a schema-v1 sidecar to `~/Downloads`. On the firmware side, `control.c::cmd_start` now echoes the UUID in its response (`OK started <uuid>`) via a new `gosteady_session_get_uuid_str()` public API in `src/session.h`, so the browser can correlate its notes to the same primary key the session file header carries. `tools/ingest_capture.py` joins pulled `.dat` headers with the notes JSON and emits a CSV matching the `Captures` sheet of `GoSteady_Capture_Annotations_v1.xlsx` column-for-column (13 FIRMWARE + 12 PRE-WALK + 7 POST-WALK + 2 DERIVED), plus two trailing diagnostic columns (`run_idx`, `post_walk_status`) the operator drops before spreadsheet append. End-to-end validation: a 10-run shakedown across the Indoor Polished Concrete segment produced 10/10 clean session files (100.00 Hz, mono=✓, flash_errors=0, UUID-match) and 10/10 POST-WALK notes that joined cleanly at ingest. Stationary baseline (run 9) showed |a|=1.01g / |ω|=0°/s across 37 s of captured data; stumble run (run 10) showed a textbook 4.76g impulse at the event moment — IMU signal quality is solid at the hardware level.
- **M9 Phase 1–3 (Python distance estimator, 2026-04-24):** M8 was paused at 10/30 runs to develop the algorithm pipeline end-to-end first. The `algo/` package implements a streaming-first step-based distance estimator — **streaming DF-II-T biquad filters** (matching CMSIS-DSP's `arm_biquad_cascade_df2T_f32`) for 0.2 Hz HP gravity removal and 5 Hz LP peak shaping → **Schmitt-trigger peak FSM** with per-peak feature extraction (amplitude / duration / energy) → **per-run distance regression** against `actual_distance_ft`. A parallel **500 ms running-σ motion gate** with hysteresis produces `motion_duration_s` (the `active_minutes` product metric) and passes the stationary robustness gate. Phase-3 single-surface (indoor polished concrete) baseline: single-feature (amplitude) stride regression with loose Schmitt thresholds (0.02 g enter / 0.005 g exit / 0.5 s min-gap), **16.4% LOO MAPE / 3.45 ft MAE / both robustness gates PASS**. Key finding: the two-wheel/glide gait produces a compound 2:1 impulse structure (loose detector finds ≈2× the operator-perceived step count — one for wheel glide, one for rear-leg plant), but the stride regression adapts naturally to whatever detector density is consistent. Energy-based alternatives lost decisively (E ∝ speed² while distance ∝ speed — physics mismatch). Multi-feature regression overfit at n=8.
- **M9 Phase 4 (cross-surface validation + auto-surface architecture, 2026-04-25):** Captured the outdoor concrete sidewalk segment (8 walks + 1 stationary baseline; s-curve skipped per spacing constraints), then re-ran the algorithm across both surfaces. **Three universal findings**: (a) detector behaves identically across surfaces (detected/manual ratio ≈1.5× on both — same compound-impulse signature); (b) stride lengths essentially identical (median 1.43 indoor / 1.39 outdoor ft/step — same gait); (c) walking signal σ ~1.93× higher outdoor (texture-induced impulse energy). **One critical surface-specific finding**: the slow-vs-fast amplitude relationship *inverts* outdoors — slow walks have the *biggest* σ (1.04 g) because slower wheel motion gives more dwell time per surface irregularity. Indoor coefficients applied to outdoor data without surface info hit 90.3% MAPE — surface info is non-optional for v1. **Architecture decision (2026-04-25): auto-surface roughness adjustment.** The firmware computes a session-level **motion-gated inter-peak RMS** as a continuous roughness metric R, then auto-classifies the session into a surface-coefficient bucket. Operator does NOT pick the surface — the algorithm self-calibrates from the IMU stream. With n=16 walks across two surfaces, hard threshold τ=0.245 hits **22.4% pooled MAPE (15/16 walks correctly classified)**. Per-surface operator-supplied baseline is 16.4% / 23.3%; auto-surface costs ~2.5 percentage points pooled vs operator-supplied, with the gap concentrated on a single boundary case (outdoor run 17, R=0.219). Expected to close at v1.5 with carpet captures + more data per surface widening the R distribution gaps. See `algo/run_auto_surface.py`, `algo/run_roughness_experiment.py`, `algo/run_cross_surface.py`, `algo/run_summary.py`, and `algo/roughness.py` for the full experiment ladder.
- **HardFault on STOP fixed (2026-04-25, commit `81ddb11`):** Symptom — first encountered as `← FATAL ERROR: HardFault` over BLE on a STOP, after 7 outdoor capture attempts. uart0 firmware-side logging (added the same morning) showed every session's close path emitting `<err> writer fs_write -9/1792` even on clean closes — `-EBADF` on the writer's batch flush. Root cause: in `gosteady_session_stop()` the order was `raise(stop_signal); k_sem_take(stop_done_sem); s_active=false`, which let the sampler keep enqueueing after the writer had already closed the file (window between writer ack and `s_active` flip). Sampler enqueues → writer wakes → drains samples that pass the now-stale `s_active` check → batch fills → `fs_write` against closed file. Most of the time the failure is benign (logged error, file already had its header); at 2026-04-23 09:53:49 the same race landed in a use-after-free path inside LittleFS and HardFaulted. Fix: move `s_active = false` to **before** raising `stop_signal`, mirroring `session_start`'s structure where `s_active = true` is set only after the writer has acked and the file is open. Verified across 4 stress-test START/STOP cycles (8 s, 2 s, 1 s, 1 s rapid back-to-back) — zero `-EBADF` errors. Pairs symmetrically with the 2026-04-22 start-side handshake.
- **Logging infrastructure (2026-04-25, commit `00e6ed4`):** `tools/log_console.py` is a continuous uart0 console logger that auto-saves to `logs/uart0_YYYY-MM-DD.log` with timestamped lines, auto-reconnects across power cycles, and captures every session boundary + base64 header + Zephyr fault dump that would otherwise vanish from a screen session. uart0 is one-way (firmware → host) so it never conflicts with `pull_sessions` / `control` / `cleanup_device`. `tools/capture.html` got **three-tier log persistence**: DOM (last 80 entries shown), localStorage (full buffer up to 10000 entries, restored on page load), and disk auto-export on `FATAL` / `HardFault` patterns + manual "Export Log" / "Clear Log" footer buttons. POST-WALK notes are still a separate localStorage key with their own "Export capture notes (JSON)" button — the export-everything-in-one-button consolidation is a follow-up.
- **M14-prep Phase 5 (FIELD_MODE Kconfig, 2026-04-27):** Single Kconfig flag — `CONFIG_GOSTEADY_FIELD_MODE` — that gates off bench-only paths in deployment builds. Lives in a new project-local `Kconfig` file at the project root (`source "Kconfig.zephyr"` first, then a `menu "GoSteady"` block with the symbol). Default is `n` (bench), so `prj.conf` builds keep all current tooling. Deployment builds apply `prj_field.conf` overlay via `west build -- -DEXTRA_CONF_FILE=prj_field.conf`. **What gets gated** (all via `IS_ENABLED(CONFIG_GOSTEADY_FIELD_MODE)` so dead code is still compiler-checked):
    - SW0 button handler — `configure_button()` not called; button press sem never fires.
    - uart1 dump channel — `gosteady_dump_start()` not called; this also gates off the BLE NUS START path since the bridge tunnels NUS RX through the same uart1 stream the dump channel listens on.
    - Heartbeat tick LEDs (purple blink) + `LOG_INF("heartbeat tick=N")` line + `log_adxl367_motion_counts()`. Main loop's idle branch becomes a no-op except the auto-stop check.
    - Recording green LED — `led_set_recording(true)` early-returns in field mode. Per M10.5 "no LEDs in normal operation."
    - Bench `BENCH_PREWALK` swapped for `FIELD_PREWALK` (subject_id="deployed", course_id="field", operator_id="auto"). Patient attribution is post-hoc cloud-side per portal contract.
  Deployment build still keeps: ADXL367 wake-on-motion, BMI270 PM, auto-start coordinator, auto-stop, sampler, writer, M9 algo, cellular module, session lifecycle. **Build verified 2026-04-27**: `build_field/merged.hex` = 789 KB (`CONFIG_GOSTEADY_FIELD_MODE=y` confirmed in `.config`); `build/merged.hex` = 805 KB (~16 KB code dropped — button + dump + heartbeat). Bench reflashed; "Bring-up complete. Press SW0 to start/stop a session." log line + heartbeat ticks intact. Field-mode end-to-end on hardware needs a separate flash-and-bench session.
- **M14-prep Phase 4 (Zephyr PM = SoC idle, 2026-04-27):** `CONFIG_PM=y` in `prj.conf` enables the kernel-idle low-power transition: when all firmware threads are blocked on `k_msleep` / `k_poll` / `k_sem_take`, the kernel idle thread drops the nRF9151 into System ON Idle (~50 µA) instead of staying at ~1 mA active. **Pre-flight thread audit verified all threads use blocking primitives** that release the idle thread (no busy waits, no `k_yield` loops). Two minor remaining wake sources logged for follow-up: the sampler polls at 100 Hz when no session is active (could block on a `session_started` signal between sessions); the dump UART polls its RX ring at 200 Hz (could be IRQ→sem driven). Both immaterial in deployment because FIELD_MODE (Phase 5) gates off the dump channel and the sampler-when-idle wakes are still small individual ticks (~µs). `CONFIG_PM_DEVICE` deliberately NOT enabled — that's per-driver PM (suspend SPI/I2C peripherals), much riskier touch-up; defer to Phase 4.x once we have field battery numbers. **Functionally validated 2026-04-27**: full auto-start → walk → auto-stop cycle ran cleanly with PM on (47 s session, 4244 samples, 100 Hz exact, dropped=0, M9 algo correct). Actual µA measurement requires a multimeter on the Thingy:91 X current jumper — deferred to a separate bench session, but the architectural piece (kernel idle is now PM-driven) is shipped.
- **M14-prep Phase 3 (no-motion auto-stop, 2026-04-27):** Inverse of Phase 2 — sessions auto-close after sustained stillness without operator intervention. Hooks into the M9 motion gate that already runs in the writer thread on every persisted sample (added in M10). The writer maintains a `s_stationary_samples` counter that resets to 0 on every motion-active sample and increments on every motion-inactive sample. Main thread polls via `gosteady_session_stationary_samples()` getter once per heartbeat tick (1 Hz) and calls `gosteady_session_stop()` once `stationary_samples / 100 ≥ AUTO_STOP_STATIONARY_S` (default 15 s for bench, M10.5 spec calls for 30 s in production — TODO: move to Kconfig with FIELD_MODE override). The motion gate's own 500 ms running window + Schmitt exit-hold (2 s) already debounces against brief mid-walk pauses, so the counter only climbs after the gate has truly settled into stillness. **Bench-verified end-to-end 2026-04-27 — full deployment-mode flow with zero button presses:** walker picked up → 0.7 s confirmation → green LED + auto-start → walking → set down → 15 s of stillness → auto-stop → idle. Resulting session: `distance_ft=4.06, R=0.0265, surface=0, steps=10, motion_s=10.40, total_s=25.90, motion_frac=0.402` — algo correctly accounts for the 15 s of stillness in its outputs (motion_frac drops from ~1.0 in manual sessions to 0.40 in auto-stopped sessions, exactly what the math predicts).
- **M14-prep Phase 2 (auto-start on motion, 2026-04-27):** Wires Phase 1a (ADXL367 wake-on-motion) and Phase 1b (BMI270 PM) into the deployment-mode auto-start state machine per the M10.5 spec. New `auto_start` thread in `src/main.c` blocks on `motion_event_sem`, gates on `gosteady_session_is_active()` (manual SW0/BLE start takes precedence), then runs a **500 ms BMI270 confirmation window** before committing to a session: resume BMI270, settle + drop 4 (Phase 1b warm-up), sample 50 ticks of `|a|/g − 1`, compute σ. If σ > 0.05 g → real walking → `gosteady_session_start()` + green LED, sampler thread takes over. Else → suspend BMI270, drain `motion_event_sem` (debounce), return to idle. Why a confirmation window: chip-level activity at 50 mg threshold fires on plenty of things that aren't walking (door slam, walker bumping wall, cap setting down on hard surface), and each false-positive session-open + close cycle wastes BMI270 power and writes a useless little file. Spending 500 ms confirming with the algo-grade sensor before committing pays for itself in deployment. **Bench-verified 2026-04-27 — clean separation.** Walking pickup: σ=0.0815 g → CONFIRMED, session opened in 744 ms (matches predicted 0.7 s budget). Five subsequent incidental motion events (σ=0.0021–0.0427 g) all correctly REJECTED — no spurious sessions. Latency budget: ~100 ms ADXL367 chip filter + ~50 ms coordinator + warm-up + 500 ms confirmation + ~few ms session_start handshake = ≈0.7 s real-motion-to-green-LED. SW0 + BLE START paths preserved as bench fallback; FIELD_MODE Kconfig (Phase 5) will gate them off in deployment.
- **M14-prep Phase 1b (BMI270 suspend between sessions, 2026-04-27):** Second piece of the deployment-mode power architecture. The BMI270 (~325 µA continuous in normal mode, ~0.5 µA suspended) now sleeps between sessions — that's the dominant idle term in the deployment power budget. Implementation in `src/main.c::sampler_entry`: on `idle→active` transition (session start), call `bmi270_set_active(true)` to write `SAMPLING_FREQUENCY=100` (re-enables `PWR_CTRL.{ACC_EN,GYR_EN}` per the upstream Zephyr driver) → settle 25 ms → 2 prime fetches → drop next 4 body samples (chip's data registers report zeros for ~3 ODR periods past the settle window — empirically determined on bench with a debug log instrumented in the discard branch). On `active→idle` transition (session stop), suspend again. Skip `sensor_sample_fetch` entirely while idle so SPI bus is quiet. Boot-time also suspends the chip after `configure_bmi270()` so deployment-mode firmware spends pre-first-session in idle current (one-time 30 ms warm-up at boot before the first suspend so the chip's data registers contain real gravity samples before going to sleep — needed for the ZeroSampleAvoidance pattern to work after subsequent resumes). Verified on bench: first persisted sample at t_ms=31 with real gravity data, rate=100.01 Hz, monotonic, dropped=0. Cost: 4 dropped samples (~40 ms) at session start = immaterial vs the M9 motion gate's 500 ms settling.
- **M10.7 production-telemetry stack + M12.1c.2 production-shaped heartbeat — code-complete + bench-validated end-to-end 2026-04-29 (6 commits across the four milestones; +1 bug-fix follow-up). Cloud Shadow `GS9999999999` carries every locked optional extra cleanly. Posted as §F5 in the coordination doc 2026-04-29.**
  - **M10.7.1 Storage repartition** (`pm_static.yml`, `sysbuild.conf`; commit `cd377af`). Carved the unused 19 MB after `littlefs_storage` into three new external-flash partitions: `crash_forensics` (64 KB at 0xcd2000), `telemetry_queue` (256 KB at 0xce2000), `snippet_storage` (16 MB at 0xd22000). Catch-all `external_flash` remainder shrank to ~2.86 MB. `sysbuild.conf` opts out of the Thingy:91 X board's factory `pm_static` override via `SB_CONFIG_THINGY91X_NO_PREDEFINED_LAYOUT=y` and re-asserts `BOOTLOADER_MCUBOOT` + secure-boot Kconfigs explicitly. Build verified across all three configs.
  - **M10.7.2 nPM1300 fuel gauge wiring** (`src/battery.{h,c}`, `src/battery_model.inc`; commit `1ac3614`). Wraps the nPM1300 charger sensor + `nrf_fuel_gauge` SoC estimator in a public API (`gosteady_battery_init`, `gosteady_battery_get(mv,pct)`). Worker thread runs `nrf_fuel_gauge_process` every 5 s; getter returns mutex-protected cached snapshot. Cloud heartbeat now reads real `battery_pct` (0.0-1.0) + adds optional `battery_mv` instead of M12.1c.1's hardcoded `0.5`. Battery model is the bundled "Example" 1100 mAh LiPol from the upstream NCS sample (close enough to the Thingy:91 X's LP803448 ~1300 mAh for v1; v1.5 should swap in a tuned LP803448 model from field discharge curves). Gated on `CONFIG_NRF_FUEL_GAUGE` (auto-selected in `prj_cloud.conf` + `prj_field.conf`; bench builds skip the ~50 KB lib).
  - **M10.7.3 Crash forensics + watchdog** (`src/forensics.{h,c}`; commit `32daa1e`). Persists `boot_count` + `reset_reason` (hwinfo bitmask, both this-boot and previous-boot) + `fault_count` + `watchdog_hits` + `last_fault` (PC/LR/xPSR/K_ERR_reason/uptime_ms/thread_name) into a 256-byte record on the `crash_forensics` partition. Read-modify-erase-write pattern on the whole 4 KB block; wear bounded to ~270 years at 1 boot/day. Overrides Zephyr's weak `k_sys_fatal_error_handler` to snapshot the ESF + flash-write before the default reboot. Hardware watchdog (nRF91 `wdt0`, 60 s timeout) kicked from a dedicated supervisor thread at 20 s cadence. New `CONFIG_GOSTEADY_FORENSICS_ENABLE` Kconfig (selects `WATCHDOG` + `HWINFO` + `REBOOT`); on by default in cloud + field, off in bench. Public API exposes `get_reset_reason(out, sz)`, `get_fault_count`, `get_watchdog_hits`, `fault_counters_json(buf, sz)`, `get_uptime_s` for the M12.1c.2 heartbeat extras. No per-thread liveness check in v1 — supervisor existence + kick is the canary.
  - **M12.1c.2 Production-shaped heartbeat** (`src/cloud.{c,h}`, `src/version.h`; commit `916c2aa`). Promoted M12.1c.1's one-shot bring-up heartbeat to the production shape: hourly cadence (`HEARTBEAT_INTERVAL = 60 min`) with linear-backoff retry (1 / 5 / 15 min, max 3 attempts per tick) before giving up on this hour's heartbeat — beyond ~21 min into the hour buys nothing because the cloud's offline-detection threshold is 2 hr. Adds all locked optional extras independently gated: `battery_mv`, `firmware`, `uptime_s`, `last_cmd_id`, `reset_reason`, `fault_counters` (JSON object), `watchdog_hits`. New `gosteady_cloud_set_last_cmd_id()` public API is a dormant hook for M12.1e.2 to populate when the activate cmd lands. Heartbeat payload max bumped 256→512 B (full extras ~360 B observed). New `src/version.h` is the single source of truth for `GS_FIRMWARE_VERSION_STR`; bumped `0.7.0-cloud` → `0.8.0-prod`.
  - **Build sizes after the four-milestone sprint:** bench 811 KB (unchanged — all gates are off); cloud 954 KB (+60 KB over M12.1d for fuel-gauge lib + battery model + forensics); field 931 KB (+62 KB over M12.1d).
  - **Hardware bench-validation 2026-04-29:** flash → uart0 boot stream → AWS IoT Shadow round-trip all confirmed clean. First production-shape heartbeat published `2026-04-29T20:02:32Z` with payload `{serial, ts, battery_pct=0.936, rsrp_dbm=-82, snr_db=5, battery_mv=4218, firmware="0.8.0-prod", uptime_s=13, reset_reason="SOFTWARE", fault_counters={"fatal":0,"asserts":0,"watchdog":0}, watchdog_hits=0}` (253 B serialized). Boot-to-PUBACK 18-19 s. Battery model is the bundled "Example" 1100 mAh LiPol from the upstream NCS sample — Thingy:91 X carries an LP803448 (~1300 mAh) so voltage-based SoC correction keeps it within ±5-10 % absolute (fine for v1; v1.5 should swap in a tuned model from field discharge curves). The cloud Shadow merge correctly preserved a stale `fault_counters.i2c: 0` key from an earlier cloud-side probe — accept-all per-leaf merge per portal contract working as documented (immortal absent a `null` write; flagged in §F5.1 for future shadow-cleanup awareness, not a bug).
  - **M10.7.3 fault-recovery axis bench-validated end-to-end via two stress-test commands** (`CRASH` / `STALL` on the uart1 dump channel, gated on bench-only `CONFIG_GOSTEADY_FORENSICS_STRESS=y`):
    - `STALL` → wedges WDT supervisor → 60 s later HW WDT fires → SoC reset → next-boot init reads `RESET_WATCHDOG` via hwinfo → `reset_reason=WATCHDOG`, `watchdog_hits` ++.
    - `CRASH` → `k_panic()` → `k_sys_fatal_error_handler` → noinit-stamp → `sys_reboot(SYS_REBOOT_WARM)` → next-boot init drains noinit → `reset_reason=SOFTWARE`, `fault_counters.fatal` ++.
    - Cycle times: WDT path ~60 s, fault-handler path ~30 s (cellular re-attach is the dominant cost on the fault path).
    - Pre-test: `fatal=0, watchdog=3, watchdog_hits=3`. Post-STALL: `watchdog=4, watchdog_hits=4`. Post-CRASH ×2: `fatal=2`. All counters monotonically increase via the cloud Shadow.
  - **Two M10.7.3 bugs found + fixed during validation, in commit `eea8d7e`:**
    - **In-handler flash persist doesn't survive the reboot timing on this nRF9151+TF-M platform.** Original M10.7.3 handler called `flash_area_erase + flash_area_write` directly. Post-`LOG_PANIC` the kernel scheduler is locked, the SPI flash driver state freezes, and the writes never complete before reboot fires. Empirical signal: `fault_counters.fatal` stayed 0 across forced fault triggers; only `watchdog_hits` (which next-boot init bumps from the hwinfo bitmask, no fault-time flash I/O needed) tracked recoveries. Fix: stamp fault info into a `__noinit` SRAM struct (Cortex-M `NVIC_SystemReset` retains SRAM) gated by a magic word, drain in next-boot init where flash I/O is fully ready, clear magic to prevent double-counting.
    - **`k_fatal_halt` was a 60 s death spiral.** It's an infinite loop, not a reboot. Without explicit handler-side reset trigger, recovery only happened via the watchdog timeout — costing 60 s per fault AND mis-attributing the reset_reason as `WATCHDOG`. Fix: handler now calls `sys_reboot(SYS_REBOOT_WARM)` after the noinit stamp; falls through to `k_fatal_halt` only if reboot somehow returns. Recovery time drops 60 s → ~30 s, and `reset_reason` correctly reads `SOFTWARE` for handler-recovered faults so cloud-side triage can distinguish from kernel hangs.
  - **Stress-test infrastructure preserved in tree as M14.5 acceptance-test surface** (commit `eea8d7e`). `CONFIG_GOSTEADY_FORENSICS_STRESS=y` (depends on `GOSTEADY_FORENSICS_ENABLE`, default n) compiles in CRASH and STALL handlers. Bench-only — never enable in a real deployment build (the doc spec for the Kconfig says so explicitly). Each shipping unit (`GS0000000001/2/3`) should be stress-tested through both commands during M14.5 site-survey shakedown to confirm forensics+watchdog still recover cleanly under the housing+battery conditions. Build invocation: `west build -b thingy91x/nrf9151/ns -d build_stress -p always -- -DEXTRA_CONF_FILE=prj_cloud.conf -DCONFIG_GOSTEADY_FORENSICS_STRESS=y`; reflash with the clean `prj_cloud.conf` (or `prj_field.conf` for shipping) before site-survey unit goes live.
- **M14-prep Phase 1a (ADXL367 wake-on-motion, 2026-04-26):** First piece of the deployment-mode power architecture — interrupt-driven wake-on-motion via the ADXL367 INT1 line replaces the M2-era 1 Hz sanity poll. Bench-verified end-to-end: the chip now fires INT1 on any motion above 49 mg sustained for ≥100 ms (production thresholds: `act_thr=200 raw, act_time=10 samples` at the upstream-default 100 Hz ODR), the Zephyr GPIO ISR catches it, the work-queue handler dispatches our callback, and our trigger-handler increments an atomic counter that the foreground heartbeat loop reads (logs only on change to avoid spam). Two upstream Zephyr driver bugs found and worked around in `src/main.c`:
  - **LINKED-mode deadlock.** The upstream `adxl367` driver hardcodes `LINKLOOP=01` (LINKED) in `act_proc_mode` whenever `CONFIG_ADXL367_TRIGGER` is enabled. With LINKED + `AWAKE=1` (the chip's state in MEASURE mode), the activity state machine starts in the "looking-for-inactivity" half of the loop, and `STATUS.ACT` never fires on motion until INACT fires once first — which can't happen if INACT thresholds are below 1 g of gravity. ADI's documented workaround is DEFAULT mode (`LINKLOOP=00`), where ACT and INACT events fire independently regardless of any chip state. Our workaround in `apply_adxl367_default_mode_workaround()` does a STANDBY → clear LINKLOOP → MEASURE sequence right after `sensor_trigger_set`. Verified by direct register dump that ACT_INACT_CTL goes from `0x1f` (driver default) to `0x0f` (DEFAULT mode). References: [ADI EZ — adxl367 loop mode](https://ez.analog.com/mems/f/q-a/575264), [ADI EZ — ADXL367 initialization issue](https://ez.analog.com/mems/f/q-a/575260).
  - **`sensor_trigger_set` silently disables GPIO interrupt on unsupported types.** Driver disables INT1 GPIO interrupt at the very top of `adxl367_trigger_set`, then switches on `trig->type`. The `default:` case (anything other than THRESHOLD or DATA_READY) returns `-ENOTSUP` early — *without* re-enabling the GPIO interrupt. The result: INT1 line pulses fine but the Zephyr work handler never fires. Our original code registered TWO triggers (one for `SENSOR_TRIG_THRESHOLD`, one for `SENSOR_TRIG_DELTA`) and the second one was bricking the path. Fix: only register `SENSOR_TRIG_THRESHOLD` — that handler covers both ACT and INACT events per `adxl367_thread_cb` in the driver. **Both bugs worth filing upstream as Zephyr issues / PRs**; for now we live with the local workaround which is small, well-commented, and verified.
- **M12.1a (cellular bring-up, 2026-04-26):** First cut of `src/cellular.{h,c}` — drives `nrf_modem_lib` + LTE link control + `nrf_modem_at` to attach to LTE-M, then reports RSRP/SNR + UTC time once registered. Pure bring-up: no MQTT, no sockets, no telemetry yet. `prj.conf` adds `CONFIG_NRF_MODEM_LIB=y` + `CONFIG_LTE_LINK_CONTROL=y` + `CONFIG_LTE_LC_PSM_MODULE=y` + `_EDRX_MODULE` + `_CONN_EVAL_MODULE`. Reporter thread polls `AT+CESQ` for RSRP, `AT%XSNRSQ?` for SNR (Nordic-modem extension), `AT+CCLK?` for network UTC. **Bench result on first try:** modem registered as `roaming` in **6.0 s** from boot (Nordic Onomondo SIM on local LTE-M carrier), `psm: tau=3240 s, active=-1 s` (54 min sleep negotiated; active timer rejected), `rsrp=-100 dBm snr=2 dB`, `network_time=2026-04-26T17:58:14Z`. PSM `active=-1` is benign — the modem reports the network's response and -1 means "network didn't grant the active timer we asked for"; PSM still works, the modem just transitions immediately. M5/M6/M7/M10 paths untouched: `littlefs_storage` partition unchanged at `0x4d2000-0xcd2000`, boot_count incremented cleanly, BMI270 + ADXL367 still sampling. Outstanding nit: first signal/time poll fired at uptime 1:12 instead of registration+5s as coded — reporter thread's first iteration is delayed by something (possibly `nrf_modem_at_scanf` blocking on the AT+CESQ call until RRC has fully settled). Not blocking; will refine cadence when M12.1c heartbeat publish lands.

The repo is public on GitHub at `https://github.com/Jabl1629/gosteady-firmware` and also serves the operator UI via GitHub Pages at **`https://jabl1629.github.io/gosteady-firmware/tools/capture.html`** (auto-rebuilds on every push to `main`).

### File Structure

```
gosteady-firmware/
├── CMakeLists.txt                    # Zephyr app build (main.c + session.c + dump.c + control.c)
├── prj.conf                          # Kconfig for nRF9151 app (sensors, LittleFS, UART, JSON, POLL, etc)
├── boards/
│   └── thingy91x_nrf9151_ns.overlay  # Enable BMI270, ADXL367, uart1
├── src/                              # ** nRF9151 application **
│   ├── main.c                        # LED/heartbeat, button handler, 100 Hz sampler thread
│   ├── session.h/c                   # Session file format + lifecycle (start/append/stop), msgq + writer thread
│   ├── dump.h/c                      # uart1 line protocol (LIST/DUMP/DEL/PING)
│   ├── control.h/c                   # Transport-agnostic START/STOP/STATUS JSON parser
│   ├── cellular.h/c                  # M12.1a: nrf_modem_lib + LTE attach + AT+CCLK?/AT+CESQ reporter
│   └── algo/                         # ** M10 on-device V1 distance estimator (DONE 2026-04-25) **
│       ├── gosteady_algo_params.h    # Auto-generated from algo/export_c_header.py — DO NOT EDIT
│       ├── gs_filters.h/c            # DF-II-T biquad cascade (CMSIS-DSP layout)
│       ├── gs_motion_gate.h/c        # Running-σ + Schmitt FSM
│       ├── gs_step_detector.h/c      # Schmitt peak FSM + per-peak features
│       ├── gs_roughness.h/c          # Batch motion-gated inter-peak RMS
│       ├── gs_pipeline.h/c           # Orchestrator + surface classifier + stride sum
│       └── test_vectors/             # Reference fixtures for the C-port regression suite
│           ├── indoor_run05_walk_20ft.{bin,json}      # Canonical indoor walk
│           ├── indoor_run09_stationary_30s.{bin,json} # Stationary robustness check
│           └── outdoor_run13_walk_20ft.{bin,json}     # Cross-surface (outdoor) walk
├── tests/host/                       # ** Host-side algo regression suite (no Zephyr dep) **
│   ├── Makefile                      # `make` builds + runs against the .bin fixtures
│   ├── test_loader.h/c               # Fixture loader (mirrors algo/export_reference_vectors.py layout)
│   └── test_main.c                   # Per-module + end-to-end checks (31/31 passing)
├── bridge_fw/                        # ** nRF5340 bridge firmware (vendored fork of Nordic's) **
│   ├── PATCHES.md                    # What we changed and why
│   ├── boards/thingy91x_nrf5340_cpuapp.overlay  # uart1 default 1 Mbaud (load-bearing!)
│   └── src/modules/{ble_handler,uart_handler}.c # dev_idx 0→1 patches
├── tools/
│   ├── capture.html                  # Web BLE operator page + M7 POST-WALK popup + 3-tier log persistence
│   ├── control.py                    # USB-uart1 command CLI with run-matrix presets (bench fallback)
│   ├── pull_sessions.py              # USB-uart1 LIST/DUMP session-file puller (noise-tolerant)
│   ├── cleanup_device.py             # Wipe /lfs/sessions before a fresh capture
│   ├── ingest_capture.py             # Join .dat headers + POST-WALK notes JSON → spreadsheet CSV
│   ├── read_session.py               # Parse .dat file, validate against v1 vocabulary
│   └── log_console.py                # Continuous uart0 logger → logs/uart0_YYYY-MM-DD.log
├── algo/                             # ** M9 Python distance estimator + eval harness **
│   ├── data_loader.py                # Parse raw_sessions/<date>/ into per-run objects
│   ├── evaluator.py                  # LOO harness + metrics + robustness gates
│   ├── filters.py                    # Streaming DF-II-T biquads (mirrors CMSIS-DSP layout)
│   ├── motion_gate.py                # Running-σ motion gate with Schmitt hysteresis
│   ├── step_detector.py              # Schmitt peak FSM + per-peak feature extraction
│   ├── stride_model.py               # Per-run aggregate regression (peaks → distance)
│   ├── roughness.py                  # Surface-roughness metrics (motion-gated IPR + HF/LF PSD ratio)
│   ├── distance_estimator.py         # Orchestrator implementing Predictor interface (V1 defaults)
│   ├── energy_estimator.py           # Path B alternative (integrated energy; lost A/B, kept as diag)
│   ├── characterization.py           # Phase 2 plots + PSD + noise-floor numbers
│   ├── diagnose_steps.py             # Strict-vs-loose detector comparison + autocorrelation
│   ├── run_phase3.py                 # Single-surface single-config LOO report
│   ├── run_path_comparison.py        # 5-way A/B: peaks (strict+loose) vs energy (E, E+T, T)
│   ├── run_cross_surface.py          # Per-surface vs combined vs no-info (Path A/B/C/D)
│   ├── compare_outdoor_vs_indoor.py  # Phase-2 style cross-surface signal characterization
│   ├── run_roughness_experiment.py   # Roughness-modulated combined regression (Path E)
│   ├── run_auto_surface.py           # ★ Auto-surface classifier — winning v1 architecture
│   ├── run_summary.py                # Per-run consolidated summary table for anomaly scanning
│   ├── export_c_header.py            # ★ Emit src/algo/gosteady_algo_params.h from locked V1 (M10 prereq #1)
│   ├── export_reference_vectors.py   # ★ Emit src/algo/test_vectors/*.{bin,json} regression fixtures (M10 prereq #2)
│   ├── figures/                      # Generated plots (gitignored)
│   ├── venv/                         # Self-contained Python 3.14 env (gitignored)
│   ├── requirements.txt              # Loose pins
│   ├── requirements.lock.txt         # Frozen versions for reproducibility
│   └── README.md
├── logs/                             # uart0 console logs (gitignored). Layout:
│   └── uart0_YYYY-MM-DD.log          # Daily-rotated, timestamped, with OPENED/CLOSED markers
├── raw_sessions/                     # Per-capture-date archive (gitignored). Layout:
│   └── YYYY-MM-DD/                   #   <uuid>.dat × N, notes JSON, capture_<date>.csv
├── data collection and protocols/    # v1 capture protocol + annotation spreadsheet
├── nordic resources/                 # Redistributable Nordic bundle (gitignored)
├── .claude/agents/embedded-systems.md
├── README.md                         # (removed; GOSTEADY_CONTEXT.md is canonical)
├── GOSTEADY_CONTEXT.md               # This document
└── .gitignore
```

### Build + flash targets

Two separate builds now:

| Target | Board qualifier | Build dir | Flash with SW2 at |
|---|---|---|---|
| nRF9151 app (`src/`) | `thingy91x/nrf9151/ns` | `build/` | **nRF91** |
| nRF5340 bridge (`bridge_fw/`) | `thingy91x/nrf5340/cpuapp` | `build_bridge/` | **nRF53** |

The app rebuild-and-reflash cycle is the frequent path; the bridge only gets reflashed when you're changing the BLE tunnel itself. **Always flip SW2 back to nRF91 after a bridge reflash** or subsequent app flashes will overwrite the wrong chip.

### 15-Step Firmware Arc (from README.md)

1. **Dev environment setup** — blinky on the board. *(done 2026-04-19)*
2. **Sensor bring-up** — BMI270 / ADXL367 reads over SPI / I²C. *(done 2026-04-19)*
3. **External flash** — LittleFS on the 32 MB SPI NOR. *(done 2026-04-19)*
4. **Session logging** — binary session files on flash with versioned header. *(done 2026-04-19)*
5. **USB dump** — mass storage / CDC-ACM path for host-side extraction. *(done 2026-04-19)*
6. **BLE control** — start/stop session commands over GATT (NUS). *(done 2026-04-21; M6a = parser, M6b = bridge fork + Web BLE page. Further hardened 2026-04-22 with `sendCommand` chunking to ≤180 B to survive NUS → uart1 reassembly limits.)*
7. **Python CLI** — host tool for session control and data dump. *(done 2026-04-23 — superseded by the M3 popup + `tools/ingest_capture.py` + `tools/cleanup_device.py` combination. `control.py` remains as the bench-open-cap fallback. No unified CLI needed; the operator workflow is fully browser-driven now.)*
8. **Dataset collection** — run the capture protocol end-to-end. *(paused at 19/30 — 10 indoor polished concrete + 8 outdoor concrete sidewalk + 1 outdoor stationary baseline. Carpet runs 21–30 deferred to v1.5 retrain by decision 2026-04-25; algorithm pipeline now ships with auto-surface adjustment so capturing more data is a tuning task, not a release blocker.)*
9. **Python algorithm** — distance estimator trained on the dataset. *(Phase 1–4 done 2026-04-25. Architecture: streaming filter + motion gate + Schmitt peak FSM + single-feature stride regression + **auto-surface classifier** (motion-gated inter-peak RMS roughness metric → R-thresholded coefficient lookup). Cross-surface LOO at n=16: 22.4% pooled MAPE, 15/16 walks correctly auto-classified. v1.5 retrain at n=24+ planned post-carpet captures.)*
10. **C port** — move the estimator on-device. *(done 2026-04-25 — `src/algo/{gs_filters,gs_motion_gate,gs_step_detector,gs_roughness,gs_pipeline}.h/c` + auto-generated `gosteady_algo_params.h`. Wired into `src/session.c` writer thread; emits two `ALGO_V1{A,B}` log lines at session close with distance_ft, R, surface_class, step_count, motion_s, total_s, motion_frac, overflow flag. Host regression suite (`tests/host/`, `make`-driven, no Zephyr/CMSIS dep) runs against the three M10 reference vectors and passes 31/31 checks within float32-vs-float64 tolerance. Firmware version bumped to `0.6.0-algo`. Stationary on-device validation passes (0 steps / 0 ft / NaN R, exactly mirroring `indoor_run09` fixture). Walking-path on-device validation **passes** (2026-04-25 ~18 s real walk: 12 peaks / R=0.056 / surface=indoor / distance=4.99 ft / motion_frac=43%; cadence 0.66 Hz matches characterized 0.4–0.7 Hz gait band; firmware_version=`0.6.0-algo` stamped in the pulled session header).)*
10.5. **Field deployment requirements** — define the firmware capability set + cloud contract for the first remote deployment (3 Thingy:91 X units, rehab clinic, ~1 month, cellular only, no charging, no OTA, no user interaction). Drives M10.7 + M11–M14.5 sequencing. *(spec done 2026-04-25; M14-prep Phase 1a–5 power architecture work done 2026-04-27 — see "Field Deployment (M10.5)" section + "Portal Scope Impact" section + Current State entries above.)*
10.7. **Initial production telemetry** — deployment-readiness work pulled forward from former M14 because it's needed before site-survey ship. Sequenced parallel/interleaved with M12. *(NEW 2026-04-27.)*
   - **10.7.1 Storage repartition** — `pm_static.yml` carves the unused 19 MB of external flash into snippet partition (~16 MB), telemetry queue (~256 KB raw), crash forensics region (~64 KB raw). Foundation for 10.7.3 + 12.1f. ~3 hr.
   - **10.7.2 nPM1300 fuel gauge wiring** — real `battery_pct` + `battery_mv`. Hard prereq for 12.1c.2 since `battery_pct` is a required heartbeat field per portal contract. ~1 day with `nrf_fuel_gauge` lib.
   - **10.7.3 Crash forensics + watchdog** — reset reason, fault counters, last-N log lines, watchdog hit counter persisted in 10.7.1's crash forensics region. Hardware watchdog kicked from a dedicated supervisor thread; fault triggers next-heartbeat surfacing. **No-OTA constraint means this must work first time** — bench-stress (forced HardFaults / forced WDT hits / fault-during-FS-write) before M14.5. ~2 days.
11. **Validation** — split into algo-side and deployment-side.
   - **11.1 Algo-side validation** — *passing*. M10 host regression suite (31/31 vs reference vectors) and on-device walking-path validation (2026-04-25, 12 peaks / 4.99 ft over ~18 s) cover the algorithm in isolation. Will formally redo a walking-path validation with the walker once the deployment-grade firmware is fully feature-complete (during/after M14.5) so the algo is verified against the same firmware build that's about to ship.
   - **11.2 Deployment-side validation** — continuous in-field comparison vs any clinic-supplied ground truth, ~1 month. Outcome of M15.
12. **Cellular** — LTE-M / NB-IoT link up on nRF9151. Scoped by M10.5: MQTT/TLS to AWS IoT Core, hourly heartbeat, session-end activity uplink, opportunistic snippet flush.
   - **12.1a Modem attach + signal stats + UTC** — `AT+CESQ`, `AT%XSNRSQ?`, `AT+CCLK?`. *(done 2026-04-26 — registered as roaming in 6 s on first try.)*
   - **12.1b** — *dropped 2026-04-27* (folded into 12.1c.2; cadence + PSM enter/exit logging are part of the production-shaped heartbeat, not a separate deliverable).
   - **12.1c.1 Bench-cert minimum-viable heartbeat — DONE 2026-04-27.** Bench unit `GS9999999999` boot-to-PUBACK in 17 s; LTE-M attach 8 s, AWS IoT TLS+MQTT CONNECT in 5 s, publish to `gs/GS9999999999/heartbeat`, broker PUBACK in 342 ms, Shadow.reported updated with all 5 required fields (verified `aws iot-data get-thing-shadow`). **§F2.3 spec drift resolved in code:** Phase 1B revision Lambda was deployed by cloud team at 23:38:54 UTC; our publish at 03:22:45 UTC was the first real-firmware heartbeat through the new Shadow-write path. Two firmware-side bugs found + fixed during validation: (a) QoS 0 + fixed-delay drain raced NB-IoT latency (now QoS 1 + PUBACK wait); (b) `CONFIG_AWS_IOT_AUTO_DEVICE_SHADOW_REQUEST=y` (NCS default) silently violated per-thing IoT policy on connect (now disabled in `prj_cloud.conf` + `prj_field.conf`; M12.1e.2 will need policy update for explicit shadow get). Code in `src/cloud.{h,c}` + `tools/flash_cert.py` + cert-bundle path; commits 67c3051 → e36a14e. See coord §F3 for milestone-complete entry.
   - **12.1c.2 Production-shaped heartbeat** — hourly cadence (`k_timer` + work item) + all locked optional extras (`last_cmd_id`, `reset_reason`, `fault_counters`, `watchdog_hits`, `uptime_s`, `firmware`, `battery_mv`) + real `battery_pct` from 10.7.2 + populated `reset_reason` / `fault_counters` / `watchdog_hits` from 10.7.3. NEW 2026-04-27.
   - **12.1d Activity uplink on session close — DONE 2026-04-27.** Bench unit GS9999999999, 30 s walk session (15 steps, 11.05 ft, R=0.1587, surface=indoor) hit broker with PUBACK in 655 ms; activity-processor Lambda resolved device → patient (`pt_test_001`) and wrote Activity Series DDB row with all M9 algo outputs intact. session.c calls new `gosteady_cloud_publish_activity()` from cloud.c at end of `session_stop()` (gated on `IS_ENABLED(CONFIG_GOSTEADY_CLOUD_ENABLE)`); cloud.c has a persistent activity worker thread blocked on a 4-deep msgq, sharing a mutex with the M12.1c.1 heartbeat one-shot to serialize aws_iot lib access. Required + optional schema fields per coord §C.7 / §F.3. Bumped firmware version 0.6.0-algo → 0.7.0-cloud. Code in commit cb94d17.
   - **12.1e.1 NCS Shadow lib bench confirmation** — *micro-milestone, ~half day*. Verifies §C.5.1: `aws_iot_shadow_get` + `aws_iot_shadow_update` work in NCS 3.2.4 against a stub Thing in dev IoT account. If yes → 12.1e.2 uses Shadow per §C.4.4. If no → fallback is MQTT-retained `activate` cmd (cloud-side option still on the table per §C.4.4 fallback paragraph). NEW 2026-04-27.
   - **12.1e.2 Pre-activation gate + Shadow re-check** — refuses session capture until first activation cmd received; blue LED slow-blink while in `ready_to_provision`; Shadow `desired.activated_at` re-check on every cellular wake (per §C.4.4); persists `activated_at` to flash; echoes `cmd_id` on next heartbeat as ack. Depends on 12.1e.1 outcome AND on **cloud-side Phase 1B revision deploy** (heartbeat handler writing `reported.{...}` to Shadow + threshold-detector consuming shadow-delta + activation-ack consumer) per coord §C4.1/§C4.2. Cloud team commits to landing before firmware reaches this milestone. ~2 days firmware work.
   - **12.1f Snippet uplink** — JSON-header framing per §F.3 + binary layout per §F.4 (4-byte BE length-prefix + JSON header + 16-byte payload header + 28-byte sample records). Opportunistic upload piggybacking on Priority-1 cellular wakes per M10.5 snippet upload policy. Depends on 10.7.1. ~3 days.
13. **Cloud backend** — session telemetry upload. (Coordinated with portal Phase 1A/1B — largely cloud-side; firmware-side responsibilities live in M12.)
14. **Final production telemetry** — *renamed from "Production telemetry" 2026-04-27*. With nPM1300 + crash forensics moved to M10.7 (because they're prereqs for the production-shaped heartbeat / no-OTA-safety-net respectively, both required before site-survey ship), M14 becomes the future-work bucket: OTA hooks (AWS IoT Jobs + S3 + MCUboot signing), unit-4+ telemetry hardening, post-first-deployment iterations.
14.5. **Site-survey unit shakedown** — flash `GS0000000001` with the deployment build, leave on the bench desk for ~1 week, observe heartbeat stream + Shadow + crash forensics + battery curve. Dress rehearsal between feature-complete and clinic ship. **Exit:** ≥7 days clean stream, zero unhandled resets, battery curve within ~20% of projection, M11.1 confirmation walk against this firmware build passes. NEW 2026-04-27.
15. **Field testing** — clinic deployment of `GS0000000001/2/3` (or 0002/3 if 0001 stays as the site-survey unit). M11.2 measures the outcome.

---

## Algorithm: Distance Estimation

### V1 Architecture — Auto-Surface Roughness Adjustment (locked 2026-04-25)

The v1 algorithm lives in `algo/` and is a rebuild from scratch of the step-based family the prior work validated, plus an **auto-surface adjustment block** that lets the firmware self-calibrate to whatever surface the walker is on without operator input. The operator never picks a surface — the algorithm computes a roughness metric from the IMU stream and selects coefficients automatically.

**Pipeline (all blocks streaming, O(1) state per block, ~200 B total on-device footprint):**

```
IMU (100 Hz)  →  |a| (m/s²)  ÷ g  →  |a|_g  →  HP 0.2 Hz  →  |a|_HP
                                                                 │
                                  ┌──────────────────────────────┤
                                  │                              │
                                  ▼                              ▼
                            LP 5 Hz                       500 ms running σ
                                  │                              │
                                  ▼                              ▼
                       Schmitt peak FSM                   motion gate (Schmitt hysteresis)
                                  │                              │
                                  ▼                              │
                        per-peak features                        │
                       {amp, dur, energy}                        │
                                  │                              │
                                  └─────────────┬────────────────┘
                                                ▼
                            inter-peak RMS over motion-gated samples
                                                │
                                                ▼
                                      session roughness R
                                                │
                                                ▼
                                  R-thresholded coefficient lookup
                                                │
                                                ▼
              stride_ft = a_surface + b_surface · peak_amp_g     (per peak)
                                                │
                                                ▼
                                 distance_ft = Σ stride_ft        (per session)

                            (motion gate also emits motion_duration_s → active_minutes)
```

**V1 frozen coefficients (see `algo/distance_estimator.py` + `algo/run_auto_surface.py`):**

| block | param | value | rationale |
|---|---|---|---|
| HP filter | Butterworth, order 2, cutoff | 0.2 Hz | Well below 0.37 Hz min observed gait cadence; ≥60 dB DC attenuation |
| LP filter | Butterworth, order 2, cutoff | 5 Hz | Preserves ~100 ms impulse width; kills high-freq sensor noise |
| Peak detector | Schmitt enter / exit / min-gap | 0.02 g / 0.005 g / 0.5 s | "Loose" thresholds capture the compound 2:1 impulse structure consistently across surfaces; passes the 1 ft stationary robustness gate |
| Motion gate | window / enter σ / exit σ / exit hold | 500 ms / 0.01 g / 0.005 g / 2 s | Enter threshold is 11× stationary σ; hysteresis prevents chatter at boundaries |
| Roughness metric | inter-peak RMS, motion-gated | over `(motion_mask & ~peak_window_±200ms)` of `|a|_HP_LP` | Captures wheel-on-surface texture energy *between* gait impulses; motion-gating excludes settling tails that confound short walks |
| Surface classifier | hard threshold on R | `τ = 0.245 g` | Median midpoint between indoor and outdoor R distributions; 15/16 walks correctly classified on the n=16 calibration set |
| Stride coefficients | indoor (R < τ) | `stride_ft = +0.217 + 2.757 · amp_g` | Fit on 8 indoor polished concrete walks; LOO MAPE 16.4% on this surface |
| Stride coefficients | outdoor (R ≥ τ) | `stride_ft = −0.024 + 1.705 · amp_g` | Fit on 8 outdoor concrete sidewalk walks; LOO MAPE 23.3% on this surface |
| Stride coefficients | low-pile carpet | TBD — runs 21–30 not yet captured | v1 fallback: nearest-R indoor coefficients until carpet data lands |

**Performance (LOO across 16 walks, 2026-04-23 indoor + 2026-04-25 outdoor):**

- **Pooled distance MAPE: 22.4%**, indoor 17.2% / outdoor 27.6%
- 15/16 walks correctly auto-classified by R (one outdoor boundary case at R=0.219 misclassifies as indoor → +63.9% prediction error, dominates the outdoor-MAPE penalty)
- Per-surface operator-supplied baseline (for reference): 16.4% / 23.3% / pooled ~19.9%. Auto-surface costs ~2.5 percentage points pooled vs operator-supplied surface — accepted trade for the single-button UX.

**Why this is the right architecture (not a hardcoded surface table):**

Real-world deployment surfaces are a continuum (hardwood, tile, vinyl, low-pile carpet, high-pile carpet, polished concrete, outdoor concrete, asphalt, …). A hardcoded `surface_id → coefficients` lookup requires the operator to pick the correct enum on every session — viable for a 30-run controlled capture, terrible for daily home use. R is a scalar derived from the IMU stream; the firmware self-calibrates. The classifier table grows as we collect data on new surfaces, but the *runtime* doesn't require operator input.

The v1.5 transition path is data-driven: capture more sessions across more surfaces (carpet first, then field telemetry), retrain coefficients per R-bin, refine the classifier (hard threshold → soft blend → multi-bin lookup as N grows). The architecture is unchanged across the v1 → v1.5 → v2 progression — only coefficients and bin counts grow.

### Key Phase 2–4 findings (verified against data)

**Phase 2 — single-surface characterization (indoor polished concrete, n=10):**

- **Stationary noise floor is exceptional.** σ of |a|_HP during run 9 is **0.0009 g** (1 mg). Walking σ is 0.307 g (median) — a **350× SNR** separation.
- **Dominant walk energy sits 0.3–3 Hz**, walking PSDs are 10⁻³–10⁻² g²/Hz, stationary PSD is 10⁻⁸ g²/Hz — **5–6 orders of magnitude** separation.
- **The gait is rhythmic, not smooth glide.** Autocorrelation of |a|_HP_LP shows clean side-lobes at 1.5–2 s lag matching 0.4–0.7 Hz manual cadence.
- **Compound 2:1 impulse structure per operator-step.** Loose detector finds ≈2× the manual step count — one impulse as wheels roll forward, one as rear legs plant. The regression adapts to whatever density is consistent.
- **Cadence is tightly bounded (0.37–0.65 Hz), speed comes from stride length.** "Fast" walks don't have higher cadence — they have longer strides with bigger peak-amplitude impulses. Validates the peak-amp → stride regression.

**Phase 3 — algorithm A/B on indoor data:**

- **Energy-based distance is the wrong primitive.** E ∝ speed² while distance ∝ speed, so "energy per foot" varies 10× across the speed range on identical-distance walks. Energy regression alone hit 44% MAPE; E+T made it 28%; neither beat peaks. The glide cap's impulse amplitude *scales with speed*, so step-based primitives naturally absorb the speed dependency.
- **Multi-feature regression (amp + duration + energy) overfit at n=8.** Cond(X) = 318, energy coefficient came out at -21.2 (nonsensical sign). Single-feature won.

**Phase 4 — cross-surface validation (added outdoor concrete, n=8 walks):**

- **Three universal observations** (algorithm-relevant invariants):
    1. **Stationary noise floor is identical between surfaces.** Outdoor σ = 0.82 mg vs indoor σ = 0.84 mg — **1.0× ratio**. PSDs overlap completely. The motion gate's 0.01 g threshold has the same 12× headroom on both surfaces; no threshold tuning needed.
    2. **Stride lengths are universal.** Indoor median 1.43 ft/step (range 0.95–1.82), outdoor median 1.39 ft/step (range 1.00–2.00). Same operator gait dynamics.
    3. **Detector behaves identically across surfaces.** Detected/manual ratio: indoor median 1.50, outdoor median 1.52. The compound 2:1 impulse signature is universal — the Schmitt thresholds are surface-agnostic.
- **One critical surface-specific effect:** walking σ |a|_HP nearly doubles outdoor (median 0.80 g vs indoor 0.41 g — **1.93× higher**). The outdoor surface adds texture energy on top of every gait impulse. **The slow-vs-fast σ relationship inverts**: indoor slow has the smallest σ (0.10 g), outdoor slow has the *largest* (1.04 g) — slower wheel motion means more dwell time per surface irregularity. Indoor coefficients applied to outdoor data without surface info hit 90.3% MAPE. Surface info is non-optional.
- **Settling-period confound matters.** First-pass roughness metrics ran across the whole session and were dragged down by start/stop quiet periods, especially on short walks. **Motion-gating the metric** (compute R only over `motion_mask == True` samples, excluding peak windows) tightened the indoor-vs-outdoor distribution boundaries from substantial overlap to one boundary case. The same motion gate that drives `active_minutes` also gates the roughness metric.
- **Auto-surface classifier verified.** Hard threshold on motion-gated IPR at τ=0.245 → 15/16 walks correctly classified. Soft sigmoid blends underperformed at this n. R is genuinely a useful signal, not a desperate workaround.

### Why Step-Based, Not Dead Reckoning?

Dead reckoning (double-integrating accelerometer) was tested in the prior prototype and gives 73% MAPE — unusable. Even 0.001g of bias integrates to feet of phantom displacement over a 30 s session. This is a fundamental limitation of low-cost MEMS IMUs and not a tuning problem.

### Walker-Type Architecture (Production Goal, deferred to v2)

For v1, walker_type is fixed to `two_wheel` (with `cap_type` = `glide`). Production firmware will eventually support standard walkers (no wheels — lift/shift/plant gait) and 2-wheel walkers (glide/shuffle gait). The two motion models likely need different detector tunings (and possibly different stride coefficients). Adding a walker-type classifier is a v2 concern; the auto-surface architecture sets the precedent (IMU-derived classification, no operator selection at runtime) so the v2 walker-type extension follows the same pattern.

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

These are hard blockers — capturing data the ingest pipeline can't handle is more expensive than waiting.

- **Session logging to external flash** working end-to-end. ✅ *M4 — `/lfs/sessions/<uuid>.dat` with full versioned header.*
- **Remote `start_session` / `stop_session` with full 12-field PRE-WALK payload** deliverable while the cap is sealed. ✅ *M6a + M6b — `tools/capture.html` (Chrome on Mac) over BLE NUS.*
- **Session-file pull path** from host after capture. ✅ *M5 — `tools/pull_sessions.py` over USB uart1.*
- **Ingest validator + spreadsheet row producer** that reads each session file's header and cross-checks against the v1 vocabulary. ✅ *`tools/read_session.py` validates (now with `t_ms_monotonic` flag + derived_rate_hz). `tools/ingest_capture.py` joins .dat headers + POST-WALK notes JSON and emits a CSV whose columns are ready to append to the `Captures` sheet.*
- **POST-WALK notes capture** for every run, keyed by session_uuid. ✅ *M7 — capture.html post-STOP popup with Good/Discard/Notes fast-paths + localStorage persistence + JSON export.*
- **Full end-to-end rehearsal** (BLE start → sealed-cap walk → stop → M3 popup → USB pull → ingest CSV) covering multiple preset rows. ✅ *2026-04-23 — 10-run shakedown across Indoor Polished Concrete (runs 1-10) with 10/10 pass on monotonicity, rate, flash_errors, UUID match, and POST-WALK join.*

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

### Firmware Implications (status as of M1–M7)

The schema forces several concrete firmware requirements. Status against what's implemented today:

- ✅ **UUIDv4 generation** at session start for `session_uuid` — done via DTS-chosen entropy (PSA RNG via TF-M). Echoed back to the host on `OK started <uuid>` so POST-WALK notes can key against it.
- ✅ **Session file header format** serializes all 13 FIRMWARE + 12 PRE-WALK fields. Versioned (`GOSTEADY_SESSION_VERSION=1`, 256-byte packed struct).
- ✅ **BLE `START` accepts + validates the full 12-field PRE-WALK payload** — Zephyr JSON library against `prewalk_descr` schema; all 7 controlled-vocab fields string-validated against the v1 vocabulary before firmware stamps anything. Invalid → `ERR <reason>`, no silent defaults.
- ✅ **Flash integrity counter** — bumped on `k_msgq_put` failure (queue full = sample dropped). Reported as `flash_errors` in the header.
- ✅ **Cross-session sample isolation** — writer thread local_batch reset via synchronous start-signal handshake in `session_start()` (fixed 2026-04-22). First sample of every session has `t_ms ≈ 0`; no stragglers from previous session's close window leak into the next file.
- ✅ **POST-WALK layer capture** — 7 operator-judgment fields captured in the browser at STOP time (M7). Not in the session file header; keyed by `session_uuid` in a separate JSON sidecar. Join happens at ingest via `tools/ingest_capture.py`.
- ⚠️ **RTC with UTC time** for `session_start_utc` / `session_end_utc`. Currently stamps `0` as a "not yet synced" sentinel. Waiting on M12 (cellular LTE time sync) or an alternative host-side stamp at ingest.
- ⚠️ **Battery voltage** in millivolts, measured at both session start and end. Currently stamps `0`. Needs nPM1300 fuel-gauge wiring (separate side quest; ~half a day of work with `nrf_fuel_gauge` lib).

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

## Prior-prototype algorithm reference

A Python codebase from the prior standard-walker / tacky-cap prototype lives at `GoSteady/null_old_fw/` (parent folder, not in this repo). Pipeline was the same family as our V1 — Butterworth gravity removal → |a| magnitude → peak detection → single-feature (peak amp) stride regression → distance. Achieved **12.4% MAPE on 9 calibration runs** with `stride_ft = 1.257 · peak_accel_g − 1.052` and prominence 0.15 g / min-distance 70 samples. This is the *proof of concept* that the step-based family works for the product. The `algo/` directory in this repo is the streaming-production-shaped rebuild against current hardware / walker type / cap type — see the Algorithm: Distance Estimation section above for what's actually deployed.

The prior `.dat` format (tab-separated, 7 columns, 99.3 Hz) is *not* what our firmware produces — our session files are 256 B packed header + 28 B packed binary records @ 100 Hz. No cross-compatibility; `tools/read_session.py` + `algo/data_loader.py` are the canonical parsers.

---

## Field Deployment (M10.5) — First Clinic Deployment Requirements

This section defines what the firmware must do to ship the first remote deployment. Decisions captured here drive the M11–M14 work plan; nothing in M10.5 is itself implementation, but every M10.5 line item has a downstream firmware milestone or a portal-coordination dependency in the Portal Scope Impact section.

### Deployment profile

| Item | Value | Source |
|---|---|---|
| Units | 3 | Decided 2026-04-25 |
| Site | Single rehab clinic | Decided 2026-04-25 |
| Duration | ~1 month | Decided 2026-04-25 |
| Hardware | Thingy:91 X (no custom PCB) | Decided 2026-04-25 |
| Connectivity | Cellular only (LTE-M primary, NB-IoT fallback per portal spec) | Portal ARCHITECTURE.md §1 |
| Charging | None — battery must last the deployment on a single charge (1300 mAh) | Decided 2026-04-25 |
| OTA | None — firmware is fixed for the deployment duration | Decided 2026-04-25 (matches portal Phase 5A deferral of OTA pipeline) |
| User interaction | None — clinic staff observe via portal only | Decided 2026-04-25 |
| Mounting | Custom housing, designed separately | Decided 2026-04-25 |

### Success criteria (all three)

1. **Reliability** — devices stay alive and uplinking for the full deployment window with no physical intervention.
2. **Algorithm validation** — distance estimates uplinked per session are evaluable against any clinic-supplied ground truth (PT session logs, observed walks).
3. **v1.5 retrain corpus** — raw IMU snippets retrieved on device return seed coefficient retraining for the next algorithm version.

### Hard requirements (must ship in deployment build)

- **Auto-start session capture on motion.** ADXL367 wake-on-motion via INT1 line wakes the nRF9151 from deep sleep; nRF9151 confirms sustained motion via short BMI270 sample window before opening a session. No SW0 button, no BLE START path enabled.
- **Auto-stop session on stationarity.** Hysteresis-based stop: N seconds of below-motion-gate σ ends the session. Tunable threshold; default 30 s.
- **Sleep-when-idle as the default state.** Between sessions: nRF9151 in deep sleep (System OFF or System ON-Idle depending on what the LTE-M context allows), BMI270 powered down (not just idle), ADXL367 in low-power motion-detect mode, modem in PSM. Wake sources: ADXL367 INT1 (motion), modem (heartbeat alarm), watchdog.
- **Hourly heartbeat uplink.** Every 60 min the modem wakes from PSM, publishes a heartbeat to `gs/{serial}/heartbeat`, and returns to PSM. Contract per portal spec — see Portal Scope Impact.
- **Per-session activity uplink.** On session close, after on-device M10 algorithm finishes computing distance/R/active_minutes/step_count, publish to `gs/{serial}/activity`. Idempotent on `(serial, session_end)`.
- **Pre-activation gate.** A device that has not yet received its first cloud activation message refuses to capture sessions. Behavior: wake on motion → connect → publish heartbeat → wait for activation → if not activated, return to sleep without opening any session. Visual indicator: blue LED slow-blink (1 Hz, 100 ms on / 900 ms off) while in `ready_to_provision` state. LED off in normal operation (battery + clinic-discretion). See Portal Scope Impact for the activation message contract.
- **Snippet capture — session-aligned by construction.** Snippets are guaranteed-motion slices because they are always portions of real sessions; there is no standalone snippet-capture path with its own motion gate. Two trigger types:
    - **Scheduled snippet** — every 6 hr a schedule timer fires and sets a `snippet_armed_scheduled` flag. The *next* session that opens (whenever motion next causes one) captures its first 30 s (or its entirety, if shorter) as a retained snippet. Flag clears on capture; timer resets on capture, NOT on tick. If the walker is unused for days, the schedule simply waits — no stationary garbage gets recorded.
    - **Anomaly snippet** — post-hoc decision at session close. If the just-finished session triggers an anomaly condition (R out-of-bin vs. running history, session σ outlier, peak |a| > 4 g), promote its raw data from the session partition (which rotates) to the snippet partition (which doesn't).
    Hard cap: 8 snippets/day total across both trigger types. Each snippet ≤ 30 s of raw 100 Hz BMI270 (~84 KB max; typically smaller given short clinic walks). Implementation footprint is two flags (`snippet_armed_scheduled`, `anomaly_this_session`) plus a "promote to snippet partition" copy at session close — no new state machine.
- **Snippet upload — opportunistic + Priority-2.** See Snippet Upload Policy below. Goal: get snippets to cloud when battery permits; otherwise retain on flash for USB retrieval on device return.
- **Persistent crash forensics.** Reset-reason register, fault counter, last N log lines (or fault frame), watchdog hit counter — all persisted across reset in a dedicated flash region (separate from LittleFS so a corrupted FS doesn't take the postmortem with it). Included as fields in the next heartbeat after reset; full context retrievable via USB on device return.
- **Watchdog.** Hardware watchdog kicked from a dedicated supervisor thread; if any pinned thread (sampler, writer, modem, supervisor) misses its beacon, the watchdog fires and the reset is captured by the forensics path above.
- **Cellular network time sync.** UTC obtained via `AT+CCLK?` on each modem attach; used to stamp `session_start`, `session_end`, and heartbeat `ts`. Drift expected to be <1 s over a heartbeat interval.
- **Manually-flashed per-device cert.** No fleet provisioning for first 3 units (per portal coordination). Per-device cert + private key flashed at build time, stored in nRF9151 CryptoCell-312 / TF-M secure element. Each unit gets a distinct serial `GS00000000XX` baked at flash time.
- **Battery measurement via nPM1300.** Fuel gauge reads `battery_mv` and `battery_pct` for inclusion in heartbeat payload. Currently stamps `0` — must be wired before deployment.

### Power budget (preliminary — to be measured, not a constraint at this stage per 2026-04-25 decision)

Rough back-of-envelope to size expectations:

| Component | Avg current | Notes |
|---|---|---|
| Heartbeat uplinks @ 1/hr | ~0.33 mA | 24/day × ~50 mC/uplink LTE-M |
| Activity uplinks (sessions) | ~0.05 mA | ~5/day × ~50 mC/uplink |
| Snippet uplinks (Priority-2) | ~0.05–0.2 mA | piggyback on cellular wakes; 1/wake max |
| Session capture (BMI270 + nRF9151 active) | ~2.5 mA | ~2 hr/day @ 30 mA active |
| Sleep + ADXL367 motion-detect background | ~0.05 mA | nRF9151 deep sleep + ADXL367 low-power |
| **Total avg** | **~3 mA** | → ~18 days at 1300 mAh |

The hourly-heartbeat × 1300 mAh × 1-month combination is acknowledged as tight (~18 days projected). Decision was to ship with hourly cadence and **measure actual battery life as a deployment outcome**, not pre-tune. If first-deployment battery falls short, options: relax heartbeat cadence (with portal-side offline-detector adjustment), upgrade battery, or both.

### Snippet upload policy

This is firmware-internal policy (no portal contract for snippets in v1).

- **Trigger to upload:** every cellular wake that opens for a Priority-1 publish (heartbeat or activity) is a candidate snippet-flush window.
- **Order of operations within a wake:** modem attach → publish all queued Priority-1 → wait for ACK → if `battery_pct ≥ 30%` AND queued snippets exist, publish 1 snippet → wait for ACK → detach. Hard cap: 1 snippet per cellular wake regardless of backlog.
- **Battery floor:** `battery_pct < 30%` skips snippet upload entirely. Heartbeats continue at full cadence — they are non-negotiable for the offline-detection contract.
- **Order across snippets:** FIFO oldest-first.
- **Post-upload retention:** uploaded snippets are marked `uploaded = true` but **not deleted** until storage pressure forces rotation. USB retrieval on device return still pulls every snippet that was ever captured, even those already in cloud — belt-and-suspenders.
- **Storage-full behavior:** when the snippet partition crosses 90% full, *promotion-to-snippet* declines for both trigger types (do NOT overwrite unsent snippets — they're the v1.5 retrain corpus). Sessions still record + summarize normally; only the snippet copy is skipped. Capture resumes when uploads or USB retrieval frees slots, OR when the stale-cutoff (14 days) lets us overwrite older uploaded snippets first.
- **8/day cap applies to both trigger types pooled.** Once today's count reaches 8, further triggers (scheduled or anomaly) decline-with-log until midnight UTC reset.

### Offline buffering policy (firmware-internal; cloud handles dedup via portal contract)

- **Priority 1 (heartbeat + activity):** queue in RAM if cellular session in progress; if modem unavailable, persist to a small dedicated buffer in flash (NOT LittleFS — separate region so FS pressure doesn't lose telemetry). On next cellular wake, drain Priority-1 queue first, oldest-first.
- **Priority 2 (snippets):** stored in LittleFS snippet partition as the buffer. No separate queue.
- **Cloud idempotency** (per portal spec) means firmware can retry freely without duplicate-row risk: heartbeats dedup by `(serial, ts)`-class, activity dedups by `(serial, session_end)`, alerts dedup by `(patientId, ts#alertType)`. Firmware retry strategy: exponential backoff on failure, no upper retry limit, persist queue across reset.

### Storage layout (changes from current)

Current LittleFS partition is 8 MB at `0x4d2000–0xcd2000`; remaining ~19 MB on the GD25LE255E (32 MB) is unused. M10.5 needs to repartition:

| Partition | Size | Purpose |
|---|---|---|
| LittleFS sessions + summaries | 8 MB (unchanged) | Raw `.dat` sessions (rotated aggressively) + small summary records |
| LittleFS snippets | ~16 MB (new — carved out of the 19 MB unused) | Snippet ring with capture-pause-when-full + 14-day stale cutoff |
| Telemetry queue | ~256 KB | Persistent Priority-1 queue (separate region; not LittleFS) |
| Crash forensics | ~64 KB | Reset reason, fault counters, last log lines, watchdog hits |

Repartition is a one-time partition-manager change in `pm_static.yml`; needs `-p always` rebuild.

### Anti-features (explicitly NOT in first deployment)

- **No fall / tipover / impact detection.** The portal spec defines an alert payload schema with `alert_type ∈ {tipover, fall, impact}` (`gs/{serial}/alert`); shipping that detector is its own algorithm milestone with zero current Python prototype work. Deferred. Portal alert channel exists but firmware does not publish to it in v1.
- **No fleet provisioning / claim-cert flow.** Manual per-device cert at flash time covers 3 units. Fleet provisioning is portal Phase 5A; firmware-side integration to be addressed at scale.
- **No OTA.** MCUboot partition layout is preserved (so future OTA is non-breaking) but no AWS IoT Jobs client in the deployment build.
- **No downlink config.** All knobs are compile-time constants in the deployment build. Portal `cmd` topic intentionally not subscribed (portal spec also defers this).
- **No active LEDs in normal operation.** Pre-activation blue LED is the only LED behavior in deployment. Heartbeat tick + green/purple session LEDs are gated off via `CONFIG_GOSTEADY_FIELD_MODE`.
- **No fall back to USB / BLE control surface.** Both transports are compiled out (or runtime-disabled) in the deployment build to prevent accidental use in field. Re-enable for return-to-bench debug.
- **No real-time streaming.** All uplinks are summary-form (heartbeat, session-end activity). Snippets are bulk-uploaded post-hoc, not streamed.
- **No on-device patient identity.** Per portal contract, attribution is post-hoc cloud-side via DeviceAssignment lookup on `serial`. PRE-WALK schema fields (`subject_id`, `course_id`, etc.) that the bench operator filled via `capture.html` are stamped with deployment-mode placeholders (e.g. `subject_id = "deployed"`, `course_id = "field"`).

### Risks and open items

| Risk | Mitigation |
|---|---|
| Hourly-heartbeat × 1300 mAh × 1 month is tight (~18 day projection). | Measure in deployment; have a "site survey" unit that ships first with full instrumentation. Negotiate cadence relax with portal team if first-week data is bad. |
| Cellular coverage at clinic unknown. | Site-survey unit goes first (1-2 weeks before main 3-unit deployment) — confirms LTE-M signal + battery behavior before committing the other two units. |
| Crash forensics path is new code that must work the first time (no OTA to fix it). | Stress-test on bench: forced HardFaults, forced watchdog hits, forced fault-during-FS-write. Verify forensics survive reset and surface in the next heartbeat. |
| Snippet partition fills mid-deployment. | 14-day stale cutoff + promotion-skip-when-full prevents data loss. Realistic fill rate is well under the 84 KB max because clinic walks are typically 5-20 s (≈14-56 KB per snippet), so 16 MB partition holds ~300 typical snippets ≈ 37 days at the 8/day cap before any stale-cutoff overwrites. |
| Battery measurement (nPM1300 fuel gauge) not yet wired. | Pre-deployment milestone — must be done before site-survey unit ships. |
| Pre-activation contract is portal-side TBD. | Coordination item in Portal Scope Impact; blocking for site-survey unit ship. |
| nPM1300 / fuel-gauge accuracy unknown over month-long discharge. | Calibrate against known-good measurement on bench before deployment. |
| Custom housing thermal / mechanical impact on IMU readings unknown. | Bench-shakedown with custom housing before deployment to confirm no σ shift. |

---

## Portal Scope Impact

> **Meta context for future sessions:** GoSteady is a one-developer project (Jace) at this point. The "firmware team" and "cloud team" you'll see in the shared coordination doc are **both Jace working with Claude in two parallel sessions** — one focused on this firmware repo, one focused on `gosteady-portal`. The append-only convention isn't really about cross-team etiquette; it's because each Claude session only sees its own repo's history, so the shared doc is the only place both sessions reliably read the other's decisions. Treat coordination items as self-managed: "waiting on cloud team to deliver X" really means "switch back to the other Claude session and have it deliver X." Cert handoffs, shadow registry rows, etc. are minutes away whenever firmware needs them.

This section is the **firmware-side reference snapshot of the contract tables** between firmware and the GoSteady portal. As of 2026-04-26, the **canonical cross-team conversation record** lives in the shared portal repo, not here:

> **Shared coordination doc:** `gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md`
> Local mirror: `~/Library/Mobile Documents/com~apple~CloudDocs/Documents/GoSteady/gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md`
> GitHub: `https://github.com/Jabl1629/GoSteadyPortal/blob/feature/infra-scaffold/docs/firmware-coordination/2026-04-17-cloud-contracts.md`

That doc is **append-only conversation style**: each team writes a dated, signed entry; nobody edits another team's entries. Use it for: (a) responding to a question raised by the other team, (b) raising a new contract question, (c) announcing a new firmware milestone or cloud capability the other side needs to know about, (d) flagging a deferred decision back open. The tables below are the firmware-side cached view of locked contracts — useful for a glance while writing firmware code, but if there's a discrepancy with the shared doc, **the shared doc wins**.

Other portal-side authoritative spec sources: `~/Library/Mobile Documents/com~apple~CloudDocs/Documents/GoSteady/gosteady-portal/docs/specs/` — `ARCHITECTURE.md`, `phase-1a-ingestion.md`, `phase-1a-revision.md`, `phase-1b-processing.md`, `phase-2a-device-lifecycle.md`. Re-read those when planning anything portal-touching; they're the underlying spec the shared coordination doc is reconciling.

### Identity & provisioning (FIRST DEPLOYMENT)

| Item | Decision | Status |
|---|---|---|
| Device ID format | `GS` + 10 digits (e.g. `GS0000001234`). Used as MQTT client ID + IoT Thing Name. | Locked by portal spec |
| First-3-unit serials | **`GS0000000001`, `GS0000000002`, `GS0000000003`** | **Locked 2026-04-26 (cloud, §C.4.3)** |
| Reserved test/dev serial range | `GS9999999990–GS9999999999` (visually distinct, won't collide with low-range production) | **Locked 2026-04-26 (cloud)** |
| Auth (first 3 units) | Per-device cert + private key flashed manually at build time, stored in nRF9151 CryptoCell-312 / TF-M secure element. | **Locked 2026-04-25** — defers AWS IoT fleet provisioning for first deployment |
| Cert generation flow | **Option (a) cloud-generates-and-sends.** Cloud team runs `aws iot create-keys-and-certificate --set-as-active`, creates per-thing IoT Thing + per-thing IoT policy, hands off cert PEM + private key PEM via 1Password shared item (one per device, named by serial, 7-day expiry). Firmware writes via `AT%CMNG=0,<sec_tag>,...` at flash time. | **Locked 2026-04-26 (cloud, §C.4.1)** |
| Per-thing IoT policy actions | `iot:Connect` on own thing, `iot:Publish` on `gs/{serial}/{heartbeat,activity,snippet}`, `iot:Subscribe`+`Receive` on `gs/{serial}/cmd`, `iot:GetThingShadow`+`UpdateThingShadow` on own thing (added for §F.9.4 Shadow re-check). Nothing else. | **Locked 2026-04-26 (cloud)** |
| AWS IoT root CA pin | Amazon Root CA 1 — `https://www.amazontrust.com/repository/AmazonRootCA1.pem`. No rotation without coordination here first. | **Locked 2026-04-26 (cloud)** |
| AWS IoT MQTT endpoint (dev) | `a2dl73jkjzv6h5-ats.iot.us-east-1.amazonaws.com`, port 8883 | **Locked 2026-04-26 (cloud, §C.4.2)** |
| AWS IoT MQTT endpoint (prod) | TBD — separate AWS account per Phase 1.5 multi-account plan, separate hostname. Per-env Kconfig + separate firmware builds (e.g. `CONFIG_GOSTEADY_IOT_ENDPOINT_PROD`). | **Approach locked 2026-04-26**; prod hostname pending |
| Auth (post-first-deployment) | AWS IoT Fleet Provisioning with claim cert at flash time + CSR exchange on first boot | Deferred to portal Phase 5A coordination |
| First-boot binding | Cloud-side state machine: `ready_to_provision` → `provisioned` → `active_monitoring` driven by first-heartbeat | Portal-side; no firmware action beyond "send heartbeat normally" |
| Manufacturer-side enrollment workflow | **Two-step.** Short-term (first ≤10 units): private companion firmware repo with checked-in `device-registry.csv` (`serial, cert_fingerprint, flash_date, firmware_version`); cloud team has read access; per-shipment Slack ping triggers cloud team to bulk-import CSV → `ready_to_provision` Registry rows. Long-term (post-Phase 2A): firmware flash script calls `POST /admin/devices` directly. CSV becomes audit trail. | **Locked 2026-04-26 (cloud, §C.4.5)** — repo creation pending firmware-side |

### Pre-activation behavior

| Item | Decision | Status |
|---|---|---|
| Firmware behavior pre-activation | No session capture. Wake on motion → connect → publish heartbeat → wait for activation cmd → if absent, return to sleep. | **Locked firmware-side 2026-04-25** |
| Visual indicator | Blue LED slow-blink (1 Hz, 100 ms on / 900 ms off) while in `ready_to_provision`. Extinguishes once activation cmd received. **Purely firmware-side** — no cloud "show LED" command; firmware self-extinguishes. Driven by the firmware's local view of provisioning state, which it confirms with the cloud at every cellular wake. | **Locked 2026-04-25** |
| Activation cmd topic | `gs/{serial}/cmd` (downlink, cloud → device). Per-thing IoT policy authorizes each device to subscribe only to its own `cmd` topic. | **Locked 2026-04-17 (portal)** |
| Activation cmd payload | `{"cmd":"activate","cmd_id":"act_<uuid>","ts":"<ISO8601>","session_id":"<provision audit ID>"}` | **Locked 2026-04-17 (portal)** |
| When cloud sends it | Synchronously, when a caregiver successfully provisions the device for the first time via portal API. State machine: `ready_to_provision → provisioned`. | **Locked 2026-04-17 (portal)** |
| Firmware behavior on receipt | (1) persist `activated_at` to flash; (2) exit pre-activation sleep loop; (3) extinguish blue LED; (4) begin normal session capture; (5) echo `cmd_id` back via `last_cmd_id` field on next heartbeat. | **Locked 2026-04-17** |
| Ack semantics | Heartbeat-side `last_cmd_id` echo IS the ack. Cloud's heartbeat handler matches it against most-recent issued `activate` cmd → marks `Device Registry.activated_at` and emits `device.activated` audit event. | **Locked 2026-04-17 (portal)** |
| Pre-activation heartbeats | Cloud accepts them, updates Device Shadow normally, **suppresses synthetic alerts** until `activated_at` is set (no patient → no caregiver → all alerts are noise). Sampled audit log at 1/hr/serial. Firmware can publish freely without producing user-facing noise. | **Locked 2026-04-17 (portal)** |
| Failure modes | Cloud publish fails → portal returns 500 to caregiver; provision is idempotent so retry republishes a fresh `cmd_id`. Firmware never echoes → cloud surfaces "stuck in `provisioned` >24h" via ops alarm. | **Locked 2026-04-17 (portal)** |
| `last_cmd_id` ack matching breadth | Cloud's heartbeat handler matches `last_cmd_id` against any `cmd_id` issued to the serial **within the last 24 h**, not just the most recent — handles benign portal-retry windows where two `cmd_id`s are in flight. Firmware always echoes the most-recently-received `cmd_id`; cloud's matching is the lenient side. | **Locked 2026-04-26 (cloud, §C.2 / D14a)** |
| Re-check on every cellular wake | **Device Shadow `desired.activated_at`** (NOT MQTT retained — chosen for forward-compat with future per-device knobs like sampling rate, thresholds, OTA gating). Cloud writes `desired.activated_at` ISO 8601 timestamp at provision-time AND publishes the immediate-push `activate` cmd. Firmware on every cellular wake: `GET` shadow → read `desired.activated_at` → if non-null & matches on-flash value → normal operation; else → re-enter pre-activation, blue LED on, no session capture. Firmware writes `reported.activated_at` to confirm device-side persistence after every state change. | **Locked 2026-04-26 (cloud, §C.4.4)** — firmware-side build path subject to NCS Shadow library check (see §C.5.1, currently OPEN below) |
| Cloud-side invariant | `desired.activated_at` is non-null **iff** Device Registry status ∈ {`provisioned`, `active_monitoring`}. Every transition out of those states writes `desired.activated_at = null`. So a de-provisioned / RMA'd device firmware re-checking shadow naturally re-enters pre-activation. | **Locked 2026-04-26 (cloud)** |
| Stray `activate` cmd handling | If firmware receives an `activate` cmd it didn't expect (no preceding cmd_id in its issuance log), treat as authoritative + persist + ack normally. Cloud is the canonical issuance authority; firmware shouldn't second-guess. | **Locked 2026-04-26 (cloud)** |

### Transport & topics

| Item | Decision | Status |
|---|---|---|
| Protocol | MQTT over TLS 1.2 to AWS IoT Core (us-east-1) | Locked by portal spec |
| Heartbeat topic | `gs/{serial}/heartbeat` | Locked by portal spec |
| Activity topic | `gs/{serial}/activity` | Locked by portal spec |
| Alert topic | `gs/{serial}/alert` (firmware does NOT publish in first deployment — see Anti-features above) | Channel exists; firmware deferred |
| Snippet topic | `gs/{serial}/snippet` (uplink, binary payload up to 100 KB). | **Locked 2026-04-17 (portal)** |
| Downlink / `cmd` topic | `gs/{serial}/cmd` — used in v1 for the **activation cmd only**. Firmware subscribes during pre-activation, unsubscribes (or ignores further messages) after activation. No other downlink commands in first deployment. | **Locked 2026-04-17** |

### Heartbeat uplink

| Item | Decision | Status |
|---|---|---|
| Cadence | 1 hr | **Locked 2026-04-25** — accepting tight battery budget; measure actual life as deployment outcome |
| Cloud offline-detection threshold | > 2 hr without heartbeat → device marked offline | Locked by portal spec |
| Required payload fields | `serial`, `ts` (ISO 8601 UTC), `battery_pct` (0.0–1.0), `rsrp_dbm` (−140 to 0), `snr_db` (−20 to 40) | Locked by portal spec |
| Optional payload fields | `battery_mv`, `firmware` (semver string), `uptime_s`, `last_cmd_id` (echoes most-recent downlink cmd — used for activation ack), `reset_reason`, `fault_counters` (object), `watchdog_hits` (int) | **Locked 2026-04-17 (portal)** — all formerly "firmware-added" extras now explicitly listed in `ARCHITECTURE.md §7` |
| Cloud handling of unknown fields | **Accept-all**: any unknown field is persisted to Device Shadow `reported` state alongside named fields. Validation rejects only on missing required fields or out-of-range required values. Crash-forensics extras are queryable via Shadow. | **Locked 2026-04-17 (portal)** |
| Storage in cloud | AWS IoT Device Shadow `reported` state — Phase 1B revision Lambda deployed by cloud team at 2026-04-27T23:38:54 UTC; firmware-side validation completed M12.1c.1 (first real-firmware heartbeat at 2026-04-28T03:22:45 UTC landed at Shadow version 9 with all 5 required fields). DDB row no longer written from heartbeat path. | **Locked + verified end-to-end 2026-04-27** |

### Activity uplink

| Item | Decision | Status |
|---|---|---|
| Trigger | On session close (after on-device M10 algorithm computes outputs) | Locked firmware-side |
| Required payload fields | `serial`, `session_start` (ISO 8601), `session_end` (ISO 8601), `steps` (int 0–100,000), `distance_ft` (number 0–50,000), `active_min` (int 0–1,440) | Locked by portal spec |
| Optional payload fields | `roughness_R` (float), `surface_class` (`indoor` \| `outdoor` — M9 classifier), `firmware_version` (semver). Other unknown fields land in a per-row `extras` map on the Activity Series DDB row. | **Locked 2026-04-17 (portal)** — all three firmware-derived extras added to schema in `ARCHITECTURE.md §7` |
| Idempotency | `(serial, session_end)` — firmware retries are safe | Locked by portal spec |
| Patient attribution | Post-hoc, cloud-side, by `serial` → DeviceAssignment lookup. Firmware does NOT receive or send `patient_id`. | Locked by portal spec |

### Alerts (DEFERRED)

| Item | Decision | Status |
|---|---|---|
| Channel exists | `gs/{serial}/alert` with `alert_type ∈ {tipover, fall, impact}` and `severity ∈ {critical, warning, info}` | Locked by portal spec |
| Firmware ships event detection in first deployment | **No** — fall/tipover/impact algorithm has zero Python prototype; deferred to a later algorithm milestone | **Locked 2026-04-25** |
| Cloud-side synthetic alerts (e.g. `device_offline`, `battery_critical`) | Generated by Threshold Detector from heartbeat data — firmware does nothing | Locked by portal spec |

### Snippets / raw IMU data

| Item | Decision | Status |
|---|---|---|
| Portal spec coverage | Resolved 2026-04-17 — covered by `phase-1a-revision.md` snippet IoT Rule | **Locked 2026-04-17 (portal)** |
| First-deployment plan | MQTT direct publish, opportunistic. Flash retention as belt-and-suspenders for USB retrieval on device return. | **Locked 2026-04-17** |
| Opportunistic upload | Firmware piggybacks snippet upload on cellular wakes opened for Priority-1 publishes. See Snippet Upload Policy in M10.5 section. | **Locked 2026-04-25 firmware-side** |
| Topic | `gs/{serial}/snippet` (uplink) | **Locked 2026-04-17 (portal)** |
| Payload | Binary — raw 100 Hz BMI270 IMU samples for a 30 s window (~84 KB typical, 100 KB max). | **Locked 2026-04-17 (portal)** |
| MQTT user properties (required) | `snippet_id` (firmware-generated UUID for idempotency), `window_start_ts` (ISO 8601 UTC) | **Locked 2026-04-17 (portal)** |
| MQTT user properties (optional) | `anomaly_trigger ∈ {session_sigma, R_outlier, high_g}`. Absent for scheduled snippets. | **Locked 2026-04-17 (portal)** |
| MQTT version assumption | Cloud spec assumes MQTT 5 user properties. **Verify NCS MQTT client support** — if only 3.1.1, fall back to small JSON header inside the binary payload. | **Action item** (firmware side) |
| Cloud routing | **IoT Rule + thin Python Lambda** that parses the 4-byte length-prefix + JSON header, then writes the full payload to `s3://gosteady-{env}-snippets/{serial}/{date}/{snippet_id}.bin` (full payload = JSON header + binary body, keeps S3 object self-describing for offline analytics). AWS-managed SSE encryption. **The "no Lambda in path" claim from the original 2026-04-17 doc no longer holds** — IoT Rule SQL alone can't extract `snippet_id` from a binary preamble for the S3 key. Negligible cost (~720 invocations/month at expected v1 cadence). No firmware-side change. | **Locked 2026-04-26 (cloud, §C.2 update from §C.2 §F.3 ack)** |
| Retention | 90 days hot Standard → Glacier; 13-month total before delete (aligned with v1.5 retrain need). | **Locked 2026-04-17 (portal)** |
| Audit | Every upload generates a `device.snippet_uploaded` event. | **Locked 2026-04-17 (portal)** |
| v2 migration heads-up | If snippet size exceeds 100 KB later (longer windows, multi-sensor), switch to S3 presigned-URL flow (same pattern as OTA). MQTT topic deprecates then. **No v1 changes anticipated.** | Informational |

### OTA

| Item | Decision | Status |
|---|---|---|
| Protocol target | AWS IoT Jobs + S3 + MCUboot (signed, anti-rollback) | Locked by portal spec |
| First deployment | **Not in scope** — firmware fixed for the deployment duration | **Locked 2026-04-25** |
| Forward-compatibility | MCUboot partition layout preserved in deployment build so future OTA is non-breaking. | Already true in current build |

### Time sync

| Item | Decision | Status |
|---|---|---|
| Mechanism | Cellular network time via `AT+CCLK?` on each modem attach | **Locked firmware-side 2026-04-25** — portal confirmed device-authoritative posture 2026-04-17 |
| Cloud trust contract | Timestamps are device-authoritative. Cloud accepts ISO 8601 strings as-is, no NTP fallback or cloud-side time correction in v1. Validation rejects only unparseable ISO 8601; clock skew or seconds-level drift is accepted. | **Locked 2026-04-17 (portal)** |
| Acceptable accuracy | ~seconds for ISO 8601 timestamps in payloads (hourly heartbeats, session-end activity, 2-hour offline detection are not sub-second sensitive) | **Locked 2026-04-17** |
| Empirical bench validation | M12.1a end-to-end on 2026-04-26: LTE-M attach in 6 s on roaming, NITZ-derived UTC parsed cleanly out of `AT+CCLK?` and stamped into `cellular: network_time=2026-04-26T17:58:14Z`. Mechanism works on first try. | **Verified 2026-04-26 (firmware)** |
| Fallback | None in v1 (no NTP, no RTC battery backup). v2 revisits if a use case requires sub-second precision. | **Locked firmware-side** |

### Configuration management (no downlink in v1)

| Item | Decision | Status |
|---|---|---|
| Config delivery | Compile-time constants only. No runtime config update path. | **Locked 2026-04-25** |
| Knobs frozen at flash time | Heartbeat cadence (1 hr), snippet schedule (4/day + ≤4 anomaly), motion-gate thresholds, R classifier τ, sleep timeouts, battery floor for snippet upload (30%), all algorithm coefficients (`gosteady_algo_params.h` from M10) | Document in deployment build's `prj.conf` + `gosteady_algo_params.h` |

### Open coordination items

**Canonical record:** `gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md` (append-only conversation; search for the latest entry from each team to see current status).

**Conversation timeline (each entry shipped as a single commit on `feature/infra-scaffold` of `gosteady-portal`):**

| Date | Team | Section refs | Summary |
|---|---|---|---|
| 2026-04-17 | Cloud | §1–§9 | First batch — activation contract, heartbeat / activity extras, snippet upload schema, time sync, pre-activation handling. 4 firmware-side action items. |
| 2026-04-26 | Firmware | §F.1–§F.10 | Acknowledged 2026-04-17 batch + answered all 6 cloud action items. Announced M12.1a complete on bench. **5 new firmware-side questions** for cloud (cert flow, IoT endpoint, starting serials, re-check-on-wake mechanism, manufacturer enrollment). |
| 2026-04-26 | Cloud | §C.1–§C.6 | Acknowledged firmware response. **All 6 §F.9 questions answered with decisions** (folded into the tables above). 3 new cloud-side asks back at firmware. |
| 2026-04-27 | Cloud | §C2 + §C3 | Cert bundle minted + delivered (path-corrected to filesystem in §C3 for single-dev setup). 4 cert+key pairs ready: bench `GS9999999999` + shipping `GS0000000001/2/3`. |
| 2026-04-27 | Firmware | §F2.1–§F2.6 | M12.1c.1 sub-task 0 complete (cloud heartbeat path validated end-to-end without firmware via `aws iot-data publish`). Raised §F2.3 spec drift question (heartbeat storage = Shadow per spec vs DDB per impl). Renumbering heads-up §F2.5. |
| 2026-04-27 | Cloud | §C4.1–§C4.7 | **§F2.3 resolved as Option 2** — OLD heartbeat Lambda is pre-revision; spec describes post-1B-revision target; cloud commits to deploying 1B revision before firmware reaches M12.1e.2. §C4.3 ran threshold-detector probe (logic intact; PutItem fails on patientId-keyed alerts table — 1B-rev fix). §C4.4 covers minor §F2.4 items (uptimeS default-fill also fixed in 1B-rev). |
| 2026-04-27 | Firmware | §F3.1–§F3.5 | **M12.1c.1 milestone-complete** — first real-firmware heartbeat from `GS9999999999` lands in Shadow.reported (boot-to-PUBACK 17 s, broker ACK 342 ms). Confirms Phase 1B revision Lambda deployed and writing Shadow correctly. Surfaces 2 firmware-side bugs found+fixed during validation (§F3.2: QoS 0 race + auto-shadow-request policy violation). Heads-up for M12.1e.2 cloud-side: shadow MQTT topics will need policy update. |
| 2026-04-27 | Firmware | §F4.1–§F4.5 | **M12.1d milestone-complete** — first real-firmware activity uplink from a 30 s walk session lands in Activity Series DDB (PUBACK 655 ms). Activity-processor (1B-rev) + DeviceAssignment patient resolution + DDB write all working end-to-end with real firmware payload. Two of four uplink topics (heartbeat + activity) under concrete test. |
| 2026-04-28 | Cloud | §C6.1–§C6.5 | Shadow MQTT topic policy grants live (closes §C5.4). M12.1e.1 + M12.1e.2 cloud-side unblocked. Wildcard scoping inside per-thing policy (§C6.2) used because explicit channel enumeration exceeded the 2048 B IoT policy size cap. Cloud-side processing layer + ingestion infrastructure now feature-complete through M14.5 scope. |
| 2026-04-29 | Firmware | §F5.1–§F5.6 | **M10.7 + M12.1c.2 milestone-complete** — first production-shape heartbeat from real firmware lands in Shadow with all locked optional extras populated (battery_pct=0.936 real fuel gauge, battery_mv, firmware="0.8.0-prod", uptime_s, reset_reason, fault_counters object, watchdog_hits). Bench-validated full M10.7.3 fault-recovery axis end-to-end via two stress-test commands (CRASH / STALL). Surfaced + fixed 2 bugs in M10.7.3 fault path (§F5.3): in-handler flash persist doesn't survive sys_reboot timing on this nRF9151+TF-M platform → noinit-retention fix; k_fatal_halt was a 60 s death spiral → switched to sys_reboot(SYS_REBOOT_WARM) for ~30 s recovery + accurate reset_reason="SOFTWARE" attribution. |
| 2026-04-29 | Firmware | §F6.1–§F6.5 | **M12.1e.1 PASS — closes §C.5.1.** NCS 3.2.4's `aws_iot` lib supports the Device Shadow surface end-to-end on this firmware build. Bench-validated GET (1036 B doc response) + UPDATE (`shadow_bench_check_at` leaf merged cleanly into Shadow.reported) round-trips against `GS9999999999`. Subscribe-on-connect for all five shadow/* topics takes ~700 ms once per CONNECT. M12.1e.2 unblocked to use Shadow `desired.activated_at` re-check per §C.4.4 — no fallback to MQTT-retained activate cmd needed. Notes for M12.1e.2 sizing (2 KB shadow buffer target), JSON parsing (Zephyr json lib already in tree from M6a), and one observation about anomalously slow LTE-M registration on this run (~2:25 vs the usual 8 s — network conditions, not firmware). |

**Currently-open items (all firmware-side action):**

1. **§C.5.1 — Confirm NCS 3.2.4 `aws_iot` library Shadow get/update support.** Gates the M12.1e.2 build path (Device Shadow `desired.activated_at` re-check). Cloud's quick read suggests yes (`AWS_IOT_SHADOW_TOPIC_GET` + `aws_iot_shadow_update_accepted` event flow), but firmware needs to verify on bench. If Shadow turns out to be painful in NCS 3.2.4, fallback is MQTT-retained `activate` cmd. **Now formalized as micro-milestone M12.1e.1 in the arc; sequenced after M12.1c.2 production-shaped heartbeat lands.**
2. **§C.5.2 — Heartbeat clock drift characterization (deferred follow-up).** After M12.1c.2 is up and a few weeks of heartbeats are flowing, post a one-paragraph note on observed AT+CCLK? drift across PSM cycles + sub-second precision. Helps cloud-side anomaly detection threshold tuning. Not a blocker.
3. **§C.5.3 — Pre-activation battery cost per cycle (deferred follow-up).** After M12.1c.2 gives empirical numbers, report rough mC per modem-attach + heartbeat + Shadow-get + sleep cycle. If high, cloud may add a "stuck in pre-activation > 7 days" alarm threshold. Not a spec change. Not a blocker.

**Resolved 2026-04-27 (firmware §F2 ↔ cloud §C4 exchange + §F3 closure):**

- ✅ **§F2.3 heartbeat storage drift** — answer is Option 2 (deployed Lambda is pre-revision; spec describes post-1B-revision target). **Cloud team deployed Phase 1B revision at 23:38:54 UTC; firmware-side §F3.1 verified Shadow.reported writes end-to-end with our heartbeat at 03:22:45 UTC.** Heartbeat uplink table "Storage in cloud" row now reflects production reality.
- ✅ **§F2.4a uptimeS default-fill** — resolved by Phase 1B revision (Lambda 4 spec D16: missing fields stay missing in Shadow, no default-fill).
- ✅ **§F2.4b threshold-detector probe** — cloud ran it (§C4.3); logic fires correctly. Phase 1B revision is the fix for the patientId-keyed alerts table issue. Not a firmware concern.

**Cloud-side commitments — DONE 2026-04-27 (per §F3.3 verification):**

- ✅ **Phase 1B revision deploy** — heartbeat handler slimmed to Shadow update only; verified end-to-end via firmware §F3.1 publish landing in Shadow.reported version 9 with all 5 required fields.

**Cloud-side commitments — DONE 2026-04-27** (per cloud entries §C2 + §C3):

- ✅ Mint cert + key for `GS0000000001/2/3` and attach per-thing IoT policies — done; also added bench cert `GS9999999999` from reserved test range, reusable forever for firmware-team bench unit
- ✅ ~~DM the four 1Password shared items~~ — *cancelled* per §C3 single-dev correction; bundle staged at `~/Desktop/gosteady-firmware-cert-handoff-2026-04-27/` instead (firmware reads cert + key + Root CA directly from disk)
- ✅ Pre-create the four `ready_to_provision` Device Registry records
- ✅ `iot:GetThingShadow` + `iot:UpdateThingShadow` policy grants added early so M12.1e.2 path is unblocked from day one (per §C2.3)

**Cloud-side architectural follow-ups (within ~1 week, parallel to firmware M12.1c):**

- Phase 1A revision spec update — snippet IoT Rule + Lambda redesign, Shadow `desired.activated_at` write hooks across state-machine transitions, cumulative requirement updates
- Phase 1A revision deploy — `gs/{serial}/cmd` IoT policy, snippet S3 bucket, snippet parser Lambda, pre-activation alert suppression
- CLI helper for Device Registry bulk-import from companion repo CSV (deferred relevance — single-dev mode means rows for `GS9999999999` + `GS0000000001/2/3` are already pre-created; CLI helper matters for unit 5+)

**Firmware-side blockers cleared:** all 6 §F.9 items decided; cert bundle on disk; **M12.1c.1 (first heartbeat publish) DONE 2026-04-27 — first real-firmware heartbeat lands in cloud Shadow.** Next milestones (M12.1d activity uplink, M10.7.2 nPM1300, M12.1c.2 production-shaped heartbeat) all unblocked.

---

## Immediate Next Steps

### Where the project stands (2026-04-27)

A two-day sprint shipped the entire **M14-prep power architecture** (Phases 1a–5) — full deployment-mode autonomous capture works on bench:

> walker moves → ADXL367 chip-level activity detection fires → BMI270 confirmation window (σ > 50 mg gate) → green LED, session opens → walking → walker stops → 15 s of stillness → auto-stop closes session → BMI270 suspends → idle.

Zero button presses end-to-end. `CONFIG_PM=y` enables System ON Idle (~50 µA) between motion events. `CONFIG_GOSTEADY_FIELD_MODE` Kconfig gates off bench-only paths (SW0, dump UART, heartbeat LEDs) for the deployment build via `prj_field.conf` overlay.

Cross-team coordination with the cloud-side Claude session (see Portal Scope Impact §meta context) is fully caught up as of 2026-04-27. **All M12.1c.1 blockers are decided and the cert bundle is on disk** at `~/Desktop/gosteady-firmware-cert-handoff-2026-04-27/` — endpoint URL, Amazon Root CA 1, four cert+key pairs (one reusable bench cert `GS9999999999` + three shipping certs `GS0000000001/2/3`), all pre-attached to IoT Things + per-thing IoT policies + `ready_to_provision` Device Registry rows on cloud side.

**Done milestones (chronological):** M1–M7 (bench data capture pipeline) → M9 Phase 1–4 (auto-surface algorithm) → M10 (algo C port + on-device walking-path validation) → M10.5 (deployment requirements + portal coordination) → M12.1a (modem attach + AT+CCLK?) → **M14-prep Phase 1a/1b/2/3/4/5** (full power architecture, both build configs).

**Renumbering as of 2026-04-27** — the 15-Step Firmware Arc was refactored to: (a) split M12.1c into .1/.2 (separating "first cloud↔firmware connection" from "production-shaped heartbeat"); (b) split M11 into 11.1 algo-side / 11.2 deployment-side; (c) introduce **M10.7 Initial production telemetry** (storage repartition + nPM1300 + crash forensics, pulled forward from M14 because they're prereqs for the production-shaped heartbeat / no-OTA-safety-net respectively, both required before site-survey ship); (d) rename M14 to "Final production telemetry" (now a future-work bucket: OTA + unit-4+ hardening); (e) add **M14.5 Site-survey unit shakedown** as the explicit dress-rehearsal milestone between feature-complete and clinic ship; (f) drop M12.1b (folded into M12.1c.2); (g) introduce micro-milestone **M12.1e.1 NCS Shadow lib bench confirmation**. Driving priority is **see the cloud↔firmware connection ASAP** (M12.1c.1) so cloud-side work that's been built speculatively for weeks gets concrete acceptance testing.

### Recommended sequence

**M12.1c.1 + M12.1d done 2026-04-27** — first heartbeat from real firmware lands in cloud Shadow.reported (PUBACK 342 ms); first activity uplink from a real session-close lands in Activity Series DDB with all M9 algo outputs intact (PUBACK 655 ms). Cloud team's Phase 1B revision Lambda (heartbeat → Shadow) and activity-processor Lambda (activity → DDB + DeviceAssignment patient resolution) both verified end-to-end. Remaining cloud-surface milestones: **M10.7.2** + **M12.1c.2** for the production-shaped heartbeat. Deployment-readiness work — **M10.7.1** + **M10.7.3** + **M12.1e.1** + **M12.1e.2** + **M12.1f** — is largely parallel-safe and lands before **M14.5** site-survey shakedown (~1-2 weeks total). M14.5 includes the **M11.1** confirmation walk against shipping firmware. Then **M15** = clinic deployment with **M11.2** as the measured outcome.

Detailed per-milestone notes below in execution order. Resumption of **M8** carpet captures (runs 21-30) is non-blocking and can happen any time.

### M12.1c.1 — First cloud↔firmware connection — DONE 2026-04-27

Boot-to-PUBACK 17 s, broker ACK 342 ms. First real-firmware heartbeat from `GS9999999999` lands in cloud Shadow.reported with all 5 required fields (serial, ts, battery_pct=0.5 placeholder, rsrp_dbm, snr_db). See coord §F3 for milestone-complete entry; §F3.2 documents two firmware-side bugs found+fixed during validation. Detailed sub-task notes preserved below for reference.

Goal: any well-formed MQTT message reaches Device Shadow. Skip every optional field, every parallel polish item, every component that doesn't sit on the publish path.

**Sub-task 0 — Verify bundle + cloud-side path is live (~5 min):**

1. `ls ~/Desktop/gosteady-firmware-cert-handoff-2026-04-27/GS9999999999/` — confirm cert + key + Root CA files. Skim `GS9999999999/README.txt` — already documents the `AT%CMNG=0,<sec_tag>,...` flashing pattern.
2. Optional sanity check: re-run the verification block in cloud coord doc §C2.2 to confirm Thing + Registry row are still there and cert is still ACTIVE.
3. Cloud-side acceptance probe: `aws iot publish` a synthetic minimum-payload heartbeat to `gs/GS9999999999/heartbeat` and watch the Shadow update with `aws iot get-thing-shadow --thing-name GS9999999999`. **Validates the cloud-side path independently of firmware** — clean signal if firmware bring-up later fails.

**Sub-task 1 — Implementation:**

4. **Cert-flash host script** — `tools/flash_cert.py` (DONE 2026-04-27). Thin convenience wrapper around Nordic's `nrfcredstore` CLI (canonical NCS toolchain tool that handles AT%CMNG quoting, PEM parsing, and modem offline-mode dance internally — much more reliable than rolling our own). Resolves cert-bundle layout, globs the USB CDC port (handles macOS digit-shift), drives writes for `ROOT_CA_CERT` + `CLIENT_CERT` + `CLIENT_KEY` at one sec_tag, verifies via `nrfcredstore list`. **sec_tag locked at `201`** (matches NCS aws_iot sample default `CONFIG_MQTT_HELPER_SEC_TAG=201` — keeps gosteady firmware aligned with canonical lib config). Defaults flash bench cert `GS9999999999` from `~/Desktop/gosteady-firmware-cert-handoff-2026-04-27/`; `--serial GS0000000001` flashes shipping units later. **Prereq:** bench unit must be running the NCS at_client sample (`/opt/nordic/ncs/v3.2.4/nrf/samples/cellular/at_client/`) — temporarily overwrites gosteady firmware to expose the modem's AT interface over uart0 → bridge → `/dev/cu.usbmodem*102 @ 115200`. Reflash gosteady with `west flash` after; cert survives in CryptoCell-312 secure store.
5. **MQTT/TLS bring-up via NCS `aws_iot` lib** (~2 days). New `src/cloud.c` (do not entangle with `cellular.c`). `CONFIG_AWS_IOT=y`. Endpoint + sec_tag + serial all from Kconfig. Connect-on-attach, log handshake outcome verbosely first time through. Pin Amazon Root CA 1 explicitly via the bundled `AmazonRootCA1.pem`.
6. **One-shot heartbeat publisher with placeholders** (~half day). Required-fields-only payload: `serial = "GS9999999999"` (compile-time), real `ts` from M12.1a's `AT+CCLK?`, real `rsrp_dbm` + `snr_db` from M12.1a's reporter, **`battery_pct: 0.5` hardcoded placeholder**. One publish on first attach, then log + sleep.
7. **Watch it land cloud-side** with `aws iot get-thing-shadow --thing-name GS9999999999`. **THE moment** — schema validation, IoT Rule routing, Shadow `reported` write, all the speculatively-built cloud machinery, all gets exercised by this single message. Stop, take a breath, look at what cloud-side acceptance testing surfaces. Post a firmware-side milestone entry in `gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md` so cloud-side Claude reads it on next session.

### M12.1d — Activity uplink — DONE 2026-04-27

Bench session at 03:54:26-03:54:56Z (15 steps, 11.05 ft, R=0.1587, surface=indoor) hit broker with PUBACK in 655 ms; Activity Lambda resolved device → patient (`pt_test_001`) and wrote DDB row with all M9 algo outputs preserved. Reused M12.1c.1's connect-publish-disconnect helper inside cloud.c after a small refactor (mutex around aws_iot ops to serialize heartbeat + activity workers). session.c hooks the publish at the end of `session_stop()` (gated on `IS_ENABLED(CONFIG_GOSTEADY_CLOUD_ENABLE)`). Schema per coord §C.7 / §F.3 — all 6 required fields + 3 optional (roughness_R / surface_class / firmware_version) populated. Code in commit cb94d17.

### M10.7.2 — nPM1300 fuel gauge wiring (~1 day, parallel to M12.1d)

Real `battery_pct` + `battery_mv` via `nrf_fuel_gauge` lib. One-line swap from the M12.1c.1 placeholder. Hard prereq for M12.1c.2 since `battery_pct` is a required heartbeat field per portal contract.

### M12.1c.2 — Production-shaped heartbeat (~1-2 days; depends on M10.7.2 + M10.7.3)

Hourly cadence (`k_timer` + work item) + add the locked optional extras one at a time (`last_cmd_id`, `reset_reason`, `fault_counters`, `watchdog_hits`, `uptime_s`, `firmware`). Watch each appear in Shadow `extras` — confirms cloud's accept-all-unknown-fields behavior. The `reset_reason` / `fault_counters` / `watchdog_hits` fields stay zero-stamped until M10.7.3 lands; that's fine — they wire up cleanly later.

### M10.7.1 — Storage repartition (~3 hr)

`pm_static.yml` change. Foundation for M10.7.3 + M12.1f. Verify boot_count still increments + existing M10 sessions still mount unchanged. Use `-p always` after this change.

### M10.7.3 — Crash forensics + watchdog (~2 days; depends on M10.7.1)

Reset reason / fault counters / last-N log lines / watchdog hit counter persisted in M10.7.1's `crash_forensics` partition. Hardware watchdog kicked from a dedicated supervisor thread. **Stress-test: forced HardFaults, forced WDT hits, fault-during-FS-write.** Verify forensics survive reset and surface in next heartbeat (M12.1c.2's `reset_reason` / `fault_counters` / `watchdog_hits` fields read from this region). Best landed after M12.1c.2 is up because the new cellular code paths are where most novel faults will originate, so the forensics design knows what to capture.

### M12.1e.1 — NCS Shadow lib bench check — DONE 2026-04-29

PASS. NCS 3.2.4 `aws_iot` lib supports the Device Shadow surface end-to-end on this firmware build. Bench-validated against `GS9999999999`: GET pulled the 1036 B reported-doc payload via `AWS_IOT_SHADOW_TOPIC_GET_ACCEPTED` event; UPDATE wrote `shadow_bench_check_at` to `state.reported` (verified cloud-side via `aws iot-data get-thing-shadow`). Subscribe-on-connect for all five shadow/* topics (`get/{accepted,rejected}`, `update/{accepted,rejected,delta}`) takes ~700 ms once per CONNECT, then GET + UPDATE round-trip in normal broker timing. Implementation in commit `ca086be`: `CONFIG_GOSTEADY_CLOUD_SHADOW_BENCH_CHECK` Kconfig + a one-shot worker thread + handler-side dispatch on `evt->data.msg.topic.type_received`. Coord doc §C.5.1 closed via §F6 entry; M12.1e.2 cleared to use Shadow per §C.4.4 (no MQTT-retained activate fallback needed).

### M12.1e.2 — Pre-activation gate + Shadow re-check (~2 days; depends on M12.1e.1)

Refuses session capture until first activation cmd received. Blue LED slow-blink while in `ready_to_provision`. Shadow `desired.activated_at` re-check on every cellular wake (per §C.4.4). Persisted `activated_at` to flash. Echoes `cmd_id` on next heartbeat as ack.

### M12.1f — Snippet uplink (~3 days; depends on M10.7.1)

JSON-header framing per coord doc §F.3 + binary layout per §F.4 (4-byte BE length-prefix + JSON header + 16-byte payload header + 28-byte sample records). Opportunistic upload piggybacking on Priority-1 cellular wakes per M10.5 snippet upload policy.

### M14.5 — Site-survey unit shakedown (~1 day setup, ~1 week observation)

Flash `GS0000000001` with shipping cert + deployment build. Sit on bench desk powered up. Observe heartbeat stream + Shadow + battery curve + crash forensics across ≥7 days. Includes **M11.1 confirmation walk** with the walker against this exact build — algo-side validation against shipping firmware, not just bench firmware.

### M15 — Field testing + M11.2 deployment-side validation

Flash `GS0000000002/3` with shipping certs (procedure now well-shaken from M14.5; `GS0000000001` may stay as the survey/dev unit per use case). Deploy to clinic. **M11.2** runs continuously across the ~1-month deployment window: in-field distance estimates vs any clinic-supplied ground truth. Battery life is a measured outcome (not a pre-tuned constraint per 2026-04-25 decision). v1.5 retrain inputs accumulate: snippets returned from devices + resumed M8 carpet captures.

### M8 cont. — Resume carpet captures (runs 21-30), non-blocking

Not blocking anything else; the 19-run dataset already in the v1 algorithm coefficients is enough for the deployment build. Useful for v1.5 retrain when convenient. Capture command sequence:

```bash
cd ~/Documents/gosteady-firmware
STAMP=$(date +%F)
mkdir -p raw_sessions/$STAMP
PY=/opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3
$PY tools/cleanup_device.py --port /dev/cu.usbmodem*1105 --all --yes
$PY tools/log_console.py &
# Open https://jabl1629.github.io/gosteady-firmware/tools/capture.html in Chrome,
# Connect, walk the protocol row by row. Hard-refresh once at the start to pick
# up any Pages updates. Use the M7 popup at every STOP.
$PY tools/pull_sessions.py --port /dev/cu.usbmodem*1105 --out raw_sessions/$STAMP/
mv ~/Downloads/gosteady_capture_notes_$STAMP.json raw_sessions/$STAMP/
$PY tools/ingest_capture.py \
    --sessions raw_sessions/$STAMP/ \
    --notes    raw_sessions/$STAMP/gosteady_capture_notes_$STAMP.json \
    --out      raw_sessions/$STAMP/capture_$STAMP.csv
algo/venv/bin/python3 -m algo.run_auto_surface
algo/venv/bin/python3 -m algo.run_summary
```

Outstanding from prior captures: **outdoor s-curve (run 18)** is still missing (skipped 2026-04-25 due to spacing). Worth grabbing on the next outdoor session.

### v1.5 retrain (post-deployment)

Whenever carpet captures + additional outdoor reps + field-collected sessions accumulate:

1. Add carpet rows to the per-surface coefficient table.
2. Refine the R classifier (hard-threshold → multi-bin lookup or soft sigmoid blend if boundary cases warrant).
3. Re-A/B single vs multi-feature stride regression at n=24+ where multi-feature collinearity should be tractable.
4. Regenerate `src/algo/gosteady_algo_params.h`, push as a coefficient-only firmware update via OTA (post-deployment) or manual reflash (during deployment).
5. Re-evaluate motion-gate thresholds against the carpet stationary baseline + field stationary windows.

The algorithm code is unchanged in this transition — only the generated coefficient header rolls forward. That's the upside of the streaming + lookup-table architecture.

---

## Host-side operator tooling

Six tools, all in `tools/`. They share the same line-protocol with the firmware; they differ only in transport and UX.

| Tool | Transport | Purpose |
|---|---|---|
| `tools/capture.html` | BLE NUS via **Chrome on Mac** (not Bluefy/iOS anymore — see M6b notes) | Operator UI during a sealed-cap capture run. 30 preset buttons from the v1 matrix; pinned STOP button; auto-opens a response log on connect. **Post-STOP modal** captures POST-WALK notes per run (M7). Completion badges + notes persisted in `localStorage`. **Export capture notes (JSON)** button downloads a schema-v1 sidecar. Served at **`https://jabl1629.github.io/gosteady-firmware/tools/capture.html`** (GitHub Pages). |
| `tools/control.py` | USB `/dev/cu.usbmodem*1105` @ 1 Mbaud | Bench equivalent of `capture.html` for when the cap is open. Same `START` / `STOP` / `STATUS` commands, same preset dict as `capture.html`, plus a `raw` subcommand for arbitrary lines. |
| `tools/pull_sessions.py` | USB `/dev/cu.usbmodem*1105` @ 1 Mbaud | Post-session file pull. `LIST`s the session directory, `DUMP`s each file to local disk (`./sessions/<uuid>.dat` by default; conventionally used with `--out raw_sessions/<date>/`), optionally `DEL`s on device with `--rm`. Noise-tolerant `_read_protocol_line()` handles cross-channel STATUS bleed from a concurrent BLE client. |
| `tools/cleanup_device.py` | USB `/dev/cu.usbmodem*1105` @ 1 Mbaud | Wipe `/lfs/sessions/` before a fresh capture. Dry-run by default; `--all --yes` for unattended use. Reuses pull_sessions.py's transport. |
| `tools/ingest_capture.py` | — (offline) | Joins pulled `.dat` files (`--sessions DIR`) with POST-WALK notes JSON (`--notes FILE`) on `session_uuid`, emits a CSV matching the `Captures` sheet of `GoSteady_Capture_Annotations_v1.xlsx` column-for-column. Warns on missing notes, orphan notes, and header errors. |
| `tools/read_session.py` | — (offline) | Parses a pulled `.dat` file or a base64 header string, validates every controlled-vocab field against the v1 vocabulary, prints FIRMWARE + PRE-WALK + body summary. Reports `duration_ms` / `derived_rate_hz` from min/max `t_ms` (robust to non-monotonic streams), and flags `t_ms_monotonic=false` + the first backward jump so contaminated pre-fix session files self-identify loudly. |

**Firmware-side command protocol** (same on USB uart1 and BLE NUS after M6b):

```
PING\n              → PONG\n
STATUS\n            → STATUS active={0|1}\n
START <json>\n      → OK started <uuid>\n   (or ERR <reason>\n) — UUID added 2026-04-23 (M7)
STOP\n              → OK samples=N\n        (or ERR not active\n)
LIST\n              → <name> <size>\n ... END\n
DUMP <name>\n       → SIZE <n>\n <n raw bytes> \nOK\n
DEL <name>\n        → OK\n                  (or ERR <reason>\n)
```

All commands are newline-terminated. `\n` is a literal `0x0A` byte — no escape-sequence parsing.

**Shared-channel caveat:** uart1 is a broadcast channel — the bridge forwards the 91's uart1 TX to BOTH the USB CDC endpoint AND the BLE NUS TX notify. If a BLE client (capture.html) is connected, its 5 s STATUS poll causes `STATUS active=0\n` to appear on the USB stream between any other host's command transactions. The firmware's dispatch loop is serial so noise NEVER interleaves mid-transaction (body bytes are safe), but any host tool that reads uart1 must **skip recognised noise lines** when looking for its expected response. `tools/pull_sessions.py::_read_protocol_line()` does this — reuse it in new host tools.

---

## Key Design Decisions Made

**Algorithm / sensing:**
- **Step-based, not dead reckoning** — dead reckoning gives 73% MAPE with MEMS IMUs, unusable.
- **Accelerometer magnitude** for step detection — orientation-independent (`sqrt(x² + y² + z²)`).
- **Stride regression from peak acceleration** — no speed label needed; prior work hit 12.4% MAPE on standard/tacky, our rebuild hits 16.4% MAPE on two-wheel/glide (10-run shakedown, to be re-fit at 30).
- **No microphone** for surface detection — IMU high-frequency content has the friction signal, mic adds cost/power/board complexity.
- **BMI270 is the session recorder; ADXL367 is the power switch** — the 7-column `.dat` format the prior Python algorithm expected requires accel + gyro, which only BMI270 provides. ADXL367 stays online as a sanity print in M2+ and will be used for wake-on-motion in the later power-optimization milestone.

**Scope / platform:**
- **v1 data collection is single-walker, single-cap, 3 surfaces** — complexity deferred to v2.
- **Thingy:91 X as dev platform** — production will use a custom PCB with its own BLE + cellular silicon, so bridge firmware work here is prototype-only.
- **`thingy91x/nrf9151/ns`** as build target — non-secure application core, TF-M in secure partition.

**M4–M6 architecture:**
- **Binary session file with versioned 256-byte header** — `_Static_assert`ed on-device, `struct.calcsize`'d on-host, so C/Python drift fails loudly on either side.
- **Sampler thread decoupled from fs_write via k_msgq** — sampler's only job is "sample and enqueue"; a dedicated writer thread batches flash writes. Rate went 96 Hz → 100 Hz when we made this change; `flash_errors` now means "queue-full drops" rather than "fs_write short-returns".
- **Transport-agnostic command parser** (`src/control.c`) — same parser handles USB uart1 and BLE NUS. Adding a new transport (cellular in M12? another BLE service?) is a nRF5340-side change only.
- **BLE, not cellular, for v1 operator control** — operator is always in the room during capture; a Web Bluetooth page is the right UX. Cellular is overkill for v1 and adds indoor-coverage / backend failure modes that would kill data capture. Cellular is M12/M13 for production telemetry upload, not session control.
- **Don't delete bad runs, mark `valid=N`** — canonical per v1 capture protocol. Firmware has a `DEL` command available to the ingest script for deliberate cleanup, but the operator UI (`capture.html`) has no delete button. All files survive to post-session review.
- **Bridge firmware vendored wholesale** into `bridge_fw/` rather than patched at build-time. 264 KB of Nordic source in the repo is fine; the build-system fragility of a patch-on-build approach would be worse.
- **Browser-side chunking of BLE writes instead of slimming the protocol (2026-04-22).** Found during M6b end-to-end validation: the ~270-byte `START` JSON doesn't make it through the BLE NUS RX → bridge → uart1 path (see Known Gotchas). Considered moving to a short `START <uuid>` with host-side metadata and lean session headers, which is architecturally cleaner and closer to how cellular telemetry will look in M12+. Chose instead to chunk the outbound write at ≤180 B in `capture.html` and keep session files fully self-describing, because (a) the chunking patch is 6 lines and ~10 min to ship; (b) a lean rewrite touches `control.c`, `session.h/c`, `capture.html`, `control.py`, `read_session.py` plus re-validation on hardware, a 1–2 day cost against the immediate goal of v1 capture; (c) self-describing session files preserve metadata recoverability if the host-side state is ever lost between `START` and ingest; (d) the natural time to revisit is M12–M14 when we're wiring cellular + backend + custom PCB anyway — the lean protocol falls out of that work informed by production requirements, not a guess.
- **Web BLE page on GitHub Pages**, not a native iOS app — a single static HTML file shipped from the repo auto-redeploys on every push. Originally intended for Bluefy on iOS; as of 2026-04-23 we run it in **Chrome on the Mac** instead (Bluefy's App Store build doesn't opt into `WKWebView.isInspectable`, so Safari Web Inspector can't attach — Chrome gives us full DevTools + Claude-in-Chrome extension visibility for debugging). For the 30-run v1 capture the Mac sits at course midpoint; operator walks the course and returns to the laptop between runs. Native iOS app / phone-in-pocket workflow deferred until the debug need goes away.

**M7 architecture:**

- **POST-WALK captured in the browser at STOP time, NOT in the session file header (2026-04-23).** Considered expanding `struct gosteady_session_header` to include the 7 POST-WALK fields so sessions stay self-describing for POST-WALK too. Chose instead to keep POST-WALK in a separate JSON sidecar (`gosteady_capture_notes_<date>.json`) keyed by `session_uuid`, joined at ingest by `tools/ingest_capture.py`. Rationale: (a) POST-WALK is operator judgment, not device state — no reason the firmware needs to know; (b) editing POST-WALK after the fact is trivial in a JSON file, hard in a 256-byte packed binary header; (c) versioning the POST-WALK schema independently is safer; (d) the firmware's 256-byte header already has `_reserved[67]` for room-to-grow on FIRMWARE/PRE-WALK fields but we didn't want to burn that on POST-WALK. The join tool is responsible for reconciling the two sides.
- **`session_uuid` echoed on `OK started <uuid>` as the join key.** Without this the browser has no way to correlate its POST-WALK entry to the specific `.dat` file on flash (the header UUID is known only to the 91 until LIST/DUMP at ingest). `gosteady_session_get_uuid_str()` in `src/session.h` is the public API. Backward-compatible: a bare `OK started` still parses on the browser side (UUID becomes null, POST-WALK still saves by position — but we'd lose the primary-key guarantee, so don't regress the echo).
- **Post-STOP modal, not an inline form.** Alternatives considered: (a) full POST-WALK form always visible as a side panel; (b) per-row edit icon on completed rows. Went with (a)+(b) hybrid: modal on STOP forces an in-the-moment decision (`valid=Y/N` is the one field that's hard to reconstruct later), and the modal's three paths (Good / Discard / Notes) trade off friction for expressiveness. Tap-to-edit on completed rows deferred until it's actually needed — `capture.html` is currently write-once per run and that's been fine through the shakedown.

**M9 architecture (2026-04-24 / 2026-04-25):**

- **Paused M8 at 10/30 runs to build the M9 framework first.** Normally you'd capture the full dataset then develop the algorithm. Chose instead to develop the pipeline end-to-end against the 10-run shakedown and let later runs feed a locked-in framework. Rationale: (a) the *architecture* decisions (step-based vs energy, single vs multi-feature, motion-gate design, surface handling) need data to be made honestly, but 10–16 runs is plenty to make them; (b) the *parameter* decisions (filter coefficients, regression weights) scale well with data once the framework exists; (c) architecture exploration *during* data collection would cause scope creep at capture time. With the framework locked, the remaining captures are mechanical.
- **Streaming-first Python, matching CMSIS-DSP semantics exactly.** Every DSP block (`filters.py::BiquadSOS`, `motion_gate.py::MotionGate`, `step_detector.py::StepDetector`) has a `.step(x)` method that processes one sample at a time with O(1) state, and a `.apply(arr)` method that's a trivial loop over `.step()`. Numerically bit-identical. `BiquadSOS` uses Direct Form II Transposed specifically because that's what `arm_biquad_cascade_df2T_f32` uses on the nRF9151 — the C port is a mechanical translation, not a re-derivation. Total on-device state across all blocks: ~200 bytes.
- **Motion gate as a shared block.** The 500 ms running-σ gate serves four purposes simultaneously: (a) primary output for the `active_minutes` product metric; (b) distance-accumulation gate (rejects peaks during stationary periods — what passes the 1 ft stationary robustness gate); (c) **roughness-metric gate** (the inter-peak RMS R is computed only over motion-positive samples — settling tails would otherwise drag short-walk R values down and break auto-classification); (d) future power gating on-device (skip full peak detection when parked). One block, four uses, one place to tune.
- **Per-run total distance as the regression target, not per-step stride.** Fit objective is Σ_peaks(c₀ + c·features) = actual_distance_ft per run. This trains on all walk runs (including those where `manual_step_count` was missed) rather than only the runs with step-count ground truth. Also matches what production inference actually has access to: the firmware will never have a manual step count.
- **Single-feature regression (amplitude only) for v1.** Multi-feature (amplitude + duration + energy) overfit at n=8 (cond(X)=212, energy coefficient came out at −21.2 — nonsensical) and didn't help at n=16 (cond(X)=318). At n=24+ with surface variation, multi-feature may stabilize and reduce indoor MAPE by 1–3 pt; revisit at v1.5 retrain.
- **Loose Schmitt thresholds (0.02 g enter / 0.005 g exit / 0.5 s min-gap) over strict (0.05 g / 0.01 g / 1.0 s).** Both hit essentially identical MAPE (16.4% vs 16.2%) on indoor data. Loose was picked because: (a) passes the stationary robustness gate (0.79 ft) while strict narrowly fails (1.28 ft); (b) the gait has a compound 2:1 accel-impulse-to-operator-step ratio — loose captures both impulses consistently across surfaces; (c) the detector ratio (detected/manual ≈1.5×) is *universal across indoor and outdoor* — surface changes the impulse amplitude but not the cycle structure, so a single detector configuration carries.
- **Energy-based distance abandoned as the primitive, kept as a diagnostic.** Integrated `∫ |a|² dt` over motion windows hit 44% MAPE alone and 28% with motion-duration as a second feature. Root cause: E ∝ speed² while distance ∝ speed, so "energy per foot" varies 10× across the speed range. `energy_estimator.py` is kept in the tree as a per-run diagnostic so we can watch E-vs-distance as the dataset grows.

**Auto-surface architecture (2026-04-25):**

- **Auto-surface roughness adjustment over hardcoded operator-supplied surface table.** Per-surface single-feature regressions delivered the best raw accuracy (16.4% indoor / 23.3% outdoor) on the available data. The temptation was to ship a v1 that asks the operator to pick the surface and looks up coefficients. Rejected because: (a) the field deployment surfaces are a continuum (hardwood, tile, vinyl, carpet variants, indoor/outdoor concrete, asphalt, rugs, transitions) — operator-supplied surface enums don't generalize; (b) operator-burden grows with every session in real use, vs once-per-30-runs in our capture protocol; (c) the IMU stream itself contains a surface-discriminating signal (motion-gated inter-peak RMS shows ~1.7× separation between indoor and outdoor). Decision: ship the auto-surface classifier from v1 — operator never picks a surface, the firmware self-calibrates.
- **Motion-gated inter-peak RMS as the roughness metric.** Surface texture transmits through the wheels into the cap during the *glide / recovery* phase between gait impulses. Inter-peak RMS captures this directly. Motion-gating (compute only over `motion_mask == True` samples, excluding ±200 ms peak windows) was critical — first-pass un-gated metrics were dragged down by start/stop quiet periods on short walks, putting short outdoor walks in the indoor R range. The HF/LF PSD ratio was tested as an alternative; both metrics work, IPR slightly cleaner separation, both motion-gated. IPR also has a trivial streaming form for the C port.
- **Hard threshold τ=0.245 over soft sigmoid blend.** With n=16, soft blends underperformed because the blended coefficients pull every walk toward the average — penalizing correctly-classifiable walks to slightly help boundary cases. Hard threshold misclassifies 1 of 16 walks and matches the per-surface baseline on the other 15. At v1.5 with more data, switching to soft blend or multi-bin lookup is a one-line code change. The *architecture* is unchanged.
- **R-overlap accepted as a known v1 limitation, not a blocker.** At n=8 per surface there's a small overlap zone (0.219–0.232) where one walk per surface lives. The single misclassification (outdoor run 17) drives the +63.9% prediction error that dominates the outdoor LOO MAPE. Expected to improve substantially at v1.5 retrain when carpet adds a third R cluster + per-surface n grows toward 12+.
- **R logged as session metadata regardless.** Even before v1.5 retrain, every shipped session's distance + active_minutes + R will be logged to telemetry. R distribution from real-world deployment becomes the dataset that drives v1.5 calibration. We're shipping the *data collection mechanism* in v1, not just the v1 algorithm.

**Runtime gates we know about:**
- `BLE_ENABLED=1` in `Config.txt` on `/Volumes/NO NAME` — gets wiped by `west flash --recover`, needs re-toggle after.
- **Chrome remembers the BLE pairing per-origin** for the capture.html URL, so reconnects are usually one-click. If Chrome's BLE chooser starts re-prompting, `chrome://settings/content/bluetoothDevices` is where persistent permissions live.
- **USB CDC endpoints re-enumerate on replug** (macOS may drop or add a leading digit: `11105` ↔ `1105`). Re-check `ls /dev/cu.usbmodem*` before any host-side tool invocation after a physical replug.
- *(Legacy — iOS-only, not applicable to Chrome-on-Mac workflow):* "Forget This Device" after every bridge reflash, or iOS occupies the single BLE connection slot.
