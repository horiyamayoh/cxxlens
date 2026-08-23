#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * Immutable identity tuple used by the Wave 3 lifecycle helpers.
	 *
	 * The tuple is deliberately made from the same opaque backend identities used by the
	 * authenticated SHM implementation.  A pointer, pathname, PID, or one open epoch alone is
	 * never sufficient.  This helper is source-private; production callers bind it from their
	 * already sealed runtime/VFS/filesystem observations.
	 */
	struct sqlite_wave3_identity_binding
	{
		sqlite_backend_opaque_identity process_instance;
		sqlite_backend_opaque_identity shared_runtime_vfs_cohort;
		sqlite_backend_opaque_identity exact_file_family;
		sqlite_backend_opaque_identity runtime_lifetime_pin;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity namespace_epoch;
		sqlite_backend_opaque_identity parent_namespace;
		sqlite_backend_opaque_identity shm_object;
		sqlite_backend_opaque_identity shm_entry;
		sqlite_backend_opaque_identity filesystem;
		sqlite_backend_opaque_identity mount;
		std::uint64_t fork_generation{};
		std::uint64_t mapping_generation{};
		std::uint64_t page_number{};
		std::uint64_t page_size{};

		[[nodiscard]] bool operator==(const sqlite_wave3_identity_binding&) const = default;
	};

	/** Closed reason set for identity validation; callers must branch on this enum, not prose. */
	enum class sqlite_wave3_identity_failure : std::uint8_t
	{
		none,
		missing_identity,
		identity_too_large,
		invalid_epoch,
		invalid_page,
		continuity_mismatch,
	};

	/** Return the typed validation result without mutating the binding. */
	[[nodiscard]] sqlite_wave3_identity_failure
	inspect_sqlite_wave3_identity(const sqlite_wave3_identity_binding& binding) noexcept;

	/** Validate all authority components and bounded numeric fields. */
	[[nodiscard]] result<void>
	validate_sqlite_wave3_identity(const sqlite_wave3_identity_binding& binding);

	/** Validate exact continuity between a previously sealed and observed tuple. */
	[[nodiscard]] result<void> validate_sqlite_wave3_identity_continuity(
		const sqlite_wave3_identity_binding& expected,
		const sqlite_wave3_identity_binding& observed);

	/** Monotonic lifecycle phase for one map/open epoch. */
	enum class sqlite_wave3_epoch_phase : std::uint8_t
	{
		unresolved,
		armed,
		native_started,
		sealed,
		revoked,
		quarantined,
	};

	/**
	 * Small state machine for identity-bound admission and terminal custody.
	 *
	 * The controller performs no native or filesystem operation.  It only makes the ordering and
	 * continuity barrier explicit so a caller cannot promote a stale/ABA/forked observation.
	 */
	class sqlite_wave3_epoch_controller final
	{
	  public:
		sqlite_wave3_epoch_controller() = default;
		~sqlite_wave3_epoch_controller() = default;
		sqlite_wave3_epoch_controller(sqlite_wave3_epoch_controller&&) noexcept = default;
		sqlite_wave3_epoch_controller& operator=(sqlite_wave3_epoch_controller&&) noexcept = default;
		sqlite_wave3_epoch_controller(const sqlite_wave3_epoch_controller&) = delete;
		sqlite_wave3_epoch_controller& operator=(const sqlite_wave3_epoch_controller&) = delete;

		[[nodiscard]] sqlite_wave3_epoch_phase phase() const noexcept;
		[[nodiscard]] bool admission_allowed() const noexcept;
		[[nodiscard]] bool terminal() const noexcept;
		[[nodiscard]] const std::optional<sqlite_wave3_identity_binding>& binding() const noexcept;
		[[nodiscard]] std::string_view quarantine_detail() const noexcept;

		[[nodiscard]] result<void>
		arm(sqlite_wave3_identity_binding binding);
		[[nodiscard]] result<void>
		start_native(const sqlite_wave3_identity_binding& observed);
		[[nodiscard]] result<void>
		seal(const sqlite_wave3_identity_binding& observed);
		[[nodiscard]] result<void> revoke();
		[[nodiscard]] result<void> quarantine(std::string detail);

	  private:
		sqlite_wave3_epoch_phase phase_{sqlite_wave3_epoch_phase::unresolved};
		std::optional<sqlite_wave3_identity_binding> binding_;
		std::string quarantine_detail_;
	};
} // namespace cxxlens::sdk
