#include "sqlite_private_snapshot_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <ranges>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "sqlite_payload_streaming_internal.hpp"

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#if defined(__linux__)
#include <linux/memfd.h>
#include <sys/syscall.h>
#endif

namespace cxxlens::sdk
{
	namespace
	{
		constexpr int sqlite_ok = 0;
		constexpr int sqlite_error = 1;
		constexpr int sqlite_readonly = 8;
		constexpr int sqlite_io_error = 10;
		constexpr int sqlite_not_found = 12;
		constexpr int sqlite_cannot_open = 14;
		constexpr int sqlite_io_error_short_read = sqlite_io_error | (2 << 8);
		constexpr int sqlite_open_readonly = 0x00000001;
		constexpr int sqlite_open_readwrite = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_delete_on_close = 0x00000008;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_file_type_mask = 0x000fff00;
		constexpr int sqlite_file_control_lock_state = 1;
		constexpr int sqlite_file_control_powersafe_overwrite = 13;
		constexpr int sqlite_file_control_has_moved = 20;
		constexpr std::size_t copy_chunk_bytes = 64U * 1024U;
		constexpr std::uint64_t maximum_sha256_byte_count = sqlite_sha256_maximum_byte_count;
		constexpr std::string_view private_path_prefix{"/cxxlens-sqlite-private-v1/"};

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

		[[nodiscard]] error private_snapshot_error(std::string detail)
		{
			return {"store.backend-unavailable", "sqlite", std::move(detail)};
		}

		using private_snapshot_digest_result =
			std::variant<std::string, sqlite_private_snapshot_digest_failure>;

		[[nodiscard]] result<void>
		verify_digest_binding(const private_snapshot_digest_result& observed,
							  const std::string_view expected)
		{
			if (const auto* failure =
					std::get_if<sqlite_private_snapshot_digest_failure>(&observed))
				return unexpected(private_snapshot_error(
					std::string{sqlite_private_snapshot_digest_failure_detail(*failure)}));
			const auto* digest = std::get_if<std::string>(&observed);
			if (digest == nullptr || *digest != expected)
				return unexpected(private_snapshot_error("private-snapshot-seal-binding"));
			return {};
		}

		struct private_snapshot_runtime
		{
			sqlite_private_snapshot_registry_binding registry;
			sqlite3_vfs* delegate{};
		};

		[[nodiscard]] result<std::shared_ptr<private_snapshot_runtime>>
		bind_runtime(sqlite_private_snapshot_registry_binding registry)
		{
			if (registry.runtime_identity == nullptr || registry.pinned_default_vfs == nullptr ||
				registry.find == nullptr || registry.register_vfs == nullptr ||
				registry.unregister_vfs == nullptr || !registry.runtime_lifetime)
				return unexpected(private_snapshot_error("private-snapshot-vfs-lifetime"));
			auto* delegate = static_cast<sqlite3_vfs*>(registry.pinned_default_vfs);
			if (delegate->version < 2 || delegate->randomness == nullptr ||
				delegate->sleep == nullptr || delegate->current_time == nullptr)
				return unexpected(private_snapshot_error("private-snapshot-vfs-lifetime"));
			std::shared_ptr<private_snapshot_runtime> output;
			try
			{
				output = std::make_shared<private_snapshot_runtime>();
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(private_snapshot_error("private-snapshot-allocation"));
			}
			output->registry = std::move(registry);
			output->delegate = delegate;
			return output;
		}

