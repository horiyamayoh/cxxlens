#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_disposable_qualification_internal.hpp"

namespace cxxlens::detail::sqlite_qualification
{
	/**
	 * Production-inactive #202 interruption/reclassification witness.
	 *
	 * This vocabulary models only the fail-closed recovery boundary.  It carries no source
	 * capability, effect permission, SQLite result, or Store handoff.  Once an effect attempt is
	 * interrupted, the original receipt is discarded and the durable bytes must be
	 * cold-reclassified; neither reclassification terminal can return to normalization success.
	 */
	enum class sqlite_disposable_normalization_recovery_phase : std::uint8_t
	{
		effect_pre_sealed,
		effect_admitted,
		effect_transcript_sealed,
		durability_barrier_sealed,
		normalization_receipt,
		fresh_init_handoff_candidate,
		recoverable_interruption,
		original_receipt_discarded,
		cold_reclassification_required,
		reclassified_fz_post,
		reclassified_other_family,
		terminal_quarantined,
	};

	inline constexpr std::array sqlite_disposable_normalization_recovery_phases{
		sqlite_disposable_normalization_recovery_phase::effect_pre_sealed,
		sqlite_disposable_normalization_recovery_phase::effect_admitted,
		sqlite_disposable_normalization_recovery_phase::effect_transcript_sealed,
		sqlite_disposable_normalization_recovery_phase::durability_barrier_sealed,
		sqlite_disposable_normalization_recovery_phase::normalization_receipt,
		sqlite_disposable_normalization_recovery_phase::fresh_init_handoff_candidate,
		sqlite_disposable_normalization_recovery_phase::recoverable_interruption,
		sqlite_disposable_normalization_recovery_phase::original_receipt_discarded,
		sqlite_disposable_normalization_recovery_phase::cold_reclassification_required,
		sqlite_disposable_normalization_recovery_phase::reclassified_fz_post,
		sqlite_disposable_normalization_recovery_phase::reclassified_other_family,
		sqlite_disposable_normalization_recovery_phase::terminal_quarantined,
	};

	[[nodiscard]] constexpr bool is_sqlite_disposable_normalization_recovery_transition(
		const sqlite_disposable_normalization_recovery_phase origin,
		const sqlite_disposable_normalization_recovery_phase destination) noexcept
	{
		using phase = sqlite_disposable_normalization_recovery_phase;
		switch (origin)
		{
			case phase::effect_pre_sealed:
				return destination == phase::effect_admitted ||
					destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::effect_admitted:
				return destination == phase::effect_transcript_sealed ||
					destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::effect_transcript_sealed:
				return destination == phase::durability_barrier_sealed ||
					destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::durability_barrier_sealed:
				return destination == phase::normalization_receipt ||
					destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::normalization_receipt:
				return destination == phase::fresh_init_handoff_candidate ||
					destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::recoverable_interruption:
				return destination == phase::original_receipt_discarded ||
					destination == phase::terminal_quarantined;
			case phase::original_receipt_discarded:
				return destination == phase::cold_reclassification_required ||
					destination == phase::terminal_quarantined;
			case phase::cold_reclassification_required:
				return destination == phase::reclassified_fz_post ||
					destination == phase::reclassified_other_family ||
					destination == phase::terminal_quarantined;
			case phase::fresh_init_handoff_candidate:
				return destination == phase::recoverable_interruption ||
					destination == phase::terminal_quarantined;
			case phase::reclassified_fz_post:
			case phase::reclassified_other_family:
			case phase::terminal_quarantined:
				return false;
		}
		return false;
	}

	/** Validate an uninterrupted candidate, a cold-reclassified interruption, or quarantine. */
	[[nodiscard]] constexpr bool validate_sqlite_disposable_normalization_recovery_path(
		const std::span<const sqlite_disposable_normalization_recovery_phase> path) noexcept
	{
		using phase = sqlite_disposable_normalization_recovery_phase;
		if (path.empty() || path.front() != phase::effect_pre_sealed)
			return false;
		for (std::size_t index = 1U; index < path.size(); ++index)
			if (!is_sqlite_disposable_normalization_recovery_transition(path[index - 1U],
																		path[index]))
				return false;
		return path.back() == phase::fresh_init_handoff_candidate ||
			path.back() == phase::reclassified_fz_post ||
			path.back() == phase::reclassified_other_family ||
			path.back() == phase::terminal_quarantined;
	}

	/**
	 * The only physical main-header states admitted by the receiptless qualification family
	 * classifier.  These names describe bytes observed in the current invocation; they never
	 * reconstruct an earlier operation.
	 */
	enum class sqlite_disposable_main_header_state : std::uint8_t
	{
		wal_empty,
		rollback_empty,
	};

	enum class sqlite_disposable_wal_state : std::uint8_t
	{
		absent,
		readable_zero_byte,
		readable_nonzero,
		invalid_or_unknown,
	};

	enum class sqlite_disposable_journal_state : std::uint8_t
	{
		absent,
		nonhot_prefix,
		hot_with_exact_preimages,
		invalidated_with_exact_post,
		invalid_or_unknown,
	};

