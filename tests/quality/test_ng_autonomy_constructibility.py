#!/usr/bin/env python3
"""Negative tests for remaining high-risk constructibility authority."""

from __future__ import annotations

import pathlib
import shutil
import sys
import tempfile
import unittest

import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools" / "quality"))
from check_ng_autonomy_constructibility import ConstructibilityError, MODEL, SCHEMA, validate  # noqa: E402


class ConstructibilityTest(unittest.TestCase):
    def copied_root(self, temporary: str) -> pathlib.Path:
        root = pathlib.Path(temporary)
        model = yaml.safe_load((ROOT / MODEL).read_text(encoding="utf-8"))
        paths = {MODEL, SCHEMA}
        paths.update(pathlib.Path(machine["authority"]) for machine in model["machines"].values())
        for relative in paths:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(ROOT / relative, destination)
        return root

    @staticmethod
    def rewrite(root: pathlib.Path, mutate) -> None:
        path = root / MODEL
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
        mutate(value)
        path.write_text(yaml.safe_dump(value, sort_keys=False), encoding="utf-8")

    def test_repository_model_is_valid(self) -> None:
        validate(ROOT)

    def test_message_id_collision_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["source_closure"]["message_ids"].__setitem__("source_closure", [23, 24, 25, 26, 27, 28]))
            with self.assertRaisesRegex(ConstructibilityError, "message registry"):
                validate(root)

    def test_report_attempt_before_reservation_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            def mutate(value) -> None:
                states = value["machines"]["store_candidate_report"]["report_states"]
                states[2], states[3] = states[3], states[2]
            self.rewrite(root, mutate)
            with self.assertRaisesRegex(ConstructibilityError, "tail is not reserved"):
                validate(root)

    def test_predelegation_lease_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_read_mapping"].__setitem__("predelegation_authority", "lease"))
            with self.assertRaisesRegex(ConstructibilityError, "predelegation authority"):
                validate(root)

    def test_physical_census_normalization_entry_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = self.copied_root(temporary)
            self.rewrite(root, lambda value: value["machines"]["sqlite_normalization_effect"].__setitem__("entry", "physical-census"))
            with self.assertRaisesRegex(ConstructibilityError, "normalization entry"):
                validate(root)


if __name__ == "__main__":
    unittest.main()
