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
#include <utility>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "sdk/provider_runtime_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
	using cxxlens::sdk::provider::detail::make_process_inherited_channel_binding;
	using cxxlens::sdk::provider::detail::process_inherited_channel_binding;

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

	void check_binding_validation(const channel_fixture& fixture,
								  const std::string& task,
								  const std::string& session,
								  const std::string& closure,
								  const std::string& transfer)
	{
		auto binding = make_process_inherited_channel_binding(
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

		auto duplicate = make_process_inherited_channel_binding(
			fixture.read.get(), fixture.read.get(), task, session, closure, transfer);
		require(!duplicate && duplicate.error().detail == "duplicate",
				"duplicate inherited descriptors were accepted");
		auto reserved = make_process_inherited_channel_binding(
			3, fixture.write.get(), task, session, closure, transfer);
		require(!reserved && reserved.error().detail == "reserved-descriptor",
				"reserved descriptor 3 was accepted");

		const auto close_on_exec = ::fcntl(fixture.write.get(), F_DUPFD_CLOEXEC, 4);
		require(close_on_exec >= 4, "CLOEXEC validation duplicate failed");
		auto cloexec = make_process_inherited_channel_binding(
			fixture.read.get(), close_on_exec, task, session, closure, transfer);
		(void)::close(close_on_exec);
		require(!cloexec && cloexec.error().detail == "close-on-exec-set",
				"CLOEXEC inherited descriptor was accepted");

		const auto blocking = ::fcntl(fixture.write.get(), F_DUPFD, 4);
		require(blocking >= 4, "blocking validation duplicate failed");
		const auto blocking_flags = ::fcntl(blocking, F_GETFL);
		require(::fcntl(blocking, F_SETFL, blocking_flags & ~O_NONBLOCK) == 0,
				"blocking validation setup failed");
		auto blocking_result = make_process_inherited_channel_binding(
			fixture.read.get(), blocking, task, session, closure, transfer);
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
	check_binding_validation(fixture, task, session, closure, transfer);
	auto binding = make_process_inherited_channel_binding(
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
