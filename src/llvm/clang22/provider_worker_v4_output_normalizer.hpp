#pragma once

/** @file provider_worker_v4_output_normalizer.hpp
 *  @brief Source-private task-v4 AST observation to six-descriptor output normalizer.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>
#include <cxxlens/sdk/relation.hpp>

#include "provider_task_v4.hpp"
#include "provider_worker_v4_ast_observer.hpp"

namespace cxxlens::detail::clang22
{
	inline constexpr std::string_view provider_worker_v4_output_normalizer_schema =
		"cxxlens.clang22.worker-output-normalizer.v4";

	/** Bounds for the detached normalizer, independent of the wire credit budget. */
	struct provider_worker_v4_output_normalizer_limits
	{
		std::size_t maximum_observations{100000U};
		std::size_t maximum_rows{200000U};
		std::size_t maximum_output_bytes{16U * 1024U * 1024U};
		std::size_t maximum_diagnostics{10000U};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** Typed authority supplied by the task owner to the worker output seam. */
	struct provider_worker_v4_output_normalizer_options
	{
		/** Exact build.toolchain_context identity; no ambient or digest-derived fallback. */
		std::string toolchain_context_id;
		bool invocation_exact{true};
		std::vector<std::string> invocation_limitations;
		provider_worker_v4_output_normalizer_limits limits{};
		std::stop_token cancellation{};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/** One value-owned batch in the task-v4 six-descriptor output plan. */
	struct provider_worker_v4_output_batch
	{
		std::string descriptor_id;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::vector<sdk::detached_row> rows;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Complete normalizer output.  The array order is the task-v4 authority order and is never
	 * inferred from a relation registry, process environment, or a row payload digest.
	 */
	struct provider_worker_v4_normalized_output
	{
		std::string task_id;
		std::string task_v4_digest;
		std::string compile_unit;
		std::array<provider_worker_v4_output_batch, task_v4_output_descriptor_ids.size()> batches{};
		std::vector<sdk::provider::unresolved_item> unresolved;
		std::vector<std::string> limitations;
		bool exact_equivalence{true};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Canonicalize typed, value-owned Clang observations into the exact six task-v4 descriptors.
	 *
	 * The function runs after the borrowed-AST callback and therefore owns no compiler pointers.
	 * Observation rows are copied only after the observer batch validates them; their opaque
	 * payload_digest is never decoded or used to reconstruct a payload map.  Canonical rows are
	 * derived from the typed observation fields and the explicit toolchain authority.  Claim
	 * sealing, coverage, relation-engine admission, frame serialization, and Store publication
	 * remain later host/runtime boundaries.
	 */
	[[nodiscard]] sdk::result<provider_worker_v4_normalized_output>
	normalize_provider_worker_v4_output(const provider_worker_v4_ast_observation_batch& input,
										provider_worker_v4_output_normalizer_options options);
} // namespace cxxlens::detail::clang22
