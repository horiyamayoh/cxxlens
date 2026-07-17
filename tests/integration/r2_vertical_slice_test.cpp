#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <sstream>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/cc_type.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <cxxlens/sdk.hpp>

#include "llvm/clang22/provider_worker.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::sdk;
	namespace query = cxxlens::sdk::query;
	using cxxlens::detail::clang22::canonicalized_provider_batch;
	using cxxlens::detail::clang22::detached_observation;
	using cxxlens::detail::clang22::observation_batch;
	using cxxlens::detail::clang22::observation_kind;

	constexpr std::string_view clang_contract{
		"sha256:1111111111111111111111111111111111111111111111111111111111111111"};
	constexpr std::string_view lock_contract{
		"sha256:2222222222222222222222222222222222222222222222222222222222222222"};
	constexpr std::string_view basis_digest{
		"sha256:3333333333333333333333333333333333333333333333333333333333333333"};

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] detached_cell optional_typed(std::string parameter, std::string value)
	{
		auto output = detached_cell::typed(std::move(parameter), std::move(value));
		output.type.optional = true;
		return output;
	}

	[[nodiscard]] detached_cell open_symbol(std::string parameter, std::string value)
	{
		return {{scalar_kind::open_symbol, std::move(parameter), false},
				cell_state::present,
				scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] std::string string_cell(const detached_row& row, const std::string& column)
	{
		const auto found = row.cells.find(column);
		require(found != row.cells.end() && found->second.value.has_value(),
				"expected present string cell: " + column);
		const auto* value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "expected string scalar: " + column);
		return *value;
	}

	[[nodiscard]] detached_observation entity_observation(std::string semantic_key,
														  std::string qualified_name,
														  const char source_suffix)
	{
		detached_observation value;
		value.kind = observation_kind::entity;
		value.compile_unit = "cu-" + std::string(64U, 'a');
		value.semantic_key = std::move(semantic_key);
		value.source_span_id = "span-" + std::string(63U, 'd') + source_suffix;
		value.payload.emplace("symbol.kind", "function");
		value.payload.emplace("symbol.qualified_name", std::move(qualified_name));
		value.payload.emplace("symbol.signature", "void ()");
		return value;
	}

	[[nodiscard]] detached_observation
	call_observation(std::string semantic_key, std::string callee, const char source_suffix)
	{
		detached_observation value;
		value.kind = observation_kind::call;
		value.compile_unit = "cu-" + std::string(64U, 'a');
		value.semantic_key = std::move(semantic_key);
		value.source_span_id = "span-" + std::string(63U, 'e') + source_suffix;
		value.payload.emplace("call.kind", "direct_function");
		value.payload.emplace("call.caller", callee);
		value.payload.emplace("call.direct_callee", std::move(callee));
		return value;
	}

	[[nodiscard]] canonicalized_provider_batch clang_rows(const std::string& qualified_name,
														  const bool ambiguous)
	{
		observation_batch batch;
		batch.unit = "cu-" + std::string(64U, 'a');
		batch.variant = "variant-" + std::string(64U, 'b');
		batch.observations.push_back(entity_observation("clang-usr:target-a", qualified_name, '1'));
		batch.observations.push_back(call_observation("call:caller:1", "clang-usr:target-a", '1'));
		if (ambiguous)
		{
			batch.observations.push_back(
				entity_observation("clang-usr:target-b", qualified_name, '2'));
			batch.observations.push_back(
				call_observation("call:caller:2", "clang-usr:target-b", '2'));
		}
		require(batch.validate().has_value(), "Clang observation batch rejected");
		auto normalized =
			detail::clang22::canonicalize_provider_batch(batch, std::string{clang_contract}, true);
		require(normalized.has_value(), "Clang observation canonicalization failed");
		return std::move(*normalized);
	}

	[[nodiscard]] detached_row lock_row(std::string acquire, std::string function, std::string mode)
	{
		using relation = company::relations::lock_acquire;
		relation::builder builder;
		for (auto result : {
				 builder.set<relation::acquire>(
					 detached_cell::typed("company_lock_acquire_id", std::move(acquire))),
				 builder.set<relation::lock>(
					 detached_cell::typed("company_lock_id", "lock:global")),
				 builder.set<relation::function>(
					 optional_typed("cc_entity_id", std::move(function))),
				 builder.set<relation::source>(
					 detached_cell::typed("source_span_id", "span-" + std::string(64U, 'f'))),
				 builder.set<relation::mode>(open_symbol("company.lock-mode/1", std::move(mode))),
				 builder.set<relation::ordinal>(detached_cell::unsigned_integer(0U)),
			 })
			require(result.has_value(), "lock row column rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "lock row rejected");
		return std::move(*row);
	}

	class lock_provider final : public provider::portable_provider
	{
	  public:
		explicit lock_provider(detached_row row) : row_{std::move(row)} {}

		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "company.lock.provider@1.0.0";
		}

		[[nodiscard]] semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}

		[[nodiscard]] result<void> run(const provider::task& task,
									   provider::context& context) override
		{
			auto sink = context.relation(company::relations::lock_acquire::descriptor());
			if (auto opened = sink.begin("calls", "lock-output", "lock-batch"); !opened)
				return opened;
			if (auto pushed = sink.push(row_); !pushed)
				return pushed;
			if (auto ended = sink.end(); !ended)
				return ended;
			context.coverage().request("compile-unit", task.project.compile_units.front());
			if (auto covered = context.coverage().classify(
					{"compile-unit", task.project.compile_units.front(), "covered", {}});
				!covered)
				return covered;
			context.coverage().request("task", task.task_id);
			if (auto covered = context.coverage().classify({"task", task.task_id, "covered", {}});
				!covered)
				return covered;
			context.evidence().add(
				{"provider.observation", "lock:global", std::string{id()}, "lock acquire"});
			return {};
		}

	  private:
		detached_row row_;
	};

	struct fixture
	{
		relation_engine engine;
		std::vector<claim> claims;
		std::string target_entity;
	};

	[[nodiscard]] claim make_claim(const relation_engine& engine,
								   detached_row row,
								   claim_producer producer,
								   std::string interpretation)
	{
		auto value =
			make_assertion(engine,
						   {std::move(row),
							{"r2.universe", {"all"}},
							std::move(interpretation),
							std::move(producer),
							{std::string{basis_digest}},
							"evidence:r2-vertical-slice",
							{"exact", "compile-unit", "assumptions:none", {"schema_validated"}}});
		require(value.has_value(), "vertical-slice assertion rejected");
		return std::move(*value);
	}

	[[nodiscard]] fixture
	make_fixture(const std::string& qualified_name, const bool ambiguous, const bool differential)
	{
		auto normalized = clang_rows(qualified_name, ambiguous);
		relation_registry registry;
		for (const auto* descriptor : {
				 &build::relations::project::descriptor(),
				 &build::relations::variant::descriptor(),
				 &build::relations::toolchain_context::descriptor(),
				 &source::relations::file::descriptor(),
				 &source::relations::span::descriptor(),
				 &build::relations::compile_unit::descriptor(),
				 &cc::relations::type::descriptor(),
				 &cc::relations::entity::descriptor(),
				 &cc::relations::call_site::descriptor(),
				 &cc::relations::call_direct_target::descriptor(),
				 &company::relations::lock_acquire::descriptor(),
			 })
			require(registry.add(*descriptor).has_value(), "relation registry rejected descriptor");
		auto engine = registry.build("r2-generation");
		require(engine.has_value(),
				"vertical-slice relation engine failed: " +
					(engine ? std::string{} : engine.error().code + ":" + engine.error().field));
		require(!normalized.entities.empty(), "normalizer emitted no entity");
		const auto target = string_cell(normalized.entities.front(), "cc.entity.v1.entity");
		std::vector<claim> claims;
		for (auto& row : normalized.entities)
			claims.push_back(
				make_claim(*engine,
						   std::move(row),
						   {"cxxlens.clang22.reference@1.0.0", std::string{clang_contract}},
						   "cxxlens.clang22"));
		for (auto& row : normalized.call_sites)
			claims.push_back(
				make_claim(*engine,
						   std::move(row),
						   {"cxxlens.clang22.reference@1.0.0", std::string{clang_contract}},
						   "cxxlens.clang22"));
		for (auto& row : normalized.direct_targets)
			claims.push_back(
				make_claim(*engine,
						   std::move(row),
						   {"cxxlens.clang22.reference@1.0.0", std::string{clang_contract}},
						   "cxxlens.clang22"));
		claims.push_back(make_claim(*engine,
									lock_row("lock-acquire:1", target, "exclusive"),
									{"company.lock.provider@1.0.0", std::string{lock_contract}},
									"cxxlens.clang22"));
		if (differential)
			claims.push_back(make_claim(*engine,
										lock_row("lock-acquire:1", target, "shared"),
										{"company.lock.provider@1.0.0", std::string{lock_contract}},
										"company.lock.alternative"));
		return {std::move(*engine), std::move(claims), target};
	}

	[[nodiscard]] snapshot_draft draft(const relation_engine& engine)
	{
		return {{"catalog:r2",
				 "stable",
				 std::string{engine.generation()},
				 "r2.universe",
				 std::string{engine.registry_digest()},
				 "sha256:4444444444444444444444444444444444444444444444444444444444444444",
				 "sha256:5555555555555555555555555555555555555555555555555555555555555555"},
				{1U, 0U, 0U},
				"sha256:6666666666666666666666666666666666666666666666666666666666666666",
				std::nullopt};
	}

	[[nodiscard]] partition_draft
	partition(const claim& value, const bool incomplete, const std::size_t ordinal)
	{
		auto basis = claim_input_basis_digest(value.input_basis);
		require(basis.has_value(), "partition input basis rejected");
		partition_draft output;
		output.relation_descriptor_id = value.descriptor;
		output.scope = "r2:" + std::to_string(ordinal);
		output.condition = value.presence;
		output.interpretation = value.interpretation;
		output.producer_semantics = value.producer.semantic_contract;
		output.producer_input_basis_digest = std::move(*basis);
		output.precision_profile = "exact";
		output.assumption_set_id = "assumptions:none";
		output.claims = {value};
		output.coverage = {{"compile-unit",
							"cu-" + std::string(64U, 'a'),
							incomplete ? "unresolved" : "covered",
							incomplete ? "provider.partial" : ""}};
		if (incomplete)
			output.unresolved = {
				{value.assertion, value.descriptor, value.descriptor, {}, "provider.partial"}};
		return output;
	}

	[[nodiscard]] snapshot_handle
	publish(snapshot_store& store, const fixture& data, const bool reverse, const bool incomplete)
	{
		auto writer = store.begin(draft(data.engine));
		require(writer.has_value(), "snapshot writer failed");
		std::vector<std::size_t> order(data.claims.size());
		for (std::size_t index = 0U; index < order.size(); ++index)
			order[index] = index;
		if (reverse)
			std::ranges::reverse(order);
		for (const auto index : order)
			require(writer
						->stage(partition(data.claims[index],
										  incomplete &&
											  data.claims[index].descriptor ==
												  cc::relations::entity::descriptor().id,
										  index))
						.has_value(),
					"snapshot partition stage failed");
		require(writer->validate().has_value(), "snapshot validation failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "snapshot publication failed");
		return std::move(*snapshot);
	}

	[[nodiscard]] std::vector<std::string> rows(const query::query_result& result)
	{
		std::vector<std::string> output;
		auto cursor = result.rows();
		while (true)
		{
			auto next = cursor.next();
			require(next.has_value(), "query result cursor failed");
			if (!*next)
				break;
			auto row = (*next)->copy();
			require(row.has_value(), "query result row copy failed");
			output.push_back(row->canonical_form());
		}
		return output;
	}

	[[nodiscard]] std::string semantic_result(const query::query_result& result)
	{
		std::ostringstream output;
		output << result.logical_ir_digest() << '|' << static_cast<int>(result.execution()) << '|'
			   << result.inputs_complete() << '|' << result.closed() << '|'
			   << result.summary_guarantee().approximation << '\n';
		for (const auto& row : rows(result))
			output << row << '\n';
		for (const auto& producer : result.producer_contracts())
			output << producer.id << '|' << producer.semantic_contract << '\n';
		for (const auto& unresolved : result.unresolved_items())
			output << unresolved.code << '|' << unresolved.subject << '|' << unresolved.detail
				   << '\n';
		for (const auto& disagreement : result.differential_disagreements())
			output << disagreement.relation << '|' << disagreement.semantic_key << '|'
				   << disagreement.left_interpretation << '|' << disagreement.right_interpretation
				   << '\n';
		return *semantic_digest("cxxlens.r2-query-result.v1", output.str());
	}

	class cancel_after_first_row final : public query::cancellation_probe
	{
	  public:
		[[nodiscard]] bool
		stop_requested(const query::execution_checkpoint& checkpoint) const noexcept override
		{
			return checkpoint.current == query::execution_checkpoint::phase::before_publish_row &&
				checkpoint.ordinal == 1U;
		}
	};

	[[nodiscard]] query::logical_query_ir typed_custom_query()
	{
		using call = cc::relations::call_site;
		using lock = company::relations::lock_acquire;
		auto left = query::from<call>();
		auto right = query::from<lock>();
		auto predicate =
			query::equals_present(query::col<call::caller>(), query::col<lock::function>());
		require(left && right && predicate, "typed custom query setup failed");
		auto joined = std::move(*left).inner_join(std::move(*right), std::move(*predicate));
		require(joined.has_value(), "typed custom join failed");
		const std::array order{query::col<call::call>(), query::col<lock::acquire>()};
		auto ordered = std::move(*joined).order_by(order);
		require(ordered.has_value(), "typed custom order failed");
		const std::array projection{query::col<call::call>(),
									query::col<call::source>(),
									query::col<lock::lock>(),
									query::col<lock::mode>()};
		auto projected = std::move(*ordered).project(projection);
		require(projected.has_value(), "typed custom projection failed");
		return std::move(*projected).finish();
	}

	[[nodiscard]] query::logical_query_ir dynamic_custom_query(const relation_engine& engine)
	{
		auto call = engine.require("cc.call_site", 1U);
		auto lock = engine.require("company.lock.acquire", 1U);
		require(call && lock, "dynamic custom relation lookup failed");
		auto call_caller = call->column("caller");
		auto call_id = call->column("call");
		auto call_source = call->column("source");
		auto lock_function = lock->column("function");
		auto acquire = lock->column("acquire");
		auto lock_id = lock->column("lock");
		auto mode = lock->column("mode");
		require(call_caller && call_id && call_source && lock_function && acquire && lock_id &&
					mode,
				"dynamic custom column lookup failed");
		auto left = query::dynamic_query::from(*call);
		auto right = query::dynamic_query::from(*lock);
		auto predicate = query::equals_present(*call_caller, *lock_function);
		require(left && right && predicate, "dynamic custom query setup failed");
		auto joined = std::move(*left).inner_join(std::move(*right), std::move(*predicate));
		require(joined.has_value(), "dynamic custom join failed");
		const std::array order{*call_id, *acquire};
		auto ordered = std::move(*joined).order_by(order);
		require(ordered.has_value(), "dynamic custom order failed");
		const std::array projection{*call_id, *call_source, *lock_id, *mode};
		auto projected = std::move(*ordered).project(projection);
		require(projected.has_value(), "dynamic custom projection failed");
		return std::move(*projected).finish();
	}

	void check_portable_provider(const detached_row& row,
								 const std::string& root,
								 const std::uint64_t jobs)
	{
		lock_provider implementation{row};
		provider::task task;
		task.task_id = "r2-task-jobs-" + std::to_string(jobs);
		task.project = {"catalog:r2", "sha256:catalog-r2", root, {"cu-" + std::string(64U, 'a')}};
		task.outputs = {company::relations::lock_acquire::descriptor()};
		task.condition = "condition:all";
		task.interpretation = "cxxlens.clang22";
		testing::provider_harness harness;
		auto report = harness.run(implementation, task);
		require(report && report->accepted && report->frames.size() >= 7U,
				"portable provider did not emit the external relation: " +
					(report ? report->reason_code : std::string{"no-report"}));
	}

	void check_provider_fault_preserves_snapshot(const snapshot_handle& snapshot,
												 const detached_row& row)
	{
		lock_provider implementation{row};
		provider::task task;
		task.task_id = "r2-fault";
		task.project = {"catalog:r2", "sha256:catalog-r2", ".", {"cu-" + std::string(64U, 'a')}};
		task.outputs = {company::relations::lock_acquire::descriptor()};
		task.condition = "condition:all";
		task.interpretation = "cxxlens.clang22";
		testing::provider_harness harness;
		auto failed =
			harness.run(implementation, task, testing::provider_fault::truncate_last_frame);
		require(failed && !failed->accepted, "provider fault was accepted");
		auto recipe = recipes::calls_to_function("ns::target");
		require(recipe.has_value(), "prior-snapshot recipe setup failed");
		auto report = recipe->run(snapshot);
		require(report && report->state() == recipes::call_search_state::matched,
				"provider fault invalidated the prior snapshot");
	}
} // namespace

