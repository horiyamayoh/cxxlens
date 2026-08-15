#include "sqlite_disposable_normalization_internal.hpp"

#include <utility>

namespace cxxlens::detail::sqlite_qualification
{
	namespace
	{
		[[nodiscard]] cxxlens::sdk::error family_error(const char* detail)
		{
			return {"store.sqlite-failure", "sqlite-initialization-recovery", detail};
		}

		[[nodiscard]] bool
		exact_empty_topology(const sqlite_disposable_empty_family_observation& observation) noexcept
		{
			return observation.source_anchor_stable && observation.main_identity_stable &&
				observation.main_entry_stable && observation.exact_logical_empty &&
				!observation.shared_memory_present && !observation.other_sidecar_present;
		}

		[[nodiscard]] bool
		no_journal_or_wal(const sqlite_disposable_empty_family_observation& observation) noexcept
		{
			return observation.wal == sqlite_disposable_wal_state::absent &&
				observation.journal == sqlite_disposable_journal_state::absent;
		}
	} // namespace

	cxxlens::sdk::result<sqlite_disposable_empty_family_receipt>
	classify_sqlite_disposable_empty_family(
		const sqlite_disposable_empty_family_observation& observation)
	{
		if (!exact_empty_topology(observation))
			return cxxlens::sdk::unexpected(family_error("unstable-or-not-exact-empty"));

		if (observation.journal == sqlite_disposable_journal_state::invalid_or_unknown ||
			observation.wal == sqlite_disposable_wal_state::invalid_or_unknown)
			return cxxlens::sdk::unexpected(family_error("unrecognized-preauthority-state"));
		if (observation.main_header != sqlite_disposable_main_header_state::wal_empty &&
			observation.main_header != sqlite_disposable_main_header_state::rollback_empty)
			return cxxlens::sdk::unexpected(family_error("unrecognized-preauthority-state"));

		if (no_journal_or_wal(observation))
		{
			if (observation.main_header == sqlite_disposable_main_header_state::wal_empty)
				return sqlite_disposable_empty_family_receipt{
					sqlite_disposable_empty_family::exact_pre_no_sidecar,
					sqlite_disposable_family_phase::pre};
			if (observation.main_header == sqlite_disposable_main_header_state::rollback_empty)
				return sqlite_disposable_empty_family_receipt{
					sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar,
					sqlite_disposable_family_phase::post};
		}

		if (observation.journal == sqlite_disposable_journal_state::absent &&
			observation.wal == sqlite_disposable_wal_state::readable_zero_byte)
		{
			return sqlite_disposable_empty_family_receipt{
				sqlite_disposable_empty_family::exact_pre_or_post_zero_wal,
				observation.main_header == sqlite_disposable_main_header_state::wal_empty
					? sqlite_disposable_family_phase::pre
					: sqlite_disposable_family_phase::post};
		}

		if (observation.wal == sqlite_disposable_wal_state::absent &&
			observation.journal == sqlite_disposable_journal_state::nonhot_prefix &&
			observation.main_header == sqlite_disposable_main_header_state::wal_empty)
		{
			return sqlite_disposable_empty_family_receipt{
				sqlite_disposable_empty_family::exact_pre_nonhot_journal_prefix,
				sqlite_disposable_family_phase::pre};
		}

		if (observation.wal == sqlite_disposable_wal_state::absent &&
			observation.journal == sqlite_disposable_journal_state::hot_with_exact_preimages)
		{
			return sqlite_disposable_empty_family_receipt{
				sqlite_disposable_empty_family::valid_hot_journal_with_exact_preimages,
				sqlite_disposable_family_phase::pre_or_post};
		}

		if (observation.wal == sqlite_disposable_wal_state::absent &&
			observation.journal == sqlite_disposable_journal_state::invalidated_with_exact_post &&
			observation.main_header == sqlite_disposable_main_header_state::rollback_empty)
		{
			return sqlite_disposable_empty_family_receipt{
				sqlite_disposable_empty_family::invalidated_journal_with_exact_post,
				sqlite_disposable_family_phase::post};
		}

		return cxxlens::sdk::unexpected(family_error("unrecognized-preauthority-state"));
	}

	cxxlens::sdk::result<sqlite_disposable_normalization_plan>
	plan_sqlite_disposable_empty_normalization(
		const sqlite_disposable_empty_family_observation& observation)
	{
		auto family = classify_sqlite_disposable_empty_family(observation);
		if (!family)
			return cxxlens::sdk::unexpected(std::move(family.error()));

		const auto route =
			family->family == sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar ||
				(family->family == sqlite_disposable_empty_family::exact_pre_or_post_zero_wal &&
				 family->phase == sqlite_disposable_family_phase::post) ||
				family->family ==
					sqlite_disposable_empty_family::invalidated_journal_with_exact_post
			? sqlite_disposable_normalization_route::establish_rollback_empty_anchor
			: sqlite_disposable_normalization_route::start_new_live_receipted_normalizer;

		return sqlite_disposable_normalization_plan{
			*family,
			route,
			family->family == sqlite_disposable_empty_family::exact_pre_or_post_zero_wal &&
				family->phase == sqlite_disposable_family_phase::pre,
			false};
	}
} // namespace cxxlens::detail::sqlite_qualification
