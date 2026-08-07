#!/usr/bin/env python3
"""Corrected U2a1c transformation driver.

The first workbench driver deliberately fails closed, but its header insertion targeted the
forward-declared lease state instead of the public coordinator bridge.  Execute the exact driver
with that target corrected, then add the missing coordinator-to-state delegation.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V1 = Path(__file__).with_name("u2a1c_apply_source.py")
source = V1.read_text(encoding="utf-8")
old = 'text = edit_class(text, "sqlite_shm_mapping_lease_state", lease_state_decl)'
new = 'text = edit_class(text, "sqlite_same_process_shm_mapping_lease_coordinator", lease_state_decl)'
if source.count(old) != 1:
    raise RuntimeError("expected one lease-state header target in the v1 driver")
source = source.replace(old, new, 1)
exec(compile(source, str(V1), "exec"), {"__name__": "__main__", "__file__": str(V1)})

lease_cpp = ROOT / "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
text = lease_cpp.read_text(encoding="utf-8")
wrapper = r'''
	sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
	sqlite_same_process_shm_mapping_lease_coordinator::
		validate_registry_reader_mapped_attachment_effect(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_mapped_effect_identity_validation_capability capability,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend,
			sqlite_backend_opaque_identity observed_shm_object_receipt,
			sqlite_backend_opaque_identity observed_shm_entry_receipt,
			sqlite_backend_opaque_identity observed_device_receipt,
			sqlite_backend_opaque_identity observed_mount_receipt) noexcept
	{
		return state_->validate_reader_mapped_attachment_effect(family,
			inflight,
			std::move(capability),
			native_status,
			native_mapping,
			delegated_extend,
			std::move(observed_shm_object_receipt),
			std::move(observed_shm_entry_receipt),
			std::move(observed_device_receipt),
			std::move(observed_mount_receipt));
	}

'''
marker = (
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n"
    "\tsqlite_same_process_shm_mapping_lease_coordinator::\n"
    "\t\tvalidate_registry_reader_zero_attachment_effect("
)
if wrapper not in text:
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"expected one zero-effect coordinator wrapper, found {count}")
    text = text.replace(marker, wrapper + marker, 1)
    lease_cpp.write_text(text, encoding="utf-8")

print("U2a1c corrected coordinator bridge applied")
