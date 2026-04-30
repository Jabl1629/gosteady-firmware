#!/usr/bin/env python3
"""
Flash an AWS IoT cert + key + root CA bundle into the nRF9151 modem secure store.

Talks to the modem AT host directly via uart0 → bridge → /dev/cu.usbmodem*102.
Originally a thin wrapper around nrfcredstore but the canonical Nordic CLI
opens the CDC port without asserting DTR — and our bridge firmware buffers
RX bytes until DTR is high, so nrfcredstore times out on every command.
Doing the AT%CMNG dance ourselves in pyserial (with explicit DTR=True) is
~80 LOC and avoids that incompatibility.

Prerequisite — bench unit must be running the NCS at_client sample
(/opt/nordic/ncs/v3.2.4/nrf/samples/cellular/at_client/) flashed to the nRF9151
with SW2 in the nRF91 position. This temporarily overwrites gosteady firmware
with an AT-shell that exposes the modem's AT interface over uart0 → bridge →
/dev/cu.usbmodem*102 @ 115200. Reflash gosteady with `west flash` after; cert
survives in CryptoCell-312 secure store across the firmware swap.

Bundle layout (per cloud team's 2026-04-27 handoff):

    <bundle>/AmazonRootCA1.pem
    <bundle>/<serial>/
        <serial>.cert.pem
        <serial>.private.key
        <serial>.public.key       — reference only, NOT flashed
        <serial>.README.txt
    <bundle>/MANIFEST.csv

Usage:

    tools/flash_cert.py                                 # bench cert (default)
    tools/flash_cert.py --serial GS0000000001           # specific shipping cert
    tools/flash_cert.py --erase-first                   # wipe slot before write

AT%CMNG slots used at sec_tag:
    type 0  ROOT_CA_CERT  Amazon Root CA 1
    type 1  CLIENT_CERT   <serial>.cert.pem
    type 2  CLIENT_KEY    <serial>.private.key

Why sec_tag 201:
    Matches NCS aws_iot sample default (CONFIG_MQTT_HELPER_SEC_TAG=201).
    gosteady firmware looks up certs at this same sec_tag via the aws_iot lib.
"""

from __future__ import annotations

import argparse
import glob
import sys
import time
from pathlib import Path

import serial

DEFAULT_BUNDLE = Path.home() / "Desktop" / "gosteady-firmware-cert-handoff-2026-04-27"
DEFAULT_SERIAL = "GS9999999999"
DEFAULT_PORT_GLOB = "/dev/cu.usbmodem*102"  # uart0 — at_client AT host
DEFAULT_SEC_TAG = 201

CRED_TYPES = {
    0: "ROOT_CA_CERT",
    1: "CLIENT_CERT",
    2: "CLIENT_KEY",
}


def resolve_port(port_arg: str | None) -> str:
    if port_arg and not any(c in port_arg for c in "*?["):
        return port_arg
    pattern = port_arg or DEFAULT_PORT_GLOB
    matches = sorted(glob.glob(pattern))
    if not matches:
        sys.exit(f"error: no USB CDC port matched {pattern!r} — bench unit plugged in?")
    if len(matches) > 1:
        sys.exit(f"error: multiple ports matched {pattern!r}: {matches} — pass --port explicitly")
    return matches[0]


def resolve_files(bundle: Path, serial: str) -> tuple[Path, Path, Path]:
    root_ca = bundle / "AmazonRootCA1.pem"
    cert_dir = bundle / serial
    client_cert = cert_dir / f"{serial}.cert.pem"
    client_key = cert_dir / f"{serial}.private.key"
    for p, label in [(root_ca, "Root CA"),
                     (client_cert, "client cert"),
                     (client_key, "client key")]:
        if not p.is_file():
            sys.exit(f"error: {label} missing at {p}")
    return root_ca, client_cert, client_key


class ATShell:
    """
    Minimal AT-host driver over a CDC serial port (works against the NCS
    at_client sample). Sets DTR=True on open to defeat the bridge's RX
    buffering, sends commands as `<cmd>\\r\\n`, reads lines until OK or
    ERROR or +CME ERROR.
    """

    def __init__(self, port: str, timeout: float = 5.0):
        self.s = serial.Serial(port, 115200, timeout=timeout)
        self.s.dtr = True
        time.sleep(0.5)
        self.s.reset_input_buffer()
        # Wake the line — at_client sometimes ignores the very first byte.
        self.s.write(b"\r\n")
        self.s.flush()
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def cmd(self, at: str, *, wait: float = 5.0) -> tuple[bool, list[str]]:
        """Send an AT command. Return (ok, [response lines excluding OK/ERROR])."""
        self.s.write(at.encode("utf-8") + b"\r\n")
        self.s.flush()
        deadline = time.time() + wait
        body: list[str] = []
        while time.time() < deadline:
            line = self.s.readline()
            if not line:
                continue
            line = line.decode("utf-8", errors="replace").strip("\r\n").rstrip()
            if not line:
                continue
            if line == "OK":
                return True, body
            if line == "ERROR" or line.startswith("+CME ERROR") or line.startswith("+CMS ERROR"):
                body.append(line)
                return False, body
            body.append(line)
        return False, body + ["TIMEOUT"]

    def close(self) -> None:
        self.s.close()


