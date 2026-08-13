#include "sqlite_limit_length_control_internal.hpp"

#include <algorithm>
#include <limits>

namespace cxxlens::sdk
{
	namespace
	{
		thread_local sqlite_limit_length_control_scope* active_scope{};
	} // namespace

	sqlite_limit_length_control_scope::sqlite_limit_length_control_scope(
		const sqlite_limit_length_boundary boundary) noexcept
		: boundary_{boundary}
	{
		receipt_.requested_limit_length = sqlite_limit_length_boundary_value(boundary);
		if (receipt_.requested_limit_length < 0 || active_scope != nullptr)
			return;
		active_scope = this;
		active_ = true;
	}

	sqlite_limit_length_control_scope::~sqlite_limit_length_control_scope() noexcept
	{
		if (active_ && active_scope == this)
			active_scope = nullptr;
	}

	bool sqlite_limit_length_control_scope::active() const noexcept
	{
		return active_;
	}

	sqlite_limit_length_boundary sqlite_limit_length_control_scope::boundary() const noexcept
	{
		return boundary_;
	}

	sqlite_limit_length_control_receipt
	sqlite_limit_length_control_scope::observation() const noexcept
	{
		return receipt_;
	}

	void sqlite_limit_length_control_scope::record(const int actual_limit) noexcept
	{
		if (receipt_.observed_connection_count == std::numeric_limits<std::uint64_t>::max())
		{
			receipt_.observation_count_overflow = true;
		}
		else
		{
			++receipt_.observed_connection_count;
		}
		if (receipt_.observed_connection_count == 1U)
		{
			receipt_.minimum_actual_limit = actual_limit;
			receipt_.maximum_actual_limit = actual_limit;
		}
		else
		{
			receipt_.minimum_actual_limit = std::min(receipt_.minimum_actual_limit, actual_limit);
			receipt_.maximum_actual_limit = std::max(receipt_.maximum_actual_limit, actual_limit);
		}
		receipt_.all_actual_limits_exact =
			receipt_.all_actual_limits_exact && actual_limit == receipt_.requested_limit_length;
	}

	int observe_actual_sqlite_limit_length(const sqlite_limit_length_function limit,
										   void* const actual_connection) noexcept
	{
		if (limit == nullptr || actual_connection == nullptr)
		{
			if (active_scope != nullptr)
				active_scope->record(-1);
			return -1;
		}

		if (active_scope != nullptr)
		{
			(void)limit(actual_connection,
						sqlite_limit_length_category,
						active_scope->receipt_.requested_limit_length);
		}
		const auto actual = limit(actual_connection, sqlite_limit_length_category, -1);
		if (active_scope != nullptr)
			active_scope->record(actual);
		return actual;
	}
} // namespace cxxlens::sdk
