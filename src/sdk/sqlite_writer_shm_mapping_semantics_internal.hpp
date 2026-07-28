#pragma once

#include <cstdint>

#include "sqlite_writer_shm_mapping_epoch_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * Audit-only semantic classification of one writer SHM mapping epoch.
	 *
	 * The four values are the complete accepted semantic matrix. They describe only the
	 * relationship among the immutable request, stat, namespace, effect, and range evidence in
	 * one epoch receipt.
	 */
	enum class sqlite_writer_shm_mapping_semantic_route : std::uint8_t
	{
		zero_zero_preexisting_unchanged,
		one_one_preexisting_preallocated,
		one_one_preexisting_grown,
		one_one_absent_created,
	};

	/**
	 * Non-authoritative result of pure writer mapping evidence validation.
	 *
	 * This value does not prove that an epoch arm, registry activity, family pin, coordinator,
	 * native attachment, or OS handle remains live. It owns no cleanup capability and cannot be
	 * converted to `sqlite_shm_verified_writer_post_map_receipt`, pending state, a writer holder,
	 * or reader authority. The mapping pointer is retained solely as audited tuple evidence.
	 */
	struct sqlite_writer_shm_mapping_semantic_audit
	{
		sqlite_writer_shm_mapping_semantic_route route{
			sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged};
		sqlite_shm_writer_extend_pair extend_pair{sqlite_shm_writer_extend_pair::zero_zero};
		sqlite_shm_mapping_tuple mapping;
		sqlite_backend_opaque_identity holder_specific_effect_receipt;
	};

	/**
	 * Validate and classify the closed four-route writer mapping semantic matrix.
	 *
	 * This function takes only immutable audit evidence. It derives the checked mapping range,
	 * tuple, extend-pair classification, route, and holder-specific effect receipt. It does not
	 * inspect or retain the epoch's weak state binding and therefore deliberately grants no
	 * lifecycle or mapping authority. A future authoritative validator must separately bind this
	 * semantic result to the exact strongly retained arm, registry activity, coordinator state,
	 * and post-native token. The opaque source, callback, WAL-lock, and effect-gate identities are
	 * checked only for closed structural presence here; authentication against typed writer-route
	 * authorities is deliberately deferred to that authoritative boundary.
	 *
	 * Since the input records a post-native observation, a determinate mismatch requests exact
	 * non-removing cleanup before the outer I/O error. Lost/overflowed/unknown or replacement
	 * evidence is lifecycle ambiguity and is normalized to terminal quarantine without retry.
	 */
	[[nodiscard]] sqlite_shm_lease_result<sqlite_writer_shm_mapping_semantic_audit>
	validate_sqlite_writer_shm_mapping_semantics_for_audit(
		const sqlite_writer_shm_mapping_epoch_receipt& receipt) noexcept;
} // namespace cxxlens::sdk
