#include "source_closure_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cxxlens::detail::clang22
{
	namespace
	{
		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string errno_detail(const int value)
		{
			return std::to_string(value);
		}

#if defined(__unix__) || defined(__APPLE__)
		constexpr std::uint64_t poll_slice_nanoseconds = 25'000'000ULL;

		struct progress_deadline
		{
			std::uint64_t last_observed_ns{};
			std::uint64_t expires_at_ns{};
			bool enabled{};
		};

		[[nodiscard]] sdk::result<std::uint64_t>
		observe_clock(const source_closure_monotonic_clock& clock, const std::string_view field)
		{
			auto current = clock.now_ns();
			if (!current)
				return sdk::unexpected(failure("source-closure.channel-clock-invalid",
											   std::string(field),
											   current.error().detail));
			return *current;
		}

		[[nodiscard]] sdk::result<progress_deadline>
		start_progress_deadline(const source_closure_monotonic_clock& clock,
								const std::uint64_t timeout_ns,
								const std::string_view field)
		{
			auto started = observe_clock(clock, field);
			if (!started)
				return sdk::unexpected(std::move(started.error()));
			progress_deadline result{*started, 0U, timeout_ns != 0U};
			if (!result.enabled)
				return result;
			if (*started > std::numeric_limits<std::uint64_t>::max() - timeout_ns)
				return sdk::unexpected(failure("source-closure.channel-clock-invalid",
											   std::string(field),
											   "deadline-overflow"));
			result.expires_at_ns = *started + timeout_ns;
			return result;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		observe_progress_deadline(const source_closure_monotonic_clock& clock,
								  progress_deadline& deadline,
								  const std::string_view field)
		{
			auto current = observe_clock(clock, field);
			if (!current)
				return sdk::unexpected(std::move(current.error()));
			if (*current < deadline.last_observed_ns)
				return sdk::unexpected(failure(
					"source-closure.channel-clock-invalid", std::string(field), "backwards"));
			deadline.last_observed_ns = *current;
			if (deadline.enabled && *current >= deadline.expires_at_ns)
				return sdk::unexpected(failure(
					"source-closure.channel-timeout", std::string(field), "progress-deadline"));
			return *current;
		}

		[[nodiscard]] sdk::result<void>
		restart_progress_deadline(const source_closure_monotonic_clock& clock,
								  progress_deadline& deadline,
								  const std::uint64_t timeout_ns,
								  const std::string_view field)
		{
			auto current = observe_progress_deadline(clock, deadline, field);
			if (!current)
				return sdk::unexpected(std::move(current.error()));
			deadline.enabled = timeout_ns != 0U;
			if (!deadline.enabled)
			{
				deadline.expires_at_ns = 0U;
				return {};
			}
			if (*current > std::numeric_limits<std::uint64_t>::max() - timeout_ns)
				return sdk::unexpected(failure("source-closure.channel-clock-invalid",
											   std::string(field),
											   "deadline-overflow"));
			deadline.expires_at_ns = *current + timeout_ns;
			return {};
		}

		struct descriptor_identity
		{
			std::uint64_t device{};
			std::uint64_t inode{};
			std::uint32_t mode{};
		};

		struct write_outcome
		{
			ssize_t count{-1};
			int error{};
		};

		enum class descriptor_stage
		{
			inherited,
			private_pinned,
		};

		class input_descriptor_custody final
		{
		  public:
			explicit input_descriptor_custody(
				const source_closure_fd_channel_options& options) noexcept
				: read_{options.read.value}, write_{options.write.value},
				  close_read_{options.read.ownership == source_closure_fd_ownership::owned},
				  close_write_{options.write.ownership == source_closure_fd_ownership::owned}
			{
			}

			input_descriptor_custody(const input_descriptor_custody&) = delete;
			input_descriptor_custody& operator=(const input_descriptor_custody&) = delete;

			~input_descriptor_custody()
			{
				if (read_ == write_)
				{
					if (read_ >= 0 && (close_read_ || close_write_))
						(void)::close(read_);
					return;
				}
				if (read_ >= 0 && close_read_)
					(void)::close(read_);
				if (write_ >= 0 && close_write_)
					(void)::close(write_);
			}

		  private:
			int read_{-1};
			int write_{-1};
			bool close_read_{false};
			bool close_write_{false};
		};

		class pinned_descriptor final
		{
		  public:
			pinned_descriptor() = default;
			explicit pinned_descriptor(const int value) noexcept : value_{value} {}
			pinned_descriptor(const pinned_descriptor&) = delete;
			pinned_descriptor& operator=(const pinned_descriptor&) = delete;
			pinned_descriptor(pinned_descriptor&& other) noexcept : value_{other.release()} {}
			pinned_descriptor& operator=(pinned_descriptor&& other) noexcept
			{
				if (this == &other)
					return *this;
				if (value_ >= 0)
					(void)::close(value_);
				value_ = other.release();
				return *this;
			}
			~pinned_descriptor()
			{
				if (value_ >= 0)
					(void)::close(value_);
			}

			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] int release() noexcept
			{
				const auto result = value_;
				value_ = -1;
				return result;
			}

		  private:
			int value_{-1};
		};

		/** Write without changing process-wide or thread-local SIGPIPE state. */
		[[nodiscard]] write_outcome
		write_without_sigpipe(const int descriptor, const void* bytes, const std::size_t size)
		{
			errno = 0;
			iovec vector{const_cast<void*>(bytes), size};
			msghdr message{};
			message.msg_iov = &vector;
			message.msg_iovlen = 1U;
#if defined(MSG_NOSIGNAL)
			const auto count = ::sendmsg(descriptor, &message, MSG_NOSIGNAL);
#else
			// SO_NOSIGPIPE is installed on the private write duplicate during admission.
			const auto count = ::sendmsg(descriptor, &message, 0);
#endif
			return write_outcome{count, count < 0 ? errno : 0};
		}

		[[nodiscard]] sdk::result<descriptor_identity>
		validate_descriptor(const source_closure_fd_descriptor endpoint,
							const std::string_view field,
							const bool read_endpoint,
							const descriptor_stage stage = descriptor_stage::inherited)
		{
			if (endpoint.value < source_closure_first_inherited_descriptor)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "reserved-descriptor"));

			const auto descriptor_flags = ::fcntl(endpoint.value, F_GETFD);
			if (descriptor_flags < 0)
				return sdk::unexpected(failure("source-closure.channel-invalid",
											   std::string(field),
											   "descriptor-" + errno_detail(errno)));
			if (stage == descriptor_stage::inherited && (descriptor_flags & FD_CLOEXEC) != 0)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "close-on-exec-set"));
			if (stage == descriptor_stage::private_pinned && (descriptor_flags & FD_CLOEXEC) == 0)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "close-on-exec-clear"));

			const auto status_flags = ::fcntl(endpoint.value, F_GETFL);
			if (status_flags < 0)
				return sdk::unexpected(failure("source-closure.channel-invalid",
											   std::string(field),
											   "status-" + errno_detail(errno)));
			if ((status_flags & O_NONBLOCK) == 0)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "blocking-descriptor"));

			const auto access_mode = status_flags & O_ACCMODE;
			if (read_endpoint && access_mode == O_WRONLY)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", std::string(field), "not-readable"));
			if (!read_endpoint && access_mode == O_RDONLY)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", std::string(field), "not-writable"));

			struct stat metadata{};
			if (::fstat(endpoint.value, &metadata) != 0)
				return sdk::unexpected(failure("source-closure.channel-invalid",
											   std::string(field),
											   "stat-" + errno_detail(errno)));
			if (!S_ISSOCK(metadata.st_mode))
				return sdk::unexpected(
					failure("source-closure.channel-invalid", std::string(field), "not-socket"));
			return descriptor_identity{static_cast<std::uint64_t>(metadata.st_dev),
									   static_cast<std::uint64_t>(metadata.st_ino),
									   static_cast<std::uint32_t>(metadata.st_mode)};
		}

		[[nodiscard]] sdk::result<pinned_descriptor>
		pin_descriptor(const source_closure_fd_descriptor endpoint,
					   const descriptor_identity expected,
					   const std::string_view field,
					   const bool read_endpoint)
		{
			int duplicate{-1};
#if defined(F_DUPFD_CLOEXEC)
			duplicate =
				::fcntl(endpoint.value, F_DUPFD_CLOEXEC, source_closure_first_inherited_descriptor);
#else
			return sdk::unexpected(
				failure("source-closure.channel-unavailable", std::string(field), "atomic-pin"));
#endif
			if (duplicate < 0)
				return sdk::unexpected(failure("source-closure.channel-invalid",
											   std::string(field),
											   "pin-" + errno_detail(errno)));

			pinned_descriptor pinned{duplicate};
			auto actual = validate_descriptor({pinned.get(), source_closure_fd_ownership::borrowed},
											  field,
											  read_endpoint,
											  descriptor_stage::private_pinned);
			if (!actual)
				return sdk::unexpected(std::move(actual.error()));
			if (actual->device != expected.device || actual->inode != expected.inode ||
				actual->mode != expected.mode)
				return sdk::unexpected(
					failure("source-closure.channel-foreign", std::string(field), "pin-identity"));
			if (!read_endpoint)
			{
#if !defined(MSG_NOSIGNAL) && defined(SO_NOSIGPIPE)
				constexpr int enabled = 1;
				if (::setsockopt(
						pinned.get(), SOL_SOCKET, SO_NOSIGPIPE, &enabled, sizeof(enabled)) != 0)
					return sdk::unexpected(failure("source-closure.channel-invalid",
												   std::string(field),
												   "no-sigpipe-" + errno_detail(errno)));
#elif !defined(MSG_NOSIGNAL)
				return sdk::unexpected(failure(
					"source-closure.channel-unavailable", std::string(field), "no-sigpipe"));
#endif
			}
			return pinned;
		}

		[[nodiscard]] sdk::result<short> wait_for(const int descriptor,
												  const short events,
												  const std::stop_token cancellation,
												  const source_closure_monotonic_clock& clock,
												  progress_deadline& deadline,
												  const std::string_view field)
		{
			for (;;)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(failure(
						"source-closure.channel-cancelled", std::string(field), "stop-requested"));
				auto current = observe_progress_deadline(clock, deadline, field);
				if (!current)
					return sdk::unexpected(std::move(current.error()));

				int poll_milliseconds = 25;
				if (deadline.enabled)
				{
					const auto remaining = deadline.expires_at_ns - *current;
					const auto nanos = std::min(remaining, poll_slice_nanoseconds);
					poll_milliseconds =
						static_cast<int>(std::max<std::uint64_t>(1U, nanos / 1'000'000ULL));
				}
				pollfd descriptor_poll{descriptor, events, 0};
				const auto outcome = ::poll(&descriptor_poll, 1, poll_milliseconds);
				if (outcome < 0)
				{
					if (errno == EINTR)
						continue;
					return sdk::unexpected(failure("source-closure.channel-io",
												   std::string(field),
												   "poll-" + errno_detail(errno)));
				}
				if (outcome == 0)
					continue;
				if ((descriptor_poll.revents & POLLNVAL) != 0)
					return sdk::unexpected(failure(
						"source-closure.channel-invalid", std::string(field), "poll-invalid"));
				return descriptor_poll.revents;
			}
		}
