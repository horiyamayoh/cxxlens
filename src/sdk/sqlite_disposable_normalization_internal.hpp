#pragma once

#include <cstdint>
#include <optional>
#include <string>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_disposable_qualification_internal.hpp"

namespace cxxlens::detail::sqlite_qualification
{
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

} // namespace cxxlens::detail::sqlite_qualification
