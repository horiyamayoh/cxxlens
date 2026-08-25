// Test-only SQLite API interposer.
//
// Store deliberately resolves SQLite functions from the handle returned by
// dlopen().  A plain sqlite3_exec symbol in LD_PRELOAD would therefore not
// replace that function pointer.  This DSO wraps dlsym itself: once the child
// explicitly arms the controller, the SQLite function pointers returned by
// the real dlsym are replaced by wrappers which retain and call those real
// pointers.  No production dispatcher, test seam, or TLS is involved.

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>

#include <dlfcn.h>

namespace
{
	constexpr int sqlite_ok = 0;
	constexpr int sqlite_full = 13;
	constexpr int sqlite_ioerr = 10;

	enum class fault_mode : std::uint8_t
	{
		none,
		observe_only,
		pre_effect_first_migration_ddl,
		commit_after_delegate,
		close_after_commit,
	};

	using dlsym_function = void* (*)(void*, const char*);
	using sqlite3_exec_function =
		int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
	using sqlite3_close_v2_function = int (*)(void*);
	using sqlite3_errmsg_function = const char* (*)(void*);
	using sqlite3_db_filename_function = const char* (*)(void*, const char*);

	struct controller
	{
		std::mutex mutex;
		fault_mode mode{fault_mode::none};
		bool armed{};
		bool fired{};
		bool commit_seen{};
		std::string database_path;
		void* sqlite_library_handle{};
		void* target_database{};
		void* forced_error_database{};
		std::uint64_t migration_ddl_calls{};
		std::uint64_t migration_ddl_delegate_calls{};
		std::uint64_t commit_calls{};
		std::uint64_t commit_delegate_calls{};
		std::uint64_t close_calls{};
		std::uint64_t close_delegate_calls{};
		std::uint64_t close_delegate_ok_calls{};
		sqlite3_exec_function real_exec{};
		sqlite3_close_v2_function real_close_v2{};
		sqlite3_errmsg_function real_errmsg{};
		sqlite3_db_filename_function real_db_filename{};
	};

	controller& state() noexcept
	{
		static controller value;
		return value;
	}

