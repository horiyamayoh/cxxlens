"""Original FieldDecl bit-field facts and independent full record membership."""
import copy
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
fixture = Path(__file__).with_name("fixtures") / "field_bitfield_inputs.cpp"
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
    return result


def verify(bundle):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    scans = {item["relation_id"]: item["result"] for item in bundle["queries"]}
    assert all(scan["status"] == "complete" for scan in scans.values())
    rows = {name: [{key.removeprefix("output."): cell.get("value")
                   for key, cell in row["values"].items()} for row in scan["rows"]]
            for name, scan in scans.items()}
    assert all(row["semantic_output"] == "produced" and row["parse_error_count"] == 0 and
               row["fatal_error_count"] == 0 for row in rows["build.compile_unit_analysis.v1"])
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    details = rows["cc.entity_detail.v1"]
    records = {entities[row["entity"]]["qualified_name"]: row for row in rows["cc.record_surface.v1"]
               if row["is_definition"]}
    assert {"LongOnly", "Bitfields", "DependentBits"} <= set(records), records.keys()

    def bind(record, field_id):
        assert record["enumeration_state"] == "complete"
        assert record["field_count"] == len(members(record["fields"]))
        assert field_id in members(record["fields"])
        assert entities[field_id]["kind"] == "field"
        matched = [row for row in details if row["entity"] == field_id and
                   row["compile_unit"] == record["compile_unit"]]
        assert len(matched) == 1
        row, = matched
        assert row["source"] and row["field_bitfield_profile"] == "clang22-original-field-bitfield/1"
        assert type(row["field_is_bitfield"]) is bool
        state = row["field_bitfield_state"]
        if state == "complete":
            assert not row.get("field_bitfield_reason")
            if row["field_is_bitfield"]:
                assert type(row["field_bit_width"]) is int and row["field_bit_width"] >= 0
            else:
                assert row.get("field_bit_width") is None
        else:
            assert state == "partial" and row["field_is_bitfield"]
            assert row.get("field_bit_width") is None and row["field_bitfield_reason"]
        return row

    bound = {name: [bind(record, field) for field in members(record["fields"])]
             for name, record in records.items() if name in ("LongOnly", "Bitfields", "DependentBits")}
    plain, = bound["LongOnly"]
    assert plain["field_is_bitfield"] is False and plain["field_bitfield_state"] == "complete"
    widths = sorted(row["field_bit_width"] for row in bound["Bitfields"] if row["field_is_bitfield"])
    assert widths == [0, 7, 9], widths
    assert len(bound["Bitfields"]) == 4 and len(bound["DependentBits"]) == 2
    dependent, = [row for row in bound["DependentBits"] if row["field_is_bitfield"]]
    assert dependent["field_bitfield_state"] == "partial"
    assert dependent["field_bitfield_reason"] == "original-bitfield-width-requires-specialization"
    assert all(row.get("field_bitfield_profile") is None for row in details
               if entities[row["entity"]]["kind"] != "field")
    field_ids = members(records["Bitfields"]["fields"])
    broken = copy.deepcopy(records["Bitfields"])
    broken["fields"] = ""
    try:
        bind(broken, field_ids[0])
    except AssertionError:
        pass
    else:
        raise AssertionError("missing independent original field census was admitted")
    foreign = members(records["LongOnly"]["fields"])[0]
    try:
        bind(records["Bitfields"], foreign)
    except AssertionError:
        pass
    else:
        raise AssertionError("a foreign original field was admitted")
    print("Original FieldDecl facts: non-bitfield, signed/unsigned, zero and dependent widths passed")
    return {name: [{key: row.get(key) for key in ("field_bitfield_profile", "field_bitfield_state",
                                                "field_is_bitfield", "field_bit_width", "field_bitfield_reason")}
                   for row in sorted(values, key=lambda row: (row["field_is_bitfield"], row.get("field_bit_width") or 0))]
            for name, values in bound.items()}


if len(sys.argv) == 4:
    verify(json.loads(Path(sys.argv[3]).read_text()))
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-field-bitfield-") as directory:
        root = Path(directory)
        shutil.copyfile(fixture, root / "main.cpp")
        commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                     [str(compiler), "-std=c++23", "-nostdinc", "-nostdinc++", "-O0", "-c", "main.cpp", "-o", "main.o"]}]
        (root / "compile_commands.json").write_text(json.dumps(commands))
        arguments = [str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")]
        first = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert first.returncode == 0, first.stderr
        expected = verify(json.loads(first.stdout))
        repeated = subprocess.run(arguments, env=environment, text=True, capture_output=True, timeout=180)
        assert repeated.returncode == 0, repeated.stderr
        assert expected == verify(json.loads(repeated.stdout))
        bounded = subprocess.run(arguments + ["--maximum-output-bytes", "1"], env=environment, text=True, capture_output=True, timeout=180)
        assert bounded.returncode == 1 and bounded.stdout == ""
        assert bounded.stderr.splitlines()[-1] == "application-analysis.query-export-invalid: output: byte-limit", bounded.stderr
