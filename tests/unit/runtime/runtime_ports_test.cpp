#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include <sys/types.h>
#include <unistd.h>

#include "runtime/dynamic_library_port.hpp"
#include "runtime/filesystem_port.hpp"
#include "runtime/hash_port.hpp"
#include "runtime/process_port.hpp"
#include "runtime/time_port.hpp"

namespace
{
	using namespace cxxlens::detail::runtime;

	auto check(const bool condition, const std::string& message) -> bool
	{
		if (!condition)
		{
			std::cerr << message << '\n';
		}
		return condition;
	}

	auto context(std::string operation, const std::uint64_t call_index = 0U) -> request_context
	{
		request_context result;
		result.operation = std::move(operation);
		result.call_index = call_index;
		return result;
	}

	auto make_process_request(std::vector<std::string> argv) -> process_request
	{
		process_request result;
		result.argv = std::move(argv);
		return result;
	}

	auto read_pid(const std::filesystem::path& file) -> std::optional<pid_t>
	{
		std::ifstream input{file};
		long value{};
		if (!(input >> value) || value <= 0)
			return std::nullopt;
		return static_cast<pid_t>(value);
	}

	auto process_is_live(const pid_t process) -> bool
	{
#if defined(__linux__)
		std::ifstream status{"/proc/" + std::to_string(process) + "/stat"};
		std::string ignored;
		char state{};
		if (status >> ignored >> ignored >> state && state == 'Z')
			return false;
#endif
		errno = 0;
		return ::kill(process, 0) == 0 || errno == EPERM;
	}

	auto descendant_was_cleaned(const std::filesystem::path& pid_file) -> bool
	{
		for (std::size_t attempt = 0U; attempt < 100U; ++attempt)
		{
			if (const auto process = read_pid(pid_file); process && !process_is_live(*process))
				return true;
			std::this_thread::sleep_for(std::chrono::milliseconds{10});
		}
		return false;
	}

	auto test_filesystem_contract() -> bool
	{
		memory_filesystem_adapter forward;
		forward.add("root/z.cpp", "z").add("root/a.cpp", "a");
		memory_filesystem_adapter reversed;
		reversed.add("root/z.cpp", "z").add("root/a.cpp", "a").reverse_enumeration(true);
		auto first = forward.list("root", context("fs.list"));
		auto second = reversed.list("root", context("fs.list"));
		bool passed = check(first.has_value() && second.has_value(), "memory list failed");
		if (first && second)
		{
			passed &= check(canonical_path_order(*first) == canonical_path_order(*second),
							"enumeration changed canonical result");
		}
		passed &= check(is_within_root("root", "root/a.cpp"), "contained path rejected");
		passed &= check(!is_within_root("root", "root/../escape"), "traversal path accepted");
		passed &= check(forward.read("root/a.cpp", context("fs.read")).value_or("") == "a",
						"memory read failed");
		passed &= check(forward.remove("root/a.cpp", context("fs.remove")).value_or(false) &&
							!forward.stat("root/a.cpp", context("fs.stat-removed")).value().exists,
						"filesystem remove port failed");
		auto expired = context("fs.expired");
		expired.deadline = std::chrono::steady_clock::time_point::min();
		passed &=
			check(forward.read("root/a.cpp", expired).error().status == runtime_status::timed_out,
				  "filesystem deadline did not propagate");
		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled_library = context("dynamic-library.cancelled");
		cancelled_library.cancellation = cancellation.get_token();
		system_dynamic_library_adapter libraries;
		passed &= check(libraries.open({"not-loaded"}, cancelled_library).error().status ==
							runtime_status::cancelled,
						"dynamic-library cancellation did not propagate");
		standard_filesystem_adapter standard;
		const auto directory =
			std::filesystem::temp_directory_path() / "cxxlens-runtime-port-directory" / "nested";
		std::filesystem::remove_all(directory.parent_path());
		passed &= check(standard.create_directories(directory, context("fs.production.mkdir"))
								.value_or(false) &&
							std::filesystem::is_directory(directory),
						"production filesystem directory creation failed");
		std::filesystem::remove_all(directory.parent_path());
		const std::filesystem::path this_file{__FILE__};
		const auto production_read = standard.read(this_file, context("fs.production.read"));
		passed &= check(production_read &&
							production_read->find("test_filesystem_contract") != std::string::npos,
						"production filesystem read failed");
		passed &= check(standard.stat(this_file, context("fs.production.stat")).value().regular,
						"production filesystem stat failed");
		passed &=
			check(standard.canonicalize(this_file, context("fs.production.canonical")).has_value(),
				  "production filesystem canonicalize failed");
		return passed;
	}

