#pragma once

#include <memory>
#include <span>
#include <string>
#include <string_view>

#include "sqlite_default_observation_internal.hpp"

namespace cxxlens::sdk
{
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
	 * Construct the Linux/default-filesystem behavioral qualifier retained by one observation
	 * capability. The qualifier never opens the target locator; it creates and removes an exact
	 * scratch fixture beneath the already-bound target parent instead.
	 */
	[[nodiscard]] result<std::shared_ptr<sqlite_source_shm_readonly_port>>
	make_sqlite_source_shm_readonly_preflight(
		const sqlite_default_observation_binding& binding,
		sqlite_backend_opaque_identity observation_capability_token);
} // namespace cxxlens::sdk
