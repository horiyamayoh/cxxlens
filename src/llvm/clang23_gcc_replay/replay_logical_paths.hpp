#pragma once

#include <algorithm>
#include <string>
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

	[[nodiscard]] inline std::string replay_logical_path(const std::string_view filename)
	{
		auto relative = std::string{filename.substr(replay_synthetic_prefix.size())};
#if defined(_WIN32)
		std::ranges::replace(relative, '\\', '/');
#endif
		return std::string{replay_logical_prefix} + relative;
	}
} // namespace cxxlens::detail::clang23_gcc_replay
