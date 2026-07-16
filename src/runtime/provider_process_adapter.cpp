#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <memory>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::sdk::provider
{
	namespace
	{
		[[nodiscard]] error
		process_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool contains_nul(const std::string_view value) noexcept
		{
			return value.contains('\0');
		}

		[[nodiscard]] result<std::string> executable_digest(const std::string& path)
		{
			std::ifstream input{path, std::ios::binary};
			if (!input)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-launch-failed", "executable-open", path));
			const std::string bytes{std::istreambuf_iterator<char>{input},
									std::istreambuf_iterator<char>{}};
			if (input.bad())
				return cxxlens::sdk::unexpected(
					process_error("provider.process-launch-failed", "executable-read", path));
			return cxxlens::sdk::content_digest(std::as_bytes(std::span{bytes}));
		}

		[[nodiscard]] std::vector<std::string> mechanisms()
		{
			return {
				"address-space-limit",
				"anonymous-readonly-input",
				"cpu-limit",
				"explicit-environment",
				"network-syscall-deny",
				"no-new-privileges",
				"no-shell-argv-exec",
				"open-file-limit",
				"output-file-size-limit",
				"process-group-cleanup",
				"subprocess-limit",
			};
		}

		[[nodiscard]] sandbox_report sandbox_evidence(const sandbox_requirement& requirement,
													  const sandbox_assurance achieved)
		{
			auto applied = mechanisms();
			std::string evidence = requirement.policy_digest;
			for (const auto& mechanism : applied)
				evidence += "\n" + mechanism;
			return {
				"linux-glibc",
				std::move(applied),
				achieved,
				requirement.policy_digest,
				cxxlens::sdk::semantic_digest("cxxlens.provider-sandbox-evidence.v1", evidence)};
		}

#if defined(__linux__) && defined(__GLIBC__)
		class descriptor
		{
		  public:
			explicit descriptor(const int value = -1) noexcept : value_{value} {}
			descriptor(const descriptor&) = delete;
			descriptor& operator=(const descriptor&) = delete;
			descriptor(descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
			descriptor& operator=(descriptor&& other) noexcept
			{
				if (this != &other)
					reset(std::exchange(other.value_, -1));
				return *this;
			}
			~descriptor()
			{
				reset();
			}
			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] int release() noexcept
			{
				return std::exchange(value_, -1);
			}
			void reset(const int value = -1) noexcept
			{
				if (value_ >= 0)
					(void)::close(value_);
				value_ = value;
			}

		  private:
			int value_;
		};

		struct pipe_pair
		{
			descriptor read;
			descriptor write;
		};

		[[nodiscard]] result<pipe_pair> make_pipe()
		{
			std::array<int, 2U> values{};
			if (::pipe2(values.data(), O_CLOEXEC) != 0)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-launch-failed", "pipe", std::to_string(errno)));
			const auto flags = ::fcntl(values[0], F_GETFL);
			if (flags < 0 || ::fcntl(values[0], F_SETFL, flags | O_NONBLOCK) != 0)
			{
				const auto failure = errno;
				(void)::close(values[0]);
				(void)::close(values[1]);
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "pipe-nonblocking", std::to_string(failure)));
			}
			return pipe_pair{descriptor{values[0]}, descriptor{values[1]}};
		}

		[[nodiscard]] result<descriptor> make_input(const std::span<const std::byte> input)
		{
			const int value =
				::memfd_create("cxxlens-provider-input", MFD_CLOEXEC | MFD_ALLOW_SEALING);
			if (value < 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "input", std::to_string(errno)));
			descriptor output{value};
			std::size_t offset{};
			while (offset < input.size())
			{
				const auto written = ::write(value, input.data() + offset, input.size() - offset);
				if (written > 0)
				{
					offset += static_cast<std::size_t>(written);
					continue;
				}
				if (written < 0 && errno == EINTR)
					continue;
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "input-write", std::to_string(errno)));
			}
			if (::lseek(value, 0, SEEK_SET) < 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "input-seek", std::to_string(errno)));
			if (::fcntl(value,
						F_ADD_SEALS,
						F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "input-seal", std::to_string(errno)));
			return output;
		}

		struct resource_limit
		{
			int resource;
			std::uint64_t value;
		};

		[[nodiscard]] bool set_limit(const resource_limit requested) noexcept
		{
			const auto bounded = requested.value > std::numeric_limits<rlim_t>::max()
				? std::numeric_limits<rlim_t>::max()
				: static_cast<rlim_t>(requested.value);
			const rlimit limit{bounded, bounded};
			return ::setrlimit(requested.resource, &limit) == 0;
		}

		[[nodiscard]] bool install_network_filter() noexcept
		{
			const auto statement = [](const std::uint16_t code, const std::uint32_t value)
			{
				return sock_filter{code, 0U, 0U, value};
			};
			const auto jump = [](const std::uint16_t code,
								 const std::uint32_t value,
								 const std::uint8_t yes,
								 const std::uint8_t no)
			{
				return sock_filter{code, yes, no, value};
			};
			const std::array<sock_filter, 16U> filter{
				statement(BPF_LD | BPF_W | BPF_ABS,
						  static_cast<std::uint32_t>(offsetof(seccomp_data, nr))),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_socket, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_socketpair, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_connect, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_bind, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_listen, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				jump(BPF_JMP | BPF_JEQ | BPF_K, __NR_accept4, 0U, 1U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ERRNO | EPERM),
				statement(BPF_RET | BPF_K, SECCOMP_RET_ALLOW),
			};
			const sock_fprog program{static_cast<unsigned short>(filter.size()),
									 const_cast<sock_filter*>(filter.data())};
			return ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) == 0 &&
				::prctl(PR_SET_SECCOMP, SECCOMP_MODE_FILTER, &program) == 0;
		}

		[[nodiscard]] bool configure_child(const process_invocation& invocation) noexcept
		{
			const auto cpu_seconds =
				std::max<std::uint64_t>(1U, (invocation.budget.cpu_ms + 999U) / 1000U);
			return set_limit({RLIMIT_CPU, cpu_seconds}) &&
				set_limit({RLIMIT_AS, invocation.budget.rss_bytes}) &&
				set_limit({RLIMIT_FSIZE, invocation.budget.output_bytes}) &&
				set_limit({RLIMIT_NOFILE, invocation.budget.open_files}) &&
				set_limit({RLIMIT_NPROC, invocation.budget.subprocesses}) &&
				install_network_filter();
		}

		[[nodiscard]] std::vector<char*> pointers(std::vector<std::string>& values)
		{
			std::vector<char*> output;
			output.reserve(values.size() + 1U);
			for (auto& value : values)
				output.push_back(value.data());
			output.push_back(nullptr);
			return output;
		}

		[[nodiscard]] bool drain(const int source,
								 std::vector<std::byte>& bytes,
								 std::string& text,
								 const bool binary,
								 std::size_t& total,
								 const std::size_t limit,
								 bool& ended)
		{
			std::array<char, 4096U> buffer{};
			for (;;)
			{
				const auto count = ::read(source, buffer.data(), buffer.size());
				if (count > 0)
				{
					const auto received = static_cast<std::size_t>(count);
					if (received > limit - total)
						return false;
					total += received;
					if (binary)
						for (const auto value : std::span{buffer}.first(received))
							bytes.push_back(
								static_cast<std::byte>(static_cast<unsigned char>(value)));
					else
						text.append(buffer.data(), received);
					continue;
				}
				if (count == 0)
				{
					ended = true;
					return true;
				}
				if (errno == EINTR)
					continue;
				if (errno == EAGAIN || errno == EWOULDBLOCK)
					return true;
				ended = true;
				return false;
			}
		}

		class linux_process_port final : public provider_process_port
		{
		  public:
			[[nodiscard]] result<process_output>
			run(const process_invocation& invocation,
				const std::stop_token cancellation) const override
			{
				if (invocation.argv.empty() || invocation.argv.front().empty() ||
					!invocation.argv.front().contains('/') ||
					std::ranges::any_of(invocation.argv, contains_nul) ||
					contains_nul(invocation.working_directory) || invocation.budget.wall_ms == 0U ||
					invocation.budget.cpu_ms == 0U || invocation.budget.rss_bytes == 0U ||
					invocation.budget.output_bytes == 0U || invocation.budget.open_files == 0U ||
					invocation.budget.subprocesses == 0U)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "invocation"));
				if (auto valid = invocation.sandbox.validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				auto actual_binary_digest = executable_digest(invocation.argv.front());
				if (!actual_binary_digest)
					return cxxlens::sdk::unexpected(std::move(actual_binary_digest.error()));
				if (invocation.expected_binary_digest.empty() ||
					*actual_binary_digest != invocation.expected_binary_digest)
					return process_output{
						process_status::launch_failed,
						0,
						0,
						{},
						"selected provider executable digest does not match its manifest",
						sandbox_evidence(invocation.sandbox, sandbox_assurance::none),
						"provider.binary-identity-mismatch"};
				if (cancellation.stop_requested())
					return process_output{
						process_status::cancelled,
						0,
						0,
						{},
						{},
						sandbox_evidence(invocation.sandbox, sandbox_assurance::enforced),
						{}};

				auto input = make_input(invocation.standard_input);
				auto output_pipe = make_pipe();
				auto error_pipe = make_pipe();
				if (!input)
					return cxxlens::sdk::unexpected(std::move(input.error()));
				if (!output_pipe)
					return cxxlens::sdk::unexpected(std::move(output_pipe.error()));
				if (!error_pipe)
					return cxxlens::sdk::unexpected(std::move(error_pipe.error()));

				std::vector<std::string> environment_storage;
				environment_storage.reserve(invocation.environment.size() + 2U);
				environment_storage.emplace_back("LANG=C");
				environment_storage.emplace_back("LC_ALL=C");
				for (const auto& [name, value] : invocation.environment)
				{
					if (name.empty() || name.contains('=') || contains_nul(name) ||
						contains_nul(value))
						return cxxlens::sdk::unexpected(
							process_error("provider.process-request-invalid", "environment"));
					std::string entry{name};
					entry += '=';
					entry += value;
					environment_storage.push_back(std::move(entry));
				}
				auto arguments_storage = invocation.argv;
				auto arguments = pointers(arguments_storage);
				auto environment = pointers(environment_storage);

				const auto child = ::fork();
				if (child < 0)
					return process_output{
						process_status::launch_failed,
						0,
						0,
						{},
						std::strerror(errno),
						sandbox_evidence(invocation.sandbox, sandbox_assurance::none),
						"provider.runtime-unavailable"};
				if (child == 0)
				{
					(void)::setpgid(0, 0);
					(void)::dup2(input->get(), STDIN_FILENO);
					(void)::dup2(output_pipe->write.get(), STDOUT_FILENO);
					(void)::dup2(error_pipe->write.get(), STDERR_FILENO);
					if (!invocation.working_directory.empty() &&
						::chdir(invocation.working_directory.c_str()) != 0)
						::_exit(125);
					if (!configure_child(invocation))
						::_exit(126);
					::execve(
						arguments_storage.front().c_str(), arguments.data(), environment.data());
					::_exit(127);
				}

				(void)::setpgid(child, child);
				input->reset();
				output_pipe->write.reset();
				error_pipe->write.reset();
				const auto started = std::chrono::steady_clock::now();
				const auto deadline =
					started + std::chrono::milliseconds{invocation.budget.wall_ms};
				process_output output;
				output.sandbox = sandbox_evidence(invocation.sandbox, sandbox_assurance::enforced);
				std::size_t total{};
				bool stdout_ended{};
				bool stderr_ended{};
				bool reaped{};
				int wait_status{};
				auto terminate = [&](const process_status status)
				{
					(void)::kill(-child, SIGKILL);
					while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR)
					{
					}
					reaped = true;
					output.status = status;
				};

				while (!reaped || !stdout_ended || !stderr_ended)
				{
					if (cancellation.stop_requested())
					{
						terminate(process_status::cancelled);
						break;
					}
					if (std::chrono::steady_clock::now() >= deadline)
					{
						terminate(process_status::timed_out);
						break;
					}
					std::array<pollfd, 2U> descriptors{
						pollfd{output_pipe->read.get(), POLLIN | POLLHUP, 0},
						pollfd{error_pipe->read.get(), POLLIN | POLLHUP, 0},
					};
					(void)::poll(descriptors.data(), descriptors.size(), 10);
					if (!stdout_ended &&
						!drain(output_pipe->read.get(),
							   output.standard_output,
							   output.standard_error,
							   true,
							   total,
							   static_cast<std::size_t>(invocation.budget.output_bytes),
							   stdout_ended))
					{
						terminate(process_status::output_limit);
						break;
					}
					if (!stderr_ended &&
						!drain(error_pipe->read.get(),
							   output.standard_output,
							   output.standard_error,
							   false,
							   total,
							   static_cast<std::size_t>(invocation.budget.output_bytes),
							   stderr_ended))
					{
						terminate(process_status::output_limit);
						break;
					}
					if (!reaped)
					{
						const auto waited = ::waitpid(child, &wait_status, WNOHANG);
						if (waited == child)
							reaped = true;
						else if (waited < 0 && errno != EINTR)
						{
							terminate(process_status::launch_failed);
							break;
						}
					}
				}

				if (!reaped)
					while (::waitpid(child, &wait_status, 0) < 0 && errno == EINTR)
					{
					}
				if (output.status == process_status::launch_failed && WIFSIGNALED(wait_status))
				{
					output.status = process_status::crashed;
					output.termination_signal = WTERMSIG(wait_status);
				}
				else if (output.status == process_status::launch_failed && WIFEXITED(wait_status))
				{
					output.status = process_status::exited;
					output.exit_code = WEXITSTATUS(wait_status);
				}
				return output;
			}
		};
#else
		class unavailable_process_port final : public provider_process_port
		{
		  public:
			result<process_output> run(const process_invocation& invocation,
									   std::stop_token) const override
			{
				return process_output{process_status::unavailable,
									  0,
									  0,
									  {},
									  "linux-glibc-required",
									  {"unsupported",
									   {},
									   sandbox_assurance::none,
									   invocation.sandbox.policy_digest,
									   cxxlens::sdk::semantic_digest(
										   "cxxlens.provider-sandbox-evidence.v1", "unsupported")},
									  "provider.runtime-unavailable"};
			}
		};
#endif
	} // namespace

	std::unique_ptr<provider_process_port> make_system_provider_process_port()
	{
#if defined(__linux__) && defined(__GLIBC__)
		return std::make_unique<linux_process_port>();
#else
		return std::make_unique<unavailable_process_port>();
#endif
	}
} // namespace cxxlens::sdk::provider
