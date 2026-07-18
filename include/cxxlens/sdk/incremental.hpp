#pragma once

/** @file incremental.hpp @brief Exact incremental materialization and bounded closure primitives.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk::incremental
{
	/** @brief Every semantic input whose change invalidates a materialized partition. */
	struct input_fingerprint
	{
		std::string source_digest;
		std::string dependency_digest;
		std::string invocation_digest;
		std::string toolchain_digest;
		std::string condition_universe_digest;
		std::string variant_digest;
		std::string provider_set_digest;
		std::string registry_digest;
		std::string interpretation_policy_digest;
		std::string refresh_policy_digest;
		std::string environment_digest;
		std::string provider_binary_digest;
		std::string provider_semantics_digest;
		std::string relation_descriptor_digest;
		std::string normalizer_version;
		std::string model_digest;
		std::string assumption_digest;
		std::string precision_profile;

		/** @brief Reject an omitted exact input before planning. */
		[[nodiscard]] result<void> validate() const;
		/** @brief Typed deterministic identity over every exact input. */
		[[nodiscard]] result<std::string> digest() const;
		[[nodiscard]] bool operator==(const input_fingerprint&) const = default;
	};

	/** @brief One prior or current partition state used by the independent planner. */
	struct partition_state
	{
		std::string partition_id;
		input_fingerprint input;
		std::string coverage_digest;
		std::string closure_digest;
		bool corruption_detected{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const partition_state&) const = default;
	};

	/** @brief Exact current state and its optional prior materialization. */
	struct partition_candidate
	{
		partition_state current;
		std::optional<partition_state> prior;
	};

	/** @brief Closed action set for a deterministic materialization plan. */
	enum class action : std::uint8_t
	{
		reuse,
		recompute,
	};

	/** @brief One canonical partition decision with a stable machine reason. */
	struct plan_entry
	{
		std::string partition_id;
		action decision{action::recompute};
		std::string reason;
		[[nodiscard]] bool operator==(const plan_entry&) const = default;
	};

	/** @brief Canonically ordered plan; warm-zero means no provider execution is permitted. */
	struct materialization_plan
	{
		std::vector<plan_entry> entries;
		std::uint64_t frontend_provider_executions{};
		bool warm_zero{};
		std::string plan_digest;

		/** @brief Independently check ordering, counts, warm-zero, reasons, and digest. */
		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const materialization_plan&) const = default;
	};

	/** @brief Compare exact prior/current inputs and invalidate only affected partitions. */
	[[nodiscard]] result<materialization_plan>
	make_materialization_plan(std::span<const partition_candidate> candidates);

	/** @brief One condition-aware directed edge for bounded transitive closure. */
	struct closure_edge
	{
		std::string from;
		std::string to;
		std::vector<std::string> condition_fragments;
		std::string interpretation;
		std::vector<std::string> evidence;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const closure_edge&) const = default;
	};

	/** @brief Explicit roots and independent iteration/edge budgets. */
	struct closure_request
	{
		std::vector<std::string> roots;
		std::string closure_kind;
		std::vector<closure_edge> edges;
		std::uint64_t max_iterations{};
		std::uint64_t max_edges{};

		[[nodiscard]] result<void> validate() const;
	};

	/** @brief One positive reachable pair with preserved condition and evidence. */
	struct closure_row
	{
		std::string root;
		std::string target;
		std::vector<std::string> condition_fragments;
		std::string interpretation;
		std::vector<std::string> evidence;
		[[nodiscard]] bool operator==(const closure_row&) const = default;
	};

	/** @brief Positive rows survive truncation; closure certification never does. */
	struct closure_result
	{
		std::string closure_kind;
		std::vector<closure_row> rows;
		std::uint64_t iterations{};
		bool closure_certified{};
		std::vector<error> unresolved;
		std::string result_digest;

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const closure_result&) const = default;
	};

	/** @brief Deterministic least-fixpoint evaluation with fail-closed bounded completion. */
	[[nodiscard]] result<closure_result> bounded_transitive_closure(const closure_request& request);
} // namespace cxxlens::sdk::incremental
