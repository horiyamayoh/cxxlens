#!/usr/bin/env python3
"""Bind repository inputs to one immutable regular-file ``HEAD`` snapshot."""

from __future__ import annotations

import hashlib
import errno
import os
import pathlib
import re
import stat
import subprocess
from dataclasses import dataclass


HEX40 = re.compile(r"^[0-9a-f]{40}$")
READ_CHUNK_SIZE = 1024 * 1024


class GitAuthorityError(ValueError):
    """A fail-closed repository authority binding error."""


def _fail(code: str, relative: str) -> None:
    raise GitAuthorityError(f"git-authority.{code}:{relative}")


@dataclass(frozen=True)
class HeadSnapshot:
    """One exact commit/tree pair used for every authority lookup."""

    revision: str
    tree: str

    @classmethod
    def capture(cls, root: pathlib.Path) -> "HeadSnapshot":
        try:
            revision_result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "rev-parse",
                    "--verify",
                    "HEAD^{commit}",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            revision = revision_result.stdout.strip()
            tree_result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "rev-parse",
                    "--verify",
                    f"{revision}^{{tree}}",
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            tree = tree_result.stdout.strip()
        except (OSError, subprocess.CalledProcessError) as error:
            raise GitAuthorityError("git-authority.head-snapshot-unavailable") from error
        if not HEX40.fullmatch(revision) or not HEX40.fullmatch(tree):
            raise GitAuthorityError("git-authority.head-snapshot-invalid")
        return cls(revision=revision, tree=tree)

    @classmethod
    def capture_expected(
        cls,
        root: pathlib.Path,
        *,
        revision: str,
        tree: str,
    ) -> "HeadSnapshot":
        snapshot = cls.capture(root)
        if (snapshot.revision, snapshot.tree) != (revision, tree):
            raise GitAuthorityError(
                "git-authority.head-snapshot-mismatch:"
                f"{revision}:{tree}->{snapshot.revision}:{snapshot.tree}"
            )
        return snapshot

    def assert_current(self, root: pathlib.Path) -> None:
        current = type(self).capture(root)
        if current != self:
            raise GitAuthorityError(
                "git-authority.head-snapshot-changed:"
                f"{self.revision}:{self.tree}->{current.revision}:{current.tree}"
            )

    def tracked_paths(
        self, root: pathlib.Path, prefix: str
    ) -> tuple[str, ...]:
        """List paths from this tree, never from the mutable worktree/index."""
        try:
            result = subprocess.run(
                [
                    "git",
                    "-C",
                    str(root),
                    "ls-tree",
                    "-r",
                    "-z",
                    "--name-only",
                    self.tree,
                    "--",
                    prefix,
                ],
                check=True,
                capture_output=True,
            )
        except (OSError, subprocess.CalledProcessError) as error:
            raise GitAuthorityError(
                f"git-authority.head-snapshot-paths-unavailable:{prefix}"
            ) from error
        return tuple(
            path.decode("utf-8")
            for path in result.stdout.split(b"\0")
            if path
        )


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


def _safe_open_flags(*, directory: bool) -> int:
    required = ["O_CLOEXEC", "O_NOFOLLOW"]
    if directory:
        required.append("O_DIRECTORY")
    else:
        # O_RDONLY alone can block forever when an authority path is replaced
        # with a FIFO before fstat() gets a chance to reject it.
        required.append("O_NONBLOCK")
    missing = [name for name in required if not hasattr(os, name)]
    if missing:
        raise GitAuthorityError(
            "git-authority.safe-descriptor-unavailable:" + ",".join(missing)
        )
    flags = os.O_RDONLY
    for name in required:
        flags |= getattr(os, name)
    return flags


def _descriptor_error(relative: str, error: OSError) -> GitAuthorityError:
    if error.errno == errno.ELOOP:
        code = "path-symlink"
    elif error.errno in {errno.ENOENT, errno.ENOTDIR}:
        code = "path-missing"
    elif error.errno in {errno.EACCES, errno.EPERM}:
        code = "path-open-denied"
    else:
        code = "path-open-failed"
    return GitAuthorityError(f"git-authority.{code}:{relative}")


def _file_identity(metadata: os.stat_result) -> tuple[int, int, int, int, int, int]:
    return (
        metadata.st_dev,
        metadata.st_ino,
        stat.S_IMODE(metadata.st_mode),
        metadata.st_size,
        metadata.st_mtime_ns,
        metadata.st_ctime_ns,
    )


def _require_regular_mode(
    metadata: os.stat_result, expected_mode: str, relative: str
) -> None:
    if not stat.S_ISREG(metadata.st_mode):
        _fail("path-not-regular-file", relative)
    if stat.S_IMODE(metadata.st_mode) != stat.S_IMODE(int(expected_mode, 8)):
        _fail("path-mode-mismatch", relative)


