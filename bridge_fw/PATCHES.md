# bridge_fw — fork of Nordic's `applications/connectivity_bridge`

This directory is a **verbatim copy** of `applications/connectivity_bridge`
from nRF Connect SDK v3.2.4, with three source-code patches and one DTS
override. The purpose is to route the Nordic UART Service (NUS) on the
nRF5340's BLE radio to **uart1** — which the nRF9151 side runs our M5/M6a
command parser on — instead of the upstream default of uart0 (which is
the nRF9151 app console and has no command listener).

Upstream path: `/opt/nordic/ncs/v3.2.4/nrf/applications/connectivity_bridge/`.
Re-fork from a newer NCS version by diffing against the three files
listed below and re-applying the same changes.

## Why we vendor the whole thing

The application is a sysbuild project that pulls in an `ipc_radio`
net-core image, MCUboot, and several event-driven modules. Zephyr has no
clean way to override a single application source file, so copying the
whole tree in is the least brittle option.

## Patches

### 1) `src/modules/ble_handler.c` — three 0 → 1 changes

Three references to `dev_idx = 0` or `dev_idx != 0` are flipped to 1:

- The `peer_conn_event` emitted on BLE **connect** (around the original
  line 107) — its `dev_idx` is used by `uart_handler` to track which
  UART has an active peer (subscriber-count, baud-rate updates).
- The `peer_conn_event` emitted on BLE **disconnect** (around line 129)
  — symmetrical to the above.
- The upstream `uart_data_event` **filter** (around line 350) that
  gates which UART's RX bytes get forwarded to NUS TX. Upstream accepts
  only `dev_idx == 0` (uart0 → BLE); we accept only `dev_idx == 1`
  (uart1 → BLE).

Each patch site carries an inline comment beginning with "GoSteady M6b".

### 2) `src/modules/uart_handler.c` — one 0 → 1 change

The `is_ble_data_event` branch of `app_event_handler` routes inbound
NUS RX bytes to `uart_tx_enqueue(..., dev_idx)`. Upstream uses
`dev_idx = 0` (NUS → uart0). We use `dev_idx = 1` (NUS → uart1).

### 3) `boards/thingy91x_nrf5340_cpuapp.overlay` — uart1 default baud

Added `current-speed = <1000000>;` to the `&uart1 {}` node. The nRF9151
side runs uart1 at 1 Mbaud. USB CDC opens already reconfigure the baud
dynamically via `SET_LINE_CODING`, but the BLE peer-conn event passes
`baudrate = 0` ("don't change"), so without a 1 Mbaud default the BLE
path would leave uart1 at the upstream default of 115200 and mismatch
the nRF9151 side.

## Build

From the top of the repo, with the NCS env sourced:

```bash
west build -b thingy91x/nrf5340/cpuapp -d build_bridge bridge_fw
```

## Flash

**SW2 must be in the nRF53 position** before `west flash`. First build
after a factory rescue image usually trips APPROTECT — use
`west flash --recover` to wipe both cores and reflash. After flashing,
flip SW2 back to nRF91 for any future nRF9151 reflashes.

## Enable BLE at runtime

The bridge ships BLE disabled by default. Each time the nRF5340 settings
flash is wiped (e.g., from `--recover`), re-enable BLE via the mass-
storage `Config.txt`:

1. Plug in USB. A `NO NAME` volume mounts on macOS.
2. Edit `Config.txt`: `BLE_ENABLED=0` → `BLE_ENABLED=1`. Save.
3. Eject the drive in Finder (this triggers the settings write).
4. Unplug + replug USB.

The device should now advertise as "Thingy:91 X UART" (BT device name
is itself editable via the same file's `BLE_NAME` key).
