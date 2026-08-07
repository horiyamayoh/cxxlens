#!/usr/bin/env python3
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]


def read(rel: str) -> str:
    return (ROOT / rel).read_text(encoding="utf-8")


def write(rel: str, text: str) -> None:
    (ROOT / rel).write_text(text, encoding="utf-8")


def find_block(text: str, marker: str, *, semicolon: bool = False) -> tuple[int, int, str]:
    start = text.find(marker)
    if start < 0:
        raise RuntimeError(f"marker not found: {marker}")
    brace = text.find("{", start)
    if brace < 0:
        raise RuntimeError(f"opening brace not found: {marker}")
    depth = 0
    in_string = False
    quote = ""
    escaped = False
    line_comment = False
    block_comment = False
    index = brace
    while index < len(text):
        ch = text[index]
        nxt = text[index + 1] if index + 1 < len(text) else ""
        if line_comment:
            if ch == "\n":
                line_comment = False
            index += 1
            continue
        if block_comment:
            if ch == "*" and nxt == "/":
                block_comment = False
                index += 2
            else:
                index += 1
            continue
        if in_string:
            if escaped:
                escaped = False
            elif ch == "\\":
                escaped = True
            elif ch == quote:
                in_string = False
            index += 1
            continue
        if ch == "/" and nxt == "/":
            line_comment = True
            index += 2
            continue
        if ch == "/" and nxt == "*":
            block_comment = True
            index += 2
            continue
        if ch in ('"', "'"):
            in_string = True
            quote = ch
            index += 1
            continue
        if ch == "{":
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                if semicolon:
                    while end < len(text) and text[end] in " \t\r\n":
                        end += 1
                    if end < len(text) and text[end] == ";":
                        end += 1
                return start, end, text[start:end]
        index += 1
    raise RuntimeError(f"unbalanced block: {marker}")


matrix = ROOT / "tools/agent/u2a1c_add_mapped_review_matrix.py"
if not matrix.is_file():
    raise RuntimeError("mapped review matrix transform is unavailable")
subprocess.run(["python3", str(matrix)], cwd=ROOT, check=True, timeout=240)

# ---------------------------------------------------------------------------
# Lease header: narrow construction authority and introduce a move-only,
# non-projectable post-native observation capability.
# ---------------------------------------------------------------------------
rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.hpp"
t = read(rel)

if "class sqlite_shm_reader_mapped_post_native_observation;" not in t:
    anchor = "\tclass sqlite_shm_reader_mapped_effect_identity_validation_capability;"
    if anchor not in t:
        raise RuntimeError("mapped effect capability forward declaration not found")
    t = t.replace(anchor, anchor + "\n\tclass sqlite_shm_reader_mapped_post_native_observation;", 1)

if "class sqlite_shm_reader_mapped_post_native_observation_minter;" not in t:
    anchor = "\t\tclass sqlite_shm_reader_identity_completion_control;"
    if anchor not in t:
        raise RuntimeError("detail completion-control forward declaration not found")
    t = t.replace(
        anchor,
        anchor + "\n\t\tclass sqlite_shm_reader_mapped_post_native_observation_minter;",
        1,
    )

start, end, block = find_block(t, "\tclass sqlite_shm_reader_native_attachment_identity")
block = block.replace("\n\t\tfriend class detail::sqlite_shm_mapping_lease_state;", "", 1)
if "friend class sqlite_shm_reader_mapped_post_native_observation;" not in block:
    anchor = "\t\tfriend class sqlite_same_process_shm_reader_receipt_validator;"
    if anchor not in block:
        raise RuntimeError("native attachment validator friend not found")
    block = block.replace(
        anchor,
        "\t\tfriend class sqlite_shm_reader_mapped_post_native_observation;\n" + anchor,
        1,
    )
t = t[:start] + block + t[end:]

start, end, block = find_block(t, "\tclass sqlite_shm_reader_attachment_map_inflight")
if "friend class sqlite_shm_reader_mapped_post_native_observation;" not in block:
    anchor = "\t\tfriend class sqlite_shm_verified_reader_attachment_zero_effect_receipt;"
    if anchor not in block:
        raise RuntimeError("inflight friend anchor not found")
    block = block.replace(
        anchor,
        "\t\tfriend class sqlite_shm_reader_mapped_post_native_observation;\n" + anchor,
        1,
    )
