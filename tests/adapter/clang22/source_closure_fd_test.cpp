#include "llvm/clang22/source_closure_fd.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <sys/socket.h>
#include <time.h>
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
		{
			if (result.error().detail != detail)
				std::cerr << "expected detail " << detail << ", actual " << result.error().detail
						  << '\n';
			require(result.error().detail == detail, "unexpected channel error detail");
		}
	}

	void rejects_reserved_and_inherited_flags()
	{
		source_closure_fd_channel_options reserved;
		reserved.read.value = 3;
		reserved.write.value = 4;
		auto result = source_closure_fd_channel::create(reserved);
		require_error(result, "source-closure.channel-invalid", "read", "reserved-descriptor");

		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		auto flags = ::fcntl(input.first, F_GETFD);
		require(flags >= 0, "fixture flags unavailable");
		require(::fcntl(input.first, F_SETFD, flags | FD_CLOEXEC) == 0,
				"fixture close-on-exec setup failed");
		source_closure_fd_channel_options cloexec;
		cloexec.read.value = input.first;
		cloexec.write.value = ack.first;
		result = source_closure_fd_channel::create(cloexec);
		require_error(result, "source-closure.channel-invalid", "read", "close-on-exec-set");

		flags = ::fcntl(input.first, F_GETFD);
		require(::fcntl(input.first, F_SETFD, flags & ~FD_CLOEXEC) == 0,
				"fixture close-on-exec clear failed");
		auto status = ::fcntl(input.first, F_GETFL);
		require(status >= 0, "fixture status unavailable");
		require(::fcntl(input.first, F_SETFL, status & ~O_NONBLOCK) == 0,
				"fixture blocking setup failed");
		result = source_closure_fd_channel::create(cloexec);
		require_error(result, "source-closure.channel-invalid", "read", "blocking-descriptor");

		auto duplicate = make_socket_pair();
		source_closure_fd_channel_options duplicated;
		duplicated.read.value = duplicate.first;
		duplicated.write.value = duplicate.first;
		result = source_closure_fd_channel::create(duplicated);
		require_error(result, "source-closure.channel-invalid", "descriptor", "duplicate");
		const auto aliased_descriptor =
			::fcntl(duplicate.first,
					F_DUPFD,
					cxxlens::detail::clang22::source_closure_first_inherited_descriptor);
		require(aliased_descriptor >= 0, "aliased descriptor creation failed");
		duplicated.write.value = aliased_descriptor;
		result = source_closure_fd_channel::create(duplicated);
		require_error(
			result, "source-closure.channel-invalid", "descriptor", "same-physical-endpoint");
		require(::close(aliased_descriptor) == 0, "aliased descriptor close failed");

		auto pipe = make_pipe();
		auto write_socket = make_socket_pair();
		source_closure_fd_channel_options non_socket;
		non_socket.read.value = pipe.first;
		non_socket.write.value = write_socket.first;
		result = source_closure_fd_channel::create(non_socket);
		require_error(result, "source-closure.channel-invalid", "read", "not-socket");
	}

	void reads_bounded_and_writes_complete()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		{
			auto channel = source_closure_fd_channel::create(options);
			require(static_cast<bool>(channel), "socket channel creation failed");

			constexpr std::string_view inbound = "closure-frame";
			require(::write(input.second, inbound.data(), inbound.size()) ==
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
			if (!written)
				std::cerr << written.error().code << ':' << written.error().field << ':'
						  << written.error().detail << '\n';
			require(static_cast<bool>(written), "channel write failed");
			std::array<char, outbound.size()> peer_read{};
			require(::read(ack.second, peer_read.data(), peer_read.size()) ==
						static_cast<ssize_t>(peer_read.size()),
					"peer read failed");
			require(std::string_view{peer_read.data(), peer_read.size()} == outbound,
					"peer read bytes mismatch");

			(void)::close(input.second);
			input.second = -1;
			auto eof = channel->read(second_read);
			require(eof && *eof == 0U, "closed peer must produce read EOF");
		}
		require(::fcntl(input.first, F_GETFD) >= 0 && ::fcntl(ack.first, F_GETFD) >= 0,
				"borrowed descriptor was closed");
	}

	void borrowed_descriptor_reuse_cannot_redirect_the_channel()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "reuse channel creation failed");

		auto replacement_input = make_socket_pair();
		auto replacement_ack = make_socket_pair();
		require(::dup2(replacement_input.first, input.first) == input.first,
				"read descriptor reuse failed");
		require(::dup2(replacement_ack.first, ack.first) == ack.first,
				"write descriptor reuse failed");

		constexpr std::string_view inbound = "pinned-inbound";
		require(::write(input.second, inbound.data(), inbound.size()) ==
					static_cast<ssize_t>(inbound.size()),
				"old input peer write failed");
		std::array<std::byte, inbound.size()> received{};
		auto count = channel->read(received);
		require(count && *count == received.size(), "pinned read was redirected by FD reuse");
		require(std::memcmp(received.data(), inbound.data(), inbound.size()) == 0,
				"pinned read content mismatch");

		constexpr std::string_view outbound = "pinned-ack";
		auto sent = channel->write(std::as_bytes(std::span{outbound.data(), outbound.size()}));
		require(static_cast<bool>(sent), "pinned write failed after external FD reuse");
		std::array<char, outbound.size()> observed{};
		require(::read(ack.second, observed.data(), observed.size()) ==
					static_cast<ssize_t>(observed.size()),
				"old ACK peer did not receive pinned write");
		require(std::string_view{observed.data(), observed.size()} == outbound,
				"pinned ACK content mismatch");
	}

	[[nodiscard]] ssize_t read_after_poll(const int descriptor,
										  std::span<std::byte> destination,
										  const int timeout_ms = 2'000)
	{
		pollfd ready{descriptor, POLLIN, 0};
		int outcome{};
		do
		{
			outcome = ::poll(&ready, 1, timeout_ms);
		} while (outcome < 0 && errno == EINTR);
		require(outcome > 0, "test peer read timed out");
		ssize_t count{};
		do
		{
			count = ::read(descriptor, destination.data(), destination.size());
		} while (count < 0 && errno == EINTR);
		require(count >= 0, "test peer read failed");
		return count;
	}

	void partial_nonblocking_write_is_drained_exactly()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		constexpr int small_send_buffer = 4'096;
		require(
			::setsockopt(
				ack.first, SOL_SOCKET, SO_SNDBUF, &small_send_buffer, sizeof(small_send_buffer)) ==
				0,
			"fixture send-buffer limit failed");
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "partial channel creation failed");

		std::array<std::byte, 4'096> filler{};
		std::fill(filler.begin(), filler.end(), std::byte{0xa5});
		std::size_t prefilled{};
		for (;;)
		{
			const auto count = ::write(ack.first, filler.data(), filler.size());
			if (count > 0)
			{
				prefilled += static_cast<std::size_t>(count);
				continue;
			}
			if (count < 0 && errno == EINTR)
				continue;
			require(count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK),
					"fixture did not reach nonblocking backpressure");
			break;
		}
		require(prefilled != 0U, "fixture did not prefill ACK socket");

		std::vector<std::byte> payload(512U * 1'024U);
		for (std::size_t index = 0; index < payload.size(); ++index)
			payload[index] = static_cast<std::byte>(index % 251U);
		std::optional<cxxlens::sdk::result<void>> outcome;
		std::atomic_bool writer_done{false};
		std::thread writer{[&]
						   {
							   outcome.emplace(channel->write(payload));
							   writer_done.store(true, std::memory_order_release);
						   }};

		std::vector<std::byte> received;
		received.reserve(payload.size());
		std::size_t discarded{};
		std::array<std::byte, 4'096> chunk{};
		while (received.empty())
		{
			const auto count = read_after_poll(ack.second, chunk);
			require(count > 0, "ACK peer closed before payload prefix");
			const auto size = static_cast<std::size_t>(count);
			const auto prefix = std::min(prefilled - discarded, size);
			discarded += prefix;
			received.insert(received.end(),
							chunk.begin() + static_cast<std::ptrdiff_t>(prefix),
							chunk.begin() + count);
		}
		require(!writer_done.load(std::memory_order_acquire),
				"large ACK completed without exercising the partial-write loop");
		while (received.size() < payload.size())
		{
			const auto count = read_after_poll(ack.second, chunk);
			require(count > 0, "ACK peer closed during partial write");
			received.insert(received.end(), chunk.begin(), chunk.begin() + count);
		}
		writer.join();
		require(outcome.has_value() && static_cast<bool>(*outcome),
				"partial ACK write did not complete");
		require(received == payload, "partial ACK write duplicated or reordered bytes");
	}

	void failure_after_partial_write_poisoned_the_channel()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		constexpr int small_send_buffer = 4'096;
		require(
			::setsockopt(
				ack.first, SOL_SOCKET, SO_SNDBUF, &small_send_buffer, sizeof(small_send_buffer)) ==
				0,
			"poison fixture send-buffer limit failed");
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "poison channel creation failed");

		std::vector<std::byte> payload(4U * 1'024U * 1'024U, std::byte{0x5a});
		std::optional<cxxlens::sdk::result<void>> outcome;
		std::atomic_bool writer_done{false};
		std::thread writer{[&]
						   {
							   outcome.emplace(channel->write(payload));
							   writer_done.store(true, std::memory_order_release);
						   }};
		std::array<std::byte, 1> observed{};
		require(read_after_poll(ack.second, observed) == 1,
				"partial failure fixture observed no write progress");
		require(!writer_done.load(std::memory_order_acquire),
				"partial failure fixture unexpectedly completed");
		require(::close(ack.second) == 0, "partial failure peer close failed");
		ack.second = -1;
		writer.join();
		require(outcome.has_value(), "partial failure writer returned no result");
		require_error(*outcome, "source-closure.channel-closed", "write");

		auto retry = channel->write(std::span<const std::byte>{payload.data(), 1U});
		require_error(retry, "source-closure.channel-poisoned", "write", "partial-write");
		auto read = channel->read(observed);
		require_error(read, "source-closure.channel-poisoned", "read", "partial-write");
	}

	void cancellation_is_typed()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		std::stop_source stop;
		stop.request_stop();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		options.cancellation = stop.get_token();
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "cancel channel creation failed");
		std::array<std::byte, 1> byte{};
		auto read = channel->read(byte);
		require_error(read, "source-closure.channel-cancelled", "read", "stop-requested");
		auto write = channel->write(byte);
		require_error(write, "source-closure.channel-cancelled", "write", "stop-requested");
	}

	volatile std::sig_atomic_t sigusr1_count{};

	void count_sigusr1(int)
	{
		sigusr1_count = 1;
	}

	void interrupted_poll_retries_without_losing_bytes()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "EINTR channel creation failed");

		struct sigaction action{};
		action.sa_handler = count_sigusr1;
		require(::sigemptyset(&action.sa_mask) == 0, "SIGUSR1 mask setup failed");
		action.sa_flags = 0;
		struct sigaction previous{};
		require(::sigaction(SIGUSR1, &action, &previous) == 0,
				"SIGUSR1 handler installation failed");
		sigusr1_count = 0;
		const auto reader_thread = ::pthread_self();
		std::atomic_bool entering_read{false};
		int signal_error{};
		ssize_t peer_write{-1};
		std::thread interrupter{[&]
								{
									while (!entering_read.load(std::memory_order_acquire))
										std::this_thread::yield();
									constexpr timespec interval{0, 1'000'000};
									for (int attempt = 0; attempt < 5; ++attempt)
									{
										(void)::nanosleep(&interval, nullptr);
										const auto sent = ::pthread_kill(reader_thread, SIGUSR1);
										if (sent != 0 && signal_error == 0)
											signal_error = sent;
									}
									constexpr std::byte inbound{0x6d};
									peer_write = ::write(input.second, &inbound, 1U);
								}};
		std::array<std::byte, 1> observed{};
		entering_read.store(true, std::memory_order_release);
		auto read = channel->read(observed);
		interrupter.join();
		require(::sigaction(SIGUSR1, &previous, nullptr) == 0,
				"SIGUSR1 handler restoration failed");
		require(signal_error == 0 && sigusr1_count > 0,
				"fixture did not interrupt the channel poll");
		require(peer_write == 1, "EINTR peer write failed");
		require(read && *read == 1U && observed.front() == std::byte{0x6d},
				"channel did not recover from interrupted poll");
	}

	class sequence_clock final : public cxxlens::detail::clang22::source_closure_monotonic_clock
	{
	  public:
		explicit sequence_clock(std::vector<std::uint64_t> values,
								std::atomic_bool* observation = nullptr,
								const std::size_t notify_after_calls = 0U)
			: values_{std::move(values)}, observation_{observation},
			  notify_after_calls_{notify_after_calls}
		{
		}

		cxxlens::sdk::result<std::uint64_t> now_ns() const override
		{
			if (index_ >= values_.size())
				return cxxlens::sdk::unexpected(cxxlens::sdk::error{
					"source-closure.test-clock-exhausted", "clock", "script-exhausted"});
			const auto value = values_[index_];
			++index_;
			if (observation_ != nullptr && index_ == notify_after_calls_)
				observation_->store(true, std::memory_order_release);
			return value;
		}

	  private:
		std::vector<std::uint64_t> values_;
		std::atomic_bool* observation_{};
		std::size_t notify_after_calls_{};
		mutable std::size_t index_{};
	};

	void timeout_is_typed()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		sequence_clock clock{{0U, 5U}};
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		options.clock = &clock;
		options.progress_timeout_ns = 5U;
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "timeout channel creation failed");
		std::array<std::byte, 1> byte{};
		auto result = channel->read(byte);
		require_error(result, "source-closure.channel-timeout", "read", "progress-deadline");
	}

	void completed_byte_progress_starts_the_next_absolute_interval()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		sequence_clock clock{{0U, 4U, 4U, 8U}};
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		options.clock = &clock;
		options.progress_timeout_ns = 5U;
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "byte progress channel creation failed");

		constexpr std::array<std::byte, 2> inbound{std::byte{0x31}, std::byte{0x32}};
		require(::write(input.second, inbound.data(), inbound.size()) ==
					static_cast<ssize_t>(inbound.size()),
				"byte progress peer write failed");
		std::array<std::byte, 2> observed{};
		auto first = channel->read(std::span{observed}.first(1U));
		auto second = channel->read(std::span{observed}.subspan(1U));
		require(first && *first == 1U && second && *second == 1U && observed == inbound,
				"completed byte progress did not start the next deadline interval");
	}

	void invalid_clock_is_typed_before_io()
	{
		auto run = [](sequence_clock& clock, const std::uint64_t timeout_ns)
		{
			auto input = make_socket_pair();
			auto ack = make_socket_pair();
			source_closure_fd_channel_options options;
			options.read = {input.first, source_closure_fd_ownership::borrowed};
			options.write = {ack.first, source_closure_fd_ownership::borrowed};
			options.clock = &clock;
			options.progress_timeout_ns = timeout_ns;
			auto channel = source_closure_fd_channel::create(options);
			require(static_cast<bool>(channel), "clock failure channel creation failed");
			std::array<std::byte, 1> byte{};
			return channel->read(byte);
		};

		sequence_clock backwards_clock{{10U, 9U}};
		auto backwards = run(backwards_clock, 10U);
		require_error(backwards, "source-closure.channel-clock-invalid", "read", "backwards");

		sequence_clock overflow_clock{{std::numeric_limits<std::uint64_t>::max()}};
		auto overflow = run(overflow_clock, 1U);
		require_error(
			overflow, "source-closure.channel-clock-invalid", "read", "deadline-overflow");

		sequence_clock failed_clock{{}};
		auto failed = run(failed_clock, 1U);
		require_error(failed, "source-closure.channel-clock-invalid", "read", "script-exhausted");
	}

	void interrupted_poll_keeps_one_absolute_deadline()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		std::atomic_bool poll_observation_complete{false};
		sequence_clock clock{
			{0U, 0U, 4'000'000'000ULL, 5'000'000'000ULL}, &poll_observation_complete, 2U};
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		options.clock = &clock;
		options.progress_timeout_ns = 5'000'000'000ULL;
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "absolute deadline channel creation failed");

		struct sigaction action{};
		action.sa_handler = count_sigusr1;
		require(::sigemptyset(&action.sa_mask) == 0, "deadline signal mask setup failed");
		struct sigaction previous{};
		require(::sigaction(SIGUSR1, &action, &previous) == 0,
				"deadline signal handler installation failed");
		sigusr1_count = 0;
		const auto reader_thread = ::pthread_self();
		int signal_error{};
		std::thread interrupter{
			[&]
			{
				while (!poll_observation_complete.load(std::memory_order_acquire))
					std::this_thread::yield();
				constexpr timespec interval{0, 1'000'000};
				for (int attempt = 0; attempt < 2; ++attempt)
				{
					(void)::nanosleep(&interval, nullptr);
					const auto sent = ::pthread_kill(reader_thread, SIGUSR1);
					if (sent != 0 && signal_error == 0)
						signal_error = sent;
				}
			}};
		std::array<std::byte, 1> byte{};
		auto result = channel->read(byte);
		interrupter.join();
		require(::sigaction(SIGUSR1, &previous, nullptr) == 0,
				"deadline signal handler restoration failed");
		require(signal_error == 0 && sigusr1_count > 0,
				"fixture did not deliver the deadline interrupt");
		require_error(result, "source-closure.channel-timeout", "read", "progress-deadline");
	}

	volatile std::sig_atomic_t sigpipe_count{};

	void count_sigpipe(int)
	{
		sigpipe_count = 1;
	}

	void closed_peer_is_typed_and_sigpipe_safe()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "closed channel creation failed");
		const auto previous = std::signal(SIGPIPE, count_sigpipe);
		require(previous != SIG_ERR, "SIGPIPE handler installation failed");
		sigpipe_count = 0;
		require(::close(ack.second) == 0, "peer close failed");
		ack.second = -1;
		std::array<std::byte, 1> byte{};
		auto result = channel->write(byte);
		require_error(result, "source-closure.channel-closed", "write");
		require(sigpipe_count == 0, "ACK write delivered SIGPIPE");
		require(std::signal(SIGPIPE, previous) != SIG_ERR, "SIGPIPE handler restoration failed");
	}

	void caller_pending_sigpipe_is_not_consumed()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		source_closure_fd_channel_options options;
		options.read = {input.first, source_closure_fd_ownership::borrowed};
		options.write = {ack.first, source_closure_fd_ownership::borrowed};
		auto channel = source_closure_fd_channel::create(options);
		require(static_cast<bool>(channel), "pending-signal channel creation failed");

		sigset_t pipe_set{};
		require(::sigemptyset(&pipe_set) == 0 && ::sigaddset(&pipe_set, SIGPIPE) == 0,
				"SIGPIPE set creation failed");
		sigset_t previous_mask{};
		require(::pthread_sigmask(SIG_BLOCK, &pipe_set, &previous_mask) == 0,
				"SIGPIPE block failed");
		require(::close(ack.second) == 0, "pending-signal peer close failed");
		ack.second = -1;
		std::array<std::byte, 1> byte{};
		errno = 0;
		require(::write(ack.first, byte.data(), byte.size()) < 0 && errno == EPIPE,
				"fixture did not queue SIGPIPE");
		sigset_t pending{};
		require(::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1,
				"fixture SIGPIPE was not pending");

		auto result = channel->write(byte);
		require_error(result, "source-closure.channel-closed", "write");
		sigset_t current_mask{};
		require(::pthread_sigmask(SIG_SETMASK, nullptr, &current_mask) == 0 &&
					::sigismember(&current_mask, SIGPIPE) == 1,
				"channel changed the caller SIGPIPE mask");
		require(::sigpending(&pending) == 0 && ::sigismember(&pending, SIGPIPE) == 1,
				"channel consumed the caller's pending SIGPIPE");
		constexpr timespec no_wait{};
		require(::sigtimedwait(&pipe_set, nullptr, &no_wait) == SIGPIPE,
				"pending SIGPIPE cleanup failed");
		require(::pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) == 0,
				"SIGPIPE mask restoration failed");
	}

	void owned_endpoint_closes_once()
	{
		auto input = make_socket_pair();
		auto ack = make_socket_pair();
		const auto owned_read = input.first;
		const auto owned_write = ack.first;
		{
			source_closure_fd_channel_options options;
			options.read = {owned_read, source_closure_fd_ownership::owned};
			options.write = {owned_write, source_closure_fd_ownership::owned};
			auto channel = source_closure_fd_channel::create(options);
			require(static_cast<bool>(channel), "owned channel creation failed");
			input.first = -1;
			ack.first = -1;
		}
		require(::fcntl(owned_read, F_GETFD) < 0 && errno == EBADF,
				"owned read descriptor was not closed");
		require(::fcntl(owned_write, F_GETFD) < 0 && errno == EBADF,
				"owned write descriptor was not closed");
	}

	void owned_input_is_closed_when_factory_rejects_the_other_endpoint()
	{
		auto input = make_socket_pair();
		auto invalid_write = make_pipe();
		const auto owned_read = input.first;
		const auto owned_write = invalid_write.second;
		source_closure_fd_channel_options options;
		options.read = {owned_read, source_closure_fd_ownership::owned};
		options.write = {owned_write, source_closure_fd_ownership::owned};
		auto channel = source_closure_fd_channel::create(options);
		input.first = -1;
		invalid_write.second = -1;
		require_error(channel, "source-closure.channel-invalid", "write", "not-socket");
		errno = 0;
		require(::fcntl(owned_read, F_GETFD) < 0 && errno == EBADF,
				"owned read leaked after factory rejection");
		errno = 0;
		require(::fcntl(owned_write, F_GETFD) < 0 && errno == EBADF,
				"owned write leaked after factory rejection");
	}

	void move_transfers_private_cleanup_custody_once()
	{
		auto first_input = make_socket_pair();
		auto first_ack = make_socket_pair();
		auto second_input = make_socket_pair();
		auto second_ack = make_socket_pair();
		const auto first_read = first_input.first;
		const auto first_write = first_ack.first;
		const auto second_read = second_input.first;
		const auto second_write = second_ack.first;

		source_closure_fd_channel_options first_options;
		first_options.read = {first_read, source_closure_fd_ownership::owned};
		first_options.write = {first_write, source_closure_fd_ownership::owned};
		auto first = source_closure_fd_channel::create(first_options);
		first_input.first = -1;
		first_ack.first = -1;
		require(static_cast<bool>(first), "first move channel creation failed");
		for (const auto descriptor : {first_read, first_write})
		{
			errno = 0;
			require(::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF,
					"first owned input was not consumed by successful factory");
		}
		source_closure_fd_channel_options second_options;
		second_options.read = {second_read, source_closure_fd_ownership::owned};
		second_options.write = {second_write, source_closure_fd_ownership::owned};
		auto second = source_closure_fd_channel::create(second_options);
		second_input.first = -1;
		second_ack.first = -1;
		require(static_cast<bool>(second), "second move channel creation failed");
		for (const auto descriptor : {second_read, second_write})
		{
			errno = 0;
			require(::fcntl(descriptor, F_GETFD) < 0 && errno == EBADF,
					"second owned input was not consumed by successful factory");
		}

		{
			auto moving = std::move(*first);
			auto destination = std::move(*second);
			destination = std::move(moving);
			std::array<std::byte, 1> eof{};
			require(read_after_poll(second_ack.second, eof) == 0,
					"move assignment did not release replaced write custody");

			constexpr std::string_view inbound = "move-in";
			require(::write(first_input.second, inbound.data(), inbound.size()) ==
						static_cast<ssize_t>(inbound.size()),
					"move input peer write failed");
			std::array<std::byte, inbound.size()> input_bytes{};
			auto read = destination.read(input_bytes);
			require(read && *read == input_bytes.size(), "moved channel lost read custody");

			constexpr std::string_view outbound = "move-ack";
			auto write =
				destination.write(std::as_bytes(std::span{outbound.data(), outbound.size()}));
			require(static_cast<bool>(write), "moved channel lost write custody");
			std::array<std::byte, outbound.size()> output_bytes{};
			require(read_after_poll(first_ack.second, output_bytes) ==
						static_cast<ssize_t>(output_bytes.size()),
					"moved ACK peer did not receive bytes");
		}
		std::array<std::byte, 1> eof{};
		require(read_after_poll(first_ack.second, eof) == 0,
				"final moved channel did not release write custody");
	}
#endif
} // namespace

int main()
{
#if defined(__unix__) || defined(__APPLE__)
	rejects_reserved_and_inherited_flags();
	reads_bounded_and_writes_complete();
	borrowed_descriptor_reuse_cannot_redirect_the_channel();
	partial_nonblocking_write_is_drained_exactly();
	failure_after_partial_write_poisoned_the_channel();
	cancellation_is_typed();
	interrupted_poll_retries_without_losing_bytes();
	timeout_is_typed();
	completed_byte_progress_starts_the_next_absolute_interval();
	invalid_clock_is_typed_before_io();
	interrupted_poll_keeps_one_absolute_deadline();
	closed_peer_is_typed_and_sigpipe_safe();
	caller_pending_sigpipe_is_not_consumed();
	owned_endpoint_closes_once();
	owned_input_is_closed_when_factory_rejects_the_other_endpoint();
	move_transfers_private_cleanup_custody_once();
	return 0;
#else
	return 0;
#endif
}
