#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <linux/audit.h>
#include <linux/close_range.h>
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk/provider.hpp>

#include "../sdk/provider_ng1_process_internal.hpp"
#include "../sdk/provider_runtime_internal.hpp"

namespace cxxlens::sdk::provider
{
#if defined(__linux__) && defined(__GLIBC__)
	namespace detail
	{
		ng1_post_fork_process_guard::ng1_post_fork_process_guard(const int child) noexcept
			: child_{child}
		{
		}

		ng1_post_fork_process_guard::~ng1_post_fork_process_guard() noexcept
		{
			cleanup();
		}

		void ng1_post_fork_process_guard::release() noexcept
		{
			child_ = -1;
		}

		void ng1_post_fork_process_guard::cleanup() noexcept
		{
			const auto child = static_cast<pid_t>(std::exchange(child_, -1));
			if (child <= 0)
				return;

			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
			while (std::chrono::steady_clock::now() < deadline)
			{
				// The child PID is freshly allocated and remains waitable until this guard
				// reaps it, so a group with that ID cannot belong to an unrelated process.
				// Attempt the group first even when the setup ACK was not observed; this
				// closes the scheduler/ACK-timeout race for descendants.
				(void)::kill(-child, SIGKILL);
				(void)::kill(child, SIGKILL);
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			}
			while (::waitpid(child, nullptr, 0) < 0 && errno == EINTR)
			{
			}
		}
	} // namespace detail
#endif

	namespace detail
	{
		namespace
		{
			[[nodiscard]] error
			process_error(std::string code, std::string field, std::string detail = {})
			{
				return {std::move(code), std::move(field), std::move(detail)};
			}

			constexpr std::string_view semantic_digest_prefix{"semantic-v2:sha256:"};
			constexpr std::string_view task_digest_prefix{"task:semantic-v2:sha256:"};
			constexpr std::string_view session_digest_prefix{"provider-session:sha256:"};

			[[nodiscard]] bool valid_typed_digest(const std::string_view value,
												  const std::string_view prefix) noexcept
			{
				if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U)
					return false;
				for (const auto byte : value.substr(prefix.size()))
					if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
						return false;
				return true;
			}

			[[nodiscard]] result<std::string>
			process_channel_binding_digest(const process_inherited_channel_binding& value)
			{
				return canonical_identity_digest(
					"process-channel",
					std::array{
						canonical_value::from_string(value.task_id),
						canonical_value::from_string(value.session_id),
						canonical_value::from_string(value.closure_digest),
						canonical_value::from_string(value.transfer_digest),
						canonical_value::from_integer(value.read_descriptor),
						canonical_value::from_integer(value.write_descriptor),
						canonical_value::from_string(std::to_string(value.read_device)),
						canonical_value::from_string(std::to_string(value.read_inode)),
						canonical_value::from_string(std::to_string(value.read_mode)),
						canonical_value::from_string(std::to_string(value.write_device)),
						canonical_value::from_string(std::to_string(value.write_inode)),
						canonical_value::from_string(std::to_string(value.write_mode)),
					});
			}

#if defined(__linux__) && defined(__GLIBC__)
			struct descriptor_identity
			{
				std::uint64_t device{};
				std::uint64_t inode{};
				std::uint32_t mode{};
			};

			[[nodiscard]] result<descriptor_identity> inspect_channel_descriptor(
				const int descriptor, const std::string_view field, const bool read_endpoint)
			{
				if (descriptor < 4)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-channel-invalid",
									  std::string{field},
									  "reserved-descriptor"));
				const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
				if (descriptor_flags < 0)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-channel-invalid",
									  std::string{field},
									  "descriptor-closed"));
				if ((descriptor_flags & FD_CLOEXEC) != 0)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-channel-invalid",
									  std::string{field},
									  "close-on-exec-set"));
				const auto status_flags = ::fcntl(descriptor, F_GETFL);
				if (status_flags < 0)
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-channel-invalid", std::string{field}, "status"));
				if ((status_flags & O_NONBLOCK) == 0)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-channel-invalid",
									  std::string{field},
									  "blocking-descriptor"));
				const auto access_mode = status_flags & O_ACCMODE;
				if ((read_endpoint && access_mode == O_WRONLY) ||
					(!read_endpoint && access_mode == O_RDONLY))
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-channel-invalid", std::string{field}, "access-mode"));
				struct stat metadata{};
				if (::fstat(descriptor, &metadata) != 0)
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-channel-invalid", std::string{field}, "stat"));
				if (!S_ISFIFO(metadata.st_mode) && !S_ISSOCK(metadata.st_mode))
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-channel-invalid", std::string{field}, "channel-type"));
				return descriptor_identity{static_cast<std::uint64_t>(metadata.st_dev),
										   static_cast<std::uint64_t>(metadata.st_ino),
										   static_cast<std::uint32_t>(metadata.st_mode)};
			}
