#!/usr/bin/env python3
"""
Delete session .dat files from a GoSteady device's LittleFS over uart1.

Typical use: run immediately before a fresh capture session so the
post-capture pull contains only the real data (and not a trail of
debug / bench sessions).

Usage:
    python tools/cleanup_device.py                  # dry-run (list, don't delete)
    python tools/cleanup_device.py --all            # delete every .dat (prompts)
    python tools/cleanup_device.py --all --yes      # delete without prompting
    python tools/cleanup_device.py --only <name>    # delete just one file

Reuses the noise-tolerant protocol plumbing from tools/pull_sessions.py,
so it's safe to run while capture.html is still connected over BLE and
its 5 s STATUS poll is bleeding onto uart1 TX.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

# Reuse pull_sessions.py's transport layer.
sys.path.insert(0, str(Path(__file__).parent))
from pull_sessions import connect, list_files, delete_file, DEFAULT_PORT, DEFAULT_BAUD  # noqa: E402


def confirm(prompt: str) -> bool:
    try:
        ans = input(f"{prompt} [y/N]: ").strip().lower()
    except EOFError:
        return False
    return ans in ("y", "yes")


def main() -> int:
    p = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    p.add_argument("--port", default=DEFAULT_PORT)
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument("--all", action="store_true",
                   help="delete every .dat file on the device")
    p.add_argument("--only", help="delete just this one file (full <uuid>.dat name)")
    p.add_argument("--yes", action="store_true", help="skip the confirmation prompt")
    args = p.parse_args()

    if args.all and args.only:
        print("error: --all and --only are mutually exclusive", file=sys.stderr)
        return 1

    try:
        ser = connect(args.port, args.baud)
    except Exception as e:
        print(f"connect failed: {e}", file=sys.stderr)
        return 1

    try:
        files = list_files(ser)
    except Exception as e:
        print(f"LIST failed: {e}", file=sys.stderr)
        return 1

    print(f"device has {len(files)} session file(s):")
    total_bytes = 0
    for name, size in files:
        total_bytes += size
        print(f"  {name}  {size:>8} bytes")
    if files:
        print(f"  --- total: {total_bytes:,} bytes ({total_bytes/1024:.1f} KB)")

    if args.only:
        todo = [(n, s) for (n, s) in files if n == args.only]
        if not todo:
            print(f"\nerror: {args.only!r} not on device", file=sys.stderr)
            return 1
    elif args.all:
        todo = list(files)
    else:
        # Dry-run: no flags → just list and exit.
        print("\nno action taken — pass --all or --only <name> to delete")
        return 0

    if not todo:
        print("\nnothing to delete")
        return 0

    print(f"\nabout to DELETE {len(todo)} file(s). This is permanent.")
    if not args.yes and not confirm("Continue?"):
        print("aborted")
        return 0

    deleted = failed = 0
    for name, _ in todo:
        try:
            delete_file(ser, name)
            print(f"  deleted {name}")
            deleted += 1
        except Exception as e:
            print(f"  {name}: {e}", file=sys.stderr)
            failed += 1

    print(f"\ndone: {deleted} deleted, {failed} failed")

    # Post-cleanup re-list so the operator can confirm at a glance.
    try:
        remaining = list_files(ser)
        print(f"device now has {len(remaining)} file(s) remaining.")
        for name, size in remaining:
            print(f"  {name}  {size} bytes")
    except Exception as e:
        print(f"(post-cleanup LIST failed: {e})", file=sys.stderr)

    return 0 if failed == 0 else 2


if __name__ == "__main__":
    sys.exit(main())
