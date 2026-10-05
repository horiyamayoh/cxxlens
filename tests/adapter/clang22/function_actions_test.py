"""Actual compiler action/subject oracle, independent from provider inventories."""
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
using size_type = decltype(sizeof(0));
void* operator new(size_type);
void* operator new[](size_type);
void* operator new(size_type, void*);
void operator delete(void*) noexcept;
void operator delete[](void*) noexcept;
void operator delete(void*, void*) noexcept;
struct Resource { Resource(); Resource(const Resource&); ~Resource(); };
void api(int);
volatile int global;
void cleanup_int(int*);
void actions(bool branch, void* storage) {
 Resource first;
 Resource second;
 if (branch) return;
 Resource{};
 auto* allocated = new Resource;
 delete allocated;
 auto* array = new Resource[2];
 delete[] array;
 auto* placement = new (storage) Resource;
 placement->~Resource();
 int local __attribute__((cleanup(cleanup_int))) = 1;
 int copied = global;
 global = copied;
 ++global;
 int* indirect = &local;
 copied = *indirect;
 __atomic_store_n(indirect, copied, __ATOMIC_SEQ_CST);
 api(copied);
}
void indirect(void (*pointer)(int), int value) { pointer(value); }
void unevaluated() { (void)sizeof(global); (void)noexcept(api(global)); using T = decltype(global); }
void captures(int parameter) {
 int local = parameter;
 auto lambda = [local, &parameter]() { return local + parameter; };
 (void)lambda;
}
void bodyless(int value = (api(1), 0));
void unnamed(int, const int*, int = 7);
int status();
struct Defaults { int field = (status(), 1); Defaults() {} };
void use_initializer() { Defaults object; (void)object; }
void primitive() { asm volatile("" ::: "memory"); }
void flow_values() { int handle=status(); handle=status(); int used=handle; (void)used; }
void use_default() { bodyless(); }
int main() { return 0; }
'''

def values(row):
    return {key.removeprefix("output."): value.get("value") for key, value in row["values"].items()}

def elements(encoded):
    raw = bytes.fromhex(encoded)
    result = []
    while raw:
        size = int.from_bytes(raw[:4], "little")
        result.append(raw[4:4 + size].decode())
        raw = raw[4 + size:]
    assert result == sorted(set(result))
    return result

with tempfile.TemporaryDirectory(prefix="cxxlens-actions-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source)
    (root / "compile_commands.json").write_text(json.dumps([{
        "directory": str(root), "file": "main.cpp", "arguments": [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]
    }]))
    run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")],
                         env=environment, text=True, capture_output=True, timeout=240)
    assert run.returncode == 0, run.stderr
    bundle = json.loads(run.stdout)
    scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]: query["result"] for query in bundle["queries"]}
    rows = {name: [values(row) for row in scan["rows"]] for name, scan in scans.items()}
    outcomes = rows["build.compile_unit_analysis.v1"]
    assert outcomes and all(row["semantic_output"] == "produced" for row in outcomes), outcomes
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    nodes = {row["node"]: row for row in rows["cc.cfg_node.v1"]}
    operations = rows["cc.operation.v1"]
    by_scope = {}
    for row in operations:
        assert row["profile"] == "clang22-function-compiler-actions/1"
        assert row["scope_declaration"] in declarations
        assert declarations[row["scope_declaration"]]["entity"] == row["function"]
        if row["origin"] == "cfg":
            assert row["node"] in nodes and nodes[row["node"]]["body"] == row["body"]
            assert row["element_index"] < nodes[row["node"]]["element_count"]
            assert row["program_point"] is None and row["program_point_profile"] is None
        if row["expression"]:
            assert row["expression"] in syntax
        by_scope.setdefault(row["scope_declaration"], []).append(row)
    for detail in rows["cc.entity_detail.v1"]:
        if detail["operation_profile"] is None:
            continue
        declaration, = [row for row in declarations.values() if row["entity"] == detail["entity"] and row["source"] == detail["source"]]
        population = by_scope.get(declaration["declaration"], [])
        assert detail["operation_count"] == len(population), (detail, population)
        assert set(elements(detail["operation_ids"])) == {row["operation"] for row in population}
    # Every actual AST admission survives independently from an operation census.
    # Written field initializers have a declaration owner, and acquire a function
    # execution owner only through the separately observed CFG context.
    admitted = {(row["node"], flag.removeprefix("operation_")) for row in syntax.values() if row["function"]
                for flag in elements(row["flags"]) if flag.startswith("operation_")}
    observed = {(row["expression"], row["kind"]) for row in operations if row["origin"] == "ast"}
    assert admitted <= observed, admitted - observed
    unnamed = [row for row in operations if entities.get(row["function"], {}).get("qualified_name") == "unnamed"
               and row["type_use_kind"] == "parameter"]
    assert {row["subject_index"] for row in unnamed} == {0, 1, 2}, unnamed
    assert all(row["object_type"] and row["source"] and row["site_state"] == "complete" for row in unnamed), unnamed
    assert {row["subject_index"]: row["parameter_has_default_argument"] for row in unnamed} == {0: False, 1: False, 2: True}, unnamed
    actions = [row for row in operations if entities.get(row["function"], {}).get("qualified_name") == "actions"]
    assert {"construction", "allocation", "initialization_failure_deallocation", "deallocation", "destruction", "cleanup_function", "load", "store", "update", "atomic", "type_use", "invocation"} <= {row["kind"] for row in actions}
    # Compiler CFG views reuse the same original new-expression site, without byte-span joins.
    allocations = [row for row in actions if row["kind"] == "allocation"]
    assert len({row["site"] for row in allocations}) == 3
    for expression in {row["expression"] for row in allocations}:
        views = [row for row in allocations if row["expression"] == expression]
        assert {row["origin"] for row in views} == {"ast", "cfg"}
        assert len({row["site"] for row in views}) == 1
    # The two objects are distinct, while repeated exit destruction of each is one static site.
    automatic = [row for row in actions if row["element_kind"] == "automatic_object_dtor"]
    assert len({row["object_declaration"] for row in automatic}) == 2
    for declaration in {row["object_declaration"] for row in automatic}:
        assert len({row["site"] for row in automatic if row["object_declaration"] == declaration}) == 1
    for node in nodes.values():
        implicit = [row for row in operations if row["node"] == node["node"] and row["element_kind"] in {
            "constructor", "new_allocator", "automatic_object_dtor", "delete_dtor", "member_dtor", "base_dtor", "temporary_dtor", "cleanup_function"}]
        assert node["implicit_operation_count"] == len(implicit), (node, implicit)
    unevaluated = [row for row in operations if entities.get(row["function"], {}).get("qualified_name") == "unevaluated" and row["kind"] in {"invocation", "load"}]
    assert unevaluated and all(row["evaluation"] == "unevaluated" for row in unevaluated)
    reads = [row for row in rows["cc.entity_edge.v1"] if row["kind"] == "reads" and entities.get(row["source_entity"], {}).get("qualified_name") == "unevaluated"]
    assert not reads, reads
    indirect, = [row for row in operations if row["origin"] == "ast" and row["kind"] == "invocation" and entities.get(row["function"], {}).get("qualified_name") == "indirect"]
    assert indirect["site_state"] == "complete" and indirect["target"] is None
    captures = [row for row in operations if row["type_use_kind"] == "capture"]
    assert {row["subject_index"] for row in captures} == {0, 1}
    assert len({row["site"] for row in captures}) == 2
    main, = [row for row in rows["cc.entity_detail.v1"] if entities[row["entity"]]["qualified_name"] == "main"]
    assert {"main_entry", "hosted_environment"} <= set(elements(main["flags"]))
    flow_values = [row for row in rows["cc.flow_fact.v1"] if entities.get(row["function"], {}).get("qualified_name") == "flow_values"]
    bound = [row for row in flow_values if row["kind"] == "definition" and row["value_expression"] and syntax[row["value_expression"]]["kind"] == "CallExpr"]
    assert len(bound) == 2, bound
    result_nodes = {row["expression"] for row in rows["cc.call_operand.v1"] if row["kind"] == "result"}
    for fact in bound:
        assert fact["expression_state"] == "complete" and fact["value_expression"] in result_nodes, fact
        assert fact["cfg_binding_state"] == "complete" and fact["element_kind"] == "statement", fact
        assert fact["element_index"] < nodes[fact["node"]]["element_count"]
    defaults = [row for row in operations if entities.get(row["function"], {}).get("qualified_name") == "use_default" and row["expression_context"] == "default_argument"]
    # Clang omits activated function default arguments from its CFG. The original
    # bodyless declaration trace remains, and the execution scope must be partial.
    assert all(declarations[row["context_declaration"]]["kind"] == "ParmVar" for row in defaults), defaults
    use_default, = [row for row in rows["cc.entity_detail.v1"] if entities.get(row["entity"], {}).get("qualified_name") == "use_default"]
    assert use_default["operation_state"] == "partial", use_default
    assert any(row["kind"] == "invocation" and entities.get(row["function"], {}).get("qualified_name") == "bodyless" for row in operations)
    initializers = [row for row in operations if row["expression_context"] == "default_initializer"]
    assert initializers and all(declarations[row["context_declaration"]]["kind"] == "Field" for row in initializers), initializers
    primitive, = [row for row in rows["cc.entity_detail.v1"] if entities.get(row["entity"], {}).get("qualified_name") == "primitive"]
    assert primitive["operation_state"] == "partial", primitive
    global_details = [row for row in rows["cc.entity_detail.v1"] if entities.get(row["entity"], {}).get("qualified_name") == "global"]
    assert global_details and all({"finite_variable_storage_v1", "global_storage", "static_storage", "file_context_storage", "volatile_qualified"} <= set(elements(row["flags"])) for row in global_details)
print("actual compiler action/subject oracle passed")