#endif
		} // namespace

		result<void> process_inherited_channel_binding::validate() const
		{
			if (!valid_typed_digest(task_id, task_digest_prefix))
				return cxxlens::sdk::unexpected(
					process_error("provider.process-channel-invalid", "task_id", "typed-digest"));
			if (!valid_typed_digest(session_id, session_digest_prefix))
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-channel-invalid", "session_id", "typed-digest"));
			if (!valid_typed_digest(closure_digest, semantic_digest_prefix))
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-channel-invalid", "closure_digest", "typed-digest"));
			if (!valid_typed_digest(transfer_digest, semantic_digest_prefix))
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-channel-invalid", "transfer_digest", "typed-digest"));
			if (read_descriptor < 4 || write_descriptor < 4)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-channel-invalid", "descriptor", "reserved-descriptor"));
			if (read_descriptor == write_descriptor)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-channel-invalid", "descriptor", "duplicate"));
			if (!binding_digest.starts_with("process-channel:sha256:") ||
				binding_digest.size() != std::string_view{"process-channel:sha256:"}.size() + 64U)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-channel-invalid", "binding_digest", "typed-digest"));

#if defined(__linux__) && defined(__GLIBC__)
			auto read = inspect_channel_descriptor(read_descriptor, "read_descriptor", true);
			if (!read)
				return cxxlens::sdk::unexpected(std::move(read.error()));
			auto write = inspect_channel_descriptor(write_descriptor, "write_descriptor", false);
			if (!write)
				return cxxlens::sdk::unexpected(std::move(write.error()));
			if (read->device != read_device || read->inode != read_inode ||
				read->mode != read_mode || write->device != write_device ||
				write->inode != write_inode || write->mode != write_mode)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-channel-foreign", "descriptor", "identity"));
#else
			return cxxlens::sdk::unexpected(process_error(
				"provider.process-channel-unavailable", "platform", "linux-glibc-required"));
#endif
			auto expected = process_channel_binding_digest(*this);
			if (!expected || *expected != binding_digest)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-channel-foreign", "binding", "identity"));
			return {};
		}

		result<std::shared_ptr<const process_inherited_channel_binding>>
		make_process_inherited_channel_binding(const int read_descriptor,
											   const int write_descriptor,
											   std::string task_id,
											   std::string session_id,
											   std::string closure_digest,
											   std::string transfer_digest)
		{
			if (read_descriptor == write_descriptor)
				return cxxlens::sdk::unexpected(
					process_error("provider.process-channel-invalid", "descriptor", "duplicate"));
			process_inherited_channel_binding value;
			value.read_descriptor = read_descriptor;
			value.write_descriptor = write_descriptor;
			value.task_id = std::move(task_id);
			value.session_id = std::move(session_id);
			value.closure_digest = std::move(closure_digest);
			value.transfer_digest = std::move(transfer_digest);
#if defined(__linux__) && defined(__GLIBC__)
			auto read = inspect_channel_descriptor(read_descriptor, "read_descriptor", true);
			if (!read)
				return cxxlens::sdk::unexpected(std::move(read.error()));
			auto write = inspect_channel_descriptor(write_descriptor, "write_descriptor", false);
			if (!write)
				return cxxlens::sdk::unexpected(std::move(write.error()));
			value.read_device = read->device;
			value.read_inode = read->inode;
			value.read_mode = read->mode;
			value.write_device = write->device;
			value.write_inode = write->inode;
			value.write_mode = write->mode;
#else
			return cxxlens::sdk::unexpected(process_error(
				"provider.process-channel-unavailable", "platform", "linux-glibc-required"));
#endif
			auto digest = process_channel_binding_digest(value);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			value.binding_digest = std::move(*digest);
			if (auto valid = value.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			return std::shared_ptr<const process_inherited_channel_binding>{
				std::make_shared<process_inherited_channel_binding>(std::move(value))};
		}
	} // namespace detail

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

		[[nodiscard]] sandbox_report sandbox_evidence(const sandbox_policy& policy,
													  const execution_budget& budget,
													  const sandbox_assurance achieved,
													  const bool installed,
													  const std::string_view executable_digest)
		{
			auto applied = installed ? policy.mechanisms : std::vector<std::string>{};
			auto evidence =
				sandbox_evidence_digest(policy, budget, achieved, applied, executable_digest);
			return {"linux-glibc",
					applied,
					achieved,
					policy.policy_digest(),
					evidence ? std::move(*evidence) : std::string{}};
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

		struct verified_executable
		{
			descriptor image;
			std::string digest;
		};

		[[nodiscard]] result<verified_executable>
		make_verified_executable(const process_invocation& invocation)
		{
			descriptor directory;
			int source_value{-1};
			const bool relative = invocation.argv.front().front() != '/';
			if (relative && !invocation.working_directory.empty())
			{
				directory.reset(
					::open(invocation.working_directory.c_str(), O_PATH | O_DIRECTORY | O_CLOEXEC));
				if (directory.get() < 0)
					return cxxlens::sdk::unexpected(process_error("provider.process-launch-failed",
																  "working-directory-open",
																  std::to_string(errno)));
				source_value = ::openat(
					directory.get(), invocation.argv.front().c_str(), O_RDONLY | O_CLOEXEC);
			}
			else
				source_value = ::open(invocation.argv.front().c_str(), O_RDONLY | O_CLOEXEC);
			if (source_value < 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "executable-open", std::to_string(errno)));
			descriptor source{source_value};
			struct stat metadata{};
			if (::fstat(source.get(), &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
				(metadata.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "executable-type", std::to_string(errno)));

			const int image_value =
				::memfd_create("cxxlens-provider-executable", MFD_CLOEXEC | MFD_ALLOW_SEALING);
			if (image_value < 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "executable-memfd", std::to_string(errno)));
			descriptor image{image_value};
			std::vector<std::byte> measured;
			std::array<std::byte, 65536U> buffer{};
			for (;;)
			{
				const auto count = ::read(source.get(), buffer.data(), buffer.size());
				if (count == 0)
					break;
				if (count < 0)
				{
					if (errno == EINTR)
						continue;
					return cxxlens::sdk::unexpected(process_error("provider.process-launch-failed",
																  "executable-read",
																  std::to_string(errno)));
				}
				const auto received = static_cast<std::size_t>(count);
				measured.insert(measured.end(), buffer.begin(), buffer.begin() + received);
				std::size_t offset{};
				while (offset < received)
				{
					const auto written =
						::write(image.get(), buffer.data() + offset, received - offset);
					if (written > 0)
					{
						offset += static_cast<std::size_t>(written);
						continue;
					}
					if (written < 0 && errno == EINTR)
						continue;
					return cxxlens::sdk::unexpected(process_error("provider.process-launch-failed",
																  "executable-copy",
																  std::to_string(errno)));
				}
			}
			if (::fchmod(image.get(), S_IRUSR | S_IXUSR) != 0 ||
				::fcntl(image.get(),
						F_ADD_SEALS,
						F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL) != 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "executable-seal", std::to_string(errno)));
			return verified_executable{std::move(image), cxxlens::sdk::content_digest(measured)};
		}

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

		class replayable_input_sink final : public frame_sink
		{
		  public:
			explicit replayable_input_sink(const int output) : output_{output} {}

			result<void> write(const std::span<const std::byte> bytes) override
			{
				// One exact 64 MiB logical input plus the maximum bounded frame/control envelope.
				// This is a transport safety bound only; the shared encoder owns semantic limits.
				constexpr std::uint64_t maximum_wire_input_bytes =
					std::uint64_t{72U} * 1024U * 1024U;
				if (bytes.size() > maximum_wire_input_bytes - written_)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "input-limit"));
				std::size_t offset{};
				while (offset < bytes.size())
				{
					const auto count =
						::write(output_, bytes.data() + offset, bytes.size() - offset);
					if (count > 0)
					{
						offset += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-launch-failed", "input-write", std::to_string(errno)));
				}
				written_ += bytes.size();
				return {};
			}

		  private:
			int output_{};
			std::uint64_t written_{};
		};

		[[nodiscard]] result<descriptor> make_replayable_input(
			const detail::replayable_provider_process_port::input_writer& write_input)
		{
			const int value =
				::memfd_create("cxxlens-provider-input", MFD_CLOEXEC | MFD_ALLOW_SEALING);
			if (value < 0)
				return cxxlens::sdk::unexpected(process_error(
					"provider.process-launch-failed", "input", std::to_string(errno)));
			descriptor output{value};
			replayable_input_sink sink{value};
			if (auto written = write_input(sink); !written)
				return cxxlens::sdk::unexpected(std::move(written.error()));
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
#if defined(__x86_64__)
			constexpr std::uint32_t audit_architecture = AUDIT_ARCH_X86_64;
#elif defined(__aarch64__)
			constexpr std::uint32_t audit_architecture = AUDIT_ARCH_AARCH64;
#else
			return false;
#endif
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
			const std::array<sock_filter, 19U> filter{
				statement(BPF_LD | BPF_W | BPF_ABS,
						  static_cast<std::uint32_t>(offsetof(seccomp_data, arch))),
				jump(BPF_JMP | BPF_JEQ | BPF_K, audit_architecture, 1U, 0U),
				statement(BPF_RET | BPF_K, SECCOMP_RET_KILL_PROCESS),
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

		[[nodiscard]] bool close_descriptor_range(const unsigned int first,
												  const unsigned int last) noexcept
		{
#if defined(SYS_close_range)
			if (first > last)
				return true;
			return ::syscall(SYS_close_range, first, last, CLOSE_RANGE_UNSHARE) == 0;
#else
			(void)first;
			(void)last;
			return false;
#endif
		}

		[[nodiscard]] bool prepare_inherited_descriptors(
			const detail::process_inherited_channel_binding* binding) noexcept
		{
			if (binding == nullptr)
				return true;
			for (const auto descriptor : {binding->read_descriptor, binding->write_descriptor})
			{
				const auto descriptor_flags = ::fcntl(descriptor, F_GETFD);
				const auto status_flags = ::fcntl(descriptor, F_GETFL);
				if (descriptor_flags < 0 || status_flags < 0 ||
					::fcntl(descriptor, F_SETFD, descriptor_flags & ~FD_CLOEXEC) < 0 ||
					::fcntl(descriptor, F_SETFL, status_flags | O_NONBLOCK) < 0)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool close_inherited_descriptors(
			const detail::process_inherited_channel_binding* binding) noexcept
		{
			std::array<int, 2U> ordered{-1, -1};
			if (binding != nullptr)
			{
				ordered[0] = binding->read_descriptor;
				ordered[1] = binding->write_descriptor;
				if (ordered[1] >= 0 && ordered[1] < ordered[0])
					std::swap(ordered[0], ordered[1]);
			}
			unsigned int first = 4U;
			for (const auto descriptor : ordered)
			{
				if (descriptor < 0)
					break;
				const auto value = static_cast<unsigned int>(descriptor);
				if (value < first || !close_descriptor_range(first, value - 1U))
					return false;
				if (value == std::numeric_limits<unsigned int>::max())
					return true;
				first = value + 1U;
			}
			return close_descriptor_range(first, std::numeric_limits<unsigned int>::max());
		}

		[[nodiscard]] bool configure_child(const process_invocation& invocation,
										   const sandbox_policy& policy) noexcept
		{
			const auto cpu_seconds =
				std::max<std::uint64_t>(1U, (invocation.budget.cpu_ms + 999U) / 1000U);
			const bool baseline = set_limit({RLIMIT_CPU, cpu_seconds}) &&
				set_limit({RLIMIT_AS, invocation.budget.address_space_bytes}) &&
				set_limit({RLIMIT_FSIZE, invocation.budget.transport_bytes}) &&
				set_limit({RLIMIT_NOFILE, invocation.budget.open_files}) &&
				set_limit({RLIMIT_NPROC, invocation.budget.subprocesses});
			if (!baseline || (policy.zero_core_dump && !set_limit({RLIMIT_CORE, 0U})) ||
				(policy.zero_locked_memory && !set_limit({RLIMIT_MEMLOCK, 0U})))
				return false;
			return !policy.deny_network || install_network_filter();
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

		[[nodiscard]] bool write_process_group_ack(const int destination,
												   const std::byte value) noexcept
		{
			for (;;)
			{
				const auto written = ::write(destination, &value, sizeof(value));
				if (written == sizeof(value))
					return true;
				if (written < 0 && errno == EINTR)
					continue;
				return false;
			}
		}

		[[nodiscard]] std::optional<bool> read_process_group_ack(const int source)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
			for (;;)
			{
				const auto now = std::chrono::steady_clock::now();
				if (now >= deadline)
					return std::nullopt;
				const auto remaining =
					std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
				const auto timeout = static_cast<int>(std::clamp<std::int64_t>(
					remaining.count(), 1, std::numeric_limits<int>::max()));
				pollfd descriptor{source, POLLIN | POLLHUP | POLLERR, 0};
				const auto polled = ::poll(&descriptor, 1U, timeout);
				if (polled < 0 && errno == EINTR)
					continue;
				if (polled <= 0)
					return std::nullopt;
				std::byte value{};
				const auto received = ::read(source, &value, sizeof(value));
				if (received == sizeof(value))
					return value == std::byte{0x01};
				if (received == 0 ||
					(received < 0 && errno != EINTR && errno != EAGAIN && errno != EWOULDBLOCK))
					return false;
			}
		}

		/** Write to a provider pipe without allowing a closed reader to terminate the host. */
		[[nodiscard]] ssize_t write_without_sigpipe(const int destination,
													const std::byte* bytes,
													const std::size_t size) noexcept
		{
			sigset_t blocked{};
			sigset_t previous{};
			if (sigemptyset(&blocked) != 0 || sigaddset(&blocked, SIGPIPE) != 0 ||
				sigprocmask(SIG_BLOCK, &blocked, &previous) != 0)
			{
				errno = EPERM;
				return -1;
			}

			sigset_t pending_before{};
			const bool was_pending =
				sigpending(&pending_before) == 0 && sigismember(&pending_before, SIGPIPE) == 1;
			const auto written = ::write(destination, bytes, size);
			const auto write_errno = errno;
			sigset_t pending_after{};
			if (!was_pending && sigpending(&pending_after) == 0 &&
				sigismember(&pending_after, SIGPIPE) == 1)
			{
				timespec timeout{};
				(void)sigtimedwait(&blocked, nullptr, &timeout);
			}
			const auto restore_status = sigprocmask(SIG_SETMASK, &previous, nullptr);
			if (written < 0)
				errno = write_errno;
			else if (restore_status != 0)
			{
				errno = restore_status;
				return -1;
			}
			return written;
		}

		/**
		 * Live Linux process channel used by the source-private NG1 seam.  The existing
		 * linux_process_port below intentionally remains the completed-process NG0 path; this
		 * channel has its own explicit lifetime so a caller cannot accidentally reclassify run().
		 */
		class linux_ng1_duplex_process final : public detail::ng1_duplex_process
		{
		  public:
			linux_ng1_duplex_process(descriptor input,
									 descriptor output,
									 descriptor error,
									 const pid_t child,
									 const protocol_limits limits,
									 sandbox_policy policy,
									 execution_budget budget,
									 std::string measured_digest) noexcept
				: input_{std::move(input)}, output_{std::move(output)}, error_{std::move(error)},
				  child_{child}, limits_{limits}, policy_{std::move(policy)}, budget_{budget},
				  measured_digest_{std::move(measured_digest)},
				  started_{std::chrono::steady_clock::now()}
			{
			}

			linux_ng1_duplex_process(const linux_ng1_duplex_process&) = delete;
			linux_ng1_duplex_process& operator=(const linux_ng1_duplex_process&) = delete;
			~linux_ng1_duplex_process() override
			{
				if (!finished_)
				{
					forced_status_ = process_status::cancelled;
					kill_process_group();
				}
			}

			result<void> send_frame(const frame& value) override
			{
				if (finished_ || input_.get() < 0)
					return cxxlens::sdk::unexpected(process_error(
						"provider.protocol-state-invalid", "ng1-live", "input-closed"));
				if (auto deadline = check_deadline(); !deadline)
				{
					(void)terminate(process_status::timed_out);
					return deadline;
				}
				auto encoded = encode_frame(value, limits_);
				if (!encoded)
					return cxxlens::sdk::unexpected(std::move(encoded.error()));
				std::size_t offset{};
				while (offset < encoded->size())
				{
					const auto written = write_without_sigpipe(
						input_.get(), encoded->data() + offset, encoded->size() - offset);
					if (written > 0)
					{
						offset += static_cast<std::size_t>(written);
						continue;
					}
					if (written < 0 && errno == EINTR)
						continue;
					if (written < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
					{
						std::array<pollfd, 3U> descriptors{
							pollfd{input_.get(), POLLOUT | POLLHUP | POLLERR, 0},
							pollfd{output_.get(), POLLIN | POLLHUP | POLLERR, 0},
							pollfd{error_.get(), POLLIN | POLLHUP | POLLERR, 0},
						};
						const auto polled = ::poll(descriptors.data(), descriptors.size(), 10);
						if (polled < 0 && errno == EINTR)
							continue;
						if (polled < 0)
							return cxxlens::sdk::unexpected(
								process_error("provider.process-launch-failed",
											  "ng1-live-write",
											  std::to_string(errno)));
						if ((descriptors[1].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) !=
								0 &&
							!drain_stdout())
						{
							forced_status_ = process_status::output_limit;
							kill_process_group();
							return cxxlens::sdk::unexpected(process_error(
								"provider.output-limit", "ng1-live", "transport-bytes"));
						}
						if ((descriptors[2].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)) !=
								0 &&
							!drain_stderr())
						{
							forced_status_ = process_status::output_limit;
							kill_process_group();
							return cxxlens::sdk::unexpected(process_error(
								"provider.output-limit", "ng1-live", "transport-bytes"));
						}
						if ((descriptors[0].revents & (POLLHUP | POLLERR | POLLNVAL)) != 0)
							return cxxlens::sdk::unexpected(process_error(
								"provider.worker-exit", "ng1-live-write", "stdin-closed"));
						if (auto deadline = check_deadline(); !deadline)
						{
							(void)terminate(process_status::timed_out);
							return deadline;
						}
						continue;
					}
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-launch-failed", "ng1-live-write", std::to_string(errno)));
				}
				return {};
			}

			result<std::optional<frame>> receive_frame(const std::stop_token cancellation) override
			{
				if (finished_)
					return cxxlens::sdk::unexpected(
						process_error("provider.protocol-state-invalid", "ng1-live", "finished"));
				for (;;)
				{
					auto next = take_frame();
					if (!next)
						return cxxlens::sdk::unexpected(std::move(next.error()));
					if (next->has_value())
						return next;
					if (cancellation.stop_requested())
					{
						(void)terminate(process_status::cancelled);
						return cxxlens::sdk::unexpected(
							process_error("provider.cancelled", "ng1-live", "stop-requested"));
					}
					if (auto deadline = check_deadline(); !deadline)
					{
						(void)terminate(process_status::timed_out);
						return cxxlens::sdk::unexpected(std::move(deadline.error()));
					}
					if (stdout_ended_)
					{
						if (!pending_.empty())
							return cxxlens::sdk::unexpected(process_error(
								"provider.truncated-stream", "ng1-live", "partial-frame"));
						return std::optional<frame>{};
					}

					std::array<pollfd, 2U> descriptors{
						pollfd{output_.get(), POLLIN | POLLHUP | POLLERR, 0},
						pollfd{error_.get(), POLLIN | POLLHUP | POLLERR, 0},
					};
					const auto polled = ::poll(descriptors.data(), descriptors.size(), 10);
					if (polled < 0 && errno == EINTR)
						continue;
					if (polled < 0)
						return cxxlens::sdk::unexpected(
							process_error("provider.process-launch-failed",
										  "ng1-live-read",
										  std::to_string(errno)));
					if (!drain_stdout() || !drain_stderr())
					{
						(void)terminate(process_status::output_limit);
						return cxxlens::sdk::unexpected(
							process_error("provider.output-limit", "ng1-live", "transport-bytes"));
					}
				}
			}

			result<process_output> finish(const std::stop_token cancellation) override
			{
				if (finished_)
				{
					if (completed_output_)
						return *completed_output_;
					return cxxlens::sdk::unexpected(
						process_error("provider.protocol-state-invalid", "ng1-live", "finished"));
				}
				input_.reset();
				for (;;)
				{
					reap_nonblocking();
					if (cancellation.stop_requested())
						forced_status_ = process_status::cancelled;
					if (forced_status_ != process_status::launch_failed &&
						(!reaped_ || !stdout_ended_ || !stderr_ended_))
						kill_process_group();
					if (reaped_ && stdout_ended_ && stderr_ended_)
						break;
					if (std::chrono::steady_clock::now() >= deadline_)
					{
						forced_status_ = process_status::timed_out;
						kill_process_group();
						output_.reset();
						error_.reset();
						stdout_ended_ = true;
						stderr_ended_ = true;
						break;
					}
					std::array<pollfd, 2U> descriptors{
						pollfd{output_.get(), POLLIN | POLLHUP | POLLERR, 0},
						pollfd{error_.get(), POLLIN | POLLHUP | POLLERR, 0},
					};
					const auto polled = ::poll(descriptors.data(), descriptors.size(), 10);
					if (polled < 0 && errno == EINTR)
						continue;
					if (polled < 0)
						return cxxlens::sdk::unexpected(
							process_error("provider.process-launch-failed",
										  "ng1-live-finish",
										  std::to_string(errno)));
					if (!drain_stdout() || !drain_stderr())
					{
						forced_status_ = process_status::output_limit;
						kill_process_group();
					}
				}
				finished_ = true;
				process_output output;
				output.status = forced_status_;
				if (forced_status_ == process_status::launch_failed)
				{
					if (WIFSIGNALED(wait_status_))
					{
						output.status = process_status::crashed;
						output.termination_signal = WTERMSIG(wait_status_);
					}
					else if (WIFEXITED(wait_status_))
					{
						output.status = process_status::exited;
						output.exit_code = WEXITSTATUS(wait_status_);
					}
				}
				else if (WIFSIGNALED(wait_status_))
					output.termination_signal = WTERMSIG(wait_status_);
				else if (WIFEXITED(wait_status_))
					output.exit_code = WEXITSTATUS(wait_status_);
				output.standard_output = std::move(stdout_bytes_);
				output.standard_error = std::move(stderr_text_);
				output.measured_executable_digest = measured_digest_;
				output.sandbox = sandbox_evidence(
					policy_, budget_, sandbox_assurance::enforced, true, measured_digest_);
				if (output.exit_code == 126 && output.status == process_status::exited)
				{
					output.failure_code = "security.sandbox-insufficient";
					output.sandbox = sandbox_evidence(
						policy_, budget_, sandbox_assurance::none, false, measured_digest_);
				}
				completed_output_ = output;
				return output;
			}

			result<process_output> terminate(const process_status status) override
			{
				if (finished_)
				{
					if (completed_output_)
						return *completed_output_;
					return cxxlens::sdk::unexpected(
						process_error("provider.protocol-state-invalid", "ng1-live", "finished"));
				}
				forced_status_ = status;
				kill_process_group();
				return finish({});
			}

		  private:
			static constexpr std::size_t wire_header_bytes = 104U;

			[[nodiscard]] static std::uint64_t read_big_endian(
				const std::span<const std::byte> input,
				// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): wire offset and field width
				const std::size_t offset,
				const std::size_t width) noexcept
			{
				std::uint64_t value{};
				for (std::size_t index{}; index < width; ++index)
					value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
				return value;
			}

			[[nodiscard]] result<void> check_deadline() const
			{
				if (std::chrono::steady_clock::now() >= deadline_)
					return cxxlens::sdk::unexpected(
						process_error("provider.timeout", "ng1-live", "wall-deadline"));
				return {};
			}

			[[nodiscard]] result<std::optional<frame>> take_frame()
			{
				if (pending_.size() < wire_header_bytes)
					return std::optional<frame>{};
				const auto header = std::span<const std::byte>{pending_}.first(wire_header_bytes);
				const auto control_length = read_big_endian(header, 28U, 4U);
				const auto payload_length = read_big_endian(header, 32U, 8U);
				if (control_length > limits_.max_control_bytes ||
					payload_length > limits_.max_payload_bytes)
					return cxxlens::sdk::unexpected(
						process_error("provider.oversized-frame", "ng1-live", "frame-length"));
				if (payload_length >
					std::numeric_limits<std::size_t>::max() - wire_header_bytes - control_length)
					return cxxlens::sdk::unexpected(
						process_error("provider.oversized-frame", "ng1-live", "frame-overflow"));
				const auto total = wire_header_bytes + static_cast<std::size_t>(control_length) +
					static_cast<std::size_t>(payload_length);
				if (pending_.size() < total)
					return std::optional<frame>{};
				auto decoded =
					decode_frame(std::span<const std::byte>{pending_}.first(total), limits_);
				if (!decoded)
					return cxxlens::sdk::unexpected(std::move(decoded.error()));
				pending_.erase(pending_.begin(),
							   pending_.begin() + static_cast<std::ptrdiff_t>(total));
				return std::optional<frame>{std::move(*decoded)};
			}

			[[nodiscard]] bool account_transport(const std::size_t received) noexcept
			{
				if (transport_bytes_ > budget_.transport_bytes ||
					received > budget_.transport_bytes - transport_bytes_)
					return false;
				transport_bytes_ += received;
				return true;
			}

			[[nodiscard]] bool drain_stdout()
			{
				std::array<char, 4096U> buffer{};
				for (;;)
				{
					const auto count = ::read(output_.get(), buffer.data(), buffer.size());
					if (count > 0)
					{
						const auto received = static_cast<std::size_t>(count);
						if (!account_transport(received))
							return false;
						for (const auto value : std::span{buffer}.first(received))
						{
							const auto byte =
								static_cast<std::byte>(static_cast<unsigned char>(value));
							stdout_bytes_.push_back(byte);
							pending_.push_back(byte);
						}
						continue;
					}
					if (count == 0)
					{
						stdout_ended_ = true;
						return true;
					}
					if (errno == EINTR)
						continue;
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						return true;
					stdout_ended_ = true;
					return false;
				}
			}

			[[nodiscard]] bool drain_stderr()
			{
				std::array<char, 4096U> buffer{};
				for (;;)
				{
					const auto count = ::read(error_.get(), buffer.data(), buffer.size());
					if (count > 0)
					{
						const auto received = static_cast<std::size_t>(count);
						if (!account_transport(received))
							return false;
						stderr_text_.append(buffer.data(), received);
						continue;
					}
					if (count == 0)
					{
						stderr_ended_ = true;
						return true;
					}
					if (errno == EINTR)
						continue;
					if (errno == EAGAIN || errno == EWOULDBLOCK)
						return true;
					stderr_ended_ = true;
					return false;
				}
			}

			void reap_nonblocking() noexcept
			{
				if (reaped_ || child_ <= 0)
					return;
				const auto waited = ::waitpid(child_, &wait_status_, WNOHANG);
				if (waited == child_ || (waited < 0 && errno == ECHILD))
					reaped_ = true;
			}

			void kill_process_group() noexcept
			{
				if (child_ <= 0 || process_group_killed_)
					return;
				const auto signal_deadline =
					std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
				while (std::chrono::steady_clock::now() < signal_deadline)
				{
					(void)::kill(-child_, SIGKILL);
					std::this_thread::sleep_for(std::chrono::milliseconds{1});
				}
				if (!reaped_)
				{
					while (::waitpid(child_, &wait_status_, 0) < 0 && errno == EINTR)
					{
					}
					reaped_ = true;
				}
				process_group_killed_ = true;
			}

			descriptor input_;
			descriptor output_;
			descriptor error_;
			pid_t child_{};
			protocol_limits limits_;
			sandbox_policy policy_;
			execution_budget budget_;
			std::string measured_digest_;
			std::chrono::steady_clock::time_point started_;
			std::chrono::steady_clock::time_point deadline_ =
				started_ + std::chrono::milliseconds{budget_.wall_ms};
			std::vector<std::byte> pending_;
			std::vector<std::byte> stdout_bytes_;
			std::string stderr_text_;
			std::uint64_t transport_bytes_{};
			process_status forced_status_{process_status::launch_failed};
			int wait_status_{};
			bool stdout_ended_{};
			bool stderr_ended_{};
			bool reaped_{};
			bool process_group_killed_{};
			bool finished_{};
			std::optional<process_output> completed_output_;
		};

		class linux_process_port final : public provider_process_port,
										 public detail::replayable_provider_process_port,
										 public detail::ng1_duplex_process_port
		{
		  public:
			[[nodiscard]] result<std::unique_ptr<detail::ng1_duplex_process>>
			start(const process_invocation& invocation,
				  const protocol_limits limits,
				  const std::stop_token cancellation) const override
			{
				if (auto valid = invocation.budget.validate(); !valid)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "budget"));
				if (invocation.argv.empty() || invocation.argv.front().empty() ||
					!invocation.argv.front().contains('/') ||
					std::ranges::any_of(invocation.argv, contains_nul) ||
					contains_nul(invocation.working_directory))
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "invocation"));
				if (limits.minimum_minor > limits.maximum_minor)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "protocol-limits"));
				if (auto valid = invocation.sandbox.validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				auto policy = resolve_sandbox_policy(invocation.sandbox.policy_digest);
				if (!policy)
					return cxxlens::sdk::unexpected(std::move(policy.error()));
				if (invocation.inherited_channel)
				{
					if (auto valid = invocation.inherited_channel->validate(); !valid)
						return cxxlens::sdk::unexpected(std::move(valid.error()));
				}
				auto verified = make_verified_executable(invocation);
				if (!verified)
					return cxxlens::sdk::unexpected(std::move(verified.error()));
				if (invocation.expected_binary_digest.empty() ||
					verified->digest != invocation.expected_binary_digest)
					return cxxlens::sdk::unexpected(process_error(
						"provider.binary-identity-mismatch", "ng1-live", "executable"));
				if (cancellation.stop_requested())
					return cxxlens::sdk::unexpected(
						process_error("provider.cancelled", "ng1-live", "before-launch"));

				auto input = make_pipe();
				auto output_pipe = make_pipe();
				auto error_pipe = make_pipe();
				auto process_group_pipe = make_pipe();
				if (!input)
					return cxxlens::sdk::unexpected(std::move(input.error()));
				if (!output_pipe)
					return cxxlens::sdk::unexpected(std::move(output_pipe.error()));
				if (!error_pipe)
					return cxxlens::sdk::unexpected(std::move(error_pipe.error()));
				if (!process_group_pipe)
					return cxxlens::sdk::unexpected(std::move(process_group_pipe.error()));
				const auto input_flags = ::fcntl(input->write.get(), F_GETFL);
				if (input_flags < 0 ||
					::fcntl(input->write.get(), F_SETFL, input_flags | O_NONBLOCK) != 0)
					return cxxlens::sdk::unexpected(process_error("provider.process-launch-failed",
																  "input-nonblocking",
																  std::to_string(errno)));

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
					return cxxlens::sdk::unexpected(process_error(
						"provider.process-launch-failed", "fork", std::to_string(errno)));
				if (child == 0)
				{
					process_group_pipe->read.reset();
					const bool process_group_established = ::setpgid(0, 0) == 0;
					if (!write_process_group_ack(process_group_pipe->write.get(),
												 process_group_established ? std::byte{0x01}
																		   : std::byte{0x00}))
						::_exit(126);
					process_group_pipe->write.reset();
					if (!process_group_established)
						::_exit(126);
					const auto input_read_flags = ::fcntl(input->read.get(), F_GETFL);
					if (input_read_flags < 0 ||
						::fcntl(input->read.get(), F_SETFL, input_read_flags & ~O_NONBLOCK) != 0)
						::_exit(126);
					if (::dup2(input->read.get(), STDIN_FILENO) < 0 ||
						::dup2(output_pipe->write.get(), STDOUT_FILENO) < 0 ||
						::dup2(error_pipe->write.get(), STDERR_FILENO) < 0)
						::_exit(126);
					if ((verified->image.get() == 3
							 ? ::fcntl(verified->image.get(), F_SETFD, FD_CLOEXEC)
							 : ::dup3(verified->image.get(), 3, O_CLOEXEC)) < 0 ||
						!prepare_inherited_descriptors(invocation.inherited_channel.get()) ||
						!close_inherited_descriptors(invocation.inherited_channel.get()))
						::_exit(126);
					if (!invocation.working_directory.empty() &&
						::chdir(invocation.working_directory.c_str()) != 0)
						::_exit(125);
					if (!configure_child(invocation, *policy))
						::_exit(126);
#if defined(SYS_execveat)
					(void)::syscall(
						SYS_execveat, 3, "", arguments.data(), environment.data(), AT_EMPTY_PATH);
#endif
					::_exit(127);
				}
				detail::ng1_post_fork_process_guard child_guard{child};

				process_group_pipe->write.reset();
				const auto parent_setpgid = ::setpgid(child, child);
				const auto parent_setpgid_errno = errno;
				const auto process_group_ack =
					read_process_group_ack(process_group_pipe->read.get());
				const auto actual_process_group = ::getpgid(child);
				const bool parent_group_confirmed =
					parent_setpgid == 0 || (parent_setpgid < 0 && parent_setpgid_errno == EACCES);
				const bool process_group_established = process_group_ack && *process_group_ack &&
					parent_group_confirmed && actual_process_group == child;
				process_group_pipe->read.reset();
				if (!process_group_established)
				{
					return cxxlens::sdk::unexpected(
						process_error("provider.runtime-unavailable", "ng1-live", "process-group"));
				}
				input->read.reset();
				output_pipe->write.reset();
				error_pipe->write.reset();
				std::unique_ptr<detail::ng1_duplex_process> process =
					std::make_unique<linux_ng1_duplex_process>(std::move(input->write),
															   std::move(output_pipe->read),
															   std::move(error_pipe->read),
															   child,
															   limits,
															   std::move(*policy),
															   invocation.budget,
															   verified->digest);
				child_guard.release();
				return process;
			}

			[[nodiscard]] result<process_output>
			run(const process_invocation& invocation,
				const std::stop_token cancellation) const override
			{
				return run_with_input(
					invocation,
					[&invocation]
					{
						return make_input(invocation.standard_input);
					},
					cancellation);
			}

			[[nodiscard]] result<process_output> run_replayable(
				const process_invocation& invocation,
				const detail::replayable_provider_process_port::input_writer& write_input,
				const std::stop_token cancellation) const override
			{
				if (!invocation.standard_input.empty())
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "standard-input-mixed"));
				return run_with_input(
					invocation,
					[&write_input]
					{
						return make_replayable_input(write_input);
					},
					cancellation);
			}

		  private:
			template <typename InputFactory>
			[[nodiscard]] result<process_output>
			run_with_input(const process_invocation& invocation,
						   InputFactory&& input_factory,
						   const std::stop_token& cancellation) const
			{
				if (auto valid = invocation.budget.validate(); !valid)
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "budget"));
				if (invocation.argv.empty() || invocation.argv.front().empty() ||
					!invocation.argv.front().contains('/') ||
					std::ranges::any_of(invocation.argv, contains_nul) ||
					contains_nul(invocation.working_directory))
					return cxxlens::sdk::unexpected(
						process_error("provider.process-request-invalid", "invocation"));
				if (auto valid = invocation.sandbox.validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				auto policy = resolve_sandbox_policy(invocation.sandbox.policy_digest);
				if (!policy)
					return cxxlens::sdk::unexpected(std::move(policy.error()));
				if (invocation.inherited_channel)
				{
					if (auto valid = invocation.inherited_channel->validate(); !valid)
						return cxxlens::sdk::unexpected(std::move(valid.error()));
				}
				auto verified = make_verified_executable(invocation);
				if (!verified)
					return cxxlens::sdk::unexpected(std::move(verified.error()));
				if (invocation.expected_binary_digest.empty() ||
					verified->digest != invocation.expected_binary_digest)
					return process_output{
						process_status::launch_failed,
						0,
						0,
						{},
						"selected provider executable digest does not match its manifest",
						sandbox_evidence(*policy,
										 invocation.budget,
										 sandbox_assurance::none,
										 false,
										 verified->digest),
						"provider.binary-identity-mismatch",
						verified->digest};
				if (cancellation.stop_requested())
					return process_output{process_status::cancelled,
										  0,
										  0,
										  {},
										  {},
										  sandbox_evidence(*policy,
														   invocation.budget,
														   sandbox_assurance::none,
														   false,
														   verified->digest),
										  {},
										  verified->digest};

				auto input = std::forward<InputFactory>(input_factory)();
				auto output_pipe = make_pipe();
				auto error_pipe = make_pipe();
				auto process_group_pipe = make_pipe();
				if (!input)
					return cxxlens::sdk::unexpected(std::move(input.error()));
				if (!output_pipe)
					return cxxlens::sdk::unexpected(std::move(output_pipe.error()));
				if (!error_pipe)
					return cxxlens::sdk::unexpected(std::move(error_pipe.error()));
				if (!process_group_pipe)
					return cxxlens::sdk::unexpected(std::move(process_group_pipe.error()));

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
					return process_output{process_status::launch_failed,
										  0,
										  0,
										  {},
										  std::strerror(errno),
										  sandbox_evidence(*policy,
														   invocation.budget,
														   sandbox_assurance::none,
														   false,
														   verified->digest),
										  "provider.runtime-unavailable",
										  verified->digest};
				if (child == 0)
				{
					process_group_pipe->read.reset();
					const bool process_group_established = ::setpgid(0, 0) == 0;
					if (!write_process_group_ack(process_group_pipe->write.get(),
												 process_group_established ? std::byte{0x01}
																		   : std::byte{0x00}))
						::_exit(126);
					process_group_pipe->write.reset();
					if (!process_group_established)
						::_exit(126);
					if (::dup2(input->get(), STDIN_FILENO) < 0 ||
						::dup2(output_pipe->write.get(), STDOUT_FILENO) < 0 ||
						::dup2(error_pipe->write.get(), STDERR_FILENO) < 0)
						::_exit(126);
					if ((verified->image.get() == 3
							 ? ::fcntl(verified->image.get(), F_SETFD, FD_CLOEXEC)
							 : ::dup3(verified->image.get(), 3, O_CLOEXEC)) < 0 ||
						!prepare_inherited_descriptors(invocation.inherited_channel.get()) ||
						!close_inherited_descriptors(invocation.inherited_channel.get()))
						::_exit(126);
					if (!invocation.working_directory.empty() &&
						::chdir(invocation.working_directory.c_str()) != 0)
						::_exit(125);
					if (!configure_child(invocation, *policy))
						::_exit(126);
#if defined(SYS_execveat)
					(void)::syscall(
						SYS_execveat, 3, "", arguments.data(), environment.data(), AT_EMPTY_PATH);
#endif
					::_exit(127);
				}

				process_group_pipe->write.reset();
				const auto parent_setpgid = ::setpgid(child, child);
				const auto parent_setpgid_errno = errno;
				const auto process_group_ack =
					read_process_group_ack(process_group_pipe->read.get());
				const auto actual_process_group = ::getpgid(child);
				const bool parent_group_confirmed =
					parent_setpgid == 0 || (parent_setpgid < 0 && parent_setpgid_errno == EACCES);
				const bool process_group_established = process_group_ack && *process_group_ack &&
					parent_group_confirmed && actual_process_group == child;
				process_group_pipe->read.reset();
				if (!process_group_established)
				{
					(void)::kill(child, SIGKILL);
					int failed_status{};
					while (::waitpid(child, &failed_status, 0) < 0 && errno == EINTR)
					{
					}
					return process_output{process_status::launch_failed,
										  0,
										  0,
										  {},
										  "provider process-group setup failed",
										  sandbox_evidence(*policy,
														   invocation.budget,
														   sandbox_assurance::none,
														   false,
														   verified->digest),
										  "provider.runtime-unavailable",
										  verified->digest};
				}
				input->reset();
				output_pipe->write.reset();
				error_pipe->write.reset();
				const auto started = std::chrono::steady_clock::now();
				const auto deadline =
					started + std::chrono::milliseconds{invocation.budget.wall_ms};
				process_output output;
				output.measured_executable_digest = verified->digest;
				output.sandbox = sandbox_evidence(*policy,
												  invocation.budget,
												  sandbox_assurance::enforced,
												  true,
												  verified->digest);
				std::size_t total{};
				bool stdout_ended{};
				bool stderr_ended{};
				bool reaped{};
				int wait_status{};
				auto terminate = [&](const process_status status)
				{
					(void)::kill(-child, SIGKILL);
					if (!reaped)
					{
						// A descendant can be concurrent with the first group traversal; keep the
						// leader unreaped and retry for a bounded handoff window.
						const auto signal_deadline =
							std::chrono::steady_clock::now() + std::chrono::milliseconds{100};
						while (std::chrono::steady_clock::now() < signal_deadline)
						{
							std::this_thread::sleep_for(std::chrono::milliseconds{1});
							(void)::kill(-child, SIGKILL);
						}
					}
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
							   static_cast<std::size_t>(invocation.budget.transport_bytes),
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
							   static_cast<std::size_t>(invocation.budget.transport_bytes),
							   stderr_ended))
					{
						terminate(process_status::output_limit);
						break;
					}
					// Keep the group leader waitable while a descendant may still hold an
					// output pipe; timeout cleanup needs its stable process-group identity.
					if (!reaped && stdout_ended && stderr_ended)
					{
						const auto waited = ::waitpid(child, &wait_status, WNOHANG);
						if (waited == child)
						{
							reaped = true;
						}
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
					output.exit_code = WEXITSTATUS(wait_status);
					if (output.exit_code == 126)
					{
						output.failure_code = "security.sandbox-insufficient";
						output.sandbox = sandbox_evidence(*policy,
														  invocation.budget,
														  sandbox_assurance::none,
														  false,
														  verified->digest);
					}
					else
						output.status = process_status::exited;
				}
				return output;
			}
		};
