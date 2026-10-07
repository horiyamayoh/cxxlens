#pragma once

namespace clang
{
	class ASTContext;
} // namespace clang

namespace cxxlens::detail::clang22
{
	// Native observation only. The owning collector binds the actual unit/world.
	struct original_language_environment
	{
		bool freestanding;
	};

	[[nodiscard]] original_language_environment
	observe_original_language_environment(const clang::ASTContext&) noexcept;
} // namespace cxxlens::detail::clang22
