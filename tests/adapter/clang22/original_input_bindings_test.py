"""Actual target admission, storage, dispatch, identifier, literal and CFG joins."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile

analyzer, compiler = map(Path, sys.argv[1:3])
projection = Path(sys.argv[3])
environment = dict(os.environ)
for variable in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(variable, None)

def cells(row):
    return {name.removeprefix("output."): value.get("value") for name, value in row["values"].items()}

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

source = r'''
#include "helper.hpp"
struct Base { virtual int read() { return 3; } };
struct Derived final : Base { int read() override { return 4; } };
int state;
int calls(Base& base, Derived& derived) {
 base.read(); base.Base::read(); derived.read();
 return helper();
}
int storage(int input) {
 const int fixed_value = 5;
 alignas(32) int User_value = 42;
 int* pointer = &User_value;
 int& reference = User_value;
 static int retained = 0;
 thread_local int thread_value = 0;
 state = User_value;
 if (User_value < 9) User_value = input;
 long expanded = User_value;
 return reference + fixed_value + retained + thread_value + *pointer + int(expanded);
}
void only_unevaluated() { (void)sizeof(state); (void)noexcept(helper()); }
int default_value(int value = 19) { return value; }
int use_default() { return default_value(); }
struct Holder { int field; Holder(): field(11) {} };
struct __attribute__((packed)) Packed { char c; int value; };
#pragma pack(push, 2)
struct PackedPragma { char c; int value; };
#pragma pack(pop)
#pragma GCC diagnostic push
#pragma GCC diagnostic pop
int portability_values() {
 (void)"A\\B\n\0" "C";
 (void)u8"雪"; (void)u"Ω"; (void)L"W";
 return sizeof(void*) == sizeof(long);
}
template<class T> concept Integral = __is_integral(T);
template<class T> requires Integral<T> T plus(T value) { return value + 1; }
int template_values() {
 int value = 2;
 auto captured = [value](int input) { return plus(input) + value; };
 return captured(3);
}
template<class T> auto choose_candidate(T value) -> decltype(value.member()) {
 return value.member();
}
int choose_candidate(...) { return 0; }
int failed_candidate() { return choose_candidate(1); }
constexpr int event_leaf(int value) { return value + 1; }
constexpr int event_branch(bool selected) {
 return selected ? event_leaf(7) : event_leaf(99);
}
constexpr int evaluated_constant = event_branch(true);
int runtime_constant() { return event_leaf(1); }
struct Receiver {
 void acquire() noexcept {}
 void release() noexcept {}
};
void owned_receiver(Receiver receiver) {
 receiver.acquire(); receiver.release(); receiver.release();
}
int main() { return 0; }
'''

with tempfile.TemporaryDirectory(prefix="cxxlens-original-bindings-") as directory:
    root = Path(directory)
    (root / "main.cpp").write_text(source)
    (root / "helper.hpp").write_text("inline int helper() noexcept { return 7; }\n")
    (root / "compile_commands.json").write_text(json.dumps([{
        "directory": str(root), "file": "main.cpp", "arguments":
        [str(compiler), "-std=c++23", "-c", "main.cpp", "-o", "main.o"]}]))
    run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands",
                          str(root / "compile_commands.json")],
                         env=environment, text=True, capture_output=True, timeout=240)
    debug = Path(os.environ.get("CXXLENS_INPUT_DEBUG_DIR", directory))
    debug.mkdir(parents=True, exist_ok=True)
    (debug / "analyzer.stdout.json").write_text(run.stdout)
    (debug / "analyzer.stderr.log").write_text(run.stderr)
    (debug / "main.cpp").write_text(source)
    (debug / "helper.hpp").write_text("inline int helper() noexcept { return 7; }\n")
    assert run.returncode == 0, run.stderr
    bundle = json.loads(run.stdout)
    rows = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]:
            [cells(row) for row in query["result"]["rows"]] for query in bundle["queries"]}
    outcomes = rows["build.compile_unit_analysis.v1"]
    assert outcomes and all(row["semantic_output"] == "produced" for row in outcomes), (outcomes, run.stderr)
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    by_name = {row.get("qualified_name"): row for row in entities.values()}
    assert by_name["Holder::field"]["semantic_owner"] == by_name["Holder"]["entity"]
    assert by_name["storage"]["semantic_owner"] is None
    assert any(row.get("kind") == "parameter" and row.get("qualified_name") == "input"
               and row.get("semantic_owner") == by_name["storage"]["entity"]
               for row in entities.values()), entities
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    syntax = {row["node"]: row for row in rows["cc.syntax_node.v1"]}
    bodies = {entities[row["function"]]["qualified_name"]: row for row in rows["cc.body.v1"]}
    slots = rows["cc.target_resolution_slot.v1"]
    assert {row["domain"] for row in slots} == {"callable", "state_access", "ownership", "inheritance", "override", "nominal_type", "include"}, slots
    inventory, = rows["cc.declaration_inventory.v1"]
    assert inventory["target_slot_profile"] == "clang22-original-consumer-target-relations/1"
    # The newly observed uninstantiated template pattern has no completed CFG
    # action domain. Its full target census must remain partial even though all
    # retained original target slots are independently source/identity complete.
    assert inventory["target_slot_state"] == "partial", (inventory["target_slot_state"], run.stderr)
    # The pattern and its actual int instantiation share a display name. Bind the
    # original primary subject and physical definition rather than choosing a row
    # through hash-dependent serialization order.
    subjects = rows["cc.template_subject.v1"]
    primary_plus, = [row for row in subjects if row["kind"] == "primary" and
                     row["entity"] in entities and entities[row["entity"]]["qualified_name"] == "plus"]
    plus_pattern, = [row for row in rows["cc.body.v1"] if row["function"] == primary_plus["entity"]]
    assert declarations[plus_pattern["function_exit_declaration"]]["entity"] == primary_plus["entity"]
    assert plus_pattern["eligibility"] == "dependent" and plus_pattern["reason"] == "instantiate-required"
    assert plus_pattern["operation_state"] == "partial"
    assert all(row["observation_state"] == "complete" for row in slots), [
        {key: row[key] for key in ("subject_kind", "domain", "slot_index", "reason")}
        for row in slots if row["observation_state"] != "complete"]
    assert inventory["target_slot_count"] == len(slots)
    assert set(members(inventory["target_slot_ids"])) == {row["slot"] for row in slots}
    derived_declaration, = [row for row in declarations.values()
                            if row["entity"] == by_name["Derived"]["entity"]]
    derived_types = [row for row in slots if row["domain"] == "nominal_type" and
                     row["declaration"] == derived_declaration["declaration"]]
    assert {row["target_entity"] for row in derived_types} == {
        by_name["Base"]["entity"], by_name["Derived"]["entity"]}, derived_types
    assert len({row["slot_index"] for row in derived_types}) == 2, derived_types
    lambda_owners = [row for row in entities.values() if row["kind"] == "class" and
                     row["semantic_owner"] == by_name["template_values"]["entity"]]
    lambda_owner, = lambda_owners
    assert any(row["domain"] == "ownership" and row["subject_kind"] == "CXXMethod" and
               row["owner"] == lambda_owner["entity"] and row["observation_state"] == "complete"
               for row in slots), lambda_owner
    assert all(row["is_system"] is False for row in slots)
    assert any(row["eligibility"] == "excluded" and row["resolution"] == "not_applicable" for row in slots if row["domain"] == "nominal_type")
    include, = [row for row in slots if row["domain"] == "include"]
    assert include["resolution"] == "resolved" and include["include"] and include["target_file"]
    calls = [row for row in rows["cc.call_site.v1"]
             if entities.get(row["caller"], {}).get("qualified_name") == "calls"]
    assert calls and all(row["dispatch_profile"] == "clang22-original-call-dispatch/1" for row in calls), calls
    assert sum(row["dispatch_kind"] == "virtual" for row in calls) == 1, calls
    assert sum(row["dispatch_kind"] == "direct" for row in calls) == 3, calls
    # A reference parameter's declared type differs from its receiver expression
    # type. Retain both without contradicting the original syntax observation.
    types = {row["type"]: row for row in rows["cc.type.v1"]}
    reference_calls = [row for row in syntax.values()
                       if row.get("object_fact_kind") == "direct_member_lifetime" and
                       entities[row["function"]]["qualified_name"] == "calls"]
    assert len(reference_calls) == 3, reference_calls
    for call in reference_calls:
        receiver = syntax[call["object_receiver"]]
        declared = types[call["declared_object_type"]]
        expression = types[receiver["canonical_type"]]
        assert declared["constructor"] == "lvalue_reference", declared
        assert expression["constructor"] == "record", expression
        pointee, = [row for row in rows["cc.type_component.v1"]
                    if row["owner_type"] == declared["type"] and row["role"] == "pointee"]
        assert pointee["component_type"] == expression["type"], (declared, expression)
        assert declarations[receiver["object_declaration"]]["entity"] == receiver["object_entity"]
    virtual, = [row for row in calls if row["dispatch_kind"] == "virtual"]
    assert virtual["candidate_presence"] == "present" and virtual["candidate_state"] == "complete", virtual
    assert virtual["candidate_count"] == 2 and len(members(virtual["candidate_targets"])) == 2
    storage = bodies["storage"]
    assert storage["automatic_storage_state"] == "complete" and storage["automatic_storage_count"] == 4, storage
    storage_ids = set(members(storage["automatic_storage_ids"]))
    footprint = [row for row in rows["cc.operation.v1"] if row["operation"] in storage_ids]
    assert len(footprint) == 4 and all(row["storage_state"] == "complete" and row["storage_abi_context"] and row["storage_target_triple"] for row in footprint), footprint
    aligned, = [row for row in footprint if entities[row["object_entity"]]["qualified_name"] == "User_value"]
    assert aligned["storage_size_bytes"] == 4 and aligned["storage_alignment_bytes"] == 32, aligned
    name, = [row for row in declarations.values() if row["identifier_name"] == "User_value"]
    assert name["identifier_state"] == "complete" and name["identifier_function"] == storage["function"] and name["identifier_source"]
    literals = [syntax[node] for node in members(storage["literal_ids"])]
    assert storage["literal_state"] == "complete" and storage["literal_count"] == len(literals), storage
    assert all(row["kind"] in {"IntegerLiteral", "FloatingLiteral", "StringLiteral"} for row in literals)
    constant, = [row for row in literals if row["integer_value"] == "5"]
    assert constant["literal_context"] == "const_initializer" and constant["literal_declaration"] in declarations, constant
    assert bodies["use_default"]["literal_state"] == "partial", bodies["use_default"]
    constructor = bodies["Holder::Holder"]
    assert constructor["literal_count"] == 1 and constructor["literal_state"] == "complete", constructor
    for row in syntax.values():
        child_ids = members(row["child_ids"])
        if row["child_state"] == "complete":
            assert row["child_count"] == len(child_ids) and all(node in syntax for node in child_ids)
        if row["cast_kind"]:
            assert row["operand"] in syntax and row["operand_type"] and row["canonical_type"] and row["conversion_state"] == "complete", row
    widened = [row for row in syntax.values() if row["cast_kind"] == "IntegralCast" and row["value_preservation"] == "preserves"]
    assert widened, list(syntax.values())
    conditional = [row for row in rows["cc.cfg_edge.v1"] if entities[row["function"]]["qualified_name"] == "storage" and row["kind"] in {"true", "false"}]
    assert conditional and all(row["condition_state"] == "complete" and row["condition"] in syntax for row in conditional), conditional
    assert {row["kind"] for row in conditional} == {"true", "false"}
    receiver_function = by_name["owned_receiver"]["entity"]
    receiver_parameter, = [row for row in entities.values()
                           if row["kind"] == "parameter" and row["semantic_owner"] == receiver_function]
    receiver_detail, = [row for row in rows["cc.entity_detail.v1"]
                        if row["entity"] == receiver_parameter["entity"]]
    receiver_flags = set(members(receiver_detail["flags"]))
    assert {"finite_variable_storage_v1", "storage_parameter", "automatic_storage"} <= receiver_flags
    assert "storage_non_object" not in receiver_flags
    receiver_type, = [row for row in rows["cc.type.v1"]
                      if row["type"] == receiver_detail["canonical_type"]]
    assert receiver_type["constructor"] == "record" and receiver_type["nominal_entity"] == by_name["Receiver"]["entity"]
    receiver_calls = {row["call"] for row in rows["cc.call_site.v1"]
                      if row["caller"] == receiver_function}
    receiver_operands = [row for row in rows["cc.call_operand.v1"]
                         if row["call"] in receiver_calls and row["kind"] == "receiver"]
    assert len(receiver_calls) == len(receiver_operands) == 3
    assert all(row["referenced_entity"] == receiver_parameter["entity"] and
               row["observation_state"] == "complete" and row["expression"] in syntax
               for row in receiver_operands), receiver_operands
    receiver_cfg = [row for row in rows["cc.operation.v1"]
                    if row["function"] == receiver_function and row["origin"] == "cfg" and
                    row["kind"] == "invocation"]
    assert len(receiver_cfg) == 3 and all(row["node"] and row["element_index"] is not None and
                                         row["element_kind"] == "statement" and row["site_state"] == "complete"
                                         for row in receiver_cfg), receiver_cfg
    receiver_targets = {row["target"] for row in rows["cc.call_direct_target.v1"]
                        if row["call"] in receiver_calls}
    assert receiver_targets == {by_name["Receiver::acquire"]["entity"], by_name["Receiver::release"]["entity"]}
    assert all({"noexcept_classified", "noexcept"} <= set(members(row["flags"]))
               for row in rows["cc.entity_detail.v1"] if row["entity"] in receiver_targets)
    simple_returns = [row for row in rows["cc.cfg_edge.v1"] if entities[row["function"]]["qualified_name"] == "main" and row["outcome"] == "normal_return"]
    assert simple_returns and all(row["outcome_expression"] in syntax and syntax[row["outcome_expression"]]["kind"] == "ReturnStmt" for row in simple_returns), simple_returns

    # Compiler-decoded strings preserve escaped characters and embedded NUL;
    # source spelling is a separate original field.
    strings = [row for row in syntax.values() if row["kind"] == "StringLiteral"]
    assert strings and all(row["literal_value_state"] == "complete" and
                           row["literal_value_profile"] == "clang22-original-string-literal-value/1"
                           for row in strings), strings
    ordinary, = [row for row in strings if row["literal_encoding"] == "ordinary"]
    assert bytes.fromhex(ordinary["literal_value_bytes"]) == b"A\\B\n\0C", ordinary
    utf8, = [row for row in strings if row["literal_encoding"] == "utf8"]
    assert bytes.fromhex(utf8["literal_value_bytes"]) == "雪".encode() and utf8["literal_element_width_bits"] == 8, utf8
    assert any(row["literal_encoding"] == "utf16" and row["literal_element_width_bits"] == 16 for row in strings)
    assert any(row["literal_encoding"] == "wide" for row in strings)
    surfaces = rows["cc.abi_surface.v1"]
    assert surfaces and all(row["target_data_model_state"] == "complete" and
                            row["target_data_model_profile"] == "clang22-original-target-data-model/1"
                            and row["long_width_bits"] > 0 and row["pointer_width_bits"] > 0
                            and row["wchar_width_bits"] > 0 and row["byte_order"] in {"little", "big"}
                            and isinstance(row["plain_char_signed"], bool)
                            for row in surfaces), surfaces
    packed, = [row for row in surfaces if row["kind"] == "record" and
               row["entity"] == by_name["Packed"]["entity"]]
    assert packed["packing_state"] == "complete" and packed["packed_attribute"] is True and packed["packing_applied"] is True
    pragma, = [row for row in surfaces if row["kind"] == "record" and
               row["entity"] == by_name["PackedPragma"]["entity"]]
    assert pragma["packed_attribute"] is False and pragma["maximum_field_alignment_bits"] == 16 and pragma["packing_applied"] is True
    plain, = [row for row in surfaces if row["kind"] == "record" and
              row["entity"] == by_name["Holder"]["entity"]]
    assert plain["packing_state"] == "complete" and plain["packed_attribute"] is False and plain["maximum_field_alignment_bits"] == 0 and plain["packing_applied"] is False
    assert all(row["packing_state"] is None for row in surfaces if row["kind"] == "function")
    pragmas = [row for row in rows["source.preprocessor_event.v1"] if row["kind"] == "raw_pragma"]
    assert {row["pragma_kind"] for row in pragmas} == {"pack", "diagnostic"}, pragmas
    assert all(row["pragma_state"] == "complete" and row["pragma_profile"] == "clang22-original-raw-pragma-kind/1" for row in pragmas)
    template_inventory, = rows["cc.template_inventory.v1"]
    subjects = rows["cc.template_subject.v1"]
    captures = rows["cc.lambda_capture.v1"]
    constraints = rows["cc.constraint_node.v1"]
    frames = rows["cc.template_instantiation_frame.v1"]
    assert subjects and captures and constraints and frames, template_inventory
    for observed in subjects:
        if observed["entity"] is not None:
            assert bytes.fromhex(entities[observed["entity"]]["provider_local_key"]) == b"clang-usr:" + bytes.fromhex(observed["semantic_usr"])
    actual_instance, = [row for row in subjects if row["kind"] == "implicit_instance"]
    assert actual_instance["entity"] in entities and entities[actual_instance["entity"]]["qualified_name"] == "plus"
    assert actual_instance["entity"] != primary_plus["entity"]
    plus_instance, = [row for row in rows["cc.body.v1"] if row["function"] == actual_instance["entity"]]
    assert declarations[plus_instance["function_exit_declaration"]]["entity"] == actual_instance["entity"]
    assert plus_instance["function_exit_declaration"] != plus_pattern["function_exit_declaration"]
    assert plus_instance["eligibility"] == "closed"
    assert any(row["kind"] == "primary" and row["entity"] == by_name["Integral"]["entity"] for row in subjects)
    actual_lambda, = [row for row in subjects if row["kind"] == "lambda"]
    assert actual_lambda["entity"] == lambda_owner["entity"]
    assert actual_lambda["owner_entity"] == by_name["template_values"]["entity"]
    for kind, members_rows in (("subject", subjects), ("capture", captures), ("constraint", constraints), ("frame", frames)):
        assert template_inventory[kind + "_count"] == len(members_rows), (kind, template_inventory)
        assert set(members(template_inventory[kind + "_ids"])) == {row[{"subject":"subject", "capture":"capture", "constraint":"node", "frame":"frame"}[kind]] for row in members_rows}
    projected = subprocess.run([str(projection), str(debug / "analyzer.stdout.json"), *sys.argv[4:]],
                               env=environment, text=True, capture_output=True, timeout=60)
    assert projected.returncode == 0, (projected.stdout, projected.stderr)
