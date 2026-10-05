"""Original final candidate and reached-call events through public query data."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile


def cells(row):
    return {name.removeprefix("output."): value.get("value")
            for name, value in row["values"].items()}


def members(encoded):
    data = bytes.fromhex(encoded)
    result = []
    while data:
        assert len(data) >= 4
        width = int.from_bytes(data[:4], "little")
        assert len(data) >= width + 4
        result.append(data[4:4 + width].decode())
        data = data[4 + width:]
    assert result == sorted(set(result))
    return result


analyzer, compiler, projection = map(Path, sys.argv[1:4])
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)
source = r'''
template<class T> auto choose(T value) -> decltype(value.missing()) {
 return value.missing();
}
int choose(...) { return 0; }
constexpr int leaf(int value) { return value + 1; }
constexpr int constant = leaf(7);
int runtime() { return leaf(1) + choose(1); }
'''
with tempfile.TemporaryDirectory(prefix="cxxlens-template-events-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source)
    (root / "compile_commands.json").write_text(json.dumps([{
        "directory": str(root), "file": "main.cpp", "arguments":
        [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]}]))
    run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                          str(root / "compile_commands.json")],
                         env=environment, text=True, capture_output=True, timeout=240)
    debug = Path(os.environ.get("CXXLENS_TEMPLATE_EVENT_DEBUG_DIR", directory))
    debug.mkdir(parents=True, exist_ok=True)
    (debug / "analyzer.stdout.json").write_text(run.stdout)
    (debug / "analyzer.stderr.log").write_text(run.stderr)
    (debug / "main.cpp").write_text(source)
    assert run.returncode == 0, run.stderr
    bundle = json.loads(run.stdout)
    rows = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]:
            [cells(row) for row in query["result"]["rows"]] for query in bundle["queries"]}
    outcome, = rows["build.compile_unit_analysis.v1"]
    assert outcome["parse_outcome"] == "success" and outcome["semantic_output"] == "produced", outcome
    inventory, = rows["cc.template_inventory.v1"]
    candidates = rows["cc.template_candidate.v1"]
    roots = rows["cc.constant_evaluation_root.v1"]
    calls = rows["cc.constant_evaluated_call.v1"]
    for kind, population, identity in (("candidate", candidates, "candidate"),
                                       ("evaluation_root", roots, "root")):
        assert inventory[kind + "_state"] == "complete", inventory
        assert inventory[kind + "_count"] == len(population)
        assert set(members(inventory[kind + "_ids"])) == {row[identity] for row in population}
    assert any(row["deduction_result"] == "substitution_failure" and
               row["exclusion_disposition"] == "substitution_exclusion" and
               row["completed"] is True for row in candidates), candidates
    assert len(calls) == 1, calls
    call, = calls
    assert call["is_system"] is False and call["binding_state"] == "complete" and call["root_count"] > 0
    # The returned compiler USR is compared as bytes. The runtime leaf call is a
    # distinct AST occurrence and must not be counted by this original profile.
    assert [bytes.fromhex(value) for value in members(call["callee_usr_hexes"])] == [b"c:@F@leaf#I#"]
    root_ids = set(members(call["root_ids"]))
    assert root_ids <= {row["root"] for row in roots}
    assert all(row["completion"] == "complete" and row["call_state"] == "complete" for row in roots)
    assert all(call["call"] in members(row["call_ids"]) for row in roots if row["root"] in root_ids)
    projected = subprocess.run([str(projection), str(debug / "analyzer.stdout.json"),
                                "--complete-template-events"],
                               env=environment, text=True, capture_output=True, timeout=60)
    assert projected.returncode == 0, (projected.stdout, projected.stderr)
