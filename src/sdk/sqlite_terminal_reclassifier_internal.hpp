#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

#include "sqlite_store_terminal_internal.hpp"

namespace cxxlens::sdk
{
	/** Physical formats which participate in the registered descendant algebra. */
	enum class sqlite_authority_format : std::uint8_t
	{
		legacy_v2,
		current_v3,
	};

	/** Byte-distinct tag for the committed-generation maximum. */
	enum class sqlite_committed_maximum_tag : std::uint8_t
	{
		none,
		some,
	};

	struct sqlite_committed_generation_maximum
	{
		sqlite_committed_maximum_tag tag{sqlite_committed_maximum_tag::none};
		std::uint64_t value{};

		[[nodiscard]] bool operator==(const sqlite_committed_generation_maximum&) const = default;
	};

	/**
	 * Independently decoded view of one canonical authority-state projection.
	 *
	 * canonical_bytes are the sole state-equality authority. acceleration_digest may be empty or
	 * contain one SHA-256 value, but is deliberately excluded from equality decisions.
	 */
	struct sqlite_authority_state_view
	{
		sqlite_authority_format format{sqlite_authority_format::current_v3};
		std::uint64_t committed_row_count{};
		sqlite_committed_generation_maximum committed_generation_maximum{};
		std::span<const std::byte> canonical_bytes;
		std::span<const std::byte> acceleration_digest;
	};

	/** Total class produced by the post-close filesystem observation layer. */
	enum class sqlite_terminal_observation_kind : std::uint8_t
	{
		exact_logical_empty_preauthority,
		valid_authority_state,
		invalid_census,
		mixed_or_ambiguous,
		unavailable,
	};

	struct sqlite_terminal_authority_observation
	{
		sqlite_terminal_observation_kind kind{sqlite_terminal_observation_kind::unavailable};
		std::optional<sqlite_authority_state_view> state;
	};

	/** Receipt-to-reopened-main identity decision made before authority classification. */
	enum class sqlite_main_identity_class : std::uint8_t
	{
		same,
		changed,
		unavailable,
	};

	/** Closed result for each independently validated structural projection gate. */
	enum class sqlite_projection_gate : std::uint8_t
	{
		rejected,
		accepted,
	};

	/** Candidate families in the canonical compressed normal form. */
	enum class sqlite_descendant_candidate_case : std::uint8_t
	{
		same_format_no_reset,
		same_format_final_compaction,
		v2_to_v3_migration_last,
		v2_to_v3_final_v3_compaction,
	};

	enum class sqlite_descendant_reset_kind : std::uint8_t
	{
		none,
		migration,
		current_format_compaction,
	};

	enum class sqlite_compaction_edge_format : std::uint8_t
	{
		legacy_v2,
		current_v3,
	};

	struct sqlite_compaction_run
	{
		sqlite_compaction_edge_format format{sqlite_compaction_edge_format::current_v3};
		std::uint64_t population{};
		std::uint64_t count{};

		[[nodiscard]] bool operator==(const sqlite_compaction_run&) const = default;
	};

	/**
	 * Fixed-storage candidate witness. The residual contains at most two population runs; the
	 * designated final edge is separate even when it has the same population as a residual run.
	 */
	struct sqlite_descendant_candidate_witness
	{
		sqlite_descendant_candidate_case candidate_case{
			sqlite_descendant_candidate_case::same_format_no_reset};
		std::optional<std::uint64_t> migration_population;
		sqlite_descendant_reset_kind last_reset_kind{sqlite_descendant_reset_kind::none};
		std::optional<std::uint64_t> last_reset_population;
		std::array<sqlite_compaction_run, 2U> residual_runs{};
		std::uint8_t residual_run_count{};
		std::optional<sqlite_compaction_run> designated_final_edge;

		[[nodiscard]] bool operator==(const sqlite_descendant_candidate_witness&) const = default;
	};

	/** Result returned by a candidate-specific, precomputed byte/rank/topology validator. */
	enum class sqlite_candidate_gate_result : std::uint8_t
	{
		rejected,
		accepted,
	};

