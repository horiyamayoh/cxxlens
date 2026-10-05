#pragma once

/** @file call_operands.hpp @brief Owned original compiler call operand
 * populations. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** call_inputs_complete asserts independently complete call-site and syntax scans.
	 * Original syntax finite_call_admission_v1/admitted_call_site flags are a
	 * separate finite membership witness, including bodyless declaration scopes. */
	struct call_operand_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, types,
			type_components, bodies, syntax_nodes, call_sites, direct_targets, operands;
		bool compile_units_complete{}, call_inputs_complete{}, operand_inputs_complete{},
			scope_inputs_complete{}, type_inputs_complete{};
	};
	/** @brief Original invocation operand; named storage is a separate optional
	 * witness. */
	struct observed_call_operand
	{
		std::string id, call, kind, origin, source_span, file, source_snapshot;
		std::string expression, type, referenced_entity;
		std::uint64_t index{};
		std::optional<std::uint64_t> formal_parameter_index, actual_argument_index,
			written_argument_index, begin, end;
		bool implicit{}, default_argument{};
		std::string value_category, evaluation_state;
		finite_population_state source_state{finite_population_state::unknown},
			expression_state{finite_population_state::unknown},
			type_state{finite_population_state::unknown},
			reference_state{finite_population_state::unknown};
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** @brief Actual target identity/type facet witnessed at the original call.
	 * Complete retains available original type-node facets. It does not interpret
	 * their structural grammar or certify a consumer semantic type identity.
	 * A partial referenced node remains unknown, with its original evidence. */
	struct observed_call_target_signature
	{
		std::string target, profile, canonical_type, canonical_type_profile,
			canonical_type_structure;
		std::string structural_signature_digest, canonical_type_digest, language, linkage,
			module_domain;
		std::vector<std::byte> usr;
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** @brief Exactly one original invocation in an actual unit and semantic world.
	 */
	struct call_operand_population
	{
		std::string call, compile_unit, caller, body, source_span, file, source_snapshot;
		std::string universe, variant, interpretation, profile, kind, expression;
		std::optional<std::uint64_t> argument_count, operand_count, begin, end;
		finite_population_state state{finite_population_state::unknown};
		std::vector<observed_call_operand> operands;
		std::vector<observed_call_target_signature> signatures;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** @brief Independent original function call enumeration, including bodyless scopes. */
	struct function_call_scope
	{
		std::string function, compile_unit, source_span, file, source_snapshot, body;
		std::string universe, variant, interpretation, profile;
		std::optional<std::uint64_t> call_count, begin, end;
		std::vector<std::string> call_ids;
		finite_population_state state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct call_operand_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<call_operand_population> calls;
		std::vector<function_call_scope> function_scopes;
		bool compile_units_complete{}, call_inputs_complete{}, operand_inputs_complete{},
			scope_inputs_complete{}, type_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Original target signature canonical_type_digest is cc.type.component_signature_digest;
	 * raw inputs retain closure flags as false unless independently established. */
	[[nodiscard]] result<call_operand_projection> project_call_operands(
		call_operand_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<call_operand_projection> project_call_operands(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** @brief Function scope projection requires original site + syntax scans, without operand/type
	 * scans. */
	[[nodiscard]] result<call_operand_projection> project_function_call_scopes(
		call_operand_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<call_operand_projection> project_function_call_scopes(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
