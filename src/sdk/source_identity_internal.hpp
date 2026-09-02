#pragma once

/**
 * @file source_identity_internal.hpp
 * @brief Canonical source.file and source snapshot identity projections.
 */

#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::detail
{
	/** Derive a file ID from one validated project-relative logical path. */
	[[nodiscard]] result<std::string>
	derive_source_file_id(std::string_view project_relative_logical_path);

	/** Derive a source snapshot ID from validated file, content, and encoding authority. */
	[[nodiscard]] result<std::string> derive_source_snapshot_id(std::string_view file_id,
																std::string_view content_digest,
																std::string_view encoding);
} // namespace cxxlens::sdk::detail
