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
		constexpr int poll_slice_milliseconds = 25;

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
												  const std::string_view field)
		{
			for (;;)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(failure(
						"source-closure.channel-cancelled", std::string(field), "stop-requested"));

				pollfd descriptor_poll{descriptor, events, 0};
				const auto outcome = ::poll(&descriptor_poll, 1, poll_slice_milliseconds);
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
		  cancellation_{options.cancellation}
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
		  cancellation_{other.cancellation_}
	{
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.owns_read_ = false;
		other.owns_write_ = false;
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
		other.read_descriptor_ = -1;
		other.write_descriptor_ = -1;
		other.owns_read_ = false;
		other.owns_write_ = false;
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
			auto ready = wait_for(read_descriptor_, POLLIN, cancellation_, "read");
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
			auto ready = wait_for(write_descriptor_, POLLOUT, cancellation_, "write");
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
