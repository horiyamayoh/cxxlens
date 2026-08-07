#!/usr/bin/env python3
"""Apply the authority-complete U2a1c mapped-result validator patch.

The mapped native observation is a distinct move-only, private-construction source receipt.  The
closed validator consumes that receipt only after exact registry/owner/effect authentication and
then promotes it to the retained native attachment identity.  The retained identity is never an
input to its own validator.
"""

from __future__ import annotations

import re
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
V1 = Path(__file__).with_name("u2a1c_apply_source.py")
source = V1.read_text(encoding="utf-8")

# Correct the declaration target and compare the terminal proof against the receipt after it has
# moved into the map record.
replacements = [
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
for old, new in replacements:
    count = source.count(old)
    if count != 1:
        raise RuntimeError(f"expected one base-driver occurrence for {old!r}, found {count}")
    source = source.replace(old, new, 1)
exec(compile(source, str(V1), "exec"), {"__name__": "__main__", "__file__": str(V1)})


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_text(text, encoding="utf-8")


def insert_before_once(text: str, marker: str, insertion: str, label: str) -> str:
    if insertion in text:
        return text
    count = text.count(marker)
    if count != 1:
        raise RuntimeError(f"{label}: expected one marker, found {count}")
    return text.replace(marker, insertion + marker, 1)


def edit_class(text: str, class_name: str, transform) -> str:
    marker = f"\tclass {class_name}"
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"class not found: {class_name}")
    end = text.find("\n\t};", start)
    if end < 0:
        raise RuntimeError(f"class terminator not found: {class_name}")
    end += len("\n\t};")
    segment = text[start:end]
    changed = transform(segment)
    return text[:start] + changed + text[end:]


def replace_signatures(rel: str, expected: int) -> None:
    text = read(rel)
    pattern = re.compile(
        r"(?P<i>\t+)(?:const )?int native_status,\n"
        r"(?P=i)const volatile void\* native_mapping,\n"
        r"(?P=i)(?:const )?int delegated_extend,\n"
        r"(?P=i)sqlite_backend_opaque_identity observed_shm_object_receipt,\n"
        r"(?P=i)sqlite_backend_opaque_identity observed_shm_entry_receipt,\n"
        r"(?P=i)sqlite_backend_opaque_identity observed_device_receipt,\n"
        r"(?P=i)sqlite_backend_opaque_identity observed_mount_receipt"
    )
    text, count = pattern.subn(
        lambda match: match.group("i") +
        "sqlite_shm_reader_mapped_native_observation observation",
        text,
    )
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} mapped signatures, found {count}")
    write(rel, text)


def replace_forwarding(rel: str, expected: int) -> None:
    text = read(rel)
    pattern = re.compile(
        r"(?P<i>\t+)std::move\(observed_shm_object_receipt\),\n"
        r"(?P=i)std::move\(observed_shm_entry_receipt\),\n"
        r"(?P=i)std::move\(observed_device_receipt\),\n"
        r"(?P=i)std::move\(observed_mount_receipt\)"
    )
    text, count = pattern.subn(
        lambda match: match.group("i") + "std::move(observation)", text
    )
    if count != expected:
        raise RuntimeError(f"{rel}: expected {expected} mapped forwarding blocks, found {count}")
    write(rel, text)


# ---------------------------------------------------------------------------
# Distinct source-private native observation receipt.
# ---------------------------------------------------------------------------

rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"
text = read(rel)
observation_decl = r'''
	/**
	 * Move-only source-private observation of one determinate reader native xShmMap result.
	 *
	 * This is not attachment authority.  The registry-private observation source seals the exact
	 * expected reservation, native status/pointer/extend result, and direct SHM
	 * object/entry/device/mount receipts.  The owner-qualified mapped validator consumes it exactly
	 * once and may promote it to a retained native attachment identity only after all private owner,
	 * callback, effect, family, mapping and observation checks succeed.
	 */
	class sqlite_shm_reader_mapped_native_observation final
	{
	  public:
		sqlite_shm_reader_mapped_native_observation(
			sqlite_shm_reader_mapped_native_observation&&) noexcept = default;
		sqlite_shm_reader_mapped_native_observation& operator=(
			sqlite_shm_reader_mapped_native_observation&&) = delete;
		sqlite_shm_reader_mapped_native_observation(
			const sqlite_shm_reader_mapped_native_observation&) = delete;
		sqlite_shm_reader_mapped_native_observation& operator=(
			const sqlite_shm_reader_mapped_native_observation&) = delete;

		[[nodiscard]] const sqlite_shm_reader_attachment_reservation_identity&
		expected() const noexcept;
		[[nodiscard]] int native_status() const noexcept;
		[[nodiscard]] const volatile void* native_mapping() const noexcept;
		[[nodiscard]] int delegated_extend() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_shm_object_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_shm_entry_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		observed_device_receipt() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& observed_mount_receipt() const noexcept;

	  private:
		friend class detail::sqlite_shm_mapping_registry_state;
		friend class sqlite_same_process_shm_lease_test_peer;

		sqlite_shm_reader_mapped_native_observation(
			sqlite_shm_reader_attachment_reservation_identity expected,
			int native_status,
			const volatile void* native_mapping,
			int delegated_extend,
			sqlite_backend_opaque_identity observed_shm_object_receipt,
			sqlite_backend_opaque_identity observed_shm_entry_receipt,
			sqlite_backend_opaque_identity observed_device_receipt,
			sqlite_backend_opaque_identity observed_mount_receipt);

		sqlite_shm_reader_attachment_reservation_identity expected_;
		int native_status_{};
		const volatile void* native_mapping_{};
		int delegated_extend_{};
		sqlite_backend_opaque_identity observed_shm_object_receipt_;
		sqlite_backend_opaque_identity observed_shm_entry_receipt_;
		sqlite_backend_opaque_identity observed_device_receipt_;
		sqlite_backend_opaque_identity observed_mount_receipt_;
	};

'''
text = insert_before_once(
    text,
    "\t/**\n\t * Issuer-sealed post-map observed reader attachment identity.",
    observation_decl,
    "mapped native observation declaration",
)

# The exact lease state promotes a checked observation into the retained identity.  The registry
# state never constructs a final mapped receipt.
def final_identity_friends(segment: str) -> str:
    old = (
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n"
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;\n"
        "\t\tfriend class sqlite_same_process_shm_lease_test_peer;"
    )
    new = (
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n"
        "\t\tfriend class sqlite_same_process_shm_lease_test_peer;"
    )
    count = segment.count(old)
    if count != 1:
        raise RuntimeError(f"native attachment friends: expected one block, found {count}")
    return segment.replace(old, new, 1)

text = edit_class(text, "sqlite_shm_reader_native_attachment_identity", final_identity_friends)

def mapped_receipt_friends(segment: str) -> str:
    old = (
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n"
        "\t\tfriend class detail::sqlite_shm_mapping_registry_state;\n"
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;"
    )
    new = (
        "\t\tfriend class detail::sqlite_shm_mapping_lease_state;\n"
        "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;"
    )
    count = segment.count(old)
    if count != 1:
        raise RuntimeError(f"mapped receipt friends: expected one block, found {count}")
    return segment.replace(old, new, 1)

text = edit_class(text, "sqlite_shm_verified_reader_attachment_post_map_receipt", mapped_receipt_friends)
write(rel, text)

for target, count in (
    ("src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp", 2),
    ("src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp", 1),
    ("src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp", 2),
    ("src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp", 2),
):
    replace_signatures(target, count)

for target, count in (
    ("src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp", 1),
    ("src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp", 2),
):
    replace_forwarding(target, count)

