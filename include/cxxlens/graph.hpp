#pragma once

/**
 * @file graph.hpp
 * @brief Stable semantic graph API declarations.
 *
 * These declarations form the installed public graph contract.
 */

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/select.hpp>
#include <cxxlens/workspace.hpp>

namespace cxxlens::graph
{
	enum class graph_kind : std::uint8_t
	{
		class_hierarchy,
		override_relation,
		call,
		include,
		impact,
		custom,
	};

	enum class graph_node_kind : std::uint8_t
	{
		symbol,
		file,
		call_site,
		unresolved_target,
		boundary,
	};

	enum class graph_edge_kind : std::uint8_t
	{
		base,
		override_relation,
		direct_call,
		possible_dynamic_call,
		indirect_call,
		include,
		impact,
		custom,
	};

	enum class graph_direction : std::uint8_t
	{
		forward,
		reverse,
		both,
	};

	enum class graph_path_policy : std::uint8_t
	{
		shortest,
		all_bounded,
		representative,
	};

	struct graph_node_id
	{
		std::string value;
		auto operator<=>(const graph_node_id&) const = default;
	};

	struct graph_edge_id
	{
		std::string value;
		auto operator<=>(const graph_edge_id&) const = default;
	};

	struct graph_limits
	{
		std::size_t max_depth{32};
		std::size_t max_nodes{10000};
		std::size_t max_edges{50000};
		std::size_t max_paths{1000};
	};

	struct graph_node
	{
		graph_node_id id;
		graph_node_kind kind{graph_node_kind::unresolved_target};
		std::optional<symbol_id> symbol;
		std::optional<path> file;
		std::optional<source_span> source;
		std::vector<build_variant_id> variants;
		std::vector<std::pair<std::string, std::string>> properties;
	};

	struct graph_edge
	{
		graph_edge_id id;
		graph_node_id from;
		graph_node_id to;
		graph_edge_kind kind{graph_edge_kind::custom};
		std::optional<source_span> source;
		std::vector<build_variant_id> variants;
		confidence certainty{confidence::probable};
		result_guarantee guarantee{result_guarantee::best_effort};
		evidence why;
		bool target_unresolved{};
	};

	struct graph_path
	{
		std::vector<graph_node_id> nodes;
		std::vector<graph_edge_id> edges;
		bool truncated{};
	};

	struct graph_options
	{
		analysis_scope scope{analysis_scope::all()};
		graph_direction direction{graph_direction::forward};
		graph_limits limits;
		bool include_unresolved{true};
		execution_context execution;
	};

	namespace detail
	{
		struct semantic_graph_data;
		struct semantic_graph_access;
		struct impact_query_data;
	} // namespace detail

	class semantic_graph
	{
	  public:
		semantic_graph() = default;
		[[nodiscard]] graph_kind kind() const noexcept;
		[[nodiscard]] std::span<const graph_node> nodes() const noexcept;
		[[nodiscard]] std::span<const graph_edge> edges() const noexcept;
		[[nodiscard]] std::span<const graph_path> paths() const noexcept;
		[[nodiscard]] const coverage_report& coverage() const noexcept;
		[[nodiscard]] std::span<const unresolved> unresolved_items() const noexcept;
		[[nodiscard]] result_guarantee guarantee() const noexcept;
		[[nodiscard]] result<semantic_graph> subgraph(std::vector<graph_node_id> roots,
													  graph_options options = {}) const;
		[[nodiscard]] std::string to_json() const;
		[[nodiscard]] std::string to_dot() const;

	  private:
		explicit semantic_graph(std::shared_ptr<const detail::semantic_graph_data> data);
		std::shared_ptr<const detail::semantic_graph_data> data_;
		friend struct detail::semantic_graph_access;
	};

	[[nodiscard]] result<semantic_graph> class_hierarchy(const workspace& workspace,
														 graph_options options = {});
	[[nodiscard]] result<semantic_graph> override_graph(const workspace& workspace,
														graph_options options = {});
	[[nodiscard]] result<semantic_graph> call_graph(const workspace& workspace,
													graph_options options = {});
	[[nodiscard]] result<semantic_graph> include_graph(const workspace& workspace,
													   graph_options options = {});

	class impact_query
	{
	  public:
		[[nodiscard]] static impact_query changed_symbols(std::vector<symbol_id> values);
		[[nodiscard]] static impact_query matching(select::semantic_selector value);
		[[nodiscard]] impact_query relations(std::vector<graph_edge_kind> values) const;
		[[nodiscard]] impact_query direction(graph_direction value) const;
		[[nodiscard]] impact_query paths(graph_path_policy value) const;
		[[nodiscard]] impact_query scope(analysis_scope value) const;
		[[nodiscard]] impact_query limits(graph_limits value) const;
		[[nodiscard]] impact_query callers(bool value = true) const;
		[[nodiscard]] impact_query derived_types(bool value = true) const;
		[[nodiscard]] impact_query reverse_includes(bool value = true) const;
		[[nodiscard]] impact_query tests(bool value = true) const;
		[[nodiscard]] result<semantic_graph> run(const workspace& workspace,
												 execution_context execution = {}) const;

	  private:
		std::shared_ptr<const detail::impact_query_data> data_;
	};
} // namespace cxxlens::graph
