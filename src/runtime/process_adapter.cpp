#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <map>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <poll.h>
#include <spawn.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/mman.h>
#endif

#include "process_port.hpp"

namespace cxxlens::detail::runtime
{
	namespace
	{
		struct pipe_pair
		{
			std::array<int, 2> descriptors{-1, -1};

			pipe_pair() = default;
			pipe_pair(const pipe_pair&) = delete;
			pipe_pair& operator=(const pipe_pair&) = delete;
			~pipe_pair()
			{
				close_all();
			}

			void close(const std::size_t index) noexcept
			{
				auto& descriptor = descriptors.at(index);
				if (descriptor >= 0)
				{
					(void)::close(descriptor);
					descriptor = -1;
				}
			}

			void close_all() noexcept
			{
				close(0U);
				close(1U);
			}

			[[nodiscard]] int read_end() const noexcept
			{
				return descriptors[0];
			}

			[[nodiscard]] int write_end() const noexcept
			{
				return descriptors[1];
			}
		};

		class owned_descriptor
		{
		  public:
			explicit owned_descriptor(const int value = -1) noexcept : value_{value} {}
			owned_descriptor(const owned_descriptor&) = delete;
			owned_descriptor& operator=(const owned_descriptor&) = delete;
			~owned_descriptor()
			{
				if (value_ >= 0)
					(void)::close(value_);
			}

			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}

		  private:
			int value_;
		};

		class spawn_file_actions
		{
		  public:
			spawn_file_actions() : error_{::posix_spawn_file_actions_init(&value_)} {}
			spawn_file_actions(const spawn_file_actions&) = delete;
			spawn_file_actions& operator=(const spawn_file_actions&) = delete;
			~spawn_file_actions()
			{
				if (error_ == 0)
					(void)::posix_spawn_file_actions_destroy(&value_);
			}

			[[nodiscard]] int error() const noexcept
			{
				return error_;
			}

			[[nodiscard]] posix_spawn_file_actions_t* get() noexcept
			{
				return &value_;
			}

		  private:
			posix_spawn_file_actions_t value_{};
			int error_{};
		};

		class spawn_attributes
		{
		  public:
			spawn_attributes() : error_{::posix_spawnattr_init(&value_)} {}
			spawn_attributes(const spawn_attributes&) = delete;
			spawn_attributes& operator=(const spawn_attributes&) = delete;
			~spawn_attributes()
			{
				if (error_ == 0)
					(void)::posix_spawnattr_destroy(&value_);
			}

			[[nodiscard]] int error() const noexcept
			{
				return error_;
			}

			[[nodiscard]] posix_spawnattr_t* get() noexcept
			{
				return &value_;
			}

		  private:
			posix_spawnattr_t value_{};
			int error_{};
		};

		enum class drain_state : std::uint8_t
		{
			ready,
			end_of_file,
			output_limit,
			read_error,
		};

		struct drain_result
		{
			drain_state state{drain_state::ready};
			int platform_code{};
		};

		[[nodiscard]] runtime_result<process_result> invalid(const request_context& context)
		{
			return unexpected{
				runtime_failure{runtime_status::invalid_request, context.operation, 0}};
		}

		[[nodiscard]] runtime_result<process_result>
		platform_failure(const request_context& context, const int error)
		{
			return unexpected{
				runtime_failure{runtime_status::platform_failure, context.operation, error}};
		}

		[[nodiscard]] bool contains_nul(const std::string_view value) noexcept
		{
			return value.find('\0') != std::string_view::npos;
		}

		[[nodiscard]] bool valid_request(const process_request& request)
		{
			constexpr std::size_t maximum_input_bytes = std::size_t{64U} * 1024U * 1024U;
			if (request.argv.empty() || request.argv.front().empty() || request.shell_allowed ||
				request.timeout < std::chrono::milliseconds::zero() ||
				request.standard_input.size() > maximum_input_bytes)
				return false;
			if (std::ranges::any_of(request.argv, contains_nul))
				return false;
			if (contains_nul(request.working_directory.generic_string()))
				return false;
			return std::ranges::none_of(request.environment,
										[](const auto& variable)
										{
											return variable.first.empty() ||
												variable.first.contains('=') ||
												contains_nul(variable.first) ||
												contains_nul(variable.second);
										});
		}