t = t[:start] + block + t[end:]

if "\tclass sqlite_shm_reader_mapped_post_native_observation\n" not in t:
    observation = '''\t/**
\t * Move-only, non-projectable post-native observation for one exact reader map attempt.
\t *
\t * Production has no general constructor or aggregate overload. A future source-private backend
\t * minter may form this capability only after the exact native callback and direct SHM
\t * object/entry/device/mount observation complete. The test peer is privileged solely to exercise
\t * this production-inert boundary. The terminal validator consumes the capability by value.
\t */
\tclass sqlite_shm_reader_mapped_post_native_observation final
\t{
\t  public:
\t\tsqlite_shm_reader_mapped_post_native_observation(
\t\t\tsqlite_shm_reader_mapped_post_native_observation&&) noexcept = default;
\t\tsqlite_shm_reader_mapped_post_native_observation& operator=(
\t\t\tsqlite_shm_reader_mapped_post_native_observation&&) = delete;
\t\tsqlite_shm_reader_mapped_post_native_observation(
\t\t\tconst sqlite_shm_reader_mapped_post_native_observation&) = delete;
\t\tsqlite_shm_reader_mapped_post_native_observation& operator=(
\t\t\tconst sqlite_shm_reader_mapped_post_native_observation&) = delete;

\t  private:
\t\tfriend class detail::sqlite_shm_mapping_lease_state;
\t\tfriend class detail::sqlite_shm_mapping_registry_state;
\t\tfriend class detail::sqlite_shm_reader_mapped_post_native_observation_minter;
\t\tfriend class sqlite_same_process_shm_lease_test_peer;

\t\tsqlite_shm_reader_mapped_post_native_observation(
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_attachment_map_request request,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt);

\t\tstd::weak_ptr<detail::sqlite_shm_mapping_lease_state> state_;
\t\tstd::uint64_t token_{};
\t\tstd::uint64_t generation_{};
\t\tsqlite_shm_reader_attachment_map_request request_;
\t\tint native_status_{};
\t\tconst volatile void* native_mapping_{};
\t\tint delegated_extend_{};
\t\tsqlite_shm_reader_native_attachment_identity observed_attachment_;
\t};

'''
    marker = "\tclass sqlite_shm_verified_reader_attachment_post_map_receipt"
    pos = t.find(marker)
    if pos < 0:
        raise RuntimeError("mapped post-map receipt class not found")
    t = t[:pos] + observation + t[pos:]

start, end, block = find_block(t, "\tclass sqlite_same_process_shm_reader_receipt_validator final")
old_parameters = '''\t\t\t const sqlite_shm_issued_reader_effect_identity& effect,
\t\t\t int native_status,
\t\t\t const volatile void* native_mapping,
\t\t\t int delegated_extend,
\t\t\t sqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\t sqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\t sqlite_backend_opaque_identity observed_device_receipt,
\t\t\t sqlite_backend_opaque_identity observed_mount_receipt) noexcept;'''
new_parameters = '''\t\t\t const sqlite_shm_issued_reader_effect_identity& effect,
\t\t\t sqlite_shm_reader_mapped_post_native_observation observation) noexcept;'''
if old_parameters not in block:
    raise RuntimeError("raw mapped validator parameter list not found")
block = block.replace(old_parameters, new_parameters, 1)
block = block.replace(
    "The caller supplies only the closed native result and independently\n"
    "\t * observed SHM object/entry/device/mount receipts. There is no raw request, mapping tuple,\n"
    "\t * generation, or opaque-effect overload.",
    "The caller supplies one source-private move-only post-native observation capability. There is\n"
    "\t * no raw status, pointer, observation aggregate, request, mapping tuple, generation, or\n"
    "\t * opaque-effect overload.",
)
t = t[:start] + block + t[end:]

old_method = '''\t\tvalidate_registry_reader_mapped_attachment_effect(
\t\t\tconst sqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept;'''
new_method = '''\t\tvalidate_registry_reader_mapped_attachment_effect(
\t\t\tconst sqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability,
\t\t\tsqlite_shm_reader_mapped_post_native_observation observation) noexcept;'''
if old_method not in t:
    raise RuntimeError("raw lease mapped validation declaration not found")
