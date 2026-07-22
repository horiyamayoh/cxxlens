#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	/** Closed Store operations which may enter SQLite terminal recovery. */
	enum class sqlite_store_operation : std::uint8_t
	{
		fresh_initialization,
		publish,
		migrate_predecessor,
		compact_current,
		wal_recovery_handoff,
	};

	/** The operation phase selected before a terminal database class is interpreted. */
	enum class sqlite_terminal_phase : std::uint8_t
	{
		pre_effect,
		journal_transition,
		precommit,
		commit_outcome_unknown,
		successful_handoff,
	};

	/** Typed cause which routed an operation into terminal handling. */
	enum class sqlite_terminal_cause : std::uint8_t
	{
		triggering_failure,
		rollback_uncertain,
		finalization_uncertain,
		close_non_ok_or_unknown,
		terminal_observation_failure,
		main_identity_changed,
		reopen_failure,
	};

	/**
	 * Closed result of the receipt-aware terminal database classifier.
	 *
	 * The operation-edge distinction is authority, not reporting decoration: migration and
	 * compaction may recover success only when the independently validated descendant proof
	 * contains their required edge.
	 */
	enum class sqlite_terminal_class : std::uint8_t
	{
		not_classified,
		exact_logical_empty,
		authorized_pre_state,
		authorized_post_state_without_operation_edge,
		authorized_post_state_with_operation_edge,
		valid_non_descendant,
		invalid_census,
		mixed_or_ambiguous,
		reclassifier_unavailable,
	};

	/** Public result selected only after operation, phase, cause, and class are fixed. */
	enum class sqlite_terminal_public_result : std::uint8_t
	{
		recovered_success,
		original_trigger,
		database_opaque,
		initialization_recovery_opaque,
		initialization_concurrent_authority_change,
		initialization_partial_or_mixed,
		migration_recovery_opaque,
		migration_concurrent_authority_change,
		migration_unexpected_census,
		migration_mixed_or_ambiguous,
		compaction_recovery_opaque,
		compaction_concurrent_authority_change,
		compaction_unexpected_census,
		compaction_mixed_or_ambiguous,
		sqlite_recovery_opaque,
		recovery_handoff_opaque,
	};

	/** Store-process effect paired with the selected public result. */
	enum class sqlite_terminal_state_effect : std::uint8_t
	{
		preserve_last_validated,
		install_authorized_state,
		poison_result_operations,
		return_no_store,
	};

	struct sqlite_terminal_context
	{
		sqlite_store_operation operation{sqlite_store_operation::publish};
		sqlite_terminal_phase phase{sqlite_terminal_phase::pre_effect};
		sqlite_terminal_cause cause{sqlite_terminal_cause::triggering_failure};
		sqlite_terminal_class terminal_class{sqlite_terminal_class::not_classified};

		[[nodiscard]] bool operator==(const sqlite_terminal_context&) const = default;
	};

	struct sqlite_terminal_resolution
	{
		sqlite_terminal_public_result public_result{sqlite_terminal_public_result::database_opaque};
		sqlite_terminal_state_effect state_effect{
			sqlite_terminal_state_effect::poison_result_operations};

		[[nodiscard]] bool operator==(const sqlite_terminal_resolution&) const = default;
	};

	/** Last independently validated compatibility tuple, without the fixed SQLite backend name. */
	struct sqlite_last_validated_compatibility
	{
		semantic_version readable_format;
		bool direct_open{};
		bool migration_required{};

		[[nodiscard]] bool operator==(const sqlite_last_validated_compatibility&) const = default;
	};

	enum class sqlite_store_availability : std::uint8_t
	{
		available,
		recovery_handoff_pending,
		poisoned,
	};

	/** Non-result observers which remain available after Store result operations are poisoned. */
	struct sqlite_store_nonresult_observation
	{
		sqlite_last_validated_compatibility compatibility;
		std::size_t retained_generation_count{};

		[[nodiscard]] bool operator==(const sqlite_store_nonresult_observation&) const = default;
	};

	/**
	 * Allocation-free policy state for result-operation availability and observer preservation.
	 *
	 * Poison is terminal for one Store instance. Installing an independently validated state is
	 * possible while available or during an authorized handoff, but never resurrects a poisoned
	 * instance.
	 */
#if defined(__GNUC__) || defined(__clang__)
	class __attribute__((visibility("default"))) sqlite_store_availability_policy
#else
	class sqlite_store_availability_policy
#endif
	{
	  public:
		explicit sqlite_store_availability_policy(
			sqlite_last_validated_compatibility compatibility) noexcept;

		[[nodiscard]] sqlite_store_availability availability() const noexcept;
		[[nodiscard]] bool result_operations_available() const noexcept;
		[[nodiscard]] bool begin_recovery_handoff() noexcept;
		[[nodiscard]] bool restore_available_after_confirmed_no_effect() noexcept;
		[[nodiscard]] bool
		install_independently_validated(sqlite_last_validated_compatibility compatibility) noexcept;
		void poison() noexcept;
		[[nodiscard]] sqlite_store_nonresult_observation
		observe(std::size_t live_preexisting_generation_count) const noexcept;

	  private:
		sqlite_last_validated_compatibility last_validated_;
		sqlite_store_availability availability_{sqlite_store_availability::available};
	};

	/** Fail-closed phase-specific terminal resolution. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	sqlite_terminal_resolution
	resolve_sqlite_terminal(const sqlite_terminal_context& context) noexcept;

	/** Convert the already-selected typed public result to the exact SDK error tuple. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	std::optional<error> map_sqlite_terminal_public_error(sqlite_terminal_public_result selected,
														  const error& original_trigger);

	/** Exact failure returned by every blocked result operation on a poisoned Store. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	error sqlite_store_reopen_required_error();
} // namespace cxxlens::sdk