	auto test_hash_and_time_contract() -> bool
	{
		fnv1a_hash_adapter hashes;
		fixed_time_adapter fixed{std::chrono::system_clock::time_point{std::chrono::seconds{7}},
								 std::chrono::steady_clock::time_point{std::chrono::seconds{9}}};
		real_time_adapter real;
		const std::string payload = "semantic payload";
		const auto first = hashes.calculate(make_hash_request("test.semantic.v1", payload),
											context("hash.calculate", 1U));
		(void)fixed.wall_now();
		(void)fixed.steady_now();
		(void)real.wall_now();
		(void)real.steady_now();
		const auto second = hashes.calculate(make_hash_request("test.semantic.v1", payload),
											 context("hash.calculate", 2U));
		bool passed = check(first.has_value() && second.has_value() && *first == *second,
							"clock selection changed semantic digest");
		const hash_request invalid{"", 0U, "", {}};
		passed &= check(!hashes.calculate(invalid, context("hash.invalid")),
						"unversioned hash request accepted");
		memory_filesystem_adapter files;
		files.add("root/input", payload);
		passed &=
			check(digest_file(files, hashes, "root/input", "test.file.v1", context("fs.digest"))
					  .has_value(),
				  "filesystem digest failed");
		return passed;
	}

	auto test_fault_schedule() -> bool
	{
		fault_plan faults;
		faults.fail("fs.read", 1U, runtime_status::permission_denied)
			.fail("fs.read", 2U, runtime_status::missing)
			.fail("fs.read", 3U, runtime_status::short_read)
			.fail("fs.read", 4U, runtime_status::resource_exhausted)
			.fail("process.run", 5U, runtime_status::timed_out)
			.fail("process.run", 6U, runtime_status::output_limit);
		memory_filesystem_adapter files{faults};
		files.add("root/file", "data");
		std::vector<runtime_status> observed(4U);
		std::vector<std::jthread> workers;
		for (std::uint64_t index = 1U; index <= 4U; ++index)
		{
			workers.emplace_back(
				[&, index]
				{
					const auto result = files.read("root/file", context("fs.read", index));
					observed.at(static_cast<std::size_t>(index - 1U)) = result.error().status;
				});
		}
		workers.clear();
		bool passed = check(observed ==
								std::vector{runtime_status::permission_denied,
											runtime_status::missing,
											runtime_status::short_read,
											runtime_status::resource_exhausted},
							"fault schedule depended on thread completion order");
		fake_process_adapter process{{}, faults};
		auto request = make_process_request({"literal"});
		passed &= check(process.run(request, context("process.run", 5U)).error().status ==
							runtime_status::timed_out,
						"timeout injection failed");
		passed &= check(process.run(request, context("process.run", 6U)).error().status ==
							runtime_status::output_limit,
						"output-limit injection failed");
		return passed;
	}

