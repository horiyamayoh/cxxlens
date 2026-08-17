#pragma once

#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
#include <llvm/ADT/IntrusiveRefCntPtr.h>

namespace llvm::vfs
{
	class FileSystem;
}
#endif

namespace cxxlens::provider::clang22::detail
{
#if defined(CXXLENS_HAS_CLANG22) && CXXLENS_HAS_CLANG22
	/** Source-private execution seam for an already authenticated compiler-facing VFS. */
	[[nodiscard]] sdk::result<void> with_translation_unit_vfs(
		const translation_unit_input& input,
		std::string compiler_filename,
		std::string tool_name,
		std::vector<std::string> compiler_arguments,
		llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> filesystem,
		translation_unit_callback callback);
#endif
} // namespace cxxlens::provider::clang22::detail
