#pragma once

#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <llvm/ADT/IntrusiveRefCntPtr.h>

namespace llvm::vfs
{
	class FileSystem;
} // namespace llvm::vfs
#endif

namespace cxxlens::provider::clang22::detail
{
	/** Configure provider-owned PP callbacks before preprocessing, within one native job. */
	using preprocessor_setup = std::move_only_function<void(clang::Preprocessor&)>;
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
	/** Source-private execution seam for an already authenticated compiler-facing VFS. */
	[[nodiscard]] sdk::result<void>
	with_translation_unit_vfs(const translation_unit_input& input,
							  const std::string& compiler_filename,
							  const std::string& tool_name,
							  const std::vector<std::string>& compiler_arguments,
							  llvm::vfs::FileSystem& filesystem,
							  translation_unit_callback callback,
							  preprocessor_setup setup = {});
#endif
} // namespace cxxlens::provider::clang22::detail
