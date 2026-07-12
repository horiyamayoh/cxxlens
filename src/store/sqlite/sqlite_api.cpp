#include "sqlite_api.hpp"

#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

namespace cxxlens::detail::store::sqlite
{
	namespace
	{
		[[nodiscard]] error api_error(std::string reason)
		{
			error failure;
			failure.code.value = "facts.backend-unavailable";
			failure.message = "SQLite runtime is unavailable";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("backend", "sqlite");
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		template <typename Function>
		[[nodiscard]] result<Function> resolve(const runtime::dynamic_library& library,
											   const std::string_view name)
		{
			static_assert(std::is_pointer_v<Function>);
			auto symbol = library.symbol(name);
			if (!symbol)
				return api_error("missing-symbol:" + std::string{name});
			Function function{};
			static_assert(sizeof(function) == sizeof(symbol.value()));
			// POSIX dlsym returns an object pointer for a function address.
			// NOLINTBEGIN(bugprone-multi-level-implicit-pointer-conversion)
			std::memcpy(static_cast<void*>(&function),
						static_cast<const void*>(&symbol.value()),
						sizeof(function));
			// NOLINTEND(bugprone-multi-level-implicit-pointer-conversion)
			return function;
		}

		template <typename Function>
		[[nodiscard]] bool assign(Function& target,
								  const runtime::dynamic_library& library,
								  const std::string_view name)
		{
			auto resolved = resolve<Function>(library, name);
			if (!resolved)
				return false;
			target = resolved.value();
			return true;
		}
	} // namespace

	result<std::shared_ptr<const api>> load_api()
	{
		runtime::system_dynamic_library_adapter loader;
		runtime::request_context context;
		context.operation = "store.sqlite.load-runtime";
#if defined(__APPLE__)
		const std::vector<std::string> candidates{"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
#elif defined(__unix__)
		const std::vector<std::string> candidates{"libsqlite3.so.0", "libsqlite3.so"};
#else
		const std::vector<std::string> candidates;
#endif
		auto loaded = loader.open(candidates, context);
		if (!loaded)
			return api_error("runtime-library-not-found");
		auto value = std::make_shared<api>();
		value->library = loaded.value();
		const auto& library = *value->library;
		if (!assign(value->open_v2, library, "sqlite3_open_v2") ||
			!assign(value->close_v2, library, "sqlite3_close_v2") ||
			!assign(value->errmsg, library, "sqlite3_errmsg") ||
			!assign(value->exec, library, "sqlite3_exec") ||
			!assign(value->free_memory, library, "sqlite3_free") ||
			!assign(value->prepare_v2, library, "sqlite3_prepare_v2") ||
			!assign(value->step, library, "sqlite3_step") ||
			!assign(value->finalize, library, "sqlite3_finalize") ||
			!assign(value->bind_text, library, "sqlite3_bind_text") ||
			!assign(value->bind_int64, library, "sqlite3_bind_int64") ||
			!assign(value->bind_blob64, library, "sqlite3_bind_blob64") ||
			!assign(value->column_text, library, "sqlite3_column_text") ||
			!assign(value->column_int64, library, "sqlite3_column_int64") ||
			!assign(value->column_blob, library, "sqlite3_column_blob") ||
			!assign(value->column_bytes, library, "sqlite3_column_bytes") ||
			!assign(value->busy_timeout, library, "sqlite3_busy_timeout"))
			return api_error("runtime-api-incomplete");
		return std::shared_ptr<const api>{std::move(value)};
	}
} // namespace cxxlens::detail::store::sqlite
