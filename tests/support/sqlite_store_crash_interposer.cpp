#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdlib>
#include <cstring>

#include <dlfcn.h>
#include <unistd.h>

namespace
{
	using dlsym_function = void* (*)(void*, const char*);
	using prepare_v3_function =
		int (*)(void*, const char*, int, unsigned int, void**, const char**);
	using bind_text_function = int (*)(void*, int, const char*, int, void (*)(void*));
	using step_function = int (*)(void*);
	using statement_lifecycle_function = int (*)(void*);

	prepare_v3_function prepare_v3_delegate{};
	bind_text_function bind_text_delegate{};
	step_function step_delegate{};
	statement_lifecycle_function finalize_delegate{};
	statement_lifecycle_function reset_delegate{};
	void* prepared_marker_statement{};
	void* physical_format_statement{};

	constexpr auto marker_sql =
		std::to_array("INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?1,?2)");
	constexpr auto marker_key = std::to_array("physical_format");

	[[nodiscard]] dlsym_function next_dlsym() noexcept
	{
#if defined(__linux__)
		static const auto function =
			reinterpret_cast<dlsym_function>(::dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.2.5"));
		return function;
#else
		return nullptr;
#endif
	}

	[[nodiscard]] bool crash_pause_enabled() noexcept
	{
		const auto* value = std::getenv("CXXLENS_TEST_SQLITE_CRASH_PAUSE");
		return value != nullptr && std::strcmp(value, "1") == 0;
	}

	[[nodiscard]] bool exact_text(const char* value,
								  const int byte_count,
								  const char* expected,
								  const std::size_t expected_size) noexcept
	{
		if (value == nullptr)
			return false;
		const auto size =
			byte_count < 0 ? std::strlen(value) : static_cast<std::size_t>(byte_count);
		return size == expected_size && std::memcmp(value, expected, expected_size) == 0;
	}

	[[noreturn]] void announce_and_pause() noexcept
	{
		const auto* descriptor_text = std::getenv("CXXLENS_TEST_SQLITE_CRASH_READY_FD");
		if (descriptor_text == nullptr)
			std::_Exit(124);
		char* end{};
		errno = 0;
		const auto parsed = std::strtol(descriptor_text, &end, 10);
		if (errno != 0 || end == descriptor_text || *end != '\0' || parsed < 0)
			std::_Exit(124);
		const char ready{'R'};
		if (::write(static_cast<int>(parsed), &ready, sizeof(ready)) != sizeof(ready))
			std::_Exit(124);
		for (;;)
			(void)::pause();
	}

	int sqlite_crash_prepare_v3(void* database,
								const char* sql,
								const int byte_count,
								const unsigned int flags,
								void** statement,
								const char** tail) noexcept
	{
		if (prepare_v3_delegate == nullptr)
			return 21; // SQLITE_MISUSE
		const auto result = prepare_v3_delegate(database, sql, byte_count, flags, statement, tail);
		if (result == 0 && crash_pause_enabled() && statement != nullptr && *statement != nullptr &&
			exact_text(sql, byte_count, marker_sql.data(), marker_sql.size() - 1U))
			prepared_marker_statement = *statement;
		return result;
	}

	int sqlite_crash_bind_text(void* statement,
							   const int index,
							   const char* value,
							   const int byte_count,
							   void (*destructor)(void*)) noexcept
	{
		if (bind_text_delegate == nullptr)
			return 21; // SQLITE_MISUSE
		const auto result = bind_text_delegate(statement, index, value, byte_count, destructor);
		if (result == 0 && crash_pause_enabled() && statement == prepared_marker_statement &&
			index == 1 && exact_text(value, byte_count, marker_key.data(), marker_key.size() - 1U))
			physical_format_statement = statement;
		return result;
	}

	int sqlite_crash_step(void* statement) noexcept
	{
		if (step_delegate == nullptr)
			return 21; // SQLITE_MISUSE
		const auto result = step_delegate(statement);
		constexpr int sqlite_done = 101;
		if (result == sqlite_done && crash_pause_enabled() &&
			physical_format_statement == statement)
		{
			physical_format_statement = nullptr;
			prepared_marker_statement = nullptr;
			announce_and_pause();
		}
		return result;
	}

	int sqlite_crash_finalize(void* statement) noexcept
	{
		if (finalize_delegate == nullptr)
			return 21; // SQLITE_MISUSE
		if (statement == prepared_marker_statement)
			prepared_marker_statement = nullptr;
		if (statement == physical_format_statement)
			physical_format_statement = nullptr;
		return finalize_delegate(statement);
	}

	int sqlite_crash_reset(void* statement) noexcept
	{
		if (reset_delegate == nullptr)
			return 21; // SQLITE_MISUSE
		if (statement == prepared_marker_statement)
			prepared_marker_statement = nullptr;
		if (statement == physical_format_statement)
			physical_format_statement = nullptr;
		return reset_delegate(statement);
	}
} // namespace

// Interposing the system declaration keeps descriptive local parameter names.
extern "C" __attribute__((visibility("default"))) void*
dlsym(			  // NOLINT(readability-inconsistent-declaration-parameter-name)
	void* handle, // NOLINT(readability-inconsistent-declaration-parameter-name)
	const char* symbol) noexcept
{
	const auto delegate = next_dlsym();
	if (delegate == nullptr)
		return nullptr;
	void* const resolved = delegate(handle, symbol);
	if (!crash_pause_enabled())
		return resolved;
	if (std::strcmp(symbol, "sqlite3_prepare_v3") == 0)
	{
		prepare_v3_delegate = reinterpret_cast<prepare_v3_function>(resolved);
		return reinterpret_cast<void*>(&sqlite_crash_prepare_v3);
	}
	if (std::strcmp(symbol, "sqlite3_bind_text") == 0)
	{
		bind_text_delegate = reinterpret_cast<bind_text_function>(resolved);
		return reinterpret_cast<void*>(&sqlite_crash_bind_text);
	}
	if (std::strcmp(symbol, "sqlite3_step") == 0)
	{
		step_delegate = reinterpret_cast<step_function>(resolved);
		return reinterpret_cast<void*>(&sqlite_crash_step);
	}
	if (std::strcmp(symbol, "sqlite3_finalize") == 0)
	{
		finalize_delegate = reinterpret_cast<statement_lifecycle_function>(resolved);
		return reinterpret_cast<void*>(&sqlite_crash_finalize);
	}
	if (std::strcmp(symbol, "sqlite3_reset") == 0)
	{
		reset_delegate = reinterpret_cast<statement_lifecycle_function>(resolved);
		return reinterpret_cast<void*>(&sqlite_crash_reset);
	}
	return resolved;
}
