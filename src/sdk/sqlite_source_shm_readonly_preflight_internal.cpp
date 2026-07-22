#include "sqlite_source_shm_readonly_preflight_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__)
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#endif

#include "sqlite_connection_lifecycle_internal.hpp"
#include "sqlite_payload_streaming_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::string_view observation_profile{"default-filesystem-v1"};
		constexpr std::string_view source_shm_profile{"sqlite-source-shm-readonly-unix-uri-v1"};
		constexpr std::string_view qualification_candidate_profile{
			"sqlite-source-shm-readonly-qualification-candidate-v1"};
		constexpr std::string_view qualification_token_profile{
			"sqlite-source-shm-readonly-unix-uri-v1.sealed-receipt.v1"};
		constexpr int sqlite_ok = 0;
		constexpr int sqlite_readonly = 8;
		constexpr int sqlite_readonly_cannot_initialize = sqlite_readonly | (5 << 8);
		constexpr int sqlite_open_read_only = 0x00000001;
		constexpr int sqlite_open_read_write = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_uri = 0x00000040;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_full_mutex = 0x00010000;
		constexpr int sqlite_open_private_cache = 0x00040000;
		constexpr int qualified_read_flags = sqlite_open_read_only | sqlite_open_uri |
			sqlite_open_full_mutex | sqlite_open_private_cache;
		constexpr int source_shm_main_xopen_flags =
			sqlite_open_read_only | sqlite_open_uri | sqlite_open_main_database;
		constexpr int fixture_write_flags = sqlite_open_read_write | sqlite_open_create |
			sqlite_open_full_mutex | sqlite_open_private_cache;
		constexpr std::uint64_t maximum_fixture_file_bytes = 64U * 1024U * 1024U;
		constexpr std::size_t copy_buffer_bytes = 64U * 1024U;
		constexpr off_t unix_shm_deadman_switch_offset = 128;
		constexpr std::size_t maximum_source_id_bytes = 4096U;

		[[nodiscard]] error qualification_error(const std::string_view diagnostic_stage = {})
		{
			// Stage labels are intentionally internal only; callers observe one stable fail-closed
			// diagnostic tuple regardless of which qualification proof was unavailable.
			(void)diagnostic_stage;
			return {"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification"};
		}

		void append_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (std::uint32_t shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_bytes(std::vector<std::byte>& output, const std::string_view value)
		{
			append_u64(output, static_cast<std::uint64_t>(value.size()));
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		void append_opaque(std::vector<std::byte>& output,
						   const sqlite_backend_opaque_identity& value)
		{
			append_bytes(output, value.profile);
			append_u64(output, static_cast<std::uint64_t>(value.bytes.size()));
			output.insert(output.end(), value.bytes.begin(), value.bytes.end());
		}

		template <class Value>
		void append_trivial(std::vector<std::byte>& output, const Value& value)
		{
			static_assert(std::is_trivially_copyable_v<Value>);
			const auto bytes = std::as_bytes(std::span{&value, std::size_t{1U}});
			append_u64(output, static_cast<std::uint64_t>(bytes.size()));
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		[[nodiscard]] bool uri_unreserved(const unsigned char value) noexcept
		{
			return (value >= static_cast<unsigned char>('a') &&
					value <= static_cast<unsigned char>('z')) ||
				(value >= static_cast<unsigned char>('A') &&
				 value <= static_cast<unsigned char>('Z')) ||
				(value >= static_cast<unsigned char>('0') &&
				 value <= static_cast<unsigned char>('9')) ||
				value == static_cast<unsigned char>('-') ||
				value == static_cast<unsigned char>('.') ||
				value == static_cast<unsigned char>('_') ||
				value == static_cast<unsigned char>('~');
		}

#if defined(__linux__) && defined(F_OFD_SETLK)
		class owned_descriptor
		{
		  public:
			owned_descriptor() noexcept = default;
			explicit owned_descriptor(const int value) noexcept : value_{value} {}
			owned_descriptor(const owned_descriptor&) = delete;
			owned_descriptor& operator=(const owned_descriptor&) = delete;
			owned_descriptor(owned_descriptor&& other) noexcept : value_{other.release()} {}
			owned_descriptor& operator=(owned_descriptor&& other) noexcept
			{
				if (this != &other)
				{
					reset();
					value_ = other.release();
				}
				return *this;
			}
			~owned_descriptor()
			{
				reset();
			}

			[[nodiscard]] int get() const noexcept
			{
				return value_;
			}
			[[nodiscard]] explicit operator bool() const noexcept
			{
				return value_ >= 0;
			}
			[[nodiscard]] int release() noexcept
			{
				return std::exchange(value_, -1);
			}

		  private:
			void reset() noexcept
			{
				if (value_ >= 0)
					(void)::close(value_);
				value_ = -1;
			}

			int value_{-1};
		};

		struct sqlite3_file;
		struct sqlite3_io_methods;
		struct sqlite3_vfs;
		using sqlite3_syscall_ptr = void (*)();

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

		[[nodiscard]] bool retained_parent_anchor(const std::string_view path) noexcept
		{
			constexpr std::string_view prefix{"/proc/self/fd/"};
			if (!path.starts_with(prefix) || path.size() == prefix.size())
				return false;
			const auto digits = path.substr(prefix.size());
			if ((digits.size() > 1U && digits.front() == '0') ||
				!std::ranges::all_of(digits,
									 [](const char value)
									 {
										 return value >= '0' && value <= '9';
									 }))
				return false;
			std::uint64_t descriptor{};
			const auto [end, error] =
				std::from_chars(digits.data(), digits.data() + digits.size(), descriptor);
			return error == std::errc{} && end == digits.data() + digits.size() &&
				descriptor <= static_cast<std::uint64_t>(std::numeric_limits<int>::max());
		}

		[[nodiscard]] int open_directory(const char* path) noexcept
		{
			if (path == nullptr || !retained_parent_anchor(path))
				return -1;
			for (;;)
			{
				const auto output = ::open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] int
		open_at(const int parent, const char* leaf, const int flags, const mode_t mode = 0) noexcept
		{
			for (;;)
			{
				const auto output =
					mode == 0 ? ::openat(parent, leaf, flags) : ::openat(parent, leaf, flags, mode);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] bool same_object(const struct stat& left, const struct stat& right) noexcept
		{
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
				(left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
		}

		[[nodiscard]] sqlite_backend_opaque_identity parent_identity(const struct stat& value)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.parent-namespace.v1";
			append_u64(output.bytes, static_cast<std::uint64_t>(value.st_dev));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.st_ino));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.st_mode & S_IFMT));
			return output;
		}

		struct filesystem_profile
		{
			dev_t parent_device{};
			std::uint64_t mount_id{};
			decltype(statfs::f_type) type{};
			decltype(statfs::f_bsize) block_size{};
			decltype(statfs::f_namelen) name_length{};
			decltype(statfs::f_flags) flags{};
			int fsid_first{};
			int fsid_second{};

			[[nodiscard]] bool operator==(const filesystem_profile&) const = default;
		};

		[[nodiscard]] result<filesystem_profile>
		read_filesystem_profile(const int descriptor, const struct stat& identity)
		{
			struct statfs observed
			{
			};
			struct statx mount
			{
			};
			if (::fstatfs(descriptor, &observed) != 0 ||
				::statx(
					descriptor, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &mount) !=
					0 ||
				(mount.stx_mask & STATX_MNT_ID) == 0U)
				return unexpected(qualification_error());
			return filesystem_profile{
				identity.st_dev,
				mount.stx_mnt_id,
				observed.f_type,
				observed.f_bsize,
				observed.f_namelen,
				observed.f_flags,
				observed.f_fsid.__val[0],
				observed.f_fsid.__val[1],
			};
		}

		[[nodiscard]] std::string filesystem_profile_string(const filesystem_profile& value)
		{
			return "default-filesystem-v1.linux-statfs-v1:" +
				std::to_string(static_cast<std::uint64_t>(value.parent_device)) + ":" +
				std::to_string(value.mount_id) + ":" +
				std::to_string(static_cast<std::int64_t>(value.type)) + ":" +
				std::to_string(static_cast<std::int64_t>(value.block_size)) + ":" +
				std::to_string(static_cast<std::int64_t>(value.name_length)) + ":" +
				std::to_string(static_cast<std::int64_t>(value.flags)) + ":" +
				std::to_string(value.fsid_first) + ":" + std::to_string(value.fsid_second);
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		object_filesystem_profile_identity(const filesystem_profile& value)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.object-filesystem.v1";
			output.bytes.reserve(56U);
			append_u64(output.bytes, static_cast<std::uint64_t>(value.parent_device));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.type));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.block_size));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.name_length));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.flags));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.fsid_first));
			append_u64(output.bytes, static_cast<std::uint64_t>(value.fsid_second));
			return output;
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		object_mount_identity(const filesystem_profile& value)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.linux-mount-id.v1";
			output.bytes.reserve(8U);
			append_u64(output.bytes, value.mount_id);
			return output;
		}

		struct file_snapshot
		{
			dev_t device{};
			ino_t inode{};
			mode_t kind{};
			std::uint64_t byte_count{};
			std::string sha256;

			[[nodiscard]] bool operator==(const file_snapshot&) const = default;
		};

		[[nodiscard]] result<file_snapshot> capture_file_snapshot(const int parent,
																  const char* leaf)
		{
			owned_descriptor descriptor{
				open_at(parent, leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
			if (!descriptor)
				return unexpected(qualification_error());
			struct stat before
			{
			};
			if (::fstat(descriptor.get(), &before) != 0 || !S_ISREG(before.st_mode) ||
				before.st_size < 0 ||
				static_cast<std::uint64_t>(before.st_size) > maximum_fixture_file_bytes)
				return unexpected(qualification_error());

			sqlite_incremental_sha256 digest;
			std::array<std::byte, copy_buffer_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < static_cast<std::uint64_t>(before.st_size))
			{
				const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
					buffer.size(), static_cast<std::uint64_t>(before.st_size) - offset));
				ssize_t count{};
				do
				{
					count = ::pread(
						descriptor.get(), buffer.data(), requested, static_cast<off_t>(offset));
				} while (count < 0 && errno == EINTR);
				if (count <= 0 || static_cast<std::size_t>(count) > requested)
					return unexpected(qualification_error());
				if (auto updated =
						digest.update(std::span{buffer}.first(static_cast<std::size_t>(count)));
					!updated)
					return unexpected(qualification_error());
				offset += static_cast<std::uint64_t>(count);
			}
			struct stat after
			{
			};
			if (::fstat(descriptor.get(), &after) != 0 || !same_object(before, after) ||
				before.st_size != after.st_size)
				return unexpected(qualification_error());
			auto sealed = digest.finish();
			if (!sealed)
				return unexpected(qualification_error());
			return file_snapshot{
				before.st_dev,
				before.st_ino,
				static_cast<mode_t>(before.st_mode & S_IFMT),
				static_cast<std::uint64_t>(before.st_size),
				std::move(*sealed),
			};
		}

		[[nodiscard]] result<void> copy_regular_file(const int source_parent,
													 const char* source_leaf,
													 const int destination_parent,
													 const char* destination_leaf)
		{
			owned_descriptor source{open_at(
				source_parent, source_leaf, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC)};
			if (!source)
				return unexpected(qualification_error());
			struct stat source_before
			{
			};
			if (::fstat(source.get(), &source_before) != 0 || !S_ISREG(source_before.st_mode) ||
				source_before.st_size < 0 ||
				static_cast<std::uint64_t>(source_before.st_size) > maximum_fixture_file_bytes)
				return unexpected(qualification_error());
			owned_descriptor destination{
				open_at(destination_parent,
						destination_leaf,
						O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
						static_cast<mode_t>(0600))};
			if (!destination)
				return unexpected(qualification_error());
			std::array<std::byte, copy_buffer_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < static_cast<std::uint64_t>(source_before.st_size))
			{
				const auto requested = static_cast<std::size_t>(std::min<std::uint64_t>(
					buffer.size(), static_cast<std::uint64_t>(source_before.st_size) - offset));
				ssize_t count{};
				do
				{
					count =
						::pread(source.get(), buffer.data(), requested, static_cast<off_t>(offset));
				} while (count < 0 && errno == EINTR);
				if (count <= 0 || static_cast<std::size_t>(count) > requested)
					return unexpected(qualification_error());
				std::size_t written{};
				while (written < static_cast<std::size_t>(count))
				{
					ssize_t output{};
					do
					{
						output = ::write(destination.get(),
										 buffer.data() + written,
										 static_cast<std::size_t>(count) - written);
					} while (output < 0 && errno == EINTR);
					if (output <= 0)
						return unexpected(qualification_error());
					written += static_cast<std::size_t>(output);
				}
				offset += static_cast<std::uint64_t>(count);
			}
			if (::fdatasync(destination.get()) != 0)
				return unexpected(qualification_error());
			struct stat source_after
			{
			};
			struct stat destination_status
			{
			};
			if (::fstat(source.get(), &source_after) != 0 ||
				::fstat(destination.get(), &destination_status) != 0 ||
				!same_object(source_before, source_after) ||
				source_before.st_size != source_after.st_size ||
				destination_status.st_size != source_before.st_size ||
				!S_ISREG(destination_status.st_mode))
				return unexpected(qualification_error());
			return {};
		}

		struct family_snapshot
		{
			file_snapshot main;
			file_snapshot wal;
			file_snapshot shm;

			[[nodiscard]] bool operator==(const family_snapshot&) const = default;
		};

		[[nodiscard]] result<family_snapshot> capture_family(const int parent)
		{
			auto main = capture_file_snapshot(parent, "main.db");
			auto wal = capture_file_snapshot(parent, "main.db-wal");
			auto shm = capture_file_snapshot(parent, "main.db-shm");
			if (!main || !wal || !shm)
				return unexpected(qualification_error());
			return family_snapshot{std::move(*main), std::move(*wal), std::move(*shm)};
		}

		[[nodiscard]] result<void> validate_family_directory(const int directory)
		{
			// fdopendir/readdir advance their open-file-description offset.  A duplicated file
			// descriptor would therefore poison every later exact census of the same directory.
			const auto census_descriptor =
				::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (census_descriptor < 0)
				return unexpected(qualification_error());
			auto* stream = ::fdopendir(census_descriptor);
			if (stream == nullptr)
			{
				(void)::close(census_descriptor);
				return unexpected(qualification_error());
			}
			std::vector<std::string> names;
			errno = 0;
			while (const auto* entry = ::readdir(stream))
			{
				const std::string_view name{entry->d_name};
				if (name != "." && name != "..")
					names.emplace_back(name);
				errno = 0;
			}
			const auto read_error = errno;
			if (::closedir(stream) != 0 || read_error != 0)
				return unexpected(qualification_error());
			std::ranges::sort(names);
			constexpr std::array expected{
				std::string_view{"main.db"},
				std::string_view{"main.db-shm"},
				std::string_view{"main.db-wal"},
			};
			return names.size() == expected.size() &&
					std::ranges::equal(names,
									   expected,
									   {},
									   [](const std::string& value)
									   {
										   return std::string_view{value};
									   })
				? result<void>{}
				: result<void>{qualification_error()};
		}

		class directory_mutation_watch
		{
		  public:
			static result<directory_mutation_watch> create(const std::string& path)
			{
				owned_descriptor descriptor{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
				if (!descriptor)
					return unexpected(qualification_error());
				constexpr std::uint32_t mask = IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_CREATE |
					IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO;
				const auto watch = ::inotify_add_watch(descriptor.get(), path.c_str(), mask);
				if (watch < 0)
					return unexpected(qualification_error());
				return directory_mutation_watch{std::move(descriptor), watch};
			}

			directory_mutation_watch(directory_mutation_watch&&) noexcept = default;
			directory_mutation_watch& operator=(directory_mutation_watch&&) noexcept = default;
			directory_mutation_watch(const directory_mutation_watch&) = delete;
			directory_mutation_watch& operator=(const directory_mutation_watch&) = delete;

			[[nodiscard]] result<void> require_no_events()
			{
				std::array<std::byte, 4096U> buffer{};
				for (;;)
				{
					ssize_t count{};
					do
					{
						count = ::read(descriptor_.get(), buffer.data(), buffer.size());
					} while (count < 0 && errno == EINTR);
					if (count > 0)
						return unexpected(qualification_error());
					if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
						return {};
					return unexpected(qualification_error());
				}
			}

			[[nodiscard]] result<void> finish()
			{
				if (watch_ < 0 || !descriptor_)
					return unexpected(qualification_error());
				if (auto unchanged = require_no_events(); !unchanged)
					return unchanged;
				if (::inotify_rm_watch(descriptor_.get(), watch_) != 0)
					return unexpected(qualification_error());
				watch_ = -1;
				descriptor_ = owned_descriptor{};
				return {};
			}

		  private:
			directory_mutation_watch(owned_descriptor descriptor, const int watch) noexcept
				: descriptor_{std::move(descriptor)}, watch_{watch}
			{
			}

			owned_descriptor descriptor_;
			int watch_{-1};
		};

		[[nodiscard]] bool unlink_if_present(const int parent, const char* leaf) noexcept
		{
			if (::unlinkat(parent, leaf, 0) == 0)
				return true;
			return errno == ENOENT;
		}

		class scratch_tree
		{
		  public:
			static result<scratch_tree>
			create(const std::string& parent_path,
				   const sqlite_backend_opaque_identity& expected_parent)
			{
				owned_descriptor parent{open_directory(parent_path.c_str())};
				if (!parent)
					return unexpected(qualification_error());
				struct stat parent_status
				{
				};
				if (::fstat(parent.get(), &parent_status) != 0 || !S_ISDIR(parent_status.st_mode) ||
					parent_identity(parent_status) != expected_parent)
					return unexpected(qualification_error());
				auto target_profile = read_filesystem_profile(parent.get(), parent_status);
				if (!target_profile)
					return unexpected(qualification_error());

				for (std::size_t attempt{}; attempt < 64U; ++attempt)
				{
					const auto sequence = next_name_.fetch_add(1U, std::memory_order_relaxed);
					auto leaf = ".cxxlens-sqlite-shm-preflight-" +
						std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
						std::to_string(sequence);
					if (::mkdirat(parent.get(), leaf.c_str(), 0700) != 0)
					{
						if (errno == EEXIST)
							continue;
						return unexpected(qualification_error());
					}
					owned_descriptor root{open_at(parent.get(),
												  leaf.c_str(),
												  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
					if (!root)
					{
						(void)::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR);
						return unexpected(qualification_error());
					}
					struct stat root_status
					{
					};
					if (::fstat(root.get(), &root_status) != 0 || !S_ISDIR(root_status.st_mode) ||
						(root_status.st_mode & 0777) != 0700)
					{
						(void)::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR);
						return unexpected(qualification_error());
					}
					auto scratch_profile = read_filesystem_profile(root.get(), root_status);
					if (!scratch_profile || *scratch_profile != *target_profile)
					{
						(void)::unlinkat(parent.get(), leaf.c_str(), AT_REMOVEDIR);
						return unexpected(qualification_error());
					}
					if (::mkdirat(root.get(), "producer", 0700) != 0 ||
						::mkdirat(root.get(), "cold", 0700) != 0 ||
						::mkdirat(root.get(), "active", 0700) != 0)
					{
						scratch_tree partial{std::move(parent),
											 std::move(root),
											 parent_path,
											 std::move(leaf),
											 parent_status,
											 *target_profile};
						(void)partial.cleanup();
						return unexpected(qualification_error());
					}
					return scratch_tree{std::move(parent),
										std::move(root),
										parent_path,
										std::move(leaf),
										parent_status,
										*target_profile};
				}
				return unexpected(qualification_error());
			}

			scratch_tree(scratch_tree&& other) noexcept
				: parent_{std::move(other.parent_)}, root_{std::move(other.root_)},
				  parent_path_{std::move(other.parent_path_)}, leaf_{std::move(other.leaf_)},
				  parent_status_{other.parent_status_}, profile_{other.profile_},
				  namespace_removed_{other.namespace_removed_},
				  cleaned_{std::exchange(other.cleaned_, true)}
			{
			}
			scratch_tree& operator=(scratch_tree&&) = delete;
			scratch_tree(const scratch_tree&) = delete;
			scratch_tree& operator=(const scratch_tree&) = delete;
			~scratch_tree()
			{
				(void)cleanup();
			}

			[[nodiscard]] int root_descriptor() const noexcept
			{
				return root_.get();
			}
			[[nodiscard]] const filesystem_profile& profile() const noexcept
			{
				return profile_;
			}
			[[nodiscard]] std::string canonical_namespace_path(const std::string_view suffix) const
			{
				auto output = parent_path_;
				if (output.empty() || output.back() != '/')
					output.push_back('/');
				output.append(leaf_);
				if (!suffix.empty())
				{
					output.push_back('/');
					output.append(suffix);
				}
				return output;
			}
			[[nodiscard]] std::string retained_root_watch_path() const
			{
				return "/proc/self/fd/" + std::to_string(root_.get());
			}
			[[nodiscard]] std::string descriptor_path(const std::string_view suffix) const
			{
				auto output = retained_root_watch_path();
				if (!suffix.empty())
				{
					output.push_back('/');
					output.append(suffix);
				}
				return output;
			}
			[[nodiscard]] result<void> recheck_namespace_entry() const
			{
				struct stat retained_root
				{
				};
				struct stat named_root
				{
				};
				struct stat current_parent
				{
				};
				if (!root_ || !parent_ || ::fstat(root_.get(), &retained_root) != 0 ||
					::fstat(parent_.get(), &current_parent) != 0 ||
					::fstatat(parent_.get(), leaf_.c_str(), &named_root, AT_SYMLINK_NOFOLLOW) !=
						0 ||
					!S_ISDIR(retained_root.st_mode) || !S_ISDIR(named_root.st_mode) ||
					(retained_root.st_mode & 0777) != 0700 ||
					!same_object(retained_root, named_root) ||
					!same_object(parent_status_, current_parent) ||
					parent_identity(current_parent) != parent_identity(parent_status_))
					return unexpected(qualification_error());
				return {};
			}
			[[nodiscard]] result<owned_descriptor> open_child(const char* name) const
			{
				owned_descriptor output{
					open_at(root_.get(), name, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
				if (!output)
					return unexpected(qualification_error());
				return std::move(output);
			}

			[[nodiscard]] result<void> cleanup()
			{
				if (cleaned_)
					return {};
				if (!namespace_removed_)
				{
					if (auto entry = recheck_namespace_entry(); !entry)
						return unexpected(qualification_error());
					bool complete = true;
					for (const auto* directory : {"producer", "cold", "active"})
					{
						owned_descriptor child{
							open_at(root_.get(),
									directory,
									O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC)};
						if (child)
						{
							for (const auto* name :
								 {"main.db", "main.db-wal", "main.db-shm", "main.db-journal"})
								complete = unlink_if_present(child.get(), name) && complete;
						}
						else if (errno != ENOENT)
							complete = false;
						if (::unlinkat(root_.get(), directory, AT_REMOVEDIR) != 0 &&
							errno != ENOENT)
							complete = false;
					}
					complete = unlink_if_present(root_.get(), "origin-probe.db") && complete;
					if (!complete || ::unlinkat(parent_.get(), leaf_.c_str(), AT_REMOVEDIR) != 0)
						return unexpected(qualification_error());
					namespace_removed_ = true;
					root_ = owned_descriptor{};
				}
				if (::fsync(parent_.get()) != 0)
					return unexpected(qualification_error());
				struct stat current_parent
				{
				};
				if (::fstat(parent_.get(), &current_parent) != 0 ||
					!same_object(parent_status_, current_parent) ||
					parent_identity(current_parent) != parent_identity(parent_status_))
					return unexpected(qualification_error());
				struct stat removed
				{
				};
				if (::fstatat(parent_.get(), leaf_.c_str(), &removed, AT_SYMLINK_NOFOLLOW) == 0 ||
					errno != ENOENT)
					return unexpected(qualification_error());
				cleaned_ = true;
				return {};
			}

		  private:
			scratch_tree(owned_descriptor parent,
						 owned_descriptor root,
						 std::string parent_path,
						 std::string leaf,
						 const struct stat& parent_status,
						 filesystem_profile profile)
				: parent_{std::move(parent)}, root_{std::move(root)},
				  parent_path_{std::move(parent_path)}, leaf_{std::move(leaf)},
				  parent_status_{parent_status}, profile_{profile}
			{
			}

			inline static std::atomic<std::uint64_t> next_name_{1U};
			owned_descriptor parent_;
			owned_descriptor root_;
			std::string parent_path_;
			std::string leaf_;
			struct stat parent_status_
			{
			};
			filesystem_profile profile_;
			bool namespace_removed_{};
			bool cleaned_{};
		};

		class default_source_shm_target_namespace_epoch final
			: public sqlite_source_shm_target_namespace_epoch
		{
		  public:
			static result<std::shared_ptr<sqlite_source_shm_target_namespace_epoch>>
			create(const std::string& logical_main_locator,
				   const sqlite_backend_namespace_census& census)
			{
				try
				{
					auto guard = census.source_shm_guard;
					if (!guard || guard->logical_main_locator() != logical_main_locator ||
						guard->anchored_main_locator().empty() ||
						guard->identity().profile.empty() || guard->identity().bytes.empty() ||
						!guard->recheck())
						return unexpected(qualification_error());

					constexpr std::array roles{
						sqlite_backend_file_role::main_database,
						sqlite_backend_file_role::write_ahead_log,
						sqlite_backend_file_role::shared_memory,
					};
					for (const auto role : roles)
					{
						const sqlite_backend_entry_observation* selected{};
						for (const auto& candidate : census.entries)
							if (candidate.role == role)
							{
								if (selected != nullptr)
									return unexpected(qualification_error());
								selected = &candidate;
							}
						auto retained = guard->retained_entry(role);
						if (selected == nullptr ||
							selected->state != sqlite_backend_entry_state::held_regular ||
							!selected->object_identity || !selected->directory_entry_identity ||
							!selected->object_filesystem_profile ||
							!selected->direct_regular_entry || !selected->held_object ||
							!selected->held_object->object_mount_identity() || !retained ||
							retained->state != sqlite_backend_entry_state::held_regular ||
							retained->object_identity != selected->object_identity ||
							retained->directory_entry_identity !=
								selected->directory_entry_identity ||
							retained->object_filesystem_profile !=
								selected->object_filesystem_profile ||
							retained->held_object.get() != selected->held_object.get() ||
							!retained->direct_regular_entry)
							return unexpected(qualification_error());
					}
					if (auto claimed = guard->claim_target_epoch(); !claimed)
						return unexpected(qualification_error());

					sqlite_backend_opaque_identity identity;
					identity.profile =
						"sqlite-source-shm-readonly-unix-uri-v1.target-namespace-epoch.v1";
					append_bytes(identity.bytes, logical_main_locator);
					append_bytes(identity.bytes, guard->anchored_main_locator());
					append_opaque(identity.bytes, census.parent_namespace_identity);
					append_opaque(identity.bytes, guard->identity());

					auto output = std::shared_ptr<default_source_shm_target_namespace_epoch>(
						new default_source_shm_target_namespace_epoch(
							std::move(guard),
							std::string{logical_main_locator},
							std::string{census.source_shm_guard->anchored_main_locator()},
							std::move(identity)));
					if (auto checked = output->recheck(); !checked)
						return unexpected(std::move(checked.error()));
					return std::static_pointer_cast<sqlite_source_shm_target_namespace_epoch>(
						std::move(output));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(qualification_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(qualification_error());
				}
			}

			~default_source_shm_target_namespace_epoch() override = default;

			[[nodiscard]] std::string_view logical_main_locator() const noexcept override
			{
				return logical_main_locator_;
			}
			[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
			{
				return anchored_main_locator_;
			}
			[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept override
			{
				return identity_;
			}
			[[nodiscard]] result<sqlite_backend_entry_observation>
			retained_entry(const sqlite_backend_file_role role) const override
			{
				std::scoped_lock lock{mutex_};
				if (auto checked = recheck_locked(); !checked)
					return unexpected(std::move(checked.error()));
				return guard_->retained_entry(role);
			}

			[[nodiscard]] result<void> recheck() const override
			{
				std::scoped_lock lock{mutex_};
				return recheck_locked();
			}

			[[nodiscard]] result<void> finish() override
			{
				std::scoped_lock lock{mutex_};
				if (auto checked = recheck_locked(); !checked)
					return checked;
				if (!guard_->finish())
					return unexpected(qualification_error());
				finished_ = true;
				return {};
			}

		  private:
			default_source_shm_target_namespace_epoch(
				std::shared_ptr<sqlite_source_shm_namespace_guard> guard,
				std::string logical_main_locator,
				std::string anchored_main_locator,
				sqlite_backend_opaque_identity identity)
				: guard_{std::move(guard)}, logical_main_locator_{std::move(logical_main_locator)},
				  anchored_main_locator_{std::move(anchored_main_locator)},
				  identity_{std::move(identity)}
			{
			}

			[[nodiscard]] result<void> recheck_locked() const
			{
				if (finished_ || !guard_ || !guard_->recheck())
					return unexpected(qualification_error());
				return {};
			}

			std::shared_ptr<sqlite_source_shm_namespace_guard> guard_;
			std::string logical_main_locator_;
			std::string anchored_main_locator_;
			sqlite_backend_opaque_identity identity_;
			mutable std::mutex mutex_;
			mutable bool finished_{};
		};

		[[nodiscard]] result<void>
		validate_target_census(const sqlite_backend_namespace_census& census,
							   const sqlite_backend_opaque_identity& capability_token,
							   const sqlite_backend_opaque_identity& expected_parent,
							   const sqlite_backend_opaque_identity& expected_filesystem)
		{
			if (census.profile != observation_profile ||
				census.capability_token != capability_token ||
				census.parent_namespace_identity != expected_parent || !census.source_shm_guard ||
				census.source_shm_guard->identity().profile.empty() ||
				census.source_shm_guard->identity().bytes.empty())
				return unexpected(qualification_error());
			constexpr std::array required_roles{
				sqlite_backend_file_role::main_database,
				sqlite_backend_file_role::write_ahead_log,
				sqlite_backend_file_role::shared_memory,
				sqlite_backend_file_role::rollback_journal,
			};
			for (const auto role : required_roles)
			{
				const sqlite_backend_entry_observation* selected{};
				for (const auto& entry : census.entries)
					if (entry.role == role)
					{
						if (selected != nullptr)
							return unexpected(qualification_error());
						selected = &entry;
					}
				if (selected == nullptr)
					return unexpected(qualification_error());
				auto retained = census.source_shm_guard->retained_entry(role);
				if (!retained || retained->role != selected->role ||
					retained->state != selected->state ||
					retained->object_identity != selected->object_identity ||
					retained->directory_entry_identity != selected->directory_entry_identity ||
					retained->held_object.get() != selected->held_object.get() ||
					retained->object_filesystem_profile != selected->object_filesystem_profile ||
					retained->direct_regular_entry != selected->direct_regular_entry)
					return unexpected(qualification_error());
				if (role == sqlite_backend_file_role::rollback_journal)
				{
					if (selected->state != sqlite_backend_entry_state::absent ||
						selected->object_identity || selected->directory_entry_identity ||
						selected->held_object || selected->object_filesystem_profile)
						return unexpected(qualification_error());
					continue;
				}
				if (selected->state != sqlite_backend_entry_state::held_regular ||
					!selected->object_identity || !selected->directory_entry_identity ||
					!selected->held_object || !selected->object_filesystem_profile ||
					!selected->held_object->object_mount_identity() ||
					!selected->direct_regular_entry ||
					*selected->object_filesystem_profile != expected_filesystem ||
					!selected->held_object->object_filesystem_profile() ||
					*selected->held_object->object_filesystem_profile() != expected_filesystem ||
					selected->held_object->role() != selected->role ||
					selected->held_object->object_identity() != *selected->object_identity ||
					selected->held_object->directory_entry_identity() !=
						*selected->directory_entry_identity)
					return unexpected(qualification_error());
			}
			return census.source_shm_guard->recheck();
		}

		template <class Function>
		[[nodiscard]] const void* function_address(const Function function) noexcept
		{
			static_assert(std::is_pointer_v<Function>);
			static_assert(sizeof(Function) == sizeof(const void*));
			return std::bit_cast<const void*>(function);
		}

		template <class Function>
		[[nodiscard]] bool function_from_image(const Function function, const void* image) noexcept
		{
			if (function == nullptr || image == nullptr)
				return false;
			Dl_info information{};
			return ::dladdr(function_address(function), &information) != 0 &&
				information.dli_fbase == image;
		}

		[[nodiscard]] bool validate_runtime_image(const sqlite_source_shm_runtime_binding& runtime)
		{
			return runtime.runtime_identity != nullptr &&
				runtime.runtime_image_identity != nullptr &&
				runtime.runtime_lifetime_identity != nullptr && runtime.runtime_lifetime &&
				runtime.runtime_lifetime.get() == runtime.runtime_lifetime_identity &&
				function_from_image(runtime.open_v2, runtime.runtime_image_identity) &&
				function_from_image(runtime.close_v2, runtime.runtime_image_identity) &&
				function_from_image(runtime.exec, runtime.runtime_image_identity) &&
				function_from_image(runtime.errmsg, runtime.runtime_image_identity) &&
				function_from_image(runtime.free_memory, runtime.runtime_image_identity) &&
				function_from_image(runtime.source_id, runtime.runtime_image_identity) &&
				function_from_image(runtime.uri_parameter, runtime.runtime_image_identity) &&
				function_from_image(runtime.uri_key, runtime.runtime_image_identity) &&
				function_from_image(runtime.vfs_find, runtime.runtime_image_identity) &&
				function_from_image(runtime.vfs_register, runtime.runtime_image_identity) &&
				function_from_image(runtime.vfs_unregister, runtime.runtime_image_identity);
		}

		[[nodiscard]] result<void>
		validate_underlying_callback_image(sqlite3_vfs& underlying,
										   const sqlite_source_shm_runtime_binding& runtime,
										   const std::string& scratch_probe_path)
		{
			if (underlying.version < 1 || underlying.os_file_bytes <= 0 ||
				underlying.os_file_bytes > 1024 * 1024 || underlying.name == nullptr ||
				underlying.name[0] == '\0' || underlying.app_data == nullptr ||
				underlying.open == nullptr ||
				!function_from_image(underlying.open, runtime.runtime_image_identity))
				return unexpected(qualification_error());
			const auto storage_count = (static_cast<std::size_t>(underlying.os_file_bytes) +
										sizeof(std::max_align_t) - 1U) /
				sizeof(std::max_align_t);
			auto storage = std::make_unique<std::max_align_t[]>(storage_count);
			std::memset(storage.get(), 0, storage_count * sizeof(std::max_align_t));
			auto* file = reinterpret_cast<sqlite3_file*>(storage.get());
			int returned_flags{};
			const auto open_status =
				underlying.open(&underlying,
								scratch_probe_path.c_str(),
								file,
								sqlite_open_read_only | sqlite_open_main_database,
								&returned_flags);
			if (open_status != sqlite_ok)
			{
				if (file->methods != nullptr)
					(void)storage
						.release(); // Unproven ownership/callback: retain storage permanently.
				return unexpected(qualification_error());
			}
			if (file->methods == nullptr)
			{
				(void)storage.release(); // Successful xOpen without a proven xClose is quarantined.
				return unexpected(qualification_error());
			}
			const auto* methods = file->methods;
			if (methods->close == nullptr ||
				!function_from_image(methods->close, runtime.runtime_image_identity))
			{
				(void)storage.release();
				return unexpected(qualification_error());
			}
			const auto valid = methods->version >= 2 && methods->shm_map != nullptr &&
				methods->shm_lock != nullptr && methods->shm_unmap != nullptr &&
				function_from_image(methods->shm_map, runtime.runtime_image_identity) &&
				function_from_image(methods->shm_lock, runtime.runtime_image_identity) &&
				function_from_image(methods->shm_unmap, runtime.runtime_image_identity);
			const auto close_status = methods->close(file);
			if (close_status != sqlite_ok)
				(void)storage.release();
			if (!valid || close_status != sqlite_ok)
				return unexpected(qualification_error());
			return {};
		}

		struct sentinel_query_evidence
		{
			bool called{};
			bool invalid{};
		};

		int sentinel_query_callback(void* opaque, const int count, char** values, char**) noexcept
		{
			auto* evidence = static_cast<sentinel_query_evidence*>(opaque);
			if (evidence == nullptr || evidence->called || count != 2 || values == nullptr ||
				values[0] == nullptr || values[1] == nullptr ||
				std::string_view{values[0]} != "5000" || std::string_view{values[1]} != "2000000")
			{
				if (evidence != nullptr)
					evidence->invalid = true;
				return 1;
			}
			evidence->called = true;
			return 0;
		}

		[[nodiscard]] result<void>
		execute_sql(const sqlite_source_shm_runtime_binding& runtime,
					void* database,
					const char* sql,
					sqlite_source_shm_runtime_binding::exec_callback callback = nullptr,
					void* callback_context = nullptr)
		{
			char* message{};
			const auto status = runtime.exec(database, sql, callback, callback_context, &message);
			if (message != nullptr)
				runtime.free_memory(message);
			return status == sqlite_ok ? result<void>{} : result<void>{qualification_error()};
		}

		[[nodiscard]] sqlite_connection_lifetime_pins connection_pins(
			const sqlite_source_shm_runtime_binding& runtime,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_connection_observation_scope>& observation = {})
		{
			return sqlite_connection_lifetime_pins{
				runtime.runtime_lifetime,
				backend_lifetime,
				observation,
				observation,
			};
		}

		[[nodiscard]] result<void> close_connection(sqlite_connection_lifecycle& connection)
		{
			auto outcome = connection.close_exactly_once();
			const auto* closed = std::get_if<sqlite_confirmed_close_token>(&outcome);
			if (closed == nullptr || !closed->valid() ||
				closed->kind() != sqlite_confirmed_close_kind::sqlite_ok)
				return unexpected(qualification_error());
			return {};
		}

		[[nodiscard]] result<sqlite_connection_lifecycle> open_connection(
			const sqlite_source_shm_runtime_binding& runtime,
			const std::shared_ptr<void>& backend_lifetime,
			const std::string& locator,
			const int flags,
			const std::string& vfs_name,
			std::shared_ptr<sqlite_backend_connection_observation_scope> observation = {})
		{
			sqlite_connection_lifecycle connection{
				nullptr,
				runtime.close_v2,
				connection_pins(runtime, backend_lifetime, observation),
			};
			const auto status = runtime.open_v2(
				locator.c_str(), connection.open_handle_out_parameter(), flags, vfs_name.c_str());
			if (status != sqlite_ok || !connection.owns_connection())
			{
				(void)connection.close_exactly_once();
				return unexpected(qualification_error());
			}
			return std::move(connection);
		}

		[[nodiscard]] result<void>
		create_fixture(const sqlite_source_shm_runtime_binding& runtime,
					   const std::shared_ptr<void>& backend_lifetime,
					   const std::string& path,
					   const std::string& vfs_name,
					   sqlite_connection_lifecycle& output,
					   std::shared_ptr<sqlite_backend_connection_observation_scope> observation)
		{
			auto opened = open_connection(runtime,
										  backend_lifetime,
										  path,
										  fixture_write_flags,
										  vfs_name,
										  std::move(observation));
			if (!opened)
				return unexpected(qualification_error());
			output = std::move(*opened);
			constexpr const char* fixture_sql =
				"PRAGMA page_size=512;"
				"VACUUM;"
				"CREATE TABLE cxxlens_preflight_sentinel("
				"id INTEGER PRIMARY KEY,payload BLOB NOT NULL);"
				"PRAGMA journal_mode=WAL;"
				"PRAGMA wal_autocheckpoint=0;"
				"BEGIN IMMEDIATE;"
				"WITH digits(value) AS (VALUES(0),(1),(2),(3),(4),(5),(6),(7),(8),(9)),"
				"sequence(value) AS ("
				"SELECT a.value+10*b.value+100*c.value+1000*d.value "
				"FROM digits a,digits b,digits c,digits d) "
				"INSERT INTO cxxlens_preflight_sentinel(id,payload) "
				"SELECT value+1,zeroblob(400) FROM sequence WHERE value<5000 ORDER BY value;"
				"COMMIT;";
			return execute_sql(runtime, output.get(), fixture_sql);
		}

		[[nodiscard]] bool
		map_events_prove_candidate(const sqlite_backend_connection_observation& observation,
								   const sqlite_source_shm_qualification_request& request,
								   const std::string_view expected_locator,
								   const std::string_view expected_uri,
								   const bool cold_route,
								   const bool require_later_map)
		{
			if (!observation.complete || !observation.source_shm_open_callback_receipt ||
				observation.shm_map_events.empty() ||
				(require_later_map && observation.shm_map_events.size() < 2U))
				return false;
			const sqlite_backend_open_observation* main_open{};
			for (const auto& event : observation.open_events)
				if (event.role == sqlite_backend_file_role::main_database)
				{
					if (main_open != nullptr)
						return false;
					main_open = &event;
				}
			if (main_open == nullptr || main_open->input_flags != source_shm_main_xopen_flags ||
				main_open->outcome != sqlite_backend_open_outcome::succeeded ||
				!main_open->returned_flags ||
				(*main_open->returned_flags & sqlite_open_read_only) == 0 ||
				(*main_open->returned_flags & sqlite_open_read_write) != 0)
				return false;
			const auto& callback = *observation.source_shm_open_callback_receipt;
			if (callback.profile != qualification_candidate_profile ||
				callback.connection_token != observation.connection_token ||
				callback.runtime_identity != request.runtime.runtime_identity ||
				callback.forwarding_vfs_identity != request.forwarding_vfs_identity ||
				callback.pinned_underlying_vfs_identity != request.pinned_underlying_vfs_identity ||
				callback.pinned_underlying_vfs_app_data_identity !=
					request.pinned_underlying_vfs_app_data_identity ||
				callback.qualification_token != request.observation_capability_token ||
				callback.canonical_vfs_locator != expected_locator ||
				callback.application_generated_uri != expected_uri ||
				callback.registered_vfs_name != request.registered_vfs_name ||
				callback.mode != "ro" || callback.cache != "private" ||
				callback.readonly_shm != "1" || callback.input_flags != source_shm_main_xopen_flags)
				return false;
			return validate_sqlite_source_shm_readonly_map_sequence(
				observation.shm_map_events,
				request.pinned_underlying_vfs_identity,
				request.pinned_underlying_vfs_app_data_identity,
				cold_route,
				require_later_map);
		}

		[[nodiscard]] bool
		held_read_lock_proves_route(const sqlite_backend_connection_observation& observation,
									const bool cold_route) noexcept
		{
			constexpr int wal_read_lock_zero = 3;
			constexpr int wal_read_lock_count = 5;
			if (observation.held_shm_locks.size() != 1U)
				return false;
			const auto& lock = observation.held_shm_locks.front();
			return lock.count == 1 && lock.mode == sqlite_backend_shm_lock_mode::shared &&
				(cold_route ? lock.offset == wal_read_lock_zero
							: lock.offset >= wal_read_lock_zero &&
						 lock.offset < wal_read_lock_zero + wal_read_lock_count);
		}

		struct route_proof
		{
			family_snapshot before;
			family_snapshot after;
			sqlite_backend_connection_observation observation;
			directory_mutation_watch mutation_watch;
		};

		[[nodiscard]] result<route_proof> exercise_readonly_route(
			const sqlite_source_shm_qualification_request& request,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<const sqlite_default_connection_observation_port>&
				connection_port,
			const int family_directory,
			const std::string& canonical_locator,
			const std::string& filesystem_profile_value,
			const bool active_mapped_route)
		{
			if (auto directory = validate_family_directory(family_directory); !directory)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-initial-census"));
			const auto separator = canonical_locator.rfind('/');
			if (separator == std::string::npos)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-locator"));
			auto mutation_watch =
				directory_mutation_watch::create(canonical_locator.substr(0U, separator));
			if (!mutation_watch)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-watch"));
			auto before = capture_family(family_directory);
			if (!before)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-before-snapshot"));

			owned_descriptor deadman_lock;
			if (active_mapped_route)
			{
				deadman_lock = owned_descriptor{
					open_at(family_directory, "main.db-shm", O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
				if (!deadman_lock)
					return unexpected(
						qualification_error("source-shm-readonly-qualification-route-dms-open"));
				struct flock lock
				{
				};
				lock.l_type = F_RDLCK;
				lock.l_whence = SEEK_SET;
				lock.l_start = unix_shm_deadman_switch_offset;
				lock.l_len = 1;
				if (::fcntl(deadman_lock.get(), F_OFD_SETLK, &lock) != 0)
					return unexpected(
						qualification_error("source-shm-readonly-qualification-route-dms-lock"));
			}

			auto uri = make_sqlite_source_shm_readonly_uri(canonical_locator);
			if (!uri)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-uri"));
			auto scope = connection_port->begin_source_shm_qualification_observation(
				canonical_locator, request.observation_capability_token);
			if (!scope || !*scope)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-observation"));
			auto* gate = (*scope)->effect_gate_port();
			if (gate == nullptr)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-gate"));
			auto denied = gate->activate_denied(
				request.observation_capability_token, (*scope)->token(), canonical_locator);
			if (!denied || denied->stage != sqlite_backend_effect_stage::denied)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-deny"));
			sqlite_source_shm_qualification_open_plan candidate{
				request.runtime,
				canonical_locator,
				filesystem_profile_value,
				*uri,
				request.registered_vfs_name,
				request.forwarding_vfs_identity,
				request.pinned_underlying_vfs_identity,
				request.pinned_underlying_vfs_app_data_identity,
				request.backend_lifetime_identity,
				request.observation_capability_token,
				qualified_read_flags,
			};
			if (auto armed =
					(*scope)->arm_source_shm_readonly_qualification_candidate(std::move(candidate));
				!armed)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-arm"));
			auto database = open_connection(request.runtime,
											backend_lifetime,
											*uri,
											qualified_read_flags,
											request.registered_vfs_name,
											*scope);
			if (!database)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-open"));
			sentinel_query_evidence query;
			if (auto executed = execute_sql(request.runtime,
											database->get(),
											"BEGIN;SELECT count(*),sum(length(payload)) "
											"FROM cxxlens_preflight_sentinel;",
											sentinel_query_callback,
											&query);
				!executed || !query.called || query.invalid)
			{
				(void)execute_sql(request.runtime, database->get(), "ROLLBACK;");
				(void)close_connection(*database);
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-query"));
			}
			auto observed = (*scope)->snapshot();
			if (!observed || !observed->main_handle_open ||
				!held_read_lock_proves_route(*observed, !active_mapped_route))
			{
				(void)execute_sql(request.runtime, database->get(), "ROLLBACK;");
				(void)close_connection(*database);
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-snapshot"));
			}
			if (!map_events_prove_candidate(*observed,
											request,
											canonical_locator,
											*uri,
											!active_mapped_route,
											active_mapped_route))
			{
				(void)execute_sql(request.runtime, database->get(), "ROLLBACK;");
				(void)close_connection(*database);
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-map-evidence"));
			}
			if (auto committed = execute_sql(request.runtime, database->get(), "COMMIT;");
				!committed)
			{
				(void)execute_sql(request.runtime, database->get(), "ROLLBACK;");
				(void)close_connection(*database);
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-commit"));
			}
			if (auto closed = close_connection(*database); !closed)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-close"));
			deadman_lock = owned_descriptor{};
			auto after = capture_family(family_directory);
			if (!after || *before != *after || !validate_family_directory(family_directory) ||
				!mutation_watch->require_no_events())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-route-nonmutation"));
			return route_proof{std::move(*before),
							   std::move(*after),
							   std::move(*observed),
							   std::move(*mutation_watch)};
		}

		void append_file_snapshot(std::vector<std::byte>& output, const file_snapshot& value)
		{
			append_u64(output, static_cast<std::uint64_t>(value.device));
			append_u64(output, static_cast<std::uint64_t>(value.inode));
			append_u64(output, static_cast<std::uint64_t>(value.kind));
			append_u64(output, value.byte_count);
			append_bytes(output, value.sha256);
		}

		void append_family_snapshot(std::vector<std::byte>& output, const family_snapshot& value)
		{
			append_file_snapshot(output, value.main);
			append_file_snapshot(output, value.wal);
			append_file_snapshot(output, value.shm);
		}

		void append_map_events(std::vector<std::byte>& output,
							   const sqlite_backend_connection_observation& observation)
		{
			append_u64(output, static_cast<std::uint64_t>(observation.held_shm_locks.size()));
			for (const auto& lock : observation.held_shm_locks)
			{
				append_u64(output, static_cast<std::uint64_t>(lock.offset));
				append_u64(output, static_cast<std::uint64_t>(lock.count));
				append_u64(output, static_cast<std::uint64_t>(lock.mode));
			}
			append_u64(output, static_cast<std::uint64_t>(observation.shm_map_events.size()));
			for (const auto& event : observation.shm_map_events)
			{
				append_u64(output, static_cast<std::uint64_t>(event.page));
				append_u64(output, static_cast<std::uint64_t>(event.page_size));
				append_u64(output, static_cast<std::uint64_t>(event.caller_extend));
				append_u64(output, static_cast<std::uint64_t>(event.delegated_extend));
				append_u64(output, static_cast<std::uint64_t>(event.native_status));
				append_u64(output, static_cast<std::uint64_t>(event.returned_status));
				append_u64(output, event.native_mapping_nonnull ? 1U : 0U);
				append_u64(output, event.returned_mapping_nonnull ? 1U : 0U);
				append_u64(output, event.readonly_family_seen_before ? 1U : 0U);
				append_u64(output, event.readonly_family_seen_after ? 1U : 0U);
			}
		}

		void append_namespace_census(std::vector<std::byte>& output,
									 const sqlite_backend_namespace_census& census)
		{
			append_bytes(output, census.profile);
			append_opaque(output, census.capability_token);
			append_opaque(output, census.parent_namespace_identity);
			for (const auto& entry : census.entries)
			{
				append_u64(output, static_cast<std::uint64_t>(entry.role));
				append_u64(output, static_cast<std::uint64_t>(entry.state));
				append_u64(output, entry.object_identity ? 1U : 0U);
				if (entry.object_identity)
					append_opaque(output, *entry.object_identity);
				append_u64(output, entry.directory_entry_identity ? 1U : 0U);
				if (entry.directory_entry_identity)
					append_opaque(output, *entry.directory_entry_identity);
				append_u64(output, entry.object_filesystem_profile ? 1U : 0U);
				if (entry.object_filesystem_profile)
					append_opaque(output, *entry.object_filesystem_profile);
				const auto mount = entry.held_object
					? entry.held_object->object_mount_identity()
					: std::optional<sqlite_backend_opaque_identity>{};
				append_u64(output, mount ? 1U : 0U);
				if (mount)
					append_opaque(output, *mount);
				append_u64(output, entry.direct_regular_entry ? 1U : 0U);
			}
		}

		class default_source_shm_readonly_preflight final : public sqlite_source_shm_readonly_port
		{
		  public:
			default_source_shm_readonly_preflight(const sqlite_default_observation_binding& binding,
												  sqlite_backend_opaque_identity capability_token,
												  std::string main_leaf)
				: canonical_locator_{binding.canonical_vfs_locator},
				  registered_vfs_name_{binding.registered_vfs_name},
				  forwarding_vfs_identity_{binding.forwarding_vfs_identity},
				  pinned_underlying_vfs_identity_{binding.pinned_underlying_vfs_identity},
				  pinned_underlying_vfs_app_data_identity_{
					  binding.pinned_underlying_vfs_app_data_identity},
				  backend_lifetime_{binding.backend_lifetime},
				  expected_runtime_identity_{binding.private_snapshot_registry.runtime_identity},
				  expected_runtime_lifetime_identity_{
					  binding.private_snapshot_registry.runtime_lifetime.get()},
				  expected_find_{binding.private_snapshot_registry.find},
				  expected_register_{binding.private_snapshot_registry.register_vfs},
				  expected_unregister_{binding.private_snapshot_registry.unregister_vfs},
				  connection_port_{binding.connection_observation_port},
				  capability_token_{std::move(capability_token)}, main_leaf_{std::move(main_leaf)}
			{
			}

			[[nodiscard]] result<sqlite_source_shm_qualified_open_plan>
			qualify(sqlite_source_shm_qualification_request request) override
			{
				try
				{
					std::scoped_lock lock{mutex_};
					return qualify_locked(std::move(request));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(qualification_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(qualification_error());
				}
			}

		  private:
			[[nodiscard]] result<sqlite_source_shm_qualified_open_plan>
			qualify_locked(sqlite_source_shm_qualification_request request);

			std::string canonical_locator_;
			std::string registered_vfs_name_;
			const void* forwarding_vfs_identity_{};
			const void* pinned_underlying_vfs_identity_{};
			const void* pinned_underlying_vfs_app_data_identity_{};
			std::shared_ptr<void> backend_lifetime_;
			const void* expected_runtime_identity_{};
			const void* expected_runtime_lifetime_identity_{};
			sqlite_private_snapshot_registry_binding::find_function expected_find_{};
			sqlite_private_snapshot_registry_binding::register_function expected_register_{};
			sqlite_private_snapshot_registry_binding::unregister_function expected_unregister_{};
			std::shared_ptr<const sqlite_default_connection_observation_port> connection_port_;
			sqlite_backend_opaque_identity capability_token_;
			std::string main_leaf_;
			std::mutex mutex_;
		};

		result<sqlite_source_shm_qualified_open_plan>
		default_source_shm_readonly_preflight::qualify_locked(
			sqlite_source_shm_qualification_request request)
		{
			if (request.canonical_vfs_locator != canonical_locator_ ||
				request.source_census.capability_token != capability_token_ ||
				request.parent_namespace_identity !=
					request.source_census.parent_namespace_identity ||
				request.registered_vfs_name != registered_vfs_name_ ||
				request.forwarding_vfs_identity != forwarding_vfs_identity_ ||
				request.pinned_underlying_vfs_identity != pinned_underlying_vfs_identity_ ||
				request.pinned_underlying_vfs_app_data_identity !=
					pinned_underlying_vfs_app_data_identity_ ||
				request.backend_lifetime_identity != backend_lifetime_.get() ||
				request.observation_capability_token != capability_token_ || !connection_port_ ||
				request.runtime.runtime_identity != expected_runtime_identity_ ||
				request.runtime.runtime_lifetime_identity != expected_runtime_lifetime_identity_ ||
				request.runtime.vfs_find != expected_find_ ||
				request.runtime.vfs_register != expected_register_ ||
				request.runtime.vfs_unregister != expected_unregister_ ||
				!validate_runtime_image(request.runtime) ||
				request.runtime.vfs_find(registered_vfs_name_.c_str()) != forwarding_vfs_identity_)
				return unexpected(qualification_error("source-shm-readonly-qualification-binding"));
			const auto callback_binding = connection_port_->binding();
			if (callback_binding.registered_vfs_name != registered_vfs_name_ ||
				callback_binding.forwarding_vfs_identity != forwarding_vfs_identity_ ||
				callback_binding.pinned_underlying_vfs_identity !=
					pinned_underlying_vfs_identity_ ||
				callback_binding.pinned_underlying_vfs_app_data_identity !=
					pinned_underlying_vfs_app_data_identity_ ||
				callback_binding.backend_lifetime_identity != backend_lifetime_.get())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-callback-binding"));

			const auto* first_source_id = request.runtime.source_id();
			if (first_source_id == nullptr)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-source-id"));
			const auto source_id_size = ::strnlen(first_source_id, maximum_source_id_bytes + 1U);
			if (source_id_size == 0U || source_id_size > maximum_source_id_bytes)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-source-id"));
			std::string source_id{first_source_id, source_id_size};
			const auto* second_source_id = request.runtime.source_id();
			if (second_source_id == nullptr)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-source-id"));
			const auto second_source_id_size =
				::strnlen(second_source_id, maximum_source_id_bytes + 1U);
			if (second_source_id_size != source_id_size ||
				std::string_view{second_source_id, second_source_id_size} != source_id)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-source-id"));

			auto target_guard = request.source_census.source_shm_guard;
			const sqlite_backend_entry_observation* expected_main{};
			for (const auto& entry : request.source_census.entries)
				if (entry.role == sqlite_backend_file_role::main_database)
				{
					if (expected_main != nullptr)
						return unexpected(qualification_error());
					expected_main = &entry;
				}
			if (!target_guard || target_guard->logical_main_locator() != canonical_locator_ ||
				target_guard->identity().profile.empty() ||
				target_guard->identity().bytes.empty() || expected_main == nullptr ||
				!expected_main->object_filesystem_profile ||
				!validate_target_census(request.source_census,
										capability_token_,
										request.parent_namespace_identity,
										*expected_main->object_filesystem_profile) ||
				!target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-target-census"));

			const std::string anchored_main{target_guard->anchored_main_locator()};
			const auto anchored_separator = anchored_main.rfind('/');
			if (anchored_separator == std::string::npos || anchored_separator == 0U ||
				anchored_main.substr(anchored_separator + 1U) != main_leaf_)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-target-anchor"));
			const auto anchored_parent = anchored_main.substr(0U, anchored_separator);
			auto scratch = scratch_tree::create(anchored_parent, request.parent_namespace_identity);
			if (!scratch)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-scratch-create"));
			const auto exact_filesystem_profile = filesystem_profile_string(scratch->profile());
			if (exact_filesystem_profile.empty() ||
				object_filesystem_profile_identity(scratch->profile()) !=
					*expected_main->object_filesystem_profile ||
				!expected_main->held_object->object_mount_identity() ||
				object_mount_identity(scratch->profile()) !=
					*expected_main->held_object->object_mount_identity())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-filesystem-profile"));

			{
				owned_descriptor probe{open_at(scratch->root_descriptor(),
											   "origin-probe.db",
											   O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
											   static_cast<mode_t>(0600))};
				if (!probe || ::fdatasync(probe.get()) != 0)
					return unexpected(qualification_error(
						"source-shm-readonly-qualification-origin-probe-create"));
			}
			auto* underlying = static_cast<sqlite3_vfs*>(
				const_cast<void*>(request.pinned_underlying_vfs_identity));
			if (underlying == nullptr ||
				underlying->app_data != request.pinned_underlying_vfs_app_data_identity ||
				!validate_underlying_callback_image(
					*underlying, request.runtime, scratch->descriptor_path("origin-probe.db")) ||
				!target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-origin-probe"));
			if (auto entry = scratch->recheck_namespace_entry(); !entry)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-scratch-entry"));
			auto scratch_namespace_watch =
				directory_mutation_watch::create(scratch->retained_root_watch_path());
			if (!scratch_namespace_watch)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-scratch-watch"));

			auto producer_directory = scratch->open_child("producer");
			auto cold_directory = scratch->open_child("cold");
			auto active_directory = scratch->open_child("active");
			if (!producer_directory || !cold_directory || !active_directory)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-scratch-directories"));
			sqlite_connection_lifecycle producer{
				nullptr,
				request.runtime.close_v2,
				connection_pins(request.runtime, backend_lifetime_),
			};
			const auto producer_locator = scratch->descriptor_path("producer/main.db");
			auto producer_scope = connection_port_->begin_source_shm_qualification_observation(
				producer_locator, request.observation_capability_token);
			if (!producer_scope || !*producer_scope)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-observation"));
			auto* producer_gate = (*producer_scope)->effect_gate_port();
			if (producer_gate == nullptr ||
				!producer_gate->activate_denied(request.observation_capability_token,
												(*producer_scope)->token(),
												producer_locator))
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-deny"));
			if (auto armed = (*producer_scope)
								 ->arm_source_shm_qualification_fixture_fullpath(
									 sqlite_source_shm_qualification_fixture_fullpath_plan{
										 producer_locator,
										 registered_vfs_name_,
										 forwarding_vfs_identity_,
										 pinned_underlying_vfs_identity_,
										 pinned_underlying_vfs_app_data_identity_,
										 backend_lifetime_.get(),
										 request.observation_capability_token,
									 });
				!armed)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-arm"));
			if (auto created = create_fixture(request.runtime,
											  backend_lifetime_,
											  producer_locator,
											  registered_vfs_name_,
											  producer,
											  *producer_scope);
				!created)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-create"));
			if (!target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-target-guard"));
			auto producer_family = capture_family(producer_directory->get());
			if (!producer_family || producer_family->main.byte_count == 0U ||
				producer_family->wal.byte_count <= 32U ||
				producer_family->shm.byte_count < 32U * 1024U)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-family"));
			for (const auto destination : {cold_directory->get(), active_directory->get()})
			{
				for (const auto* name : {"main.db", "main.db-wal", "main.db-shm"})
					if (auto copied =
							copy_regular_file(producer_directory->get(), name, destination, name);
						!copied)
						return unexpected(
							qualification_error("source-shm-readonly-qualification-family-copy"));
				if (::fsync(destination) != 0 || !validate_family_directory(destination))
					return unexpected(
						qualification_error("source-shm-readonly-qualification-family-census"));
			}
			if (auto closed = close_connection(producer); !closed)
				return unexpected(
					qualification_error("source-shm-readonly-qualification-producer-close"));
			if (!target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-target-guard"));

			auto cold = exercise_readonly_route(request,
												backend_lifetime_,
												connection_port_,
												cold_directory->get(),
												scratch->descriptor_path("cold/main.db"),
												exact_filesystem_profile,
												false);
			if (!cold)
				return unexpected(cold.error());
			auto active = exercise_readonly_route(request,
												  backend_lifetime_,
												  connection_port_,
												  active_directory->get(),
												  scratch->descriptor_path("active/main.db"),
												  exact_filesystem_profile,
												  true);
			if (!active)
				return unexpected(active.error());

			if (!cold->mutation_watch.finish() || !active->mutation_watch.finish() ||
				!scratch->recheck_namespace_entry() || !target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-pre-cleanup-recheck"));

			sqlite_backend_opaque_identity sealed;
			sealed.profile = qualification_token_profile;
			sealed.bytes.reserve(4096U);
			append_bytes(sealed.bytes, source_shm_profile);
			append_bytes(sealed.bytes, source_id);
			append_bytes(sealed.bytes, exact_filesystem_profile);
			append_bytes(sealed.bytes, canonical_locator_);
			append_bytes(sealed.bytes, registered_vfs_name_);
			append_namespace_census(sealed.bytes, request.source_census);
			append_opaque(sealed.bytes, request.parent_namespace_identity);
			append_opaque(sealed.bytes, target_guard->identity());
			append_trivial(sealed.bytes, request.runtime.runtime_identity);
			append_trivial(sealed.bytes, request.runtime.runtime_image_identity);
			append_trivial(sealed.bytes, request.runtime.runtime_lifetime_identity);
			append_trivial(sealed.bytes, request.runtime.open_v2);
			append_trivial(sealed.bytes, request.runtime.close_v2);
			append_trivial(sealed.bytes, request.runtime.exec);
			append_trivial(sealed.bytes, request.runtime.errmsg);
			append_trivial(sealed.bytes, request.runtime.free_memory);
			append_trivial(sealed.bytes, request.runtime.source_id);
			append_trivial(sealed.bytes, request.runtime.uri_parameter);
			append_trivial(sealed.bytes, request.runtime.uri_key);
			append_trivial(sealed.bytes, request.runtime.vfs_find);
			append_trivial(sealed.bytes, request.runtime.vfs_register);
			append_trivial(sealed.bytes, request.runtime.vfs_unregister);
			append_trivial(sealed.bytes, request.forwarding_vfs_identity);
			append_trivial(sealed.bytes, request.pinned_underlying_vfs_identity);
			append_trivial(sealed.bytes, request.pinned_underlying_vfs_app_data_identity);
			append_trivial(sealed.bytes, request.backend_lifetime_identity);
			append_opaque(sealed.bytes, request.observation_capability_token);
			append_trivial(sealed.bytes, scratch->profile().parent_device);
			append_trivial(sealed.bytes, scratch->profile().mount_id);
			append_trivial(sealed.bytes, scratch->profile().type);
			append_trivial(sealed.bytes, scratch->profile().block_size);
			append_trivial(sealed.bytes, scratch->profile().name_length);
			append_trivial(sealed.bytes, scratch->profile().flags);
			append_trivial(sealed.bytes, scratch->profile().fsid_first);
			append_trivial(sealed.bytes, scratch->profile().fsid_second);
			append_family_snapshot(sealed.bytes, cold->before);
			append_family_snapshot(sealed.bytes, cold->after);
			append_map_events(sealed.bytes, cold->observation);
			append_family_snapshot(sealed.bytes, active->before);
			append_family_snapshot(sealed.bytes, active->after);
			append_map_events(sealed.bytes, active->observation);
			append_bytes(sealed.bytes, "scratch-cleanup-required-before-seal");

			if (!scratch->recheck_namespace_entry() || !scratch_namespace_watch->finish())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-scratch-namespace"));
			producer_directory = owned_descriptor{};
			cold_directory = owned_descriptor{};
			active_directory = owned_descriptor{};
			if (auto cleaned = scratch->cleanup(); !cleaned)
				return unexpected(qualification_error("source-shm-readonly-qualification-cleanup"));
			if (!target_guard->recheck())
				return unexpected(
					qualification_error("source-shm-readonly-qualification-post-cleanup-recheck"));
			append_bytes(sealed.bytes, "scratch-cleanup-complete");
			const sqlite_backend_entry_observation* expected_shared_memory{};
			for (const auto& entry : request.source_census.entries)
				if (entry.role == sqlite_backend_file_role::shared_memory)
				{
					if (expected_shared_memory != nullptr)
						return unexpected(qualification_error());
					expected_shared_memory = &entry;
				}
			if (expected_shared_memory == nullptr || !expected_shared_memory->object_identity ||
				!expected_shared_memory->directory_entry_identity)
				return unexpected(qualification_error());
			auto target_epoch = default_source_shm_target_namespace_epoch::create(
				canonical_locator_, request.source_census);
			if (!target_epoch || !*target_epoch)
				return unexpected(qualification_error());
			append_opaque(sealed.bytes, (*target_epoch)->identity());
			append_opaque(sealed.bytes, *expected_shared_memory->object_identity);
			append_opaque(sealed.bytes, *expected_shared_memory->directory_entry_identity);

			auto target_uri = make_sqlite_source_shm_readonly_uri(canonical_locator_);
			if (!target_uri)
				return unexpected(qualification_error());
			sqlite_source_shm_qualification_receipt receipt{
				std::string{source_shm_profile},
				std::move(source_id),
				exact_filesystem_profile,
				request.runtime.runtime_identity,
				request.runtime.runtime_image_identity,
				request.runtime.runtime_lifetime_identity,
				request.forwarding_vfs_identity,
				request.pinned_underlying_vfs_identity,
				request.pinned_underlying_vfs_app_data_identity,
				request.backend_lifetime_identity,
				request.observation_capability_token,
				request.parent_namespace_identity,
				*expected_shared_memory->object_identity,
				*expected_shared_memory->directory_entry_identity,
				(*target_epoch)->identity(),
				*target_epoch,
				std::move(sealed),
				true,
				true,
				true,
				true,
			};
			return sqlite_source_shm_qualified_open_plan{
				std::move(request.runtime),
				std::move(receipt),
				canonical_locator_,
				std::string{(*target_epoch)->anchored_main_locator()},
				std::move(*target_uri),
				registered_vfs_name_,
				qualified_read_flags,
			};
		}
#endif
	} // namespace

	bool validate_sqlite_source_shm_readonly_map_sequence(
		const std::span<const sqlite_backend_shm_map_observation> events,
		const void* pinned_underlying_vfs_identity,
		const void* pinned_underlying_vfs_app_data_identity,
		const bool cold_route,
		const bool require_later_map) noexcept
	{
		if (events.empty() || (require_later_map && events.size() < 2U))
			return false;
		std::optional<std::size_t> first_page_zero;
		std::size_t mapped_events{};
		for (std::size_t index{}; index < events.size(); ++index)
		{
			const auto& event = events[index];
			if ((event.caller_extend != 0 && event.caller_extend != 1) ||
				event.delegated_extend != 0 ||
				event.pinned_underlying_vfs_identity != pinned_underlying_vfs_identity ||
				event.pinned_underlying_vfs_app_data_identity !=
					pinned_underlying_vfs_app_data_identity)
				return false;
			if ((index == 0U &&
				 (event.readonly_family_seen_before || !event.readonly_family_seen_after)) ||
				(index != 0U &&
				 (!event.readonly_family_seen_before || !event.readonly_family_seen_after)))
				return false;
			if (event.native_status == sqlite_ok || event.returned_status == sqlite_ok)
				return false;
			const auto cantinit = event.native_status == sqlite_readonly_cannot_initialize &&
				event.returned_status == sqlite_readonly_cannot_initialize &&
				!event.native_mapping_nonnull && !event.returned_mapping_nonnull;
			const auto mapped = event.native_status == sqlite_readonly &&
				event.returned_status == sqlite_readonly && event.native_mapping_nonnull &&
				event.returned_mapping_nonnull;
			if (!cantinit && !mapped)
				return false;
			if (cantinit && event.page != 0)
				return false;
			if (event.page == 0 && !first_page_zero)
				first_page_zero = index;
			if (mapped)
				++mapped_events;
		}
		if (!first_page_zero || (cold_route && *first_page_zero != 0U))
			return false;
		const auto& first = events[*first_page_zero];
		const auto first_cantinit = first.native_status == sqlite_readonly_cannot_initialize &&
			first.returned_status == sqlite_readonly_cannot_initialize &&
			!first.native_mapping_nonnull && !first.returned_mapping_nonnull;
		const auto first_mapped = first.native_status == sqlite_readonly &&
			first.returned_status == sqlite_readonly && first.native_mapping_nonnull &&
			first.returned_mapping_nonnull;
		return cold_route ? first_cantinit : first_mapped && mapped_events >= 2U;
	}

	result<void> validate_sqlite_source_shm_readonly_scratch_family(const int directory_descriptor)
	{
#if defined(__linux__) && defined(F_OFD_SETLK)
		return validate_family_directory(directory_descriptor);
#else
		(void)directory_descriptor;
		return unexpected(qualification_error());
#endif
	}

	result<std::string>
	make_sqlite_source_shm_readonly_uri(const std::string_view canonical_absolute_path)
	{
		if (canonical_absolute_path.empty() || canonical_absolute_path.front() != '/' ||
			canonical_absolute_path.contains('\0'))
			return unexpected(qualification_error());
		try
		{
			constexpr std::array hexadecimal{
				'0', '1', '2', '3', '4', '5', '6', '7', '8', '9', 'A', 'B', 'C', 'D', 'E', 'F'};
			if (canonical_absolute_path.size() >
				(std::numeric_limits<std::size_t>::max() - 64U) / 3U)
				return unexpected(qualification_error());
			std::string output{"file:"};
			output.reserve(5U + canonical_absolute_path.size() * 3U + 48U);
			for (const auto raw : canonical_absolute_path)
			{
				const auto value = static_cast<unsigned char>(raw);
				if (uri_unreserved(value))
					output.push_back(static_cast<char>(value));
				else
				{
					output.push_back('%');
					output.push_back(hexadecimal.at(value >> 4U));
					output.push_back(hexadecimal.at(value & 0x0fU));
				}
			}
			output.append("?mode=ro&cache=private&readonly_shm=1");
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(qualification_error());
		}
		catch (const std::length_error&)
		{
			return unexpected(qualification_error());
		}
	}

	result<std::shared_ptr<sqlite_source_shm_readonly_port>>
	make_sqlite_source_shm_readonly_preflight(
		const sqlite_default_observation_binding& binding,
		sqlite_backend_opaque_identity observation_capability_token)
	{
#if defined(__linux__) && defined(F_OFD_SETLK)
		// Connection callbacks are an active-WAL-only proof dependency.  Their absence must not
		// prevent construction of the baseline observation capability used by quiescent v2.
		if (!binding.connection_observation_port)
			return std::shared_ptr<sqlite_source_shm_readonly_port>{};
		if (binding.canonical_vfs_locator.size() < 2U ||
			binding.canonical_vfs_locator.front() != '/' ||
			binding.canonical_vfs_locator.back() == '/' ||
			binding.canonical_vfs_locator.contains('\0') || binding.registered_vfs_name.empty() ||
			binding.registered_vfs_name.contains('\0') ||
			binding.forwarding_vfs_identity == nullptr ||
			binding.pinned_underlying_vfs_identity == nullptr ||
			binding.pinned_underlying_vfs_app_data_identity == nullptr ||
			!binding.backend_lifetime || observation_capability_token.profile.empty() ||
			observation_capability_token.bytes.empty())
			return unexpected(qualification_error());
		const auto separator = binding.canonical_vfs_locator.rfind('/');
		if (separator == std::string::npos ||
			separator + 1U >= binding.canonical_vfs_locator.size())
			return unexpected(qualification_error());
		try
		{
			auto main_leaf = binding.canonical_vfs_locator.substr(separator + 1U);
			return std::static_pointer_cast<sqlite_source_shm_readonly_port>(
				std::make_shared<default_source_shm_readonly_preflight>(
					binding, std::move(observation_capability_token), std::move(main_leaf)));
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(qualification_error());
		}
		catch (const std::length_error&)
		{
			return unexpected(qualification_error());
		}
#else
		(void)binding;
		(void)observation_capability_token;
		// The readonly-SHM route is an optional active-WAL capability.  Unsupported hosts must
		// still be able to construct the baseline observation capability used by quiescent v2.
		return std::shared_ptr<sqlite_source_shm_readonly_port>{};
#endif
	}
} // namespace cxxlens::sdk
