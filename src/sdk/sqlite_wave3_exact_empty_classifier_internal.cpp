#include "sqlite_wave3_exact_empty_classifier_internal.hpp"

#include <string_view>

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::string_view error_code = "store.sqlite-failure";
		constexpr std::string_view error_field = "sqlite-wave3-exact-empty";

		[[nodiscard]] error classifier_error(const std::string_view detail)
		{
			return {std::string{error_code}, std::string{error_field}, std::string{detail}};
		}

		[[nodiscard]] result<sqlite_wave3_exact_empty_classification>
		reject(const std::string_view detail)
		{
			return unexpected(classifier_error(detail));
		}

		[[nodiscard]] sqlite_wave3_exact_empty_classification
		live(const sqlite_wave3_exact_empty_family family)
		{
			return {family,
					sqlite_wave3_exact_empty_route::live_receipted_normalizer,
					true,
					true,
					false};
		}

		[[nodiscard]] sqlite_wave3_exact_empty_classification
		cleanup(const sqlite_wave3_exact_empty_family family)
		{
			return {family,
					sqlite_wave3_exact_empty_route::cleanup_then_revalidate_f0,
					true,
					true,
					false};
		}

		[[nodiscard]] sqlite_wave3_exact_empty_classification
		anchor(const sqlite_wave3_exact_empty_family family)
		{
			return {family, sqlite_wave3_exact_empty_route::fresh_anchor_only, true, true, false};
		}
	} // namespace

	result<sqlite_wave3_exact_empty_classification>
	classify_sqlite_wave3_exact_empty(const sqlite_wave3_exact_empty_observation& observation)
	{
		if (!observation.main_present || !observation.main_regular ||
			!observation.main_identity_valid)
		{
			return reject("main-identity-not-sealed");
		}
		if (!observation.main_exact_empty)
		{
			return reject("main-not-exact-empty");
		}
		if (!observation.namespace_stable || !observation.source_epoch_valid)
		{
			return reject("source-epoch-not-stable");
		}
		if (observation.shm_present || observation.extra_sidecar_present)
		{
			return reject("sidecar-ambiguous");
		}
		if (observation.main_form == sqlite_wave3_empty_main_form::unknown)
		{
			return reject("main-form-unknown");
		}
		if (observation.wal == sqlite_wave3_empty_wal_state::invalid ||
			observation.journal == sqlite_wave3_empty_journal_state::invalid)
		{
			return reject("sidecar-state-invalid");
		}

		// WAL and rollback journal are mutually exclusive in every accepted family.
		if (observation.wal != sqlite_wave3_empty_wal_state::absent &&
			observation.journal != sqlite_wave3_empty_journal_state::absent)
		{
			return reject("wal-journal-mixed");
		}
		if (observation.wal == sqlite_wave3_empty_wal_state::nonzero)
		{
			return reject("nonzero-wal-not-empty");
		}

		if (observation.wal == sqlite_wave3_empty_wal_state::size_zero)
		{
			if (observation.journal != sqlite_wave3_empty_journal_state::absent)
			{
				return reject("zero-wal-journal-mixed");
			}
			return observation.main_form == sqlite_wave3_empty_main_form::pre
				? result<sqlite_wave3_exact_empty_classification>{{sqlite_wave3_exact_empty_family::
																	   fz_pre,
																   sqlite_wave3_exact_empty_route::
																	   live_receipted_normalizer,
																   true,
																   true,
																   false}}
				: result<sqlite_wave3_exact_empty_classification>{
					  {sqlite_wave3_exact_empty_family::fz_post,
					   sqlite_wave3_exact_empty_route::fresh_anchor_only,
					   true,
					   true,
					   false}};
		}

		if (observation.wal != sqlite_wave3_empty_wal_state::absent)
		{
			return reject("wal-state-not-closed");
		}
		switch (observation.journal)
		{
			case sqlite_wave3_empty_journal_state::absent:
				return observation.main_form == sqlite_wave3_empty_main_form::pre
					? result<sqlite_wave3_exact_empty_classification>{live(
						  sqlite_wave3_exact_empty_family::f0)}
					: result<sqlite_wave3_exact_empty_classification>{
						  anchor(sqlite_wave3_exact_empty_family::fo)};
			case sqlite_wave3_empty_journal_state::nonhot_prefix:
				if (observation.main_form != sqlite_wave3_empty_main_form::pre)
				{
					return reject("nonhot-journal-post-form");
				}
				return cleanup(sqlite_wave3_exact_empty_family::fp);
			case sqlite_wave3_empty_journal_state::hot_exact_preimages:
				return cleanup(sqlite_wave3_exact_empty_family::fh);
			case sqlite_wave3_empty_journal_state::invalidated_exact_post:
				if (observation.main_form != sqlite_wave3_empty_main_form::post)
				{
					return reject("invalidated-journal-pre-form");
				}
				return anchor(sqlite_wave3_exact_empty_family::fi);
			case sqlite_wave3_empty_journal_state::invalid:
				return reject("sidecar-state-invalid");
		}
		return reject("classifier-state-invalid");
	}
} // namespace cxxlens::sdk
