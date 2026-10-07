"""Original AtomicExpr membership and independent original Sema order observations."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
stock = "--stock-template-events" in sys.argv[3:]
fixture = Path(__file__).with_name("fixtures") / "atomic_sema_inputs.cpp"
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
    assert rows["build.compile_unit_analysis.v1"] and all(
        row["semantic_output"] == "produced" and
        row["parse_error_count"] == row["fatal_error_count"] == 0
        for row in rows["build.compile_unit_analysis.v1"])
    features = {row["feature"]: row for row in rows["cc.source_feature.v1"]}
    population, = [row for row in rows["cc.source_feature_inventory.v1"]
                   if row["scope"] == "translation_unit"]
    assert population["profile"] == "clang22-original-static-source-features/1"
    assert all(population[axis] == "complete" for axis in
               ("enumeration_state", "traversal_state", "entry_state"))
    assert population["feature_count"] == len(features)
    assert members(population["feature_ids"]) == set(features)
    candidates = [row for row in features.values() if row["kind"] == "AtomicExpr"]
    assert candidates
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    bodies = {row["body"]: row for row in rows["cc.body.v1"]}
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    atomics = [row for row in syntax.values() if row["kind"] == "AtomicExpr"]
    by_function = {}
    for node in atomics:
        assert node["atomic_profile"] == "clang22-original-atomic-expression/1"
        assert node["atomic_pointer_expression"] in syntax
        assert node["atomic_sema_profile"] == "clang22-original-atomic-sema/1"
        bound = [feature for feature in candidates if feature.get("syntax") == node["node"]]
        assert bound and all(feature["compile_unit"] == node["compile_unit"] for feature in bound)
        if stock:
            assert node["atomic_sema_state"] == "unavailable"
            assert node.get("atomic_operation_kind") is None
            assert node.get("atomic_evaluation_context") is None
        else:
            assert node["atomic_sema_state"] in ("complete", "partial", "unavailable"), node
            if node["atomic_sema_state"] == "complete":
                assert node["atomic_evaluation_context"] != "unknown"
            else:
                assert node["atomic_sema_reason"], node
        assert node["syntax_scope_profile"] == "clang22-original-syntax-body-scope/1"
        if node["syntax_scope_state"] == "complete":
            body = bodies[node["syntax_scope_body"]]
            assert body["function_exit_declaration"] == node["syntax_scope_declaration"]
            physical = declarations[body["function_exit_declaration"]]
            assert physical["entity"] == node["function"] == body["function"]
            assert body["compile_unit"] == node["compile_unit"]
            by_function.setdefault(entities[physical["entity"]]["qualified_name"], []).append(node)
        else:
            assert node["syntax_scope_state"] == "partial" and node["syntax_scope_reason"]
            assert node.get("syntax_scope_body") is None and node.get("syntax_scope_declaration") is None
    if stock:
        return

    def single(name):
        node, = by_function[name]
        assert node["atomic_sema_state"] == "complete", node
        return node

    def order(node, operation, classification, validation="valid"):
        assert node["atomic_operation_kind"] == operation, node
        assert node["atomic_success_order_validation"] == validation, node
        assert node.get("atomic_success_order_class") == classification, node
        if validation != "not_applicable":
            assert node["atomic_success_order_expression"] in syntax, node

    observed = by_function["orders"]
    assert len(observed) == 6
    assert {row["atomic_success_order_class"] for row in observed} == {
        "relaxed", "consume", "acquire", "release", "acq_rel", "seq_cst"}
    for node in observed:
        assert node["atomic_sema_state"] == "complete", node
        order(node, "rmw", node["atomic_success_order_class"])
        assert node["atomic_evaluation_context"] == "potentially_evaluated"
        assert node["atomic_failure_order_validation"] == "not_applicable"
        assert node["atomic_pair_validation"] == "not_applicable"
    for name, operation, classification in (
            ("loaded", "load", "acquire"), ("stored", "store", "release"),
            ("exchanged", "exchange", "acq_rel"),
            ("tested", "test_and_set", "acquire"), ("cleared", "clear", "release")):
        order(single(name), operation, classification)
    order(single("initialized"), "init", None, "not_applicable")
    order(single("dynamic_order"), "load", None, "unknown")
    assert single("dynamic_order")["atomic_sema_reason"] == "original-atomic-order-not-constant"
    for name, operation in (("invalid_load", "load"), ("invalid_store", "store"),
                            ("invalid_clear", "clear")):
        order(single(name), operation, None, "invalid")
    compared = single("compared")
    order(compared, "compare_exchange", "relaxed")
    assert compared["atomic_failure_order_expression"] in syntax
    assert compared["atomic_failure_order_class"] == "seq_cst"
    assert compared["atomic_failure_order_validation"] == "valid"
    assert compared["atomic_pair_validation"] == "unknown"
    assert compared["atomic_sema_reason"] == "original-atomic-compare-exchange-pair-unvalidated"
    failure = single("invalid_failure")
    assert failure["atomic_failure_order_validation"] == "invalid"
    assert failure.get("atomic_failure_order_class") is None
    assert failure["atomic_pair_validation"] == "unknown"
    assert single("unevaluated")["atomic_evaluation_context"] == "unevaluated"
    assert single("discarded")["atomic_evaluation_context"] == "discarded_statement"
    assert single("discarded")["atomic_sema_discarded"] is True
    order(single("local_value"), "load", "relaxed")
    assert any(declarations[feature["context_declaration"]]["kind"] == "Var" and
               syntax[feature["syntax"]]["syntax_scope_state"] == "complete"
               for feature in candidates if feature.get("context_declaration") and
               feature.get("syntax") in syntax)
    nested = [node for node in atomics if node.get("syntax_scope_declaration") and
              declarations[node["syntax_scope_declaration"]]["kind"] == "CXXMethod"]
    nested_node, = nested
    order(nested_node, "load", "relaxed")
    default_node, = [node for node in atomics if node["atomic_sema_default_context"]]
    assert default_node["syntax_scope_state"] == "partial"
    global_node, = [node for node in atomics if node.get("function") is None]
    assert global_node["syntax_scope_state"] == "partial"
    assert any(feature.get("context_declaration") and
               declarations[feature["context_declaration"]]["kind"] == "ParmVar"
               for feature in candidates)
    print("Original atomic Sema and independent source-feature membership controls passed")


if saved := next((argument for argument in sys.argv[3:] if not argument.startswith("--")), None):
    verify(json.loads(Path(saved).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-atomic-sema-") as directory:
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
        assert bounded.stderr.splitlines()[-1] == "application-analysis.query-export-invalid: output: byte-limit", bounded.stderr
