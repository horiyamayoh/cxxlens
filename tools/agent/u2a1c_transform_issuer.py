#!/usr/bin/env python3
from u2a1c_transform_common import find_balanced_block, insert_after, read, write

rel = "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"
t = read(rel)
if "sqlite_shm_reader_mapped_effect_identity_validation_capability" not in t:
    start, end, zero_class = find_balanced_block(
        t,
        "\tclass sqlite_shm_reader_zero_effect_identity_validation_capability final",
        include_trailing_semicolon=True,
    )
    mapped = zero_class.replace(
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "sqlite_shm_reader_mapped_effect_identity_validation_capability",
    )
    mapped = mapped.replace("zero-effect", "mapped-effect").replace(
        "zero effect", "mapped effect"
    )
    t = t[:end] + "\n\n" + mapped + t[end:]
write(rel, t)

rel = "src/sdk/sqlite_same_process_shm_identity_issuer_internal.cpp"
t = read(rel)
if "validate_mapped_effect_identity_for_registry(" not in t:
    _, end, block = find_balanced_block(
        t, "\t\t\tvalidate_zero_effect_identity_for_registry("
    )
    mapped = block.replace(
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "sqlite_shm_reader_mapped_effect_identity_validation_capability",
    ).replace(
        "validate_zero_effect_identity_for_registry",
        "validate_mapped_effect_identity_for_registry",
    ).replace(
        "sqlite_shm_reader_effect_identity_role::zero_attachment_result",
        "sqlite_shm_reader_effect_identity_role::mapped_result",
    )
    t = t[:end] + "\n\n" + mapped + t[end:]

    _, end, block = find_balanced_block(
        t, "\t\t\t[[nodiscard]] bool zero_effect_capability_is_current("
    )
    mapped = block.replace(
        "zero_effect_capability_is_current", "mapped_effect_capability_is_current"
    ).replace(
        "sqlite_shm_reader_effect_identity_role::zero_attachment_result",
        "sqlite_shm_reader_effect_identity_role::mapped_result",
    )
    t = t[:end] + "\n\n" + mapped + t[end:]

    zero_friend = (
        "\t\t\tfriend class ::cxxlens::sdk::\n"
        "\t\t\t\tsqlite_shm_reader_zero_effect_identity_validation_capability;"
    )
    mapped_friend = (
        "\n\t\t\tfriend class ::cxxlens::sdk::\n"
        "\t\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability;"
    )
    t = insert_after(t, zero_friend, mapped_friend, "mapped issuer friend")

    _, end, block = find_balanced_block(
        t,
        "\t\tsqlite_shm_lease_result<sqlite_shm_reader_zero_effect_identity_validation_capability>\n"
        "\t\tvalidate_zero_effect_identity_for_registry(",
    )
    mapped = block.replace(
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "sqlite_shm_reader_mapped_effect_identity_validation_capability",
    ).replace(
        "validate_zero_effect_identity_for_registry",
        "validate_mapped_effect_identity_for_registry",
    )
    t = t[:end] + "\n\n" + mapped + t[end:]

    start = t.find(
        "\tsqlite_shm_reader_zero_effect_identity_validation_capability::\n"
        "\t\tsqlite_shm_reader_zero_effect_identity_validation_capability("
    )
    if start < 0:
        raise RuntimeError("zero capability constructor block not found")
    end = t.find("\n\tsqlite_shm_issued_reader_session_terminal_identity::", start)
    if end < 0:
        raise RuntimeError("zero capability implementation end not found")
    mapped = t[start:end].replace(
        "sqlite_shm_reader_zero_effect_identity_validation_capability",
        "sqlite_shm_reader_mapped_effect_identity_validation_capability",
    ).replace(
        "zero_effect_capability_is_current", "mapped_effect_capability_is_current"
    )
    t = t[:end] + "\n" + mapped + t[end:]
write(rel, t)