		[[nodiscard]] result<std::string> random_token()
		{
#if defined(__linux__) && defined(SYS_getrandom)
			std::array<std::byte, 16U> bytes{};
			std::size_t consumed{};
			while (consumed < bytes.size())
			{
				const auto count = static_cast<long>(
					::syscall(SYS_getrandom, bytes.data() + consumed, bytes.size() - consumed, 0U));
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return unexpected(private_snapshot_error("private-snapshot-random"));
			}
			constexpr std::string_view digits{"0123456789abcdef"};
			try
			{
				std::string output;
				output.reserve(bytes.size() * 2U);
				for (const auto byte : bytes)
				{
					const auto value = std::to_integer<unsigned>(byte);
					output.push_back(digits[value >> 4U]);
					output.push_back(digits[value & 0x0fU]);
				}
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(private_snapshot_error("private-snapshot-allocation"));
			}
#else
			return unexpected(private_snapshot_error("private-snapshot-platform"));
#endif
		}

		class private_snapshot_state;

		struct private_snapshot_file
		{
			sqlite3_file base{};
			private_snapshot_state* owner{};
			int descriptor{-1};
		};

		[[nodiscard]] private_snapshot_file* private_file(sqlite3_file* value) noexcept
		{
			return reinterpret_cast<private_snapshot_file*>(value);
		}

		int private_close(sqlite3_file* base) noexcept;
		int private_read(sqlite3_file* base, void* output, int count, long long offset) noexcept;
		int private_write(sqlite3_file*, const void*, int, long long) noexcept;
		int private_truncate(sqlite3_file*, long long) noexcept;
		int private_sync(sqlite3_file*, int) noexcept;
		int private_file_size(sqlite3_file* base, long long* output) noexcept;
		int private_lock(sqlite3_file*, int) noexcept;
		int private_unlock(sqlite3_file*, int) noexcept;
		int private_reserved(sqlite3_file*, int* output) noexcept;
		int private_control(sqlite3_file*, int operation, void* output) noexcept;
		int private_sector(sqlite3_file*) noexcept;
		int private_characteristics(sqlite3_file*) noexcept;
		int private_shm_map(sqlite3_file*, int, int, int, volatile void** output) noexcept;
		int private_shm_lock(sqlite3_file*, int, int, int) noexcept;
		void private_shm_barrier(sqlite3_file*) noexcept;
		int private_shm_unmap(sqlite3_file*, int) noexcept;
		int private_fetch(sqlite3_file*, long long, int, void** output) noexcept;
		int private_unfetch(sqlite3_file*, long long, void*) noexcept;

		const sqlite3_io_methods private_io_methods{
			3,
			private_close,
			private_read,
			private_write,
			private_truncate,
			private_sync,
			private_file_size,
			private_lock,
			private_unlock,
			private_reserved,
			private_control,
			private_sector,
			private_characteristics,
			private_shm_map,
			private_shm_lock,
			private_shm_barrier,
			private_shm_unmap,
			private_fetch,
			private_unfetch,
		};

		std::mutex private_vfs_registration_mutex;