t = t.replace(old_method, new_method, 1)
write(rel, t)

# ---------------------------------------------------------------------------
# Lease implementation: source capability construction, closed validator API,
# and exact capability consumption inside the lease owner.
# ---------------------------------------------------------------------------
rel = "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = read(rel)

if "sqlite_shm_reader_mapped_post_native_observation::\n\t\tsqlite_shm_reader_mapped_post_native_observation(" not in t:
    implementation = '''\tsqlite_shm_reader_mapped_post_native_observation::
\t\tsqlite_shm_reader_mapped_post_native_observation(
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_attachment_map_request request,
\t\t\tconst int native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tconst int delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt)
\t\t: state_{inflight.state_}, token_{inflight.token_}, generation_{inflight.generation_},
\t\t  request_{std::move(request)}, native_status_{native_status},
\t\t  native_mapping_{native_mapping}, delegated_extend_{delegated_extend},
\t\t  observed_attachment_{request_.expected_attachment,
\t\t\tstd::move(observed_shm_object_receipt),
\t\t\tstd::move(observed_shm_entry_receipt),
\t\t\tstd::move(observed_device_receipt),
\t\t\tstd::move(observed_mount_receipt)}
\t{
\t}

'''
    marker = (
        "\tsqlite_shm_verified_reader_attachment_post_map_receipt::\n"
        "\t\tsqlite_shm_verified_reader_attachment_post_map_receipt("
    )
    pos = t.find(marker)
    if pos < 0:
        raise RuntimeError("mapped post-map receipt implementation not found")
    t = t[:pos] + implementation + t[pos:]

start, end, _ = find_block(t, "\tsqlite_same_process_shm_reader_receipt_validator::validate(")
validator_impl = '''\tsqlite_same_process_shm_reader_receipt_validator::validate(
\t\tsqlite_same_process_shm_mapping_registry& registry,
\t\tsqlite_shm_registry_family_pin& family,
\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\tsqlite_shm_reader_mapped_post_native_observation observation) noexcept
\t{
\t\treturn registry.validate_reader_mapped_attachment_effect(family,
\t\t\tinflight,
\t\t\tscope,
\t\t\tcallback,
\t\t\teffect,
\t\t\tstd::move(observation));
\t}'''
# Preserve the return type immediately preceding the marker.
prefix_start = t.rfind(
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n",
    0,
    start,
)
if prefix_start < 0:
    raise RuntimeError("mapped validator return type not found")
start = prefix_start
validator_impl = (
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n"
    + validator_impl
)
t = t[:start] + validator_impl + t[end:]