# Observation accessors and constructor.
rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
text = read(rel)
observation_impl = r'''
	sqlite_shm_reader_mapped_native_observation::sqlite_shm_reader_mapped_native_observation(
		sqlite_shm_reader_attachment_reservation_identity expected,
		const int native_status,
		const volatile void* native_mapping,
		const int delegated_extend,
		sqlite_backend_opaque_identity observed_shm_object_receipt,
		sqlite_backend_opaque_identity observed_shm_entry_receipt,
		sqlite_backend_opaque_identity observed_device_receipt,
		sqlite_backend_opaque_identity observed_mount_receipt)
		: expected_{std::move(expected)}, native_status_{native_status},
		  native_mapping_{native_mapping}, delegated_extend_{delegated_extend},
		  observed_shm_object_receipt_{std::move(observed_shm_object_receipt)},
		  observed_shm_entry_receipt_{std::move(observed_shm_entry_receipt)},
		  observed_device_receipt_{std::move(observed_device_receipt)},
		  observed_mount_receipt_{std::move(observed_mount_receipt)}
	{
	}

	const sqlite_shm_reader_attachment_reservation_identity&
	sqlite_shm_reader_mapped_native_observation::expected() const noexcept
	{
		return expected_;
	}

	int sqlite_shm_reader_mapped_native_observation::native_status() const noexcept
	{
		return native_status_;
	}

	const volatile void*
	sqlite_shm_reader_mapped_native_observation::native_mapping() const noexcept
	{
		return native_mapping_;
	}

	int sqlite_shm_reader_mapped_native_observation::delegated_extend() const noexcept
	{
		return delegated_extend_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_mapped_native_observation::observed_shm_object_receipt() const noexcept
	{
		return observed_shm_object_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_mapped_native_observation::observed_shm_entry_receipt() const noexcept
	{
		return observed_shm_entry_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_mapped_native_observation::observed_device_receipt() const noexcept
	{
		return observed_device_receipt_;
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_reader_mapped_native_observation::observed_mount_receipt() const noexcept
	{
		return observed_mount_receipt_;
	}

'''
text = insert_before_once(
    text,
    "\tsqlite_shm_reader_native_attachment_identity::sqlite_shm_reader_native_attachment_identity(",
    observation_impl,
    "mapped native observation implementation",
)

# The lease state derives all native fields from the sealed observation and promotes it only after
# exact owner/effect matching.
old_shape = (
    "\t\t\t\t\tconst auto exact_native_shape =\n"
    "\t\t\t\t\t\tnative_status == static_cast<int>(sqlite_native_map_status::ok) &&\n"
    "\t\t\t\t\t\tnative_mapping != nullptr && delegated_extend == 0 &&\n"
    "\t\t\t\t\t\tnative_mapping == map->expected_mapping.native_mapping &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_shm_object_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_shm_entry_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_device_receipt) &&\n"
    "\t\t\t\t\t\tvalid_identity(observed_mount_receipt);"
)
new_shape = (
    "\t\t\t\t\tconst auto exact_native_shape =\n"
    "\t\t\t\t\t\tobservation.expected() == map->request.expected_attachment &&\n"
    "\t\t\t\t\t\tobservation.native_status() ==\n"
    "\t\t\t\t\t\t\tstatic_cast<int>(sqlite_native_map_status::ok) &&\n"
    "\t\t\t\t\t\tobservation.native_mapping() != nullptr &&\n"
    "\t\t\t\t\t\tobservation.delegated_extend() == 0 &&\n"
    "\t\t\t\t\t\tobservation.native_mapping() == map->expected_mapping.native_mapping &&\n"
    "\t\t\t\t\t\tvalid_identity(observation.observed_shm_object_receipt()) &&\n"
    "\t\t\t\t\t\tvalid_identity(observation.observed_shm_entry_receipt()) &&\n"
    "\t\t\t\t\t\tvalid_identity(observation.observed_device_receipt()) &&\n"
    "\t\t\t\t\t\tvalid_identity(observation.observed_mount_receipt());"
)
count = text.count(old_shape)
if count != 1:
    raise RuntimeError(f"mapped observation shape: expected one block, found {count}")
text = text.replace(old_shape, new_shape, 1)
old_observed = (
    "\t\t\t\t\tauto observed = sqlite_shm_reader_native_attachment_identity{\n"
    "\t\t\t\t\t\trequest.expected_attachment,\n"
    "\t\t\t\t\t\tstd::move(observed_shm_object_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_shm_entry_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_device_receipt),\n"
    "\t\t\t\t\t\tstd::move(observed_mount_receipt)};"
)
new_observed = (
    "\t\t\t\t\tauto observed = sqlite_shm_reader_native_attachment_identity{\n"
    "\t\t\t\t\t\trequest.expected_attachment,\n"
    "\t\t\t\t\t\tobservation.observed_shm_object_receipt(),\n"
    "\t\t\t\t\t\tobservation.observed_shm_entry_receipt(),\n"
    "\t\t\t\t\t\tobservation.observed_device_receipt(),\n"
    "\t\t\t\t\t\tobservation.observed_mount_receipt()};"
)
count = text.count(old_observed)
if count != 1:
    raise RuntimeError(f"mapped observation promotion: expected one block, found {count}")