	auto test_process_contract() -> bool
	{
		argv_process_adapter process;
		auto hostile =
			make_process_request({"/usr/bin/printf", "%s", ";$(touch /tmp/cxxlens-shell)\n"});
		hostile.timeout = std::chrono::seconds{2};
		const auto literal = process.run(hostile, context("process.hostile"));
		bool passed = check(literal && literal->standard_output == ";$(touch /tmp/cxxlens-shell)\n",
							"shell metacharacters were not literal argv data");
		auto sleeping = make_process_request({"/bin/sleep", "1"});
		sleeping.timeout = std::chrono::milliseconds{10};
		const auto timed = process.run(sleeping, context("process.timeout"));
		passed &= check(!timed && timed.error().status == runtime_status::timed_out,
						"process timeout did not propagate");
		process_result huge_result;
		huge_result.standard_output = std::string(64U, 'x');
		fake_process_adapter huge{huge_result};
		auto limited_context = context("process.limit");
		limited_context.output_limit = 8U;
		passed &= check(huge.run(make_process_request({"fake"}), limited_context).error().status ==
							runtime_status::output_limit,
						"huge output limit did not propagate");
		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled_context = context("process.cancel");
		cancelled_context.cancellation = cancellation.get_token();
		passed &=
			check(huge.run(make_process_request({"fake"}), cancelled_context).error().status ==
					  runtime_status::cancelled,
				  "cancellation did not propagate");
		auto deadline_context = context("process.deadline");
		deadline_context.deadline = std::chrono::steady_clock::time_point::min();
		passed &= check(huge.run(make_process_request({"fake"}), deadline_context).error().status ==
							runtime_status::timed_out,
						"deadline did not propagate");
		return passed;
	}

	auto test_production_process_fail_closed() -> bool
	{
		bool passed = true;
		const auto python = std::string{"/usr/bin/python3"};

		argv_process_adapter delayed{{.drain_only_after_exit = true}};
		auto final_burst =
			make_process_request({python, "-c", "import os; os.write(1, b'x' * 4096)"});
		final_burst.timeout = std::chrono::seconds{2};
		auto limited = context("process.production.final-limit");
		limited.output_limit = 64U;
		const auto final_limit = delayed.run(final_burst, limited);
		passed &= check(!final_limit && final_limit.error().status == runtime_status::output_limit,
						"final drain output limit was accepted");

		argv_process_adapter process;
		auto combined = make_process_request(
			{python, "-c", "import os; os.write(1,b'12345678'); os.write(2,b'abcdefgh')"});
		combined.timeout = std::chrono::seconds{2};
		auto combined_context = context("process.production.combined-limit");
		combined_context.output_limit = 12U;
		const auto combined_limit = process.run(combined, combined_context);
		passed &=
			check(!combined_limit && combined_limit.error().status == runtime_status::output_limit,
				  "stdout and stderr did not share an output limit");

		argv_process_adapter final_read_error{{.final_drain_error = EIO}};
		const auto read_failure = final_read_error.run(make_process_request({"/bin/true"}),
													   context("process.production.final-read"));
		passed &= check(!read_failure &&
							read_failure.error().status == runtime_status::platform_failure &&
							read_failure.error().platform_code == EIO,
						"final drain read error was accepted");

		argv_process_adapter nonblocking_error{{.nonblocking_setup_error = EIO}};
		const auto setup_failure = nonblocking_error.run(make_process_request({"/bin/true"}),
														 context("process.production.nonblocking"));
		passed &= check(!setup_failure &&
							setup_failure.error().status == runtime_status::platform_failure &&
							setup_failure.error().platform_code == EIO,
						"nonblocking setup failure was accepted");

		const auto working_directory =
			std::filesystem::temp_directory_path() / "cxxlens-process-production-cwd";
		std::filesystem::remove_all(working_directory);
		std::filesystem::create_directories(working_directory);
		auto projected = make_process_request(
			{python,
			 "-c",
			 "import os; print(os.getcwd()); print(os.environ['CXXLENS_PROCESS_TEST'])"});
		projected.working_directory = working_directory;
		projected.environment = {{"CXXLENS_PROCESS_TEST", "override-value"}};
		const auto projection = process.run(projected, context("process.production.cwd-env"));
		passed &= check(projection &&
							projection->standard_output ==
								working_directory.generic_string() + "\noverride-value\n",
						"working directory or environment override was not applied");

		auto invalid_cwd = make_process_request({"/bin/true"});
		invalid_cwd.working_directory = working_directory / "missing";
		const auto cwd_failure =
			process.run(invalid_cwd, context("process.production.invalid-cwd"));
		passed &=
			check(!cwd_failure && cwd_failure.error().status == runtime_status::platform_failure &&
					  cwd_failure.error().platform_code != 0,
				  "invalid cwd did not return a platform failure");
		auto invalid_environment = make_process_request({"/bin/true"});
		invalid_environment.environment = {{"INVALID=NAME", "value"}};
		passed &= check(
			process.run(invalid_environment, context("process.production.invalid-environment"))
					.error()
					.status == runtime_status::invalid_request,
			"invalid environment name was accepted");
		const auto exec_failure =
			process.run(make_process_request({"/cxxlens/definitely-missing-executable"}),
						context("process.production.invalid-exec"));
		passed &= check(!exec_failure &&
							exec_failure.error().status == runtime_status::platform_failure &&
							exec_failure.error().platform_code == ENOENT,
						"invalid executable did not return spawn evidence");
		std::filesystem::remove_all(working_directory);
		return passed;
	}

