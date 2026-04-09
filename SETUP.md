# Dev Environment Setup (macOS)

This document walks a fresh macOS machine all the way from "no toolchain" to "blinky running on the Thingy:91 X and the serial console printing heartbeats." Read it top to bottom the first time — the steps are ordered deliberately and skipping around will bite you.

Target audience: someone who is comfortable in a terminal but has not set up a Zephyr / nRF Connect SDK environment before. Expect the full install to take **1–2 hours** the first time, mostly sitting through downloads.

---

## 0. Hardware and accounts

Before touching any software, confirm you have:

- A Thingy:91 X dev board.
- A USB-C (or USB-A) cable known good for **data**, not just power. Many USB cables are charge-only and will cause baffling "board not detected" errors. If in doubt, try a different cable early.
- A Mac with macOS 13 or later. Apple Silicon (M1/M2/M3/M4) is supported — the Nordic toolchain ships native arm64 binaries now.
- At least **15 GB of free disk space** for the SDK, toolchain, and build artifacts.
- A Nordic DevZone account (free, sign up at [devzone.nordicsemi.com](https://devzone.nordicsemi.com)). Not required for install, but you will want it the first time something breaks and you need to search the forum.

---

## 1. System prerequisites via Homebrew

If you don't already have Homebrew, install it from [brew.sh](https://brew.sh). Then install the base dependencies the Zephyr build system needs:

```bash
brew install cmake ninja gperf python3 ccache qemu dtc wget libmagic
```

Notes:

- `cmake` and `ninja` are the build system.
- `gperf` and `dtc` are needed by Zephyr's devicetree compiler.
- `ccache` makes incremental rebuilds dramatically faster — not optional in practice.
- `python3` must be ≥ 3.10. Check with `python3 --version`.
- `qemu` is used by some Zephyr tests; not strictly required for a physical board, but harmless.

Verify:

```bash
cmake --version    # >= 3.20
ninja --version    # any recent
python3 --version  # >= 3.10
dtc --version      # any recent
```

---

## 2. nRF Connect for Desktop + Toolchain Manager

Nordic ships a GUI installer called **nRF Connect for Desktop** that bundles all their dev tools. The one you specifically need from inside it is the **Toolchain Manager**, which installs and manages nRF Connect SDK versions alongside their matching Zephyr toolchain.

1. Download nRF Connect for Desktop from [nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop](https://www.nordicsemi.com/Products/Development-tools/nrf-connect-for-desktop). Pick the macOS `.dmg`, open it, drag the app into `/Applications`.
2. Open **nRF Connect for Desktop**. On first launch it will ask about telemetry — your call.
3. In the app's "Apps" tab, find **Toolchain Manager** and click **Install**. This is an app inside an app — a bit confusing the first time.
4. Open Toolchain Manager.
5. Click **Install** next to the latest stable nRF Connect SDK version (at time of writing this is the v2.x line — take the newest non-preview). This download is **large** (~3–5 GB) and takes a while. It pulls down NCS, Zephyr, the ARM GCC toolchain, and all the Nordic samples.
6. When done, click the **Open Terminal** button next to the installed version. This opens a shell with `ZEPHYR_BASE`, `PATH`, and all the toolchain environment variables correctly set. **Every `west` command in this repo has to run from a shell that has the NCS environment sourced** — either via this button, or by sourcing the environment script directly (the Toolchain Manager prints the path to it under the "..." menu → "Open env in Terminal").

Sanity check from that terminal:

```bash
west --version      # should print a version number
echo $ZEPHYR_BASE   # should point inside the installed NCS tree
arm-zephyr-eabi-gcc --version  # should print a Zephyr-branded arm GCC
```

If any of those fail, you do not have the NCS environment sourced correctly and nothing downstream will work. Go back to the Toolchain Manager and use its "Open Terminal" button again.

---

## 3. nRF Command Line Tools (J-Link + nrfjprog)

The Toolchain Manager installs the SDK and compiler but **not** the JTAG/SWD flashing utilities. Those come from a separate package called **nRF Command Line Tools**, which bundles SEGGER J-Link plus Nordic's `nrfjprog` and `mergehex` utilities.

1. Download nRF Command Line Tools from [nordicsemi.com/Products/Development-tools/nrf-command-line-tools](https://www.nordicsemi.com/Products/Development-tools/nrf-command-line-tools).
2. Install the `.pkg`. This also installs the SEGGER J-Link drivers.
3. On Apple Silicon you may get a Gatekeeper warning the first time you run a J-Link binary. Go to System Settings → Privacy & Security and allow it. You may need to do this separately for J-Link and nrfjprog.

Sanity check:

```bash
nrfjprog --version
JLinkExe -help 2>&1 | head -3
```

---

## 4. VS Code + nRF Connect Extension Pack

You *can* drive the whole toolchain from the command line, but the Nordic VS Code extension is significantly friendlier for build/flash/debug cycles and is the path Nordic officially supports.

1. Install VS Code from [code.visualstudio.com](https://code.visualstudio.com) if you don't already have it.
2. Open VS Code → Extensions → search for **nRF Connect Extension Pack** (published by Nordic Semiconductor) → Install. This is a meta-extension that pulls in several individual extensions — let it install all of them.
3. Open the **nRF Connect** tab in the VS Code sidebar (it appears after the extension installs).
4. It will ask you to point at your nRF Connect SDK and toolchain. If the Toolchain Manager installed NCS in its default location the extension should auto-detect it — if not, manually point it at the install path the Toolchain Manager shows you.

---

## 5. Clone this repo

From the nRF-environment-sourced terminal (see Step 2):

```bash
cd ~/src                  # or wherever you keep code
git clone <remote-url> gosteady-firmware
cd gosteady-firmware
```

If there is no remote yet, just copy the local `gosteady-firmware/` directory from the GoSteady workspace folder.

---

## 6. First build (the actual test)

Still from the NCS-environment terminal, inside the `gosteady-firmware` directory:

```bash
west build -b thingy91x/nrf9151/ns -p auto
```

The `-b thingy91x/nrf9151/ns` flag targets the non-secure image on the nRF9151 application core. The `-p auto` flag tells west to do a pristine rebuild if the board or config changed.

**What to expect:**

- First build takes several minutes while CMake configures Zephyr and the compiler builds ~everything. Subsequent builds are seconds thanks to `ccache`.
- You will see a lot of warnings scroll by. Most are from Zephyr / NCS internal modules, not our code. Ignore anything that doesn't come from `src/main.c`.
- Success looks like:

  ```
  [XXX/XXX] Linking C executable zephyr/zephyr.elf
  Memory region         Used Size  Region Size  %age Used
             FLASH:       34.0 KB       256 KB     13.28%
             SRAM:        12.1 KB       128 KB      9.45%
  ```

  Exact numbers will differ; what matters is that it says `Linking C executable zephyr/zephyr.elf` at the end, not an error.

**If the build fails:**

- `error: Please set ZEPHYR_BASE` → your shell does not have the NCS environment sourced. Re-open the terminal via the Toolchain Manager.
- `error: board 'thingy91x/nrf9151/ns' not found` → your NCS version is too old to know about the Thingy:91 X. Update to the latest stable in the Toolchain Manager.
- `CMake Error at ... Boards for the 'thingy91x' SoC ...` → try `west update` from inside the NCS install directory to pull missing board files. If that doesn't work, reinstall NCS via the Toolchain Manager.

---

## 7. Connect the board

1. Plug the Thingy:91 X into a USB port on your Mac with a data-capable cable.
2. Power it on (slide switch on the board).
3. The onboard debug probe should enumerate. Check:

   ```bash
   nrfjprog --ids
   ```

   This should print one serial number. If it prints nothing, the board is not enumerating — usually a cable or power issue. Try a different cable, try the other USB port, make sure the power switch is on.

4. Check the serial console device is present:

   ```bash
   ls /dev/tty.usbmodem*
   ```

   You should see at least one entry. This is the UART console that the heartbeat will print on.

---

## 8. Flash it

Still inside `gosteady-firmware`:

```bash
west flash
```

What to expect:

- `nrfjprog` programs the application core, resets, and exits cleanly.
- The board briefly flashes its LEDs during reset, then settles.
- **An LED starts toggling at 1 Hz.** This is the success signal.

---

## 9. Watch the console

Open a serial terminal in a second window. Any of these work:

```bash
# screen (built in) — Ctrl-A then K to exit
screen /dev/tty.usbmodem<id> 115200

# or picocom (brew install picocom)
picocom -b 115200 /dev/tty.usbmodem<id>

# or the VS Code "Serial Monitor" extension, which is graphical
```

(The `<id>` is whatever came back from the `ls` in step 7. Tab-completion is your friend.)

You should see a line like:

```
*** Booting nRF Connect SDK v2.x.x ***
[00:00:00.xxx,xxx] <inf> gosteady: GoSteady firmware starting (build ...)
[00:00:00.xxx,xxx] <inf> gosteady: Bring-up complete. Entering blink loop.
[00:00:00.xxx,xxx] <inf> gosteady: heartbeat tick=0
[00:00:01.xxx,xxx] <inf> gosteady: heartbeat tick=1
[00:00:02.xxx,xxx] <inf> gosteady: heartbeat tick=2
...
```

One heartbeat line per second. The LED toggles in sync.

**Congratulations — dev environment is live.** You have now proved, in order:

1. The toolchain is installed correctly.
2. The SDK knows how to target the Thingy:91 X.
3. The build system can compile your code.
4. The flashing path to the board works.
5. The serial console path back from the board works.

This is the foundation every subsequent firmware milestone builds on.

---

## 10. If any of that went wrong

Don't try to power through it. The debugging surface only grows as you add more moving parts, so getting every step of this clean before writing any real code is genuinely worth the time.

Common failure modes and where to look:

| Symptom | Likely cause | Fix |
|---|---|---|
| `west` not found | NCS environment not sourced | Use the Toolchain Manager's "Open Terminal" |
| `nrfjprog --ids` empty | Bad cable or unpowered board | Swap cable, confirm power switch |
| Build fails with no error you understand | Stale build dir | `rm -rf build && west build -b thingy91x/nrf9151/ns -p always` |
| LED blinks but console is silent | Serial port is a different tty | `ls /dev/tty.*` and try each `tty.usbmodem*` |
| Console prints garbage | Wrong baud rate | Make sure your terminal is at 115200 |
| Board enumerates but `west flash` fails | J-Link driver / Gatekeeper block | Check System Settings → Privacy & Security for blocked Nordic binaries |

For anything stranger, the Nordic DevZone ([devzone.nordicsemi.com](https://devzone.nordicsemi.com)) is the first place to search — almost every obscure error you will hit has been hit by someone before.

---

## What's next

Step 3 of the firmware arc: **sensor bring-up**. The goal is to read the BMI270 IMU over SPI and print raw accel / gyro values to the same console. That is the point at which `main.c` starts to become a real file instead of a toolchain smoke test.
