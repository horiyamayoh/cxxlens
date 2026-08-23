#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "sdk/sqlite_exact_empty_normalization_internal.hpp"

namespace
{
	using namespace cxxlens::detail::sqlite_qualification;
	using phase = sqlite_exact_empty_normalization_phase;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	constexpr sqlite_logical_read_receipt closed_empty_receipt{true, true, true, 0U, true};

	constexpr std::array f0_path{
		phase::unresolved,
		phase::logical_read_receipt,
		phase::receipt_revalidated,
		phase::effect_profile_capability_sealed,
		phase::exclusive_normalization_owner,
		phase::pre_effect_sealed,
		phase::effect_journal_open,
		phase::journal_created,
		phase::journal_parent_fsynced,
		phase::permitted_callback_effects,
		phase::terminal_journal_deleted,
		phase::terminal_journal_parent_fsynced,
		phase::file_and_parent_durable,
		phase::confirmed_close,
		phase::post_close_census,
		phase::normalization_receipt,
	};

	constexpr std::array fz_path{
		phase::unresolved,
		phase::logical_read_receipt,
		phase::receipt_revalidated,
		phase::effect_profile_capability_sealed,
		phase::exclusive_normalization_owner,
		phase::pre_effect_sealed,
		phase::effect_journal_open,
		phase::coordination_wal_deleted,
		phase::coordination_wal_parent_fsynced,
		phase::journal_created,
		phase::journal_parent_fsynced,
		phase::permitted_callback_effects,
		phase::terminal_journal_deleted,
		phase::terminal_journal_parent_fsynced,
		phase::file_and_parent_durable,
		phase::confirmed_close,
		phase::post_close_census,
		phase::normalization_receipt,
	};

	constexpr sqlite_exact_empty_effect_evidence f0_evidence{
		true,
		true,
		false,
		false,
		true,
		true,
		true,
		{false, true, true},
		true,
		true,
		true,
	};

	constexpr sqlite_exact_empty_effect_evidence fz_evidence{
		true,
		true,
		true,
		true,
		true,
		true,
		true,
		{true, true, true},
		true,
		true,
		true,
	};

	void check_entry_requires_closed_zero_effect_receipt()
	{
		require(validate_sqlite_exact_empty_normalization_entry(closed_empty_receipt).admitted(),
				"closed exact-empty zero-custody zero-effect receipt was not admitted");

		auto missing = closed_empty_receipt;
		missing.sealed = false;
		require(validate_sqlite_exact_empty_normalization_entry(missing).failure ==
					sqlite_exact_empty_entry_failure::missing_receipt,
				"unsealed logical read receipt entered normalization");
		missing = closed_empty_receipt;
		missing.exact_empty = false;
		require(validate_sqlite_exact_empty_normalization_entry(missing).failure ==
					sqlite_exact_empty_entry_failure::not_exact_empty,
				"nonempty logical receipt entered normalization");
		missing = closed_empty_receipt;
		missing.connection_closed = false;
		require(validate_sqlite_exact_empty_normalization_entry(missing).failure ==
					sqlite_exact_empty_entry_failure::connection_not_closed,
				"open connection entered normalization");
		missing = closed_empty_receipt;
		missing.live_custody_count = 1U;
		require(validate_sqlite_exact_empty_normalization_entry(missing).failure ==
					sqlite_exact_empty_entry_failure::live_custody,
				"live custody entered normalization");
		missing = closed_empty_receipt;
		missing.zero_effect_callback_receipt = false;
		require(validate_sqlite_exact_empty_normalization_entry(missing).failure ==
					sqlite_exact_empty_entry_failure::nonzero_callback_effect,
				"nonzero callback effect entered normalization");
	}

	void check_full_effect_requires_all_parent_syncs_and_handoff()
	{
		require(validate_sqlite_exact_empty_normalization_path(f0_path, false),
				"F0 effect grammar did not validate without a coordination WAL");
		require(validate_sqlite_exact_empty_normalization_path(fz_path, true),
				"FZ-pre effect grammar did not validate its coordination WAL pair");

		const auto f0 =
			complete_sqlite_exact_empty_normalization(closed_empty_receipt, f0_path, f0_evidence);
		require(f0.completed() && f0.handoff_to_ordinary_fresh_initialization &&
					!f0.public_store_success,
				"F0 normalization did not seal an internal-only handoff");
		const auto fz =
			complete_sqlite_exact_empty_normalization(closed_empty_receipt, fz_path, fz_evidence);
		require(fz.completed() && fz.handoff_to_ordinary_fresh_initialization &&
					!fz.public_store_success,
				"FZ normalization did not seal an internal-only handoff");

		auto missing_sync = fz_evidence;
		missing_sync.parent_fsync.terminal_journal_delete = false;
		auto failed =
			complete_sqlite_exact_empty_normalization(closed_empty_receipt, fz_path, missing_sync);
		require(
			!failed.completed() &&
				failed.failure ==
					sqlite_exact_empty_completion_failure::terminal_journal_parent_not_fsynced &&
				!failed.handoff_to_ordinary_fresh_initialization && !failed.public_store_success,
			"terminal journal parent-fsync failure reached normalization handoff");

		auto unexpected_coordination = f0_evidence;
		unexpected_coordination.coordination_wal_deleted = true;
		const auto rejected_extra_effect = complete_sqlite_exact_empty_normalization(
			closed_empty_receipt, f0_path, unexpected_coordination);
		require(!rejected_extra_effect.completed() &&
					rejected_extra_effect.failure ==
						sqlite_exact_empty_completion_failure::forbidden_effect_observed &&
					!rejected_extra_effect.public_store_success,
				"F0 accepted an unbound coordination-WAL delete effect");

		constexpr std::array public_success_path{
			phase::unresolved,
			phase::logical_read_receipt,
			phase::receipt_revalidated,
			phase::effect_profile_capability_sealed,
			phase::exclusive_normalization_owner,
			phase::pre_effect_sealed,
			phase::effect_journal_open,
			phase::journal_created,
			phase::journal_parent_fsynced,
			phase::permitted_callback_effects,
			phase::terminal_journal_deleted,
			phase::terminal_journal_parent_fsynced,
			phase::file_and_parent_durable,
			phase::confirmed_close,
			phase::post_close_census,
			phase::normalization_receipt,
			phase::public_store_success,
		};
		require(!validate_sqlite_exact_empty_normalization_path(public_success_path, false),
				"normalization receipt exposed a public Store success edge");
	}
} // namespace

int main()
{
	check_entry_requires_closed_zero_effect_receipt();
	check_full_effect_requires_all_parent_syncs_and_handoff();
	return 0;
}
