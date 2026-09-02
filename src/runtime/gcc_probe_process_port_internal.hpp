#pragma once

/** @file gcc_probe_process_port_internal.hpp
 * @brief Source-private shell-free process authority for bounded GCC introspection.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_RUNTIME_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_RUNTIME_DETAIL_HIDDEN
#endif

namespace cxxlens::sdk::detail
{
	enum class gcc_probe_process_terminal : std::uint8_t
	{
		exited,
		crashed,
		timed_out,
		cancelled,
		output_limit,
		launch_failed,
		unavailable,
	};

	enum class gcc_process_standard_stream_mode : std::uint8_t
	{
		captured,
		inherited,
	};

	struct CXXLENS_RUNTIME_DETAIL_HIDDEN gcc_probe_process_limits
	{
		std::size_t maximum_argument_count{};
		std::size_t maximum_argument_bytes{};
		std::size_t maximum_environment_count{};
		std::size_t maximum_environment_bytes{};
		std::size_t maximum_output_bytes{};
		std::uint64_t maximum_executable_image_bytes{};
		std::size_t maximum_canonical_path_bytes{};
	};

	struct CXXLENS_RUNTIME_DETAIL_HIDDEN gcc_probe_process_request
	{
		std::vector<std::string> argv;
		std::string working_directory;
		std::vector<std::string> environment;
		gcc_probe_process_limits limits;
		std::uint64_t absolute_wall_deadline_ns{};
		gcc_process_standard_stream_mode standard_stream_mode{
			gcc_process_standard_stream_mode::captured};
	};

	struct CXXLENS_RUNTIME_DETAIL_HIDDEN gcc_probe_process_output
	{
		gcc_probe_process_terminal terminal{gcc_probe_process_terminal::launch_failed};
		int exit_code{};
		int signal{};
		std::string standard_output;
		std::string standard_error;
		std::string executable_path;
		std::string executable_digest;
		std::uint64_t executable_bytes{};
		std::string failure_stage;
		std::string failure_detail;
	};

	class CXXLENS_RUNTIME_DETAIL_HIDDEN gcc_probe_process_port
	{
	  public:
		virtual ~gcc_probe_process_port() = default;
		[[nodiscard]] virtual result<gcc_probe_process_output>
		run(const gcc_probe_process_request& request, const std::stop_token& cancellation) = 0;
	};

	/** Run one exact GCC introspection command without shell or ambient environment lookup. */
	[[nodiscard]] CXXLENS_RUNTIME_DETAIL_HIDDEN result<gcc_probe_process_output>
	run_gcc_probe_process(const gcc_probe_process_request& request,
						  const std::stop_token& cancellation = {});

	/** Linux/glibc production adapter; other hosts return structured unavailable. */
	[[nodiscard]] CXXLENS_RUNTIME_DETAIL_HIDDEN std::unique_ptr<gcc_probe_process_port>
	make_system_gcc_probe_process_port();

	/** Snapshot the current process environment with explicit aggregate bounds. */
	[[nodiscard]] CXXLENS_RUNTIME_DETAIL_HIDDEN result<std::vector<std::string>>
	read_current_process_environment(std::size_t maximum_count, std::size_t maximum_bytes);
} // namespace cxxlens::sdk::detail

#undef CXXLENS_RUNTIME_DETAIL_HIDDEN
