#!/usr/bin/env python3
"""
Flash an AWS IoT cert + key + root CA bundle into the nRF9151 modem secure store.

Thin convenience wrapper around Nordic's `nrfcredstore` CLI (ships in the NCS
toolchain bin). nrfcredstore handles all the AT%CMNG quoting, PEM parsing, and
modem offline-mode dance internally; this script just resolves the cert-bundle
layout, globs the USB CDC AT port, drives the three writes at one sec_tag, and
verifies via list.

Prerequisite — bench unit must be running the NCS at_client sample
(/opt/nordic/ncs/v3.2.4/nrf/samples/cellular/at_client/) flashed to the nRF9151
with SW2 in the nRF91 position. This temporarily overwrites gosteady firmware
with an AT-shell that exposes the modem's AT interface over uart0 → bridge →
/dev/cu.usbmodem*102 @ 115200. Reflash gosteady with `west flash` from this
repo afterwards (cert survives because it lives in the nRF9151's CryptoCell-312
secure store, not in firmware flash).

Bundle layout (per cloud team's 2026-04-27 handoff):

    <bundle>/AmazonRootCA1.pem            # AWS IoT server-cert chain anchor
    <bundle>/<serial>/                    # one subdir per serial
        <serial>.cert.pem                 # public cert
        <serial>.private.key              # private key (mode 0600)
        <serial>.public.key               # reference only — NOT flashed
        <serial>.README.txt
    <bundle>/MANIFEST.csv                 # serial → fingerprint mapping

Usage:

    # Default — flash bench cert GS9999999999 from default bundle path
    tools/flash_cert.py

    # Specific serial from the same bundle
    tools/flash_cert.py --serial GS0000000001

    # Custom bundle / port / sec_tag
    tools/flash_cert.py \\
      --bundle  ~/Desktop/gosteady-firmware-cert-handoff-2026-04-27 \\
      --serial  GS9999999999 \\
      --port    /dev/cu.usbmodem1102 \\
      --sec-tag 201

    # Wipe-and-rewrite (delete any existing entries at sec_tag first)
    tools/flash_cert.py --erase-first

Why sec_tag 201:
    Matches NCS aws_iot sample default (CONFIG_MQTT_HELPER_SEC_TAG=201).
    gosteady firmware will look up certs at this sec_tag via the aws_iot lib.
"""

from __future__ import annotations

import argparse
import glob
import os
import subprocess
import sys
from pathlib import Path

# Defaults match cloud team's 2026-04-27 single-dev cert handoff.
DEFAULT_BUNDLE = Path.home() / "Desktop" / "gosteady-firmware-cert-handoff-2026-04-27"
DEFAULT_SERIAL = "GS9999999999"  # bench cert; reusable forever
DEFAULT_PORT_GLOB = "/dev/cu.usbmodem*102"  # uart0 (AT-host UART for at_client)
DEFAULT_SEC_TAG = 201  # matches NCS aws_iot sample default

# nrfcredstore lives in the NCS toolchain bin. Fall back to PATH if env doesn't
# point at the toolchain (e.g. plain shell without the NCS env sourced).
_NCS_TOOLCHAIN_BIN = "/opt/nordic/ncs/toolchains/185bb0e3b6/bin"
NRFCREDSTORE = (
    f"{_NCS_TOOLCHAIN_BIN}/nrfcredstore"
    if os.path.exists(f"{_NCS_TOOLCHAIN_BIN}/nrfcredstore")
    else "nrfcredstore"
)


def resolve_port(port_arg: str | None) -> str:
    """Resolve the USB CDC AT port. macOS digit-shifts on replug — glob to be safe."""
    if port_arg and not any(c in port_arg for c in "*?["):
        return port_arg  # explicit path, use as-is
    pattern = port_arg or DEFAULT_PORT_GLOB
    matches = sorted(glob.glob(pattern))
    if not matches:
        sys.exit(f"error: no USB CDC port matched {pattern!r} — is the bench unit plugged in?")
    if len(matches) > 1:
        sys.exit(f"error: multiple ports matched {pattern!r}: {matches} — pass --port explicitly")
    return matches[0]


