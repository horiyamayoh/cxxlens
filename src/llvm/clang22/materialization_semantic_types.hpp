#pragma once

/**
 * @file materialization_semantic_types.hpp
 * @brief Value-owned task context, canonicalization, and origin records.
 *
 * These records are shared by the task-v4 Store ingress and its independent projection validator.
 * They contain semantic evidence only; request-version and provider-task wire compatibility are
 * deliberately kept out of this header.
 */

#include <optional>
#include <string>

namespace cxxlens::detail::clang22::materialization
{
	/** Exact task context retained with each Store-visible semantic record. */
	struct materialization_semantic_task_context
	{
		std::string provider_task_id;
		std::string task_input_digest;
		std::string selected_catalog_compile_unit_id;
		std::string compile_unit_id;
		std::string condition_universe_id;
		std::string condition_id;
		std::string interpretation_domain;

		[[nodiscard]] bool operator==(const materialization_semantic_task_context&) const = default;
	};

	/** Exact hidden-precursor to stored-final canonicalization edge. */
	struct materialization_canonicalization_edge
	{
		std::string precursor_claim_ref;
		std::string final_claim_ref;
		std::string transform_semantics;

		[[nodiscard]] bool operator==(const materialization_canonicalization_edge&) const = default;
	};

	/** Lossless task/row/evidence association kept separate from claim occurrence identity. */
	struct materialization_origin_association
	{
		std::string association_id;
		std::string stored_claim_ref;
		materialization_semantic_task_context originating_task;
		std::string sealed_row_digest;
		std::optional<std::string> source_evidence_digest;

		[[nodiscard]] bool operator==(const materialization_origin_association&) const = default;
	};
} // namespace cxxlens::detail::clang22::materialization
