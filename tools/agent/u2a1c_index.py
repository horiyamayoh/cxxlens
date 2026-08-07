#!/usr/bin/env python3
from pathlib import Path
ROOT = Path(__file__).resolve().parents[2]
TERMS = (
    "validate_zero_effect_identity_for_registry",
    "zero_effect_capability_is_current",
    "sqlite_shm_reader_zero_effect_identity_validation_capability",
)
for path in sorted((ROOT / "src").rglob("*")):
    if not path.is_file() or path.suffix not in {".cpp", ".hpp"}:
        continue
    lines = path.read_text(encoding="utf-8").splitlines()
    hits = [(i + 1, line.strip()) for i, line in enumerate(lines)
            if any(term in line for term in TERMS)]
    if hits:
        print(f"[{path.relative_to(ROOT)}]")
        for line_no, line in hits:
            print(f"  {line_no}: {line}")
