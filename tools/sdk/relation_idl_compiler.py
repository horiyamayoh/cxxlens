#!/usr/bin/env python3
"""Generate C++23 relation tags from the accepted relation registry."""

from __future__ import annotations

import argparse
import copy
import hashlib
import json
import pathlib
import re
import sys

import jsonschema
import yaml


SCRIPT_PATH = pathlib.Path(__file__).resolve()


def default_layout() -> tuple[pathlib.Path, pathlib.Path]:
    """Return the registry and generated-header roots for source or install use."""
    source_root = SCRIPT_PATH.parents[2]
    source_registry = source_root / "schemas/cxxlens_ng_relation_registry.yaml"
    if source_registry.is_file():
        return source_registry, source_root / "include/cxxlens/relations"

    install_prefix = SCRIPT_PATH.parent.parent
    return (
        install_prefix
        / "share/cxxlens/schemas/cxxlens_ng_relation_registry.yaml",
        install_prefix / "include/cxxlens/relations",
    )


DEFAULT_REGISTRY, DEFAULT_OUTPUT_DIR = default_layout()


def canonical_relation(relation: dict[str, object]) -> dict[str, object]:
    """Canonicalize descriptor semantics, excluding installed-header admission metadata."""
    canonical = copy.deepcopy(relation)
    # `cpp_projection` controls the public admission workflow, not the descriptor
    # semantics bound into a tag or snapshot. Keeping it outside this canonical
    # projection lets an authority-only admission proposal avoid silently changing
    # the already installed eleven descriptor bindings.
    projection = canonical.pop("cpp_projection", None)
    # Registry 1.4 included this spelling in the three dynamic observation
    # descriptor digests. Keep the published binding stable while 1.5 moves
    # admission to `cpp_projection`.
    if projection == "dynamic-only":
        canonical["api_surface"] = "dynamic_only"
    references = canonical.get("references", [])
    assert isinstance(references, list)
    references.sort(
        key=lambda reference: (
            tuple(reference["source_columns"]),
            str(reference["strength"]),
            str(reference["target_relation"]),
            tuple(reference["target_columns"]),
            bool(reference.get("container_elements", False)),
        )
    )
    merge = canonical["merge"]
    assert isinstance(merge, dict)
    conflict_columns = merge.get("conflict_columns", [])
    assert isinstance(conflict_columns, list)
    conflict_columns.sort()
    row_constraints = canonical.get("row_constraints")
    if row_constraints is not None:
        assert isinstance(row_constraints, dict)
        all_or_none = row_constraints.get("all_or_none", [])
        assert isinstance(all_or_none, list)
        normalized_groups: list[list[object]] = []
        for group in all_or_none:
            assert isinstance(group, list)
            normalized_groups.append(sorted(group, key=str))
        normalized_groups.sort(key=lambda group: tuple(map(str, group)))
        row_constraints["all_or_none"] = normalized_groups
    return canonical


def parse_type(value: str) -> tuple[str, str, bool]:
    optional = value.startswith("optional<") and value.endswith(">")
    if optional:
        value = value[len("optional<") : -1]
    parameter = ""
    match = re.fullmatch(r"([a-z0-9_]+)<(.+)>", value)
    if match:
        value, parameter = match.groups()
    kinds = {
        "bool": "boolean",
        "int64": "signed_integer",
        "uint64": "unsigned_integer",
        "utf8_string": "utf8_string",
        "bytes": "bytes",
        "digest": "digest",
        "semantic_version": "semantic_version",
        "typed_id": "typed_id",
        "open_symbol": "open_symbol",
        "condition_ref": "condition_ref",
        "source_span_id": "source_span_id",
        "evidence_id": "evidence_id",
        "relation_name": "relation_name",
        "semantic_key_id": "semantic_key_id",
        "assertion_id": "assertion_id",
        "content_digest": "content_digest",
        "interpretation_domain_id": "interpretation_domain_id",
        "closed_symbol": "closed_symbol",
        "set": "set",
    }
    if value not in kinds:
        raise ValueError(f"unsupported generated SDK type: {value}")
    return kinds[value], parameter, optional


