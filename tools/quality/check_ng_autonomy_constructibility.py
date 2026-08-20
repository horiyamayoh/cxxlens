#!/usr/bin/env python3
"""Validate the closed constructibility witnesses for remaining high-risk work."""

from __future__ import annotations

import argparse
import hashlib
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


def validate_closed_graph(
    graph: dict[str, Any], initial: str, external_terminals: set[str] | None = None
) -> None:
    external = external_terminals or set()
    reachable: set[str] = set()
    pending = [initial]
    while pending:
        state = pending.pop()
        if state in reachable or state in external:
            continue
        if state not in graph:
            raise ConstructibilityError(f"graph target has no state: {state}")
        reachable.add(state)
        targets = graph[state]
        targets = [targets] if isinstance(targets, str) else targets
        if not isinstance(targets, list) or not all(
            isinstance(target, str) for target in targets
        ):
            raise ConstructibilityError(f"graph transition is not closed: {state}")
        pending.extend(targets)
    if reachable != set(graph):
        raise ConstructibilityError("graph contains unreachable state")


def execute_store_symbolic_witness(store: dict[str, Any]) -> None:
    expected_records = [("claim", "a", b"alpha"), ("claim", "b", b"beta")]
    backend_rows = [
        {"ordinal": 0, "kind": "claim", "key": "a", "payload": b"alpha"},
        {"ordinal": 1, "kind": "claim", "key": "b", "payload": b"beta"},
    ]
    actual_records = [
        (row["kind"], row["key"], row["payload"])
        for row in sorted(backend_rows, key=lambda row: row["ordinal"])
    ]

    def frame(record: tuple[str, str, bytes]) -> bytes:
        body = record[0].encode() + b"\0" + record[1].encode() + b"\0" + record[2]
        return len(body).to_bytes(8, "big") + body + hashlib.sha256(body).digest()

    def projections_equal(
        actual: list[tuple[str, str, bytes]],
        expected: list[tuple[str, str, bytes]],
    ) -> bool:
        return len(actual) == len(expected) and all(
            frame(left) == frame(right)
            for left, right in zip(actual, expected, strict=True)
        )

    if not projections_equal(actual_records, expected_records):
        raise ConstructibilityError("Store symbolic dual projection mismatch")
    adversarial_actuals = [
        actual_records[:-1],
        [actual_records[0], actual_records[0], actual_records[1]],
        list(reversed(actual_records)),
        [actual_records[0], ("claim", "b", b"BETA")],
    ]
    if any(projections_equal(candidate, expected_records) for candidate in adversarial_actuals):
        raise ConstructibilityError("Store symbolic projection counterexample accepted")

    expected_policy = {
        "not-attempted": {"response": "typed-failure", "exit": 1, "recovery_authority": "none"},
        "rejected-stale": {"response": "typed-publication-conflict", "exit": 1, "recovery_authority": "Store"},
        "rejected-store-failure": {"response": "typed-store-failure", "exit": 1, "recovery_authority": "Store"},
        "publication-outcome-unknown": {"response": "none", "exit": 2, "recovery_authority": "Store"},
        "committed-unverified": {"response": "safe-committed-unverified-detail-or-none", "exit": "0-or-2", "recovery_authority": "Store"},
        "committed-verified": {"response": "validated-success", "exit": 0, "recovery_authority": "Store"},
    }
    require_exact(store["outcome_policy"], expected_policy, "Store outcome policy")
    if set(store["outcome_policy"]) != set(store["publication_outcomes"]):
        raise ConstructibilityError("Store outcome policy census mismatch")

    bounds = store["bounds"]
    def checked_add(left: int, right: int, maximum: int) -> int:
        if left < 0 or right < 0 or left > maximum - right:
            raise ConstructibilityError("Store symbolic checked arithmetic overflow")
        return left + right

    def checked_multiply(left: int, right: int, maximum: int) -> int:
        if left < 0 or right < 0 or (right and left > maximum // right):
            raise ConstructibilityError("Store symbolic checked arithmetic overflow")
        return left * right

    cursor_bytes = bounds["comparator_cursor_count"] * bounds["comparator_cursor_bytes_each"]
    resident = checked_add(
        checked_add(bounds["source_window_bytes"], bounds["sort_arena_bytes"], bounds["counter_max"]),
        cursor_bytes,
        bounds["counter_max"],
    )
    if resident != bounds["resident_window_limit_bytes"]:
        raise ConstructibilityError("Store symbolic bounded-window witness invalid")
    if bounds["merge_file_descriptors"] != bounds["merge_fan_in"] + 2:
        raise ConstructibilityError("Store symbolic merge descriptor witness invalid")
    admitted_scale_accumulator = checked_add(
        checked_multiply(
            bounds["tasks"],
            bounds["counter_max"],
            bounds["accumulator_max"],
        ),
        bounds["scale_bytes"],
        bounds["accumulator_max"],
    )
    if admitted_scale_accumulator <= bounds["counter_max"]:
        raise ConstructibilityError("Store symbolic u128 accumulator witness invalid")
    try:
        checked_add(bounds["counter_max"], 1, bounds["counter_max"])
    except ConstructibilityError:
        pass
    else:
        raise ConstructibilityError("Store symbolic overflow counterexample accepted")
    try:
        checked_multiply(bounds["accumulator_max"], 2, bounds["accumulator_max"])
    except ConstructibilityError:
        pass
    else:
        raise ConstructibilityError("Store symbolic u128 overflow counterexample accepted")


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
    require_exact(closure["success_states"], ["task-v4-sealed", "manifest-open", "manifest-streaming", "manifest-validated", "blob-open", "blob-streaming", "blob-sealed", "closure-sealed", "closure-acknowledged", "task-accepted"], "source-closure success states")
    require_exact(closure["message_ids"], {"heartbeat": 23, "source_closure": [24, 25, 26, 27, 28, 29]}, "source-closure message registry")
    require_exact(closure["bounds"]["manifest_bytes"], 40 * 1024 * 1024, "source-closure manifest bound")
    require_exact(closure["bounds"]["spool_bytes"], 88 * 1024 * 1024, "source-closure spool bound")
    if set(closure["terminal_failures"]) != {"source-closure.rejected", "source-closure.cancelled", "provider.crash", "provider.connection-lost"}:
        raise ConstructibilityError("constructibility drift: source-closure terminal union")

    store = machines["store_candidate_report"]
    require_exact(store["candidate_states"], ["idle", "staging-session-open", "appending", "input-sealed", "candidate-identity-sealed", "independently-validating", "validation-sealed", "publication-attempt", "publication-terminal"], "Store candidate states")
    require_exact(store["report_states"], ["publication-independent-projection", "projection-validated", "maximum-tail-reserved", "publication-attempt", "exact-outcome-captured", "outcome-tail-finalized", "full-schema-validated", "bottom-up-cross-binding-validated", "stdout-published"], "Store report states")
    if store["candidate_states"].index("candidate-identity-sealed") >= store["candidate_states"].index("independently-validating"):
        raise ConstructibilityError("candidate identity is not sealed before validation")
    if store["report_states"].index("maximum-tail-reserved") >= store["report_states"].index("publication-attempt"):
        raise ConstructibilityError("report tail is not reserved before publication")
    require_exact(store["post_attempt_failure"], "exit-2-zero-authoritative-response-store-recovery-only", "post-attempt failure")
    require_exact(store["publication_outcomes"], ["not-attempted", "rejected-stale", "rejected-store-failure", "publication-outcome-unknown", "committed-unverified", "committed-verified"], "Store publication outcomes")
    require_exact(store["projections"], {"actual_source": "backend-staging-canonical-physical-order", "expected_source": "immutable-sealed-task-plus-selected-request-and-journal", "comparison": "separate-cursors-full-record-byte-exact"}, "dual projection")
    require_exact(store["bounds"], {"tasks": 4096, "scale_bytes": 512 * 1024 * 1024, "source_window_bytes": 64 * 1024 * 1024, "sort_arena_bytes": 8 * 1024 * 1024, "comparator_cursor_count": 2, "comparator_cursor_bytes_each": 32 * 1024, "resident_window_limit_bytes": 72 * 1024 * 1024 + 64 * 1024, "merge_fan_in": 16, "merge_file_descriptors": 18, "report_bytes": 1024 * 1024 * 1024, "counter_max": (1 << 64) - 1, "accumulator_max": (1 << 128) - 1}, "DF-0200 numeric bounds")
    require_exact(store["representation"], {"logical_write": "cxxlens.ng-snapshot-payload.v5", "sqlite_physical": "cxxlens.sqlite-semantic-store.v3", "sqlite_version": "3.0.0", "chunk_profile": "cxxlens.sqlite-payload-chunks.v1", "chunk_bytes": 8 * 1024 * 1024, "legacy_read": ["v1", "v2", "v3", "v4"]}, "Store exact representation")
    execute_store_symbolic_witness(store)
    require_exact(model["counterexample_sets"]["store_candidate_report"], ["second-full-graph", "shared-projection-traversal", "digest-only", "compact-after-attempt", "lost-publication-outcome-unknown", "eager-sqlite-residency-claim"], "Store counterexamples")

    mapping = machines["sqlite_read_mapping"]
    require_exact(mapping["nesting"], "#205-inside-#201-active-read-connection", "SQLite nesting")
    require_exact(mapping["no_effect_boundary"], "before-target-xOpen", "SQLite no-effect boundary")
    require_exact(mapping["outer_states"], ["unresolved", "runtime-vfs-filesystem-sealed", "retained-parent-held", "no-effect-boundary-armed", "typed-family-census", "active-read-connection-open", "wal-lock-and-prefix-held", "mapping-subprotocol-or-private-index", "eager-decode", "decoded-read-candidate-sealed", "connection-revoking", "outer-custody-join-sealed", "connection-closed", "zero-effect-callback-receipt-sealed", "logical-read-receipt"], "SQLite outer states")
    require_exact(mapping["writer_states"], ["callback-admitted", "pre-callback-sequence-cut", "attempt-pin-held", "native-started", "native-outcome-captured", "pending-mapping-receipt", "identity-validated", "mapping-lease-promoted", "eager-use-owner-held", "writer-handoff-sealed"], "SQLite writer states")
    require_exact(mapping["reader_states"], ["reader-session-reserved", "writer-lease-page-support-pin-held", "native-started", "native-outcome-captured", "reader-attachment-candidate", "identity-and-effect-validated", "reader-attachment-group-promoted", "eager-session-owner-admitted", "reader-handoff-sealed"], "SQLite reader states")
    require_exact(mapping["reader_forbidden_products"], ["mapping-lease-promoted", "writer-authority", "transitive-page-authority"], "SQLite reader product separation")
    require_exact(mapping["predelegation_authority"], {"writer": "attempt-and-in-flight-pin-only", "reader": "fresh-active-writer-lease-page-support-pin-required-before-native"}, "predelegation authority")
    require_exact(mapping["promotion_predicate"], "native-SQLITE_OK-nonnull-plus-full-identity-zero-effect-receipt-and-writer-gates", "SQLite promotion predicate")
    teardown = mapping["teardown_transition_graph"]
    require_exact(teardown, {"active": "hide-generation", "hide-generation": "seal-pre-callback-cut", "seal-pre-callback-cut": "revoke-admission", "revoke-admission": "drain-callbacks-and-use-owners", "drain-callbacks-and-use-owners": "seal-owner-census", "seal-owner-census": "native-unmap-deleteFlag-zero", "native-unmap-deleteFlag-zero": ["unmap-confirmed-OK", "terminal-opaque-quarantine-zero-close"], "unmap-confirmed-OK": "consume-distinct-close-owner-and-native-close-once", "consume-distinct-close-owner-and-native-close-once": ["close-confirmed", "terminal-opaque-quarantine"], "close-confirmed": "seal-zero-effect-outcomes-retire-registry-release-pins", "terminal-opaque-quarantine-zero-close": [], "terminal-opaque-quarantine": []}, "SQLite teardown transition graph")
    writer_terminal = "seal-zero-effect-outcomes-retire-registry-release-pins"
    validate_closed_graph(teardown, "active", {writer_terminal})
    reader_teardown = mapping["reader_teardown_transition_graph"]
    require_exact(reader_teardown, {"reader-handoff-sealed": "hide-reader-generation", "hide-reader-generation": "seal-reader-pre-callback-cut", "seal-reader-pre-callback-cut": "revoke-reader-admission", "revoke-reader-admission": "drain-reader-callbacks-and-use-owners", "drain-reader-callbacks-and-use-owners": "seal-reader-member-and-use-owner-census", "seal-reader-member-and-use-owner-census": "native-reader-unmap-deleteFlag-zero", "native-reader-unmap-deleteFlag-zero": ["reader-unmap-confirmed-OK", "reader-terminal-opaque-quarantine"], "reader-unmap-confirmed-OK": "consume-reader-close-owner-and-native-close-once", "consume-reader-close-owner-and-native-close-once": ["reader-close-confirmed", "reader-terminal-opaque-quarantine"], "reader-close-confirmed": "seal-reader-callback-effect-transcript-and-cleanup-ack", "seal-reader-callback-effect-transcript-and-cleanup-ack": "retire-reader-attachment-group-and-release-page-support-pin", "retire-reader-attachment-group-and-release-page-support-pin": "reader-retired", "reader-retired": [], "reader-terminal-opaque-quarantine": []}, "SQLite reader teardown transition graph")
    validate_closed_graph(reader_teardown, "reader-handoff-sealed")
    require_exact(mapping["retirement_join"], {"writer_terminal": "seal-zero-effect-outcomes-retire-registry-release-pins", "reader_terminal": "reader-retired", "scope": "outer-connection-owned-writer-attachments-and-reader-sessions-only", "join": "outer-custody-join-sealed", "forbidden": "reader-retirement-claims-independent-writer-retirement"}, "SQLite scoped retirement join")
    require_exact(mapping["zero_effect_receipt"], ["initialize", "create", "write", "truncate", "extend", "delete", "resize"], "SQLite zero-effect receipt")
    require_exact(mapping["read_receipt_barrier"], ["outer-custody-join-sealed", "connection-closed", "outer-scoped-zero-live-callbacks-leases-and-use-owners", "zero-effect-callback-receipt-sealed"], "SQLite read receipt barrier")
    fork_graph = mapping["fork_transition_graph"]
    require_exact(fork_graph, {"running": "atfork-prepare-seal-admission-and-census", "atfork-prepare-seal-admission-and-census": ["parent-revalidate-process-and-fork-generation", "child-transfer-all-inherited-custody"], "parent-revalidate-process-and-fork-generation": "running", "child-transfer-all-inherited-custody": "child-inherited-custody-quarantine", "child-inherited-custody-quarantine": ["child-exec", "child-exit"], "child-exec": [], "child-exit": []}, "SQLite fork transition graph")
    validate_closed_graph(fork_graph, "running")
    require_exact(mapping["child_quarantine_forbidden"], ["SQLite-entry", "native-unmap", "native-close", "retry", "cleanup", "owner-drain", "authority-reconstruction"], "SQLite child quarantine")
    require_exact(mapping["ambiguous_callback"], "permanent-quarantine-no-retry", "ambiguous callback")
    require_exact(mapping["production_activation_predicate"], ["accepted-independent-review", "exact-static-shared-runtime-vfs-sqlite-dso-source-id-hash-build-toolchain-identity", "two-live-store-cas", "materialization-race", "cross-process-race", "cantinit-readonly-negative", "pending-only-state-rejection", "four-extend-pairs", "simultaneous-first-writer-join-and-mismatch", "w2-retirement-ordering", "new-page-size-update", "duplicate-fd-lock-loss", "native-close-pinning", "timeout-and-unknown-outcomes", "same-page-successor", "different-page-successor-rejection", "fork-aba-unload-replacement", "connected-main-ci-and-platform-qualification"], "mapping production predicate")

    effect = machines["sqlite_normalization_effect"]
    require_exact(effect["separate_from_zero_effect_read"], True, "normalization isolation")
    require_exact(effect["entry"], "logical-read-receipt-exact-empty-after-connection-closed-zero-custody-zero-effect", "normalization entry")
    partitions = effect["fixture_partition_machine"]
    expected_guards = {"F0": "exact-pre-no-sidecar", "FP": "exact-pre-nonhot-journal-prefix", "FH": "valid-hot-journal-with-exact-preimages", "FZ-pre": "exact-pre-plus-size-zero-WAL", "FI": "journal-preimages-exact-pre-and-deterministic-post-plus-invalidated-journal", "FZ-post": "exact-post-plus-size-zero-WAL", "FO": "complete-valid-rollback-exact-empty-current-main-no-sidecar"}
    expected_routes = {
        "F0": ["live-receipt", "fixture-normalizer"],
        "FP": ["authenticated-cleanup-or-recovery", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer"],
        "FH": ["authenticated-cleanup-or-recovery", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer"],
        "FZ-pre": ["retain-and-revalidate-exact-size-zero-coordination-WAL", "authenticated-coordination-WAL-delete", "retained-parent-fsync", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer"],
        "FI": ["independently-validated-rollback-empty-fresh-anchor"],
        "FZ-post": ["authenticated-size-zero-WAL-delete", "retained-parent-fsync", "independently-validated-rollback-empty-fresh-anchor"],
        "FO": ["independently-validated-rollback-empty-fresh-anchor"],
    }
    expected_dominators = {
        "F0": ["confirmed-close", "post-close-census"],
        "FP": ["authenticated-cleanup-or-recovery", "parent-fsync", "confirmed-close", "post-close-census"],
        "FH": ["authenticated-cleanup-or-recovery", "parent-fsync", "confirmed-close", "post-close-census"],
        "FZ-pre": ["authenticated-coordination-WAL-delete", "retained-parent-fsync", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer", "confirmed-close", "post-close-census"],
        "FI": ["independently-validated-rollback-empty-fresh-anchor"],
        "FZ-post": ["authenticated-size-zero-WAL-delete", "retained-parent-fsync", "independently-validated-rollback-empty-fresh-anchor"],
        "FO": ["independently-validated-rollback-empty-fresh-anchor"],
    }
    recrash_graph = ["pre-effect-to-effect-admitted", "effect-admitted-to-recoverable-interruption-or-live-terminal", "recoverable-interruption-to-recrash-classified", "recrash-classified-to-reenter-seven-family-classifier", "reclassified-family-authority-only-no-original-success"]
    if set(partitions) != set(expected_guards):
        raise ConstructibilityError("constructibility drift: DF-0202 partition machine")
    for family, row in partitions.items():
        terminal = "normalization-receipt" if family in {"F0", "FP", "FH", "FZ-pre"} else "fresh-anchor-only-never-completed-edge"
        require_exact(row["entry_guard"], expected_guards[family], f"{family} entry guard")
        require_exact(row["route"], expected_routes[family], f"{family} route")
        require_exact(row["recrash_graph"], recrash_graph, f"{family} recrash graph")
        require_exact(row["success_dominators"], expected_dominators[family], f"{family} success dominance")
        require_exact(row["terminal"], terminal, f"{family} terminal")
    require_exact(effect["recrash_transition"], "any-recoverable-interruption-to-durable-bytes-through-same-seven-family-classifier", "DF-0202 recrash transition")
    require_exact(effect["production_activation_predicate"], ["tracked-exact-harness", "static-shared-loaded-sqlite-dso-source-id-hash-build-toolchain-identity", "runtime-vfs-device-filesystem-profile", "all-callback-boundaries", "parameterized-sector-page-record-set", "authenticated-coordination-wal-delete", "parent-sync-after-each-delete", "rebind-at-unlink", "seven-family-recrash-idempotence", "post-normalization-fresh-transition", "canonical-report-digest", "distinct-implementation-review", "explicit-accepted-effect-profile", "connected-main-ci-and-platform-qualification"], "normalization production predicate")
    require_exact(effect["canonical_user_source_activation"], "prohibited", "production normalization activation")
    require_exact(effect["parent_sync_after_each_delete"], "required", "normalization parent sync")
    require_exact(model["counterexample_sets"]["sqlite_read_mapping"], ["first-map-mutation", "writer-predelegation-lease", "reader-without-predelegation-writer-lease-pin", "missing-zero-effect-receipt", "nonzero-unmap-deleteFlag", "ok-null", "pointer-substitution", "aba", "fork-ordinary-drain", "unload-before-revoke", "callback-retry"], "SQLite mapping counterexamples")
    require_exact(model["counterexample_sets"]["sqlite_normalization_effect"], ["physical-census-entry", "fixture-production-promotion", "missing-parent-sync", "nonempty", "sidecar-ambiguous"], "SQLite effect counterexamples")
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
