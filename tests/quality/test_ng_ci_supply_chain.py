#!/usr/bin/env python3
"""Positive and fail-closed tests for the CI supply-chain contract."""

from __future__ import annotations

import copy
import json
import pathlib
import sys
import tempfile
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))
sys.path.insert(0, str(ROOT / "tools/ci"))

from bootstrap_supply_chain import SupplyChainError, load_lock, verify_bytes  # noqa: E402
from check_ci_supply_chain import (  # noqa: E402
    CiSupplyChainError,
    parse_hash_lock,
    validate_repository,
    validate_workflow,
)


class NgCiSupplyChainTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lock = load_lock(ROOT)

    def test_repository_contract_and_workflows_are_valid(self) -> None:
        validate_repository(ROOT)

    def test_checksum_mismatch_is_rejected_before_effect(self) -> None:
        with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
            verify_bytes(b"substituted", "0" * 64, "fixture")

    def test_mutable_or_unknown_action_is_rejected(self) -> None:
        for action_line in (
            "      - uses: actions/checkout@v4\n",
            "      - name: upload\n        uses: actions/upload-artifact@v4\n",
        ):
            with self.subTest(action_line=action_line), tempfile.TemporaryDirectory() as temporary:
                workflow = pathlib.Path(temporary) / "workflow.yml"
                workflow.write_text(
                    "jobs:\n  check:\n    runs-on: ubuntu-24.04\n"
                    "    steps:\n" + action_line,
                    encoding="utf-8",
                )
                with self.assertRaisesRegex(CiSupplyChainError, "action differs"):
                    validate_workflow(workflow, self.lock)

    def test_remote_root_script_bootstrap_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            workflow = pathlib.Path(temporary) / "workflow.yml"
            workflow.write_text(
                "jobs:\n  check:\n    runs-on: ubuntu-24.04\n"
                "    steps:\n      - run: sudo ./llvm.sh 22\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CiSupplyChainError, "forbidden bootstrap"):
                validate_workflow(workflow, self.lock)

    def test_unhashed_or_unpinned_python_dependency_is_rejected(self) -> None:
        for content in (
            "jsonschema>=4\n",
            "jsonschema==4.23.0 --hash=sha256:short\n",
        ):
            with self.subTest(content=content), tempfile.TemporaryDirectory() as temporary:
                requirement = pathlib.Path(temporary) / "requirements.lock"
                requirement.write_text(content, encoding="utf-8")
                with self.assertRaisesRegex(
                    CiSupplyChainError, "exact version/hash"
                ):
                    parse_hash_lock(requirement)

    def test_profile_cannot_reference_unlocked_package(self) -> None:
        changed = copy.deepcopy(self.lock)
        changed["llvm"]["profiles"]["compiler"].append("clang-23")
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            lock_path = root / "tools/ci/llvm22-noble.lock.json"
            requirements = root / "tools/quality/requirements.lock"
            lock_path.parent.mkdir(parents=True)
            requirements.parent.mkdir(parents=True)
            requirements.write_bytes((ROOT / changed["python"]["requirements"]).read_bytes())
            lock_path.write_text(json.dumps(changed), encoding="utf-8")
            with self.assertRaisesRegex(SupplyChainError, "unlocked packages"):
                load_lock(root)


if __name__ == "__main__":
    unittest.main()