start, end, _ = find_block(t, "\t\t\tvalidate_reader_mapped_attachment_effect(")
lease_method = '''\t\t\tvalidate_reader_mapped_attachment_effect(
\t\t\t\tconst sqlite_shm_registry_family_pin& registry_family,
\t\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\t\tsqlite_shm_reader_mapped_effect_identity_validation_capability capability,
\t\t\t\tsqlite_shm_reader_mapped_post_native_observation observation) noexcept
\t\t\t{
\t\t\t\tstd::shared_ptr<sqlite_shm_reader_map_identity_owner_control> exact_owner;
\t\t\t\tbool observation_burned{};
\t\t\t\ttry
\t\t\t\t{
\t\t\t\t\tstd::scoped_lock lock{mutex_};
\t\t\t\t\tif (!owns(inflight.state_, inflight.token_))
\t\t\t\t\t\treturn sqlite_shm_unexpected(stale_token(
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));
\t\t\t\t\tconst auto map = find_by_token(reader_attachment_maps_, inflight.token_);
\t\t\t\t\tif (map == reader_attachment_maps_.end() ||
\t\t\t\t\t\tmap->phase != reader_phase::inflight || !map->registry_bound ||
\t\t\t\t\t\t!map->qualified_identity_bound || !map->identity_owner_control ||
\t\t\t\t\t\tmap->identity_owner_control.get() != inflight.qualified_owner_control_.get() ||
\t\t\t\t\t\tmap->qualified_owner_phase.get() != inflight.qualified_owner_phase_.get() ||
\t\t\t\t\t\tmap->generation != inflight.generation_ ||
\t\t\t\t\t\t(map->registry_predelegate_authority &&
\t\t\t\t\t\t !map->registry_predelegate_authority->retains_exact_owned_terminal_lifetimes(
\t\t\t\t\t\t\t registry_family, map->request)))
\t\t\t\t\t\treturn sqlite_shm_unexpected(rejection(
\t\t\t\t\t\t\tsqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));

\t\t\t\t\texact_owner = map->identity_owner_control;
\t\t\t\t\tconst std::shared_ptr<sqlite_shm_reader_lifecycle_owner_abandonment_control>
\t\t\t\t\t\texact_owner_base = exact_owner;
\t\t\t\t\tif (!capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t\treturn sqlite_shm_unexpected(rejection(
\t\t\t\t\t\t\tsqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));

\t\t\t\t\tconst auto observation_state = observation.state_.lock();
\t\t\t\t\tconst auto exact_observation_binding = observation_state.get() == this &&
\t\t\t\t\t\tobservation.token_ == map->token &&
\t\t\t\t\t\tobservation.generation_ == map->generation &&
\t\t\t\t\t\tobservation.request_ == map->request &&
\t\t\t\t\t\tobservation.observed_attachment_.expected() ==
\t\t\t\t\t\t\tmap->request.expected_attachment;
\t\t\t\t\tif (!exact_observation_binding)
\t\t\t\t\t\treturn sqlite_shm_unexpected(rejection(
\t\t\t\t\t\t\tsqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));

\t\t\t\t\tauto expected_phase =
\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::unclaimed;
\t\t\t\t\tif (!exact_owner->mapped_effect_validation_phase.compare_exchange_strong(
\t\t\t\t\t\t\texpected_phase,
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::validating,
\t\t\t\t\t\t\tstd::memory_order_acq_rel,
\t\t\t\t\t\t\tstd::memory_order_acquire))
\t\t\t\t\t{
\t\t\t\t\t\tif (expected_phase ==
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::terminal_consumed)
\t\t\t\t\t\t\treturn sqlite_shm_unexpected(stale_token(
\t\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry));
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}
\t\t\t\t\tobservation_burned = true;

\t\t\t\t\tconst auto exact_native_result =
\t\t\t\t\t\tobservation.native_status_ ==
\t\t\t\t\t\t\tstatic_cast<int>(sqlite_native_map_status::ok) &&
\t\t\t\t\t\tobservation.native_mapping_ != nullptr &&
\t\t\t\t\t\tobservation.delegated_extend_ == 0 &&
\t\t\t\t\t\tmap->expected_mapping.native_mapping == observation.native_mapping_ &&
\t\t\t\t\t\tvalid_observed_reader_native_attachment(
\t\t\t\t\t\t\tobservation.observed_attachment_);
\t\t\t\t\tif (!exact_native_result)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::presented_invalid;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}

\t\t\t\t\tauto request = map->request;
\t\t\t\t\tauto mapping = map->expected_mapping;
\t\t\t\t\tauto observed_attachment = std::move(observation.observed_attachment_);
\t\t\t\t\tauto effect_identity = capability.copy_effect_identity();
\t\t\t\t\tif (!capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}

\t\t\t\t\tauto qualified_control =
\t\t\t\t\t\tstd::make_shared<sqlite_shm_reader_mapped_effect_receipt_control>(
\t\t\t\t\t\t\tstd::move(capability),
\t\t\t\t\t\t\texact_owner,
\t\t\t\t\t\t\tobservation.native_status_,
\t\t\t\t\t\t\tobservation.delegated_extend_,
\t\t\t\t\t\t\tmapping,
\t\t\t\t\t\t\tobserved_attachment);
\t\t\t\t\tauto receipt = sqlite_shm_verified_reader_attachment_post_map_receipt{
\t\t\t\t\t\tstd::move(request),
\t\t\t\t\t\tmap->generation,
\t\t\t\t\t\tmapping,
\t\t\t\t\t\tstd::move(observed_attachment),
\t\t\t\t\t\tstd::move(effect_identity),
\t\t\t\t\t\tstd::move(qualified_control)};
\t\t\t\t\tstatic_assert(std::is_nothrow_move_constructible_v<
\t\t\t\t\t\tsqlite_shm_verified_reader_attachment_post_map_receipt>);
\t\t\t\t\tauto output = sqlite_shm_lease_result<
\t\t\t\t\t\tsqlite_shm_verified_reader_attachment_post_map_receipt>{
\t\t\t\t\t\tstd::move(receipt)};
\t\t\t\t\tstatic_assert(std::is_nothrow_move_constructible_v<decltype(output)>);
\t\t\t\t\tif (!output->qualified_control_ ||
\t\t\t\t\t\t!output->qualified_control_->capability.matches_live_owner(exact_owner_base) ||
\t\t\t\t\t\texact_owner->disposition.load(std::memory_order_acquire) !=
\t\t\t\t\t\t\tsqlite_shm_reader_map_identity_disposition::live)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\tmap->quarantine_reason =
\t\t\t\t\t\t\tsqlite_shm_reader_terminal_quarantine_reason::owner_abandoned;
\t\t\t\t\t\tquarantine_reader_map_terminal_commit_locked(map->token, map->session_token);
\t\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t\t}
\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed,
\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\treturn output;
\t\t\t\t}
\t\t\t\tcatch (...)
\t\t\t\t{
\t\t\t\t\tif (observation_burned && exact_owner)
\t\t\t\t\t{
\t\t\t\t\t\texact_owner->mapped_effect_validation_phase.store(
\t\t\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::poisoned,
\t\t\t\t\t\t\tstd::memory_order_release);
\t\t\t\t\t\texact_owner->abandon();
\t\t\t\t\t}
\t\t\t\t\treturn sqlite_shm_unexpected(ambiguous());
\t\t\t\t}
\t\t\t}'''
return_type_start = t.rfind(
    "\t\t\t[[nodiscard]]\n"
    "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n",
    0,
    start,
)
if return_type_start < 0:
    raise RuntimeError("mapped lease method return type not found")
