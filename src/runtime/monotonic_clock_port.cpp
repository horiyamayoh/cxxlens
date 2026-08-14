#include "monotonic_clock_port_internal.hpp"

namespace cxxlens::runtime
{
	std::chrono::steady_clock::time_point monotonic_now() noexcept
	{
		return std::chrono::steady_clock::now();
	}
} // namespace cxxlens::runtime
