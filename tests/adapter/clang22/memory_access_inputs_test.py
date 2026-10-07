"""Original builtin access census, eligibility and independent object storage."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
fixture = Path(__file__).with_name("fixtures") / "memory_access_inputs.cpp"
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)


def members(encoded):
    data = bytes.fromhex(encoded)
    values = []
    while data:
        assert len(data) >= 4
        width = int.from_bytes(data[:4], "little")
        assert 0 < width <= len(data) - 4
        values.append(data[4:4 + width].decode())
        data = data[4 + width:]
    assert values == sorted(set(values))
    return set(values)


def verify(bundle):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    scans = {query["relation_id"]: query["result"] for query in bundle["queries"]}
    assert all(scan["status"] == "complete" for scan in scans.values())
    rows = {name: [{key.removeprefix("output."): cell.get("value")
                   for key, cell in row["values"].items()} for row in scan["rows"]]
            for name, scan in scans.items()}
    outcomes = rows["build.compile_unit_analysis.v1"]
    assert outcomes and all(row["semantic_output"] == "produced" and
                            row["parse_error_count"] == row["fatal_error_count"] == 0
                            for row in outcomes)
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    nodes = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    types = {row["type"]: row for row in rows["cc.type.v1"]}
    bodies = {entities[row["function"]]["qualified_name"]: row
              for row in rows["cc.body.v1"]}

    def observed(name):
        body = bodies[name]
        assert body["memory_access_profile"] == "clang22-original-memory-access-occurrences/1"
        retained = members(body["memory_access_ids"])
        assert body["memory_access_count"] >= len(retained)
        scope = body["function_exit_declaration"]
        declaration = declarations[scope]
        detail, = [row for row in rows["cc.entity_detail.v1"]
                   if row["entity"] == declaration["entity"] and
                   row["source"] == declaration["source"]]
        for axis in ("profile", "state", "count", "ids", "reason"):
            assert detail.get("memory_access_" + axis) == body.get("memory_access_" + axis)
        selected = [nodes[node] for node in retained]
        assert all(row["memory_scope_declaration"] == scope and
                   row["function"] == body["function"] and
                   row["memory_access_profile"] == "clang22-original-memory-access/1"
                   for row in selected)
        if body["memory_access_state"] == "complete":
            assert body["memory_access_count"] == len(retained)
            assert all(row["memory_access_state"] == "complete" for row in selected)
        else:
            assert body["memory_access_reason"]
        return body, selected

    for name, count, kind in (("array_indices", 5, "array_element"),
                              ("array_addresses", 2, "array_address"),
                              ("pointer_dereference", 1, "pointer_dereference"),
                              ("pointer_subscript", 1, "array_element"),
                              ("arrow_access", 1, "pointer_member"),
                              ("cancelled_dereference", 1, "cancelled_dereference_address")):
        body, selected = observed(name)
        assert body["memory_access_state"] == "complete" and len(selected) == count
        assert all(row["memory_access_kind"] == kind and
                   row["memory_evaluation"] == "potentially_evaluated" and
                   row["memory_pointer_expression"] in nodes for row in selected)
        for row in selected:
            if row["kind"] == "ArraySubscriptExpr":
                assert row["memory_pointer_expression"] == row["base_expression"]
                assert row["index_expression"] in nodes
            elif row["kind"] == "UnaryOperator":
                assert row["opcode"] == "*" and row["memory_pointer_expression"] == row["operand"]
            else:
                assert row["kind"] == "MemberExpr" and kind == "pointer_member"
            if kind in ("array_address", "cancelled_dereference_address"):
                address = nodes[row["memory_address_expression"]]
                assert address["kind"] == "UnaryOperator" and address["opcode"] == "&"
                operand = nodes[address["operand"]]
                while operand["kind"] == "ParenExpr":
                    assert operand["child_state"] == "complete" and operand["child_count"] == 1
                    operand = nodes[next(iter(members(operand["child_ids"])))]
                assert operand["node"] == row["node"]
            else:
                assert row.get("memory_address_expression") is None
        if name.startswith("array_"):
            assert all(row["memory_region_expression"] in nodes for row in selected)
            for row in selected:
                region = nodes[row["memory_region_expression"]]
                region_type = types[region["canonical_type"]]
                assert region_type["constructor"] == "array" and region_type["structure_state"] == "complete"
                extent, = [component for component in rows["cc.type_component.v1"]
                           if component["owner_type"] == region_type["type"] and component["role"] == "extent"]
                assert extent["value_preimage"] == "3" and extent["value_digest"]
        else:
            assert all(row.get("memory_region_expression") is None for row in selected)
    body, selected = observed("nested_address")
    assert body["memory_access_state"] == "complete" and len(selected) == 2
    assert {row["memory_access_kind"] for row in selected} == {"array_element", "array_address"}
    inner, = [row for row in selected if row["memory_access_kind"] == "array_element"]
    outer, = [row for row in selected if row["memory_access_kind"] == "array_address"]
    assert inner.get("memory_address_expression") is None
    assert outer["memory_region_expression"] == inner["node"]
    for name in ("sizeof_access", "decltype_access"):
        _, selected = observed(name)
        assert selected and all(row["memory_evaluation"] == "unevaluated" for row in selected)
    for name in ("overloaded_control", "healthy_sibling"):
        body, selected = observed(name)
        assert body["memory_access_state"] == "complete" and body["memory_access_count"] == 0 and not selected
    for name in ("dependent_access", "default_activation", "default_field::default_field"):
        body, _ = observed(name)
        assert body["memory_access_state"] == "partial"
    activated = bodies["default_activation"]
    assert activated["memory_access_count"] == 1 and not members(activated["memory_access_ids"])
    assert activated["memory_access_reason"] == "original-default-activation-unbound"
    nested, selected = observed("nested_default_activations")
    assert nested["memory_access_state"] == "partial" and nested["memory_access_count"] == 2
    assert not selected
    absent, = [row for row in rows["cc.entity_detail.v1"]
               if entities[row["entity"]]["qualified_name"] == "absent_body"]
    assert absent["memory_access_state"] == "complete" and absent["memory_access_count"] == 0

    storage_function = bodies["storage_units"]["function"]
    traits = [row for row in nodes.values() if row.get("function") == storage_function and
              row.get("type_trait_kind") == "sizeof"]
    assert len(traits) == 3
    for trait in traits:
        scalar = types[trait["type_trait_argument_type"]]
        assert trait["type_trait_state"] == "complete"
        assert scalar["integer_storage_profile"] == "clang22-original-integer-object-storage/1"
        assert scalar["integer_storage_state"] == "complete"
        assert scalar["integer_object_bytes"] == int(trait["type_trait_result_decimal"])
    bitint, = [types[trait["type_trait_argument_type"]] for trait in traits
               if types[trait["type_trait_argument_type"]]["constructor"] == "bitint"]
    assert bitint["integer_bit_width"] == 9
    abi = [row for row in rows["cc.abi_surface.v1"] if row["entity"] == storage_function]
    assert abi and all(row["target_data_model_profile"] == "clang22-original-target-data-model/1" and
                       row["target_data_model_state"] == "complete" and row["char_width_bits"] > 0
                       for row in abi)
    print("Original memory access and integer storage controls passed")


if len(sys.argv) == 4:
    verify(json.loads(Path(sys.argv[3]).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-memory-access-") as directory:
        root = Path(directory)
        shutil.copyfile(fixture, root / "main.cpp")
        commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                     [str(compiler), "-std=c++23", "-nostdinc", "-nostdinc++", "-O0", "-c",
                      "main.cpp", "-o", "main.o"]}]
        (root / "compile_commands.json").write_text(json.dumps(commands))
        arguments = [str(analyzer), "--project-root", str(root), "--compile-commands",
                     str(root / "compile_commands.json")]
        run = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert run.returncode == 0, run.stderr
        verify(json.loads(run.stdout))
        bounded = subprocess.run(arguments + ["--maximum-output-bytes", "1"],
                                 env=environment, text=True, capture_output=True, timeout=180)
        assert bounded.returncode == 1 and bounded.stdout == ""
        assert bounded.stderr.startswith("application-analysis.query-export-invalid: output: byte-limit")
