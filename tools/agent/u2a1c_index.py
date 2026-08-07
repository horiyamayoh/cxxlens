#!/usr/bin/env python3
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
TERMS = (
    "mapped_result",
    "SQLITE_READONLY",
    "SQLITE_OK/non-null",
    "sqlite_readonly_status",
    "readonly_cantinit",
    "exact_predecessor_mapped_route",
)
ALLOWED = {".cpp", ".hpp", ".md", ".yaml", ".yml", ".json"}
for path in sorted(ROOT.rglob("*")):
    if not path.is_file() or path.suffix not in ALLOWED or ".git" in path.parts:
        continue
    try:
        lines = path.read_text(encoding="utf-8").splitlines()
    except UnicodeDecodeError:
        continue
    hits = []
    for index, line in enumerate(lines, start=1):
        if any(term in line for term in TERMS):
            hits.append((index, line.strip()))
    if not hits:
        continue
    print(f"[{path.relative_to(ROOT)}]")
    for index, line in hits[:40]:
        print(f"  {index}: {line}")
