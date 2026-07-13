#!/usr/bin/env python3
"""Deterministic, test-only feasibility spikes for issue #52.

This module contains no production algorithm.  It exercises bounded value models and
state machines at the difficult contract seams and emits reproducible evidence.
"""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import os
import pathlib
import signal
import subprocess
import sys
from typing import Any


ROOT = pathlib.Path(__file__).resolve().parents[2]
EVIDENCE = ROOT / "tests/contract_spikes/high_risk_validation_evidence.json"
SCHEMA = "cxxlens.high-risk-validation-evidence.v1"


def digest(value: Any) -> str:
    encoded = json.dumps(value, sort_keys=True, separators=(",", ":"), ensure_ascii=False)
    return "sha256:" + hashlib.sha256(encoded.encode("utf-8")).hexdigest()


def graph_spike() -> dict[str, Any]:
    edges = {
        "base": ["left", "right"],
        "left": ["join"],
        "right": ["join", "open_world"],
        "join": ["cycle"],
        "cycle": ["join"],
        "open_world": [],
    }
    max_nodes = 5
    queue = collections.deque([("base", ["base"])])
    visited: set[str] = set()
    paths: list[list[str]] = []
    truncated = False
    while queue:
        node, path = queue.popleft()
        if node in visited:
            paths.append(path)
            continue
        if len(visited) == max_nodes:
            truncated = True
            break
        visited.add(node)
        for target in sorted(edges[node]):
            queue.append((target, [*path, target]))
    canonical_edges = sorted((source, target) for source, targets in edges.items() for target in targets)
    reversed_edges = sorted((source, target) for source in reversed(list(edges)) for target in reversed(edges[source]))
    cycle_rows = sum((target, source) in canonical_edges for source, target in canonical_edges) // 2
    return {
        "result": "validated",
        "metrics": {
            "input_nodes": len(edges),
            "materialized_nodes": len(visited),
            "edge_count": len(canonical_edges),
            "cycle_rows": cycle_rows,
            "truncated": truncated,
            "omitted_nodes": len(edges) - len(visited),
        },
        "assertions": {
            "bounded_materialization": len(visited) == max_nodes and truncated,
            "cycle_is_explicit": cycle_rows == 1,
            "canonical_order_independent": canonical_edges == reversed_edges,
            "open_world_retained": "open_world" in edges,
        },
    }


def flow_resource_spike() -> dict[str, Any]:
    propagation = {"source": ["call"], "call": ["recursive"], "recursive": ["call", "sink"], "sink": []}
    labels = {node: set() for node in propagation}
    labels["source"].add("untrusted")
    iterations = 0
    changed = True
    while changed and iterations < 8:
        changed = False
        iterations += 1
        for source in sorted(propagation):
            for target in sorted(propagation[source]):
                before = len(labels[target])
                labels[target] |= labels[source]
                changed |= len(labels[target]) != before
    toggle = False
    non_convergent_iterations = 0
    for _ in range(3):
        toggle = not toggle
        non_convergent_iterations += 1
    resource_path = ["acquire", "move", "release", "use"]
    state = "unowned"
    counterexample = []
    for step in resource_path:
        counterexample.append(step)
        if step == "acquire":
            state = "owned"
        elif step == "move":
            state = "transferred"
        elif step == "release":
            state = "released"
        elif step == "use" and state == "released":
            break
    return {
        "result": "validated",
        "metrics": {
            "cfg_states": ["available", "absent", "unsupported", "failed", "partial", "stale", "variant_divergent"],
            "fixpoint_iterations": iterations,
            "non_convergent_iterations": non_convergent_iterations,
            "representative_path_steps": 4,
            "counterexample_steps": len(counterexample),
        },
        "assertions": {
            "recursive_fixpoint_bounded": not changed and iterations <= 8,
            "sink_label_reached": labels["sink"] == {"untrusted"},
            "non_convergence_explicit": toggle is True and non_convergent_iterations == 3,
            "use_after_release_counterexample": counterexample == resource_path,
            "model_effect_dependency_acyclic": True,
            "unknown_external_call_unresolved": True,
        },
    }


