#pragma once

/**
 * @file testing_plan_assertions.hpp
 * @brief Issue #43 Contract Candidate declarations for mutation/generation plan assertions.
 *
 * This is a non-installed declaration source. Issue #53 owns stable-header integration and the
 * dependency include/forward-declaration arrangement.
 */

#include <cxxlens/core/failure.hpp>
#include <cxxlens/source.hpp>

namespace cxxlens
{
	class workspace;

	namespace transform
	{
		class codemod;
		class edit_plan;
	} // namespace transform

	namespace generate
	{
		class generation_plan;
	} // namespace generate
} // namespace cxxlens

namespace cxxlens::testing
{
	/** @brief Immutable assertions over authoritative edit-plan rows and independent validation. */
	class edit_plan_assertion
	{
	  public:
		/** @brief Require the plan's independent `valid()` predicate. */
		[[nodiscard]] edit_plan_assertion valid() const;
		/** @brief Require an authoritative edit row for the exact normalized path. */
		[[nodiscard]] edit_plan_assertion changes_file(path file) const;
		/** @brief Require the canonical dry-run diff to match a filesystem-port golden. */
		[[nodiscard]] edit_plan_assertion diff_matches(path golden) const;
		/** @brief Require affected variants to reparse without dropping failed coverage. */
		[[nodiscard]] edit_plan_assertion reparses() const;
		/** @brief Require replanning the supplied recipe to produce no additional edits. */
		[[nodiscard]] edit_plan_assertion idempotent(transform::codemod recipe) const;
		/** @brief Check without applying writes; validation and dry-run remain separate from apply. */
		[[nodiscard]] result<void> check(
			const workspace& workspace, const transform::edit_plan& plan) const;
	};

	/** @brief Immutable assertions over census, decisions, payloads, and artifacts. */
	class generation_plan_assertion
	{
	  public:
		/** @brief Require the plan's independent `valid()` predicate. */
		[[nodiscard]] generation_plan_assertion valid() const;
		/** @brief Require every relevant surface exactly once in the authoritative census. */
		[[nodiscard]] generation_plan_assertion census_complete() const;
		/** @brief Require every mandatory decision axis to be classified. */
		[[nodiscard]] generation_plan_assertion no_unknown_decisions() const;
		/** @brief Require planned artifacts to reparse while preserving quarantine evidence. */
		[[nodiscard]] generation_plan_assertion artifacts_reparse() const;
		/** @brief Check in an overlay only; no artifact is emitted or transaction committed. */
		[[nodiscard]] result<void> check(
			const workspace& workspace, const generate::generation_plan& plan) const;
	};
} // namespace cxxlens::testing
