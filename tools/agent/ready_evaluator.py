#!/usr/bin/env python3
"""Generate a deterministic atomic-unit DAG, ready predicate, prompts, and CI shards."""

from __future__ import annotations

import argparse
import collections
import copy
import hashlib
import json
import pathlib
import re
import sys
from typing import Any

import jsonschema
import yaml


ROOT = pathlib.Path(__file__).resolve().parents[2]
REPORT_SCHEMA = "cxxlens.api-ready.v1"
RUN_SCHEMA = "cxxlens.api-ready.run-manifest.v1"


class ReadyError(ValueError):
    """A ready-evaluator invariant violation with a stable code."""

    def __init__(self, code: str, message: str) -> None:
        super().__init__(f"{code}: {message}")
        self.code = code


def fail(code: str, message: str) -> None:
    raise ReadyError(code, message)


def canonical_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, separators=(",", ":"), sort_keys=True)


def pretty_json(value: Any) -> str:
    return json.dumps(value, ensure_ascii=False, indent=2, sort_keys=True) + "\n"


def digest(value: Any) -> str:
    return "sha256:" + hashlib.sha256(canonical_json(value).encode("utf-8")).hexdigest()


def canonicalize_input(value: Any) -> Any:
    if isinstance(value, dict):
        return {key: canonicalize_input(item) for key, item in sorted(value.items())}
    if isinstance(value, list):
        normalized = [canonicalize_input(item) for item in value]
        if all(isinstance(item, dict) and isinstance(item.get("id"), str) for item in normalized):
            return sorted(normalized, key=lambda item: item["id"])
        if all(
            isinstance(item, dict) and isinstance(item.get("request_id"), str)
            for item in normalized
        ):
            return sorted(normalized, key=lambda item: item["request_id"])
        if all(isinstance(item, (str, int, float, bool)) or item is None for item in normalized):
            return sorted(normalized, key=canonical_json)
        return normalized
    return value


def global_gates() -> list[dict[str, Any]]:
    commands = [
        (
            "task-packets",
            ["python3", "tools/agent/task_packet_generator.py", "check", "--root", "."],
            {},
        ),
        (
            "ownership",
            ["python3", "tools/agent/ownership_generator.py", "check", "--root", "."],
            {},
        ),
        ("configure", ["cmake", "--preset", "dev-clang"], {"CXX": "clang++"}),
        ("build", ["cmake", "--build", "--preset", "dev-clang"], {}),
        ("test", ["ctest", "--preset", "dev-clang", "--output-on-failure"], {}),
        (
            "quality",
            [
                "cmake",
                "--build",
                "--preset",
                "dev-clang",
                "--target",
                "cxxlens-quality",
            ],
            {},
        ),
    ]
    return [
        {
            "id": command_id,
            "state": "satisfied",
            "command": {"id": command_id, "argv": argv, "environment": environment},
        }
        for command_id, argv, environment in commands
    ]


def provider_registry(
    corpus: dict[str, Any], m1: dict[str, Any]
) -> dict[str, list[dict[str, Any]]]:
    references: dict[tuple[str, str], dict[str, Any]] = {}
    for packet in corpus["packets"]:
        for component in packet["dependencies"]["components"]:
            if component["kind"] not in {
                "fact_provider_implementation",
                "capability_provider_implementation",
            }:
                continue
            key = (component["kind"], component["subject"])
            if key in references and references[key] != component:
                fail("ready.provider-ambiguity", f"provider contract differs for {key[1]}")
            references[key] = component
    unit_states = {
        unit["id"]: unit["generation_state"] for unit in corpus["atomic_units"]
    }
    available_facts = set(m1["fact_kinds"])
    available_capabilities = {"interop.clang"}
    rows = {"facts": [], "capabilities": []}
    for (kind, subject), component in sorted(references.items()):
        available = subject in (
            available_facts
            if kind == "fact_provider_implementation"
            else available_capabilities
        )
        semantics_complete = unit_states.get(component["owner_atomic_unit"]) == "complete"
        rows[
            "facts" if kind == "fact_provider_implementation" else "capabilities"
        ].append(
            {
                "id": subject,
                "contract_id": component["id"],
                "kind": kind,
                "state": "available" if available else "unavailable",
                "semantics_state": "complete" if semantics_complete else "incomplete",
                "required_semantics_version": component["required_semantics_version"],
                "provided_semantics_version": (
                    component["required_semantics_version"] if semantics_complete else None
                ),
                "owner": {
                    "atomic_unit_id": component["owner_atomic_unit"],
                    "steward": component["steward"],
                },
                "source_contract": component["source_contract"],
                "evidence": (
                    [component["source_contract"]]
                    if available
                    else [f"provider_unavailable:{kind}:{subject}"]
                ),
            }
        )
    facts = rows["facts"]
    capabilities = rows["capabilities"]
    return {"facts": facts, "capabilities": capabilities}


