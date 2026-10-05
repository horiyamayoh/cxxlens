"""Exercise independent declaration/comment/include/flow populations with actual Clang inputs."""

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


def values(row):
    return {name.removeprefix("output."): cell.get("value") for name, cell in row["values"].items()}


def elements(value):
    raw = bytes.fromhex(value)
    result = []
    while raw:
        assert len(raw) >= 4
        size = int.from_bytes(raw[:4], "little")
        assert len(raw) >= size + 4
        result.append(raw[4:4 + size].decode())
        raw = raw[4 + size:]
    assert result == sorted(set(result))
    return result


source = r'''
#include "helper.hpp"
#if 0
#include "never.hpp"
// TODO inactive comment
#endif
const char* text = "// not a comment";
const char* raw_text = R"tag(/* not a comment */)tag";
// TODO actual line
/* actual block */
constexpr int constant(int x) noexcept { return x; }
consteval int immediate() { return 1; }
template<class T> struct Generic { T data; };
template<class T> struct Generic<T*> { T* data; };
template<> struct Generic<void> {};
extern template struct Generic<int>;
template struct Generic<double>;
template<class T> concept Small = sizeof(T) < 8;
int flow(int input) { int local = input; local = local + 1; return local; }
int empty() { return 0; }
int missing_summary(int value) { return header_fn() + value; }
namespace std { class type_info; }
struct Polymorphic { virtual ~Polymorphic() = default; };
int evaluated_typeid(Polymorphic& live, int ignored) {
 (void)typeid(live); (void)typeid(++ignored); return 0;
}
int evaluated_vla(int count) { return sizeof(int[++count]); }
int existing_vla(int count) { int array[++count]; return sizeof(array); }
int evaluated_flow(int live, int ignored, int* pointer) {
 (void)sizeof(++ignored);
 (void)noexcept(++ignored);
 using Unevaluated = decltype(++ignored);
 __typeof__(++ignored) other = 0;
 (void)requires { ++ignored; *pointer; header_fn(); };
 (void)__is_constructible(decltype(ignored), decltype(++ignored));
 int local = live;
 auto nested = [capture = live]() { int inner = capture; return inner; };
 if constexpr (false) { ++ignored; header_fn(); }
 return local + live;
}
namespace std {
template<class Promise = void> struct coroutine_handle {
 static coroutine_handle from_address(void*) noexcept { return {}; }
 operator coroutine_handle<void>() const noexcept { return {}; }
};
template<class Return, class... Args> struct coroutine_traits { using promise_type = typename Return::promise_type; };
}
struct Awaiter {
 bool await_ready() noexcept { return false; }
 void await_suspend(std::coroutine_handle<>) noexcept {}
 void await_resume() noexcept {}
};
struct Task { struct promise_type {
 Task get_return_object() { return {}; }
 Awaiter initial_suspend() { return {}; }
 Awaiter final_suspend() noexcept { return {}; }
 void return_void() noexcept {}
 void unhandled_exception() noexcept {}
}; };
Task coroutine() { co_await Awaiter{}; co_yield 1; co_return; }
'''.replace("Awaiter final_suspend() noexcept", "Awaiter yield_value(int) { return {}; }\n Awaiter final_suspend() noexcept")