def string(value: str) -> str:
    return value.replace("\\", "\\\\").replace('"', '\\"')


def type_expr(value: str) -> str:
    kind, parameter, optional = parse_type(value)
    return (
        f'{{sdk::scalar_kind::{kind}, "{string(parameter)}", '
        f'{str(optional).lower()}}}'
    )


def render(relation: dict[str, object]) -> str:
    if relation.get("cpp_projection") == "dynamic-only":
        raise ValueError(
            f"dynamic-only relation has no generated C++ tag: {relation['name']}"
        )
    relation = canonical_relation(relation)
    qualified_value = relation.get("generated_cpp_tag")
    if not isinstance(qualified_value, str):
        raise ValueError(f"relation has no generated_cpp_tag: {relation['name']}")
    qualified = qualified_value
    parts = qualified.split("::")
    if parts[0] != "cxxlens" or len(parts) < 3:
        raise ValueError(f"invalid generated_cpp_tag: {qualified}")
    namespace = "::".join(parts[:-1])
    tag = parts[-1]
    descriptor_id = str(relation["descriptor_id"])
    generated_filename = str(relation["name"]).replace(".", "_") + ".hpp"
    contract_canonical = json.dumps(
        relation, ensure_ascii=False, separators=(",", ":"), sort_keys=True
    )
    contract_digest = "sha256:" + hashlib.sha256(
        contract_canonical.encode("utf-8")
    ).hexdigest()
    columns = relation["columns"]
    assert isinstance(columns, list)
    lines = [
        "#pragma once",
        "",
        "/**",
        f" * @file {generated_filename}",
        " * @brief Generated by cxxlens relation_idl_compiler.py; do not hand edit.",
        " */",
        "#include <cxxlens/sdk/query.hpp>",
        "",
        "// clang-format off",
        f"namespace {namespace}",
        "{",
        f'\t/** @brief Generated relation tag backed by descriptor `{descriptor_id}`. */',
        f"\tstruct {tag}",
        "\t{",
        "\t\t/** @brief Return the immutable descriptor shared with dynamic lookup. */",
        "\t\t[[nodiscard]] static const sdk::relation_descriptor& descriptor()",
        "\t\t{",
        "\t\t\tstatic const sdk::relation_descriptor value = []",
        "\t\t\t{",
        "\t\t\t\tsdk::relation_descriptor output;",
        f'\t\t\t\toutput.id = "{string(descriptor_id)}";',
        f'\t\t\t\toutput.name = "{string(str(relation["name"]))}";',
    ]
    version = [int(part) for part in str(relation["version"]).split(".")]
    lines.extend(
        [
            f"\t\t\t\toutput.version = {{{version[0]}U, {version[1]}U, {version[2]}U}};",
            f'\t\t\t\toutput.semantic_major = {int(relation["semantic_major"])}U;',
            f'\t\t\t\toutput.semantics = "{string(str(relation["semantics"]))}";',
            f'\t\t\t\toutput.owner_namespace = "{string(str(relation["owner_namespace"]))}";',
            f'\t\t\t\toutput.contract_canonical = R"cxxlens({contract_canonical})cxxlens";',
            f'\t\t\t\toutput.contract_digest = "{contract_digest}";',
            "\t\t\t\toutput.columns = {",
        ]
    )
    for column in columns:
        assert isinstance(column, dict)
        required = bool(column.get("required", False))
        role = str(column.get("identity_role", "auxiliary"))
        lines.append(
            f'\t\t\t\t\t{{"{string(str(column["id"]))}", '
            f'"{string(str(column["name"]))}", {type_expr(str(column["type"]))}, '
            f'{str(required).lower()}, sdk::column_role::{role}}},'
        )
    claim = relation["claim"]
    assert isinstance(claim, dict)
    key = claim["key"]
    assert isinstance(key, list)
    domain_identity = claim["domain_identity"]
    assert isinstance(domain_identity, dict)
    identity_projection = domain_identity["projection"]
    assert isinstance(identity_projection, list)
    result_column = domain_identity["result_column"]
    references = relation.get("references", [])
    assert isinstance(references, list)
    merge = relation["merge"]
    assert isinstance(merge, dict)
    lines.extend(
        [
            "\t\t\t\t};",
            *(
                [
                    "\t\t\t\toutput.domain_identity.result_column = "
                    f'"{string(str(result_column))}";'
                ]
                if result_column is not None
                else []
            ),
            "\t\t\t\toutput.domain_identity.projection = {",
            *[
                f'\t\t\t\t\t"{string(str(value))}",'
                for value in identity_projection
            ],
            "\t\t\t\t};",
            "\t\t\t\toutput.domain_identity.contract = "
            f'"{string(str(domain_identity["contract"]))}";',
            "\t\t\t\toutput.key_columns = {",
            *[f'\t\t\t\t\t"{string(str(value))}",' for value in key],
            "\t\t\t\t};",
            "\t\t\t\toutput.references = {",
        ]
    )
    for reference in references:
        assert isinstance(reference, dict)
        source = ", ".join(f'"{string(str(value))}"' for value in reference["source_columns"])
        target = ", ".join(f'"{string(str(value))}"' for value in reference["target_columns"])
        container = ", true" if reference.get("container_elements", False) else ""
        lines.append(
            f'\t\t\t\t\t{{{{{source}}}, "{string(str(reference["target_relation"]))}", '
            f'{{{target}}}, sdk::reference_strength::{reference["strength"]}{container}}},'
        )
    conflict_columns = merge.get("conflict_columns", [])
    assert isinstance(conflict_columns, list)
    lines.extend(
        [
            "\t\t\t\t};",
            f'\t\t\t\toutput.merge = sdk::merge_mode::{merge["mode"]};',
            "\t\t\t\toutput.conflict_columns = {",
            *[f'\t\t\t\t\t"{string(str(value))}",' for value in conflict_columns],
            "\t\t\t\t};",
            "\t\t\t\toutput.descriptor_digest = *sdk::semantic_digest(",
            '\t\t\t\t\t"cxxlens.relation-descriptor-binding.v2",',
            "\t\t\t\t\toutput.contract_digest + \"\\n\" + output.canonical_form());",
            "\t\t\t\treturn output;",
            "\t\t\t}();",
            "\t\t\treturn value;",
            "\t\t}",
            f"\t\tusing builder = sdk::static_row_builder<{tag}>;",
            f"\t\tusing view = sdk::static_row_view<{tag}>;",
        ]
    )
    for column in columns:
        assert isinstance(column, dict)
        column_tag = str(column["name"])
        if column_tag == tag:
            column_tag += "_column"
        lines.extend(
            [
                f'\t\t/** @brief Generated column tag for `{column["id"]}`. */',
                f"\t\tstruct {column_tag}",
                "\t\t{",
                "\t\t\t/** @brief Materialize the stable descriptor/column/type reference. */",
                "\t\t\t[[nodiscard]] static sdk::column_ref ref()",
                "\t\t\t{",
                f'\t\t\t\treturn {{{tag}::descriptor().id, "{string(str(column["id"]))}", '
                f'{type_expr(str(column["type"]))}}};',
                "\t\t\t}",
                "\t\t};",
            ]
        )
    lines.extend(["\t};", f"}} // namespace {namespace}", "// clang-format on", ""])
    return "\n".join(lines)


