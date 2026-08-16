#!/usr/bin/env python3
"""Positive and fail-closed tests for the CI supply-chain contract."""

from __future__ import annotations

import copy
import json
import os
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
    install,
    install_documentation,
    load_lock,
    package_cache_authority_digest,
    package_cache_directory,
    resolve_cached_archive,
    verify_bytes,
    verify_deb_archive,
)
from check_ci_supply_chain import (  # noqa: E402
    CiSupplyChainError,
    parse_hash_lock,
    validate_repository,
    validate_workflow,
)

from collect_toolchain_provenance import (  # noqa: E402
    file_digest,
    package_cache_provenance,
)


class NgCiSupplyChainTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.lock = load_lock(ROOT)

    def package_rows(self, profile: str, source: str) -> list[dict[str, str]]:
        if profile == "documentation":
            authority = self.lock["documentation"]
            packages = [authority["package"]]
            architecture = authority["architecture"]
            versions = {authority["package"]: authority["version"]}
            digests = {authority["package"]: authority["sha256"]}
        else:
            packages = self.lock["llvm"]["profiles"][profile]
            architecture = self.lock["llvm"]["architecture"]
            versions = self.lock["llvm"]["packages"]
            digests = self.lock["llvm"]["package_sha256"]
        return [
            {
                "package": package,
                "version": versions[package],
                "architecture": architecture,
                "package_digest": "sha256:" + digests[package],
                "source": source,
            }
            for package in packages
        ]

    def write_receipt(
        self,
        path: pathlib.Path,
        profiles: dict[str, list[dict[str, str]]],
        *,
        cache_hit: str = "hit",
        authority_digest: str | None = None,
        key: str | None = None,
        schema: str | None = None,
    ) -> None:
        path.write_text(
            json.dumps(
                {
                    "schema": schema or self.lock["package_cache"]["receipt_schema"],
                    "authority_digest": authority_digest
                    or package_cache_authority_digest(self.lock),
                    "key": key or "cxxlens-ci-packages-v1-Linux-X64-developer-false-"
                    + file_digest(ROOT / "tools/ci/llvm22-noble.lock.json").removeprefix(
                        "sha256:"
                    ),
                    "cache_hit": cache_hit,
                    "profiles": profiles,
                },
                sort_keys=True,
            ),
            encoding="utf-8",
        )

    def cache_environment(
        self,
        receipt: pathlib.Path,
        *,
        profile: str = "developer",
        documentation: str = "false",
        runner_os: str = "Linux",
        runner_arch: str = "X64",
        cache_hit: str = "true",
        key: str | None = None,
        include: set[str] | None = None,
    ) -> dict[str, str]:
        config = self.lock["package_cache"]
        lock_digest = file_digest(
            ROOT / "tools/ci/llvm22-noble.lock.json"
        ).removeprefix("sha256:")
        expected_key = config["key_template"]
        for token, value in {
            "${runner.os}": runner_os,
            "${runner.arch}": runner_arch,
            "${profile}": profile,
            "${documentation}": documentation,
            "${lock_digest}": lock_digest,
        }.items():
            expected_key = expected_key.replace(token, value)
        values = {
            config["environment"]: "/tmp/cxxlens-package-cache",
            config["documentation_environment"]: documentation,
            config["hit_environment"]: cache_hit,
            config["key_environment"]: key or expected_key,
            config["profile_environment"]: profile,
            config["receipt_environment"]: str(receipt),
            config["runner_arch_environment"]: runner_arch,
            config["runner_os_environment"]: runner_os,
        }
        return (
            values
            if include is None
            else {name: value for name, value in values.items() if name in include}
        )

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
            shutil.copytree(ROOT / ".github/actions", root / ".github/actions")
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
        with tempfile.TemporaryDirectory() as temporary, mock.patch(
            "bootstrap_supply_chain.package_cache_directory",
            return_value=pathlib.Path(temporary),
        ), mock.patch(
            "bootstrap_supply_chain.download", return_value=b"substituted"
        ), mock.patch(
            "bootstrap_supply_chain.package_cache_hit_claimed", return_value=False
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

    def test_common_setup_action_is_locked_and_used(self) -> None:
        action = ROOT / ".github/actions/setup-ci/action.yml"
        self.assertTrue(action.is_file())
        self.assertEqual(
            self.lock["local_actions"][".github/actions/setup-ci/action.yml"],
            __import__("hashlib").sha256(action.read_bytes()).hexdigest(),
        )
        workflow_text = "\n".join(
            (ROOT / path).read_text(encoding="utf-8")
            for path in (
                ".github/workflows/quality.yml",
                ".github/workflows/nightly.yml",
            )
        )
        self.assertGreaterEqual(workflow_text.count("./.github/actions/setup-ci"), 10)
        self.assertNotIn("actions/setup-python@", workflow_text)
        self.assertNotIn("bootstrap_supply_chain.py install --profile", workflow_text)

    def test_mutated_local_setup_action_is_rejected(self) -> None:
        import shutil

        with tempfile.TemporaryDirectory() as temporary:
            root = pathlib.Path(temporary)
            shutil.copytree(ROOT / ".github", root / ".github")
            shutil.copytree(ROOT / "schemas", root / "schemas")
            shutil.copytree(ROOT / "tools", root / "tools")
            action = root / ".github/actions/setup-ci/action.yml"
            action.write_text(action.read_text(encoding="utf-8") + "# mutation\n", encoding="utf-8")
            with self.assertRaisesRegex(CiSupplyChainError, "local action differs"):
                validate_repository(root)



    def test_downloaded_package_cache_is_exact_and_has_no_fallback_key(self) -> None:
        action = (ROOT / ".github/actions/setup-ci/action.yml").read_text(
            encoding="utf-8"
        )
        self.assertIn(
            "actions/cache@0057852bfaa89a56745cba8c7296529d2fc39830",
            action,
        )
        self.assertIn("CXXLENS_PACKAGE_CACHE_RECEIPT", action)
        self.assertIn("${{ inputs.profile }}-${{ inputs.documentation }}-", action)
        self.assertIn("hashFiles('tools/ci/llvm22-noble.lock.json')", action)
        self.assertNotIn("restore-keys:", action)
        self.assertEqual(
            self.lock["package_cache"]["correctness_role"],
            "transport-optimization-only",
        )

    def test_relative_downloaded_package_cache_path_is_rejected(self) -> None:
        with mock.patch.dict(
            os.environ, {"CXXLENS_PACKAGE_CACHE": "relative/cache"}, clear=False
        ):
            with self.assertRaisesRegex(SupplyChainError, "must be absolute"):
                package_cache_directory(self.lock)

    def test_cache_hit_configures_repository_before_archive_resolution(self) -> None:
        lock = copy.deepcopy(self.lock)
        events = mock.Mock()
        with mock.patch("bootstrap_supply_chain.load_lock", return_value=lock), \
             mock.patch("bootstrap_supply_chain.assert_runner"), \
             mock.patch("bootstrap_supply_chain.download", return_value=b"verified-key"), \
             mock.patch("bootstrap_supply_chain.verify_bytes"), \
             mock.patch("bootstrap_supply_chain.configure_llvm_repository") as configure, \
             mock.patch("bootstrap_supply_chain.package_cache_directory", return_value=pathlib.Path("/tmp/cache")), \
             mock.patch("bootstrap_supply_chain.package_cache_hit_claimed", return_value=True), \
             mock.patch("bootstrap_supply_chain.resolve_cached_archive", side_effect=RuntimeError("archive-resolution")) as resolve:
            events.attach_mock(configure, "configure")
            events.attach_mock(resolve, "resolve")
            with self.assertRaisesRegex(RuntimeError, "archive-resolution"):
                install(ROOT, "compiler")
        self.assertEqual(
            events.mock_calls[0],
            mock.call.configure(lock["llvm"], b"verified-key"),
        )

    def test_verified_cached_package_is_reused(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "clang.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_deb_archive") as verify:
                resolved = resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=True,
                )
            self.assertEqual(resolved, archive)
            verify.assert_called_once()

    def test_cache_miss_is_explicit_and_claimed_incomplete_hit_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "missing.deb"
            self.assertIsNone(
                resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=False,
                )
            )
            with self.assertRaisesRegex(SupplyChainError, "omitted locked package"):
                resolve_cached_archive(
                    archive,
                    package="clang-22",
                    version="fixture-version",
                    architecture="amd64",
                    digest="0" * 64,
                    cache_hit=True,
                )

    def test_corrupt_cached_package_is_rejected_before_metadata_probe(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"substituted")
            with mock.patch("bootstrap_supply_chain.run") as run:
                with self.assertRaisesRegex(SupplyChainError, "checksum mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version=self.lock["llvm"]["packages"]["clang-22"],
                        architecture=self.lock["llvm"]["architecture"],
                        digest=self.lock["llvm"]["package_sha256"]["clang-22"],
                    )
                run.assert_not_called()

    def test_cached_package_wrong_version_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_bytes"), mock.patch(
                "bootstrap_supply_chain.run",
                side_effect=["clang-22", "wrong-version", "amd64"],
            ):
                with self.assertRaisesRegex(SupplyChainError, "metadata mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version="expected-version",
                        architecture="amd64",
                        digest="0" * 64,
                    )

    def test_cached_package_wrong_architecture_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            archive = pathlib.Path(temporary) / "package.deb"
            archive.write_bytes(b"fixture")
            with mock.patch("bootstrap_supply_chain.verify_bytes"), mock.patch(
                "bootstrap_supply_chain.run",
                side_effect=["clang-22", "expected-version", "arm64"],
            ):
                with self.assertRaisesRegex(SupplyChainError, "metadata mismatch"):
                    verify_deb_archive(
                        archive,
                        package="clang-22",
                        version="expected-version",
                        architecture="amd64",
                        digest="0" * 64,
                    )

    def test_package_cache_provenance_binds_verified_source_and_authority(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"
            authority_digest = package_cache_authority_digest(self.lock)
            self.write_receipt(
                receipt,
                {"developer": self.package_rows("developer", "verified-cache")},
            )
            environment = self.cache_environment(receipt)
            with mock.patch.dict(os.environ, environment, clear=True):
                evidence = package_cache_provenance(self.lock)
            self.assertEqual(evidence["status"], "verified")
            self.assertEqual(evidence["authority_digest"], authority_digest)
            self.assertEqual(
                evidence["lock_digest"],
                file_digest(ROOT / "tools/ci/llvm22-noble.lock.json"),
            )
            self.assertEqual(evidence["profile"], "developer")
            self.assertEqual(evidence["documentation"], "false")
            self.assertEqual(
                evidence["profiles"]["developer"][0]["source"],
                "verified-cache",
            )

    def test_package_cache_provenance_rejects_non_authoritative_package_rows(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"

            def write_receipt(row_overrides: dict[str, str]) -> None:
                rows = self.package_rows("developer", "verified-cache")
                rows[0].update(row_overrides)
                self.write_receipt(
                    receipt,
                    {"developer": rows},
                )

            environment = self.cache_environment(receipt)
            for field, value in (
                ("package", "not-locked"),
                ("version", "1:22.1.7-1"),
                ("architecture", "arm64"),
                ("package_digest", "sha256:" + "0" * 64),
            ):
                with self.subTest(field=field):
                    write_receipt({field: value})
                    with mock.patch.dict(os.environ, environment, clear=True):
                        with self.assertRaisesRegex(ValueError, "package"):
                            package_cache_provenance(self.lock)

            write_receipt({"source": "verified-download"})
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "cache-hit source"):
                    package_cache_provenance(self.lock)

    def test_package_cache_provenance_rejects_incomplete_hit_and_invalid_binding(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"
            self.write_receipt(
                receipt,
                {
                    "developer": [
                        self.package_rows("developer", "verified-cache")[0]
                    ]
                },
            )
            environment = self.cache_environment(
                receipt, cache_hit="not-a-boolean"
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "environment binding"):
                    package_cache_provenance(self.lock)

            environment[self.lock["package_cache"]["hit_environment"]] = "true"
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "package set differs"):
                    package_cache_provenance(self.lock)

    def test_stale_package_cache_provenance_authority_is_rejected(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "receipt.json"
            environment = self.cache_environment(
                receipt, cache_hit="false"
            )
            self.write_receipt(
                receipt,
                {"developer": self.package_rows("developer", "verified-download")},
                cache_hit="miss",
                authority_digest="sha256:" + "0" * 64,
                key=environment[self.lock["package_cache"]["key_environment"]],
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "binding differs"):
                    package_cache_provenance(self.lock)

    def test_package_cache_provenance_without_any_cache_variables_is_not_requested(self) -> None:
        with mock.patch.dict(os.environ, {}, clear=True):
            evidence = package_cache_provenance(self.lock)
        self.assertEqual(evidence["status"], "not-requested")
        self.assertEqual(evidence["profiles"], {})

    def test_package_cache_provenance_rejects_partial_cache_environment(self) -> None:
        config = self.lock["package_cache"]
        names = (
            "environment",
            "documentation_environment",
            "hit_environment",
            "key_environment",
            "profile_environment",
            "receipt_environment",
            "runner_arch_environment",
            "runner_os_environment",
        )
        for field in names:
            with self.subTest(field=field):
                with mock.patch.dict(
                    os.environ, {config[field]: "fixture"}, clear=True
                ):
                    with self.assertRaises(ValueError):
                        package_cache_provenance(self.lock)

    def test_package_cache_provenance_binds_every_locked_profile_and_rejects_cross_profile_scope(
        self,
    ) -> None:
        profiles = list(self.lock["llvm"]["profiles"])
        with tempfile.TemporaryDirectory() as temporary:
            for profile in profiles:
                with self.subTest(profile=profile):
                    receipt = pathlib.Path(temporary) / f"{profile}.json"
                    environment = self.cache_environment(
                        receipt, profile=profile, cache_hit="false"
                    )
                    key = environment[self.lock["package_cache"]["key_environment"]]
                    self.write_receipt(
                        receipt,
                        {profile: self.package_rows(profile, "verified-download")},
                        cache_hit="miss",
                        key=key,
                    )
                    with mock.patch.dict(os.environ, environment, clear=True):
                        evidence = package_cache_provenance(self.lock)
                    self.assertEqual(evidence["profile"], profile)

            requested = profiles[0]
            alternate = profiles[1]
            receipt = pathlib.Path(temporary) / "cross-profile.json"
            environment = self.cache_environment(
                receipt, profile=requested, cache_hit="false"
            )
            key = environment[self.lock["package_cache"]["key_environment"]]
            self.write_receipt(
                receipt,
                {alternate: self.package_rows(alternate, "verified-download")},
                cache_hit="miss",
                key=key,
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "profile scope differs"):
                    package_cache_provenance(self.lock)

    def test_package_cache_provenance_rejects_all_profile_and_documentation_mismatch(
        self,
    ) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "all-profiles.json"
            environment = self.cache_environment(
                receipt, profile="developer", cache_hit="false"
            )
            key = environment[self.lock["package_cache"]["key_environment"]]
            self.write_receipt(
                receipt,
                {
                    profile: self.package_rows(profile, "verified-download")
                    for profile in self.lock["llvm"]["profiles"]
                },
                cache_hit="miss",
                key=key,
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "profile scope differs"):
                    package_cache_provenance(self.lock)

            receipt = pathlib.Path(temporary) / "documentation-mismatch.json"
            environment = self.cache_environment(
                receipt, profile="developer", documentation="true", cache_hit="false"
            )
            key = environment[self.lock["package_cache"]["key_environment"]]
            self.write_receipt(
                receipt,
                {"developer": self.package_rows("developer", "verified-download")},
                cache_hit="miss",
                key=key,
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "profile scope differs"):
                    package_cache_provenance(self.lock)

            receipt = pathlib.Path(temporary) / "extra-documentation.json"
            environment = self.cache_environment(
                receipt, profile="developer", documentation="false", cache_hit="false"
            )
            key = environment[self.lock["package_cache"]["key_environment"]]
            self.write_receipt(
                receipt,
                {
                    "developer": self.package_rows("developer", "verified-download"),
                    "documentation": self.package_rows(
                        "documentation", "verified-download"
                    ),
                },
                cache_hit="miss",
                key=key,
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "profile scope differs"):
                    package_cache_provenance(self.lock)

    def test_package_cache_provenance_rejects_noncanonical_key_and_runner(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            receipt = pathlib.Path(temporary) / "binding.json"
            environment = self.cache_environment(receipt, key="fixture-key")
            self.write_receipt(
                receipt,
                {"developer": self.package_rows("developer", "verified-cache")},
                key="fixture-key",
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "locked authority"):
                    package_cache_provenance(self.lock)

            environment = self.cache_environment(receipt, runner_os="Windows")
            key = environment[self.lock["package_cache"]["key_environment"]]
            self.write_receipt(
                receipt,
                {"developer": self.package_rows("developer", "verified-cache")},
                key=key,
            )
            with mock.patch.dict(os.environ, environment, clear=True):
                with self.assertRaisesRegex(ValueError, "runner differs"):
                    package_cache_provenance(self.lock)


if __name__ == "__main__":
    unittest.main()
