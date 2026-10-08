"""Original direct-return referents, independent statement census and actual ends."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
fixture = Path(__file__).with_name("fixtures") / "direct_return_referent_inputs.cpp"
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)


def members(encoded):
    data = bytes.fromhex(encoded)
    result = []
    while data:
        assert len(data) >= 4
        length = int.from_bytes(data[:4], "little")
        assert 0 < length <= len(data) - 4
        result.append(data[4:4 + length].decode())
        data = data[4 + length:]
    assert result == sorted(set(result))
    return set(result)


def verify(bundle):
    scans = {query["relation_id"]: query["result"] for query in bundle["queries"]}
    assert all(scan["status"] == "complete" for scan in scans.values())
    rows = {name: [{key.removeprefix("output."): cell.get("value")
                   for key, cell in row["values"].items()} for row in scan["rows"]]
            for name, scan in scans.items()}
    assert all(row["semantic_output"] == "produced" and
               row["parse_error_count"] == row["fatal_error_count"] == 0
               for row in rows["build.compile_unit_analysis.v1"])
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    elements = {row["element"]: row for row in rows["cc.cfg_element.v1"]}
    bodies = {row["function_exit_declaration"]: row for row in rows["cc.body.v1"]}
    by_name = {entities[row["function"]]["qualified_name"]: row for row in bodies.values()}
    returns = [row for row in rows["cc.address_transfer.v1"] if row["kind"] == "return"]

    def bind(row):
        assert row["return_referent_profile"] == "clang22-original-direct-return-referent/1"
        body = bodies[row["owner_declaration"]]
        assert row["compile_unit"] == body["compile_unit"]
        statement = syntax[row["carrier_syntax"]]
        assert statement["kind"] == "ReturnStmt"
        assert statement["function"] == body["function"] and statement["compile_unit"] == body["compile_unit"]
        assert row["carrier_syntax"] in members(body["return_statement_ids"])
        ends = members(row["return_lifetime_end_ids"])
        assert row["return_lifetime_end_count"] >= len(ends)
        if row["return_lifetime_end_state"] == "complete":
            assert row["return_lifetime_end_count"] == len(ends)
            assert body["cfg_element_state"] == "complete"
            for key in ends:
                end = elements[key]
                assert end["kind"] == "lifetime_end" and end["binding_state"] == "complete"
                assert end["declaration"] == row["return_referent_declaration"]
                assert end["body"] == body["body"] and end["function"] == body["function"]
                assert end["compile_unit"] == body["compile_unit"]
            expected = {key for key, end in elements.items()
                        if end["kind"] == "lifetime_end" and end["body"] == body["body"] and
                        end.get("declaration") == row["return_referent_declaration"]}
            assert ends == expected
        if row["return_referent_state"] == "complete" and row["return_referent_kind"] != "nonborrow":
            declaration = declarations[row["return_referent_declaration"]]
            leaf = syntax[row["return_referent_expression"]]
            assert declaration["kind"] in ("Var", "ParmVar")
            assert leaf["kind"] == "DeclRefExpr"
            assert leaf["compile_unit"] == body["compile_unit"] and leaf["function"] == body["function"]
        return row

    for body in bodies.values():
        assert body["return_statement_profile"] == "clang22-original-written-return-statements/1"
        original = members(body["return_statement_ids"])
        assert body["return_statement_count"] >= len(original)
        if body["return_statement_state"] == "complete":
            assert body["return_statement_count"] == len(original)
            assert body["ast_state"] == "complete" and body["eligibility"] == "closed"
        else:
            assert body["return_statement_reason"]
        assert all(syntax[key]["kind"] == "ReturnStmt" for key in original)
        admitted = {row["carrier_syntax"] for row in returns if row["owner_declaration"] == body["function_exit_declaration"]}
        if body["return_statement_state"] == body["address_input_state"] == "complete":
            assert original == admitted
    named = {}
    for row in returns:
        bind(row)
        name = entities[declarations[row["owner_declaration"]]["entity"]]["qualified_name"]
        named.setdefault(name, []).append(row)
    for name in ("local_address", "local_reference", "parenthesized_address", "qualified_address", "local_array", "parameter_address"):
        row = named[name][0]
        assert row["return_referent_state"] == row["return_lifetime_end_state"] == "complete"
        assert row["return_referent_storage_duration"] == "automatic" and row["return_lifetime_end_count"] > 0
    assert named["local_reference"][0]["return_referent_kind"] == "direct_variable_reference"
    assert named["local_array"][0]["return_referent_kind"] == "direct_array_decay"
    for name, duration in (("global_address", "static"), ("static_address", "static"), ("thread_address", "thread")):
        row = named[name][0]
        assert row["return_referent_state"] == row["return_lifetime_end_state"] == "complete"
        assert row["return_referent_storage_duration"] == duration
        assert row["return_lifetime_end_count"] == 0 and not members(row["return_lifetime_end_ids"])
    for name in ("reference_parameter", "pointer_value", "explicit_cast_address", "member_address", "conditional_address", "default_call", "dependent_return"):
        row = named[name][0]
        assert row["return_referent_state"] != "complete" and row["return_referent_reason"]
        assert row["return_lifetime_end_state"] != "complete"
    for name in ("scalar_return", "void_return"):
        assert named[name][0]["return_referent_kind"] == "nonborrow"
        assert named[name][0]["return_referent_state"] == "complete"
    assert len(named["branch_address"]) == 2
    assert by_name["branch_address"]["return_statement_count"] == 2
    for row in named["branch_address"]:
        assert row["return_lifetime_end_state"] == "complete" and row["return_lifetime_end_count"] > 0
    for name in ("captured_copy()::(lambda)::operator()", "captured_reference()::(lambda)::operator()"):
        row = named[name][0]
        assert row["return_referent_kind"] == "indirect" and row["return_referent_state"] == "partial"
        assert row["return_referent_reason"] == "original-captured-or-enclosing-variable-referent-unavailable"
        assert row["return_referent_declaration"] is None and row["return_referent_expression"] is None
        assert row["return_lifetime_end_state"] == "unavailable"
    assert "bodyless" not in by_name
    assert all(all(value is None for key, value in row.items()
                   if key.startswith("return_referent_") or key.startswith("return_lifetime_end_"))
               for row in rows["cc.address_transfer.v1"] if row["kind"] != "return")
    broken = copy.deepcopy(named["local_address"][0])
    broken["return_lifetime_end_ids"] = ""
    try:
        bind(broken)
    except AssertionError:
        pass
    else:
        raise AssertionError("a complete lifetime-end subset was admitted")
    broken = copy.deepcopy(named["local_address"][0])
    broken["return_referent_declaration"] = named["local_reference"][0]["return_referent_declaration"]
    try:
        bind(broken)
    except AssertionError:
        pass
    else:
        raise AssertionError("a foreign physical referent was admitted")
    body = bodies[named["local_address"][0]["owner_declaration"]]
    original_returns = body["return_statement_ids"]
    body["return_statement_ids"] = ""
    try:
        bind(named["local_address"][0])
    except AssertionError:
        pass
    else:
        raise AssertionError("a return missing from the independent census was admitted")
    finally:
        body["return_statement_ids"] = original_returns
    print("Original direct returns: exact referents, actual ends and independent statement population passed")
    return {name: [{key: value for key, value in row.items() if key.startswith("return_")} for row in values]
            for name, values in named.items()}


if len(sys.argv) == 4:
    verify(json.loads(Path(sys.argv[3]).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-direct-return-") as directory:
        root = Path(directory)
        shutil.copyfile(fixture, root / "main.cpp")
        commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                     [str(compiler), "-std=c++23", "-nostdinc", "-nostdinc++", "-O0", "-c", "main.cpp", "-o", "main.o"]}]
        (root / "compile_commands.json").write_text(json.dumps(commands))
        arguments = [str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")]
        run = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert run.returncode == 0, run.stderr
        first = verify(json.loads(run.stdout))
        repeated = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert repeated.returncode == 0, repeated.stderr
        assert first == verify(json.loads(repeated.stdout))
        bounded = subprocess.run(arguments + ["--maximum-output-bytes", "1"], env=environment, text=True, capture_output=True, timeout=180)
        assert bounded.returncode == 1 and bounded.stdout == ""
        assert bounded.stderr.splitlines()[-1] == "application-analysis.query-export-invalid: output: byte-limit", bounded.stderr