#endif
	} // namespace

	source_closure_fd_channel::source_closure_fd_channel(source_closure_fd_channel_options options,
														 const int pinned_read,
														 const int pinned_write,
														 const std::uint64_t read_device,
														 const std::uint64_t read_inode,
														 const std::uint32_t read_mode,
														 const std::uint64_t write_device,
														 const std::uint64_t write_inode,
														 const std::uint32_t write_mode) noexcept
		: read_descriptor_{pinned_read}, write_descriptor_{pinned_write}, read_device_{read_device},
		  read_inode_{read_inode}, read_mode_{read_mode}, write_device_{write_device},
		  write_inode_{write_inode}, write_mode_{write_mode}, cancellation_{options.cancellation},
		  injected_clock_{options.clock}, progress_timeout_ns_{options.progress_timeout_ns}
	{
	}

	sdk::result<source_closure_fd_channel>
	source_closure_fd_channel::create(source_closure_fd_channel_options options)
	{
#if defined(__unix__) || defined(__APPLE__)
		input_descriptor_custody input_custody{options};
		if (options.read.value == options.write.value)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "descriptor", "duplicate"));
		auto read = validate_descriptor(options.read, "read", true);
		if (!read)
			return sdk::unexpected(std::move(read.error()));
		auto write = validate_descriptor(options.write, "write", false);
		if (!write)
			return sdk::unexpected(std::move(write.error()));
		if (read->device == write->device && read->inode == write->inode &&
			read->mode == write->mode)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "descriptor", "same-physical-endpoint"));
		auto pinned_read = pin_descriptor(options.read, *read, "read", true);
		if (!pinned_read)
			return sdk::unexpected(std::move(pinned_read.error()));
		auto pinned_write = pin_descriptor(options.write, *write, "write", false);
		if (!pinned_write)
			return sdk::unexpected(std::move(pinned_write.error()));
		return source_closure_fd_channel{std::move(options),
										 pinned_read->release(),
										 pinned_write->release(),
										 read->device,
										 read->inode,
										 read->mode,
										 write->device,
										 write->inode,
										 write->mode};
