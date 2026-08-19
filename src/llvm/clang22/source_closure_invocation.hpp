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
	 * Validate one effective invocation and rewrite only known path-bearing options. Response
	 * files, VFS overlays, modules, and PCH inputs fail closed here, as do relative and
	 * unqualified host paths given to any option this function recognizes.
	 *
	 * The recognized set is explicitly not exhaustive. Other path-bearing spellings -- `-F`,
	 * `-B`, `-iframework`, `-iwithprefix`, `-iwithprefixbefore`, `-iwithsysroot`,
	 * `-isystem-after`, `-fcrash-diagnostics-dir=`, and `-Xclang -internal-isystem <dir>` among
	 * them -- are passed through verbatim rather than rejected, so this function alone must not
	 * be read as a complete argument-admission boundary. In practice such paths are still
	 * contained downstream: the compiler-facing VFS serves only the closure and the admitted
	 * toolchain roots, and answers everything else with ENOENT. (The split `-Xclang -ivfsoverlay`
	 * spelling is rejected here, since an overlay would otherwise redefine that very VFS.)
	 */
	[[nodiscard]] sdk::result<source_closure_invocation>
	prepare_source_closure_invocation(std::span<const std::string> effective_arguments,
									  std::string_view main_logical_path,
									  std::string_view logical_working_directory,
									  std::span<const std::string> qualified_read_roots = {});
} // namespace cxxlens::detail::clang22
