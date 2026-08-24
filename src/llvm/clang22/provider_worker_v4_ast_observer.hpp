#pragma once

/** @file provider_worker_v4_ast_observer.hpp
 *  @brief Source-private Clang AST to detached task-v4 observations.
 */

#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/provider/clang22.hpp>
#include <cxxlens/sdk/provider.hpp>

#include "observation_v2.hpp"
#include "source_closure_task_v4.hpp"

namespace cxxlens::detail::clang22
{
	enum class provider_worker_v4_ast_observation_kind : std::uint8_t
	{
		entity = 1,
		type = 2,
		call = 3,
	};

	/** One value-owned observation produced while a borrowed AST is still in scope. */
	struct provider_worker_v4_ast_observation
	{
		provider_worker_v4_ast_observation_kind kind{
			provider_worker_v4_ast_observation_kind::entity};
		std::string compile_unit;
		std::string semantic_key;
		std::map<std::string, std::string, std::less<>> payload;
		std::optional<materialization::observation_v2_primary_span> primary_span;
		std::vector<materialization::observation_v2_origin> origins;
		bool exact_equivalence{true};
		std::optional<std::string> limitation;

		[[nodiscard]] sdk::result<void> validate() const;
		[[nodiscard]] std::string canonical_form() const;
		[[nodiscard]] bool operator==(const provider_worker_v4_ast_observation&) const = default;
	};

	/** Detached observations and observation-v2 rows for one authenticated task-v4 unit. */
	struct provider_worker_v4_ast_observation_batch
	{
		std::string task_id;
		std::string task_v4_digest;
		std::string compile_unit;
		std::string source_snapshot;
		std::string source_file;
		std::uint64_t source_size_bytes{};
		std::vector<provider_worker_v4_ast_observation> observations;
		std::vector<sdk::detached_row> rows;
		std::uint64_t failed_count{};
		std::vector<std::string> diagnostics;

		[[nodiscard]] sdk::result<void> validate() const;
	};

	/**
	 * Walk the supplied Clang AST and detach source-bound observations before the callback ends.
	 *
	 * `metadata` is independently authenticated task-v4/closure metadata.  The function consumes
	 * no filesystem, process, environment, task-v3, or Store state and does not infer relation,
	 * recipe, publication, or trust authority.  The returned rows are observation-v2 rows only;
	 * claim sealing and Store publication remain an explicit later boundary.
	 */
	[[nodiscard]] sdk::result<provider_worker_v4_ast_observation_batch>
	observe_provider_worker_v4_ast(provider::clang22::borrowed_translation_unit& unit,
								   const source_closure_task_v4_decoded& metadata,
								   std::string compile_unit);
} // namespace cxxlens::detail::clang22
