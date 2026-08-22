#!/usr/bin/env python3
"""Check the small, pinned CI surface without collecting operational evidence."""

from __future__ import annotations

import argparse
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
WORKFLOWS = (pathlib.Path(".github/workflows/quality.yml"), pathlib.Path(".github/workflows/release.yml"))
PINNED_ACTION = re.compile(r"(?:^|\s)uses:\s+[^@\s]+@([0-9a-f]{40})(?:\s|$)")


class CiSupplyChainError(ValueError):
    pass


def job_body(workflow: str, job: str) -> str:
    """Return one top-level job body, without accepting a similarly named job."""
    match = re.search(
        rf"^  {re.escape(job)}:\n(?P<body>.*?)(?=^  [a-z0-9-]+:\n|\Z)",
        workflow,
        re.MULTILINE | re.DOTALL,
    )
    if match is None:
        raise CiSupplyChainError(f"workflow job is missing: {job}")
    return match.group("body")


def require_job_markers(workflow: str, job: str, *markers: str) -> None:
    body = job_body(workflow, job)
    missing = [marker for marker in markers if marker not in body]
    if missing:
        raise CiSupplyChainError(
            f"workflow job lacks required test marker: {job}: {', '.join(missing)}"
        )


def check(root: pathlib.Path) -> None:
    for relative in WORKFLOWS:
        path = root / relative
        if not path.is_file():
            raise CiSupplyChainError(f"workflow is missing: {relative}")
        text = path.read_text(encoding="utf-8")
        for forbidden in (
            "run_gate.py",
            "collect_toolchain_provenance.py",
            "check_quality_ownership.py",
            "actions/upload-artifact",
            "actions/download-artifact",
            "upload-artifact",
            "download-artifact",
            "qualification-report",
            "evidence-output",
            "--report",
            "--output-junit",
            "output-junit",
            "junit",
            "timing.json",
            "toolchain-provenance",
            "toolchain_provenance",
            "always()",
            "continue-on-error:",
        ):
            if forbidden in text:
                raise CiSupplyChainError(f"operational evidence machinery remains in {relative}: {forbidden}")
        if re.search(r"--output\s+[\"']?\$RUNNER_TEMP", text):
            raise CiSupplyChainError(f"test output report remains in {relative}")
        for line in text.splitlines():
            if "uses:" not in line or "./" in line:
                continue
            if PINNED_ACTION.search(line) is None:
                raise CiSupplyChainError(f"external action is not pinned to a commit: {relative}: {line.strip()}")
    quality = (root / WORKFLOWS[0]).read_text(encoding="utf-8")
    release = (root / WORKFLOWS[1]).read_text(encoding="utf-8")
    for marker in ("pull_request:", "push:", "branches: [main]"):
        if marker not in quality:
            raise CiSupplyChainError(f"main workflow lacks deterministic-test marker: {marker}")
    require_job_markers(
        quality,
        "deterministic-tests",
        "ctest --preset ci-quick",
        "--output-on-failure",
        'shared: ["OFF", "ON"]',
    )
    require_job_markers(
        quality,
        "contract-and-docs",
        "--target cxxlens-quality\n",
        "ctest --test-dir build/docs",
        "quality|security|docs",
    )
    require_job_markers(
        quality,
        "installed-consumers",
        "ctest --preset install-check",
        "^install\\\\.",
        'shared: ["OFF", "ON"]',
    )
    require_job_markers(
        quality,
        "gcc-public-headers",
        "g++ -std=c++23",
        "tests/public_headers/cxxlens_header_test.cpp",
        "tests/public_headers/sdk_header_test.cpp",
    )
    required_release_jobs = (
        "main-tests",
        "contract-and-docs",
        "gcc-public-headers",
        "asan-ubsan",
        "tsan",
        "static-analysis",
        "stress-and-repeat",
        "maximum-scale",
        "real-projects",
        "package:",
    )
    for marker in ("workflow_dispatch:", 'tags: [\"v*\"]', *required_release_jobs):
        if marker not in release:
            raise CiSupplyChainError(f"release workflow lacks marker: {marker}")
    require_job_markers(
        release,
        "main-tests",
        "ctest --preset ci-quick",
        "--output-on-failure",
        'shared: ["OFF", "ON"]',
    )
    require_job_markers(
        release,
        "contract-and-docs",
        "--target cxxlens-quality\n",
        "ctest --test-dir build/docs",
        "quality|security|docs",
    )
    require_job_markers(
        release,
        "gcc-public-headers",
        "g++ -std=c++23",
        "tests/public_headers/cxxlens_header_test.cpp",
        "tests/public_headers/sdk_header_test.cpp",
    )
    require_job_markers(release, "asan-ubsan", "ctest --preset asan-ubsan")
    require_job_markers(release, "tsan", "ctest --preset tsan")
    require_job_markers(release, "static-analysis", "cxxlens-clang-tidy")
    require_job_markers(release, "stress-and-repeat", "ctest --test-dir build/ci-quick --repeat")
    require_job_markers(release, "maximum-scale", "clang22_materializer_scale_test.py")
    require_job_markers(
        release,
        "real-projects",
        "ctest --test-dir build/install-check --label-regex",
        "install",
        "integration",
    )
    for job in ("maximum-scale", "real-projects"):
        if 'shared: ["OFF", "ON"]' not in job_body(release, job):
            raise CiSupplyChainError(f"release job lacks static/shared supported matrix: {job}")
    package_needs = job_body(release, "package").replace("\n", " ")
    required_needs = (
        "main-tests",
        "contract-and-docs",
        "gcc-public-headers",
        "asan-ubsan",
        "tsan",
        "static-analysis",
        "stress-and-repeat",
        "maximum-scale",
        "real-projects",
    )
    if any(job not in package_needs for job in required_needs):
        raise CiSupplyChainError("release package does not depend on every release test job")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        check(args.root.resolve())
    except (CiSupplyChainError, OSError) as error:
        print(f"CI workflow check failed: {error}", file=sys.stderr)
        return 1
    print("CI workflows contain only pinned, test-only jobs")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