def generated_filename(relation: dict[str, object]) -> str:
    """Return the deterministic public-header name for a static relation."""
    return str(relation["name"]).replace(".", "_") + ".hpp"


def load_registry(registry_path: pathlib.Path) -> dict[str, object]:
    """Load and validate the relation registry before any generation occurs."""
    document = yaml.safe_load(registry_path.read_text(encoding="utf-8"))
    schema_path = registry_path.with_name(
        "cxxlens_ng_relation_registry.schema.yaml"
    )
    schema = yaml.safe_load(schema_path.read_text(encoding="utf-8"))
    try:
        jsonschema.Draft202012Validator(schema).validate(document)
    except jsonschema.ValidationError as error:
        raise ValueError(
            f"relation registry validation failed: {error.message}"
        ) from error

    relations = document["relations"]
    names = [str(item["name"]) for item in relations]
    tags = [
        str(item["generated_cpp_tag"])
        for item in relations
        if item["generated_cpp_tag"] is not None
    ]
    if len(names) != len(set(names)) or len(tags) != len(set(tags)):
        raise ValueError("relation registry contains duplicate names or C++ tags")
    return document


def generated_relations(document: dict[str, object]) -> list[dict[str, object]]:
    """Return every relation admitted to the installed static C++ projection."""
    relations = document["relations"]
    result: list[dict[str, object]] = []
    for relation in relations:
        projection = relation.get("cpp_projection")
        tag = relation.get("generated_cpp_tag")
        if projection == "dynamic-only":
            if tag is not None:
                raise ValueError(
                    "dynamic-only relation has a generated C++ tag: "
                    f"{relation['name']}"
                )
            continue
        if not isinstance(tag, str):
            raise ValueError(f"relation has no generated_cpp_tag: {relation['name']}")
        result.append(relation)
    return result


