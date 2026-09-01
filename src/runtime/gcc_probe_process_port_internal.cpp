#include "gcc_probe_process_port_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <compare>
#include <csignal>
#include <cstdlib>
#include <limits>
#include <new>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>

#include <spawn.h>

#include "sealed_executable_internal.hpp"

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error request_error(std::string field, std::string detail = {})
		{
			return {"application-analysis.gcc-probe-request-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] std::uint64_t monotonic_now_ns() noexcept
		{
			const auto now = std::chrono::duration_cast<std::chrono::nanoseconds>(
								 std::chrono::steady_clock::now().time_since_epoch())
								 .count();
			return now < 0 ? 0U : static_cast<std::uint64_t>(now);
		}

		[[nodiscard]] bool
		add_bounded(std::size_t& total, const std::size_t amount, const std::size_t limit) noexcept
		{
			if (amount > limit || total > limit - amount)
				return false;
			total += amount;
			return true;
		}

		[[nodiscard]] result<void> validate_request(const gcc_probe_process_request& request)
		{
			const auto& limits = request.limits;
			if (limits.maximum_argument_count == 0U || limits.maximum_argument_bytes == 0U ||
				limits.maximum_environment_count == 0U || limits.maximum_environment_bytes == 0U ||
				limits.maximum_output_bytes == 0U || limits.maximum_executable_image_bytes == 0U ||
				limits.maximum_canonical_path_bytes == 0U ||
				request.absolute_wall_deadline_ns == 0U)
				return unexpected(request_error("limits", "zero"));
			if (request.argv.empty() || request.argv.size() > limits.maximum_argument_count ||
				request.argv.front().empty() || request.argv.front().front() != '/' ||
				request.working_directory.empty() || request.working_directory.front() != '/' ||
				request.working_directory.contains('\0'))
				return unexpected(request_error("invocation", "absolute-path-required"));
			std::size_t argument_bytes{};
			for (const auto& argument : request.argv)
				if (argument.contains('\0') ||
					argument.size() == std::numeric_limits<std::size_t>::max() ||
					!add_bounded(
						argument_bytes, argument.size() + 1U, limits.maximum_argument_bytes))
					return unexpected(request_error("argv", "bounded-token-vector"));
			if (request.environment.size() > limits.maximum_environment_count)
				return unexpected(request_error("environment", "count"));
			std::size_t environment_bytes{};
			for (std::size_t index{}; index < request.environment.size(); ++index)
			{
				const auto& entry = request.environment[index];
				const auto separator = entry.find('=');
				if (entry.contains('\0') || separator == 0U || separator == std::string::npos ||
					entry.size() == std::numeric_limits<std::size_t>::max() ||
					!add_bounded(
						environment_bytes, entry.size() + 1U, limits.maximum_environment_bytes))
					return unexpected(request_error("environment", "bounded-name-value"));
				const std::string_view name{entry.data(), separator};
				for (std::size_t previous{}; previous < index; ++previous)
				{
					const auto& candidate = request.environment[previous];
					const auto candidate_separator = candidate.find('=');
					if (std::string_view{candidate.data(), candidate_separator} == name)
						return unexpected(request_error("environment", "duplicate-name"));
				}
			}
			return {};
		}

#if defined(__linux__) && defined(__GLIBC__)
		class descriptor final
		{
		  public:
			explicit descriptor(const int value = -1) noexcept : value_{value} {}
			~descriptor()
			{
				if (value_ >= 0)
					(void)::close(value_);
			}
			descriptor(const descriptor&) = delete;
			descriptor& operator=(const descriptor&) = delete;
			descriptor(descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
			descriptor& operator=(descriptor&& other) noexcept
			{
				if (this != &other)
				{
					if (value_ >= 0)
						(void)::close(value_);
					value_ = std::exchange(other.value_, -1);
				}
				return *this;
			}
			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			void reset() noexcept
			{
				if (value_ >= 0)
					(void)::close(value_);
				value_ = -1;
			}

		  private:
			int value_;
		};

		struct pipe_pair
		{
			descriptor read;
			descriptor write;
		};

		[[nodiscard]] result<pipe_pair> make_pipe(const std::string_view stream)
		{
			std::array<int, 2U> values{};
			if (::pipe2(values.data(), O_CLOEXEC) != 0)
				return unexpected(
					request_error(std::string{stream}, "pipe:" + std::to_string(errno)));
			const auto flags = ::fcntl(values[0], F_GETFL);
			if (flags < 0 || ::fcntl(values[0], F_SETFL, flags | O_NONBLOCK) != 0)
			{
				const auto failure = errno;
				(void)::close(values[0]);
				(void)::close(values[1]);
				return unexpected(request_error(std::string{stream},
												"pipe-nonblocking:" + std::to_string(failure)));
			}
			return pipe_pair{descriptor{values[0]}, descriptor{values[1]}};
		}

		class file_actions final
		{
		  public:
			file_actions() noexcept : initialized_{::posix_spawn_file_actions_init(&value_) == 0} {}
			~file_actions()
			{
				if (initialized_)
					(void)::posix_spawn_file_actions_destroy(&value_);
			}
			[[nodiscard]] bool initialized() const noexcept
			{
				return initialized_;
			}
			[[nodiscard]] posix_spawn_file_actions_t* get() noexcept
			{
				return &value_;
			}

		  private:
			posix_spawn_file_actions_t value_{};
			bool initialized_{};
		};

		class spawn_attributes final
		{
		  public:
			spawn_attributes() noexcept : initialized_{::posix_spawnattr_init(&value_) == 0} {}
			~spawn_attributes()
			{
				if (initialized_)
					(void)::posix_spawnattr_destroy(&value_);
			}
			[[nodiscard]] bool initialized() const noexcept
			{
				return initialized_;
			}
			[[nodiscard]] posix_spawnattr_t* get() noexcept
			{
				return &value_;
			}

		  private:
			posix_spawnattr_t value_{};
			bool initialized_{};
		};

		[[nodiscard]] std::vector<char*> pointers(std::vector<std::string>& storage)
		{
			std::vector<char*> output;
			output.reserve(storage.size() + 1U);
			for (auto& value : storage)
				output.push_back(value.data());
			output.push_back(nullptr);
			return output;
		}

		struct child_process_state
		{
			pid_t child{};
			int wait_status{};
		};

		void terminate_group(child_process_state& state) noexcept
		{
			(void)::kill(-state.child, SIGKILL);
			const auto handoff_deadline =
				std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
			while (std::chrono::steady_clock::now() < handoff_deadline)
			{
				(void)::kill(-state.child, SIGKILL);
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			}
			while (::waitpid(state.child, &state.wait_status, 0) < 0 && errno == EINTR)
			{
			}
		}

		class child_group_guard final
		{
		  public:
			explicit child_group_guard(child_process_state& state) noexcept : state_{state} {}
			~child_group_guard()
			{
				terminate();
			}
			child_group_guard(const child_group_guard&) = delete;
			child_group_guard& operator=(const child_group_guard&) = delete;
			void terminate() noexcept
			{
				if (active_)
				{
					terminate_group(state_);
					active_ = false;
				}
			}

		  private:
			child_process_state& state_;
			bool active_{true};
		};

		enum class drain_terminal : std::uint8_t
		{
			open,
			ended,
			limit,
			failed,
		};

		struct drain_result
		{
			drain_terminal terminal{drain_terminal::open};
			int failure{};
		};

		[[nodiscard]] drain_result drain_descriptor(const int source,
													std::string& destination,
													std::size_t& total,
													const std::size_t limit)
		{
			std::array<char, 4096U> buffer{};
			for (;;)
			{
				const auto count = ::read(source, buffer.data(), buffer.size());
				if (count > 0)
				{
					const auto received = static_cast<std::size_t>(count);
					if (!add_bounded(total, received, limit))
					{
						return {drain_terminal::limit, 0};
					}
					destination.append(buffer.data(), received);
					continue;
				}
				if (count == 0)
				{
					return {drain_terminal::ended, 0};
				}
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return {drain_terminal::open, 0};
				return {drain_terminal::failed, errno};
			}
		}
#endif
	} // namespace

	result<gcc_probe_process_output> run_gcc_probe_process(const gcc_probe_process_request& request,
														   const std::stop_token& cancellation)
	{
		if (auto valid = validate_request(request); !valid)
			return unexpected(std::move(valid.error()));
#if defined(__linux__) && defined(__GLIBC__)
		try
		{
			gcc_probe_process_output output;
			if (cancellation.stop_requested())
			{
				output.terminal = gcc_probe_process_terminal::cancelled;
				return output;
			}
			if (monotonic_now_ns() >= request.absolute_wall_deadline_ns)
			{
				output.terminal = gcc_probe_process_terminal::timed_out;
				return output;
			}
			auto executable = open_sealed_executable({request.argv.front(),
													  request.working_directory,
													  request.absolute_wall_deadline_ns,
													  request.limits.maximum_executable_image_bytes,
													  request.limits.maximum_canonical_path_bytes,
													  cancellation});
			if (!executable)
			{
				if (executable.error().code == "runtime.sealed-executable-timeout")
					output.terminal = gcc_probe_process_terminal::timed_out;
				else if (executable.error().code == "runtime.sealed-executable-cancelled")
					output.terminal = gcc_probe_process_terminal::cancelled;
				else
					output.terminal = executable.error().detail == "unsupported"
						? gcc_probe_process_terminal::unavailable
						: gcc_probe_process_terminal::launch_failed;
				output.failure_stage = executable.error().field;
				output.failure_detail = executable.error().detail;
				return output;
			}
			output.executable_path = executable->canonical_source_path();
			output.executable_digest = executable->digest();
			output.executable_bytes = executable->byte_count();

			auto standard_output = make_pipe("stdout");
			if (!standard_output)
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = standard_output.error().field;
				output.failure_detail = standard_output.error().detail;
				return output;
			}
			auto standard_error = make_pipe("stderr");
			if (!standard_error)
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = standard_error.error().field;
				output.failure_detail = standard_error.error().detail;
				return output;
			}
			descriptor null_input{::open("/dev/null", O_RDONLY | O_CLOEXEC)};
			if (null_input.get() < 0)
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = "stdin";
				output.failure_detail = "open:" + std::to_string(errno);
				return output;
			}
			file_actions actions;
			spawn_attributes attributes;
			if (!actions.initialized() || !attributes.initialized())
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = "spawn-initialize";
				return output;
			}
			auto action_result =
				::posix_spawn_file_actions_adddup2(actions.get(), executable->native_handle(), 3);
			if (action_result == 0)
				action_result = ::posix_spawn_file_actions_adddup2(
					actions.get(), null_input.get(), STDIN_FILENO);
			if (action_result == 0)
				action_result = ::posix_spawn_file_actions_adddup2(
					actions.get(), standard_output->write.get(), STDOUT_FILENO);
			if (action_result == 0)
				action_result = ::posix_spawn_file_actions_adddup2(
					actions.get(), standard_error->write.get(), STDERR_FILENO);
			if (action_result == 0)
				action_result = ::posix_spawn_file_actions_addchdir_np(
					actions.get(), request.working_directory.c_str());
			if (action_result == 0)
				action_result = ::posix_spawn_file_actions_addclosefrom_np(actions.get(), 4);
			if (action_result == 0)
				action_result = ::posix_spawnattr_setflags(attributes.get(), POSIX_SPAWN_SETPGROUP);
			if (action_result == 0)
				action_result = ::posix_spawnattr_setpgroup(attributes.get(), 0);
			if (action_result != 0)
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = "spawn-configure";
				output.failure_detail = std::to_string(action_result);
				return output;
			}
			auto arguments_storage = request.argv;
			auto environment_storage = request.environment;
			auto arguments = pointers(arguments_storage);
			auto environment = pointers(environment_storage);
			pid_t child{};
			const auto spawn_result = ::posix_spawn(&child,
													"/proc/self/fd/3",
													actions.get(),
													attributes.get(),
													arguments.data(),
													environment.data());
			if (spawn_result != 0)
			{
				output.terminal = gcc_probe_process_terminal::launch_failed;
				output.failure_stage = "spawn";
				output.failure_detail = std::to_string(spawn_result);
				return output;
			}
			standard_output->write.reset();
			standard_error->write.reset();
			bool stdout_ended{};
			bool stderr_ended{};
			bool leader_exited{};
			child_process_state child_state{child, 0};
			child_group_guard child_guard{child_state};
			std::size_t total{};
			for (;;)
			{
				if (cancellation.stop_requested())
				{
					output.terminal = gcc_probe_process_terminal::cancelled;
					child_guard.terminate();
					return output;
				}
				if (monotonic_now_ns() >= request.absolute_wall_deadline_ns)
				{
					output.terminal = gcc_probe_process_terminal::timed_out;
					child_guard.terminate();
					return output;
				}
				std::array<pollfd, 2U> descriptors{{{standard_output->read.get(), POLLIN, 0},
													{standard_error->read.get(), POLLIN, 0}}};
				(void)::poll(descriptors.data(), descriptors.size(), 10);
				const auto stdout_drain = drain_descriptor(standard_output->read.get(),
														   output.standard_output,
														   total,
														   request.limits.maximum_output_bytes);
				const auto stderr_drain = drain_descriptor(standard_error->read.get(),
														   output.standard_error,
														   total,
														   request.limits.maximum_output_bytes);
				stdout_ended = stdout_ended || stdout_drain.terminal == drain_terminal::ended;
				stderr_ended = stderr_ended || stderr_drain.terminal == drain_terminal::ended;
				if (stdout_drain.terminal == drain_terminal::failed ||
					stderr_drain.terminal == drain_terminal::failed)
				{
					output.terminal = gcc_probe_process_terminal::launch_failed;
					output.failure_stage = "output-read";
					output.failure_detail = stdout_drain.terminal == drain_terminal::failed
						? "stdout:" + std::to_string(stdout_drain.failure)
						: "stderr:" + std::to_string(stderr_drain.failure);
					child_guard.terminate();
					return output;
				}
				if (stdout_drain.terminal == drain_terminal::limit ||
					stderr_drain.terminal == drain_terminal::limit)
				{
					output.terminal = gcc_probe_process_terminal::output_limit;
					child_guard.terminate();
					return output;
				}
				if (!leader_exited)
				{
					siginfo_t information{};
					const auto observed = ::waitid(
						P_PID, static_cast<id_t>(child), &information, WEXITED | WNOHANG | WNOWAIT);
					leader_exited = observed == 0 && information.si_pid == child;
				}
				if (leader_exited && stdout_ended && stderr_ended)
				{
					// Keep the leader waitable until every inherited output endpoint is closed,
					// then kill the group before reaping so a quiet detached descendant cannot
					// escape the bounded probe lifecycle.
					child_guard.terminate();
					break;
				}
			}
			if (WIFEXITED(child_state.wait_status))
			{
				output.terminal = gcc_probe_process_terminal::exited;
				output.exit_code = WEXITSTATUS(child_state.wait_status);
			}
			else
			{
				output.terminal = gcc_probe_process_terminal::crashed;
				output.signal =
					WIFSIGNALED(child_state.wait_status) ? WTERMSIG(child_state.wait_status) : 0;
			}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(request_error("allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(request_error("allocation"));
		}
#else
		(void)cancellation;
		gcc_probe_process_output output;
		output.terminal = gcc_probe_process_terminal::unavailable;
		output.failure_stage = "platform";
		output.failure_detail = "unsupported";
		return output;
#endif
	}
} // namespace cxxlens::sdk::detail
