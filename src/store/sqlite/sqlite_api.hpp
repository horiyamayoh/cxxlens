#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include <cxxlens/core/failure.hpp>

#include "../../runtime/dynamic_library_port.hpp"

namespace cxxlens::detail::store::sqlite
{
	struct database;
	struct statement;

	struct api
	{
		using open_v2_fn = int (*)(const char*, database**, int, const char*);
		using close_v2_fn = int (*)(database*);
		using errmsg_fn = const char* (*)(database*);
		using exec_fn =
			int (*)(database*, const char*, int (*)(void*, int, char**, char**), void*, char**);
		using free_fn = void (*)(void*);
		using prepare_v2_fn = int (*)(database*, const char*, int, statement**, const char**);
		using step_fn = int (*)(statement*);
		using finalize_fn = int (*)(statement*);
		using bind_text_fn = int (*)(statement*, int, const char*, int, void (*)(void*));
		using bind_int64_fn = int (*)(statement*, int, std::int64_t);
		using bind_blob64_fn =
			int (*)(statement*, int, const void*, std::uint64_t, void (*)(void*));
		using column_text_fn = const unsigned char* (*)(statement*, int);
		using column_int64_fn = std::int64_t (*)(statement*, int);
		using column_blob_fn = const void* (*)(statement*, int);
		using column_bytes_fn = int (*)(statement*, int);
		using busy_timeout_fn = int (*)(database*, int);

		std::shared_ptr<const runtime::dynamic_library> library;
		open_v2_fn open_v2{};
		close_v2_fn close_v2{};
		errmsg_fn errmsg{};
		exec_fn exec{};
		free_fn free_memory{};
		prepare_v2_fn prepare_v2{};
		step_fn step{};
		finalize_fn finalize{};
		bind_text_fn bind_text{};
		bind_int64_fn bind_int64{};
		bind_blob64_fn bind_blob64{};
		column_text_fn column_text{};
		column_int64_fn column_int64{};
		column_blob_fn column_blob{};
		column_bytes_fn column_bytes{};
		busy_timeout_fn busy_timeout{};
	};

	inline constexpr int ok{0};
	inline constexpr int row{100};
	inline constexpr int done{101};
	inline constexpr int open_read_write{0x00000002};
	inline constexpr int open_create{0x00000004};
	inline constexpr int open_full_mutex{0x00010000};

	[[nodiscard]] result<std::shared_ptr<const api>> load_api();
} // namespace cxxlens::detail::store::sqlite
