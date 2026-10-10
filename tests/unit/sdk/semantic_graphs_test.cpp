#include <algorithm>
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

#include <cxxlens/sdk/semantic_graphs.hpp>

#include "../../../src/sdk/query_result_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace q = cxxlens::sdk::query;
	void require(bool condition, std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}
	q::annotated_row row(std::string_view relation,
						 std::initializer_list<std::pair<std::string, detached_cell>> columns)
	{
		q::annotated_row value;
		value.presence = {"graphs:test", {"debug"}};
		value.interpretation = "clang22";
		value.provenance = {"graph:evidence"};
		value.claim_contributors = {"claim:test"};
		value.producer_contracts = {{"graph.test", "semantic:hand-labelled-v1"}};
		value.contributor_guarantees = {
			{"exact", "hand-labelled-local-graph", "fixture", {"schema_validated"}}};
		value.contributor_edges = {{value.claim_contributors.front(),
									value.producer_contracts.front(),
									value.provenance.front(),
									value.contributor_guarantees.front(),
									value.presence,
									value.interpretation}};
		for (const auto& [key, cell] : columns)
		{
			auto typed = cell;
			const auto descriptors = standard_relation_descriptors();
			const auto descriptor =
				std::ranges::find(descriptors, relation, &relation_descriptor::id);
			require(descriptor != descriptors.end(), "fixture descriptor missing");
			const auto column =
				std::ranges::find(descriptor->columns, key, &column_descriptor::name);
			require(column != descriptor->columns.end(), "fixture column missing");
			typed.type = column->type;
			value.values.emplace("output." + key, std::move(typed));
		}
		return value;
	}
	struct fixture
	{
		std::vector<q::annotated_row> units, files, entities, edges, sites, targets, includes;
		fixture()
		{
			units = {row("build.compile_unit.v1",
						 {{"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")}})};
			for (const auto name : {"caller", "callee"})
				entities.push_back(row("cc.entity.v1",
									   {{"entity", detached_cell::typed("cc_entity_id", name)},
										{"kind", detached_cell::utf8("function")},
										{"qualified_name", detached_cell::utf8(name)}}));
			sites = {row("cc.call_site.v1",
						 {{"call", detached_cell::typed("call_id", "call:test")},
						  {"caller", detached_cell::typed("cc_entity_id", "caller")},
						  {"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")},
						  {"source", detached_cell::typed("source_span_id", "span:call")},
						  {"kind", detached_cell::utf8("direct")}})};
			targets = {row("cc.call_direct_target.v1",
						   {{"call", detached_cell::typed("call_id", "call:test")},
							{"target", detached_cell::typed("cc_entity_id", "callee")},
							{"resolution", detached_cell::utf8("direct")}})};
			for (const auto name : {"main", "header"})
				files.push_back(
					row("source.file.v1",
						{{"file", detached_cell::typed("file_id", name)},
						 {"logical_path",
						  detached_cell::utf8(std::string{"project://root/"} + name)}}));
			includes = {row("source.include.v1",
							{{"include", detached_cell::typed("include_id", "include:test")},
							 {"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")},
							 {"from_file", detached_cell::typed("file_id", "main")},
							 {"to_file", detached_cell::typed("file_id", "header")},
							 {"resolution", detached_cell::utf8("resolved")},
							 {"source", detached_cell::typed("source_span_id", "span:include")}})};
			edges = {row("cc.entity_edge.v1",
						 {{"edge", detached_cell::typed("entity_edge_id", "edge:test")},
						  {"compile_unit", detached_cell::typed("compile_unit_id", "tu:test")},
						  {"source_entity", detached_cell::typed("cc_entity_id", "caller")},
						  {"target_entity", detached_cell::typed("cc_entity_id", "callee")},
						  {"resolution", detached_cell::utf8("resolved")},
						  {"kind", detached_cell::utf8("uses_type")},
						  {"source", detached_cell::typed("source_span_id", "span:type")}})};
		}
		q::semantic_graph_input input(bool complete = true) const
		{
			return {units, files, entities, edges, sites, targets, includes, complete};
		}
	};
	q::application_query_results
	queries(const fixture& fixture, bool complete = true, bool closed = true)
	{
		q::application_query_results result;
		result.snapshot_id = "snapshot:test";
		const auto input = fixture.input();
		const std::array groups{input.units,
								input.files,
								input.entities,
								input.entity_edges,
								input.call_sites,
								input.call_targets,
								input.includes};
		const std::array<std::string, 7> names{"build.compile_unit.v1",
											   "source.file.v1",
											   "cc.entity.v1",
											   "cc.entity_edge.v1",
											   "cc.call_site.v1",
											   "cc.call_direct_target.v1",
											   "source.include.v1"};
		for (std::size_t i = 0; i < groups.size(); ++i)
		{
			auto data = std::make_shared<q::query_result::data>();
			data->row_values.assign(groups[i].begin(), groups[i].end());
			data->status = q::execution_status::complete;
			data->input_complete = complete;
			data->closed_world = closed;
			if (closed)
				data->closures = {"closure:" + names[i]};
			data->snapshot = result.snapshot_id;
			q::logical_query_ir plan;
			plan.root = "independent-scan";
			plan.nodes.push_back({"independent-scan", "scan", {}, "relation=" + names[i]});
			result.scans.push_back(
				{names[i], std::move(plan), q::query_transfer_access::make(data)});
		}
		return result;
	}

	const std::array specs{q::semantic_graph_spec{"calls", {"function"}, {"calls"}, false},
						   q::semantic_graph_spec{"includes", {}, {"includes"}, true},
						   q::semantic_graph_spec{"types", {"*"}, {"uses_type"}, false}};
	q::semantic_graph_projection project(const fixture& data)
	{
		auto result = q::project_semantic_graphs(data.input(), specs);
		if (!result)
			std::cerr << result.error().code << ": " << result.error().field << ": "
					  << result.error().detail << '\n';
		require(result.has_value(), "valid graph projection rejected");
		return std::move(*result);
	}
	const q::semantic_graph& graph(const q::semantic_graph_projection& result,
								   std::string_view id,
								   std::string_view variant = "debug")
	{
		const auto found =
			std::ranges::find_if(result.graphs,
								 [&](const auto& value)
								 {
									 return value.spec.id == id && value.variant == variant;
								 });
		require(found != result.graphs.end(), "graph/world omitted");
		return *found;
	}
	bool has_gap(const q::semantic_graph& value, std::string_view code)
	{
		return std::ranges::any_of(value.gaps,
								   [&](const auto& gap)
								   {
									   return gap.code == code;
								   });
	}
} // namespace
int main()
{
	fixture data;
	const auto result = project(data);
	require(result.graphs.size() == 3U, "typed graphs conflated");
	for (const auto& value : result.graphs)
	{
		require(value.state == q::semantic_graph_state::complete && !value.closed,
				"finite input claimed closure");
		require(value.nodes.size() == 2U && value.edges.size() == 1U, "typed topology incorrect");
	}
	require(graph(result, "calls").edges[0].evidence.size() == 2U,
			"call target/source evidence lost");
	const auto original_source =
		result.evidence[graph(result, "calls").edges[0].evidence.front()].row.provenance;
	require(!original_source.empty(), "annotations lost");
	fixture native_direct;
	native_direct.targets.front().values["output.resolution"].value =
		std::string{"syntactic_direct"};
	const auto native_projection = project(native_direct);
	const auto& native_calls = graph(native_projection, "calls");
	require(native_calls.state == q::semantic_graph_state::complete && !native_calls.closed &&
				native_calls.edges.front().resolution == "syntactic_direct" &&
				native_calls.edges.front().evidence.size() == 2U &&
				!has_gap(native_calls, "sdk.graph-edge-unresolved"),
			"native syntactic target became unresolved or claimed closure");
	const auto native_query_projection = q::project_semantic_graphs(queries(native_direct), specs);
	require(native_query_projection &&
				graph(*native_query_projection, "calls").state ==
					q::semantic_graph_state::complete &&
				graph(*native_query_projection, "calls").closed,
			"native target lost independently supplied query closure");
	native_direct.includes.front().values["output.resolution"].value =
		std::string{"syntactic_direct"};
	native_direct.edges.front().values["output.resolution"].value = std::string{"syntactic_direct"};
	const auto foreign_resolution = project(native_direct);
	require(graph(foreign_resolution, "includes").state == q::semantic_graph_state::partial &&
				graph(foreign_resolution, "types").state == q::semantic_graph_state::partial &&
				graph(foreign_resolution, "calls").state == q::semantic_graph_state::complete,
			"call-specific resolution promoted another relation vocabulary");
	std::ranges::reverse(data.entities);
	auto reordered_specs = specs;
	std::ranges::reverse(reordered_specs);
	const auto reordered = q::project_semantic_graphs(data.input(), reordered_specs);
	require(reordered && reordered->graphs[0].edges[0].id == result.graphs[0].edges[0].id &&
				reordered->graphs[0].nodes[0].id == result.graphs[0].nodes[0].id,
			"order changed projection");

	fixture repeated;
	const auto site = repeated.sites.front(), target = repeated.targets.front();
	repeated.sites.assign(1000U, site);
	repeated.targets.assign(1000U, target);
	auto bounded = q::semantic_graph_limits{};
	bounded.maximum_evidence_references = 5000U;
	auto repeated_projection =
		q::project_semantic_graphs(repeated.input(), std::span{specs}.first(1U), bounded);
	require(repeated_projection && repeated_projection->graphs.front().edges.size() == 1U &&
				repeated_projection->graphs.front().edges.front().evidence.size() == 2000U,
			"duplicate call evidence produced a Cartesian fanout");

	fixture unresolved;
	unresolved.targets.clear();
	auto missing = project(unresolved);
	require(graph(missing, "calls").edges.size() == 1U &&
				has_gap(graph(missing, "calls"), "sdk.graph-target-missing"),
			"missing call target became zero");
	unresolved = fixture{};
	unresolved.entities.pop_back();
	require(has_gap(graph(project(unresolved), "calls"), "sdk.graph-target-missing"),
			"missing entity invented");
	unresolved = fixture{};
	unresolved.sites.clear();
	require(has_gap(graph(project(unresolved), "calls"), "sdk.graph-source-missing"),
			"orphan target omitted");
	unresolved = fixture{};
	unresolved.targets.front().interpretation = "gcc";
	unresolved.targets.front().contributor_edges.front().interpretation = "gcc";
	require(has_gap(graph(project(unresolved), "calls"), "sdk.graph-target-missing"),
			"interpretations crossed");
	unresolved = fixture{};
	unresolved.targets.front().presence.fragments = {"release"};
	unresolved.targets.front().contributor_edges.front().condition =
		unresolved.targets.front().presence;
	const auto worlds = project(unresolved);
	require(has_gap(graph(worlds, "calls"), "sdk.graph-target-missing"), "variants crossed");
	require(graph(worlds, "calls", "release").state == q::semantic_graph_state::partial,
			"unobserved world became complete");
	unresolved = fixture{};
	unresolved.targets.front().values["output.compile_unit"] =
		detached_cell::typed("compile_unit_id", "wrong-unit");
	require(has_gap(graph(project(unresolved), "calls"), "sdk.graph-call-owner-mismatch"),
			"call owner ignored");
	unresolved = fixture{};
	auto conflicting = unresolved.targets.front();
	conflicting.values["output.target"] = detached_cell::typed("cc_entity_id", "caller");
	unresolved.targets.push_back(conflicting);
	const auto conflict = project(unresolved);
	require(graph(conflict, "calls").state == q::semantic_graph_state::conflicting &&
				graph(conflict, "calls").edges.size() == 2U,
			"conflict selected a winner");
	unresolved = fixture{};
	unresolved.targets.front().values["output.resolution"].value = std::string{"candidate"};
	require(graph(project(unresolved), "calls").state == q::semantic_graph_state::partial,
			"candidate became definite");
	auto partial = q::project_semantic_graphs(data.input(false), specs);
	require(partial && partial->graphs.front().state == q::semantic_graph_state::partial,
			"unstated enumeration became complete");

	fixture empty;
	empty.entities.clear();
	empty.sites.clear();
	empty.targets.clear();
	auto zero = q::project_semantic_graphs(empty.input(), std::span{specs}.first(1U));
	require(zero && zero->graphs.size() == 1U && zero->graphs.front().nodes.empty() &&
				zero->graphs.front().state == q::semantic_graph_state::complete &&
				!zero->graphs.front().closed,
			"observed empty confused with unobserved");
	for (const auto bound :
		 {"rows", "bytes", "expanded", "references", "nodes", "edges", "graphs", "specs"})
	{
		q::semantic_graph_limits limit;
		const std::string name{bound};
		if (name == "rows")
			limit.maximum_rows = 1U;
		if (name == "bytes")
			limit.maximum_evidence_bytes = 1U;
		if (name == "expanded")
			limit.maximum_expanded_row_bytes = 1U;
		if (name == "references")
			limit.maximum_evidence_references = 1U;
		if (name == "nodes")
			limit.maximum_graph_nodes = 1U;
		if (name == "edges")
		{
			limit.maximum_graph_edges = 1U;
			data.targets.push_back(conflicting);
		}
		if (name == "graphs")
			limit.maximum_graphs = 1U;
		if (name == "specs")
			limit.maximum_specs = 1U;
		const auto rejected = q::project_semantic_graphs(data.input(), specs, limit);
		require(!rejected && rejected.error().code == "sdk.graph-budget", "graph bound not typed");
	}
	q::semantic_graph_limits invalid;
	invalid.maximum_rows = 0U;
	require(!q::project_semantic_graphs(data.input(), specs, invalid), "zero bound accepted");
	auto duplicates = specs;
	duplicates[1].id = duplicates[0].id;
	const auto invalid_specs = q::project_semantic_graphs(data.input(), duplicates);
	require(!invalid_specs && invalid_specs.error().code == "sdk.graph-spec-invalid",
			"ambiguous spec accepted");
	std::stop_source cancellation;
	cancellation.request_stop();
	const auto stopped =
		q::project_semantic_graphs(data.input(), specs, {}, cancellation.get_token());
	require(!stopped && stopped.error().code == "sdk.graph-cancelled", "cancellation ignored");
	q::application_query_results no_scans;
	no_scans.snapshot_id = "snapshot:missing";
	const auto absent = q::project_semantic_graphs(no_scans, specs);
	require(absent && absent->source_queries && absent->unresolved.size() == 7U &&
				absent->graphs.empty(),
			"missing scans became zero or lost original queries");
	fixture valid;
	const auto original_queries = queries(valid);
	const auto closed_projection = q::project_semantic_graphs(original_queries, specs);
	require(closed_projection && closed_projection->source_queries &&
				graph(*closed_projection, "calls").closed &&
				graph(*closed_projection, "calls").closure_ids.size() == 4U,
			"actual query closure or original query metadata lost");
	const auto open_projection = q::project_semantic_graphs(queries(valid, true, false), specs);
	require(open_projection &&
				graph(*open_projection, "calls").state == q::semantic_graph_state::complete &&
				!graph(*open_projection, "calls").closed,
			"open scans acquired closure");
	const auto incomplete_projection =
		q::project_semantic_graphs(queries(valid, false, true), specs);
	require(incomplete_projection &&
				graph(*incomplete_projection, "calls").state == q::semantic_graph_state::partial &&
				!graph(*incomplete_projection, "calls").closed,
			"incomplete scans became a closed exact graph");
	q::semantic_graph_limits plan_limit;
	plan_limit.maximum_source_plan_bytes = 1U;
	const auto plan_budget = q::project_semantic_graphs(original_queries, specs, plan_limit);
	require(!plan_budget && plan_budget.error().code == "sdk.graph-budget",
			"source plan budget ignored");
	q::semantic_graph_limits joined_limit;
	auto expensive = valid;
	for (auto& entity : expensive.entities)
		entity.values["output.qualified_name"].value = std::string(3000U, 'n');
	joined_limit.maximum_evidence_bytes = 1024U;
	const auto expensive_input = expensive.input();
	for (const auto group : {expensive_input.units,
							 expensive_input.files,
							 expensive_input.entities,
							 expensive_input.entity_edges,
							 expensive_input.call_sites,
							 expensive_input.call_targets,
							 expensive_input.includes})
		for (const auto& value : group)
			joined_limit.maximum_evidence_bytes += 3U * value.canonical_form().size();
	const auto pure_bounded =
		q::project_semantic_graphs(expensive.input(), std::span{specs}.first(1U), joined_limit);
	require(pure_bounded.has_value(), "fixture did not fit the row projection budget");
	const auto combined_budget =
		q::project_semantic_graphs(queries(expensive), std::span{specs}.first(1U), joined_limit);
	require(!combined_budget && combined_budget.error().code == "sdk.graph-budget",
			"query copies used an independent row byte allowance");
	valid.entities.front().values["output.entity"] = detached_cell::utf8("caller");
	const auto bad_column = q::project_semantic_graphs(valid.input(), specs);
	require(!bad_column && bad_column.error().code == "sdk.graph-input-invalid",
			"wrong scalar identity type joined as a valid graph");
	valid = fixture{};
	valid.entities.front().contributor_edges.clear();
	const auto bad_annotations = q::project_semantic_graphs(valid.input(), specs);
	require(!bad_annotations && bad_annotations.error().code == "sdk.graph-input-invalid",
			"missing claim edges became valid evidence");
}
