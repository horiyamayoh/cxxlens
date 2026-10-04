#pragma once

/** @file record_surfaces.hpp @brief Owned conditioned finite record member surfaces. */

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <vector>

#include <cxxlens/sdk/query_transfer.hpp>

namespace cxxlens::sdk::query
{
	struct record_surface_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, edges, surfaces;
		bool observations_complete{};
	};
	struct record_surface_limits
	{
		std::size_t maximum_rows{2'000'000U};
		std::size_t maximum_condition_expansions{4'000'000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_retained_bytes{512U * 1024U * 1024U};
		std::size_t maximum_evidence_references{16'000'000U};
		std::size_t maximum_surfaces{500'000U};
		std::size_t maximum_members{1'000'000U};
		std::size_t maximum_operations{128'000'000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};
	enum class record_surface_state
	{
		complete,
		unknown,
		partial,
		conflicting
	};
	struct record_surface_evidence
	{
		std::string relation_id;
		annotated_row row;
	};
	struct record_surface_member
	{
		std::string entity, kind, access, canonical_type;
		std::vector<std::string> flags;
		std::vector<std::size_t> evidence;
	};
	struct record_surface
	{
		std::string id, entity, compile_unit, source_span, file, source_snapshot, profile;
		std::string universe, variant, interpretation;
		bool is_definition{};
		std::uint64_t declared_methods{}, declared_fields{}, declared_base_specifiers{};
		record_surface_state state{record_surface_state::unknown};
		std::vector<record_surface_member> methods, fields;
		std::vector<std::string> base_targets;
		std::optional<std::uint64_t> declared_type_references;
		record_surface_state type_reference_state{record_surface_state::unknown};
		std::string type_reference_profile, type_reference_reason;
		std::vector<std::string> type_reference_targets;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct record_surface_projection
	{
		std::vector<record_surface_evidence> evidence;
		std::vector<record_surface> surfaces;
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Validate local enumeration without inferring relation-wide closure. */
	[[nodiscard]] result<record_surface_projection>
	project_record_surfaces(record_surface_input input,
							record_surface_limits limits = {},
							std::stop_token cancellation = {});
	/** Retain original independent scans and all their epistemic side channels. */
	[[nodiscard]] result<record_surface_projection>
	project_record_surfaces(const application_query_results& input,
							record_surface_limits limits = {},
							std::stop_token cancellation = {});
} // namespace cxxlens::sdk::query
