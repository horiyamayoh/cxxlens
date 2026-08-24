#include "llvm/clang22/source_closure_fd.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace
{
	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

#if defined(__unix__) || defined(__APPLE__)
	using cxxlens::detail::clang22::source_closure_fd_channel;
	using cxxlens::detail::clang22::source_closure_fd_channel_options;
	using cxxlens::detail::clang22::source_closure_fd_descriptor;
	using cxxlens::detail::clang22::source_closure_fd_ownership;

	struct descriptor_pair
	{
		int first{-1};
		int second{-1};

		descriptor_pair() = default;
		descriptor_pair(const int first_descriptor, const int second_descriptor)
			: first{first_descriptor}, second{second_descriptor}
		{
		}
		descriptor_pair(descriptor_pair&& other) noexcept : first{other.first}, second{other.second}
		{
			other.first = -1;
			other.second = -1;
		}
		descriptor_pair& operator=(descriptor_pair&& other) noexcept
		{
			if (this == &other)
				return *this;
			if (first >= 0)
				(void)::close(first);
			if (second >= 0)
				(void)::close(second);
			first = other.first;
			second = other.second;
			other.first = -1;
			other.second = -1;
			return *this;
		}

		~descriptor_pair()
		{
			if (first >= 0)
				(void)::close(first);
			if (second >= 0)
				(void)::close(second);
		}

		descriptor_pair(const descriptor_pair&) = delete;
		descriptor_pair& operator=(const descriptor_pair&) = delete;
	};

	[[nodiscard]] int promote_descriptor(const int descriptor)
	{
		const auto promoted =
			::fcntl(descriptor,
					F_DUPFD,
					cxxlens::detail::clang22::source_closure_first_inherited_descriptor);
		require(promoted >= 0, "descriptor promotion failed");
		(void)::close(descriptor);

		auto descriptor_flags = ::fcntl(promoted, F_GETFD);
		require(descriptor_flags >= 0, "descriptor flags unavailable");
		require(::fcntl(promoted, F_SETFD, descriptor_flags & ~FD_CLOEXEC) == 0,
				"descriptor close-on-exec clear failed");
		auto status_flags = ::fcntl(promoted, F_GETFL);
		require(status_flags >= 0, "descriptor status unavailable");
		require(::fcntl(promoted, F_SETFL, status_flags | O_NONBLOCK) == 0,
				"descriptor non-blocking setup failed");
		return promoted;
	}

	[[nodiscard]] descriptor_pair make_pipe()
	{
		int descriptors[2]{};
		require(::pipe(descriptors) == 0, "pipe creation failed");
		return descriptor_pair{promote_descriptor(descriptors[0]),
							   promote_descriptor(descriptors[1])};
	}

	[[nodiscard]] descriptor_pair make_socket_pair()
	{
		int descriptors[2]{};
		require(::socketpair(AF_UNIX, SOCK_STREAM, 0, descriptors) == 0,
				"socket pair creation failed");
		return descriptor_pair{promote_descriptor(descriptors[0]),
							   promote_descriptor(descriptors[1])};
	}

	void require_error(const auto& result,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view detail = {})
	{
		require(!result, "operation unexpectedly succeeded");
		require(result.error().code == code, "unexpected channel error code");
		require(result.error().field == field, "unexpected channel error field");
		if (!detail.empty())
			require(result.error().detail == detail, "unexpected channel error detail");
	}

	void rejects_reserved_and_inherited_flags()
	{
		source_closure_fd_channel_options reserved;
		reserved.read.value = 3;
		reserved.write.value = 4;
		auto result = source_closure_fd_channel::create(reserved);
		require_error(result, "source-closure.channel-invalid", "read", "reserved-descriptor");

		auto pair = make_pipe();
		auto flags = ::fcntl(pair.first, F_GETFD);
		require(flags >= 0, "fixture flags unavailable");
		require(::fcntl(pair.first, F_SETFD, flags | FD_CLOEXEC) == 0,
				"fixture close-on-exec setup failed");
		source_closure_fd_channel_options cloexec;
		cloexec.read.value = pair.first;
		cloexec.write.value = pair.second;
		result = source_closure_fd_channel::create(cloexec);
		require_error(result, "source-closure.channel-invalid", "read", "close-on-exec-set");

		flags = ::fcntl(pair.first, F_GETFD);
		require(::fcntl(pair.first, F_SETFD, flags & ~FD_CLOEXEC) == 0,
				"fixture close-on-exec clear failed");
		auto status = ::fcntl(pair.first, F_GETFL);
		require(status >= 0, "fixture status unavailable");
		require(::fcntl(pair.first, F_SETFL, status & ~O_NONBLOCK) == 0,
				"fixture blocking setup failed");
		result = source_closure_fd_channel::create(cloexec);
		require_error(result, "source-closure.channel-invalid", "read", "blocking-descriptor");
	}

	void reads_bounded_and_writes_complete()
	{
		auto pair = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {pair.first, source_closure_fd_ownership::borrowed};
		options.write = {pair.first, source_closure_fd_ownership::borrowed};
		{
			auto channel = source_closure_fd_channel::create(options);
			require(static_cast<bool>(channel), "socket channel creation failed");

			constexpr std::string_view inbound = "closure-frame";
			require(::write(pair.second, inbound.data(), inbound.size()) ==
						static_cast<ssize_t>(inbound.size()),
					"peer write failed");
			std::array<std::byte, 7> first_read{};
			auto first_count = channel->read(first_read);
			require(first_count && *first_count == first_read.size(), "bounded read size mismatch");
			require(std::memcmp(first_read.data(), inbound.data(), first_read.size()) == 0,
					"bounded read bytes mismatch");

			std::array<std::byte, inbound.size() - first_read.size()> second_read{};
			auto second_count = channel->read(second_read);
			require(second_count && *second_count == second_read.size(),
					"second bounded read size mismatch");
			require(std::memcmp(second_read.data(),
								inbound.data() + first_read.size(),
								second_read.size()) == 0,
					"second bounded read bytes mismatch");

			constexpr std::string_view outbound = "ack-frame";
			auto written =
				channel->write(std::as_bytes(std::span{outbound.data(), outbound.size()}));
			require(static_cast<bool>(written), "channel write failed");
			std::array<char, outbound.size()> peer_read{};
			require(::read(pair.second, peer_read.data(), peer_read.size()) ==
						static_cast<ssize_t>(peer_read.size()),
					"peer read failed");
			require(std::string_view{peer_read.data(), peer_read.size()} == outbound,
					"peer read bytes mismatch");

			(void)::close(pair.second);
			pair.second = -1;
			auto eof = channel->read(second_read);
			require(eof && *eof == 0U, "closed peer must produce read EOF");
		}
		require(::fcntl(pair.first, F_GETFD) >= 0, "borrowed descriptor was closed");
	}

	void cancellation_is_typed()
	{
		auto pair = make_pipe();
		std::stop_source stop;
		stop.request_stop();
		source_closure_fd_channel_options options;
		options.read = {pair.first, source_closure_fd_ownership::borrowed};
		options.write = {pair.second, source_closure_fd_ownership::borrowed};
		options.cancellation = stop.get_token();
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "cancel channel creation failed");
		std::array<std::byte, 1> byte{};
		auto read = channel->read(byte);
		require_error(read, "source-closure.channel-cancelled", "read", "stop-requested");
		auto write = channel->write(byte);
		require_error(write, "source-closure.channel-cancelled", "write", "stop-requested");
	}

	class sequence_clock final : public cxxlens::detail::clang22::source_closure_monotonic_clock
	{
	  public:
		explicit sequence_clock(std::vector<std::uint64_t> values) : values_{std::move(values)} {}

		cxxlens::sdk::result<std::uint64_t> now_ns() const override
		{
			const auto index = std::min(index_, values_.size() - 1U);
			++index_;
			return values_[index];
		}

	  private:
		std::vector<std::uint64_t> values_;
		mutable std::size_t index_{};
	};

	void timeout_is_typed()
	{
		auto pair = make_pipe();
		sequence_clock clock{{0U, 5U}};
		source_closure_fd_channel_options options;
		options.read = {pair.first, source_closure_fd_ownership::borrowed};
		options.write = {pair.second, source_closure_fd_ownership::borrowed};
		options.clock = &clock;
		options.progress_timeout_ns = 5U;
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "timeout channel creation failed");
		std::array<std::byte, 1> byte{};
		auto result = channel->read(byte);
		require_error(result, "source-closure.channel-timeout", "read", "progress-deadline");
	}

	void closed_peer_is_typed()
	{
		auto pair = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {pair.first, source_closure_fd_ownership::borrowed};
		options.write = {pair.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "closed channel creation failed");
		(void)::close(pair.second);
		pair.second = -1;
		std::array<std::byte, 1> byte{};
		auto result = channel->write(byte);
		require_error(result, "source-closure.channel-closed", "write");
	}

	void owned_endpoint_closes_once()
	{
		auto pair = make_socket_pair();
		const auto owned_descriptor = pair.first;
		(void)::close(pair.second);
		pair.second = -1;
		{
			source_closure_fd_channel_options options;
			options.read = {owned_descriptor, source_closure_fd_ownership::owned};
			options.write = {owned_descriptor, source_closure_fd_ownership::owned};
			auto channel = source_closure_fd_channel::create(options);
			require(static_cast<bool>(channel), "owned channel creation failed");
			pair.first = -1;
		}
		require(::fcntl(owned_descriptor, F_GETFD) < 0 && errno == EBADF,
				"owned descriptor was not closed");
	}
#endif
} // namespace

int main()
{
#if defined(__unix__) || defined(__APPLE__)
	rejects_reserved_and_inherited_flags();
	reads_bounded_and_writes_complete();
	cancellation_is_typed();
	timeout_is_typed();
	closed_peer_is_typed();
	owned_endpoint_closes_once();
	return 0;
#else
	return 0;
#endif
}