def dependency_edges(corpus: dict[str, Any]) -> list[dict[str, Any]]:
    api_to_unit = {
        packet["api_id"]: packet["atomic_unit_id"] for packet in corpus["packets"]
    }
    grouped: dict[str, dict[str, Any]] = {}

    def add(edge: dict[str, Any], api_id: str) -> None:
        key_value = copy.deepcopy(edge)
        key_value.pop("via_api_ids")
        key = canonical_json(key_value)
        if key not in grouped:
            grouped[key] = edge
        grouped[key]["via_api_ids"].add(api_id)

    for packet in corpus["packets"]:
        source = packet["atomic_unit_id"]
        for dependency_api in packet["dependencies"]["apis"]:
            target = api_to_unit[dependency_api]
            if target == source:
                continue
            add(
                {
                    "from": source,
                    "to": target,
                    "scope": "readiness",
                    "kind": "api_dependency",
                    "component_id": None,
                    "reason": f"{packet['api_id']} requires {dependency_api}",
                    "owner": {
                        "atomic_unit_id": target,
                        "steward": f"steward.atomic.{target.lower()}",
                    },
                    "source_contract": "schemas/cxxlens_public_api_contract.yaml",
                    "required_semantics_version": None,
                    "via_api_ids": set(),
                },
                packet["api_id"],
            )
        for component in packet["dependencies"]["components"]:
            target = component["owner_atomic_unit"]
            if target == source:
                continue
            add(
                {
                    "from": source,
                    "to": target,
                    "scope": "readiness",
                    "kind": component["kind"],
                    "component_id": component["id"],
                    "reason": component["reason"],
                    "owner": {
                        "atomic_unit_id": target,
                        "steward": component["steward"],
                    },
                    "source_contract": component["source_contract"],
                    "required_semantics_version": component[
                        "required_semantics_version"
                    ],
                    "via_api_ids": set(),
                },
                packet["api_id"],
            )

    packets_by_package: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for packet in corpus["packets"]:
        packets_by_package[packet["package"]["id"]].append(packet)
    for package_id, packets in sorted(packets_by_package.items()):
        integration_id = f"integration.{package_id}"
        for packet in packets:
            shared = next(
                component
                for component in packet["dependencies"]["components"]
                if component["kind"] == "package_internal_engine"
            )
            add(
                {
                    "from": integration_id,
                    "to": packet["atomic_unit_id"],
                    "scope": "package_integration",
                    "kind": "package_integration_prerequisite",
                    "component_id": integration_id,
                    "reason": "package integration consumes the conformant atomic-unit artifact",
                    "owner": {
                        "atomic_unit_id": packet["atomic_unit_id"],
                        "steward": shared["steward"],
                    },
                    "source_contract": shared["source_contract"],
                    "required_semantics_version": shared[
                        "required_semantics_version"
                    ],
                    "via_api_ids": set(),
                },
                packet["api_id"],
            )
    edges = []
    for key in sorted(grouped):
        edge = grouped[key]
        edge["via_api_ids"] = sorted(edge["via_api_ids"])
        edges.append(edge)
    return edges