	using sqlite_candidate_gate =
		sqlite_candidate_gate_result (*)(const sqlite_descendant_candidate_witness& candidate,
										 const void* precomputed_projection) noexcept;

	/**
	 * Evidence prepared in one streaming pass over the source and target canonical projections.
	 *
	 * The callback must inspect only precomputed row/byte/rank facts. It is intentionally invoked
	 * only for row-count-bounded structural candidates, never once per generation edge.
	 */
	struct sqlite_descendant_validation
	{
		sqlite_projection_gate logical_extension{sqlite_projection_gate::rejected};
		sqlite_projection_gate diagnostic_projection{sqlite_projection_gate::rejected};
		sqlite_projection_gate reset_rank_and_topology{sqlite_projection_gate::rejected};
		sqlite_projection_gate physical_projection{sqlite_projection_gate::rejected};
		sqlite_projection_gate migration_boundary_commutation{sqlite_projection_gate::rejected};
		sqlite_candidate_gate candidate_gate{};
		const void* precomputed_projection{};
	};

	enum class sqlite_compaction_schedule_status : std::uint8_t
	{
		reachable,
		unreachable,
		invalid_input,
	};

	struct sqlite_compaction_schedule
	{
		sqlite_compaction_schedule_status status{sqlite_compaction_schedule_status::invalid_input};
		std::array<sqlite_compaction_run, 2U> residual_runs{};
		std::uint8_t residual_run_count{};
		std::optional<std::uint64_t> designated_final_population;
	};

	struct sqlite_authorized_descendant_proof
	{
		bool accepted{};
		bool exact_state{};
		std::uint64_t added_publish_count{};
		bool legacy_v2_compaction_present{};
		bool migration_present{};
		bool v3_compaction_present{};
		bool v3_compaction_at_or_above_required_population{};
		std::optional<sqlite_descendant_candidate_witness> canonical_reporting_witness;
	};

	/** Typed fail-closed reason; no diagnostic prose participates in control flow. */
	enum class sqlite_terminal_reclassifier_failure : std::uint8_t
	{
		none,
		invalid_operation,
		invalid_observation,
		invalid_source_projection,
		invalid_candidate_projection,
		invalid_identity_class,
		invalid_projection_gate,
		missing_candidate_gate,
		invalid_candidate_gate_result,
		invalid_equation,
	};

	struct sqlite_terminal_reclassifier_input
	{
		sqlite_store_operation operation{sqlite_store_operation::publish};
		sqlite_main_identity_class main_identity{sqlite_main_identity_class::unavailable};
		sqlite_authority_state_view source;
		std::optional<sqlite_authority_state_view> expected_candidate;
		sqlite_terminal_authority_observation terminal;
		sqlite_descendant_validation descendant_validation;
		/** Zero denotes the no-write zero-authority compaction path. */
		std::uint64_t required_v3_compaction_population{};
	};

	struct sqlite_terminal_reclassifier_result
	{
		sqlite_terminal_class terminal_class{sqlite_terminal_class::reclassifier_unavailable};
		sqlite_terminal_reclassifier_failure failure{sqlite_terminal_reclassifier_failure::none};
		sqlite_authorized_descendant_proof proof;
		bool exact_expected_candidate{};
		bool required_operation_edge_present{};
	};

	/** Canonical-byte equality which never promotes equal digests to authority. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	bool sqlite_authority_state_bytes_equal(std::span<const std::byte> left,
											std::span<const std::byte> right) noexcept;

	/** Constant-time-in-generation-distance compact residual solver. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	sqlite_compaction_schedule solve_sqlite_compaction_residual(
		std::uint64_t delta,
		std::uint64_t minimum_population,
		std::uint64_t maximum_population,
		std::optional<std::uint64_t> designated_final_population = std::nullopt) noexcept;

	/** Receipt-aware, allocation-free terminal authority reclassifier. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	sqlite_terminal_reclassifier_result
	reclassify_sqlite_terminal(const sqlite_terminal_reclassifier_input& input) noexcept;
} // namespace cxxlens::sdk
