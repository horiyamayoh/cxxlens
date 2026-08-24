#pragma once

#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

#include "sdk/sqlite_writer_shm_mapping_epoch_internal.hpp"

namespace cxxlens::sdk::test_support
{
	/**
	 * Build a valid native-lifetime fixture through the production validation boundary.
	 *
	 * Tests intentionally use the same role, identity, xOpen, and retained-owner checks as the
	 * forwarding VFS. A rejected tuple is a fixture construction error, not a reason to bypass the
	 * production factory with a private object constructor.
	 */
	[[nodiscard]] inline std::pair<sqlite_writer_shm_native_lifetime_revoker,
								   sqlite_writer_shm_native_lifetime_source>
	make_sqlite_writer_shm_native_lifetime(
		sqlite_writer_shm_native_lifetime_role role,
		sqlite_backend_opaque_identity native_lifetime_identity,
		sqlite_backend_opaque_identity semantic_receipt,
		std::optional<sqlite_backend_opaque_identity> native_xopen_receipt,
		const std::shared_ptr<void>& retained_owner)
	{
		auto result = sqlite_writer_shm_native_lifetime_production_factory::create_source(
			role,
			std::move(native_lifetime_identity),
			std::move(semantic_receipt),
			std::move(native_xopen_receipt),
			retained_owner);
		if (!result)
			throw std::runtime_error{"test fixture supplied an invalid native lifetime tuple"};
		return std::move(*result);
	}
} // namespace cxxlens::sdk::test_support
