#!/usr/bin/env python3
"""Validate the structural invariants of the public API catalog."""

from __future__ import annotations

import pathlib
import sys

import yaml


def fail(message: str) -> None:
    raise ValueError(message)


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} CATALOG", file=sys.stderr)
        return 2

    catalog_path = pathlib.Path(sys.argv[1])
    document = yaml.safe_load(catalog_path.read_text(encoding="utf-8"))
    if not isinstance(document, dict):
        fail("catalog root must be a mapping")
    if document.get("schema") != "cxxlens.api-catalog.v1":
        fail("unexpected catalog schema")
    if document.get("language") != "C++23":
        fail("catalog language must be C++23")

    packages = document.get("packages")
    if not isinstance(packages, list) or not packages:
        fail("packages must be a non-empty sequence")

    package_ids: set[str] = set()
    api_ids: set[str] = set()
    required_package_fields = {"id", "header", "purpose", "public_types", "apis"}
    required_api_fields = {"id", "symbol", "kind", "phase", "maturity"}
    allowed_maturity = {"contract-defined", "planned", "experimental", "stable", "deprecated"}

    for package in packages:
        if not isinstance(package, dict):
            fail("every package must be a mapping")
        missing = required_package_fields - package.keys()
        if missing:
            fail(f"package is missing fields: {sorted(missing)}")
        package_id = package["id"]
        if package_id in package_ids:
            fail(f"duplicate package id: {package_id}")
        package_ids.add(package_id)

        if not isinstance(package["apis"], list):
            fail(f"package {package_id}: apis must be a sequence")
        for api in package["apis"]:
            if not isinstance(api, dict):
                fail(f"package {package_id}: API entry must be a mapping")
            missing = required_api_fields - api.keys()
            if missing:
                fail(f"package {package_id}: API is missing fields: {sorted(missing)}")
            api_id = api["id"]
            if api_id in api_ids:
                fail(f"duplicate API id: {api_id}")
            api_ids.add(api_id)
            if api["maturity"] not in allowed_maturity:
                fail(f"{api_id}: unknown maturity {api['maturity']}")
            if not str(api["phase"]).startswith("M"):
                fail(f"{api_id}: phase must use an M milestone")

    print(f"validated {len(package_ids)} packages and {len(api_ids)} API entries")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, ValueError, yaml.YAMLError) as error:
        print(f"API catalog validation failed: {error}", file=sys.stderr)
        sys.exit(1)
