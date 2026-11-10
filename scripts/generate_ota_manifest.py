#!/usr/bin/env python3
"""Create the small OTA manifest published at the GitHub Pages root."""

from __future__ import annotations

import argparse
import hashlib
import json
import re
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--base-url", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()

    if not re.fullmatch(r"[A-Za-z0-9._-]+", args.version):
        parser.error("version contains unsupported characters")
    if not args.base_url.startswith("https://gianmarco112.github.io/esp32-teams-presence/firmware/"):
        parser.error("base URL must point to this project's HTTPS GitHub Pages firmware directory")
    if not args.firmware.is_file():
        parser.error(f"firmware binary not found: {args.firmware}")

    data = args.firmware.read_bytes()
    if not data:
        parser.error("firmware binary is empty")

    manifest = {
        "name": "Teams Presence",
        "version": args.version,
        "channel": "prerelease" if "-" in args.version else "stable",
        "firmware_url": args.base_url,
        "size": len(data),
        "sha256": hashlib.sha256(data).hexdigest(),
    }
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    print(f"OTA manifest: {args.output} -> {args.version} ({len(data)} bytes)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