def topological_waves(unit_ids: set[str], edges: list[dict[str, Any]]) -> list[list[str]]:
    dependencies: dict[str, set[str]] = {unit_id: set() for unit_id in unit_ids}
    dependents: dict[str, set[str]] = {unit_id: set() for unit_id in unit_ids}
    for edge in edges:
        source, target = edge["from"], edge["to"]
        if source not in unit_ids or target not in unit_ids:
            fail("ready.dangling-edge", f"{source} -> {target}")
        dependencies[source].add(target)
        dependents[target].add(source)
    remaining = {unit_id: len(values) for unit_id, values in dependencies.items()}
    frontier = sorted(unit_id for unit_id, count in remaining.items() if count == 0)
    waves: list[list[str]] = []
    visited: set[str] = set()
    while frontier:
        waves.append(frontier)
        next_frontier: set[str] = set()
        for unit_id in frontier:
            visited.add(unit_id)
            for dependent in dependents[unit_id]:
                remaining[dependent] -= 1
                if remaining[dependent] == 0:
                    next_frontier.add(dependent)
        frontier = sorted(next_frontier)
    if visited != unit_ids:
        fail("ready.dependency-cycle", f"cycle units: {sorted(unit_ids - visited)}")
    return waves


def blocker(
    code: str,
    subject: str,
    steward: str,
    unit_id: str,
    chain: list[str] | None = None,
) -> dict[str, Any]:
    return {
        "code": code,
        "subject": subject,
        "steward": steward,
        "chain": chain or [unit_id, f"{code}:{subject}"],
    }


def unique_blockers(values: list[dict[str, Any]]) -> list[dict[str, Any]]:
    return sorted(
        {canonical_json(value): value for value in values}.values(),
        key=lambda value: (value["code"], value["subject"], value["steward"]),
    )


def make_shard(
    node: dict[str, Any],
    packets: list[dict[str, Any]],
    ownership_unit: dict[str, Any],
    gates: list[dict[str, Any]],
) -> dict[str, Any]:
    shard_id = node["shard_id"]
    evidence = sorted(
        {
            path
            for packet in packets
            for path in packet["implementation_evidence"]
            + [
                path
                for fixture in packet["fixtures"]
                for path in fixture["evidence_candidates"]
            ]
        }
    )
    shard = {
        "id": shard_id,
        "atomic_unit_id": node["atomic_unit_id"],
        "api_ids": node["api_ids"],
        "state": {
            "complete": "verification",
            "ready": "active",
            "blocked": "blocked",
        }[node["state"]],
        "package_integration_role": ownership_unit["package_integration_role"],
        "fixture_categories": ["ambiguous", "negative", "positive"],
        "selected_evidence_paths": evidence,
        "mandatory_gate_ids": [gate["id"] for gate in gates],
        "commands": [gate["command"] for gate in gates],
        "acceptance_command": {
            "id": "atomic-unit-acceptance",
            "argv": [
                "python3",
                "tools/agent/api_task_runner.py",
                "run",
                "--unit",
                node["atomic_unit_id"],
            ],
            "environment": {},
        },
    }
    shard["semantic_digest"] = digest(shard)
    return shard


