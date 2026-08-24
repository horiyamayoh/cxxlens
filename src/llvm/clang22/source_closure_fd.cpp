#include "source_closure_fd.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
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

		[[nodiscard]] sdk::result<std::uint64_t> now_ns(const source_closure_monotonic_clock* clock)
		{
			if (clock != nullptr)
				return clock->now_ns();
			const auto now = std::chrono::steady_clock::now().time_since_epoch();
			const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
			if (count < 0)
				return sdk::unexpected(
					failure("source-closure.channel-clock-invalid", "clock", "negative"));
			return static_cast<std::uint64_t>(count);
		}

		[[nodiscard]] sdk::result<void>
		validate_descriptor(const source_closure_fd_descriptor endpoint,
							const std::string_view field,
							const bool read_endpoint)
		{
			if (endpoint.value < source_closure_first_inherited_descriptor)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "reserved-descriptor"));

			const auto descriptor_flags = ::fcntl(endpoint.value, F_GETFD);
			if (descriptor_flags < 0)
				return sdk::unexpected(failure("source-closure.channel-invalid",
											   std::string(field),
											   "descriptor-" + errno_detail(errno)));
			if ((descriptor_flags & FD_CLOEXEC) != 0)
				return sdk::unexpected(failure(
					"source-closure.channel-invalid", std::string(field), "close-on-exec-set"));

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
			return {};
		}

		[[nodiscard]] sdk::result<short> wait_for(const int descriptor,
												  const short events,
												  const std::stop_token cancellation,
												  const source_closure_monotonic_clock* clock,
												  const std::uint64_t timeout_ns,
												  const std::string_view field)
		{
			const auto started = now_ns(clock);
			if (!started)
				return sdk::unexpected(failure("source-closure.channel-clock-invalid",
											   std::string(field),
											   started.error().detail));
			for (;;)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(failure(
						"source-closure.channel-cancelled", std::string(field), "stop-requested"));
				const auto current = now_ns(clock);
				if (!current)
					return sdk::unexpected(failure("source-closure.channel-clock-invalid",
												   std::string(field),
												   current.error().detail));
				if (timeout_ns != 0U)
				{
					if (*current < *started)
						return sdk::unexpected(failure("source-closure.channel-clock-invalid",
													   std::string(field),
													   "backwards"));
					if (*current - *started >= timeout_ns)
						return sdk::unexpected(failure("source-closure.channel-timeout",
													   std::string(field),
													   "progress-deadline"));
				}

				int poll_milliseconds = 25;
				if (timeout_ns != 0U)
				{
					const auto elapsed = *current - *started;
					const auto remaining = timeout_ns - std::min(timeout_ns, elapsed);
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

	source_closure_fd_channel::source_closure_fd_channel(
		source_closure_fd_channel_options options) noexcept
		: read_descriptor_{options.read.value}, write_descriptor_{options.write.value},
		  owns_read_{options.read.ownership == source_closure_fd_ownership::owned},
		  owns_write_{options.write.ownership == source_closure_fd_ownership::owned},
		  cancellation_{options.cancellation}, clock_{options.clock},
		  progress_timeout_ns_{options.progress_timeout_ns}
	{
	}

	sdk::result<source_closure_fd_channel>
	source_closure_fd_channel::create(source_closure_fd_channel_options options)
	{
#if defined(__unix__) || defined(__APPLE__)
		if (auto valid = validate_descriptor(options.read, "read", true); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_descriptor(options.write, "write", false); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return source_closure_fd_channel{std::move(options)};
#else
		(void)options;
		return sdk::unexpected(
			failure("source-closure.channel-unavailable", "platform", "posix-required"));
#endif
	}

	source_closure_fd_channel::source_closure_fd_channel(source_closure_fd_channel&& other) noexcept
		: read_descriptor_{other.read_descriptor_}, write_descriptor_{other.write_descriptor_},
		  owns_read_{other.owns_read_}, owns_write_{other.owns_write_},
		  cancellation_{other.cancellation_}, clock_{other.clock_},
		  progress_timeout_ns_{other.progress_timeout_ns_}
	{
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.owns_read_ = false;
		other.owns_write_ = false;
		other.clock_ = nullptr;
		other.progress_timeout_ns_ = source_closure_default_progress_timeout_ns;
	}

	source_closure_fd_channel&
	source_closure_fd_channel::operator=(source_closure_fd_channel&& other) noexcept
	{
		if (this == &other)
			return *this;
		close_owned();
		read_descriptor_ = other.read_descriptor_;
		write_descriptor_ = other.write_descriptor_;
		owns_read_ = other.owns_read_;
		owns_write_ = other.owns_write_;
		cancellation_ = other.cancellation_;
		clock_ = other.clock_;
		progress_timeout_ns_ = other.progress_timeout_ns_;
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.owns_read_ = false;
		other.owns_write_ = false;
		other.clock_ = nullptr;
		other.progress_timeout_ns_ = source_closure_default_progress_timeout_ns;
		return *this;
	}

	source_closure_fd_channel::~source_closure_fd_channel()
	{
		close_owned();
	}

	void source_closure_fd_channel::close_owned() noexcept
	{
#if defined(__unix__) || defined(__APPLE__)
		if (read_descriptor_ == write_descriptor_)
		{
			if (read_descriptor_ >= 0 && (owns_read_ || owns_write_))
				(void)::close(read_descriptor_);
		}
		else
		{
			if (read_descriptor_ >= 0 && owns_read_)
				(void)::close(read_descriptor_);
			if (write_descriptor_ >= 0 && owns_write_)
				(void)::close(write_descriptor_);
		}
#endif
		read_descriptor_ = -1;
		write_descriptor_ = -1;
		owns_read_ = false;
		owns_write_ = false;
	}

	sdk::result<std::size_t> source_closure_fd_channel::read(const std::span<std::byte> destination)
	{
		if (destination.empty())
			return std::size_t{};
#if defined(__unix__) || defined(__APPLE__)
		if (read_descriptor_ < 0)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "read", "descriptor-unset"));
		for (;;)
		{
			auto ready = wait_for(
				read_descriptor_, POLLIN, cancellation_, clock_, progress_timeout_ns_, "read");
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
		if (frame_bytes.empty())
			return {};
#if defined(__unix__) || defined(__APPLE__)
		if (write_descriptor_ < 0)
			return sdk::unexpected(
				failure("source-closure.channel-invalid", "write", "descriptor-unset"));
		std::size_t offset{};
		while (offset < frame_bytes.size())
		{
			auto ready = wait_for(
				write_descriptor_, POLLOUT, cancellation_, clock_, progress_timeout_ns_, "write");
			if (!ready)
				return sdk::unexpected(std::move(ready.error()));
			if ((*ready & (POLLERR | POLLHUP)) != 0)
				return sdk::unexpected(
					failure("source-closure.channel-closed", "write", "peer-closed"));

			const auto maximum = static_cast<std::size_t>(std::numeric_limits<ssize_t>::max());
			const auto count = ::write(write_descriptor_,
									   frame_bytes.data() + offset,
									   std::min(frame_bytes.size() - offset, maximum));
			if (count > 0)
			{
				offset += static_cast<std::size_t>(count);
				continue;
			}
			if (count == 0)
				return sdk::unexpected(
					failure("source-closure.channel-closed", "write", "zero-progress"));
			if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)
				continue;
			if (errno == EBADF)
				return sdk::unexpected(
					failure("source-closure.channel-invalid", "write", "descriptor-closed"));
			if (errno == EPIPE || errno == ECONNRESET)
				return sdk::unexpected(
					failure("source-closure.channel-closed", "write", "peer-closed"));
			return sdk::unexpected(
				failure("source-closure.channel-io", "write", "write-" + errno_detail(errno)));
		}
		return {};
#else
		(void)frame_bytes;
		return sdk::unexpected(
			failure("source-closure.channel-unavailable", "platform", "posix-required"));
#endif
	}
} // namespace cxxlens::detail::clang22
