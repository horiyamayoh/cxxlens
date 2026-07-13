#include "graph.hpp"

int main()
{
	using namespace cxxlens;
	const auto options = graph::graph_options{
		.scope = analysis_scope::all(),
		.direction = graph::graph_direction::forward,
		.limits = {.max_depth = 8, .max_nodes = 512, .max_edges = 2048, .max_paths = 64},
		.include_unresolved = true,
	};
	const graph::semantic_graph empty;
	(void)empty.nodes();
	(void)empty.edges();
	(void)empty.paths();
	(void)options;
	const auto query = graph::impact_query::changed_symbols({})
						   .relations({graph::graph_edge_kind::direct_call, graph::graph_edge_kind::include})
						   .direction(graph::graph_direction::reverse)
						   .paths(graph::graph_path_policy::representative)
						   .limits({.max_depth = 4, .max_nodes = 128, .max_edges = 512, .max_paths = 16});
	(void)query;
	return 0;
}
