#pragma once

/** @file preprocessor.hpp @brief Owned original raw and evaluated preprocessor
 * domains. */

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Independent scans, including empty scans. Missing scan assertions are
	 * unknown. */
	struct preprocessor_input
	{
		std::span<const annotated_row> units, files, spans, inventories, events;
		std::span<const annotated_row> token_inventories, tokens, syntax_nodes;
		bool compile_units_complete{}, inventory_inputs_complete{}, event_inputs_complete{};
		bool token_inputs_complete{}, syntax_inputs_complete{};
	};
	enum class preprocessor_state : std::uint8_t
	{
		complete,
		unknown,
		partial,
		unavailable,
		unsupported,
		conflicting
	};
	struct preprocessor_evidence
	{
		std::string relation_id;
		annotated_row row;
	};
	/** Original event facets. Display name/value are never parsed into semantic
	 * fields. */
	struct preprocessor_event
	{
		std::string id, compile_unit, source_span, file, source_snapshot;
		std::string universe, variant, interpretation, kind, phase, profile;
		std::string symbol, display_value, observation_state;
		std::optional<std::uint64_t> source_begin, source_end;
		std::optional<std::string> raw_event, parent_event, closing_event, branch_event;
		std::optional<std::uint64_t> region_begin, region_end, depth;
		preprocessor_state structure_state{preprocessor_state::unknown};
		std::optional<std::string> activity;
		std::optional<std::vector<std::string>> feature_symbols, condition_tokens;
		std::optional<bool> function_like, variadic;
		std::optional<std::uint64_t> parameter_count, replacement_count;
		std::optional<std::uint64_t> parameter_index, argument_index;
		std::optional<std::string> parameter_symbol;
		std::optional<std::vector<std::string>> raw_token_ids, stringify_token_ids, paste_token_ids;
		/// Direct replacement-token membership and actual expanded AST evaluation multiplicity
		/// are independent; forwarding macros can evaluate more often than their direct membership.
		std::optional<std::uint64_t> substitution_count, evaluating_substitution_count;
		std::optional<std::string> argument_source;
		std::optional<bool> argument_empty;
		std::optional<std::vector<std::byte>> spelling_bytes;
		std::optional<std::string> parent_expansion, definition_event;
		std::optional<std::uint64_t> expansion_depth;
		std::optional<std::string> effect_expression, effect_function;
		std::optional<bool> argument_may_have_side_effects;
		preprocessor_state effect_state{preprocessor_state::unknown};
		preprocessor_state macro_state{preprocessor_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** One exact original unit/file/snapshot/phase/profile/world. Facets are
	 * independent. */
	struct preprocessor_population
	{
		std::string id, compile_unit, file, source_snapshot, phase, profile;
		std::string universe, variant, interpretation;
		std::optional<std::uint64_t> source_size, declared_count;
		preprocessor_state state{preprocessor_state::unknown};
		preprocessor_state structure_state{preprocessor_state::unknown};
		preprocessor_state activity_state{preprocessor_state::unknown};
		preprocessor_state macro_state{preprocessor_state::unknown};
		preprocessor_state candidate_state{preprocessor_state::unknown};
		preprocessor_state expansion_state{preprocessor_state::unknown};
		preprocessor_state argument_effect_state{preprocessor_state::unknown};
		std::string reason;
		std::vector<std::string> event_ids;
		std::vector<preprocessor_event> events;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct preprocessor_projection
	{
		std::vector<preprocessor_evidence> evidence;
		std::vector<preprocessor_population> populations;
		bool compile_units_complete{}, inventory_inputs_complete{}, event_inputs_complete{};
		bool token_inputs_complete{}, syntax_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Validate original finite domains without parsing preprocessor source or
	 * display prose. */
	[[nodiscard]] result<preprocessor_projection> project_preprocessor(
		preprocessor_input, finite_population_limits = {}, std::stop_token = {});
	/** Retain original scan plans and all coverage/closure/conflict/provenance side
	 * channels. */
	[[nodiscard]] result<preprocessor_projection> project_preprocessor(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
