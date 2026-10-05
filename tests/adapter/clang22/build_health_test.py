"""Original selected-unit parser outcomes remain available after semantic failure."""
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


def values(row):
    return {name.removeprefix("output."): cell.get("value") for name, cell in row["values"].items()}


def elements(value):
    raw = bytes.fromhex(value)
    result = []
    while raw:
        assert len(raw) >= 4
        length = int.from_bytes(raw[:4], "little")
        assert length <= len(raw) - 4
        result.append(raw[4:4 + length].decode())
        raw = raw[4 + length:]
    assert result == sorted(set(result))
    return result


with tempfile.TemporaryDirectory(prefix="cxxlens-build-health-") as directory:
    root = Path(directory)
    fixtures = {"good.cpp": "int good() { return 1; }\n",
                "recovery.cpp": "int recovery() { return missing_name; }\n",
                "preprocessor.cpp": '#error original preprocessing diagnostic\nint preprocessor() { return 2; }\n',
                "fatal.cpp": '#include "missing.hpp"\nint fatal() { return 0; }\n'}
    for name, content in fixtures.items():
        (root / name).write_text(content, encoding="utf8")

    def command(name, mode):
        return {"directory": str(root), "file": name,
                "arguments": [str(compiler), "-std=c++23", f"-DMODE={mode}", "-c", name, "-o", name + ".o"]}

    def analyze(entries):
        (root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf8")
        result = subprocess.run([str(analyzer), "--project-root", str(root), "--compile-commands", str(root / "compile_commands.json")],
                                env=environment, text=True, capture_output=True, timeout=240)
        assert result.returncode == 0, result.stderr
        bundle = json.loads(result.stdout)
        scans = {query["logical_ir"]["relation_requirements"][0]["descriptor_id"]: query["result"] for query in bundle["queries"]}
        rows = {name: [values(row) for row in scan["rows"]] for name, scan in scans.items()}
        units = {row["compile_unit"]: row for row in rows["build.compile_unit.v1"]}
        variants = {row["variant"]: row for row in rows["build.variant.v1"]}
        sources = {row["snapshot"]: row for row in rows["source.file.v1"]}
        outcomes = {row["compile_unit"]: row for row in rows["build.compile_unit_analysis.v1"]}
        inventories = rows["build.analysis_inventory.v1"]
        assert set(outcomes) == set(units)
        for unit, outcome in outcomes.items():
            original = units[unit]
            assert outcome["main_source"] == original["main_source"] in sources
            assert outcome["project"] == original["project"] == sources[outcome["main_source"]]["project"]
            assert outcome["profile"] == "clang22-selected-unit-analysis/1"
            assert outcome["parse_error_count"] >= outcome["fatal_error_count"]
            assert not original["variant"].startswith("unavailable-unit:")
        for row in inventories:
            assert row["profile"] == "clang22-selected-analysis-units/1"
            assert row["compile_unit_count"] == len(elements(row["compile_units"]))
            assert row["selected_variant_count"] == len(elements(row["selected_variant_ids"]))
            assert set(elements(row["selected_variant_ids"])) == set(variants)
            assert set(elements(row["compile_units"])) <= set(units)
        inventory_worlds = set()
        for row in scans["build.analysis_inventory.v1"]["rows"]:
            current = values(row)
            members = elements(current["compile_units"])
            assert len(row["condition_fragments"]) == 1, row
            variant = row["condition_fragments"][0]
            assert variant not in inventory_worlds, (variant, inventories)
            inventory_worlds.add(variant)
            assert set(members) == {unit for unit, original in units.items() if original["variant"] == variant}, (variant, members, units)
        assert inventory_worlds == set(variants), (inventory_worlds, variants)
        # Original outcomes carry the actual selected configuration condition.
        for row in scans["build.compile_unit_analysis.v1"]["rows"]:
            original = units[values(row)["compile_unit"]]
            assert row["condition_fragments"] == [original["variant"]]
        return rows, units, sources, outcomes, inventories

    entries = [command("good.cpp", 1), command("recovery.cpp", 1), command("preprocessor.cpp", 1), command("fatal.cpp", 1), command("fatal.cpp", 2)]
    rows, units, sources, outcomes, inventories = analyze(entries)
    # The original catalog defines variants from all actual compiler facets;
    # a MODE option alone does not define their identity.
    assert len(units) == 5, {"units": units, "inventories": inventories, "outcomes": outcomes}
    assert len(inventories) == len({row["variant"] for row in units.values()}) >= 2, {"units": units, "inventories": inventories}
    by_file = {}
    for unit, outcome in outcomes.items():
        by_file.setdefault(sources[units[unit]["main_source"]]["logical_path"].split("/")[-1], []).append(outcome)
    assert by_file["good.cpp"][0]["parse_outcome"] == "success"
    assert by_file["good.cpp"][0]["semantic_output"] == "produced"
    assert by_file["recovery.cpp"][0]["parse_outcome"] == "recovery"
    assert by_file["recovery.cpp"][0]["parse_error_count"] > 0
    assert by_file["recovery.cpp"][0]["fatal_error_count"] == 0
    assert by_file["recovery.cpp"][0]["semantic_output"] == "produced"
    assert by_file["preprocessor.cpp"][0]["parse_outcome"] == "recovery"
    assert by_file["preprocessor.cpp"][0]["parse_error_count"] > 0
    assert by_file["preprocessor.cpp"][0]["fatal_error_count"] == 0
    assert by_file["preprocessor.cpp"][0]["semantic_output"] == "produced"
    assert all(row["parse_outcome"] == "failed" and row["fatal_error_count"] > 0 and row["semantic_output"] == "not_produced"
               for row in by_file["fatal.cpp"])
    assert all(row["enumeration_state"] == row["selected_variant_state"] == "complete" for row in inventories)
    original_identity = {(sources[unit["main_source"]]["logical_path"], unit["variant"], unit["compile_unit"]) for unit in units.values()}
    _, repeated, repeated_sources, _, _ = analyze(list(reversed(entries)))
    assert original_identity == {(repeated_sources[unit["main_source"]]["logical_path"], unit["variant"], unit["compile_unit"]) for unit in repeated.values()}
    _, failed_units, _, failed_outcomes, failed_inventories = analyze([command("fatal.cpp", 1)])
    assert len(failed_units) == 1 and all(row["semantic_output"] == "not_produced" for row in failed_outcomes.values())
    assert failed_inventories[0]["enumeration_state"] == "complete"
    unavailable = command("good.cpp", 1)
    unavailable["arguments"].insert(1, "--no-such-cxxlens-option")
    _, actual, _, actual_outcomes, incomplete = analyze([command("good.cpp", 1), unavailable])
    assert len(actual) == len(actual_outcomes) == 1
    assert incomplete[0]["enumeration_state"] == incomplete[0]["selected_variant_state"] == "partial"
print("native selected-unit outcomes, actual configuration census, all-failed and preparation frontier passed")
