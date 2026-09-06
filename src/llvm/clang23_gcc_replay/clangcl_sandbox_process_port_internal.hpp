#pragma once

/**
 * @file clangcl_sandbox_process_port_internal.hpp
 * @brief Source-private process isolation port for the Windows clang-cl replay child.
 */

#include <memory>
#include <span>
#include <vector>

#include <cxxlens/sdk/application_analysis.hpp>

#include "provider_worker.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	/** Execute a child that may emit Protocol v2 but has neither signing nor publication authority.
	 */
	class clangcl_sandbox_process_port
	{
	  public:
		virtual ~clangcl_sandbox_process_port() = default;
		[[nodiscard]] virtual sdk::result<std::vector<std::byte>>
		execute(std::span<const std::byte> host_transcript,
				const provider_worker_authority& authority,
				sdk::import_limits limits) const = 0;
	};

#ifdef _WIN32
	/** Create the AppContainer and Job Object backed production process port. */
	[[nodiscard]] std::unique_ptr<clangcl_sandbox_process_port>
	make_windows_clangcl_sandbox_process_port();
#endif
} // namespace cxxlens::detail::clang23_gcc_replay