		[[nodiscard]] int make_standard_input(const std::string_view input)
		{
#if defined(__linux__)
			int descriptor = ::memfd_create("cxxlens-process-input", MFD_CLOEXEC);
			if (descriptor < 0)
				return -errno;
			if (descriptor <= STDERR_FILENO)
			{
				const auto moved = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
				if (moved < 0)
				{
					const auto failure = errno;
					(void)::close(descriptor);
					return -failure;
				}
				(void)::close(descriptor);
				descriptor = moved;
			}
			std::size_t offset{};
			while (offset < input.size())
			{
				const auto written =
					::write(descriptor, input.data() + offset, input.size() - offset);
				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR)
					continue;
				const auto failure = written < 0 ? errno : EIO;
				(void)::close(descriptor);
				return -failure;
			}
			if (::lseek(descriptor, 0, SEEK_SET) < 0)
			{
				const auto failure = errno;
				(void)::close(descriptor);
				return -failure;
			}
			return descriptor;
#else
			(void)input;
			return -ENOTSUP;
#endif
		}

		[[nodiscard]] int make_pipe(pipe_pair& output)
		{
#if defined(__linux__)
			if (::pipe2(output.descriptors.data(), O_CLOEXEC) != 0)
				return errno;
			for (auto& descriptor : output.descriptors)
			{
				if (descriptor > STDERR_FILENO)
					continue;
				const auto moved = ::fcntl(descriptor, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
				if (moved < 0)
					return errno;
				(void)::close(descriptor);
				descriptor = moved;
			}
			return 0;
#else
			(void)output;
			return ENOTSUP;
#endif
		}

		struct nonblocking_request
		{
			int descriptor{};
			int injected_error{};
		};

		[[nodiscard]] int set_nonblocking(const nonblocking_request request)
		{
			if (request.injected_error != 0)
				return request.injected_error;
			const auto flags = ::fcntl(request.descriptor, F_GETFL);
			if (flags < 0)
				return errno;
			if (::fcntl(request.descriptor, F_SETFL, flags | O_NONBLOCK) != 0)
				return errno;
			return 0;
		}

		[[nodiscard]] int add_pipe_actions(spawn_file_actions& actions,
										   const int standard_input,
										   const pipe_pair& standard_output,
										   const pipe_pair& standard_error)
		{
			for (const auto [source, destination] :
				 {std::pair{standard_input, STDIN_FILENO},
				  std::pair{standard_output.write_end(), STDOUT_FILENO},
				  std::pair{standard_error.write_end(), STDERR_FILENO}})
				if (const auto error =
						::posix_spawn_file_actions_adddup2(actions.get(), source, destination);
					error != 0)
					return error;
			for (const auto descriptor : {standard_input,
										  standard_output.read_end(),
										  standard_output.write_end(),
										  standard_error.read_end(),
										  standard_error.write_end()})
				if (const auto error =
						::posix_spawn_file_actions_addclose(actions.get(), descriptor);
					error != 0)
					return error;
			return 0;
		}

		[[nodiscard]] int add_working_directory(spawn_file_actions& actions,
												const process_request& request)
		{
			if (request.working_directory.empty())
				return 0;
#if defined(__GLIBC__)
			return ::posix_spawn_file_actions_addchdir_np(actions.get(),
														  request.working_directory.c_str());
#else
			(void)actions;
			(void)request;
			return ENOTSUP;
#endif
		}

		[[nodiscard]] int configure_process_group(spawn_attributes& attributes)
		{
			if (const auto error = ::posix_spawnattr_setpgroup(attributes.get(), 0); error != 0)
				return error;
			return ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETPGROUP);
		}

		struct spawn_input
		{
			std::vector<char*> arguments;
			std::vector<std::string> environment_storage;
			std::vector<char*> environment;
		};

		[[nodiscard]] spawn_input make_spawn_input(const process_request& request)
		{
			spawn_input output;
			output.arguments.reserve(request.argv.size() + 1U);
			for (const auto& argument : request.argv)
				output.arguments.push_back(const_cast<char*>(argument.c_str()));
			output.arguments.push_back(nullptr);

			std::map<std::string, std::string> variables;
			for (auto current = environ; current != nullptr && *current != nullptr; ++current)
			{
				const std::string_view entry{*current};
				const auto separator = entry.find('=');
				if (separator != std::string_view::npos)
					variables.insert_or_assign(std::string{entry.substr(0U, separator)},
											   std::string{entry.substr(separator + 1U)});
			}
			for (const auto& [name, value] : request.environment)
				variables.insert_or_assign(name, value);
			output.environment_storage.reserve(variables.size());
			for (const auto& [name, value] : variables)
			{
				auto variable = name;
				variable.push_back('=');
				variable.append(value);
				output.environment_storage.push_back(std::move(variable));
			}
			output.environment.reserve(output.environment_storage.size() + 1U);
			for (auto& variable : output.environment_storage)
				output.environment.push_back(variable.data());
			output.environment.push_back(nullptr);
			return output;
		}

		[[nodiscard]] drain_result drain(const int descriptor,
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
					const auto received = static_cast<std::size_t>(count);
					if (received > output_limit - total)
						return {drain_state::output_limit, 0};
					total += received;
					destination.append(buffer.data(), received);
					continue;
				}
				if (count == 0)
					return {drain_state::end_of_file, 0};
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return {drain_state::ready, 0};
				return {drain_state::read_error, errno};
			}
		}

		[[nodiscard]] std::optional<runtime_failure> drain_failure(const drain_result& output,
																   const request_context& context,
																   const drain_result& errors)
		{
			if (output.state == drain_state::output_limit ||
				errors.state == drain_state::output_limit)
				return runtime_failure{runtime_status::output_limit, context.operation, 0};
			if (output.state == drain_state::read_error)
				return runtime_failure{
					runtime_status::platform_failure, context.operation, output.platform_code};
			if (errors.state == drain_state::read_error)
				return runtime_failure{
					runtime_status::platform_failure, context.operation, errors.platform_code};
			return std::nullopt;
		}

		[[nodiscard]] int wait_for_child(const pid_t child, int& wait_status, const int options)
		{
			for (;;)
			{
				const auto waited = ::waitpid(child, &wait_status, options);
				if (waited >= 0)
					return static_cast<int>(waited);
				if (errno != EINTR)
					return -errno;
			}
		}

		[[nodiscard]] int
		terminate_group_and_reap(const pid_t child, int& wait_status, const bool already_reaped)
		{
			int first_error{};
			if (::kill(-child, SIGKILL) != 0 && errno != ESRCH)
				first_error = errno;
			if (!already_reaped)
			{
				const auto waited = wait_for_child(child, wait_status, 0);
				if (waited != child && first_error == 0)
					first_error = waited < 0 ? -waited : ECHILD;
			}
			return first_error;
		}

		[[nodiscard]] runtime_result<process_result>
		fail_after_spawn(const pid_t child,
						 int& wait_status,
						 const bool already_reaped,
						 const runtime_failure& failure,
						 pipe_pair& standard_output,
						 pipe_pair& standard_error)
		{
			const auto cleanup_error = terminate_group_and_reap(child, wait_status, already_reaped);
			standard_output.close_all();
			standard_error.close_all();
			if (cleanup_error != 0)
				return unexpected{runtime_failure{
					runtime_status::platform_failure, failure.operation, cleanup_error}};
			return unexpected{failure};
		}
	} // namespace

	argv_process_adapter::argv_process_adapter(process_adapter_test_seam seam) : seam_{seam} {}

	runtime_result<process_result> argv_process_adapter::run(const process_request& request,
															 const request_context& context) const
	{
		if (!valid_request(request))
			return invalid(context);
		if (context.cancelled())
			return unexpected{runtime_failure{runtime_status::cancelled, context.operation, 0}};
		const auto standard_input_descriptor = make_standard_input(request.standard_input);
		if (standard_input_descriptor < 0)
			return platform_failure(context, -standard_input_descriptor);
		owned_descriptor standard_input{standard_input_descriptor};

		pipe_pair standard_output_pipe;
		pipe_pair standard_error_pipe;
		if (const auto error = make_pipe(standard_output_pipe); error != 0)
			return platform_failure(context, error);
		if (const auto error = make_pipe(standard_error_pipe); error != 0)
			return platform_failure(context, error);
		if (const auto error =
				set_nonblocking({standard_output_pipe.read_end(), seam_.nonblocking_setup_error});
			error != 0)
			return platform_failure(context, error);
		if (const auto error =
				set_nonblocking({standard_error_pipe.read_end(), seam_.nonblocking_setup_error});
			error != 0)
			return platform_failure(context, error);

		spawn_file_actions actions;
		if (actions.error() != 0)
			return platform_failure(context, actions.error());
		if (const auto error = add_pipe_actions(
				actions, standard_input.get(), standard_output_pipe, standard_error_pipe);
			error != 0)
			return platform_failure(context, error);
		if (const auto error = add_working_directory(actions, request); error != 0)
			return platform_failure(context, error);
		spawn_attributes attributes;
		if (attributes.error() != 0)
			return platform_failure(context, attributes.error());
		if (const auto error = configure_process_group(attributes); error != 0)
			return platform_failure(context, error);
		auto input = make_spawn_input(request);
		pid_t child{};
		const auto spawn_error = ::posix_spawnp(&child,
												request.argv.front().c_str(),
												actions.get(),
												attributes.get(),
												input.arguments.data(),
												input.environment.data());
		if (spawn_error != 0)
			return platform_failure(context, spawn_error);

		standard_output_pipe.close(1U);
		standard_error_pipe.close(1U);
		process_result result;
		std::size_t total_output{};
		int wait_status{};
		const auto started = std::chrono::steady_clock::now();
		auto deadline = started + request.timeout;
		if (context.deadline && *context.deadline < deadline)
			deadline = *context.deadline;

		for (;;)
		{
			std::optional<runtime_failure> failure;
			if (context.cancelled())
				failure = runtime_failure{runtime_status::cancelled, context.operation, 0};
			else if (std::chrono::steady_clock::now() >= deadline)
				failure = runtime_failure{runtime_status::timed_out, context.operation, 0};
			if (!failure && !seam_.drain_only_after_exit)
			{
				const auto output = drain(standard_output_pipe.read_end(),
										  result.standard_output,
										  context.output_limit,
										  total_output);
				const auto errors = drain(standard_error_pipe.read_end(),
										  result.standard_error,
										  context.output_limit,
										  total_output);
				failure = drain_failure(output, context, errors);
			}
			if (failure)
				return fail_after_spawn(
					child, wait_status, false, *failure, standard_output_pipe, standard_error_pipe);

			const auto waited = wait_for_child(child, wait_status, WNOHANG);
			if (waited == child)
				break;
			if (waited < 0)
				return fail_after_spawn(
					child,
					wait_status,
					false,
					runtime_failure{runtime_status::platform_failure, context.operation, -waited},
					standard_output_pipe,
					standard_error_pipe);
			std::array<pollfd, 2> descriptors{{{standard_output_pipe.read_end(), POLLIN, 0},
											   {standard_error_pipe.read_end(), POLLIN, 0}}};
			const auto polled = ::poll(descriptors.data(), descriptors.size(), 10);
			if (polled < 0 && errno != EINTR)
				return fail_after_spawn(
					child,
					wait_status,
					false,
					runtime_failure{runtime_status::platform_failure, context.operation, errno},
					standard_output_pipe,
					standard_error_pipe);
			if (std::ranges::any_of(descriptors,
									[](const pollfd& descriptor)
									{
										return (descriptor.revents & POLLNVAL) != 0;
									}))
				return fail_after_spawn(
					child,
					wait_status,
					false,
					runtime_failure{runtime_status::platform_failure, context.operation, EBADF},
					standard_output_pipe,
					standard_error_pipe);
		}

		drain_result output;
		drain_result errors;
		if (seam_.final_drain_error != 0)
			output = {drain_state::read_error, seam_.final_drain_error};
		else
		{
			output = drain(standard_output_pipe.read_end(),
						   result.standard_output,
						   context.output_limit,
						   total_output);
			errors = drain(standard_error_pipe.read_end(),
						   result.standard_error,
						   context.output_limit,
						   total_output);
		}
		if (const auto failure = drain_failure(output, context, errors))
			return fail_after_spawn(
				child, wait_status, true, *failure, standard_output_pipe, standard_error_pipe);
		standard_output_pipe.close_all();
		standard_error_pipe.close_all();
		result.exit_code = WIFEXITED(wait_status) ? WEXITSTATUS(wait_status) : 128;
		result.termination_signal = WIFSIGNALED(wait_status) ? WTERMSIG(wait_status) : 0;
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
			return invalid(context);
		if (const auto* failure = faults_.match(context))
			return unexpected{*failure};
		if (context.cancelled())
			return unexpected{runtime_failure{runtime_status::cancelled, context.operation, 0}};
		if (context.deadline && std::chrono::steady_clock::now() >= *context.deadline)
			return unexpected{runtime_failure{runtime_status::timed_out, context.operation, 0}};
		if (result_.standard_output.size() + result_.standard_error.size() > context.output_limit)
			return unexpected{runtime_failure{runtime_status::output_limit, context.operation, 0}};
		return result_;
	}

} // namespace cxxlens::detail::runtime
