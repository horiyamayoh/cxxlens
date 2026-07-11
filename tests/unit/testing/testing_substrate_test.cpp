#include <array>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#include <cxxlens/testing.hpp>

#include "runtime/time_port.hpp"

namespace
{
	[[nodiscard]] bool check(const bool condition, const std::string& message)
	{
		if (!condition)
			std::cerr << message << '\n';
		return condition;
	}

	[[nodiscard]] bool test_fixture_canonicalization()
	{
		using namespace cxxlens::testing;
		fixture_variant debug{"debug", {"-DDEBUG"}, {{"MODE", "debug"}}};
		fixture_variant release{"release", {"-DNDEBUG"}, {{"MODE", "release"}}};
		const auto first = workspace_fixture::cpp("#include \"a.hpp\"\nint main(){return f();}")
							   .add_file("z.cpp", "int z;")
							   .add_header("a.hpp", "int f();")
							   .add_variant(release)
							   .add_variant(debug)
							   .generated("z.cpp")
							   .system_header("a.hpp")
							   .argument("-Wall");
		const auto second = workspace_fixture::cpp("#include \"a.hpp\"\nint main(){return f();}")
								.add_header("a.hpp", "int f();")
								.add_file("z.cpp", "int z;")
								.add_variant(debug)
								.add_variant(release)
								.system_header("a.hpp")
								.generated("z.cpp")
								.argument("-Wall");
		auto first_bundle = first.materialize("/one/root");
		auto second_bundle = second.materialize("/other/root");
		bool passed = check(first_bundle && second_bundle, "valid fixture failed to materialize");
		if (!first_bundle || !second_bundle)
			return false;
		passed &= check(first_bundle.value().to_json() == second_bundle.value().to_json(),
						"fixture insertion order or root changed canonical bytes");
		passed &= check(first_bundle.value().compilation_database_json ==
							second_bundle.value().compilation_database_json,
						"compile database order is unstable");
		passed &= check(first_bundle.value().to_json().find("/one/root") == std::string::npos,
						"absolute fixture root leaked into semantic projection");
		passed &= check(!first.add_file("../escape.cpp", "").materialize(),
						"fixture traversal path was accepted");
		passed &= check(!first.add_header("a.hpp", "different").materialize(),
						"duplicate fixture file was accepted");
		return passed;
	}

	[[nodiscard]] cxxlens::testing::result_observation partial_observation()
	{
		cxxlens::testing::result_observation observation;
		observation.coverage.request({"compile-unit", "cu_test"});
		observation.coverage.classify({"compile-unit",
									   "cu_test",
									   cxxlens::coverage_state::unresolved,
									   "semantic.ambiguous-symbol"});
		cxxlens::unresolved unresolved;
		unresolved.kind = cxxlens::unresolved_kind::ambiguous_symbol;
		unresolved.stable_code = "semantic.ambiguous-symbol";
		unresolved.summary = "ambiguous";
		observation.unresolved_items.push_back(std::move(unresolved));
		return observation;
	}

	[[nodiscard]] bool test_result_assertions()
	{
		using namespace cxxlens::testing;
		result_observation empty;
		bool passed = check(result_assertion{}
								.has_exactly(0)
								.has_no_errors()
								.is_complete()
								.check(empty)
								.has_value(),
							"success-empty assertion failed");
		auto partial = partial_observation();
		passed &= check(result_assertion{}
							.is_partial_with(cxxlens::unresolved_kind::ambiguous_symbol)
							.check(partial)
							.has_value(),
						"partial unresolved assertion failed");
		passed &= check(!result_assertion{}.is_complete().check(partial),
						"partial result was treated as complete");
		result_observation failed;
		cxxlens::error cancelled;
		cancelled.code.value = "core.cancelled";
		cancelled.message = "cancelled";
		failed.failure = std::move(cancelled);
		auto failure = result_assertion{}.has_no_errors().check(failed);
		passed &=
			check(!failure && failure.error().code.value == "testing.assertion-failed" &&
					  failure.error().attributes.at("original_error_code") == "core.cancelled",
				  "failed error was not preserved by assertion");

		const auto golden = std::filesystem::temp_directory_path() / "cxxlens-testing-golden.json";
		{
			std::ofstream output{golden};
			output << "{\"schema\":\"x\"}";
		}
		empty.canonical_json = "{\"schema\":\"x\"}";
		passed &= check(result_assertion{}.json_matches(golden).check(empty).has_value(),
						"matching golden was rejected");
		std::error_code ignored;
		std::filesystem::remove(golden, ignored);
		return passed;
	}

	[[nodiscard]] bool test_schema_and_golden()
	{
		using namespace cxxlens::testing;
		auto bundle = workspace_fixture::cpp("int main(){}").materialize();
		bool passed = check(
			bundle &&
				assert_schema_conforms("cxxlens.testing.fixture.v1", bundle.value().to_json()),
			"valid fixture schema was rejected");
		auto missing = assert_schema_conforms(
			"cxxlens.testing.fixture.v1",
			"{\"schema\":\"cxxlens.testing.fixture.v1\",\"library_version\":\"0.1.0\"}");
		passed &= check(!missing && missing.error().code.value == "testing.schema-mismatch" &&
							missing.error().attributes.at("field") == "$.arguments",
						"schema failure path is not deterministic");
		const std::string original =
			"{\"id\":\"fact_/checkout/keep\",\"path\":\"/checkout/src/a.cpp\","
			"\"range\":{\"begin\":3,\"end\":5},\"reason\":\"r.code\","
			"\"timestamp\":123,\"variant\":\"v1\"}";
		auto normalized = normalize_golden(original, {"/checkout", "/checkout/build", "/resource"});
		passed &= check(normalized &&
							normalized.value().find("$WORKSPACE/src/a.cpp") != std::string::npos &&
							normalized.value().find("<runtime>") != std::string::npos,
						"allowed root/runtime metadata was not normalized");
		for (const auto& visible :
			 {"fact_/checkout/keep", "\"begin\":3", "\"end\":5", "r.code", "\"variant\":\"v1\""})
			passed &= check(normalized && normalized.value().find(visible) != std::string::npos,
							"golden normalizer hid semantic identity/range/reason data");
		passed &= check(!normalize_golden("{bad", {}), "malformed golden JSON was accepted");
		return passed;
	}