		class private_snapshot_state final
			: public sqlite_backend_private_snapshot_builder,
			  public sqlite_backend_private_snapshot,
			  public std::enable_shared_from_this<private_snapshot_state>
		{
		  public:
			private_snapshot_state(std::shared_ptr<private_snapshot_runtime> runtime,
								   const int descriptor,
								   sqlite_backend_opaque_identity source_token,
								   std::string token)
				: runtime_{std::move(runtime)}, descriptor_{descriptor},
				  source_token_{std::move(source_token)}, token_{std::move(token)}
			{
				path_.reserve(private_path_prefix.size() + token_.size());
				path_.append(private_path_prefix);
				path_.append(token_);
				vfs_name_ = "cxxlens-sqlite-private-v1-" + token_;
				uri_ = "file:" + path_ + "?mode=ro&cache=private&immutable=1";
				wrapper_ = sqlite3_vfs{
					3,
					static_cast<int>(sizeof(private_snapshot_file)),
					4096,
					nullptr,
					vfs_name_.c_str(),
					this,
					vfs_open,
					vfs_remove,
					vfs_access,
					vfs_full_path,
					vfs_dl_open,
					vfs_dl_error,
					vfs_dl_sym,
					vfs_dl_close,
					vfs_randomness,
					vfs_sleep,
					vfs_current_time,
					vfs_last_error,
					vfs_current_time_int64,
					vfs_set_system_call,
					vfs_get_system_call,
					vfs_next_system_call,
				};
			}

			~private_snapshot_state() override
			{
				if (open_file_count_.load(std::memory_order_acquire) != 0U)
					std::terminate();
				if (registered_)
				{
					std::scoped_lock lock{private_vfs_registration_mutex};
					if (runtime_->registry.find(vfs_name_.c_str()) != &wrapper_ ||
						runtime_->registry.unregister_vfs(&wrapper_) != sqlite_ok)
						std::terminate();
					registered_ = false;
				}
#if defined(__unix__) || defined(__APPLE__)
				if (descriptor_ >= 0)
					(void)::close(descriptor_);
#endif
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
			{
				std::scoped_lock lock{mutex_};
				if (sealed_.load(std::memory_order_acquire) ||
					byte_count_ > maximum_sha256_byte_count ||
					bytes.size() > maximum_sha256_byte_count - byte_count_)
					return unexpected(private_snapshot_error("private-snapshot-state"));
#if defined(__unix__) || defined(__APPLE__)
				const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
				if (byte_count_ > maximum || bytes.size() > maximum - byte_count_)
					return unexpected(private_snapshot_error("private-snapshot-size"));
				std::size_t consumed{};
				while (consumed < bytes.size())
				{
					const auto count = ::pwrite(descriptor_,
												bytes.data() + consumed,
												bytes.size() - consumed,
												static_cast<off_t>(byte_count_ + consumed));
					if (count > 0)
					{
						consumed += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					return unexpected(private_snapshot_error("private-snapshot-write"));
				}
				byte_count_ += static_cast<std::uint64_t>(bytes.size());
				return {};
#else
				(void)bytes;
				return unexpected(private_snapshot_error("private-snapshot-platform"));
#endif
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
			seal(std::uint64_t expected_byte_count, std::string_view expected_sha256) override;

			[[nodiscard]] std::string_view application_generated_uri() const noexcept override
			{
				return uri_;
			}
			[[nodiscard]] std::string_view registered_vfs_name() const noexcept override
			{
				return vfs_name_;
			}
			[[nodiscard]] const void* vfs_implementation_identity() const noexcept override
			{
				return &wrapper_;
			}
			[[nodiscard]] const sqlite_backend_opaque_identity&
			source_capability_token() const noexcept override
			{
				return source_token_;
			}
			[[nodiscard]] const sqlite_backend_copy_receipt& receipt() const noexcept override
			{
				return receipt_;
			}

			[[nodiscard]] private_snapshot_digest_result digest_exact() const;

			static int vfs_open(sqlite3_vfs* vfs,
								const char* name,
								sqlite3_file* output,
								int flags,
								int* out_flags) noexcept;
			static int vfs_remove(sqlite3_vfs*, const char*, int) noexcept;
			static int
			vfs_access(sqlite3_vfs* vfs, const char* name, int flags, int* output) noexcept;
			static int
			vfs_full_path(sqlite3_vfs* vfs, const char* name, int size, char* output) noexcept;
			static void* vfs_dl_open(sqlite3_vfs*, const char*) noexcept;
			static void vfs_dl_error(sqlite3_vfs*, int size, char* output) noexcept;
			static void (*vfs_dl_sym(sqlite3_vfs*, void*, const char*) noexcept)(void);
			static void vfs_dl_close(sqlite3_vfs*, void*) noexcept;
			static int vfs_randomness(sqlite3_vfs* vfs, int size, char* output) noexcept;
			static int vfs_sleep(sqlite3_vfs* vfs, int microseconds) noexcept;
			static int vfs_current_time(sqlite3_vfs* vfs, double* output) noexcept;
			static int vfs_last_error(sqlite3_vfs*, int size, char* output) noexcept;
			static int vfs_current_time_int64(sqlite3_vfs* vfs, long long* output) noexcept;
			static int vfs_set_system_call(sqlite3_vfs*, const char*, sqlite3_syscall_ptr) noexcept;
			static sqlite3_syscall_ptr vfs_get_system_call(sqlite3_vfs*, const char*) noexcept;
			static const char* vfs_next_system_call(sqlite3_vfs*, const char*) noexcept;

			std::shared_ptr<private_snapshot_runtime> runtime_;
			int descriptor_{-1};
			sqlite_backend_opaque_identity source_token_;
			std::string token_;
			std::string path_;
			std::string vfs_name_;
			std::string uri_;
			sqlite3_vfs wrapper_{};
			mutable std::mutex mutex_;
			std::atomic<bool> sealed_{};
			std::atomic<std::size_t> open_file_count_{};
			std::uint64_t byte_count_{};
			sqlite_backend_copy_receipt receipt_;
			bool registered_{};
		};

		private_snapshot_digest_result private_snapshot_state::digest_exact() const
		{
#if defined(__unix__) || defined(__APPLE__)
			struct stat before{};
			if (::fstat(descriptor_, &before) != 0 || before.st_size < 0 ||
				static_cast<std::uint64_t>(before.st_size) != byte_count_)
				return sqlite_private_snapshot_digest_failure::source_read;
			sqlite_incremental_sha256 digest;
			std::array<std::byte, copy_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < byte_count_)
			{
				const auto wanted = static_cast<std::size_t>(
					std::min<std::uint64_t>(buffer.size(), byte_count_ - offset));
				std::size_t consumed{};
				while (consumed < wanted)
				{
					const auto count = ::pread(descriptor_,
											   buffer.data() + consumed,
											   wanted - consumed,
											   static_cast<off_t>(offset + consumed));
					if (count > 0)
					{
						consumed += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					return sqlite_private_snapshot_digest_failure::source_read;
				}
				if (auto updated = digest.update(std::span{buffer}.first(wanted)); !updated)
					return sqlite_private_snapshot_digest_failure::hash_update;
				offset += wanted;
			}
			struct stat after{};
			if (::fstat(descriptor_, &after) != 0 || before.st_dev != after.st_dev ||
				before.st_ino != after.st_ino || before.st_size != after.st_size ||
				before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
				before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
				return sqlite_private_snapshot_digest_failure::source_drift;
			auto finished = digest.finish();
			if (!finished)
				return sqlite_private_snapshot_digest_failure::hash_finalize;
			return std::move(*finished);
#else
			return sqlite_private_snapshot_digest_failure::source_read;
#endif
		}

		result<std::shared_ptr<sqlite_backend_private_snapshot>>
		private_snapshot_state::seal(const std::uint64_t expected_byte_count,
									 const std::string_view expected_sha256)
		{
			std::scoped_lock state_lock{mutex_};
			if (sealed_.load(std::memory_order_acquire) ||
				expected_byte_count > maximum_sha256_byte_count ||
				expected_byte_count != byte_count_ || expected_sha256.size() != 71U ||
				!expected_sha256.starts_with("sha256:") ||
				!std::ranges::all_of(expected_sha256.substr(7U),
									 [](const char value)
									 {
										 return (value >= '0' && value <= '9') ||
											 (value >= 'a' && value <= 'f');
									 }))
				return unexpected(private_snapshot_error("private-snapshot-seal-binding"));
			auto before_digest = digest_exact();
			if (auto verified = verify_digest_binding(before_digest, expected_sha256); !verified)
				return unexpected(std::move(verified.error()));
#if defined(__linux__)
			constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
			if (::fcntl(descriptor_, F_ADD_SEALS, required_seals) != 0 ||
				(::fcntl(descriptor_, F_GET_SEALS) & required_seals) != required_seals)
				return unexpected(private_snapshot_error("private-snapshot-seal"));
#else
			return unexpected(private_snapshot_error("private-snapshot-platform"));
#endif
			auto after_digest = digest_exact();
			if (auto verified = verify_digest_binding(after_digest, expected_sha256); !verified)
				return unexpected(std::move(verified.error()));
			try
			{
				receipt_ = {expected_byte_count, std::string{expected_sha256}};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(private_snapshot_error("private-snapshot-allocation"));
			}
			sealed_.store(true, std::memory_order_release);
			{
				std::scoped_lock registry_lock{private_vfs_registration_mutex};
				if (runtime_->registry.find(vfs_name_.c_str()) != nullptr ||
					runtime_->registry.register_vfs(&wrapper_, 0) != sqlite_ok ||
					runtime_->registry.find(vfs_name_.c_str()) != &wrapper_)
				{
					sealed_.store(false, std::memory_order_release);
					return unexpected(private_snapshot_error("private-snapshot-vfs-register"));
				}
				registered_ = true;
			}
			auto self = shared_from_this();
			return std::static_pointer_cast<sqlite_backend_private_snapshot>(std::move(self));
		}

		int private_close(sqlite3_file* base) noexcept
		{
			auto* file = private_file(base);
			const auto closed = file->descriptor < 0 || ::close(file->descriptor) == 0;
			file->descriptor = -1;
			file->owner->open_file_count_.fetch_sub(1U, std::memory_order_acq_rel);
			file->~private_snapshot_file();
			return closed ? sqlite_ok : sqlite_io_error;
		}

		int private_read(sqlite3_file* base,
						 void* output,
						 const int count,
						 const long long offset) noexcept
		{
			if (output == nullptr || count < 0 || offset < 0)
				return sqlite_io_error;
			auto* file = private_file(base);
			auto* destination = static_cast<std::byte*>(output);
			std::size_t consumed{};
			while (consumed < static_cast<std::size_t>(count))
			{
				const auto read_count =
					::pread(file->descriptor,
							destination + consumed,
							static_cast<std::size_t>(count) - consumed,
							static_cast<off_t>(offset) + static_cast<off_t>(consumed));
				if (read_count > 0)
				{
					consumed += static_cast<std::size_t>(read_count);
					continue;
				}
				if (read_count < 0 && errno == EINTR)
					continue;
				if (read_count < 0)
					return sqlite_io_error;
				std::memset(destination + consumed, 0, static_cast<std::size_t>(count) - consumed);
				return sqlite_io_error_short_read;
			}
			return sqlite_ok;
		}

		int private_write(sqlite3_file*, const void*, int, long long) noexcept
		{
			return sqlite_readonly;
		}

		int private_truncate(sqlite3_file*, long long) noexcept
		{
			return sqlite_readonly;
		}

		int private_sync(sqlite3_file*, int) noexcept
		{
			return sqlite_ok;
		}

		int private_file_size(sqlite3_file* base, long long* output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = 0;
			struct stat observed{};
			if (::fstat(private_file(base)->descriptor, &observed) != 0 || observed.st_size < 0)
				return sqlite_io_error;
			*output = static_cast<long long>(observed.st_size);
			return sqlite_ok;
		}

		int private_lock(sqlite3_file*, int) noexcept
		{
			return sqlite_ok;
		}

		int private_unlock(sqlite3_file*, int) noexcept
		{
			return sqlite_ok;
		}

		int private_reserved(sqlite3_file*, int* output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = 0;
			return sqlite_ok;
		}

		int private_control(sqlite3_file*, const int operation, void* output) noexcept
		{
			if (output == nullptr)
				return sqlite_not_found;
			switch (operation)
			{
				case sqlite_file_control_lock_state:
					*static_cast<int*>(output) = 0;
					return sqlite_ok;
				case sqlite_file_control_powersafe_overwrite:
					*static_cast<int*>(output) = 0;
					return sqlite_ok;
				case sqlite_file_control_has_moved:
					*static_cast<int*>(output) = 0;
					return sqlite_ok;
				default:
					return sqlite_not_found;
			}
		}

		int private_sector(sqlite3_file*) noexcept
		{
			return 4096;
		}

		int private_characteristics(sqlite3_file*) noexcept
		{
			return 0;
		}

		int private_shm_map(sqlite3_file*, int, int, int, volatile void** output) noexcept
		{
			if (output != nullptr)
				*output = nullptr;
			return sqlite_readonly;
		}

		int private_shm_lock(sqlite3_file*, int, int, int) noexcept
		{
			return sqlite_readonly;
		}

		void private_shm_barrier(sqlite3_file*) noexcept {}

		int private_shm_unmap(sqlite3_file*, int) noexcept
		{
			return sqlite_ok;
		}

		int private_fetch(sqlite3_file*, long long, int, void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			return sqlite_ok;
		}

		int private_unfetch(sqlite3_file*, long long, void*) noexcept
		{
			return sqlite_ok;
		}

		int private_snapshot_state::vfs_open(sqlite3_vfs* vfs,
											 const char* name,
											 sqlite3_file* output,
											 const int flags,
											 int* out_flags) noexcept
		{
			if (out_flags != nullptr)
				*out_flags = 0;
			if (output == nullptr)
				return sqlite_cannot_open;
			output->methods = nullptr;
			if (vfs == nullptr || vfs->app_data == nullptr || name == nullptr)
				return sqlite_cannot_open;
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			if (!owner->sealed_.load(std::memory_order_acquire) ||
				std::string_view{name} != owner->path_ ||
				(flags & sqlite_open_file_type_mask) != sqlite_open_main_database ||
				(flags & sqlite_open_readonly) == 0 ||
				(flags &
				 (sqlite_open_readwrite | sqlite_open_create | sqlite_open_delete_on_close)) != 0)
				return sqlite_cannot_open;
#if defined(__unix__) || defined(__APPLE__)
			const auto duplicate = ::fcntl(owner->descriptor_, F_DUPFD_CLOEXEC, 0);
			if (duplicate < 0)
				return sqlite_cannot_open;
			auto* file = new (output) private_snapshot_file{};
			file->owner = owner;
			file->descriptor = duplicate;
			owner->open_file_count_.fetch_add(1U, std::memory_order_acq_rel);
			if (out_flags != nullptr)
				*out_flags = sqlite_open_readonly | sqlite_open_main_database;
			file->base.methods = &private_io_methods;
			return sqlite_ok;
#else
			(void)owner;
			return sqlite_cannot_open;
#endif
		}

		int private_snapshot_state::vfs_remove(sqlite3_vfs*, const char*, int) noexcept
		{
			return sqlite_readonly;
		}

		int private_snapshot_state::vfs_access(sqlite3_vfs* vfs,
											   const char* name,
											   const int flags,
											   int* output) noexcept
		{
			if (output == nullptr)
				return sqlite_error;
			*output = 0;
			if (vfs == nullptr || vfs->app_data == nullptr || name == nullptr)
				return sqlite_ok;
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			if (owner->sealed_.load(std::memory_order_acquire) &&
				std::string_view{name} == owner->path_)
				*output = flags == 1 ? 0 : 1;
			return sqlite_ok;
		}

		int private_snapshot_state::vfs_full_path(sqlite3_vfs* vfs,
												  const char* name,
												  const int size,
												  char* output) noexcept
		{
			if (output == nullptr || size <= 0)
				return sqlite_cannot_open;
			output[0] = '\0';
			if (vfs == nullptr || vfs->app_data == nullptr || name == nullptr)
				return sqlite_cannot_open;
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			if (std::string_view{name} != owner->path_ ||
				owner->path_.size() + 1U > static_cast<std::size_t>(size))
				return sqlite_cannot_open;
			std::memcpy(output, owner->path_.data(), owner->path_.size());
			output[owner->path_.size()] = '\0';
			return sqlite_ok;
		}

		void* private_snapshot_state::vfs_dl_open(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		void
		private_snapshot_state::vfs_dl_error(sqlite3_vfs*, const int size, char* output) noexcept
		{
			if (output != nullptr && size > 0)
				output[0] = '\0';
		}

		void (*private_snapshot_state::vfs_dl_sym(sqlite3_vfs*, void*, const char*) noexcept)(void)
		{
			return nullptr;
		}

		void private_snapshot_state::vfs_dl_close(sqlite3_vfs*, void*) noexcept {}

		int private_snapshot_state::vfs_randomness(sqlite3_vfs* vfs,
												   const int size,
												   char* output) noexcept
		{
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			return owner->runtime_->delegate->randomness(owner->runtime_->delegate, size, output);
		}

		int private_snapshot_state::vfs_sleep(sqlite3_vfs* vfs, const int microseconds) noexcept
		{
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			return owner->runtime_->delegate->sleep(owner->runtime_->delegate, microseconds);
		}

		int private_snapshot_state::vfs_current_time(sqlite3_vfs* vfs, double* output) noexcept
		{
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			return owner->runtime_->delegate->current_time(owner->runtime_->delegate, output);
		}

		int
		private_snapshot_state::vfs_last_error(sqlite3_vfs*, const int size, char* output) noexcept
		{
			if (output != nullptr && size > 0)
				output[0] = '\0';
			return sqlite_not_found;
		}

		int private_snapshot_state::vfs_current_time_int64(sqlite3_vfs* vfs,
														   long long* output) noexcept
		{
			auto* owner = static_cast<private_snapshot_state*>(vfs->app_data);
			return owner->runtime_->delegate->version >= 2 &&
					owner->runtime_->delegate->current_time_int64 != nullptr
				? owner->runtime_->delegate->current_time_int64(owner->runtime_->delegate, output)
				: sqlite_not_found;
		}

		int private_snapshot_state::vfs_set_system_call(sqlite3_vfs*,
														const char*,
														sqlite3_syscall_ptr) noexcept
		{
			return sqlite_not_found;
		}

		sqlite3_syscall_ptr private_snapshot_state::vfs_get_system_call(sqlite3_vfs*,
																		const char*) noexcept
		{
			return nullptr;
		}

		const char* private_snapshot_state::vfs_next_system_call(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}
	} // namespace

	result<std::shared_ptr<sqlite_backend_private_snapshot_builder>>
	make_sqlite_private_snapshot_builder(sqlite_backend_opaque_identity source_capability_token,
										 sqlite_private_snapshot_registry_binding registry)
	{
		if (source_capability_token.profile.empty() || source_capability_token.bytes.empty())
			return unexpected(private_snapshot_error("private-snapshot-source-binding"));
		auto runtime = bind_runtime(std::move(registry));
		if (!runtime)
			return unexpected(std::move(runtime.error()));
		auto token = random_token();
		if (!token)
			return unexpected(std::move(token.error()));
#if defined(__linux__) && defined(SYS_memfd_create)
		const auto descriptor = static_cast<int>(::syscall(
			SYS_memfd_create, "cxxlens-sqlite-private-v1", MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (descriptor < 0)
			return unexpected(private_snapshot_error("private-snapshot-create"));
		try
		{
			auto output =
				std::make_shared<private_snapshot_state>(std::move(*runtime),
														 descriptor,
														 std::move(source_capability_token),
														 std::move(*token));
			return std::static_pointer_cast<sqlite_backend_private_snapshot_builder>(
				std::move(output));
		}
		catch (const std::bad_alloc&)
		{
			(void)::close(descriptor);
			return unexpected(private_snapshot_error("private-snapshot-allocation"));
		}
		catch (const std::length_error&)
		{
			(void)::close(descriptor);
			return unexpected(private_snapshot_error("private-snapshot-allocation"));
		}
#else
		return unexpected(private_snapshot_error("private-snapshot-platform"));
#endif
	}
} // namespace cxxlens::sdk
