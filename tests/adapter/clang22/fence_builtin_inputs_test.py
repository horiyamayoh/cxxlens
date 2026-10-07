"""Compiler builtin fence membership, physical scope and unchecked original order axis."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
stock = "--stock-template-events" in sys.argv[3:]
fixture = Path(__file__).with_name("fixtures") / "fence_builtin_inputs.cpp"
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
    assert all(row["semantic_output"] == "produced" and
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
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    bodies = {row["body"]: row for row in rows["cc.body.v1"]}
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    calls = [node for node in syntax.values() if node.get("compiler_builtin_profile")]
    assert calls and all(node["compiler_builtin_profile"] == "clang22-original-compiler-builtin-call/1"
                         and node["compiler_builtin_state"] == "complete" for node in calls)
    assert any(node["compiler_builtin_kind"] == "other_builtin" and node["compiler_builtin_id"] > 0
               for node in calls)
    assert sum(node["compiler_builtin_kind"] == "not_builtin" and node["compiler_builtin_id"] == 0
               for node in calls) >= 4  # direct, indirect, same-spelled user call, lambda invocation
    fences = [node for node in calls if node["compiler_builtin_kind"] in
              ("atomic_thread_fence", "atomic_signal_fence")]
    assert len(fences) == 10 and len({node["compiler_builtin_id"] for node in fences}) == 5
    by_function = {}
    for node in fences:
        assert node["compiler_builtin_id"] > 0 and node["fence_order_expression"] in syntax
        callee = declarations[node["compiler_builtin_callee_declaration"]]
        assert callee["kind"] == "Function" and callee["is_implicit"]
        assert node["fence_sema_profile"] == "clang22-original-fence-sema/1"
        assert any(feature.get("syntax") == node["node"] and
                   feature["compile_unit"] == node["compile_unit"] for feature in features.values())
        if stock:
            assert node["fence_sema_state"] == "unavailable"
            assert node.get("fence_evaluation_context") is None
            assert node.get("fence_order_validation") is None
        else:
            assert node["fence_sema_state"] == "complete", node
            assert node["fence_order_validation"] == "not_checked"
            assert node["fence_sema_reason"] == "original-fence-order-predicate-not-checked"
        if node["syntax_scope_state"] == "complete":
            body = bodies[node["syntax_scope_body"]]
            physical = declarations[node["syntax_scope_declaration"]]
            assert body["function_exit_declaration"] == physical["declaration"]
            assert body["compile_unit"] == node["compile_unit"]
            assert body["function"] == node["function"] == physical["entity"]
            by_function.setdefault(entities[physical["entity"]]["qualified_name"], []).append(node)
        else:
            assert node["syntax_scope_state"] == "partial" and node["syntax_scope_reason"]
            assert node.get("syntax_scope_body") is None and node.get("syntax_scope_declaration") is None
    assert len(by_function["sites"]) == 5
    assert sum(node["syntax_scope_state"] == "partial" for node in fences) == 2
    assert any(node.get("syntax_scope_declaration") and
               declarations[node["syntax_scope_declaration"]]["kind"] == "CXXMethod" for node in fences)
    if not stock:
        assert by_function["discarded"][0]["fence_sema_discarded"] is True
        assert by_function["discarded"][0]["fence_evaluation_context"] == "discarded_statement"
        assert by_function["unevaluated"][0]["fence_evaluation_context"] == "unevaluated"
        assert sum(bool(node["fence_sema_default_context"]) for node in fences) == 1
    print("Original compiler fence identity/scope and unchecked order controls passed")


if saved := next((argument for argument in sys.argv[3:] if not argument.startswith("--")), None):
    verify(json.loads(Path(saved).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-fence-builtin-") as directory:
        root = Path(directory)
        shutil.copyfile(fixture, root / "main.cpp")
        (root / "compile_commands.json").write_text(json.dumps([{
            "directory": str(root), "file": "main.cpp", "arguments": [str(compiler),
            "-std=c++23", "-nostdinc", "-nostdinc++", "-O0", "-c", "main.cpp", "-o", "main.o"]}]))
        arguments = [str(analyzer), "--project-root", str(root), "--compile-commands",
                     str(root / "compile_commands.json")]
        run = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert run.returncode == 0, run.stderr
        verify(json.loads(run.stdout))
        bounded = subprocess.run(arguments + ["--maximum-output-bytes", "1"],
                                 env=environment, text=True, capture_output=True, timeout=180)
        assert bounded.returncode == 1 and bounded.stdout == ""
        assert bounded.stderr.splitlines()[-1] == "application-analysis.query-export-invalid: output: byte-limit", bounded.stderr
