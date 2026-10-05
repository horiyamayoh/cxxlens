"""Actual source type facts, detached through the public application query format."""
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
typedef bool UserAlias;
bool actual_bool() { return true; }
enum Fixed : unsigned short;
Fixed fixed_enum;
enum Complete : int { first = 0, second = 1 };
Complete complete_enum;
struct BoolLike { explicit operator bool() const; };
void observe(int value, const UserAlias boolean, bool* pointer, BoolLike record) {
    _BitInt(9) signed_bits = 1;
    unsigned _BitInt(9) unsigned_bits = 1;
    int sum = value + 1;
    unsigned char narrowed = (unsigned char)sum;
    if (actual_bool()) {}
    if (value) {}
}
"""
with tempfile.TemporaryDirectory(prefix="cxxlens-original-scalar-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source)
    commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                 [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]}]
    (root / "compile_commands.json").write_text(json.dumps(commands))
    run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                          str(root / "compile_commands.json")], env=environment,
                         text=True, capture_output=True, timeout=180)
    if debug_root := os.environ.get("CXXLENS_INPUT_DEBUG_DIR"):
        debug = Path(debug_root) / "scalar-types"
        debug.mkdir(parents=True, exist_ok=True)
        (debug / "analyzer.stdout.json").write_text(run.stdout)
        (debug / "analyzer.stderr.log").write_text(run.stderr)
    assert run.returncode == 0, run.stderr
    rows = {scan["logical_ir"]["relation_requirements"][0]["descriptor_id"]:
            [{name.removeprefix("output."): value.get("value")
              for name, value in row["values"].items()} for row in scan["result"]["rows"]]
            for scan in json.loads(run.stdout)["queries"]}
    assert rows["build.compile_unit_analysis.v1"] and all(
        row["semantic_output"] == "produced"
        for row in rows["build.compile_unit_analysis.v1"]), run.stderr
    types = {row["type"]: row for row in rows["cc.type.v1"]}
    syntax = rows["cc.syntax_node.v1"]
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    details = rows["cc.entity_detail.v1"]

    def original_variable(name):
        entity, = [row["entity"] for row in entities.values()
                   if row["qualified_name"] == name]
        detail, = [row for row in details if row["entity"] == entity]
        return types[detail["canonical_type"]]

    for name, signed in (("signed_bits", True), ("unsigned_bits", False)):
        observed = original_variable(name)
        assert observed["integer_profile"] == "clang22-original-integer-representation/1", observed
        assert observed["integer_state"] == "complete" and observed["integer_bit_width"] == 9, observed
        assert observed["integer_signed"] is signed, observed
    for name, width, signed in (("fixed_enum", 16, False), ("complete_enum", 32, True)):
        observed = original_variable(name)
        assert observed["integer_state"] == "complete" and observed["integer_bit_width"] == width, observed
        assert observed["integer_signed"] is signed and observed["integer_underlying_type"] in types, observed
    boolean = original_variable("boolean")
    assert boolean["builtin_kind"] == "Bool" and boolean["builtin_state"] == "complete", boolean
    assert boolean["builtin_profile"] == "clang22-original-builtin-type/1", boolean
    for name in ("pointer", "record"):
        observed = original_variable(name)
        assert observed.get("builtin_kind") is None and observed["integer_state"] == "not_applicable", observed
    call, = [row for row in syntax if row["kind"] == "CallExpr"]
    assert types[call["canonical_type"]]["builtin_kind"] == "Bool", call
    narrowing, = [row for row in syntax if row.get("cast_kind") == "IntegralCast"
                  and types[row["canonical_type"]].get("builtin_kind") == "UChar"]
    destination, origin = types[narrowing["canonical_type"]], types[narrowing["operand_type"]]
    assert (destination["integer_bit_width"], destination["integer_signed"]) == (8, False), destination
    assert (origin["integer_bit_width"], origin["integer_signed"]) == (32, True), origin
    arithmetic, = [row for row in syntax if row["kind"] == "BinaryOperator" and row.get("opcode") == "+"]
    assert types[arithmetic["canonical_type"]]["integer_bit_width"] == 32, arithmetic
    conversions = [row for row in syntax if row.get("cast_kind") == "IntegralToBoolean"]
    assert len(conversions) == 1, conversions
    assert types[conversions[0]["operand_type"]]["builtin_kind"] == "Int", conversions