def write_cred(at: ATShell, sec_tag: int, cred_type: int, pem_path: Path) -> None:
    pem = pem_path.read_text()
    cmd = f'AT%CMNG=0,{sec_tag},{cred_type},"{pem}"'
    print(f"  AT%CMNG=0,{sec_tag},{cred_type} ({CRED_TYPES[cred_type]}, {len(pem)} B from {pem_path.name})")
    ok, body = at.cmd(cmd, wait=10.0)
    if not ok:
        sys.exit(f"  ERROR: {body}")


def list_creds(at: ATShell, sec_tag: int) -> dict[int, str]:
    """Return {cred_type: sha256_hex} for everything stored at sec_tag."""
    ok, body = at.cmd(f"AT%CMNG=1,{sec_tag}", wait=5.0)
    if not ok:
        sys.exit(f"  list error: {body}")
    out: dict[int, str] = {}
    for line in body:
        if line.startswith("%CMNG:"):
            parts = line.removeprefix("%CMNG:").strip().split(",")
            if len(parts) >= 3:
                ct = int(parts[1])
                sha = parts[2].strip().strip('"')
                out[ct] = sha
    return out


def main() -> None:
    p = argparse.ArgumentParser(
        description="Flash an AWS IoT cert bundle to the nRF9151 modem secure store.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="See module docstring for prerequisites + bundle layout.",
    )
    p.add_argument("--bundle", type=Path, default=DEFAULT_BUNDLE)
    p.add_argument("--serial", default=DEFAULT_SERIAL)
    p.add_argument("--port", default=None)
    p.add_argument("--sec-tag", type=int, default=DEFAULT_SEC_TAG)
    p.add_argument("--erase-first", action="store_true")
    p.add_argument("--dry-run", action="store_true")
    args = p.parse_args()

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
        print("dry-run — exiting without touching modem")
        return

    at = ATShell(port)
    try:
        print("Open AT shell:")
        ok, body = at.cmd("AT", wait=2.0)
        if not ok:
            sys.exit(f"  AT failed: {body}")
        print(f"  AT OK")

        # CMNG requires the modem to be in offline mode (CFUN=4 or CFUN=0).
        ok, body = at.cmd("AT+CMEE=1", wait=2.0)
        ok, body = at.cmd("AT+CFUN=4", wait=10.0)
        if not ok:
            sys.exit(f"  AT+CFUN=4 failed: {body}")
        print("  modem offline (CFUN=4)")

        if args.erase_first:
            print(f"\nDelete existing entries at sec_tag {args.sec_tag}:")
            for ct in (0, 1, 2):
                at.cmd(f"AT%CMNG=3,{args.sec_tag},{ct}", wait=5.0)  # ignore not-found
            print("  done")

        print(f"\nWrite credentials at sec_tag {args.sec_tag}:")
        write_cred(at, args.sec_tag, 0, root_ca)
        write_cred(at, args.sec_tag, 1, client_cert)
        write_cred(at, args.sec_tag, 2, client_key)

        print(f"\nVerify via AT%CMNG=1,{args.sec_tag}:")
        stored = list_creds(at, args.sec_tag)
        missing = {ct for ct in (0, 1, 2) if ct not in stored}
        if missing:
            sys.exit(f"  missing types after write: {sorted(missing)}")
        for ct in (0, 1, 2):
            print(f"  type {ct} ({CRED_TYPES[ct]}): sha256={stored[ct]}")

        print(f"\nOK — sec_tag {args.sec_tag} on serial {args.serial} provisioned via {port}")
        print("Next: reflash gosteady firmware with cloud build:")
        print("  west build -d build_cloud -b thingy91x/nrf9151/ns -- -DEXTRA_CONF_FILE=prj_cloud.conf")
        print("  west flash --build-dir build_cloud --erase")
    finally:
        at.close()


if __name__ == "__main__":
    main()
