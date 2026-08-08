#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "sqlite_same_process_shm_process_port_internal.hpp"

namespace cxxlens::sdk
{
	class sqlite_same_process_shm_vfs_alias_identity_sealer;
	class sqlite_same_process_shm_vfs_alias_registration_test_peer;

	/**
	 * Sealed input produced from one exact owned SQLite VFS alias lifecycle.
	 *
	 * This value deliberately does not infer a cohort from a name, pointer, pathname, or runtime
	 * lifetime identity. The source-private identity sealer is the only non-test producer and binds
	 * the complete DF-0205 process/runtime/image/source-id/original-VFS/callback receipt before
	 * constructing it. The registration port remains callback-family and Store inert.
	 */
	class sqlite_shm_vfs_alias_lifecycle_binding
	{
	  public:
		using find_function = void* (*)(const char*);
		using register_function = int (*)(void*, int);
		using unregister_function = int (*)(void*);

		~sqlite_shm_vfs_alias_lifecycle_binding() noexcept = default;
		[[nodiscard]] bool valid() const noexcept;

		sqlite_shm_vfs_alias_lifecycle_binding(
			sqlite_shm_vfs_alias_lifecycle_binding&&) noexcept = default;
		sqlite_shm_vfs_alias_lifecycle_binding&
		operator=(sqlite_shm_vfs_alias_lifecycle_binding&&) = delete;
		sqlite_shm_vfs_alias_lifecycle_binding(
			const sqlite_shm_vfs_alias_lifecycle_binding&) = delete;
		sqlite_shm_vfs_alias_lifecycle_binding&
		operator=(const sqlite_shm_vfs_alias_lifecycle_binding&) = delete;

	  private:
		friend class sqlite_same_process_shm_vfs_alias_identity_sealer;
		friend class sqlite_same_process_shm_vfs_alias_registration_port;
		friend class sqlite_same_process_shm_vfs_alias_registration_test_peer;

		sqlite_shm_vfs_alias_lifecycle_binding(
			sqlite_shm_process_registry_handle process,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity runtime_lifetime_identity,
			sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
			std::shared_ptr<void> runtime_lifetime_owner,
			std::string registered_vfs_name,
			void* vfs_implementation,
			find_function find,
			register_function register_vfs,
			unregister_function unregister_vfs) noexcept;

		sqlite_shm_process_registry_handle process_;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity runtime_lifetime_identity_;
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity_;
		std::shared_ptr<void> runtime_lifetime_owner_;
		std::string registered_vfs_name_;
		void* vfs_implementation_{};
		find_function find_{};
		register_function register_vfs_{};
		unregister_function unregister_vfs_{};
	};

	/**
	 * Complete source-private receipt needed to seal one forwarding-VFS alias binding.
	 *
	 * Runtime/image/source-id and original-VFS identities are kept as separate coordinates. The
	 * sealer never calls SQLite or accepts a caller-authored cohort/lifetime identity; it derives
	 * all registry identities from this complete tuple and retains the supplied runtime owner.
	 * The registration lifecycle itself remains separate from the explicit family coordinator
	 * bridge below; no callback or outward SQLite status is inferred here.
	 */
	struct sqlite_shm_vfs_alias_identity_sealing_input
	{
		sqlite_shm_process_registry_handle process;
		sqlite_source_shm_runtime_binding runtime;
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* pinned_underlying_open_callback_address{};
		const void* backend_lifetime_identity{};
		std::string registered_vfs_name;
		void* vfs_implementation{};
	};

	/**
	 * Source-private identity/owner bridge for the exact forwarding-VFS lifecycle.
	 *
	 * The returned binding is the sole value accepted by the closed registration port. Missing,
	 * inconsistent, or incomplete receipt coordinates are rejected before any native registration
	 * attempt. This unit does not install a family, bind xShmMap/xShmUnmap/xClose, or project native
	 * SQLite status.
	 */
	class sqlite_same_process_shm_vfs_alias_identity_sealer final
	{
	  public:
		sqlite_same_process_shm_vfs_alias_identity_sealer() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_vfs_alias_lifecycle_binding>
		seal(sqlite_shm_vfs_alias_identity_sealing_input input) noexcept;
	};