def make_package_integration_shards(
    nodes: list[dict[str, Any]],
    shards: list[dict[str, Any]],
    packets_by_unit: dict[str, list[dict[str, Any]]],
    ownership: dict[str, Any],
    gates: list[dict[str, Any]],
) -> list[dict[str, Any]]:
    nodes_by_id = {node["atomic_unit_id"]: node for node in nodes}
    shards_by_unit = {shard["atomic_unit_id"]: shard for shard in shards}
    units_by_package: dict[str, set[str]] = collections.defaultdict(set)
    for unit_id, packets in packets_by_unit.items():
        package_ids = {packet["package"]["id"] for packet in packets}
        if len(package_ids) != 1:
            fail(
                "ready.cross-package-unit",
                f"{unit_id}: atomic unit spans packages {sorted(package_ids)}",
            )
        units_by_package[next(iter(package_ids))].add(unit_id)

    tracked_paths_by_role: dict[str, list[str]] = collections.defaultdict(list)
    for tracked in ownership["tracked_paths"]:
        tracked_paths_by_role[tracked["owner_role"]].append(tracked["path"])

    integration_shards = []
    for package_id, unit_ids in sorted(units_by_package.items()):
        role = f"integration.package.{package_id}"
        conformant_artifacts = []
        blocked_units = []
        for unit_id in sorted(unit_ids):
            node = nodes_by_id[unit_id]
            shard = shards_by_unit[unit_id]
            if node["state"] == "complete":
                conformant_artifacts.append(
                    {
                        "atomic_unit_id": unit_id,
                        "shard_id": shard["id"],
                        "shard_digest": shard["semantic_digest"],
                    }
                )
            else:
                blocked_units.append(unit_id)
        integration_shard = {
            "id": f"integration.{package_id}",
            "package_id": package_id,
            "package_integration_role": role,
            "state": "blocked" if blocked_units else "verification",
            "conformant_unit_artifacts": conformant_artifacts,
            "blocked_atomic_units": blocked_units,
            "allowed_write_paths": sorted(tracked_paths_by_role[role]),
            "mandatory_gate_ids": [gate["id"] for gate in gates],
            "commands": [gate["command"] for gate in gates],
            "acceptance_command": {
                "id": "package-integration-acceptance",
                "argv": [
                    "python3",
                    "tools/agent/api_task_runner.py",
                    "integrate",
                    "--package-id",
                    package_id,
                ],
                "environment": {},
            },
        }
        integration_shard["semantic_digest"] = digest(integration_shard)
        integration_shards.append(integration_shard)
    return integration_shards