	[[nodiscard]] bool test_property_and_determinism()
	{
		using namespace cxxlens::testing;
		const property_options options{41U, 5U, std::nullopt};
		auto report = check_property(
			options,
			[](const std::uint64_t seed, const std::size_t index)
			{
				return seed + index + 59U;
			},
			[](const std::uint64_t value)
			{
				return value < 10U;
			},
			[](const std::uint64_t value)
			{
				return std::vector<std::uint64_t>{value / 2U};
			},
			[](const std::uint64_t value)
			{
				return std::to_string(value);
			});
		bool passed = check(!report.passed() && report.failure->replay == "seed=41;case=0" &&
								report.failure->minimal_input == "12",
							"property failure did not report replayable seed/case/input");
		auto replay = check_property(
			property_options{41U, 100U, 0U},
			[](const std::uint64_t seed, const std::size_t index)
			{
				return seed + index + 59U;
			},
			[](const std::uint64_t value)
			{
				return value < 10U;
			},
			[](const std::uint64_t value)
			{
				return std::vector<std::uint64_t>{value / 2U};
			},
			[](const std::uint64_t value)
			{
				return std::to_string(value);
			});
		passed &= check(!replay.passed() && replay.failure->minimal_input == "12",
						"property case did not replay");
		auto fixture = workspace_fixture::cpp("int main(){}");
		auto stable = check_determinism(fixture,
										[](const fixture_bundle& bundle,
										   std::size_t,
										   std::uint64_t) -> cxxlens::result<std::string>
										{
											return bundle.to_json();
										});
		passed &= check(stable && stable.value().stable() && stable.value().executions == 18U,
						"stable determinism matrix failed");
		auto unstable = check_determinism(fixture,
										  [](const fixture_bundle&,
											 const std::size_t jobs,
											 std::uint64_t) -> cxxlens::result<std::string>
										  {
											  return std::to_string(jobs);
										  });
		passed &= check(unstable && !unstable.value().stable(),
						"operationally unstable output was not detected");
		return passed;
	}

	[[nodiscard]] bool test_fault_replay()
	{
		using cxxlens::testing::fault_target;
		const auto make_failure = []
		{
			cxxlens::error failure;
			failure.code.value = "io.retryable";
			failure.message = "injected";
			return failure;
		};
		auto plan = cxxlens::testing::fault_plan::make(
			{{fault_target::filesystem, "fixture.file", 1U, make_failure()},
			 {fault_target::process, "fixture.process", 2U, make_failure()},
			 {fault_target::hash, "fixture.hash", 3U, make_failure()},
			 {fault_target::time, "fixture.time", 4U, make_failure()}});
		if (!plan)
			return check(false, "valid public fault plan was rejected");
		const std::array requests{std::tuple{fault_target::filesystem, "fixture.file", 1U},
								  std::tuple{fault_target::process, "fixture.process", 2U},
								  std::tuple{fault_target::hash, "fixture.hash", 3U},
								  std::tuple{fault_target::time, "fixture.time", 4U}};
		std::array<bool, 4U> observed{};
		std::vector<std::jthread> workers;
		for (std::size_t index = 0U; index < requests.size(); ++index)
			workers.emplace_back(
				[&, index]
				{
					const auto& [target, operation, call_index] = requests.at(index);
					observed.at(index) = !plan.value().probe(target, operation, call_index);
				});
		workers.clear();
		bool passed = true;
		for (std::size_t index = 0U; index < requests.size(); ++index)
		{
			const auto& [target, operation, call_index] = requests.at(index);
			auto injected = plan.value().probe(target, operation, call_index);
			cxxlens::testing::result_observation observation;
			if (!injected)
				observation.failure = injected.error();
			auto asserted = cxxlens::testing::result_assertion{}.has_no_errors().check(observation);
			passed &=
				check(observed.at(index) && !asserted &&
						  asserted.error().attributes.at("original_error_code") == "io.retryable",
					  "deterministic port fault did not reach typed assertion");
		}
		cxxlens::detail::runtime::fixed_time_adapter time{
			std::chrono::system_clock::time_point{std::chrono::seconds{7}},
			std::chrono::steady_clock::time_point{std::chrono::seconds{9}}};
		passed &=
			check(time.wall_now() == std::chrono::system_clock::time_point{std::chrono::seconds{7}},
				  "fixed time replay changed");
		return passed;
	}
} // namespace

int main()
{
	const bool passed = test_fixture_canonicalization() && test_result_assertions() &&
		test_schema_and_golden() && test_property_and_determinism() && test_fault_replay();
	return passed ? 0 : 1;
}
