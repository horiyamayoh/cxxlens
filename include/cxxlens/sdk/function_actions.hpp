#pragma once

/** @file function_actions.hpp @brief Owned original compiler action and type-use populations.
 * Actual static site identity is distinct from original AST/CFG occurrences and runtime execution.
 */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/call_operands.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Independently scanned original relations; unset scan assertions retain unknown. */
	struct function_action_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, declarations, types,
			type_components, bodies, syntax_nodes, cfg_nodes, call_sites, operations;
		bool compile_units_complete{}, scope_inputs_complete{}, operation_inputs_complete{},
			admission_inputs_complete{}, type_inputs_complete{}, call_inputs_complete{};
	};
	/** @brief One actual compiler occurrence. Site, source, target, object and type axes are
	 * separate. */
	struct observed_function_action
	{
		std::string operation, site, scope_declaration, function, compile_unit, body;
		std::string kind, origin, profile, evaluation, outcome, observation_state, site_state;
		std::string source_span, file, source_snapshot, expression, call, parent_site;
		std::string expression_context, context_declaration;
		std::string node, element_kind, program_point_profile, target_kind, type_use_kind;
		std::uint64_t ordinal{};
		std::optional<std::uint64_t> begin, end, element_index, program_point, subject_index;
		/** Actual original parameter declaration classification; absence stays unknown. */
		std::optional<bool> parameter_has_default_argument;
		observed_call_target_signature target_signature;
		std::string object_declaration, object_entity, object_expression, object_source,
			object_type;
		std::string object_binding_state;
		finite_population_state site_binding_state{finite_population_state::unknown},
			context_state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			expression_state{finite_population_state::unknown},
			call_state{finite_population_state::unknown},
			cfg_state{finite_population_state::unknown},
			object_state{finite_population_state::unknown},
			type_state{finite_population_state::unknown};
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** @brief Independent original declaration/body facet, checked against admitted syntax/type/CFG
	 * facts. */
	struct function_action_population
	{
		std::string declaration, function, compile_unit, body, source_span, file, source_snapshot;
		std::string universe, variant, interpretation, profile;
		std::optional<std::uint64_t> operation_count, begin, end;
		std::vector<std::string> operation_ids;
		/** Independent count/member/admission closure, before dependent member facets. */
		finite_population_state enumeration_state{finite_population_state::unknown};
		finite_population_state state{finite_population_state::unknown};
		std::vector<observed_function_action> actions;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct function_action_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<function_action_population> populations;
		bool compile_units_complete{}, scope_inputs_complete{}, operation_inputs_complete{},
			admission_inputs_complete{}, type_inputs_complete{}, call_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** @brief Validate finite occurrence membership without guessing site, value, alias or effect
	 * roles. */
	[[nodiscard]] result<function_action_projection> project_function_actions(
		function_action_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<function_action_projection> project_function_actions(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
