#pragma once

/**
 * @file gcc_capture_file_port_internal.hpp
 * @brief Source-private filesystem authority for bounded GCC build capture.
 */

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "gcc_capture_bundle_internal.hpp"

namespace cxxlens::sdk::detail
{
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

	struct gcc_compile_commands_capture_request
	{
		std::string project_id;
		std::string project_root;
		std::string compile_commands_path;
		gcc_toolchain_observation toolchain;
	};

	/** Read one compilation database and its main sources through a single bounded file port. */
	[[nodiscard]] result<std::vector<std::byte>>
	capture_gcc_compile_commands(gcc_capture_file_port& files,
								 const gcc_compile_commands_capture_request& request,
								 import_limits limits = {});

	/** Linux production adapter. Unsupported hosts return structured-unavailable failures. */
	[[nodiscard]] std::unique_ptr<gcc_capture_file_port> make_system_gcc_capture_file_port();
} // namespace cxxlens::sdk::detail
