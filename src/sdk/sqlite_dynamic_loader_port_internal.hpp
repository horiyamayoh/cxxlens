#pragma once

#include <memory>
#include <string_view>

#include <cxxlens/sdk/store.hpp>

namespace cxxlens::sdk::detail
{
	/** Source-private authority for loading SQLite and resolving symbols from that exact image. */
	class sqlite_dynamic_loader_port
	{
	  public:
		virtual ~sqlite_dynamic_loader_port() = default;
		[[nodiscard]] virtual result<void*> open(std::string_view candidate) const = 0;
		virtual void close(void* library) const noexcept = 0;
		[[nodiscard]] virtual result<void*> resolve(void* library,
													std::string_view symbol) const = 0;
		[[nodiscard]] virtual const void* image_identity(const void* symbol) const noexcept = 0;
	};

	[[nodiscard]] std::shared_ptr<const sqlite_dynamic_loader_port>
	make_system_sqlite_dynamic_loader_port();

	/** Testable internal entry point; the accepted public Store surface remains unchanged. */
	[[nodiscard]] result<snapshot_store> open_sqlite_snapshot_store_with_loader_port(
		const std::string& database_path,
		relation_engine engine,
		std::shared_ptr<const sqlite_dynamic_loader_port> loader);
} // namespace cxxlens::sdk::detail