	/**
	 * A closed, path-free observation supplied by the qualification harness after it has retained
	 * and rechecked the exact main/sidecar objects.  This value is deliberately not constructible
	 * from a public Store locator and carries no effect capability.
	 */
	struct sqlite_disposable_empty_family_observation
	{
		bool source_anchor_stable{};
		bool main_identity_stable{};
		bool main_entry_stable{};
		bool exact_logical_empty{};
		sqlite_disposable_main_header_state main_header{};
		sqlite_disposable_wal_state wal{};
		bool shared_memory_present{};
		sqlite_disposable_journal_state journal{};
		bool other_sidecar_present{};

		[[nodiscard]] bool
		operator==(const sqlite_disposable_empty_family_observation&) const = default;
	};

	enum class sqlite_disposable_empty_family : std::uint8_t
	{
		exact_pre_no_sidecar,
		exact_pre_or_post_zero_wal,
		exact_pre_nonhot_journal_prefix,
		valid_hot_journal_with_exact_preimages,
		invalidated_journal_with_exact_post,
		complete_rollback_empty_no_sidecar,
	};

	enum class sqlite_disposable_family_phase : std::uint8_t
	{
		pre,
		post,
		pre_or_post,
	};

	struct sqlite_disposable_empty_family_receipt
	{
		sqlite_disposable_empty_family family{};
		sqlite_disposable_family_phase phase{};

		[[nodiscard]] bool
		operator==(const sqlite_disposable_empty_family_receipt&) const = default;
	};

	/** A qualification-only next-route disposition; none of these routes is public Store success.
	 */
	enum class sqlite_disposable_normalization_route : std::uint8_t
	{
		start_new_live_receipted_normalizer,
		establish_rollback_empty_anchor,
	};

	struct sqlite_disposable_normalization_plan
	{
		sqlite_disposable_empty_family_receipt family;
		sqlite_disposable_normalization_route route{};
		bool uses_existing_zero_byte_wal{};
		bool may_handoff_to_ordinary_fresh_initialization{};

		[[nodiscard]] bool operator==(const sqlite_disposable_normalization_plan&) const = default;
	};

	/** One held direct regular file observed by the receiptless raw classifier. */
	struct sqlite_disposable_raw_file_observation
	{
		sqlite_disposable_object_identity object;
		sqlite_disposable_object_identity entry;
		std::uint64_t byte_count{};
		std::string sha256;

		[[nodiscard]] bool
		operator==(const sqlite_disposable_raw_file_observation&) const = default;
	};

	/**
	 * Raw, receiptless observation produced only from the retained fixture root. The family value
	 * is derived from current bytes and topology; it does not claim an earlier operation, a
	 * completed normalization edge, or public Store success.
	 */
	struct sqlite_disposable_raw_family_observation
	{
		sqlite_disposable_empty_family_observation observation;
		sqlite_disposable_empty_family_receipt family;
		sqlite_disposable_raw_file_observation main;
		std::optional<sqlite_disposable_raw_file_observation> wal;

		[[nodiscard]] bool
		operator==(const sqlite_disposable_raw_family_observation&) const = default;
	};

	/**
	 * Fixture-only evidence for the one known-name FZ-post cleanup edge.  The two observations are
	 * intentionally retained as raw classifications: this result does not certify a completed
	 * normalization transaction, ordinary fresh initialization, or public Store success.
	 */
	struct sqlite_disposable_fz_post_cleanup_result
	{
		sqlite_disposable_raw_family_observation before;
		sqlite_disposable_raw_family_observation after;

		[[nodiscard]] bool
		operator==(const sqlite_disposable_fz_post_cleanup_result&) const = default;
	};

	/**
	 * Classify exactly one receiptless family.  Ambiguous, mixed, nonempty, unstable, and
	 * unsupported observations return a typed failure and never select a family by precedence
	 * guesswork.
	 */
	[[nodiscard]] cxxlens::sdk::result<sqlite_disposable_empty_family_receipt>
	classify_sqlite_disposable_empty_family(
		const sqlite_disposable_empty_family_observation& observation);

	/**
	 * Select the only qualification route allowed for the classified family.  In particular,
	 * post-form FZ, FI, and FO establish only a new rollback-empty anchor; they never claim that a
	 * prior normalization operation completed.
	 */
	[[nodiscard]] cxxlens::sdk::result<sqlite_disposable_normalization_plan>
	plan_sqlite_disposable_empty_normalization(
		const sqlite_disposable_empty_family_observation& observation);

	/**
	 * Read-only qualification seam for the currently admitted no-journal families (F0/FZ/FO).
	 * It uses only the capability's retained root FD and direct known leaves; SQLite open,
	 * recovery, checkpoint, delete, cleanup, and production activation are deliberately absent.
	 */
	[[nodiscard]] cxxlens::sdk::result<sqlite_disposable_raw_family_observation>
	observe_sqlite_disposable_raw_empty_family(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request);

	/**
	 * Fixture-only FZ-post seam.  After a fresh raw classification and a second exact leaf check,
	 * it removes only the known empty `main-wal` leaf, synchronizes its retained parent, and
	 * reclassifies the remaining main file as the rollback-empty anchor.  Any uncertainty after the
	 * unlink is returned as a typed failure without retry; no SQLite recovery, Store handoff, or
	 * production source activation is performed.
	 */
	[[nodiscard]] cxxlens::sdk::result<sqlite_disposable_fz_post_cleanup_result>
	cleanup_sqlite_disposable_fz_post_wal_for_testing(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request) noexcept;
} // namespace cxxlens::detail::sqlite_qualification