text = text.replace(old_observed, new_observed, 1)
write(rel, text)

# Missing public coordinator-to-state delegation.
text = read(rel)
coordinator_wrapper = r'''
	sqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>
	sqlite_same_process_shm_mapping_lease_coordinator::
		validate_registry_reader_mapped_attachment_effect(
			const sqlite_shm_registry_family_pin& family,
			const sqlite_shm_reader_attachment_map_inflight& inflight,
			sqlite_shm_reader_mapped_effect_identity_validation_capability capability,
			sqlite_shm_reader_mapped_native_observation observation) noexcept
	{
		return state_->validate_reader_mapped_attachment_effect(
			family, inflight, std::move(capability), std::move(observation));
	}

'''
text = insert_before_once(
    text,
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_zero_effect_receipt>\n"
    "\tsqlite_same_process_shm_mapping_lease_coordinator::\n"
    "\t\tvalidate_registry_reader_zero_attachment_effect(",
    coordinator_wrapper,
    "mapped coordinator delegation",
)
write(rel, text)

# ---------------------------------------------------------------------------
# Test-only source receipt minting and focused closed-validator integration.
# ---------------------------------------------------------------------------

rel = "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
text = read(rel)
observation_helper = r'''
		[[nodiscard]] static sqlite_shm_reader_mapped_native_observation
		reader_mapped_native_observation(
			const sqlite_shm_reader_attachment_map_request& request,
			const int native_status,
			const volatile void* native_mapping,
			const int delegated_extend)
		{
			return {request.expected_attachment,
				native_status,
				native_mapping,
				delegated_extend,
				{"test.registry.reader-observed-shm-object", {std::byte{1}}},
				{"test.registry.reader-observed-shm-entry", {std::byte{2}}},
				{"test.registry.reader-observed-device", {std::byte{3}}},
				{"test.registry.reader-observed-mount", {std::byte{4}}}};
		}

'''
text = insert_before_once(
    text,
    "\t\t[[nodiscard]] static sqlite_shm_verified_reader_attachment_post_map_receipt\n"
    "\t\treader_attachment_map(",
    observation_helper,
    "mapped observation test helper",
)
needle = (
    "\t\t\t\tconst auto exact_mapping = mapping(setup.writer_attempt.native_page.get());\n"
    "\t\t\t\tauto receipt = sqlite_same_process_shm_reader_receipt_validator::validate("
)
replacement = (
    "\t\t\t\tconst auto exact_mapping = mapping(setup.writer_attempt.native_page.get());\n"
    "\t\t\t\tauto observation =\n"
    "\t\t\t\t\tsqlite_same_process_shm_lease_test_peer::reader_mapped_native_observation(\n"
    "\t\t\t\t\t\trequest, 0, exact_mapping.native_mapping, 0);\n"
    "\t\t\t\tauto receipt = sqlite_same_process_shm_reader_receipt_validator::validate("
)
count = text.count(needle)
if count != 1:
    raise RuntimeError(f"mapped observation test insertion: expected one location, found {count}")
text = text.replace(needle, replacement, 1)
raw_args = (
    "\t\t\t\t\t0,\n"
    "\t\t\t\t\texact_mapping.native_mapping,\n"
    "\t\t\t\t\t0,\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-shm-object\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-shm-entry\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-device\", 78U),\n"
    "\t\t\t\t\tidentity(\"test.registry.qualified-mapped-mount\", 78U));"
)
count = text.count(raw_args)
if count != 1:
    raise RuntimeError(f"mapped observation test raw args: expected one block, found {count}")
text = text.replace(raw_args, "\t\t\t\t\tstd::move(observation));", 1)
write(rel, text)

print("U2a1c authority-complete mapped-result patch applied")
