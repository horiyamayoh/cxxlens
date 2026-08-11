#pragma once

#include <memory>

#include "sqlite_same_process_shm_mapping_registry_internal.hpp"

namespace cxxlens::sdk
{
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
