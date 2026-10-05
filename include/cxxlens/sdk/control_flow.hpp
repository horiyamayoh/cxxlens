#pragma once

/** @file control_flow.hpp @brief Conditioned, evidence-preserving finite CFG projection. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Independent scan rows, borrowed only for the projection call. */
	struct control_flow_input
	{
		std::span<const annotated_row> bodies;
		std::span<const annotated_row> nodes;
		std::span<const annotated_row> edges;
	};

	/** @brief Checked bounds independent of semantic identity. */
	struct control_flow_limits
	{
		std::size_t maximum_rows{1000000U};
		std::size_t maximum_condition_expansions{4000000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_expanded_row_bytes{512U * 1024U * 1024U};
		std::size_t maximum_bodies{100000U};
		std::size_t maximum_body_nodes{200000U};
		std::size_t maximum_body_edges{500000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};

	/** @brief Local enumeration state; never a query or project closed-world guarantee. */
	enum class control_flow_state : std::uint8_t
	{
		complete,
		partial,
		conflicting,
	};

	/** @brief One finite node and all supporting owned evidence-row indices. */
	struct control_flow_node
	{
		std::string id;
		std::string kind;
		std::optional<std::uint64_t> ordinal;
		std::optional<std::string> source_span;
		std::optional<std::uint64_t> statement_count;
		std::vector<std::size_t> evidence;
		/** @brief Original compiler terminator facet; absent legacy fields remain unknown. */
		std::optional<std::string> terminator{};
		std::optional<std::string> terminator_state{}, terminator_profile{};
	};

	/** @brief A distinct edge; absent target remains a frontier. */
	struct control_flow_edge
	{
		std::string id;
		std::string from;
		std::optional<std::string> to;
		std::string kind;
		std::optional<std::uint64_t> ordinal;
		std::vector<std::size_t> evidence;
		/** @brief Original condition and outcome observations, independent of CFG enumeration. */
		std::optional<std::string> condition{};
		std::optional<std::string> condition_state{}, condition_profile{};
		std::optional<std::string> outcome{}, outcome_state{}, outcome_profile{};
		std::optional<std::string> outcome_expression{};
	};

	/** @brief One body in exactly one condition fragment and interpretation. */
	struct control_flow_body
	{
		std::string id, compile_unit, function, source_span, analysis_profile;
		std::string universe, variant, interpretation;
		std::optional<std::uint64_t> declared_nodes, declared_edges;
		std::optional<std::string> entry, exit;
		control_flow_state state{control_flow_state::partial};
		std::vector<std::size_t> evidence;
		std::vector<control_flow_node> nodes;
		std::vector<control_flow_edge> edges;
		std::vector<query_unresolved> gaps;
	};

	/** @brief Owned facts and their conditioned projection; original queries retain all axes. */
	struct control_flow_projection
	{
		std::vector<annotated_row> evidence_rows;
		std::vector<control_flow_body> bodies;
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};

	/**
	 * @brief Project already validated independent-scan rows with @c output.column bindings.
	 * @details This validates the local CFG join, not claim identity or global absence.
	 * Equal observations retain every evidence row; inconsistent conditioned payloads conflict.
	 */
	[[nodiscard]] result<control_flow_projection>
	project_control_flow(control_flow_input input,
						 control_flow_limits limits = {},
						 std::stop_token cancellation = {});

	/** @brief Project public application scans while retaining every original query side channel.
	 */
	[[nodiscard]] result<control_flow_projection>
	project_control_flow(const application_query_results& input,
						 control_flow_limits limits = {},
						 std::stop_token cancellation = {});
} // namespace cxxlens::sdk::query
