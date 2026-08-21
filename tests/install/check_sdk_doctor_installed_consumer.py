#!/usr/bin/env python3
"""Verify the installed SDK-doctor capability projection as a consumer would."""

from __future__ import annotations

import json
import os
import pathlib
import subprocess
import sys
import tempfile


def run(executable: str, *arguments: str, expected: int = 0) -> tuple[str, str]:
    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    completed = subprocess.run(
        [executable, *arguments],
        check=False,
        capture_output=True,
        text=True,
        env=environment,
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"unexpected exit {completed.returncode} (expected {expected}) for {arguments}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    return completed.stdout, completed.stderr


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_sdk_doctor_installed_consumer.py <cxxlens-sdk-doctor>", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="cxxlens-installed-sdk-doctor-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project.json"
        project.write_text(
            json.dumps(
                {
                    "schema": "cxxlens.agent-capability-resolution.v1",
                    "use_case_id": "installed.demo.v1",
                    "capability_path": [
                        {"id": "input.source", "kind": "input", "state": "proved"},
                        {
                            "id": "provider.semantic",
                            "kind": "provider",
                            "state": "unknown",
                            "requires": ["input.source"],
                            "reason_code": "provider.missing",
                            "owner_issue": "#261",
                            "action": "install-provider",
                        },
                        {
                            "id": "recipe.explain",
                            "kind": "recipe",
                            "state": "proved",
                            "requires": ["provider.semantic"],
                        },
                    ],
                }
            ),
            encoding="utf-8",
        )

        output, _ = run(
            executable,
            "capability",
            "installed.demo.v1",
            "--project",
            str(project),
        )
        report = json.loads(output)
        assert report["schema"] == "cxxlens.agent-capability-resolution.v1"
        assert report["role"] == "sdk-doctor-capability-resolution"
        assert report["state"] == "partial"
        assert report["result"]["state"] == "partial"
        assert report["result"]["coverage"] == {
            "proved": 1,
            "required": 3,
            "unresolved": 2,
        }
        assert [
            row["id"] for row in report["result"]["capabilities"]
        ] == ["input.source", "provider.semantic", "recipe.explain"]
        assert report["result"]["completion_plan"][0]["reason_code"] == "provider.missing"

        markdown, _ = run(
            executable,
            "capability",
            "installed.demo.v1",
            "--project",
            str(project),
            "--format",
            "markdown",
        )
        assert "state: `partial`" in markdown
        assert "`provider.semantic`" in markdown
        assert "provider.missing" in markdown

        report_path = root / "report.json"
        report_path.write_text(output, encoding="utf-8")
        explained, _ = run(executable, "explain", str(report_path))
        explained_report = json.loads(explained)
        assert explained_report["mode"] == "explain"
        assert explained_report["state"] == report["state"]
        assert explained_report["result"]["completion_plan"] == report["result"]["completion_plan"]

        missing, _ = run(executable, "missing", "--project", str(project))
        missing_report = json.loads(missing)
        assert missing_report["mode"] == "missing"
        assert missing_report["state"] == report["state"]
        assert missing_report["result"]["missing"] == report["result"]["missing"]

        _, stderr = run(
            executable,
            "capability",
            "installed.demo.v1",
            "--project",
            str(project),
            "--format",
            "invalid",
            expected=2,
        )
        assert "sdk.capability-option-invalid" in stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
