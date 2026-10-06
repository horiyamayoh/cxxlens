#pragma once

#include <optional>
#include <string_view>

namespace clang
{
	class FunctionDecl;
} // namespace clang

namespace cxxlens::detail::clang22
{
	/** Actual already parsed function type; observing it never resolves a lazy specification. */
	struct function_exception_specification
	{
		std::string_view kind{"no_prototype"};
		std::string_view state{"unavailable"};
		std::optional<bool> nonthrowing;
		static constexpr std::string_view profile{"clang22-function-exception-specification/1"};
	};

	[[nodiscard]] function_exception_specification
	observe_function_exception_specification(const clang::FunctionDecl&);
} // namespace cxxlens::detail::clang22
