#!/usr/bin/env python3
"""Isolate the documentation download checksum test from restored CI caches."""

from __future__ import annotations

import pathlib


ROOT = pathlib.Path(__file__).resolve().parents[2]
PATH = ROOT / "tests/quality/test_ng_ci_supply_chain.py"


def main() -> None:
    text = PATH.read_text(encoding="utf-8")
    old = '''    def test_documentation_checksum_rejects_before_root_effect(self) -> None:
        with mock.patch(
            "bootstrap_supply_chain.download", return_value=b"substituted"
        ), mock.patch("bootstrap_supply_chain.run") as run:
            with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
                install_documentation(self.lock)
            run.assert_not_called()
'''
    new = '''    def test_documentation_checksum_rejects_before_root_effect(self) -> None:
        with tempfile.TemporaryDirectory() as temporary, mock.patch(
            "bootstrap_supply_chain.package_cache_directory",
            return_value=pathlib.Path(temporary),
        ), mock.patch(
            "bootstrap_supply_chain.download", return_value=b"substituted"
        ), mock.patch("bootstrap_supply_chain.run") as run:
            with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
                install_documentation(self.lock)
            run.assert_not_called()
'''
    if old not in text:
        if new in text:
            pathlib.Path(__file__).unlink()
            return
        raise RuntimeError("documentation checksum test marker differs")
    PATH.write_text(text.replace(old, new, 1), encoding="utf-8")
    pathlib.Path(__file__).unlink()


if __name__ == "__main__":
    main()
