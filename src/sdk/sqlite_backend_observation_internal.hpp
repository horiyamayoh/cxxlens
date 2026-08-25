#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	class sqlite_wal_recovery_workspace_builder;
	class store_operation_port;
	class sqlite_logical_read_receipt;
	struct sqlite_exact_empty_normalization_input;
	class sqlite_exact_empty_normalization_completed_edge;

	/** Closed filesystem roles observed around one SQLite main database. */
	enum class sqlite_backend_file_role : std::uint8_t
	{
		main_database,
		write_ahead_log,
		shared_memory,
		rollback_journal,
	};

	/**
	 * A namespace entry is either absent, a held regular/equivalent object, a known but unreadable
	 * entry, or a present object whose semantics are not admissible as SQLite storage.
	 */
	enum class sqlite_backend_entry_state : std::uint8_t
	{
		absent,
		held_regular,
		present_unreadable,
		unsupported_kind,
	};

	/** Backend-owned identity bytes. Consumers may compare them but must not interpret them. */
	struct sqlite_backend_opaque_identity
	{
		std::string profile;
		std::vector<std::byte> bytes;

		[[nodiscard]] bool operator==(const sqlite_backend_opaque_identity&) const = default;
	};

	struct sqlite_backend_copy_receipt
	{
		std::uint64_t byte_count{};
		std::string sha256;

		[[nodiscard]] bool operator==(const sqlite_backend_copy_receipt&) const = default;
	};

	class sqlite_backend_private_snapshot;
	struct sqlite_backend_entry_observation;
	class sqlite_source_shm_namespace_guard;
	class sqlite_source_shm_target_namespace_epoch;
	class sqlite_shm_reader_native_ok_projection_reservation;
	class sqlite_shm_writer_reader_borrow_mint_capability;
	namespace detail
	{
		struct sqlite_source_shm_target_namespace_epoch_reader_borrow_state;
		struct sqlite_source_shm_target_namespace_epoch_borrow_minter_state;
		class sqlite_writer_shm_mapping_epoch_state;
		class sqlite_writer_shm_generation_epoch_custody;
		class sqlite_shm_mapping_lease_state;
	} // namespace detail

	/**
	 * Source-private move-only custody for a reader's exact writer namespace epoch.
	 *
	 * Its implementation intentionally hides the owner capability; no raw or shared epoch pointer
	 * crosses this boundary.  Only authenticated writer-member and reader-projection machinery may
	 * mint or consume it.
	 */
	class sqlite_source_shm_target_namespace_epoch_reader_borrow final
	{
	  public:
		~sqlite_source_shm_target_namespace_epoch_reader_borrow() noexcept;
		sqlite_source_shm_target_namespace_epoch_reader_borrow(
			sqlite_source_shm_target_namespace_epoch_reader_borrow&&) noexcept;
		sqlite_source_shm_target_namespace_epoch_reader_borrow&
		operator=(sqlite_source_shm_target_namespace_epoch_reader_borrow&&) = delete;
		sqlite_source_shm_target_namespace_epoch_reader_borrow(
			const sqlite_source_shm_target_namespace_epoch_reader_borrow&) = delete;
		sqlite_source_shm_target_namespace_epoch_reader_borrow&
		operator=(const sqlite_source_shm_target_namespace_epoch_reader_borrow&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class sqlite_source_shm_target_namespace_epoch;
		friend class sqlite_source_shm_target_namespace_epoch_borrow_minter;
		friend class sqlite_writer_shm_mapping_epoch_arm;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_shm_reader_native_ok_projection_permit;
		friend class sqlite_shm_reader_handoff;
		friend class detail::sqlite_shm_mapping_lease_state;

		explicit sqlite_source_shm_target_namespace_epoch_reader_borrow(
			std::unique_ptr<detail::sqlite_source_shm_target_namespace_epoch_reader_borrow_state>
				state) noexcept;
		[[nodiscard]] result<void> recheck() const;
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity&
		parent_namespace_identity() const noexcept;
		[[nodiscard]] result<sqlite_backend_entry_observation>
		retained_entry(sqlite_backend_file_role role) const;
		[[nodiscard]] bool matches_projection_reservation(
			const sqlite_shm_reader_native_ok_projection_reservation& reservation) const noexcept;
		[[nodiscard]] result<void> disarm();

		std::unique_ptr<detail::sqlite_source_shm_target_namespace_epoch_reader_borrow_state>
			state_;
	};

	/** Opaque writer-held mint authority; it exposes neither target epoch nor controller. */
	class sqlite_source_shm_target_namespace_epoch_borrow_minter final
	{
	  public:
		~sqlite_source_shm_target_namespace_epoch_borrow_minter() noexcept;
		sqlite_source_shm_target_namespace_epoch_borrow_minter(
			sqlite_source_shm_target_namespace_epoch_borrow_minter&&) noexcept;
		sqlite_source_shm_target_namespace_epoch_borrow_minter&
		operator=(sqlite_source_shm_target_namespace_epoch_borrow_minter&&) = delete;
		sqlite_source_shm_target_namespace_epoch_borrow_minter(
			const sqlite_source_shm_target_namespace_epoch_borrow_minter&) = delete;
		sqlite_source_shm_target_namespace_epoch_borrow_minter&
		operator=(const sqlite_source_shm_target_namespace_epoch_borrow_minter&) = delete;

		[[nodiscard]] bool valid() const noexcept;

	  private:
		friend class sqlite_writer_shm_mapping_epoch_arm;
		friend class sqlite_shm_writer_member_authority;
		friend class sqlite_shm_writer_reader_borrow_mint_capability;
		friend class detail::sqlite_writer_shm_mapping_epoch_state;
		friend class detail::sqlite_writer_shm_generation_epoch_custody;
		friend result<sqlite_source_shm_target_namespace_epoch_borrow_minter>
		make_sqlite_source_shm_target_namespace_epoch_borrow_minter(
			const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>&);

		explicit sqlite_source_shm_target_namespace_epoch_borrow_minter(
			std::unique_ptr<detail::sqlite_source_shm_target_namespace_epoch_borrow_minter_state>
				state) noexcept;
		[[nodiscard]] result<sqlite_source_shm_target_namespace_epoch_reader_borrow>
		mint(const sqlite_shm_reader_native_ok_projection_reservation& reservation);
		[[nodiscard]] bool retain_generation_authority() noexcept;
		void release_generation_authority() noexcept;

		std::unique_ptr<detail::sqlite_source_shm_target_namespace_epoch_borrow_minter_state>
			state_;
	};

	/** Bounded builder for one pathname-free source-private SQLite snapshot. */
	class sqlite_backend_private_snapshot_builder
	{
	  public:
		virtual ~sqlite_backend_private_snapshot_builder() = default;

		[[nodiscard]] virtual result<void> append(std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_private_snapshot>>
		seal(std::uint64_t expected_byte_count, std::string_view expected_sha256) = 0;
	};

	/**
	 * Owned sealed private-copy VFS. The holder must outlive every SQLite connection opened from
	 * the generated URI; destruction unregisters the VFS and releases the pathname-free object.
	 */
	class sqlite_backend_private_snapshot
	{
	  public:
		virtual ~sqlite_backend_private_snapshot() = default;

		[[nodiscard]] virtual std::string_view application_generated_uri() const noexcept = 0;
		[[nodiscard]] virtual std::string_view registered_vfs_name() const noexcept = 0;
		[[nodiscard]] virtual const void* vfs_implementation_identity() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity&
		source_capability_token() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_copy_receipt& receipt() const noexcept = 0;
	};

	enum class sqlite_backend_replacement_state : std::uint8_t
	{
		exact_same_entry_and_object,
		absent,
		replaced,
		unreadable,
		unsupported_kind,
	};

	/** One retained exact object opened by the bound VFS observation backend. */
	class sqlite_backend_held_object
	{
	  public:
		virtual ~sqlite_backend_held_object() = default;

		[[nodiscard]] virtual sqlite_backend_file_role role() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity&
		object_identity() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity&
		directory_entry_identity() const noexcept = 0;
		/** Exact filesystem/mount profile observed through the retained object descriptor. */
		[[nodiscard]] virtual const std::optional<sqlite_backend_opaque_identity>&
		object_filesystem_profile() const noexcept
		{
			static const std::optional<sqlite_backend_opaque_identity> unavailable;
			return unavailable;
		}
		/** Exact Linux mount identity observed through the retained object descriptor. */
		[[nodiscard]] virtual const std::optional<sqlite_backend_opaque_identity>&
		object_mount_identity() const noexcept
		{
			static const std::optional<sqlite_backend_opaque_identity> unavailable;
			return unavailable;
		}
		/** Recheck only the already-retained object descriptor; never resolve its host pathname. */
		[[nodiscard]] virtual result<void> recheck_retained_object() const
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "retained-object-recheck-unavailable"};
		}
		[[nodiscard]] virtual result<std::uint64_t> size() const = 0;
		[[nodiscard]] virtual result<void> read_exact(std::uint64_t offset,
													  std::span<std::byte> output) const = 0;
		[[nodiscard]] virtual result<std::string> sha256() const = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_private_snapshot>>
		copy_exact(sqlite_backend_private_snapshot_builder& builder,
				   std::span<std::byte> scratch) const = 0;
		[[nodiscard]] virtual result<sqlite_backend_replacement_state>
		recheck_current_entry() const = 0;
	};

	struct sqlite_backend_entry_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
		sqlite_backend_entry_state state{sqlite_backend_entry_state::absent};
		std::optional<sqlite_backend_opaque_identity> object_identity;
		std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
		std::shared_ptr<const sqlite_backend_held_object> held_object;
		std::optional<sqlite_backend_opaque_identity> object_filesystem_profile;
		bool direct_regular_entry{};
	};

	/**
	 * Source-private stat-only SHM census used by the writer-map epoch. It deliberately carries no
	 * duplicate target descriptor or owned object; the native SQLite SHM attachment and retained
	 * parent namespace are the lifetime authorities.
	 */
	struct sqlite_backend_writer_shm_stat_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::shared_memory};
		sqlite_backend_entry_state state{sqlite_backend_entry_state::absent};
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity filesystem_profile;
		sqlite_backend_opaque_identity mount_identity;
		std::optional<sqlite_backend_opaque_identity> object_identity;
		std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
		std::uint64_t byte_count{};

		[[nodiscard]] bool
		operator==(const sqlite_backend_writer_shm_stat_observation&) const = default;
	};

	/** Retained-parent observation for a normalizer-owned WAL or rollback-journal leaf. */
	struct sqlite_backend_normalization_sidecar_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::write_ahead_log};
		sqlite_backend_opaque_identity object_identity;
		sqlite_backend_opaque_identity directory_entry_identity;
		std::uint64_t byte_count{};

		[[nodiscard]] bool
		operator==(const sqlite_backend_normalization_sidecar_observation&) const = default;
	};

	/**
	 * Census-owned continuous namespace proof. It retains the exact parent descriptor and
	 * root-to-parent watches used while the census was captured; consumers must recheck it around
	 * every pre-qualification read and transfer it into the qualified target epoch.
	 */
	class sqlite_source_shm_namespace_guard
	{
	  public:
		virtual ~sqlite_source_shm_namespace_guard() = default;

		[[nodiscard]] virtual std::string_view logical_main_locator() const noexcept = 0;
		[[nodiscard]] virtual std::string_view anchored_main_locator() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity& identity() const noexcept = 0;
		[[nodiscard]] virtual result<sqlite_backend_entry_observation>
		retained_entry(sqlite_backend_file_role role) const = 0;
		/**
		 * Writer-map-only stat boundary. `after_native_map` permits the exact one-time absent-to-
		 * direct-regular create transition; it never opens a duplicate target descriptor.
		 */
		[[nodiscard]] virtual result<sqlite_backend_writer_shm_stat_observation>
		observe_writer_shm_stat(const bool after_native_map) const
		{
			(void)after_native_map;
			return error{
				"store.backend-unavailable", "sqlite-observation", "writer-shm-stat-unavailable"};
		}
		[[nodiscard]] virtual result<void> recheck() const = 0;
		/** One-shot transfer from census guard authority into one qualified target epoch. */
		[[nodiscard]] virtual result<void> claim_target_epoch() = 0;
		/**
		 * Enter the one-shot accepted-empty normalization namespace epoch.  The default filesystem
		 * implementation retains the already-open parent descriptor and admits only the exact WAL
		 * and rollback-journal create/delete transitions requested through the methods below.
		 */
		[[nodiscard]] virtual result<void> claim_exact_empty_normalization_epoch()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-epoch-unavailable"};
		}
		[[nodiscard]] virtual result<void> recheck_exact_empty_normalization_epoch() const
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-recheck-unavailable"};
		}
		[[nodiscard]] virtual result<sqlite_backend_normalization_sidecar_observation>
		observe_exact_empty_normalization_sidecar(sqlite_backend_file_role)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-sidecar-unavailable"};
		}
		[[nodiscard]] virtual result<sqlite_backend_opaque_identity>
		synchronize_exact_empty_normalization_parent(sqlite_backend_file_role,
													 store_operation_port&)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-parent-sync-unavailable"};
		}
		[[nodiscard]] virtual result<sqlite_backend_opaque_identity>
		delete_exact_empty_normalization_sidecar(sqlite_backend_file_role,
												 const sqlite_backend_opaque_identity&,
												 const sqlite_backend_opaque_identity&,
												 std::span<const std::byte>,
												 store_operation_port&)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-delete-unavailable"};
		}
		[[nodiscard]] virtual result<void> finish_exact_empty_normalization_epoch()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-finish-unavailable"};
		}
		/** Consume the continuous guard only after the target epoch has completed. */
		[[nodiscard]] virtual result<void> finish() = 0;
	};

	struct sqlite_backend_namespace_census
	{
		std::string profile;
		sqlite_backend_opaque_identity capability_token;
		sqlite_backend_opaque_identity parent_namespace_identity;
		std::array<sqlite_backend_entry_observation, 4U> entries;
		std::shared_ptr<sqlite_source_shm_namespace_guard> source_shm_guard;
	};

	/** One duplicate-target-open-free namespace observation safe under a live SQLite lock. */
	struct sqlite_backend_stat_only_entry_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
		sqlite_backend_entry_state state{sqlite_backend_entry_state::absent};
		sqlite_backend_opaque_identity parent_namespace_identity;
		std::optional<sqlite_backend_opaque_identity> object_identity;
		std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
	};

	/** Durable raw-bootstrap proof; absence of a receipt is never an empty-database authority. */
	struct sqlite_backend_zero_main_receipt
	{
		std::string profile;
		sqlite_backend_opaque_identity capability_token;
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity directory_entry_identity;
		sqlite_backend_opaque_identity object_identity;
		std::shared_ptr<const sqlite_backend_held_object> held_main;
		std::uint64_t byte_count{};
		std::string sha256;
		bool object_full_sync{};
		bool parent_namespace_sync{};
	};

	struct sqlite_backend_vfs_binding
	{
		std::string_view observation_profile;
		std::string_view registered_vfs_name;
		const void* vfs_implementation_identity{};
		const void* backend_lifetime_identity{};
		const void* observation_capability_identity{};
		const void* runtime_identity{};
		const void* runtime_lifetime_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
	};

	/** Exact functions and lifetime retained from the one pinned SQLite runtime image. */
	struct sqlite_source_shm_runtime_binding
	{
		using open_v2_function = int (*)(const char*, void**, int, const char*);
		using close_v2_function = int (*)(void*);
		using exec_callback = int (*)(void*, int, char**, char**);
		using exec_function = int (*)(void*, const char*, exec_callback, void*, char**);
		using errmsg_function = const char* (*)(void*);
		using free_function = void (*)(void*);
		using source_id_function = const char* (*)();
		using uri_parameter_function = const char* (*)(const char*, const char*);
		using uri_key_function = const char* (*)(const char*, int);
		using vfs_find_function = void* (*)(const char*);
		using vfs_register_function = int (*)(void*, int);
		using vfs_unregister_function = int (*)(void*);

		const void* runtime_identity{};
		const void* runtime_image_identity{};
		const void* runtime_lifetime_identity{};
		std::shared_ptr<void> runtime_lifetime;
		open_v2_function open_v2{};
		close_v2_function close_v2{};
		exec_function exec{};
		errmsg_function errmsg{};
		free_function free_memory{};
		source_id_function source_id{};
		uri_parameter_function uri_parameter{};
		uri_key_function uri_key{};
		vfs_find_function vfs_find{};
		vfs_register_function vfs_register{};
		vfs_unregister_function vfs_unregister{};
	};

	/** Inputs sealed by the target-independent readonly-SHM behavioral qualification. */
	struct sqlite_source_shm_qualification_request
	{
		sqlite_source_shm_runtime_binding runtime;
		std::string canonical_vfs_locator;
		sqlite_backend_namespace_census source_census;
		sqlite_backend_opaque_identity parent_namespace_identity;
		std::string registered_vfs_name;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* backend_lifetime_identity{};
		sqlite_backend_opaque_identity observation_capability_token;
	};

	/** One-shot fd-anchored writable fixture pathname accepted only by a qualification scope. */
	struct sqlite_source_shm_qualification_fixture_fullpath_plan
	{
		std::string canonical_vfs_locator;
		std::string registered_vfs_name;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* backend_lifetime_identity{};
		sqlite_backend_opaque_identity observation_capability_token;
	};

	/**
	 * Retained target-parent namespace epoch. Native SQLite paths are projected through its
	 * descriptor anchor, and every qualified SHM map is bracketed by exact namespace rechecks.
	 */
	class sqlite_source_shm_target_namespace_epoch
	{
	  public:
		virtual ~sqlite_source_shm_target_namespace_epoch() = default;

		[[nodiscard]] virtual std::string_view logical_main_locator() const noexcept = 0;
		[[nodiscard]] virtual std::string_view anchored_main_locator() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity& identity() const noexcept = 0;
		/** Exact parent namespace identity retained by this target epoch. */
		[[nodiscard]] virtual const sqlite_backend_opaque_identity&
		parent_namespace_identity() const noexcept = 0;
		[[nodiscard]] virtual result<sqlite_backend_entry_observation>
		retained_entry(sqlite_backend_file_role role) const = 0;
		/** Writer-map stat-only observation projected from the retained namespace epoch. */
		[[nodiscard]] virtual result<sqlite_backend_writer_shm_stat_observation>
		observe_writer_shm_stat(const bool after_native_map) const
		{
			(void)after_native_map;
			return error{
				"store.backend-unavailable", "sqlite-observation", "writer-shm-stat-unavailable"};
		}
		[[nodiscard]] virtual result<void> recheck() const = 0;
		/** Consume the epoch after the qualified connection and eager decode have completed. */
		[[nodiscard]] virtual result<void> finish() = 0;
	};

	/** Closed proof that one runtime/VFS/filesystem profile passed both required SHM routes. */
	struct sqlite_source_shm_qualification_receipt
	{
		std::string profile;
		std::string sqlite_source_id;
		std::string filesystem_profile;
		const void* runtime_identity{};
		const void* runtime_image_identity{};
		const void* runtime_lifetime_identity{};
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* backend_lifetime_identity{};
		sqlite_backend_opaque_identity observation_capability_token;
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity expected_shared_memory_object_identity;
		sqlite_backend_opaque_identity expected_shared_memory_entry_identity;
		sqlite_backend_opaque_identity target_namespace_epoch_identity;
		std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
		sqlite_backend_opaque_identity sealed_qualification_token;
		bool first_map_nonmutating{};
		bool later_map_nonmutating{};
		bool cantinit_heap_wal_index_route_proven{};
		bool readonly_mapped_wal_index_retry_route_proven{};
		/** Stable exact main/WAL/SHM family identity, independent of observation capability/FDs. */
		sqlite_backend_opaque_identity exact_file_family;
	};

	/** Exact source URI/open tuple accepted by the qualified forwarding-VFS callback. */
	struct sqlite_source_shm_qualified_open_plan
	{
		sqlite_source_shm_runtime_binding runtime;
		sqlite_source_shm_qualification_receipt qualification;
		std::string canonical_vfs_locator;
		std::string delegated_vfs_locator;
		std::string application_generated_uri;
		std::string registered_vfs_name;
		int open_flags{};
	};

	/** Strict scratch-only candidate tuple used to produce, but never replace, qualification. */
	struct sqlite_source_shm_qualification_open_plan
	{
		sqlite_source_shm_runtime_binding runtime;
		std::string canonical_vfs_locator;
		std::string filesystem_profile;
		std::string application_generated_uri;
		std::string registered_vfs_name;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		const void* backend_lifetime_identity{};
		sqlite_backend_opaque_identity observation_capability_token;
		int open_flags{};
	};

	/** Receipt produced inside the owned main xOpen callback before native delegation. */
	struct sqlite_source_shm_open_callback_receipt
	{
		std::string profile;
		sqlite_backend_opaque_identity connection_token;
		sqlite_backend_opaque_identity qualification_token;
		sqlite_backend_opaque_identity target_namespace_epoch_identity;
		std::string canonical_vfs_locator;
		std::string delegated_vfs_locator;
		std::string application_generated_uri;
		std::string registered_vfs_name;
		std::string mode;
		std::string cache;
		std::string readonly_shm;
		int input_flags{};
		const void* runtime_identity{};
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
	};

	enum class sqlite_backend_open_outcome : std::uint8_t
	{
		attempted,
		failed,
		succeeded,
	};

	struct sqlite_backend_open_observation
	{
		sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
		int input_flags{};
		sqlite_backend_open_outcome outcome{sqlite_backend_open_outcome::attempted};
		std::optional<int> returned_flags;
		std::optional<sqlite_backend_opaque_identity> object_identity;
		std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
	};

	enum class sqlite_backend_shm_lock_mode : std::uint8_t
	{
		shared,
		exclusive,
	};

	struct sqlite_backend_shm_lock_observation
	{
		int offset{};
		int count{};
		sqlite_backend_shm_lock_mode mode{sqlite_backend_shm_lock_mode::shared};
	};

	/**
	 * Source-private receipt for the narrow DF-0205 native-OK projection. The receipt is present
	 * only after the registry has committed this exact map and the forwarding VFS has checked the
	 * corresponding live handoff, mapping tuple, callback invocation, and native effect identity.
	 */
	struct sqlite_backend_shm_map_projection_receipt
	{
		std::uint64_t reader_generation{};
		int page{};
		int page_size{};
		int caller_extend{};
		int delegated_extend{};
		const volatile void* native_mapping{};
		std::uint64_t byte_offset{};
		std::uint64_t byte_count{};
		std::uint64_t sealed_shm_size{};
		sqlite_backend_opaque_identity callback_invocation;
		sqlite_backend_opaque_identity native_effect;
	};

	/** One delegated xShmMap call and the exact native-to-forwarded transition. */
	struct sqlite_backend_shm_map_observation
	{
		int page{};
		int page_size{};
		int caller_extend{};
		int delegated_extend{};
		int native_status{};
		int returned_status{};
		bool native_mapping_nonnull{};
		bool returned_mapping_nonnull{};
		bool readonly_family_seen_before{};
		bool readonly_family_seen_after{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		/** Exact native pointer observed by this one map callback; never an authority by itself. */
		const volatile void* native_mapping_identity{};
		/** Present only for a committed, independently authenticated native-OK map. */
		std::optional<sqlite_backend_shm_map_projection_receipt> native_ok_projection_receipt;
	};

	struct sqlite_backend_connection_observation
	{
		std::string profile;
		sqlite_backend_opaque_identity capability_token;
		sqlite_backend_opaque_identity connection_token;
		std::vector<sqlite_backend_open_observation> open_events;
		std::optional<sqlite_backend_opaque_identity> shared_memory_object_identity;
		std::optional<sqlite_backend_opaque_identity> shared_memory_entry_identity;
		std::vector<sqlite_backend_shm_lock_observation> held_shm_locks;
		bool complete{};
		bool main_handle_open{};
		std::optional<sqlite_source_shm_open_callback_receipt> source_shm_open_callback_receipt;
		std::vector<sqlite_backend_shm_map_observation> shm_map_events;
	};

	/** Target-independent behavioral qualifier for the closed readonly-SHM source profile. */
	class sqlite_source_shm_readonly_port
	{
	  public:
		virtual ~sqlite_source_shm_readonly_port() = default;

		[[nodiscard]] virtual result<sqlite_source_shm_qualified_open_plan>
		qualify(sqlite_source_shm_qualification_request request) = 0;
	};

	/** Persistent-effect authority exposed by an observed SQLite connection. */
	enum class sqlite_backend_effect_stage : std::uint8_t
	{
		denied,
		wal_shm_coordination_only,
		fully_armed,
	};

	class sqlite_backend_connection_observation_scope;
	class sqlite_backend_effect_arm_authority;

	/** Exact precondition and target for one monotonic effect-stage transition. */
	struct sqlite_backend_effect_arm_request
	{
		sqlite_backend_effect_stage target_stage{sqlite_backend_effect_stage::denied};
		sqlite_backend_opaque_identity capability_token;
		sqlite_backend_opaque_identity connection_token;
		std::string canonical_vfs_locator;
		sqlite_backend_opaque_identity prerequisite_receipt;
		std::shared_ptr<const sqlite_backend_effect_arm_authority> authority;
	};

	/** Backend-sealed evidence for an activated deny state or a later effect arm. */
	struct sqlite_backend_effect_arm_receipt
	{
		std::string profile;
		sqlite_backend_opaque_identity capability_token;
		sqlite_backend_opaque_identity connection_token;
		std::string canonical_vfs_locator;
		sqlite_backend_opaque_identity prerequisite_receipt;
		sqlite_backend_opaque_identity validation_receipt;
		sqlite_backend_effect_stage stage{sqlite_backend_effect_stage::denied};
		std::uint64_t sequence{};
		bool armed_after_underlying_exclusive_lock{};
	};

	/**
	 * Backend-specific synchronous verifier invoked immediately before effects become reachable.
	 * Implementations must not create or close a duplicate target-file descriptor while a POSIX
	 * SQLite lock may be live.
	 */
	class sqlite_backend_effect_arm_authority
	{
	  public:
		virtual ~sqlite_backend_effect_arm_authority() = default;

		[[nodiscard]] virtual result<sqlite_backend_opaque_identity>
		recheck_and_seal(const sqlite_backend_effect_arm_request& request,
						 const sqlite_backend_connection_observation_scope& connection) const = 0;
	};

	/** Source-private control plane for one connection's VFS effect policy. */
	class sqlite_backend_effect_gate
	{
	  public:
		virtual ~sqlite_backend_effect_gate() = default;

		[[nodiscard]] virtual result<sqlite_backend_effect_arm_receipt>
		activate_denied(const sqlite_backend_opaque_identity& capability_token,
						const sqlite_backend_opaque_identity& connection_token,
						std::string_view canonical_vfs_locator) = 0;
		[[nodiscard]] virtual result<sqlite_backend_effect_arm_receipt>
		arm_now(sqlite_backend_effect_arm_request request) = 0;
		[[nodiscard]] virtual result<void>
		install_arm_on_exclusive_lock(sqlite_backend_effect_arm_request request) = 0;
		[[nodiscard]] virtual sqlite_backend_effect_stage stage() const noexcept = 0;
		[[nodiscard]] virtual bool enforcement_active() const noexcept = 0;
		[[nodiscard]] virtual std::optional<sqlite_backend_effect_arm_receipt>
		latest_receipt() const = 0;
	};

	/** One exact sqlite3_open_v2 occurrence and its later VFS SHM-lock observations. */
	class sqlite_backend_connection_observation_scope
	{
	  public:
		virtual ~sqlite_backend_connection_observation_scope() = default;

		[[nodiscard]] virtual const sqlite_backend_opaque_identity& token() const noexcept = 0;
		[[nodiscard]] virtual result<sqlite_backend_connection_observation> snapshot() const = 0;
		/** Older observation profiles fail closed and cannot arm this source-only profile. */
		[[nodiscard]] virtual result<void>
		// NOLINTNEXTLINE(performance-unnecessary-value-param): the override consumes this plan.
		arm_source_shm_readonly_profile(sqlite_source_shm_qualified_open_plan)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "source-shm-readonly-arm-unavailable"};
		}
		/** Only a dedicated scratch qualification scope may accept this provisional tuple. */
		[[nodiscard]] virtual result<void>
		// NOLINTNEXTLINE(performance-unnecessary-value-param): the override consumes this plan.
		arm_source_shm_readonly_qualification_candidate(sqlite_source_shm_qualification_open_plan)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "source-shm-qualification-arm-unavailable"};
		}
		/** One exact producer main pathname may bypass host canonicalization exactly once. */
		[[nodiscard]] virtual result<void> arm_source_shm_qualification_fixture_fullpath(
			// NOLINTNEXTLINE(performance-unnecessary-value-param): the override consumes this plan.
			sqlite_source_shm_qualification_fixture_fullpath_plan)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "source-shm-qualification-fixture-fullpath-unavailable"};
		}
		/** Older observation profiles return null and are rejected by effect-gated Store paths. */
		[[nodiscard]] virtual sqlite_backend_effect_gate* effect_gate_port() noexcept
		{
			return nullptr;
		}
		/** Consume #201 and prepare the actual callback-bound #202 profile before main xOpen. */
		[[nodiscard]] virtual result<void>
		arm_exact_empty_normalization_profile(sqlite_logical_read_receipt&&,
											  const sqlite_exact_empty_normalization_input&)
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-profile-unavailable"};
		}
		/** Install sequence two; the VFS installs sequence three after its zero-WAL xOpen. */
		[[nodiscard]] virtual result<void> install_exact_empty_normalization_coordination_arm()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-arm-unavailable"};
		}
		/** Seal only after the real connection close and retained-parent post census. */
		[[nodiscard]] virtual result<
			std::shared_ptr<const sqlite_exact_empty_normalization_completed_edge>>
		finish_exact_empty_normalization_profile()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "exact-empty-normalization-finish-unavailable"};
		}
		/**
		 * Whether the current-v3 Store writer must enter the source-SHM mapping epoch route.
		 * Unknown observation profiles enter this route by default and therefore fail closed if
		 * they do not implement the required epoch operations. A source-private VFS that owns
		 * and authenticates its own SHM callbacks must explicitly override this to false.
		 */
		[[nodiscard]] virtual bool requires_source_shm_writer_mapping_epoch() const noexcept
		{
			return true;
		}
		/** Source-private cut after the exact fully-armed current-v3 receipt. */
		[[nodiscard]] virtual result<void> install_current_v3_writer_eligibility()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "writer-eligibility-unavailable"};
		}
		/** Retain the qualified source namespace epoch for the following current-v3 writer. */
		[[nodiscard]] virtual result<void> arm_writer_shm_mapping_epoch(
			// NOLINTNEXTLINE(performance-unnecessary-value-param): the override consumes the epoch.
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch,
			sqlite_backend_opaque_identity sqlite_source_id) // NOLINT(performance-unnecessary-value-param):
															 // overrides consume identity.
		{
			(void)target_namespace_epoch;
			(void)sqlite_source_id;
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "writer-mapping-epoch-unavailable"};
		}
		/** Whether this scope has already entered a native SHM-map callback. */
		[[nodiscard]] virtual bool native_shm_map_attempted() const noexcept
		{
			return false;
		}
		/** Request the current-v3 writer epoch to be armed before the first native SHM map. */
		[[nodiscard]] virtual result<void> request_writer_shm_mapping_epoch()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "writer-mapping-epoch-request-unavailable"};
		}
		/** Whether the writer epoch has already been armed for this connection. */
		[[nodiscard]] virtual bool writer_shm_mapping_epoch_armed() const noexcept
		{
			return false;
		}
	};

	/**
	 * Source-private typed observation port required by the SQLite v3 filesystem profiles. The
	 * implementation is bound to one exact registered VFS object and its separate lifetime token.
	 */
	class sqlite_backend_observation_capability
	{
	  public:
		virtual ~sqlite_backend_observation_capability() = default;

		[[nodiscard]] virtual sqlite_backend_vfs_binding binding() const noexcept = 0;
		[[nodiscard]] virtual const sqlite_backend_opaque_identity&
		capability_token() const noexcept = 0;
		/** Older and non-filesystem profiles expose no readonly-SHM behavioral qualifier. */
		[[nodiscard]] virtual sqlite_source_shm_readonly_port* source_shm_readonly_port() noexcept
		{
			return nullptr;
		}
		[[nodiscard]] virtual result<sqlite_backend_namespace_census>
		capture_namespace(std::string_view canonical_vfs_locator) const = 0;
		/** Repeated parent-relative stat only; never opens or closes the target inode. */
		[[nodiscard]] virtual result<sqlite_backend_stat_only_entry_observation>
		observe_entry_state_without_open(std::string_view, sqlite_backend_file_role) const
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "stat-only-entry-observation-unavailable"};
		}
		[[nodiscard]] virtual result<bool>
		recheck_namespace(const sqlite_backend_namespace_census& prior,
						  std::string_view canonical_vfs_locator) const = 0;
		[[nodiscard]] virtual result<sqlite_backend_zero_main_receipt>
		exclusive_create_sync_zero_main(std::string_view canonical_vfs_locator) = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_private_snapshot_builder>>
		create_private_snapshot() = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_wal_recovery_workspace_builder>>
		create_wal_recovery_workspace()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "wal-recovery-workspace-unavailable"};
		}
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		begin_connection_observation(std::string_view canonical_vfs_locator) = 0;
		/** Same pinned alias/runtime, with SQLite's explicit no-filesystem `:memory:` profile. */
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		begin_ephemeral_connection_observation()
		{
			return error{"store.backend-unavailable",
						 "sqlite-observation",
						 "ephemeral-observation-unavailable"};
		}
	};
} // namespace cxxlens::sdk
