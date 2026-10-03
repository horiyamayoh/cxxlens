"""Exercise the normal local application path with real compiler inputs."""

import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile


analyzer, compiler = map(Path, sys.argv[1:3])
environment = dict(os.environ)
for name in ("CPATH", "CPLUS_INCLUDE_PATH", "C_INCLUDE_PATH", "OBJC_INCLUDE_PATH"):
    environment.pop(name, None)


def run(root, *, source=None, success=True):
    arguments = [str(analyzer), "--project-root", str(root),
                 "--compile-commands", str(root / "compile_commands.json")]
    if source:
        arguments += ["--file", source]
    result = subprocess.run(arguments, env=environment, capture_output=True, text=True, timeout=180)
    if success:
        assert result.returncode == 0, result.stderr
        return json.loads(result.stdout)
    assert result.returncode != 0 and not result.stdout, result
    return result.stderr


def database(root, files, extra=()):
    entries = [{"directory": str(root), "file": file,
                "arguments": [str(compiler), "-std=c++23", "-I.", *extra, "-c", file, "-o", file + ".o"]}
               for file in files]
    (root / "compile_commands.json").write_text(json.dumps(entries), encoding="utf-8")


def scans(bundle):
    assert bundle["schema"] == "cxxlens.application-query-results.v1"
    queries = bundle["queries"]
    assert len(queries) == 12
    assert [q["relation_id"] for q in queries] == sorted({q["relation_id"] for q in queries})
    publications = set()
    for query in queries:
        result = query["result"]
        assert result["snapshot_id"] == bundle["snapshot_id"]
        assert result["status"] == "complete" and not result["closed"]
        assert not result["closure_ids"]
        assert result["summary_guarantee"]["approximation"] == (
            "unknown" if result["unresolved"] else "under_approximation")
        publications.add(result["publication_id"])
        for row in result["rows"]:
            assert row["claim_contributors"] and row["contributor_edges"]
    assert len(publications) == 1
    return {q["relation_id"]: q["result"] for q in queries}


with tempfile.TemporaryDirectory(prefix="cxxlens-application-") as temporary:
    root = Path(temporary) / "first"
    root.mkdir()
    (root / "math.hpp").write_text("inline int twice(int value) { return value * 2; }\n", encoding="utf-8")
    source = ('#include "math.hpp"\n'
              'int leaf(int value) { return twice(value); }\n'
              'int branch(int value) { return leaf(value) + twice(value); }\n'
              'int main() { return branch(3); }\n')
    (root / "main.cpp").write_text(source, encoding="utf-8")
    (root / "other.cpp").write_text("int other() { return 0; }\n", encoding="utf-8")
    database(root, ["main.cpp", "other.cpp"])
    assert "ambiguous-variants" in run(root, success=False)
    first = run(root, source="main.cpp")
    results = scans(first)
    entities = results["cc.entity.v1"]["rows"]
    assert {row["values"]["output.qualified_name"]["value"] for row in entities} == {"leaf", "branch", "main"}
    calls = results["cc.call_site.v1"]["rows"]
    targets = results["cc.call_direct_target.v1"]["rows"]
    assert len(calls) == len(targets) == 4
    assert {r["values"]["output.call"]["value"] for r in calls} == {r["values"]["output.call"]["value"] for r in targets}
    spans = {r["values"]["output.span"]["value"]: r["values"] for r in results["source.span.v1"]["rows"]}
    for row in calls:
        span = spans[row["values"]["output.source"]["value"]]
        text = source.encode()[span["output.begin"]["value"]:span["output.end"]["value"]].decode()
        assert "(" in text and ")" in text, text
    # Header definitions are outside this main-source tier. Retain their soft references.
    assert results["cc.call_direct_target.v1"]["unresolved"]
    assert not results["cc.call_direct_target.v1"]["inputs_complete"]

    relocated = Path(temporary) / "relocated"
    shutil.copytree(root, relocated)
    database(relocated, ["main.cpp", "other.cpp"])
    second = run(relocated, source="main.cpp")
    second_results = scans(second)
    # Semantic identity and public results do not depend on checkout placement.
    assert first["snapshot_id"] == second["snapshot_id"]
    for relation, result in results.items():
        assert result == second_results[relation], relation

    database(root, ["main.cpp", "main.cpp"])
    assert "ambiguous-variants" in run(root, source="main.cpp", success=False)
    database(root, ["main.cpp"], extra=("@extra.rsp",))
    assert "unsupported-option" in run(root, success=False)
    database(root, ["main.cpp"], extra=("--target=aarch64-linux-gnu",))
    assert "Linux x86_64 required" in run(root, success=False)
    database(root, ["main.cpp"])
    (root / "main.cpp").write_text("int broken( {\n", encoding="utf-8")
    assert run(root, success=False)
    (root / "compile_commands.json").write_text("{", encoding="utf-8")
    assert run(root, success=False)

assert subprocess.run([str(analyzer), "--help"], capture_output=True).returncode == 0
