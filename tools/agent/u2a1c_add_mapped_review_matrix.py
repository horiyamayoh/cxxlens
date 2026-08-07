#!/usr/bin/env python3
from __future__ import annotations

import subprocess
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
issuer_header = ROOT / "src/sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"

if "class sqlite_shm_reader_mapped_effect_identity_validation_capability final" not in issuer_header.read_text(encoding="utf-8"):
    scripts = [
        "tools/agent/u2a1c_transform_issuer.py",
        "tools/agent/u2a1c_transform_lease_header.py",
        "tools/agent/u2a1c_transform_lease_support.py",
        "tools/agent/u2a1c_transform_lease_state.py",
        "tools/agent/u2a1c_transform_registry.py",
        "tools/agent/u2a1c_transform_test.py",
    ]
    for rel in scripts:
        path = ROOT / rel
        if not path.is_file():
            raise RuntimeError(f"mapped source absent and transform missing: {rel}")
        subprocess.run(["python3", str(path)], cwd=ROOT, check=True, timeout=60)

lease_cpp = ROOT / "src/sdk/sqlite_same_process_shm_mapping_lease_internal.cpp"
t = lease_cpp.read_text(encoding="utf-8")
implementation = (
    "\t\tsqlite_shm_reader_mapped_effect_receipt_control::\n"
    "\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept"
)
if implementation not in t:
    marker = (
        "\t\tsqlite_shm_reader_zero_effect_receipt_control::\n"
        "\t\t\t~sqlite_shm_reader_zero_effect_receipt_control() noexcept"
    )
    start = t.find(marker)
    if start < 0:
        raise RuntimeError("zero receipt control destructor not found")
    brace = t.find("{", start)
    depth = 0
    end = None
    for index in range(brace, len(t)):
        if t[index] == "{":
            depth += 1
        elif t[index] == "}":
            depth -= 1
            if depth == 0:
                end = index + 1
                break
    if end is None:
        raise RuntimeError("zero receipt control destructor is unbalanced")
    destructor = '''

\t\tsqlite_shm_reader_mapped_effect_receipt_control::
\t\t\t~sqlite_shm_reader_mapped_effect_receipt_control() noexcept
\t\t{
\t\t\tif (!process_epoch || expected_process_epoch == 0U ||
\t\t\t\tprocess_epoch->load(std::memory_order_acquire) != expected_process_epoch)
\t\t\t\treturn;
\t\t\tconst auto exact_owner = owner.lock();
\t\t\tif (exact_owner &&
\t\t\t\texact_owner->mapped_effect_validation_phase.load(std::memory_order_acquire) ==
\t\t\t\t\tsqlite_shm_reader_mapped_effect_validation_phase::sealed)
\t\t\t\texact_owner->abandon();
\t\t}'''
    t = t[:end] + destructor + t[end:]
    lease_cpp.write_text(t, encoding="utf-8")