lease_method = (
    "\t\t\t[[nodiscard]]\n"
    "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n"
    + lease_method
)
t = t[:return_type_start] + lease_method + t[end:]
write(rel, t)

# ---------------------------------------------------------------------------
# Registry signatures and forwarding: consume the source-private capability.
# ---------------------------------------------------------------------------
rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.hpp"
t = read(rel)
old = '''\t\tvalidate_reader_mapped_attachment_effect(
\t\t\tsqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tint native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tint delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt) noexcept;'''
new = '''\t\tvalidate_reader_mapped_attachment_effect(
\t\t\tsqlite_shm_registry_family_pin& family,
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\tsqlite_shm_reader_mapped_post_native_observation observation) noexcept;'''
if old not in t:
    raise RuntimeError("raw registry mapped declaration not found")
t = t.replace(old, new, 1)
write(rel, t)

rel = "src/sdk/sqlite_same_process_shm_mapping_registry_internal.cpp"
t = read(rel)

start, end, _ = find_block(t, "\t\t\tvalidate_reader_mapped_attachment_effect(")
state_method = '''\t\t\tvalidate_reader_mapped_attachment_effect(
\t\t\t\tsqlite_shm_registry_family_pin& pin,
\t\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\t\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\t\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\t\t\tsqlite_shm_reader_mapped_post_native_observation observation,
\t\t\t\tconst std::shared_ptr<sqlite_shm_process_identity_issuer_state>& issuer) noexcept
\t\t\t{
\t\t\t\tif (!current(pin.process_epoch_))
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\tif (inflight.terminal_presentation_stale_for_registry())
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\ttry
\t\t\t\t{
\t\t\t\t\tstd::scoped_lock lock{mutex_};
\t\t\t\t\tif (pin.state_.get() != this)
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tsynchronize_activity_controls_locked();
\t\t\t\t\tsynchronize_reader_open_controls_locked();
\t\t\t\t\tsynchronize_coordinator_quarantines_locked();
\t\t\t\t\tconst auto exact_owner = inflight.qualified_identity_owned_for_registry(
\t\t\t\t\t\tpin.family_epoch_, pin.pin_token_, pin.alias_token_, seal_->process_epoch,
\t\t\t\t\t\tactivity_emergency_latch_);
\t\t\t\t\tif (!inflight.has_qualified_identity_for_registry())
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::receipt_mismatch,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tif (!exact_owner)
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tauto* family_pin = current_family_pin_locked(pin);
\t\t\t\t\tauto* alias = find_alias_locked(pin.alias_token_);
\t\t\t\t\tauto* family = find_family_epoch_locked(pin.family_epoch_);
\t\t\t\t\tif (family_pin == nullptr || alias == nullptr || family == nullptr ||
\t\t\t\t\t\t!family->coordinator || admission_quarantined_locked())
\t\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::stale_token,
\t\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t\tauto capability = detail::validate_mapped_effect_identity_for_registry(issuer,
\t\t\t\t\t\tscope,
\t\t\t\t\t\tcallback,
\t\t\t\t\t\teffect,
\t\t\t\t\t\tinflight.qualified_identity_owner_abandonment_for_registry());
\t\t\t\t\tif (!capability)
\t\t\t\t\t\treturn capability.error();
\t\t\t\t\treturn family->coordinator->validate_registry_reader_mapped_attachment_effect(
\t\t\t\t\t\tpin,
\t\t\t\t\t\tinflight,
\t\t\t\t\t\tstd::move(*capability),
\t\t\t\t\t\tstd::move(observation));
\t\t\t\t}
\t\t\t\tcatch (...)
\t\t\t\t{
\t\t\t\t\treturn rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
\t\t\t\t\t\tsqlite_shm_lease_recovery_action::quarantine_no_retry);
\t\t\t\t}
\t\t\t}'''
return_start = t.rfind(
    "\t\t\t[[nodiscard]]\n"
    "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n",
    0,
    start,
)
if return_start < 0:
    raise RuntimeError("registry state mapped return type not found")
