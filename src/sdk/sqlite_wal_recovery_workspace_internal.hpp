#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_backend_observation_internal.hpp"
#include "sqlite_private_snapshot_internal.hpp"
#include "sqlite_wal_receipt_internal.hpp"

namespace cxxlens::sdk
{
	inline constexpr int sqlite_wal_recovery_open_readwrite = 0x00000002;
	inline constexpr int sqlite_wal_recovery_open_fullmutex = 0x00010000;
	inline constexpr int sqlite_wal_recovery_open_privatecache = 0x00040000;
	inline constexpr int sqlite_wal_recovery_required_open_flags =
		sqlite_wal_recovery_open_readwrite | sqlite_wal_recovery_open_fullmutex |
		sqlite_wal_recovery_open_privatecache;
	inline constexpr std::size_t sqlite_wal_recovery_copy_buffer_bound = 64U * 1024U;

	enum class sqlite_wal_recovery_copy_role : std::uint8_t
	{
		main_database,
		authoritative_wal_prefix,
	};

	struct sqlite_wal_recovery_workspace_expectation
	{
		sqlite_backend_copy_receipt main_database;
		sqlite_backend_copy_receipt authoritative_wal_prefix;
		sqlite_wal_scan_receipt source_wal_scan;
	};

	struct sqlite_wal_recovery_open_receipt
	{
		std::uint64_t main_attempt_count{};
		std::uint64_t main_success_count{};
		int last_main_input_flags{};
		int last_main_output_flags{};
		std::uint64_t wal_attempt_count{};
		std::uint64_t wal_success_count{};
		int last_wal_input_flags{};
		int last_wal_output_flags{};
		bool main_readwrite_no_create{};
		bool wal_was_preexisting{};
	};

	struct sqlite_wal_recovery_shm_receipt
	{
		std::uint64_t map_request_count{};
		std::uint64_t created_region_count{};
		std::uint64_t private_byte_count{};
		std::uint64_t lock_request_count{};
		std::uint64_t unlock_request_count{};
		std::uint64_t barrier_count{};
		std::uint64_t unmap_request_count{};
		std::uint64_t held_lock_count{};
	};

	struct sqlite_wal_recovery_effect_receipt
	{
		std::uint64_t denied_main_write_count{};
		std::uint64_t denied_main_truncate_count{};
		std::uint64_t denied_wal_write_count{};
		std::uint64_t denied_wal_truncate_count{};
		std::uint64_t denied_rollback_journal_open_count{};
		std::uint64_t denied_delete_count{};
		std::uint64_t denied_other_open_count{};
		std::uint64_t main_sync_request_count{};
		std::uint64_t wal_sync_request_count{};
		bool only_private_shm_mutation_permitted{true};
	};

	struct sqlite_wal_recovery_workspace_receipt
	{
		std::string profile;
		sqlite_backend_opaque_identity source_capability_token;
		sqlite_backend_copy_receipt main_database;
		sqlite_backend_copy_receipt authoritative_wal_prefix;
		sqlite_wal_scan_receipt source_wal_scan;
		sqlite_wal_recovery_open_receipt opens;
		sqlite_wal_recovery_shm_receipt shm;
		sqlite_wal_recovery_effect_receipt effects;
		bool private_wal_present{};
		bool sealed{};
	};

	class sqlite_wal_recovery_workspace;

	class sqlite_wal_recovery_workspace_builder
	{
	  public:
		virtual ~sqlite_wal_recovery_workspace_builder() = default;

		[[nodiscard]] virtual result<void> append(sqlite_wal_recovery_copy_role role,
												  std::span<const std::byte> bytes) = 0;
		[[nodiscard]] virtual result<std::shared_ptr<sqlite_wal_recovery_workspace>>
		seal(sqlite_wal_recovery_workspace_expectation expectation) = 0;
	};

	/**
	 * Process-private main/WAL namespace. Main and WAL bytes are sealed and immutable; only the
	 * in-process SHM implementation can mutate. The holder must outlive its SQLite connection.
	 */
	class sqlite_wal_recovery_workspace
	{
	  public:
		virtual ~sqlite_wal_recovery_workspace() = default;

		[[nodiscard]] virtual std::string_view database_path() const noexcept = 0;
		[[nodiscard]] virtual std::string_view registered_vfs_name() const noexcept = 0;
		[[nodiscard]] virtual const void* vfs_implementation_identity() const noexcept = 0;
		[[nodiscard]] virtual result<sqlite_wal_recovery_workspace_receipt>
		snapshot_receipt() const = 0;
		[[nodiscard]] virtual result<void> verify_sealed_objects() const = 0;
	};

#if defined(__GNUC__) || defined(__clang__)
	[[nodiscard]] __attribute__((visibility("default")))
#else
	[[nodiscard]]
#endif
	result<std::shared_ptr<sqlite_wal_recovery_workspace_builder>>
	make_sqlite_wal_recovery_workspace_builder(
		sqlite_backend_opaque_identity source_capability_token,
		sqlite_private_snapshot_registry_binding registry);
} // namespace cxxlens::sdk
