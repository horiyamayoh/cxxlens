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

	[[nodiscard]] sqlite_exact_empty_classifier_observation observation(
		const sqlite_exact_empty_main_form main,
		const sqlite_exact_empty_wal_form wal = sqlite_exact_empty_wal_form::absent,
		const sqlite_exact_empty_journal_form journal = sqlite_exact_empty_journal_form::absent)
	{
		return {true, true, true, true, true, true, main, wal, false, journal, false};
	}

	void check_disjoint_family_partition()
	{
		const auto f0 =
			classify_sqlite_exact_empty_family(observation(sqlite_exact_empty_main_form::wal_pre));
		require(f0.accepted() && f0.classification.family == sqlite_exact_empty_family::f0 &&
					f0.classification.phase == sqlite_exact_empty_family_phase::pre &&
					f0.classification.route ==
						sqlite_exact_empty_family_route::live_receipt_normalizer,
				"F0 was not classified as a pre-form live-receipt candidate");

		const auto fz_pre = classify_sqlite_exact_empty_family(observation(
			sqlite_exact_empty_main_form::wal_pre, sqlite_exact_empty_wal_form::zero_byte));
		require(fz_pre.accepted() &&
					fz_pre.classification.family == sqlite_exact_empty_family::fz_pre &&
					fz_pre.classification.route ==
						sqlite_exact_empty_family_route::live_receipt_normalizer,
				"FZ-pre did not retain the exact zero-byte WAL coordination route");

		const auto fz_post = classify_sqlite_exact_empty_family(observation(
			sqlite_exact_empty_main_form::rollback_post, sqlite_exact_empty_wal_form::zero_byte));
		require(fz_post.accepted() &&
					fz_post.classification.family == sqlite_exact_empty_family::fz_post &&
					fz_post.classification.phase == sqlite_exact_empty_family_phase::post &&
					fz_post.classification.route ==
						sqlite_exact_empty_family_route::rollback_empty_fresh_anchor_only,
				"FZ-post inferred a normalizer continuation instead of a fresh anchor");

		const auto fp = classify_sqlite_exact_empty_family(
			observation(sqlite_exact_empty_main_form::wal_pre,
						sqlite_exact_empty_wal_form::absent,
						sqlite_exact_empty_journal_form::nonhot_prefix));
		require(fp.accepted() && fp.classification.family == sqlite_exact_empty_family::fp &&
					fp.classification.route ==
						sqlite_exact_empty_family_route::cleanup_or_recovery_then_f0,
				"FP did not require cleanup/recovery and independent F0 validation");

		const auto fh = classify_sqlite_exact_empty_family(
			observation(sqlite_exact_empty_main_form::rollback_post,
						sqlite_exact_empty_wal_form::absent,
						sqlite_exact_empty_journal_form::hot_with_exact_preimages));
		require(fh.accepted() && fh.classification.family == sqlite_exact_empty_family::fh &&
					fh.classification.phase == sqlite_exact_empty_family_phase::pre_or_post &&
					fh.classification.route ==
						sqlite_exact_empty_family_route::cleanup_or_recovery_then_f0,
				"FH did not preserve the pre-or-post hot-journal recovery boundary");

		const auto fi = classify_sqlite_exact_empty_family(
			observation(sqlite_exact_empty_main_form::rollback_post,
						sqlite_exact_empty_wal_form::absent,
						sqlite_exact_empty_journal_form::invalidated_with_exact_post));
		require(fi.accepted() && fi.classification.family == sqlite_exact_empty_family::fi &&
					fi.classification.route ==
						sqlite_exact_empty_family_route::rollback_empty_fresh_anchor_only,
				"FI was treated as a completed normalization edge");

		const auto fo = classify_sqlite_exact_empty_family(
			observation(sqlite_exact_empty_main_form::rollback_post));
		require(fo.accepted() && fo.classification.family == sqlite_exact_empty_family::fo &&
					fo.classification.route ==
						sqlite_exact_empty_family_route::rollback_empty_fresh_anchor_only,
				"FO did not remain a rollback-empty fresh anchor");
	}

	void check_receiptless_rejections()
	{
		auto unstable = observation(sqlite_exact_empty_main_form::wal_pre);
		unstable.main_identity_stable = false;
		require(classify_sqlite_exact_empty_family(unstable).failure ==
					sqlite_exact_empty_classifier_failure::unstable_or_not_exact_empty,
				"main identity drift was accepted as a cold family");

		auto mixed = observation(sqlite_exact_empty_main_form::wal_pre,
								 sqlite_exact_empty_wal_form::zero_byte);
		mixed.journal = sqlite_exact_empty_journal_form::nonhot_prefix;
		require(classify_sqlite_exact_empty_family(mixed).failure ==
					sqlite_exact_empty_classifier_failure::mixed_sidecars,
				"WAL plus journal mixed topology was accepted");

		auto extra = observation(sqlite_exact_empty_main_form::wal_pre);
		extra.other_sidecar_present = true;
		require(classify_sqlite_exact_empty_family(extra).failure ==
					sqlite_exact_empty_classifier_failure::mixed_sidecars,
				"extra sidecar was ignored");

		const auto ordinary_wal = classify_sqlite_exact_empty_family(observation(
			sqlite_exact_empty_main_form::wal_pre, sqlite_exact_empty_wal_form::nonzero));
		require(ordinary_wal.failure == sqlite_exact_empty_classifier_failure::ordinary_wal_only,
				"nonzero WAL-only input was routed into exact-empty normalization");

		auto orphan = observation(sqlite_exact_empty_main_form::wal_pre);
		orphan.main_present = false;
		orphan.wal = sqlite_exact_empty_wal_form::zero_byte;
		require(classify_sqlite_exact_empty_family(orphan).failure ==
					sqlite_exact_empty_classifier_failure::orphan_sidecar,
				"main-absent sidecar was accepted as a family");

		auto nonempty = observation(sqlite_exact_empty_main_form::wal_pre);
		nonempty.exact_logical_empty = false;
		require(classify_sqlite_exact_empty_family(nonempty).failure ==
					sqlite_exact_empty_classifier_failure::unstable_or_not_exact_empty,
				"nonempty source entered the exact-empty classifier");
	}
} // namespace

int main()
{
	check_disjoint_family_partition();
	check_receiptless_rejections();
	return 0;
}