state_method = (
    "\t\t\t[[nodiscard]]\n"
    "\t\t\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n"
    + state_method
)
t = t[:return_start] + state_method + t[end:]

start, end, _ = find_block(
    t,
    "\tsqlite_same_process_shm_mapping_registry::validate_reader_mapped_attachment_effect(",
)
wrapper = '''\tsqlite_same_process_shm_mapping_registry::validate_reader_mapped_attachment_effect(
\t\tsqlite_shm_registry_family_pin& family,
\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\tconst sqlite_shm_reader_lifecycle_identity_scope& scope,
\t\tconst sqlite_shm_issued_reader_callback_identity& callback,
\t\tconst sqlite_shm_issued_reader_effect_identity& effect,
\t\tsqlite_shm_reader_mapped_post_native_observation observation) noexcept
\t{
\t\treturn state_->validate_reader_mapped_attachment_effect(family,
\t\t\tinflight,
\t\t\tscope,
\t\t\tcallback,
\t\t\teffect,
\t\t\tstd::move(observation),
\t\t\tidentity_issuer_state_);
\t}'''
return_start = t.rfind(
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n",
    0,
    start,
)
if return_start < 0:
    raise RuntimeError("registry wrapper mapped return type not found")
wrapper = (
    "\tsqlite_shm_lease_result<sqlite_shm_verified_reader_attachment_post_map_receipt>\n"
    + wrapper
)
t = t[:return_start] + wrapper + t[end:]
write(rel, t)

# ---------------------------------------------------------------------------
# Tests: the privileged peer mints the source capability; all production-facing
# validator calls consume it. Add wrong-source and noncopyability coverage.
# ---------------------------------------------------------------------------
rel = "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
t = read(rel)

