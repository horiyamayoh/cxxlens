#pragma once

/**
 * @file msvc_capture_bundle.hpp
 * @brief Portable canonical capture encoder used by the minimal Windows capture component.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::application_analysis_worker
{
	struct msvc_capture_limits
	{
		std::size_t maximum_arguments{4096U};
		std::size_t maximum_nesting_depth{32U};
		std::size_t maximum_sources{4096U};
		std::size_t maximum_response_files{4096U};
		std::size_t maximum_string_bytes{4096U};
		std::uint64_t maximum_source_bytes{std::uint64_t{48U} * 1024U * 1024U};
		std::size_t maximum_bundle_bytes{std::size_t{64U} * 1024U * 1024U};
	};

	struct captured_source
	{
		std::string canonical_path;
		std::vector<std::byte> content;
		std::string role{"header"};
		std::string encoding{"binary_or_unknown"};
	};

	struct captured_response_file
	{
		std::string canonical_path;
		std::vector<std::byte> content;
		std::optional<std::size_t> parent_index;
	};

	struct unavailable_capture_field
	{
		std::string reason;
		std::string completion_action;
	};

	struct msvc_capture_input
	{
		std::string project_id;
		std::string canonical_project_root;
		std::string canonical_working_directory;
		std::string canonical_compiler_path;
		std::string compiler_binary_digest;
		std::string windows_sdk_root;
		std::string abi_digest;
		std::string builtin_headers_digest;
		std::string builtin_macros_digest;
		std::string include_search_digest;
		std::vector<std::string> original_arguments;
		captured_source main_source;
		std::vector<captured_source> dependency_sources;
		std::vector<captured_response_file> response_files;
		std::optional<unavailable_capture_field> source_closure_membership;
		std::string language_standard{"c++latest"};
	};

	/** Encode one complete, bounded MSVC compile-unit capture without Store authority. */
	[[nodiscard]] cxxlens::sdk::result<std::vector<std::byte>>
	encode_msvc_capture_bundle(const msvc_capture_input& input, msvc_capture_limits limits = {});
} // namespace cxxlens::application_analysis_worker
