#pragma once

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sqlite_wave3_identity_epoch_internal.hpp"

namespace cxxlens::sdk
{
	/** Closed phase set for one authenticated writer/reader SHM lease. */
	enum class sqlite_wave3_lease_phase : std::uint8_t
	{
		unresolved,
		reserved,
		native_started,
		pending,
		promoted,
		revoked,
		draining,
		retired,
		quarantined,
	};

	/** Typed lease failure; no caller should infer authority from diagnostic text. */
	enum class sqlite_wave3_lease_failure : std::uint8_t
	{
		invalid_identity,
		invalid_transition,
		continuity_mismatch,
		duplicate_binding,
		registry_closed,
		registry_quarantined,
		registry_capacity,
		stale_token,
		live_use_owners,
		use_bound_exceeded,
		missing_quarantine_detail,
		generation_exhausted,
	};

	struct sqlite_wave3_lease_error
	{
		sqlite_wave3_lease_failure reason{sqlite_wave3_lease_failure::invalid_transition};
	};

	/**
	 * Move-only lease state machine.  The object never performs a native callback; it only gates
	 * the exact identity and close/revoke ordering around one callback-owned generation.
	 */
	class sqlite_wave3_shm_lease final
	{
	  public:
		~sqlite_wave3_shm_lease() noexcept = default;
		sqlite_wave3_shm_lease(sqlite_wave3_shm_lease&& other) noexcept;
		sqlite_wave3_shm_lease& operator=(sqlite_wave3_shm_lease&&) = delete;
		sqlite_wave3_shm_lease(const sqlite_wave3_shm_lease&) = delete;
		sqlite_wave3_shm_lease& operator=(const sqlite_wave3_shm_lease&) = delete;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] std::uint64_t token() const noexcept;
		[[nodiscard]] sqlite_wave3_lease_phase phase() const noexcept;
		[[nodiscard]] const sqlite_wave3_identity_binding& binding() const noexcept;
		[[nodiscard]] std::size_t active_use_owners() const noexcept;
		[[nodiscard]] std::string_view quarantine_detail() const noexcept;

		[[nodiscard]] result<void> begin_native(const sqlite_wave3_identity_binding& observed);
		[[nodiscard]] result<void> record_pending(const sqlite_wave3_identity_binding& observed);
		[[nodiscard]] result<void> promote(const sqlite_wave3_identity_binding& observed);
		[[nodiscard]] result<void> admit_use();
		[[nodiscard]] result<void> release_use();
		[[nodiscard]] result<void> revoke();
		[[nodiscard]] result<void> begin_drain();
		[[nodiscard]] result<void> retire();
		[[nodiscard]] result<void> quarantine(std::string detail);

	  private:
		friend class sqlite_wave3_shm_lease_registry;

		sqlite_wave3_shm_lease(sqlite_wave3_identity_binding binding, std::uint64_t token);
		[[nodiscard]] result<void>
		check_observed(const sqlite_wave3_identity_binding& observed) const;

		sqlite_wave3_identity_binding binding_;
		std::uint64_t token_{};
		sqlite_wave3_lease_phase phase_{sqlite_wave3_lease_phase::unresolved};
		std::size_t active_use_owners_{};
		std::string quarantine_detail_;
	};

	/**
	 * Bounded process-local reservation registry.  It retains terminal tombstones so an exact
	 * generation cannot be resurrected or reused after retirement/quarantine.
	 */
	class sqlite_wave3_shm_lease_registry final
	{
	  public:
		static constexpr std::size_t capacity = 256U;

		sqlite_wave3_shm_lease_registry() = default;
		~sqlite_wave3_shm_lease_registry() = default;
		sqlite_wave3_shm_lease_registry(const sqlite_wave3_shm_lease_registry&) = delete;
		sqlite_wave3_shm_lease_registry& operator=(const sqlite_wave3_shm_lease_registry&) = delete;

		[[nodiscard]] result<sqlite_wave3_shm_lease> reserve(sqlite_wave3_identity_binding binding);
		[[nodiscard]] result<void> retire(sqlite_wave3_shm_lease& lease);
		[[nodiscard]] result<void> close_admission();
		[[nodiscard]] result<void> quarantine();
		[[nodiscard]] bool admission_allowed() const noexcept;
		[[nodiscard]] bool quarantined() const noexcept;
		[[nodiscard]] bool contains(const sqlite_wave3_identity_binding& binding) const;
		[[nodiscard]] std::size_t size() const noexcept;

	  private:
		struct entry
		{
			sqlite_wave3_identity_binding binding;
			std::uint64_t token{};
			bool terminal{};
		};

		mutable std::mutex mutex_;
		std::vector<entry> entries_;
		std::uint64_t next_token_{1};
		bool admission_allowed_{true};
		bool quarantined_{};
	};
} // namespace cxxlens::sdk
