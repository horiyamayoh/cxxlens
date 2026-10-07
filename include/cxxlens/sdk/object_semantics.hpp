#pragma once

/** @file object_semantics.hpp @brief Compiler-observed object, sequencing and value facts with
 * independent missing axes. */

#include <variant>

#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Original pointwise compiler facts, not a rule verdict. Unsupported or
	 * absent observation axes remain unknown; a known member never closes a
	 * wider function's runtime object/alias/path domain. */
	struct original_integer_interval
	{
		std::string lower, upper;
	};
	struct original_function_exit
	{
		std::string return_kind, exit_kind, edge, from, to, body;
		std::optional<bool> is_main, is_coroutine, coroutine_has_return_void;
		finite_population_state exit_state{finite_population_state::unknown};
	};
	struct original_sequence_pair
	{
		std::string left_expression, right_expression, left_source, right_source;
		std::string storage_declaration, storage_entity, sequencing, language;
		std::optional<bool> left_modifies, right_modifies, scalar_nonreference_storage,
			jointly_potentially_evaluated;
		// Original invocation and argument ordinals, independent of the diagnostic
		// checker's internal region. Different C++17 ordinary arguments are
		// indeterminately sequenced, including actual default arguments.
		struct argument_context
		{
			std::string invocation, argument_expression;
			std::uint64_t argument_index{};
			bool indeterminately_sequenced{};
			[[nodiscard]] auto operator<=>(const argument_context&) const = default;
		};
		std::vector<argument_context> left_contexts, right_contexts;
		finite_population_state storage_state{finite_population_state::unknown},
			sequencing_state{finite_population_state::unknown},
			context_state{finite_population_state::unknown};
	};
	struct original_union_access
	{
		std::string storage, selected_field, active_member_state;
		std::optional<std::string> active_field;
		std::optional<bool> actual_read, common_initial_sequence_permitted;
		finite_population_state active_state{finite_population_state::unknown};
	};
	struct original_downcast
	{
		std::string kind, storage, dynamic_type, target_type;
		std::optional<bool> nonnull, target_is_compatible_dynamic_subobject;
		finite_population_state dynamic_state{finite_population_state::unknown};
	};
	struct original_lifetime_access
	{
		std::string storage, phase;
		std::optional<bool> actual_access, permitted_in_phase;
		finite_population_state phase_state{finite_population_state::unknown};
	};
	struct original_type_access
	{
		std::string storage, effective_type, access_type;
		std::optional<bool> actual_value_access, language_type_access_permitted;
		finite_population_state effective_type_state{finite_population_state::unknown};
	};
	struct original_dynamic_object_access
	{
		std::string storage;
		std::optional<bool> actual_virtual_or_dynamic_type_access, valid_dynamic_object;
		finite_population_state dynamic_state{finite_population_state::unknown};
	};
	struct original_bool_representation
	{
		std::string kind, operand, operand_type, result_type;
		std::optional<bool> canonical_bool;
		std::optional<original_integer_interval> original_representation;
		finite_population_state representation_state{finite_population_state::unknown};
	};
	struct original_enum_value
	{
		std::string operand, operand_type, result_type;
		std::optional<bool> fixed_underlying;
		std::optional<original_integer_interval> compiler_value_domain, original_value;
		finite_population_state enumeration_state{finite_population_state::unknown};
	};
	using original_object_fact = std::variant<original_function_exit,
											  original_sequence_pair,
											  original_union_access,
											  original_downcast,
											  original_lifetime_access,
											  original_type_access,
											  original_dynamic_object_access,
											  original_bool_representation,
											  original_enum_value>;
	struct original_object_observation
	{
		std::optional<bool> is_system;
		std::string observation, kind, profile, compile_unit, function, declaration, body,
			source_span, expression, universe, variant, interpretation;
		original_object_fact fact;
		finite_population_state source_state{finite_population_state::unknown},
			owner_state{finite_population_state::unknown},
			membership_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** Original compiler event census. Complete refers to this exact named
	 * stream, never to absence of runtime object/alias/path violations. */
	struct original_object_event_population
	{
		std::string compile_unit, universe, variant, interpretation, stream, profile;
		std::optional<std::uint64_t> member_count;
		std::vector<std::string> member_ids;
		finite_population_state enumeration_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct object_semantics_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<original_object_observation> observations;
		std::vector<original_object_event_population> populations;
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Raw originals are independently SDK-validated. Population closure defaults
	 * to false for direct fixtures and cannot be inferred from returned rows. */
	struct object_semantics_input
	{
		std::span<const annotated_row> compile_units, files, spans, entities, details, declarations,
			bodies, cfg_nodes, cfg_edges, types, syntax_nodes, sequence_contexts, sequence_pairs,
			object_states, declaration_inventories, evaluation_roots;
		bool source_inputs_complete{}, owner_inputs_complete{}, body_inputs_complete{},
			cfg_inputs_complete{}, type_inputs_complete{}, syntax_inputs_complete{},
			sequence_inputs_complete{}, object_state_inputs_complete{}, inventory_inputs_complete{},
			evaluation_inputs_complete{};
	};
	/** Exact ordered original APValue designator. Unknown path is represented by
	 * absent bytes in the row, not by an empty root path. No alias is inferred. */
	struct compiler_object_path_step
	{
		enum class kind : std::uint8_t
		{
			array,
			complex,
			vector,
			field,
			base
		};
		kind category{kind::array};
		std::uint64_t index{};
		bool virtual_base{};
		std::vector<std::byte> member_usr;
		[[nodiscard]] bool operator==(const compiler_object_path_step&) const = default;
	};
	[[nodiscard]] result<std::vector<compiler_object_path_step>> decode_compiler_object_path(
		std::span<const std::byte>, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<std::vector<std::byte>>
		encode_compiler_object_path(std::span<const compiler_object_path_step>,
									finite_population_limits = {},
									std::stop_token = {});
	[[nodiscard]] result<object_semantics_projection> project_object_semantics(
		object_semantics_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<object_semantics_projection> project_object_semantics(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<object_semantics_projection>
	project_object_semantics(object_semantics_input,
							 finite_population_limits,
							 std::stop_token,
							 projection_resource_usage&);
	[[nodiscard]] result<object_semantics_projection>
	project_object_semantics(const application_query_results&,
							 finite_population_limits,
							 std::stop_token,
							 projection_resource_usage&);
} // namespace cxxlens::sdk::query
