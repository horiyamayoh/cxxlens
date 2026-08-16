#!/usr/bin/env python3
"""Positive and fail-closed tests for the CI supply-chain contract."""

from __future__ import annotations

import copy
import json
import pathlib
import shutil
import sys
import tempfile
import unittest
from unittest import mock


ROOT = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools/quality"))
sys.path.insert(0, str(ROOT / "tools/ci"))

from bootstrap_supply_chain import (  # noqa: E402
    SupplyChainError,
    build_package_cache_provenance,
    cache_provenance_digest,
    install_documentation,
    load_lock,
    package_authority,
    resolve_cached_archives,
    sha256_bytes,
    verify_bytes,
    write_package_cache_provenance,
)
from check_ci_supply_chain import (  # noqa: E402
    CiSupplyChainError,
    parse_hash_lock,
    validate_repository,
    validate_workflow,
)
from collect_toolchain_provenance import (  # noqa: E402
    load_package_cache_provenance,
)


class NgCiSupplyChainTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lock = load_lock(ROOT)

    def test_repository_contract_and_workflows_are_valid(self) -> None:
        validate_repository(ROOT)

    def test_tracked_reusable_workflow_is_repository_scoped(self) -> None:
        validate_workflow(ROOT / ".github/workflows/quality.yml", self.lock)

    def test_unavailable_reusable_workflow_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            workflow = root / ".github/workflows/quality.yml"
            workflow.parent.mkdir(parents=True)
            workflow.write_text(
                "jobs:\n  check:\n    uses: ./.github/workflows/missing.yml\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CiSupplyChainError, "unavailable"):
                validate_workflow(workflow, self.lock)

    def test_unlocked_reusable_workflow_is_rejected(self) -> None:
        lock = copy.deepcopy(self.lock)
        del lock["local_workflows"][".github/workflows/nightly.yml"]
        with self.assertRaisesRegex(CiSupplyChainError, "absent from supply-chain lock"):
            validate_workflow(ROOT / ".github/workflows/quality.yml", lock)

    def test_reusable_workflow_digest_mismatch_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            workflows = root / ".github/workflows"
            workflows.mkdir(parents=True)
            shutil.copy2(ROOT / ".github/workflows/quality.yml", workflows / "quality.yml")
            shutil.copy2(ROOT / ".github/workflows/nightly.yml", workflows / "nightly.yml")
            (workflows / "nightly.yml").write_text(
                (workflows / "nightly.yml").read_text(encoding="utf-8") + "\n# substituted\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(CiSupplyChainError, "differs from supply-chain lock"):
                validate_workflow(workflows / "quality.yml", self.lock)

    def test_bootstrap_lock_rejects_reusable_workflow_digest_mismatch(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            (root / "tools/ci").mkdir(parents=True)
            (root / "tools/quality").mkdir(parents=True)
            (root / ".github/workflows").mkdir(parents=True)
            shutil.copy2(
                ROOT / "tools/ci/llvm22-noble.lock.json",
                root / "tools/ci/llvm22-noble.lock.json",
            )
            shutil.copy2(
                ROOT / "tools/quality/requirements.lock",
                root / "tools/quality/requirements.lock",
            )
            nightly = root / ".github/workflows/nightly.yml"
            shutil.copy2(ROOT / ".github/workflows/nightly.yml", nightly)
            nightly.write_text(
                nightly.read_text(encoding="utf-8") + "\n# substituted\n",
                encoding="utf-8",
            )
            with self.assertRaisesRegex(SupplyChainError, "local workflow .*checksum mismatch"):
                load_lock(root)

    def test_checksum_mismatch_is_rejected_before_effect(self) -> None:
        with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
            verify_bytes(b"substituted", "0" * 64, "fixture")

    def test_documentation_package_is_exactly_locked(self) -> None:
        self.assertEqual(self.lock["documentation"]["package"], "doxygen")
        self.assertEqual(
            self.lock["documentation"]["version"], "1.9.8+ds-2build5"
        )
        self.assertEqual(len(self.lock["documentation"]["sha256"]), 64)

    def test_documentation_checksum_rejects_before_root_effect(self) -> None:
        with mock.patch(
            "bootstrap_supply_chain.download", return_value=b"substituted"
        ), mock.patch("bootstrap_supply_chain.run") as run:
            with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
                install_documentation(self.lock)
            run.assert_not_called()

    def test_compiler_profile_contains_sanitizer_runtime(self) -> None:
        self.assertIn(
            "libclang-rt-22-dev", self.lock["llvm"]["profiles"]["compiler"]
        )

    def test_developer_profile_contains_sanitizer_runtime(self) -> None:
        self.assertIn(
            "libclang-rt-22-dev", self.lock["llvm"]["profiles"]["developer"]
        )

    def test_configure_profiles_contain_required_clang_format(self) -> None:
        self.assertIn(
            "clang-format-22", self.lock["llvm"]["profiles"]["compiler"]
        )
        self.assertIn(
            "clang-format-22", self.lock["llvm"]["profiles"]["static-analysis"]
        )

    def test_llvm_package_revision_is_exactly_locked(self) -> None:
        self.assertEqual(self.lock["llvm"]["expected_release"], "22.1.8")
        self.assertEqual(
            set(self.lock["llvm"]["packages"].values()),
            {
                "1:22.1.8~++20260714014902+ca7933e47d3a-1~exp1~"
                "20260714135019.80"
            },
        )

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

    def test_valid_cached_package_requires_exact_identity_and_digest(self) -> None:
        expected = package_authority(self.lock, "compiler")["clang-22"].copy()
        expected["sha256"] = sha256_bytes(b"locked archive")
        with tempfile.TemporaryDirectory() as temporary:
            cache = pathlib.Path(temporary)
            archive = cache / "restored.deb"
            archive.write_bytes(b"locked archive")
            with mock.patch(
                "bootstrap_supply_chain.package_fields",
                return_value={
                    "Package": "clang-22",
                    "Version": expected["version"],
                    "Architecture": expected["architecture"],
                },
            ):
                archives, status, reason = resolve_cached_archives(
                    cache, {"clang-22": expected}
                )
            self.assertEqual(status, "hit")
            self.assertIsNone(reason)
            self.assertEqual(archives, {"clang-22": archive})

    def test_corrupted_cached_package_is_not_selected(self) -> None:
        expected = package_authority(self.lock, "compiler")["clang-22"].copy()
        expected["sha256"] = sha256_bytes(b"locked archive")
        with tempfile.TemporaryDirectory() as temporary:
            cache = pathlib.Path(temporary)
            (cache / "restored.deb").write_bytes(b"corrupted archive")
            with mock.patch(
                "bootstrap_supply_chain.package_fields",
                return_value={
                    "Package": "clang-22",
                    "Version": expected["version"],
                    "Architecture": expected["architecture"],
                },
            ):
                archives, status, reason = resolve_cached_archives(
                    cache, {"clang-22": expected}
                )
            self.assertIsNone(archives)
            self.assertEqual(status, "invalid")
            self.assertIn("checksum mismatch", reason or "")

    def test_wrong_version_or_architecture_is_not_selected(self) -> None:
        expected = package_authority(self.lock, "compiler")["clang-22"].copy()
        expected["sha256"] = sha256_bytes(b"locked archive")
        for field, received in (
            ("Version", "1:22.1.7-1"),
            ("Architecture", "arm64"),
        ):
            with self.subTest(field=field), tempfile.TemporaryDirectory() as temporary:
                cache = pathlib.Path(temporary)
                (cache / "restored.deb").write_bytes(b"locked archive")
                metadata = {
                    "Package": "clang-22",
                    "Version": expected["version"],
                    "Architecture": expected["architecture"],
                }
                metadata[field] = received
                with mock.patch(
                    "bootstrap_supply_chain.package_fields", return_value=metadata
                ):
                    archives, status, reason = resolve_cached_archives(
                        cache, {"clang-22": expected}
                    )
                self.assertIsNone(archives)
                self.assertEqual(status, "invalid")
                self.assertIn("metadata mismatch", reason or "")

    def test_cache_miss_is_explicit_and_does_not_select_an_archive(self) -> None:
        expected = package_authority(self.lock, "compiler")["clang-22"]
        with tempfile.TemporaryDirectory() as temporary:
            archives, status, reason = resolve_cached_archives(
                pathlib.Path(temporary), {"clang-22": expected}
            )
        self.assertIsNone(archives)
        self.assertEqual(status, "miss")
        self.assertIsNone(reason)

    def test_package_cache_provenance_binds_lock_key_and_source(self) -> None:
        lock_digest = "sha256:" + sha256_bytes(
            (ROOT / "tools/ci/llvm22-noble.lock.json").read_bytes()
        )
        with self.assertRaisesRegex(SupplyChainError, "status/source mismatch"):
            build_package_cache_provenance(
                self.lock,
                "compiler",
                lock_digest,
                "hit",
                "verified-download",
            )
        record = build_package_cache_provenance(
            self.lock, "compiler", lock_digest, "hit", "verified-cache"
        )
        with tempfile.TemporaryDirectory() as temporary:
            path = pathlib.Path(temporary) / "package-cache.json"
            write_package_cache_provenance(path, record)
            self.assertEqual(load_package_cache_provenance(path, ROOT), record)

            changed = copy.deepcopy(record)
            changed["cache_key_authority_digest"] = "sha256:" + "0" * 64
            changed["digest"] = cache_provenance_digest(changed)
            write_package_cache_provenance(path, changed)
            with self.assertRaisesRegex(ValueError, "authority digest mismatch"):
                load_package_cache_provenance(path, ROOT)

            changed = copy.deepcopy(record)
            changed["packages"][0]["sha256"] = "sha256:" + "0" * 64
            changed["digest"] = cache_provenance_digest(changed)
            write_package_cache_provenance(path, changed)
            with self.assertRaisesRegex(ValueError, "differs from lock"):
                load_package_cache_provenance(path, ROOT)


if __name__ == "__main__":
    unittest.main()
