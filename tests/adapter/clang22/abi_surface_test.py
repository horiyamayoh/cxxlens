"""Observe actual storage/call ABI through the ordinary public native query path."""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(name, None)
source = r"""
struct Plain { char c; int i; };
struct RenamedPlain { char renamed_c; int renamed_i; };
struct __attribute__((packed)) Packed { char c; int i; };
struct Empty {};
struct Ebo : Empty { char c; };
struct Overlap { [[no_unique_address]] Empty e; int i; };
struct EmptyMarkers { int i; [[no_unique_address]] Empty e, e2; };
struct Bits { unsigned a:3; unsigned:0; unsigned b:4; };
union Union { char c; int i; };
struct Reference { long double& r; };
struct VirtualBase { virtual int f(int); virtual ~VirtualBase(); int i; };
struct VirtualDerived : virtual VirtualBase { VirtualDerived(); ~VirtualDerived(); char c; };
struct Tail { int i; char c; ~Tail(); };
struct Reused : Tail { char c; };
struct AtomicMember { Tail t; char c; };
struct Forward;
struct DefinedLater;
struct DefinedLater { int i; };
template<class T> struct Dependent { T t; };
Plain call(Plain, int);
RenamedPlain renamed_call(RenamedPlain, int);
int scalar(int);
int __attribute__((ms_abi)) alternate(int);
int main() { return 0; }
"""


def run(root):
    result = subprocess.run(
        [str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")],
        env=environment, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stderr
    return json.loads(result.stdout)


def values(row):
    return {name.removeprefix("output."): value.get("value")
            for name, value in row["values"].items()}


def ranges(value):
    assert value is not None
    raw = bytes.fromhex(value)
    assert len(raw) % 16 == 0
    return list(struct.iter_unpack("<QQ", raw))


with tempfile.TemporaryDirectory(prefix="cxxlens-abi-") as temporary:
    root = Path(temporary)
    (root / "main.cpp").write_text(source, encoding="utf-8")
    variants = [[], ["-DUNRELATED=1", "-O2"], ["-fpack-struct=1"]]
    commands = [{"directory": str(root), "file": "main.cpp",
                 "arguments": [str(compiler), "-std=c++23", *options, "-c", "main.cpp", "-o", f"main-{index}.o"]}
                for index, options in enumerate(variants)]
    (root / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
    bundle = run(root)
    scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]: query["result"]
             for query in bundle["queries"]}
    assert "cc.abi_surface.v1" in scans
    assert scans["cc.abi_surface.v1"]["status"] == "complete"
    entities = {(value["entity"], row["condition_universe"], row["condition_fragments"][0], row["interpretation"]): value
                for row in scans["cc.entity.v1"]["rows"] for value in [values(row)]}
    by_name = {}
    for row in scans["cc.abi_surface.v1"]["rows"]:
        value = values(row)
        entity = entities[(value["entity"], row["condition_universe"], row["condition_fragments"][0], row["interpretation"])]
        name = entity["qualified_name"]
        assert row["claim_contributors"] and row["contributor_edges"]
        assert value["profile"] == "clang22-storage-and-call-interface/1"
        if value["abi_state"] == "complete":
            assert value["abi_context"] and value["abi_fingerprint"] and value["abi_signature"]
            signature = bytes.fromhex(value["abi_signature"])
            assert b"main.cpp" not in signature and b"Plain" not in signature
        by_name.setdefault(name, []).append(value)
    for name in ("Plain", "Packed", "Empty", "Ebo", "Overlap", "EmptyMarkers", "Bits", "Union", "Reference", "VirtualBase", "VirtualDerived", "Reused", "AtomicMember"):
        assert len(by_name[name]) == 3
        assert all(value["layout_state"] == "complete" and value["abi_state"] == "complete" for value in by_name[name]), name
    plain = by_name["Plain"]
    normal = [value for value in plain if value["byte_size"] == 8]
    packed = [value for value in plain if value["byte_size"] == 5]
    assert len(normal) == 2 and len(packed) == 1
    assert ranges(normal[0]["occupied_ranges"]) == [(0, 1), (4, 8)]
    assert ranges(packed[0]["occupied_ranges"]) == [(0, 5)]
    assert normal[0]["abi_context"] == normal[1]["abi_context"]
    assert normal[0]["abi_fingerprint"] == normal[1]["abi_fingerprint"]
    assert normal[0]["abi_fingerprint"] != packed[0]["abi_fingerprint"]
    assert {value["abi_fingerprint"] for value in by_name["Plain"]} == {
        value["abi_fingerprint"] for value in by_name["RenamedPlain"]}
    assert {value["abi_fingerprint"] for value in by_name["call"]} == {
        value["abi_fingerprint"] for value in by_name["renamed_call"]}
    assert len({value["abi_fingerprint"] for value in by_name["call"]}) == 2
    for name in ("call", "scalar", "alternate", "VirtualDerived::VirtualDerived", "VirtualDerived::~VirtualDerived"):
        assert all(value["abi_state"] == "complete" and value["layout_state"] == "unknown"
                   and value["byte_size"] is None for value in by_name[name]), name
    assert {value["abi_fingerprint"] for value in by_name["scalar"]}.isdisjoint({
        value["abi_fingerprint"] for value in by_name["alternate"]})
    assert all(value["abi_state"] == "unknown" and value["layout_state"] == "unknown"
               for value in by_name["Forward"])
    later = by_name["DefinedLater"]
    assert len(later) == 6
    assert sum(value["abi_state"] == "complete" for value in later) == 3
    assert sum(value["abi_state"] == "unknown" for value in later) == 3
    assert all(value["abi_state"] == "unknown" for value in by_name["Dependent"])
    # The producer's canonical ABI/storage does not depend on compile-job order.
    (root / "compile_commands.json").write_text(json.dumps(list(reversed(commands))), encoding="utf-8")
    reordered = run(root)
    assert bundle == reordered
