#pragma once

#include <string_view>

namespace cxxlens::detail::clang23_gcc_replay
{
	inline constexpr std::string_view replay_logical_prefix{"project://"};
#if defined(_WIN32)
	inline constexpr std::string_view replay_synthetic_root{"C:/__cxxlens_replay__"};
	inline constexpr std::string_view replay_synthetic_prefix{"C:/__cxxlens_replay__/"};
#else
	inline constexpr std::string_view replay_synthetic_root{"/__cxxlens_gcc_replay__"};
	inline constexpr std::string_view replay_synthetic_prefix{"/__cxxlens_gcc_replay__/"};
#endif
} // namespace cxxlens::detail::clang23_gcc_replay
