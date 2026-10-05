#pragma once

/** @file target_resolution.hpp
 * @brief Original eligible relation-target slots and their independent finite populations.
 * Resolution is compiler observation, never a second name/type resolver.
 */
#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Raw original relation scans. Absent census and scan assertions remain unknown. */
	struct target_resolution_input
	{
		std::span<const annotated_row> units, files, spans, entities, types, declarations,
			syntax_nodes, operations, includes, entity_edges, call_sites, inventories, slots;
		bool compile_units_complete{}, inventory_inputs_complete{}, slot_inputs_complete{};
	};
	/** All IDs retain the exact original unit/world. Optional named targets are separate
	 * from source, original-subject admission and the completed compiler outcome. */
	struct observed_target_resolution_slot
	{
		std::string slot, compile_unit, domain, relation_kind, subject_kind, profile;
		std::string source_span, file, source_snapshot, declaration, expression, operation, include,
			edge, call, owner, target_entity, target_file, target_type;
		std::string eligibility, resolution, observation_state, reason;
		std::optional<std::uint64_t> subject_ordinal, slot_index, begin, end;
		std::optional<bool> is_system;
		finite_population_state source_state{finite_population_state::unknown},
			subject_state{finite_population_state::unknown},
			target_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** The compiler's full profile census. Dependent resolution can remain unknown
	 * while enumeration is complete. This API does not calculate any ratio. */
	struct target_resolution_population
	{
		std::string inventory, compile_unit, universe, variant, interpretation, profile;
		std::optional<std::uint64_t> slot_count;
		std::vector<std::string> slot_ids;
		finite_population_state enumeration_state{finite_population_state::unknown};
		std::vector<observed_target_resolution_slot> slots;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** Owned annotated inputs and all original query side channels. */
	struct target_resolution_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<target_resolution_population> populations;
		bool compile_units_complete{}, inventory_inputs_complete{}, slot_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<target_resolution_projection> project_target_resolution(
		target_resolution_input, finite_population_limits = {}, std::stop_token = {});
	/** Generic declaration partiality does not replace the separately typed
	 * target-slot census. Missing original scans/members retain a frontier. */
	[[nodiscard]] result<target_resolution_projection> project_target_resolution(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