def check_generated_headers(
    relations: list[dict[str, object]], output_dir: pathlib.Path
) -> list[str]:
    """Return deterministic drift diagnostics for a generated-header directory."""
    expected = {
        generated_filename(relation): render(relation) for relation in relations
    }
    actual_paths = {
        path.name: path for path in output_dir.glob("*.hpp") if path.is_file()
    }
    diagnostics: list[str] = []
    for filename in sorted(set(expected) - set(actual_paths)):
        diagnostics.append(
            f"missing generated relation header: {output_dir / filename}"
        )
    for filename in sorted(set(actual_paths) - set(expected)):
        diagnostics.append(f"unexpected relation header: {actual_paths[filename]}")
    for filename in sorted(set(expected) & set(actual_paths)):
        path = actual_paths[filename]
        if path.read_text(encoding="utf-8") != expected[filename]:
            diagnostics.append(f"generated relation header differs: {path}")
    return diagnostics


def arguments() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--registry",
        type=pathlib.Path,
        default=DEFAULT_REGISTRY,
    )
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--relation")
    mode.add_argument("--all", action="store_true")
    parser.add_argument("--output", type=pathlib.Path)
    parser.add_argument("--output-dir", type=pathlib.Path)
    parser.add_argument(
        "--check",
        action="store_true",
        help="check generated output instead of writing it",
    )
    return parser.parse_args()


def main() -> int:
    args = arguments()
    try:
        document = load_registry(args.registry)
        if args.all:
            if args.output is not None:
                print("--output is only valid with --relation", file=sys.stderr)
                return 2
            output_dir = args.output_dir or DEFAULT_OUTPUT_DIR
            relations = generated_relations(document)
            if args.check:
                diagnostics = check_generated_headers(relations, output_dir)
                if diagnostics:
                    print("\n".join(diagnostics), file=sys.stderr)
                    return 1
                print(
                    f"checked {len(relations)} generated relation headers in {output_dir}"
                )
                return 0
            if args.output_dir is None:
                print("--output-dir is required when generating --all", file=sys.stderr)
                return 2
            output_dir.mkdir(parents=True, exist_ok=True)
            for relation in relations:
                output = output_dir / generated_filename(relation)
                output.write_text(render(relation), encoding="utf-8")
            print(f"generated {len(relations)} relation headers -> {output_dir}")
            return 0

        if args.output_dir is not None:
            print("--output-dir is only valid with --all", file=sys.stderr)
            return 2
        if args.output is None:
            print("--output is required with --relation", file=sys.stderr)
            return 2
        relation = next(
            (item for item in document["relations"] if item["name"] == args.relation),
            None,
        )
        if relation is None:
            print(f"relation not found: {args.relation}", file=sys.stderr)
            return 2
        generated = render(relation)
        if args.check:
            if not args.output.is_file():
                print(
                    f"missing generated relation header: {args.output}",
                    file=sys.stderr,
                )
                return 1
            if args.output.read_text(encoding="utf-8") != generated:
                print(
                    f"generated relation header differs: {args.output}",
                    file=sys.stderr,
                )
                return 1
            print(f"checked {args.relation} -> {args.output}")
            return 0
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(generated, encoding="utf-8")
        print(f"generated {args.relation} -> {args.output}")
        return 0
    except (OSError, KeyError, TypeError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
