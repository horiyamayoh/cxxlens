#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "sqlite_default_observation_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * One owned, non-default SQLite VFS alias which delegates to an already-pinned default VFS.
	 * The implementation is intentionally opaque: callers receive identities and the exact
	 * xFullPathname projection, but never an ABI-dependent sqlite3_vfs definition.
	 */
	class sqlite_default_forwarding_vfs
	{
	  public:
		virtual ~sqlite_default_forwarding_vfs() = default;

		[[nodiscard]] virtual std::string_view registered_vfs_name() const noexcept = 0;
		[[nodiscard]] virtual const void* vfs_implementation_identity() const noexcept = 0;
		[[nodiscard]] virtual const void* pinned_underlying_vfs_identity() const noexcept = 0;
		[[nodiscard]] virtual const void*
		pinned_underlying_vfs_app_data_identity() const noexcept = 0;
		[[nodiscard]] virtual const void* runtime_identity() const noexcept = 0;
		[[nodiscard]] virtual const void* backend_lifetime_identity() const noexcept = 0;
		[[nodiscard]] virtual result<std::string> canonicalize(std::string_view raw_path) const = 0;
	};

	/** Complete default-filesystem binding retained for one Store lifetime. */
	struct sqlite_default_forwarding_store_bundle
	{
		std::shared_ptr<sqlite_default_forwarding_vfs> forwarding_vfs;
		std::string canonical_vfs_locator;
		std::shared_ptr<sqlite_backend_observation_capability> observation;
		std::shared_ptr<const sqlite_default_connection_observation_port>
			connection_observation_port;
		const void* runtime_identity{};
		std::shared_ptr<void> runtime_lifetime;
	};

	/** Typed non-filesystem binding retained for one `:memory:` Store lifetime. */
	struct sqlite_default_ephemeral_store_bundle
	{
		std::shared_ptr<sqlite_default_forwarding_vfs> forwarding_vfs;
		std::string sqlite_locator;
		std::shared_ptr<sqlite_backend_observation_capability> observation;
		std::shared_ptr<const sqlite_default_connection_observation_port>
			connection_observation_port;
		const void* runtime_identity{};
		std::shared_ptr<void> runtime_lifetime;
	};

	/**
	 * Register a collision-free non-default alias for one already-resolved SQLite default VFS.
	 * This function never calls the registry's find function with a null name.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<std::shared_ptr<sqlite_default_forwarding_vfs>>
	make_sqlite_default_forwarding_vfs(sqlite_private_snapshot_registry_binding registry);

	/**
	 * Register the alias, obtain its exact xFullPathname locator, and attach the platform and
	 * connection observation companions. The returned bundle owns every required lifetime.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<sqlite_default_forwarding_store_bundle>
	make_sqlite_default_forwarding_store_bundle(std::string_view raw_path,
												sqlite_private_snapshot_registry_binding registry);

	/** Register and bind the same exact alias/runtime proof for SQLite's `:memory:` profile. */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<sqlite_default_ephemeral_store_bundle>
	make_sqlite_default_ephemeral_store_bundle(sqlite_private_snapshot_registry_binding registry);
} // namespace cxxlens::sdk
