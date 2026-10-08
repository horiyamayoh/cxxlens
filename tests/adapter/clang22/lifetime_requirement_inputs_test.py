"""Original observed lifetime requirement inventory and independent empty domains."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
fixture = Path(__file__).with_name("fixtures") / "lifetime_requirement_inputs.cpp"
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)


def members(encoded):
    data = bytes.fromhex(encoded)
    result = []
    while data:
        assert len(data) >= 4
        size = int.from_bytes(data[:4], "little")
        assert 0 < size <= len(data) - 4
        result.append(data[4:4 + size].decode())
        data = data[4 + size:]
    assert result == sorted(set(result))
    return set(result)


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
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    elements = {row["element"]: row for row in rows["cc.cfg_element.v1"]}
    bodies = {entities[row["function"]]["qualified_name"]: row for row in rows["cc.body.v1"]}
    profile = "clang22-original-observed-lifetime-requirements/1"

    def inventory(body):
        assert body["lifetime_requirements_profile"] == profile
        selected_syntax = members(body["lifetime_requirement_syntax_ids"])
        selected_declarations = members(body["lifetime_requirement_declaration_ids"])
        selected_elements = members(body["lifetime_requirement_cfg_element_ids"])
        retained = len(selected_syntax) + len(selected_declarations) + len(selected_elements)
        assert body["lifetime_requirement_count"] >= retained
        physical = declarations[body["function_exit_declaration"]]
        assert physical["entity"] == body["function"]
        assert all(syntax[key]["function"] == body["function"] and
                   syntax[key]["compile_unit"] == body["compile_unit"] for key in selected_syntax)
        assert all(elements[key]["body"] == body["body"] and
                   elements[key]["function"] == body["function"] and
                   elements[key]["compile_unit"] == body["compile_unit"] for key in selected_elements)
        assert all(key in declarations for key in selected_declarations)
        if body["lifetime_requirements_state"] == "complete":
            assert body["lifetime_requirement_count"] == retained
            assert body["ast_state"] == body["cfg_element_state"] == "complete"
            assert body["eligibility"] == "closed"
            assert all(elements[key]["binding_state"] == "complete" for key in selected_elements)
        else:
            assert body["lifetime_requirements_reason"]
        return selected_syntax, selected_declarations, selected_elements

    for body in bodies.values():
        inventory(body)
    zero = bodies["scalar_zero"]
    assert zero["lifetime_requirements_state"] == "complete"
    assert zero["lifetime_requirement_count"] == 0
    assert not any(inventory(zero))
    # These independent observations remain present despite the empty requirements.
    cfg = [elements[key] for key in members(zero["cfg_element_ids"])]
    assert len(cfg) >= 8 and sum(row["kind"] == "lifetime_end" for row in cfg) >= 2
    assert zero["address_input_count"] >= 2
    assert bodies["enum_zero"]["lifetime_requirements_state"] == "complete"
    assert bodies["enum_zero"]["lifetime_requirement_count"] == 0

    for name in ("call_candidate", "pointer_candidate", "reference_candidate",
                 "record_candidate", "array_candidate", "address_candidate",
                 "xvalue_cast_candidate", "reference_cast_candidate", "cstyle_xvalue_candidate",
                 "reference_capture_candidate", "implicit_move_candidate"):
        body = bodies[name]
        assert body["lifetime_requirement_count"] > 0
        assert any(inventory(body))
    call_syntax, _, call_cfg = inventory(bodies["call_candidate"])
    assert any(syntax[key]["kind"] == "CallExpr" for key in call_syntax)
    assert any(elements[key].get("expression") in call_syntax for key in call_cfg)
    for name in ("pointer_candidate", "reference_candidate", "record_candidate"):
        _, selected, _ = inventory(bodies[name])
        assert any(declarations[key]["kind"] == "ParmVar" for key in selected)
    for name in ("xvalue_cast_candidate", "reference_cast_candidate", "cstyle_xvalue_candidate"):
        selected, _, _ = inventory(bodies[name])
        assert any(syntax[key]["kind"] in ("CXXStaticCastExpr", "CStyleCastExpr") for key in selected)
    capture_syntax, _, _ = inventory(bodies["reference_capture_candidate"])
    assert any(syntax[key]["kind"] == "LambdaExpr" for key in capture_syntax)
    capture_bodies = [body for body in bodies.values()
                      if body.get("lifetime_requirements_reason") == "original-lambda-capture-body-context-unbound"]
    assert capture_bodies and all(body["lifetime_requirements_state"] == "partial" and
                                 body["lifetime_requirement_count"] > sum(map(len, inventory(body)))
                                 for body in capture_bodies)
    _, selected, _ = inventory(bodies["array_candidate"])
    assert any(declarations[key]["kind"] == "Var" for key in selected)
    for name in ("default_candidate", "default_field::default_field",
                 "assembly_candidate", "dependent_candidate"):
        body = bodies[name]
        assert body["lifetime_requirements_state"] == "partial"
        assert body["lifetime_requirement_count"] > 0 and body["lifetime_requirements_reason"]
    default = bodies["default_field::default_field"]
    assert default["lifetime_requirement_count"] > sum(map(len, inventory(default)))
    assert "absent_body" not in bodies
    absent = [row for row in declarations.values()
              if entities[row["entity"]]["qualified_name"] == "absent_body"]
    assert absent and all(not any(key.startswith("lifetime_requirement") for key in row) for row in absent)

    # A missing retained member cannot be admitted as complete by this independent reader.
    broken = copy.deepcopy(bodies["pointer_candidate"])
    broken["lifetime_requirement_syntax_ids"] = ""
    try:
        inventory(broken)
    except AssertionError:
        pass
    else:
        raise AssertionError("complete subset was incorrectly admitted")
    print("Original lifetime requirements: closed scalar zeros, typed members and partial frontiers passed")
    return {name: {key: value for key, value in body.items()
                   if key.startswith("lifetime_requirement")} for name, body in bodies.items()}


if len(sys.argv) == 4:
    verify(json.loads(Path(sys.argv[3]).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-lifetime-requirements-") as directory:
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
        first = verify(json.loads(run.stdout))
        repeated = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert repeated.returncode == 0, repeated.stderr
        assert first == verify(json.loads(repeated.stdout))
        bounded = subprocess.run(arguments + ["--maximum-output-bytes", "1"],
                                 env=environment, text=True, capture_output=True, timeout=180)
        assert bounded.returncode == 1 and bounded.stdout == ""
        assert bounded.stderr.splitlines()[-1] == "application-analysis.query-export-invalid: output: byte-limit", bounded.stderr
