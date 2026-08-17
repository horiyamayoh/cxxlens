#pragma once

#include "compiler_vfs.hpp"

#include <expected>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace clang
{
	class ASTContext;
	class SourceManager;
} // namespace clang

namespace cxxlens::detail::clang22::source_closure
{
	struct clang_run_error
	{
		std::string code;
		std::string path;
		std::string detail;

		[[nodiscard]] bool operator==(const clang_run_error&) const = default;
	};

	using clang_translation_unit_callback = std::move_only_function<
		std::expected<void, clang_run_error>(clang::ASTContext&, clang::SourceManager&)>;

	/**
	 * Parse exactly one main source through an in-memory-only Clang filesystem.
	 * Project/generated lookups cannot fall through to the host filesystem.
	 * Toolchain, sysroot, module, and PCH overlays are deliberately unsupported in v1.
	 */
	[[nodiscard]] std::expected<void, clang_run_error>
	run_with_compiler_vfs(std::shared_ptr<const validated_snapshot> snapshot,
		std::span<const std::string> arguments,
		std::string_view logical_working_directory,
		clang_translation_unit_callback callback);
} // namespace cxxlens::detail::clang22::source_closure
