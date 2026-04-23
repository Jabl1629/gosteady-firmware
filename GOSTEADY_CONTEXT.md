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

The repo is at a **BLE-controllable session-logging target with closed-loop POST-WALK capture** — the walker cap can be sealed and the operator drives `START` / `STOP` wirelessly from Chrome on the Mac, with a per-run popup that collects the operator's validity + notes decision in the same UI. On boot (nRF9151 side): mount LittleFS, bump the boot counter, start a 100 Hz BMI270 sampling thread with a decoupled writer, wire SW0 as a bench-fallback toggle for session start/stop, bring up the uart1 dump/control channel. LED state tells the operator which mode they're in (solid green while recording, purple blink while idle). On the nRF5340 side: our patched connectivity bridge runs, exposing BLE NUS tunneled to uart1. As of **2026-04-23**, **Milestones 1–7 are complete and shakedown-validated**: toolchain, SDK, J-Link path, build+flash cycle, serial console, sensor drivers, LittleFS, session-file writer, USB host-pull path, BLE session control, and the full POST-WALK capture-notes-to-spreadsheet pipeline are all verified against real hardware with a 10-run shakedown (Indoor Polished Concrete segment of the v1 matrix) that produced clean 100.00 Hz data on every file, zero `flash_errors`, zero `t_ms` monotonicity violations, and correct POST-WALK → session file joins on all 10.

