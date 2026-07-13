#include "scheduler.hpp"

#include <algorithm>
#include <future>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <thread>
#include <utility>

#include "../core/canonical_encoding.hpp"
#include "../core/canonical_json.hpp"
#include "../llvm/clang22/frontend_job.hpp"

namespace cxxlens::detail::scheduling
{
	namespace
	{
		using json::json_value;

		[[nodiscard]] error scheduler_error(std::string code, std::string reason)
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Scheduler invariant failed";
			failure.scope = failure_scope::operation;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		[[nodiscard]] std::string_view state_name(const task_state value) noexcept
		{
			switch (value)
			{
				case task_state::succeeded:
					return "succeeded";
				case task_state::failed:
					return "failed";
				case task_state::cancelled:
					return "cancelled";
				case task_state::deadline_exceeded:
					return "deadline_exceeded";
				case task_state::budget_exhausted:
					return "budget_exhausted";
				case task_state::output_limited:
					return "output_limited";
				case task_state::dependency_failed:
					return "dependency_failed";
			}
			return "failed";
		}

		[[nodiscard]] std::string_view kind_name(const task_kind value) noexcept
		{
			return value == task_kind::parse ? "parse" : "refinement";
		}

		[[nodiscard]] std::pair<task_state, std::string> classify(const error& failure)
		{
			if (failure.code.value == "core.cancelled")
				return {task_state::cancelled, failure.code.value};
			if (failure.code.value == "core.deadline-exceeded" ||
				failure.code.value == "parse.timeout")
				return {task_state::deadline_exceeded, failure.code.value};
			if (failure.code.value == "core.budget-exhausted")
				return {task_state::budget_exhausted, failure.code.value};
			return {task_state::failed,
					failure.code.value.empty() ? std::string{"core.internal-invariant-violation"}
											   : failure.code.value};
		}

		void account(scheduler_coverage& coverage, const task_state state)
		{
			switch (state)
			{
				case task_state::succeeded:
					++coverage.succeeded;
					break;
				case task_state::failed:
					++coverage.failed;
					break;
				case task_state::cancelled:
					++coverage.cancelled;
					break;
				case task_state::deadline_exceeded:
					++coverage.deadline_exceeded;
					break;
				case task_state::budget_exhausted:
					++coverage.budget_exhausted;
					break;
				case task_state::output_limited:
					++coverage.output_limited;
					break;
				case task_state::dependency_failed:
					++coverage.dependency_failed;
					break;
			}
		}

		[[nodiscard]] bool successful(const task_state state) noexcept
		{
			return state == task_state::succeeded;
		}

		void normalize_dependencies(task_request& task)
		{
			std::ranges::sort(task.dependencies);
			task.dependencies.erase(std::ranges::unique(task.dependencies).begin(),
									task.dependencies.end());
		}

		struct canonical_task_identity
		{
			std::string task_key;
			std::string input_fingerprint;
			std::vector<std::byte> canonical_bytes;
		};

		[[nodiscard]] result<std::vector<std::pair<std::string, std::string>>>
		normalized_virtual_sources(const task_request& task)
		{
			std::vector<std::pair<std::string, std::string>> sources;
			sources.reserve(task.parse.files.size());
			const auto working_directory = task.parse.unit.command().directory.lexically_normal();
			for (const auto& source : task.parse.files)
			{
				if (source.file.empty())
					return scheduler_error("core.invalid-argument", "virtual-source-path");
				auto normalized = source.file;
				if (normalized.is_relative())
				{
					if (working_directory.empty() || working_directory.is_relative())
						return scheduler_error("core.invalid-argument", "virtual-source-base");
					normalized = working_directory / normalized;
				}
				normalized = normalized.lexically_normal();
				if (!normalized.is_absolute() || normalized.empty())
					return scheduler_error("core.invalid-argument", "virtual-source-path");
				sources.emplace_back(normalized.generic_string(), source.content);
			}
			std::ranges::sort(sources, {}, &std::pair<std::string, std::string>::first);
			if (std::ranges::adjacent_find(sources,
										   [](const auto& left, const auto& right)
										   {
											   return left.first == right.first;
										   }) != sources.end())
				return scheduler_error("core.invalid-argument", "duplicate-virtual-source-path");
			return sources;
		}