#else
		(void)options;
		return sdk::unexpected(
			failure("source-closure.channel-unavailable", "platform", "posix-required"));
#endif
	}

	source_closure_fd_channel::source_closure_fd_channel(source_closure_fd_channel&& other) noexcept
		: read_descriptor_{other.read_descriptor_}, write_descriptor_{other.write_descriptor_},
		  read_device_{other.read_device_}, read_inode_{other.read_inode_},
		  read_mode_{other.read_mode_}, write_device_{other.write_device_},
		  write_inode_{other.write_inode_}, write_mode_{other.write_mode_},
		  poisoned_{other.poisoned_}, cancellation_{other.cancellation_},
		  injected_clock_{other.injected_clock_}, progress_timeout_ns_{other.progress_timeout_ns_}
	{
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.read_device_ = 0U;
		other.read_inode_ = 0U;
		other.read_mode_ = 0U;
		other.write_device_ = 0U;
		other.write_inode_ = 0U;
		other.write_mode_ = 0U;
		other.poisoned_ = true;
		other.injected_clock_ = nullptr;
		other.progress_timeout_ns_ = source_closure_default_progress_timeout_ns;
	}

	source_closure_fd_channel&
	source_closure_fd_channel::operator=(source_closure_fd_channel&& other) noexcept
	{
		if (this == &other)
			return *this;
		close_pinned();
		read_descriptor_ = other.read_descriptor_;
		write_descriptor_ = other.write_descriptor_;
		read_device_ = other.read_device_;
		read_inode_ = other.read_inode_;
		read_mode_ = other.read_mode_;
		write_device_ = other.write_device_;
		write_inode_ = other.write_inode_;
		write_mode_ = other.write_mode_;
		poisoned_ = other.poisoned_;
		cancellation_ = other.cancellation_;
		injected_clock_ = other.injected_clock_;
		progress_timeout_ns_ = other.progress_timeout_ns_;
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.read_device_ = 0U;
		other.read_inode_ = 0U;
		other.read_mode_ = 0U;
		other.write_device_ = 0U;
		other.write_inode_ = 0U;
		other.write_mode_ = 0U;
		other.poisoned_ = true;
		other.injected_clock_ = nullptr;
		other.progress_timeout_ns_ = source_closure_default_progress_timeout_ns;
		return *this;
	}

	source_closure_fd_channel::~source_closure_fd_channel()
	{
		close_pinned();
	}

	void source_closure_fd_channel::close_pinned() noexcept
	{
#if defined(__unix__) || defined(__APPLE__)
		if (read_descriptor_ >= 0)
			(void)::close(read_descriptor_);
		if (write_descriptor_ >= 0 && write_descriptor_ != read_descriptor_)
			(void)::close(write_descriptor_);
#endif
		read_descriptor_ = -1;
		write_descriptor_ = -1;
		read_device_ = 0U;
		read_inode_ = 0U;
		read_mode_ = 0U;
		write_device_ = 0U;
		write_inode_ = 0U;
		write_mode_ = 0U;
		poisoned_ = true;
	}

	const source_closure_monotonic_clock& source_closure_fd_channel::clock() const noexcept
	{
		if (injected_clock_ != nullptr)
			return *injected_clock_;
		return system_clock_;
	}

	sdk::result<std::size_t> source_closure_fd_channel::read(const std::span<std::byte> destination)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (poisoned_)
			return sdk::unexpected(
				failure("source-closure.channel-poisoned", "read", "partial-write"));
