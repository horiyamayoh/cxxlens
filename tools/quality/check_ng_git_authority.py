#!/usr/bin/env python3
"""Bind repository inputs to the regular-file blobs present at ``HEAD``."""

from __future__ import annotations

import hashlib
import pathlib
import re
import subprocess


HEX40 = re.compile(r"^[0-9a-f]{40}$")


class GitAuthorityError(ValueError):
    """A fail-closed repository authority binding error."""


def _fail(code: str, relative: str) -> None:
    raise GitAuthorityError(f"git-authority.{code}:{relative}")


def _working_tree_path(root: pathlib.Path, relative: str) -> pathlib.Path:
    root = root.resolve()
    candidate = root
    for component in relative.split("/"):
        candidate /= component
        try:
            if candidate.is_symlink():
                _fail("path-symlink", relative)
        except OSError as error:
            raise GitAuthorityError(
                f"git-authority.path-inspection-failed:{relative}"
            ) from error
    if not candidate.exists():
        _fail("path-missing", relative)
    return candidate


def _head_blob(root: pathlib.Path, relative: str) -> tuple[str, str, bytes]:
    try:
        tree = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "--literal-pathspecs",
                "ls-tree",
                "-z",
                "--full-tree",
                "HEAD",
                "--",
                relative,
            ],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise GitAuthorityError("git-authority.head-tree-unavailable") from error

    records = [record for record in tree.stdout.split(b"\0") if record]
    if len(records) != 1:
        _fail("path-not-tracked", relative)
    try:
        metadata, path_bytes = records[0].split(b"\t", 1)
        mode_bytes, type_bytes, blob_bytes = metadata.split(b" ", 2)
        actual_path = path_bytes.decode("utf-8")
        mode = mode_bytes.decode("ascii")
        object_type = type_bytes.decode("ascii")
        blob = blob_bytes.decode("ascii")
    except (UnicodeDecodeError, ValueError) as error:
        raise GitAuthorityError(
            f"git-authority.head-tree-entry-invalid:{relative}"
        ) from error
    if actual_path != relative or not HEX40.fullmatch(blob):
        _fail("path-not-tracked", relative)
    if object_type != "blob":
        _fail("path-not-blob", relative)
    if mode == "120000":
        _fail("path-symlink", relative)
    if mode not in {"100644", "100755"}:
        _fail("path-not-regular-blob", relative)

    try:
        content = subprocess.run(
            ["git", "-C", str(root), "cat-file", "blob", blob],
            check=True,
            capture_output=True,
        ).stdout
    except (OSError, subprocess.CalledProcessError) as error:
        raise GitAuthorityError(
            f"git-authority.blob-content-unavailable:{relative}"
        ) from error
    return mode, blob, content


def _require_normal_index_entry(root: pathlib.Path, relative: str) -> None:
    try:
        index = subprocess.run(
            [
                "git",
                "-C",
                str(root),
                "--literal-pathspecs",
                "ls-files",
                "-v",
                "--error-unmatch",
                "--",
                relative,
            ],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError) as error:
        raise GitAuthorityError(
            f"git-authority.index-entry-unavailable:{relative}"
        ) from error
    records = index.stdout.splitlines()
    if len(records) != 1 or not records[0]:
        _fail("path-not-tracked", relative)
    marker = chr(records[0][0])
    if marker == "H":
        return
    if marker.lower() == "h":
        _fail("path-assume-unchanged", relative)
    if marker.lower() == "s":
        _fail("path-skip-worktree", relative)
    _fail("path-index-flag", relative)


def bind_head_blob(root: pathlib.Path, relative: str) -> tuple[str, str, bytes]:
    """Return the exact regular ``HEAD`` mode/blob after checking the worktree.

    The current path must exist without symlink traversal and must contain the
    same bytes as the blob selected from ``HEAD``.  The index entry must also
    be a normal entry, so assume-unchanged and skip-worktree cannot hide a
    mutation from the authority binding.
    """
    if (
        not relative
        or relative.startswith(("/", "\\"))
        or "\\" in relative
        or "\x00" in relative
        or any(component in {"", ".", "..", ".git"} for component in relative.split("/"))
    ):
        _fail("path-noncanonical", relative)
    candidate = _working_tree_path(root, relative)
    mode, blob, content = _head_blob(root, relative)
    if not candidate.is_file():
        _fail("path-not-regular-file", relative)
    try:
        working_tree_content = candidate.read_bytes()
    except (OSError, UnicodeError) as error:
        raise GitAuthorityError(
            f"git-authority.path-content-unavailable:{relative}"
        ) from error
    if working_tree_content != content:
        _fail("path-content-mismatch", relative)
    _require_normal_index_entry(root, relative)
    return mode, blob, content


def sha256_digest(content: bytes) -> str:
    return "sha256:" + hashlib.sha256(content).hexdigest()


def require_head_bound_paths(root: pathlib.Path, paths: list[str] | tuple[str, ...]) -> None:
    for relative in paths:
        bind_head_blob(root, relative)
