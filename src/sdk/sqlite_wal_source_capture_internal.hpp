#pragma once

#include <cstddef>
#include <memory>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_backend_observation_internal.hpp"
#include "sqlite_wal_recovery_workspace_internal.hpp"

namespace cxxlens::sdk
{
	inline constexpr std::size_t sqlite_wal_source_capture_buffer_bound = 64U * 1024U;

	/** Stable held-source evidence and the sealed private workspace derived from it. */
	struct sqlite_wal_source_capture
	{
		sqlite_backend_copy_receipt full_source_main;
		sqlite_backend_copy_receipt full_source_wal;
		sqlite_wal_scan_receipt wal_scan;
		sqlite_wal_recovery_workspace_receipt workspace_receipt;
		std::shared_ptr<sqlite_wal_recovery_workspace> workspace;
	};

	/**
	 * Capture one held main+WAL pair without reading SHM or journal bytes.
	 *
	 * Both full-source receipts are fixed before the scan. Reads and builder appends use at most
	 * 64 KiB. Only the exact last committed prefix is copied from WAL; a valid no-commit WAL copies
	 * zero bytes. Both held entries and their full size/digest receipts are rechecked before seal.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<sqlite_wal_source_capture>
	capture_sqlite_wal_source(const std::shared_ptr<const sqlite_backend_held_object>& held_main,
							  const std::shared_ptr<const sqlite_backend_held_object>& held_wal,
							  sqlite_wal_recovery_workspace_builder& workspace_builder);
} // namespace cxxlens::sdk
