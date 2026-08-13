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
	 * Test-minted typed proof for one authenticated owned-forwarding writer route.
	 *
	 * The private constructor is the authority boundary: this production-inert checkpoint has no
	 * production minter. Every duplicated field is intentional cross-binding evidence rather than
	 * an independently trusted claim. In particular, this type does not infer raw SQLite flags or
	 * effect-stage semantics from opaque identities.
	 */
	class sqlite_shm_verified_writer_route_proof
	{
	  public:
		~sqlite_shm_verified_writer_route_proof() noexcept = default;
		sqlite_shm_verified_writer_route_proof(const sqlite_shm_verified_writer_route_proof&) =
			default;
		sqlite_shm_verified_writer_route_proof&
		operator=(const sqlite_shm_verified_writer_route_proof&) = delete;
		sqlite_shm_verified_writer_route_proof(sqlite_shm_verified_writer_route_proof&&) noexcept =
			default;
		sqlite_shm_verified_writer_route_proof&
		operator=(sqlite_shm_verified_writer_route_proof&&) = delete;

	  private:
		friend class sqlite_writer_shm_mapping_receipt_validator;
		friend class sqlite_same_process_shm_registry_test_peer;
		friend class sqlite_shm_writer_route_proof_production_factory;

		sqlite_shm_verified_writer_route_proof(
			sqlite_writer_shm_mapping_semantic_route route,
			sqlite_shm_writer_map_request request,
			int delegated_extend,
			sqlite_backend_opaque_identity authenticated_owned_forwarding_rw_main_route_seal,
			sqlite_backend_opaque_identity main_native_file_receipt,
			sqlite_backend_opaque_identity main_xopen_receipt,
			sqlite_backend_opaque_identity sqlite_source_id,
			sqlite_backend_opaque_identity callback_transcript,
			sqlite_backend_opaque_identity wal_write_lock_receipt,
			sqlite_backend_opaque_identity effect_gate_receipt,
			sqlite_backend_opaque_identity route_validation_seal);

		sqlite_writer_shm_mapping_semantic_route route_{
			sqlite_writer_shm_mapping_semantic_route::zero_zero_preexisting_unchanged};
		sqlite_shm_writer_map_request request_;
		int delegated_extend_{};
		sqlite_backend_opaque_identity authenticated_owned_forwarding_rw_main_route_seal_;
		sqlite_backend_opaque_identity main_native_file_receipt_;
		sqlite_backend_opaque_identity main_xopen_receipt_;
		sqlite_backend_opaque_identity sqlite_source_id_;
		sqlite_backend_opaque_identity callback_transcript_;
		sqlite_backend_opaque_identity wal_write_lock_receipt_;
		sqlite_backend_opaque_identity effect_gate_receipt_;
		sqlite_backend_opaque_identity route_validation_seal_;
	};

	/**
	 * Source-private producer for the exact production writer-route proof.
	 *
	 * The factory only packages evidence already sealed by the VFS/epoch bridge. The authoritative
	 * post-map validator still cross-checks every field against the epoch receipt, so this boundary
	 * cannot manufacture mapping authority from a copied route or pointer.
	 */
	class sqlite_shm_writer_route_proof_production_factory final
	{
	  public:
		sqlite_shm_writer_route_proof_production_factory() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_verified_writer_route_proof>
		seal(sqlite_writer_shm_mapping_semantic_route route,
			 sqlite_shm_writer_map_request request,
			 int delegated_extend,
			 sqlite_backend_opaque_identity authenticated_owned_forwarding_rw_main_route_seal,
			 sqlite_backend_opaque_identity main_native_file_receipt,
			 sqlite_backend_opaque_identity main_xopen_receipt,
			 sqlite_backend_opaque_identity sqlite_source_id,
			 sqlite_backend_opaque_identity callback_transcript,
			 sqlite_backend_opaque_identity wal_write_lock_receipt,
			 sqlite_backend_opaque_identity effect_gate_receipt,
			 sqlite_backend_opaque_identity route_validation_seal) noexcept;
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

	/**
	 * One-shot authoritative writer post-map validator.
	 *
	 * The reusable semantic audit is rederived after consuming the exact epoch validation attempt.
	 * A successful receipt remains non-authoritative until the process registry cross-checks its
	 * hidden weak epoch binding against the strongly retained exact member authority.
	 */
	class sqlite_writer_shm_mapping_receipt_validator final
	{
	  public:
		sqlite_writer_shm_mapping_receipt_validator() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_verified_writer_post_map_receipt>
		validate(const sqlite_writer_shm_mapping_epoch_receipt& epoch,
				 const sqlite_shm_verified_writer_route_proof& route) noexcept;
	};
} // namespace cxxlens::sdk