- **M1 (dev env):** `<inf> gosteady: heartbeat tick=N` prints at 1 Hz on `/dev/cu.usbmodem102`.
- **M2 (sensor bring-up):** BMI270 (SPI) and ADXL367 (I²C) both produce sane readings — ~±1 g on the gravity axis at rest, noise-floor-level signal on the other axes. BMI270 is configured at 4 g / 500 dps / 100 Hz per the v1 annotation schema. **Note:** Z-axis signs are opposite between BMI270 (+1 g) and ADXL367 (-1 g) because the two chips are mounted in different orientations on the PCB — handle this at the algorithm fusion layer.
- **M3 (external flash):** LittleFS mounted at `/lfs` on an 8 MB `littlefs_storage` partition carved out of `external_flash` by the NCS partition manager. Persistence verified — `/lfs/boot_count` increments across hardware resets. 4096-byte erase blocks, 512-cycle wear threshold. Partition range: `0x4d2000`–`0xcd2000` (8 MB); remainder of external flash (`0xcd2000`–`0x2000000`, ~19 MB) stays free for future use.
- **M4 (session logging):** 256-byte packed binary header (`src/session.h`) stamps the 13 FIRMWARE + 12 PRE-WALK fields from the v1 annotation schema; body is 28-byte packed sample records (uptime_ms + 3×float accel m/s² + 3×float gyro rad/s) from BMI270 at 100 Hz. UUIDv4 from the TF-M PSA RNG. Files land at `/lfs/sessions/<uuid>.dat`. SW0 short-press toggles start/stop. On close, the header is base64-logged to UART so the host ingest script can round-trip without needing USB mass-storage. Every controlled-vocab enum decodes cleanly through `tools/read_session.py`.
- **M5 (USB dump + sampler decoupling):** `src/dump.c` runs a tiny line protocol on nRF9151 uart1 (routed by the nRF5340 connectivity bridge to `/dev/cu.usbmodem105 @ 1 Mbaud`): `PING`, `LIST`, `DUMP <name>`, `DEL <name>`. `tools/pull_sessions.py` drives the host side — round-trips session files to local disk and cross-validates each with `read_session.py`. Under the hood, `src/session.c` was refactored: sampler thread now only does a non-blocking `k_msgq_put`; a dedicated writer thread drains the queue, batches into `fs_write`, and rewrites the header on close. Rate went from ~96 Hz → **100.08 Hz** (1,300 samples / 12.989 s body duration on a fresh bench session). `flash_errors` now means `dropped_samples` (queue-full events); stays 0 when headroom is adequate. `battery_mv_{start,end}` and `session_*_utc_ms` still stamp 0 until M12 / nPM1300 fuel gauge.
- **M6a (transport-agnostic session control):** `src/control.c` parses `START <json>` / `STOP` / `STATUS` commands using Zephyr's JSON library against a schema mirroring `struct gosteady_prewalk`. All 7 controlled-vocab fields are string-validated against the v1 vocabulary; free-form strings are truncated-copied. The parser writes its response into a caller-provided buffer — transport-agnostic by design, so the M6b BLE bridge can tunnel the same commands from a NUS write without the nRF9151 firmware caring. Wired into `src/dump.c` as an additional dispatch target, so the existing uart1 channel now carries both file ops and session control. `tools/control.py` drives it from the host (supports named presets like `concrete-normal-20` so the operator doesn't hand-type JSON 30 times). End-to-end test: a preset-driven `start → sample → stop → pull` cycle produced a session file whose `course_id`, `intended_distance_ft`, and `operator_id` came through exactly as sent (and the other 9 fields all validate against the canonical vocabulary). The M4 button handler is still wired up as a fallback for bench work while the cap is open. Rate held at ~100 Hz through the JSON path.
- **M6b (BLE control over NUS):** `bridge_fw/` is a vendored fork of Nordic's `applications/connectivity_bridge` with four surgical edits (see `bridge_fw/PATCHES.md`) that retarget the Nordic UART Service from uart0 to uart1 — the latter being where our M5/M6a command parser listens. The nRF5340 net core gets built as `ipc_radio` automatically via sysbuild and programmed alongside the app image. BLE is compiled in upstream but **gated off at runtime by default**; the operator enables it by editing `BLE_ENABLED=0` → `1` in `Config.txt` on the bridge's USB mass-storage endpoint and ejecting. `bridge_fw/boards/thingy91x_nrf5340_cpuapp.overlay` pins the bridge's uart1 default to 1 Mbaud so the BLE path's `baudrate=0` ("don't change") peer-conn event doesn't leave a 115200/1Mbaud mismatch against the nRF9151. **Operator-facing UI:** `tools/capture.html` is a single-file Web Bluetooth page with a preset button per row of the v1 30-run capture matrix and a persistent STOP button. As of 2026-04-23 we drive it from **Chrome on the Mac** (not Bluefy on iOS — Bluefy's App Store build doesn't opt into `WKWebView.isInspectable` so Safari Web Inspector can't attach, blocking debug; Chrome gives us full DevTools + Claude-in-Chrome extension integration for end-to-end debugging).
- **M7 (POST-WALK capture loop):** `capture.html` gained a post-STOP modal that fires on every `OK samples=N` notification with three fast-path actions — **✓ Good** (writes `valid=Y` with defaults), **✗ Discard** (writes `valid=N` + `discard_reason` dropdown), and **📝 Notes** (full 7-field form covering the POST-WALK annotation schema). Entries are keyed by `session_uuid` in `localStorage` under `gosteady_post_walk_notes_v1`; an **Export capture notes (JSON)** button downloads a schema-v1 sidecar to `~/Downloads`. On the firmware side, `control.c::cmd_start` now echoes the UUID in its response (`OK started <uuid>`) via a new `gosteady_session_get_uuid_str()` public API in `src/session.h`, so the browser can correlate its notes to the same primary key the session file header carries. `tools/ingest_capture.py` joins pulled `.dat` headers with the notes JSON and emits a CSV matching the `Captures` sheet of `GoSteady_Capture_Annotations_v1.xlsx` column-for-column (13 FIRMWARE + 12 PRE-WALK + 7 POST-WALK + 2 DERIVED), plus two trailing diagnostic columns (`run_idx`, `post_walk_status`) the operator drops before spreadsheet append. End-to-end validation: a 10-run shakedown across the Indoor Polished Concrete segment produced 10/10 clean session files (100.00 Hz, mono=✓, flash_errors=0, UUID-match) and 10/10 POST-WALK notes that joined cleanly at ingest. Stationary baseline (run 9) showed |a|=1.01g / |ω|=0°/s across 37 s of captured data; stumble run (run 10) showed a textbook 4.76g impulse at the event moment — IMU signal quality is solid at the hardware level.

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
│   └── control.h/c                   # Transport-agnostic START/STOP/STATUS JSON parser
├── bridge_fw/                        # ** nRF5340 bridge firmware (vendored fork of Nordic's) **
│   ├── PATCHES.md                    # What we changed and why
│   ├── boards/thingy91x_nrf5340_cpuapp.overlay  # uart1 default 1 Mbaud (load-bearing!)
│   └── src/modules/{ble_handler,uart_handler}.c # dev_idx 0→1 patches
├── tools/
│   ├── capture.html                  # Web BLE operator page + M3 POST-WALK popup (GitHub Pages)
│   ├── control.py                    # USB-uart1 command CLI with run-matrix presets (bench fallback)
│   ├── pull_sessions.py              # USB-uart1 LIST/DUMP session-file puller (noise-tolerant)
│   ├── cleanup_device.py             # Wipe /lfs/sessions before a fresh capture
│   ├── ingest_capture.py             # Join .dat headers + POST-WALK notes JSON → spreadsheet CSV
│   └── read_session.py               # Parse .dat file, validate against v1 vocabulary
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
8. **Dataset collection** — run the capture protocol end-to-end. *(← next — 10/30 runs done as shakedown 2026-04-23 with all quality gates passing; runs 11-30 remaining.)*
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

