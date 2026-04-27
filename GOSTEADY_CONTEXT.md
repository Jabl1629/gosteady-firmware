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
- **M14-prep Phase 1b (BMI270 suspend between sessions, 2026-04-27):** Second piece of the deployment-mode power architecture. The BMI270 (~325 µA continuous in normal mode, ~0.5 µA suspended) now sleeps between sessions — that's the dominant idle term in the deployment power budget. Implementation in `src/main.c::sampler_entry`: on `idle→active` transition (session start), call `bmi270_set_active(true)` to write `SAMPLING_FREQUENCY=100` (re-enables `PWR_CTRL.{ACC_EN,GYR_EN}` per the upstream Zephyr driver) → settle 25 ms → 2 prime fetches → drop next 4 body samples (chip's data registers report zeros for ~3 ODR periods past the settle window — empirically determined on bench with a debug log instrumented in the discard branch). On `active→idle` transition (session stop), suspend again. Skip `sensor_sample_fetch` entirely while idle so SPI bus is quiet. Boot-time also suspends the chip after `configure_bmi270()` so deployment-mode firmware spends pre-first-session in idle current (one-time 30 ms warm-up at boot before the first suspend so the chip's data registers contain real gravity samples before going to sleep — needed for the ZeroSampleAvoidance pattern to work after subsequent resumes). Verified on bench: first persisted sample at t_ms=31 with real gravity data, rate=100.01 Hz, monotonic, dropped=0. Cost: 4 dropped samples (~40 ms) at session start = immaterial vs the M9 motion gate's 500 ms settling.
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
10.5. **Field deployment requirements** — define the firmware capability set + cloud contract for the first remote deployment (3 Thingy:91 X units, rehab clinic, ~1 month, cellular only, no charging, no OTA, no user interaction). Drives M11–M14 sequencing. *(active in parallel with M10 — see "Field Deployment (M10.5)" section + "Portal Scope Impact" section.)*
11. **Validation** — hold-out error characterization. (Algorithm-side: re-run cross-surface LOO with M10 C outputs from reference vectors; deployment-side: continuous in-field comparison against any clinic-supplied ground truth.)
12. **Cellular** — LTE-M / NB-IoT link up on nRF9151. (Scoped by M10.5: MQTT/TLS to AWS IoT Core, hourly heartbeat, session-end activity uplink, opportunistic snippet flush. Sub-milestones: **M12.1a (modem attach + signal stats + AT+CCLK? time) DONE 2026-04-26**; M12.1b (refine reporter cadence + PSM enter/exit logging) deferred until 1c lands; **M12.1c — MQTT/TLS to AWS IoT Core + first heartbeat publish to `gs/{serial}/heartbeat`**, requires per-device cert provisioning into the nRF9151 modem (sec_tag 16842753-style flow); M12.1d — activity uplink on session close; M12.1e — pre-activation gate + `gs/{serial}/cmd` activation downlink; M12.1f — snippet uplink on `gs/{serial}/snippet` once snippet capture path lands.)
13. **Cloud backend** — session telemetry upload. (Coordinated with portal Phase 1A/1B — already specified; firmware-side work is the on-device side of the contract.)
14. **Production telemetry** — battery (nPM1300 fuel gauge), error/fault counters, OTA hooks. (M10.5 defines what's required vs. nice-to-have for first deployment.)
15. **Field testing.**

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
| Auth (first 3 units) | Per-device cert + private key flashed manually at build time, stored in nRF9151 CryptoCell-312 / TF-M secure element. | **Locked 2026-04-25** — defers AWS IoT fleet provisioning for first deployment |
| Auth (post-first-deployment) | AWS IoT Fleet Provisioning with claim cert at flash time + CSR exchange on first boot | Deferred to portal Phase 5A coordination |
| First-boot binding | Cloud-side state machine: `ready_to_provision` → `provisioned` → `active_monitoring` driven by first-heartbeat | Portal-side; no firmware action beyond "send heartbeat normally" |

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
| Storage in cloud | AWS IoT Device Shadow `reported` state (not DynamoDB direct write) | Locked by portal spec |

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
| Cloud routing | IoT Rule with direct S3 action — no Lambda. Lands at `s3://gosteady-{env}-snippets/{serial}/{date}/{snippet_id}.bin`. AWS-managed SSE encryption. | **Locked 2026-04-17 (portal)** |
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

**Moved to the shared coordination doc 2026-04-26.** See `gosteady-portal/docs/firmware-coordination/2026-04-17-cloud-contracts.md` (and any subsequent dated entries below it) for the live conversation record. The append-only convention means every dated entry stays as a permanent record — search the file for the latest entry from each team to see current status.

Brief current state (full detail in the shared doc):

- All four items the portal team flagged on 2026-04-17 (activation contract, heartbeat extras, activity extras, snippet upload schema) — **resolved** in their 2026-04-17 entry; firmware acknowledged in the 2026-04-26 entry.
- Cloud team's 6 action items for firmware (last_cmd_id ack, MQTT 5 user properties, snippet binary layout, site-survey timeline, pre-activation LED, serial number enrollment) — **all six answered firmware-side in the 2026-04-26 entry**.
- New firmware-team questions pending cloud response (cert provisioning workflow, AWS IoT endpoint URL + Root CA confirmation, starting serial range, pre-activation cmd-listen-window contract, snippet payload encryption posture) — **logged in the 2026-04-26 entry**, awaiting portal response.

---

## Immediate Next Steps

The M9 algorithm architecture is locked as of 2026-04-25 (auto-surface roughness adjustment, 22.4% pooled MAPE LOO at n=16). The user has explicitly chosen to **proceed to M10+ now** rather than wait for the carpet captures to finish — the v1 algorithm ships with auto-surface from the start, and v1.5 retrains coefficients with more data later. M10 (C port) and M10.5 (deployment requirements + portal coordination) run in parallel; M11–M14 are scoped by what M10.5 lands on.

### Track A — M10 (C port), DONE 2026-04-25

The full V1 distance estimator runs on-device. Modules under `src/algo/` (one .h + .c each):

- `gs_filters` — DF-II-T biquad cascade. Coefficients in CMSIS-DSP `arm_biquad_cascade_df2T_f32` storage layout (`a1`/`a2` already-negated per CMSIS convention) so the on-device path can later swap to CMSIS-DSP without touching the algo header. Currently uses our own biquad (~25 LoC) — portable to host tests and bit-equivalent.
- `gs_motion_gate` — running-σ + Schmitt FSM with hysteresis. Fixed 64-sample ring (handles the V1 50-sample / 500ms window with headroom).
- `gs_step_detector` — Schmitt FSM + per-peak features (amplitude, duration, energy). Mirrors the Python including the off-by-one duration vs energy-sum-includes-exit-sample quirk; tests caught + locked the convention.
- `gs_roughness` — batch motion-gated inter-peak RMS. Streaming variant deferred — the orchestrator buffers `mag_lp` + `motion_mask` in RAM (12000 sample cap = 120s @ 100Hz, 60 KB) and runs the batch at session close. Sessions exceeding the cap still emit distance/step_count/active_minutes correctly; only `roughness_R` falls back to NaN with `buffer_overflowed=true`.
- `gs_pipeline` — orchestrator. `init` → `session_start(first_mag_g)` (primes HP filter to steady-state at the first sample's gravity) → `step(mag_g)` per sample → `finalize(&outputs)`. Outputs: `distance_ft`, `roughness_R`, `surface_class`, `step_count`, `motion_duration_s`, `total_duration_s`, `motion_fraction`, `buffer_overflowed`.

Integration in [src/session.c](src/session.c): the writer thread feeds every persisted sample to `gs_pipeline_step()`. At session stop, after the final `writer_flush` and before the header rewrite, `gs_pipeline_finalize()` runs and the outputs are emitted as two log lines (split because the combined string overflows Zephyr's per-message buffer):

```
ALGO_V1A uuid=<u> distance_ft=<f> R=<f|nan> surface=<n> steps=<u>
ALGO_V1B uuid=<u> motion_s=<f> total_s=<f> motion_frac=<f> overflow=<0|1>
```

`prj.conf` adds `CONFIG_CBPRINTF_FP_SUPPORT=y` so `%f` renders actual numbers (without it, the deferred-logging backend prints `*float*`). `firmware_version` in the session header bumped to `0.6.0-algo` so any session captured by this build is unambiguously identifiable. Algo outputs are NOT yet stamped into the 256-byte header's `_reserved[67]` block — that's a follow-up; for now the log line is the canonical channel.

**Host regression suite** at [tests/host/](tests/host/): `make` builds the algo .c files + `test_main.c` + `test_loader.c` against the host's compiler (no Zephyr/CMSIS dep), runs the three M10 reference vectors through five test layers (HP, LP, motion gate, step detector, end-to-end pipeline), passes 31/31 checks within float32-vs-float64 tolerance. Tightest result: indoor_run05 distance differs from Python by 0.0007% (14.972 ft on-device vs 14.972 ft Python LOO). Loosest: outdoor_run13 R differs by 0.27% — within expected float32 cumulative roundoff. Per-peak detail is "best-effort match" rather than strict because float32 cumulative noise can shift threshold-crossing peaks ±a-few samples without changing the regression sum.

**On-device validation:** stationary baseline (cap on desk, 8s session) emits `distance_ft=0.00 R=nan surface=0 steps=0 motion_s=0.00` — exactly mirrors the `indoor_run09` fixture's expected output. No `writer fs_write` errors (the 2026-04-25 STOP-race fix held). Walking-path on-device validation is the one outstanding piece — needs a physical "pick up the cap and walk" test to confirm the on-device numbers match what the algo predicts for the same data when run in Python.

The 19-run dataset (10 indoor + 8 outdoor + 1 outdoor stationary) is enough to ship initial coefficients into the C header. v1.5 regenerates the header from the 30+ run dataset once carpet captures land — the architecture is unchanged across that transition; only `gosteady_algo_params.h` rolls forward.

### Track B — Resume M8 captures (runs 21–30 carpet) when convenient

Not blocking M10. Capture-time command sequence (unchanged from before, just execute when the time is right):

```bash
cd ~/Documents/gosteady-firmware
STAMP=$(date +%F)
mkdir -p raw_sessions/$STAMP

# Wipe the device so the pull only contains real runs.
PY=/opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3
$PY tools/cleanup_device.py --port /dev/cu.usbmodem*1105 --all --yes

# Start the uart0 logger in the background — captures every session start/stop +
# any fault dumps automatically to logs/uart0_$STAMP.log.
$PY tools/log_console.py &

# Open https://jabl1629.github.io/gosteady-firmware/tools/capture.html in Chrome,
# Connect, walk the protocol row by row. Hard-refresh once at the start to pick
# up any Pages updates. Use the M7 popup at every STOP.

# Post-capture:
$PY tools/pull_sessions.py --port /dev/cu.usbmodem*1105 --out raw_sessions/$STAMP/
mv ~/Downloads/gosteady_capture_notes_$STAMP.json raw_sessions/$STAMP/
$PY tools/ingest_capture.py \
    --sessions raw_sessions/$STAMP/ \
    --notes    raw_sessions/$STAMP/gosteady_capture_notes_$STAMP.json \
    --out      raw_sessions/$STAMP/capture_$STAMP.csv

# Re-run cross-surface analysis against the cumulative dataset:
algo/venv/bin/python3 -m algo.run_auto_surface
algo/venv/bin/python3 -m algo.run_summary
```

Outstanding: **outdoor s-curve (run 18)** is still missing from the v1 dataset (skipped on 2026-04-25 due to spacing). Worth grabbing on the next outdoor session.

### Track C — M10.5 deployment-prep work (parallel to M10 algo port)

These are the firmware tracks that M10.5 calls out as required for first deployment. All proceed independently of M8/M9 retrain. Scope and contracts are defined in the **Field Deployment (M10.5)** + **Portal Scope Impact** sections above.

- **Cellular (LTE-M / NB-IoT) + MQTT/TLS to AWS IoT Core** — heartbeat publish on `gs/{serial}/heartbeat` every hour, activity publish on `gs/{serial}/activity` per session close. Per-device cert flashed manually (no fleet provisioning for first 3 units). Cellular network time via `AT+CCLK?` fills `session_*_utc_ms`. Long lead time on provisioning + RF; start now.
- **Power architecture** — ADXL367 wake-on-motion as primary out-of-session wake source; nRF9151 deep sleep + BMI270 power-down between sessions; LTE-M PSM with hourly wake. This is the single most impactful work for hitting the 1-month battery target.
- **nPM1300 fuel gauge wiring** — must land before site-survey unit ships. Fills `battery_mv` + `battery_pct` in heartbeat payload.
- **Crash forensics** — persistent reset reason + fault counters + last-N log lines + watchdog hit counter in a dedicated flash region (separate from LittleFS); surfaced in next heartbeat after reset. Hardware watchdog kicked from a dedicated supervisor thread.
- **Storage repartition** — grow LittleFS into the unused 19 MB on the GD25LE255E to add a snippet partition (~16 MB) + dedicated telemetry queue + crash-forensics region. Done via `pm_static.yml`; needs `-p always` rebuild.
- **Snippet capture + storage** — scheduled (4/day) + anomaly-triggered (≤4/day cap), 30 s windows of raw 100 Hz BMI270 stored in the new snippet partition. Capture-pause-when-full and 14-day stale cutoff per M10.5 policy.
- **`CONFIG_GOSTEADY_FIELD_MODE` Kconfig** — gates off heartbeat tick LED + session LEDs + bench USB/BLE control surface. Pre-activation blue LED is the only LED behavior. Enables one build target for deployment vs bench.
- **Pre-activation gate** — refuses session capture until first cloud activation message received. Blocked on portal-side activation contract definition (open coordination item #1).

### v1.5 retrain (after M10 ships, when carpet + extra data lands)

Whenever the carpet captures + additional outdoor reps are done:

1. Add carpet rows to the per-surface coefficient table.
2. Refine the R classifier from hard-threshold to either a multi-bin lookup (3+ surface bins) or a soft sigmoid blend if the boundary cases warrant it.
3. Re-A/B single vs multi-feature stride regression at n=24+ where multi-feature collinearity should be tractable.
4. Regenerate `src/algo/gosteady_algo_params.h`, push as a coefficient-only firmware update.
5. Re-evaluate motion-gate thresholds against the carpet stationary baseline (run 29) and any field-collected stationary windows.

The algorithm code is *unchanged* in this transition — only the generated coefficient header rolls forward. That's the upside of the streaming + lookup-table architecture.

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
