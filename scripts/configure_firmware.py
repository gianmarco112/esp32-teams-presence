#!/usr/bin/env python3
"""Generate include/config.h from GitHub Actions repository variables."""

from __future__ import annotations

import os
import re
import sys
import uuid
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TEMPLATE = ROOT / "include" / "config.h.example"
TARGET = ROOT / "include" / "config.h"
UUID_RE = re.compile(
    r"^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-8][0-9a-fA-F]{3}-[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}$"
)


def validate_uuid(name: str, value: str) -> None:
    if not UUID_RE.fullmatch(value):
        raise ValueError(f"{name} must be a Microsoft Entra GUID.")
    uuid.UUID(value)


def main() -> int:
    tenant_id = os.getenv("MS_TENANT_ID", "").strip()
    client_id = os.getenv("MS_CLIENT_ID", "").strip()
    require = os.getenv("REQUIRE_ENTRA_CONFIG", "false").lower() == "true"

    if require and (not tenant_id or not client_id):
        print(
            "::error::Set repository Actions variables MS_TENANT_ID and MS_CLIENT_ID before publishing a firmware release.",
            file=sys.stderr,
        )
        return 2

    if tenant_id:
        validate_uuid("MS_TENANT_ID", tenant_id)
    if client_id:
        validate_uuid("MS_CLIENT_ID", client_id)

    if not tenant_id:
        tenant_id = "YOUR_ENTRA_TENANT_ID"
        print("::warning::MS_TENANT_ID is not configured; compile validation only.")
    if not client_id:
        client_id = "YOUR_PUBLIC_CLIENT_APPLICATION_ID"
        print("::warning::MS_CLIENT_ID is not configured; compile validation only.")

    content = TEMPLATE.read_text(encoding="utf-8")
    content = content.replace('"YOUR_ENTRA_TENANT_ID"', f'"{tenant_id}"')
    content = content.replace('"YOUR_PUBLIC_CLIENT_APPLICATION_ID"', f'"{client_id}"')
    TARGET.write_text(content, encoding="utf-8")
    print(f"Generated {TARGET.relative_to(ROOT)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
