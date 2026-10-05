"""Exercise the normal local application path with real compiler inputs."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(name, None)


def run(root, *, source=None, success=True):
    arguments = [str(analyzer), "--project-root", str(root),
                 "--compile-commands", str(root / "compile_commands.json")]
    if source:
        arguments += ["--file", source]
    result = subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=180)
    if success:
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    assert result.returncode != 0 and not result.stdout, result
    return result.stderr


def database(root, files, extra=()):
    entries = [{"directory": str(root), "file": file,
                "arguments": [str(compiler), "-std=c++23", "-I.", *extra, "-c", file, "-o", file + ".o"]}
               for file in files]
    (root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")


def scans(bundle):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    queries = bundle["queries"]
    assert len(queries) == 35
    def relation(query):
        requirements = query["logical_ir"]["relation_requirements"]
        assert len(requirements) == 1
        return requirements[0]["descriptor_id"]
    assert [relation(q) for q in queries] == sorted({relation(q) for q in queries})
    publications = set()
    for query in queries:
        result = query["result"]
        assert result["snapshot_id"] == bundle["snapshot_id"]
        assert result["status"] == "complete" and not result["closed"]
        assert not result["closure_ids"]
        assert result["summary_guarantee"]["approximation"] == (
            "unknown" if result["unresolved"] or not result["rows"] else "under_approximation"), relation(query)
        publications.add(result["publication_id"])
        for row in result["rows"]:
            assert row["claim_contributors"] and row["contributor_edges"]
    assert len(publications) == 1
    return {relation(q): q["result"] for q in queries}


with tempfile.TemporaryDirectory(prefix="cxxlens-application-") as temporary:
    root = Path(temporary) / "first"
    root.mkdir()
    (root / "math.hpp").write_text("inline int twice(int value) { return value * 2; }\n", encoding="utf-8")
    source = ('#include "math.hpp"\n'
              'int leaf(int value) { return twice(value); }\n'
              'int branch(int value) { return leaf(value) + twice(value); }\n'
              'int main() { return branch(3); }\n')
    (root / "main.cpp").write_text(source, encoding="utf-8")
    (root / "other.cpp").write_text("int other() { return 0; }\n", encoding="utf-8")
    database(root, ["main.cpp", "other.cpp"])
    whole = scans(run(root))
    assert len(whole["build.compile_unit.v1"]["rows"]) == 2
    assert {row["values"]["output.qualified_name"]["value"]
            for row in whole["cc.entity.v1"]["rows"] if row["values"]["output.kind"]["value"] in {"function", "method"}} == {"twice", "leaf", "branch", "main", "other"}
    first = run(root, source="main.cpp")
    results = scans(first)
    record_inventories = results["cc.record_inventory.v1"]
    assert record_inventories["inputs_complete"] and not record_inventories["closed"]
    assert len(record_inventories["rows"]) == 1
    inventory_values = record_inventories["rows"][0]["values"]
    assert inventory_values["output.enumeration_state"]["value"] == "complete"
    assert inventory_values["output.definition_count"]["value"] == 0
    assert inventory_values["output.definitions"]["value"] == ""
    entities = results["cc.entity.v1"]["rows"]
    assert {row["values"]["output.qualified_name"]["value"] for row in entities if row["values"]["output.kind"]["value"] in {"function", "method"}} == {"twice", "leaf", "branch", "main"}
    calls = results["cc.call_site.v1"]["rows"]
    targets = results["cc.call_direct_target.v1"]["rows"]
    assert len(calls) == len(targets) == 4
    assert {r["values"]["output.call"]["value"] for r in calls} == {r["values"]["output.call"]["value"] for r in targets}
    spans = {r["values"]["output.span"]["value"]: r["values"] for r in results["source.span.v1"]["rows"]}
    for row in calls:
        span = spans[row["values"]["output.source"]["value"]]
        text = source.encode()[span["output.begin"]["value"]:span["output.end"]["value"]].decode()
        assert "(" in text and ")" in text, text
    # Project header facts retain the header's own source identity and offsets.
    assert not results["cc.call_direct_target.v1"]["unresolved"]
    assert results["cc.call_direct_target.v1"]["inputs_complete"]
    files = {row["values"]["output.file"]["value"]: row["values"]["output.logical_path"]["value"]
             for row in results["source.file.v1"]["rows"]}
    assert any(files[span["output.file"]["value"]].endswith("/math.hpp") for span in spans.values())

    relocated = Path(temporary) / "relocated"
    shutil.copytree(root, relocated)
    database(relocated, ["main.cpp", "other.cpp"])
    second = run(relocated, source="main.cpp")
    second_results = scans(second)
    # Semantic identity and public results do not depend on checkout placement.
    assert first["snapshot_id"] == second["snapshot_id"]
    for relation, result in results.items():
        assert result == second_results[relation], relation

    database(root, ["main.cpp", "main.cpp"])
    duplicate = scans(run(root, source="main.cpp"))
    assert len(duplicate["build.compile_unit.v1"]["rows"]) == 1
    assert len(duplicate["cc.call_site.v1"]["rows"]) == 4

    entries = json.loads((root / "compile_commands.json").read_text())
    entries[1]["arguments"].insert(1, "-DSECOND_VARIANT=1")
    (root / "compile_commands.json").write_text(json.dumps(entries))
    variants = run(root, source="main.cpp")
    variant_scans = scans(variants)
    assert len(variant_scans["build.compile_unit.v1"]["rows"]) == 2
    assert len({r["values"]["output.variant"]["value"] for r in variant_scans["build.variant.v1"]["rows"]}) == 2
    # An unrelated macro changes the variant, without changing the ABI or entity identity.
    assert len({r["values"]["output.toolchain"]["value"]
                for r in variant_scans["build.toolchain_context.v1"]["rows"]}) == 1
    assert {r["values"]["output.entity"]["value"] for r in variant_scans["cc.entity.v1"]["rows"]} == {
        r["values"]["output.entity"]["value"] for r in results["cc.entity.v1"]["rows"]}
    (root / "compile_commands.json").write_text(json.dumps(list(reversed(entries))))
    assert run(root, source="main.cpp") == variants

    # One broken TU does not discard independently valid project facts.
    database(root, ["main.cpp", "other.cpp"])
    (root / "other.cpp").write_text("int broken( {\n", encoding="utf-8")
    partial = scans(run(root))
    assert partial["cc.entity.v1"]["rows"]
    assert any(c["state"] == "not_covered" for c in partial["cc.entity.v1"]["input_coverage"])
    assert not partial["cc.entity.v1"]["inputs_complete"]
    (root / "other.cpp").write_text("int other() { return 0; }\n", encoding="utf-8")

    semantic = Path(temporary) / "semantic"
    semantic.mkdir()
    (semantic / "system").mkdir()
    (semantic / "system" / "types.hpp").write_text(
        '#pragma once\nnamespace sys { struct External {}; '
        'template<class T> struct Box { T value; }; }\n', encoding="utf-8")
    (semantic / "model.hpp").write_text(
        '#pragma once\n#include <types.hpp>\n#define FACTOR 2\n'
        'namespace demo {\n'
        'struct Plain { char tag; int number; unsigned flags : 3; };\n'
        'struct Empty {};\n'
        'enum class Tone { light, dark };\nstruct Other {};\n'
        'struct Coupled { Plain* first; Tone mode; Coupled* self; '
        'int Plain::*member; Plain (*callback)(Other const&); '
        'static Other* shared; Plain& convert(Other, Plain const*); '
        'sys::External* external; sys::Box<Plain> box; };\n'
        'struct StaticOnly { static Other* shared; };\n'
        'struct Surface { int state; static int shared; Surface() = default; '
        '~Surface() = default; static int utility() { return 1; } int outside(); '
        'template<class T> int convert(T value) { return int(value); } };\n'
        'inline int Surface::outside() { return state; }\n'
        'struct Access { int left, right; static int shared; '
        'Access(int v) : left(v), right(v) {} '
        'int read() { return left + right; } '
        'int no_access() { return shared; } };\n'
        'struct Base { virtual int run(int value) = 0; };\n'
        'struct Derived : Base { int state; int run(int x) override { state += x; return state * FACTOR; } };\n'
        'struct Forward;\n'
        'template<class T> struct Lazy { T value; Lazy() = default; '
        'Lazy(Lazy&&) = default; ~Lazy() = default; };\n'
        'inline constexpr auto lazy_size = sizeof(Lazy<int>);\n'
        'inline int call_leaf(int value = 0) { return value; }\n'
        'extern int call_external(int);\n'
        'struct Response { int value; Response() : value(call_leaf()) {} '
        'static int helper() { return call_leaf(1); } '
        'int repeated() { return call_leaf() + call_leaf() + helper() + call_external(2); } '
        'int recursive(int n) { return n ? recursive(n-1) : 0; } '
        'int defaults(int n = call_leaf()) { return n; } '
        'int indirect(int (*target)()) { return target(); } '
        'int nested() { auto inner = [captured = call_leaf()] { return call_external(3); }; '
        'return inner(); } };\n'
        'template<class T> struct ResponseDependent { int invoke(T value) { return value.run(); } };\n'
        'int category_value(); void category_sink(int);\n'
        'inline void category_ignored() { category_value(); (void)category_value(); '
        'int x = category_value(); category_sink(category_value()); '
        'int y = (category_value(), 1); category_value() + category_value(); '
        '(void)sizeof((category_value(), 1)); '
        'decltype((void)category_value())* unused = nullptr; }\n'
        'struct CategoryNontrivial { ~CategoryNontrivial() {} };\n'
        'inline void category_exceptions() { try { throw 1; } catch (...) { ; } '
        'try { throw CategoryNontrivial{}; } '
        'catch (CategoryNontrivial value) { category_sink(1); } '
        'try { throw CategoryNontrivial{}; } '
        'catch (CategoryNontrivial const& value) { category_sink(2); } }\n'
        'inline void category_casts(void* p, unsigned long n) { '
        'auto number = reinterpret_cast<unsigned long>(p); '
        'auto pointer = reinterpret_cast<void*>(n); '
        'asm volatile ("" ::: "memory"); auto lambda = [] { return 1; }; }\n'
        'inline void safe() noexcept {}\ninline void maybe() noexcept(false) {}\n'
        'inline int Plain::*member = &Plain::number;\ninline int extent[17];\n'
        '}\n', encoding="utf-8")
    (semantic / "main.cpp").write_text(
        '#include "model.hpp"\n'
        'int choose(bool flag, int input) { int result = 0; '
        'if (flag) result = input; else result = 7; return result; }\n'
        'int loop(int n) { int sum = 0; for (int i=0; i<n; ++i) sum += i; return sum; }\n'
        'int parameters(int& ref, int* ptr, int optional = 3) { return ref + *ptr + optional; }\n'
        '#if MODE\nint enabled() { return FACTOR; }\n#else\nint disabled() { return 0; }\n#endif\n'
        '#undef FACTOR\n', encoding="utf-8")
    database(semantic, ["main.cpp"], extra=("-DMODE=1", "-isystem", "system"))
    facts = scans(run(semantic))

    def value(row, name):
        return row["values"]["output." + name].get("value")

    token_spans = {value(row, "span"): row for row in facts["source.span.v1"]["rows"]}
    token_files = {value(row, "snapshot"): row for row in facts["source.file.v1"]["rows"]}
    tokens = facts["source.token.v1"]["rows"]
    inventories = facts["source.token_inventory.v1"]["rows"]
    assert len(inventories) == 6  # Both domains for the main source and its two headers.
    for inventory in inventories:
        phase = value(inventory, "phase")
        assert value(inventory, "profile") == "clang22.tokens.lexical-pp-template.v1"
        selected = [row for row in tokens
                    if value(row, "compile_unit") == value(inventory, "compile_unit")
                    and value(row, "phase") == phase
                    and value(token_spans[value(row, "source")], "snapshot") == value(inventory, "source_snapshot")]
        assert value(inventory, "enumeration_state") == "complete"
        assert len(selected) == value(inventory, "token_count")
        assert sorted(value(row, "ordinal") for row in selected) == list(range(len(selected)))
        assert len({value(row, "token") for row in selected}) == len(selected)
        for token in selected:
            span = token_spans[value(token, "source")]
            assert value(span, "role") == phase + "_token"
            assert 0 <= value(span, "begin") <= value(span, "end") <= value(token_files[value(span, "snapshot")], "size")
        if phase == "raw":
            starts = [row for row in selected if value(row, "directive_start")]
            assert len(starts) == value(inventory, "directive_count")
            assert all(value(row, "kind") == "hash" and value(row, "preprocessor") for row in starts)
        else:
            assert all(value(row, "active") and not value(row, "preprocessor") and not value(row, "directive_start") for row in selected)
    assert any(value(row, "phase") == "raw" and value(row, "spelling") == "disabled"
               and value(row, "active") is False and value(row, "template_context") is None for row in tokens)
    assert not any(value(row, "phase") == "expanded" and value(row, "spelling") == "disabled" for row in tokens)
    assert any(value(row, "phase") == "expanded" and value(row, "macro") for row in tokens)
    assert any(value(row, "template_context") is True for row in tokens)
    assert any(value(row, "phase") == "raw" and value(row, "template_state") == "partial" for row in inventories)

    names = {value(row, "entity"): value(row, "qualified_name") for row in facts["cc.entity.v1"]["rows"]}
    ids_by_name = {name: entity for entity, name in names.items() if name}
    edges = {(names[value(row, "source_entity")], names.get(value(row, "target_entity")), value(row, "kind"))
             for row in facts["cc.entity_edge.v1"]["rows"]}
    assert ("demo::Derived", "demo::Base", "inherits") in edges
    assert ("demo::Derived::run", "demo::Base::run", "overrides") in edges
    assert ("demo::Plain", "demo::Plain::number", "owns") in edges
    assert all(value(row, "canonical_type") for row in facts["cc.entity_detail.v1"]["rows"]
               if value(row, "entity") in {ids_by_name["demo::Plain"], ids_by_name["choose"]})
    details = {value(row, "entity"): row for row in facts["cc.entity_detail.v1"]["rows"]}
    def row_flags(row, column="flags"):
        encoded = bytes.fromhex(value(row, column))
        observed = set()
        while encoded:
            assert len(encoded) >= 4
            length = int.from_bytes(encoded[:4], "little")
            assert len(encoded) >= 4 + length
            observed.add(encoded[4:4 + length].decode("utf-8"))
            encoded = encoded[4 + length:]
        return observed
    def flags(name):
        return row_flags(details[ids_by_name[name]])
    assert {"record_definition", "abstract"} <= flags("demo::Base")
    assert "record_definition" in flags("demo::Derived") and "abstract" not in flags("demo::Derived")
    assert "record_definition" in flags("demo::Plain") and "abstractness_unknown" not in flags("demo::Plain")
    assert "abstractness_unknown" in flags("demo::Forward") and "record_definition" not in flags("demo::Forward")
    assert any("abstractness_unknown" in row_flags(row) for row in facts["cc.entity_detail.v1"]["rows"]
               if names[value(row, "entity")] == "demo::Lazy")
    surfaces = {value(row, "entity"): row for row in facts["cc.record_surface.v1"]["rows"]
                if value(row, "is_definition")}
    plain = surfaces[ids_by_name["demo::Plain"]]
    assert value(plain, "enumeration_state") == "complete"
    assert (value(plain, "method_count"), value(plain, "field_count"),
            value(plain, "base_specifier_count")) == (0, 3, 0)
    empty = surfaces[ids_by_name["demo::Empty"]]
    assert value(empty, "enumeration_state") == "complete"
    assert (value(empty, "method_count"), value(empty, "field_count")) == (0, 0)
    forward = [row for row in facts["cc.record_surface.v1"]["rows"]
               if value(row, "entity") == ids_by_name["demo::Forward"]]
    assert len(forward) == 1 and value(forward[0], "enumeration_state") == "unknown"
    assert not value(forward[0], "is_definition") and value(forward[0], "reason")
    derived = surfaces[ids_by_name["demo::Derived"]]
    assert (value(derived, "method_count"), value(derived, "field_count"),
            value(derived, "base_specifier_count")) == (1, 1, 1)
    explicit = surfaces[ids_by_name["demo::Surface"]]
    assert (value(explicit, "method_count"), value(explicit, "field_count")) == (5, 1)
    assert value(explicit, "enumeration_state") == "complete"
    record_inventory = facts["cc.record_inventory.v1"]
    assert record_inventory["inputs_complete"] and not record_inventory["closed"], record_inventory
    assert len(record_inventory["rows"]) == len(facts["build.compile_unit.v1"]["rows"])
    record_sources = {value(row, "span"): value(row, "file") for row in facts["source.span.v1"]["rows"]}
    record_files = {value(row, "file"): value(row, "logical_path") for row in facts["source.file.v1"]["rows"]}
    for inventory in record_inventory["rows"]:
        definitions = [row for row in facts["cc.record_surface.v1"]["rows"]
                       if value(row, "is_definition") and value(row, "compile_unit") == value(inventory, "compile_unit")]
        assert value(inventory, "enumeration_state") == "complete", inventory
        assert value(inventory, "definition_count") == len(definitions)
        assert row_flags(inventory, "definitions") == {value(row, "surface") for row in definitions}
        system = {value(row, "surface") for row in definitions
                  if record_files[record_sources[value(row, "source")]].endswith("/system/types.hpp")}
        assert system and row_flags(inventory, "system_definitions") == system
        assert value(forward[0], "surface") not in row_flags(inventory, "definitions")
    for row in facts["cc.record_surface.v1"]["rows"]:
        assert value(row, "type_reference_profile") == "clang22-explicit-nonsystem-nominal-type-uses/1"
        targets = row_flags(row, "type_reference_targets")
        assert value(row, "type_reference_count") == len(targets)
        assert all((names[value(row, "entity")], names.get(target), "uses_type") in edges
                   for target in targets)
    assert value(plain, "type_reference_state") == "complete" and value(plain, "type_reference_count") == 0
    assert value(empty, "type_reference_state") == "complete" and value(empty, "type_reference_count") == 0
    assert value(forward[0], "type_reference_state") == "unknown" and value(forward[0], "type_reference_reason")
    assert value(explicit, "type_reference_state") == "partial" and value(explicit, "type_reference_reason")
    assert row_flags(derived, "type_reference_targets") == {ids_by_name["demo::Base"]}
    coupled = surfaces[ids_by_name["demo::Coupled"]]
    assert value(coupled, "type_reference_state") == "complete", coupled
    assert row_flags(coupled, "type_reference_targets") == {
        ids_by_name[name] for name in ("demo::Plain", "demo::Other", "demo::Tone", "demo::Coupled")}
    static = surfaces[ids_by_name["demo::StaticOnly"]]
    assert value(static, "field_count") == 0 and value(static, "type_reference_state") == "complete"
    assert row_flags(static, "type_reference_targets") == {ids_by_name["demo::Other"]}
    accesses = {value(row, "function"): row for row in facts["cc.body.v1"]["rows"]}
    for name, count in (("demo::Access::Access", 2), ("demo::Access::read", 2),
                        ("demo::Access::no_access", 0), ("demo::Surface::outside", 1)):
        body = accesses[ids_by_name[name]]
        assert value(body, "member_access_count") == count, body
        assert value(body, "member_access_state") == "complete"
        assert value(body, "member_access_profile") == "clang22-explicit-member-access/1"
        assert value(body, "member_access_targets") is not None
    call_sites = {value(row, "call"): row for row in facts["cc.call_site.v1"]["rows"]}
    call_targets = {value(row, "call"): value(row, "target")
                    for row in facts["cc.call_direct_target.v1"]["rows"]}
    for name, expected_count, expected_targets in (
            ("demo::Response::Response", 1, {"demo::call_leaf"}),
            ("demo::Response::helper", 1, {"demo::call_leaf"}),
            ("demo::Response::repeated", 4, {"demo::call_leaf", "demo::Response::helper", "demo::call_external"}),
            ("demo::Response::recursive", 1, {"demo::Response::recursive"}),
            ("demo::Response::defaults", 0, set()),
            ("demo::Response::indirect", 0, set()),
            ("demo::Access::no_access", 0, set())):
        body = accesses[ids_by_name[name]]
        sites = row_flags(body, "direct_call_sites")
        targets = row_flags(body, "direct_call_targets")
        assert value(body, "direct_call_profile") == "clang22-written-syntactic-direct-calls/1"
        assert value(body, "direct_call_state") == "complete", body
        assert value(body, "direct_call_count") == len(sites) == expected_count, body
        assert {names[target] for target in targets} == expected_targets, body
        assert {call_targets[site] for site in sites} == targets
        assert all(value(call_sites[site], "caller") == ids_by_name[name] and
                   value(call_sites[site], "compile_unit") == value(body, "compile_unit") for site in sites)
    nested = accesses[ids_by_name["demo::Response::nested"]]
    nested_targets = row_flags(nested, "direct_call_targets")
    assert value(nested, "direct_call_state") == "complete", nested
    assert value(nested, "direct_call_count") == 2
    assert ids_by_name["demo::call_leaf"] in nested_targets
    assert ids_by_name["demo::call_external"] not in nested_targets
    lambda_calls = [row for row in facts["cc.call_site.v1"]["rows"]
                    if call_targets.get(value(row, "call")) == ids_by_name["demo::call_external"] and
                    value(row, "caller") != ids_by_name["demo::Response::repeated"]]
    assert len(lambda_calls) == 1 and value(lambda_calls[0], "caller") != ids_by_name["demo::Response::nested"]
    lambda_body = accesses[value(lambda_calls[0], "caller")]
    assert value(lambda_body, "direct_call_state") == "complete", lambda_body
    assert row_flags(lambda_body, "direct_call_targets") == {ids_by_name["demo::call_external"]}
    dependent_body = accesses[ids_by_name["demo::ResponseDependent::invoke"]]
    assert value(dependent_body, "direct_call_state") == "partial", dependent_body
    def function_syntax(name):
        return [row for row in facts["cc.syntax_node.v1"]["rows"]
                if value(row, "function") == ids_by_name[name]]
    ignored_nodes = function_syntax("demo::category_ignored")
    assert ignored_nodes and all("finite_categories_v1" in row_flags(row, "flags") for row in ignored_nodes)
    ignored_calls = [row for row in ignored_nodes if "discarded_nonvoid_result" in row_flags(row, "flags")]
    assert len(ignored_calls) == 3, ignored_calls
    assert all(value(row, "kind") == "CallExpr" for row in ignored_calls)
    assert not any("discarded_result_unknown" in row_flags(row, "flags") for row in ignored_nodes)
    exception_nodes = function_syntax("demo::category_exceptions")
    handlers = [row for row in exception_nodes if value(row, "kind") == "CXXCatchStmt"]
    assert len(handlers) == 3
    assert sum("catch_all" in row_flags(row, "flags") for row in handlers) == 1
    assert sum("catch_empty" in row_flags(row, "flags") for row in handlers) == 1
    assert sum("catch_nontrivial_by_value" in row_flags(row, "flags") for row in handlers) == 1
    cast_nodes = function_syntax("demo::category_casts")
    assert sum("cast_pointer_to_integer" in row_flags(row, "flags") for row in cast_nodes) == 1
    assert sum("cast_integer_to_pointer" in row_flags(row, "flags") for row in cast_nodes) == 1
    assert sum(value(row, "kind") == "GCCAsmStmt" for row in cast_nodes) == 1
    assert sum(value(row, "kind") == "LambdaExpr" for row in cast_nodes) == 1
    assert "noexcept" in row_flags(details[ids_by_name["demo::safe"]], "flags")
    assert "noexcept" not in row_flags(details[ids_by_name["demo::maybe"]], "flags")
    # Unused defaulted members have lazy exception specifications. Observation cannot
    # force instantiation or call Clang's evaluated-only canThrow()/isNothrow() APIs.
    lazy = [row for entity, row in details.items() if (names[entity] or "").startswith("demo::Lazy")]
    assert lazy and any("noexcept_unknown" in row_flags(row, "flags") for row in lazy)
    types = {value(row, "type"): row for row in facts["cc.type.v1"]["rows"]}
    assert value(types[value(details[ids_by_name["demo::member"]], "canonical_type")], "constructor") == "member_pointer"
    for row in types.values():
        assert value(row, "structure_profile") == "clang22-structural-type/1"
        assert value(row, "structure_preimage").startswith("clang22-structural-type-v1\n")
        assert value(row, "structure_state") in ("complete", "partial")
    extent = value(details[ids_by_name["demo::extent"]], "canonical_type")
    component = next(row for row in facts["cc.type_component.v1"]["rows"]
                     if value(row, "owner_type") == extent and value(row, "role") == "extent")
    assert value(component, "value_preimage") == "17" and value(component, "value_digest")
    assert any(value(row, "structure_state") == "partial" for row in types.values())
    layout = [row for row in facts["cc.layout_fact.v1"]["rows"] if value(row, "entity") == ids_by_name["demo::Plain"]]
    object_layout = next(row for row in layout if value(row, "kind") == "object")
    assert (value(object_layout, "byte_size"), value(object_layout, "byte_alignment")) == (12, 4)
    field_layout = {names[value(row, "member")]: row for row in layout if value(row, "kind") == "field"}
    assert value(field_layout["demo::Plain::tag"], "bit_offset") == 0
    assert value(field_layout["demo::Plain::number"], "bit_offset") == 32
    assert (value(field_layout["demo::Plain::flags"], "bit_offset"), value(field_layout["demo::Plain::flags"], "bit_width")) == (64, 3)
    for function in ("choose", "loop"):
        function_id = ids_by_name[function]
        body = next(row for row in facts["cc.body.v1"]["rows"] if value(row, "function") == function_id)
        assert value(body, "eligibility") == "closed"
        nodes = [row for row in facts["cc.cfg_node.v1"]["rows"] if value(row, "function") == function_id]
        cfg_edges = [row for row in facts["cc.cfg_edge.v1"]["rows"] if value(row, "function") == function_id]
        assert len(cfg_edges) - len(nodes) + 2 == 2  # One decision in each independent source body.
        assert {value(row, "kind") for row in cfg_edges} >= {"true", "false"}
        syntax = [row for row in facts["cc.syntax_node.v1"]["rows"]
                  if value(row, "function") == function_id]
        assert value(body, "ast_state") == "complete", body
        assert value(body, "ast_node_count") == len(syntax)
        assert value(body, "local_variable_count") == (1 if function == "choose" else 2)
        roots = [row for row in syntax if value(row, "parent") is None]
        assert len(roots) == 1 and b"role_body" in bytes.fromhex(value(roots[0], "flags"))
        node_ids = {value(row, "node") for row in syntax}
        assert all(value(row, "parent") in node_ids for row in syntax if value(row, "parent"))
        assert any(b"role_condition" in bytes.fromhex(value(row, "flags")) for row in syntax)
    parameter_flags = {
        names[value(row, "entity")]: bytes.fromhex(value(row, "flags"))
        for row in facts["cc.entity_detail.v1"]["rows"]
        if names.get(value(row, "entity")) in {"ref", "ptr", "optional"}
    }
    assert b"mutable_lvalue_reference" in parameter_flags["ref"]
    assert b"pointer" in parameter_flags["ptr"]
    assert b"default_argument" in parameter_flags["optional"]
    parameter_body = next(row for row in facts["cc.body.v1"]["rows"]
                          if value(row, "function") == ids_by_name["parameters"])
    parameter_syntax = [row for row in facts["cc.syntax_node.v1"]["rows"]
                        if value(row, "function") == ids_by_name["parameters"]]
    # Default argument expressions belong to the declaration, outside this body.
    body_nodes = [row for row in parameter_syntax
                  if value(row, "kind") != "IntegerLiteral"]
    assert value(parameter_body, "ast_node_count") == len(body_nodes)
    flow = facts["cc.flow_fact.v1"]["rows"]
    definitions = {value(row, "fact"): row for row in flow if value(row, "kind") == "definition"}
    reaching = [json.loads(value(row, "value")) for row in flow
                if value(row, "kind") == "def_use" and value(row, "function") == ids_by_name["choose"]
                and value(row, "subject") == ids_by_name["result"]]
    assert len(reaching) == 1 and len(reaching[0]) == 2
    assert all(value(definitions[definition], "subject") == ids_by_name["result"] for definition in reaching[0])
    assert {value(row, "kind") for row in flow} >= {"definition", "use", "def_use", "live_in", "live_out", "reaching_in", "reaching_out"}
    frontiers = [row for row in flow if value(row, "kind") == "frontier"]
    assert any(value(row, "function") == ids_by_name["parameters"]
               and value(row, "value") == "indirect-memory" for row in frontiers)
    assert all(value(row, "guarantee") == "unknown" and value(row, "node") for row in frontiers)
    pp = facts["source.preprocessor_event.v1"]["rows"]
    assert any(value(row, "kind") == "if" and value(row, "state") == "true" for row in pp)
    assert any(value(row, "kind") == "skipped_range" and value(row, "state") == "skipped" for row in pp)
    assert any(value(row, "kind") == "macro_definition" and value(row, "name") == "FACTOR" for row in pp)
    assert any(value(row, "kind") == "macro_expansion" and value(row, "name") == "FACTOR" for row in pp)
    assert any(value(row, "kind") == "macro_undefinition" and value(row, "name") == "FACTOR" for row in pp)
    assert "enabled" in ids_by_name and "disabled" not in ids_by_name
    includes = facts["source.include.v1"]["rows"]
    assert len(includes) == 2 and all(value(row, "resolution") == "resolved" for row in includes)
    assert {value(row, "is_system") for row in includes} == {False, True}

    database(root, ["main.cpp"], extra=("@extra.rsp",))
    assert "unsupported-option" in run(root, success=False)
    database(root, ["main.cpp"], extra=("--target=aarch64-linux-gnu",))
    assert "Linux x86_64 required" in run(root, success=False)
    database(root, ["main.cpp"])
    (root / "main.cpp").write_text("int broken( {\n", encoding="utf-8")
    assert run(root, success=False)
    (root / "compile_commands.json").write_text("{", encoding="utf-8")
    assert run(root, success=False)

assert subprocess.run([str(analyzer), "--help"], capture_output=True).returncode == 0
