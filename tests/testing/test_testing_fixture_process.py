#!/usr/bin/env python3
"""Validate fixture schema/golden and root/order/locale determinism."""

from __future__ import annotations

import hashlib
import json
import locale
import pathlib
import subprocess
import sys

import jsonschema
import yaml


def run(executable: pathlib.Path, root: str, order: str, locale_name: str) -> bytes:
    completed = subprocess.run(
        [str(executable), root, order],
        check=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env={"LC_ALL": locale_name, "LANG": locale_name},
    )
    return completed.stdout


def main() -> int:
    executable = pathlib.Path(sys.argv[1])
    root = pathlib.Path(sys.argv[2])
    schema = yaml.safe_load((root / "schemas/cxxlens_testing_fixture.schema.yaml").read_text())
    locales = ["C"]
    try:
        locale.setlocale(locale.LC_ALL, "C.UTF-8")
        locales.append("C.UTF-8")
    except locale.Error:
        pass
    outputs = [
        run(executable, fixture_root, order, locale_name)
        for locale_name in locales
        for fixture_root, order in (("/workspace/a", "forward"), ("/relocated/b", "reverse"))
    ]
    if any(output != outputs[0] for output in outputs[1:]):
        raise AssertionError("fixture insertion order, root, or locale changed semantic bytes")
    text = outputs[0].decode("utf-8").strip()
    document = json.loads(text)
    jsonschema.validate(document, schema)
    if json.dumps(document, sort_keys=True, separators=(",", ":"), ensure_ascii=False) != text:
        raise AssertionError("fixture output is not canonical JSON")
    if "/workspace/a" in text or "/relocated/b" in text:
        raise AssertionError("absolute fixture root leaked into semantic output")
    digest = hashlib.sha256(outputs[0]).hexdigest()
    expected = (root / "tests/golden/testing/fixture_v1.sha256").read_text().strip()
    if digest != expected:
        raise AssertionError(f"fixture golden mismatch: {digest} != {expected}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
