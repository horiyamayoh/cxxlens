#pragma once

/**
 * @file gcc_capture_file_port_internal.hpp
 * @brief Source-private filesystem authority for bounded GCC build capture.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "gcc_auxiliary_capture_internal.hpp"
#include "gcc_toolchain_probe_internal.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_SDK_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_SDK_DETAIL_HIDDEN
#endif

namespace cxxlens::sdk::detail
{
	inline constexpr std::string_view capture_file_unavailable_code{
		"application-analysis.capture-file-unavailable"};

	struct capture_file_snapshot
	{
		std::string canonical_path;
		std::vector<std::byte> content;
	};

	struct capture_file_read_limits
	{
		std::uint64_t maximum_file_bytes{};
		std::size_t maximum_path_bytes{};
		std::string_view required_canonical_root;
	};

	class gcc_capture_workspace
	{
	  public:
		virtual ~gcc_capture_workspace() = default;
		[[nodiscard]] virtual std::string_view dependency_output_path() const noexcept = 0;
		[[nodiscard]] virtual result<std::string>
		publish_bundle(std::span<const std::byte> content) = 0;
	};

	class gcc_capture_file_port
	{
	  public:
		virtual ~gcc_capture_file_port() = default;
		[[nodiscard]] virtual result<std::string>
		canonical_directory(std::string_view path, std::size_t maximum_path_bytes) = 0;
		[[nodiscard]] virtual result<capture_file_snapshot>
		read_regular_file(std::string_view path, capture_file_read_limits limits) = 0;
		[[nodiscard]] virtual result<std::unique_ptr<gcc_capture_workspace>>
		create_workspace(std::string_view capture_directory, std::size_t maximum_path_bytes)
		{
			(void)capture_directory;
			(void)maximum_path_bytes;
			return unexpected(
				{std::string{capture_file_unavailable_code}, "capture.workspace", "unsupported"});
		}
	};

	struct CXXLENS_SDK_DETAIL_HIDDEN gcc_compile_commands_capture_request
	{
		std::string project_id;
		std::string project_root;
		std::string compile_commands_path;
		std::string compiler_path;
		std::vector<std::string> execution_environment;
		gcc_probe_process_limits process_limits;
		std::uint64_t absolute_wall_deadline_ns{};
	};

	struct CXXLENS_SDK_DETAIL_HIDDEN gcc_invocation_capture_request
	{
		std::string project_id;
		std::string project_root;
		std::string working_directory;
		std::string source_path;
		std::string compiler_path;
		std::vector<std::string> original_arguments;
		std::vector<std::string> capture_arguments;
		std::vector<build_capture_environment_effect> environment_effects;
		std::vector<std::string> execution_environment;
		gcc_probe_process_limits process_limits;
		std::uint64_t absolute_wall_deadline_ns{};
		std::string expected_compiler_path;
		std::string expected_compiler_digest;
	};

	/** Probe one exact GCC toolchain, then read its compilation database and main sources. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_compile_commands(gcc_capture_file_port& files,
								 gcc_probe_process_port& processes,
								 const gcc_compile_commands_capture_request& request,
								 import_limits limits = {},
								 const std::stop_token& cancellation = {});

	/** Capture one shell-free invocation whose dependency output came from that execution. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_invocation(gcc_capture_file_port& files,
						   gcc_probe_process_port& processes,
						   const gcc_invocation_capture_request& request,
						   import_limits limits = {},
						   const std::stop_token& cancellation = {},
						   gcc_prepared_response_capture prepared_responses = {});

	/** Linux production adapter. Unsupported hosts return structured-unavailable failures. */
	[[nodiscard]] std::unique_ptr<gcc_capture_file_port> make_system_gcc_capture_file_port();
} // namespace cxxlens::sdk::detail

#undef CXXLENS_SDK_DETAIL_HIDDEN
