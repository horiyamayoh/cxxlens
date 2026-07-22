#include <array>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string_view>

#include "sdk/sqlite_store_terminal_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void require_resolution(const sqlite_terminal_context& context,
							const sqlite_terminal_public_result public_result,
							const sqlite_terminal_state_effect state_effect,
							const std::string_view label)
	{
		require(resolve_sqlite_terminal(context) ==
					sqlite_terminal_resolution{public_result, state_effect},
				label);
	}

	void require_error(const sqlite_terminal_public_result selected,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view detail,
					   const std::string_view label)
	{
		const error trigger{"trigger.code", "trigger.field", "trigger.detail"};
		auto mapped = map_sqlite_terminal_public_error(selected, trigger);
		require(mapped && mapped->code == code && mapped->field == field &&
					mapped->detail == detail,
				label);
	}

	void check_publish_precedence()
	{
		constexpr auto preserve = sqlite_terminal_state_effect::preserve_last_validated;
		constexpr auto install = sqlite_terminal_state_effect::install_authorized_state;
		constexpr auto poison = sqlite_terminal_state_effect::poison_result_operations;
		require_resolution({sqlite_store_operation::publish,
							sqlite_terminal_phase::pre_effect,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::not_classified},
						   sqlite_terminal_public_result::original_trigger,
						   preserve,
						   "publish pre-effect failure did not preserve its original result");
		require_resolution({sqlite_store_operation::publish,
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::authorized_pre_state},
						   sqlite_terminal_public_result::original_trigger,
						   install,
						   "publish precommit authorized state did not retain the trigger");
		require_resolution(
			{sqlite_store_operation::publish,
			 sqlite_terminal_phase::commit_outcome_unknown,
			 sqlite_terminal_cause::triggering_failure,
			 sqlite_terminal_class::authorized_post_state_with_operation_edge},
			sqlite_terminal_public_result::database_opaque,
			install,
			"publish COMMIT uncertainty did not remain database-opaque after recovery");
		require_resolution({sqlite_store_operation::publish,
							sqlite_terminal_phase::commit_outcome_unknown,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::invalid_census},
						   sqlite_terminal_public_result::database_opaque,
						   poison,
						   "unsafe publish terminal state was not opaque and poisoned");
		require_resolution(
			{sqlite_store_operation::publish,
			 sqlite_terminal_phase::precommit,
			 sqlite_terminal_cause::close_non_ok_or_unknown,
			 sqlite_terminal_class::authorized_pre_state},
			sqlite_terminal_public_result::database_opaque,
			poison,
			"publish close uncertainty incorrectly trusted an authorized classification");
	}

	void check_initialization_precedence()
	{
		constexpr auto no_store = sqlite_terminal_state_effect::return_no_store;
		constexpr auto install = sqlite_terminal_state_effect::install_authorized_state;
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::exact_logical_empty},
						   sqlite_terminal_public_result::initialization_recovery_opaque,
						   no_store,
						   "fresh journal-transition exact-empty state was not opaque");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::authorized_post_state_with_operation_edge},
						   sqlite_terminal_public_result::recovered_success,
						   install,
						   "fresh journal-transition expected-v3 state did not recover success");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::authorized_post_state_without_operation_edge},
						   sqlite_terminal_public_result::initialization_recovery_opaque,
						   no_store,
						   "fresh journal-transition inferred success without its operation edge");
		require_resolution(
			{sqlite_store_operation::fresh_initialization,
			 sqlite_terminal_phase::journal_transition,
			 sqlite_terminal_cause::triggering_failure,
			 sqlite_terminal_class::valid_non_descendant},
			sqlite_terminal_public_result::initialization_concurrent_authority_change,
			no_store,
			"fresh journal-transition non-descendant lost its typed result");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::invalid_census},
						   sqlite_terminal_public_result::initialization_partial_or_mixed,
						   no_store,
						   "fresh journal-transition invalid census lost its typed result");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::terminal_observation_failure,
							sqlite_terminal_class::authorized_post_state_with_operation_edge},
						   sqlite_terminal_public_result::initialization_recovery_opaque,
						   no_store,
						   "fresh journal-transition trusted an unavailable reclassifier");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::journal_transition,
							sqlite_terminal_cause::close_non_ok_or_unknown,
							sqlite_terminal_class::authorized_post_state_with_operation_edge},
						   sqlite_terminal_public_result::initialization_recovery_opaque,
						   no_store,
						   "fresh journal-transition trusted a non-OK close");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::rollback_uncertain,
							sqlite_terminal_class::exact_logical_empty},
						   sqlite_terminal_public_result::original_trigger,
						   no_store,
						   "fresh precommit exact-empty result did not retain the trigger");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::commit_outcome_unknown,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::exact_logical_empty},
						   sqlite_terminal_public_result::database_opaque,
						   no_store,
						   "fresh COMMIT uncertainty inferred success from exact empty");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::commit_outcome_unknown,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::authorized_post_state_with_operation_edge},
						   sqlite_terminal_public_result::recovered_success,
						   install,
						   "fresh expected-v3 descendant did not recover initialization success");
		require_resolution(
			{sqlite_store_operation::fresh_initialization,
			 sqlite_terminal_phase::precommit,
			 sqlite_terminal_cause::main_identity_changed,
			 sqlite_terminal_class::authorized_post_state_with_operation_edge},
			sqlite_terminal_public_result::initialization_concurrent_authority_change,
			no_store,
			"fresh main replacement was not a no-Store non-descendant result");
		require_resolution({sqlite_store_operation::fresh_initialization,
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::mixed_or_ambiguous},
						   sqlite_terminal_public_result::initialization_partial_or_mixed,
						   no_store,
						   "fresh mixed authority was not rejected without a Store");
	}

	void check_migration_and_compaction_precedence()
	{
		constexpr auto install = sqlite_terminal_state_effect::install_authorized_state;
		constexpr auto poison = sqlite_terminal_state_effect::poison_result_operations;
		for (const auto operation :
			 {sqlite_store_operation::migrate_predecessor, sqlite_store_operation::compact_current})
		{
			require_resolution({operation,
								sqlite_terminal_phase::precommit,
								sqlite_terminal_cause::rollback_uncertain,
								sqlite_terminal_class::authorized_pre_state},
							   sqlite_terminal_public_result::original_trigger,
							   install,
							   "precommit authorized pre-state did not retain the trigger");
			require_resolution({operation,
								sqlite_terminal_phase::precommit,
								sqlite_terminal_cause::rollback_uncertain,
								sqlite_terminal_class::authorized_post_state_with_operation_edge},
							   sqlite_terminal_public_result::recovered_success,
							   install,
							   "precommit authorized operation edge did not recover success");
			require_resolution({operation,
								sqlite_terminal_phase::commit_outcome_unknown,
								sqlite_terminal_cause::triggering_failure,
								sqlite_terminal_class::authorized_pre_state},
							   sqlite_terminal_public_result::database_opaque,
							   install,
							   "COMMIT uncertainty inferred an absent operation edge");
			require_resolution({operation,
								sqlite_terminal_phase::commit_outcome_unknown,
								sqlite_terminal_cause::triggering_failure,
								sqlite_terminal_class::authorized_post_state_with_operation_edge},
							   sqlite_terminal_public_result::recovered_success,
							   install,
							   "COMMIT uncertainty lost an independently proved operation edge");
		}

		require_resolution({sqlite_store_operation::migrate_predecessor,
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::mixed_or_ambiguous},
						   sqlite_terminal_public_result::migration_mixed_or_ambiguous,
						   poison,
						   "migration mixed state did not produce its typed poisoned result");
		require_resolution({sqlite_store_operation::compact_current,
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::invalid_census},
						   sqlite_terminal_public_result::compaction_recovery_opaque,
						   poison,
						   "precommit compaction unsafe state did not use opaque precedence");
		require_resolution({sqlite_store_operation::compact_current,
							sqlite_terminal_phase::commit_outcome_unknown,
							sqlite_terminal_cause::triggering_failure,
							sqlite_terminal_class::invalid_census},
						   sqlite_terminal_public_result::compaction_unexpected_census,
						   poison,
						   "COMMIT-unknown compaction invalid census lost its typed result");
	}

	void check_wal_handoff_is_fail_closed()
	{
		constexpr auto install = sqlite_terminal_state_effect::install_authorized_state;
		constexpr auto poison = sqlite_terminal_state_effect::poison_result_operations;
		constexpr std::array non_success_phases{sqlite_terminal_phase::pre_effect,
												sqlite_terminal_phase::journal_transition,
												sqlite_terminal_phase::precommit,
												sqlite_terminal_phase::commit_outcome_unknown};
		constexpr std::array authorized_classes{
			sqlite_terminal_class::authorized_pre_state,
			sqlite_terminal_class::authorized_post_state_without_operation_edge,
			sqlite_terminal_class::authorized_post_state_with_operation_edge};
		for (const auto phase : non_success_phases)
		{
			for (const auto terminal_class : authorized_classes)
			{
				require_resolution({sqlite_store_operation::wal_recovery_handoff,
									phase,
									sqlite_terminal_cause::rollback_uncertain,
									terminal_class},
								   sqlite_terminal_public_result::recovery_handoff_opaque,
								   poison,
								   "failed WAL handoff trusted an authorized reclassification");
			}
		}
		for (const auto terminal_class : authorized_classes)
		{
			require_resolution(
				{sqlite_store_operation::wal_recovery_handoff,
				 sqlite_terminal_phase::successful_handoff,
				 sqlite_terminal_cause::triggering_failure,
				 terminal_class},
				sqlite_terminal_public_result::recovered_success,
				install,
				"successful WAL handoff did not install the independently validated state");
		}
	}

	void check_invalid_enums_fail_closed()
	{
		constexpr auto opaque = sqlite_terminal_public_result::database_opaque;
		constexpr auto poison = sqlite_terminal_state_effect::poison_result_operations;
		const sqlite_terminal_context valid_context{sqlite_store_operation::publish,
													sqlite_terminal_phase::precommit,
													sqlite_terminal_cause::triggering_failure,
													sqlite_terminal_class::authorized_pre_state};

		auto invalid = valid_context;
		invalid.operation = static_cast<sqlite_store_operation>(255U);
		require_resolution(invalid, opaque, poison, "invalid operation was not fail-closed");
		invalid = valid_context;
		invalid.phase = static_cast<sqlite_terminal_phase>(255U);
		require_resolution(invalid, opaque, poison, "invalid phase was not fail-closed");
		invalid = valid_context;
		invalid.cause = static_cast<sqlite_terminal_cause>(255U);
		require_resolution(invalid, opaque, poison, "invalid cause was not fail-closed");
		invalid = valid_context;
		invalid.terminal_class = static_cast<sqlite_terminal_class>(255U);
		require_resolution(invalid, opaque, poison, "invalid terminal class was not fail-closed");
	}

	void check_public_error_mapping()
	{
		const error trigger{"trigger.code", "trigger.field", "trigger.detail"};
		require(!map_sqlite_terminal_public_error(sqlite_terminal_public_result::recovered_success,
												  trigger),
				"recovered success unexpectedly produced an error");
		auto original = map_sqlite_terminal_public_error(
			sqlite_terminal_public_result::original_trigger, trigger);
		require(original && *original == trigger, "original trigger was rewritten");
		require_error(sqlite_terminal_public_result::database_opaque,
					  "store.sqlite-failure",
					  "database",
					  "opaque",
					  "database opaque mapping drifted");
		require_error(sqlite_terminal_public_result::initialization_recovery_opaque,
					  "store.sqlite-failure",
					  "sqlite-initialization-recovery",
					  "opaque",
					  "initialization opaque mapping drifted");
		require_error(sqlite_terminal_public_result::initialization_concurrent_authority_change,
					  "store.sqlite-failure",
					  "sqlite-initialization-recovery",
					  "concurrent-authority-change",
					  "initialization non-descendant mapping drifted");
		require_error(sqlite_terminal_public_result::initialization_partial_or_mixed,
					  "store.corrupt",
					  "sqlite-initialization-recovery",
					  "partial-or-mixed-authority",
					  "initialization invalid mapping drifted");
		require_error(sqlite_terminal_public_result::migration_recovery_opaque,
					  "store.sqlite-failure",
					  "migration-recovery",
					  "opaque",
					  "migration opaque mapping drifted");
		require_error(sqlite_terminal_public_result::migration_concurrent_authority_change,
					  "store.sqlite-failure",
					  "migration-recovery",
					  "concurrent-authority-change",
					  "migration non-descendant mapping drifted");
		require_error(sqlite_terminal_public_result::migration_unexpected_census,
					  "store.corrupt",
					  "migration-recovery",
					  "unexpected-census",
					  "migration invalid census mapping drifted");
		require_error(sqlite_terminal_public_result::migration_mixed_or_ambiguous,
					  "store.corrupt",
					  "migration-recovery",
					  "mixed-or-ambiguous",
					  "migration mixed mapping drifted");
		require_error(sqlite_terminal_public_result::compaction_recovery_opaque,
					  "store.sqlite-failure",
					  "compaction-recovery",
					  "opaque",
					  "compaction opaque mapping drifted");
		require_error(sqlite_terminal_public_result::compaction_concurrent_authority_change,
					  "store.sqlite-failure",
					  "compaction-recovery",
					  "concurrent-authority-change",
					  "compaction non-descendant mapping drifted");
		require_error(sqlite_terminal_public_result::compaction_unexpected_census,
					  "store.corrupt",
					  "compaction-recovery",
					  "unexpected-census",
					  "compaction invalid census mapping drifted");
		require_error(sqlite_terminal_public_result::compaction_mixed_or_ambiguous,
					  "store.corrupt",
					  "compaction-recovery",
					  "mixed-or-ambiguous",
					  "compaction mixed mapping drifted");
		require_error(sqlite_terminal_public_result::recovery_handoff_opaque,
					  "store.sqlite-failure",
					  "sqlite-recovery-handoff",
					  "opaque",
					  "recovery handoff mapping drifted");
	}

	void check_availability_and_observer_preservation()
	{
		const sqlite_last_validated_compatibility v2{{2U, 6U, 0U}, true, true};
		const sqlite_last_validated_compatibility v3{{3U, 0U, 0U}, true, false};
		sqlite_store_availability_policy policy{v2};
		require(policy.availability() == sqlite_store_availability::available &&
					policy.result_operations_available() &&
					policy.observe(3U) == sqlite_store_nonresult_observation{v2, 3U},
				"available policy changed its validated compatibility or pin observer");

		require(policy.begin_recovery_handoff() && !policy.result_operations_available(),
				"policy did not close its result-operation gate during handoff");
		auto pending = policy.observe(4U);
		require(!pending.compatibility.direct_open &&
					pending.compatibility.readable_format == v2.readable_format &&
					pending.compatibility.migration_required &&
					pending.retained_generation_count == 4U && !policy.begin_recovery_handoff(),
				"pending handoff changed last compatibility or admitted a second handoff");
		require(policy.restore_available_after_confirmed_no_effect() &&
					policy.result_operations_available(),
				"confirmed no-effect cleanup did not restore availability");
		require(policy.install_independently_validated(v3) &&
					policy.observe(5U).compatibility == v3,
				"independently validated v3 state was not installed");

		policy.poison();
		auto poisoned = policy.observe(6U);
		require(policy.availability() == sqlite_store_availability::poisoned &&
					!policy.result_operations_available() && !poisoned.compatibility.direct_open &&
					poisoned.compatibility.readable_format == v3.readable_format &&
					!poisoned.compatibility.migration_required &&
					poisoned.retained_generation_count == 6U,
				"poison did not preserve the last validated tuple and exact live-pin observer");
		require(!policy.install_independently_validated(v2) &&
					!policy.restore_available_after_confirmed_no_effect() &&
					policy.availability() == sqlite_store_availability::poisoned,
				"a poisoned Store instance was resurrected");

		const auto blocked = sqlite_store_reopen_required_error();
		require(blocked ==
					error{"store.backend-unavailable", "sqlite-connection", "reopen-required"},
				"poisoned result-operation error tuple drifted");
	}
} // namespace

int main()
{
	check_publish_precedence();
	check_initialization_precedence();
	check_migration_and_compaction_precedence();
	check_wal_handoff_is_fail_closed();
	check_invalid_enums_fail_closed();
	check_public_error_mapping();
	check_availability_and_observer_preservation();
	std::cout << "sqlite Store terminal policy tests passed\n";
	return 0;
}
