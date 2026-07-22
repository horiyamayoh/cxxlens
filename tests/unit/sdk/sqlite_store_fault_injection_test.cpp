#include <array>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <thread>
#include <type_traits>

#include "sdk/sqlite_store_fault_injection_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	static_assert(std::is_trivially_copyable_v<sqlite_store_fault_event>);
	static_assert(std::is_trivially_copyable_v<sqlite_store_fault_plan>);
	static_assert(std::is_trivially_copyable_v<sqlite_store_fault_directive>);
	static_assert(std::is_trivially_copyable_v<sqlite_store_fault_observation>);
	static_assert(std::is_standard_layout_v<sqlite_store_fault_observation>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] constexpr sqlite_store_fault_event publish_middle_chunk_after()
	{
		return {sqlite_store_operation::publish,
				sqlite_store_fault_boundary::payload_chunk,
				sqlite_store_fault_timing::after,
				2U,
				3U};
	}

	void check_production_no_scope_is_noop()
	{
		const auto event = publish_middle_chunk_after();
		const auto directive = dispatch_sqlite_store_fault(event);
		require(!directive.issued && directive.action == sqlite_store_fault_action::none &&
					directive.event == event,
				"production dispatch produced a fault directive without a scope");
	}

	void check_exact_typed_match_and_exactly_once_directive()
	{
		const auto target = publish_middle_chunk_after();
		const sqlite_store_fault_plan plan{
			target, sqlite_store_fault_action::report_failure_after_delegate};
		require(valid_sqlite_store_fault_plan(plan), "valid typed fault plan was rejected");
		{
			sqlite_store_fault_scope scope{plan};
			require(scope.active() && scope.plan() == plan,
					"outermost valid fault scope did not activate");

			auto different_timing = target;
			different_timing.timing = sqlite_store_fault_timing::before;
			auto different_ordinal = target;
			different_ordinal.ordinal = 1U;
			const auto first = dispatch_sqlite_store_fault(different_timing);
			const auto second = dispatch_sqlite_store_fault(different_ordinal);
			const auto matched = dispatch_sqlite_store_fault(target);
			const auto repeated = dispatch_sqlite_store_fault(target);
			require(!first.issued && !second.issued &&
						first.action == sqlite_store_fault_action::none &&
						second.action == sqlite_store_fault_action::none,
					"non-exact timing or ordinal matched the typed plan");
			require(matched.issued && matched.event == target &&
						matched.action == sqlite_store_fault_action::report_failure_after_delegate,
					"exact typed event did not return its planned directive");
			require(!repeated.issued && repeated.action == sqlite_store_fault_action::none,
					"one fault plan issued more than one directive");

			const auto observed = scope.observation();
			require(observed.observed_event_count == 4U && observed.matching_event_count == 2U &&
						observed.issued_directive_count == 1U && observed.has_last_observed_event &&
						observed.last_observed_event == target && observed.has_matched_event &&
						observed.matched_event == target && !observed.count_overflow &&
						!observed.invalid_plan && !observed.invalid_event_observed &&
						!observed.nested_scope_suppressed,
					"fault observation receipt lost exact event or directive counts");
		}
		require(!dispatch_sqlite_store_fault(target).issued,
				"destroyed fault scope remained active");
	}

	void check_passive_observation_never_changes_execution()
	{
		const sqlite_store_fault_event begin_after{sqlite_store_operation::migrate_predecessor,
												   sqlite_store_fault_boundary::transaction_begin,
												   sqlite_store_fault_timing::after,
												   1U,
												   1U};
		sqlite_store_fault_scope scope{{begin_after, sqlite_store_fault_action::observe_only}};
		require(scope.active(), "passive observation scope did not activate");
		for (std::uint64_t index{}; index < 3U; ++index)
		{
			const auto directive = dispatch_sqlite_store_fault(begin_after);
			require(!directive.issued && directive.action == sqlite_store_fault_action::none,
					"passive observation returned an execution directive");
		}
		const auto observed = scope.observation();
		require(observed.observed_event_count == 3U && observed.matching_event_count == 3U &&
					observed.issued_directive_count == 0U && observed.has_matched_event &&
					observed.matched_event == begin_after && !observed.count_overflow,
				"passive migration BEGIN observation did not retain its exact count");
	}

	void check_coordination_and_journal_boundaries_are_closed_and_exact()
	{
		constexpr std::array boundaries{sqlite_store_fault_boundary::wal_coordination,
										sqlite_store_fault_boundary::journal_transition};
		for (const auto boundary : boundaries)
		{
			const sqlite_store_fault_event before{sqlite_store_operation::fresh_initialization,
												  boundary,
												  sqlite_store_fault_timing::before,
												  1U,
												  1U};
			const auto after = sqlite_store_fault_event{
				before.operation, boundary, sqlite_store_fault_timing::after, 1U, 1U};
			require(valid_sqlite_store_fault_event(before) &&
						valid_sqlite_store_fault_plan(
							{before, sqlite_store_fault_action::report_failure}) &&
						valid_sqlite_store_fault_plan(
							{after, sqlite_store_fault_action::report_failure_after_delegate}),
					"typed coordination or journal transition boundary was not admissible");
			{
				sqlite_store_fault_scope scope{{before, sqlite_store_fault_action::report_failure}};
				require(dispatch_sqlite_store_fault(before).issued &&
							!dispatch_sqlite_store_fault(after).issued &&
							scope.observation().matching_event_count == 1U,
						"before transition boundary matched a delegated after event");
			}
			{
				sqlite_store_fault_scope scope{
					{after, sqlite_store_fault_action::report_failure_after_delegate}};
				require(!dispatch_sqlite_store_fault(before).issued &&
							dispatch_sqlite_store_fault(after).issued &&
							scope.observation().matching_event_count == 1U,
						"after transition boundary matched a pre-delegate event");
			}
		}
	}

	void check_crash_and_close_are_directives_only()
	{
		const sqlite_store_fault_event crash_event{sqlite_store_operation::migrate_predecessor,
												   sqlite_store_fault_boundary::format_marker,
												   sqlite_store_fault_timing::after,
												   1U,
												   1U};
		{
			sqlite_store_fault_scope scope{
				{crash_event, sqlite_store_fault_action::request_process_crash}};
			const auto directive = dispatch_sqlite_store_fault(crash_event);
			require(directive.issued &&
						directive.action == sqlite_store_fault_action::request_process_crash,
					"crash action was not returned as a typed directive");
			require(scope.observation().issued_directive_count == 1U,
					"crash directive was not observed without terminating the unit process");
		}

		const sqlite_store_fault_event close_event{sqlite_store_operation::compact_current,
												   sqlite_store_fault_boundary::connection_close,
												   sqlite_store_fault_timing::before,
												   1U,
												   1U};
		{
			sqlite_store_fault_scope scope{
				{close_event, sqlite_store_fault_action::request_close_non_ok}};
			const auto directive = dispatch_sqlite_store_fault(close_event);
			require(directive.issued &&
						directive.action == sqlite_store_fault_action::request_close_non_ok,
					"close-non-OK action was not returned as a typed directive");
			require(scope.observation().issued_directive_count == 1U,
					"close directive was not observed without invoking a close callback");
		}
	}

	void check_invalid_plans_and_events_fail_closed()
	{
		auto invalid_ordinal = publish_middle_chunk_after();
		invalid_ordinal.ordinal = 0U;
		require(!valid_sqlite_store_fault_event(invalid_ordinal),
				"zero fault ordinal was accepted");
		auto invalid_total = publish_middle_chunk_after();
		invalid_total.total = 1U;
		require(!valid_sqlite_store_fault_event(invalid_total),
				"fault ordinal above total was accepted");
		auto invalid_operation = publish_middle_chunk_after();
		invalid_operation.operation = static_cast<sqlite_store_operation>(255U);
		require(!valid_sqlite_store_fault_event(invalid_operation),
				"invalid fault operation enum was accepted");
		auto invalid_boundary = publish_middle_chunk_after();
		invalid_boundary.boundary = static_cast<sqlite_store_fault_boundary>(255U);
		require(!valid_sqlite_store_fault_event(invalid_boundary),
				"invalid fault boundary enum was accepted");
		auto invalid_timing = publish_middle_chunk_after();
		invalid_timing.timing = static_cast<sqlite_store_fault_timing>(255U);
		require(!valid_sqlite_store_fault_event(invalid_timing),
				"invalid fault timing enum was accepted");

		require(!valid_sqlite_store_fault_plan(
					{publish_middle_chunk_after(), sqlite_store_fault_action::none}),
				"none action activated a qualification scope");
		auto after_delegate_before = publish_middle_chunk_after();
		after_delegate_before.timing = sqlite_store_fault_timing::before;
		require(
			!valid_sqlite_store_fault_plan(
				{after_delegate_before, sqlite_store_fault_action::report_failure_after_delegate}),
			"after-delegate failure was accepted at a before boundary");
		require(!valid_sqlite_store_fault_plan({publish_middle_chunk_after(),
												sqlite_store_fault_action::request_close_non_ok}),
				"close-non-OK directive was accepted away from connection close");
		require(!valid_sqlite_store_fault_plan(
					{publish_middle_chunk_after(), static_cast<sqlite_store_fault_action>(255U)}),
				"invalid fault action enum was accepted");

		{
			sqlite_store_fault_scope invalid_scope{
				{invalid_ordinal, sqlite_store_fault_action::report_failure}};
			require(!invalid_scope.active() && invalid_scope.observation().invalid_plan,
					"invalid plan activated a TLS fault scope");
		}

		const auto target = publish_middle_chunk_after();
		{
			sqlite_store_fault_scope scope{
				{target, sqlite_store_fault_action::report_failure_after_delegate}};
			const auto ignored = dispatch_sqlite_store_fault(invalid_ordinal);
			const auto issued = dispatch_sqlite_store_fault(target);
			const auto observed = scope.observation();
			require(!ignored.issued && issued.issued && observed.observed_event_count == 2U &&
						observed.invalid_event_observed && observed.issued_directive_count == 1U,
					"invalid event altered matching or hid the later exact event");
		}
	}

	void check_nested_scope_and_thread_local_isolation()
	{
		const auto outer_event = publish_middle_chunk_after();
		const sqlite_store_fault_event inner_event{sqlite_store_operation::fresh_initialization,
												   sqlite_store_fault_boundary::ddl_object,
												   sqlite_store_fault_timing::before,
												   1U,
												   6U};
		sqlite_store_fault_scope outer{
			{outer_event, sqlite_store_fault_action::report_failure_after_delegate}};
		{
			sqlite_store_fault_scope inner{
				{inner_event, sqlite_store_fault_action::report_failure}};
			require(!inner.active() && inner.observation().nested_scope_suppressed,
					"nested scope displaced the outer TLS authority");
			require(!dispatch_sqlite_store_fault(inner_event).issued,
					"suppressed nested plan issued a directive");
		}

		sqlite_store_fault_action worker_without_scope{sqlite_store_fault_action::report_failure};
		sqlite_store_fault_action worker_with_scope{sqlite_store_fault_action::none};
		std::uint64_t worker_match_count{};
		std::thread worker{[&]()
						   {
							   worker_without_scope =
								   dispatch_sqlite_store_fault(outer_event).action;
							   sqlite_store_fault_scope local{
								   {outer_event, sqlite_store_fault_action::request_process_crash}};
							   worker_with_scope = dispatch_sqlite_store_fault(outer_event).action;
							   worker_match_count = local.observation().matching_event_count;
						   }};
		worker.join();
		require(worker_without_scope == sqlite_store_fault_action::none &&
					worker_with_scope == sqlite_store_fault_action::request_process_crash &&
					worker_match_count == 1U,
				"fault scope leaked across threads or blocked a thread-local scope");

		const auto main_directive = dispatch_sqlite_store_fault(outer_event);
		const auto observed = outer.observation();
		require(main_directive.issued &&
					main_directive.action ==
						sqlite_store_fault_action::report_failure_after_delegate &&
					observed.observed_event_count == 2U && observed.matching_event_count == 1U &&
					observed.issued_directive_count == 1U,
				"thread-local activity changed the outer scope receipt");
	}
} // namespace

int main()
{
	check_production_no_scope_is_noop();
	check_exact_typed_match_and_exactly_once_directive();
	check_passive_observation_never_changes_execution();
	check_coordination_and_journal_boundaries_are_closed_and_exact();
	check_crash_and_close_are_directives_only();
	check_invalid_plans_and_events_fail_closed();
	check_nested_scope_and_thread_local_isolation();
	std::cout << "sqlite Store fault injection tests passed\n";
	return 0;
}
