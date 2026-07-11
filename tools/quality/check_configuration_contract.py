#!/usr/bin/env python3
"""Enforce configuration resolver ownership, schema, and security fixtures."""

from __future__ import annotations

import pathlib
import sys

import yaml


def main() -> int:
    root = pathlib.Path(sys.argv[1])
    failures: list[str] = []
    public = (root / "include/cxxlens/configuration.hpp").read_text(encoding="utf-8")
    implementation = (root / "src/config/configuration.cpp").read_text(encoding="utf-8")
    loader = (root / "src/config/yaml_loader.cpp").read_text(encoding="utf-8")
    unit = (root / "tests/unit/config/configuration_test.cpp").read_text(encoding="utf-8")

    for schema in (
        "cxxlens.config.schema.yaml",
        "cxxlens.config.resolved.schema.yaml",
        "cxxlens.config.explain.schema.yaml",
    ):
        yaml.safe_load((root / "schemas" / schema).read_text(encoding="utf-8"))
    for symbol in (
        "defaults",
        "load",
        "load_nearest",
        "with_profile",
        "overlay",
        "validate",
        "resolved_json",
        "explain",
    ):
        if symbol not in public:
            failures.append(f"missing public configuration symbol: {symbol}")
    if "detail::json::write" not in implementation:
        failures.append("configuration output bypasses the common canonical writer")
    if "filesystem_port" not in loader or "environment_port" not in loader:
        failures.append("configuration loader bypasses runtime ports")
    for path in root.glob("src/**/*.cpp"):
        text = path.read_text(encoding="utf-8")
        if "std::getenv" in text and path.name != "environment_adapter.cpp":
            failures.append(f"direct environment read outside adapter: {path}")
        if '".cxxlens.yaml"' in text and path.name != "yaml_loader.cpp":
            failures.append(f"direct configuration discovery outside loader: {path}")
    for fixture in (
        "config.unknown-key",
        "config.invalid-value",
        "config.profile-not-found",
        "config.overlay-conflict",
        "symlink escape was accepted",
        "unreadable YAML",
        "secret leaked",
        "deep YAML",
    ):
        if fixture not in unit:
            failures.append(f"configuration branch fixture missing: {fixture}")
    if "maximum_document_bytes" not in loader or "maximum_depth" not in loader:
        failures.append("YAML size/depth limits are missing")

    if failures:
        print("configuration contract quality check failed:\n" + "\n".join(failures), file=sys.stderr)
        return 1
    print("validated typed configuration ownership, ports, limits, and security fixtures")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
