#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

#include <cxxlens/sdk/store.hpp>

#include "sqlite_backend_observation_internal.hpp"

namespace cxxlens::sdk
{
	/**
	 * One already-pinned SQLite runtime image. `native_library_handle` is used only as the dlsym
	 * authority; this bridge never acquires or releases an additional loader reference.
	 */
	struct sqlite_backend_runtime_binding
	{
		void* native_library_handle{};
		const void* runtime_identity{};
		std::shared_ptr<void> runtime_lifetime;
	};

	/** Source-private bridge for a backend whose VFS must outlive its SQLite connection. */
#if defined(__GNUC__) || defined(__clang__)
	struct __attribute__((visibility("default"))) snapshot_store_backend_lifetime_access
#else
	struct snapshot_store_backend_lifetime_access
#endif
	{
		/** Test-only mutation entry points kept behind the source-private friend. */
		[[nodiscard]] static result<void> mark_publication_corrupt(snapshot_store&,
																   std::string_view);
		[[nodiscard]] static result<void> rewrite_publication_payload(
			snapshot_store&, std::string_view, std::string_view, std::string_view, std::size_t);
		[[nodiscard]] static result<void>
		rewrite_publication_identity_field(snapshot_store&, std::string_view, std::string_view);
		[[nodiscard]] static result<std::string> rewrite_snapshot_version(
			snapshot_store&, std::string_view, std::string_view, std::uint64_t, std::uint32_t);
		[[nodiscard]] static result<std::string> rewrite_publication_counters(snapshot_store&,
																			  std::string_view,
																			  std::uint64_t,
																			  std::uint64_t);
		[[nodiscard]] static result<void>
		poison_rejected_generation(snapshot_store&, std::string_view, std::uint64_t);

		[[nodiscard]] static result<snapshot_store>
		open_sqlite(const std::string& database_path,
					relation_engine engine,
					const std::string& vfs_name,
					sqlite_backend_runtime_binding runtime,
					std::shared_ptr<void> backend_lifetime,
					std::shared_ptr<sqlite_backend_observation_capability> observation);
	};
} // namespace cxxlens::sdk