def generate_report(
    corpus: dict[str, Any],
    ownership: dict[str, Any],
    m0: dict[str, Any],
    m1: dict[str, Any],
    m2: dict[str, Any],
    requests: dict[str, Any],
) -> dict[str, Any]:
    packets_by_unit: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for packet in corpus["packets"]:
        packets_by_unit[packet["atomic_unit_id"]].append(packet)
    ownership_units = {unit["atomic_unit_id"]: unit for unit in ownership["units"]}
    skeletons = {item["api_id"]: item for item in ownership["skeletons"]}
    unit_ids = set(packets_by_unit)
    edges = dependency_edges(corpus)
    readiness_edges = [edge for edge in edges if edge["scope"] == "readiness"]
    waves = topological_waves(unit_ids, readiness_edges)
    edges_by_source: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    for edge in readiness_edges:
        edges_by_source[edge["from"]].append(edge)
    provider_data = provider_registry(corpus, m1)
    providers_by_key = {
        (item["kind"], item["id"]): item
        for provider_kind in ("facts", "capabilities")
        for item in provider_data[provider_kind]
    }
    pending_requests: dict[str, list[dict[str, Any]]] = collections.defaultdict(list)
    request_rows = []
    for request in sorted(requests["requests"], key=lambda item: item["request_id"]):
        row = {
            key: request[key]
            for key in ("request_id", "state", "requesting_atomic_unit", "blocked_api_ids")
        }
        request_rows.append(row)
        if request["state"] in {"pending", "accepted"}:
            pending_requests[request["requesting_atomic_unit"]].append(request)
    gates = global_gates()
    nodes_by_id: dict[str, dict[str, Any]] = {}
    for wave in waves:
        for unit_id in wave:
            packets = sorted(packets_by_unit[unit_id], key=lambda item: item["api_id"])
            dependency_units = sorted(
                {
                    dependency
                    for packet in packets
                    for dependency in packet["dependencies"]["atomic_units"]
                    if dependency != unit_id
                }
            )
            facts = sorted(
                {fact for packet in packets for fact in packet["dependencies"]["facts"]}
                | {
                    fact
                    for packet in packets
                    for expression in packet["dependencies"]["expressions"]
                    for fact in expression["expands_to"]
                }
            )
            capabilities = sorted(
                {
                    capability
                    for packet in packets
                    for capability in packet["dependencies"]["capabilities"]
                }
            )
            shared_components = sorted(
                {
                    component["id"]
                    for packet in packets
                    for component in packet["dependencies"]["components"]
                }
            )
            exact = all(packet["declaration"]["status"] == "exact" for packet in packets)
            frozen = all(skeletons[packet["api_id"]]["state"] == "frozen" for packet in packets)
            owned = unit_id in ownership_units and bool(
                ownership_units[unit_id]["allowed_write_prefixes"]
            )
            fixtures = all(
                {fixture["category"] for fixture in packet["fixtures"]}
                == {"positive", "negative", "ambiguous"}
                for packet in packets
            )
            required_provider_rows = [
                providers_by_key.get(("fact_provider_implementation", fact))
                for fact in facts
            ] + [
                providers_by_key.get(("capability_provider_implementation", capability))
                for capability in capabilities
            ]
            provider_availability = all(
                row is not None and row["state"] == "available"
                for row in required_provider_rows
            )
            provider_semantics = all(
                row is not None
                and row["semantics_state"] == "complete"
                and row["provided_semantics_version"]
                == row["required_semantics_version"]
                for row in required_provider_rows
            )
            providers = provider_availability and provider_semantics
            dependencies_ready = all(
                nodes_by_id[dependency]["state"] == "complete"
                for dependency in dependency_units
            )
            blockers = []
            for packet in packets:
                for reason in packet["generation"]["block_reasons"]:
                    blockers.append(blocker(reason, packet["api_id"], "generator.catalog", unit_id))
            if not exact:
                blockers.append(
                    blocker("exact-contract-missing", unit_id, "generator.catalog", unit_id)
                )
            if not frozen:
                blockers.append(
                    blocker("frozen-skeleton-missing", unit_id, "steward.ownership", unit_id)
                )
            if not owned:
                blockers.append(
                    blocker("ownership-missing", unit_id, "steward.ownership", unit_id)
                )
            if not fixtures:
                blockers.append(blocker("fixtures-missing", unit_id, "steward.testing", unit_id))
            for fact in facts:
                provider = providers_by_key.get(("fact_provider_implementation", fact))
                if provider is None or provider["state"] != "available":
                    blockers.append(
                        blocker("fact-provider-unavailable", fact, "steward.facts", unit_id)
                    )
                elif (
                    provider["semantics_state"] != "complete"
                    or provider["provided_semantics_version"]
                    != provider["required_semantics_version"]
                ):
                    blockers.append(
                        blocker(
                            "provider-semantics-incomplete",
                            provider["contract_id"],
                            provider["owner"]["steward"],
                            unit_id,
                            [
                                unit_id,
                                provider["contract_id"],
                                provider["owner"]["atomic_unit_id"],
                            ],
                        )
                    )
            for capability in capabilities:
                provider = providers_by_key.get(
                    ("capability_provider_implementation", capability)
                )
                if provider is None or provider["state"] != "available":
                    blockers.append(
                        blocker(
                            "capability-provider-unavailable",
                            capability,
                            "steward.repository",
                            unit_id,
                        )
                    )
                elif (
                    provider["semantics_state"] != "complete"
                    or provider["provided_semantics_version"]
                    != provider["required_semantics_version"]
                ):
                    blockers.append(
                        blocker(
                            "provider-semantics-incomplete",
                            provider["contract_id"],
                            provider["owner"]["steward"],
                            unit_id,
                            [
                                unit_id,
                                provider["contract_id"],
                                provider["owner"]["atomic_unit_id"],
                            ],
                        )
                    )
            for edge in edges_by_source[unit_id]:
                dependency = edge["to"]
                dependency_node = nodes_by_id[dependency]
                if dependency_node["state"] != "complete":
                    code = (
                        "dependency-not-complete"
                        if edge["kind"] == "api_dependency"
                        else "shared-component-not-complete"
                    )
                    subject = edge["component_id"] or dependency
                    downstream = (
                        dependency_node["blockers"][0]["chain"]
                        if dependency_node["blockers"]
                        else [dependency]
                    )
                    blockers.append(
                        blocker(
                            code,
                            subject,
                            edge["owner"]["steward"],
                            unit_id,
                            [unit_id, subject, *downstream],
                        )
                    )
            for request in pending_requests[unit_id]:
                blockers.append(
                    blocker(
                        "dependency-request-open",
                        request["request_id"],
                        request["steward_target"],
                        unit_id,
                    )
                )
            packet_states = {packet["generation"]["state"] for packet in packets}
            if packet_states == {"complete"}:
                state = "complete"
                blockers = []
            elif packet_states == {"ready"} and all(
                (exact, frozen, owned, fixtures, providers, dependencies_ready)
            ) and not pending_requests[unit_id]:
                state = "ready"
            else:
                state = "blocked"
            node = {
                "atomic_unit_id": unit_id,
                "api_ids": [packet["api_id"] for packet in packets],
                "phases": sorted({packet["phase"] for packet in packets}),
                "state": state,
                "dependencies": dependency_units,
                "dependency_kinds": sorted(
                    {edge["kind"] for edge in edges_by_source[unit_id]}
                ),
                "shared_components": shared_components,
                "required_facts": facts,
                "required_capabilities": capabilities,
                "prerequisites": {
                    "exact_contract": exact,
                    "frozen_skeleton": frozen,
                    "ownership": owned,
                    "fixtures": fixtures,
                    "providers": providers,
                    "provider_availability": provider_availability,
                    "provider_semantics": provider_semantics,
                    "dependencies": dependencies_ready,
                    "global_gates": True,
                },
                "blockers": unique_blockers(blockers),
                "shard_id": f"shard.{unit_id.lower()}",
            }
            nodes_by_id[unit_id] = node
    nodes = [nodes_by_id[unit_id] for unit_id in sorted(nodes_by_id)]
    shards = [
        make_shard(
            node,
            packets_by_unit[node["atomic_unit_id"]],
            ownership_units[node["atomic_unit_id"]],
            gates,
        )
        for node in nodes
    ]
    package_integration_shards = make_package_integration_shards(
        nodes, shards, packets_by_unit, ownership, gates
    )
    state_counts = collections.Counter(node["state"] for node in nodes)
    report: dict[str, Any] = {
        "schema": REPORT_SCHEMA,
        "input_fingerprints": {
            "task_packets": corpus["semantic_digest"],
            "ownership": ownership["semantic_digest"],
            "m0": digest(canonicalize_input(m0)),
            "m1": digest(canonicalize_input(m1)),
            "m2": digest(canonicalize_input(m2)),
            "dependency_requests": digest(canonicalize_input(requests)),
        },
        "providers": provider_data,
        "global_gates": gates,
        "dependency_requests": request_rows,
        "edges": edges,
        "topological_waves": waves,
        "nodes": nodes,
        "shards": shards,
        "package_integration_shards": package_integration_shards,
        "summary": {
            "api_count": len(corpus["packets"]),
            "unit_count": len(nodes),
            "package_count": len(package_integration_shards),
            "edge_count": len(edges),
            "readiness_edge_count": len(readiness_edges),
            "edge_kind_counts": dict(
                sorted(collections.Counter(edge["kind"] for edge in edges).items())
            ),
            "state_counts": {
                state: state_counts.get(state, 0) for state in ("blocked", "complete", "ready")
            },
            "ready_waves": [
                ready
                for wave in waves
                if (
                    ready := [
                        unit_id
                        for unit_id in wave
                        if nodes_by_id[unit_id]["state"] == "ready"
                    ]
                )
            ],
        },
    }
    report["semantic_digest"] = digest(report)
    return report


