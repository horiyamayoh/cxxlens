#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>

#if defined(__linux__) && defined(__GLIBC__)
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "runtime/gcc_probe_process_port_internal.hpp"

namespace
{
	using cxxlens::sdk::detail::gcc_probe_process_limits;
	using cxxlens::sdk::detail::gcc_probe_process_request;
	using cxxlens::sdk::detail::gcc_probe_process_terminal;
	using cxxlens::sdk::detail::gcc_process_standard_stream_mode;
	using cxxlens::sdk::detail::run_gcc_probe_process;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
			throw std::runtime_error{message};
	}

	[[nodiscard]] std::uint64_t deadline_after(const std::chrono::milliseconds duration)
	{
		return static_cast<std::uint64_t>(
			std::chrono::duration_cast<std::chrono::nanoseconds>(
				std::chrono::steady_clock::now().time_since_epoch() + duration)
				.count());
	}

	[[nodiscard]] gcc_probe_process_limits limits()
	{
		return {32U,
				std::size_t{16U} * 1024U,
				16U,
				std::size_t{16U} * 1024U,
				std::size_t{128U} * 1024U,
				std::uint64_t{128U} * 1024U * 1024U,
				4096U};
	}

	[[nodiscard]] gcc_probe_process_request
	request_for(const std::string& executable,
				const std::filesystem::path& working_directory,
				std::string mode)
	{
		gcc_probe_process_request request{{executable, std::move(mode)},
										  working_directory.string(),
										  {"LC_ALL=C", "CXXLENS_PROBE_TOKEN=explicit"},
										  limits(),
										  deadline_after(std::chrono::seconds{2})};
		// LeakSanitizer cannot inspect the sealed self-exec child in this fixture. The
		// instrumented parent remains leak-checked; child ASan/UBSan/TSan findings stay fatal.
		if (std::getenv("ASAN_OPTIONS") != nullptr)
			request.environment.emplace_back("ASAN_OPTIONS=detect_leaks=0:halt_on_error=1:exitcode="
											 "86:handle_segv=0:symbolize=1");
		for (const std::string_view name : {"UBSAN_OPTIONS", "TSAN_OPTIONS"})
			if (const auto* value = std::getenv(name.data()); value != nullptr)
				request.environment.emplace_back(std::string{name} + '=' + value);
		return request;
	}

#if defined(__linux__) && defined(__GLIBC__)
	[[nodiscard]] int child_main(const std::string_view mode)
	{
		if (mode == "--child-observe")
		{
			const auto* token = std::getenv("CXXLENS_PROBE_TOKEN");
			const auto* path = std::getenv("PATH");
			std::cout << "cwd=" << std::filesystem::current_path().string() << '\n'
					  << "token=" << (token == nullptr ? "absent" : token) << '\n'
					  << "path=" << (path == nullptr ? "absent" : path) << '\n';
			std::cerr << "diagnostic=explicit\n";
			return 0;
		}
		if (mode == "--child-exit")
			return 7;
		if (mode == "--child-inherit")
		{
			const auto* token = std::getenv("CXXLENS_PROBE_TOKEN");
			struct stat standard_output{};
			struct stat standard_error{};
			if (::fstat(STDOUT_FILENO, &standard_output) != 0 ||
				::fstat(STDERR_FILENO, &standard_error) != 0)
				return 12;
			std::ofstream ready{"inherited-ready"};
			ready << (token == nullptr ? "absent" : token) << '\n'
				  << static_cast<std::uintmax_t>(standard_output.st_dev) << ' '
				  << static_cast<std::uintmax_t>(standard_output.st_ino) << '\n'
				  << static_cast<std::uintmax_t>(standard_error.st_dev) << ' '
				  << static_cast<std::uintmax_t>(standard_error.st_ino) << '\n';
			return 11;
		}
		if (mode == "--child-crash")
		{
			(void)::kill(::getpid(), SIGKILL);
			return 127;
		}
		if (mode == "--child-flood")
		{
			std::string block(4096U, 'x');
			for (std::size_t index{}; index < 1024U; ++index)
			{
				std::size_t offset{};
				while (offset < block.size())
				{
					const auto count =
						::write(STDOUT_FILENO, block.data() + offset, block.size() - offset);
					if (count > 0)
						offset += static_cast<std::size_t>(count);
					else if (count < 0 && errno == EINTR)
						continue;
					else
						return 8;
				}
			}
			return 0;
		}
		if (mode == "--child-descendant")
		{
			const auto descendant = ::fork();
			if (descendant < 0)
				return 9;
			if (descendant == 0)
			{
				for (;;)
					::pause();
			}
			std::cerr << "descendant=" << descendant << '\n' << std::flush;
			for (;;)
				::pause();
		}
		if (mode == "--child-wait")
		{
			std::ofstream{"live-ready"} << "ready";
			for (;;)
				::pause();
		}
		return 64;
	}
#endif
} // namespace