if "reader_mapped_post_native_observation(" not in t:
    marker = (
        "\t\t[[nodiscard]] static sqlite_shm_verified_reader_attachment_post_map_receipt\n"
        "\t\treader_attachment_map("
    )
    _, end, _ = find_block(t, marker)
    peer = '''

\t\t[[nodiscard]] static sqlite_shm_reader_mapped_post_native_observation
\t\treader_mapped_post_native_observation(
\t\t\tconst sqlite_shm_reader_attachment_map_inflight& inflight,
\t\t\tsqlite_shm_reader_attachment_map_request request,
\t\t\tconst int native_status,
\t\t\tconst volatile void* native_mapping,
\t\t\tconst int delegated_extend,
\t\t\tsqlite_backend_opaque_identity observed_shm_object_receipt,
\t\t\tsqlite_backend_opaque_identity observed_shm_entry_receipt,
\t\t\tsqlite_backend_opaque_identity observed_device_receipt,
\t\t\tsqlite_backend_opaque_identity observed_mount_receipt)
\t\t{
\t\t\treturn {inflight,
\t\t\t\tstd::move(request),
\t\t\t\tnative_status,
\t\t\t\tnative_mapping,
\t\t\t\tdelegated_extend,
\t\t\t\tstd::move(observed_shm_object_receipt),
\t\t\t\tstd::move(observed_shm_entry_receipt),
\t\t\t\tstd::move(observed_device_receipt),
\t\t\t\tstd::move(observed_mount_receipt)};
\t\t}'''
    t = t[:end] + peer + t[end:]

start, end, _ = find_block(t, "\t[[nodiscard]] auto validate_qualified_mapped_result(")
helper = '''\t[[nodiscard]] auto validate_qualified_mapped_result(
\t\treader_candidate_setup& setup,
\t\tconst qualified_zero_map_owner& owner,
\t\tconst int native_status,
\t\tconst volatile void* native_mapping,
\t\tconst int delegated_extend,
\t\tconst std::uint8_t marker)
\t{
\t\tauto request = reader_attachment_map_request(
\t\t\towner.identity.request, owner.identity.callback_identity.receipt());
\t\tauto observation =
\t\t\tsqlite_same_process_shm_lease_test_peer::reader_mapped_post_native_observation(
\t\t\t\towner.inflight,
\t\t\t\tstd::move(request),
\t\t\t\tnative_status,
\t\t\t\tnative_mapping,
\t\t\t\tdelegated_extend,
\t\t\t\tidentity("test.registry.qualified-mapped-shm-object", marker),
\t\t\t\tidentity("test.registry.qualified-mapped-shm-entry", marker),
\t\t\t\tidentity("test.registry.qualified-mapped-device", marker),
\t\t\t\tidentity("test.registry.qualified-mapped-mount", marker));
\t\treturn sqlite_same_process_shm_reader_receipt_validator::validate(
\t\t\t*setup.fixture.registry,
\t\t\t*setup.fixture.family_pin,
\t\t\towner.inflight,
\t\t\towner.identity.scope,
\t\t\towner.identity.callback_identity,
\t\t\towner.effect,
\t\t\tstd::move(observation));
\t}'''
t = t[:start] + helper + t[end:]

start, end, block = find_block(t, "\tvoid verify_qualified_mapped_validator_foreign_proof_is_nonmutating()")
old_call = '''\t\tauto rejected = sqlite_same_process_shm_reader_receipt_validator::validate(
\t\t\t*exact.fixture.registry,
\t\t\t*exact.fixture.family_pin,
\t\t\texact_owner.inflight,
\t\t\tforeign_owner.identity.scope,
\t\t\tforeign_owner.identity.callback_identity,
\t\t\tforeign_owner.effect,
\t\t\t0,
\t\t\texact.writer_attempt.native_page.get(),
\t\t\t0,
\t\t\tidentity("test.registry.foreign-mapped-shm-object", 226U),
\t\t\tidentity("test.registry.foreign-mapped-shm-entry", 226U),
\t\t\tidentity("test.registry.foreign-mapped-device", 226U),
\t\t\tidentity("test.registry.foreign-mapped-mount", 226U));'''
new_call = '''\t\tauto request = reader_attachment_map_request(
\t\t\texact_owner.identity.request, exact_owner.identity.callback_identity.receipt());
\t\tauto observation =
\t\t\tsqlite_same_process_shm_lease_test_peer::reader_mapped_post_native_observation(
\t\t\t\texact_owner.inflight,
\t\t\t\tstd::move(request),
\t\t\t\t0,
\t\t\t\texact.writer_attempt.native_page.get(),
\t\t\t\t0,
\t\t\t\tidentity("test.registry.foreign-mapped-shm-object", 226U),
\t\t\t\tidentity("test.registry.foreign-mapped-shm-entry", 226U),
\t\t\t\tidentity("test.registry.foreign-mapped-device", 226U),
\t\t\t\tidentity("test.registry.foreign-mapped-mount", 226U));
\t\tauto rejected = sqlite_same_process_shm_reader_receipt_validator::validate(
\t\t\t*exact.fixture.registry,
\t\t\t*exact.fixture.family_pin,
\t\t\texact_owner.inflight,
\t\t\tforeign_owner.identity.scope,
\t\t\tforeign_owner.identity.callback_identity,
\t\t\tforeign_owner.effect,
\t\t\tstd::move(observation));'''
if old_call not in block:
    raise RuntimeError("foreign raw mapped validator call not found")
