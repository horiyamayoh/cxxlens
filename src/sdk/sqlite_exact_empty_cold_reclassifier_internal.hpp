#pragma once

#include <cstdint>
#include <optional>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/** The closed seven-way partition of retained exact-empty cold bytes. */
	enum class sqlite_exact_empty_cold_family : std::uint8_t
	{
		f0,
		fz_pre,
		fz_post,
		fp,
		fh,
		fi,
		fo,
	};

	/** The exact current-byte main-header form observed by the cold classifier. */
	enum class sqlite_exact_empty_cold_main_form : std::uint8_t
	{
		pre,
		post,
	};

	/** The retained sidecar shape observed beside the exact-empty main bytes. */
	enum class sqlite_exact_empty_cold_sidecar_kind : std::uint8_t
	{
		none,
		zero_wal,
		journal_prefix,
		hot_journal,
		invalidated_journal,
	};

	/** The next internal observation route suggested by a cold byte partition. */
	enum class sqlite_exact_empty_cold_route : std::uint8_t
	{
		live_normalizer,
		cleanup_then_fresh_read,
		fresh_rollback_read,
	};

	/** Why the retained observation was not admitted to the seven-way partition. */
	enum class sqlite_exact_empty_cold_failure : std::uint8_t
	{
		none,
		invalid_input,
		unsupported_profile,
		entry_not_retained,
		shared_memory_present,
		orphan_sidecar,
		mixed_sidecars,
		current_observation_changed,
		main_not_exact_empty,
		ordinary_wal_only,
		unknown_journal,
		ambiguous_journal,
		size_limit,
		arithmetic_overflow,
		resource_limit,
		read_failure,
	};

	/**
	 * A bounded, observation-only description of current durable bytes.
	 *
	 * This value contains no object identity, capability, authority, receipt, effect, cleanup
	 * decision, or historical fact.  It is a parser result only; a later authenticated producer
	 * must independently establish any #201/#202 authority before an effect can be attempted.
	 */
	struct sqlite_exact_empty_cold_observation
	{
		sqlite_exact_empty_cold_family family{};
		sqlite_exact_empty_cold_route route{};
		sqlite_exact_empty_cold_main_form main_form{};
		sqlite_exact_empty_cold_sidecar_kind sidecar{};
		std::uint64_t main_byte_count{};
		std::uint64_t sidecar_byte_count{};
		std::uint32_t page_size{};
		std::uint32_t page_count{};
		std::uint32_t sector_size{};
		std::uint32_t journal_record_count{};
	};

	/**
	 * Total classification result.  `observation` is engaged only for
	 * F0/FZ-pre/FZ-post/FP/FH/FI/FO; every other state is fail-closed and carries only a reason.
	 */
	struct sqlite_exact_empty_cold_classification
	{
		sqlite_exact_empty_cold_failure failure{sqlite_exact_empty_cold_failure::invalid_input};
		std::optional<sqlite_exact_empty_cold_observation> observation;
	};

	/**
	 * Partition one retained current namespace observation without opening SQLite or changing the
	 * filesystem.  The census is consumed only as an observation source; this function never calls
	 * claim/finish on its namespace guard and cannot mint or return a capability or receipt.
	 */
	[[nodiscard]] result<sqlite_exact_empty_cold_classification>
	classify_sqlite_exact_empty_cold_observation(
		const sqlite_backend_namespace_census& source_census,
		std::uint64_t maximum_main_bytes = std::uint64_t{512U} * 1024U * 1024U);
} // namespace cxxlens::sdk
