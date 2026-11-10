#!/usr/bin/env python3
"""Collect PlatformIO ESP32 binaries and produce the web-installer manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BUILD = ROOT / ".pio" / "build" / "esp32dev"

FILES = {
    "bootloader.bin": (0x1000, [BUILD / "bootloader.bin"]),
    "partitions.bin": (0x8000, [BUILD / "partitions.bin"]),
    "boot_app0.bin": (
        0xE000,
        [
            BUILD / "boot_app0.bin",
            Path.home() / ".platformio/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin",
        ],
    ),
    "firmware.bin": (0x10000, [BUILD / "firmware.bin"]),
}


def find_file(candidates: list[Path], name: str) -> Path:
    for candidate in candidates:
        if candidate.is_file():
            return candidate
    if name == "boot_app0.bin":
        packages = Path.home() / ".platformio" / "packages"
        if packages.exists():
            matches = list(
                packages.glob("framework-arduinoespressif32*/tools/partitions/boot_app0.bin")
            )
            if matches:
                return matches[0]
    raise FileNotFoundError(
        f"Missing {name}. Checked: " + ", ".join(str(path) for path in candidates)
    )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, default=ROOT / "dist")
    args = parser.parse_args()

    if not BUILD.is_dir():
        print(f"Build directory not found: {BUILD}", file=sys.stderr)
        return 2

    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)

    hashes: list[str] = []
    parts: list[dict[str, object]] = []
    for filename, (offset, candidates) in FILES.items():
        source = find_file(candidates, filename)
        target = output / filename
        shutil.copy2(source, target)
        digest = hashlib.sha256(target.read_bytes()).hexdigest()
        hashes.append(f"{digest}  {filename}")
        parts.append({"path": filename, "offset": offset})
        print(f"Packaged {filename} ({target.stat().st_size} bytes) at 0x{offset:X}")

    manifest = {
        "name": "Teams Presence",
        "version": args.version,
        "builds": [{"chipFamily": "ESP32", "parts": parts}],
    }
    (output / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8"
    )
    (output / "SHA256SUMS").write_text("\n".join(hashes) + "\n", encoding="utf-8")
    print(f"Firmware package created in {output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
