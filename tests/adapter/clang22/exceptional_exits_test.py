"""Original lowering census and public SDK projection on actual compiler input."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile

analyzer, compiler, projector = map(Path, sys.argv[1:4])
replay = len(sys.argv) == 5 and sys.argv[4] == "--replay"
assert len(sys.argv) == 4 or replay, "unexpected fixture arguments"
environment = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(name, None)
source = r'''
void risky();
void safe() noexcept;
struct Copy { Copy(const Copy&); ~Copy() noexcept; };
struct Written { Written() { risky(); } ~Written() { safe(); } };
void empty() {}
void escaping_call() { risky(); }
void safe_call() { safe(); }
void explicit_throw() { throw 1; }
void throwing_copy(Copy& value) { throw value; }
void nonthrowing_owner() noexcept { risky(); }
void default_target(int = (risky(), 1));
void activated_default() { default_target(); }
struct Receiver {
 void acquire() noexcept {}
 void release() noexcept {}
 void use() noexcept {}
 bool try_acquire() noexcept { return true; }
};
void interfere(Receiver&) noexcept;
void owned_receiver(Receiver receiver) { receiver.acquire(); receiver.release(); receiver.release(); }
void use_after_release(Receiver receiver) { receiver.acquire(); receiver.release(); receiver.use(); }
void other_receiver(Receiver receiver, Receiver other) { receiver.acquire(); other.acquire(); receiver.release(); other.release(); receiver.release(); }
void unknown_interference(Receiver receiver) { receiver.acquire(); receiver.release(); interfere(receiver); receiver.release(); }
void conditional_receiver(Receiver receiver) { if (receiver.try_acquire()) { receiver.release(); receiver.release(); } }
void consume(const char*);
void path_probe() { consume("a/b"); }
struct Raw { int field; };
void raw_sink(const void*);
void raw_probe(Raw& object) { raw_sink(&object); }
struct Guard { Guard(); ~Guard() noexcept(false); };
struct Safe { Safe(); ~Safe() noexcept; };
Guard make();
void local_cleanup() { Guard first; risky(); }
void two_cleanups() { Guard first; Guard second; make(); risky(); }
void normal_cleanup() { Safe value; }
void true_spec() noexcept(sizeof(int)>0);
void false_spec() noexcept(false);
template<class T> void dependent_spec() noexcept(T::value);
using Alias = void() noexcept; Alias alias_spec;
int plain_char_probe(char value) { return value; }
int signed_char_control(signed char value) { return value; }
int unsigned_char_control(unsigned char value) { return value; }
'''


def values(row):
    return {name.removeprefix("output."): cell.get("value") for name, cell in row["values"].items()}


def elements(encoded):
    raw = bytes.fromhex(encoded)
    result = []
    while raw:
        assert len(raw) >= 4
        size = int.from_bytes(raw[:4], "little")
        assert size <= len(raw) - 4
        result.append(raw[4:4 + size].decode())
        raw = raw[4 + size:]
    assert result == sorted(set(result))
    return result


retained_directory = environment.get("CXXLENS_NATIVE_TEST_ARTIFACT_DIR")
assert not replay or retained_directory, "replay requires the original retained input directory"
root = Path(retained_directory) if retained_directory else Path(tempfile.mkdtemp(prefix="cxxlens-exceptional-exits-"))
root.mkdir(parents=True, exist_ok=True)
success = False
try:
    if not replay:
        (root / "main.cpp").write_text(source)
        (root / "compile_commands.json").write_text(json.dumps([{
            "directory": str(root), "file": "main.cpp", "arguments": [str(compiler), "-std=c++23", "-fexceptions", "-fcxx-exceptions", "-O0", "-c", "main.cpp", "-o", "main.o"]
        }]))
        run = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")],
                             env=environment, text=True, capture_output=True, timeout=240)
        (root / "analyzer.stderr").write_text(run.stderr)
        (root / "queries.json").write_text(run.stdout)
        assert run.returncode == 0, run.stderr
    bundle = json.loads((root / "queries.json").read_text())
    scans = {item["logical_ir"]["relation_requirements"][0]["descriptor_id"]: item["result"] for item in bundle["queries"]}
    rows = {name: [values(row) for row in scan["rows"]] for name, scan in scans.items()}
    assert all(row["semantic_output"] == "produced" for row in rows["build.compile_unit_analysis.v1"]), rows["build.compile_unit_analysis.v1"]
    entities = {row["entity"]: row for row in rows["cc.entity.v1"]}
    details = {row["detail"]: row for row in rows["cc.entity_detail.v1"]}
    bodies = {row["body"]: row for row in rows["cc.body.v1"]}
    declarations = {row["declaration"]: row for row in rows["cc.declaration.v1"]}
    exits = {row["exit"]: row for row in rows["cc.exceptional_exit.v1"]}
    assert exits, "actual lowering population is absent"
    by_detail = {}
    for row in exits.values():
        assert row["profile"] == "clang22-original-exceptional-occurrences/1"
        assert row["lowering_profile"] == "clang22-written-definition-analysis-lowering/1"
        original = details[row["scope_detail"]]
        assert original["entity"] == row["function"] and original["source"] == row["definition_source"]
        if row["body"]:
            assert bodies[row["body"]]["function"] == row["function"]
        by_detail.setdefault(row["scope_detail"], []).append(row)
        if row["ordinal"] == 0:
            assert row["role"] == "lowering_variant" and row["eligibility"] == "excluded" and row["variant"] is None
        else:
            variant = exits[row["variant"]]
            for axis in ("compile_unit", "scope_detail", "function", "definition_source", "variant_kind", "variant_index", "variant_symbol"):
                assert row[axis] == variant[axis], (axis, row, variant)
    for detail, population in by_detail.items():
        original = details[detail]
        expected = {row["exit"] for row in population}
        assert original["exceptional_exit_count"] == len(expected)
        assert set(elements(original["exceptional_exit_ids"])) == expected
        for body in {row["body"] for row in population if row["body"]}:
            assert bodies[body]["exceptional_exit_count"] == len(expected)
            assert set(elements(bodies[body]["exceptional_exit_ids"])) == expected
    # Stored semantic exception specifications stay independent of lowering.
    specifications = {}
    for row in details.values():
        if row["exception_spec_profile"]:
            specifications.setdefault(entities[row["entity"]]["qualified_name"], []).append(row)
    for name, kind, nonthrowing in (("risky", "none", False), ("safe", "basic_noexcept", True),
                                  ("true_spec", "noexcept_true", True), ("false_spec", "noexcept_false", False),
                                  ("alias_spec", "basic_noexcept", True)):
        assert specifications[name]
        assert all(row["exception_spec_profile"] == "clang22-function-exception-specification/1"
                   and row["exception_spec_state"] == "complete"
                   and row["exception_spec_kind"] == kind
                   and row["exception_spec_nonthrowing"] is nonthrowing for row in specifications[name]), specifications[name]
    assert all(row["exception_spec_state"] == "partial" and row["exception_spec_nonthrowing"] is None
               for row in specifications["dependent_spec"]), specifications["dependent_spec"]
    cleanup_objects = set()
    cleanup_routes = set()
    runtime_helpers = 0
    for row in exits.values():
        if row["cleanup_profile"] is None:
            continue
        assert row["cleanup_profile"] == "clang22-destroy-object-cleanup-emission/1"
        assert row["cleanup_emission_ordinal"] > 0
        if row["cleanup_declaration"] is None:
            assert row["cleanup_registration_ordinal"] is None
            continue
        original = declarations[row["cleanup_declaration"]]
        unit_declarations = set()
        for inventory in rows["cc.declaration_inventory.v1"]:
            if inventory["compile_unit"] == row["compile_unit"]:
                unit_declarations.update(elements(inventory["declarations"]))
        assert original["declaration"] in unit_declarations
        assert row["cleanup_registration_ordinal"] > 0
        if not row["emitter_methods"] or not row["emitter_methods"] & 1:
            assert row["cleanup_target_usr"] is None and row["cleanup_target_dtor_type"] is None
            runtime_helpers += 1
            continue
        assert row["cleanup_target_profile"] == "clang22-destructor-emission-target/1"
        assert row["cleanup_target_usr"] is not None and row["cleanup_target_dtor_type"] is not None
        # Optional target entity binding is not replaced by the generic target.
        if row["cleanup_target"] is not None:
            assert entities[row["cleanup_target"]]["kind"] == "destructor"
        cleanup_objects.add(row["cleanup_declaration"])
        cleanup_routes.add(row["cleanup_route"])
    assert len(cleanup_objects) >= 4 and cleanup_routes == {"normal", "exceptional"}
    assert runtime_helpers > 0
    by_name = {}
    for row in exits.values():
        by_name.setdefault(entities[row["function"]]["qualified_name"], []).append(row)
    for name, count in {"empty": 0, "safe_call": 0, "escaping_call": 1, "explicit_throw": 1, "nonthrowing_owner": 1}.items():
        population = by_name[name]
        assert sum(row["eligibility"] == "eligible" for row in population) == count, (name, population)
        assert all(row["observation_state"] == "complete" for row in population), (name, population)
    copy = by_name["throwing_copy"]
    assert sum(row["role"] == "written_throw" for row in copy) == 1
    assert any(row["target_usr"] and row["emitter_methods"] is not None and row["emitter_methods"] & 1 and row["role"] != "lowering_helper" for row in copy), copy
    assert any(row["role"] == "unhandled_resume" and row["eligibility"] == "eligible" for row in copy), copy
    assert any(row["role"] == "lowering_helper" and row["eligibility"] == "excluded" for row in copy), copy
    assert all(details[row["scope_detail"]]["exceptional_exit_state"] == "partial" for row in by_name["activated_default"])
    projected = subprocess.run([str(projector), str(root / "queries.json"), "--exceptional-exits"], text=True, capture_output=True, timeout=60)
    (root / "projection.stdout").write_text(projected.stdout)
    (root / "projection.stderr").write_text(projected.stderr)
    assert projected.returncode == 0, projected.stderr
    populations = {}
    variants = {}
    for line in projected.stdout.splitlines():
        kind, function, *fields = line.split()
        if kind == "population":
            populations[function] = tuple(map(int, fields))
        elif kind == "variant":
            variants.setdefault(function, []).append(tuple(map(int, fields)))
    for name, count in {"empty": 0, "safe_call": 0, "escaping_call": 1, "explicit_throw": 1, "nonthrowing_owner": 1}.items():
        function = by_name[name][0]["function"]
        assert populations[function][:2] == (0, 0), (name, populations[function], projected.stdout)
        assert variants[function] and all(state == 0 and eligible == count for state, eligible in variants[function]), (name, variants.get(function))
    function = by_name["activated_default"][0]["function"]
    assert populations[function][0] != 0, populations[function]
    success = True
    print("actual original lowering census, throw-copy/helper distinction and public SDK projection passed")
finally:
    if success and not retained_directory:
        shutil.rmtree(root)
    elif not success:
        print(f"original failing inputs and outputs retained: {root}", file=sys.stderr)