def _schema_validate(value: Any, schema: dict[str, Any], context: str) -> None:
    try:
        jsonschema.Draft202012Validator.check_schema(schema)
        jsonschema.Draft202012Validator(schema).validate(value)
    except (jsonschema.SchemaError, jsonschema.ValidationError) as error:
        fail("ready.schema-invalid", f"{context}: {error.message}")


def validate_report(
    report: dict[str, Any],
    expected: dict[str, Any],
    schema: dict[str, Any] | None = None,
) -> None:
    if not isinstance(report, dict) or report.get("schema") != REPORT_SCHEMA:
        fail("ready.unknown-schema", "ready report schema is not supported")
    provider_ids = [
        (provider["kind"], provider["id"])
        for kind in ("facts", "capabilities")
        for provider in report.get("providers", {}).get(kind, [])
    ]
    if len(provider_ids) != len(set(provider_ids)):
        fail("ready.provider-ambiguity", "provider identifiers are ambiguous")
    nodes = report.get("nodes", [])
    node_ids = [node["atomic_unit_id"] for node in nodes]
    if len(node_ids) != len(set(node_ids)):
        fail("ready.duplicate-unit", "DAG unit identifiers are not unique")
    edges = report.get("edges", [])
    readiness_edges = [edge for edge in edges if edge.get("scope") == "readiness"]
    topological_waves(set(node_ids), readiness_edges)
    integration_ids = {
        integration["id"] for integration in report.get("package_integration_shards", [])
    }
    known_graph_ids = set(node_ids) | integration_ids
    for edge in edges:
        if edge.get("from") not in known_graph_ids or edge.get("to") not in known_graph_ids:
            fail("ready.dangling-edge", f"{edge.get('from')} -> {edge.get('to')}")
        if not edge.get("reason") or not edge.get("source_contract"):
            fail("ready.edge-provenance-missing", f"{edge.get('from')} -> {edge.get('to')}")
        owner = edge.get("owner", {})
        if not owner.get("steward") or owner.get("atomic_unit_id") not in node_ids:
            fail("ready.edge-owner-missing", f"{edge.get('from')} -> {edge.get('to')}")
    shards = report.get("shards", [])
    shard_units = [shard["atomic_unit_id"] for shard in shards]
    if sorted(shard_units) != sorted(node_ids) or len(shard_units) != len(
        set(shard_units)
    ):
        fail("ready.shard-coverage", "atomic-unit shards do not cover DAG nodes exactly once")
    api_ids = [api_id for node in nodes for api_id in node["api_ids"]]
    if len(api_ids) != len(set(api_ids)):
        fail("ready.api-ambiguity", "API identifiers resolve to multiple DAG nodes")
    shard_by_id = {shard["id"]: shard for shard in shards}
    covered_units: set[str] = set()
    package_ids: set[str] = set()
    for integration in report.get("package_integration_shards", []):
        package_id = integration["package_id"]
        if package_id in package_ids:
            fail(
                "ready.package-shard-ambiguity",
                f"duplicate package integration shard: {package_id}",
            )
        package_ids.add(package_id)
        conformant = {
            artifact["atomic_unit_id"]
            for artifact in integration["conformant_unit_artifacts"]
        }
        blocked = set(integration["blocked_atomic_units"])
        if conformant & blocked:
            fail(
                "ready.package-shard-overlap",
                f"{package_id}: unit is both conformant and blocked",
            )
        for artifact in integration["conformant_unit_artifacts"]:
            shard = shard_by_id.get(artifact["shard_id"])
            if (
                shard is None
                or shard["atomic_unit_id"] != artifact["atomic_unit_id"]
                or shard["state"] != "verification"
                or shard["semantic_digest"] != artifact["shard_digest"]
            ):
                fail(
                    "ready.nonconformant-integration-input",
                    f"{package_id}: {artifact['atomic_unit_id']}",
                )
        covered_units.update(conformant | blocked)
    if covered_units != set(node_ids):
        fail("ready.package-shard-coverage", "package shards do not cover every DAG node")
    expected_states = {node["atomic_unit_id"]: node["state"] for node in expected["nodes"]}
    for node in nodes:
        if node["state"] == "ready" and expected_states.get(node["atomic_unit_id"]) != "ready":
            fail(
                "ready.maturity-not-readiness",
                f"{node['atomic_unit_id']}: ready lacks prerequisite evidence",
            )
        if node["state"] == "ready" and not all(node["prerequisites"].values()):
            fail(
                "ready.prerequisite-incomplete",
                f"{node['atomic_unit_id']}: ready has an incomplete prerequisite closure",
            )
    unsigned = copy.deepcopy(report)
    semantic_digest = unsigned.pop("semantic_digest", None)
    if semantic_digest != digest(unsigned):
        fail("ready.digest-mismatch", "ready report digest mismatch")
    if schema is not None:
        _schema_validate(report, schema, "ready report")
    if report != expected:
        fail("ready.stale-report", "ready report is stale")


