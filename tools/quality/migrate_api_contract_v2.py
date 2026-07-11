#!/usr/bin/env python3
"""One-time, deterministic migration from the draft v1 API catalog to v2."""

from __future__ import annotations

import argparse
import hashlib
import pathlib

import yaml


FACT_KINDS = (
    "call",
    "cfg_summary",
    "compile_command",
    "control_flow",
    "conversion",
    "coverage",
    "declaration",
    "definition",
    "effect_summary",
    "file",
    "finding",
    "flow_summary",
    "include_relation",
    "inheritance",
    "macro_expansion",
    "override_relation",
    "reference",
    "symbol",
    "type",
)

DEPENDENCY_EXPRESSIONS = {
    "method_inspection": ("declaration", "definition", "type", "control_flow"),
    "recipe_dependent": ("symbol", "type", "reference", "call"),
    "requested_profile": (),
    "selector_dependent": ("symbol", "type", "reference", "call"),
}

EXPRESSION_DESCRIPTIONS = {
    "method_inspection": "Facts selected by the normalized method-inspection request.",
    "recipe_dependent": "Concrete closure computed from the validated mutation recipe.",
    "requested_profile": "Concrete fact kinds explicitly present in the validated fact profile.",
    "selector_dependent": "Concrete closure computed from semantic_selector::requirements().",
}

CAPABILITIES = (
    "include.cleaner",
    "interop.clang",
    "qa.coverage",
    "qa.sanitizers",
)

USE_CASES = tuple(
    [f"UC-SR-{index:03d}" for index in range(1, 11)]
    + [f"UC-RL-{index:03d}" for index in range(1, 9)]
    + [f"UC-TF-{index:03d}" for index in range(1, 7)]
    + [f"UC-GN-{index:03d}" for index in range(1, 9)]
    + [f"UC-GR-{index:03d}" for index in range(1, 4)]
    + [f"UC-RV-{index:03d}" for index in range(1, 3)]
    + [f"UC-QA-{index:03d}" for index in range(1, 3)]
)

REQUIREMENTS = tuple(
    [f"FR-{index:03d}" for index in range(1, 21)]
    + [f"QR-{index:03d}" for index in range(1, 13)]
)


def fingerprint(signature: str) -> str:
    return "sha256:" + hashlib.sha256(signature.encode("utf-8")).hexdigest()


def migrate(document: dict) -> dict:
    if document.get("schema") not in {"cxxlens.api-catalog.v1", "cxxlens.api-catalog.v2"}:
        raise ValueError("unsupported input schema")
    if document.get("schema") == "cxxlens.api-catalog.v2":
        return document

    document["schema"] = "cxxlens.api-catalog.v2"
    document["document_version"] = "2.0.0-draft"
    document["authority"] = {
        "machine_readable_source": "schemas/cxxlens_public_api_contract.yaml",
        "human_inventory": "docs/design/api_catalog_inventory.md",
        "change_policy": "docs/design/api_catalog_change_policy.md",
    }
    document["registries"] = {
        "fact_kinds": list(FACT_KINDS),
        "capabilities": list(CAPABILITIES),
        "dependency_expressions": [
            {
                "id": name,
                "description": EXPRESSION_DESCRIPTIONS[name],
                "expands_to": sorted(expansion),
                "expansion_order": "lexicographic_unique",
            }
            for name, expansion in DEPENDENCY_EXPRESSIONS.items()
        ],
        "implementation_states": ["unimplemented", "implemented", "conformant"],
        "readiness_states": ["blocked", "ready", "complete"],
        "use_cases": sorted(USE_CASES),
        "requirements": sorted(REQUIREMENTS),
        "error_codes": sorted(
            {
                error
                for package in document["packages"]
                for api in package["apis"]
                for error in api.get("errors", [])
            }
        ),
    }
    document["migrations"] = [
        {
            "from_schema": "cxxlens.api-catalog.v1",
            "to_schema": "cxxlens.api-catalog.v2",
            "api_id_changes": [],
            "symbol_changes": [],
            "notes": [
                "All 123 API IDs and symbols are preserved.",
                "maturity is renamed to contract_maturity; implementation_state is independent.",
                "Placeholder fact names are migrated to typed dependency expressions.",
            ],
        }
    ]

    for package in document["packages"]:
        for api in package["apis"]:
            api_id = api["id"]
            api["contract_maturity"] = api.pop("maturity")
            implemented = api_id == "API-CORE-001"
            api["implementation_state"] = "conformant" if implemented else "unimplemented"
            if implemented:
                api["requirements"] = ["FR-018"]
                signature = "cxxlens::api_versions cxxlens::versions()"
                api["declaration"] = {
                    "status": "exact",
                    "source": "include/cxxlens/core.hpp",
                    "signature": signature,
                    "signature_fingerprint": fingerprint(signature),
                }
                api["implementation_evidence"] = [
                    "include/cxxlens/core.hpp",
                    "src/core/version.cpp",
                    "tests/unit/version_test.cpp",
                    "tests/public_headers/core_header_test.cpp",
                    "tests/install/consumer/main.cpp",
                ]
            else:
                api["declaration"] = {
                    "status": "unresolved",
                    "source": "docs/design/cxxlens_integrated_design_ja.md",
                    "signature": None,
                    "signature_fingerprint": None,
                }
            api["atomic_unit"] = {
                "id": "AU-" + api_id.removeprefix("API-"),
                "indivisible": True,
            }
            api["readiness"] = {
                "state": "complete" if implemented else "blocked",
                "blockers": [] if implemented else ["exact_declaration_unresolved"],
            }
            requires = api.get("requires")
            if requires:
                facts = requires.get("facts", [])
                expressions = sorted(fact for fact in facts if fact in DEPENDENCY_EXPRESSIONS)
                concrete = sorted(fact for fact in facts if fact not in DEPENDENCY_EXPRESSIONS)
                if concrete:
                    requires["facts"] = concrete
                else:
                    requires.pop("facts", None)
                if expressions:
                    requires["expressions"] = expressions
                if "capabilities" in requires:
                    requires["capabilities"] = sorted(set(requires["capabilities"]))
                requires.setdefault("apis", [])

    document["summary"] = {
        "package_count": len(document["packages"]),
        "api_entry_count": sum(len(package["apis"]) for package in document["packages"]),
        "atomic_unit_count": len(
            {
                api["atomic_unit"]["id"]
                for package in document["packages"]
                for api in package["apis"]
            }
        ),
        "implementation_state_counts": {"unimplemented": 122, "implemented": 0, "conformant": 1},
    }
    return document


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("catalog", type=pathlib.Path)
    args = parser.parse_args()
    document = yaml.safe_load(args.catalog.read_text(encoding="utf-8"))
    migrated = migrate(document)
    args.catalog.write_text(
        yaml.safe_dump(migrated, allow_unicode=True, sort_keys=False, width=100), encoding="utf-8"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