	dlsym_function real_dlsym() noexcept
	{
		// glibc exports dlsym with both historical and current symbol versions.
		// dlvsym is not interposed by this file, so it is a bootstrap path for
		// the real dlsym and cannot recurse through our wrapper.
		static auto function =
			reinterpret_cast<dlsym_function>(::dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.34"));
		if (function == nullptr)
			function =
				reinterpret_cast<dlsym_function>(::dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
		return function;
	}

	bool exact_sql(const char* value, const std::string_view expected) noexcept
	{
		return value != nullptr && std::strlen(value) == expected.size() &&
			std::memcmp(value, expected.data(), expected.size()) == 0;
	}

	bool is_commit(const char* sql) noexcept
	{
		return sql != nullptr && std::strcmp(sql, "COMMIT;") == 0;
	}

	constexpr std::string_view migration_metadata_ddl =
		"CREATE TABLE cxxlens_ng_migration_metadata(key TEXT NOT NULL PRIMARY KEY,value TEXT NOT "
		"NULL) STRICT, WITHOUT ROWID;";

	bool is_target_database(controller& value, void* database) noexcept
	{
		if (value.real_db_filename == nullptr || value.database_path.empty())
			return false;
		const auto* filename = value.real_db_filename(database, "main");
		return filename != nullptr && std::strlen(filename) == value.database_path.size() &&
			std::memcmp(filename, value.database_path.data(), value.database_path.size()) == 0;
	}

	int wrapped_sqlite3_exec(void* database,
							 const char* sql,
							 int (*callback)(void*, int, char**, char**),
							 void* context,
							 char** error_message) noexcept
	{
		auto& value = state();
		sqlite3_exec_function real{};
		fault_mode mode{};
		{
			std::scoped_lock lock{value.mutex};
			real = value.real_exec;
			mode = value.mode;
		}
		if (real == nullptr)
			return sqlite_ioerr;

		const auto migration_ddl = exact_sql(sql, migration_metadata_ddl);
		const auto commit = is_commit(sql);
		if (migration_ddl)
		{
			std::scoped_lock lock{value.mutex};
			if (value.armed && is_target_database(value, database) &&
				(value.target_database == nullptr || value.target_database == database))
			{
				value.target_database = database;
				++value.migration_ddl_calls;
				if (mode == fault_mode::pre_effect_first_migration_ddl && !value.fired)
				{
					// The delegate is deliberately not called.  sqlite3_errmsg is
					// wrapped below so Store receives a deterministic SQLite failure
					// tuple rather than an arbitrary previous connection message.
					value.fired = true;
					value.forced_error_database = database;
					return sqlite_full;
				}
			}
		}

		const auto result = real(database, sql, callback, context, error_message);
		std::scoped_lock lock{value.mutex};
		if (!value.armed || value.target_database != database)
			return result;
		if (migration_ddl)
			++value.migration_ddl_delegate_calls;
		if (!commit)
			return result;
		++value.commit_calls;
		++value.commit_delegate_calls;
		if (result != sqlite_ok)
			return result;
		if (mode == fault_mode::commit_after_delegate && !value.fired)
		{
			value.commit_seen = true;
			value.fired = true;
			return sqlite_ioerr;
		}
		if (mode == fault_mode::close_after_commit)
			value.commit_seen = true;
		return result;
	}

	int wrapped_sqlite3_close_v2(void* database) noexcept
	{
		auto& value = state();
		sqlite3_close_v2_function real{};
		bool inject{};
		{
			std::scoped_lock lock{value.mutex};
			real = value.real_close_v2;
			inject = value.armed && value.target_database == database &&
				value.mode == fault_mode::close_after_commit && value.commit_seen && !value.fired;
			if (value.armed && value.target_database == database)
				++value.close_calls;
		}
		if (real == nullptr)
			return sqlite_ioerr;
		const auto result = real(database);
		{
			std::scoped_lock lock{value.mutex};
			if (value.target_database == database)
			{
				++value.close_delegate_calls;
				if (result == sqlite_ok)
					++value.close_delegate_ok_calls;
				if (inject && result == sqlite_ok)
					value.fired = true;
				value.target_database = nullptr;
			}
		}
		// Return a non-OK result only after the real close callback has run.  The
		// production lifecycle therefore has to quarantine the callback and pins.
		return inject && result == sqlite_ok ? sqlite_ioerr : result;
	}

	const char* wrapped_sqlite3_errmsg(void* database) noexcept
	{
		auto& value = state();
		sqlite3_errmsg_function real{};
		bool forced{};
		{
			std::scoped_lock lock{value.mutex};
			real = value.real_errmsg;
			forced = value.forced_error_database == database;
			if (forced)
				value.forced_error_database = nullptr;
		}
		if (forced)
			return "database or disk is full";
		return real == nullptr ? "sqlite interposer" : real(database);
	}

	void* substitute_sqlite_symbol(void* handle, const char* name, void* resolved) noexcept
	{
		if (name == nullptr)
			return resolved;
		auto& value = state();
		std::scoped_lock lock{value.mutex};
		if (value.real_db_filename == nullptr)
		{
			value.real_db_filename = reinterpret_cast<sqlite3_db_filename_function>(
				real_dlsym()(handle, "sqlite3_db_filename"));
			if (value.real_db_filename != nullptr)
				value.sqlite_library_handle = handle;
		}
		if (handle != value.sqlite_library_handle)
			return resolved;
		if (std::strcmp(name, "sqlite3_exec") == 0)
		{
			value.real_exec = reinterpret_cast<sqlite3_exec_function>(resolved);
			return value.armed ? reinterpret_cast<void*>(&wrapped_sqlite3_exec) : resolved;
		}
		if (std::strcmp(name, "sqlite3_close_v2") == 0)
		{
			value.real_close_v2 = reinterpret_cast<sqlite3_close_v2_function>(resolved);
			return value.armed ? reinterpret_cast<void*>(&wrapped_sqlite3_close_v2) : resolved;
		}
		if (std::strcmp(name, "sqlite3_errmsg") == 0)
		{
			value.real_errmsg = reinterpret_cast<sqlite3_errmsg_function>(resolved);
			return value.armed ? reinterpret_cast<void*>(&wrapped_sqlite3_errmsg) : resolved;
		}
		return resolved;
	}
} // namespace

extern "C" __attribute__((visibility("default"))) void* dlsym(void* handle,
															  const char* name) noexcept
{
	const auto real = real_dlsym();
	if (real == nullptr)
		return nullptr;
	const auto resolved = real(handle, name);
	return substitute_sqlite_symbol(handle, name, resolved);
}

extern "C" __attribute__((visibility("default"))) int
cxxlens_sqlite_migration_interposer_arm(const char* database_path,
										const char* requested_mode) noexcept
{
	if (database_path == nullptr || *database_path == '\0' || requested_mode == nullptr)
		return -1;
	fault_mode mode = fault_mode::none;
	if (std::strcmp(requested_mode, "observe-only") == 0)
		mode = fault_mode::observe_only;
	else if (std::strcmp(requested_mode, "pre-effect-first-migration-ddl") == 0)
		mode = fault_mode::pre_effect_first_migration_ddl;
	else if (std::strcmp(requested_mode, "commit-after-delegate") == 0)
		mode = fault_mode::commit_after_delegate;
	else if (std::strcmp(requested_mode, "close-after-commit") == 0)
		mode = fault_mode::close_after_commit;
	else
		return -1;
	try
	{
		auto& value = state();
		std::scoped_lock lock{value.mutex};
		value.mode = mode;
		value.armed = true;
		value.fired = false;
		value.commit_seen = false;
		value.database_path = database_path;
		value.target_database = nullptr;
		value.forced_error_database = nullptr;
		value.migration_ddl_calls = 0U;
		value.migration_ddl_delegate_calls = 0U;
		value.commit_calls = 0U;
		value.commit_delegate_calls = 0U;
		value.close_calls = 0U;
		value.close_delegate_calls = 0U;
		value.close_delegate_ok_calls = 0U;
		return 0;
	}
	catch (...)
	{
		return -1;
	}
}

extern "C" __attribute__((visibility("default"))) int
cxxlens_sqlite_migration_interposer_fired() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return value.fired ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) int
cxxlens_sqlite_migration_interposer_commit_delegate_seen() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return value.commit_seen ? 1 : 0;
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_migration_ddl_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.migration_ddl_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_migration_ddl_delegate_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.migration_ddl_delegate_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_commit_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.commit_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_commit_delegate_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.commit_delegate_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_close_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.close_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_close_delegate_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.close_delegate_calls);
}

extern "C" __attribute__((visibility("default"))) unsigned long long
cxxlens_sqlite_migration_interposer_close_delegate_ok_calls() noexcept
{
	auto& value = state();
	std::scoped_lock lock{value.mutex};
	return static_cast<unsigned long long>(value.close_delegate_ok_calls);
}
