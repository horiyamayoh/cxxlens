"""Retain actual compiler routes, physical definitions and resource operands."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
fixture = Path(__file__).with_name("fixtures") / "route_resource_inputs.cpp"
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)


def members(encoded):
    data = bytes.fromhex(encoded)
    result = []
    while data:
        assert len(data) >= 4
        size = int.from_bytes(data[:4], "little")
        assert size and size <= len(data) - 4
        result.append(data[4:4 + size].decode())
        data = data[4 + size:]
    assert result == sorted(set(result))
    return set(result)


def verify(bundle):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]:
             query["result"] for query in bundle["queries"]}
    assert all(scan["status"] == "complete" for scan in scans.values())
    rows = {relation: [dict({key.removeprefix("output."): cell.get("value")
                            for key, cell in row["values"].items()},
                           _cells=row["values"]) for row in scan["rows"]]
            for relation, scan in scans.items()}
    names = {row["entity"]: row["qualified_name"] for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    nodes = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    types = {row["type"]: row for row in rows["cc.type.v1"]}
    bodies = {row["body"]: row for row in rows["cc.body.v1"]}
    unit, = rows["build.compile_unit.v1"]
    assert all(row["semantic_output"] == "produced"
               for row in rows["build.compile_unit_analysis.v1"])
    assert all(row["parse_error_count"] == 0 and row["fatal_error_count"] == 0
               for row in rows["build.compile_unit_analysis.v1"])

    def function_rows(relation, function):
        return [row for row in rows[relation] if names.get(row.get("function")) == function]

    topology = rows["cc.exceptional_block.v1"]
    successors = rows["cc.exceptional_successor.v1"]
    exits = rows["cc.exceptional_exit.v1"]
    exit_index = {row["exit"]: row for row in exits}
    blocks = {row["block"]: row for row in topology}
    edge_index = {row["successor"]: row for row in successors}
    assert blocks and successors
    for carrier in [row for row in exits if row["role"] == "lowering_variant"]:
        assert carrier["ordinal"] == 0
        if carrier["lowered_topology_state"] != "complete":
            continue
        assert carrier["lowered_topology_profile"] == "clang22-original-lowering-topology/1"
        selected_blocks = [row for row in topology if row["variant"] == carrier["exit"]]
        selected_edges = [row for row in successors if row["variant"] == carrier["exit"]]
        assert len(selected_blocks) == carrier["lowered_block_count"]
        assert {row["block"] for row in selected_blocks} == members(carrier["lowered_block_ids"])
        assert len(selected_edges) == carrier["lowered_successor_count"]
        assert {row["successor"] for row in selected_edges} == members(carrier["lowered_successor_ids"])
        entry, = [row for row in selected_blocks if row["is_entry"]]
        assert carrier["lowered_entry"] == entry["block"]
        assert len({row["ordinal"] for row in selected_blocks}) == len(selected_blocks)
        assert len({(row["from_block"], row["terminator_instruction_ordinal"], row["ordinal"])
                    for row in selected_edges}) == len(selected_edges)
        for edge in selected_edges:
            source = blocks[edge["from_block"]]
            assert edge["to_block"] in blocks
            assert source["instruction_count"] == edge["terminator_instruction_ordinal"] + 1
            assert blocks[edge["to_block"]]["variant"] == carrier["exit"]
            if edge.get("invoke"):
                original = exit_index[edge["invoke"]]
                assert original["is_invoke"] and original["lowered_block"] == edge["from_block"]
                assert original["instruction_ordinal"] == edge["terminator_instruction_ordinal"]
                assert (edge["ordinal"], edge["kind"]) in ((0, "normal"), (1, "unwind"))
                assert original["normal_successor" if edge["ordinal"] == 0 else "unwind_successor"] == edge["successor"]
    direct = [row for row in function_rows("cc.exceptional_exit.v1", "direct_boundary")
              if row.get("eh_disposition") == "direct_function_spec_termination"]
    assert direct
    for row in direct:
        assert row["is_invoke"] and row["eh_selected_scope_kind"] == "terminate"
        assert row["eh_boundary_profile"] == "clang22-original-invoke-eh-boundary/1"
        assert declarations[row["eh_boundary_declaration"]]["entity"] == row["function"]
        assert row["normal_successor"] in edge_index and row["unwind_successor"] in edge_index
    for name in ("caught_boundary", "caught_then_termination"):
        observed = function_rows("cc.exceptional_exit.v1", name)
        written_calls = [row for row in observed if names.get(row.get("target")) == "risky"]
        assert written_calls and all(row.get("eh_disposition") == "catch_dispatch"
                                     for row in written_calls)
    assert any(row.get("eh_disposition") == "cleanup_dispatch"
               for row in function_rows("cc.exceptional_exit.v1", "cleanup_boundary"))

    for name, kind, form, result in (("trait_type", "sizeof", "type", "1"),
                                    ("trait_expression", "sizeof", "expression", "4"),
                                    ("trait_alignment", "alignof", "type", "4")):
        original, = [row for row in function_rows("cc.syntax_node.v1", name)
                     if row["kind"] == "UnaryExprOrTypeTraitExpr"]
        assert original["type_trait_kind"] == kind and original["type_trait_argument_form"] == form
        assert original["type_trait_state"] == "complete" and original["type_trait_result_decimal"] == result
        assert original["type_trait_profile"] == "clang22-original-type-trait-expression/1"
        assert original["type_trait_argument_type"] in types
        assert (original.get("type_trait_argument_expression") in nodes) == (form == "expression")
    dependent = function_rows("cc.syntax_node.v1", "trait_dependent")
    assert dependent and any(row.get("type_trait_state") != "complete" and
                             row.get("type_trait_result_decimal") is None for row in dependent)
    atomic = [row for row in nodes.values() if row["kind"] == "AtomicExpr"]
    assert len(atomic) == 4
    for row in atomic:
        assert row["atomic_profile"] == "clang22-original-atomic-expression/1"
        assert row["atomic_pointer_expression"] in nodes and row["atomic_operation"] is not None
    exchange, = [row for row in atomic if names[row["function"]] == "atomic_exchange"]
    assert exchange["atomic_success_order_value"] == 5 and exchange["atomic_failure_order_value"] == 2
    assert exchange["atomic_success_order_expression"] in nodes and exchange["atomic_failure_order_expression"] in nodes
    initialization, = [row for row in atomic if names[row["function"]] == "atomic_initialize"]
    assert initialization["atomic_success_order_state"] == "not_applicable"
    assert initialization.get("atomic_success_order_expression") is None
    unknown, = [row for row in atomic if names[row["function"]] == "atomic_unknown_order"]
    assert unknown["atomic_success_order_expression"] in nodes and unknown.get("atomic_success_order_value") is None
    assert all(row.get("atomic_profile") is None for row in function_rows("cc.syntax_node.v1", "ordinary_builtin"))

    inventory, = rows["cc.declaration_inventory.v1"]
    sequence, = rows["cc.sequence_pair.v1"]
    assert sequence["profile"] == "clang22-original-sequence-checker-candidates/1"
    assert sequence["binding_state"] == sequence["membership_state"] == "complete"
    assert names[sequence["owner_entity"]] == "original_sequence_probe"
    assert sequence["checker_relation"] == "unsequenced"
    assert sequence["left_modifies"] and sequence["right_modifies"]
    assert sequence["scalar_nonreference_storage"] and sequence["jointly_potentially_evaluated"]
    assert declarations[sequence["storage_declaration"]]["entity"] == sequence["storage_entity"]
    assert all(sequence[field] in nodes for field in
               ("expression", "left_expression", "right_expression"))
    assert inventory["sequence_pair_state"] == "complete"
    assert members(inventory["sequence_pair_ids"]) == {sequence["pair"]}
    assert inventory["sequence_pair_count"] == 1
    roots = {row["root"]: row for row in rows["cc.constant_evaluation_root.v1"]}
    live_call, = [row for row in rows["cc.constant_evaluated_call.v1"]
                  if "c:@F@original_live_union#" in
                  {bytes.fromhex(usr).decode() for usr in members(row["callee_usr_hexes"])}]
    assert live_call["membership_state"] == "complete"
    live_root, = members(live_call["root_ids"])
    assert roots[live_root]["completion"] == "complete"
    assert roots[live_root]["requested_constant_context"] and not roots[live_root]["fold_failure"]
    assert not rows["cc.object_state_observation.v1"]
    # Other genuinely failed qualifying roots keep the combined object census
    # partial; the successful union control never supplies a violation witness.
    assert any(root["requested_constant_context"] and root["fold_failure"]
               for root in roots.values())
    assert inventory["object_state_state"] == "partial"
    assert inventory["physical_definition_profile"] == "clang22-original-physical-definitions/1"
    physical = members(inventory["physical_definition_ids"])
    assert inventory["physical_definition_count"] >= len(physical)
    coroutine_bodies = [row for row in bodies.values() if row.get("is_coroutine")]
    assert len(coroutine_bodies) == 2
    assert any(names[row["function"]] == "named_coroutine" for row in coroutine_bodies)
    assert any(declarations[row["coroutine_definition"]]["kind"] == "CXXMethod" for row in coroutine_bodies)
    for body in coroutine_bodies:
        assert body["coroutine_binding_state"] == "complete"
        assert body["coroutine_definition"] in physical
        assert declarations[body["coroutine_definition"]]["entity"] == body["function"]
        assert nodes[body["coroutine_body"]]["kind"] == "CoroutineBodyStmt"
    elements = rows["cc.cfg_element.v1"]
    cfg_nodes = {row["node"]: row for row in rows["cc.cfg_node.v1"]}
    for element in elements:
        assert element["body"] in bodies and element["node"] in cfg_nodes
        assert element["function"] == bodies[element["body"]]["function"]
        assert element["index"] < cfg_nodes[element["node"]]["element_count"]
    lifetime = [row for row in rows["cc.flow_fact.v1"] if row["kind"] == "lifetime_end"]
    assert lifetime and all(row.get("declaration") in declarations for row in lifetime)

    moves = rows["cc.move_event.v1"]
    assert {row["kind"] for row in moves} >= {"move_constructor", "copy_constructor", "move_assignment", "copy_assignment", "unresolved_constructor"}
    assert all(row["syntax"] in nodes and row["compile_unit"] == unit["compile_unit"] for row in moves)
    copy_owner = {row["declaration"] for row in declarations.values()
                  if names.get(row["entity"]) == "selected_copy_despite_xvalue"}
    copy_selections = [row for row in moves if row.get("owner_declaration") in copy_owner]
    assert copy_owner and copy_selections
    assert all(row["kind"] == "copy_constructor" for row in copy_selections)
    concrete = [row for row in moves if row["kind"] in
                ("move_constructor", "copy_constructor", "move_assignment", "copy_assignment")]
    assert concrete and all(row["source_actual"] in nodes and
                            row["source_formal"] in declarations and
                            row["selected_declaration"] in declarations for row in concrete)
    attrs = rows["cc.declaration_attribute.v1"]
    carriers = [row for row in attrs if row["record_kind"] == "carrier"]
    assert {row.get("ownership_kind") for row in carriers} >= {"returns", "takes", "holds"}
    annotation, = [row for row in carriers if row.get("annotation_payload")]
    assert bytes.fromhex(annotation["annotation_payload"]) == b"arbitrary annotation"
    for carrier in carriers:
        assert carrier["argument_slot"] == 0 and carrier["declaration"] in declarations
        if carrier["argument_state"] == "complete":
            arguments = [row for row in attrs if row.get("carrier_attribute") == carrier["attribute"]]
            assert len(arguments) == carrier["argument_count"]
            assert members(carrier["argument_ids"]) == {row["attribute"] for row in arguments}
    ownership_slots = {"acquire": [], "acquire_sized": [(1, 0)],
                       "consume": [(1, 0), (2, 1)], "hold": [(1, 0)],
                       "ownership_methods::consume": [(2, 0), (3, 1)]}
    for name, expected in ownership_slots.items():
        carrier, = [row for row in carriers
                    if names[declarations[row["declaration"]]["entity"]] == name]
        assert bytes.fromhex(carrier["ownership_module"]) == b"heap"
        assert carrier["argument_state"] == "complete"
        arguments = sorted([row for row in attrs
                            if row.get("carrier_attribute") == carrier["attribute"]],
                           key=lambda row: row["argument_ordinal"])
        assert [(row["source_parameter_index"], row["ast_parameter_index"])
                for row in arguments] == expected
        assert [row["argument_slot"] for row in arguments] == list(range(1, len(expected) + 1))
        assert all(row["formal_declaration"] in declarations and
                   row["is_implicit_this"] is False for row in arguments)
    ordinary, = [row for row in carriers
                 if names[declarations[row["declaration"]]["entity"]] == "ordinary"]
    assert ordinary["argument_state"] == "unsupported"
    assert ordinary["_cells"]["output.argument_count"]["state"] == "absent"
    assert ordinary["_cells"]["output.argument_ids"]["state"] == "absent"
    empty, = [row for row in declarations.values()
              if names[row["entity"]] == "no_attributes"]
    assert empty["attribute_inventory_state"] == "complete" and empty["attribute_count"] == 0
    assert members(empty["attribute_ids"]) == set()
    addresses = rows["cc.address_transfer.v1"]
    assert {row["kind"] for row in addresses} >= {"address_of", "array_decay", "function_decay", "return", "capture", "reference_bind", "initializer", "assignment", "member_initializer", "this_address"}
    assert all(row["compile_unit"] == unit["compile_unit"] for row in addresses)
    copied, = [row for row in addresses if row["kind"] == "initializer" and
               names.get(declarations.get(row.get("destination_declaration"), {}).get("entity")) == "plain_pointer_copy"]
    assert copied["origin_state"] == "not_classified"
    assert copied["_cells"]["output.origin_expression"]["state"] == "absent"
    assert copied["_cells"]["output.origin_declaration"]["state"] == "absent"
    assert names[declarations[copied["referenced_declaration"]]["entity"]] == "direct_address"
    captures = sorted([row for row in addresses if row["kind"] == "capture"],
                      key=lambda row: row["endpoint_ordinal"])
    assert [row["endpoint_ordinal"] for row in captures] == [0, 1, 2]
    assert [row["capture_kind"] for row in captures] == ["by_ref", "by_copy", "by_ref"]
    print(f"Original124: {len(blocks)} blocks, {len(successors)} successors, {len(moves)} selected members, {len(attrs)} attributes, {len(addresses)} address endpoints")


def run_original_fixture():
    with tempfile.TemporaryDirectory(prefix="cxxlens-route-resource-") as directory:
        root = Path(directory)
        shutil.copyfile(fixture, root / "main.cpp")
        commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                     [str(compiler), "-std=c++23", "-nostdinc", "-nostdinc++", "-fexceptions",
                      "-fcxx-exceptions", "-O0", "-c", "main.cpp", "-o", "main.o"]}]
        (root / "compile_commands.json").write_text(json.dumps(commands))
        run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                              str(root / "compile_commands.json")], env=environment,
                             text=True, capture_output=True, timeout=180)
        if debug_root := os.environ.get("CXXLENS_INPUT_DEBUG_DIR"):
            debug = Path(debug_root) / "route-resource"
            debug.mkdir(parents=True, exist_ok=True)
            (debug / "queries.json").write_text(run.stdout)
            (debug / "analyzer.stderr.log").write_text(run.stderr)
            shutil.copyfile(root / "main.cpp", debug / "main.cpp")
            shutil.copyfile(root / "compile_commands.json", debug / "compile_commands.json")
        assert run.returncode == 0, run.stderr
        verify(json.loads(run.stdout))


if len(sys.argv) == 4:
    verify(json.loads(Path(sys.argv[3]).read_text()))
else:
    run_original_fixture()
