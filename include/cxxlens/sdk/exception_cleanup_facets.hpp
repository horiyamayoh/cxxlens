#pragma once

/** @file exception_cleanup_facets.hpp @brief Stored exception specifications
 * and original automatic-object cleanup emitter correspondences.
 * These optional facets do not establish cleanup population completeness,
 * runtime object versions, alias closure, or an LLVM-to-AST CFG mapping.
 */
#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	struct exception_cleanup_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, bodies, exits,
			declarations, inventories;
		bool compile_units_complete{}, detail_inputs_complete{}, exit_inputs_complete{},
			declaration_inputs_complete{};
	};
	struct observed_function_exception_specification
	{
		std::string detail, function, compile_unit, source_span, universe, variant, interpretation,
			kind, profile, observation_state;
		std::optional<bool> nonthrowing;
		finite_population_state specification_state{finite_population_state::unknown},
			identity_state{finite_population_state::unknown},
			source_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_cleanup_emission
	{
		std::string exit, lowering_variant, scope_detail, function, compile_unit, body,
			definition_source, universe, variant, interpretation;
		std::string declaration, object, declaration_source, route, profile, target, target_profile;
		/** Lossless compiler USR bytes; no provider-local-key equality is implied. */
		std::vector<std::byte> target_usr;
		std::optional<std::uint64_t> registration_ordinal, emission_ordinal, target_dtor_type,
			emitter_methods;
		/** True only for an observed emitter mask that excludes generic EmitCall. */
		std::optional<bool> destructor_target_excluded;
		finite_population_state emission_state{finite_population_state::unknown},
			registration_state{finite_population_state::unknown},
			scope_state{finite_population_state::unknown},
			definition_source_state{finite_population_state::unknown},
			declaration_state{finite_population_state::unknown},
			declaration_source_state{finite_population_state::unknown},
			target_attribution_state{finite_population_state::unknown},
			target_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct exception_cleanup_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<observed_function_exception_specification> specifications;
		/** One observation for each original exit identity/world, including missing
		 * facets. */
		std::vector<observed_cleanup_emission> cleanups;
		bool compile_units_complete{}, detail_inputs_complete{}, exit_inputs_complete{},
			declaration_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<exception_cleanup_projection> project_exception_cleanup_facets(
		exception_cleanup_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<exception_cleanup_projection> project_exception_cleanup_facets(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** Exact successful work and conservative owned/index/temporary storage; zero
	 * on failure. */
	[[nodiscard]] result<exception_cleanup_projection>
	project_exception_cleanup_facets(exception_cleanup_input,
									 finite_population_limits,
									 std::stop_token,
									 projection_resource_usage&);
	[[nodiscard]] result<exception_cleanup_projection>
	project_exception_cleanup_facets(const application_query_results&,
									 finite_population_limits,
									 std::stop_token,
									 projection_resource_usage&);
} // namespace cxxlens::sdk::query
