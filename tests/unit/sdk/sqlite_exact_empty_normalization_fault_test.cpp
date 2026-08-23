#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>

#include "sdk/sqlite_exact_empty_normalization_internal.hpp"

namespace
{
	using namespace cxxlens::detail::sqlite_qualification;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void check_callback_boundary_crashes_never_resume()
	{
		constexpr std::array boundaries{
			sqlite_exact_empty_normalization_fault_boundary::before_effect,
			sqlite_exact_empty_normalization_fault_boundary::effect_journal_open,
			sqlite_exact_empty_normalization_fault_boundary::journal_parent_fsync,
			sqlite_exact_empty_normalization_fault_boundary::main_write,
			sqlite_exact_empty_normalization_fault_boundary::terminal_journal_delete,
			sqlite_exact_empty_normalization_fault_boundary::after_normalization_receipt,
		};
		for (const auto boundary : boundaries)
		{
			const auto result = evaluate_sqlite_exact_empty_normalization_fault(
				{boundary, sqlite_exact_empty_normalization_fault_kind::process_crash});
			require(
				result.outcome ==
						sqlite_exact_empty_normalization_fault_outcome::recoverable_interruption &&
					result.terminal_phase ==
						sqlite_exact_empty_normalization_phase::recoverable_interruption &&
					!result.may_resume && !result.normalization_receipt &&
					!result.public_store_success && result.requires_cold_family_classifier,
				"callback-boundary crash was resumable or reported success");
		}
	}

	void check_fsync_and_rebind_faults_are_terminal()
	{
		constexpr std::array fsync_boundaries{
			sqlite_exact_empty_normalization_fault_boundary::coordination_wal_parent_fsync,
			sqlite_exact_empty_normalization_fault_boundary::journal_parent_fsync,
			sqlite_exact_empty_normalization_fault_boundary::terminal_journal_parent_fsync,
		};
		for (const auto boundary : fsync_boundaries)
		{
			for (const auto kind : {sqlite_exact_empty_normalization_fault_kind::fsync_failure,
									sqlite_exact_empty_normalization_fault_kind::fsync_unknown})
			{
				const auto result =
					evaluate_sqlite_exact_empty_normalization_fault({boundary, kind});
				require(result.outcome ==
								sqlite_exact_empty_normalization_fault_outcome::durability_opaque &&
							result.terminal_phase ==
								sqlite_exact_empty_normalization_phase::durability_opaque &&
							!result.may_resume && !result.normalization_receipt &&
							!result.public_store_success && result.requires_cold_family_classifier,
						"parent-directory fsync fault was not durability-opaque");
			}
		}

		const auto rebound = evaluate_sqlite_exact_empty_normalization_fault(
			{sqlite_exact_empty_normalization_fault_boundary::coordination_wal_delete,
			 sqlite_exact_empty_normalization_fault_kind::identity_rebind});
		require(rebound.outcome == sqlite_exact_empty_normalization_fault_outcome::quarantined &&
					!rebound.may_resume && !rebound.normalization_receipt &&
					!rebound.public_store_success,
				"delete rebind fault was allowed to continue or claim success");

		const auto close = evaluate_sqlite_exact_empty_normalization_fault(
			{sqlite_exact_empty_normalization_fault_boundary::confirmed_close,
			 sqlite_exact_empty_normalization_fault_kind::close_unknown});
		require(close.outcome == sqlite_exact_empty_normalization_fault_outcome::quarantined &&
					!close.may_resume && !close.normalization_receipt &&
					!close.public_store_success,
				"unknown close outcome was not quarantined");

		const auto invalid = evaluate_sqlite_exact_empty_normalization_fault(
			{static_cast<sqlite_exact_empty_normalization_fault_boundary>(255U),
			 static_cast<sqlite_exact_empty_normalization_fault_kind>(255U)});
		require(invalid.outcome == sqlite_exact_empty_normalization_fault_outcome::quarantined &&
					!invalid.may_resume && !invalid.normalization_receipt &&
					!invalid.public_store_success,
				"unknown fault enum was allowed to continue");
	}

	void check_recrash_discards_in_memory_phase()
	{
		sqlite_exact_empty_classifier_observation post{
			true,
			true,
			true,
			true,
			true,
			true,
			sqlite_exact_empty_main_form::rollback_post,
			sqlite_exact_empty_wal_form::zero_byte,
			false,
			sqlite_exact_empty_journal_form::absent,
			false,
		};
		const auto result = reclassify_sqlite_exact_empty_after_recrash(post);
		require(result.classification.accepted() &&
					result.classification.classification.family ==
						sqlite_exact_empty_family::fz_post &&
					result.classification.classification.route ==
						sqlite_exact_empty_family_route::rollback_empty_fresh_anchor_only &&
					!result.resumed_in_memory_phase && !result.normalization_receipt &&
					!result.public_store_success,
				"recrash resumed the original phase or inferred a normalization edge");

		const auto recrash = evaluate_sqlite_exact_empty_normalization_fault(
			{sqlite_exact_empty_normalization_fault_boundary::crash_recovery,
			 sqlite_exact_empty_normalization_fault_kind::recrash});
		require(
			recrash.outcome ==
					sqlite_exact_empty_normalization_fault_outcome::recrash_cold_reclassification &&
				recrash.terminal_phase ==
					sqlite_exact_empty_normalization_phase::cold_family_reclassified &&
				!recrash.may_resume && !recrash.normalization_receipt &&
				!recrash.public_store_success,
			"recrash fault was not a cold classifier terminal");
	}
} // namespace

int main()
{
	check_callback_boundary_crashes_never_resume();
	check_fsync_and_rebind_faults_are_terminal();
	check_recrash_discards_in_memory_phase();
	return 0;
}