	auto test_concurrent_process_launch() -> bool
	{
		argv_process_adapter process;
		std::atomic_bool passed{true};
		std::vector<std::jthread> workers;
		for (std::size_t worker = 0U; worker < 8U; ++worker)
			workers.emplace_back(
				[&]
				{
					for (std::size_t iteration = 0U; iteration < 16U; ++iteration)
					{
						auto request = make_process_request({"/usr/bin/printf", "ok"});
						request.timeout = std::chrono::seconds{2};
						const auto launched =
							process.run(request, context("process.production.concurrent"));
						if (!launched || launched->exit_code != 0 ||
							launched->standard_output != "ok")
							passed.store(false);
					}
				});
		workers.clear();
		return check(passed.load(), "concurrent production launches were not reliable");
	}

	auto test_process_group_cleanup() -> bool
	{
		const auto directory =
			std::filesystem::temp_directory_path() / "cxxlens-process-group-cleanup";
		std::filesystem::remove_all(directory);
		std::filesystem::create_directories(directory);
		argv_process_adapter process;
		bool passed = true;
		for (const auto cancelled : {false, true})
		{
			const auto pid_file = directory / (cancelled ? "cancelled.pid" : "timed.pid");
			auto request = make_process_request({"/bin/sh",
												 "-c",
												 "sleep 30 & child=$!; printf '%s' \"$child\" > " +
													 pid_file.generic_string() + "; wait"});
			request.timeout = cancelled ? std::chrono::seconds{5} : std::chrono::milliseconds{200};
			auto run_context = context(cancelled ? "process.production.cancel-group"
												 : "process.production.timeout-group");
			std::stop_source cancellation;
			std::jthread canceller;
			if (cancelled)
			{
				run_context.cancellation = cancellation.get_token();
				canceller =
					std::jthread{[&]
								 {
									 std::this_thread::sleep_for(std::chrono::milliseconds{100});
									 cancellation.request_stop();
								 }};
			}
			const auto stopped = process.run(request, run_context);
			const auto expected = cancelled ? runtime_status::cancelled : runtime_status::timed_out;
			passed &= check(!stopped && stopped.error().status == expected,
							"timeout/cancellation status did not propagate");
			passed &= check(descendant_was_cleaned(pid_file),
							"timeout/cancellation left a live descendant");
		}
		std::filesystem::remove_all(directory);
		return passed;
	}
} // namespace

auto main() -> int
{
	const bool passed = test_filesystem_contract() && test_hash_and_time_contract() &&
		test_fault_schedule() && test_process_contract() && test_production_process_fail_closed() &&
		test_concurrent_process_launch() && test_process_group_cleanup();
	return passed ? 0 : 1;
}
