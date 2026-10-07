#include "project_language_environment.hpp"

#include <clang/AST/ASTContext.h>

namespace cxxlens::detail::clang22
{
	original_language_environment
	observe_original_language_environment(const clang::ASTContext& context) noexcept
	{
		return {context.getLangOpts().Freestanding};
	}
} // namespace cxxlens::detail::clang22
