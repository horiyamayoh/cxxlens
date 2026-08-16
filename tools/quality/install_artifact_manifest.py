#!/usr/bin/env python3
"""Create and verify a source/configuration/toolchain-bound install manifest."""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
SCHEMA = pathlib.Path("schemas/cxxlens_ng_install_artifact_manifest.schema.yaml")
OCCURRENCE_RELATIVE_PATH = pathlib.Path(
    "share/cxxlens/materialization/clang22/occurrence-v1.json"
)


class InstallArtifactError(ValueError):
    """An installed prefix does not match its immutable manifest."""


def digest_bytes(value: bytes) -> str:
    return "sha256:" + hashlib.sha256(value).hexdigest()


def canonical_digest(value: Any) -> str:
    return digest_bytes(
        json.dumps(value, ensure_ascii=False, sort_keys=True, separators=(",", ":")).encode(
            "utf-8"
        )
    )


def command_output(command: list[str], root: pathlib.Path) -> str:
    return subprocess.run(
        command, cwd=root, check=True, capture_output=True, text=True
    ).stdout.strip()


def source_identity(root: pathlib.Path) -> dict[str, str]:
    return {
        "revision": command_output(["git", "rev-parse", "HEAD"], root),
        "tree": command_output(["git", "rev-parse", "HEAD^{tree}"], root),
    }


def exact_source_identity(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 40
        and value == value.lower()
        and all(character in "0123456789abcdef" for character in value)
    )


def installed_occurrence_source(prefix: pathlib.Path) -> dict[str, str] | None:
    """Read the installed occurrence's source binding when this is a cxxlens prefix.

    The generic quality-ownership fixture intentionally uses a small arbitrary
    prefix without an occurrence manifest, so absence remains allowed here.
    Actual cxxlens installs always contain this fixed source-private witness.
    """

    path = prefix / OCCURRENCE_RELATIVE_PATH
    if not path.exists() and not path.is_symlink():
        return None
    if path.is_symlink() or not path.is_file():
        raise InstallArtifactError(
            f"installed occurrence provenance is not a regular file: {path}"
        )
    try:
        document = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, json.JSONDecodeError) as error:
        raise InstallArtifactError(
            f"installed occurrence provenance cannot be read: {path}: {error}"
        ) from error
    if not isinstance(document, dict):
        raise InstallArtifactError(
            f"installed occurrence provenance is not a JSON object: {path}"
        )
    source = {
        "revision": document.get("source_revision"),
        "tree": document.get("source_tree"),
    }
    if not all(exact_source_identity(value) for value in source.values()):
        raise InstallArtifactError(
            f"installed occurrence provenance has invalid revision/tree: {path}"
        )
    return {key: str(value) for key, value in source.items()}


def toolchain(compiler: pathlib.Path, root: pathlib.Path) -> dict[str, str]:
    resolved = compiler.resolve(strict=True)
    identity_lines = command_output([str(resolved), "--version"], root).splitlines()
    if not identity_lines:
        raise InstallArtifactError("compiler did not report an identity")
    value = {
        "compiler": str(resolved),
        "identity": identity_lines[0],
        "binary_digest": digest_bytes(resolved.read_bytes()),
    }
    value["digest"] = canonical_digest(value)
    return value


def prefix_files(prefix: pathlib.Path) -> list[dict[str, Any]]:
    if not prefix.is_dir():
        raise InstallArtifactError(f"installed prefix does not exist: {prefix}")
    entries = []
    for path in sorted(prefix.rglob("*")):
        if path.is_dir():
            continue
        relative = path.relative_to(prefix).as_posix()
        mode = path.lstat().st_mode & 0o7777
        if path.is_symlink():
            entries.append(
                {
                    "path": relative,
                    "kind": "symlink",
                    "mode": mode,
                    "digest": digest_bytes(os.readlink(path).encode("utf-8")),
                }
            )
        elif path.is_file():
            entries.append(
                {
                    "path": relative,
                    "kind": "file",
                    "mode": mode,
                    "digest": digest_bytes(path.read_bytes()),
                }
            )
        else:
            raise InstallArtifactError(f"unsupported installed artifact type: {relative}")
    if not entries:
        raise InstallArtifactError("installed prefix is empty")
    return entries