int main(const int argc, char** argv)
{
#if defined(__linux__) && defined(__GLIBC__)
	if (argc == 2 && std::string_view{argv[1]}.starts_with("--child-"))
		return child_main(argv[1]);

	const auto root = std::filesystem::temp_directory_path() /
		("cxxlens-gcc-probe-process-" + std::to_string(::getpid()));
	try
	{
		std::filesystem::create_directories(root);
		const std::string executable = std::filesystem::canonical("/proc/self/exe").string();

		auto observed_request = request_for(executable, root, "--child-observe");
		auto observed = run_gcc_probe_process(observed_request);
		require(observed && observed->terminal == gcc_probe_process_terminal::exited &&
					observed->exit_code == 0,
				"explicit probe did not exit successfully");
		require(observed->standard_output ==
						"cwd=" + root.string() + "\ntoken=explicit\npath=absent\n" &&
					observed->standard_error == "diagnostic=explicit\n",
				"probe inherited ambient state or lost exact output");
		require(observed->executable_path == executable &&
					observed->executable_digest.starts_with("sha256:") &&
					observed->executable_bytes == std::filesystem::file_size(executable),
				"probe result did not bind the measured executable");
		auto observed_again = run_gcc_probe_process(observed_request);
		require(observed_again && observed_again->executable_digest == observed->executable_digest,
				"measured executable identity was nondeterministic");

		auto exit_result = run_gcc_probe_process(request_for(executable, root, "--child-exit"));
		require(exit_result && exit_result->terminal == gcc_probe_process_terminal::exited &&
					exit_result->exit_code == 7,
				"nonzero compiler exit was not preserved");
		auto inherited_request = request_for(executable, root, "--child-inherit");
		inherited_request.standard_stream_mode = gcc_process_standard_stream_mode::inherited;
		struct stat parent_standard_output{};
		struct stat parent_standard_error{};
		require(::fstat(STDOUT_FILENO, &parent_standard_output) == 0 &&
					::fstat(STDERR_FILENO, &parent_standard_error) == 0,
				"could not identify parent standard streams");
		auto inherited = run_gcc_probe_process(inherited_request);
		require(inherited && inherited->terminal == gcc_probe_process_terminal::exited &&
					inherited->exit_code == 11 && inherited->standard_output.empty() &&
					inherited->standard_error.empty() &&
					inherited->executable_digest == observed->executable_digest,
				"inherited stream execution lost its exact terminal or executable binding");
		std::ifstream inherited_ready{root / "inherited-ready"};
		std::string inherited_token;
		std::uintmax_t child_stdout_device{};
		std::uintmax_t child_stdout_inode{};
		std::uintmax_t child_stderr_device{};
		std::uintmax_t child_stderr_inode{};
		inherited_ready >> inherited_token >> child_stdout_device >> child_stdout_inode >>
			child_stderr_device >> child_stderr_inode;
		require(
			inherited_token == "explicit" &&
				child_stdout_device == static_cast<std::uintmax_t>(parent_standard_output.st_dev) &&
				child_stdout_inode == static_cast<std::uintmax_t>(parent_standard_output.st_ino) &&
				child_stderr_device == static_cast<std::uintmax_t>(parent_standard_error.st_dev) &&
				child_stderr_inode == static_cast<std::uintmax_t>(parent_standard_error.st_ino),
			"inherited stream execution lost its explicit cwd, environment, or streams");
		auto crash_result = run_gcc_probe_process(request_for(executable, root, "--child-crash"));
		require(crash_result && crash_result->terminal == gcc_probe_process_terminal::crashed &&
					crash_result->signal == SIGKILL,
				"signal terminal was not typed");

		auto flood_request = request_for(executable, root, "--child-flood");
		flood_request.limits.maximum_output_bytes = 1024U;
		auto flood = run_gcc_probe_process(flood_request);
		require(flood && flood->terminal == gcc_probe_process_terminal::output_limit,
				"combined output limit did not terminate the process group");

		require(::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0, "could not establish test subreaper");
		auto timeout_request = request_for(executable, root, "--child-descendant");
		timeout_request.absolute_wall_deadline_ns = deadline_after(std::chrono::seconds{3});
		auto timed_out = run_gcc_probe_process(timeout_request);
		require(timed_out && timed_out->terminal == gcc_probe_process_terminal::timed_out,
				"absolute deadline did not terminate the probe");
		const std::string prefix{"descendant="};
		const auto position = timed_out->standard_error.find(prefix);
		require(position != std::string::npos,
				"descendant identity was not observed before timeout");
		const auto descendant = static_cast<pid_t>(
			std::stol(timed_out->standard_error.substr(position + prefix.size())));
		int descendant_status{};
		require(::waitpid(descendant, &descendant_status, 0) == descendant &&
					WIFSIGNALED(descendant_status) && WTERMSIG(descendant_status) == SIGKILL,
				"deadline cleanup did not kill the descendant process");

		std::stop_source stopped;
		stopped.request_stop();
		auto cancelled = run_gcc_probe_process(observed_request, stopped.get_token());
		require(cancelled && cancelled->terminal == gcc_probe_process_terminal::cancelled &&
					cancelled->executable_digest.empty(),
				"prelaunch cancellation did not stop before executable measurement");
		std::stop_source live_stop;
		auto live_request = request_for(executable, root, "--child-wait");
		live_request.absolute_wall_deadline_ns = deadline_after(std::chrono::seconds{5});
		auto live_future = std::async(std::launch::async,
									  [&live_request, token = live_stop.get_token()]
									  {
										  return run_gcc_probe_process(live_request, token);
									  });
		const auto ready_deadline = std::chrono::steady_clock::now() + std::chrono::seconds{3};
		while (!std::filesystem::exists(root / "live-ready") &&
			   std::chrono::steady_clock::now() < ready_deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds{5});
		require(std::filesystem::exists(root / "live-ready"),
				"live cancellation fixture did not reach the spawned process");
		live_stop.request_stop();
		auto live_cancelled = live_future.get();
		require(static_cast<bool>(live_cancelled), "live cancellation returned an API error");
		require(live_cancelled->terminal == gcc_probe_process_terminal::cancelled &&
					!live_cancelled->executable_digest.empty(),
				"live cancellation terminal=" +
					std::to_string(static_cast<int>(live_cancelled->terminal)) +
					" digest-bytes=" + std::to_string(live_cancelled->executable_digest.size()));

		auto missing_request = request_for((root / "missing").string(), root, "--child-exit");
		auto missing = run_gcc_probe_process(missing_request);
		require(missing && missing->terminal == gcc_probe_process_terminal::launch_failed &&
					missing->failure_stage == "executable-open",
				"missing executable did not produce a typed launch terminal");
		auto missing_directory =
			request_for(executable, root / "missing-directory", "--child-exit");
		auto setup_failure = run_gcc_probe_process(missing_directory);
		require(
			setup_failure && setup_failure->terminal == gcc_probe_process_terminal::launch_failed &&
				setup_failure->failure_stage == "spawn" && !setup_failure->failure_detail.empty(),
			"spawn setup failure did not retain its typed OS detail");
		auto duplicate_environment = observed_request;
		duplicate_environment.environment.emplace_back("LC_ALL=other");
		auto duplicate = run_gcc_probe_process(duplicate_environment);
		require(!duplicate && duplicate.error().field == "environment" &&
					duplicate.error().detail == "duplicate-name",
				"duplicate explicit environment authority was accepted");
		auto argument_limit = observed_request;
		argument_limit.limits.maximum_argument_count = 1U;
		auto too_many_arguments = run_gcc_probe_process(argument_limit);
		require(!too_many_arguments && too_many_arguments.error().field == "invocation",
				"argument count bound was not enforced");
		auto unknown_stream_mode = observed_request;
		unknown_stream_mode.standard_stream_mode =
			static_cast<gcc_process_standard_stream_mode>(255U);
		auto rejected_stream_mode = run_gcc_probe_process(unknown_stream_mode);
		require(!rejected_stream_mode &&
					rejected_stream_mode.error().field == "standard-stream-mode",
				"unknown standard stream mode was accepted");
		auto image_limit = observed_request;
		image_limit.limits.maximum_executable_image_bytes = 1U;
		image_limit.absolute_wall_deadline_ns = deadline_after(std::chrono::seconds{2});
		auto oversized_image = run_gcc_probe_process(image_limit);
		require(oversized_image &&
					oversized_image->terminal == gcc_probe_process_terminal::launch_failed &&
					oversized_image->failure_stage == "executable-size",
				"executable image bound was not enforced");
		auto path_limit = observed_request;
		path_limit.limits.maximum_canonical_path_bytes = 1U;
		path_limit.absolute_wall_deadline_ns = deadline_after(std::chrono::seconds{2});
		auto oversized_path = run_gcc_probe_process(path_limit);
		require(oversized_path &&
					oversized_path->terminal == gcc_probe_process_terminal::launch_failed &&
					oversized_path->failure_stage == "canonical-path",
				"canonical path bound was not enforced");

		std::filesystem::remove_all(root);
		std::cout << "GCC probe process port tests passed\n";
		return EXIT_SUCCESS;
	}
	catch (const std::exception& exception)
	{
		std::filesystem::remove_all(root);
		std::cerr << exception.what() << '\n';
		return EXIT_FAILURE;
	}
#else
	(void)argc;
	(void)argv;
	gcc_probe_process_request request{
		{"/unsupported"}, "/", {}, limits(), std::numeric_limits<std::uint64_t>::max()};
	auto unavailable = run_gcc_probe_process(request);
	require(unavailable && unavailable->terminal == gcc_probe_process_terminal::unavailable,
			"unsupported platform did not return structured unavailable");
	return EXIT_SUCCESS;
#endif
}
