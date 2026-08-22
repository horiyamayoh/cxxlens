#!/ usr / bin / env python3
"""Exercise the installed-facing sdk-doctor capability-resolution commands."""

from __future__ import annotations

import json
import pathlib
import subprocess
import sys
import tempfile


def run(executable: str, *arguments: str, expected: int = 0) -> tuple[str, str]:
    completed = subprocess.run(
        [executable, *arguments], check=False, capture_output=True, text=True
    )
    if completed.returncode != expected:
        raise AssertionError(
            f"unexpected exit {completed.returncode} (expected {expected}) for {arguments}\n"
            f"stdout={completed.stdout}\nstderr={completed.stderr}"
        )
    return completed.stdout, completed.stderr


def main() -> int:
    if len(sys.argv) != 2:
        print("usage: check_sdk_doctor_capability.py <cxxlens-sdk-doctor>", file=sys.stderr)
        return 2
    executable = sys.argv[1]
    with tempfile.TemporaryDirectory(prefix="cxxlens-sdk-doctor-") as temporary:
        root = pathlib.Path(temporary)
        project = root / "project.json"
        project.write_text(
            json.dumps(
                {
                    "schema": "cxxlens.agent-capability-resolution.v1",
                    "use_case": "demo.explain.v1",
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
                    "coverage": {"closure": "partial"},
                    "provenance": {"revision": "test-revision"},
                }
            ),
            encoding="utf-8",
        )

        output, _ = run(
            executable,
            "capability",
            "demo.explain.v1",
            "--project",
            str(project),
        )
        report = json.loads(output)
        assert report["schema"] == "cxxlens.agent-capability-resolution.v1"
        assert report["authority"]["status"] == "unbound"
        assert report["authority"]["stale_policy"] == "reject"
        assert report["authority"]["source"] == "schemas/cxxlens_ng_api_development_readiness.yaml"
        assert report["state"] == "partial"
        assert report["result"]["coverage"] == {"proved": 1, "required": 3, "unresolved": 2}
        assert [
            row["capability"] for row in report["result"]["completion_plan"]
        ] == ["provider.semantic", "recipe.explain"]
        assert report["result"]["missing"][0]["reason_code"] == "provider.missing"

        markdown, _ = run(
            executable,
            "capability",
            "demo.explain.v1",
            "--project",
            str(project),
            "--format",
            "markdown",
        )
        assert "state: `partial`" in markdown
        assert "`provider.semantic`" in markdown

        report_path = root / "report.json"
        report_path.write_text(output, encoding="utf-8")
        explained, _ = run(executable, "explain", str(report_path))
        explained_document = json.loads(explained)
        assert explained_document["mode"] == "explain"
        assert explained_document["state"] == report["state"]
        assert explained_document["result"]["completion_plan"] == report["result"]["completion_plan"]

        missing, _ = run(executable, "missing", "--project", str(project))
        missing_document = json.loads(missing)
        assert missing_document["mode"] == "missing"
        assert missing_document["state"] == "partial"

        # A packet may become canonical only after it carries the exact source
        # authority emitted by this executable.  The status is intentionally
        # explicit: the legacy fixture above remains an unbound diagnostic.
        authority_output, _ = run(executable, "missing", "cc.call_site.v1")
        authority = json.loads(authority_output)["authority"]
        authority["status"] = "bound"
        bound_project = root / "bound-project.json"
        bound_project.write_text(
            json.dumps(
                {
                    "schema": "cxxlens.agent-capability-resolution.v1",
                    "use_case": "bound.demo.v1",
                    "authority": authority,
                    "capability_path": [
                        {"id": "input.source", "kind": "input", "state": "proved"}
                    ],
                }
            ),
            encoding="utf-8",
        )
        bound_output, _ = run(
            executable,
            "capability",
            "bound.demo.v1",
            "--project",
            str(bound_project),
        )
        bound_report = json.loads(bound_output)
        assert bound_report["authority"]["status"] == "bound"
        assert bound_report["authority"]["revision"] == authority["revision"]
        assert bound_report["authority"]["tree"] == authority["tree"]
        bound_report_path = root / "bound-report.json"
        bound_report_path.write_text(bound_output, encoding="utf-8")
        explained_bound, _ = run(executable, "explain", str(bound_report_path))
        assert json.loads(explained_bound)["authority"]["status"] == "bound"
        missing_bound, _ = run(executable, "missing", "--project", str(bound_project))
        assert json.loads(missing_bound)["authority"]["status"] == "bound"

        stale_project = root / "stale-project.json"
        stale_authority = dict(authority)
        stale_authority["revision"] = "0" * 40
        stale_project.write_text(
            json.dumps(
                {
                    "schema": "cxxlens.agent-capability-resolution.v1",
                    "use_case": "bound.demo.v1",
                    "authority": stale_authority,
                    "capability_path": [
                        {"id": "input.source", "kind": "input", "state": "proved"}
                    ],
                }
            ),
            encoding="utf-8",
        )
        _, stale_stderr = run(
            executable,
            "capability",
            "bound.demo.v1",
            "--project",
            str(stale_project),
            expected=1,
        )
        assert "sdk.capability-authority-stale" in stale_stderr

        _, stderr = run(
            executable,
            "capability",
            "demo.explain.v1",
            "--project",
            str(project),
            "--format",
            "invalid",
            expected=2,
        )
        assert "sdk.capability-option-invalid" in stderr

#Existing commands remain byte / exit compatible and relation presence stays
#separate from the project capability diagnosis.
        inspect, _ = run(executable, "inspect")
        assert '"mode":"inspect"' in inspect
        relation_presence, _ = run(executable, "missing", "cc.call_site.v1")
        assert json.loads(relation_presence)["status"] == "complete"

        cycle = root / "cycle.json"
        cycle.write_text(
            json.dumps(
                {
                    "schema": "cxxlens.agent-capability-resolution.v1",
                    "use_case": "cycle.v1",
                    "capability_path": [
                        {"id": "a", "state": "proved", "requires": ["b"]},
                        {"id": "b", "state": "proved", "requires": ["a"]},
                    ],
                }
            ),
            encoding="utf-8",
        )
        _, cycle_stderr = run(
            executable,
            "capability",
            "cycle.v1",
            "--project",
            str(cycle),
            expected=1,
        )
        assert "agent.capability-cycle" in cycle_stderr
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