block = block.replace(old_call, new_call, 1)
t = t[:start] + block + t[end:]

if "verify_qualified_mapped_validator_wrong_source_observation_is_nonmutating" not in t:
    test = r'''
	void verify_qualified_mapped_validator_wrong_source_observation_is_nonmutating()
	{
		auto exact = make_reader_candidate_setup(232U);
		auto foreign = make_reader_candidate_setup(233U);
		auto exact_owner = prepare_qualified_map_effect_owner(
			exact, 234U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto foreign_owner = prepare_qualified_map_effect_owner(
			foreign, 235U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto foreign_request = reader_attachment_map_request(
			foreign_owner.identity.request,
			foreign_owner.identity.callback_identity.receipt());
		auto foreign_observation =
			sqlite_same_process_shm_lease_test_peer::reader_mapped_post_native_observation(
				foreign_owner.inflight,
				std::move(foreign_request),
				0,
				foreign.writer_attempt.native_page.get(),
				0,
				identity("test.registry.wrong-source-mapped-shm-object", 236U),
				identity("test.registry.wrong-source-mapped-shm-entry", 236U),
				identity("test.registry.wrong-source-mapped-device", 236U),
				identity("test.registry.wrong-source-mapped-mount", 236U));
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*exact.coordinator);
		auto rejected = sqlite_same_process_shm_reader_receipt_validator::validate(
			*exact.fixture.registry,
			*exact.fixture.family_pin,
			exact_owner.inflight,
			exact_owner.identity.scope,
			exact_owner.identity.callback_identity,
			exact_owner.effect,
			std::move(foreign_observation));
		const auto after =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*exact.coordinator);
		require(!rejected &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::receipt_mismatch &&
				exact_owner.effect.valid() && foreign_owner.effect.valid() &&
				before.last_issued_sequence == after.last_issued_sequence &&
				before.last_committed_sequence == after.last_committed_sequence &&
				before.map_attempts.size() == after.map_attempts.size() &&
				before.terminal_quarantines.size() == after.terminal_quarantines.size(),
			"a source-private observation for another owner cannot mutate the exact attempt");
	}

'''
    main_pos = t.find("\nint main(")
    if main_pos < 0:
        raise RuntimeError("registry main not found for wrong-source test")
    t = t[:main_pos] + "\n" + test + t[main_pos:]
    anchor = "verify_qualified_mapped_validator_foreign_proof_is_nonmutating();"
    call_pos = t.find(anchor, main_pos + len(test))
    if call_pos < 0:
        raise RuntimeError("mapped foreign-proof main call not found")
    line_start = t.rfind("\n", 0, call_pos) + 1
    indent = t[line_start:call_pos]
    t = (
        t[:line_start]
        + indent
        + "verify_qualified_mapped_validator_wrong_source_observation_is_nonmutating();\n"
        + t[line_start:]
    )

static_anchor = "\t[[nodiscard]] auto validate_qualified_mapped_result("
if "mapped observation capability is noncopyable" not in t:
    pos = t.find(static_anchor)
    if pos < 0:
        raise RuntimeError("mapped validation helper not found for static assertions")
    assertions = '''\tstatic_assert(
\t\t!std::is_copy_constructible_v<sqlite_shm_reader_mapped_post_native_observation>,
\t\t"mapped observation capability is noncopyable");
\tstatic_assert(
\t\t!std::is_copy_assignable_v<sqlite_shm_reader_mapped_post_native_observation>,
\t\t"mapped observation capability is nonassignable");

'''
    t = t[:pos] + assertions + t[pos:]

write(rel, t)
