"""Supported Linux-host capture of actual driver-selected semantic TargetInfo."""
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
long original_long;
wchar_t original_wchar;
void* original_pointer;
unsigned original_sizes() {
    return sizeof(long) + sizeof(void*) + sizeof(wchar_t);
}
"""


def verify(bundle, target, long_width, wchar_width, long_bytes, wchar_bytes):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    scans = {query["relation_id"]: query["result"] for query in bundle["queries"]}
    assert all(scan["status"] == "complete" for scan in scans.values())
    rows = {name: [{key.removeprefix("output."): cell.get("value")
                   for key, cell in row["values"].items()} for row in scan["rows"]]
            for name, scan in scans.items()}
    assert all(row["semantic_output"] == "produced" and
               row["parse_error_count"] == row["fatal_error_count"] == 0
               for row in rows["build.compile_unit_analysis.v1"])
    context, = rows["build.toolchain_context.v1"]
    assert context["target_triple"] == target, context
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    types = {row["type"]: row for row in rows["cc.type.v1"]}

    def variable(name):
        entity, = [row["entity"] for row in entities.values() if row["qualified_name"] == name]
        detail, = [row for row in rows["cc.entity_detail.v1"] if row["entity"] == entity]
        return types[detail["canonical_type"]]

    for name, width, size in (("original_long", long_width, long_bytes),
                              ("original_wchar", wchar_width, wchar_bytes)):
        observed = variable(name)
        assert observed["integer_state"] == "complete" and observed["integer_bit_width"] == width
        assert observed["integer_storage_profile"] == "clang22-original-integer-object-storage/1"
        assert observed["integer_storage_state"] == "complete"
        assert observed["integer_object_bytes"] == size
    assert variable("original_pointer")["integer_storage_state"] == "not_applicable"
    sizes, = [row["entity"] for row in entities.values() if row["qualified_name"] == "original_sizes"]
    abi = [row for row in rows["cc.abi_surface.v1"] if row["entity"] == sizes]
    assert abi and all(row["target_data_model_profile"] == "clang22-original-target-data-model/1" and
                       row["target_data_model_state"] == "complete" and
                       (row["char_width_bits"], row["long_width_bits"], row["pointer_width_bits"],
                        row["wchar_width_bits"], row["byte_order"]) ==
                       (8, long_width, 64, wchar_width, "little") for row in abi)
    traits = [row for row in rows["cc.syntax_node.v1"]
              if row.get("function") == sizes and row.get("type_trait_kind") == "sizeof"]
    assert len(traits) == 3 and all(row["type_trait_state"] == "complete" for row in traits)
    assert sorted(int(row["type_trait_result_decimal"]) for row in traits) == sorted(
        (long_bytes, 8, wchar_bytes))
    return context["target_triple"], tuple(row["integer_object_bytes"] for row in
                                          (variable("original_long"), variable("original_wchar")))


if len(sys.argv) == 5:
    verify(json.loads(Path(sys.argv[3]).read_text()), "x86_64-unknown-linux-gnu", 64, 32, 8, 4)
    verify(json.loads(Path(sys.argv[4]).read_text()), "x86_64-pc-windows-msvc", 32, 16, 4, 2)
else:
    with tempfile.TemporaryDirectory(prefix="cxxlens-semantic-target-") as directory:
        root = Path(directory)
        (root / "main.cpp").write_text(source)
        observations = []
        for target, long_width, wchar_width, long_bytes, wchar_bytes in (
                ("x86_64-unknown-linux-gnu", 64, 32, 8, 4),
                ("x86_64-pc-windows-msvc", 32, 16, 4, 2)):
            commands = [{"directory": str(root), "file": "main.cpp", "arguments":
                         [str(compiler), "--target=" + target, "-std=c++23", "-nostdinc", "-nostdinc++",
                          "-O0", "-c", "main.cpp", "-o", "main.o"]}]
            (root / "compile_commands.json").write_text(json.dumps(commands))
            run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                                  str(root / "compile_commands.json")], env=environment,
                                 text=True, capture_output=True, timeout=180)
            assert run.returncode == 0, run.stderr
            observations.append(verify(json.loads(run.stdout), target, long_width, wchar_width, long_bytes, wchar_bytes))
        assert observations[0] != observations[1]
print("Original Linux/Windows semantic target and storage controls passed on the Linux host")