		[[nodiscard]] result<void> normalize_parse_input(task_request& task)
		{
			auto sources = normalized_virtual_sources(task);
			if (!sources)
				return scheduler_error(sources.error().code.value,
									   sources.error().attributes.at("reason"));
			task.parse.files.clear();
			task.parse.files.reserve(sources.value().size());
			for (auto& [file, content] : sources.value())
				task.parse.files.push_back({path{std::move(file)}, std::move(content)});
			return {};
		}

		[[nodiscard]] result<canonical_task_identity>
		make_task_identity(const task_request& task,
						   const runtime::hash_port& hashes,
						   const std::string_view toolchain)
		{
			if (!task.parse.unit.id().valid() || !task.parse.unit.variant_id().valid() ||
				task.snapshot_key.empty() || task.cost == 0U)
				return scheduler_error("core.invalid-argument", "task-identity");
			auto sources = normalized_virtual_sources(task);
			if (!sources)
				return scheduler_error(sources.error().code.value,
									   sources.error().attributes.at("reason"));

			identity::canonical_encoder input_encoder{"cxxlens.scheduler-parse-input.v2", {2U, 2U}};
			input_encoder.string_map("virtual_sources", std::move(sources.value()));
			input_encoder.enum_field("injected_fault",
									 static_cast<std::int64_t>(task.parse.injected_fault));
			input_encoder.string_field("compile_unit", std::string{task.parse.unit.id().value()});
			input_encoder.string_field("variant",
									   std::string{task.parse.unit.variant_id().value()});
			input_encoder.string_field("compile_command", task.parse.unit.command_digest());
			input_encoder.string_field("profile", task.profile.to_json());
			input_encoder.string_field("toolchain", std::string{toolchain});
			auto input_payload = input_encoder.finish();
			if (!input_payload)
				return scheduler_error("core.invalid-argument", "canonical-parse-input");

			identity::identity_service identities{hashes};
			identity::collision_registry collisions;
			auto input_fingerprint = identities.make_id("input", input_payload.value(), collisions);
			if (!input_fingerprint)
				return scheduler_error("core.internal-invariant-violation", "parse-input-hash");

			identity::canonical_encoder task_encoder{"cxxlens.scheduler-task.v2", {2U, 2U}};
			task_encoder.enum_field("kind", static_cast<std::int64_t>(task.kind));
			task_encoder.string_field("compile_unit", std::string{task.parse.unit.id().value()});
			task_encoder.string_field("variant", std::string{task.parse.unit.variant_id().value()});
			task_encoder.string_field("profile", task.profile.to_json());
			task_encoder.string_field("snapshot", task.snapshot_key);
			task_encoder.string_field("refinement", task.refinement_id);
			task_encoder.string_field("parse_input", input_fingerprint.value());
			task_encoder.string_set("dependencies", task.dependencies);
			task_encoder.unsigned_field("cost", task.cost);
			auto task_payload = task_encoder.finish();
			if (!task_payload)
				return scheduler_error("core.invalid-argument", "canonical-task-payload");
			auto task_key = identities.make_id("task", task_payload.value(), collisions);
			if (!task_key)
				return scheduler_error("core.internal-invariant-violation", "task-key-hash");
			return canonical_task_identity{
				task_key.value(), input_fingerprint.value(), task_payload.value().bytes};
		}

		[[nodiscard]] json_value delivery_json(const subscriber_delivery& delivery)
		{
			return json_value::object{{"id", delivery.id},
									  {"reason_code", delivery.reason_code},
									  {"state", std::string{state_name(delivery.state)}}};
		}