def build_manifest(
    root: pathlib.Path,
    prefix: pathlib.Path,
    compiler: pathlib.Path,
    configuration: str,
) -> dict[str, Any]:
    source = source_identity(root)
    occurrence_source = installed_occurrence_source(prefix)
    if occurrence_source is not None and occurrence_source != source:
        differences = ", ".join(
            f"{key} expected={source[key]} actual={occurrence_source[key]}"
            for key in ("revision", "tree")
            if occurrence_source[key] != source[key]
        )
        raise InstallArtifactError(
            "installed occurrence source provenance mismatch; refusing to create or "
            f"verify a stale artifact ({differences})"
        )
    files = prefix_files(prefix)
    document: dict[str, Any] = {
        "schema": "cxxlens.install-artifact-manifest.v1",
        "source": source,
        "configuration": configuration,
        "configuration_digest": canonical_digest(configuration),
        "toolchain": toolchain(compiler, root),
        "files": files,
        "prefix_digest": canonical_digest(files),
    }
    document["manifest_digest"] = canonical_digest(document)
    return document


def load_schema(root: pathlib.Path) -> dict[str, Any]:
    value = yaml.safe_load((root / SCHEMA).read_text(encoding="utf-8"))
    if not isinstance(value, dict):
        raise InstallArtifactError("install artifact schema is not a mapping")
    return value


def verify_manifest(
    root: pathlib.Path,
    prefix: pathlib.Path,
    compiler: pathlib.Path,
    configuration: str,
    document: dict[str, Any],
) -> None:
    jsonschema.Draft202012Validator(load_schema(root)).validate(document)
    expected = build_manifest(root, prefix, compiler, configuration)
    if document != expected:
        mismatches: list[str] = []
        for field in ("source", "configuration", "configuration_digest", "prefix_digest"):
            if document.get(field) != expected.get(field):
                if field == "source":
                    for identity in ("revision", "tree"):
                        actual_value = document.get("source", {}).get(identity)
                        expected_value = expected["source"][identity]
                        if actual_value != expected_value:
                            mismatches.append(
                                f"source.{identity} expected={expected_value} actual={actual_value}"
                            )
                else:
                    mismatches.append(
                        f"{field} expected={expected.get(field)} actual={document.get(field)}"
                    )
        actual_toolchain = document.get("toolchain", {})
        expected_toolchain = expected["toolchain"]
        for field in ("compiler", "identity", "binary_digest", "digest"):
            if actual_toolchain.get(field) != expected_toolchain[field]:
                mismatches.append(
                    f"toolchain.{field} expected={expected_toolchain[field]} "
                    f"actual={actual_toolchain.get(field)}"
                )

        expected_files = {row["path"]: row for row in expected["files"]}
        actual_files = {row["path"]: row for row in document.get("files", [])}
        missing = sorted(set(expected_files) - set(actual_files))
        extra = sorted(set(actual_files) - set(expected_files))
        changed = []
        for path in sorted(set(expected_files) & set(actual_files)):
            if expected_files[path] != actual_files[path]:
                changed.append(path)
        if missing or extra or changed:
            file_detail = (
                f"files expected={len(expected_files)} actual={len(actual_files)}"
            )
            if missing:
                file_detail += f" missing={missing[:4]}"
            if extra:
                file_detail += f" extra={extra[:4]}"
            if changed:
                file_detail += f" changed={changed[:4]}"
            mismatches.append(file_detail)
        if document.get("manifest_digest") != expected["manifest_digest"]:
            mismatches.append(
                "manifest_digest "
                f"expected={expected['manifest_digest']} "
                f"actual={document.get('manifest_digest')}"
            )
        if not mismatches:
            mismatches.append("document fields differ")
        raise InstallArtifactError(
            "installed artifact revision/tree/configuration/toolchain/file binding "
            f"mismatch: {'; '.join(mismatches)}"
        )


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("create", "verify"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--prefix", type=pathlib.Path, required=True)
    parser.add_argument("--compiler", type=pathlib.Path, required=True)
    parser.add_argument("--configuration", required=True)
    parser.add_argument("--manifest", type=pathlib.Path, required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.command == "create":
            document = build_manifest(
                root, args.prefix.resolve(), args.compiler, args.configuration
            )
            jsonschema.Draft202012Validator(load_schema(root)).validate(document)
            args.manifest.parent.mkdir(parents=True, exist_ok=True)
            args.manifest.write_text(
                json.dumps(document, ensure_ascii=False, indent=2, sort_keys=True) + "\n",
                encoding="utf-8",
            )
        else:
            document = json.loads(args.manifest.read_text(encoding="utf-8"))
            verify_manifest(
                root,
                args.prefix.resolve(),
                args.compiler,
                args.configuration,
                document,
            )
    except (
        InstallArtifactError,
        OSError,
        subprocess.CalledProcessError,
        json.JSONDecodeError,
        jsonschema.ValidationError,
        yaml.YAMLError,
    ) as error:
        print(f"install artifact manifest failed: {error}", file=sys.stderr)
        return 1
    print(f"install artifact manifest {args.command} passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
