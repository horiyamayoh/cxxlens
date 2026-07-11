#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include "process_port.hpp"

namespace cxxlens::detail::runtime
{
	namespace
	{
		[[nodiscard]] runtime_result<process_result> invalid(const request_context& context)
		{
			return unexpected{
				runtime_failure{runtime_status::invalid_request, context.operation, 0}};
		}

		void close_pair(std::array<int, 2>& descriptors) noexcept
		{
			for (auto& descriptor : descriptors)
			{
				if (descriptor >= 0)
				{
					(void)::close(descriptor);
					descriptor = -1;
				}
			}
		}

		[[nodiscard]] bool drain(const int descriptor,
								 std::string& destination,
								 const std::size_t output_limit,
								 std::size_t& total)
		{
			std::array<char, 4096> buffer{};
			for (;;)
			{
				const auto count = ::read(descriptor, buffer.data(), buffer.size());
				if (count > 0)
				{
					total += static_cast<std::size_t>(count);
					if (total > output_limit)
					{
						return false;
					}
					destination.append(buffer.data(), static_cast<std::size_t>(count));
					continue;
				}
				if (count == 0 || errno == EAGAIN || errno == EWOULDBLOCK)
				{
					return true;
				}
				return false;
			}
		}
	} // namespace

	runtime_result<process_result> argv_process_adapter::run(const process_request& request,
															 const request_context& context) const
	{
		if (request.argv.empty() || request.argv.front().empty() || request.shell_allowed ||
			request.timeout < std::chrono::milliseconds::zero())
		{
			return invalid(context);
		}
		if (context.cancelled())
		{
			return unexpected{runtime_failure{runtime_status::cancelled, context.operation, 0}};
		}

		std::array<int, 2> stdout_pipe{-1, -1};
		std::array<int, 2> stderr_pipe{-1, -1};
		if (::pipe(stdout_pipe.data()) != 0 || ::pipe(stderr_pipe.data()) != 0)
		{
			const auto error = errno;
			close_pair(stdout_pipe);
			close_pair(stderr_pipe);
			return unexpected{
				runtime_failure{runtime_status::platform_failure, context.operation, error}};
		}

		const auto child = ::fork();
		if (child < 0)
		{
			const auto error = errno;
			close_pair(stdout_pipe);
			close_pair(stderr_pipe);
			return unexpected{
				runtime_failure{runtime_status::platform_failure, context.operation, error}};
		}
		if (child == 0)
		{
			(void)::dup2(stdout_pipe[1], STDOUT_FILENO);
			(void)::dup2(stderr_pipe[1], STDERR_FILENO);
			close_pair(stdout_pipe);
			close_pair(stderr_pipe);
			if (!request.working_directory.empty() &&
				::chdir(request.working_directory.c_str()) != 0)
			{
				::_exit(126);
			}
			for (const auto& [name, value] : request.environment)
			{
				if (::setenv(name.c_str(), value.c_str(), 1) != 0)
				{
					::_exit(126);
				}
			}
			std::vector<char*> arguments;
			arguments.reserve(request.argv.size() + 1U);
			for (const auto& argument : request.argv)
			{
				arguments.push_back(const_cast<char*>(argument.c_str()));
			}
			arguments.push_back(nullptr);
			::execvp(arguments.front(), arguments.data());
			::_exit(127);
		}

		(void)::close(stdout_pipe[1]);
		stdout_pipe[1] = -1;
		(void)::close(stderr_pipe[1]);
		stderr_pipe[1] = -1;
		(void)::fcntl(stdout_pipe[0], F_SETFL, O_NONBLOCK);
		(void)::fcntl(stderr_pipe[0], F_SETFL, O_NONBLOCK);

		process_result result;
		std::size_t total_output{};
		int wait_status{};
		const auto started = std::chrono::steady_clock::now();
		auto deadline = started + request.timeout;
		if (context.deadline && *context.deadline < deadline)
		{
			deadline = *context.deadline;
		}
		auto failure_status = runtime_status::platform_failure;
		bool failed = false;
		for (;;)
		{
			if (context.cancelled())
			{
				failure_status = runtime_status::cancelled;
				failed = true;
			}
			else if (std::chrono::steady_clock::now() >= deadline)
			{
				failure_status = runtime_status::timed_out;
				failed = true;
			}
			if (!drain(
					stdout_pipe[0], result.standard_output, context.output_limit, total_output) ||
				!drain(stderr_pipe[0], result.standard_error, context.output_limit, total_output))
			{
				failure_status = runtime_status::output_limit;
				failed = true;
			}
			if (failed)
			{
				(void)::kill(child, SIGKILL);
				(void)::waitpid(child, &wait_status, 0);
				close_pair(stdout_pipe);
				close_pair(stderr_pipe);
				return unexpected{runtime_failure{failure_status, context.operation, 0}};
			}
			const auto waited = ::waitpid(child, &wait_status, WNOHANG);
			if (waited == child)
			{
				break;
			}
			if (waited < 0)
			{
				close_pair(stdout_pipe);
				close_pair(stderr_pipe);
				return unexpected{
					runtime_failure{runtime_status::platform_failure, context.operation, errno}};
			}
			std::array<pollfd, 2> descriptors{
				{{stdout_pipe[0], POLLIN, 0}, {stderr_pipe[0], POLLIN, 0}}};
			(void)::poll(descriptors.data(), descriptors.size(), 10);
		}
		(void)drain(stdout_pipe[0], result.standard_output, context.output_limit, total_output);
		(void)drain(stderr_pipe[0], result.standard_error, context.output_limit, total_output);
		close_pair(stdout_pipe);
		close_pair(stderr_pipe);
		result.exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128;
		return result;
	}

	fake_process_adapter::fake_process_adapter(process_result result, fault_plan faults)
		: result_{std::move(result)}, faults_{std::move(faults)}
	{
	}

	runtime_result<process_result> fake_process_adapter::run(const process_request& request,
															 const request_context& context) const
	{
		if (request.argv.empty() || request.shell_allowed)
		{
			return invalid(context);
		}
		if (const auto* failure = faults_.match(context))
		{
			return unexpected{*failure};
		}
		if (context.cancelled())
		{
			return unexpected{runtime_failure{runtime_status::cancelled, context.operation, 0}};
		}
		if (context.deadline && std::chrono::steady_clock::now() >= *context.deadline)
		{
			return unexpected{runtime_failure{runtime_status::timed_out, context.operation, 0}};
		}
		if (result_.standard_output.size() + result_.standard_error.size() > context.output_limit)
		{
			return unexpected{runtime_failure{runtime_status::output_limit, context.operation, 0}};
		}
		return result_;
	}

} // namespace cxxlens::detail::runtime
