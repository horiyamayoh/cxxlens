#pragma once

/** @file build_health.hpp @brief Original selected-unit analysis outcomes and finite scopes. */
#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Independent original catalog and outcome scans. Raw absence is unknown by default. */
	struct build_health_input
	{
		std::span<const annotated_row> projects, variants, units, source_files, unit_analysis,
			analysis_inventories;
		bool catalog_inputs_complete{}, analysis_inputs_complete{}, inventory_inputs_complete{};
	};
	/** Original parser and extraction axes; neither is inferred from generic query partiality. */
	struct observed_compile_unit_analysis
	{
		std::string compile_unit, project, source_snapshot, file, profile;
		std::string universe, variant, interpretation;
		std::string parse_outcome, semantic_output, reason;
		std::optional<std::uint64_t> parse_error_count, fatal_error_count;
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** Exact world/unit census and independently recorded all-selected configuration census. */
	struct build_health_population
	{
		std::string inventory_id, project, profile, universe, variant, interpretation;
		std::optional<std::uint64_t> declared_units, selected_variant_count;
		std::vector<std::string> compile_unit_ids, selected_variant_ids;
		std::string enumeration_state, selected_variant_state;
		finite_population_state state{finite_population_state::unknown};
		finite_population_state selection_state{finite_population_state::unknown};
		std::vector<observed_compile_unit_analysis> units;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** Owned original annotated rows and per-world metadata, without metric formula replay. */
	struct build_health_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<build_health_population> populations;
		bool catalog_inputs_complete{}, analysis_inputs_complete{}, inventory_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<build_health_projection> project_build_health(
		build_health_input, finite_population_limits = {}, std::stop_token = {});
	/** Retain all original public plans, results and side channels. */
	[[nodiscard]] result<build_health_projection> project_build_health(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
