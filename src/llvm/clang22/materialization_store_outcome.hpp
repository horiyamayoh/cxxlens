#pragma once

#include <cstdint>
#include <optional>

#include "materialization_store.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Source-private publication result after the Store boundary has been observed.
	 *
	 * This is deliberately not a public SDK error or report schema.  The value retains the first
	 * typed Store issue so a later response builder cannot replace an SDK tuple with a phase guess
	 * or diagnostic prose.  A result returned by this classifier is therefore still evidence, not a
	 * claim that the installed materializer has completed its release qualification.
	 */
	enum class materialization_store_publication_outcome_kind : std::uint8_t
	{
		prepublication_zero_effect,
		rejected_stale,
		rejected_store_failure,
		publication_outcome_unknown,
		committed_verified,
		committed_unverified,
	};

	enum class materialization_store_rejected_failure_category : std::uint8_t
	{
		counter_overflow,
		hash_collision,
		persistence_corrupt,
	};

	enum class materialization_store_unknown_category : std::uint8_t
	{
		persistence_io,
	};

	struct materialization_store_publication_outcome
	{
		materialization_store_publication_outcome_kind kind{
			materialization_store_publication_outcome_kind::prepublication_zero_effect};
		std::optional<materialization_store_rejected_failure_category> rejected_failure;
		std::optional<materialization_store_unknown_category> unknown_cause;
		std::optional<materialization_store_issue> first_issue;
		std::optional<materialization_store_recovery_receipt> recovery;
	};

	/**
	 * Map one authenticated Store observation to the closed materialization outcome union.
	 *
	 * Only the exact writer-publish tuples in the installed-materialization contract are mapped.
	 * An unlisted tuple, a memory prepublication SDK failure, or an impossible observation is
	 * returned as the original SDK error (or the reserved transaction-state invariant error) so the
	 * caller can take the exit-two/no-authoritative-response path.
	 */
	[[nodiscard]] sdk::result<materialization_store_publication_outcome>
	classify_materialization_store_publication_outcome(
		const materialization_store_observation& observation);
} // namespace cxxlens::detail::clang22::materialization
