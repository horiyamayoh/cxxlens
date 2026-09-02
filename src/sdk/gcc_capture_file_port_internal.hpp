#pragma once

/**
 * @file gcc_capture_file_port_internal.hpp
 * @brief Source-private filesystem authority for bounded GCC build capture.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

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

	class gcc_capture_file_port
	{
	  public:
		virtual ~gcc_capture_file_port() = default;
		[[nodiscard]] virtual result<std::string>
		canonical_directory(std::string_view path, std::size_t maximum_path_bytes) = 0;
		[[nodiscard]] virtual result<capture_file_snapshot>
		read_regular_file(std::string_view path, capture_file_read_limits limits) = 0;
	};

	struct CXXLENS_SDK_DETAIL_HIDDEN gcc_compile_commands_capture_request
	{
		std::string project_id;
		std::string project_root;
		std::string compile_commands_path;
		std::string compiler_path;
		gcc_probe_process_limits process_limits;
		std::uint64_t absolute_wall_deadline_ns{};
	};

	/** Probe one exact GCC toolchain, then read its compilation database and main sources. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_compile_commands(gcc_capture_file_port& files,
								 gcc_probe_process_port& processes,
								 const gcc_compile_commands_capture_request& request,
								 import_limits limits = {},
								 const std::stop_token& cancellation = {});

	/** Linux production adapter. Unsupported hosts return structured-unavailable failures. */
	[[nodiscard]] std::unique_ptr<gcc_capture_file_port> make_system_gcc_capture_file_port();
} // namespace cxxlens::sdk::detail

#undef CXXLENS_SDK_DETAIL_HIDDEN
