#pragma once

#include <cstdint>

#include "sqlite_store_terminal_internal.hpp"

namespace cxxlens::sdk
{
	/** Typed Store/SQLite boundaries; no SQL text or diagnostic prose participates in matching. */
	enum class sqlite_store_fault_boundary : std::uint8_t
	{
		transaction_begin,
		wal_coordination,
		journal_transition,
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
		source_shm_symbol_resolution,
	};

	enum class sqlite_store_fault_timing : std::uint8_t
	{
		before,
		after,
	};

	/**
	 * A directive requested by source-private fault tests.
	 *
	 * The production dispatcher performs no failure, close, or process action. These actions are
	 * interpreted only by Store integration or a subprocess test after a test scope is installed.
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

	/** Allocation-free instruction returned to the exact integration call site. */
	struct sqlite_store_fault_directive
	{
		sqlite_store_fault_event event;
		sqlite_store_fault_action action{sqlite_store_fault_action::none};
		bool issued{};

		[[nodiscard]] bool operator==(const sqlite_store_fault_directive&) const = default;
	};

	/** Production no-op unless a BUILD_TESTING scope owns the calling thread. */
#if defined(__GNUC__) || defined(__clang__)
	// The test dispatcher is intentionally interposable by the private test object library.  The
	// production no-op remains source-private so it cannot become an installed testing surface.
#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]] __attribute__((visibility("hidden")))
#endif
#else
	[[nodiscard]]
#endif
	sqlite_store_fault_directive
	dispatch_sqlite_store_fault(const sqlite_store_fault_event& event) noexcept;
} // namespace cxxlens::sdk
