#pragma once

/**
 * @file replay_frontend_authority.hpp
 * @brief Exact compiler replay frontend owned by the GCC-mode worker closure.
 */

#include <string_view>

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::string_view gcc_replay_frontend_id = "clang-23.1.0-gcc-mode";
	inline constexpr std::string_view msvc_replay_frontend_id = "clang-cl-23.1.0";
} // namespace cxxlens::detail::clang23_gcc_replay
