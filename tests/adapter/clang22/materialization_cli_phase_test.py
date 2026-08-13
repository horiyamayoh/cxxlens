#!/usr/bin/env python3
"""Exercise the installed materializer's phase-authentic schema failure path."""

from __future__ import annotations

import argparse
import json
import pathlib
import subprocess


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--driver", required=True, type=pathlib.Path)
    args = parser.parse_args()

    # This is a valid request envelope but intentionally lacks the selected contract's
    # request-derived members.  The executable must classify the failure before request
    # authentication; otherwise the journal would incorrectly report request-binding.
    payload = json.dumps(
        {
            "schema": "cxxlens.clang22-materialization-request.v2",
            "request_version": "2.1.0",
        },
        separators=(",", ":"),
    ).encode("utf-8")
    completed = subprocess.run(
        [str(args.driver)],
        input=payload,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        check=False,
    )
    assert completed.returncode == 1, (completed.stdout, completed.stderr)
    assert completed.stderr == b""
    report = json.loads(completed.stdout)
    assert report["response_kind"] == "compact_failure"
    assert report["error"]["code"] == "materialization.request-invalid"
    assert report["error"]["phase"] == "request-schema"
    assert report["error"]["subject"] == "request-schema"
    assert report["binding"]["state"] == "raw-input-only"
    assert report["effects"]["publication_attempted"] is False
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
