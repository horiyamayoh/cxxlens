#include <algorithm>
#include <chrono>
#include <filesystem>
#include <iostream>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

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
		auto expired = context("fs.expired");
		expired.deadline = std::chrono::steady_clock::time_point::min();
		passed &=
			check(forward.read("root/a.cpp", expired).error().status == runtime_status::timed_out,
				  "filesystem deadline did not propagate");
		standard_filesystem_adapter standard;
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
} // namespace

auto main() -> int
{
	const bool passed = test_filesystem_contract() && test_hash_and_time_contract() &&
		test_fault_schedule() && test_process_contract();
	return passed ? 0 : 1;
}
