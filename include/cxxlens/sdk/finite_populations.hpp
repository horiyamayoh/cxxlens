#pragma once

/** @file finite_populations.hpp @brief Owned finite declaration, comment, include and flow domains.
 */

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query
{
	/** @brief Validated independent scans; absence of scan assertions is unknown. */
	struct finite_population_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, bodies, cfg_nodes,
			inventories, members;
		bool compile_units_complete{}, inventory_inputs_complete{}, member_inputs_complete{};
	};
	struct finite_population_limits
	{
		std::size_t maximum_rows{2'000'000U};
		std::size_t maximum_condition_expansions{4'000'000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_retained_bytes{512U * 1024U * 1024U};
		std::size_t maximum_evidence_references{16'000'000U};
		std::size_t maximum_populations{100'000U};
		std::size_t maximum_members{1'000'000U};
		std::size_t maximum_operations{128'000'000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		/** @brief Optional caller cancellation, polled with the stop token during work. */
		std::function<bool()> cancelled;
		[[nodiscard]] result<void> validate() const;
	};
	enum class finite_population_state : std::uint8_t
	{
		complete,
		unknown,
		partial,
		conflicting
	};
	/** @brief Original member plus actual source/entity/detail joins, without reconstructed
	 * identity. */
	struct finite_population_member
	{
		std::string id, entity, kind, declaration_kind, source_span, file, source_snapshot;
		std::optional<std::uint64_t> begin, end;
		std::vector<std::byte> spelling_bytes;
		std::vector<std::size_t> evidence;
	};
	/** @brief Exactly one finite inventory in an actual semantic world and compile unit. */
	struct finite_population
	{
		std::string id, domain, compile_unit, profile, universe, variant, interpretation;
		std::string function, body, source_span, file, source_snapshot;
		std::optional<std::uint64_t> source_size, declared_count, declared_bytes;
		finite_population_state state{finite_population_state::unknown};
		std::string resolution_state, subject_state, points_to_state, null_state, range_state,
			summary_state;
		std::string reaching_state, liveness_state, program_point_profile;
		std::optional<std::uint64_t> program_point_count;
		std::vector<std::string> reaching_points, liveness_points;
		std::vector<std::uint64_t> physical_line_starts;
		std::string physical_line_profile;
		std::vector<std::string> system_members, pointer_subjects, integer_subjects;
		std::vector<std::string> parsed_files, parsed_source_snapshots;
		std::string file_state;
		std::vector<finite_population_member> members;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct finite_population_evidence
	{
		std::string relation_id;
		annotated_row row;
	};
	struct finite_population_projection
	{
		std::vector<finite_population_evidence> evidence;
		std::vector<finite_population> populations;
		bool compile_units_complete{}, inventory_inputs_complete{}, member_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** @brief Project finite actual named-declaration occurrences and their independent inputs. */
	[[nodiscard]] result<finite_population_projection> project_declarations(
		finite_population_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<finite_population_projection> project_declarations(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** @brief Project raw frozen comments and immutable physical line starts. */
	[[nodiscard]] result<finite_population_projection> project_source_comments(
		finite_population_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<finite_population_projection> project_source_comments(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** @brief Project actual preprocessed inclusion directives; resolution remains independent. */
	[[nodiscard]] result<finite_population_projection> project_source_includes(
		finite_population_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<finite_population_projection> project_source_includes(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
	/** @brief Project finite local flow observations without upgrading analysis guarantees. */
	[[nodiscard]] result<finite_population_projection> project_body_flow(
		finite_population_input, finite_population_limits = {}, std::stop_token = {});
	[[nodiscard]] result<finite_population_projection> project_body_flow(
		const application_query_results&, finite_population_limits = {}, std::stop_token = {});
} // namespace cxxlens::sdk::query
