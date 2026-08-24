#include "sqlite202_crash_recovery_harness.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <chrono>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

#include <dlfcn.h>
#include <poll.h>

#include "sdk/sqlite_vfs_abi_internal.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

struct sqlite3;

namespace cxxlens::test
{
#if defined(__linux__)
	namespace
	{
		using sqlite_file = cxxlens::sdk::sqlite_vfs_abi::file;
		using sqlite_io_methods = cxxlens::sdk::sqlite_vfs_abi::io_methods;
		using sqlite_vfs = cxxlens::sdk::sqlite_vfs_abi::vfs;
		using sqlite_syscall_ptr = cxxlens::sdk::sqlite_vfs_abi::syscall_ptr;

		constexpr int sqlite_ok = 0;
		constexpr int sqlite_open_read_write = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_main_journal = 0x00000800;
		constexpr int sqlite_open_write_ahead_log = 0x00080000;
		constexpr int sqlite_io_error = 10;
		constexpr auto callback_deadline = std::chrono::seconds{15};
		constexpr auto child_deadline = std::chrono::seconds{10};
		constexpr std::size_t maximum_journal_page_size = 65'536U;
		constexpr std::uint32_t maximum_journal_record_count = 4096U;
		constexpr std::array<std::byte, 8U> rollback_journal_magic{
			std::byte{0xd9U},
			std::byte{0xd5U},
			std::byte{0x05U},
			std::byte{0xf9U},
			std::byte{0x20U},
			std::byte{0xa1U},
			std::byte{0x63U},
			std::byte{0xd7U},
		};

		using sqlite_vfs_find_function = sqlite_vfs* (*)(const char*);
		using sqlite_vfs_register_function = int (*)(sqlite_vfs*, int);
		using sqlite_vfs_unregister_function = int (*)(sqlite_vfs*);
		using sqlite_open_function = int (*)(const char*, sqlite3**, int, const char*);
		using sqlite_close_function = int (*)(sqlite3*);
		using sqlite_exec_callback = int (*)(void*, int, char**, char**);
		using sqlite_exec_function =
			int (*)(sqlite3*, const char*, sqlite_exec_callback, void*, char**);
		using sqlite_error_function = const char* (*)(sqlite3*);
		using sqlite_free_function = void (*)(void*);

		template <class Function>
		[[nodiscard]] Function load_symbol(void* library, const char* name) noexcept
		{
			const auto symbol = ::dlsym(library, name);
			if (symbol == nullptr || sizeof(Function) != sizeof(symbol))
				return nullptr;
			return std::bit_cast<Function>(symbol);
		}

		struct sqlite_api final
		{
			void* library{};
			sqlite_vfs_find_function vfs_find{};
			sqlite_vfs_register_function vfs_register{};
			sqlite_vfs_unregister_function vfs_unregister{};
			sqlite_open_function open{};
			sqlite_close_function close{};
			sqlite_exec_function exec{};
			sqlite_error_function errmsg{};
			sqlite_free_function free_memory{};

			sqlite_api() = default;
			sqlite_api(const sqlite_api&) = delete;
			sqlite_api& operator=(const sqlite_api&) = delete;
			sqlite_api(sqlite_api&& other) noexcept
				: library{std::exchange(other.library, nullptr)}, vfs_find{other.vfs_find},
				  vfs_register{other.vfs_register}, vfs_unregister{other.vfs_unregister},
				  open{other.open}, close{other.close}, exec{other.exec}, errmsg{other.errmsg},
				  free_memory{other.free_memory}
			{
			}
			sqlite_api& operator=(sqlite_api&&) = delete;
			~sqlite_api()
			{
				if (library != nullptr)
					(void)::dlclose(library);
			}

			[[nodiscard]] bool load() noexcept
			{
				library = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
				if (library == nullptr)
					library = ::dlopen("libsqlite3.so", RTLD_NOW | RTLD_LOCAL);
				if (library == nullptr)
					return false;
				vfs_find = load_symbol<sqlite_vfs_find_function>(library, "sqlite3_vfs_find");
				vfs_register =
					load_symbol<sqlite_vfs_register_function>(library, "sqlite3_vfs_register");
				vfs_unregister =
					load_symbol<sqlite_vfs_unregister_function>(library, "sqlite3_vfs_unregister");
				open = load_symbol<sqlite_open_function>(library, "sqlite3_open_v2");
				close = load_symbol<sqlite_close_function>(library, "sqlite3_close_v2");
				exec = load_symbol<sqlite_exec_function>(library, "sqlite3_exec");
				errmsg = load_symbol<sqlite_error_function>(library, "sqlite3_errmsg");
				free_memory = load_symbol<sqlite_free_function>(library, "sqlite3_free");
				return vfs_find != nullptr && vfs_register != nullptr &&
					vfs_unregister != nullptr && open != nullptr && close != nullptr &&
					exec != nullptr && errmsg != nullptr && free_memory != nullptr;
			}
		};

		enum class stop_callback : std::uint8_t
		{
			sync,
			write,
		};

		[[nodiscard]] bool parse_stop_callback(const std::string_view value,
											   stop_callback& output) noexcept
		{
			if (value == "sync")
			{
				output = stop_callback::sync;
				return true;
			}
			if (value == "write")
			{
				output = stop_callback::write;
				return true;
			}
			return false;
		}

		struct forwarding_vfs;

		struct file_identity final
		{
			dev_t device{};
			ino_t inode{};
			mode_t mode{};
			off_t size{};
			timespec modified{};
			timespec changed{};
		};

		[[nodiscard]] file_identity identity_from_stat(const struct stat& value) noexcept
		{
			return {value.st_dev,
					value.st_ino,
					value.st_mode,
					value.st_size,
					value.st_mtim,
					value.st_ctim};
		}

		[[nodiscard]] bool same_object(const file_identity& left,
									   const file_identity& right) noexcept
		{
			return left.device == right.device && left.inode == right.inode &&
				left.mode == right.mode && S_ISREG(left.mode) && S_ISREG(right.mode);
		}

