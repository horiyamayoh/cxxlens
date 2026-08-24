#include <array>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stop_token>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <dirent.h>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "sdk/provider_runtime_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	using cxxlens::sdk::provider::detail::make_process_inherited_channel_binding;
	using cxxlens::sdk::provider::detail::make_process_source_closure_launch;
	using cxxlens::sdk::provider::detail::process_inherited_channel_binding;
	using cxxlens::sdk::provider::detail::process_source_closure_descriptor_projection;
	using cxxlens::sdk::provider::detail::process_source_closure_launch;
	using cxxlens::sdk::provider::detail::process_source_closure_launch_adapter_access;
	using cxxlens::sdk::provider::detail::process_source_closure_launch_view;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

#if defined(__linux__) && defined(__GLIBC__)
	class descriptor final
	{
	  public:
		explicit descriptor(const int value = -1) noexcept : value_{value} {}
		descriptor(const descriptor&) = delete;
		descriptor& operator=(const descriptor&) = delete;
		descriptor(descriptor&& other) noexcept : value_{std::exchange(other.value_, -1)} {}
		descriptor& operator=(descriptor&& other) noexcept
		{
			if (this != &other)
			{
				reset();
				value_ = std::exchange(other.value_, -1);
			}
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

	struct channel_fixture
	{
		descriptor read;
		descriptor write;
		descriptor read_peer;
		descriptor write_peer;
	};

	[[nodiscard]] descriptor duplicate_channel_endpoint(const int value)
	{
		const auto duplicate = ::fcntl(value, F_DUPFD_CLOEXEC, 4);
		require(duplicate >= 4, "channel endpoint duplication failed");
		const auto flags = ::fcntl(duplicate, F_SETFD, 0);
		require(flags == 0, "channel endpoint close-on-exec clear failed");
		return descriptor{duplicate};
	}

	[[nodiscard]] channel_fixture make_channel()
	{
		std::array<int, 2U> first{-1, -1};
		std::array<int, 2U> second{-1, -1};
		require(
			::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, first.data()) == 0,
			"read channel socketpair failed");
		require(::socketpair(
					AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, second.data()) == 0,
				"write channel socketpair failed");
		descriptor first_left{first[0]};
		descriptor first_right{first[1]};
		descriptor second_left{second[0]};
		descriptor second_right{second[1]};
		return {duplicate_channel_endpoint(first_left.get()),
				duplicate_channel_endpoint(second_left.get()),
				std::move(first_right),
				std::move(second_right)};
	}

	[[nodiscard]] int promote_host_endpoint(const int value)
	{
		if (value >= 4)
			return value;
		const auto promoted = ::fcntl(value, F_DUPFD_CLOEXEC, 4);
		require(promoted >= 4, "host endpoint promotion failed");
		(void)::close(value);
		return promoted;
	}

	[[nodiscard]] channel_fixture make_host_channel()
	{
		std::array<int, 2U> first{-1, -1};
		std::array<int, 2U> second{-1, -1};
		require(
			::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, first.data()) == 0,
			"host read channel socketpair failed");
		require(::socketpair(
					AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, second.data()) == 0,
				"host write channel socketpair failed");
		return {descriptor{promote_host_endpoint(first[0])},
				descriptor{promote_host_endpoint(second[0])},
				descriptor{promote_host_endpoint(first[1])},
				descriptor{promote_host_endpoint(second[1])}};
	}

	[[nodiscard]] std::size_t open_fd_count()
	{
		DIR* directory = ::opendir("/proc/self/fd");
		require(directory != nullptr, "fd census directory could not be opened");
		std::size_t count{};
		while (::readdir(directory) != nullptr)
			++count;
		require(::closedir(directory) == 0, "fd census directory could not be closed");
		return count;
	}

	[[nodiscard]] result<process_source_closure_launch>
	make_source_launch(const channel_fixture& fixture, const char fill)
	{
		const auto read_descriptor = ::fcntl(fixture.read.get(), F_DUPFD_CLOEXEC, 4);
		const auto write_descriptor = ::fcntl(fixture.write.get(), F_DUPFD_CLOEXEC, 4);
		require(read_descriptor >= 4 && write_descriptor >= 4,
				"source launch descriptor promotion failed");
		const auto close_inputs = [&]
		{
			(void)::close(read_descriptor);
			(void)::close(write_descriptor);
		};
		const auto hex = std::string(64U, fill);
		const auto task_v4_digest = "semantic-v2:sha256:" + hex;
		const auto closure_digest = "semantic-v2:sha256:" + hex;
		auto result = make_process_source_closure_launch(
			read_descriptor,
			write_descriptor,
			"task:" + task_v4_digest,
			"provider-session:sha256:" + hex,
			task_v4_digest,
			"source-closure:" + closure_digest,
			closure_digest,
			"semantic-v2:sha256:" + std::string(64U, static_cast<char>(fill + 1)),
			"semantic-v2:sha256:" + std::string(64U, static_cast<char>(fill + 2)),
			23U,
			0U);
		close_inputs();
		return result;
	}

	struct source_projection_operation_context
	{
		int read_peer{-1};
		int write_peer{-1};
		int foreign_descriptor{-1};
		int foreign_peer{-1};
		bool invoked{};
	};

	result<void> exercise_source_projection(void* const opaque,
											const int read_descriptor,
											const int write_descriptor)
	{
		auto& context = *static_cast<source_projection_operation_context*>(opaque);
		require(!context.invoked, "source projection callback was replayed");
		context.invoked = true;
		const char inbound = 'i';
		require(::write(context.read_peer, &inbound, 1) == 1,
				"source projection read peer could not write");
		char received_inbound{};
		require(::read(read_descriptor, &received_inbound, 1) == 1 && received_inbound == inbound,
				"source projection read descriptor did not receive data");
		const char outbound = 'o';
		require(::write(write_descriptor, &outbound, 1) == 1,
				"source projection write descriptor could not write");
		char received_outbound{};
		require(::read(context.write_peer, &received_outbound, 1) == 1 &&
					received_outbound == outbound,
				"source projection write peer did not receive data");

		require(::close(read_descriptor) == 0,
				"source projection owned read descriptor could not be closed");
		std::array<int, 2U> replacement{-1, -1};
		require(::socketpair(
					AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0, replacement.data()) ==
					0,
				"source projection foreign replacement setup failed");
		if (replacement[0] == read_descriptor)
		{
			context.foreign_descriptor = replacement[0];
			context.foreign_peer = replacement[1];
		}
		else if (replacement[1] == read_descriptor)
		{
			context.foreign_descriptor = replacement[1];
			context.foreign_peer = replacement[0];
		}
		else
		{
			require(::dup3(replacement[0], read_descriptor, O_CLOEXEC) == read_descriptor,
					"source projection foreign replacement duplication failed");
			(void)::close(replacement[0]);
			context.foreign_descriptor = read_descriptor;
			context.foreign_peer = replacement[1];
		}
		const char foreign_marker = 'r';
		require(::write(context.foreign_descriptor, &foreign_marker, 1) == 1,
				"source projection foreign replacement could not write");
		char foreign_received{};
		require(::read(context.foreign_peer, &foreign_received, 1) == 1 &&
					foreign_received == foreign_marker,
				"source projection foreign peer did not receive data");
		return {};
	}

	result<void>
	reject_source_projection(void*, const int read_descriptor, const int write_descriptor)
	{
		require(::fcntl(read_descriptor, F_GETFD) >= 0 && ::fcntl(write_descriptor, F_GETFD) >= 0,
				"failing source projection callback received closed descriptors");
		return unexpected(
			error{"provider.process-launch-failed", "source_projection", "injected-failure"});
	}

	void check_source_launch_core()
	{
		static_assert(!std::is_copy_constructible_v<process_source_closure_launch>);
		static_assert(!std::is_copy_assignable_v<process_source_closure_launch>);
		static_assert(std::is_move_constructible_v<process_source_closure_launch>);
		static_assert(!std::is_move_assignable_v<process_source_closure_launch>);
		static_assert(!std::is_aggregate_v<process_source_closure_launch>);
		static_assert(!std::is_copy_constructible_v<process_source_closure_launch_view>);
		static_assert(!std::is_copy_assignable_v<process_source_closure_launch_view>);
		static_assert(std::is_move_constructible_v<process_source_closure_launch_view>);
		static_assert(!std::is_move_assignable_v<process_source_closure_launch_view>);
		static_assert(!std::is_aggregate_v<process_source_closure_launch_view>);
		static_assert(!std::is_copy_constructible_v<process_source_closure_descriptor_projection>);
		static_assert(!std::is_copy_assignable_v<process_source_closure_descriptor_projection>);
		static_assert(std::is_move_constructible_v<process_source_closure_descriptor_projection>);
		static_assert(!std::is_move_assignable_v<process_source_closure_descriptor_projection>);
		static_assert(!std::is_aggregate_v<process_source_closure_descriptor_projection>);

		auto fixture = make_host_channel();
		const auto source_read = fixture.read.get();
		const auto source_write = fixture.write.get();
		const auto before = open_fd_count();
		auto result = make_source_launch(fixture, '1');
		require(result.has_value(),
				"move-only host launch setup failed: " +
					(result ? std::string{}
							: result.error().code + ":" + result.error().field + ":" +
							 result.error().detail));
		auto launch = std::move(*result);
		require(launch.task_id() == "task:" + std::string{launch.task_v4_digest()},
				"host launch task authority changed");
		require(launch.closure_id() == "source-closure:" + std::string{launch.closure_digest()},
				"host launch closure authority changed");
		require(launch.stream_id() == 23U && launch.first_sequence() == 0U,
				"host launch sequence authority changed");
		require(launch.binding_digest().starts_with("process-channel:sha256:") &&
					launch.binding_digest().size() ==
						std::string_view{"process-channel:sha256:"}.size() + 64U,
				"host launch digest is not canonical");
		require(launch.validate().has_value(), "host launch failed live validation");
		auto claimed = std::move(launch).claim_launch();
		require(claimed.has_value(), "host launch one-shot claim failed");
		const auto replay = std::move(launch).claim_launch();
		require(!replay && replay.error().detail == "already-consumed",
				"host launch issued a second descriptor view");
		fixture.read.reset();
		fixture.write.reset();
		auto projection_result = [&]()
		{
			auto view = std::move(*claimed);
			auto projection =
				process_source_closure_launch_adapter_access::consume(std::move(view));
			require(projection.has_value(), "host launch descriptor view was not consumable");
			const auto second_projection =
				process_source_closure_launch_adapter_access::consume(std::move(view));
			require(!second_projection && second_projection.error().detail == "already-consumed",
					"host launch descriptor view was consumed twice");
			return projection;
		}();
		auto projection = std::move(*projection_result);
		source_projection_operation_context context{.read_peer = fixture.read_peer.get(),
													.write_peer = fixture.write_peer.get()};
		auto used = process_source_closure_launch_adapter_access::consume(
			std::move(projection), &context, exercise_source_projection);
		require(used.has_value() && context.invoked,
				"host launch projection did not survive its issuing view");
		const auto projection_replay = process_source_closure_launch_adapter_access::consume(
			std::move(projection), &context, exercise_source_projection);
		require(!projection_replay && projection_replay.error().detail == "already-consumed",
				"host launch projection was consumed twice");
		require(open_fd_count() == before,
				"host launch callback cleanup closed or leaked a foreign replacement");
		require(::fcntl(context.foreign_descriptor, F_GETFD) >= 0,
				"foreign replacement was closed by projection cleanup");
		const char after_cleanup_marker = 's';
		require(::write(context.foreign_descriptor, &after_cleanup_marker, 1) == 1,
				"foreign replacement could not write after launch cleanup");
		char after_cleanup_received{};
		require(::read(context.foreign_peer, &after_cleanup_received, 1) == 1 &&
					after_cleanup_received == after_cleanup_marker,
				"foreign replacement peer failed after launch cleanup");
		(void)::close(context.foreign_descriptor);
		(void)::close(context.foreign_peer);
		require(::fcntl(source_read, F_GETFD) == -1 && errno == EBADF &&
					::fcntl(source_write, F_GETFD) == -1 && errno == EBADF,
				"host fixture cleanup did not close source descriptors");
		fixture.read_peer.reset();
		fixture.write_peer.reset();
		require(open_fd_count() + 4U == before, "host launch left an owned descriptor behind");

		auto cleanup_fixture = make_host_channel();
		const auto cleanup_before = open_fd_count();
		{
			auto cleanup_launch = make_source_launch(cleanup_fixture, '6');
			require(cleanup_launch.has_value(), "projection destructor launch setup failed");
			auto cleanup_view = std::move(*cleanup_launch).claim_launch();
			require(cleanup_view.has_value(), "projection destructor view setup failed");
			cleanup_fixture.read.reset();
			cleanup_fixture.write.reset();
			auto cleanup_projection =
				process_source_closure_launch_adapter_access::consume(std::move(*cleanup_view));
			require(cleanup_projection.has_value(), "projection destructor custody setup failed");
			auto moved_projection = std::move(*cleanup_projection);
			(void)moved_projection;
		}
		require(open_fd_count() + 2U == cleanup_before,
				"projection destructor did not close owned descriptors");
		cleanup_fixture.read_peer.reset();
		cleanup_fixture.write_peer.reset();
		require(open_fd_count() + 4U == cleanup_before,
				"projection destructor left descriptor custody behind");

		auto failure_fixture = make_host_channel();
		const auto failure_before = open_fd_count();
		{
			auto failure_launch = make_source_launch(failure_fixture, '7');
			require(failure_launch.has_value(), "projection failure launch setup failed");
			auto failure_view = std::move(*failure_launch).claim_launch();
			require(failure_view.has_value(), "projection failure view setup failed");
			failure_fixture.read.reset();
			failure_fixture.write.reset();
			auto failure_projection =
				process_source_closure_launch_adapter_access::consume(std::move(*failure_view));
			require(failure_projection.has_value(), "projection failure custody setup failed");
			auto failed = process_source_closure_launch_adapter_access::consume(
				std::move(*failure_projection), nullptr, reject_source_projection);
			require(!failed && failed.error().detail == "injected-failure",
					"projection callback failure was not preserved");
			const auto failure_replay = process_source_closure_launch_adapter_access::consume(
				std::move(*failure_projection), nullptr, reject_source_projection);
			require(!failure_replay && failure_replay.error().detail == "already-consumed",
					"failed projection callback was replayable");
		}
		require(open_fd_count() + 2U == failure_before,
				"projection callback failure did not close owned descriptors");
		failure_fixture.read_peer.reset();
		failure_fixture.write_peer.reset();
		require(open_fd_count() + 4U == failure_before,
				"projection callback failure left descriptor custody behind");

		auto fork_fixture = make_host_channel();
		auto fork_result = make_source_launch(fork_fixture, '2');
		require(fork_result.has_value(), "fork generation launch setup failed");
		auto fork_launch = std::move(*fork_result);
		const auto child = ::fork();
		require(child >= 0, "host launch fork failed");
		if (child == 0)
			::_exit(fork_launch.validate() ? EXIT_FAILURE : EXIT_SUCCESS);
		int status{};
		require(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
					WEXITSTATUS(status) == EXIT_SUCCESS,
				"forked host launch retained the creator generation");
	}

	void check_source_launch_rejections()
	{
		auto fixture = make_host_channel();
		const auto hex = std::string(64U, '3');
		const auto task_v4_digest = "semantic-v2:sha256:" + hex;
		const auto make = [&](const int read_descriptor,
							  const int write_descriptor,
							  const std::uint64_t first_sequence = 0U)
		{
			return make_process_source_closure_launch(read_descriptor,
													  write_descriptor,
													  "task:" + task_v4_digest,
													  "provider-session:sha256:" + hex,
													  task_v4_digest,
													  "source-closure:semantic-v2:sha256:" + hex,
													  "semantic-v2:sha256:" + hex,
													  "semantic-v2:sha256:" + std::string(64U, '4'),
													  "semantic-v2:sha256:" + std::string(64U, '5'),
													  23U,
													  first_sequence);
		};
		auto nonzero_first_sequence = make(fixture.read.get(), fixture.write.get(), 1U);
		require(!nonzero_first_sequence &&
					nonzero_first_sequence.error().detail == "first-sequence",
				"host launch accepted a nonzero first sequence");
		auto duplicate = make(fixture.read.get(), fixture.read.get());
		require(!duplicate && duplicate.error().detail == "duplicate",
				"host launch accepted duplicate descriptors");
		auto reserved = make(3, fixture.write.get());
		require(!reserved && reserved.error().detail == "reserved-descriptor",
				"host launch accepted reserved descriptor");

		const auto no_cloexec = ::fcntl(fixture.write.get(), F_DUPFD, 4);
		require(no_cloexec >= 4, "host no-CLOEXEC setup failed");
		auto clear_cloexec = make(fixture.read.get(), no_cloexec);
		require(!clear_cloexec && clear_cloexec.error().detail == "close-on-exec-clear",
				"host launch accepted a non-CLOEXEC descriptor");
		(void)::close(no_cloexec);

		const auto blocking = ::fcntl(fixture.write.get(), F_DUPFD_CLOEXEC, 4);
		require(blocking >= 4, "host blocking setup failed");
		const auto flags = ::fcntl(blocking, F_GETFL);
		require(flags >= 0 && ::fcntl(blocking, F_SETFL, flags & ~O_NONBLOCK) == 0,
				"host blocking setup could not clear O_NONBLOCK");
		auto blocking_result = make(fixture.read.get(), blocking);
		require(::fcntl(blocking, F_SETFL, flags) == 0, "host blocking setup restore failed");
		(void)::close(blocking);
		require(!blocking_result && blocking_result.error().detail == "blocking-descriptor",
				"host launch accepted a blocking descriptor");

		std::array<int, 2U> pipe_values{-1, -1};
		require(::pipe2(pipe_values.data(), O_NONBLOCK | O_CLOEXEC) == 0, "host pipe setup failed");
		const auto pipe_read = ::fcntl(pipe_values[0], F_DUPFD_CLOEXEC, 4);
		require(pipe_read >= 4, "host pipe promotion failed");
		(void)::close(pipe_values[0]);
		(void)::close(pipe_values[1]);
		auto pipe_result = make(pipe_read, fixture.write.get());
		(void)::close(pipe_read);
		require(!pipe_result && pipe_result.error().detail == "channel-type",
				"host launch accepted a pipe");

		const auto unconnected_raw =
			::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		require(unconnected_raw >= 0, "host unconnected socket setup failed");
		const auto unconnected = promote_host_endpoint(unconnected_raw);
		auto unconnected_result = make(unconnected, fixture.write.get());
		(void)::close(unconnected);
		require(!unconnected_result && unconnected_result.error().detail == "not-connected",
				"host launch accepted an unconnected socket");

		const auto datagram_raw = ::socket(AF_UNIX, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
		require(datagram_raw >= 0, "host datagram setup failed");
		const auto datagram = promote_host_endpoint(datagram_raw);
		auto datagram_result = make(datagram, fixture.write.get());
		(void)::close(datagram);
		require(!datagram_result && datagram_result.error().detail == "socket-type",
				"host launch accepted a datagram socket");
	}

	[[nodiscard]] std::string executable_digest(const std::string& path)
	{
		std::ifstream input(path, std::ios::binary);
		require(input.good(), "executable fixture could not be opened");
		std::vector<std::byte> bytes;
		for (std::array<char, 65536U> buffer{};
			 input.read(buffer.data(), buffer.size()) || input.gcount() > 0;)
		{
			const auto count = static_cast<std::size_t>(input.gcount());
			for (std::size_t index{}; index < count; ++index)
				bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(buffer[index])));
		}
		return content_digest(bytes);
	}

	[[nodiscard]] process_invocation invocation_for(std::vector<std::string> argv,
													const std::string& executable,
													const sandbox_policy& policy)
	{
		process_invocation invocation;
		invocation.argv = std::move(argv);
		invocation.budget.wall_ms = 3000U;
		invocation.budget.cpu_ms = 3000U;
		invocation.budget.address_space_bytes = 256U * 1024U * 1024U;
		invocation.budget.transport_bytes = 1024U * 1024U;
		invocation.budget.output_bytes = 1024U * 1024U;
		invocation.budget.open_files = 64U;
		invocation.budget.subprocesses = 1U;
		invocation.sandbox = {sandbox_assurance::enforced, policy.policy_digest()};
		invocation.expected_binary_digest = executable_digest(executable);
		return invocation;
	}

	[[nodiscard]] auto make_test_binding(const int read_descriptor,
										 const int write_descriptor,
										 std::string task,
										 std::string session,
										 std::string closure,
										 std::string transfer)
	{
		const auto task_v4_digest = task.substr(std::string_view{"task:"}.size());
		const auto closure_id = std::string{"source-closure:"} + closure;
		const auto manifest_digest = "semantic-v2:sha256:" + std::string(64U, 'e');
		return make_process_inherited_channel_binding(read_descriptor,
													  write_descriptor,
													  std::move(task),
													  std::move(session),
													  task_v4_digest,
													  closure_id,
													  std::move(closure),
													  manifest_digest,
													  std::move(transfer),
													  1U,
													  0U);
	}

	void check_binding_validation(const channel_fixture& fixture,
								  const std::string& task,
								  const std::string& session,
								  const std::string& closure,
								  const std::string& transfer)
	{
		auto binding = make_test_binding(
			fixture.read.get(), fixture.write.get(), task, session, closure, transfer);
		require(binding.has_value(), "valid inherited channel binding was rejected");
		require((*binding)->validate().has_value(), "valid inherited channel did not revalidate");
		require(::fcntl(fixture.read.get(), F_GETFD) == 0 &&
					(::fcntl(fixture.read.get(), F_GETFL) & O_NONBLOCK) != 0,
				"read endpoint lost the nonblocking/no-CLOEXEC contract");
		require(::fcntl(fixture.write.get(), F_GETFD) == 0 &&
					(::fcntl(fixture.write.get(), F_GETFL) & O_NONBLOCK) != 0,
				"write endpoint lost the nonblocking/no-CLOEXEC contract");

		auto foreign = std::make_shared<process_inherited_channel_binding>(**binding);
		foreign->task_id = "task:semantic-v2:sha256:" + std::string(64U, 'b');
		auto foreign_result = foreign->validate();
		require(!foreign_result &&
					foreign_result.error().code == "provider.process-channel-foreign" &&
					foreign_result.error().field == "binding",
				"foreign task identity was accepted by an inherited channel binding");

		auto duplicate = make_test_binding(
			fixture.read.get(), fixture.read.get(), task, session, closure, transfer);
		require(!duplicate && duplicate.error().detail == "duplicate",
				"duplicate inherited descriptors were accepted");
		auto reserved = make_test_binding(3, fixture.write.get(), task, session, closure, transfer);
		require(!reserved && reserved.error().detail == "reserved-descriptor",
				"reserved descriptor 3 was accepted");

		const auto close_on_exec = ::fcntl(fixture.write.get(), F_DUPFD_CLOEXEC, 4);
		require(close_on_exec >= 4, "CLOEXEC validation duplicate failed");
		auto cloexec =
			make_test_binding(fixture.read.get(), close_on_exec, task, session, closure, transfer);
		(void)::close(close_on_exec);
		require(!cloexec && cloexec.error().detail == "close-on-exec-set",
				"CLOEXEC inherited descriptor was accepted");

		const auto blocking = ::fcntl(fixture.write.get(), F_DUPFD, 4);
		require(blocking >= 4, "blocking validation duplicate failed");
		const auto blocking_flags = ::fcntl(blocking, F_GETFL);
		require(::fcntl(blocking, F_SETFL, blocking_flags & ~O_NONBLOCK) == 0,
				"blocking validation setup failed");
		auto blocking_result =
			make_test_binding(fixture.read.get(), blocking, task, session, closure, transfer);
		require(::fcntl(blocking, F_SETFL, blocking_flags) == 0,
				"blocking validation restore failed");
		(void)::close(blocking);
		require(!blocking_result && blocking_result.error().detail == "blocking-descriptor",
				"blocking inherited descriptor was accepted");
	}

	void check_process_inheritance_and_cleanup(
		const std::shared_ptr<const process_inherited_channel_binding>& binding,
		const channel_fixture& fixture,
		const sandbox_policy& policy)
	{
		auto port = make_system_provider_process_port();
		require(port != nullptr, "system process port unavailable");

		const auto read_fd = std::to_string(fixture.read.get());
		const auto write_fd = std::to_string(fixture.write.get());
		const std::string script = "r=" + read_fd + "; w=" + write_fd +
			"; test -e /proc/self/fd/$r || exit 10; test -e /proc/self/fd/$w || exit 11; "
			"test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_READ_FD\" = \"" +
			read_fd + "\" || exit 13; test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_WRITE_FD\" = \"" +
			write_fd +
			"\" || exit 14; "
			"test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_TASK_ID\" = \"" +
			binding->task_id +
			"\" || exit 15; test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_SESSION_ID\" = \"" +
			binding->session_id +
			"\" || exit 16; "
			"test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_DIGEST\" = \"" +
			binding->closure_digest +
			"\" || exit 17; test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_TRANSFER_DIGEST\" = \"" +
			binding->transfer_digest +
			"\" || exit 18; "
			"test \"$CXXLENS_PROVIDER_SOURCE_CLOSURE_BINDING_DIGEST\" = \"" +
			binding->binding_digest +
			"\" || exit 19; "
			"i=4; while [ $i -lt 64 ]; do "
			"if [ $i -ne $r ] && [ $i -ne $w ] && [ -e /proc/self/fd/$i ]; then exit 12; fi; "
			"i=$((i+1)); done";
		auto inherited = invocation_for({"/bin/sh", "-c", script}, "/bin/sh", policy);
		inherited.inherited_channel = binding;
		auto inherited_result = port->run(inherited, {});
		require(inherited_result && inherited_result->status == process_status::exited &&
					inherited_result->exit_code == 0,
				"child did not receive exactly the authorized inherited descriptors");

		process_invocation crashing =
			invocation_for({"/bin/sh", "-c", "kill -SEGV $$"}, "/bin/sh", policy);
		crashing.inherited_channel = binding;
		auto crashed = port->run(crashing, {});
		require(crashed && crashed->status == process_status::crashed,
				"crashed worker did not return typed crash status");

		process_invocation sleeping = invocation_for({"/bin/sleep", "30"}, "/bin/sleep", policy);
		sleeping.inherited_channel = binding;
		std::stop_source cancellation;
		const auto started = std::chrono::steady_clock::now();
		std::jthread request_cancel(
			[&cancellation]
			{
				std::this_thread::sleep_for(std::chrono::milliseconds{100});
				cancellation.request_stop();
			});
		auto cancelled = port->run(sleeping, cancellation.get_token());
		const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
			std::chrono::steady_clock::now() - started);
		require(cancelled && cancelled->status == process_status::cancelled &&
					elapsed.count() < 2000,
				"cancelled worker did not terminate and clean up within the bounded deadline");
	}
#endif
} // namespace

int main()
{
#if defined(__linux__) && defined(__GLIBC__)
	const auto policies = builtin_sandbox_policies();
	require(!policies.empty(), "sandbox policy registry is empty");
	auto fixture = make_channel();
	const auto task = "task:semantic-v2:sha256:" + std::string(64U, 'a');
	const auto session = "provider-session:sha256:" + std::string(64U, 'b');
	const auto closure = "semantic-v2:sha256:" + std::string(64U, 'c');
	const auto transfer = "semantic-v2:sha256:" + std::string(64U, 'd');
	check_source_launch_core();
	check_source_launch_rejections();
	check_binding_validation(fixture, task, session, closure, transfer);
	auto binding = make_test_binding(
		fixture.read.get(), fixture.write.get(), task, session, closure, transfer);
	if (!binding)
		require(false,
				"second inherited binding setup failed: " + binding.error().code + ":" +
					binding.error().field + ":" + binding.error().detail);
	check_process_inheritance_and_cleanup(*binding, fixture, policies.front());
	return EXIT_SUCCESS;
#else
	return 77;
#endif
}
