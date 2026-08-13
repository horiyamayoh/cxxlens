#pragma once

#include <memory>
#include <string>
#include <string_view>

#include "sqlite_backend_observation_internal.hpp"
#include "sqlite_private_snapshot_internal.hpp"

namespace cxxlens::sdk
{
	/** Exact owned-forwarding-VFS identity presented by its callback observation port. */
	struct sqlite_default_forwarding_observation_binding
	{
		std::string_view registered_vfs_name;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* backend_lifetime_identity{};
	};

	/**
	 * Optional bridge implemented by the owned forwarding VFS. The platform filesystem companion
	 * cannot infer xOpen output flags or SHM locks from pathname observations, so it delegates
	 * those observations only to an exactly bound callback port.
	 */
	class sqlite_default_connection_observation_port
	{
	  public:
		virtual ~sqlite_default_connection_observation_port() = default;

		[[nodiscard]] virtual sqlite_default_forwarding_observation_binding
		binding() const noexcept = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		begin_connection_observation(
			std::string_view canonical_vfs_locator,
			const sqlite_backend_opaque_identity& source_capability_token) const = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		begin_ephemeral_connection_observation(
			const sqlite_backend_opaque_identity& source_capability_token) const = 0;
		/** Dedicated target-independent scratch scope used only by readonly-SHM qualification. */
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		begin_source_shm_qualification_observation(std::string_view,
												   const sqlite_backend_opaque_identity&) const
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "source-shm-qualification-observation-unavailable"};
		}
	};

	/** One already-resolved default/forwarding VFS and its exact canonical main locator. */
	struct sqlite_default_observation_binding
	{
		std::string canonical_vfs_locator;
		std::string registered_vfs_name;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		std::shared_ptr<void> backend_lifetime;
		sqlite_private_snapshot_registry_binding private_snapshot_registry;
		std::shared_ptr<const sqlite_default_connection_observation_port>
			connection_observation_port;
	};

	/** Stable fstatat-only identity for one bound default-filesystem role. */
	struct sqlite_default_entry_identity_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
		sqlite_backend_opaque_identity object_identity;
		sqlite_backend_opaque_identity directory_entry_identity;
	};

	/** Stable fstatat-only state and optional identities for one bound filesystem role. */
	using sqlite_default_entry_state_observation = sqlite_backend_stat_only_entry_observation;

	/**
	 * Construct the source-private POSIX observation companion for SQLite's loader-origin default
	 * VFS. The locator must already be the exact absolute xFullPathname result. This factory never
	 * canonicalizes a host path, resolves a null/default VFS, loads SQLite, or registers a VFS.
	 */
#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<std::shared_ptr<sqlite_backend_observation_capability>>
	make_sqlite_default_observation_capability(sqlite_default_observation_binding binding);

	/**
	 * Observe one entry through lstat/fstatat only. Unlike a namespace census this never opens the
	 * target inode, so it cannot release a live process-scoped SQLite fcntl lock when destroyed.
	 */
	[[nodiscard]] result<sqlite_default_entry_identity_observation>
	observe_sqlite_default_entry_identity_without_open(
		const sqlite_backend_observation_capability& capability,
		std::string_view canonical_vfs_locator,
		sqlite_backend_file_role role);

	/**
	 * Observe absent/unreadable/unsupported/regular state through fstatat only. A regular result
	 * intentionally carries no held object; callers must retain their separately captured anchor.
	 */
	[[nodiscard]] result<sqlite_default_entry_state_observation>
	observe_sqlite_default_entry_state_without_open(
		const sqlite_backend_observation_capability& capability,
		std::string_view canonical_vfs_locator,
		sqlite_backend_file_role role);
} // namespace cxxlens::sdk
