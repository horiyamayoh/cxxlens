#pragma once

#include <cstdint>

#include "sqlite_store_terminal_internal.hpp"

namespace cxxlens::sdk
{
	/** Typed Store/SQLite boundaries; no SQL text or diagnostic prose participates in matching. */
	enum class sqlite_store_fault_boundary : std::uint8_t
	{
		transaction_begin,
		ddl_object,
		metadata_row,
		payload_chunk,
		final_object_copy,
		format_marker,
		transaction_commit,
		transaction_rollback,
		statement_finalize,
		connection_close,
		terminal_namespace_census,
		terminal_reopen,
		terminal_validation,
	};

	enum class sqlite_store_fault_timing : std::uint8_t
	{
		before,
		after,
	};

	/**
	 * A directive requested by a source-private test scope.
	 *
	 * The dispatcher performs no failure, close, or process action. In particular, process crash
	 * and close-non-OK directives are interpreted only by Store integration or a subprocess test.
	 */
	enum class sqlite_store_fault_action : std::uint8_t
	{
		none,
		observe_only,
		report_failure,
		report_failure_after_delegate,
		request_process_crash,
		request_close_non_ok,
	};

	/** One one-based occurrence at an exact typed boundary. */
	struct sqlite_store_fault_event
	{
		sqlite_store_operation operation{sqlite_store_operation::publish};
		sqlite_store_fault_boundary boundary{sqlite_store_fault_boundary::transaction_begin};
		sqlite_store_fault_timing timing{sqlite_store_fault_timing::before};
		std::uint64_t ordinal{1U};
		std::uint64_t total{1U};

		[[nodiscard]] bool operator==(const sqlite_store_fault_event&) const = default;
	};

	struct sqlite_store_fault_plan
	{
		sqlite_store_fault_event event;
		sqlite_store_fault_action action{sqlite_store_fault_action::none};

		[[nodiscard]] bool operator==(const sqlite_store_fault_plan&) const = default;
	};

	/** Allocation-free instruction returned to the exact integration call site. */
	struct sqlite_store_fault_directive
	{
		sqlite_store_fault_event event;
		sqlite_store_fault_action action{sqlite_store_fault_action::none};
		bool issued{};

		[[nodiscard]] bool operator==(const sqlite_store_fault_directive&) const = default;
	};

	/** Allocation-free observation of dispatcher activity within one scope. */
	struct sqlite_store_fault_observation
	{
		std::uint64_t observed_event_count{};
		std::uint64_t matching_event_count{};
		std::uint64_t issued_directive_count{};
		sqlite_store_fault_event last_observed_event;
		sqlite_store_fault_event matched_event;
		bool has_last_observed_event{};
		bool has_matched_event{};
		bool count_overflow{};
		bool invalid_plan{};
		bool invalid_event_observed{};
		bool nested_scope_suppressed{};

		[[nodiscard]] bool operator==(const sqlite_store_fault_observation&) const = default;
	};

	/** Only an exact event with `1 <= ordinal <= total` is admissible. */
	[[nodiscard]] constexpr bool
	valid_sqlite_store_fault_event(const sqlite_store_fault_event& event) noexcept
	{
		const auto valid_operation = [](const sqlite_store_operation value) constexpr
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
		};
		const auto valid_boundary = [](const sqlite_store_fault_boundary value) constexpr
		{
			switch (value)
			{
				case sqlite_store_fault_boundary::transaction_begin:
				case sqlite_store_fault_boundary::ddl_object:
				case sqlite_store_fault_boundary::metadata_row:
				case sqlite_store_fault_boundary::payload_chunk:
				case sqlite_store_fault_boundary::final_object_copy:
				case sqlite_store_fault_boundary::format_marker:
				case sqlite_store_fault_boundary::transaction_commit:
				case sqlite_store_fault_boundary::transaction_rollback:
				case sqlite_store_fault_boundary::statement_finalize:
				case sqlite_store_fault_boundary::connection_close:
				case sqlite_store_fault_boundary::terminal_namespace_census:
				case sqlite_store_fault_boundary::terminal_reopen:
				case sqlite_store_fault_boundary::terminal_validation:
					return true;
			}
			return false;
		};
		const auto valid_timing = [](const sqlite_store_fault_timing value) constexpr
		{
			switch (value)
			{
				case sqlite_store_fault_timing::before:
				case sqlite_store_fault_timing::after:
					return true;
			}
			return false;
		};
		return valid_operation(event.operation) && valid_boundary(event.boundary) &&
			valid_timing(event.timing) && event.total != 0U && event.ordinal != 0U &&
			event.ordinal <= event.total;
	}

	[[nodiscard]] constexpr bool
	valid_sqlite_store_fault_plan(const sqlite_store_fault_plan& plan) noexcept
	{
		const auto valid_action = [](const sqlite_store_fault_action value) constexpr
		{
			switch (value)
			{
				case sqlite_store_fault_action::observe_only:
				case sqlite_store_fault_action::report_failure:
				case sqlite_store_fault_action::report_failure_after_delegate:
				case sqlite_store_fault_action::request_process_crash:
				case sqlite_store_fault_action::request_close_non_ok:
					return true;
				case sqlite_store_fault_action::none:
					return false;
			}
			return false;
		};
		if (!valid_sqlite_store_fault_event(plan.event) || !valid_action(plan.action))
			return false;
		if (plan.action == sqlite_store_fault_action::report_failure_after_delegate &&
			plan.event.timing != sqlite_store_fault_timing::after)
			return false;
		if (plan.action == sqlite_store_fault_action::request_close_non_ok &&
			plan.event.boundary != sqlite_store_fault_boundary::connection_close)
			return false;
		return true;
	}

	/**
	 * Source-private thread-local fault scope.
	 *
	 * Only the outermost valid scope on a thread is active. A fault plan issues at most one
	 * directive, even if an integration bug dispatches the same event more than once. An
	 * `observe_only` plan never issues a directive and counts every exact matching event, providing
	 * passive qualification evidence without changing execution.
	 */
#if defined(__GNUC__) || defined(__clang__)
	class __attribute__((visibility("default"))) sqlite_store_fault_scope
#else
	class sqlite_store_fault_scope
#endif
	{
	  public:
		explicit sqlite_store_fault_scope(sqlite_store_fault_plan plan) noexcept;
		~sqlite_store_fault_scope() noexcept;

		sqlite_store_fault_scope(const sqlite_store_fault_scope&) = delete;
		sqlite_store_fault_scope& operator=(const sqlite_store_fault_scope&) = delete;
		sqlite_store_fault_scope(sqlite_store_fault_scope&&) = delete;
		sqlite_store_fault_scope& operator=(sqlite_store_fault_scope&&) = delete;

		[[nodiscard]] bool active() const noexcept;
		[[nodiscard]] const sqlite_store_fault_plan& plan() const noexcept;
		[[nodiscard]] sqlite_store_fault_observation observation() const noexcept;

	  private:
		friend sqlite_store_fault_directive
		dispatch_sqlite_store_fault(const sqlite_store_fault_event&) noexcept;

		[[nodiscard]] sqlite_store_fault_directive
		dispatch(const sqlite_store_fault_event& event) noexcept;
		void increment(std::uint64_t& value) noexcept;

		sqlite_store_fault_plan plan_;
		sqlite_store_fault_observation observation_;
		bool active_{};
		bool directive_issued_{};
	};

	/** Production no-op unless the calling thread owns one active private fault scope. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	sqlite_store_fault_directive
	dispatch_sqlite_store_fault(const sqlite_store_fault_event& event) noexcept;
} // namespace cxxlens::sdk
