#include "workspace/scheduler.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <set>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "workspace/frontend_scheduler_worker.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;
	using namespace cxxlens::detail::scheduling;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	void write(const std::filesystem::path& file, const std::string& content)
	{
		std::ofstream output{file};
		output << content;
		require(output.good(), "scheduler fixture write failed");
	}

	[[nodiscard]] workspace make_workspace(const std::filesystem::path& root,
										   const std::size_t count)
	{
		std::filesystem::remove_all(root);
		std::filesystem::create_directories(root);
		std::string database{"["};
		for (std::size_t index = 0U; index < count; ++index)
		{
			const auto source = root / ("unit" + std::to_string(index) + ".cpp");
			write(source, "int unit" + std::to_string(index) + "();\n");
			if (index != 0U)
				database += ',';
			database += "{\"directory\":\"" + root.generic_string() + "\",\"file\":\"" +
				source.generic_string() + "\",\"arguments\":[\"clang++\",\"-std=c++23\",\"" +
				source.generic_string() + "\"]}";
		}
		database += ']';
		write(root / "compile_commands.json", database);
		auto opened = workspace::open(
			workspace_options::from_compilation_database(root / "compile_commands.json"));
		require(opened.has_value(), "scheduler workspace did not open");
		return std::move(opened.value());
	}

	[[nodiscard]] task_request request_for(const compile_unit& unit, const std::size_t index)
	{
		task_request request;
		request.priority = index == 0U ? task_priority::explicit_target : task_priority::background;
		request.parse.unit = unit;
		request.parse.files = {
			{unit.command().file, "int unit" + std::to_string(index) + "(){return 0;}\n"}};
		request.profile = fact_profile::semantic_search();
		request.snapshot_key = "snapshot-v1-unit-" + std::to_string(index);
		request.subscribers = {{"client-" + std::to_string(index), {}}};
		return request;
	}

	class fake_worker final : public worker_port
	{
	  public:
		std::atomic<std::size_t> calls{};
		std::atomic<std::size_t> active{};
		std::atomic<std::size_t> maximum_active{};
		std::map<std::string, std::string> failures;
		std::set<std::string> batch_failures;
		std::size_t payload_size{};

		result<frontend::observation_batch> execute(const task_request& task,
													execution_context context) override
		{
			++calls;
			const auto now_active = ++active;
			auto observed = maximum_active.load();
			while (observed < now_active &&
				   !maximum_active.compare_exchange_weak(observed, now_active))
			{
			}
			std::this_thread::yield();
			--active;
			if (context.cancellation.stop_requested())
			{
				error failure;
				failure.code.value = "core.cancelled";
				failure.message = "cancelled";
				failure.scope = failure_scope::compile_unit;
				return failure;
			}
			const auto id = std::string{task.parse.unit.id().value()};
			if (const auto failure = failures.find(id); failure != failures.end())
			{
				error injected;
				injected.code.value = failure->second;
				injected.message = "injected";
				injected.scope = failure_scope::compile_unit;
				return injected;
			}
			frontend::observation_batch batch;
			batch.adapter_id = "clang22.frontend";
			batch.adapter_version = "scheduler-test";
			batch.unit = task.parse.unit.id();
			batch.variant = task.parse.unit.variant_id();
			batch.coverage.parsed = 1U;
			if (batch_failures.contains(id))
			{
				batch.coverage.parsed = 0U;
				batch.coverage.failed = 1U;
				batch.diagnostics.push_back({"clang.diagnostic.7",
											 frontend::diagnostic_severity::error,
											 "unit.cpp",
											 2U,
											 3U,
											 "injected parse failure"});
			}
			if (payload_size != 0U)
				batch.diagnostics.push_back({"clang.diagnostic.1",
											 frontend::diagnostic_severity::note,
											 "unit.cpp",
											 1U,
											 1U,
											 std::string(payload_size, 'x')});
			return batch;
		}
	};

	[[nodiscard]] scheduler_options options(const std::size_t jobs, const std::uint64_t seed = 0U)
	{
		scheduler_options value;
		value.jobs = jobs;
		value.seed = seed;
		return value;
	}

	void determinism_and_bounds(scheduler& service, const std::vector<task_request>& base)
	{
		std::string canonical;
		for (const auto jobs : {1U, 2U, 8U})
			for (const auto seed : {0U, 1U, 0xC771EU})
			{
				auto input = base;
				if ((seed & 1U) != 0U)
					std::ranges::reverse(input);
				fake_worker worker;
				auto output = service.run(std::move(input), worker, options(jobs, seed));
				require(output && output.value().validate(), "determinism run failed");
				if (canonical.empty())
					canonical = output.value().to_json();
				else
					require(canonical == output.value().to_json(),
							"jobs/seed/enqueue order changed canonical output");
				require(worker.maximum_active <= jobs, "worker bound exceeded jobs");
			}

		auto bounded = options(1U);
		bounded.maximum_queued_tasks = base.size() - 1U;
		fake_worker worker;
		auto rejected = service.run(base, worker, bounded);
		require(!rejected && rejected.error().code.value == "core.budget-exhausted" &&
					rejected.error().attributes.at("reason") == "queue-limit",
				"queue overflow was not structured");
	}

	void coalescing_and_subscribers(scheduler& service, task_request request)
	{
		std::stop_source cancelled;
		cancelled.request_stop();
		request.subscribers = {{"active", {}}};
		auto duplicate = request;
		duplicate.subscribers = {{"cancelled", cancelled.get_token()}};
		fake_worker worker;
		auto output = service.run({request, duplicate}, worker);
		require(output && worker.calls == 1U && output.value().tasks.size() == 1U,
				"duplicate work was not coalesced to one worker");
		const auto& deliveries = output.value().tasks.front().deliveries;
		require(deliveries.size() == 2U && deliveries.front().id == "active" &&
					deliveries.front().state == task_state::succeeded &&
					deliveries.back().state == task_state::cancelled,
				"subscriber cancellation leaked into shared work");
		require(std::ranges::any_of(output.value().trace,
									[](const trace_row& row)
									{
										return row.event == "coalesced";
									}),
				"coalescing trace was omitted");
	}

	void partial_failures_and_dag(scheduler& service, std::vector<task_request> requests)
	{
		auto failed_key = service.task_key(requests.at(0));
		require(failed_key.has_value(), "failed task key was not generated");
		requests.at(2).kind = task_kind::refinement;
		requests.at(2).refinement_id = "selector-refinement-v1";
		requests.at(2).dependencies = {failed_key.value()};
		fake_worker worker;
		worker.batch_failures.emplace(std::string{requests.at(0).parse.unit.id().value()});
		auto output = service.run(std::move(requests), worker, options(2U));
		require(output && output.value().coverage.failed == 1U &&
					output.value().coverage.dependency_failed == 1U &&
					output.value().coverage.succeeded >= 1U,
				"partial failure did not preserve/account successful siblings");
		require(std::ranges::any_of(output.value().tasks,
									[](const task_result& result)
									{
										return !result.diagnostics.empty();
									}),
				"partial parse diagnostics were dropped");
		require(std::ranges::all_of(output.value().tasks,
									[](const task_result& result)
									{
										return result.frontend_coverage.requested ==
											result.frontend_coverage.parsed +
											result.frontend_coverage.failed +
											result.frontend_coverage.cancelled;
									}),
				"partial frontend coverage was not balanced");
		require(std::ranges::any_of(output.value().tasks,
									[](const task_result& result)
									{
										return result.kind == task_kind::refinement;
									}),
				"refinement DAG task was not represented");
	}

	void controls(scheduler& service,
				  const task_request& request,
				  const runtime::fixed_time_adapter& time)
	{
		fake_worker worker;
		std::stop_source cancelled;
		cancelled.request_stop();
		auto cancelled_options = options(1U);
		cancelled_options.execution.cancellation = cancelled.get_token();
		auto cancelled_output = service.run({request}, worker, cancelled_options);
		require(cancelled_output && cancelled_output.value().coverage.cancelled == 1U,
				"operation cancellation state was lost");

		auto deadline_options = options(1U);
		deadline_options.execution.deadline = time.steady_now() - std::chrono::milliseconds{1};
		auto deadline_output = service.run({request}, worker, deadline_options);
		require(deadline_output && deadline_output.value().coverage.deadline_exceeded == 1U,
				"deadline state was lost");

		auto budget_options = options(1U);
		budget_options.cost_budget = 1U;
		auto costly = request;
		costly.cost = 2U;
		auto budget_output = service.run({costly}, worker, budget_options);
		require(budget_output && budget_output.value().coverage.budget_exhausted == 1U,
				"cost budget state was lost");

		fake_worker large_worker;
		large_worker.payload_size = 256U;
		auto output_options = options(1U);
		output_options.maximum_output_bytes = 32U;
		auto output = service.run({request}, large_worker, output_options);
		require(output && output.value().coverage.output_limited == 1U &&
					output.value().tasks.front().semantic_batch.empty(),
				"output budget was not bounded and explicit");

		fake_worker timeout_worker;
		timeout_worker.failures.emplace(std::string{request.parse.unit.id().value()},
										"parse.timeout");
		auto timeout = service.run({request}, timeout_worker);
		require(timeout && timeout.value().coverage.deadline_exceeded == 1U &&
					timeout.value().tasks.front().reason_code == "parse.timeout",
				"worker timeout code was collapsed");
		fake_worker crash_worker;
		crash_worker.failures.emplace(std::string{request.parse.unit.id().value()},
									  "parse.crashed");
		auto crashed = service.run({request}, crash_worker);
		require(crashed && crashed.value().coverage.failed == 1U &&
					crashed.value().tasks.front().reason_code == "parse.crashed",
				"worker failure state/code was collapsed");

		fake_worker constrained_worker;
		auto constrained = options(8U);
		constrained.execution.parallelism = 2U;
		constrained.execution.memory_budget_mb = constrained.memory_per_job_mb;
		std::size_t progress_calls{};
		double last_progress{};
		constrained.execution.progress =
			[&progress_calls, &last_progress](const double value, std::string_view)
		{
			++progress_calls;
			last_progress = value;
		};
		auto constrained_output = service.run({request, request}, constrained_worker, constrained);
		require(!constrained_output &&
					constrained_output.error().attributes.at("reason") == "duplicate-subscriber",
				"duplicate subscriber admission was not rejected");
		auto second = request;
		second.subscribers = {{"second", {}}};
		second.snapshot_key += "-second";
		auto bounded_parallelism = service.run({request, second}, constrained_worker, constrained);
		require(bounded_parallelism && constrained_worker.maximum_active <= 1U &&
					progress_calls == 2U && last_progress == 1.0,
				"parallelism/memory/progress controls were not honored");
	}

	void production_bridge(scheduler& service, const task_request& request)
	{
		frontend_scheduler_worker worker;
		auto output = service.run({request}, worker);
		require(output.has_value(), "production frontend bridge broke scheduler invariants");
		const auto& result = output.value().tasks.front();
		if (result.state == task_state::succeeded)
			require(result.frontend_coverage.parsed == 1U,
					"available production frontend did not parse its TU");
		else
			require(result.state == task_state::failed &&
						result.reason_code == "core.capability-unavailable",
					"production frontend bridge lost its capability state");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	const auto root = std::filesystem::temp_directory_path() / "cxxlens-scheduler-test";
	auto workspace = make_workspace(root, 4U);
	std::vector<task_request> requests;
	const auto units = workspace.compile_units();
	for (std::size_t index = 0U; index < units.size(); ++index)
		requests.push_back(request_for(units[index], index));

	runtime::fnv1a_hash_adapter hashes;
	runtime::fixed_time_adapter time{
		std::chrono::system_clock::time_point{},
		std::chrono::steady_clock::time_point{std::chrono::seconds{10}}};
	scheduler service{hashes, time};
	if (argument_count == 5 && std::string_view{arguments[1]} == "--emit")
	{
		const auto jobs = static_cast<std::size_t>(std::stoull(arguments[2]));
		const auto seed = std::stoull(arguments[3]);
		if (std::string_view{arguments[4]} == "reverse")
			std::ranges::reverse(requests);
		fake_worker worker;
		auto output = service.run(std::move(requests), worker, options(jobs, seed));
		require(output.has_value(), "scheduler emission failed");
		std::cout << output.value().to_json() << '\n';
		std::filesystem::remove_all(root);
		return 0;
	}
	determinism_and_bounds(service, requests);
	coalescing_and_subscribers(service, requests.front());
	partial_failures_and_dag(service, requests);
	controls(service, requests.front(), time);
	production_bridge(service, requests.front());
	std::filesystem::remove_all(root);
	return 0;
}
