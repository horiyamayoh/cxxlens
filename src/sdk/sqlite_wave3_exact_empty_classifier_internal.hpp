#pragma once

#include <cstdint>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	/** Exact empty main-byte form; pre is WAL-header 2/2 and post is rollback-header 1/1. */
	enum class sqlite_wave3_empty_main_form : std::uint8_t
	{
		unknown,
		pre,
		post,
	};

	/** Typed WAL census used by the closed seven-family partition. */
	enum class sqlite_wave3_empty_wal_state : std::uint8_t
	{
		absent,
		size_zero,
		nonzero,
		invalid,
	};

	/** Typed rollback-journal census used by the closed seven-family partition. */
	enum class sqlite_wave3_empty_journal_state : std::uint8_t
	{
		absent,
		nonhot_prefix,
		hot_exact_preimages,
		invalidated_exact_post,
		invalid,
	};

	/** Cold physical facts; this object grants no logical-read or normalization capability. */
	struct sqlite_wave3_exact_empty_observation
	{
		bool main_present{};
		bool main_regular{};
		bool main_identity_valid{};
		bool main_exact_empty{};
		sqlite_wave3_empty_main_form main_form{sqlite_wave3_empty_main_form::unknown};
		sqlite_wave3_empty_wal_state wal{sqlite_wave3_empty_wal_state::invalid};
		sqlite_wave3_empty_journal_state journal{sqlite_wave3_empty_journal_state::invalid};
		bool shm_present{};
		bool extra_sidecar_present{};
		bool namespace_stable{};
		bool source_epoch_valid{};
	};

	enum class sqlite_wave3_exact_empty_family : std::uint8_t
	{
		unresolved,
		f0,
		fz_pre,
		fz_post,
		fp,
		fh,
		fi,
		fo,
	};

	/** Route after cold classification; cleanup/recovery routes must reclassify before success. */
	enum class sqlite_wave3_exact_empty_route : std::uint8_t
	{
		reject,
		live_receipted_normalizer,
		cleanup_then_revalidate_f0,
		fresh_anchor_only,
	};

	struct sqlite_wave3_exact_empty_classification
	{
		sqlite_wave3_exact_empty_family family{sqlite_wave3_exact_empty_family::unresolved};
		sqlite_wave3_exact_empty_route route{sqlite_wave3_exact_empty_route::reject};
		bool exact_empty{};
		/** #201 must independently seal this receipt before any #202 effect entry. */
		bool logical_read_receipt_required{true};
		/** This classifier never mints the #202 effect capability. */
		bool effect_profile_capability{false};
	};

	/** Pure closed-partition classifier. */
	[[nodiscard]] result<sqlite_wave3_exact_empty_classification>
	classify_sqlite_wave3_exact_empty(const sqlite_wave3_exact_empty_observation& observation);
} // namespace cxxlens::sdk
