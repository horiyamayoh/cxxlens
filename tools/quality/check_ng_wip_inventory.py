#!/usr/bin/env python3
"""Snapshot and verify branch/worktree provenance without deleting branch refs."""

from __future__ import annotations

import argparse
import datetime
import json
import os
import pathlib
import subprocess
import sys
import urllib.error
import urllib.request
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
INVENTORY = pathlib.Path("schemas/cxxlens_ng_wip_inventory.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_wip_inventory.schema.yaml")
SELECTED_HEADS = {
    "9e8c30fe7c024b67e5b35a8b563c45be820f9e48": "selected-provenance",
    "7a0717be2472cea7e8fdaa267118e702bcdb0f0b": "historical-unreviewed",
}


class WipInventoryError(ValueError):
    """A WIP provenance loss or replacement."""


def git(root: pathlib.Path, *arguments: str, check: bool = True) -> str:
    result = subprocess.run(["git", "-C", str(root), *arguments], capture_output=True, text=True)
    if check and result.returncode != 0:
        raise WipInventoryError(f"git command failed: {' '.join(arguments)}")
    return result.stdout


def ancestor(root: pathlib.Path, head: str, main: str) -> bool:
    return subprocess.run(["git", "-C", str(root), "merge-base", "--is-ancestor", head, main], capture_output=True).returncode == 0


def parse_worktrees(root: pathlib.Path) -> list[dict[str, Any]]:
    blocks = git(root, "worktree", "list", "--porcelain").strip().split("\n\n")
    result: list[dict[str, Any]] = []
    for block in blocks:
        if not block:
            continue
        fields: dict[str, Any] = {"prunable": False}
        for line in block.splitlines():
            key, _, value = line.partition(" ")
            if key == "worktree":
                fields["path"] = value
            elif key == "HEAD":
                fields["head"] = value
            elif key == "branch":
                fields["branch"] = value.removeprefix("refs/heads/")
            elif key == "detached":
                fields["branch"] = None
            elif key == "prunable":
                fields["prunable"] = True
                fields["prunable_reason"] = value
        result.append(fields)
    return result


def snapshot(root: pathlib.Path) -> dict[str, Any]:
    main = git(root, "rev-parse", "main").strip()
    canonical = str(root.resolve())
    worktrees: list[dict[str, Any]] = []
    for entry in parse_worktrees(root):
        if entry["path"] == canonical:
            disposition = "moving-canonical"
        elif entry["prunable"]:
            disposition = "prunable-registration"
        elif entry["head"] in SELECTED_HEADS:
            disposition = SELECTED_HEADS[entry["head"]]
        elif ancestor(root, entry["head"], main):
            disposition = "merged"
        else:
            disposition = "historical-unreviewed"
        worktrees.append({**entry, "disposition": disposition})

    raw_refs = git(root, "for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads", "refs/remotes/origin")
    refs: list[dict[str, str]] = []
    for line in raw_refs.splitlines():
        ref, head = line.split(" ", 1)
        if ref in {"main", "origin/main", "origin/HEAD"}:
            disposition = "moving-canonical"
        elif head in SELECTED_HEADS:
            disposition = SELECTED_HEADS[head]
        elif ancestor(root, head, main):
            disposition = "merged"
        else:
            disposition = "historical-unreviewed"
        refs.append({"ref": ref, "head": head, "disposition": disposition})
    return {
        "schema": "cxxlens.ng-wip-inventory.v1",
        "document_version": "1.0.0",
        "captured_at": datetime.datetime.now(datetime.UTC).replace(microsecond=0).isoformat().replace("+00:00", "Z"),
        "source_revision": main,
        "normalization": {"prunable_registration_action": "prune-after-snapshot", "branch_deletion": "forbidden", "live_worktree_deletion": "forbidden", "status": "complete"},
        "worktrees": sorted(worktrees, key=lambda value: value["path"]),
        "refs": sorted(refs, key=lambda value: value["ref"]),
    }


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise WipInventoryError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise WipInventoryError(f"expected mapping: {path}")
    return value


