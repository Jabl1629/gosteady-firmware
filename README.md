# gosteady-firmware

Firmware for the GoSteady smart walker cap.

Target hardware: Nordic **Thingy:91 X** (nRF9151 cellular MCU + nRF5340 BLE host + nRF7002 Wi-Fi + BMI270 IMU + ADXL367 low-power accel). This repository builds for the `thingy91x/nrf9151/ns` target — the non-secure image running on the nRF9151 application core.

## Status

Pre-v0. Currently a blinky bring-up target that proves the toolchain, board connection, and flash path end-to-end. Real application code lands as we walk through the firmware arc:

1. **Dev environment setup** — blinky on the board. *(← you are here)*
2. Sensor bring-up — BMI270 / ADXL367 reads over SPI / I²C.
3. External flash — LittleFS on the 64 MB QSPI.
4. Session logging — binary session files on flash with versioned header.
5. USB dump — mass storage / CDC-ACM path for host-side extraction.
6. BLE control — start/stop session commands over GATT (NUS).
7. Python CLI — host tool for session control and data dump.
8. Dataset collection — run the capture protocol end-to-end.
9. Python algorithm — distance estimator trained on the dataset.
10. C port — move the estimator on-device.
11. Validation — hold-out error characterization.
12. Cellular — LTE-M / NB-IoT link up on nRF9151.
13. Cloud backend — session telemetry upload.
14. Production telemetry — battery, errors, OTA hooks.
15. Field testing.

## Build & flash

See [`SETUP.md`](./SETUP.md) for the full macOS development environment install. Once that is done, the short version is:

```bash
# From the gosteady-firmware directory, with the nRF Connect SDK
# environment sourced:
west build -b thingy91x/nrf9151/ns -p auto
west flash
```

Success looks like: an LED on the Thingy:91 X toggles at 1 Hz, and `heartbeat tick=N` lines appear on the serial console once per second.

## Layout

```
gosteady-firmware/
├── CMakeLists.txt      # Zephyr app build definition
├── prj.conf            # Kconfig for the baseline build
├── src/
│   └── main.c          # Current bring-up target (blinky + heartbeat)
├── README.md
├── SETUP.md            # macOS dev environment setup
└── .gitignore
```

As the firmware arc progresses, `src/` will gain modules (`sensors/`, `storage/`, `ble/`, `session/`, `cloud/`) and the build will be split across multiple source files and Kconfig options.

## Related

- `GoSteady_Capture_Protocol_v1.docx` — 30-run data capture protocol this firmware has to support.
- `GoSteady_Capture_Annotations_v1.xlsx` — annotation schema used by the capture protocol.

Both live in the parent `GoSteady/` workspace folder.
