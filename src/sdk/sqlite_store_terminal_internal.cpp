#include "sqlite_store_terminal_internal.hpp"

#include <utility>

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] constexpr bool valid(const sqlite_store_operation value) noexcept
		{
			switch (value)
			{
				case sqlite_store_operation::fresh_initialization:
				case sqlite_store_operation::publish:
				case sqlite_store_operation::migrate_predecessor:
				case sqlite_store_operation::compact_current:
				case sqlite_store_operation::wal_recovery_handoff:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_terminal_phase value) noexcept
		{
			switch (value)
			{
				case sqlite_terminal_phase::pre_effect:
				case sqlite_terminal_phase::precommit:
				case sqlite_terminal_phase::commit_outcome_unknown:
				case sqlite_terminal_phase::successful_handoff:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_terminal_cause value) noexcept
		{
			switch (value)
			{
				case sqlite_terminal_cause::triggering_failure:
				case sqlite_terminal_cause::rollback_uncertain:
				case sqlite_terminal_cause::finalization_uncertain:
				case sqlite_terminal_cause::close_non_ok_or_unknown:
				case sqlite_terminal_cause::terminal_observation_failure:
				case sqlite_terminal_cause::main_identity_changed:
				case sqlite_terminal_cause::reopen_failure:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool valid(const sqlite_terminal_class value) noexcept
		{
			switch (value)
			{
				case sqlite_terminal_class::not_classified:
				case sqlite_terminal_class::exact_logical_empty:
				case sqlite_terminal_class::authorized_pre_state:
				case sqlite_terminal_class::authorized_post_state_without_operation_edge:
				case sqlite_terminal_class::authorized_post_state_with_operation_edge:
				case sqlite_terminal_class::valid_non_descendant:
				case sqlite_terminal_class::invalid_census:
				case sqlite_terminal_class::mixed_or_ambiguous:
				case sqlite_terminal_class::reclassifier_unavailable:
					return true;
			}
			return false;
		}

		[[nodiscard]] constexpr bool authorized(const sqlite_terminal_class value) noexcept
		{
			return value == sqlite_terminal_class::authorized_pre_state ||
				value == sqlite_terminal_class::authorized_post_state_without_operation_edge ||
				value == sqlite_terminal_class::authorized_post_state_with_operation_edge;
		}

		[[nodiscard]] constexpr sqlite_terminal_class
		effective_class(const sqlite_terminal_context& context) noexcept
		{
			switch (context.cause)
			{
				case sqlite_terminal_cause::close_non_ok_or_unknown:
				case sqlite_terminal_cause::terminal_observation_failure:
				case sqlite_terminal_cause::reopen_failure:
					return sqlite_terminal_class::reclassifier_unavailable;
				case sqlite_terminal_cause::main_identity_changed:
					return sqlite_terminal_class::valid_non_descendant;
				case sqlite_terminal_cause::rollback_uncertain:
				case sqlite_terminal_cause::finalization_uncertain:
					return context.terminal_class == sqlite_terminal_class::not_classified
						? sqlite_terminal_class::reclassifier_unavailable
						: context.terminal_class;
				case sqlite_terminal_cause::triggering_failure:
					return context.terminal_class;
			}
			return sqlite_terminal_class::reclassifier_unavailable;
		}

		[[nodiscard]] constexpr sqlite_terminal_state_effect
		unsafe_effect(const sqlite_store_operation operation) noexcept
		{
			return operation == sqlite_store_operation::fresh_initialization
				? sqlite_terminal_state_effect::return_no_store
				: sqlite_terminal_state_effect::poison_result_operations;
		}

		[[nodiscard]] constexpr sqlite_terminal_resolution
		opaque_resolution(const sqlite_store_operation operation) noexcept
		{
			switch (operation)
			{
				case sqlite_store_operation::fresh_initialization:
					return {sqlite_terminal_public_result::initialization_recovery_opaque,
							sqlite_terminal_state_effect::return_no_store};
				case sqlite_store_operation::publish:
					return {sqlite_terminal_public_result::database_opaque,
							sqlite_terminal_state_effect::poison_result_operations};
				case sqlite_store_operation::migrate_predecessor:
					return {sqlite_terminal_public_result::migration_recovery_opaque,
							sqlite_terminal_state_effect::poison_result_operations};
				case sqlite_store_operation::compact_current:
					return {sqlite_terminal_public_result::compaction_recovery_opaque,
							sqlite_terminal_state_effect::poison_result_operations};
				case sqlite_store_operation::wal_recovery_handoff:
					return {sqlite_terminal_public_result::recovery_handoff_opaque,
							sqlite_terminal_state_effect::poison_result_operations};
			}
			return {sqlite_terminal_public_result::database_opaque,
					sqlite_terminal_state_effect::poison_result_operations};
		}

		[[nodiscard]] constexpr sqlite_terminal_resolution
		unsafe_resolution(const sqlite_store_operation operation,
						  const sqlite_terminal_phase phase,
						  const sqlite_terminal_class terminal_class) noexcept
		{
			if (terminal_class == sqlite_terminal_class::reclassifier_unavailable ||
				terminal_class == sqlite_terminal_class::not_classified ||
				terminal_class == sqlite_terminal_class::exact_logical_empty)
				return opaque_resolution(operation);

			const auto effect = unsafe_effect(operation);
			switch (operation)
			{
				case sqlite_store_operation::fresh_initialization:
					if (terminal_class == sqlite_terminal_class::valid_non_descendant)
						return {sqlite_terminal_public_result::
									initialization_concurrent_authority_change,
								effect};
					return {sqlite_terminal_public_result::initialization_partial_or_mixed, effect};
				case sqlite_store_operation::publish:
					return {sqlite_terminal_public_result::database_opaque, effect};
				case sqlite_store_operation::migrate_predecessor:
					if (terminal_class == sqlite_terminal_class::valid_non_descendant)
						return {
							sqlite_terminal_public_result::migration_concurrent_authority_change,
							effect};
					if (terminal_class == sqlite_terminal_class::invalid_census)
						return {sqlite_terminal_public_result::migration_unexpected_census, effect};
					return {sqlite_terminal_public_result::migration_mixed_or_ambiguous, effect};
				case sqlite_store_operation::compact_current:
					if (phase != sqlite_terminal_phase::commit_outcome_unknown)
						return {sqlite_terminal_public_result::compaction_recovery_opaque, effect};
					if (terminal_class == sqlite_terminal_class::valid_non_descendant)
						return {
							sqlite_terminal_public_result::compaction_concurrent_authority_change,
							effect};
					if (terminal_class == sqlite_terminal_class::invalid_census)
						return {sqlite_terminal_public_result::compaction_unexpected_census,
								effect};
					return {sqlite_terminal_public_result::compaction_mixed_or_ambiguous, effect};
				case sqlite_store_operation::wal_recovery_handoff:
					return {sqlite_terminal_public_result::recovery_handoff_opaque, effect};
			}
			return {sqlite_terminal_public_result::database_opaque,
					sqlite_terminal_state_effect::poison_result_operations};
		}

		[[nodiscard]] constexpr sqlite_terminal_resolution
		authorized_resolution(const sqlite_store_operation operation,
							  const sqlite_terminal_phase phase,
							  const sqlite_terminal_class terminal_class) noexcept
		{
			const bool operation_edge =
				terminal_class == sqlite_terminal_class::authorized_post_state_with_operation_edge;
			constexpr auto install = sqlite_terminal_state_effect::install_authorized_state;
			switch (operation)
			{
				case sqlite_store_operation::fresh_initialization:
					if (operation_edge)
						return {sqlite_terminal_public_result::recovered_success, install};
					if (phase == sqlite_terminal_phase::precommit)
						return {sqlite_terminal_public_result::original_trigger,
								sqlite_terminal_state_effect::return_no_store};
					return {sqlite_terminal_public_result::database_opaque,
							sqlite_terminal_state_effect::return_no_store};
				case sqlite_store_operation::publish:
					if (phase == sqlite_terminal_phase::commit_outcome_unknown)
						return {sqlite_terminal_public_result::database_opaque, install};
					if (phase == sqlite_terminal_phase::successful_handoff)
						return {sqlite_terminal_public_result::recovered_success, install};
					return {sqlite_terminal_public_result::original_trigger, install};
				case sqlite_store_operation::migrate_predecessor:
					if (operation_edge)
						return {sqlite_terminal_public_result::recovered_success, install};
					if (phase == sqlite_terminal_phase::precommit)
						return {sqlite_terminal_public_result::original_trigger, install};
					return {sqlite_terminal_public_result::database_opaque, install};
				case sqlite_store_operation::compact_current:
					if (phase == sqlite_terminal_phase::successful_handoff || operation_edge)
						return {sqlite_terminal_public_result::recovered_success, install};
					if (phase == sqlite_terminal_phase::precommit)
						return {sqlite_terminal_public_result::original_trigger, install};
					return {sqlite_terminal_public_result::database_opaque, install};
				case sqlite_store_operation::wal_recovery_handoff:
					if (phase == sqlite_terminal_phase::successful_handoff)
						return {sqlite_terminal_public_result::recovered_success, install};
					return {sqlite_terminal_public_result::recovery_handoff_opaque,
							sqlite_terminal_state_effect::poison_result_operations};
			}
			return {sqlite_terminal_public_result::database_opaque,
					sqlite_terminal_state_effect::poison_result_operations};
		}
	} // namespace

	sqlite_store_availability_policy::sqlite_store_availability_policy(
		sqlite_last_validated_compatibility compatibility) noexcept
		: last_validated_{compatibility}
	{
	}

	sqlite_store_availability sqlite_store_availability_policy::availability() const noexcept
	{
		return availability_;
	}

	bool sqlite_store_availability_policy::result_operations_available() const noexcept
	{
		return availability_ == sqlite_store_availability::available;
	}

	bool sqlite_store_availability_policy::begin_recovery_handoff() noexcept
	{
		if (availability_ != sqlite_store_availability::available)
			return false;
		availability_ = sqlite_store_availability::recovery_handoff_pending;
		return true;
	}

	bool sqlite_store_availability_policy::restore_available_after_confirmed_no_effect() noexcept
	{
		if (availability_ != sqlite_store_availability::recovery_handoff_pending)
			return false;
		availability_ = sqlite_store_availability::available;
		return true;
	}

	bool sqlite_store_availability_policy::install_independently_validated(
		sqlite_last_validated_compatibility compatibility) noexcept
	{
		if (availability_ == sqlite_store_availability::poisoned)
			return false;
		last_validated_ = compatibility;
		availability_ = sqlite_store_availability::available;
		return true;
	}

	void sqlite_store_availability_policy::poison() noexcept
	{
		availability_ = sqlite_store_availability::poisoned;
	}

	sqlite_store_nonresult_observation sqlite_store_availability_policy::observe(
		const std::size_t live_preexisting_generation_count) const noexcept
	{
		auto compatibility = last_validated_;
		if (availability_ != sqlite_store_availability::available)
			compatibility.direct_open = false;
		return {compatibility, live_preexisting_generation_count};
	}

	sqlite_terminal_resolution
	resolve_sqlite_terminal(const sqlite_terminal_context& context) noexcept
	{
		if (!valid(context.operation) || !valid(context.phase) || !valid(context.cause) ||
			!valid(context.terminal_class))
			return {sqlite_terminal_public_result::database_opaque,
					sqlite_terminal_state_effect::poison_result_operations};

		if (context.phase == sqlite_terminal_phase::pre_effect &&
			context.cause == sqlite_terminal_cause::triggering_failure &&
			context.terminal_class == sqlite_terminal_class::not_classified)
		{
			return {sqlite_terminal_public_result::original_trigger,
					context.operation == sqlite_store_operation::fresh_initialization
						? sqlite_terminal_state_effect::return_no_store
						: sqlite_terminal_state_effect::preserve_last_validated};
		}

		if (context.phase == sqlite_terminal_phase::precommit &&
			context.cause == sqlite_terminal_cause::triggering_failure &&
			context.terminal_class == sqlite_terminal_class::not_classified)
		{
			return {sqlite_terminal_public_result::original_trigger,
					context.operation == sqlite_store_operation::fresh_initialization
						? sqlite_terminal_state_effect::return_no_store
						: sqlite_terminal_state_effect::preserve_last_validated};
		}

		const auto terminal_class = effective_class(context);
		if (context.operation == sqlite_store_operation::fresh_initialization &&
			terminal_class == sqlite_terminal_class::exact_logical_empty)
		{
			if (context.phase == sqlite_terminal_phase::precommit)
				return {sqlite_terminal_public_result::original_trigger,
						sqlite_terminal_state_effect::return_no_store};
			if (context.phase == sqlite_terminal_phase::commit_outcome_unknown)
				return {sqlite_terminal_public_result::database_opaque,
						sqlite_terminal_state_effect::return_no_store};
			return {sqlite_terminal_public_result::initialization_recovery_opaque,
					sqlite_terminal_state_effect::return_no_store};
		}
		if (authorized(terminal_class))
			return authorized_resolution(context.operation, context.phase, terminal_class);
		return unsafe_resolution(context.operation, context.phase, terminal_class);
	}

	std::optional<error>
	map_sqlite_terminal_public_error(const sqlite_terminal_public_result selected,
									 const error& original_trigger)
	{
		switch (selected)
		{
			case sqlite_terminal_public_result::recovered_success:
				return std::nullopt;
			case sqlite_terminal_public_result::original_trigger:
				return original_trigger;
			case sqlite_terminal_public_result::database_opaque:
				return error{"store.sqlite-failure", "database", "opaque"};
			case sqlite_terminal_public_result::initialization_recovery_opaque:
				return error{"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"};
			case sqlite_terminal_public_result::initialization_concurrent_authority_change:
				return error{"store.sqlite-failure",
							 "sqlite-initialization-recovery",
							 "concurrent-authority-change"};
			case sqlite_terminal_public_result::initialization_partial_or_mixed:
				return error{"store.corrupt",
							 "sqlite-initialization-recovery",
							 "partial-or-mixed-authority"};
			case sqlite_terminal_public_result::migration_recovery_opaque:
				return error{"store.sqlite-failure", "migration-recovery", "opaque"};
			case sqlite_terminal_public_result::migration_concurrent_authority_change:
				return error{
					"store.sqlite-failure", "migration-recovery", "concurrent-authority-change"};
			case sqlite_terminal_public_result::migration_unexpected_census:
				return error{"store.corrupt", "migration-recovery", "unexpected-census"};
			case sqlite_terminal_public_result::migration_mixed_or_ambiguous:
				return error{"store.corrupt", "migration-recovery", "mixed-or-ambiguous"};
			case sqlite_terminal_public_result::compaction_recovery_opaque:
				return error{"store.sqlite-failure", "compaction-recovery", "opaque"};
			case sqlite_terminal_public_result::compaction_concurrent_authority_change:
				return error{
					"store.sqlite-failure", "compaction-recovery", "concurrent-authority-change"};
			case sqlite_terminal_public_result::compaction_unexpected_census:
				return error{"store.corrupt", "compaction-recovery", "unexpected-census"};
			case sqlite_terminal_public_result::compaction_mixed_or_ambiguous:
				return error{"store.corrupt", "compaction-recovery", "mixed-or-ambiguous"};
			case sqlite_terminal_public_result::recovery_handoff_opaque:
				return error{"store.sqlite-failure", "sqlite-recovery-handoff", "opaque"};
		}
		return error{"store.sqlite-failure", "database", "opaque"};
	}

	error sqlite_store_reopen_required_error()
	{
		return {"store.backend-unavailable", "sqlite-connection", "reopen-required"};
	}
} // namespace cxxlens::sdk
