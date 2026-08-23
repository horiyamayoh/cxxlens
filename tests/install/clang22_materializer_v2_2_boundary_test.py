#!/usr/bin/env python3
"""Exercise the installed Protocol 2.0 materializer ingress boundary.

Protocol 2.0 requests carry source metadata and authenticated closure references;
source bytes are transferred through the dedicated source-closure channel.  The
installed executable currently exposes only the single-document stdin boundary,
so a valid metadata-only request must fail closed before worker or Store effects.
"""

from __future__ import annotations

import argparse
import os
import pathlib
import subprocess
import sys
import tempfile


OCCURRENCE_RELATIVE_PATH = "share/cxxlens/materialization/clang22/occurrence-v1.json"


def fail(message: str) -> None:
    raise AssertionError(message)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", required=True, type=pathlib.Path)
    parser.add_argument("--prefix", required=True, type=pathlib.Path)
    parser.add_argument("--executable-suffix", default="")
    return parser.parse_args()


def resolve_executable(prefix: pathlib.Path, relative: str, suffix: str) -> pathlib.Path:
    candidate = prefix.joinpath(*pathlib.PurePosixPath(relative).parts)
    return candidate.with_name(candidate.name + suffix)


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    prefix = args.prefix.resolve()
    sys.path.insert(0, str(root / "tools" / "quality"))

    import check_ng_clang22_materialization as materialization  # noqa: PLC0415
    import check_ng_source_closure_transport as transport  # noqa: PLC0415

    occurrence_path = prefix / OCCURRENCE_RELATIVE_PATH
    if not occurrence_path.is_file():
        fail(f"installed occurrence manifest is missing: {occurrence_path}")
    occurrence = materialization.load_strict_json_bytes(
        occurrence_path.read_bytes(), "installed occurrence manifest"
    )
    materialization.validate_occurrence_manifest(root, occurrence)

    request, manifest = transport.complete_request_witness(root)
    transport.validate_request_binding(request, [manifest])
    request_bytes = materialization.canonical_json(request)
    if b"content_base64" in request_bytes or b"source_bytes" in request_bytes:
        fail("Protocol 2.0 request embedded source bytes")
    if request["schema"] != "cxxlens.clang22-materialization-request.v2_2":
        fail("installed boundary witness is not request v2.2")
    if request["request_version"] != "2.2.0":
        fail("installed boundary witness is not request version 2.2")
    if request["worker"]["protocol_major"] != 2 or request["worker"]["protocol_minor"] != 0:
        fail("installed boundary witness is not Protocol 2.0")

    materializer = resolve_executable(
        prefix, "bin/cxxlens-clang22-materialize", args.executable_suffix
    )
    if not materializer.is_file() or materializer.is_symlink():
        fail(f"installed materializer is missing or not regular: {materializer}")

    environment = dict(os.environ)
    environment.pop("LD_LIBRARY_PATH", None)
    environment.pop("DYLD_LIBRARY_PATH", None)
    with tempfile.TemporaryDirectory(prefix="clang22-materializer-v2-2-") as directory:
        working_directory = pathlib.Path(directory)
        completed = subprocess.run(
            [str(materializer)],
            input=request_bytes,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            cwd=working_directory,
            env=environment,
            check=False,
        )
        if (working_directory / "materialization.sqlite").exists():
            fail("metadata-only request created a Store file before closure transfer")
        residual = [path for path in working_directory.iterdir()]
        if residual:
            fail(f"metadata-only request left pre-transfer effects: {residual}")

    if completed.returncode != 1:
        fail(
            "installed materializer did not reject disconnected source closure "
            f"before execution: returncode={completed.returncode}, "
            f"stdout={completed.stdout[:1000]!r}, stderr={completed.stderr[:1000]!r}"
        )
    if completed.stderr:
        fail(
            "disconnected source-closure ingress wrote diagnostics to stderr: "
            f"stderr={completed.stderr[:1000]!r}"
        )
    report = materialization.load_strict_json_bytes(
        completed.stdout, "Protocol 2.0 disconnected source-closure response"
    )
    materialization.validate_report(
        root, None, report, request_bytes=request_bytes
    )
    if completed.stdout != materialization.canonical_json(report) + b"\n":
        fail("disconnected source-closure response is not canonical JSON")
    if report["error"] != {
        "code": "materialization.request-invalid",
        "phase": "request-schema",
        "subject": "request-v2_2",
        "diagnostic": (
            "source-code=materialization.source-closure-invalid;"
            "source-detail=closure-transport-not-connected"
        ),
    }:
        fail(f"unexpected disconnected source-closure error: {report['error']!r}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