#endif
		if (destination.empty())
			return std::size_t{};
#if defined(__unix__) || defined(__APPLE__)
		if (read_descriptor_ < 0)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "read", "descriptor-unset"));
		auto identity =
			validate_descriptor({read_descriptor_, source_closure_fd_ownership::borrowed},
								"read",
								true,
								descriptor_stage::private_pinned);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		if (identity->device != read_device_ || identity->inode != read_inode_ ||
			identity->mode != read_mode_)
			return sdk::unexpected(
				failure("source-closure.channel-foreign", "read", "descriptor-identity"));
		auto deadline = start_progress_deadline(clock(), progress_timeout_ns_, "read");
		if (!deadline)
			return sdk::unexpected(std::move(deadline.error()));
		for (;;)
		{
			auto ready =
				wait_for(read_descriptor_, POLLIN, cancellation_, clock(), *deadline, "read");
			if (!ready)
				return sdk::unexpected(std::move(ready.error()));
			if ((*ready & POLLERR) != 0 && (*ready & (POLLIN | POLLHUP)) == 0)
				return sdk::unexpected(failure("source-closure.channel-io", "read", "poll-error"));

			const auto maximum = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
			const auto count =
				::read(read_descriptor_, destination.data(), std::min(destination.size(), maximum));
			if (count >= 0)
				return static_cast<std::size_t>(count);
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (errno == EBADF)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", "read", "descriptor-closed"));
			return sdk::unexpected(
				failure("source-closure.channel-io", "read", "read-" + errno_detail(errno)));
		}
