#!/usr/bin/env python3
"""Positive and negative tests for dedicated source-closure transport."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_source_closure_transport import (  # noqa: E402
    ADR,
    CONTRACT,
    PROTOCOL,
    REQUEST,
    SCHEMA,
    TASK,
    SourceClosureTransportError,
    validate,
)


class SourceClosureTransportTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in (ADR, CONTRACT, PROTOCOL, REQUEST, SCHEMA, TASK):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, relative: pathlib.Path, mutate) -> None:
        path = root / relative
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_contract_is_valid(self) -> None:
        validate(ROOT)

    def test_message_collision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["message_registry"]["proposed"][0].update({"id": 23}),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "collides"):
                validate(root)

    def test_protocol_downgrade_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["versions"]["provider_protocol"].update(
                    {"downgrade": "fallback"}
                ),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "version|schema"):
                validate(root)

    def test_cross_task_cache_activation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                CONTRACT,
                lambda value: value["cache"].update({"cross_task_v1": "enabled"}),
            )
            with self.assertRaisesRegex(SourceClosureTransportError, "cache"):
                validate(root)

    def test_acceptance_without_review_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, CONTRACT, lambda value: value.update({"maturity": "accepted"}))
            with self.assertRaisesRegex(SourceClosureTransportError, "accepted authority"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
