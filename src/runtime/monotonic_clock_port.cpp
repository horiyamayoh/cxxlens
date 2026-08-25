#include "monotonic_clock_port_internal.hpp"

namespace cxxlens::runtime
{
	std::chrono::steady_clock::time_point monotonic_now() noexcept
	{
		return std::chrono::steady_clock::now();
	}

	std::chrono::system_clock::time_point wall_clock_now() noexcept
	{
		return std::chrono::system_clock::now();
	}
} // namespace cxxlens::runtime
