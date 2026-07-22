#pragma once

#include <cstdint>
#include <memory>
#include <string_view>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/** Closed source-private cause returned by exact private-snapshot digest verification. */
	enum class sqlite_private_snapshot_digest_failure : std::uint8_t
	{
		source_read,
		source_drift,
		hash_update,
		hash_finalize,
	};

	/** Project a typed digest cause without inspecting SDK diagnostic prose. */
	[[nodiscard]] constexpr std::string_view sqlite_private_snapshot_digest_failure_detail(
		const sqlite_private_snapshot_digest_failure failure) noexcept
	{
		switch (failure)
		{
			case sqlite_private_snapshot_digest_failure::hash_finalize:
				return "private-snapshot-allocation";
			case sqlite_private_snapshot_digest_failure::source_read:
			case sqlite_private_snapshot_digest_failure::source_drift:
			case sqlite_private_snapshot_digest_failure::hash_update:
				return "private-snapshot-seal-binding";
		}
		return "private-snapshot-seal-binding";
	}

	/** One already-pinned SQLite runtime registry; the factory never resolves a null/default VFS.
	 */
	struct sqlite_private_snapshot_registry_binding
	{
		using find_function = void* (*)(const char*);
		using register_function = int (*)(void*, int);
		using unregister_function = int (*)(void*);

		const void* runtime_identity{};
		void* pinned_default_vfs{};
		find_function find{};
		register_function register_vfs{};
		unregister_function unregister_vfs{};
		std::shared_ptr<void> runtime_lifetime;
	};

	/**
	 * Create a pathname-free bounded-copy builder backed by a sealed memfd and a process-local
	 * read-only SQLite VFS. This is source-private DSO linkage, not supported public C++ ABI.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<std::shared_ptr<sqlite_backend_private_snapshot_builder>>
	make_sqlite_private_snapshot_builder(sqlite_backend_opaque_identity source_capability_token,
										 sqlite_private_snapshot_registry_binding registry);
} // namespace cxxlens::sdk
