#pragma once

/** @file source_features.hpp @brief Original entered-file compiler feature
 * occurrences and independent language environment/census facets. Raw compiler
 * kinds are observations, not extension or library-facility classifications. */

#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	struct source_feature_input
	{
		std::span<const annotated_row> units, files, spans, entities, declarations,
			declaration_inventories, types, syntax_nodes, features, inventories;
		bool compile_units_complete{}, feature_inputs_complete{}, inventory_inputs_complete{};
	};
	struct observed_language_environment
	{
		std::string compile_unit, universe, variant, interpretation, profile, observation_state;
		std::optional<bool> freestanding;
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_source_feature
	{
		std::string feature, compile_unit, universe, variant, interpretation, profile;
		std::uint64_t ordinal{}, original_node_ordinal{}, compiler_kind{};
		std::string feature_class, kind, origin, evaluation, observation_state;
		std::string file, source_snapshot, source_span, source_none_reason;
		std::optional<bool> is_implicit, is_system;
		std::string declaration, context_declaration, subject_entity, call_target, subject_type,
			syntax;
		std::string reference_kind, type_role;
		// Native binding states remain available even when their optional target
		// rows are absent. A source-none observation never attributes a file.
		std::string source_binding_state, declaration_binding_state, context_binding_state,
			entity_binding_state, call_binding_state, type_binding_state;
		finite_population_state identity_state{finite_population_state::unknown},
			observation{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			declaration_state{finite_population_state::unknown},
			context_state{finite_population_state::unknown},
			entity_state{finite_population_state::unknown},
			call_state{finite_population_state::unknown},
			type_state{finite_population_state::unknown},
			syntax_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct source_feature_population
	{
		std::string inventory, compile_unit, scope, file, source_snapshot, profile;
		std::string universe, variant, interpretation;
		std::uint64_t feature_count{}, unbound_feature_count{};
		std::vector<std::string> feature_ids, unbound_feature_ids;
		std::optional<std::uint64_t> entered_file_count;
		std::vector<std::string> entered_file_ids, entered_source_snapshots;
		std::string enumeration_observation, traversal_observation, entry_observation,
			source_binding_observation, entered_file_observation;
		finite_population_state identity_state{finite_population_state::unknown},
			enumeration_state{finite_population_state::unknown},
			membership_state{finite_population_state::unknown},
			traversal_state{finite_population_state::unknown},
			entry_state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			entered_file_state{finite_population_state::unknown};
		// Indices into owned flat observations. Orphans stay visible in that array.
		std::vector<std::size_t> features;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct source_feature_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<observed_language_environment> environments;
		std::vector<observed_source_feature> features;
		std::vector<source_feature_population> populations;
		bool compile_units_complete{}, feature_inputs_complete{}, inventory_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<source_feature_projection> project_source_features(
		source_feature_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<source_feature_projection> project_source_features(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** Successful charged work and conservative owned/index/temporary peak;
	 * the usage output is zero on cancellation or any failure. */
	[[nodiscard]] result<source_feature_projection>
	project_source_features(source_feature_input,
							finite_population_limits,
							std::stop_token,
							projection_resource_usage&);
	[[nodiscard]] result<source_feature_projection>
	project_source_features(const application_query_results&,
							finite_population_limits,
							std::stop_token,
							projection_resource_usage&);
} // namespace cxxlens::sdk::query
