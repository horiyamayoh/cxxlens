#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace cxxlens::detail::sqlite_qualification
{
	/**
	 * Receiptless physical forms for the disposable DF-0202 classifier.
	 *
	 * These values describe bytes observed during the current cold census.  They do not encode an
	 * operation history and, in particular, `post` never means that a normalizer completed.
	 */
	enum class sqlite_exact_empty_main_form : std::uint8_t
	{
		wal_pre,
		rollback_post,
		invalid,
	};

	enum class sqlite_exact_empty_wal_form : std::uint8_t
	{
		absent,
		zero_byte,
		nonzero,
		invalid_or_unknown,
	};

	enum class sqlite_exact_empty_journal_form : std::uint8_t
	{
		absent,
		nonhot_prefix,
		hot_with_exact_preimages,
		invalidated_with_exact_post,
		invalid_or_unknown,
	};

	/** A closed, receiptless filesystem observation supplied by the disposable fixture harness. */
	struct sqlite_exact_empty_classifier_observation
	{
		bool main_present{};
		bool main_regular{};
		bool source_anchor_stable{};
		bool main_identity_stable{};
		bool main_entry_stable{};
		bool exact_logical_empty{};
		sqlite_exact_empty_main_form main_form{sqlite_exact_empty_main_form::invalid};
		sqlite_exact_empty_wal_form wal{sqlite_exact_empty_wal_form::absent};
		bool shared_memory_present{};
		sqlite_exact_empty_journal_form journal{sqlite_exact_empty_journal_form::absent};
		bool other_sidecar_present{};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_classifier_observation&) const = default;
	};

	/** The finite receiptless family partition from DF-0202. */
	enum class sqlite_exact_empty_family : std::uint8_t
	{
		f0,
		fz_pre,
		fz_post,
		fp,
		fh,
		fi,
		fo,
	};

	enum class sqlite_exact_empty_family_phase : std::uint8_t
	{
		pre,
		post,
		pre_or_post,
	};

	/** A classifier result is a candidate only; no route below is Store success. */
	enum class sqlite_exact_empty_family_route : std::uint8_t
	{
		live_receipt_normalizer,
		cleanup_or_recovery_then_f0,
		rollback_empty_fresh_anchor_only,
	};

	enum class sqlite_exact_empty_classifier_failure : std::uint8_t
	{
		none,
		unstable_or_not_exact_empty,
		orphan_sidecar,
		mixed_sidecars,
		unknown_main_form,
		unknown_sidecar_form,
		ordinary_wal_only,
		unsupported_family,
	};

	struct sqlite_exact_empty_family_classification
	{
		sqlite_exact_empty_family family{sqlite_exact_empty_family::f0};
		sqlite_exact_empty_family_phase phase{sqlite_exact_empty_family_phase::pre};
		sqlite_exact_empty_family_route route{
			sqlite_exact_empty_family_route::live_receipt_normalizer};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_family_classification&) const = default;
	};

	struct sqlite_exact_empty_classifier_result
	{
		sqlite_exact_empty_classifier_failure failure{sqlite_exact_empty_classifier_failure::none};
		sqlite_exact_empty_family_classification classification{};

		[[nodiscard]] constexpr bool accepted() const noexcept
		{
			return failure == sqlite_exact_empty_classifier_failure::none;
		}

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_classifier_result&) const = default;
	};

	/**
	 * Classify a cold observation without SQLite open/recovery/checkpoint, sidecar cleanup, or
	 * operation-history inference.  The order intentionally handles the journal-only partition
	 * before generic sidecar rejection and splits FZ by the exact current main bytes.
	 */
	[[nodiscard]] constexpr sqlite_exact_empty_classifier_result classify_sqlite_exact_empty_family(
		const sqlite_exact_empty_classifier_observation& observation) noexcept
	{
		using failure = sqlite_exact_empty_classifier_failure;
		using family = sqlite_exact_empty_family;
		using phase = sqlite_exact_empty_family_phase;
		using route = sqlite_exact_empty_family_route;

		if (!observation.main_present)
		{
			return {observation.wal != sqlite_exact_empty_wal_form::absent ||
							observation.journal != sqlite_exact_empty_journal_form::absent ||
							observation.shared_memory_present || observation.other_sidecar_present
						? failure::orphan_sidecar
						: failure::unstable_or_not_exact_empty,
					{}};
		}

		if (!observation.main_regular || !observation.source_anchor_stable ||
			!observation.main_identity_stable || !observation.main_entry_stable ||
			!observation.exact_logical_empty)
			return {failure::unstable_or_not_exact_empty, {}};

		const bool valid_main_form =
			observation.main_form == sqlite_exact_empty_main_form::wal_pre ||
			observation.main_form == sqlite_exact_empty_main_form::rollback_post;
		const bool valid_wal_form = observation.wal == sqlite_exact_empty_wal_form::absent ||
			observation.wal == sqlite_exact_empty_wal_form::zero_byte ||
			observation.wal == sqlite_exact_empty_wal_form::nonzero;
		const bool valid_journal_form =
			observation.journal == sqlite_exact_empty_journal_form::absent ||
			observation.journal == sqlite_exact_empty_journal_form::nonhot_prefix ||
			observation.journal == sqlite_exact_empty_journal_form::hot_with_exact_preimages ||
			observation.journal == sqlite_exact_empty_journal_form::invalidated_with_exact_post;
		if (!valid_main_form)
			return {failure::unknown_main_form, {}};
		if (!valid_wal_form || !valid_journal_form)
			return {failure::unknown_sidecar_form, {}};
		if (observation.other_sidecar_present)
			return {failure::mixed_sidecars, {}};
		if (observation.shared_memory_present)
			return {failure::mixed_sidecars, {}};

		// A journal and a WAL are never one DF-0202 family.  Reject the mixed topology before
		// considering either sidecar's individual grammar.
		if (observation.wal != sqlite_exact_empty_wal_form::absent &&
			observation.journal != sqlite_exact_empty_journal_form::absent)
			return {failure::mixed_sidecars, {}};

		if (observation.wal == sqlite_exact_empty_wal_form::nonzero &&
			observation.journal == sqlite_exact_empty_journal_form::absent)
			return {failure::ordinary_wal_only, {}};

		if (observation.journal == sqlite_exact_empty_journal_form::nonhot_prefix)
		{
			if (observation.wal != sqlite_exact_empty_wal_form::absent ||
				observation.main_form != sqlite_exact_empty_main_form::wal_pre)
				return {failure::unsupported_family, {}};
			return {failure::none, {family::fp, phase::pre, route::cleanup_or_recovery_then_f0}};
		}

		if (observation.journal == sqlite_exact_empty_journal_form::hot_with_exact_preimages)
		{
			if (observation.wal != sqlite_exact_empty_wal_form::absent)
				return {failure::unsupported_family, {}};
			return {failure::none,
					{family::fh, phase::pre_or_post, route::cleanup_or_recovery_then_f0}};
		}

		if (observation.journal == sqlite_exact_empty_journal_form::invalidated_with_exact_post)
		{
			if (observation.wal != sqlite_exact_empty_wal_form::absent ||
				observation.main_form != sqlite_exact_empty_main_form::rollback_post)
				return {failure::unsupported_family, {}};
			return {failure::none,
					{family::fi, phase::post, route::rollback_empty_fresh_anchor_only}};
		}

		if (observation.journal != sqlite_exact_empty_journal_form::absent)
			return {failure::unsupported_family, {}};

		if (observation.wal == sqlite_exact_empty_wal_form::zero_byte)
		{
			return {failure::none,
					{observation.main_form == sqlite_exact_empty_main_form::wal_pre
						 ? family::fz_pre
						 : family::fz_post,
					 observation.main_form == sqlite_exact_empty_main_form::wal_pre ? phase::pre
																					: phase::post,
					 observation.main_form == sqlite_exact_empty_main_form::wal_pre
						 ? route::live_receipt_normalizer
						 : route::rollback_empty_fresh_anchor_only}};
		}

		if (observation.wal != sqlite_exact_empty_wal_form::absent)
			return {failure::unsupported_family, {}};

		if (observation.main_form == sqlite_exact_empty_main_form::wal_pre)
			return {failure::none, {family::f0, phase::pre, route::live_receipt_normalizer}};
		return {failure::none, {family::fo, phase::post, route::rollback_empty_fresh_anchor_only}};
	}

	/** No raw classification, decoded candidate, or family route can mint this receipt. */
	struct sqlite_logical_read_receipt
	{
		bool sealed{};
		bool exact_empty{};
		bool connection_closed{};
		std::uint64_t live_custody_count{};
		bool zero_effect_callback_receipt{};

		[[nodiscard]] constexpr bool operator==(const sqlite_logical_read_receipt&) const = default;
	};

	enum class sqlite_exact_empty_entry_failure : std::uint8_t
	{
		none,
		missing_receipt,
		not_exact_empty,
		connection_not_closed,
		live_custody,
		nonzero_callback_effect,
	};

	struct sqlite_exact_empty_entry_validation
	{
		sqlite_exact_empty_entry_failure failure{sqlite_exact_empty_entry_failure::missing_receipt};

		[[nodiscard]] constexpr bool admitted() const noexcept
		{
			return failure == sqlite_exact_empty_entry_failure::none;
		}

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_entry_validation&) const = default;
	};

	/** The sole #202 effect entry: all four logical-read terminal predicates are mandatory. */
	[[nodiscard]] constexpr sqlite_exact_empty_entry_validation
	validate_sqlite_exact_empty_normalization_entry(
		const sqlite_logical_read_receipt& receipt) noexcept
	{
		if (!receipt.sealed)
			return {sqlite_exact_empty_entry_failure::missing_receipt};
		if (!receipt.exact_empty)
			return {sqlite_exact_empty_entry_failure::not_exact_empty};
		if (!receipt.connection_closed)
			return {sqlite_exact_empty_entry_failure::connection_not_closed};
		if (receipt.live_custody_count != 0U)
			return {sqlite_exact_empty_entry_failure::live_custody};
		if (!receipt.zero_effect_callback_receipt)
			return {sqlite_exact_empty_entry_failure::nonzero_callback_effect};
		return {sqlite_exact_empty_entry_failure::none};
	}

	/**
	 * Effect phases for the isolated normalization profile.  The receipt is deliberately an
	 * intermediate product: `normalization_receipt` may hand off to ordinary fresh initialization,
	 * but it is never Store/public success.
	 */
	enum class sqlite_exact_empty_normalization_phase : std::uint8_t
	{
		unresolved,
		logical_read_receipt,
		receipt_revalidated,
		effect_profile_capability_sealed,
		exclusive_normalization_owner,
		pre_effect_sealed,
		effect_journal_open,
		coordination_wal_deleted,
		coordination_wal_parent_fsynced,
		journal_created,
		journal_parent_fsynced,
		permitted_callback_effects,
		terminal_journal_deleted,
		terminal_journal_parent_fsynced,
		file_and_parent_durable,
		confirmed_close,
		post_close_census,
		normalization_receipt,
		ordinary_fresh_initialization,
		recoverable_interruption,
		recrash_classified,
		cold_family_reclassified,
		durability_opaque,
		quarantined,
		public_store_success,
	};

	inline constexpr std::array sqlite_exact_empty_normalization_phases{
		sqlite_exact_empty_normalization_phase::unresolved,
		sqlite_exact_empty_normalization_phase::logical_read_receipt,
		sqlite_exact_empty_normalization_phase::receipt_revalidated,
		sqlite_exact_empty_normalization_phase::effect_profile_capability_sealed,
		sqlite_exact_empty_normalization_phase::exclusive_normalization_owner,
		sqlite_exact_empty_normalization_phase::pre_effect_sealed,
		sqlite_exact_empty_normalization_phase::effect_journal_open,
		sqlite_exact_empty_normalization_phase::coordination_wal_deleted,
		sqlite_exact_empty_normalization_phase::coordination_wal_parent_fsynced,
		sqlite_exact_empty_normalization_phase::journal_created,
		sqlite_exact_empty_normalization_phase::journal_parent_fsynced,
		sqlite_exact_empty_normalization_phase::permitted_callback_effects,
		sqlite_exact_empty_normalization_phase::terminal_journal_deleted,
		sqlite_exact_empty_normalization_phase::terminal_journal_parent_fsynced,
		sqlite_exact_empty_normalization_phase::file_and_parent_durable,
		sqlite_exact_empty_normalization_phase::confirmed_close,
		sqlite_exact_empty_normalization_phase::post_close_census,
		sqlite_exact_empty_normalization_phase::normalization_receipt,
		sqlite_exact_empty_normalization_phase::ordinary_fresh_initialization,
		sqlite_exact_empty_normalization_phase::recoverable_interruption,
		sqlite_exact_empty_normalization_phase::recrash_classified,
		sqlite_exact_empty_normalization_phase::cold_family_reclassified,
		sqlite_exact_empty_normalization_phase::durability_opaque,
		sqlite_exact_empty_normalization_phase::quarantined,
		sqlite_exact_empty_normalization_phase::public_store_success,
	};

	/**
	 * Validate one phase edge.  The optional FZ coordination-WAL pair is the only branch in the
	 * effect grammar.  Crash/recrash and durability terminals never have a success edge.
	 */
	[[nodiscard]] constexpr bool is_sqlite_exact_empty_normalization_transition(
		const sqlite_exact_empty_normalization_phase origin,
		const sqlite_exact_empty_normalization_phase destination) noexcept
	{
		using phase = sqlite_exact_empty_normalization_phase;
		switch (origin)
		{
			case phase::unresolved:
				return destination == phase::logical_read_receipt;
			case phase::logical_read_receipt:
				return destination == phase::receipt_revalidated;
			case phase::receipt_revalidated:
				return destination == phase::effect_profile_capability_sealed;
			case phase::effect_profile_capability_sealed:
				return destination == phase::exclusive_normalization_owner;
			case phase::exclusive_normalization_owner:
				return destination == phase::pre_effect_sealed;
			case phase::pre_effect_sealed:
				return destination == phase::effect_journal_open;
			case phase::effect_journal_open:
				return destination == phase::coordination_wal_deleted ||
					destination == phase::journal_created;
			case phase::coordination_wal_deleted:
				return destination == phase::coordination_wal_parent_fsynced;
			case phase::coordination_wal_parent_fsynced:
				return destination == phase::journal_created;
			case phase::journal_created:
				return destination == phase::journal_parent_fsynced;
			case phase::journal_parent_fsynced:
				return destination == phase::permitted_callback_effects;
			case phase::permitted_callback_effects:
				return destination == phase::terminal_journal_deleted;
			case phase::terminal_journal_deleted:
				return destination == phase::terminal_journal_parent_fsynced;
			case phase::terminal_journal_parent_fsynced:
				return destination == phase::file_and_parent_durable;
			case phase::file_and_parent_durable:
				return destination == phase::confirmed_close;
			case phase::confirmed_close:
				return destination == phase::post_close_census;
			case phase::post_close_census:
				return destination == phase::normalization_receipt;
			case phase::normalization_receipt:
				return destination == phase::ordinary_fresh_initialization;
			case phase::recoverable_interruption:
				return destination == phase::recrash_classified;
			case phase::recrash_classified:
				return destination == phase::cold_family_reclassified;
			case phase::durability_opaque:
			case phase::quarantined:
			case phase::ordinary_fresh_initialization:
			case phase::cold_family_reclassified:
			case phase::public_store_success:
				return false;
		}
		return false;
	}

	/** A complete uninterrupted effect path, with optional FZ coordination-WAL cleanup. */
	[[nodiscard]] constexpr bool validate_sqlite_exact_empty_normalization_path(
		const std::span<const sqlite_exact_empty_normalization_phase> path,
		const bool coordination_wal_present) noexcept
	{
		using phase = sqlite_exact_empty_normalization_phase;
		if (path.size() < 2U || path.front() != phase::unresolved)
			return false;
		for (std::size_t index = 1U; index < path.size(); ++index)
		{
			if (!is_sqlite_exact_empty_normalization_transition(path[index - 1U], path[index]))
				return false;
		}
		if (path.back() != phase::normalization_receipt &&
			path.back() != phase::ordinary_fresh_initialization)
			return false;

		const bool has_coordination_delete =
			std::find(path.begin(), path.end(), phase::coordination_wal_deleted) != path.end();
		const bool has_coordination_sync =
			std::find(path.begin(), path.end(), phase::coordination_wal_parent_fsynced) !=
			path.end();
		if (has_coordination_delete != coordination_wal_present ||
			has_coordination_sync != coordination_wal_present)
			return false;
		if (has_coordination_delete != has_coordination_sync)
			return false;
		return true;
	}

	/** Three parent-directory durability receipts are explicit, not inferred from xDelete(syncDir).
	 */
	struct sqlite_exact_empty_parent_fsync_receipts
	{
		bool coordination_wal_delete{};
		bool journal_creation{};
		bool terminal_journal_delete{};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_parent_fsync_receipts&) const = default;
	};

	struct sqlite_exact_empty_effect_evidence
	{
		bool source_identity_and_entry_revalidated{};
		bool allowed_effects_only{};
		bool coordination_wal_present{};
		bool coordination_wal_deleted{};
		bool journal_created{};
		bool main_write_projection_exact{};
		bool terminal_journal_deleted{};
		sqlite_exact_empty_parent_fsync_receipts parent_fsync{};
		bool confirmed_close{};
		bool post_close_exact_rollback_empty{};
		bool post_close_sidecars_absent{};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_effect_evidence&) const = default;
	};

	enum class sqlite_exact_empty_completion_failure : std::uint8_t
	{
		none,
		entry_not_admitted,
		invalid_phase_path,
		source_revalidation_missing,
		forbidden_effect_observed,
		coordination_wal_not_deleted,
		coordination_wal_parent_not_fsynced,
		journal_not_created,
		journal_parent_not_fsynced,
		main_projection_mismatch,
		terminal_journal_not_deleted,
		terminal_journal_parent_not_fsynced,
		close_not_confirmed,
		post_close_state_not_exact,
	};

	struct sqlite_exact_empty_normalization_completion
	{
		sqlite_exact_empty_completion_failure failure{
			sqlite_exact_empty_completion_failure::entry_not_admitted};
		bool normalization_receipt{};
		bool handoff_to_ordinary_fresh_initialization{};
		bool public_store_success{};

		[[nodiscard]] constexpr bool completed() const noexcept
		{
			return failure == sqlite_exact_empty_completion_failure::none && normalization_receipt;
		}

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_normalization_completion&) const = default;
	};

	/**
	 * Seal only the internal normalization edge.  The result deliberately requires a later ordinary
	 * fresh initialization and can never set `public_store_success`.
	 */
	[[nodiscard]] constexpr sqlite_exact_empty_normalization_completion
	complete_sqlite_exact_empty_normalization(
		const sqlite_logical_read_receipt& receipt,
		const std::span<const sqlite_exact_empty_normalization_phase> path,
		const sqlite_exact_empty_effect_evidence& evidence) noexcept
	{
		using failure = sqlite_exact_empty_completion_failure;
		const auto entry = validate_sqlite_exact_empty_normalization_entry(receipt);
		if (!entry.admitted())
			return {failure::entry_not_admitted, false, false, false};
		if (!validate_sqlite_exact_empty_normalization_path(path,
															evidence.coordination_wal_present))
			return {failure::invalid_phase_path, false, false, false};
		if (!evidence.source_identity_and_entry_revalidated)
			return {failure::source_revalidation_missing, false, false, false};
		if (!evidence.allowed_effects_only)
			return {failure::forbidden_effect_observed, false, false, false};
		if (!evidence.coordination_wal_present &&
			(evidence.coordination_wal_deleted || evidence.parent_fsync.coordination_wal_delete))
			return {failure::forbidden_effect_observed, false, false, false};
		if (evidence.coordination_wal_present && !evidence.coordination_wal_deleted)
			return {failure::coordination_wal_not_deleted, false, false, false};
		if (evidence.coordination_wal_present && !evidence.parent_fsync.coordination_wal_delete)
			return {failure::coordination_wal_parent_not_fsynced, false, false, false};
		if (!evidence.journal_created)
			return {failure::journal_not_created, false, false, false};
		if (!evidence.parent_fsync.journal_creation)
			return {failure::journal_parent_not_fsynced, false, false, false};
		if (!evidence.main_write_projection_exact)
			return {failure::main_projection_mismatch, false, false, false};
		if (!evidence.terminal_journal_deleted)
			return {failure::terminal_journal_not_deleted, false, false, false};
		if (!evidence.parent_fsync.terminal_journal_delete)
			return {failure::terminal_journal_parent_not_fsynced, false, false, false};
		if (!evidence.confirmed_close)
			return {failure::close_not_confirmed, false, false, false};
		if (!evidence.post_close_exact_rollback_empty || !evidence.post_close_sidecars_absent)
			return {failure::post_close_state_not_exact, false, false, false};

		return {failure::none, true, true, false};
	}

	enum class sqlite_exact_empty_normalization_fault_boundary : std::uint8_t
	{
		before_effect,
		effect_journal_open,
		coordination_wal_delete,
		coordination_wal_parent_fsync,
		journal_create,
		journal_parent_fsync,
		main_write,
		terminal_journal_delete,
		terminal_journal_parent_fsync,
		confirmed_close,
		post_close_census,
		after_normalization_receipt,
		crash_recovery,
	};

	enum class sqlite_exact_empty_normalization_fault_kind : std::uint8_t
	{
		process_crash,
		fsync_failure,
		fsync_unknown,
		identity_rebind,
		close_unknown,
		recrash,
	};

	struct sqlite_exact_empty_normalization_fault
	{
		sqlite_exact_empty_normalization_fault_boundary boundary{
			sqlite_exact_empty_normalization_fault_boundary::before_effect};
		sqlite_exact_empty_normalization_fault_kind kind{
			sqlite_exact_empty_normalization_fault_kind::process_crash};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_normalization_fault&) const = default;
	};

	enum class sqlite_exact_empty_normalization_fault_outcome : std::uint8_t
	{
		source_unchanged,
		recoverable_interruption,
		durability_opaque,
		quarantined,
		recrash_cold_reclassification,
	};

	struct sqlite_exact_empty_normalization_fault_result
	{
		sqlite_exact_empty_normalization_fault_outcome outcome{
			sqlite_exact_empty_normalization_fault_outcome::source_unchanged};
		sqlite_exact_empty_normalization_phase terminal_phase{
			sqlite_exact_empty_normalization_phase::unresolved};
		bool may_resume{};
		bool normalization_receipt{};
		bool public_store_success{};
		bool requires_cold_family_classifier{};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_normalization_fault_result&) const = default;
	};

	/**
	 * Classify a typed interruption.  A crash is only the supported callback-boundary termination
	 * model; power loss/torn sectors/callback-internal termination are intentionally not inferred.
	 * Fsync failures and identity/close ambiguity become opaque or quarantined, never retries.
	 */
	[[nodiscard]] constexpr sqlite_exact_empty_normalization_fault_result
	evaluate_sqlite_exact_empty_normalization_fault(
		const sqlite_exact_empty_normalization_fault& fault) noexcept
	{
		using boundary = sqlite_exact_empty_normalization_fault_boundary;
		using kind = sqlite_exact_empty_normalization_fault_kind;
		using outcome = sqlite_exact_empty_normalization_fault_outcome;
		using phase = sqlite_exact_empty_normalization_phase;

		const auto valid_boundary = [](const boundary value) constexpr
		{
			switch (value)
			{
				case boundary::before_effect:
				case boundary::effect_journal_open:
				case boundary::coordination_wal_delete:
				case boundary::coordination_wal_parent_fsync:
				case boundary::journal_create:
				case boundary::journal_parent_fsync:
				case boundary::main_write:
				case boundary::terminal_journal_delete:
				case boundary::terminal_journal_parent_fsync:
				case boundary::confirmed_close:
				case boundary::post_close_census:
				case boundary::after_normalization_receipt:
				case boundary::crash_recovery:
					return true;
			}
			return false;
		};
		const auto valid_kind = [](const kind value) constexpr
		{
			switch (value)
			{
				case kind::process_crash:
				case kind::fsync_failure:
				case kind::fsync_unknown:
				case kind::identity_rebind:
				case kind::close_unknown:
				case kind::recrash:
					return true;
			}
			return false;
		};
		if (!valid_boundary(fault.boundary) || !valid_kind(fault.kind))
			return {outcome::quarantined, phase::quarantined, false, false, false, true};

		if (fault.kind == kind::recrash || fault.boundary == boundary::crash_recovery)
			return {outcome::recrash_cold_reclassification,
					phase::cold_family_reclassified,
					false,
					false,
					false,
					true};
		if (fault.kind == kind::process_crash)
			return {outcome::recoverable_interruption,
					phase::recoverable_interruption,
					false,
					false,
					false,
					true};
		if (fault.kind == kind::identity_rebind || fault.kind == kind::close_unknown)
			return {outcome::quarantined, phase::quarantined, false, false, false, true};
		if (fault.kind == kind::fsync_failure || fault.kind == kind::fsync_unknown)
			return {
				outcome::durability_opaque, phase::durability_opaque, false, false, false, true};
		return {outcome::source_unchanged, phase::unresolved, false, false, false, false};
	}

	/** A cold recrash always discards the in-memory phase and classifies current bytes only. */
	struct sqlite_exact_empty_recrash_result
	{
		sqlite_exact_empty_classifier_result classification{};
		bool resumed_in_memory_phase{};
		bool normalization_receipt{};
		bool public_store_success{};

		[[nodiscard]] constexpr bool
		operator==(const sqlite_exact_empty_recrash_result&) const = default;
	};

	[[nodiscard]] constexpr sqlite_exact_empty_recrash_result
	reclassify_sqlite_exact_empty_after_recrash(
		const sqlite_exact_empty_classifier_observation& observation) noexcept
	{
		return {classify_sqlite_exact_empty_family(observation), false, false, false};
	}
} // namespace cxxlens::detail::sqlite_qualification
