#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "sqlite_default_observation_internal.hpp"
#include "sqlite_same_process_shm_reader_lifecycle_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * The source-family census captured before target SQLite xOpen/xShmMap.  Every flag is
	 * evidence supplied by the observation port; an incomplete or positive effect flag can never
	 * be interpreted as zero effect by the validator.
	 */
	struct sqlite_active_read_pre_effect_census
	{
		bool source_family_complete{};
		bool source_family_unchanged{};
		bool watch_loss_or_overflow_observed{};
		bool runtime_drift_observed{};
		bool vfs_drift_observed{};
		bool process_drift_observed{};
		bool fork_drift_observed{};
		bool unload_requested{};
		bool late_callback_observed{};
		bool nested_mapping_started{};
		bool create_observed{};
		bool write_observed{};
		bool truncate_observed{};
		bool extend_observed{};
		bool delete_observed{};
		bool resize_observed{};
	};

	/** Inputs sealed by the #201 U201-R0 source-read preflight. */
	struct sqlite_active_read_connection_request
	{
		std::string canonical_vfs_locator;
		sqlite_backend_namespace_census source_census;
		sqlite_source_shm_runtime_binding runtime;
		const void* forwarding_vfs_identity{};
		const void* pinned_underlying_vfs_identity{};
		const void* pinned_underlying_vfs_app_data_identity{};
		sqlite_backend_opaque_identity runtime_epoch;
		sqlite_backend_opaque_identity vfs_epoch;
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity fork_generation;
		sqlite_backend_opaque_identity connection_custody;
		sqlite_backend_opaque_identity outer_custody;
		sqlite_active_read_pre_effect_census pre_effect;
		sqlite_backend_connection_observation connection;
	};

	/**
	 * Authenticated U201-R0 evidence.  This is a move-independent evidence record, not a logical
	 * read receipt and not a mapping/normalization capability.  In particular, no field in this
	 * type represents decoded logical state or a zero-effect conclusion; those products require
	 * the later #205/#201-R1 terminal barriers.
	 */
	struct sqlite_active_read_connection_receipt
	{
		std::string contract{"cxxlens.sqlite-active-read-connection.v1"};
		detail::sqlite_active_read_connection_phase phase{
			detail::sqlite_active_read_connection_phase::active_read_connection};
		std::string canonical_vfs_locator;
		std::string source_profile;
		sqlite_backend_opaque_identity source_capability_token;
		sqlite_backend_opaque_identity parent_namespace_identity;
		sqlite_backend_opaque_identity source_guard_identity;
		sqlite_backend_opaque_identity target_namespace_epoch_identity;
		sqlite_backend_opaque_identity main_object_identity;
		sqlite_backend_opaque_identity main_entry_identity;
		sqlite_backend_opaque_identity wal_object_identity;
		sqlite_backend_opaque_identity wal_entry_identity;
		sqlite_backend_opaque_identity shm_object_identity;
		sqlite_backend_opaque_identity shm_entry_identity;
		sqlite_backend_opaque_identity filesystem_profile;
		sqlite_backend_opaque_identity runtime_epoch;
		sqlite_backend_opaque_identity vfs_epoch;
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity fork_generation;
		sqlite_backend_opaque_identity connection_custody;
		sqlite_backend_opaque_identity outer_custody;
		sqlite_source_shm_open_callback_receipt source_open_callback;
		sqlite_active_read_pre_effect_census pre_effect;
		sqlite_backend_connection_observation connection;
		std::shared_ptr<sqlite_source_shm_namespace_guard> source_namespace_guard;
	};

	/** Validate and seal the bounded #201 active-read connection product. */
	[[nodiscard]] result<sqlite_active_read_connection_receipt>
	validate_sqlite_active_read_connection(
		const sqlite_active_read_connection_request& request);

	/** Build the exact internal URI accepted by the readonly-SHM source profile. */
	[[nodiscard]] result<std::string>
	make_sqlite_source_shm_readonly_uri(std::string_view canonical_absolute_path);

	/** Internal exact scratch-family census; repeated calls must not share directory offsets. */
	[[nodiscard]] result<void>
	validate_sqlite_source_shm_readonly_scratch_family(int directory_descriptor);

	/** Pure bounded map-sequence validator used by qualification and focused negative tests. */
	[[nodiscard]] bool validate_sqlite_source_shm_readonly_map_sequence(
		std::span<const sqlite_backend_shm_map_observation> events,
		const void* pinned_underlying_vfs_identity,
		const void* pinned_underlying_vfs_app_data_identity,
		bool cold_route,
		bool require_later_map) noexcept;

	/**
	 * Exercise the exact pinned-origin xOpen/xClose proof used by qualification. This internal
	 * boundary keeps the SQLite ABI opaque while allowing malformed callback tables to be tested.
	 */
	[[nodiscard]] result<void> validate_sqlite_source_shm_readonly_origin_probe(
		const void* pinned_underlying_vfs_identity,
		const sqlite_source_shm_runtime_binding& runtime,
		std::shared_ptr<void> backend_lifetime,
		std::string_view scratch_probe_path);

	/**
	 * Seal the stable exact main/WAL/SHM family identity used by the process-global SHM registry.
	 * The identity excludes per-observation capability tokens and descriptor anchors, includes the
	 * canonical locator and retained main/WAL/filesystem/mount receipts, and deliberately carries
	 * the stable SHM filesystem/mount pair rather than the dynamic SHM inode. The first
	 * authenticated native xShmMap may create that inode; its exact object/entry receipt remains
	 * bound to the target namespace epoch and the writer/reader attachment receipt.
	 */
	[[nodiscard]] result<sqlite_backend_opaque_identity> seal_sqlite_source_shm_exact_file_family(
		std::string_view canonical_vfs_locator,
		const sqlite_backend_opaque_identity& parent_namespace_identity,
		std::string_view sqlite_source_id,
		std::span<const sqlite_backend_entry_observation> entries);

	/**
	 * Construct the Linux/default-filesystem behavioral qualifier retained by one observation
	 * capability. The qualifier never opens the target locator; it creates and removes an exact
	 * scratch fixture beneath the already-bound target parent instead.
	 */
	[[nodiscard]] result<std::shared_ptr<sqlite_source_shm_readonly_port>>
	make_sqlite_source_shm_readonly_preflight(
		const sqlite_default_observation_binding& binding,
		sqlite_backend_opaque_identity observation_capability_token);

	/** Construct one retained target namespace epoch from an already captured active file family.
	 */
	[[nodiscard]] result<std::shared_ptr<sqlite_source_shm_target_namespace_epoch>>
	make_sqlite_source_shm_target_namespace_epoch(std::string_view logical_main_locator,
												  const sqlite_backend_namespace_census& census);

	/** Source-private factory; the returned opaque authority can mint only through writer custody.
	 */
	[[nodiscard]] result<sqlite_source_shm_target_namespace_epoch_borrow_minter>
	make_sqlite_source_shm_target_namespace_epoch_borrow_minter(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& target_epoch);
} // namespace cxxlens::sdk
