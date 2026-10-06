#pragma once

/** @file function_compiler_facets.hpp
 * @brief Owned original dispatch/candidate and declared automatic-object layout facets.
 * These compiler-static observations do not describe runtime callees or optimized machine frames.
 */

#include <cxxlens/sdk/function_actions.hpp>

namespace cxxlens::sdk::query
{
	struct function_compiler_facet_input
	{
		function_action_input actions;
		bool dispatch_inputs_complete{}, storage_inputs_complete{};
	};
	struct observed_function_dispatch
	{
		std::string operation, site, call, kind, dispatch_kind, dispatch_profile;
		std::string candidate_presence, candidate_profile;
		std::optional<std::uint64_t> candidate_count;
		std::vector<std::string> candidate_targets;
		finite_population_state dispatch_state{finite_population_state::unknown},
			candidate_state{finite_population_state::unknown},
			binding_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_function_automatic_storage
	{
		std::string operation, object_declaration, object_entity, object_type;
		std::string storage_duration, profile, abi_context, target_triple;
		std::optional<std::uint64_t> size_bytes, alignment_bytes;
		finite_population_state layout_state{finite_population_state::unknown},
			binding_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct function_compiler_facet_population
	{
		std::string declaration, function, compile_unit, body, source_span;
		std::string universe, variant, interpretation, storage_profile;
		std::optional<std::uint64_t> automatic_storage_count;
		std::vector<std::string> automatic_storage_ids;
		finite_population_state enumeration_state{finite_population_state::unknown},
			binding_state{finite_population_state::unknown},
			storage_enumeration_state{finite_population_state::unknown};
		std::vector<observed_function_dispatch> dispatches;
		std::vector<observed_function_automatic_storage> automatic_storage;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct function_compiler_facet_projection
	{
		function_action_projection original_actions;
		std::vector<function_compiler_facet_population> populations;
		bool dispatch_inputs_complete{}, storage_inputs_complete{};
		std::vector<query_unresolved> unresolved;
	};
	/** @brief Preserve separate finite enumeration, static candidate presence and object layout.
	 * Missing facets remain unknown; contradictory originals remain conflicting. */
	[[nodiscard]] result<function_compiler_facet_projection> project_function_compiler_facets(
		function_compiler_facet_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<function_compiler_facet_projection> project_function_compiler_facets(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** Report charged work and a conservative bound including owned results and temporaries.
	 * Usage is zero on failure; unchanged default limits still apply. */
	[[nodiscard]] result<function_compiler_facet_projection>
	project_function_compiler_facets(function_compiler_facet_input,
									 finite_population_limits,
									 std::stop_token,
									 projection_resource_usage&);
	[[nodiscard]] result<function_compiler_facet_projection>
	project_function_compiler_facets(const application_query_results&,
									 finite_population_limits,
									 std::stop_token,
									 projection_resource_usage&);
} // namespace cxxlens::sdk::query
