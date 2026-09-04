#pragma once

/**
 * @file replay_frontend_authority.hpp
 * @brief Exact compiler replay frontend owned by the GCC-mode worker closure.
 */

#include <string_view>

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::string_view replay_frontend_id = "clang-23.1.0-gcc-mode";
} // namespace cxxlens::detail::clang23_gcc_replay