def resolve_files(bundle: Path, serial: str) -> tuple[Path, Path, Path]:
    """Resolve (root_ca, client_cert, client_key) paths from the bundle layout."""
    root_ca = bundle / "AmazonRootCA1.pem"
    cert_dir = bundle / serial
    client_cert = cert_dir / f"{serial}.cert.pem"
    client_key = cert_dir / f"{serial}.private.key"
    for p, label in [(root_ca, "Root CA"), (client_cert, "client cert"), (client_key, "client key")]:
        if not p.is_file():
            sys.exit(f"error: {label} missing at {p}")
    return root_ca, client_cert, client_key


def run_credstore(port: str, args: list[str], *, capture: bool = False) -> str:
    """Invoke nrfcredstore with shared port + cmd-type=at flags."""
    cmd = [NRFCREDSTORE, "--cmd-type", "at", port, *args]
    print(f"  $ {' '.join(cmd)}")
    if capture:
        result = subprocess.run(cmd, capture_output=True, text=True, check=False)
        sys.stdout.write(result.stdout)
        sys.stderr.write(result.stderr)
        if result.returncode != 0:
            sys.exit(f"error: nrfcredstore exited {result.returncode}")
        return result.stdout
    result = subprocess.run(cmd, check=False)
    if result.returncode != 0:
        sys.exit(f"error: nrfcredstore exited {result.returncode}")
    return ""


def main() -> None:
    parser = argparse.ArgumentParser(
        description="Flash an AWS IoT cert bundle to the nRF9151 modem secure store.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See module docstring for prerequisites + bundle layout.",
    )
    parser.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE,
                        help=f"path to cert handoff bundle (default: {DEFAULT_BUNDLE})")
    parser.add_argument("--serial", default=DEFAULT_SERIAL,
                        help=f"serial whose subdir holds the cert + key (default: {DEFAULT_SERIAL})")
    parser.add_argument("--port", default=None,
                        help=f"USB CDC AT port (default: glob {DEFAULT_PORT_GLOB})")
    parser.add_argument("--sec-tag", type=int, default=DEFAULT_SEC_TAG,
                        help=f"modem sec_tag slot (default: {DEFAULT_SEC_TAG})")
    parser.add_argument("--erase-first", action="store_true",
                        help="delete any existing entries at sec_tag before writing")
    parser.add_argument("--dry-run", action="store_true",
                        help="resolve paths + print plan, don't invoke nrfcredstore")
    args = parser.parse_args()

    port = resolve_port(args.port)
    root_ca, client_cert, client_key = resolve_files(args.bundle, args.serial)

    print("Plan:")
    print(f"  port      : {port}")
    print(f"  serial    : {args.serial}")
    print(f"  sec_tag   : {args.sec_tag}")
    print(f"  root CA   : {root_ca}")
    print(f"  cert      : {client_cert}")
    print(f"  key       : {client_key}")
    print(f"  erase 1st : {args.erase_first}")
    print()

    if args.dry_run:
        print("dry-run — exiting without invoking nrfcredstore")
        return

    if args.erase_first:
        print(f"Step 0/4: deleteall at sec_tag {args.sec_tag}")
        run_credstore(port, ["deleteall", str(args.sec_tag)])
        print()

    print(f"Step 1/4: write Root CA at sec_tag {args.sec_tag}")
    run_credstore(port, ["write", str(args.sec_tag), "ROOT_CA_CERT", str(root_ca)])
    print()

    print(f"Step 2/4: write client cert at sec_tag {args.sec_tag}")
    run_credstore(port, ["write", str(args.sec_tag), "CLIENT_CERT", str(client_cert)])
    print()

    print(f"Step 3/4: write client private key at sec_tag {args.sec_tag}")
    run_credstore(port, ["write", str(args.sec_tag), "CLIENT_KEY", str(client_key)])
    print()

    print(f"Step 4/4: list contents of sec_tag {args.sec_tag} for verification")
    listing = run_credstore(port, ["list", "--tag", str(args.sec_tag)], capture=True)

    # Sanity check — expect entries for all 3 types.
    expected_types = {"ROOT_CA_CERT", "CLIENT_CERT", "CLIENT_KEY"}
    missing = {t for t in expected_types if t not in listing}
    if missing:
        sys.exit(f"error: nrfcredstore list missing types {missing} after write")
    print()
    print(f"OK — sec_tag {args.sec_tag} on serial {args.serial} provisioned via {port}")
    print("Next: reflash gosteady firmware (`west flash` from repo root) and the cert")
    print("survives in CryptoCell-312 secure store across the firmware swap.")


if __name__ == "__main__":
    main()