path = ROOT / "tests/unit/sdk/sqlite_same_process_shm_mapping_registry_test.cpp"
t = path.read_text(encoding="utf-8")
marker = "void verify_qualified_mapped_validator_wrong_role_is_nonmutating()"
if marker not in t:
    matrix = r'''
	[[nodiscard]] auto validate_qualified_mapped_result(
		reader_candidate_setup& setup,
		const qualified_zero_map_owner& owner,
		const int native_status,
		const volatile void* native_mapping,
		const int delegated_extend,
		const std::uint8_t marker)
	{
		return sqlite_same_process_shm_reader_receipt_validator::validate(
			*setup.fixture.registry,
			*setup.fixture.family_pin,
			owner.inflight,
			owner.identity.scope,
			owner.identity.callback_identity,
			owner.effect,
			native_status,
			native_mapping,
			delegated_extend,
			identity("test.registry.qualified-mapped-shm-object", marker),
			identity("test.registry.qualified-mapped-shm-entry", marker),
			identity("test.registry.qualified-mapped-device", marker),
			identity("test.registry.qualified-mapped-mount", marker));
	}

	void verify_qualified_mapped_validator_wrong_role_is_nonmutating()
	{
		auto setup = make_reader_candidate_setup(201U);
		auto owner = prepare_qualified_map_effect_owner(setup,
			202U,
			sqlite_shm_reader_effect_identity_role::zero_attachment_result);
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*setup.coordinator);
		auto rejected = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			203U);
		const auto after =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*setup.coordinator);
		require(!rejected &&
				rejected.error().reason ==
					sqlite_shm_lease_rejection_reason::receipt_mismatch &&
				owner.inflight.valid() && setup.session.valid() && owner.effect.valid() &&
				before.last_issued_sequence == after.last_issued_sequence &&
				before.last_committed_sequence == after.last_committed_sequence &&
				before.map_attempts.size() == after.map_attempts.size() &&
				before.terminal_quarantines.size() == after.terminal_quarantines.size(),
			"mapped validator rejects the zero-effect role without consuming the exact owner");
		complete_callback_free_identity_smoke(setup,
			owner.inflight,
			owner.identity.scope,
			owner.identity.callback_identity,
			owner.effect,
			204U);
	}

	void verify_qualified_mapped_validator_invalid_native_row_is_terminal()
	{
		auto setup = make_reader_candidate_setup(205U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 206U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto invalid = validate_qualified_mapped_result(setup, owner, 0, nullptr, 0, 207U);
		auto replay = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			208U);
		require(!invalid && !replay,
			"invalid mapped native row burns the observation and cannot be retried as success");
	}

	void verify_qualified_mapped_validator_receipt_drop_abandons_owner()
	{
		auto setup = make_reader_candidate_setup(209U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 210U, sqlite_shm_reader_effect_identity_role::mapped_result);
		{
			auto validated = validate_qualified_mapped_result(setup,
				owner,
				0,
				setup.writer_attempt.native_page.get(),
				0,
				211U);
			require(validated && owner.effect.valid(),
				"seal one exact mapped receipt before testing receipt destruction");
		}
		auto replay = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			212U);
		require(!replay && !owner.identity.scope.valid() && !owner.effect.valid(),
			"dropping the sealed mapped receipt abandons its exact qualified owner");
	}

	void verify_qualified_mapped_validator_effect_drop_blocks_commit()
	{
		auto setup = make_reader_candidate_setup(213U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 214U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto validated = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			215U);
		require(validated && owner.effect.valid(),
			"seal mapped receipt before presenter-drop test");
		{
			auto dropped = std::move(owner.effect);
			require(dropped.valid(), "move the exact mapped effect presenter before dropping it");
		}
		auto committed = setup.fixture.registry->commit_reader_map(
			*setup.fixture.family_pin, owner.inflight, *validated, setup.session);
		require(!committed,
			"dropping the issued mapped effect presenter invalidates the retained receipt");
	}

	void verify_qualified_mapped_commit_rejects_raw_receipt_bypass()
	{
		auto setup = make_reader_candidate_setup(216U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 217U, sqlite_shm_reader_effect_identity_role::mapped_result);
		const auto request = reader_attachment_map_request(
			owner.identity.request, owner.identity.callback_identity.receipt());
		auto raw = sqlite_same_process_shm_lease_test_peer::reader_attachment_map(
			request,
			setup.holder.generation(),
			mapping(setup.writer_attempt.native_page.get()),
			owner.effect.identity());
		auto committed = setup.fixture.registry->commit_reader_map(
			*setup.fixture.family_pin, owner.inflight, raw, setup.session);
		require(!committed,
			"qualified mapped terminal rejects a structurally valid raw test-peer receipt");
	}

	void verify_qualified_mapped_validator_duplicate_poisoning_is_one_shot()
	{
		auto setup = make_reader_candidate_setup(218U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 219U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto first = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			220U);
		auto duplicate = validate_qualified_mapped_result(setup,
			owner,
			0,
			setup.writer_attempt.native_page.get(),
			0,
			221U);
		require(first && !duplicate, "only the first mapped validation can seal provenance");
		auto committed = setup.fixture.registry->commit_reader_map(
			*setup.fixture.family_pin, owner.inflight, *first, setup.session);
		require(!committed,
			"a duplicate mapped observation poisons the original receipt before terminal commit");
	}

	void verify_qualified_mapped_validator_foreign_proof_is_nonmutating()
	{
		auto exact = make_reader_candidate_setup(222U);
		auto foreign = make_reader_candidate_setup(223U);
		auto exact_owner = prepare_qualified_map_effect_owner(
			exact, 224U, sqlite_shm_reader_effect_identity_role::mapped_result);
		auto foreign_owner = prepare_qualified_map_effect_owner(
			foreign, 225U, sqlite_shm_reader_effect_identity_role::mapped_result);
		const auto before =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*exact.coordinator);
		auto rejected = sqlite_same_process_shm_reader_receipt_validator::validate(
			*exact.fixture.registry,
			*exact.fixture.family_pin,
			exact_owner.inflight,
			foreign_owner.identity.scope,
			foreign_owner.identity.callback_identity,
			foreign_owner.effect,
			0,
			exact.writer_attempt.native_page.get(),
			0,
			identity("test.registry.foreign-mapped-shm-object", 226U),
			identity("test.registry.foreign-mapped-shm-entry", 226U),
			identity("test.registry.foreign-mapped-device", 226U),
			identity("test.registry.foreign-mapped-mount", 226U));
		const auto after =
			sqlite_same_process_shm_lease_test_peer::reader_lifecycle_view(*exact.coordinator);
		require(!rejected && exact_owner.effect.valid() && foreign_owner.effect.valid() &&
				before.last_issued_sequence == after.last_issued_sequence &&
				before.last_committed_sequence == after.last_committed_sequence &&
				before.map_attempts.size() == after.map_attempts.size() &&
				before.terminal_quarantines.size() == after.terminal_quarantines.size(),
			"foreign mapped scope/callback/effect cannot mutate the exact in-flight owner");
	}

	void verify_qualified_mapped_validator_concurrent_duplicate_is_bounded()
	{
		auto setup = make_reader_candidate_setup(227U);
		auto owner = prepare_qualified_map_effect_owner(
			setup, 228U, sqlite_shm_reader_effect_identity_role::mapped_result);
		using validation_result = decltype(validate_qualified_mapped_result(
			setup, owner, 0, setup.writer_attempt.native_page.get(), 0, 229U));
		std::optional<validation_result> left;
		std::optional<validation_result> right;
		std::atomic_bool start{false};
		std::thread first([&]
		{
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			left.emplace(validate_qualified_mapped_result(setup,
				owner,
				0,
				setup.writer_attempt.native_page.get(),
				0,
				230U));
		});
		std::thread second([&]
		{
			while (!start.load(std::memory_order_acquire))
				std::this_thread::yield();
			right.emplace(validate_qualified_mapped_result(setup,
				owner,
				0,
				setup.writer_attempt.native_page.get(),
				0,
				231U));
		});
		start.store(true, std::memory_order_release);
		first.join();
		second.join();
		require(left && right,
			"both bounded mapped-validation contenders return one typed result");
		const auto success_count = static_cast<int>(left->has_value()) +
			static_cast<int>(right->has_value());
		require(success_count == 1,
			"registry serialization admits exactly one concurrent mapped validation winner");
		const auto& winner = left->has_value() ? *left : *right;
		auto committed = setup.fixture.registry->commit_reader_map(
			*setup.fixture.family_pin, owner.inflight, *winner, setup.session);
		require(!committed,
			"the losing duplicate poisons the provisional winner before terminal commit");
	}

'''
    main_pos = t.find("\nint main(")
    if main_pos < 0:
        main_pos = t.find("\n\tint main(")
    if main_pos < 0:
        raise RuntimeError("registry test main function not found")
    t = t[:main_pos] + "\n" + matrix + t[main_pos:]

    call_name = "verify_callback_free_reader_identity_prepare_claim_bind_and_registry_collision();"
    call_pos = t.find(call_name, main_pos + len(matrix))
    if call_pos < 0:
        raise RuntimeError("qualified reader identity main-call anchor not found")
    line_start = t.rfind("\n", 0, call_pos) + 1
    indent = t[line_start:call_pos]
    calls = "".join(
        indent + name + "();\n"
        for name in (
            "verify_qualified_mapped_validator_wrong_role_is_nonmutating",
            "verify_qualified_mapped_validator_invalid_native_row_is_terminal",
            "verify_qualified_mapped_validator_receipt_drop_abandons_owner",
            "verify_qualified_mapped_validator_effect_drop_blocks_commit",
            "verify_qualified_mapped_commit_rejects_raw_receipt_bypass",
            "verify_qualified_mapped_validator_duplicate_poisoning_is_one_shot",
            "verify_qualified_mapped_validator_foreign_proof_is_nonmutating",
            "verify_qualified_mapped_validator_concurrent_duplicate_is_bounded",
        )
    )
    t = t[:line_start] + calls + t[line_start:]

path.write_text(t, encoding="utf-8")
