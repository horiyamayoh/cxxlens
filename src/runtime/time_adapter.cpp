#include "time_port.hpp"

namespace cxxlens::detail::runtime
{

	std::chrono::system_clock::time_point real_time_adapter::wall_now() const noexcept
	{
		return std::chrono::system_clock::now();
	}

	std::chrono::steady_clock::time_point real_time_adapter::steady_now() const noexcept
	{
		return std::chrono::steady_clock::now();
	}

	fixed_time_adapter::fixed_time_adapter(
		const std::chrono::system_clock::time_point wall,
		const std::chrono::steady_clock::time_point steady) noexcept
		: wall_{wall}, steady_{steady}
	{
	}

	std::chrono::system_clock::time_point fixed_time_adapter::wall_now() const noexcept
	{
		return wall_;
	}

	std::chrono::steady_clock::time_point fixed_time_adapter::steady_now() const noexcept
	{
		return steady_;
	}

} // namespace cxxlens::detail::runtime
