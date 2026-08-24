#pragma once

#include <cstdint>
#include <memory>
#include <optional>

#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * Exact process coordinates captured at one process-port boundary.
	 *
	 * PID is only one coordinate. A retained PID namespace identity, process start ticks, pidfd
	 * identity, liveness observation, and the registered fork epoch must all agree before a
	 * process-global SHM registry can be used. A false `pidfd_live` is deliberately an
	 * unauthenticated/dead observation; callers must fail closed.
	 */
	struct sqlite_shm_process_identity_observation
	{
		std::uint64_t pid{};
		std::uint64_t process_start_ticks{};
		std::uint64_t fork_epoch{};
		std::uint64_t pid_namespace_device{};
		std::uint64_t pid_namespace_inode{};
		std::uint64_t pidfd_device{};
		std::uint64_t pidfd_inode{};
		bool pidfd_live{};

		[[nodiscard]] bool
		operator==(const sqlite_shm_process_identity_observation&) const = default;
	};

	enum class sqlite_shm_process_identity_rejection_reason : std::uint8_t
	{
		invalid_identity,
		pid_mismatch,
		process_start_mismatch,
		pid_namespace_mismatch,
		pidfd_mismatch,
		pidfd_not_live,
		fork_epoch_mismatch,
	};

	struct sqlite_shm_process_identity_rejection
	{
		sqlite_shm_process_identity_rejection_reason reason{
			sqlite_shm_process_identity_rejection_reason::invalid_identity};
	};

	/** Result of one pure process identity comparison. */
	class sqlite_shm_process_identity_validation_result final
	{
	  public:
		constexpr sqlite_shm_process_identity_validation_result() noexcept = default;
		constexpr explicit sqlite_shm_process_identity_validation_result(
			sqlite_shm_process_identity_rejection rejection) noexcept
			: rejection_{rejection}
		{
		}

		[[nodiscard]] constexpr bool has_value() const noexcept
		{
			return !rejection_.has_value();
		}
		[[nodiscard]] constexpr explicit operator bool() const noexcept
		{
			return has_value();
		}
		[[nodiscard]] constexpr const sqlite_shm_process_identity_rejection& error() const noexcept
		{
			return rejection_.has_value() ? *rejection_ : empty_rejection_;
		}

	  private:
		inline static constexpr sqlite_shm_process_identity_rejection empty_rejection_{};
		std::optional<sqlite_shm_process_identity_rejection> rejection_;
	};

	/**
	 * Pure, fail-closed comparison used by the production process port and direct fault tests.
	 * Neither input is refreshed or mutated. The caller owns the lifecycle transition after a
	 * mismatch; the process port quarantines its registry rather than replacing an identity.
	 */
	[[nodiscard]] sqlite_shm_process_identity_validation_result
	validate_sqlite_shm_process_identity(
		const sqlite_shm_process_identity_observation& expected,
		const sqlite_shm_process_identity_observation& observed) noexcept;

	namespace detail
	{
		struct sqlite_shm_process_registry_port_state;
	} // namespace detail

	/**
	 * Copyable lifetime handle for the one qualified same-process SHM registry.
	 *
	 * The hidden process owner is minted exactly once and is never exposed. Copies only retain the
	 * already-created registry state; they cannot replay the process owner or create another
	 * registry. A handle inherited across fork is stale before it can be used.
	 */
	class sqlite_shm_process_registry_handle
	{
	  public:
		sqlite_shm_process_registry_handle() noexcept = default;
		~sqlite_shm_process_registry_handle() noexcept = default;
		sqlite_shm_process_registry_handle(const sqlite_shm_process_registry_handle&) noexcept =
			default;
		sqlite_shm_process_registry_handle&
		operator=(const sqlite_shm_process_registry_handle&) noexcept = default;
		sqlite_shm_process_registry_handle(sqlite_shm_process_registry_handle&&) noexcept = default;
		sqlite_shm_process_registry_handle&
		operator=(sqlite_shm_process_registry_handle&&) noexcept = default;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] const sqlite_backend_opaque_identity& process_instance() const noexcept;
		[[nodiscard]] sqlite_same_process_shm_mapping_registry* registry() const noexcept;

		/**
		 * Retains one alias-distinct SQLite runtime lifetime inside the process-global registry.
		 *
		 * The identity and pin identity must be independently issuer-sealed. This call does not
		 * register a VFS alias or mint a registration receipt.
		 */
		[[nodiscard]] sqlite_shm_lease_result<sqlite_shm_registry_runtime_lifetime_pin>
		adopt_runtime_lifetime(sqlite_backend_opaque_identity identity,
							   sqlite_backend_opaque_identity pin_identity,
							   std::shared_ptr<void> owner) const;

	  private:
		friend class sqlite_same_process_shm_process_port;

		explicit sqlite_shm_process_registry_handle(
			std::shared_ptr<detail::sqlite_shm_process_registry_port_state> state) noexcept;

		std::shared_ptr<detail::sqlite_shm_process_registry_port_state> state_;
	};

	/**
	 * Qualified Linux process port for the DF-0205 process-global registry.
	 *
	 * The port fail-closes unless it can bind a retained PID namespace descriptor, exact process
	 * start identity, a live self pidfd, a checked fork epoch, and fresh non-reusable entropy. The
	 * resulting process owner is consumed internally to create exactly one registry for the current
	 * process. No VFS callback, native mapping call, or native-OK projection occurs here.
	 */
	class sqlite_same_process_shm_process_port
	{
	  public:
		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_process_registry_handle> acquire();
	};
} // namespace cxxlens::sdk
