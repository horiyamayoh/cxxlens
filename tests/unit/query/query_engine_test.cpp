#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <memory>
#include <optional>
#include <ranges>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/select.hpp>
#include <cxxlens/testing.hpp>

#include "graph/virtual_candidate_resolver.hpp"
#include "query/query_executor.hpp"
#include "query/query_plan.hpp"
#include "query_fixture.hpp"
#include "store/store_port.hpp"
#include "workspace/provisioning.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;
	using namespace cxxlens::test::query_fixture;

	void require(const bool condition, const char* message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] bool contains(const std::vector<symbol_id>& values, const symbol_id& expected)
	{
		return std::ranges::find(values, expected) != values.end();
	}

	[[nodiscard]] bool contains_code(const std::vector<unresolved>& values,
									 const std::string_view expected)
	{
		return std::ranges::any_of(values,
								   [expected](const unresolved& value)
								   {
									   return value.stable_code == expected;
								   });
	}

	[[nodiscard]] override_edge override_relation(const symbol_id& overriding,
												  const symbol_id& overridden,
												  const std::uint64_t offset)
	{
		override_edge value;
		value.overriding_method = overriding;
		value.overridden_method = overridden;
		value.source = span(offset);
		return value;
	}

	[[nodiscard]] graph::candidate_request request(const graph::dispatch_form form)
	{
		graph::candidate_request value;
		value.call_key = "fixture-call";
		value.static_target = base_step;
		value.form = form;
		value.closed_world = true;
		return value;
	}

	class refinement final : public query::targeted_refinement_port
	{
	  public:
		explicit refinement(const graph::dispatch_form form, const bool fail = false)
			: form_{form}, fail_{fail}
		{
		}

		[[nodiscard]] result<query::dispatch_refinement> refine(const call_site&,
																const execution_context&) override
		{
			if (fail_)
			{
				error failure;
				failure.code.value = "search.refinement-failed";
				failure.message = "fixture refinement failure";
				return failure;
			}
			query::dispatch_refinement output;
			output.form = form_;
			output.complete = true;
			output.receiver_is_final = form_ == graph::dispatch_form::final_virtual;
			return output;
		}

	  private:
		graph::dispatch_form form_;
		bool fail_{};
	};

	class semantic_worker final : public scheduling::worker_port
	{
	  public:
		std::atomic<std::size_t> calls{};

		[[nodiscard]] result<frontend::observation_batch>
		execute(const scheduling::task_request& task, execution_context) override
		{
			++calls;
			frontend::observation_batch batch;
			batch.adapter_id = "clang22.frontend";
			batch.adapter_version = "query-fixture";
			batch.unit = task.parse.unit.id();
			batch.variant = task.parse.unit.variant_id();
			batch.coverage.requested = 1U;
			batch.coverage.parsed = 1U;
			const auto fixture_snapshot = snapshot();
			for (const auto& fact : fixture_snapshot->facts)
			{
				facts::observation_record observation;
				observation.adapter_id = batch.adapter_id;
				observation.adapter_version = batch.adapter_version;
				observation.llvm_major = 22U;
				observation.compile_unit = batch.unit;
				observation.variant = batch.variant;
				observation.kind = fact.kind;
				observation.source = fact.source;
				observation.payload_version = fact.payload_version;
				observation.payload = fact.payload;
				observation.payload["semantic_key"] = fact.stable_key;
				observation.name = fact.name;
				observation.type = fact.type;
				batch.observations.push_back(std::move(observation));
			}
			std::ranges::sort(
				batch.observations,
				[](const facts::observation_record& left, const facts::observation_record& right)
				{
					const auto key = [](const facts::observation_record& value)
					{
						return std::to_string(static_cast<std::uint16_t>(value.kind)) + ":" +
							value.payload.at("semantic_key");
					};
					return key(left) < key(right);
				});
			return batch;
		}
	};

	[[nodiscard]] select::semantic_selector flagship()
	{
		return select::semantic(
			select::calls_to_method("Base", "step")
				.include_derived_types()
				.include_virtual_overrides()
				.dispatch(select::dispatch_policy::static_and_virtual_candidates));
	}

	[[nodiscard]] query::query_plan
	plan(const std::size_t refinement_budget = 8U,
		 const std::size_t candidate_budget = 16U,
		 const std::optional<std::size_t> result_limit = std::nullopt)
	{
		query::compile_options options;
		options.candidate_budget = candidate_budget;
		options.refinement_budget = refinement_budget;
		options.result_limit = result_limit;
		auto compiled = query::compile(flagship(), options, "snapshot:fixture");
		require(compiled.has_value(), "flagship plan compilation failed");
		return std::move(compiled.value());
	}

	void check_plan()
	{
		const auto compiled = plan();
		require(compiled.validate().has_value(), "compiled plan is invalid");
		require(!compiled.to_json().empty(), "plan serialization is empty");
		const auto has_stage = [&](const query::stage_kind kind)
		{
			return std::ranges::any_of(compiled.stages,
									   [kind](const query::query_stage& stage)
									   {
										   return stage.kind == kind;
									   });
		};
		require(has_stage(query::stage_kind::fact_index_scan), "fact scan stage missing");
		require(has_stage(query::stage_kind::hierarchy_closure), "hierarchy stage missing");
		require(has_stage(query::stage_kind::override_closure), "override stage missing");
		require(has_stage(query::stage_kind::ast_refinement), "refinement stage missing");
		require(compiled.stages.back().kind == query::stage_kind::deduplicate_sort_limit,
				"terminal deterministic stage missing");

		query::compile_options invalid;
		invalid.candidate_budget = 0U;
		require(!query::compile(flagship(), invalid, "snapshot:fixture"),
				"zero candidate budget was accepted");
	}

	void check_resolver()
	{
		graph::virtual_candidate_resolver resolver{
			{override_relation(derived_step, base_step, 20U)}};
		auto virtual_request = request(graph::dispatch_form::virtual_open);
		auto resolved = resolver.resolve(virtual_request);
		require(resolved.validate().has_value(), "closed-world resolution is invalid");
		require(resolved.status == graph::resolution_status::exact &&
					resolved.possible_targets.size() == 2U &&
					contains(resolved.possible_targets, base_step) &&
					contains(resolved.possible_targets, derived_step),
				"override closure lost a virtual candidate");

		for (const auto form : {graph::dispatch_form::direct,
								graph::dispatch_form::nonvirtual_member,
								graph::dispatch_form::qualified_virtual,
								graph::dispatch_form::final_virtual})
		{
			auto static_request = request(form);
			static_request.observed_candidates = {derived_step};
			auto static_only = resolver.resolve(static_request);
			require(static_only.status == graph::resolution_status::exact &&
						static_only.possible_targets == std::vector<symbol_id>{base_step},
					"non-expanding dispatch form expanded candidates");
		}

		auto final_request = request(graph::dispatch_form::virtual_open);
		final_request.receiver_is_final = true;
		require(resolver.resolve(final_request).possible_targets ==
					std::vector<symbol_id>{base_step},
				"final receiver expanded candidates");

		auto open_request = request(graph::dispatch_form::virtual_open);
		open_request.closed_world = false;
		auto open = resolver.resolve(open_request);
		require(open.status == graph::resolution_status::partial &&
					contains_code(open.unresolved_items, "search.open-world-virtual-target"),
				"open-world uncertainty was dropped");

		auto incomplete_request = request(graph::dispatch_form::virtual_open);
		incomplete_request.inheritance_coverage_complete = false;
		incomplete_request.override_coverage_complete = false;
		auto incomplete = resolver.resolve(incomplete_request);
		require(
			contains_code(incomplete.unresolved_items, "search.inheritance-coverage-incomplete") &&
				contains_code(incomplete.unresolved_items, "search.override-coverage-incomplete"),
			"coverage uncertainty was dropped");

		auto indirect_request = request(graph::dispatch_form::indirect);
		indirect_request.static_target.reset();
		indirect_request.observed_candidates = {derived_step};
		auto indirect = resolver.resolve(indirect_request);
		require(indirect.status == graph::resolution_status::partial &&
					contains(indirect.possible_targets, derived_step) &&
					contains_code(indirect.unresolved_items, "search.indirect-call-target"),
				"indirect candidate uncertainty was collapsed");

		auto dependent_request = request(graph::dispatch_form::dependent);
		dependent_request.static_target.reset();
		auto dependent = resolver.resolve(dependent_request);
		require(dependent.status == graph::resolution_status::unresolved &&
					contains_code(dependent.unresolved_items, "search.dependent-call-target"),
				"dependent target was not unresolved");

		auto budget_request = request(graph::dispatch_form::virtual_open);
		budget_request.candidate_budget = 1U;
		auto budget = resolver.resolve(budget_request);
		require(budget.possible_targets.size() == 1U &&
					contains_code(budget.unresolved_items, "search.candidate-budget-exhausted"),
				"candidate budget was not enforced");

		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled_request = request(graph::dispatch_form::virtual_open);
		cancelled_request.cancellation = cancellation.get_token();
		require(
			contains_code(resolver.resolve(cancelled_request).unresolved_items, "core.cancelled"),
			"resolver cancellation was ignored");

		auto variants = request(graph::dispatch_form::virtual_open);
		variants.static_target.reset();
		variants.variant_policy = select::variant_match_policy::reject_disagreement;
		variants.variants = {{variant_b, other_step, {}, graph::dispatch_form::direct},
							 {variant_a, base_step, {}, graph::dispatch_form::direct}};
		auto variant_result = resolver.resolve(variants);
		require(variant_result.per_variant.size() == 2U &&
					variant_result.per_variant.front().variant == variant_a &&
					contains_code(variant_result.unresolved_items,
								  "search.variant-candidate-disagreement"),
				"variant disagreement or canonical order was lost");
		variants.variant_policy = select::variant_match_policy::all_variants;
		require(resolver.resolve(variants).possible_targets.empty(),
				"all-variant policy did not intersect candidate sets");

		const symbol_id diamond_leaf{id("symbol_", 7U)};
		graph::virtual_candidate_resolver diamond{{
			override_relation(derived_step, base_step, 30U),
			override_relation(other_step, base_step, 31U),
			override_relation(diamond_leaf, derived_step, 32U),
			override_relation(diamond_leaf, other_step, 33U),
		}};
		auto diamond_result = diamond.resolve(request(graph::dispatch_form::virtual_open));
		require(diamond_result.validate() && diamond_result.possible_targets.size() == 4U &&
					std::ranges::count(diamond_result.possible_targets, diamond_leaf) == 1,
				"override diamond did not deduplicate its transitive candidate");
	}

	void check_executor()
	{
		require(snapshot()->validate().has_value(), "query snapshot fixture is invalid");
		query::execution_options options;
		options.candidate_budget = 16U;
		options.refinement_budget = 8U;
		options.closed_world = true;
		auto executed = query::execute(plan(), fact_store_fixture(), options);
		require(executed.has_value(), "flagship query execution failed");
		const auto& value = executed.value();
		require(value.validate().has_value() && value.trace.accounting.balanced(),
				"query result accounting is invalid");
		require(value.trace.accounting.considered == 3U && value.trace.accounting.matched == 2U &&
					value.trace.accounting.rejected == 1U &&
					value.trace.accounting.unresolved == 0U,
				"flagship match accounting changed");
		require(value.matches.size() == 2U && value.rejected == std::vector<fact_id>{other_call},
				"unrelated same-name method was not rejected");
		require(value.matches.front().call == base_call &&
					contains(value.matches.front().possible_targets, base_step) &&
					contains(value.matches.front().possible_targets, derived_step),
				"base virtual call candidate closure is wrong");
		require(value.matches.back().call == derived_call &&
					value.matches.back().possible_targets == std::vector<symbol_id>{derived_step},
				"derived virtual call target is wrong");
		require(value.matches.front().per_variant.size() == 1U &&
					value.matches.front().per_variant.front().variant == variant_a,
				"call fact variant provenance was not preserved");
		require(value.guarantee == result_guarantee::exact_within_coverage &&
					!value.to_json().empty() && !value.trace.to_json().empty(),
				"complete closed-world query lost its guarantee or trace");

		auto open_world_options = options;
		open_world_options.closed_world = false;
		auto open_world = query::execute(plan(), fact_store_fixture(), open_world_options);
		require(open_world && open_world.value().guarantee == result_guarantee::best_effort &&
					contains_code(open_world.value().unresolved_items,
								  "search.open-world-virtual-target"),
				"open-world virtual query was reported as complete");

		auto incomplete = query::execute(plan(), fact_store_fixture(false), options);
		require(incomplete && incomplete.value().guarantee == result_guarantee::best_effort &&
					contains_code(incomplete.value().unresolved_items,
								  "search.inheritance-coverage-incomplete") &&
					contains_code(incomplete.value().unresolved_items,
								  "search.override-coverage-incomplete"),
				"incomplete coverage was reported as exact");

		auto budget_options = options;
		budget_options.candidate_budget = 1U;
		auto budgeted = query::execute(plan(8U, 1U), fact_store_fixture(), budget_options);
		require(budgeted && budgeted.value().trace.budget_exhausted &&
					budgeted.value().trace.accounting.balanced() &&
					budgeted.value().trace.accounting.unresolved == 2U,
				"executor budget did not produce balanced partial accounting");

		std::stop_source cancellation;
		cancellation.request_stop();
		auto cancelled_options = options;
		cancelled_options.execution.cancellation = cancellation.get_token();
		auto cancelled = query::execute(plan(), fact_store_fixture(), cancelled_options);
		require(cancelled && cancelled.value().trace.cancelled &&
					cancelled.value().trace.accounting.balanced() &&
					cancelled.value().trace.accounting.unresolved == 3U,
				"executor cancellation did not preserve balanced accounting");

		refinement qualified{graph::dispatch_form::qualified_virtual};
		auto qualified_result = query::execute(plan(), fact_store_fixture(), options, &qualified);
		require(qualified_result && qualified_result.value().trace.refinements_succeeded == 2U &&
					qualified_result.value().matches.front().possible_targets ==
						std::vector<symbol_id>{base_step},
				"qualified refinement expanded virtual targets");

		refinement final_receiver{graph::dispatch_form::final_virtual};
		auto final_result = query::execute(plan(), fact_store_fixture(), options, &final_receiver);
		require(final_result &&
					final_result.value().matches.front().possible_targets ==
						std::vector<symbol_id>{base_step},
				"final receiver refinement expanded virtual targets");

		refinement failing{graph::dispatch_form::dependent, true};
		auto failed = query::execute(plan(), fact_store_fixture(), options, &failing);
		require(failed && failed.value().trace.refinements_failed == 2U &&
					contains_code(failed.value().unresolved_items, "search.refinement-failed"),
				"refinement failure was hidden");

		auto refinement_limited = options;
		refinement_limited.refinement_budget = 1U;
		auto limited =
			query::execute(plan(1U), fact_store_fixture(), refinement_limited, &qualified);
		require(limited && limited.value().trace.budget_exhausted &&
					limited.value().trace.refinements_requested == 1U &&
					contains_code(limited.value().unresolved_items,
								  "search.refinement-budget-exhausted"),
				"refinement budget exhaustion was hidden");

		auto output_limited_options = options;
		output_limited_options.result_limit = 1U;
		auto output_limited =
			query::execute(plan(8U, 16U, 1U), fact_store_fixture(), output_limited_options);
		require(output_limited && output_limited.value().matches.size() == 1U &&
					output_limited.value().trace.output_limited &&
					contains_code(output_limited.value().unresolved_items,
								  "search.result-limit-exhausted") &&
					output_limited.value().guarantee == result_guarantee::best_effort,
				"result limit was silently reported as complete");
	}

	void check_provisioning_and_warm_reuse()
	{
		auto opened = testing::workspace_fixture::cpp("int fixture_query(){return 0;}").open();
		require(opened.has_value(), "query workspace fixture did not open");
		auto workspace = std::move(opened.value());
		auto worker = std::make_shared<semantic_worker>();
		workspace_provisioning_access::set_worker(workspace, worker);
		query::execution_options options;
		options.candidate_budget = 16U;
		options.refinement_budget = 8U;
		options.closed_world = true;
		auto first = query::provision_compile_execute(workspace, flagship(), options);
		require(first && first.value().matches.size() == 2U && worker->calls == 1U,
				"query provisioning did not execute the exact semantic fixture");
		const auto first_json = first.value().to_json();
		auto second = query::provision_compile_execute(workspace, flagship(), options);
		require(second && second.value().to_json() == first_json && worker->calls == 1U,
				"warm query provisioning reparsed or changed its result");
		const auto trace = workspace_provisioning_access::last_trace(workspace);
		require(trace.validate() && trace.scheduled == 0U && trace.warm > 0U,
				"warm query provisioning trace did not prove reuse");
	}

	[[nodiscard]] std::string
	execute_backend(const std::shared_ptr<store::fact_store_port>& backend,
					const std::shared_ptr<const store::snapshot_data>& fixture_snapshot)
	{
		facts::reduction_result reduction;
		reduction.facts = fixture_snapshot->facts;
		reduction.coverage = fixture_snapshot->coverage;
		require(reduction.validate().has_value(), "backend reduction fixture is invalid");
		auto transaction = backend->begin(fixture_snapshot->metadata);
		require(transaction.has_value(), "backend transaction did not begin");
		require(transaction.value()->stage(reduction).has_value() &&
					transaction.value()->validate().has_value() &&
					transaction.value()->commit().has_value(),
				"backend transaction did not commit");
		auto stored = backend->read();
		require(stored.has_value(), "backend snapshot could not be read");
		query::execution_options options;
		options.candidate_budget = 16U;
		options.refinement_budget = 8U;
		options.closed_world = false;
		auto executed = query::execute(
			plan(), fact_store_access::make_store(std::move(stored.value())), options);
		require(executed.has_value(), "backend query execution failed");
		return executed.value().to_json();
	}

	void check_backend_equivalence()
	{
		const auto fixture_snapshot = snapshot();
		auto memory = store::make_in_memory_store(fixture_snapshot->metadata);
		require(memory.has_value(), "in-memory query backend did not open");
		const auto memory_json = execute_backend(memory.value(), fixture_snapshot);

		const auto database =
			std::filesystem::path{CXXLENS_QUERY_TEST_DIRECTORY} / "query-backend.sqlite3";
		std::filesystem::remove(database);
		std::filesystem::remove(database.string() + "-wal");
		std::filesystem::remove(database.string() + "-shm");
		auto sqlite = store::open_sqlite_store(database, fixture_snapshot->metadata);
		require(sqlite.has_value(), "SQLite query backend did not open");
		const auto sqlite_json = execute_backend(sqlite.value(), fixture_snapshot);
		require(sqlite_json == memory_json,
				"in-memory and SQLite query backends changed canonical semantics");
	}
} // namespace

int main()
{
	check_plan();
	check_resolver();
	check_executor();
	check_provisioning_and_warm_reuse();
	check_backend_equivalence();
	return 0;
}