def validate(root: pathlib.Path) -> dict[str, Any]:
    inventory = load(root / INVENTORY)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(inventory)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise WipInventoryError(f"schema validation failed: {error.message}") from error
    captured_refs = {entry["ref"]: entry for entry in inventory["refs"]}
    current_refs = {}
    for line in git(root, "for-each-ref", "--format=%(refname:short) %(objectname)", "refs/heads", "refs/remotes/origin").splitlines():
        ref, head = line.split(" ", 1)
        current_refs[ref] = head
    strict_live_repository = len(parse_worktrees(root)) > 1 or any(
        ref in current_refs and not ref.startswith("origin/") and ref != "main" and entry["disposition"] != "moving-canonical"
        for ref, entry in captured_refs.items()
    )
    for ref, entry in captured_refs.items():
        # Remote-tracking refs are mutable fetch views, not locally preserved
        # branches.  Selected remote provenance is checked by verify-connected
        # against GitHub; requiring every fetched origin/* SHA to remain equal
        # to an old snapshot makes an ordinary fetch look like branch loss.
        if ref.startswith("origin/"):
            continue
        if entry["disposition"] == "moving-canonical":
            continue
        if strict_live_repository and current_refs.get(ref) != entry["head"]:
            raise WipInventoryError(f"preserved branch ref missing or replaced: {ref}")
    captured_worktrees = {entry["path"]: entry for entry in inventory["worktrees"]}
    for entry in parse_worktrees(root):
        if pathlib.Path(entry["path"]).resolve() == root.resolve():
            continue
        captured = captured_worktrees.get(entry["path"])
        if captured is None or captured["head"] != entry["head"] or captured["branch"] != entry.get("branch"):
            raise WipInventoryError(f"live worktree is unregistered or replaced: {entry['path']}")
        if entry["prunable"]:
            raise WipInventoryError(f"prunable registration remains after normalization: {entry['path']}")
    for entry in inventory["worktrees"]:
        # A prunable registration is an explicit record of a worktree whose
        # gitdir/evidence may already be gone; requiring its historical object
        # to remain reachable would make the normalization state impossible to
        # represent after safe pruning.
        if (
            strict_live_repository
            and entry["disposition"] != "moving-canonical"
            and not entry.get("prunable", False)
        ):
            git(root, "cat-file", "-e", f"{entry['head']}^{{commit}}")
    return inventory


def _github_json(url: str, token: str) -> dict[str, Any]:
    request = urllib.request.Request(url, headers={"Accept": "application/vnd.github+json", "Authorization": f"Bearer {token}", "X-GitHub-Api-Version": "2022-11-28"})
    try:
        with urllib.request.urlopen(request, timeout=20) as response:
            value = json.load(response)
    except (OSError, urllib.error.URLError, json.JSONDecodeError) as error:
        raise WipInventoryError(f"connected WIP verification unavailable: {error}") from error
    if not isinstance(value, dict):
        raise WipInventoryError("connected WIP response is not an object")
    return value


def verify_connected(root: pathlib.Path, token: str) -> None:
    if not token:
        raise WipInventoryError("connected WIP verification requires a token")
    inventory = validate(root)
    refs = {entry["ref"]: entry["head"] for entry in inventory["refs"]}
    branch = _github_json("https://api.github.com/repos/horiyamayoh/cxxlens/git/ref/heads/agent/remaining-issues-full-implementation", token)
    if branch.get("object", {}).get("sha") != refs.get("origin/agent/remaining-issues-full-implementation"):
        raise WipInventoryError("selected WIP remote branch head was replaced")
    pull = _github_json("https://api.github.com/repos/horiyamayoh/cxxlens/pulls/353", token)
    if pull.get("head", {}).get("sha") != refs.get("origin/pr/353") or pull.get("state") != "closed":
        raise WipInventoryError("PR #353 provenance was replaced or reopened")


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("snapshot", "check", "verify-connected"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    root = args.root.resolve()
    try:
        if args.command == "snapshot":
            print(yaml.safe_dump(snapshot(root), sort_keys=False), end="")
        elif args.command == "verify-connected":
            verify_connected(root, os.environ.get("GITHUB_TOKEN", ""))
            print("wip-inventory: connected selected provenance ok")
        else:
            inventory = validate(root)
            print(f"wip-inventory: ok ({len(inventory['worktrees'])} worktrees, {len(inventory['refs'])} refs)")
    except WipInventoryError as error:
        print(f"wip-inventory: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
