#include "sqlite_same_process_shm_process_port_internal.hpp"

#include <array>
#include <atomic>
#include <charconv>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] sqlite_shm_lease_rejection port_rejection(
			const sqlite_shm_lease_rejection_reason reason =
				sqlite_shm_lease_rejection_reason::invalid_identity,
			const sqlite_shm_lease_recovery_action action =
				sqlite_shm_lease_recovery_action::deny_before_native_map) noexcept
		{
			return {reason, action};
		}

#if defined(__linux__) && defined(SYS_pidfd_open)
		static_assert(std::atomic<std::uint64_t>::is_always_lock_free);

		class owned_descriptor
		{
		  public:
			owned_descriptor() noexcept = default;
			explicit owned_descriptor(const int value) noexcept : value_{value} {}
			~owned_descriptor() noexcept
			{
				reset();
			}
			owned_descriptor(owned_descriptor&& other) noexcept
				: value_{std::exchange(other.value_, -1)}
			{
			}
			owned_descriptor& operator=(owned_descriptor&& other) noexcept
			{
				if (this != &other)
				{
					reset();
					value_ = std::exchange(other.value_, -1);
				}
				return *this;
			}
			owned_descriptor(const owned_descriptor&) = delete;
			owned_descriptor& operator=(const owned_descriptor&) = delete;

			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] explicit operator bool() const noexcept
			{
				return value_ >= 0;
			}

		  private:
			void reset() noexcept
			{
				if (value_ < 0)
					return;
				(void)::close(value_);
				value_ = -1;
			}

			int value_{-1};
		};

		[[nodiscard]] int open_retry(const char* path, const int flags) noexcept
		{
			for (;;)
			{
				const auto output = ::open(path, flags);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] int open_self_pidfd() noexcept
		{
			for (;;)
			{
				const auto output = ::syscall(SYS_pidfd_open, ::getpid(), 0U);
				if (output >= 0 && output <= static_cast<long>(std::numeric_limits<int>::max()))
					return static_cast<int>(output);
				if (output < 0 && errno == EINTR)
					continue;
				if (output >= 0)
					(void)::close(static_cast<int>(output));
				return -1;
			}
		}

		[[nodiscard]] bool pidfd_live(const int descriptor) noexcept
		{
			if (descriptor < 0)
				return false;
			pollfd observation{descriptor, POLLIN, 0};
			for (;;)
			{
				const auto status = ::poll(&observation, 1U, 0);
				if (status == 0)
					return observation.revents == 0;
				if (status > 0)
					return false;
				if (errno != EINTR)
					return false;
			}
		}

		[[nodiscard]] bool read_process_stat(std::string& output) noexcept
		{
			owned_descriptor descriptor{open_retry("/proc/self/stat", O_RDONLY | O_CLOEXEC)};
			if (!descriptor)
				return false;
			std::array<char, 4096> buffer{};
			std::size_t size{};
			for (;;)
			{
				if (size == buffer.size())
					return false;
				const auto count = ::read(descriptor.get(), buffer.data() + size, buffer.size() - size);
				if (count > 0)
				{
					size += static_cast<std::size_t>(count);
					continue;
				}
				if (count == 0)
					break;
				if (errno != EINTR)
					return false;
			}
			try
			{
				output.assign(buffer.data(), size);
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] bool process_start_ticks(std::uint64_t& output) noexcept
		{
			std::string stat;
			if (!read_process_stat(stat))
				return false;
			const auto close = stat.rfind(')');
			if (close == std::string::npos || close + 2U >= stat.size())
				return false;
			std::string_view fields{stat.data() + close + 2U, stat.size() - close - 2U};
			for (std::size_t index = 0; index <= 19U; ++index)
			{
				while (!fields.empty() && fields.front() == ' ')
					fields.remove_prefix(1U);
				if (fields.empty())
					return false;
				const auto separator = fields.find(' ');
				const auto token = fields.substr(0U, separator);
				if (index == 19U)
				{
					std::uint64_t value{};
					const auto parsed = std::from_chars(
						token.data(), token.data() + token.size(), value, 10);
					if (parsed.ec != std::errc{} || parsed.ptr != token.data() + token.size() ||
						value == 0U)
						return false;
					output = value;
					return true;
				}
				if (separator == std::string_view::npos)
					return false;
				fields.remove_prefix(separator + 1U);
			}
			return false;
		}

		template <class Value>
		void append_unsigned(std::vector<std::byte>& bytes, const Value value)
		{
			static_assert(std::is_unsigned_v<Value>);
			for (auto shift = static_cast<unsigned>((sizeof(Value) - 1U) * 8U);; shift -= 8U)
			{
				bytes.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		[[nodiscard]] bool fill_entropy(std::span<std::byte> output) noexcept
		{
			std::size_t offset{};
			while (offset < output.size())
			{
				const auto count = ::getrandom(output.data() + offset, output.size() - offset, 0U);
				if (count > 0)
				{
					offset += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return false;
			}
			return true;
		}

		[[nodiscard]] bool descriptor_identity(const int descriptor,
											std::uint64_t& device,
											std::uint64_t& inode) noexcept
		{
			struct stat observed{};
			for (;;)
			{
				if (::fstat(descriptor, &observed) == 0)
					break;
				if (errno != EINTR)
					return false;
			}
			device = static_cast<std::uint64_t>(observed.st_dev);
			inode = static_cast<std::uint64_t>(observed.st_ino);
			return device != 0U || inode != 0U;
		}
#endif
	} // namespace

	namespace detail
	{
		struct sqlite_shm_process_registry_port_state
		{
#if defined(__linux__) && defined(SYS_pidfd_open)
			pid_t creator_pid{};
			std::uint64_t fork_epoch{};
			owned_descriptor pid_namespace;
			owned_descriptor pidfd;
#endif
			sqlite_backend_opaque_identity process_instance;
			std::shared_ptr<sqlite_same_process_shm_mapping_registry> registry;

			[[nodiscard]] static sqlite_shm_registry_process_owner
			mint_process_owner(sqlite_backend_opaque_identity process_instance)
			{
				return sqlite_shm_registry_process_owner{std::move(process_instance)};
			}

			void invalidate_inherited_registry() noexcept
			{
				if (registry)
					registry->invalidate_process_instance_from_process_port();
			}

			[[nodiscard]] bool current_process() const noexcept
			{
#if defined(__linux__) && defined(SYS_pidfd_open)
				return creator_pid > 0 && fork_epoch != 0U && ::getpid() == creator_pid &&
					pid_namespace && pidfd && pidfd_live(pidfd.get()) && registry &&
					registry->process_instance_live_from_process_port();
#else
				return false;
#endif
			}
		};
	} // namespace detail

	namespace
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		struct process_port_globals
		{
			std::mutex mutex;
			std::shared_ptr<detail::sqlite_shm_process_registry_port_state> state;
			std::uint64_t fork_epoch{1U};
			bool atfork_registered{};
			bool child_after_fork{};
			bool exhausted{};
		};

		process_port_globals globals;

		[[nodiscard]] sqlite_shm_lease_rejection quarantine_process_port_locked() noexcept
		{
			globals.exhausted = true;
			return port_rejection(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}

		struct process_state_deleter
		{
			pid_t creator_pid{};

			void operator()(detail::sqlite_shm_process_registry_port_state* state) const noexcept
			{
				if (::getpid() == creator_pid)
					delete state;
			}
		};

		void atfork_prepare() noexcept
		{
			globals.mutex.lock();
		}

		void atfork_parent() noexcept
		{
			globals.mutex.unlock();
		}

		void atfork_child() noexcept
		{
			if (globals.state)
				globals.state->invalidate_inherited_registry();
			globals.child_after_fork = true;
			if (globals.fork_epoch == std::numeric_limits<std::uint64_t>::max())
				globals.exhausted = true;
			else
			{
				++globals.fork_epoch;
				globals.exhausted = false;
			}
			globals.mutex.unlock();
		}

		[[nodiscard]] bool ensure_atfork_registered_locked() noexcept
		{
			if (globals.atfork_registered)
				return true;
			if (::pthread_atfork(&atfork_prepare, &atfork_parent, &atfork_child) != 0)
				return false;
			globals.atfork_registered = true;
			return true;
		}

		[[nodiscard]] sqlite_shm_lease_result<
			std::shared_ptr<detail::sqlite_shm_process_registry_port_state>>
		create_process_state_locked()
		{
			if (globals.exhausted || globals.fork_epoch == 0U)
				return port_rejection(sqlite_shm_lease_rejection_reason::generation_exhausted,
								  sqlite_shm_lease_recovery_action::quarantine_no_retry);

			const auto creator_pid = ::getpid();
			if (creator_pid <= 0)
				return port_rejection();
			owned_descriptor pid_namespace{
				open_retry("/proc/self/ns/pid", O_RDONLY | O_CLOEXEC)};
			owned_descriptor pidfd{open_self_pidfd()};
			if (!pid_namespace || !pidfd || !pidfd_live(pidfd.get()))
				return port_rejection();

			std::uint64_t namespace_device{};
			std::uint64_t namespace_inode{};
			std::uint64_t pidfd_device{};
			std::uint64_t pidfd_inode{};
			std::uint64_t start_ticks{};
			std::array<std::byte, 32> entropy{};
			if (!descriptor_identity(
					pid_namespace.get(), namespace_device, namespace_inode) ||
				!descriptor_identity(pidfd.get(), pidfd_device, pidfd_inode) ||
				!process_start_ticks(start_ticks) || !fill_entropy(entropy) ||
				::getpid() != creator_pid || !pidfd_live(pidfd.get()))
				return port_rejection();

			sqlite_backend_opaque_identity process_instance;
			try
			{
				process_instance.profile = "cxxlens.sqlite.process-instance.v1";
				process_instance.bytes.reserve(8U * 8U + entropy.size());
				append_unsigned(process_instance.bytes, std::uint64_t{1U});
				append_unsigned(
					process_instance.bytes, static_cast<std::uint64_t>(creator_pid));
				append_unsigned(process_instance.bytes, start_ticks);
				append_unsigned(process_instance.bytes, namespace_device);
				append_unsigned(process_instance.bytes, namespace_inode);
				append_unsigned(process_instance.bytes, pidfd_device);
				append_unsigned(process_instance.bytes, pidfd_inode);
				append_unsigned(process_instance.bytes, globals.fork_epoch);
				process_instance.bytes.insert(
					process_instance.bytes.end(), entropy.begin(), entropy.end());
			}
			catch (...)
			{
				return quarantine_process_port_locked();
			}

			std::unique_ptr<detail::sqlite_shm_process_registry_port_state, process_state_deleter>
				pending_state{new (std::nothrow) detail::sqlite_shm_process_registry_port_state,
							  process_state_deleter{creator_pid}};
			if (!pending_state)
				return quarantine_process_port_locked();

			std::shared_ptr<detail::sqlite_shm_process_registry_port_state> state;
			try
			{
				state = std::shared_ptr<detail::sqlite_shm_process_registry_port_state>{
					std::move(pending_state)};
			}
			catch (...)
			{
				return quarantine_process_port_locked();
			}

			state->creator_pid = creator_pid;
			state->fork_epoch = globals.fork_epoch;
			state->pid_namespace = std::move(pid_namespace);
			state->pidfd = std::move(pidfd);

			try
			{
				auto owner = detail::sqlite_shm_process_registry_port_state::mint_process_owner(
					process_instance);
				auto created = sqlite_same_process_shm_mapping_registry::create(std::move(owner));
				if (!created)
					return quarantine_process_port_locked();
				state->process_instance = std::move(process_instance);
				state->registry = std::shared_ptr<sqlite_same_process_shm_mapping_registry>{
					std::move(created.value())};
			}
			catch (...)
			{
				return quarantine_process_port_locked();
			}

			return state;
		}
#endif
	} // namespace

	sqlite_shm_process_registry_handle::sqlite_shm_process_registry_handle(
		std::shared_ptr<detail::sqlite_shm_process_registry_port_state> state) noexcept
		: state_{std::move(state)}
	{
	}

	bool sqlite_shm_process_registry_handle::valid() const noexcept
	{
		return state_ && state_->current_process();
	}

	const sqlite_backend_opaque_identity&
	sqlite_shm_process_registry_handle::process_instance() const noexcept
	{
		static const sqlite_backend_opaque_identity invalid;
		return valid() ? state_->process_instance : invalid;
	}

	sqlite_same_process_shm_mapping_registry*
	sqlite_shm_process_registry_handle::registry() const noexcept
	{
		return valid() ? state_->registry.get() : nullptr;
	}

	sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
	sqlite_shm_process_registry_handle::adopt_runtime_lifetime(
		sqlite_backend_opaque_identity identity,
		sqlite_backend_opaque_identity pin_identity,
		std::shared_ptr<void> owner) const
	{
		if (!valid())
			return port_rejection(sqlite_shm_lease_rejection_reason::stale_token);
		return state_->registry->adopt_runtime_lifetime_from_process_port(
			std::move(identity), std::move(pin_identity), std::move(owner));
	}

	sqlite_shm_lease_result<sqlite_shm_process_registry_handle>
	sqlite_same_process_shm_process_port::acquire()
	{
#if defined(__linux__) && defined(SYS_pidfd_open)
		std::scoped_lock lock{globals.mutex};
		if (!ensure_atfork_registered_locked())
			return port_rejection();

		const auto current_pid = ::getpid();
		if (globals.state &&
			(globals.child_after_fork || globals.state->creator_pid != current_pid))
		{
			globals.state->registry->invalidate_process_instance_from_process_port();
			globals.state.reset();
		}
		globals.child_after_fork = false;
		if (globals.state && !globals.state->current_process())
		{
			globals.state->registry->invalidate_process_instance_from_process_port();
			return quarantine_process_port_locked();
		}

		if (!globals.state)
		{
			auto created = create_process_state_locked();
			if (!created)
				return created.error();
			globals.state = std::move(created.value());
		}
		if (!globals.state->current_process())
		{
			globals.state->registry->invalidate_process_instance_from_process_port();
			return quarantine_process_port_locked();
		}
		return sqlite_shm_process_registry_handle{globals.state};
#else
		return port_rejection();
#endif
	}
} // namespace cxxlens::sdk
