#pragma once

/** @file template_domains.hpp
 * @brief Original compiler template-domain populations, normalization and closure layout.
 * Membership, arguments, source binding, normalization, layout and parser stack closure
 * retain independent states. This projection does not calculate downstream metrics.
 */
#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	/** Original finite scans. Absent independent scan assertions default to unknown. */
	struct template_domain_input
	{
		std::span<const annotated_row> units, files, spans, entities, subjects, constraints,
			captures, frames, inventories;
		bool compile_units_complete{}, inventory_inputs_complete{}, domain_inputs_complete{};
		bool subject_inputs_complete{}, constraint_inputs_complete{}, capture_inputs_complete{},
			frame_inputs_complete{};
	};
	struct observed_template_subject
	{
		std::string subject, compile_unit, kind, profile, source_span, definition_source, entity,
			owner_entity, primary, owner, semantic_usr;
		std::string materialization_kind, target_state, argument_profile, argument_state,
			normalization_state, capture_state, reason;
		std::optional<std::uint64_t> ordinal, capture_count, closure_size_bits;
		std::optional<bool> is_system, dependent;
		std::vector<std::byte> canonical_arguments;
		std::string constraint_root;
		std::vector<std::string> capture_ids;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			entity_state{finite_population_state::unknown},
			owner_state{finite_population_state::unknown},
			owner_entity_state{finite_population_state::unknown},
			argument_binding_state{finite_population_state::unknown},
			normalization_binding_state{finite_population_state::unknown},
			capture_binding_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_constraint_node
	{
		std::string node, compile_unit, root_subject, path, kind, profile, source_span, left_child,
			right_child, mapping_state, reason;
		std::optional<std::uint64_t> depth, child_count;
		std::vector<std::byte> parameter_mapping;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			tree_state{finite_population_state::unknown},
			mapping_binding_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_lambda_capture
	{
		std::string capture, compile_unit, lambda, kind, profile, source_span, captured_usr,
			captured_entity, reason;
		std::optional<std::uint64_t> index, field_index, offset_bits, size_bits;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			layout_state{finite_population_state::unknown},
			entity_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_template_instantiation_frame
	{
		std::string frame, compile_unit, parent, kind, profile, source_span, completion_state,
			entity_usr, template_usr, argument_profile, argument_state, reason;
		std::optional<std::uint64_t> ordinal, depth;
		std::optional<bool> is_instantiation;
		std::vector<std::byte> canonical_arguments;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			stack_state{finite_population_state::unknown},
			argument_binding_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	/** Exact actual unit/world inventory, independently from semantic payload binding. */
	struct template_domain_population
	{
		std::string inventory, compile_unit, universe, variant, interpretation, profile;
		std::optional<std::uint64_t> subject_count, constraint_count, capture_count, frame_count;
		std::vector<std::string> subject_ids, constraint_ids, capture_ids, frame_ids;
		finite_population_state subject_state{finite_population_state::unknown},
			constraint_state{finite_population_state::unknown},
			capture_state{finite_population_state::unknown},
			frame_state{finite_population_state::unknown};
		std::vector<observed_template_subject> subjects;
		std::vector<observed_constraint_node> constraints;
		std::vector<observed_lambda_capture> captures;
		std::vector<observed_template_instantiation_frame> frames;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct template_domain_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<template_domain_population> populations;
		bool compile_units_complete{}, inventory_inputs_complete{}, domain_inputs_complete{};
		bool subject_inputs_complete{}, constraint_inputs_complete{}, capture_inputs_complete{},
			frame_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<template_domain_projection> project_template_domains(
		template_domain_input, finite_population_limits = {}, std::stop_token = {});
	/** Retains all original public queries and their side channels. */
	[[nodiscard]] result<template_domain_projection> project_template_domains(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