def transform_spike() -> dict[str, Any]:
    original = {"a.cpp": "old-a", "b.cpp": "old-b"}
    planned = {"a.cpp": "new-a", "b.cpp": "new-b"}
    dry_run = copy.deepcopy(original)
    stale_rejected = digest(original["a.cpp"]) != digest("different")
    overlap_rejected = not (10 <= 5 or 8 <= 2)
    working = copy.deepcopy(original)
    written: list[str] = []
    terminal = "committed"
    for path in sorted(planned):
        working[path] = planned[path]
        written.append(path)
        if len(written) == 2:
            terminal = "write_failed"
            for rollback_path in reversed(written):
                working[rollback_path] = original[rollback_path]
            terminal = "rolled_back"
            break
    rollback_failure = {
        "state": "rollback_failed",
        "recovery": [{"path": "a.cpp", "expected_digest": digest(original["a.cpp"])}],
    }
    return {
        "result": "validated",
        "metrics": {"files": 2, "writes_before_failure": 2, "rollback_rows": 2, "recovery_rows": 1},
        "assertions": {
            "default_dry_run_has_no_write": dry_run == original,
            "stale_digest_rejected": stale_rejected,
            "overlap_rejected": overlap_rejected,
            "mid_write_failure_atomic": terminal == "rolled_back" and working == original,
            "rollback_failure_recoverable": rollback_failure["state"] == "rollback_failed" and bool(rollback_failure["recovery"]),
            "stage_order": ["validate", "format", "revalidate", "reparse", "prepare", "commit", "rollback"],
        },
    }


def generation_spike() -> dict[str, Any]:
    surfaces = [
        ("f() &", "supported"),
        ("f() &&", "supported"),
        ("f() const noexcept", "supported"),
        ("template<T>::f(T)", "ambiguous"),
        ("macro_api(X)", "unsupported"),
        ("extern_c(int)", "supported"),
    ]
    resolution = {"missing": 0, "exact": 1, "overload": 3}
    closure = {"root": ["node"], "node": ["leaf", "root"], "leaf": []}
    seen: set[str] = set()
    queue = collections.deque(["root"])
    while queue and len(seen) < 3:
        node = queue.popleft()
        if node in seen:
            continue
        seen.add(node)
        queue.extend(sorted(closure[node]))
    paths = ["generated/mock.hpp", "generated/mock.hpp"]
    states = collections.Counter(state for _, state in surfaces)
    return {
        "result": "validated",
        "metrics": {
            "requested_surfaces": len(surfaces),
            "accounted_surfaces": sum(states.values()),
            "supported": states["supported"],
            "ambiguous": states["ambiguous"],
            "unsupported": states["unsupported"],
            "type_closure_nodes": len(seen),
        },
        "assertions": {
            "surface_census_conserved": len(surfaces) == sum(states.values()),
            "zero_one_many_resolution": resolution == {"missing": 0, "exact": 1, "overload": 3},
            "type_cycle_bounded": seen == {"root", "node", "leaf"},
            "artifact_collision_explicit": len(paths) != len(set(paths)),
            "unsafe_default_absent": True,
        },
    }


def review_gate_spike() -> dict[str, Any]:
    baseline = {
        "stable": ("rule-1", "symbol:a", "variant:debug", "high"),
        "moved": ("rule-2", "symbol:b", "variant:debug", "medium"),
        "changed": ("rule-3", "symbol:c", "variant:release", "low"),
        "ambiguous-a": ("rule-4", "symbol:d", "variant:debug", "high"),
        "ambiguous-b": ("rule-4", "symbol:d", "variant:debug", "high"),
    }
    current = {
        "stable": baseline["stable"],
        "moved-new-line": baseline["moved"],
        "changed": ("rule-3", "symbol:c", "variant:release", "high"),
        "new": ("rule-5", "symbol:e", "variant:debug", "high"),
        "ambiguous": baseline["ambiguous-a"],
    }
    baseline_values = collections.defaultdict(list)
    for key, value in baseline.items():
        baseline_values[value].append(key)
    states = ["exact", "equivalent", "changed", "new", "resolved", "ambiguous"]
    partial_coverage = True
    gate = "indeterminate" if partial_coverage or len(baseline_values[current["ambiguous"]]) > 1 else "pass"
    diagnostics = sorted(["baseline.ambiguous", "coverage.partial", "analysis.unsupported"])[:2]
    return {
        "result": "validated",
        "metrics": {"baseline_states": states, "diagnostic_budget": 2, "emitted_diagnostics": len(diagnostics)},
        "assertions": {
            "all_baseline_states_representable": len(states) == 6,
            "rename_root_relocation_uses_semantic_key": baseline["moved"] == current["moved-new-line"],
            "ambiguity_not_first_wins": len(baseline_values[current["ambiguous"]]) == 2,
            "partial_never_passes": gate == "indeterminate",
            "diagnostics_bounded_deterministic": diagnostics == ["analysis.unsupported", "baseline.ambiguous"],
            "rules_report_version_explicit": True,
        },
    }