int main()
{
	auto data = make_fixture("ns::target", false, true);
	const auto lock = lock_row("lock-acquire:1", data.target_entity, "exclusive");
	for (const auto jobs : {1U, 2U, 8U})
		for (const auto* root : {"/workspace/a", "/relocated/workspace"})
			check_portable_provider(lock, root, jobs);

	auto memory_store = make_in_memory_snapshot_store(data.engine);
	require(memory_store.has_value(), "memory store failed");
	auto memory_snapshot = publish(*memory_store, data, false, false);

	const auto database =
		std::filesystem::temp_directory_path() / "cxxlens-r2-vertical-slice.sqlite";
	std::filesystem::remove(database);
	std::filesystem::remove(database.string() + "-wal");
	std::filesystem::remove(database.string() + "-shm");
	std::string sqlite_snapshot_id;
	{
		auto sqlite_store = open_sqlite_snapshot_store(database.string(), data.engine);
		require(sqlite_store.has_value(), "SQLite store failed");
		auto sqlite_snapshot = publish(*sqlite_store, data, true, false);
		require(sqlite_snapshot.id() == memory_snapshot.id(),
				"memory/SQLite semantic snapshot IDs diverged");
		sqlite_snapshot_id = sqlite_snapshot.id();
	}
	auto reopened = open_sqlite_snapshot_store(database.string(), data.engine);
	require(reopened.has_value(), "SQLite cold reopen failed");
	auto sqlite_snapshot = reopened->open(sqlite_snapshot_id);
	require(sqlite_snapshot.has_value(), "SQLite cold snapshot open failed");

	const auto typed = typed_custom_query();
	const auto dynamic = dynamic_custom_query(data.engine);
	require(typed.digest() == dynamic.digest(), "typed/dynamic custom IR digests diverged");
	auto memory_runtime = query::reference_engine::bind(memory_snapshot);
	auto sqlite_runtime = query::reference_engine::bind(*sqlite_snapshot);
	require(memory_runtime && sqlite_runtime, "query runtime bind failed");
	auto typed_memory = memory_runtime->execute(typed);
	auto dynamic_memory = memory_runtime->execute(dynamic);
	auto typed_sqlite = sqlite_runtime->execute(typed);
	require(typed_memory && dynamic_memory && typed_sqlite, "custom query execution failed");
	require(semantic_result(*typed_memory) == semantic_result(*dynamic_memory),
			"typed/dynamic semantic results diverged");
	require(semantic_result(*typed_memory) == semantic_result(*typed_sqlite),
			"memory/SQLite semantic query results diverged");
	require(typed_memory->producer_contracts().size() == 2U &&
				typed_memory->differential_disagreements().size() == 1U && !typed_memory->closed(),
			"producer, differential, or closure evidence was lost");
	require(typed_memory->explain_logical().id == typed.digest() &&
				typed_memory->explain_physical().text.contains("backend=memory"),
			"logical/physical explanation boundary failed");

	auto recipe = recipes::calls_to_function("ns::target");
	require(recipe.has_value(), "calls_to_function recipe creation failed");
	auto plan = recipe->lower();
	require(plan && plan->descriptor().version == semantic_version{1U, 1U, 0U} &&
				plan->requirements().size() == 3U,
			"recipe lowering requirements or semantics version differ");
	auto memory_report = recipe->run(memory_snapshot);
	auto cold_report = recipe->run(*sqlite_snapshot);
	require(memory_report && cold_report &&
				memory_report->state() == recipes::call_search_state::matched &&
				semantic_result(memory_report->result()) == semantic_result(cold_report->result()),
			"recipe memory/cold-SQLite result diverged");
	auto warm_report = recipe->run(*sqlite_snapshot);
	require(warm_report &&
				semantic_result(cold_report->result()) == semantic_result(warm_report->result()),
			"cold/warm recipe result diverged");
	require(memory_report->canonical_form().contains("cxxlens.calls-to-function-report.v1") &&
				memory_report->canonical_form().contains("producer_contracts") &&
				memory_report->canonical_form().contains("logical_explanation"),
			"recipe report omitted required evidence");

	auto empty_data = make_fixture("ns::other", false, false);
	auto empty_store = make_in_memory_snapshot_store(empty_data.engine);
	require(empty_store.has_value(), "empty store failed");
	auto empty_snapshot = publish(*empty_store, empty_data, false, false);
	auto empty_report = recipe->run(empty_snapshot);
	require(empty_report && empty_report->state() == recipes::call_search_state::empty_complete,
			"empty-complete recipe state collapsed");

	auto incomplete_store = make_in_memory_snapshot_store(empty_data.engine);
	require(incomplete_store.has_value(), "incomplete store failed");
	auto incomplete_snapshot = publish(*incomplete_store, empty_data, false, true);
	auto incomplete_report = recipe->run(incomplete_snapshot);
	require(incomplete_report &&
				incomplete_report->state() == recipes::call_search_state::empty_incomplete &&
				!incomplete_report->result().unresolved_items().empty(),
			"empty-incomplete recipe state collapsed");

	auto ambiguous_data = make_fixture("ns::target", true, false);
	auto ambiguous_store = make_in_memory_snapshot_store(ambiguous_data.engine);
	require(ambiguous_store.has_value(), "ambiguous store failed");
	auto ambiguous_snapshot = publish(*ambiguous_store, ambiguous_data, true, false);
	auto ambiguous_report = recipe->run(ambiguous_snapshot);
	require(ambiguous_report && ambiguous_report->state() == recipes::call_search_state::ambiguous,
			"ambiguous recipe state collapsed");

	std::stop_source stopped;
	stopped.request_stop();
	query::stop_token_cancellation before_execution{stopped.get_token()};
	query::execution_request cancelled_before_execution;
	cancelled_before_execution.cancellation = &before_execution;
	auto cancelled_empty = recipe->run(memory_snapshot, cancelled_before_execution);
	require(cancelled_empty &&
				cancelled_empty->result().execution() ==
					query::execution_status::failed_before_result &&
				cancelled_empty->result().inputs_complete() &&
				cancelled_empty->state() == recipes::call_search_state::failed,
			"execution-before-cancel was promoted to a complete empty recipe state");

	for (auto budget : {query::execution_budget{0U},
						query::execution_budget{std::numeric_limits<std::uint64_t>::max(),
												std::numeric_limits<std::uint64_t>::max(),
												0U,
												std::numeric_limits<std::uint64_t>::max()}})
	{
		query::execution_request failed_request;
		failed_request.budget = budget;
		auto failed_report = recipe->run(memory_snapshot, failed_request);
		require(failed_report &&
					failed_report->result().execution() ==
						query::execution_status::failed_before_result &&
					failed_report->state() == recipes::call_search_state::failed,
				"scan/intermediate budget failure was promoted to a complete recipe state");
	}

	query::execution_request output_limited_request;
	output_limited_request.budget.max_rows_output = 1U;
	auto truncated_report = recipe->run(ambiguous_snapshot, output_limited_request);
	require(truncated_report &&
				truncated_report->result().execution() == query::execution_status::truncated &&
				truncated_report->state() == recipes::call_search_state::partial,
			"truncated ambiguous result was promoted to a definitive recipe state");

	cancel_after_first_row cancel_partial;
	query::execution_request partial_request;
	partial_request.cancellation = &cancel_partial;
	auto partial_report = recipe->run(ambiguous_snapshot, partial_request);
	require(partial_report &&
				partial_report->result().execution() ==
					query::execution_status::cancelled_with_partial &&
				partial_report->state() == recipes::call_search_state::partial,
			"cancelled sealed prefix was promoted to a definitive recipe state");

	check_provider_fault_preserves_snapshot(memory_snapshot, lock);

	std::filesystem::remove(database);
	std::filesystem::remove(database.string() + "-wal");
	std::filesystem::remove(database.string() + "-shm");
	return 0;
}
