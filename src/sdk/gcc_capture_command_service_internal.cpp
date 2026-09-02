#include "gcc_capture_command_service_internal.hpp"

#include <chrono>
#include <cstdint>
#include <limits>
#include <new>
#include <stdexcept>
#include <string_view>
#include <utility>

#include "gcc_auxiliary_capture_internal.hpp"
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

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) &&
				 (root == "/" || path[root.size()] == '/'));
		}

		[[nodiscard]] std::string_view
		terminal_name(const gcc_probe_process_terminal terminal) noexcept
		{
			switch (terminal)
			{
				case gcc_probe_process_terminal::crashed:
					return "crashed";
				case gcc_probe_process_terminal::timed_out:
					return "timed-out";
				case gcc_probe_process_terminal::cancelled:
					return "cancelled";
				case gcc_probe_process_terminal::output_limit:
					return "output-limit";
				case gcc_probe_process_terminal::launch_failed:
					return "launch-failed";
				case gcc_probe_process_terminal::unavailable:
					return "unavailable";
				case gcc_probe_process_terminal::exited:
					return "exited";
			}
			return "unknown";
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
			return {4096U,
					std::size_t{1024U} * 1024U,
					4096U,
					std::size_t{1024U} * 1024U,
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
			auto environment = read_current_process_environment(4096U, std::size_t{1024U} * 1024U);
			if (!environment)
				return unexpected(std::move(environment.error()));
			capture.execution_environment = std::move(*environment);
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

	result<gcc_wrapper_command_result>
	capture_gcc_wrapper_command(const gcc_wrapper_command_request& request)
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
			const auto limits = import_limits{};
			auto root =
				files->canonical_directory(request.project_root, limits.maximum_string_bytes);
			if (!root)
				return unexpected(std::move(root.error()));
			auto working = files->canonical_directory(".", limits.maximum_string_bytes);
			if (!working)
				return unexpected(std::move(working.error()));
			auto workspace =
				files->create_workspace(request.capture_directory, limits.maximum_string_bytes);
			if (!workspace)
				return unexpected(std::move(workspace.error()));
			const auto dependency_path = (*workspace)->dependency_output_path();
			if (!at_or_below(dependency_path, *root))
				return unexpected({"application-analysis.capture-input-invalid",
								   "capture_directory",
								   "path-outside-project-root"});
			auto environment = read_current_process_environment(4096U, std::size_t{1024U} * 1024U);
			if (!environment)
				return unexpected(std::move(environment.error()));
			auto prepared = prepare_gcc_16_2_response_files(*files,
															request.compiler_arguments,
															*working,
															*root,
															limits.maximum_source_closure_bytes,
															limits);
			if (!prepared)
				return unexpected(std::move(prepared.error()));
			prepared = prepare_gcc_16_2_spec_files(*files,
												   **workspace,
												   std::move(*prepared),
												   *working,
												   *root,
												   limits.maximum_source_closure_bytes,
												   limits);
			if (!prepared)
				return unexpected(std::move(prepared.error()));
			auto plan = plan_gcc_16_2_invocation({prepared->expanded_arguments,
												  request.compiler_path,
												  (*workspace)->dependency_output_path()},
												 limits);
			if (!plan)
				return unexpected(std::move(plan.error()));
			prepared->expanded_arguments = plan->capture_arguments;
			auto effects = capture_gcc_16_2_environment_effects(*files,
																{*environment,
																 *working,
																 *root,
																 plan->language,
																 4096U,
																 std::size_t{1024U} * 1024U,
																 limits.maximum_string_bytes},
																limits);
			if (!effects)
				return unexpected(std::move(effects.error()));

			gcc_probe_process_request process;
			process.argv = plan->capture_arguments;
			process.working_directory = *working;
			process.environment = *environment;
			process.limits = capture_process_limits();
			process.absolute_wall_deadline_ns = *deadline;
			process.standard_stream_mode = gcc_process_standard_stream_mode::inherited;
			auto executed = processes->run(process, {});
			if (!executed)
				return unexpected(std::move(executed.error()));
			if (executed->terminal != gcc_probe_process_terminal::exited)
				return unexpected(
					runtime_error("compiler-" + std::string{terminal_name(executed->terminal)}));
			if (executed->exit_code != 0)
				return gcc_wrapper_command_result{executed->exit_code, std::nullopt};

			gcc_invocation_capture_request capture;
			capture.project_id = request.project_id;
			capture.project_root = *root;
			capture.working_directory = *working;
			capture.source_path = plan->source_path;
			capture.compiler_path = request.compiler_path;
			capture.original_arguments = request.compiler_arguments;
			capture.capture_arguments = plan->capture_arguments;
			capture.environment_effects = std::move(*effects);
			capture.execution_environment = *environment;
			capture.process_limits = capture_process_limits();
			capture.absolute_wall_deadline_ns = *deadline;
			capture.expected_compiler_path = executed->executable_path;
			capture.expected_compiler_digest = executed->executable_digest;
			auto bundle = capture_gcc_invocation(
				*files, *processes, capture, limits, {}, std::move(*prepared));
			if (!bundle)
				return unexpected(std::move(bundle.error()));
			auto published = (*workspace)->publish_bundle(*bundle);
			if (!published)
				return unexpected(std::move(published.error()));
			return gcc_wrapper_command_result{0, std::move(*published)};
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