#else
		(void)destination;
		return sdk::unexpected(
			failure("source-closure.channel-unavailable", "platform", "posix-required"));
#endif
	}

	sdk::result<void> source_closure_fd_channel::write(const std::span<const std::byte> frame_bytes)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (poisoned_)
			return sdk::unexpected(
				failure("source-closure.channel-poisoned", "write", "partial-write"));
#endif
		if (frame_bytes.empty())
			return {};
#if defined(__unix__) || defined(__APPLE__)
		if (write_descriptor_ < 0)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "write", "descriptor-unset"));
		auto identity =
			validate_descriptor({write_descriptor_, source_closure_fd_ownership::borrowed},
								"write",
								false,
								descriptor_stage::private_pinned);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		if (identity->device != write_device_ || identity->inode != write_inode_ ||
			identity->mode != write_mode_)
			return sdk::unexpected(
				failure("source-closure.channel-foreign", "write", "descriptor-identity"));
		auto deadline = start_progress_deadline(clock(), progress_timeout_ns_, "write");
		if (!deadline)
			return sdk::unexpected(std::move(deadline.error()));
		std::size_t offset{};
		auto reject_after_progress = [&](sdk::error error) -> sdk::result<void>
		{
			if (offset != 0U)
				poisoned_ = true;
			return sdk::unexpected(std::move(error));
		};
		while (offset < frame_bytes.size())
		{
			auto ready =
				wait_for(write_descriptor_, POLLOUT, cancellation_, clock(), *deadline, "write");
			if (!ready)
				return reject_after_progress(std::move(ready.error()));
			if ((*ready & (POLLERR | POLLHUP)) != 0 && (*ready & POLLOUT) == 0)
				return reject_after_progress(
					failure("source-closure.channel-closed", "write", "peer-closed"));

			const auto maximum = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
			const auto written =
				write_without_sigpipe(write_descriptor_,
									  frame_bytes.data() + offset,
									  std::min(frame_bytes.size() - offset, maximum));
			const auto count = written.count;
			if (count > 0)
			{
				offset += static_cast<std::size_t>(count);
				if (offset < frame_bytes.size())
				{
					auto restarted = restart_progress_deadline(
						clock(), *deadline, progress_timeout_ns_, "write");
					if (!restarted)
						return reject_after_progress(std::move(restarted.error()));
				}
				continue;
			}
			if (count == 0)
				return reject_after_progress(
					failure("source-closure.channel-closed", "write", "zero-progress"));
			if (written.error == EINTR || written.error == EAGAIN || written.error == EWOULDBLOCK)
				continue;
			if (written.error == EBADF)
				return reject_after_progress(
					failure("source-closure.channel-invalid", "write", "descriptor-closed"));
			if (written.error == EPIPE || written.error == ECONNRESET)
				return reject_after_progress(
					failure("source-closure.channel-closed", "write", "peer-closed"));
			return reject_after_progress(failure(
				"source-closure.channel-io", "write", "write-" + errno_detail(written.error)));
		}
		return {};
#else
		(void)frame_bytes;
		return sdk::unexpected(
			failure("source-closure.channel-unavailable", "platform", "posix-required"));
#endif
	}
} // namespace cxxlens::detail::clang22
