#!/usr/bin/env python3
"""Authority-correct U2a1c mapped-result transformation.

Use the exact U2a1b-derived implementation, correct the coordinator bridge and moved-receipt
comparison, then narrow post-map observations to the existing source-private typed native
attachment identity.  Callers cannot synthesize four unrelated opaque values as authority.
"""

from __future__ import annotations

from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V1 = Path(__file__).with_name("u2a1c_apply_source.py")
source = V1.read_text(encoding="utf-8")

simple_replacements = [
    (
        'text = edit_class(text, "sqlite_shm_mapping_lease_state", lease_state_decl)',
        'text = edit_class(text, "sqlite_same_process_shm_mapping_lease_coordinator", lease_state_decl)',
    ),
    (
        'qualified_control->mapping != prepared_terminal_receipt.mapping()',
        'qualified_control->mapping != map_attempt->receipt->mapping()',
    ),
    (
        'qualified_control->capability.matches_effect_identity(\n\t\t\t\t\t\t\t\tprepared_terminal_receipt.zero_resize_effect_receipt()) ||',
        'qualified_control->capability.matches_effect_identity(\n\t\t\t\t\t\t\t\tmap_attempt->receipt->zero_resize_effect_receipt()) ||',
    ),
    (
        'qualified_control->observed_attachment !=\n\t\t\t\t\t\t\t\tprepared_terminal_receipt.observed_attachment()',
        'qualified_control->observed_attachment !=\n\t\t\t\t\t\t\t\tmap_attempt->receipt->observed_attachment()',
    ),
]
for old, new in simple_replacements:
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"expected one v1 driver occurrence for {old!r}, found {count}")
    source = source.replace(old, new, 1)

exec(compile(source, str(V1), "exec"), {"__name__": "__main__", "__file__": str(V1)})


def replace_exact(rel: str, old: str, new: str, expected: int) -> None:
    path = ROOT / rel
    text = path.read_text(encoding="utf-8")
    count = text.count(old)
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} occurrences, found {count}: {old!r}")
    path.write_text(text.replace(old, new), encoding="utf-8")


raw_signature = (
    "sqlite_backend_opaque_identity observed_shm_object_receipt,\n"
    "\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,\n"
    "\t\t\tsqlite_backend_opaque_identity observed_device_receipt,\n"
    "\t\t\tsqlite_backend_opaque_identity observed_mount_receipt"
)
typed_signature = "sqlite_shm_reader_native_attachment_identity observed_attachment"
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp",
    raw_signature,
    typed_signature,
    2,
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp",
    raw_signature,
    typed_signature,
    1,
)

raw_signature_cpp = (
    "sqlite_backend_opaque_identity observed_shm_object_receipt,\n"
    "\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,\n"
    "\t\tsqlite_backend_opaque_identity observed_device_receipt,\n"
    "\t\tsqlite_backend_opaque_identity observed_mount_receipt"
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp",
    raw_signature_cpp,
    "sqlite_shm_reader_native_attachment_identity observed_attachment",
    1,
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp",
    raw_signature_cpp,
    "sqlite_shm_reader_native_attachment_identity observed_attachment",
    1,
)

raw_signature_state = (
    "sqlite_backend_opaque_identity observed_shm_object_receipt,\n"
    "\t\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,\n"
    "\t\t\t\tsqlite_backend_opaque_identity observed_device_receipt,\n"
    "\t\t\t\tsqlite_backend_opaque_identity observed_mount_receipt"
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp",
    raw_signature_state,
    "sqlite_shm_reader_native_attachment_identity observed_attachment",
    1,
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp",
    raw_signature_state,
    "sqlite_shm_reader_native_attachment_identity observed_attachment",
    1,
)

raw_forward = (
    "std::move(observed_shm_object_receipt),\n"
    "\t\t\tstd::move(observed_shm_entry_receipt),\n"
    "\t\t\tstd::move(observed_device_receipt),\n"
    "\t\t\tstd::move(observed_mount_receipt)"
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp",
    raw_forward,
    "std::move(observed_attachment)",
    1,
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp",
    raw_forward,
    "std::move(observed_attachment)",
    1,
)

raw_forward_state = (
    "std::move(observed_shm_object_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_shm_entry_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_device_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_mount_receipt)"
)
replace_exact(
    "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp",
    raw_forward_state,
    "std::move(observed_attachment)",
    1,
)

lease_cpp = ROOT / "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
text = lease_cpp.read_text(encoding="utf-8")
raw_validity = (
    "valid_identity(observed_shm_object_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_shm_entry_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_device_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_mount_receipt);"
)
typed_validity = (
    "valid_observed_reader_native_attachment(observed_attachment) &&\n"
    "\t\t\t\t\t\tobserved_attachment.expected() == map->request.expected_attachment;"
)
count = text.count(raw_validity)
if count != 1:
    raise RuntimeError(f"lease mapped validity: expected one raw block, found {count}")
text = text.replace(raw_validity, typed_validity, 1)
raw_observed = (
    "auto observed = sqlite_shm_reader_native_attachment_identity{\n"
    "\t\t\t\t\t\trequest.expected_attachment,\n"
    "\t\t\t\t\t\tstd::move(observed_shm_object_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_shm_entry_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_device_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_mount_receipt)};"
)
count = text.count(raw_observed)
if count != 1:
    raise RuntimeError(f"lease mapped observation construction: expected one block, found {count}")
text = text.replace(raw_observed, "auto observed = std::move(observed_attachment);", 1)
lease_cpp.write_text(text, encoding="utf-8")

# Add the typed coordinator-to-state delegation omitted by the original driver.
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
			sqlite_shm_reader_native_attachment_identity observed_attachment) noexcept
	{
		return state_->validate_reader_mapped_attachment_effect(family,
			inflight,
			std::move(capability),
			native_status,
			native_mapping,
			delegated_extend,
			std::move(observed_attachment));
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

# Focused test obtains one source-private typed observation from the existing test peer, then
# presents that exact value to the closed validator.
test_path = ROOT / "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
text = test_path.read_text(encoding="utf-8")
needle = (
    "\t\t\t\tconst auto exact_mapping = mapping(setup.writer_attempt.native_page.get());\n"
    "\t\t\t\tauto receipt = sqlite_same_process_shm_reader_receipt_validator::validate("
)
replacement = (
    "\t\t\t\tconst auto exact_mapping = mapping(setup.writer_attempt.native_page.get());\n"
    "\t\t\t\tconst auto observed_receipt =\n"
    "\t\t\t\t\tsqlite_same_process_shm_lease_test_peer::reader_attachment_map(\n"
    "\t\t\t\t\t\trequest, setup.holder.generation(), exact_mapping, effect->identity());\n"
    "\t\t\t\tauto receipt = sqlite_same_process_shm_reader_receipt_validator::validate("
)
count = text.count(needle)
if count != 1:
    raise RuntimeError(f"focused mapped observation insertion: expected one location, found {count}")
text = text.replace(needle, replacement, 1)
raw_test_args = (
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-shm-object\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-shm-entry\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-device\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-mount\", 78U));"
)
count = text.count(raw_test_args)
if count != 1:
    raise RuntimeError(f"focused mapped raw args: expected one block, found {count}")
text = text.replace(
    raw_test_args,
    "\t\t\t\t\tobserved_receipt.observed_attachment());",
    1,
)
test_path.write_text(text, encoding="utf-8")

print("U2a1c authority-correct typed mapped-result transformation applied")
