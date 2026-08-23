#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/** Source identity available before an outer active-WAL read opens SQLite. */
	struct sqlite_wave3_wal_source_identity
	{
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort;
		sqlite_backend_opaque_identity exact_file_family;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity namespace_epoch;
		std::uint64_t fork_generation{};

		[[nodiscard]] bool operator==(const sqlite_wave3_wal_source_identity&) const = default;
	};

	[[nodiscard]] result<void>
	validate_sqlite_wave3_wal_source_identity(const sqlite_wave3_wal_source_identity& identity);

	/** Physical WAL condition captured by the typed census. */
	enum class sqlite_wave3_wal_state : std::uint8_t
	{
		absent,
		size_zero,
		valid_nonzero,
		invalid,
	};

	/** Authenticated read route selected after native callback outcome. */
	enum class sqlite_wave3_wal_recovery_route : std::uint8_t
	{
		main_only,
		private_heap_index,
		native_readonly_mapping,
	};

	struct sqlite_wave3_wal_recovery_input
	{
		sqlite_wave3_wal_source_identity source;
		std::uint64_t main_size{};
		std::uint64_t wal_size{};
		std::string main_digest;
		std::string wal_digest;
		sqlite_wave3_wal_state wal_state{sqlite_wave3_wal_state::absent};
		std::int32_t read_lock_index{-1};
		bool native_readonly_cantinit{};
		bool native_readonly_mapping{};
		bool source_mutation_permitted{};
		bool private_shm_only{true};
		std::uint64_t authoritative_prefix_bytes{};
		std::size_t max_copy_bytes{64U * 1024U};
	};

	struct sqlite_wave3_wal_recovery_plan
	{
		sqlite_wave3_wal_source_identity source;
		sqlite_wave3_wal_recovery_route route{sqlite_wave3_wal_recovery_route::main_only};
		std::uint64_t main_size{};
		std::uint64_t wal_size{};
		std::uint64_t authoritative_prefix_bytes{};
		std::string main_digest;
		std::string wal_digest;
		bool source_zero_effect_required{true};
		bool eager_decode_required{true};
		bool close_before_receipt{true};
	};

	[[nodiscard]] result<sqlite_wave3_wal_recovery_plan>
	plan_sqlite_wave3_wal_recovery(const sqlite_wave3_wal_recovery_input& input);

	/** Deterministic bounded prefix chunk. */
	struct sqlite_wave3_wal_recovery_chunk
	{
		std::uint64_t offset{};
		std::uint64_t length{};
		[[nodiscard]] bool operator==(const sqlite_wave3_wal_recovery_chunk&) const = default;
	};

	[[nodiscard]] result<std::vector<sqlite_wave3_wal_recovery_chunk>>
	chunk_sqlite_wave3_wal_prefix(std::span<const std::byte> bytes, std::size_t chunk_bytes);

	/** Closed lifecycle around one immutable WAL/private-index recovery plan. */
	enum class sqlite_wave3_wal_recovery_phase : std::uint8_t
	{
		unresolved,
		planned,
		prefix_sealed,
		decoded_candidate,
		revoked,
		closed,
		quarantined,
	};

	class sqlite_wave3_wal_recovery_session final
	{
	  public:
		~sqlite_wave3_wal_recovery_session() noexcept = default;
		sqlite_wave3_wal_recovery_session(sqlite_wave3_wal_recovery_session&&) noexcept = default;
		sqlite_wave3_wal_recovery_session& operator=(sqlite_wave3_wal_recovery_session&&) noexcept =
			default;
		sqlite_wave3_wal_recovery_session(const sqlite_wave3_wal_recovery_session&) = delete;
		sqlite_wave3_wal_recovery_session& operator=(const sqlite_wave3_wal_recovery_session&) =
			delete;

		[[nodiscard]] static result<sqlite_wave3_wal_recovery_session>
		open(const sqlite_wave3_wal_recovery_input& input);
		[[nodiscard]] sqlite_wave3_wal_recovery_phase phase() const noexcept;
		[[nodiscard]] sqlite_wave3_wal_recovery_route route() const noexcept;
		[[nodiscard]] const sqlite_wave3_wal_recovery_plan& plan() const noexcept;
		[[nodiscard]] std::span<const std::byte> sealed_prefix() const noexcept;
		[[nodiscard]] std::string_view quarantine_detail() const noexcept;

		[[nodiscard]] result<void> seal_prefix(std::span<const std::byte> bytes);
		[[nodiscard]] result<void> mark_decoded_candidate();
		[[nodiscard]] result<void> revoke();
		[[nodiscard]] result<void> close();
		[[nodiscard]] result<void> quarantine(std::string detail);

  private:
		explicit sqlite_wave3_wal_recovery_session(sqlite_wave3_wal_recovery_plan plan);

		sqlite_wave3_wal_recovery_plan plan_;
		sqlite_wave3_wal_recovery_phase phase_{sqlite_wave3_wal_recovery_phase::unresolved};
		std::vector<std::byte> sealed_prefix_;
		std::string quarantine_detail_;
	};
} // namespace cxxlens::sdk