def parse_prompt(prompt: str) -> str:
    api_ids = sorted(set(re.findall(r"API-[A-Z]+-[0-9]{3}", prompt)))
    if len(api_ids) != 1:
        fail("ready.prompt-ambiguous", f"prompt must contain exactly one API ID: {api_ids}")
    return api_ids[0]


def resolve_api(
    api_id: str,
    report: dict[str, Any],
    corpus: dict[str, Any],
    ownership: dict[str, Any],
) -> dict[str, Any]:
    packet = next((item for item in corpus["packets"] if item["api_id"] == api_id), None)
    if packet is None:
        fail("ready.unknown-api", api_id)
    node = next(
        item
        for item in report["nodes"]
        if item["atomic_unit_id"] == packet["atomic_unit_id"]
    )
    shard = next(item for item in report["shards"] if item["id"] == node["shard_id"])
    unit = next(
        item for item in ownership["units"] if item["atomic_unit_id"] == node["atomic_unit_id"]
    )
    run_manifest = {
        "schema": RUN_SCHEMA,
        "api_id": api_id,
        "atomic_unit_id": node["atomic_unit_id"],
        "shard_id": shard["id"],
        "start_authorized": node["state"] == "ready",
        "state": node["state"],
        "task_packet_digest": packet["semantic_digest"],
        "ownership_digest": ownership["semantic_digest"],
        "shard_digest": shard["semantic_digest"],
        "allowed_write_prefixes": unit["allowed_write_prefixes"],
        "selected_evidence_paths": shard["selected_evidence_paths"],
        "acceptance_command": shard["acceptance_command"],
        "blockers": node["blockers"],
    }
    run_manifest["semantic_digest"] = digest(run_manifest)
    return run_manifest


