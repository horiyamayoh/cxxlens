#pragma once

/**
 * @file materialization_task_internal.hpp
 * @brief Compiler-neutral materialization planning and result authority.
 *
 * Frontends adapt compiler-specific capture and worker formats into these value-owned boundaries.
 * Neither boundary owns a process, compiler-native object, Store writer, or transport frame.
 */

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/incremental.hpp>
#include <cxxlens/sdk/provider.hpp>
#include <cxxlens/sdk/store.hpp>

#include "build_capture_internal.hpp"

namespace cxxlens::sdk::detail
{
	/** Exact provider, trust, sandbox, and resource authority for one materialization task. */
	struct materialization_provider_requirement
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantics_digest;
		std::string required_qualification;
		std::string trust_policy_digest;
		provider::sandbox_requirement sandbox;
		provider::execution_budget budget;
	};

	/** Store publication authority supplied by the planner, never inferred from provider output. */
	struct materialization_publication_requirement
	{
		snapshot_draft snapshot;
		std::string analysis_recipe_digest;
		std::string output_plan_digest;
		std::string publication_target;
	};

	/** Exact current/prior input for one requested relation partition. */
	struct materialization_partition_request
	{
		std::string relation_descriptor_id;
		incremental::partition_candidate candidate;
	};

	/** Mutable adapter output. It is not execution or publication authority. */
	struct materialization_task_draft
	{
		std::string materialization_request_id;
		/** Exact bytes supplied to the provider runtime, distinct from this task's identity. */
		std::string provider_input_digest;
		validated_build_capture capture;
		provider::task provider_task;
		std::vector<materialization_partition_request> partitions;
		materialization_provider_requirement provider;
		materialization_publication_requirement publication;
	};

	/** Immutable task with a derived incremental plan and exact input identity. */
	class validated_materialization_task
	{
	  public:
		[[nodiscard]] std::string_view id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view input_binding_digest() const noexcept
		{
			return input_binding_digest_;
		}
		[[nodiscard]] const materialization_task_draft& value() const noexcept
		{
			return value_;
		}
		[[nodiscard]] const incremental::materialization_plan& plan() const noexcept
		{
			return plan_;
		}

	  private:
		validated_materialization_task(materialization_task_draft value,
									   incremental::materialization_plan plan,
									   std::string input_binding_digest,
									   std::string task_id)
			: value_{std::move(value)}, plan_{std::move(plan)},
			  input_binding_digest_{std::move(input_binding_digest)}, task_id_{std::move(task_id)}
		{
		}

		materialization_task_draft value_;
		incremental::materialization_plan plan_;
		std::string input_binding_digest_;
		std::string task_id_;

		friend result<validated_materialization_task>
			validate_materialization_task(materialization_task_draft);
	};

	/** Validate every task authority and derive the only execution input binding. */
	[[nodiscard]] result<validated_materialization_task>
	validate_materialization_task(materialization_task_draft draft);

	/** Closed logical terminal set before any Store effect is attempted. */
	enum class materialization_terminal : std::uint8_t
	{
		complete,
		partial,
		rejected,
		failed,
		cancelled,
	};

	/** Exact runtime identity and receipt binding required for adoptable provider output. */
	struct materialization_runtime_binding
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string measured_provider_binary_digest;
		std::string provider_semantics_digest;
		std::string task_input_digest;
		std::string runtime_receipt_digest;

		[[nodiscard]] bool operator==(const materialization_runtime_binding&) const = default;
	};

	/** Immutable Store candidate derived from a validated partition draft. */
	struct validated_materialization_partition
	{
		partition_draft draft;
		partition_manifest manifest;
		snapshot_partition_binding binding;
	};

	/** Mutable frontend adoption output. It cannot authorize publication. */
	struct materialization_result_draft
	{
		materialization_terminal terminal{materialization_terminal::failed};
		std::string task_id;
		std::string task_input_digest;
		std::optional<materialization_runtime_binding> runtime;
		std::vector<partition_draft> partitions;
		std::vector<closure_candidate> closures;
		std::vector<provider::coverage_unit> coverage;
		std::vector<provider::unresolved_item> unresolved;
		std::vector<claim_conflict> conflicts;
		std::vector<differential_disagreement> differential_disagreements;
	};

	/** Immutable, compiler-neutral provider result eligible for the later writer boundary. */
	class validated_materialization_result
	{
	  public:
		[[nodiscard]] materialization_terminal terminal() const noexcept
		{
			return terminal_;
		}
		[[nodiscard]] std::string_view task_id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view task_input_digest() const noexcept
		{
			return task_input_digest_;
		}
		[[nodiscard]] const std::optional<materialization_runtime_binding>& runtime() const noexcept
		{
			return runtime_;
		}
		[[nodiscard]] std::span<const validated_materialization_partition>
		partitions() const noexcept
		{
			return partitions_;
		}
		[[nodiscard]] std::span<const closure_candidate> closures() const noexcept
		{
			return closures_;
		}
		[[nodiscard]] std::span<const provider::coverage_unit> coverage() const noexcept
		{
			return coverage_;
		}
		[[nodiscard]] std::span<const provider::unresolved_item> unresolved() const noexcept
		{
			return unresolved_;
		}
		[[nodiscard]] std::span<const claim_conflict> conflicts() const noexcept
		{
			return conflicts_;
		}
		[[nodiscard]] std::span<const differential_disagreement>
		differential_disagreements() const noexcept
		{
			return differential_disagreements_;
		}
		[[nodiscard]] std::string_view result_digest() const noexcept
		{
			return result_digest_;
		}

	  private:
		validated_materialization_result(
			materialization_terminal terminal,
			std::string task_id,
			std::string task_input_digest,
			std::optional<materialization_runtime_binding> runtime,
			std::vector<validated_materialization_partition> partitions,
			std::vector<closure_candidate> closures,
			std::vector<provider::coverage_unit> coverage,
			std::vector<provider::unresolved_item> unresolved,
			std::vector<claim_conflict> conflicts,
			std::vector<differential_disagreement> differential_disagreements,
			std::string result_digest)
			: terminal_{terminal}, task_id_{std::move(task_id)},
			  task_input_digest_{std::move(task_input_digest)}, runtime_{std::move(runtime)},
			  partitions_{std::move(partitions)}, closures_{std::move(closures)},
			  coverage_{std::move(coverage)}, unresolved_{std::move(unresolved)},
			  conflicts_{std::move(conflicts)},
			  differential_disagreements_{std::move(differential_disagreements)},
			  result_digest_{std::move(result_digest)}
		{
		}

		materialization_terminal terminal_;
		std::string task_id_;
		std::string task_input_digest_;
		std::optional<materialization_runtime_binding> runtime_;
		std::vector<validated_materialization_partition> partitions_;
		std::vector<closure_candidate> closures_;
		std::vector<provider::coverage_unit> coverage_;
		std::vector<provider::unresolved_item> unresolved_;
		std::vector<claim_conflict> conflicts_;
		std::vector<differential_disagreement> differential_disagreements_;
		std::string result_digest_;

		friend result<validated_materialization_result>
		validate_materialization_result(const relation_engine&,
										const validated_materialization_task&,
										materialization_result_draft);
	};

	/**
	 * Validate a frontend result atomically. Malformed output is rejected as an error; it is never
	 * reclassified as partial. Failed/cancelled/rejected terminals cannot carry publication data.
	 */
	[[nodiscard]] result<validated_materialization_result>
	validate_materialization_result(const relation_engine& engine,
									const validated_materialization_task& task,
									materialization_result_draft draft);
} // namespace cxxlens::sdk::detail
