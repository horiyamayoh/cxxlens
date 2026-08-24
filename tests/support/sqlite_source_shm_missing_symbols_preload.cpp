#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif

#include <cstring>

#include <dlfcn.h>

namespace
{
	using dlsym_function = void* (*)(void*, const char*);
	void* sqlite_handle{};

	[[nodiscard]] bool blocked_sqlite_source_shm_symbol(const char* name) noexcept
	{
		return std::strcmp(name, "sqlite3_sourceid") == 0 ||
			std::strcmp(name, "sqlite3_uri_parameter") == 0 ||
			std::strcmp(name, "sqlite3_uri_key") == 0;
	}

	[[nodiscard]] dlsym_function resolve_real_dlsym() noexcept
	{
		static_assert(sizeof(dlsym_function) == sizeof(void*));
		constexpr const char* versions[]{"GLIBC_2.34", "GLIBC_2.2.5"};
		for (const auto* version : versions)
		{
			if (auto* symbol = ::dlvsym(RTLD_NEXT, "dlsym", version); symbol != nullptr)
				return reinterpret_cast<dlsym_function>(symbol);
		}
		return nullptr;
	}

	[[nodiscard]] bool authentic_sqlite_handle(void* handle,
											   const dlsym_function real_dlsym) noexcept
	{
		if (handle == RTLD_DEFAULT || handle == RTLD_NEXT)
			return false;
		constexpr const char* controls[]{"sqlite3_libversion_number", "sqlite3_open_v2"};
		for (const auto* control : controls)
			if (real_dlsym(handle, control) == nullptr)
				return false;
		constexpr const char* source_symbols[]{
			"sqlite3_sourceid", "sqlite3_uri_parameter", "sqlite3_uri_key"};
		for (const auto* symbol : source_symbols)
			if (real_dlsym(handle, symbol) == nullptr)
				return false;
		return true;
	}
} // namespace

extern "C" __attribute__((visibility("default"))) void* dlsym(void* handle,
															  const char* name) noexcept
{
	static const auto real_dlsym = resolve_real_dlsym();
	if (real_dlsym == nullptr)
		__builtin_trap();
	if (blocked_sqlite_source_shm_symbol(name))
	{
		auto* target = __atomic_load_n(&sqlite_handle, __ATOMIC_ACQUIRE);
		if (target == nullptr && authentic_sqlite_handle(handle, real_dlsym))
		{
			auto* expected = static_cast<void*>(nullptr);
			(void)__atomic_compare_exchange_n(
				&sqlite_handle, &expected, handle, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE);
			target = __atomic_load_n(&sqlite_handle, __ATOMIC_ACQUIRE);
		}
		if (target != nullptr && handle == target)
			return nullptr;
	}
	return real_dlsym(handle, name);
}
