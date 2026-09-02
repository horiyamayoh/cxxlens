#include "gcc_capture_command_service_internal.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

#include "gcc_capture_file_port_internal.hpp"
#include "runtime/gcc_probe_process_port_internal.hpp"
#include "runtime/monotonic_clock_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::uint64_t capture_wall_budget_ns{30'000'000'000U};

		[[nodiscard]] error runtime_error(std::string detail)
		{
			return {
				"application-analysis.capture-runtime-unavailable", "capture", std::move(detail)};
		}

		[[nodiscard]] result<std::uint64_t> capture_deadline()
		{
			const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
								   runtime::monotonic_now().time_since_epoch())
								   .count();
			if (count < 0)
				return unexpected(runtime_error("monotonic-clock-negative"));
			const auto now = static_cast<std::uint64_t>(count);
			if (now > std::numeric_limits<std::uint64_t>::max() - capture_wall_budget_ns)
				return unexpected(runtime_error("monotonic-deadline-overflow"));
			return now + capture_wall_budget_ns;
		}

		[[nodiscard]] gcc_probe_process_limits capture_process_limits() noexcept
		{
			return {32U,
					std::size_t{16U} * 1024U,
					8U,
					std::size_t{16U} * 1024U,
					std::size_t{128U} * 1024U,
					std::uint64_t{128U} * 1024U * 1024U,
					4096U};
		}
	} // namespace

	result<std::vector<std::byte>> capture_gcc_command(const gcc_capture_command_request& request)
	{
		try
		{
			auto deadline = capture_deadline();
			if (!deadline)
				return unexpected(std::move(deadline.error()));
			auto files = make_system_gcc_capture_file_port();
			auto processes = make_system_gcc_probe_process_port();
			if (!files || !processes)
				return unexpected(runtime_error("port-construction"));

			gcc_compile_commands_capture_request capture;
			capture.project_id = request.project_id;
			capture.project_root = request.project_root;
			capture.compile_commands_path = request.compile_commands_path;
			capture.compiler_path = request.compiler_path;
			capture.process_limits = capture_process_limits();
			capture.absolute_wall_deadline_ns = *deadline;
			return capture_gcc_compile_commands(*files, *processes, capture);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(runtime_error("allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(runtime_error("allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
