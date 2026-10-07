#pragma once

/** @file exceptional_routes.hpp @brief Original lowered topology and Invoke EH
 * boundary facets. LLVM block/instruction ordinals are not AST CFG positions.
 * This standalone projection never searches paths or calculates a metric. */

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	struct exceptional_route_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, bodies, syntax_nodes,
			declarations, inventories, exits, blocks, successors;
		bool compile_units_complete{}, scope_inputs_complete{}, occurrence_inputs_complete{},
			topology_inputs_complete{}, declaration_inputs_complete{};
	};
	struct observed_exceptional_block
	{
		std::string block, variant, compile_unit, profile, membership_state;
		std::string universe, semantic_variant, interpretation;
		std::uint64_t ordinal{}, instruction_count{};
		bool is_entry{};
		std::optional<std::uint64_t> terminator_opcode;
		std::string terminator_kind;
		finite_population_state identity_state{finite_population_state::unknown},
			variant_state{finite_population_state::unknown},
			state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_exceptional_successor
	{
		std::string successor, variant, compile_unit, profile, membership_state;
		std::string universe, semantic_variant, interpretation;
		std::string from_block, to_block, kind, invoke;
		std::uint64_t terminator_instruction_ordinal{}, ordinal{};
		finite_population_state identity_state{finite_population_state::unknown},
			variant_state{finite_population_state::unknown},
			from_state{finite_population_state::unknown},
			to_state{finite_population_state::unknown},
			invoke_state{finite_population_state::unknown}, state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_invoke_exceptional_boundary
	{
		std::string exit, variant, compile_unit, lowered_block, normal_successor, unwind_successor;
		std::string universe, semantic_variant, interpretation;
		std::optional<std::uint64_t> block_ordinal, instruction_ordinal;
		std::optional<bool> is_invoke;
		std::string boundary_declaration, selected_scope_kind, disposition;
		std::string boundary_profile, boundary_observation_state;
		// Stored semantic specification remains independent of LLVM nounwind and
		// the existence or reachability of a generic termination block.
		std::string exception_spec_kind, exception_spec_profile;
		std::optional<bool> exception_spec_nonthrowing;
		finite_population_state identity_state{finite_population_state::unknown},
			placement_state{finite_population_state::unknown},
			successors_state{finite_population_state::unknown},
			boundary_state{finite_population_state::unknown},
			boundary_declaration_state{finite_population_state::unknown},
			exception_spec_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exceptional_route_variant
	{
		std::string carrier, kind, symbol;
		std::uint64_t index{};
		std::string detail, function, compile_unit, body, definition_source;
		std::string universe, variant, interpretation, profile;
		std::string entry;
		std::optional<std::uint64_t> block_count, successor_count;
		std::vector<std::string> block_ids, successor_ids;
		finite_population_state carrier_state{finite_population_state::unknown},
			scope_state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			enumeration_state{finite_population_state::unknown},
			entry_state{finite_population_state::unknown},
			topology_state{finite_population_state::unknown};
		// Indices into the projection's owned flat original records. Unbound
		// records remain visible there without inventing a carrier/scope.
		std::vector<std::size_t> blocks, successors, invokes;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exceptional_route_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<observed_exceptional_block> blocks;
		std::vector<observed_exceptional_successor> successors;
		std::vector<observed_invoke_exceptional_boundary> invokes;
		std::vector<exceptional_route_variant> variants;
		bool compile_units_complete{}, scope_inputs_complete{}, occurrence_inputs_complete{},
			topology_inputs_complete{}, declaration_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<exceptional_route_projection> project_exceptional_routes(
		exceptional_route_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<exceptional_route_projection> project_exceptional_routes(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** Successful charged work and conservative owned/index/temporary peak;
	 * the usage output is zero on cancellation or any failure. */
	[[nodiscard]] result<exceptional_route_projection>
	project_exceptional_routes(exceptional_route_input,
							   finite_population_limits,
							   std::stop_token,
							   projection_resource_usage&);
	[[nodiscard]] result<exceptional_route_projection>
	project_exceptional_routes(const application_query_results&,
							   finite_population_limits,
							   std::stop_token,
							   projection_resource_usage&);
} // namespace cxxlens::sdk::query
