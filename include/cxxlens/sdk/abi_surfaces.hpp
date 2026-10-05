#pragma once

/** @file abi_surfaces.hpp @brief Owned conditioned object storage and call interfaces. */

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
	struct abi_surface_input
	{
		std::span<const annotated_row> units, files, spans, entities, details, surfaces;
		bool observations_complete{};
		bool compile_units_complete{};
		bool abi_inputs_complete{};
	};
	struct abi_surface_limits
	{
		std::size_t maximum_rows{2'000'000U};
		std::size_t maximum_condition_expansions{4'000'000U};
		std::size_t maximum_evidence_bytes{256U * 1024U * 1024U};
		std::size_t maximum_retained_bytes{512U * 1024U * 1024U};
		std::size_t maximum_evidence_references{16'000'000U};
		std::size_t maximum_surfaces{500'000U};
		std::size_t maximum_extents{4'000'000U};
		std::size_t maximum_operations{128'000'000U};
		std::size_t maximum_source_queries{4096U};
		std::size_t maximum_source_plan_bytes{64U * 1024U * 1024U};
		[[nodiscard]] result<void> validate() const;
	};
	enum class abi_surface_state
	{
		complete,
		unknown,
		partial,
		conflicting
	};
	struct abi_byte_extent
	{
		std::uint64_t begin{}, end{};
		[[nodiscard]] bool operator==(const abi_byte_extent&) const = default;
	};
	struct abi_surface_evidence
	{
		std::string relation_id;
		annotated_row row;
	};
	struct abi_surface
	{
		std::string id, entity, compile_unit, source_span, file, source_snapshot, kind, profile;
		std::string universe, variant, interpretation;
		abi_surface_state abi_state{abi_surface_state::unknown};
		abi_surface_state layout_state{abi_surface_state::unknown};
		std::optional<std::uint64_t> byte_size, byte_alignment, occupied_bytes, padding_bytes;
		std::vector<abi_byte_extent> occupied_ranges;
		std::optional<std::string> abi_context, abi_fingerprint;
		std::optional<std::vector<std::byte>> abi_signature;
		std::vector<std::size_t> evidence;
		std::vector<query_unresolved> gaps;
	};
	struct abi_surface_projection
	{
		std::vector<abi_surface_evidence> evidence;
		std::vector<abi_surface> surfaces;
		bool compile_units_complete{};
		bool abi_inputs_complete{};
		std::vector<query_unresolved> unresolved;
		std::optional<application_query_results> source_queries;
	};
	/** Validate finite local storage without inferring closure over unseen declarations. */
	[[nodiscard]] result<abi_surface_projection> project_abi_surfaces(
		abi_surface_input input, abi_surface_limits limits = {}, std::stop_token cancellation = {});
	/** Retain the original independent scans and all their epistemic side channels. */
	[[nodiscard]] result<abi_surface_projection>
	project_abi_surfaces(const application_query_results& input,
						 abi_surface_limits limits = {},
						 std::stop_token cancellation = {});
} // namespace cxxlens::sdk::query
