#pragma once

#include <cstddef>
#include <string_view>

namespace cxxlens::provider::clang22::detail
{
	// Existing single-source SDK preflight only. The source-closure identity,
	// Unicode, case, and compiler VFS rules remain blocked under DF-0261.
	[[nodiscard]] constexpr bool
	contains_parent_path_segment(const std::string_view value) noexcept
	{
		std::size_t begin{};
		while (begin <= value.size())
		{
			const auto end = value.find_first_of("/\\", begin);
			const auto segment = value.substr(
				begin, end == std::string_view::npos ? std::string_view::npos : end - begin);
			if (segment == "..")
				return true;
			if (end == std::string_view::npos)
				return false;
			begin = end + 1U;
		}
		return false;
	}

	static_assert(!contains_parent_path_segment("main.cpp"));
	static_assert(!contains_parent_path_segment("src/foo..bar.hpp"));
	static_assert(!contains_parent_path_segment("src/..hidden.hpp"));
	static_assert(!contains_parent_path_segment("src/hidden..hpp"));
	static_assert(!contains_parent_path_segment("project://src/.hidden.hpp"));
	static_assert(!contains_parent_path_segment("..."));
	static_assert(contains_parent_path_segment(".."));
	static_assert(contains_parent_path_segment("../main.cpp"));
	static_assert(contains_parent_path_segment("src/../main.cpp"));
	static_assert(contains_parent_path_segment("src\\..\\main.cpp"));
	static_assert(contains_parent_path_segment("project://../main.cpp"));
} // namespace cxxlens::provider::clang22::detail
