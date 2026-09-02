#pragma once

/**
 * @file gcc_auxiliary_capture_internal.hpp
 * @brief Bounded GCC 16.2 response/spec-file capture.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "gcc_capture_bundle_internal.hpp"

namespace cxxlens::sdk::detail
{
	class gcc_capture_file_port;

	struct gcc_auxiliary_capture
	{
		gcc_invocation_observation invocation;
		std::vector<gcc_source_closure_member_observation> closure_members;
		std::uint64_t captured_bytes{};
	};

	/** Parse bytes with the exact GCC 16.2 libiberty buildargv quoting rules. */
	[[nodiscard]] result<std::vector<std::string>>
	parse_gcc_16_2_response_arguments(std::span<const std::byte> content,
									  import_limits limits = {});

	/** Capture explicit response/spec files reachable from one original GCC argv. */
	[[nodiscard]] result<gcc_auxiliary_capture>
	capture_gcc_auxiliary_files(gcc_capture_file_port& files,
								std::span<const std::string> arguments,
								std::string_view canonical_working_directory,
								std::string_view canonical_project_root,
								std::uint64_t maximum_capture_bytes,
								import_limits limits = {});
} // namespace cxxlens::sdk::detail
