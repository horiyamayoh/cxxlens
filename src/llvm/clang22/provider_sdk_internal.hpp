#pragma once

#include <cstdint>
#include <string>
#include <string_view>
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
	/** Scalar parser observations captured before invoking the semantic extractor. */
	struct native_parse_observation
	{
		bool attempted{};
		bool ast_completed{};
		std::uint64_t error_count{};
		std::uint64_t fatal_error_count{};
		[[nodiscard]] std::string_view outcome() const noexcept
		{
			if (!attempted)
				return "unavailable";
			if (!ast_completed || fatal_error_count != 0U)
				return "failed";
			return error_count == 0U ? "success" : "recovery";
		}
	};
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
							  preprocessor_setup setup = {},
							  native_parse_observation* parse_observation = nullptr);
#endif
} // namespace cxxlens::provider::clang22::detail