		[[nodiscard]] json_value task_json(const task_result& task)
		{
			json_value::array dependencies;
			for (const auto& dependency : task.dependencies)
				dependencies.emplace_back(dependency);
			json_value::array deliveries;
			for (const auto& delivery : task.deliveries)
				deliveries.emplace_back(delivery_json(delivery));
			json_value::array diagnostics;
			for (const auto& diagnostic : task.diagnostics)
				diagnostics.emplace_back(json_value::object{
					{"column", std::uint64_t{diagnostic.column}},
					{"file", diagnostic.file},
					{"id", diagnostic.id},
					{"line", std::uint64_t{diagnostic.line}},
					{"message", diagnostic.message},
					{"severity",
					 diagnostic.severity == frontend::diagnostic_severity::note			 ? "note"
						 : diagnostic.severity == frontend::diagnostic_severity::warning ? "warning"
						 : diagnostic.severity == frontend::diagnostic_severity::error	 ? "error"
																					   : "fatal"}});
			return json_value::object{
				{"dependencies", std::move(dependencies)},
				{"deliveries", std::move(deliveries)},
				{"diagnostics", std::move(diagnostics)},
				{"frontend_coverage",
				 json_value::object{
					 {"cancelled", std::uint64_t{task.frontend_coverage.cancelled}},
					 {"failed", std::uint64_t{task.frontend_coverage.failed}},
					 {"parsed", std::uint64_t{task.frontend_coverage.parsed}},
					 {"requested", std::uint64_t{task.frontend_coverage.requested}}}},
				{"kind", std::string{kind_name(task.kind)}},
				{"input_fingerprint", task.input_fingerprint},
				{"reason_code", task.reason_code},
				{"semantic_batch", task.semantic_batch},
				{"state", std::string{state_name(task.state)}},
				{"task_key", task.task_key}};
		}

		void set_deliveries(task_result& result, const task_request& request)
		{
			for (const auto& subscriber : request.subscribers)
			{
				if (subscriber.cancellation.stop_requested())
					result.deliveries.push_back(
						{subscriber.id, task_state::cancelled, "core.cancelled"});
				else
					result.deliveries.push_back({subscriber.id, result.state, result.reason_code});
			}
			std::ranges::sort(result.deliveries);
		}

		[[nodiscard]] bool any_active_subscriber(const task_request& request)
		{
			return request.subscribers.empty() ||
				std::ranges::any_of(request.subscribers,
									[](const subscriber_request& value)
									{
										return !value.cancellation.stop_requested();
									});
		}
	} // namespace

	result<void> scheduler_batch::validate() const
	{
		if (schema != "cxxlens.scheduler-trace.v1")
			return scheduler_error("core.schema-validation-failed", "schema");
		if (!std::ranges::is_sorted(tasks, {}, &task_result::task_key) ||
			std::ranges::adjacent_find(tasks, {}, &task_result::task_key) != tasks.end())
			return scheduler_error("core.internal-invariant-violation", "task-order");
		if (!std::ranges::is_sorted(trace))
			return scheduler_error("core.internal-invariant-violation", "trace-order");
		if (coverage.requested != tasks.size())
			return scheduler_error("core.internal-invariant-violation", "requested-coverage");
		const auto accounted = coverage.succeeded + coverage.failed + coverage.cancelled +
			coverage.deadline_exceeded + coverage.budget_exhausted + coverage.output_limited +
			coverage.dependency_failed;
		if (accounted != coverage.requested)
			return scheduler_error("core.internal-invariant-violation", "coverage-accounting");
		for (const auto& task : tasks)
		{
			if (!task.task_key.starts_with("task_") || task.task_key.size() != 69U ||
				!task.input_fingerprint.starts_with("input_") ||
				task.input_fingerprint.size() != 70U ||
				!std::ranges::is_sorted(task.dependencies) ||
				!std::ranges::is_sorted(task.deliveries))
				return scheduler_error("core.internal-invariant-violation", "task-shape");
			if (task.state == task_state::succeeded && task.reason_code != "ok")
				return scheduler_error("core.internal-invariant-violation", "success-reason");
			if (task.state == task_state::succeeded && !task.observation_batch)
				return scheduler_error("core.internal-invariant-violation",
									   "success-batch-missing");
			if (task.frontend_coverage.requested !=
				task.frontend_coverage.parsed + task.frontend_coverage.failed +
					task.frontend_coverage.cancelled)
				return scheduler_error("core.internal-invariant-violation",
									   "frontend-coverage-accounting");
		}
		std::map<std::string, std::string> inputs;
		for (const auto& task : tasks)
			inputs.emplace(task.task_key, task.input_fingerprint);
		for (const auto& row : trace)
			if (!inputs.contains(row.task_key) || inputs.at(row.task_key) != row.input_fingerprint)
				return scheduler_error("core.internal-invariant-violation",
									   "trace-input-fingerprint");
		return {};
	}

