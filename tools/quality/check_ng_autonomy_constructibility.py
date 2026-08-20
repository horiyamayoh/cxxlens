#!/usr/bin/env python3
"""Validate the closed constructibility witnesses for remaining high-risk work."""

from __future__ import annotations

import argparse
import pathlib
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
MODEL = pathlib.Path("schemas/cxxlens_ng_autonomy_constructibility.yaml")
SCHEMA = pathlib.Path("schemas/cxxlens_ng_autonomy_constructibility.schema.yaml")


class ConstructibilityError(ValueError):
    """A non-constructible or weakened high-risk model."""


def load(path: pathlib.Path) -> dict[str, Any]:
    try:
        value = yaml.safe_load(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeError, yaml.YAMLError) as error:
        raise ConstructibilityError(f"cannot load {path}: {error}") from error
    if not isinstance(value, dict):
        raise ConstructibilityError(f"expected mapping: {path}")
    return value


def require_exact(actual: Any, expected: Any, label: str) -> None:
    if actual != expected:
        raise ConstructibilityError(f"constructibility drift: {label}")


def validate(root: pathlib.Path) -> dict[str, Any]:
    model = load(root / MODEL)
    schema = load(root / SCHEMA)
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(model)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        raise ConstructibilityError(f"schema validation failed: {error.message}") from error
    machines = model["machines"]
    for name, machine in machines.items():
        authority = machine.get("authority")
        if not isinstance(authority, str) or not (root / authority).is_file():
            raise ConstructibilityError(f"missing authority: {name}")

    closure = machines["source_closure"]
    require_exact(closure["message_ids"], {"heartbeat": 23, "source_closure": [24, 25, 26, 27, 28, 29]}, "source-closure message registry")
    require_exact(closure["bounds"]["manifest_bytes"], 40 * 1024 * 1024, "source-closure manifest bound")
    require_exact(closure["bounds"]["spool_bytes"], 88 * 1024 * 1024, "source-closure spool bound")
    if set(closure["terminal_failures"]) != {"source-closure.rejected", "source-closure.cancelled", "provider.crash", "provider.connection-lost"}:
        raise ConstructibilityError("constructibility drift: source-closure terminal union")

    store = machines["store_candidate_report"]
    if store["candidate_states"].index("candidate-identity-sealed") >= store["candidate_states"].index("independently-validating"):
        raise ConstructibilityError("candidate identity is not sealed before validation")
    if store["report_states"].index("maximum-tail-reserved") >= store["report_states"].index("publication-attempt"):
        raise ConstructibilityError("report tail is not reserved before publication")
    require_exact(store["post_attempt_failure"], "exit-2-zero-authoritative-response-store-recovery-only", "post-attempt failure")
    if "publication-outcome-unknown" not in store["publication_outcomes"]:
        raise ConstructibilityError("publication outcome unknown was removed")
    require_exact(store["projections"]["comparison"], "separate-cursors-full-record-byte-exact", "dual projection")
    require_exact(store["bounds"], {"tasks": 4096, "scale_bytes": 512 * 1024 * 1024, "source_window_bytes": 64 * 1024 * 1024, "sort_arena_bytes": 8 * 1024 * 1024, "comparator_cursor_count": 2, "comparator_cursor_bytes_each": 32 * 1024, "merge_fan_in": 16, "merge_file_descriptors": 18, "report_bytes": 1024 * 1024 * 1024}, "DF-0200 numeric bounds")
    require_exact(store["representation"]["logical_write"], "cxxlens.ng-snapshot-payload.v5", "logical v5 write")
    require_exact(store["representation"]["chunk_bytes"], 8 * 1024 * 1024, "SQLite v3 chunk")

    mapping = machines["sqlite_read_mapping"]
    require_exact(mapping["nesting"], "#205-inside-#201-active-read-connection", "SQLite nesting")
    require_exact(mapping["no_effect_boundary"], "before-target-xOpen", "SQLite no-effect boundary")
    require_exact(mapping["predelegation_authority"], {"writer": "attempt-and-in-flight-pin-only", "reader": "fresh-active-writer-lease-page-support-pin-required-before-native"}, "predelegation authority")
    require_exact(mapping["teardown_order"], ["hide-generation", "seal-pre-callback-cut", "revoke-admission", "drain-callbacks-and-use-owners", "seal-owner-census", "native-unmap-deleteFlag-zero-then-close-once", "capture-zero-effect-outcomes", "retire-registry", "release-pins"], "SQLite teardown order")
    require_exact(mapping["zero_effect_receipt"], ["initialize", "create", "write", "truncate", "extend", "delete", "resize"], "SQLite zero-effect receipt")
    require_exact(mapping["read_receipt_barrier"], ["connection-closed", "zero-live-callbacks-leases-and-use-owners", "zero-effect-callback-receipt-sealed"], "SQLite read receipt barrier")
    require_exact(mapping["fork_machine"], {"prepare": "seal-admission-and-census", "parent": "revalidate-process-and-fork-generation", "child": "inherited-custody-quarantine-zero-native-cleanup-no-drain"}, "SQLite fork machine")
    require_exact(mapping["ambiguous_callback"], "permanent-quarantine-no-retry", "ambiguous callback")

    effect = machines["sqlite_normalization_effect"]
    require_exact(effect["separate_from_zero_effect_read"], True, "normalization isolation")
    require_exact(effect["entry"], "logical-read-receipt-exact-empty-after-connection-closed-zero-custody-zero-effect", "normalization entry")
    require_exact(effect["fixture_partition_machine"], {"F0": "live-receipt-to-fixture-normalizer", "FP": "cleanup-or-recovery-to-independently-revalidated-F0-new-live-receipt", "FH": "cleanup-or-recovery-to-independently-revalidated-F0-new-live-receipt", "FZ-pre": "coordination-WAL-cleanup-parent-sync-to-independently-revalidated-F0-new-live-receipt", "FI": "rollback-empty-fresh-anchor-only-never-completed-edge", "FZ-post": "rollback-empty-fresh-anchor-only-never-completed-edge", "FO": "rollback-empty-fresh-anchor-only-never-completed-edge"}, "DF-0202 partition machine")
    require_exact(effect["family_phase_machine"], ["pre-effect", "effect-admitted", "recoverable-interruption", "recrash-classified", "terminal-route"], "DF-0202 family phases")
    require_exact(effect["canonical_user_source_activation"], "prohibited", "production normalization activation")
    require_exact(effect["parent_sync_after_each_delete"], "required", "normalization parent sync")
    return model


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("command", choices=("check",))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    args = parser.parse_args()
    try:
        validate(args.root.resolve())
    except ConstructibilityError as error:
        print(f"autonomy-constructibility: {error}", file=sys.stderr)
        return 1
    print("autonomy-constructibility: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