	/**
	 * Move-only owner of one exact registered VFS alias and its process-registry pin.
	 *
	 * Native unregister is never attempted from the destructor. Dropping a still-registered owner
	 * abandons the alias pin, which quarantines the retained runtime/VFS owner for process life.
	 * Explicit `unregister_alias()` may be retried only while registry quiescence is pending and
	 * before the one native unregister attempt starts. Any native failure, exception, or post-call
	 * discovery mismatch consumes the owner into quarantine and forbids retry.
	 */
	class sqlite_shm_registered_vfs_alias
	{
	  public:
		~sqlite_shm_registered_vfs_alias() noexcept = default;
		sqlite_shm_registered_vfs_alias(sqlite_shm_registered_vfs_alias&&) noexcept = default;
		sqlite_shm_registered_vfs_alias& operator=(sqlite_shm_registered_vfs_alias&&) = delete;
		sqlite_shm_registered_vfs_alias(const sqlite_shm_registered_vfs_alias&) = delete;
		sqlite_shm_registered_vfs_alias&
		operator=(const sqlite_shm_registered_vfs_alias&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& process_instance() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		shared_runtime_vfs_cohort() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& alias_lifetime() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		runtime_lifetime_pin_identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& registration_epoch() const noexcept;
		[[nodiscard]] std::string_view registered_vfs_name() const noexcept;
		[[nodiscard]] const void* vfs_implementation_identity() const noexcept;
		[[nodiscard]] sqlite_same_process_shm_mapping_registry* registry() const noexcept;

		[[nodiscard]] sqlite_shm_lease_result<void> unregister_alias() noexcept;

	  private:
		friend class sqlite_same_process_shm_vfs_alias_registration_port;
		friend class sqlite_same_process_shm_vfs_alias_registration_test_peer;

		sqlite_shm_registered_vfs_alias(
			sqlite_shm_process_registry_handle process,
			sqlite_backend_opaque_identity shared_runtime_vfs_cohort,
			sqlite_backend_opaque_identity alias_lifetime,
			sqlite_backend_opaque_identity runtime_lifetime_identity,
			sqlite_backend_opaque_identity runtime_lifetime_pin_identity,
			sqlite_backend_opaque_identity registration_epoch,
			std::string registered_vfs_name,
			void* vfs_implementation,
			sqlite_shm_vfs_alias_lifecycle_binding::find_function find,
			sqlite_shm_vfs_alias_lifecycle_binding::unregister_function unregister_vfs,
			sqlite_shm_registry_alias_pin alias) noexcept;

		void quarantine_owner() noexcept;

		sqlite_shm_process_registry_handle process_;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort_;
		sqlite_backend_opaque_identity alias_lifetime_;
		sqlite_backend_opaque_identity runtime_lifetime_identity_;
		sqlite_backend_opaque_identity runtime_lifetime_pin_identity_;
		sqlite_backend_opaque_identity registration_epoch_;
		std::string registered_vfs_name_;
		void* vfs_implementation_{};
		sqlite_shm_vfs_alias_lifecycle_binding::find_function find_{};
		sqlite_shm_vfs_alias_lifecycle_binding::unregister_function unregister_vfs_{};
		std::optional<sqlite_shm_registry_alias_pin> alias_;
		bool unregistering_{};
		bool native_unregister_started_{};
		bool terminal_failure_{};
	};

	/**
	 * Closed native VFS registration validator for the process-global SHM registry.
	 *
	 * The port owns the exact reserve/begin/native-call/discovery/confirm order. It never carries the
	 * mapping-registry mutex across SQLite callbacks, never accepts caller-authored success status or
	 * discovery evidence, and constructs each closed lifecycle receipt before native effect. Same-thread
	 * reentry is rejected immediately; other-thread contention waits only at a bounded process-keyed
	 * gate which resets after fork. Native alias registration does not bind xShmMap/xShmUnmap/xClose
	 * or alter outward SQLite status projection; family installation is an explicit owner bridge.
	 */
	class sqlite_same_process_shm_vfs_alias_registration_port final
	{
	  public:
		sqlite_same_process_shm_vfs_alias_registration_port() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_registered_vfs_alias>
		register_alias(sqlite_shm_vfs_alias_lifecycle_binding binding) noexcept;

		/** Install or join one exact file-family coordinator through the registered alias owner. */
		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_registry_family_pin>
		install_or_join_family(sqlite_shm_registered_vfs_alias& alias,
							   const sqlite_shm_lease_family_binding& family);

	  private:
		friend class sqlite_shm_registered_vfs_alias;
		friend class sqlite_same_process_shm_vfs_alias_registration_test_peer;
		[[nodiscard]] static sqlite_shm_lease_result<void>
		unregister_alias(sqlite_shm_registered_vfs_alias& alias) noexcept;
		static void exhaust_lifecycle_sequence_for_testing() noexcept;
	};
} // namespace cxxlens::sdk
