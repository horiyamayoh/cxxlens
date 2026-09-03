#pragma once

/** @file gcc_toolchain_probe_internal.hpp
 * @brief Compiler-neutral coordination of exact GCC toolchain observations.
 */

#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "gcc_capture_bundle_internal.hpp"
#include "runtime/gcc_probe_process_port_internal.hpp"

#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_SDK_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_SDK_DETAIL_HIDDEN
#endif

namespace cxxlens::sdk::detail
{
	class gcc_capture_file_port;

	struct CXXLENS_SDK_DETAIL_HIDDEN gcc_toolchain_probe_request
	{
		std::string compiler_path;
		std::string working_directory;
		std::vector<std::string> execution_environment;
		gcc_probe_process_limits process_limits;
		std::uint64_t absolute_wall_deadline_ns{};
		std::size_t maximum_builtin_header_files{16384U};
		std::uint64_t maximum_builtin_header_bytes{std::uint64_t{64U} * 1024U * 1024U};
		std::size_t maximum_path_bytes{4096U};
	};

	/**
	 * Observe one exact GCC 16.2.0 x86_64-linux-gnu toolchain. Every subprocess is bound to the
	 * same measured executable identity and absolute deadline. Missing header content remains an
	 * actionable unavailable observation rather than an inferred digest.
	 */
	[[nodiscard]] CXXLENS_SDK_DETAIL_HIDDEN result<gcc_toolchain_observation>
	probe_gcc_toolchain(gcc_probe_process_port& processes,
						const gcc_toolchain_probe_request& request,
						const std::stop_token& cancellation = {},
						gcc_capture_file_port* files = nullptr);
} // namespace cxxlens::sdk::detail

#undef CXXLENS_SDK_DETAIL_HIDDEN