	std::string scheduler_batch::to_json() const
	{
		json_value::array task_values;
		for (const auto& task : tasks)
			task_values.emplace_back(task_json(task));
		json_value::array trace_values;
		for (const auto& row : trace)
			trace_values.emplace_back(
				json_value::object{{"detail", row.detail},
								   {"event", row.event},
								   {"input_fingerprint", row.input_fingerprint},
								   {"task_key", row.task_key}});
		const json_value document{json_value::object{
			{"coverage",
			 json_value::object{{"budget_exhausted", coverage.budget_exhausted},
								{"cancelled", coverage.cancelled},
								{"deadline_exceeded", coverage.deadline_exceeded},
								{"dependency_failed", coverage.dependency_failed},
								{"failed", coverage.failed},
								{"output_limited", coverage.output_limited},
								{"requested", coverage.requested},
								{"succeeded", coverage.succeeded}}},
			{"schema", schema},
			{"tasks", std::move(task_values)},
			{"trace", std::move(trace_values)}}};
		auto written = json::write(document);
		return written ? std::move(written.value()) : std::string{};
	}

	scheduler::scheduler(const runtime::hash_port& hashes,
						 const runtime::time_port& time,
						 std::string frontend_toolchain)
		: hashes_{hashes}, time_{time}, frontend_toolchain_{std::move(frontend_toolchain)}
	{
		if (frontend_toolchain_.empty())
		{
			const auto adapter = clang22::capability();
			frontend_toolchain_ = std::string{"clang22:"} + std::to_string(adapter.llvm_major) +
				"." + std::to_string(adapter.llvm_minor) + "." +
				std::to_string(adapter.llvm_patch) + "@" + adapter.adapter_version;
		}
	}

	result<std::string> scheduler::task_key(const task_request& task) const
	{
		auto identity = make_task_identity(task, hashes_, frontend_toolchain_);
		if (!identity)
			return scheduler_error(identity.error().code.value,
								   identity.error().attributes.at("reason"));
		return identity.value().task_key;
	}