with tempfile.TemporaryDirectory(prefix="cxxlens-finite-populations-") as temporary:
    root = Path(temporary)
    (root / "main.cpp").write_text(source, encoding="utf-8")
    helper = b"// header TODO\r\n/* header block */\r\n\r\nint header_fn();\r"
    (root / "helper.hpp").write_bytes(helper)
    (root / "spare.hpp").write_bytes(b"// unopened TODO\n")
    commands = [{"directory": str(root), "file": "main.cpp", "arguments": [str(compiler), "-std=c++23", "-I.", "-c", "main.cpp", "-o", "main.o"]}]
    (root / "compile_commands.json").write_text(json.dumps(commands), encoding="utf-8")
    result = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")], env=environment, capture_output=True, text=True, timeout=180)
    assert result.returncode == 0, result.stderr
    bundle = json.loads(result.stdout)
    scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]: query["result"] for query in bundle["queries"]}
    rows = {name: [values(row) for row in scan["rows"]] for name, scan in scans.items()}
    files = {row["file"]: row["logical_path"] for row in rows["source.file.v1"]}
    spans = {row["span"]: row for row in rows["source.span.v1"]}
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    declaration_inventory, = rows["cc.declaration_inventory.v1"]
    admitted = elements(declaration_inventory["declarations"])
    assert declaration_inventory["enumeration_state"] == "complete"
    assert declaration_inventory["declaration_count"] == len(admitted)
    assert all(identity in declarations for identity in admitted)
    parsed = {files[identity] for identity in elements(declaration_inventory["parsed_files"])}
    assert parsed == {"project://root/main.cpp", "project://root/helper.hpp"}, parsed
    assert "project://root/spare.hpp" not in files.values()
    details = rows["cc.entity_detail.v1"]
    by_name = {}
    for detail in details:
        name = entities[detail["entity"]]["qualified_name"]
        by_name.setdefault(name, []).append(set(elements(detail["flags"])))
    assert any({"finite_declarations_v1", "constexpr", "noexcept", "noexcept_classified"} <= flags for flags in by_name["constant"])
    assert any("consteval" in flags for flags in by_name["immediate"])
    assert any("template_primary" in flags for flags in by_name["Generic"])
    assert any("template_partial_specialization" in flags for flags in by_name["Generic"])
    assert any("template_full_specialization" in flags for flags in by_name["Generic"])
    assert any("concept" in flags for flags in by_name["Small"])
    comment_rows = {row["comment"]: row for row in rows["source.comment.v1"]}
    comment_inventories = {files[row["file"]]: row for row in rows["source.comment_inventory.v1"]}
    for path, inventory in comment_inventories.items():
        assert inventory["enumeration_state"] == "complete"
        ids = elements(inventory["comments"])
        assert inventory["comment_count"] == len(ids)
        assert inventory["comment_bytes"] == sum(comment_rows[identity]["byte_count"] for identity in ids)
        frozen = (root / path.removeprefix("project://root/")).read_bytes()
        for identity in ids:
            comment = comment_rows[identity]
            span = spans[comment["source"]]
            spelling = bytes.fromhex(comment["spelling_bytes"])
            assert spelling == frozen[span["begin"]:span["end"]]
            assert comment["byte_count"] == len(spelling)
    starts = [value[0] for value in struct.iter_unpack("<Q", bytes.fromhex(comment_inventories["project://root/helper.hpp"]["physical_line_starts"]))]
    assert starts == [0, 16, 36, 38]
    main_comments = [bytes.fromhex(comment_rows[identity]["spelling_bytes"]) for identity in elements(comment_inventories["project://root/main.cpp"]["comments"])]
    assert len(main_comments) == 3 and b"// TODO inactive comment" in main_comments
    include_inventories = {files[row["file"]]: row for row in rows["source.include_inventory.v1"]}
    assert include_inventories["project://root/main.cpp"]["enumeration_state"] == "complete"
    assert include_inventories["project://root/main.cpp"]["include_count"] == 1
    assert include_inventories["project://root/helper.hpp"]["enumeration_state"] == "complete" and include_inventories["project://root/helper.hpp"]["include_count"] == 0
    assert "project://root/spare.hpp" not in include_inventories
    facts = {row["fact"]: row for row in rows["cc.flow_fact.v1"]}
    nodes = {row["node"]: row for row in rows["cc.cfg_node.v1"]}
    inventories = {entities[row["function"]]["qualified_name"]: row for row in rows["cc.flow_inventory.v1"]}
    flow = inventories["flow"]
    assert flow["enumeration_state"] == "complete"
    admitted_flow = set(elements(flow["facts"]))
    assert flow["fact_count"] == len(admitted_flow)
    for identity in admitted_flow:
        fact = facts[identity]
        assert fact["compile_unit"] == flow["compile_unit"] and fact["function"] == flow["function"]
        node = nodes[fact["node"]]
        assert node["body"] == flow["body"] and fact["program_point"] <= node["element_count"]
        if fact["kind"] == "def_use":
            use = facts[fact["use_fact"]]
            assert use["kind"] == "use" and use["subject"] == fact["subject"] and use["node"] == fact["node"]
            assert all(facts[definition]["kind"] == "definition" and facts[definition]["subject"] == use["subject"] for definition in elements(fact["definition_facts"]))
    assert flow["program_point_count"] == len(elements(flow["reaching_points"])) == len(elements(flow["liveness_points"]))
    assert any(facts[identity]["kind"] == "frontier" for identity in elements(inventories["missing_summary"]["facts"]))
    assert inventories["missing_summary"]["enumeration_state"] == "complete" and inventories["missing_summary"]["summary_state"] == "unavailable"
    evaluated = inventories["evaluated_flow"]
    evaluated_facts = [facts[identity] for identity in elements(evaluated["facts"])]
    evaluated_uses = [fact for fact in evaluated_facts if fact["kind"] == "use"]
    use_names = [entities[fact["subject"]]["qualified_name"] for fact in evaluated_uses]
    assert sorted(use_names) == ["live", "live", "live", "local"], use_names
    assert not any(fact["kind"] in {"call", "dereference"} for fact in evaluated_facts)
    ignored_definitions = [fact for fact in evaluated_facts if fact["kind"] == "definition" and entities[fact["subject"]]["qualified_name"] == "ignored"]
    assert len(ignored_definitions) == 1, ignored_definitions
    assert not any(entities[fact["subject"]]["qualified_name"] in {"capture", "inner"} for fact in evaluated_facts)
    nested_uses = [fact for fact in facts.values() if fact["kind"] == "use" and entities[fact["subject"]]["qualified_name"] == "capture"]
    assert nested_uses and all(fact["function"] != evaluated["function"] for fact in nested_uses)
    typeid_facts = [facts[identity] for identity in elements(inventories["evaluated_typeid"]["facts"])]
    typeid_uses = [entities[fact["subject"]]["qualified_name"] for fact in typeid_facts if fact["kind"] == "use"]
    assert typeid_uses == ["live"], typeid_uses
    for name in ("evaluated_vla", "existing_vla"):
        vla_facts = [facts[identity] for identity in elements(inventories[name]["facts"])]
        vla_uses = [entities[fact["subject"]]["qualified_name"] for fact in vla_facts if fact["kind"] == "use"]
        assert vla_uses == ["count"], (name, vla_uses)
        assert sum(fact["kind"] == "definition" and entities[fact["subject"]]["qualified_name"] == "count" for fact in vla_facts) == 2
    coroutine_id = next(identity for identity, row in entities.items() if row["qualified_name"] == "coroutine")
    syntax = [row for row in rows["cc.syntax_node.v1"] if row["function"] == coroutine_id]
    suspend = [row for row in syntax if "coroutine_suspend_site" in elements(row["flags"])]
    assert len(suspend) >= 2
    assert all(spans[row["source"]]["end"] > spans[row["source"]]["begin"] for row in suspend)
    coroutine_body, = [row for row in rows["cc.body.v1"] if row["function"] == coroutine_id]
    if coroutine_body["ast_state"] == "complete":
        assert coroutine_body["ast_node_count"] == len(syntax)
    else:
        assert coroutine_body["ast_state"] == "partial"
        assert any(item["code"] == "body.ast-enumeration-frontier" and item["subject"] == coroutine_id for item in scans["cc.body.v1"]["unresolved"])
