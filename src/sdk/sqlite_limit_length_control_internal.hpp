#pragma once

#include <cstdint>

namespace cxxlens::sdk
{
	inline constexpr int sqlite_limit_length_category = 0;
	inline constexpr int sqlite_limit_length_minimum = 16'777'216;

	/** Closed qualification boundaries for SQLite's per-connection length limit. */
	enum class sqlite_limit_length_boundary : std::uint8_t
	{
		one_below_minimum,
		exact_minimum,
	};

	[[nodiscard]] constexpr int
	sqlite_limit_length_boundary_value(const sqlite_limit_length_boundary boundary) noexcept
	{
		switch (boundary)
		{
			case sqlite_limit_length_boundary::one_below_minimum:
				return sqlite_limit_length_minimum - 1;
			case sqlite_limit_length_boundary::exact_minimum:
				return sqlite_limit_length_minimum;
		}
		return -1;
	}

	using sqlite_limit_length_function = int (*)(void*, int, int);

	/** Allocation-free receipt for every actual connection observed in one scoped operation. */
	struct sqlite_limit_length_control_receipt
	{
		int requested_limit_length{};
		std::uint64_t observed_connection_count{};
		int minimum_actual_limit{};
		int maximum_actual_limit{};
		bool all_actual_limits_exact{true};
		bool observation_count_overflow{};
	};

	/**
	 * Source-private, thread-local qualification control.
	 *
	 * Only the outermost scope on a thread becomes active. The scope must outlive every Store
	 * operation whose SQLite connections it is intended to observe. Production code creates no
	 * scope, so the helper remains query-only.
	 */
#if defined(__GNUC__) || defined(__clang__)
	class __attribute__((visibility("default"))) sqlite_limit_length_control_scope
#else
	class sqlite_limit_length_control_scope
#endif
	{
	  public:
		explicit sqlite_limit_length_control_scope(sqlite_limit_length_boundary boundary) noexcept;
		~sqlite_limit_length_control_scope() noexcept;

		sqlite_limit_length_control_scope(const sqlite_limit_length_control_scope&) = delete;
		sqlite_limit_length_control_scope&
		operator=(const sqlite_limit_length_control_scope&) = delete;
		sqlite_limit_length_control_scope(sqlite_limit_length_control_scope&&) = delete;
		sqlite_limit_length_control_scope& operator=(sqlite_limit_length_control_scope&&) = delete;

		[[nodiscard]] bool active() const noexcept;
		[[nodiscard]] sqlite_limit_length_boundary boundary() const noexcept;
		[[nodiscard]] sqlite_limit_length_control_receipt observation() const noexcept;

	  private:
		friend int observe_actual_sqlite_limit_length(sqlite_limit_length_function, void*) noexcept;

		void record(int actual_limit) noexcept;

		sqlite_limit_length_boundary boundary_;
		sqlite_limit_length_control_receipt receipt_;
		bool active_{};
	};

	/**
	 * Query the actual connection's SQLITE_LIMIT_LENGTH.
	 *
	 * With no active control scope this is exactly `sqlite3_limit(db, LENGTH, -1)`. With an active
	 * scope it first sets that same raw connection to the scope's closed boundary and then queries
	 * the effective value, allowing SQLite compile-time clamping to remain observable.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	int observe_actual_sqlite_limit_length(sqlite_limit_length_function limit,
										   void* actual_connection) noexcept;
} // namespace cxxlens::sdk