def _read_bound_worktree_file(
    root: pathlib.Path, relative: str, expected_mode: str
) -> bytes:
    """Read one path through stable descriptors and rebind its final identity.

    The bytes returned by this function are the only worktree value callers may
    parse or digest.  The pathname is used only for descriptor lookup and the
    final identity check; it is never re-read by an authority consumer.
    """
    root = root.resolve()
    parts = relative.split("/")
    directory_flags = _safe_open_flags(directory=True)
    file_flags = _safe_open_flags(directory=False)
    directory_fds: list[int] = []
    leaf_fd: int | None = None
    try:
        try:
            current_fd = os.open(str(root), directory_flags)
        except OSError as error:
            raise _descriptor_error(relative, error) from error
        directory_fds.append(current_fd)

        for component in parts[:-1]:
            try:
                current_fd = os.open(
                    component, directory_flags, dir_fd=current_fd
                )
            except OSError as error:
                raise _descriptor_error(relative, error) from error
            directory_fds.append(current_fd)

        parent_fd = directory_fds[-1]
        leaf_name = parts[-1]
        try:
            leaf_fd = os.open(leaf_name, file_flags, dir_fd=parent_fd)
        except OSError as error:
            raise _descriptor_error(relative, error) from error

        try:
            before = os.fstat(leaf_fd)
        except OSError as error:
            raise GitAuthorityError(
                f"git-authority.path-stat-failed:{relative}"
            ) from error
        _require_regular_mode(before, expected_mode, relative)
        before_identity = _file_identity(before)

        chunks: list[bytes] = []
        while True:
            try:
                chunk = os.read(leaf_fd, READ_CHUNK_SIZE)
            except OSError as error:
                raise GitAuthorityError(
                    f"git-authority.path-content-unavailable:{relative}"
                ) from error
            if not chunk:
                break
            chunks.append(chunk)
        content = b"".join(chunks)

        try:
            after = os.fstat(leaf_fd)
        except OSError as error:
            raise GitAuthorityError(
                f"git-authority.path-stat-failed:{relative}"
            ) from error
        _require_regular_mode(after, expected_mode, relative)
        if _file_identity(after) != before_identity:
            _fail("path-replaced-during-read", relative)
        if len(content) != before.st_size:
            _fail("path-size-changed-during-read", relative)

        # Rebind the name from the already-open parent directory.  This catches
        # a replacement that happened after the read but before the final check.
        rebound_fd: int | None = None
        try:
            try:
                rebound_fd = os.open(leaf_name, file_flags, dir_fd=parent_fd)
            except OSError as error:
                raise _descriptor_error(relative, error) from error
            rebound = os.fstat(rebound_fd)
            _require_regular_mode(rebound, expected_mode, relative)
            if _file_identity(rebound) != before_identity:
                _fail("path-replaced", relative)
        finally:
            if rebound_fd is not None:
                try:
                    os.close(rebound_fd)
                except OSError:
                    pass

        try:
            rebound_path = os.stat(
                root.joinpath(*parts), follow_symlinks=False
            )
        except OSError as error:
            raise _descriptor_error(relative, error) from error
        if stat.S_ISLNK(rebound_path.st_mode):
            _fail("path-symlink", relative)
        _require_regular_mode(rebound_path, expected_mode, relative)
        if _file_identity(rebound_path) != before_identity:
            _fail("path-replaced", relative)
        return content
    finally:
        if leaf_fd is not None:
            try:
                os.close(leaf_fd)
            except OSError:
                pass
        for descriptor in reversed(directory_fds):
            try:
                os.close(descriptor)
            except OSError:
                pass


def _head_blob(
    root: pathlib.Path, snapshot: HeadSnapshot, relative: str
) -> tuple[str, str, bytes]:
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
                snapshot.tree,
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


def bind_head_blob(
    root: pathlib.Path,
    relative: str,
    *,
    snapshot: HeadSnapshot | None = None,
) -> tuple[str, str, bytes]:
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
    owned_snapshot = snapshot is None
    snapshot = snapshot or HeadSnapshot.capture(root)
    # Keep this preflight for a precise diagnostic, but never rely on the
    # pathname check for authority: the descriptor-bound read below is the
    # authoritative worktree observation.
    _working_tree_path(root, relative)
    mode, blob, content = _head_blob(root, snapshot, relative)
    working_tree_content = _read_bound_worktree_file(root, relative, mode)
    if working_tree_content != content:
        _fail("path-content-mismatch", relative)
    _require_normal_index_entry(root, relative)
    if owned_snapshot:
        snapshot.assert_current(root)
    return mode, blob, content


def sha256_digest(content: bytes) -> str:
    return "sha256:" + hashlib.sha256(content).hexdigest()


def require_head_bound_paths(
    root: pathlib.Path,
    paths: list[str] | tuple[str, ...],
    *,
    snapshot: HeadSnapshot | None = None,
    verify_current: bool = True,
) -> dict[str, bytes]:
    return {
        relative: record[2]
        for relative, record in require_head_bound_records(
            root,
            paths,
            snapshot=snapshot,
            verify_current=verify_current,
        ).items()
    }


def require_head_bound_records(
    root: pathlib.Path,
    paths: list[str] | tuple[str, ...],
    *,
    snapshot: HeadSnapshot | None = None,
    verify_current: bool = True,
) -> dict[str, tuple[str, str, bytes]]:
    """Bind all paths to one snapshot and retain exact mode/blob/bytes.

    Callers may bind a discovery prefix with ``verify_current=False`` and then
    bind the manifest-declared remainder using the same snapshot.  The final
    call must verify the snapshot after every authoritative byte has been
    bound; no path is resolved against a moving ``HEAD`` independently.
    """
    snapshot = snapshot or HeadSnapshot.capture(root)
    bound: dict[str, tuple[str, str, bytes]] = {}
    for relative in paths:
        bound[relative] = bind_head_blob(root, relative, snapshot=snapshot)
    if verify_current:
        snapshot.assert_current(root)
    return bound
