#pragma once

/**
 * @file materialization_store_candidate_bridge.hpp
 * @brief The narrow production bridge between materializer streams and the bounded Store port.
 *
 * This file intentionally owns no SDK writer, SQLite cursor, report DOM, or protocol codec.  The
 * Store and materializer owners provide those operations through callbacks; this bridge owns only
 * their ordering and the one-publication/no-retry boundary.  It can therefore be integrated after
 * the candidate and projection primitive commits without changing either primitive.
 */

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "materialization_store_candidate.hpp"

namespace cxxlens::detail::clang22::materialization
{
	using materialization_store_bridge_task_consumer =
		std::function<sdk::result<void>(std::span<const std::byte>)>;
	using materialization_store_bridge_task_replay =
		std::function<sdk::result<void>(const materialization_store_bridge_task_consumer&)>;

	/** All source-owned values required to enter one bounded Store candidate. */
	struct materialization_store_candidate_bridge_request
	{
		std::string staging_session_id;
		std::string expected_head;
		bounded_store_limits limits;
		bounded_store_external_census external_census;

		/** Replay one sealed task payload at a time in canonical task order. */
		std::function<sdk::result<void>(
			const std::function<sdk::result<void>(std::span<const std::byte>)>&)>
			replay_tasks;

		/** Independent expected projection from immutable request/result/receipt authorities. */
		bounded_store_candidate::projection_builder build_expected_projection;
		/** Actual projection from an authenticated backend physical-key cursor. */
		bounded_store_candidate::projection_builder build_actual_projection;

		/** Write publication-independent report sections after the maximum tail is reserved. */
		std::function<sdk::result<void>(bounded_store_report_writer&)>
			write_publication_independent_report;
		/** Write the exact terminal/outcome section; compact downgrade is never implicit. */
		std::function<sdk::result<void>(
			bounded_store_report_writer&, bounded_store_publication_terminal)>
			write_exact_outcome_report;

		/** Candidate cleanup: remove private staging and retain cleanup failure separately. */
		std::function<sdk::result<void>()> cleanup;
		/** Exactly one backend publication attempt. Empty means the typed no-effect path. */
		std::function<bounded_store_publication_terminal(std::string_view candidate_id,
																 std::string_view expected_head)>
			publish_once;

		/** Optional injected report spool; production uses an anonymous sealed spool. */
		std::unique_ptr<materialization_private_spool> report_storage;
	};

	struct materialization_store_candidate_bridge_result
	{
		bounded_store_candidate_phase phase{bounded_store_candidate_phase::aborted};
		std::optional<bounded_store_publication_terminal> terminal;
		bool report_finalized{};
		bool cleanup_failed{};
	};

	/**
	 * Run the candidate lifecycle in its only supported production order:
	 *
	 * `append task -> seal input -> expected projection -> actual projection -> compare -> reserve
	 * report tail -> publish once (or no-effect) -> finalize report`.
	 *
	 * A failure before publication closes private staging.  A backend exception or invalid terminal
	 * is retained as `publication_outcome_unknown`; this helper never retries or invents a compact
	 * success response.
	 */
	[[nodiscard]] sdk::result<materialization_store_candidate_bridge_result>
	run_materialization_store_candidate_bridge(
		materialization_store_candidate_bridge_request request);
} // namespace cxxlens::detail::clang22::materialization
