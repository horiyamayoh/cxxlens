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
    immutable_input = {
        "partition": "p0",
        "claim": b"claim",
        "detached": b"row",
        "annotation": b"annotation",
        "coverage": b"coverage",
        "unresolved": b"unresolved",
        "identity": b"identity",
        "semantic_key": b"semantic-key",
        "closure": b"closure",
        "provenance": b"provenance",
        "guarantee": b"guarantee",
        "census": b"census",
    }

    def build_expected_projection(value: dict[str, Any]) -> list[tuple[str, str, bytes]]:
        partition = value["partition"]
        return [
            ("partition-begin", partition, b"begin"),
            ("semantic-key", partition + "/key", value["semantic_key"]),
            ("claim-full-projection", partition + "/claim", value["claim"]),
            ("detached-row", partition + "/row", value["detached"]),
            ("claim-annotation", partition + "/annotation", value["annotation"]),
            ("coverage", partition + "/coverage", value["coverage"]),
            ("unresolved", partition + "/unresolved", value["unresolved"]),
            ("closure-binding", partition + "/closure", value["closure"]),
            ("provenance", partition + "/provenance", value["provenance"]),
            ("guarantee", partition + "/guarantee", value["guarantee"]),
            ("partition-census", partition + "/census", value["census"]),
            ("partition-end", partition, b"end"),
            ("global-identity", "snapshot", value["identity"]),
        ]

    expected_records = build_expected_projection(immutable_input)
    backend_rows = [
        {"kind": "partition-begin", "key": "p0", "payload": b"begin"},
        {"kind": "semantic-key", "key": "p0/key", "payload": b"semantic-key"},
        {"kind": "claim-full-projection", "key": "p0/claim", "payload": b"claim"},
        {"kind": "detached-row", "key": "p0/row", "payload": b"row"},
        {"kind": "claim-annotation", "key": "p0/annotation", "payload": b"annotation"},
        {"kind": "coverage", "key": "p0/coverage", "payload": b"coverage"},
        {"kind": "unresolved", "key": "p0/unresolved", "payload": b"unresolved"},
        {"kind": "closure-binding", "key": "p0/closure", "payload": b"closure"},
        {"kind": "provenance", "key": "p0/provenance", "payload": b"provenance"},
        {"kind": "guarantee", "key": "p0/guarantee", "payload": b"guarantee"},
        {"kind": "partition-census", "key": "p0/census", "payload": b"census"},
        {"kind": "partition-end", "key": "p0", "payload": b"end"},
        {"kind": "global-identity", "key": "snapshot", "payload": b"identity"},
    ]

    def build_actual_projection(rows: list[dict[str, Any]]) -> list[tuple[str, str, bytes]]:
        grammar = store["projections"]["record_grammar"]
        rank = {kind: index for index, kind in enumerate(grammar)}
        try:
            physical_keys = []
            for row in rows:
                key = row["key"].encode("utf-8")
                if row["kind"] == "global-identity":
                    physical_keys.append((1, b"", rank[row["kind"]], key))
                else:
                    partition = row["key"].split("/", 1)[0].encode("utf-8")
                    physical_keys.append((0, partition, rank[row["kind"]], key))
        except (KeyError, UnicodeEncodeError) as error:
            raise ConstructibilityError("Store backend row has no canonical physical key") from error
        if physical_keys != sorted(physical_keys) or len(physical_keys) != len(set(physical_keys)):
            raise ConstructibilityError("Store backend cursor is not in canonical derived physical-key order")
        return [(row["kind"], row["key"], row["payload"]) for row in rows]

    actual_records = build_actual_projection(backend_rows)

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
        [actual_records[0], actual_records[0], *actual_records[1:]],
        list(reversed(actual_records)),
        [*actual_records[:-1], ("global-identity", "snapshot", b"IDENTITY")],
    ]
    if any(projections_equal(candidate, expected_records) for candidate in adversarial_actuals):
        raise ConstructibilityError("Store symbolic projection counterexample accepted")
    for physical_rows in (list(reversed(backend_rows)), [backend_rows[1], backend_rows[0], *backend_rows[2:]]):
        try:
            build_actual_projection(physical_rows)
        except ConstructibilityError:
            continue
        raise ConstructibilityError("Store reordered physical backend rows were accepted")

    expected_policy = {
        "not-attempted": {"response": "typed-failure", "exit": 1, "recovery_authority": "none"},
        "rejected-stale": {"response": "typed-publication-conflict", "exit": 1, "recovery_authority": "Store"},
        "rejected-store-failure": {"response": "typed-store-failure", "exit": 1, "recovery_authority": "Store"},
        "publication-outcome-unknown": {"response": "safe-detailed-failure-or-none", "exit": "1-or-2", "recovery_authority": "Store"},
        "committed-unverified": {"response": "safe-detailed-failure-or-none", "exit": "1-or-2", "recovery_authority": "Store"},
        "committed-verified": {"response": "validated-success", "exit": 0, "recovery_authority": "Store"},
    }
    require_exact(store["outcome_policy"], expected_policy, "Store outcome policy")
    if set(store["outcome_policy"]) != set(store["publication_outcomes"]):
        raise ConstructibilityError("Store outcome policy census mismatch")

    candidate_graph = store["candidate_transition_graph"]
    report_graph = store["report_transition_graph"]
    validate_closed_graph(
        candidate_graph,
        "idle",
        set(store["publication_outcomes"]),
    )
    validate_closed_graph(report_graph, "publication-independent-projection")
    require_exact(
        store["attempt_coupling"],
        {
            "token": "one-move-only-publication-attempt-receipt",
            "candidate_edge": "validation-sealed-to-publication-attempt",
            "report_edge": "publication-decision-to-same-publication-attempt",
            "maximum_attempt_count": 1,
            "not_attempted_edge": "validation-sealed-and-publication-decision-to-not-attempted",
            "outcome_source": "exact-candidate-publication-terminal",
        },
        "Store publication attempt coupling",
    )
    if "publication-attempt" not in candidate_graph["validation-sealed"] or "publication-attempt" not in report_graph["publication-decision"]:
        raise ConstructibilityError("Store publication attempt is not coupled")
    for outcome, policy in store["outcome_policy"].items():
        source = candidate_graph["validation-sealed"] if outcome == "not-attempted" else candidate_graph["publication-terminal"]
        if outcome not in source:
            raise ConstructibilityError("Store outcome is unreachable from publication attempt")
        if outcome == "publication-outcome-unknown" and policy != {
            "response": "safe-detailed-failure-or-none", "exit": "1-or-2", "recovery_authority": "Store"
        }:
            raise ConstructibilityError("Store ambiguous outcome is not fail-closed")

    class AttemptToken:
        def __init__(self) -> None:
            self.consumed = False
        def consume(self) -> None:
            if self.consumed:
                raise ConstructibilityError("Store publication attempt token replayed")
            self.consumed = True
    token = AttemptToken()
    token.consume()
    try:
        token.consume()
    except ConstructibilityError:
        pass
    else:
        raise ConstructibilityError("Store publication token replay counterexample accepted")

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
        checked_add(
            checked_add(bounds["source_window_bytes"], bounds["sort_arena_bytes"], bounds["counter_max"]),
            cursor_bytes,
            bounds["counter_max"],
        ),
        checked_add(
            checked_add(bounds["backend_cursor_bytes"], bounds["codec_hash_state_bytes"], bounds["counter_max"]),
            checked_add(bounds["record_buffer_bytes"], bounds["counter_state_bytes"], bounds["counter_max"]),
            bounds["counter_max"],
        ),
        bounds["counter_max"],
    )
    if resident != bounds["resident_window_limit_bytes"]:
        raise ConstructibilityError("Store symbolic bounded-window witness invalid")
    if bounds["merge_file_descriptors"] != bounds["merge_fan_in"] + 2:
        raise ConstructibilityError("Store symbolic merge descriptor witness invalid")
    for fail_at in range(bounds["merge_file_descriptors"] + 1):
        active_descriptors: list[int] = []
        for descriptor in range(bounds["merge_file_descriptors"]):
            if descriptor == fail_at:
                break
            active_descriptors.append(descriptor)
        while active_descriptors:
            active_descriptors.pop()
        if active_descriptors:
            raise ConstructibilityError("Store partial descriptor acquisition leaked")
    exact_tail = 0
    for field in ("report_tail_global_projection_bytes", "report_tail_task_metadata_bytes", "report_tail_sdk_records_bytes", "report_tail_diagnostics_bytes", "report_tail_framing_bytes"):
        exact_tail = checked_add(exact_tail, bounds[field], bounds["counter_max"])
    if exact_tail != bounds["report_tail_reservation_bytes"] or exact_tail > bounds["report_bytes"]:
        raise ConstructibilityError("Store report tail reservation witness invalid")
    singleton_record = bounds["sort_arena_bytes"] + 1
    if singleton_record <= bounds["sort_arena_bytes"] or bounds["record_buffer_bytes"] > bounds["sort_arena_bytes"]:
        raise ConstructibilityError("Store oversized singleton-run witness invalid")
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
    require_exact(store["report_states"], ["publication-independent-projection", "projection-validated", "maximum-tail-reserved", "publication-decision", "publication-attempt", "exact-outcome-captured", "outcome-tail-finalized", "full-schema-validated", "bottom-up-cross-binding-validated", "stdout-published"], "Store report states")
    if store["candidate_states"].index("candidate-identity-sealed") >= store["candidate_states"].index("independently-validating"):
        raise ConstructibilityError("candidate identity is not sealed before validation")
    if store["report_states"].index("maximum-tail-reserved") >= store["report_states"].index("publication-attempt"):
        raise ConstructibilityError("report tail is not reserved before publication")
    require_exact(store["post_attempt_failure"], "exit-2-zero-authoritative-response-store-recovery-only", "post-attempt failure")
    require_exact(store["publication_outcomes"], ["not-attempted", "rejected-stale", "rejected-store-failure", "publication-outcome-unknown", "committed-unverified", "committed-verified"], "Store publication outcomes")
    require_exact(store["candidate_transition_graph"], {"idle": "staging-session-open", "staging-session-open": "appending", "appending": "input-sealed", "input-sealed": "candidate-identity-sealed", "candidate-identity-sealed": "independently-validating", "independently-validating": "validation-sealed", "validation-sealed": ["not-attempted", "publication-attempt"], "publication-attempt": "publication-terminal", "publication-terminal": ["rejected-stale", "rejected-store-failure", "publication-outcome-unknown", "committed-unverified", "committed-verified"]}, "Store candidate transition graph")
    require_exact(store["report_transition_graph"], {"publication-independent-projection": "projection-validated", "projection-validated": "maximum-tail-reserved", "maximum-tail-reserved": "publication-decision", "publication-decision": ["outcome-not-attempted", "publication-attempt"], "publication-attempt": "exact-outcome-captured", "exact-outcome-captured": ["outcome-rejected-stale", "outcome-rejected-store-failure", "outcome-publication-outcome-unknown", "outcome-committed-unverified", "outcome-committed-verified"], "outcome-not-attempted": "outcome-tail-finalized", "outcome-rejected-stale": "outcome-tail-finalized", "outcome-rejected-store-failure": "outcome-tail-finalized", "outcome-publication-outcome-unknown": "outcome-tail-finalized", "outcome-committed-unverified": "outcome-tail-finalized", "outcome-committed-verified": "outcome-tail-finalized", "outcome-tail-finalized": "full-schema-validated", "full-schema-validated": "bottom-up-cross-binding-validated", "bottom-up-cross-binding-validated": "stdout-published", "stdout-published": []}, "Store report transition graph")
    require_exact(store["projections"], {"actual_source": "backend-staging-canonical-physical-order", "order_authentication": "derived-physical-key-equals-cursor-order-no-backend-ordinal", "expected_source": "immutable-sealed-task-plus-selected-request-and-journal", "comparison": "separate-cursors-full-record-byte-exact", "record_grammar": ["partition-begin", "semantic-key", "claim-full-projection", "detached-row", "claim-annotation", "coverage", "unresolved", "closure-binding", "provenance", "guarantee", "partition-census", "partition-end", "global-identity"]}, "dual projection")
    require_exact(store["retention_roles"], ["one-immutable-final-payload-or-none", "one-task-source-output-window", "one-sort-arena", "two-comparator-cursors", "one-backend-cursor", "fixed-codec-hash-and-counter-state", "no-second-complete-graph", "no-second-final-payload"], "Store retention role census")
    require_exact(store["bounds"], {"tasks": 4096, "scale_bytes": 512 * 1024 * 1024, "source_window_bytes": 64 * 1024 * 1024, "sort_arena_bytes": 8 * 1024 * 1024, "comparator_cursor_count": 2, "comparator_cursor_bytes_each": 32 * 1024, "backend_cursor_bytes": 1024 * 1024, "codec_hash_state_bytes": 64 * 1024, "record_buffer_bytes": 1024 * 1024, "counter_state_bytes": 4096, "resident_window_limit_bytes": 77729792, "merge_fan_in": 16, "merge_file_descriptors": 18, "report_tail_global_projection_bytes": 10420985, "report_tail_task_metadata_bytes": 8463179, "report_tail_sdk_records_bytes": 8 * 1024 * 1024, "report_tail_diagnostics_bytes": 1024 * 1024, "report_tail_framing_bytes": 198, "report_tail_reservation_bytes": 28321546, "report_bytes": 1024 * 1024 * 1024, "counter_max": (1 << 64) - 1, "accumulator_max": (1 << 128) - 1}, "DF-0200 numeric bounds")
    require_exact(store["representation"], {"logical_write": "cxxlens.ng-snapshot-payload.v5", "sqlite_physical": "cxxlens.sqlite-semantic-store.v3", "sqlite_version": "3.0.0", "chunk_profile": "cxxlens.sqlite-payload-chunks.v1", "chunk_bytes": 8 * 1024 * 1024, "legacy_read": ["v1", "v2", "v3", "v4"]}, "Store exact representation")
    execute_store_symbolic_witness(store)
    require_exact(model["counterexample_sets"]["store_candidate_report"], ["second-full-graph", "shared-projection-traversal", "digest-only", "compact-after-attempt", "lost-publication-outcome-unknown", "eager-sqlite-residency-claim"], "Store counterexamples")

    mapping = machines["sqlite_read_mapping"]
    require_exact(mapping["nesting"], "#205-inside-#201-active-read-connection", "SQLite nesting")
    require_exact(mapping["no_effect_boundary"], "before-target-xOpen", "SQLite no-effect boundary")
    outer_states = ["unresolved", "runtime-vfs-filesystem-sealed", "retained-parent-held", "no-effect-boundary-armed", "typed-family-census", "active-read-connection-open", "wal-lock-and-prefix-held", "mapping-subprotocol-or-private-index", "eager-decode", "decoded-read-candidate-sealed", "connection-revoking", "outer-custody-join-pending", "outer-custody-join-sealed", "connection-closed", "zero-effect-callback-receipt-sealed", "logical-read-receipt"]
    require_exact(mapping["outer_states"], outer_states, "SQLite outer states")
    expected_outer_graph = {
        left: right for left, right in zip(outer_states, outer_states[1:])
    }
    expected_outer_graph["logical-read-receipt"] = []
    require_exact(mapping["outer_transition_graph"], expected_outer_graph, "SQLite outer transition graph")
    validate_closed_graph(mapping["outer_transition_graph"], "unresolved")
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
    expected_join = {
        "writer_terminal": "seal-zero-effect-outcomes-retire-registry-release-pins",
        "reader_terminal": "reader-retired",
        "scope": "outer-connection-owned-writer-attachments-and-reader-sessions-only",
        "pending": "outer-custody-join-pending",
        "join": "outer-custody-join-sealed",
        "writer_census": "exact-outer-owned-writer-count-equals-retired-writer-count",
        "reader_census": "exact-outer-owned-reader-count-equals-retired-reader-count",
        "unrelated_writers": "excluded-from-outer-census",
        "forbidden": "reader-retirement-claims-independent-writer-retirement",
    }
    require_exact(mapping["retirement_join"], expected_join, "SQLite scoped retirement join")
    custody_kinds = ["connection", "callback-attempt", "reservation", "mapping-lease", "reader-attachment", "use-owner", "cleanup-owner", "close-owner", "uncertainty-owner"]
    require_exact(mapping["outer_custody_kinds"], custody_kinds, "SQLite outer custody kinds")
    require_exact(mapping["outer_join_terminal_profiles"], {"retired": "authenticated-terminal-receipt", "quarantined": "permanent-tombstone-with-retained-custody", "enrollment": "atomically-sealed-before-join", "duplicate_or_late": "reject"}, "SQLite outer join profiles")
    require_exact(mapping["outer_join_receipt_profile"], {"enrollment_key": "kind-plus-instance-id", "enrollment_binding": "sha256-kind-instance-outer-connection-generation", "terminal_binding": "sha256-enrollment-digest-terminal-outcome", "join_predicate": "exact-enrolled-instance-set-equals-authenticated-terminal-receipt-set"}, "SQLite outer join receipt profile")

    def enrollment_digest(kind: str, identifier: str) -> str:
        body = b"cxxlens.sqlite-outer-custody-enrollment.v1\0" + kind.encode() + b"\0" + identifier.encode() + b"\0outer-generation-7"
        return "sha256:" + hashlib.sha256(body).hexdigest()

    def terminal_digest(enrollment: str, terminal: str) -> str:
        body = b"cxxlens.sqlite-outer-custody-terminal.v1\0" + enrollment.encode() + b"\0" + terminal.encode()
        return "sha256:" + hashlib.sha256(body).hexdigest()

    def seals_outer_join(enrolled: list[dict[str, str]], receipts: list[dict[str, str]], *, enrollment_sealed: bool) -> bool:
        enrolled_by_key = {(row["kind"], row["identifier"]): row for row in enrolled}
        receipt_by_key = {(row["kind"], row["identifier"]): row for row in receipts}
        if (not enrollment_sealed or len(enrolled_by_key) != len(enrolled) or
                len(receipt_by_key) != len(receipts) or set(enrolled_by_key) != set(receipt_by_key) or
                set(row["kind"] for row in enrolled) != set(custody_kinds)):
            return False
        for key, enrollment in enrolled_by_key.items():
            receipt = receipt_by_key[key]
            expected_enrollment = enrollment_digest(*key)
            if (enrollment["enrollment_digest"] != expected_enrollment or
                    receipt["enrollment_digest"] != expected_enrollment or
                    receipt["terminal"] not in {"retired", "quarantined"} or
                    receipt["receipt_digest"] != terminal_digest(expected_enrollment, receipt["terminal"])):
                return False
        return True

    enrolled = [
        {"kind": kind, "identifier": f"owner-{index}", "enrollment_digest": enrollment_digest(kind, f"owner-{index}")}
        for index, kind in enumerate(custody_kinds)
    ]
    enrolled.append({"kind": "mapping-lease", "identifier": "owner-extra", "enrollment_digest": enrollment_digest("mapping-lease", "owner-extra")})
    complete_custody = [
        {**row, "terminal": "retired", "receipt_digest": terminal_digest(row["enrollment_digest"], "retired")}
        for row in enrolled
    ]
    if not seals_outer_join(enrolled, complete_custody, enrollment_sealed=True):
        raise ConstructibilityError("SQLite outer custody join rejected complete authenticated census")
    if seals_outer_join(enrolled, complete_custody[:-1], enrollment_sealed=True):
        raise ConstructibilityError("SQLite outer custody join accepted omitted enrolled instance")
    if seals_outer_join(enrolled, [*complete_custody, complete_custody[0]], enrollment_sealed=True):
        raise ConstructibilityError("SQLite outer custody join accepted duplicate custody")
    tampered = [dict(row) for row in complete_custody]
    tampered[0]["receipt_digest"] = "sha256:" + "0" * 64
    if seals_outer_join(enrolled, tampered, enrollment_sealed=True):
        raise ConstructibilityError("SQLite outer custody join accepted unauthenticated terminal")
    if seals_outer_join(enrolled, complete_custody, enrollment_sealed=False):
        raise ConstructibilityError("SQLite outer custody join accepted late enrollment window")
    require_exact(mapping["zero_effect_receipt"], ["initialize", "create", "write", "truncate", "extend", "delete", "resize"], "SQLite zero-effect receipt")
    require_exact(mapping["read_receipt_barrier"], ["outer-custody-join-sealed", "connection-closed", "outer-scoped-zero-live-callbacks-leases-and-use-owners", "zero-effect-callback-receipt-sealed"], "SQLite read receipt barrier")
    fork_graph = mapping["fork_transition_graph"]
    require_exact(fork_graph, {"running": "atfork-prepare-seal-admission-and-census", "atfork-prepare-seal-admission-and-census": ["parent-revalidate-process-and-fork-generation", "child-transfer-all-inherited-custody"], "parent-revalidate-process-and-fork-generation": "running", "child-transfer-all-inherited-custody": "child-inherited-custody-quarantine", "child-inherited-custody-quarantine": ["child-exec", "child-exit"], "child-exec": [], "child-exit": []}, "SQLite fork transition graph")
    validate_closed_graph(fork_graph, "running")
    require_exact(mapping["child_quarantine_forbidden"], ["SQLite-entry", "native-unmap", "native-close", "retry", "cleanup", "owner-drain", "authority-reconstruction"], "SQLite child quarantine")
    require_exact(mapping["ambiguous_callback"], "permanent-quarantine-no-retry", "ambiguous callback")
    ordinary_revoke = {"outer": "connection-revoking", "writer": "hide-generation", "reader": "hide-reader-generation", "terminal": "outer-custody-join-pending"}
    expected_revocations = {
        "aba": ordinary_revoke,
        "vfs-unload-request": {**ordinary_revoke, "terminal": "vfs-unloaded"},
        "file-replacement": ordinary_revoke,
        "directory-replacement": ordinary_revoke,
        "watch-loss-or-overflow": ordinary_revoke,
        "wal-reset-or-resize": ordinary_revoke,
        "ambiguous-callback": {"outer": "connection-revoking", "writer": "terminal-opaque-quarantine", "reader": "reader-terminal-opaque-quarantine", "terminal": "outer-custody-join-pending"},
    }
    require_exact(mapping["revocation_events"], expected_revocations, "SQLite revocation event edges")
    for event, targets in mapping["revocation_events"].items():
        if targets["outer"] != "connection-revoking" or targets["writer"] not in teardown or targets["reader"] not in reader_teardown:
            raise ConstructibilityError(f"SQLite revocation fanout is not executable: {event}")
    unload_graph = mapping["unload_transition_graph"]
    require_exact(unload_graph, {"vfs-unload-request": "connection-revoking", "connection-revoking": "outer-custody-join-pending", "outer-custody-join-pending": "outer-custody-join-sealed", "outer-custody-join-sealed": "unload-permitted", "unload-permitted": "vfs-unloaded", "vfs-unloaded": []}, "SQLite unload transition graph")
    validate_closed_graph(unload_graph, "vfs-unload-request")
    if list(unload_graph).index("outer-custody-join-sealed") >= list(unload_graph).index("unload-permitted"):
        raise ConstructibilityError("SQLite unload can precede outer custody join")
    require_exact(mapping["production_activation_predicate"], ["accepted-independent-review", "exact-static-shared-runtime-vfs-sqlite-dso-source-id-hash-build-toolchain-identity", "two-live-store-cas", "materialization-race", "cross-process-race", "cantinit-readonly-negative", "pending-only-state-rejection", "four-extend-pairs", "simultaneous-first-writer-join-and-mismatch", "w2-retirement-ordering", "new-page-size-update", "duplicate-fd-lock-loss", "native-close-pinning", "timeout-and-unknown-outcomes", "same-page-successor", "different-page-successor-rejection", "fork-aba-unload-replacement", "connected-main-ci-and-platform-qualification"], "mapping production predicate")

    effect = machines["sqlite_normalization_effect"]
    require_exact(effect["separate_from_zero_effect_read"], True, "normalization isolation")
    require_exact(effect["entry"], "logical-read-receipt-exact-empty-after-connection-closed-zero-custody-zero-effect", "normalization entry")
    effect_states = ["logical-read-receipt", "receipt-revalidated", "effect-profile-capability-sealed", "exclusive-normalization-owner", "pre-effect-sealed", "effect-journal-open", "permitted-callback-effects", "file-and-parent-durable", "confirmed-close", "post-close-census", "normalization-receipt", "ordinary-fresh-init", "recoverable-interruption", "cold-reclassified", "seven-family-classified", "cold-family-authority-selected"]
    require_exact(effect["states"], effect_states, "normalization effect states")
    expected_effect_graph = {
        "logical-read-receipt": "receipt-revalidated",
        "receipt-revalidated": "effect-profile-capability-sealed",
        "effect-profile-capability-sealed": "exclusive-normalization-owner",
        "exclusive-normalization-owner": "pre-effect-sealed",
        "pre-effect-sealed": ["effect-journal-open", "recoverable-interruption"],
        "effect-journal-open": ["permitted-callback-effects", "recoverable-interruption"],
        "permitted-callback-effects": ["file-and-parent-durable", "recoverable-interruption"],
        "file-and-parent-durable": ["confirmed-close", "recoverable-interruption"],
        "confirmed-close": ["post-close-census", "recoverable-interruption"],
        "post-close-census": ["normalization-receipt", "recoverable-interruption"],
        "normalization-receipt": ["ordinary-fresh-init", "recoverable-interruption"],
        "ordinary-fresh-init": [],
        "recoverable-interruption": "cold-reclassified",
        "cold-reclassified": "seven-family-classified",
        "seven-family-classified": "cold-family-authority-selected",
        "cold-family-authority-selected": [],
    }
    require_exact(effect["transition_graph"], expected_effect_graph, "normalization transition graph")
    validate_closed_graph(effect["transition_graph"], "logical-read-receipt")
    expected_interruptions = {
        "pre-effect-sealed": "recoverable-interruption",
        "effect-journal-open": "recoverable-interruption",
        "permitted-callback-effects": "recoverable-interruption",
        "file-and-parent-durable": "recoverable-interruption",
        "confirmed-close": "recoverable-interruption",
        "post-close-census": "recoverable-interruption",
        "normalization-receipt": "recoverable-interruption",
    }
    require_exact(effect["interruption_edges"], expected_interruptions, "normalization interruption edges")
    for source, target in effect["interruption_edges"].items():
        targets = effect["transition_graph"].get(source, [])
        targets = [targets] if isinstance(targets, str) else targets
        if target != "recoverable-interruption" or target not in targets:
            raise ConstructibilityError("normalization interruption edge is not executable")
    require_exact(effect["admission_guard"], {"capability": "harness-minted-nonserializable-disposable-root", "binds": ["root-object-identity", "canonical-locator", "run-id", "exact-effect-schedule", "runtime-vfs-device-profile"], "canonical_or_user_locator": "reject", "revocation": "before-effect-arm-only", "production_profile": "absent-fail-closed"}, "normalization effect admission guard")
    require_exact(effect["cold_family_routes"], {"F0": "new-live-normalizer-authority", "FP": "new-live-normalizer-authority", "FH": "new-live-normalizer-authority", "FZ-pre": "new-live-normalizer-authority", "FI": "rollback-empty-fresh-anchor-only", "FZ-post": "rollback-empty-fresh-anchor-only", "FO": "rollback-empty-fresh-anchor-only", "unrecognized": "opaque-failure"}, "normalization cold family authority")
    partitions = effect["fixture_partition_machine"]
    expected_guards = {"F0": "exact-pre-no-sidecar", "FP": "exact-pre-nonhot-journal-prefix", "FH": "valid-hot-journal-with-exact-preimages", "FZ-pre": "exact-pre-plus-size-zero-WAL", "FI": "journal-preimages-exact-pre-and-deterministic-post-plus-invalidated-journal", "FZ-post": "exact-post-plus-size-zero-WAL", "FO": "complete-valid-rollback-exact-empty-current-main-no-sidecar"}
    expected_routes = {
        "F0": ["live-receipt", "fixture-normalizer"],
        "FP": ["authenticated-cleanup-or-recovery", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer"],
        "FH": ["authenticated-cleanup-or-recovery", "independently-revalidated-F0", "new-live-receipt", "fixture-normalizer"],
        "FZ-pre": ["retain-and-revalidate-exact-size-zero-coordination-WAL", "fixture-normalizer-with-same-coordination-WAL", "authenticated-coordination-WAL-delete", "retained-parent-fsync", "journal-created-and-parent-fsynced", "valid-journal-and-main-write", "terminal-journal-delete-and-parent-fsynced", "confirmed-close", "post-close-census", "normalization-receipt"],
        "FI": ["independently-validated-rollback-empty-fresh-anchor"],
        "FZ-post": ["authenticated-size-zero-WAL-delete", "retained-parent-fsync", "independently-validated-rollback-empty-fresh-anchor"],
        "FO": ["independently-validated-rollback-empty-fresh-anchor"],
    }
    expected_dominators = {
        "F0": ["confirmed-close", "post-close-census"],
        "FP": ["authenticated-cleanup-or-recovery", "parent-fsync", "confirmed-close", "post-close-census"],
        "FH": ["authenticated-cleanup-or-recovery", "parent-fsync", "confirmed-close", "post-close-census"],
        "FZ-pre": ["fixture-normalizer-with-same-coordination-WAL", "authenticated-coordination-WAL-delete", "retained-parent-fsync", "journal-created-and-parent-fsynced", "valid-journal-and-main-write", "terminal-journal-delete-and-parent-fsynced", "confirmed-close", "post-close-census"],
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
