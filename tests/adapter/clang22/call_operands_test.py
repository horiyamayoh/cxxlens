"""Independent actual Clang call-slot and function-declaration population oracle."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(name, None)
source = r'''
int default_value() { return 7; }
int target(int left, int right = default_value());
int target(int left, int right) { return left + right; }
void consume();
struct Box {
 Box(int);
 int member(int);
 static int static_member(int);
 int operator()(int);
 static int operator[](int);
 int explicit_member(this Box&, int);
 ~Box() = default;
};
int operator+(const Box&, int);
int direct(int value) { return target(value); }
int member(Box& box, int value) { return box.member(value); }
int static_member(Box& box, int value) { return box.static_member(value); }
int member_operator(Box& box, int value) { return box(value); }
int static_operator(Box& box, int value) { return box[value]; }
int free_operator(Box& box, int value) { return box + value; }
int explicit_object(Box& box, int value) { return box.explicit_member(value); }
int indirect(int (*pointer)(int), int value) { return pointer(value); }
int indirect_member(Box& box, int (Box::*pointer)(int), int value) { return (box.*pointer)(value); }
Box construction(int value) { return Box(value); }
void zero_arguments() { consume(); }
int no_calls() { return 0; }
int unevaluated(int value) {
 (void)sizeof(target(value));
 (void)noexcept(target(value));
 using T = decltype(target(value));
 (void)requires { target(value); };
 return 0;
}
int nested(int value) {
 auto lambda = [capture = target(value)]() { return target(capture); };
 return value;
}
#define TWICE(value) (target(value) + target(value))
int macro(int value) { return TWICE(value); }
int references(int value, int* pointer) {
 target(12); target(value + 1); target(*pointer); target(&value != pointer); return 0;
}
template<class T> auto dependent(T value) { return value(value); }
'''


def values(row):
    return {name.removeprefix("output."): cell.get("value") for name, cell in row["values"].items()}


def elements(value):
    raw = bytes.fromhex(value)
    result = []
    while raw:
        assert len(raw) >= 4
        length = int.from_bytes(raw[:4], "little")
        assert len(raw) >= length + 4
        result.append(raw[4:4 + length].decode())
        raw = raw[4 + length:]
    assert result == sorted(set(result))
    return result


with tempfile.TemporaryDirectory(prefix="cxxlens-call-operands-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source, encoding="utf8")
    (root / "compile_commands.json").write_text(json.dumps([{
        "directory": str(root), "file": "main.cpp", "arguments": [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]
    }]), encoding="utf8")
    result = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")],
                            env=environment, text=True, capture_output=True, timeout=240)
    assert result.returncode == 0, result.stderr
    bundle = json.loads(result.stdout)
    scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]: query["result"] for query in bundle["queries"]}
    rows = {name: [values(row) for row in scan["rows"]] for name, scan in scans.items()}
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    spans = {row["span"]: row for row in rows["source.span.v1"]}
    nodes = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    sites = {row["call"]: row for row in rows["cc.call_site.v1"]}
    targets = {row["call"]: row for row in rows["cc.call_direct_target.v1"]}
    types = {row["type"]: row for row in rows["cc.type.v1"]}
    operands = {}
    for row in rows["cc.call_operand.v1"]:
        operands.setdefault(row["call"], []).append(row)
    by_caller = {}
    for row in sites.values():
        if row["caller"]:
            by_caller.setdefault(entities[row["caller"]]["qualified_name"], []).append(row)
        assert row["operand_profile"] == "clang22-actual-call-operands/1"
        assert row["operand_population_state"] == "complete", row
        population = operands.get(row["call"], [])
        assert row["operand_count"] == len(population)
        arguments = [item for item in population if item["kind"] == "argument"]
        assert row["argument_count"] == len(arguments)
        assert sorted(item["index"] for item in arguments) == list(range(len(arguments)))
        assert all(item["index"] == 0 for item in population if item["kind"] != "argument")
        assert row["expression"] in nodes and nodes[row["expression"]]["compile_unit"] == row["compile_unit"], (entities.get(row["caller"], {}).get("qualified_name"), row)
        assert {"finite_call_admission_v1", "admitted_call_site"} <= set(elements(nodes[row["expression"]]["flags"]))
    # Syntax membership is observed independently of original normalized call rows.
    admitted_nodes = {row["node"] for row in nodes.values() if "admitted_call_site" in elements(row["flags"])}
    assert admitted_nodes == {row["expression"] for row in sites.values()}
    expected = {"default_value": 0, "direct": 1, "member": 1, "static_member": 1,
                "member_operator": 1, "static_operator": 1, "free_operator": 1,
                "explicit_object": 1, "indirect": 1, "indirect_member": 1,
                "construction": 1, "zero_arguments": 1, "no_calls": 0,
                "unevaluated": 4, "nested": 1, "macro": 2, "references": 4, "dependent": 1}
    details = rows["cc.entity_detail.v1"]
    for name, count in expected.items():
        actual = [row for row in details if entities[row["entity"]]["qualified_name"] == name]
        assert len(actual) == 1, (name, actual)
        detail = actual[0]
        assert detail["call_site_profile"] == "clang22-function-emitted-call-sites/1"
        assert detail["call_site_state"] == "complete", (name, detail)
        assert detail["call_site_count"] == count, (name, detail["call_site_count"], count)
        assert set(elements(detail["call_site_ids"])) == {row["call"] for row in by_caller.get(name, [])}
        assert {"finite_function_body_v1", "body_written"} <= set(elements(detail["flags"]))
    # Same canonical target entity has two distinct actual declaration scopes.
    target_details = [row for row in details if entities[row["entity"]]["qualified_name"] == "target"]
    assert sorted(row["call_site_count"] for row in target_details) == [0, 1]
    assert {"body_absent", "body_written"} <= set().union(*(set(elements(row["flags"])) for row in target_details))
    assert all(row["call_site_state"] == "complete" for row in target_details)
    assert scans["cc.call_site.v1"]["status"] == "complete", scans["cc.call_site.v1"]["status"]
    assert scans["cc.call_site.v1"]["inputs_complete"], scans["cc.call_site.v1"]["unresolved"]
    # Original executable resource coverage stays partial independently of the
    # closed syntactic call/operand population. It is retained on its body carrier.
    assert not scans["cc.body.v1"]["inputs_complete"]
    assert any(item["state"] == "unresolved" and
               item["reason"].split(":", 1)[0] == "resource.activation-domain-frontier"
               for item in scans["cc.body.v1"]["input_coverage"])
    assert scans["cc.call_operand.v1"]["status"] == "complete"
    if not scans["cc.call_operand.v1"]["inputs_complete"]:
        print("original operand input frontiers:", sorted({
            (item["domain"], item["state"], item["reason"])
            for item in scans["cc.call_operand.v1"]["input_coverage"]
            if item["state"] != "covered"}))
    assert not any(item["reason"].split(":", 1)[0] in {
        "resource.activation-domain-frontier", "object.facet-binding-frontier"}
        for name in ("cc.call_site.v1", "cc.call_operand.v1")
        for item in scans[name]["input_coverage"])
    assert not scans["cc.call_direct_target.v1"]["inputs_complete"]
    assert any(row["state"] == "unresolved" and row["reason"].startswith("provider.indirect-target-unresolved:")
               for row in scans["cc.call_direct_target.v1"]["input_coverage"])
    for name in ("indirect", "indirect_member", "dependent"):
        assert by_caller[name][0]["call"] not in targets
    for name in ("member", "member_operator", "explicit_object", "indirect_member"):
        population = operands[by_caller[name][0]["call"]]
        assert len([row for row in population if row["kind"] == "receiver"]) == 1
        assert len([row for row in population if row["kind"] == "argument"]) == 1
    for name in ("static_member", "static_operator", "free_operator", "construction"):
        population = operands[by_caller[name][0]["call"]]
        assert not any(row["kind"] == "receiver" for row in population), (name, population)
    static_argument, = [row for row in operands[by_caller["static_operator"][0]["call"]] if row["kind"] == "argument"]
    assert (static_argument["actual_argument_index"], static_argument["formal_parameter_index"], static_argument["written_argument_index"]) == (1, 0, 0)
    direct_call = by_caller["direct"][0]
    default, = [row for row in operands[direct_call["call"]] if row["default_argument"]]
    default_span = spans[default["source"]]
    assert source.encode()[default_span["begin"]:default_span["end"]] == b"default_value()"
    assert default["origin"] == "default_argument" and default["implicit"]
    assert default["index"] == default["actual_argument_index"] == default["formal_parameter_index"] == 1
    assert default["written_argument_index"] is None
    named, = [row for row in operands[direct_call["call"]] if row["kind"] == "argument" and row["index"] == 0]
    assert entities[named["referenced_entity"]]["qualified_name"] == "value"
    for call in by_caller["references"]:
        original, = [row for row in operands[call["call"]] if row["kind"] == "argument" and row["index"] == 0]
        assert original["referenced_entity"] is None
    for call in by_caller["unevaluated"]:
        assert all(row["evaluation_state"] == "unevaluated" for row in operands[call["call"]])
    zero = by_caller["zero_arguments"][0]
    assert zero["argument_count"] == zero["operand_count"] == 0 and not operands.get(zero["call"])
    constructor = by_caller["construction"][0]
    assert constructor["kind"] == "constructor"
    assert len([row for row in operands[constructor["call"]] if row["kind"] == "result"]) == 1
    # Explicit constructions are outside the preserved written direct CallExpr profile.
    body = next(row for row in rows["cc.body.v1"] if entities[row["function"]]["qualified_name"] == "construction")
    assert body["call_site_count"] == 1 and body["direct_call_count"] == 0
    assert body["direct_call_profile"] == "clang22-written-syntactic-direct-calls/1"
    for row in targets.values():
        if row["target_signature_state"] == "complete":
            target_type = types[row["target_canonical_type"]]
            assert row["target_canonical_type_digest"] == target_type["component_signature_digest"]
            assert row["target_canonical_type_profile"] == target_type["structure_profile"] == "clang22-structural-type/1"
            assert row["target_structural_signature_digest"] == entities[row["target"]]["structural_signature_digest"]
            assert row["target_usr"] == entities[row["target"]]["provider_local_key"]
            assert row["target_signature_profile"] == "clang22-original-target-signature/1"
    print("actual call operands and independent function scopes passed")