Milestones 1–7 are done and the 10-run shakedown (runs 1-10, Indoor Polished Concrete) passed every quality gate on 2026-04-23. Picking up:

### 1. Complete the remaining 20 runs

**Outdoor concrete sidewalk (runs 11–20)** and **indoor low-pile carpet (runs 21–30)** per the run matrix in "Data Collection Plan". The shakedown validated the full pipeline end-to-end, so these are "just execute the protocol" — no tooling changes expected.

**Capture-time command sequence** (paste into a shell on capture day):

```bash
cd ~/Documents/gosteady-firmware
STAMP=$(date +%F)
mkdir -p raw_sessions/$STAMP

# Before each capture session: wipe the device so the pull only contains real runs
PY=/opt/nordic/ncs/toolchains/185bb0e3b6/bin/python3
$PY tools/cleanup_device.py --port /dev/cu.usbmodem*1105 --all --yes

# Open https://jabl1629.github.io/gosteady-firmware/tools/capture.html in Chrome,
# click Connect, then walk the protocol row by row. Use the M3 popup at every STOP.

# Post-capture:
$PY tools/pull_sessions.py --port /dev/cu.usbmodem*1105 \
    --out raw_sessions/$STAMP/
mv ~/Downloads/gosteady_capture_notes_$STAMP.json raw_sessions/$STAMP/
$PY tools/ingest_capture.py \
    --sessions raw_sessions/$STAMP/ \
    --notes    raw_sessions/$STAMP/gosteady_capture_notes_$STAMP.json \
    --out      raw_sessions/$STAMP/capture_$STAMP.csv
```

### 2. One open logistics question before outdoor runs

**BLE range for the outdoor 40-ft Run 16.** Mac internal antenna range to the Thingy:91 X is typically fine indoors but near the edge at 40 ft outdoors. Three options in decreasing invasiveness: (a) Mac sits at course midpoint; (b) accept mid-run disconnects (firmware keeps sampling regardless — the browser just loses control until reconnect); (c) range-test Run 11 (warmup, 10 ft) before committing to the full outdoor set. Easiest is (c): if Run 11 pairs and round-trips cleanly, treat (a) as the operator convention and proceed.

### 3. After capture → dataset archive + M9

**Dataset deliverable:** each `raw_sessions/<date>/` becomes the v1 capture archive. Append the `capture_<date>.csv` rows (first 34 columns) to the `Captures` sheet of `GoSteady_Capture_Annotations_v1.xlsx`, commit the raw_sessions archive (currently gitignored — move it to a long-term storage location or publish as a Git LFS / release artifact).

**M9 (Python algorithm):** train/validate the step-based distance estimator from `GoSteady/null_old_fw/` on the fresh 30-run v1 dataset. Expected improvement over the 9-run calibration set: lower MAPE due to tacky→glide cap difference, tighter stride regression from the two-wheel walker's continuous glide signal.

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
- **Stride regression from peak acceleration** — no speed label needed, 12.4% MAPE.
- **No microphone** for surface detection — IMU high-frequency content has the friction signal, mic adds cost/power/board complexity.
- **BMI270 is the session recorder; ADXL367 is the power switch** — the 7-column `.dat` format the Python algorithm expects requires accel + gyro, which only BMI270 provides. ADXL367 stays online as a sanity print in M2+ and will be used for wake-on-motion in the later power-optimization milestone.

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

**Runtime gates we know about:**
- `BLE_ENABLED=1` in `Config.txt` on `/Volumes/NO NAME` — gets wiped by `west flash --recover`, needs re-toggle after.
- **Chrome remembers the BLE pairing per-origin** for the capture.html URL, so reconnects are usually one-click. If Chrome's BLE chooser starts re-prompting, `chrome://settings/content/bluetoothDevices` is where persistent permissions live.
- **USB CDC endpoints re-enumerate on replug** (macOS may drop or add a leading digit: `11105` ↔ `1105`). Re-check `ls /dev/cu.usbmodem*` before any host-side tool invocation after a physical replug.
- *(Legacy — iOS-only, not applicable to Chrome-on-Mac workflow):* "Forget This Device" after every bridge reflash, or iOS occupies the single BLE connection slot.