def load_json(path: pathlib.Path) -> Any:
    return json.loads(path.read_text(encoding="utf-8"))


def load_yaml(path: pathlib.Path) -> Any:
    return yaml.safe_load(path.read_text(encoding="utf-8"))


def resolve(root: pathlib.Path, path: pathlib.Path) -> pathlib.Path:
    return path if path.is_absolute() else root / path


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("mode", choices=("generate", "check", "resolve"))
    parser.add_argument("--root", type=pathlib.Path, default=ROOT)
    parser.add_argument("--api-id")
    parser.add_argument("--prompt")
    return parser.parse_args()


def main() -> int:
    args = arguments()
    root = args.root.resolve()
    corpus = load_json(root / "schemas/cxxlens.agent-task-packet-corpus.v1.json")
    ownership = load_json(root / "schemas/cxxlens.agent-ownership.v1.json")
    m0 = load_yaml(root / "schemas/cxxlens_m0_completion.yaml")
    m1 = load_yaml(root / "schemas/cxxlens_m1_completion.yaml")
    m2 = load_yaml(root / "schemas/cxxlens_m2_completion.yaml")
    requests = load_json(root / "schemas/cxxlens.dependency-request.examples.v1.json")
    schema = load_yaml(root / "schemas/cxxlens.api-ready.v1.schema.yaml")
    run_schema = load_yaml(root / "schemas/cxxlens.api-ready.run-manifest.v1.schema.yaml")
    report_path = root / "schemas/cxxlens.api-ready.report.v1.json"
    generated = generate_report(corpus, ownership, m0, m1, m2, requests)
    _schema_validate(generated, schema, "generated ready report")
    if args.mode == "generate":
        report_path.write_text(pretty_json(generated), encoding="utf-8")
    else:
        report = load_json(report_path)
        validate_report(report, generated, schema)
        if args.mode == "resolve":
            api_id = args.api_id or parse_prompt(args.prompt or "")
            resolution = resolve_api(api_id, report, corpus, ownership)
            _schema_validate(resolution, run_schema, "API resolution")
            print(pretty_json(resolution), end="")
    print(
        f"ready evaluator {args.mode} passed: {generated['summary']['unit_count']} units, "
        f"states {generated['summary']['state_counts']}, "
        f"digest {generated['semantic_digest']}"
    )
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (json.JSONDecodeError, OSError, ReadyError, yaml.YAMLError) as error:
        print(f"ready evaluation failed: {error}", file=sys.stderr)
        raise SystemExit(1) from error
