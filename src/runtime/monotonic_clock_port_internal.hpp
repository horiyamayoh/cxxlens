#pragma once

#include <chrono>

namespace cxxlens::runtime
{
	/** Source-private monotonic clock port for bounded runtime coordination. */
	[[nodiscard]] std::chrono::steady_clock::time_point monotonic_now() noexcept;
} // namespace cxxlens::runtime
