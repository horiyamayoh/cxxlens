#include "sqlite_wave3_exact_empty_classifier_internal.hpp"

#include <cstddef>
#include <iostream>
#include <stdexcept>

namespace
{
	using namespace cxxlens::sdk;

	sqlite_wave3_exact_empty_observation base()
	{
		return {true,
			true,
			true,
			true,
			sqlite_wave3_empty_main_form::pre,
			sqlite_wave3_empty_wal_state::absent,
			sqlite_wave3_empty_journal_state::absent,
			false,
			false,
			true,
			true};
	}

	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			throw std::runtime_error(message);
		}
	}

	sqlite_wave3_exact_empty_classification classify(
		const sqlite_wave3_exact_empty_observation& observation)
	{
		auto result = classify_sqlite_wave3_exact_empty(observation);
		require(result.has_value(), "expected exact-empty family was rejected");
		return result.value();
	}

	void require_family(const sqlite_wave3_exact_empty_observation& observation,
		const sqlite_wave3_exact_empty_family family,
		const sqlite_wave3_exact_empty_route route)
	{
		const auto value = classify(observation);
		require(value.family == family && value.route == route && value.exact_empty &&
			value.logical_read_receipt_required && !value.effect_profile_capability,
			"family route or capability boundary is wrong");
	}

	void test_seven_family_partition()
	{
		require_family(base(), sqlite_wave3_exact_empty_family::f0,
			sqlite_wave3_exact_empty_route::live_receipted_normalizer);

		auto fz_pre = base();
		fz_pre.wal = sqlite_wave3_empty_wal_state::size_zero;
		require_family(fz_pre, sqlite_wave3_exact_empty_family::fz_pre,
			sqlite_wave3_exact_empty_route::live_receipted_normalizer);

		auto fz_post = fz_pre;
		fz_post.main_form = sqlite_wave3_empty_main_form::post;
		require_family(fz_post, sqlite_wave3_exact_empty_family::fz_post,
			sqlite_wave3_exact_empty_route::fresh_anchor_only);

		auto fp = base();
		fp.journal = sqlite_wave3_empty_journal_state::nonhot_prefix;
		require_family(fp, sqlite_wave3_exact_empty_family::fp,
			sqlite_wave3_exact_empty_route::cleanup_then_revalidate_f0);

		auto fh = base();
		fh.journal = sqlite_wave3_empty_journal_state::hot_exact_preimages;
		require_family(fh, sqlite_wave3_exact_empty_family::fh,
			sqlite_wave3_exact_empty_route::cleanup_then_revalidate_f0);
		fh.main_form = sqlite_wave3_empty_main_form::post;
		require_family(fh, sqlite_wave3_exact_empty_family::fh,
			sqlite_wave3_exact_empty_route::cleanup_then_revalidate_f0);

		auto fi = base();
		fi.main_form = sqlite_wave3_empty_main_form::post;
		fi.journal = sqlite_wave3_empty_journal_state::invalidated_exact_post;
		require_family(fi, sqlite_wave3_exact_empty_family::fi,
			sqlite_wave3_exact_empty_route::fresh_anchor_only);

		auto fo = base();
		fo.main_form = sqlite_wave3_empty_main_form::post;
		require_family(fo, sqlite_wave3_exact_empty_family::fo,
			sqlite_wave3_exact_empty_route::fresh_anchor_only);
}

	void require_rejected(const sqlite_wave3_exact_empty_observation& observation,
		const char* expected_detail)
	{
		auto result = classify_sqlite_wave3_exact_empty(observation);
		require(!result && result.error().detail == expected_detail,
			"invalid exact-empty combination was admitted");
	}

	void test_negative_partition()
	{
		auto missing = base();
		missing.main_identity_valid = false;
		require_rejected(missing, "main-identity-not-sealed");
		auto nonempty = base();
		nonempty.main_exact_empty = false;
		require_rejected(nonempty, "main-not-exact-empty");
		auto unstable = base();
		unstable.namespace_stable = false;
		require_rejected(unstable, "source-epoch-not-stable");
		auto shm = base();
		shm.shm_present = true;
		require_rejected(shm, "sidecar-ambiguous");
		auto unknown_form = base();
		unknown_form.main_form = sqlite_wave3_empty_main_form::unknown;
		require_rejected(unknown_form, "main-form-unknown");
		auto mixed = base();
		mixed.wal = sqlite_wave3_empty_wal_state::nonzero;
		mixed.journal = sqlite_wave3_empty_journal_state::hot_exact_preimages;
		require_rejected(mixed, "wal-journal-mixed");
		auto nonzero = base();
		nonzero.wal = sqlite_wave3_empty_wal_state::nonzero;
		require_rejected(nonzero, "nonzero-wal-not-empty");
		auto zero_journal = base();
		zero_journal.wal = sqlite_wave3_empty_wal_state::size_zero;
		zero_journal.journal = sqlite_wave3_empty_journal_state::nonhot_prefix;
		require_rejected(zero_journal, "wal-journal-mixed");
		auto post_prefix = base();
		post_prefix.main_form = sqlite_wave3_empty_main_form::post;
		post_prefix.journal = sqlite_wave3_empty_journal_state::nonhot_prefix;
		require_rejected(post_prefix, "nonhot-journal-post-form");
		auto pre_invalidated = base();
		pre_invalidated.journal = sqlite_wave3_empty_journal_state::invalidated_exact_post;
		require_rejected(pre_invalidated, "invalidated-journal-pre-form");
		auto invalid = base();
		invalid.wal = sqlite_wave3_empty_wal_state::invalid;
		require_rejected(invalid, "sidecar-state-invalid");
	}

	void test_determinism()
	{
		auto observation = base();
		observation.journal = sqlite_wave3_empty_journal_state::hot_exact_preimages;
		const auto expected = classify(observation);
		for (int count = 0; count != 10000; ++count)
		{
			require(classify(observation).family == expected.family &&
				classify(observation).route == expected.route,
				"classifier result is not deterministic");
		}
	}
} // namespace

int main()
{
	try
	{
		test_seven_family_partition();
		test_negative_partition();
		test_determinism();
	}
	catch (const std::exception& exception)
	{
		std::cerr << "sqlite_wave3_exact_empty_classifier_test: " << exception.what() << '\n';
		return 1;
	}
	return 0;
}
