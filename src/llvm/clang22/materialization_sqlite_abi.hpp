#pragma once

namespace cxxlens::detail::clang22::materialization
{
	struct sqlite3_file;
	struct sqlite3_io_methods;
	struct sqlite3_vfs;
	using sqlite3_syscall_ptr = void (*)(void);

	struct sqlite3_file
	{
		const sqlite3_io_methods* methods;
	};

	struct sqlite3_io_methods
	{
		int version;
		int (*close)(sqlite3_file*);
		int (*read)(sqlite3_file*, void*, int, long long);
		int (*write)(sqlite3_file*, const void*, int, long long);
		int (*truncate)(sqlite3_file*, long long);
		int (*sync)(sqlite3_file*, int);
		int (*file_size)(sqlite3_file*, long long*);
		int (*lock)(sqlite3_file*, int);
		int (*unlock)(sqlite3_file*, int);
		int (*check_reserved_lock)(sqlite3_file*, int*);
		int (*file_control)(sqlite3_file*, int, void*);
		int (*sector_size)(sqlite3_file*);
		int (*device_characteristics)(sqlite3_file*);
		int (*shm_map)(sqlite3_file*, int, int, int, volatile void**);
		int (*shm_lock)(sqlite3_file*, int, int, int);
		void (*shm_barrier)(sqlite3_file*);
		int (*shm_unmap)(sqlite3_file*, int);
		int (*fetch)(sqlite3_file*, long long, int, void**);
		int (*unfetch)(sqlite3_file*, long long, void*);
	};

	struct sqlite3_vfs
	{
		int version;
		int os_file_bytes;
		int maximum_pathname;
		sqlite3_vfs* next;
		const char* name;
		void* app_data;
		int (*open)(sqlite3_vfs*, const char*, sqlite3_file*, int, int*);
		int (*remove)(sqlite3_vfs*, const char*, int);
		int (*access)(sqlite3_vfs*, const char*, int, int*);
		int (*full_pathname)(sqlite3_vfs*, const char*, int, char*);
		void* (*dl_open)(sqlite3_vfs*, const char*);
		void (*dl_error)(sqlite3_vfs*, int, char*);
		void (*(*dl_sym)(sqlite3_vfs*, void*, const char*))(void);
		void (*dl_close)(sqlite3_vfs*, void*);
		int (*randomness)(sqlite3_vfs*, int, char*);
		int (*sleep)(sqlite3_vfs*, int);
		int (*current_time)(sqlite3_vfs*, double*);
		int (*get_last_error)(sqlite3_vfs*, int, char*);
		int (*current_time_int64)(sqlite3_vfs*, long long*);
		int (*set_system_call)(sqlite3_vfs*, const char*, sqlite3_syscall_ptr);
		sqlite3_syscall_ptr (*get_system_call)(sqlite3_vfs*, const char*);
		const char* (*next_system_call)(sqlite3_vfs*, const char*);
	};
} // namespace cxxlens::detail::clang22::materialization
