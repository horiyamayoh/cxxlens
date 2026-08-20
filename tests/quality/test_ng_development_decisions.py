#!/usr/bin/env python3
"""Positive and fail-closed tests for development decisions."""

from __future__ import annotations

import copy
import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))

from check_ng_development_decisions import (  # noqa: E402
    DecisionRegisterError,
    REGISTER,
    SCHEMA,
    validate,
)


class DevelopmentDecisionTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        for relative in (REGISTER, SCHEMA):
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        register = yaml.safe_load((ROOT / REGISTER).read_text(encoding="utf-8"))
        for entry in register["decisions"]:
            for reference in entry["authority_refs"]:
                destination = root / reference
                destination.parent.mkdir(parents=True, exist_ok=True)
                destination.write_text("authority\n", encoding="utf-8")
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, mutate) -> None:
        path = root / REGISTER
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_register_is_valid(self) -> None:
        validate(ROOT)

    def test_duplicate_decision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"].append(copy.deepcopy(value["decisions"][0])))
            with self.assertRaisesRegex(DecisionRegisterError, "duplicate decision IDs"):
                validate(root)

    def test_high_risk_self_review_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["decisions"][1].__setitem__(
                    "review", {"mode": "self", "status": "not-required", "author": "owner", "reviewer": None, "refs": []}
                ),
            )
            with self.assertRaisesRegex(DecisionRegisterError, "schema validation|independent review"):
                validate(root)

    def test_completed_review_requires_canonical_comment(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(
                root,
                lambda value: value["decisions"][1].__setitem__(
                    "review", {"mode": "independent", "status": "complete", "author": "owner", "reviewer": "reviewer", "refs": ["local.md"]}
                ),
            )
            with self.assertRaisesRegex(DecisionRegisterError, "review reference is not canonical"):
                validate(root)

    def test_qualification_before_implementation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["decisions"][1].__setitem__("qualification_status", "qualified"))
            with self.assertRaisesRegex(DecisionRegisterError, "qualification precedes implementation"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