		[[nodiscard]] bool same_snapshot(const file_identity& left,
										 const file_identity& right) noexcept
		{
			return same_object(left, right) && left.size == right.size &&
				left.modified.tv_sec == right.modified.tv_sec &&
				left.modified.tv_nsec == right.modified.tv_nsec &&
				left.changed.tv_sec == right.changed.tv_sec &&
				left.changed.tv_nsec == right.changed.tv_nsec;
		}

		[[nodiscard]] bool capture_path_identity(const std::string& path,
												 file_identity& output) noexcept
		{
			struct stat observed{};
			if (::lstat(path.c_str(), &observed) != 0 || !S_ISREG(observed.st_mode))
				return false;
			output = identity_from_stat(observed);
			return true;
		}

		[[nodiscard]] bool capture_fd_identity(const int descriptor, file_identity& output) noexcept
		{
			struct stat observed{};
			if (::fstat(descriptor, &observed) != 0 || !S_ISREG(observed.st_mode))
				return false;
			output = identity_from_stat(observed);
			return true;
		}

		[[nodiscard]] bool
		read_exact_at(const int descriptor,
					  const std::span<std::byte> output,
					  const off_t offset,
					  const std::chrono::steady_clock::time_point deadline) noexcept
		{
			std::size_t consumed{};
			while (consumed < output.size())
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				const auto count = ::pread(descriptor,
										   output.data() + consumed,
										   output.size() - consumed,
										   offset + static_cast<off_t>(consumed));
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0)
				{
					if (errno == EINTR && std::chrono::steady_clock::now() < deadline)
						continue;
					return false;
				}
				return false;
			}
			return true;
		}

		[[nodiscard]] std::uint32_t read_be_u32(const std::span<const std::byte> input,
												const std::size_t offset) noexcept
		{
			return (std::to_integer<std::uint32_t>(input[offset]) << 24U) |
				(std::to_integer<std::uint32_t>(input[offset + 1U]) << 16U) |
				(std::to_integer<std::uint32_t>(input[offset + 2U]) << 8U) |
				std::to_integer<std::uint32_t>(input[offset + 3U]);
		}

		[[nodiscard]] bool valid_power_of_two(const std::uint32_t value,
											  const std::uint32_t minimum,
											  const std::uint32_t maximum) noexcept
		{
			return value >= minimum && value <= maximum && (value & (value - 1U)) == 0U;
		}

		[[nodiscard]] std::uint32_t pager_record_checksum(const std::span<const std::byte> page,
														  const std::uint32_t nonce) noexcept
		{
			auto checksum = nonce;
			if (page.size() < 200U)
				return checksum;
			for (std::size_t index = page.size() - 200U; index > 0U;)
			{
				checksum += std::to_integer<std::uint32_t>(page[index]);
				if (index <= 200U)
					break;
				index -= 200U;
			}
			return checksum;
		}

		[[nodiscard]] bool path_is_absent(const std::string& path) noexcept
		{
			struct stat observed{};
			return ::lstat(path.c_str(), &observed) != 0 && errno == ENOENT;
		}

		[[nodiscard]] bool
		validate_hot_rollback_journal(const std::string& database_path,
									  const std::string& journal_path,
									  const std::string& wal_path,
									  const std::string& shm_path,
									  const file_identity& expected_main) noexcept
		{
			const auto validation_deadline = std::chrono::steady_clock::now() + callback_deadline;
			if (!path_is_absent(wal_path) || !path_is_absent(shm_path))
				return false;

			const int database = ::open(database_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			if (database < 0)
				return false;
			const int journal = ::open(journal_path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
			bool valid = journal >= 0;
			if (valid)
			{
				valid = [&]() noexcept
				{
					file_identity database_before;
					file_identity journal_before;
					if (!capture_fd_identity(database, database_before) ||
						!capture_fd_identity(journal, journal_before) ||
						!same_snapshot(database_before, expected_main))
						return false;

					std::array<std::byte, 28U> header{};
					if (!read_exact_at(journal, header, 0, validation_deadline) ||
						!std::equal(rollback_journal_magic.begin(),
									rollback_journal_magic.end(),
									header.begin()))
						return false;

					const auto record_count = read_be_u32(header, 8U);
					const auto nonce = read_be_u32(header, 12U);
					const auto database_page_count = read_be_u32(header, 16U);
					const auto sector_size = read_be_u32(header, 20U);
					const auto page_size = read_be_u32(header, 24U);
					if (record_count == 0U || record_count > maximum_journal_record_count ||
						database_page_count == 0U ||
						database_page_count > maximum_journal_record_count ||
						!valid_power_of_two(sector_size, 32U, 65'536U) ||
						!valid_power_of_two(page_size, 512U, 65'536U))
						return false;

					const auto record_size = static_cast<std::uint64_t>(page_size) + 8U;
					const auto journal_size = static_cast<std::uint64_t>(sector_size) +
						static_cast<std::uint64_t>(record_count) * record_size;
					const auto database_size = static_cast<std::uint64_t>(database_page_count) *
						static_cast<std::uint64_t>(page_size);
					const auto maximum_file_size =
						static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
					if (journal_size > maximum_file_size || database_size > maximum_file_size ||
						journal_before.size != static_cast<off_t>(journal_size) ||
						database_before.size != static_cast<off_t>(database_size))
						return false;

					std::array<std::byte, 100U> database_header{};
					constexpr std::array<std::byte, 16U> sqlite_header{
						std::byte{'S'},
						std::byte{'Q'},
						std::byte{'L'},
						std::byte{'i'},
						std::byte{'t'},
						std::byte{'e'},
						std::byte{' '},
						std::byte{'f'},
						std::byte{'o'},
						std::byte{'r'},
						std::byte{'m'},
						std::byte{'a'},
						std::byte{'t'},
						std::byte{' '},
						std::byte{'3'},
						std::byte{0U},
					};
					if (!read_exact_at(database, database_header, 0, validation_deadline) ||
						!std::equal(
							sqlite_header.begin(), sqlite_header.end(), database_header.begin()))
						return false;
					const auto encoded_page_size =
						(std::to_integer<std::uint32_t>(database_header[16U]) << 8U) |
						std::to_integer<std::uint32_t>(database_header[17U]);
					const auto decoded_page_size =
						encoded_page_size == 1U ? 65'536U : encoded_page_size;
					if (decoded_page_size != page_size)
						return false;

					std::array<bool, static_cast<std::size_t>(maximum_journal_record_count) + 1U>
						seen_pages{};
					std::array<std::byte, maximum_journal_page_size> journal_page{};
					std::array<std::byte, maximum_journal_page_size> database_page{};
					std::array<std::byte, 4U> number_or_checksum{};
					for (std::uint32_t record{}; record < record_count; ++record)
					{
						const auto offset = static_cast<std::uint64_t>(sector_size) +
							static_cast<std::uint64_t>(record) * record_size;
						if (!read_exact_at(journal,
										   number_or_checksum,
										   static_cast<off_t>(offset),
										   validation_deadline))
							return false;
						const auto page_number = read_be_u32(number_or_checksum, 0U);
						if (page_number == 0U || page_number > database_page_count ||
							seen_pages[page_number])
							return false;
						seen_pages[page_number] = true;

						const auto page_span = std::span{journal_page}.first(page_size);
						if (!read_exact_at(journal,
										   page_span,
										   static_cast<off_t>(offset + 4U),
										   validation_deadline) ||
							!read_exact_at(journal,
										   number_or_checksum,
										   static_cast<off_t>(offset + 4U + page_size),
										   validation_deadline) ||
							read_be_u32(number_or_checksum, 0U) !=
								pager_record_checksum(page_span, nonce))
							return false;

						const auto database_offset =
							static_cast<std::uint64_t>(page_number - 1U) * page_size;
						const auto database_span = std::span{database_page}.first(page_size);
						if (!read_exact_at(database,
										   database_span,
										   static_cast<off_t>(database_offset),
										   validation_deadline) ||
							!std::equal(page_span.begin(), page_span.end(), database_span.begin()))
							return false;
					}

					file_identity database_after;
					file_identity journal_after;
					return capture_fd_identity(database, database_after) &&
						capture_fd_identity(journal, journal_after) &&
						same_snapshot(database_before, database_after) &&
						same_snapshot(journal_before, journal_after) &&
						same_snapshot(database_after, expected_main);
				}();
			}
			const bool journal_closed = journal < 0 || ::close(journal) == 0;
			const bool database_closed = ::close(database) == 0;
			return valid && journal_closed && database_closed;
		}

		// SQLite allocates this object as the prefix of every sqlite3_file.  The delegated native
		// file starts at file_offset and is never inspected by the test harness.
		struct alignas(std::max_align_t) forwarding_file final
		{
			sqlite_file public_file{};
			forwarding_vfs* owner{};
			sqlite_file* delegated{};
			int flags{};
			std::array<char, 512U> path{};
		};

		struct forwarding_vfs final
		{
			sqlite_vfs wrapper{};
			sqlite_vfs* delegate{};
			int file_offset{};
			int notify_fd{-1};
			stop_callback callback{stop_callback::sync};
			bool stop_enabled{};
			bool expected_main_valid{};
			bool binding_invalid{};
			std::array<char, 64U> name{};
			std::array<char, 16U> role{};
			std::string database_path;
			std::string journal_path;
			std::string wal_path;
			std::string shm_path;
			file_identity expected_main;
		};

		[[nodiscard]] forwarding_file* as_forwarding_file(sqlite_file* value) noexcept
		{
			return reinterpret_cast<forwarding_file*>(value);
		}

		[[nodiscard]] const char* file_role(const forwarding_file& value) noexcept
		{
			if ((value.flags & sqlite_open_main_database) != 0)
				return "main";
			if ((value.flags & sqlite_open_main_journal) != 0)
				return "journal";
			if ((value.flags & sqlite_open_write_ahead_log) != 0)
				return "wal";
			if (std::strstr(value.path.data(), "-shm") != nullptr)
				return "shm";
			return "other";
		}

		[[nodiscard]] bool role_matches(const forwarding_file& value,
										const std::string_view wanted) noexcept
		{
			return wanted == "any" || wanted == file_role(value);
		}

		[[nodiscard]] bool notify_and_stop(forwarding_vfs& owner) noexcept
		{
			owner.stop_enabled = false;
			const auto deadline = std::chrono::steady_clock::now() + callback_deadline;
			const char marker = 'S';
			bool marker_written = false;
			while (owner.notify_fd >= 0 && std::chrono::steady_clock::now() < deadline)
			{
				const auto written = ::write(owner.notify_fd, &marker, sizeof(marker));
				if (written == static_cast<ssize_t>(sizeof(marker)))
				{
					marker_written = true;
					break;
				}
				if (written >= 0 || errno != EINTR)
					break;
			}
			if (!marker_written)
			{
				owner.binding_invalid = true;
				return false;
			}

			for (;;)
			{
				if (std::chrono::steady_clock::now() >= deadline)
				{
					owner.binding_invalid = true;
					return false;
				}
				if (::kill(::getpid(), SIGSTOP) == 0)
					return true;
				if (errno != EINTR)
				{
					owner.binding_invalid = true;
					return false;
				}
			}
		}

		[[nodiscard]] bool maybe_stop_file(forwarding_file& value,
										   const stop_callback callback,
										   const bool successful) noexcept
		{
			auto& owner = *value.owner;
			if (!successful || !owner.stop_enabled || owner.callback != callback ||
				owner.binding_invalid || !role_matches(value, owner.role.data()))
				return true;
			return notify_and_stop(owner);
		}

		int forwarding_close(sqlite_file* raw) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->close(value.delegated);
		}

		int forwarding_read(sqlite_file* raw,
							void* output,
							const int amount,
							const long long offset) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->read(value.delegated, output, amount, offset);
		}

		int forwarding_write(sqlite_file* raw,
							 const void* input,
							 const int amount,
							 const long long offset) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			const auto result =
				value.delegated->methods->write(value.delegated, input, amount, offset);
			return maybe_stop_file(value, stop_callback::write, result == sqlite_ok)
				? result
				: sqlite_io_error;
		}

		int forwarding_truncate(sqlite_file* raw, const long long size) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->truncate(value.delegated, size);
		}

		int forwarding_sync(sqlite_file* raw, const int flags) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			const auto result = value.delegated->methods->sync(value.delegated, flags);
			bool authentic_boundary = result == sqlite_ok;
			if (authentic_boundary && value.owner->stop_enabled &&
				value.owner->callback == stop_callback::sync &&
				std::strcmp(file_role(value), "journal") == 0)
			{
				authentic_boundary = value.owner->expected_main_valid &&
					validate_hot_rollback_journal(value.owner->database_path,
												  value.owner->journal_path,
												  value.owner->wal_path,
												  value.owner->shm_path,
												  value.owner->expected_main);
			}
			return maybe_stop_file(value, stop_callback::sync, authentic_boundary)
				? result
				: sqlite_io_error;
		}

		int forwarding_file_size(sqlite_file* raw, long long* output) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->file_size(value.delegated, output);
		}

		int forwarding_lock(sqlite_file* raw, const int level) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->lock(value.delegated, level);
		}

		int forwarding_unlock(sqlite_file* raw, const int level) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->unlock(value.delegated, level);
		}

		int forwarding_reserved(sqlite_file* raw, int* output) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->check_reserved_lock(value.delegated, output);
		}

		int forwarding_control(sqlite_file* raw, const int operation, void* output) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->file_control(value.delegated, operation, output);
		}

		int forwarding_sector(sqlite_file* raw) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->sector_size(value.delegated);
		}

		int forwarding_characteristics(sqlite_file* raw) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			return value.delegated->methods->device_characteristics(value.delegated);
		}

		int forwarding_shm_map(sqlite_file* raw,
							   const int page,
							   const int page_size,
							   const int extend,
							   volatile void** output) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->shm_map == nullptr)
				return sqlite_io_error;
			return value.delegated->methods->shm_map(
				value.delegated, page, page_size, extend, output);
		}

		int forwarding_shm_lock(sqlite_file* raw,
								const int offset,
								const int amount,
								const int flags) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->shm_lock == nullptr)
				return sqlite_io_error;
			return value.delegated->methods->shm_lock(value.delegated, offset, amount, flags);
		}

		void forwarding_shm_barrier(sqlite_file* raw) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->shm_barrier != nullptr)
				value.delegated->methods->shm_barrier(value.delegated);
		}

		int forwarding_shm_unmap(sqlite_file* raw, const int delete_flag) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->shm_unmap == nullptr)
				return sqlite_io_error;
			return value.delegated->methods->shm_unmap(value.delegated, delete_flag);
		}

		int forwarding_fetch(sqlite_file* raw,
							 const long long offset,
							 const int amount,
							 void** output) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->fetch == nullptr)
				return sqlite_ok;
			return value.delegated->methods->fetch(value.delegated, offset, amount, output);
		}

		int forwarding_unfetch(sqlite_file* raw, const long long offset, void* page) noexcept
		{
			auto& value = *as_forwarding_file(raw);
			if (value.delegated->methods->unfetch == nullptr)
				return sqlite_ok;
			return value.delegated->methods->unfetch(value.delegated, offset, page);
		}

		const sqlite_io_methods forwarding_methods{
			3,
			forwarding_close,
			forwarding_read,
			forwarding_write,
			forwarding_truncate,
			forwarding_sync,
			forwarding_file_size,
			forwarding_lock,
			forwarding_unlock,
			forwarding_reserved,
			forwarding_control,
			forwarding_sector,
			forwarding_characteristics,
			forwarding_shm_map,
			forwarding_shm_lock,
			forwarding_shm_barrier,
			forwarding_shm_unmap,
			forwarding_fetch,
			forwarding_unfetch,
		};

		int forwarding_open(sqlite_vfs* raw_vfs,
							const char* path,
							sqlite_file* raw_file,
							const int flags,
							int* output_flags) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			auto& value = *as_forwarding_file(raw_file);
			value.path = {};
			value.owner = &owner;
			value.flags = flags;
			value.delegated = reinterpret_cast<sqlite_file*>(
				reinterpret_cast<unsigned char*>(raw_file) + owner.file_offset);
			if (path != nullptr)
			{
				const auto written =
					std::snprintf(value.path.data(), value.path.size(), "%s", path);
				if (written < 0 || static_cast<std::size_t>(written) >= value.path.size())
					owner.binding_invalid = true;
			}
			int delegated_flags{};
			const auto result = owner.delegate->open(
				owner.delegate, path, value.delegated, flags, &delegated_flags);
			if (output_flags != nullptr)
				*output_flags = delegated_flags;
			value.public_file.methods = result == sqlite_ok ? &forwarding_methods : nullptr;
			if (result == sqlite_ok && owner.stop_enabled)
			{
				const auto role = std::string_view{file_role(value)};
				if (role == "main")
				{
					file_identity opened_identity;
					if (path == nullptr || path != owner.database_path ||
						!capture_path_identity(owner.database_path, opened_identity) ||
						!owner.expected_main_valid ||
						!same_snapshot(opened_identity, owner.expected_main))
						owner.binding_invalid = true;
				}
				else if (role == "journal" && (path == nullptr || path != owner.journal_path))
					owner.binding_invalid = true;
				else if (role == "wal" && (path == nullptr || path != owner.wal_path))
					owner.binding_invalid = true;
			}
			return result;
		}

		int forwarding_remove(sqlite_vfs* raw_vfs, const char* path, const int sync) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->remove(owner.delegate, path, sync);
		}

		int forwarding_access(sqlite_vfs* raw_vfs,
							  const char* path,
							  const int flags,
							  int* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->access(owner.delegate, path, flags, output);
		}

		int forwarding_full_pathname(sqlite_vfs* raw_vfs,
									 const char* path,
									 const int size,
									 char* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->full_pathname(owner.delegate, path, size, output);
		}

		void* forwarding_dl_open(sqlite_vfs* raw_vfs, const char* path) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->dl_open != nullptr
				? owner.delegate->dl_open(owner.delegate, path)
				: nullptr;
		}

		void forwarding_dl_error(sqlite_vfs* raw_vfs, const int size, char* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			if (owner.delegate->dl_error != nullptr)
				owner.delegate->dl_error(owner.delegate, size, output);
		}

		void (*forwarding_dl_sym(sqlite_vfs* raw_vfs, void* handle, const char* name))(void)
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->dl_sym != nullptr
				? owner.delegate->dl_sym(owner.delegate, handle, name)
				: nullptr;
		}

		void forwarding_dl_close(sqlite_vfs* raw_vfs, void* handle) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			if (owner.delegate->dl_close != nullptr)
				owner.delegate->dl_close(owner.delegate, handle);
		}

		int forwarding_randomness(sqlite_vfs* raw_vfs, const int size, char* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->randomness(owner.delegate, size, output);
		}

		int forwarding_sleep(sqlite_vfs* raw_vfs, const int microseconds) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->sleep(owner.delegate, microseconds);
		}

		int forwarding_current_time(sqlite_vfs* raw_vfs, double* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->current_time(owner.delegate, output);
		}

		int forwarding_last_error(sqlite_vfs* raw_vfs, const int size, char* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->get_last_error != nullptr
				? owner.delegate->get_last_error(owner.delegate, size, output)
				: sqlite_ok;
		}

		int forwarding_current_time_int64(sqlite_vfs* raw_vfs, long long* output) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->current_time_int64 != nullptr
				? owner.delegate->current_time_int64(owner.delegate, output)
				: sqlite_ok;
		}

		int forwarding_set_system_call(sqlite_vfs* raw_vfs,
									   const char* name,
									   const sqlite_syscall_ptr function) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->set_system_call != nullptr
				? owner.delegate->set_system_call(owner.delegate, name, function)
				: sqlite_io_error;
		}

		sqlite_syscall_ptr forwarding_get_system_call(sqlite_vfs* raw_vfs,
													  const char* name) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->get_system_call != nullptr
				? owner.delegate->get_system_call(owner.delegate, name)
				: nullptr;
		}

		const char* forwarding_next_system_call(sqlite_vfs* raw_vfs, const char* name) noexcept
		{
			auto& owner = *static_cast<forwarding_vfs*>(raw_vfs->app_data);
			return owner.delegate->next_system_call != nullptr
				? owner.delegate->next_system_call(owner.delegate, name)
				: nullptr;
		}

		[[nodiscard]] bool initialize_forwarding_vfs(forwarding_vfs& output,
													 sqlite_vfs* delegate,
													 const std::string_view name,
													 const std::string_view role,
													 const std::string_view operation,
													 const int notify_fd,
													 const bool stop_enabled,
													 const std::string& database_path) noexcept
		{
			try
			{
				if (delegate == nullptr || name.size() > static_cast<std::size_t>(INT_MAX) ||
					role.size() > static_cast<std::size_t>(INT_MAX))
					return false;
				const auto aligned_offset =
					(sizeof(forwarding_file) + alignof(std::max_align_t) - 1U) &
					~(alignof(std::max_align_t) - 1U);
				if (aligned_offset > static_cast<std::size_t>(INT_MAX) ||
					delegate->os_file_bytes < 0 ||
					delegate->os_file_bytes > INT_MAX - static_cast<int>(aligned_offset))
					return false;
				output.delegate = delegate;
				output.file_offset = static_cast<int>(aligned_offset);
				const auto name_written = std::snprintf(output.name.data(),
														output.name.size(),
														"%.*s-%d",
														static_cast<int>(name.size()),
														name.data(),
														static_cast<int>(::getpid()));
				const auto role_written = std::snprintf(output.role.data(),
														output.role.size(),
														"%.*s",
														static_cast<int>(role.size()),
														role.data());
				if (name_written < 0 ||
					static_cast<std::size_t>(name_written) >= output.name.size() ||
					role_written < 0 ||
					static_cast<std::size_t>(role_written) >= output.role.size() ||
					!parse_stop_callback(operation, output.callback))
					return false;
				output.notify_fd = notify_fd;
				output.stop_enabled = stop_enabled;
				output.database_path = database_path;
				output.journal_path = database_path + "-journal";
				output.wal_path = database_path + "-wal";
				output.shm_path = database_path + "-shm";
				output.expected_main_valid =
					!stop_enabled || capture_path_identity(database_path, output.expected_main);
				if (!output.expected_main_valid)
					return false;
				output.wrapper = sqlite_vfs{
					3,
					output.file_offset + delegate->os_file_bytes,
					delegate->maximum_pathname,
					nullptr,
					output.name.data(),
					&output,
					forwarding_open,
					forwarding_remove,
					forwarding_access,
					forwarding_full_pathname,
					forwarding_dl_open,
					forwarding_dl_error,
					forwarding_dl_sym,
					forwarding_dl_close,
					forwarding_randomness,
					forwarding_sleep,
					forwarding_current_time,
					forwarding_last_error,
					forwarding_current_time_int64,
					forwarding_set_system_call,
					forwarding_get_system_call,
					forwarding_next_system_call,
				};
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] bool execute(sqlite_api& api, sqlite3* database, const char* sql) noexcept
		{
			char* message{};
			const auto result = api.exec(database, sql, nullptr, nullptr, &message);
			if (message != nullptr)
				api.free_memory(message);
			return result == sqlite_ok;
		}

		struct count_observation final
		{
			int rows{};
			bool zero{};
		};

		int observe_count(void* context, const int columns, char** values, char**) noexcept
		{
			auto& output = *static_cast<count_observation*>(context);
			++output.rows;
			output.zero = columns == 1 && values != nullptr && values[0] != nullptr &&
				std::strcmp(values[0], "0") == 0;
			return 0;
		}

		[[nodiscard]] bool query_is_empty(sqlite_api& api, sqlite3* database) noexcept
		{
			count_observation observation{};
			char* message{};
			const auto result = api.exec(
				database, "SELECT count(*) FROM t;", observe_count, &observation, &message);
			if (message != nullptr)
				api.free_memory(message);
			return result == sqlite_ok && observation.rows == 1 && observation.zero;
		}

		[[nodiscard]] bool open_database(sqlite_api& api,
										 const std::string& path,
										 const char* vfs_name,
										 const int flags,
										 sqlite3*& output) noexcept
		{
			output = nullptr;
			const auto result = api.open(path.c_str(), &output, flags, vfs_name);
			return result == sqlite_ok && output != nullptr;
		}

		[[nodiscard]] bool prepare_database(const std::string& path) noexcept
		{
			sqlite_api api;
			if (!api.load())
				return false;
			auto* delegate = api.vfs_find(nullptr);
			if (delegate == nullptr)
				return false;
			forwarding_vfs wrapper;
			if (!initialize_forwarding_vfs(
					wrapper, delegate, "cxxlens-sqlite202-setup", "any", "sync", -1, false, path))
				return false;
			if (api.vfs_register(&wrapper.wrapper, 0) != sqlite_ok)
				return false;
			sqlite3* database{};
			const bool opened = open_database(api,
											  path,
											  wrapper.wrapper.name,
											  sqlite_open_read_write | sqlite_open_create,
											  database);
			const bool prepared = opened &&
				execute(api,
						database,
						"PRAGMA journal_mode=DELETE;"
						"PRAGMA synchronous=FULL;"
						"CREATE TABLE IF NOT EXISTS t(x INTEGER NOT NULL);"
						"DELETE FROM t;");
			const bool closed = database == nullptr || api.close(database) == sqlite_ok;
			const bool unregistered = api.vfs_unregister(&wrapper.wrapper) == sqlite_ok;
			return opened && prepared && closed && unregistered;
		}

		[[nodiscard]] bool child_run(const std::string& path,
									 const std::string_view role,
									 const std::string_view operation,
									 const int notify_fd,
									 const std::string_view mode) noexcept
		{
			sqlite_api api;
			if (!api.load())
				return false;
			auto* delegate = api.vfs_find(nullptr);
			if (delegate == nullptr)
				return false;
			stop_callback parsed_callback{};
			if (!parse_stop_callback(operation, parsed_callback) && mode != "verify")
				return false;
			forwarding_vfs wrapper;
			if (!initialize_forwarding_vfs(wrapper,
										   delegate,
										   "cxxlens-sqlite202-child",
										   role,
										   operation,
										   notify_fd,
										   mode != "verify",
										   path))
				return false;
			if (mode == "recovery" &&
				!validate_hot_rollback_journal(wrapper.database_path,
											   wrapper.journal_path,
											   wrapper.wal_path,
											   wrapper.shm_path,
											   wrapper.expected_main))
				return false;
			if (api.vfs_register(&wrapper.wrapper, 0) != sqlite_ok)
				return false;
			sqlite3* database{};
			const bool opened =
				open_database(api, path, wrapper.wrapper.name, sqlite_open_read_write, database);
			bool completed = false;
			if (opened && mode == "transaction")
			{
				// The child is expected to stop inside the delegated journal sync.  If it reaches
				// this point, the callback boundary was missed and the parent must fail the test.
				completed =
					execute(api, database, "BEGIN IMMEDIATE; INSERT INTO t VALUES(42); COMMIT;") &&
					!wrapper.stop_enabled;
			}
			else if (opened && mode == "recovery")
			{
				// Opening the database performs SQLite's real hot-journal recovery before this
				// read. A successful stop therefore identifies a main-page write in fresh recovery,
				// not a hand-authored or resumed journal operation.
				completed = query_is_empty(api, database) && !wrapper.stop_enabled;
			}
			else if (opened && mode == "verify")
				completed = query_is_empty(api, database);
			if (database != nullptr)
				completed = (api.close(database) == sqlite_ok) && completed;
			const bool unregistered = api.vfs_unregister(&wrapper.wrapper) == sqlite_ok;
			return opened && completed && unregistered;
		}

		[[nodiscard]] bool nonempty_regular_file(const std::string& path) noexcept
		{
			struct stat observed{};
			return ::lstat(path.c_str(), &observed) == 0 && S_ISREG(observed.st_mode) &&
				observed.st_size > 0;
		}

		[[nodiscard]] bool sidecars_absent(const std::string& database_path) noexcept
		{
			return path_is_absent(database_path + "-journal") &&
				path_is_absent(database_path + "-wal") && path_is_absent(database_path + "-shm");
		}

		[[nodiscard]] bool close_descriptor(int& descriptor) noexcept
		{
			if (descriptor < 0)
				return true;
			const auto value = std::exchange(descriptor, -1);
			return ::close(value) == 0;
		}

		[[nodiscard]] bool close_descriptor_pair(int (&descriptors)[2]) noexcept
		{
			const bool first_closed = close_descriptor(descriptors[0]);
			const bool second_closed = close_descriptor(descriptors[1]);
			return first_closed && second_closed;
		}

		[[nodiscard]] int open_pidfd(const pid_t child) noexcept
		{
			const auto result = ::syscall(SYS_pidfd_open, child, 0U);
			if (result < 0 || result > INT_MAX)
				return -1;
			return static_cast<int>(result);
		}

		[[nodiscard]] int
		remaining_milliseconds(const std::chrono::steady_clock::time_point deadline) noexcept
		{
			const auto now = std::chrono::steady_clock::now();
			if (now >= deadline)
				return 0;
			const auto remaining =
				std::chrono::ceil<std::chrono::milliseconds>(deadline - now).count();
			return static_cast<int>(std::clamp<std::int64_t>(remaining, 1, INT_MAX));
		}

		[[nodiscard]] bool
		poll_pidfd_until(const int pidfd,
						 const std::chrono::steady_clock::time_point deadline) noexcept
		{
			for (;;)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				pollfd descriptor{pidfd, POLLIN, 0};
				const auto polled = ::poll(&descriptor, 1, remaining_milliseconds(deadline));
				if (polled < 0)
				{
					if (errno != EINTR || std::chrono::steady_clock::now() >= deadline)
						return false;
					continue;
				}
				if (polled <= 0)
					return false;
				return (descriptor.revents & POLLIN) != 0 &&
					(descriptor.revents & (POLLERR | POLLNVAL)) == 0;
			}
		}

		/**
		 * Reap a child without ever entering a blocking waitpid call.
		 *
		 * A pidfd poll is only an observation of process exit; it is not a
		 * substitute for collecting the child.  In particular, a timeout or a
		 * pidfd error must not be followed by a blocking wait, because a lost
		 * exit notification would then turn test cleanup into an unbounded wait.
		 * Keep probing with WNOHANG until the same bounded deadline used by the
		 * child lifecycle.  The zero-fd poll is a short, EINTR-tolerant yield and
		 * does not create another blocking process wait.
		 */
		[[nodiscard]] bool
		reap_child_until(const pid_t child,
						 int& status,
						 const std::chrono::steady_clock::time_point deadline) noexcept
		{
			for (;;)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				const auto waited = ::waitpid(child, &status, WNOHANG);
				if (waited == child)
					return true;
				if (waited < 0)
				{
					if (errno == EINTR && std::chrono::steady_clock::now() < deadline)
						continue;
					return false;
				}
				if (std::chrono::steady_clock::now() >= deadline)
					return false;

				// Limit every yield to one millisecond so cleanup remains bounded
				// even when the child does not observe SIGKILL promptly.
				const auto remaining = remaining_milliseconds(deadline);
				const auto yield_milliseconds = std::min(remaining, 1);
				if (::poll(nullptr, 0, yield_milliseconds) < 0)
				{
					if (errno != EINTR || std::chrono::steady_clock::now() >= deadline)
						return false;
				}
			}
		}

		[[nodiscard]] bool
		send_sigkill(const pid_t child,
					 const std::chrono::steady_clock::time_point deadline) noexcept
		{
			for (;;)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				if (::kill(child, SIGKILL) == 0)
					return true;
				if (errno == EINTR && std::chrono::steady_clock::now() < deadline)
					continue;
				return false;
			}
		}

		[[nodiscard]] bool kill_and_reap(const pid_t child) noexcept
		{
			const auto deadline = std::chrono::steady_clock::now() + child_deadline;
			const bool killed = send_sigkill(child, deadline);
			int status{};
			const bool reaped = reap_child_until(child, status, deadline);
			return killed && reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL;
		}

		[[nodiscard]] bool
		wait_for_boundary_marker(const int read_fd,
								 const int pidfd,
								 const std::chrono::steady_clock::time_point deadline) noexcept
		{
			for (;;)
			{
				if (std::chrono::steady_clock::now() >= deadline)
					return false;
				std::array<pollfd, 2U> descriptors{pollfd{read_fd, POLLIN, 0},
												   pollfd{pidfd, POLLIN, 0}};
				const auto polled = ::poll(
					descriptors.data(), descriptors.size(), remaining_milliseconds(deadline));
				if (polled < 0)
				{
					if (errno != EINTR || std::chrono::steady_clock::now() >= deadline)
						return false;
					continue;
				}
				if (polled <= 0 || (descriptors[1U].revents & POLLIN) != 0 ||
					(descriptors[0U].revents & (POLLERR | POLLNVAL)) != 0 ||
					(descriptors[1U].revents & (POLLERR | POLLNVAL)) != 0)
					return false;
				if ((descriptors[0U].revents & POLLIN) != 0)
				{
					for (;;)
					{
						if (std::chrono::steady_clock::now() >= deadline)
							return false;
						char marker{};
						const auto count = ::read(read_fd, &marker, sizeof(marker));
						if (count < 0)
						{
							if (errno != EINTR || std::chrono::steady_clock::now() >= deadline)
								return false;
							continue;
						}
						return count == static_cast<ssize_t>(sizeof(marker)) && marker == 'S';
					}
				}
				if ((descriptors[0U].revents & POLLHUP) != 0)
					return false;
			}
		}

		[[nodiscard]] bool run_stopped_child(const char* executable,
											 const std::string& database_path,
											 const std::string_view role,
											 const std::string_view operation,
											 const std::string_view mode) noexcept
		{
			std::array<char, 16U> role_text{};
			std::array<char, 16U> operation_text{};
			std::array<char, 16U> mode_text{};
			char fd_text[32]{};
			if (role.size() >= role_text.size() || operation.size() >= operation_text.size() ||
				mode.size() >= mode_text.size())
				return false;
			(void)std::copy(role.begin(), role.end(), role_text.begin());
			(void)std::copy(operation.begin(), operation.end(), operation_text.begin());
			(void)std::copy(mode.begin(), mode.end(), mode_text.begin());
			int pipe_fds[2]{-1, -1};
			if (::pipe(pipe_fds) != 0)
				return false;
			const auto fd_written = std::snprintf(fd_text, sizeof(fd_text), "%d", pipe_fds[1]);
			if (fd_written < 0 || static_cast<std::size_t>(fd_written) >= sizeof(fd_text))
			{
				if (!close_descriptor_pair(pipe_fds))
					std::fprintf(stderr, "sqlite202: child-boundary pipe cleanup failed\n");
				return false;
			}
			const auto child = ::fork();
			if (child == 0)
			{
				if (::close(pipe_fds[0]) != 0)
					::_exit(126);
				::execl(executable,
						executable,
						"--sqlite202-child",
						database_path.c_str(),
						role_text.data(),
						operation_text.data(),
						fd_text,
						mode_text.data(),
						static_cast<char*>(nullptr));
				::_exit(127);
			}
			if (child < 0)
			{
				if (!close_descriptor_pair(pipe_fds))
					std::fprintf(stderr, "sqlite202: fork-failure pipe cleanup failed\n");
				return false;
			}
			const auto parent_write_closed = close_descriptor(pipe_fds[1]);
			int pidfd = open_pidfd(child);
			const bool marker = parent_write_closed && pidfd >= 0 &&
				wait_for_boundary_marker(pipe_fds[0],
										 pidfd,
										 std::chrono::steady_clock::now() + callback_deadline);
			const bool killed_and_reaped = kill_and_reap(child);
			const bool read_closed = close_descriptor(pipe_fds[0]);
			const bool pidfd_closed = close_descriptor(pidfd);
			return marker && read_closed && killed_and_reaped && pidfd_closed;
		}

		[[nodiscard]] bool run_verification_child(const char* executable,
												  const std::string& database_path) noexcept
		{
			int liveness_pipe[2]{-1, -1};
			if (::pipe(liveness_pipe) != 0)
				return false;
			const auto child = ::fork();
			if (child == 0)
			{
				if (::close(liveness_pipe[0]) != 0)
					::_exit(126);
				::execl(executable,
						executable,
						"--sqlite202-verify",
						database_path.c_str(),
						static_cast<char*>(nullptr));
				::_exit(127);
			}
			if (child < 0)
			{
				if (!close_descriptor_pair(liveness_pipe))
					std::fprintf(stderr, "sqlite202: verification fork cleanup failed\n");
				return false;
			}
			const bool writer_closed = close_descriptor(liveness_pipe[1]);
			int pidfd = open_pidfd(child);
			if (!writer_closed || pidfd < 0)
			{
				const bool cleaned = kill_and_reap(child);
				const bool pipe_closed = close_descriptor(liveness_pipe[0]);
				const bool pidfd_closed = close_descriptor(pidfd);
				if (!cleaned || !pipe_closed || !pidfd_closed)
					std::fprintf(stderr, "sqlite202: pidfd setup cleanup failed\n");
				return false;
			}
			const bool exited =
				poll_pidfd_until(pidfd, std::chrono::steady_clock::now() + child_deadline);
			if (!exited)
			{
				const bool cleaned = kill_and_reap(child);
				const bool pipe_closed = close_descriptor(liveness_pipe[0]);
				const bool pidfd_closed = close_descriptor(pidfd);
				if (!cleaned || !pipe_closed || !pidfd_closed)
					std::fprintf(stderr, "sqlite202: timed-out child cleanup failed\n");
				return false;
			}
			int status{};
			const bool reaped =
				reap_child_until(child, status, std::chrono::steady_clock::now() + child_deadline);
			const bool pipe_closed = close_descriptor(liveness_pipe[0]);
			const bool pidfd_closed = close_descriptor(pidfd);
			return reaped && pipe_closed && pidfd_closed && WIFEXITED(status) &&
				WEXITSTATUS(status) == 0;
		}

		struct temporary_directory final
		{
			std::array<char, 96U> buffer{};
			std::filesystem::path path;
			bool created{};

			[[nodiscard]] bool create() noexcept
			{
				const auto written = std::snprintf(
					buffer.data(), buffer.size(), "/tmp/cxxlens-sqlite202-crash-XXXXXX");
				if (written <= 0 || static_cast<std::size_t>(written) >= buffer.size() ||
					::mkdtemp(buffer.data()) == nullptr)
					return false;
				created = true;
				try
				{
					path = buffer.data();
					return true;
				}
				catch (...)
				{
					if (::rmdir(buffer.data()) == 0)
						created = false;
					return false;
				}
			}

			[[nodiscard]] bool cleanup() noexcept
			{
				if (!created)
					return true;
				if (path.empty())
				{
					const bool removed = ::rmdir(buffer.data()) == 0;
					created = !removed;
					return removed;
				}
				try
				{
					std::error_code remove_error;
					(void)std::filesystem::remove_all(path, remove_error);
					std::error_code exists_error;
					const bool remains = std::filesystem::exists(path, exists_error);
					if (remove_error || exists_error || remains)
						return false;
					created = false;
					path.clear();
					return true;
				}
				catch (...)
				{
					return false;
				}
			}

			~temporary_directory()
			{
				(void)cleanup();
			}
		};
	} // namespace

	bool run_sqlite202_crash_recrash(const char* executable_path) noexcept
	{
		temporary_directory temporary;
		bool scenario_succeeded = false;
		try
		{
			if (executable_path != nullptr && executable_path[0] != '\0' && temporary.create())
			{
				const auto executable = std::filesystem::absolute(executable_path).string();
				const auto database = (temporary.path / "authority.sqlite").string();
				file_identity original_main;
				if (!prepare_database(database) || !capture_path_identity(database, original_main))
				{
					std::fprintf(stderr, "sqlite202: database preparation failed\n");
				}
				else if (!run_stopped_child(
							 executable.c_str(), database, "journal", "sync", "transaction") ||
						 !validate_hot_rollback_journal(database,
														database + "-journal",
														database + "-wal",
														database + "-shm",
														original_main))
				{
					std::fprintf(
						stderr,
						"sqlite202: crash boundary was not an identity-bound hot journal\n");
				}
				else if (!run_stopped_child(
							 executable.c_str(), database, "main", "write", "recovery") ||
						 !nonempty_regular_file(database + "-journal"))
				{
					std::fprintf(stderr,
								 "sqlite202: recovery recrash did not retain the real journal\n");
				}
				else if (!run_verification_child(executable.c_str(), database) ||
						 !sidecars_absent(database))
				{
					std::fprintf(stderr,
								 "sqlite202: fresh recovery did not prove empty/clean state\n");
				}
				else
					scenario_succeeded = true;
			}
		}
		catch (...)
		{
			scenario_succeeded = false;
		}
		const bool cleaned = temporary.cleanup();
		if (!cleaned)
			std::fprintf(stderr, "sqlite202: temporary directory cleanup failed\n");
		return scenario_succeeded && cleaned;
	}

	int sqlite202_crash_child_entry(const int argc, char** argv) noexcept
	{
		try
		{
			if (argc == 7 && std::strcmp(argv[1], "--sqlite202-child") == 0)
			{
				const auto notify_fd = std::strtol(argv[5], nullptr, 10);
				if (notify_fd < 0 || notify_fd > std::numeric_limits<int>::max())
					return 2;
				return child_run(argv[2], argv[3], argv[4], static_cast<int>(notify_fd), argv[6])
					? 0
					: 3;
			}
			if (argc == 3 && std::strcmp(argv[1], "--sqlite202-verify") == 0)
				return child_run(argv[2], "any", "sync", -1, "verify") ? 0 : 4;
		}
		catch (...)
		{
			return 5;
		}
		return 2;
	}
#else
	bool run_sqlite202_crash_recrash(const char*) noexcept
	{
		return false;
	}

	int sqlite202_crash_child_entry(int, char**) noexcept
	{
		return 2;
	}
#endif
} // namespace cxxlens::test
