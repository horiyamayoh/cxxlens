#pragma once

/** Original final candidate disposition and reached legacy-interpreter
 * occurrences. Enumeration, terminal completion and source/target/lifetime
 * binding are independent. No metric count or interpretation replay. */
#include <cxxlens/sdk/finite_populations.hpp>

namespace cxxlens::sdk::query
{
	struct template_event_input
	{
		std::span<const annotated_row> units, files, spans, entities, syntax, candidates, roots,
			calls, inventories;
		bool compile_units_complete{}, candidate_inputs_complete{}, root_inputs_complete{},
			call_inputs_complete{}, inventory_inputs_complete{};
	};
	struct observed_template_candidate
	{
		std::string candidate, compile_unit, source_span, candidate_entity, route, profile,
			deduction_result, exclusion_disposition, binding_state, reason;
		std::vector<std::byte> candidate_usr;
		std::optional<std::uint64_t> ordinal, overload_failure;
		std::optional<bool> is_system, completed;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			subject_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_constant_evaluation_root
	{
		std::string root, compile_unit, mode, interpreter, profile, completion, call_state, reason;
		std::optional<std::uint64_t> ordinal, call_count;
		std::optional<bool> requested_constant_context, potential_check, fold_failure;
		std::vector<std::string> call_ids;
		finite_population_state state{finite_population_state::unknown},
			terminal_state{finite_population_state::unknown},
			invocation_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct observed_constant_evaluated_call
	{
		std::string call, compile_unit, source_span, expression, owner_entity, invocation_kind,
			expression_kind, lifetime_source, profile, membership_state, binding_state, reason;
		std::vector<std::byte> owner_usr, lifetime_usr, lifetime_path;
		// Original lossless hex/set payload; never interpret target names or aliases.
		std::vector<std::string> callee_usr_hexes, root_ids;
		std::optional<std::uint64_t> ordinal, depth, root_count;
		std::optional<bool> is_system;
		finite_population_state state{finite_population_state::unknown},
			source_state{finite_population_state::unknown},
			root_state{finite_population_state::unknown},
			expression_state{finite_population_state::unknown},
			owner_state{finite_population_state::unknown},
			target_state{finite_population_state::unknown},
			lifetime_state{finite_population_state::unknown};
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct template_event_population
	{
		std::string inventory, compile_unit, universe, variant, interpretation, candidate_profile,
			evaluation_root_profile;
		std::optional<std::uint64_t> candidate_count, evaluation_root_count;
		std::vector<std::string> candidate_ids, evaluation_root_ids;
		finite_population_state candidate_state{finite_population_state::unknown},
			evaluation_root_state{finite_population_state::unknown},
			invocation_state{finite_population_state::unknown};
		std::vector<observed_template_candidate> candidates;
		std::vector<observed_constant_evaluation_root> roots;
		std::vector<observed_constant_evaluated_call> calls;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct template_event_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<template_event_population> populations;
		bool compile_units_complete{}, candidate_inputs_complete{}, root_inputs_complete{},
			call_inputs_complete{}, inventory_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	[[nodiscard]] result<template_event_projection> project_template_events(
		template_event_input, finite_population_limits = {}, std::stop_token = {});
	/** Preserve original query plans/rows/side channels. Select retained source and
	 * syntax only by original event FKs; validate and bound every input row. */
	[[nodiscard]] result<template_event_projection> project_template_events(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