	result<scheduler_batch> scheduler::run(std::vector<task_request> requests,
										   worker_port& worker,
										   scheduler_options options) const
	{
		if (options.jobs == 0U || options.memory_per_job_mb == 0U ||
			options.maximum_queued_tasks == 0U || options.maximum_output_bytes == 0U)
			return scheduler_error("core.invalid-argument", "scheduler-options");
		if (requests.size() > options.maximum_queued_tasks)
			return scheduler_error("core.budget-exhausted", "queue-limit");
		std::size_t effective_jobs = options.jobs;
		if (options.execution.parallelism != 0U)
			effective_jobs = std::min(effective_jobs, options.execution.parallelism);
		if (options.execution.memory_budget_mb != 0U)
		{
			const auto memory_jobs = std::max(
				std::size_t{1U}, options.execution.memory_budget_mb / options.memory_per_job_mb);
			effective_jobs = std::min(effective_jobs, memory_jobs);
		}

		struct admitted_task
		{
			std::string key;
			std::string input_fingerprint;
			std::vector<std::byte> canonical_bytes;
			task_request request;
			std::size_t duplicate_count{1U};
		};
		std::map<std::string, admitted_task> unique;
		for (auto& request : requests)
		{
			normalize_dependencies(request);
			if (auto normalized = normalize_parse_input(request); !normalized)
				return scheduler_error(normalized.error().code.value,
									   normalized.error().attributes.at("reason"));
			std::ranges::sort(request.subscribers, {}, &subscriber_request::id);
			if (std::ranges::adjacent_find(request.subscribers, {}, &subscriber_request::id) !=
				request.subscribers.end())
				return scheduler_error("core.invalid-argument", "duplicate-subscriber");
			auto identity = make_task_identity(request, hashes_, frontend_toolchain_);
			if (!identity)
				return scheduler_error(identity.error().code.value,
									   identity.error().attributes.at("reason"));
			auto position = unique.find(identity.value().task_key);
			if (position == unique.end())
			{
				auto admitted = admitted_task{identity.value().task_key,
											  identity.value().input_fingerprint,
											  std::move(identity.value().canonical_bytes),
											  std::move(request),
											  1U};
				admitted.request.input_fingerprint = identity.value().input_fingerprint;
				unique.emplace(admitted.key, std::move(admitted));
			}
			else
			{
				if (position->second.canonical_bytes != identity.value().canonical_bytes)
					return scheduler_error("core.invalid-argument", "conflicting-coalesced-task");
				position->second.request.priority =
					std::min(position->second.request.priority, request.priority);
				position->second.request.subscribers.insert(
					position->second.request.subscribers.end(),
					std::make_move_iterator(request.subscribers.begin()),
					std::make_move_iterator(request.subscribers.end()));
				std::ranges::sort(
					position->second.request.subscribers, {}, &subscriber_request::id);
				if (std::ranges::adjacent_find(
						position->second.request.subscribers, {}, &subscriber_request::id) !=
					position->second.request.subscribers.end())
					return scheduler_error("core.invalid-argument", "duplicate-subscriber");
				++position->second.duplicate_count;
			}
		}
		if (unique.size() > options.maximum_queued_tasks)
			return scheduler_error("core.budget-exhausted", "queue-limit");

		for (const auto& [key, task] : unique)
			for (const auto& dependency : task.request.dependencies)
				if (dependency == key || !unique.contains(dependency))
					return scheduler_error("core.invalid-argument", "invalid-dependency");

		scheduler_batch batch;
		batch.coverage.requested = unique.size();
		std::map<std::string, task_state> completed;
		std::uint64_t spent{};
		std::size_t output_bytes{};

		while (completed.size() < unique.size())
		{
			std::vector<admitted_task*> ready;
			for (auto& [key, task] : unique)
			{
				if (completed.contains(key))
					continue;
				const bool dependencies_done =
					std::ranges::all_of(task.request.dependencies,
										[&completed](const std::string& dependency)
										{
											return completed.contains(dependency);
										});
				if (dependencies_done)
					ready.push_back(&task);
			}
			if (ready.empty())
				return scheduler_error("core.invalid-argument", "dependency-cycle");
			std::ranges::sort(ready,
							  [](const admitted_task* left, const admitted_task* right)
							  {
								  return std::pair{left->request.priority, left->key} <
									  std::pair{right->request.priority, right->key};
							  });

			for (std::size_t offset = 0U; offset < ready.size(); offset += effective_jobs)
			{
				const auto end = std::min(ready.size(), offset + effective_jobs);
				struct pending
				{
					admitted_task* task{};
					std::stop_source cancellation;
					std::future<result<frontend::observation_batch>> future;
				};
				std::vector<pending> running;
				for (std::size_t index = offset; index < end; ++index)
				{
					auto& task = *ready[index];
					task_result immediate;
					immediate.task_key = task.key;
					immediate.input_fingerprint = task.input_fingerprint;
					immediate.kind = task.request.kind;
					immediate.dependencies = task.request.dependencies;
					immediate.frontend_coverage.requested = 0U;
					const auto dependency_failed =
						std::ranges::any_of(task.request.dependencies,
											[&completed](const std::string& dependency)
											{
												return !successful(completed.at(dependency));
											});
					if (dependency_failed)
					{
						immediate.state = task_state::dependency_failed;
						immediate.reason_code = "search.required-facts-unavailable";
					}
					else if (!any_active_subscriber(task.request) ||
							 options.execution.cancellation.stop_requested())
					{
						immediate.state = task_state::cancelled;
						immediate.reason_code = "core.cancelled";
					}
					else if (options.execution.deadline &&
							 time_.steady_now() >= *options.execution.deadline)
					{
						immediate.state = task_state::deadline_exceeded;
						immediate.reason_code = "core.deadline-exceeded";
					}
					else if (options.cost_budget != 0U &&
							 (task.request.cost >
							  options.cost_budget - std::min(spent, options.cost_budget)))
					{
						immediate.state = task_state::budget_exhausted;
						immediate.reason_code = "core.budget-exhausted";
					}
					else
					{
						spent += task.request.cost;
						batch.trace.push_back(
							{task.key, task.input_fingerprint, "scheduled", "worker"});
						auto* scheduled_task = &task;
						std::stop_source worker_cancellation;
						auto worker_context = options.execution;
						worker_context.cancellation = worker_cancellation.get_token();
						running.push_back(
							{scheduled_task,
							 worker_cancellation,
							 std::async(
								 std::launch::async,
								 [&worker, &options, scheduled_task, worker_context]() mutable
								 {
									 std::uint64_t key_fold{};
									 for (const auto value : scheduled_task->key)
										 key_fold =
											 (key_fold * 131U) ^ static_cast<unsigned char>(value);
									 const auto perturbation = (options.seed ^ key_fold) & 3U;
									 for (std::uint64_t count = 0U; count < perturbation; ++count)
										 std::this_thread::yield();
									 return worker.execute(scheduled_task->request,
														   std::move(worker_context));
								 })});
						continue;
					}
					set_deliveries(immediate, task.request);
					account(batch.coverage, immediate.state);
					completed.emplace(task.key, immediate.state);
					if (options.execution.progress)
						options.execution.progress(static_cast<double>(completed.size()) /
													   static_cast<double>(unique.size()),
												   state_name(immediate.state));
					batch.trace.push_back({task.key,
										   task.input_fingerprint,
										   std::string{state_name(immediate.state)},
										   immediate.reason_code});
					batch.tasks.push_back(std::move(immediate));
				}

				for (auto& pending : running)
				{
					while (pending.future.wait_for(std::chrono::milliseconds{5}) !=
						   std::future_status::ready)
					{
						if (options.execution.cancellation.stop_requested() ||
							!any_active_subscriber(pending.task->request) ||
							(options.execution.deadline &&
							 time_.steady_now() >= *options.execution.deadline))
							pending.cancellation.request_stop();
					}
					auto outcome = pending.future.get();
					task_result result;
					result.task_key = pending.task->key;
					result.input_fingerprint = pending.task->input_fingerprint;
					result.kind = pending.task->request.kind;
					result.dependencies = pending.task->request.dependencies;
					if (!outcome)
					{
						auto classified = classify(outcome.error());
						result.state = classified.first;
						result.reason_code = std::move(classified.second);
						if (result.state == task_state::cancelled)
							result.frontend_coverage.cancelled = 1U;
						else
							result.frontend_coverage.failed = 1U;
					}
					else if (auto checked = outcome.value().validate(); !checked)
					{
						result.state = task_state::failed;
						result.reason_code = checked.error().code.value;
						result.frontend_coverage.failed = 1U;
					}
					else
					{
						result.semantic_batch = outcome.value().semantic_representation();
						result.diagnostics = outcome.value().diagnostics;
						result.frontend_coverage = outcome.value().coverage;
						result.observation_batch = std::make_shared<frontend::observation_batch>(
							std::move(outcome.value()));
						std::size_t payload_bytes = result.semantic_batch.size();
						for (const auto& diagnostic : result.diagnostics)
							payload_bytes += diagnostic.id.size() + diagnostic.file.size() +
								diagnostic.message.size() + 32U;
						if (payload_bytes > options.maximum_output_bytes -
								std::min(output_bytes, options.maximum_output_bytes))
						{
							result.semantic_batch.clear();
							result.diagnostics.clear();
							result.observation_batch.reset();
							result.state = task_state::output_limited;
							result.reason_code = "core.budget-exhausted";
						}
						else
						{
							output_bytes += payload_bytes;
							if (result.frontend_coverage.cancelled != 0U)
							{
								result.state = task_state::cancelled;
								result.reason_code = "core.cancelled";
							}
							else if (result.frontend_coverage.failed != 0U)
							{
								result.state = task_state::failed;
								result.reason_code = "parse.frontend-failed";
							}
							else
							{
								result.state = task_state::succeeded;
								result.reason_code = "ok";
							}
						}
					}
					set_deliveries(result, pending.task->request);
					account(batch.coverage, result.state);
					completed.emplace(pending.task->key, result.state);
					if (options.execution.progress)
						options.execution.progress(static_cast<double>(completed.size()) /
													   static_cast<double>(unique.size()),
												   state_name(result.state));
					if (pending.task->duplicate_count > 1U)
						batch.trace.push_back({pending.task->key,
											   pending.task->input_fingerprint,
											   "coalesced",
											   std::to_string(pending.task->duplicate_count)});
					batch.trace.push_back({pending.task->key,
										   pending.task->input_fingerprint,
										   std::string{state_name(result.state)},
										   result.reason_code});
					batch.tasks.push_back(std::move(result));
				}
			}
		}

		std::ranges::sort(batch.tasks, {}, &task_result::task_key);
		std::ranges::sort(batch.trace);
		if (auto checked = batch.validate(); !checked)
			return checked.error();
		return batch;
	}
} // namespace cxxlens::detail::scheduling
