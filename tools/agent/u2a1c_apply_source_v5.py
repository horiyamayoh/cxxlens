#!/usr/bin/env python3
"""Least-authority U2a1c mapped-result transformation.

Run the authority-correct typed-observation transformation, then remove constructor/access friends
that are no longer required after the typed receipt is passed into the lease state.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V4 = Path(__file__).with_name("u2a1c_apply_source_v4.py")
exec(compile(V4.read_text(encoding="utf-8"), str(V4), "exec"),
     {"__name__": "__main__", "__file__": str(V4)})

path = ROOT / "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"
text = path.read_text(encoding="utf-8")


def edit_class(source: str, class_name: str, old: str, new: str) -> str:
    marker = f"\tclass {class_name}"
    start = source.find(marker)
    if start < 0:
        raise RuntimeError(f"class not found: {class_name}")
    end = source.find("\n\t};", start)
    if end < 0:
        raise RuntimeError(f"class terminator not found: {class_name}")
    end += len("\n\t};")
    segment = source[start:end]
    count = segment.count(old)
    if count != 1:
        raise RuntimeError(f"{class_name}: expected one least-authority marker, found {count}")
    return source[:start] + segment.replace(old, new, 1) + source[end:]

text = edit_class(
    text,
    "sqlite_shm_reader_native_attachment_identity",
    "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
    "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
)
text = edit_class(
    text,
    "sqlite_shm_verified_reader_attachment_post_map_receipt",
    "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n\t\tfriend class detail::sqlite_shm_mapping_registry_state;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
    "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;",
)
path.write_text(text, encoding="utf-8")
print("U2a1c least-authority mapped-result transformation applied")
