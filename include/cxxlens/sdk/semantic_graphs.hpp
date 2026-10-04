#pragma once

/** @file semantic_graphs.hpp @brief Owned typed semantic graphs with conditioned evidence. */

#include <cstddef>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query
{
	struct semantic_graph_input
	{
		std::span<const annotated_row> units, files, entities, entity_edges, call_sites,
			call_targets, includes;
		bool observations_complete{};
	};
	struct semantic_graph_spec
	{
		std::string id;
		std::vector<std::string> node_kinds, edge_kinds;
		bool file_nodes{};
	};
	struct semantic_graph_limits
	{
		std::size_t maximum_rows{2'000'000U};
		std::size_t maximum_condition_expansions{4'000'000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_expanded_row_bytes{512U * 1024U * 1024U};
		std::size_t maximum_evidence_references{16'000'000U};
		std::size_t maximum_specs{64U};
		std::size_t maximum_graphs{8192U};
		std::size_t maximum_graph_nodes{500'000U};
		std::size_t maximum_graph_edges{1'000'000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};
	enum class semantic_graph_state
	{
		complete,
		partial,
		conflicting
	};
	struct semantic_graph_evidence
	{
		std::string relation_id;
		annotated_row row;
	};
	struct semantic_graph_node
	{
		std::string id, kind, name;
		std::vector<std::size_t> evidence;
	};
	struct semantic_graph_edge
	{
		std::string id, from, to, kind, source_span, compile_unit, resolution;
		std::vector<std::size_t> evidence;
	};
	struct semantic_graph
	{
		semantic_graph_spec spec;
		std::string universe, variant, interpretation;
		semantic_graph_state state{semantic_graph_state::partial};
		bool closed{};
		std::vector<std::string> compile_units, closure_ids;
		std::vector<semantic_graph_node> nodes;
		std::vector<semantic_graph_edge> edges;
		std::vector<query_unresolved> gaps;
	};
	struct semantic_graph_projection
	{
		std::vector<semantic_graph_evidence> evidence;
		std::vector<semantic_graph> graphs;
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<semantic_graph_projection>
	project_semantic_graphs(semantic_graph_input input,
							std::span<const semantic_graph_spec> specs,
							semantic_graph_limits limits = {},
							std::stop_token cancellation = {});
	[[nodiscard]] result<semantic_graph_projection>
	project_semantic_graphs(const application_query_results& input,
							std::span<const semantic_graph_spec> specs,
							semantic_graph_limits limits = {},
							std::stop_token cancellation = {});
} // namespace cxxlens::sdk::query