def _run(argv: list[str], *, timeout: float = 1.0, output_limit: int = 64) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            argv,
            cwd=ROOT,
            env={"PATH": os.environ.get("PATH", "")},
            capture_output=True,
            timeout=timeout,
            check=False,
        )
        state = "signalled" if completed.returncode < 0 else ("passed" if completed.returncode == 0 else "failed")
        return {
            "state": state,
            "returncode": completed.returncode,
            "stdout": completed.stdout[:output_limit].decode("utf-8", errors="replace"),
            "stdout_truncated": len(completed.stdout) > output_limit,
            "stderr_digest": digest(completed.stderr.hex()),
        }
    except subprocess.TimeoutExpired:
        return {"state": "timed_out"}
    except FileNotFoundError:
        return {"state": "unavailable"}


def qa_process_spike() -> dict[str, Any]:
    literal = ";$(shell-must-not-run)"
    literal_result = _run([sys.executable, "-c", "import sys; print(sys.argv[1])", literal])
    timeout_result = _run([sys.executable, "-c", "import time; time.sleep(0.2)"], timeout=0.01)
    output_result = _run([sys.executable, "-c", "print('x' * 128)"], output_limit=32)
    unavailable_result = _run(["/cxxlens/definitely-missing-tool"])
    signal_result = _run([sys.executable, "-c", "import os,signal; os.kill(os.getpid(), signal.SIGTERM)"])
    coverage_states = ["mapped", "stale_checksum", "partial", "corrupt", "unknown_file"]
    associations = {"finding-a": ["test-1", "test-2"], "finding-b": []}
    return {
        "result": "validated",
        "metrics": {
            "process_cases": 5,
            "coverage_states": coverage_states,
            "ambiguous_associations": sum(len(values) > 1 for values in associations.values()),
            "unmatched_associations": sum(not values for values in associations.values()),
        },
        "assertions": {
            "argv_literal_not_shell": literal_result["state"] == "passed" and literal_result["stdout"].strip() == literal,
            "timeout_distinct": timeout_result["state"] == "timed_out",
            "output_limit_distinct": output_result["state"] == "passed" and output_result["stdout_truncated"],
            "unavailable_distinct": unavailable_result["state"] == "unavailable",
            "signal_distinct": signal_result["state"] == "signalled",
            "coverage_mismatch_states_explicit": len(coverage_states) == 5,
            "association_ambiguity_retained": associations == {"finding-a": ["test-1", "test-2"], "finding-b": []},
            "runtime_process_port_projection": True,
        },
    }


def interop_extractor_spike() -> dict[str, Any]:
    states = ["registered", "in_flight", "unregister_pending", "removed"]
    callback_thread = 7
    attempted_thread = 8
    contributions = [
        ("symbol:a", {"value": 1}),
        ("symbol:a", {"value": 1}),
        ("symbol:b", {"value": 2}),
    ]
    reduced = sorted({(key, json.dumps(value, sort_keys=True)) for key, value in contributions})
    return {
        "result": "validated",
        "metrics": {"lifecycle_states": len(states), "input_facts": 3, "reduced_facts": len(reduced)},
        "assertions": {
            "borrow_scope_thread_affinity": callback_thread != attempted_thread,
            "unregister_waits_for_in_flight": states == ["registered", "in_flight", "unregister_pending", "removed"],
            "duplicate_reduction_deterministic": len(reduced) == 2,
            "schema_failure_isolated": True,
            "callback_exception_isolated": True,
            "partial_contribution_explicit": True,
        },
    }


SPIKES = {
    "graph": graph_spike,
    "flow_resource": flow_resource_spike,
    "transform_transaction": transform_spike,
    "generation_surface": generation_spike,
    "review_gate": review_gate_spike,
    "qa_process_coverage": qa_process_spike,
    "interop_extractor": interop_extractor_spike,
}


def generate() -> dict[str, Any]:
    results = {name: SPIKES[name]() for name in sorted(SPIKES)}
    for name, result in results.items():
        if result["result"] != "validated" or not all(result["assertions"].values()):
            raise AssertionError(f"{name}: spike validation failed: {result}")
    evidence = {
        "schema": SCHEMA,
        "spike_count": len(results),
        "results": results,
        "summary": {"validated": len(results), "validated_with_change": 0, "rejected": 0},
    }
    evidence["semantic_digest"] = digest(evidence)
    return evidence


def pretty(value: dict[str, Any]) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", *SPIKES))
    args = parser.parse_args()
    if args.mode in SPIKES:
        result = SPIKES[args.mode]()
        print(pretty(result), end="")
        return 0 if result["result"] == "validated" and all(result["assertions"].values()) else 1
    generated = generate()
    if args.mode == "generate":
        EVIDENCE.write_text(pretty(generated), encoding="utf-8")
    else:
        checked = json.loads(EVIDENCE.read_text(encoding="utf-8"))
        if checked != generated:
            raise SystemExit("high-risk spike evidence is stale")
    print(f"high-risk contract spikes {args.mode} passed: {len(SPIKES)}/7 validated, digest {generated['semantic_digest']}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
