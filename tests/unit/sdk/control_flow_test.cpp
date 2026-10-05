#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string>
#include <vector>

#include <cxxlens/sdk/control_flow.hpp>

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
	q::annotated_row row(std::initializer_list<std::pair<std::string, detached_cell>> values)
	{
		q::annotated_row result;
		result.presence = {"cfg:test", {"debug"}};
		result.interpretation = "clang22";
		result.provenance = {"witness:test"};
		result.values["output.compile_unit"] = detached_cell::typed("compile_unit_id", "tu:test");
		result.values["output.function"] = detached_cell::typed("cc_entity_id", "function:test");
		for (const auto& [name, value] : values)
			result.values["output." + name] = value;
		return result;
	}
	struct fixture
	{
		std::vector<q::annotated_row> bodies, nodes, edges;
		fixture()
		{
			bodies.push_back(
				row({{"body", detached_cell::typed("body_id", "body:test")},
					 {"source", detached_cell::typed("source_span_id", "span:test")},
					 {"analysis_profile", detached_cell::utf8("clang22-cfg-eh-lifetime-v1")},
					 {"eligibility", detached_cell::utf8("closed")},
					 {"node_count", detached_cell::unsigned_integer(4U)},
					 {"edge_count", detached_cell::unsigned_integer(4U)},
					 {"entry", detached_cell::typed("cfg_node_id", "node:0")},
					 {"exit", detached_cell::typed("cfg_node_id", "node:3")}}));
			for (std::size_t i{}; i < 4U; ++i)
				nodes.push_back(
					row({{"node", detached_cell::typed("cfg_node_id", "node:" + std::to_string(i))},
						 {"body", detached_cell::typed("body_id", "body:test")},
						 {"kind",
						  detached_cell::utf8(i == 0U		? "entry"
												  : i == 3U ? "exit"
															: "block")},
						 {"ordinal", detached_cell::unsigned_integer(i)}}));
			std::size_t ordinal{};
			for (const auto [from, to] : {std::pair{0U, 1U}, {0U, 2U}, {1U, 3U}, {2U, 3U}})
				edges.push_back(row(
					{{"edge",
					  detached_cell::typed("cfg_edge_id", "edge:" + std::to_string(ordinal))},
					 {"from", detached_cell::typed("cfg_node_id", "node:" + std::to_string(from))},
					 {"to", detached_cell::typed("cfg_node_id", "node:" + std::to_string(to))},
					 {"kind", detached_cell::utf8("normal")},
					 {"ordinal", detached_cell::unsigned_integer(ordinal++)}}));
		}
		q::control_flow_input input() const
		{
			return {bodies, nodes, edges};
		}
	};
	q::control_flow_projection project(const fixture& data)
	{
		auto value = q::project_control_flow(data.input());
		require(value.has_value(), "projection failed");
		return std::move(*value);
	}
	bool complete(const fixture& data)
	{
		const auto value = project(data);
		return value.bodies.size() == 1U &&
			value.bodies[0U].state == q::control_flow_state::complete;
	}
} // namespace

