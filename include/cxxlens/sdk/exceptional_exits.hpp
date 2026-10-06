#pragma once

/** @file exceptional_exits.hpp @brief Original exceptional occurrences per
 * physical definition and actual ABI lowering variant. LLVM ordinals are
 * independent from AST CFG IDs; this projection does not calculate a metric.
 */

#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Independently scanned original carriers; missing optional saved facets stay unknown. */
	struct exceptional_exit_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, bodies, syntax_nodes,
			exits;
		bool compile_units_complete{}, scope_inputs_complete{}, occurrence_inputs_complete{};
	};
	struct observed_exceptional_exit
	{
		std::string exit, variant, role, eligibility, profile, lowering_profile, observation_state;
		std::string source_span, expression, target, target_usr;
		std::uint64_t ordinal{};
		std::optional<std::uint64_t> original_expression_ordinal, block_ordinal,
			instruction_ordinal, successor_ordinal, intrinsic_id, compiler_route, emitter_methods;
		std::optional<bool> is_invoke, does_not_throw, does_not_return;
		finite_population_state source_state{finite_population_state::unknown},
			expression_state{finite_population_state::unknown},
			target_state{finite_population_state::unknown}, state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exceptional_exit_variant
	{
		std::string variant, kind, symbol;
		std::uint64_t index{};
		finite_population_state state{finite_population_state::unknown};
		std::vector<observed_exceptional_exit> occurrences;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exceptional_exit_population
	{
		std::string detail, function, compile_unit, body, definition_source, file, source_snapshot,
			universe, variant, interpretation, profile, lowering_profile;
		std::optional<std::uint64_t> occurrence_count;
		std::vector<std::string> occurrence_ids;
		finite_population_state enumeration_state{finite_population_state::unknown},
			scope_state{finite_population_state::unknown}, state{finite_population_state::unknown};
		std::vector<exceptional_exit_variant> variants;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exceptional_exit_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<exceptional_exit_population> populations;
		bool compile_units_complete{}, scope_inputs_complete{}, occurrence_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Validate the original finite census and exact variant membership; no replay or role models.
	 */
	[[nodiscard]] result<exceptional_exit_projection> project_exceptional_exits(
		exceptional_exit_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<exceptional_exit_projection> project_exceptional_exits(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** Measured successful work and conservative owned/index/temporary storage. Zero on failure. */
	[[nodiscard]] result<exceptional_exit_projection>
	project_exceptional_exits(exceptional_exit_input,
							  finite_population_limits,
							  std::stop_token,
							  projection_resource_usage&);
	[[nodiscard]] result<exceptional_exit_projection>
	project_exceptional_exits(const application_query_results&,
							  finite_population_limits,
							  std::stop_token,
							  projection_resource_usage&);
} // namespace cxxlens::sdk::query
