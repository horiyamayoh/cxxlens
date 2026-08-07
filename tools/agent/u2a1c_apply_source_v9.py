#!/usr/bin/env python3
"""Hardened wrapper for the authority-complete U2a1c transformation."""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V8 = Path(__file__).with_name("u2a1c_apply_source_v8.py")
source = V8.read_text(encoding="utf-8")
old = '''text = insert_before_once(
    text,
    "\\tsqlite_shm_reader_native_attachment_identity::sqlite_shm_reader_native_attachment_identity(",
    observation_impl,
    "mapped native observation implementation",
)'''
new = '''if observation_impl not in text:
    implementation_index = text.find("\\tsqlite_shm_reader_native_attachment_identity::")
    if implementation_index < 0:
        raise RuntimeError("mapped native observation implementation: native identity methods not found")
    text = text[:implementation_index] + observation_impl + text[implementation_index:]'''
count = source.count(old)
if count != 1:
    raise RuntimeError(f"expected one brittle native-identity insertion in v8, found {count}")
source = source.replace(old, new, 1)
exec(compile(source, str(V8), "exec"), {"__name__": "__main__", "__file__": str(V8)})

# Compile-time closure of the new source receipt surface.
test_path = ROOT / "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
text = test_path.read_text(encoding="utf-8")
assertions = r'''
	static_assert(!std::is_copy_constructible_v<sqlite_shm_reader_mapped_native_observation>);
	static_assert(!std::is_copy_assignable_v<sqlite_shm_reader_mapped_native_observation>);
	static_assert(std::is_nothrow_move_constructible_v<
		sqlite_shm_reader_mapped_native_observation>);
	static_assert(!std::is_constructible_v<sqlite_shm_reader_mapped_native_observation,
		sqlite_shm_reader_attachment_reservation_identity,
		int,
		const volatile void*,
		int,
		sqlite_backend_opaque_identity,
		sqlite_backend_opaque_identity,
		sqlite_backend_opaque_identity,
		sqlite_backend_opaque_identity>);

'''
marker = "\tvoid verify_callback_free_reader_identity_prepare_claim_bind_and_registry_collision()"
if assertions not in text:
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"mapped observation assertions: expected one insertion point, found {count}")
    text = text.replace(marker, assertions + marker, 1)
    test_path.write_text(text, encoding="utf-8")

print("U2a1c hardened authority-complete transformation applied")