int main()
{
	fixture data;
	auto value = project(data);
	require(complete(data), "finite diamond must be complete");
	data.nodes[0U].values["output.terminator"] =
		detached_cell::typed("syntax_node_id", "syntax:branch");
	data.nodes[0U].values["output.terminator_state"] = detached_cell::utf8("complete");
	data.nodes[0U].values["output.terminator_profile"] =
		detached_cell::utf8("clang22-original-cfg-terminator/1");
	data.edges[0U].values["output.condition"] =
		detached_cell::typed("syntax_node_id", "syntax:predicate");
	data.edges[0U].values["output.condition_state"] = detached_cell::utf8("complete");
	data.edges[0U].values["output.condition_profile"] =
		detached_cell::utf8("clang22-original-cfg-branch-condition/1");
	data.edges[0U].values["output.outcome"] = detached_cell::utf8("unknown");
	data.edges[0U].values["output.outcome_state"] = detached_cell::utf8("unknown");
	data.edges[0U].values["output.outcome_profile"] =
		detached_cell::utf8("clang22-original-cfg-outcome/1");
	value = project(data);
	require(
		complete(data) && value.bodies[0U].nodes[0U].terminator == "syntax:branch" &&
			value.bodies[0U].edges[0U].condition == "syntax:predicate" &&
			value.bodies[0U].edges[0U].outcome_state == "unknown",
		"original condition/outcome facets were dropped or poisoned independent CFG enumeration");
	data = fixture{};
	value = project(data);
	require(value.evidence_rows.size() == 9U && value.bodies[0U].edges.size() == 4U,
			"distinct nodes, edges and evidence must survive");
	const auto original = value;
	std::ranges::reverse(data.nodes);
	std::ranges::reverse(data.edges);
	value = project(data);
	for (std::size_t i{}; i < original.evidence_rows.size(); ++i)
		require(original.evidence_rows[i].canonical_form() ==
					value.evidence_rows[i].canonical_form(),
				"input order changed owned evidence refs");
	require(value.bodies[0U].nodes[0U].evidence == original.bodies[0U].nodes[0U].evidence,
			"input order changed node evidence refs");
	data.nodes.push_back(data.nodes[0U]);
	data.nodes.back().provenance = {"second-witness"};
	value = project(data);
	require(complete(data), "equal payloads with different evidence must merge");
	require(std::ranges::any_of(value.bodies[0U].nodes,
								[](const auto& node)
								{
									return node.evidence.size() == 2U;
								}),
			"duplicate semantic nodes lost supporting evidence");
	data.nodes.back().values["output.kind"] = detached_cell::utf8("conflicting-kind");
	require(project(data).bodies[0U].state == q::control_flow_state::conflicting,
			"overlapping node conflict became first-wins");
	data = fixture{};
	data.edges.pop_back();
	require(!complete(data), "missing edge became exact smaller graph");
	data = fixture{};
	data.edges[0U].values.erase("output.to");
	value = project(data);
	require(!complete(data) && !value.bodies[0U].edges[0U].to, "frontier became a fabricated exit");
	data = fixture{};
	data.edges[0U].presence.fragments = {"release"};
	require(!complete(data), "edge from a different variant repaired local cardinality");
	data = fixture{};
	data.nodes[0U].interpretation = "other-frontend";
	require(!complete(data), "node from another interpretation repaired local cardinality");
	data = fixture{};
	data.nodes[0U].values["output.function"] =
		detached_cell::typed("cc_entity_id", "function:other");
	require(!complete(data), "wrong owned function was accepted");
	data = fixture{};
	data.edges.push_back(data.edges[0U]);
	data.edges.back().values["output.edge"] = detached_cell::typed("cfg_edge_id", "edge:stray");
	data.edges.back().values["output.from"] = detached_cell::typed("cfg_node_id", "node:missing");
	require(!complete(data), "stray function edge was silently dropped");
	data = fixture{};
	data.bodies[0U].values["output.eligibility"] = detached_cell::utf8("partial");
	require(!complete(data), "incomplete enumeration became complete");
	data = fixture{};
	data.bodies[0U].values["output.node_count"] = detached_cell::signed_integer(4);
	require(!complete(data), "signed cardinality was coerced");
	data = fixture{};
	data.bodies.push_back(data.bodies[0U]);
	data.bodies.back().provenance = {"second-body-witness"};
	value = project(data);
	require(complete(data) && value.bodies[0U].evidence.size() == 2U,
			"equal body observations lost supporting evidence");
	data.bodies.back().values["output.node_count"] = detached_cell::unsigned_integer(5U);
	require(project(data).bodies[0U].state == q::control_flow_state::conflicting,
			"competing body cardinalities became first-wins");
	data = fixture{};
	data.edges.push_back(data.edges[0U]);
	data.edges.back().values["output.edge"] = detached_cell::typed("cfg_edge_id", "edge:parallel");
	data.bodies[0U].values["output.edge_count"] = detached_cell::unsigned_integer(5U);
	value = project(data);
	require(complete(data) && value.bodies[0U].edges.size() == 5U,
			"parallel edge identities were collapsed into simple adjacency");
	data = fixture{};
	for (std::size_t index = 1U; index < 64U; ++index)
	{
		auto body = data.bodies.front();
		const auto id = "body:" + std::to_string(index);
		body.values["output.body"] = detached_cell::typed("body_id", id);
		data.bodies.push_back(body);
		for (std::size_t node{}; node < 4U; ++node)
		{
			auto observed = data.nodes[node];
			observed.values["output.body"] = detached_cell::typed("body_id", id);
			data.nodes.push_back(std::move(observed));
		}
	}
	for (std::size_t index = 4U; index < 1000U; ++index)
	{
		auto edge = data.edges.front();
		edge.values["output.edge"] =
			detached_cell::typed("cfg_edge_id", "edge:" + std::to_string(index));
		data.edges.push_back(std::move(edge));
	}
	value = project(data);
	require(value.bodies.size() == 64U && value.unresolved.size() == 1000U,
			"ambiguous ownership lost bodies or individual unresolved edges");
	for (const auto& body : value.bodies)
		require(body.state == q::control_flow_state::conflicting && body.gaps.size() <= 6U,
				"repeated ambiguous edges inflated each body's owner conflicts");
	for (auto& edge : data.edges)
		edge.values["output.from"] = detached_cell::typed("cfg_node_id", "node:missing");
	value = project(data);
	require(value.unresolved.size() == 1000U, "orphan edges lost individual source gaps");
	for (const auto& body : value.bodies)
		require(body.state == q::control_flow_state::conflicting && body.gaps.size() <= 7U,
				"orphan edge fanout inflated every possible body's gaps");
	data = fixture{};
	for (const auto field : {"rows", "bytes", "conditions", "nodes", "edges"})
	{
		q::control_flow_limits limits;
		if (field == std::string_view{"rows"})
			limits.maximum_rows = 1U;
		if (field == std::string_view{"bytes"})
			limits.maximum_evidence_bytes = 1U;
		if (field == std::string_view{"conditions"})
			limits.maximum_condition_expansions = 1U;
		if (field == std::string_view{"nodes"})
			limits.maximum_body_nodes = 1U;
		if (field == std::string_view{"edges"})
			limits.maximum_body_edges = 1U;
		auto bounded = q::project_control_flow(data.input(), limits);
		require(!bounded && bounded.error().code == "sdk.cfg-budget", "resource limit failed");
	}
	q::control_flow_limits invalid;
	invalid.maximum_bodies = 0U;
	require(!q::project_control_flow(data.input(), invalid), "zero limit accepted");
	std::stop_source stop;
	stop.request_stop();
	auto stopped = q::project_control_flow(data.input(), {}, stop.get_token());
	require(!stopped && stopped.error().code == "sdk.cfg-cancelled", "cancellation ignored");
	auto missing = q::project_control_flow(q::application_query_results{"snapshot:test", {}});
	require(missing && missing->source_queries && missing->unresolved.size() == 3U,
			"missing scans became complete empty input");
	return 0;
}
