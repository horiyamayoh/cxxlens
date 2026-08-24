#pragma once

#include <stdexcept>
#include <utility>

#include "sdk/sqlite_writer_shm_mapping_semantics_internal.hpp"

namespace cxxlens::sdk::test_support
{
	/** Seal a writer-route proof through the production semantic validator used by the VFS. */
	[[nodiscard]] inline sqlite_shm_verified_writer_route_proof
	make_sqlite_writer_shm_route_proof(sqlite_writer_shm_mapping_semantic_route route,
									   sqlite_shm_writer_map_request request,
									   int delegated_extend,
									   sqlite_backend_opaque_identity authenticated_route_seal,
									   sqlite_backend_opaque_identity main_native_file_receipt,
									   sqlite_backend_opaque_identity main_xopen_receipt,
									   sqlite_backend_opaque_identity sqlite_source_id,
									   sqlite_backend_opaque_identity callback_transcript,
									   sqlite_backend_opaque_identity wal_write_lock_receipt,
									   sqlite_backend_opaque_identity effect_gate_receipt,
									   sqlite_backend_opaque_identity validation_seal)
	{
		auto result = sqlite_shm_writer_route_proof_production_factory::seal(
			route,
			std::move(request),
			delegated_extend,
			std::move(authenticated_route_seal),
			std::move(main_native_file_receipt),
			std::move(main_xopen_receipt),
			std::move(sqlite_source_id),
			std::move(callback_transcript),
			std::move(wal_write_lock_receipt),
			std::move(effect_gate_receipt),
			std::move(validation_seal));
		if (!result)
			throw std::runtime_error{"test fixture supplied an invalid writer-route tuple"};
		return std::move(*result);
	}
} // namespace cxxlens::sdk::test_support