#else
		class unavailable_process_port final : public provider_process_port,
											   public detail::replayable_provider_process_port,
											   public detail::ng1_duplex_process_port
		{
		  public:
			result<std::unique_ptr<detail::ng1_duplex_process>>
			start(const process_invocation&, protocol_limits, std::stop_token) const override
			{
				return cxxlens::sdk::unexpected(process_error(
					"provider.runtime-unavailable", "ng1-live", "linux-glibc-required"));
			}

			result<process_output> run(const process_invocation& invocation,
									   std::stop_token) const override
			{
				auto policy = resolve_sandbox_policy(invocation.sandbox.policy_digest);
				if (!policy)
					return cxxlens::sdk::unexpected(std::move(policy.error()));
				const std::vector<std::string> applied;
				return process_output{process_status::unavailable,
									  0,
									  0,
									  {},
									  "linux-glibc-required",
									  sandbox_evidence(*policy,
													   invocation.budget,
													   sandbox_assurance::none,
													   false,
													   invocation.expected_binary_digest),
									  "provider.runtime-unavailable",
									  {}};
			}

			result<process_output>
			run_replayable(const process_invocation& invocation,
						   const detail::replayable_provider_process_port::input_writer&,
						   std::stop_token cancellation) const override
			{
				return run(invocation, cancellation);
			}
		};
#endif
	} // namespace

#if !defined(CXXLENS_PROVIDER_RUNTIME_INTERNAL_ONLY)
	std::unique_ptr<provider_process_port> make_system_provider_process_port()
	{
#if defined(__linux__) && defined(__GLIBC__)
		return std::make_unique<linux_process_port>();
#else
		return std::make_unique<unavailable_process_port>();
#endif
	}
#endif

	std::unique_ptr<detail::replayable_provider_process_port>
	detail::make_system_replayable_provider_process_port()
	{
#if defined(__linux__) && defined(__GLIBC__)
		return std::make_unique<linux_process_port>();
#else
		return std::make_unique<unavailable_process_port>();
#endif
	}

	std::unique_ptr<detail::ng1_duplex_process_port> detail::make_system_ng1_duplex_process_port()
	{
#if defined(__linux__) && defined(__GLIBC__)
		return std::make_unique<linux_process_port>();
#else
		return std::make_unique<unavailable_process_port>();
#endif
	}
} // namespace cxxlens::sdk::provider
