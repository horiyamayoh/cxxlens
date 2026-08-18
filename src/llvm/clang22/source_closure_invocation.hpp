#pragma once

#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::detail::clang22
{
	/** Compiler invocation after all project paths are bound to the synthetic closure root. */
	struct source_closure_invocation
	{
		std::string tool_name;
		std::string compiler_filename;
		std::string working_directory;
		std::vector<std::string> compiler_arguments;
		std::vector<std::string> qualified_read_roots;
	};

	/**
	 * Validate one effective invocation and rewrite only known path-bearing options. Relative or
	 * unqualified host paths, response files, VFS overlays, modules, and PCH inputs fail closed.
	 */
	[[nodiscard]] sdk::result<source_closure_invocation> prepare_source_closure_invocation(
		std::span<const std::string> effective_arguments,
		std::string_view main_logical_path,
		std::string_view logical_working_directory,
		std::span<const std::string> qualified_read_roots = {});
} // namespace cxxlens::detail::clang22
