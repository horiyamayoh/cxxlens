"""Compiler-observed local definition roles, independent from legacy point numbers."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)

source = """
int roles(int input, int untouched) {
    int initialized = input;
    int uninitialized;
    input = initialized;
    initialized += 2;
    ++initialized;
    return initialized;
}
"""

with tempfile.TemporaryDirectory(prefix="cxxlens-definition-roles-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source)
    commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                 [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]}]
    (root / "compile_commands.json").write_text(json.dumps(commands))
    run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                          str(root / "compile_commands.json")], env=environment,
                         text=True, capture_output=True, timeout=180)
    debug_root = os.environ.get("CXXLENS_INPUT_DEBUG_DIR")
    debug = Path(debug_root) / "definition-roles" if debug_root else Path(directory)
    debug.mkdir(parents=True, exist_ok=True)
    (debug / "analyzer.stdout.json").write_text(run.stdout)
    (debug / "analyzer.stderr.log").write_text(run.stderr)
    assert run.returncode == 0, run.stderr
    rows = {scan["logical_ir"]["relation_requirements"][0]["descriptor_id"]:
            [{name.removeprefix("output."): value.get("value")
              for name, value in row["values"].items()} for row in scan["result"]["rows"]]
            for scan in json.loads(run.stdout)["queries"]}
    outcomes = rows["build.compile_unit_analysis.v1"]
    assert outcomes and all(row["semantic_output"] == "produced" for row in outcomes), outcomes
    definitions = [row for row in rows["cc.flow_fact.v1"] if row["kind"] == "definition"]
    assert definitions and all(row["definition_profile"] == "clang22-original-definition-roles/1"
                               for row in definitions), definitions
    grouped = {role: [row for row in definitions if row["definition_role"] == role]
               for role in ("parameter_entry", "initialized_declaration",
                            "uninitialized_declaration", "assignment",
                            "compound_assignment", "increment_decrement")}
    assert {role: len(members) for role, members in grouped.items()} == {
        "parameter_entry": 2, "initialized_declaration": 1,
        "uninitialized_declaration": 1, "assignment": 1,
        "compound_assignment": 1, "increment_decrement": 1}, grouped
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    body, = [row for row in rows["cc.body.v1"]
             if entities[row["function"]]["qualified_name"] == "roles"]
    nodes = {row["node"]: row for row in rows["cc.cfg_node.v1"]}
    for row in grouped["parameter_entry"]:
        assert entities[row["subject"]]["kind"] == "parameter", row
        assert row["node"] == body["entry"] and row["function"] == body["function"], row
        assert nodes[row["node"]]["kind"] == "entry" and nodes[row["node"]]["body"] == body["body"], row
        assert row["compile_unit"] == body["compile_unit"] == nodes[row["node"]]["compile_unit"], row
        assert row["cfg_binding_state"] == "unavailable" and row.get("element_index") is None, row
        assert row.get("expression") is None and row.get("value_expression") is None, row
    parameter_ids = {row["subject"] for row in grouped["parameter_entry"]}
    assert grouped["assignment"][0]["subject"] in parameter_ids, grouped
    for role in ("initialized_declaration", "assignment", "compound_assignment"):
        assert all(row.get("expression") and row.get("value_expression")
                   for row in grouped[role]), grouped[role]
    assert grouped["uninitialized_declaration"][0]["value"] == "uninitialized", grouped
    assert grouped["uninitialized_declaration"][0].get("value_expression") is None, grouped
    assert grouped["increment_decrement"][0].get("expression"), grouped
