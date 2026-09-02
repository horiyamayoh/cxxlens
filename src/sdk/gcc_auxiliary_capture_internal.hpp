#pragma once

/**
 * @file gcc_auxiliary_capture_internal.hpp
 * @brief Bounded GCC 16.2 response, spec, and dependency-file capture.
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
	inline constexpr std::size_t gcc_16_2_maximum_response_expansions{1999U};

	class gcc_capture_file_port;
	class gcc_capture_workspace;

	struct gcc_auxiliary_capture
	{
		gcc_invocation_observation invocation;
		std::vector<gcc_source_closure_member_observation> closure_members;
		std::uint64_t captured_bytes{};
	};

	struct gcc_prepared_auxiliary_capture
	{
		std::vector<std::string> expanded_arguments;
		std::vector<build_capture_auxiliary_file> response_files;
		std::vector<build_capture_auxiliary_file> config_files;
		std::vector<gcc_source_closure_member_observation> closure_members;
		std::uint64_t captured_bytes{};
	};

	struct gcc_environment_capture_request
	{
		std::span<const std::string> environment;
		std::string_view canonical_working_directory;
		std::string_view canonical_project_root;
		std::string_view language;
		std::size_t maximum_environment_count{};
		std::size_t maximum_environment_bytes{};
		std::size_t maximum_canonical_path_bytes{};
	};

	struct gcc_invocation_plan_request
	{
		std::span<const std::string> expanded_arguments;
		std::string_view compiler_path;
		std::string_view injected_dependency_output_path;
	};

	struct gcc_invocation_plan
	{
		std::string source_path;
		std::string language;
		std::vector<std::string> capture_arguments;
		std::string dependency_output_path;
		bool dependency_arguments_injected{};

		[[nodiscard]] bool operator==(const gcc_invocation_plan&) const = default;
	};

	/** Parse bytes with the exact GCC 16.2 libiberty buildargv quoting rules. */
	[[nodiscard]] result<std::vector<std::string>>
	parse_gcc_16_2_response_arguments(std::span<const std::byte> content,
									  import_limits limits = {});

	/** Parse the first GCC 16.2 Make dependency rule into its ordered prerequisites. */
	[[nodiscard]] result<std::vector<std::string>>
	parse_gcc_16_2_dependency_output(std::span<const std::byte> content, import_limits limits = {});

	/** Normalize only allowlisted GCC semantic environment effects; raw values are never returned.
	 */
	[[nodiscard]] result<std::vector<build_capture_environment_effect>>
	capture_gcc_16_2_environment_effects(gcc_capture_file_port& files,
										 const gcc_environment_capture_request& request,
										 import_limits limits = {});

	/** Plan one ordinary GCC 16.2 compile without a shell or implicit source selection. */
	[[nodiscard]] result<gcc_invocation_plan>
	plan_gcc_16_2_invocation(const gcc_invocation_plan_request& request, import_limits limits = {});

	/** Snapshot and recursively expand every GCC response file before compiler execution. */
	[[nodiscard]] result<gcc_prepared_auxiliary_capture>
	prepare_gcc_16_2_response_files(gcc_capture_file_port& files,
									std::span<const std::string> arguments,
									std::string_view canonical_working_directory,
									std::string_view canonical_project_root,
									std::uint64_t maximum_capture_bytes,
									import_limits limits = {});

	/** Snapshot GCC specs and rewrite only the execution argv to sealed private copies. */
	[[nodiscard]] result<gcc_prepared_auxiliary_capture>
	prepare_gcc_16_2_spec_files(gcc_capture_file_port& files,
								gcc_capture_workspace& workspace,
								gcc_prepared_auxiliary_capture prepared,
								std::string_view canonical_working_directory,
								std::string_view canonical_project_root,
								std::uint64_t maximum_capture_bytes,
								import_limits limits = {});

	/** Capture explicit response/spec/dependency files reachable from one original GCC argv. */
	[[nodiscard]] result<gcc_auxiliary_capture>
	capture_gcc_auxiliary_files(gcc_capture_file_port& files,
								std::span<const std::string> arguments,
								std::string_view canonical_working_directory,
								std::string_view canonical_project_root,
								std::string_view canonical_main_source,
								std::uint64_t maximum_capture_bytes,
								import_limits limits = {},
								bool dependency_output_bound_to_invocation = false,
								gcc_prepared_auxiliary_capture prepared_auxiliary = {});
} // namespace cxxlens::sdk::detail
