#pragma once

#include <string>
#include <vector>

#include <cxxlens/provider/clang22.hpp>

#include "source_closure.hpp"

namespace cxxlens::detail::clang22
{
	/** Source-private native execution input for one authenticated source closure. */
	struct source_closure_native_input
	{
		source_closure_snapshot closure;
		std::string main_logical_path;
		std::string logical_working_directory;
		std::vector<std::string> effective_arguments;
		std::vector<std::string> qualified_read_roots;
	};

	/** Execute one fresh Clang job through a closure-exclusive synthetic project filesystem. */
	[[nodiscard]] sdk::result<void> with_source_closure_translation_unit(
		const source_closure_native_input& input,
		provider::clang22::translation_unit_callback callback);
} // namespace cxxlens::detail::clang22
