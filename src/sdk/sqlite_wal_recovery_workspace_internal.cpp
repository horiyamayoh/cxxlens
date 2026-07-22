#include "sqlite_wal_recovery_workspace_internal.hpp"

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
#include <vector>

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
		constexpr int sqlite_busy = 5;
		constexpr int sqlite_no_memory = 7;
		constexpr int sqlite_readonly = 8;
		constexpr int sqlite_io_error = 10;
		constexpr int sqlite_not_found = 12;
		constexpr int sqlite_cannot_open = 14;
		constexpr int sqlite_io_error_short_read = sqlite_io_error | (2 << 8);
		constexpr int sqlite_open_readonly = 0x00000001;
		constexpr int sqlite_open_readwrite = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_delete_on_close = 0x00000008;
		constexpr int sqlite_open_uri = 0x00000040;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_main_journal = 0x00000800;
		constexpr int sqlite_open_write_ahead_log = 0x00080000;
		constexpr int sqlite_open_file_type_mask = 0x000fff00;
		constexpr int sqlite_file_control_lock_state = 1;
		constexpr int sqlite_file_control_persist_wal = 10;
		constexpr int sqlite_file_control_powersafe_overwrite = 13;
		constexpr int sqlite_file_control_has_moved = 20;
		constexpr int sqlite_shm_unlock = 1;
		constexpr int sqlite_shm_lock = 2;
		constexpr int sqlite_shm_shared = 4;
		constexpr int sqlite_shm_exclusive = 8;
		constexpr std::size_t sqlite_shm_lock_slot_count = 8U;
		constexpr std::size_t maximum_shm_region_count = 64U;
		constexpr int maximum_shm_region_bytes = 1024 * 1024;
		constexpr std::string_view workspace_path_prefix{"/cxxlens-sqlite-wal-recovery-v1/"};
		constexpr std::string_view workspace_profile{
			"cxxlens.sqlite-wal-only-private-recovery-workspace.v1"};

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

		[[nodiscard]] error workspace_error(std::string detail)
		{
			return {"store.backend-unavailable", "sqlite-wal-recovery", std::move(detail)};
		}

		[[nodiscard]] bool canonical_sha256(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] bool checked_increment(std::uint64_t& value) noexcept
		{
			std::uint64_t next{};
			if (!sqlite_checked_add_u64(value, 1U, next))
				return false;
			value = next;
			return true;
		}

		struct workspace_runtime
		{
			sqlite_private_snapshot_registry_binding registry;
			sqlite3_vfs* delegate{};
		};

		[[nodiscard]] result<std::shared_ptr<workspace_runtime>>
		bind_runtime(sqlite_private_snapshot_registry_binding registry)
		{
			if (registry.runtime_identity == nullptr || registry.pinned_default_vfs == nullptr ||
				registry.find == nullptr || registry.register_vfs == nullptr ||
				registry.unregister_vfs == nullptr || !registry.runtime_lifetime)
				return unexpected(workspace_error("vfs-lifetime"));
			auto* delegate = static_cast<sqlite3_vfs*>(registry.pinned_default_vfs);
			if (delegate->version < 2 || delegate->randomness == nullptr ||
				delegate->sleep == nullptr || delegate->current_time == nullptr)
				return unexpected(workspace_error("vfs-lifetime"));
			try
			{
				auto output = std::make_shared<workspace_runtime>();
				output->registry = std::move(registry);
				output->delegate = delegate;
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(workspace_error("allocation"));
			}
		}

		[[nodiscard]] result<std::string> random_token()
		{
#if defined(__linux__) && defined(SYS_getrandom)
			std::array<std::byte, 16U> bytes{};
			std::size_t consumed{};
			while (consumed < bytes.size())
			{
				const auto count =
					::syscall(SYS_getrandom, bytes.data() + consumed, bytes.size() - consumed, 0U);
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return unexpected(workspace_error("random"));
			}
			try
			{
				constexpr std::string_view digits{"0123456789abcdef"};
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
				return unexpected(workspace_error("allocation"));
			}
#else
			return unexpected(workspace_error("platform"));
#endif
		}

		std::mutex workspace_registration_mutex;

		class workspace_state;

		enum class workspace_file_role : std::uint8_t
		{
			main_database,
			write_ahead_log,
		};

		struct workspace_file
		{
			sqlite3_file base{};
			workspace_state* owner{};
			int descriptor{-1};
			int lock_level{};
			workspace_file_role role{workspace_file_role::main_database};
		};

		[[nodiscard]] workspace_file* recovery_file(sqlite3_file* value) noexcept
		{
			return reinterpret_cast<workspace_file*>(value);
		}

		int recovery_close(sqlite3_file* base) noexcept;
		int recovery_read(sqlite3_file* base, void* output, int count, long long offset) noexcept;
		int recovery_write(sqlite3_file* base, const void*, int, long long) noexcept;
		int recovery_truncate(sqlite3_file* base, long long) noexcept;
		int recovery_sync(sqlite3_file* base, int) noexcept;
		int recovery_file_size(sqlite3_file* base, long long* output) noexcept;
		int recovery_lock(sqlite3_file* base, int level) noexcept;
		int recovery_unlock(sqlite3_file* base, int level) noexcept;
		int recovery_reserved(sqlite3_file* base, int* output) noexcept;
		int recovery_control(sqlite3_file* base, int operation, void* output) noexcept;
		int recovery_sector(sqlite3_file*) noexcept;
		int recovery_characteristics(sqlite3_file*) noexcept;
		int recovery_shm_map(sqlite3_file* base,
							 int page,
							 int page_size,
							 int extend,
							 volatile void** output) noexcept;
		int recovery_shm_lock(sqlite3_file* base, int offset, int count, int flags) noexcept;
		void recovery_shm_barrier(sqlite3_file* base) noexcept;
		int recovery_shm_unmap(sqlite3_file* base, int delete_flag) noexcept;
		int recovery_fetch(sqlite3_file*, long long, int, void** output) noexcept;
		int recovery_unfetch(sqlite3_file*, long long, void*) noexcept;

		const sqlite3_io_methods recovery_io_methods{
			3,
			recovery_close,
			recovery_read,
			recovery_write,
			recovery_truncate,
			recovery_sync,
			recovery_file_size,
			recovery_lock,
			recovery_unlock,
			recovery_reserved,
			recovery_control,
			recovery_sector,
			recovery_characteristics,
			recovery_shm_map,
			recovery_shm_lock,
			recovery_shm_barrier,
			recovery_shm_unmap,
			recovery_fetch,
			recovery_unfetch,
		};

		struct descriptor_byte_range
		{
			int descriptor{-1};
			std::uint64_t byte_count{};
		};

		class descriptor_byte_source final : public sqlite_bounded_byte_source
		{
		  public:
			explicit descriptor_byte_source(const descriptor_byte_range range) noexcept
				: descriptor_{range.descriptor}, remaining_{range.byte_count}
			{
			}

			result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (remaining_ == 0U)
					return 0U;
#if defined(__unix__) || defined(__APPLE__)
				const auto wanted =
					static_cast<std::size_t>(std::min<std::uint64_t>(output.size(), remaining_));
				for (;;)
				{
					const auto count =
						::pread(descriptor_, output.data(), wanted, static_cast<off_t>(offset_));
					if (count > 0)
					{
						const auto converted = static_cast<std::size_t>(count);
						offset_ += static_cast<std::uint64_t>(converted);
						remaining_ -= static_cast<std::uint64_t>(converted);
						return converted;
					}
					if (count < 0 && errno == EINTR)
						continue;
					return unexpected(workspace_error("read"));
				}
#else
				(void)output;
				return unexpected(workspace_error("platform"));
#endif
			}

		  private:
			int descriptor_{-1};
			std::uint64_t offset_{};
			std::uint64_t remaining_{};
		};

		struct workspace_descriptors
		{
			int main_database{-1};
			int write_ahead_log{-1};
		};

		class workspace_state final : public sqlite_wal_recovery_workspace_builder,
									  public sqlite_wal_recovery_workspace,
									  public std::enable_shared_from_this<workspace_state>
		{
		  public:
			workspace_state(std::shared_ptr<workspace_runtime> runtime,
							workspace_descriptors descriptors,
							sqlite_backend_opaque_identity source_token,
							std::string token)
				: runtime_{std::move(runtime)}, main_descriptor_{descriptors.main_database},
				  wal_descriptor_{descriptors.write_ahead_log},
				  source_token_{std::move(source_token)}, token_{std::move(token)}, sealed_{false},
				  open_file_count_{0U}
			{
				path_.reserve(workspace_path_prefix.size() + token_.size());
				path_.append(workspace_path_prefix);
				path_.append(token_);
				wal_path_ = path_ + "-wal";
				journal_path_ = path_ + "-journal";
				vfs_name_ = "cxxlens-sqlite-wal-recovery-v1-" + token_;
				wrapper_ = sqlite3_vfs{
					3,
					static_cast<int>(sizeof(workspace_file)),
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

			~workspace_state() override
			{
				if (open_file_count_.load(std::memory_order_acquire) != 0U)
					std::terminate();
				if (registered_)
				{
					std::scoped_lock lock{workspace_registration_mutex};
					if (runtime_->registry.find(vfs_name_.c_str()) != &wrapper_ ||
						runtime_->registry.unregister_vfs(&wrapper_) != sqlite_ok)
						std::terminate();
					registered_ = false;
				}
#if defined(__unix__) || defined(__APPLE__)
				if (main_descriptor_ >= 0)
					(void)::close(main_descriptor_);
				if (wal_descriptor_ >= 0)
					(void)::close(wal_descriptor_);
#endif
			}

			[[nodiscard]] result<void> append(sqlite_wal_recovery_copy_role role,
											  std::span<const std::byte> bytes) override;
			[[nodiscard]] result<std::shared_ptr<sqlite_wal_recovery_workspace>>
			seal(sqlite_wal_recovery_workspace_expectation expectation) override;

			[[nodiscard]] std::string_view database_path() const noexcept override
			{
				return path_;
			}
			[[nodiscard]] std::string_view registered_vfs_name() const noexcept override
			{
				return vfs_name_;
			}
			[[nodiscard]] const void* vfs_implementation_identity() const noexcept override
			{
				return &wrapper_;
			}
			[[nodiscard]] result<sqlite_wal_recovery_workspace_receipt>
			snapshot_receipt() const override;
			[[nodiscard]] result<void> verify_sealed_objects() const override;

			[[nodiscard]] result<std::string> digest_exact(int descriptor,
														   std::uint64_t byte_count) const;
			[[nodiscard]] bool validate_wal_binding(
				const sqlite_wal_recovery_workspace_expectation& expectation) const noexcept;

			static int vfs_open(sqlite3_vfs* vfs,
								const char* name,
								sqlite3_file* output,
								int flags,
								int* out_flags) noexcept;
			static int vfs_remove(sqlite3_vfs* vfs, const char* name, int sync_directory) noexcept;
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

			std::shared_ptr<workspace_runtime> runtime_;
			int main_descriptor_{-1};
			int wal_descriptor_{-1};
			sqlite_backend_opaque_identity source_token_;
			std::string token_;
			std::string path_;
			std::string wal_path_;
			std::string journal_path_;
			std::string vfs_name_;
			sqlite3_vfs wrapper_{};
			mutable std::mutex mutex_;
			std::atomic<bool> sealed_;
			std::atomic<std::size_t> open_file_count_;
			std::uint64_t main_byte_count_{};
			std::uint64_t wal_byte_count_{};
			sqlite_wal_recovery_workspace_receipt receipt_;
			std::vector<std::vector<std::byte>> shm_regions_;
			std::array<std::uint8_t, sqlite_shm_lock_slot_count> shm_locks_{};
			int shm_region_size_{};
			bool terminal_{};
			bool registered_{};
			bool receipt_accounting_valid_{true};
		};

		void invalidate_receipt_accounting(workspace_state& owner) noexcept
		{
			owner.receipt_accounting_valid_ = false;
			owner.receipt_.effects.only_private_shm_mutation_permitted = false;
		}

		result<void> workspace_state::append(const sqlite_wal_recovery_copy_role role,
											 const std::span<const std::byte> bytes)
		{
			std::scoped_lock lock{mutex_};
			if (terminal_ || sealed_.load(std::memory_order_acquire))
				return unexpected(workspace_error("append-after-seal"));
			if (role != sqlite_wal_recovery_copy_role::main_database &&
				role != sqlite_wal_recovery_copy_role::authoritative_wal_prefix)
				return unexpected(workspace_error("copy-role"));
			if (bytes.size() > sqlite_wal_recovery_copy_buffer_bound)
				return unexpected(workspace_error("copy-window"));
			auto* byte_count = role == sqlite_wal_recovery_copy_role::main_database
				? &main_byte_count_
				: &wal_byte_count_;
			const auto descriptor = role == sqlite_wal_recovery_copy_role::main_database
				? main_descriptor_
				: wal_descriptor_;
			const auto added = static_cast<std::uint64_t>(bytes.size());
			if (static_cast<std::size_t>(added) != bytes.size() ||
				*byte_count > sqlite_sha256_maximum_byte_count ||
				added > sqlite_sha256_maximum_byte_count - *byte_count)
				return unexpected(workspace_error("copy-size"));
#if defined(__unix__) || defined(__APPLE__)
			const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
			if (*byte_count > maximum || added > maximum - *byte_count)
				return unexpected(workspace_error("copy-size"));
			std::size_t consumed{};
			while (consumed < bytes.size())
			{
				const auto count = ::pwrite(descriptor,
											bytes.data() + consumed,
											bytes.size() - consumed,
											static_cast<off_t>(*byte_count + consumed));
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return unexpected(workspace_error("copy-write"));
			}
			*byte_count += added;
			return {};
#else
			(void)descriptor;
			(void)bytes;
			return unexpected(workspace_error("platform"));
#endif
		}

		result<std::string> workspace_state::digest_exact(const int descriptor,
														  const std::uint64_t byte_count) const
		{
#if defined(__unix__) || defined(__APPLE__)
			struct stat before
			{
			};
			if (::fstat(descriptor, &before) != 0 || before.st_size < 0 ||
				static_cast<std::uint64_t>(before.st_size) != byte_count)
				return unexpected(workspace_error("copy-read"));
			sqlite_incremental_sha256 digest;
			std::array<std::byte, sqlite_wal_recovery_copy_buffer_bound> buffer{};
			std::uint64_t offset{};
			while (offset < byte_count)
			{
				const auto wanted = static_cast<std::size_t>(
					std::min<std::uint64_t>(buffer.size(), byte_count - offset));
				std::size_t consumed{};
				while (consumed < wanted)
				{
					const auto count = ::pread(descriptor,
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
					return unexpected(workspace_error("copy-read"));
				}
				if (auto updated = digest.update(std::span{buffer}.first(wanted)); !updated)
					return unexpected(workspace_error("copy-read"));
				offset += static_cast<std::uint64_t>(wanted);
			}
			struct stat after
			{
			};
			if (::fstat(descriptor, &after) != 0 || before.st_dev != after.st_dev ||
				before.st_ino != after.st_ino || before.st_size != after.st_size ||
				before.st_mtim.tv_sec != after.st_mtim.tv_sec ||
				before.st_mtim.tv_nsec != after.st_mtim.tv_nsec)
				return unexpected(workspace_error("copy-drift"));
			auto finished = digest.finish();
			if (!finished)
				return unexpected(workspace_error("copy-read"));
			return std::move(*finished);
#else
			(void)descriptor;
			(void)byte_count;
			return unexpected(workspace_error("platform"));
#endif
		}

		bool workspace_state::validate_wal_binding(
			const sqlite_wal_recovery_workspace_expectation& expectation) const noexcept
		{
			const auto& scan = expectation.source_wal_scan;
			if (scan.classification == sqlite_wal_scan_classification::empty)
				return scan.stop == sqlite_wal_scan_stop::end_of_input && !scan.header.has_value() &&
					!scan.last_valid_frame.has_value() && !scan.last_valid_commit.has_value() &&
					scan.inspected_byte_count == 0U && scan.validated_prefix_byte_count == 0U &&
					scan.authoritative_prefix_byte_count == 0U && scan.valid_frame_count == 0U &&
					scan.valid_commit_count == 0U && scan.torn_remainder_byte_count == 0U &&
					expectation.authoritative_wal_prefix.byte_count == 0U;
			if (scan.classification == sqlite_wal_scan_classification::committed_prefix)
				return scan.header.has_value() && scan.last_valid_commit.has_value() &&
					scan.authoritative_prefix_byte_count > sqlite_wal_header_byte_count &&
					scan.authoritative_prefix_byte_count ==
					expectation.authoritative_wal_prefix.byte_count;
			if (scan.classification == sqlite_wal_scan_classification::no_valid_commit)
				return scan.header.has_value() && !scan.last_valid_commit.has_value() &&
					scan.authoritative_prefix_byte_count == 0U &&
					expectation.authoritative_wal_prefix.byte_count == 0U;
			return false;
		}

		result<std::shared_ptr<sqlite_wal_recovery_workspace>>
		workspace_state::seal(sqlite_wal_recovery_workspace_expectation expectation)
		{
			std::scoped_lock lock{mutex_};
			if (terminal_ || sealed_.load(std::memory_order_acquire))
				return unexpected(workspace_error("seal-state"));
			terminal_ = true;
			if (expectation.main_database.byte_count == 0U ||
				expectation.main_database.byte_count != main_byte_count_ ||
				expectation.authoritative_wal_prefix.byte_count != wal_byte_count_ ||
				!canonical_sha256(expectation.main_database.sha256) ||
				!canonical_sha256(expectation.authoritative_wal_prefix.sha256) ||
				!validate_wal_binding(expectation))
				return unexpected(workspace_error("seal-binding"));

			auto main_digest = digest_exact(main_descriptor_, main_byte_count_);
			auto wal_digest = digest_exact(wal_descriptor_, wal_byte_count_);
			if (!main_digest || !wal_digest || *main_digest != expectation.main_database.sha256 ||
				*wal_digest != expectation.authoritative_wal_prefix.sha256)
				return unexpected(workspace_error("seal-binding"));

			if (wal_byte_count_ != 0U)
			{
				descriptor_byte_source source{{wal_descriptor_, wal_byte_count_}};
				auto scanned = scan_sqlite_wal(source);
				const auto& expected_scan = expectation.source_wal_scan;
				if (!scanned ||
					scanned->classification != sqlite_wal_scan_classification::committed_prefix ||
					scanned->stop != sqlite_wal_scan_stop::end_of_input ||
					scanned->header != expected_scan.header ||
					scanned->last_valid_commit != expected_scan.last_valid_commit ||
					scanned->authoritative_prefix_byte_count != wal_byte_count_ ||
					scanned->validated_prefix_byte_count != wal_byte_count_ ||
					scanned->valid_commit_count != expected_scan.valid_commit_count ||
					scanned->valid_frame_count != expected_scan.last_valid_commit->frame_number)
					return unexpected(workspace_error("wal-prefix-binding"));
			}

#if defined(__linux__)
			constexpr int required_seals = F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
			for (const auto descriptor : {main_descriptor_, wal_descriptor_})
				if (::fcntl(descriptor, F_ADD_SEALS, required_seals) != 0 ||
					(::fcntl(descriptor, F_GET_SEALS) & required_seals) != required_seals)
					return unexpected(workspace_error("seal"));
#else
			return unexpected(workspace_error("platform"));
#endif

			main_digest = digest_exact(main_descriptor_, main_byte_count_);
			wal_digest = digest_exact(wal_descriptor_, wal_byte_count_);
			if (!main_digest || !wal_digest || *main_digest != expectation.main_database.sha256 ||
				*wal_digest != expectation.authoritative_wal_prefix.sha256)
				return unexpected(workspace_error("seal-binding"));
			try
			{
				receipt_ = {std::string{workspace_profile},
							source_token_,
							std::move(expectation.main_database),
							std::move(expectation.authoritative_wal_prefix),
							expectation.source_wal_scan,
							{},
							{},
							{},
							wal_byte_count_ != 0U,
							true};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(workspace_error("allocation"));
			}

			sealed_.store(true, std::memory_order_release);
			{
				std::scoped_lock registry_lock{workspace_registration_mutex};
				if (runtime_->registry.find(vfs_name_.c_str()) != nullptr ||
					runtime_->registry.register_vfs(&wrapper_, 0) != sqlite_ok ||
					runtime_->registry.find(vfs_name_.c_str()) != &wrapper_)
				{
					sealed_.store(false, std::memory_order_release);
					return unexpected(workspace_error("vfs-register"));
				}
				registered_ = true;
			}
			auto self = shared_from_this();
			return std::static_pointer_cast<sqlite_wal_recovery_workspace>(std::move(self));
		}

		result<sqlite_wal_recovery_workspace_receipt> workspace_state::snapshot_receipt() const
		{
			std::scoped_lock lock{mutex_};
			if (!receipt_accounting_valid_)
				return unexpected(workspace_error("receipt-counter-overflow"));
			try
			{
				return receipt_;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(workspace_error("allocation"));
			}
		}

		result<void> workspace_state::verify_sealed_objects() const
		{
			std::scoped_lock lock{mutex_};
			if (!sealed_.load(std::memory_order_acquire) || !receipt_.sealed)
				return unexpected(workspace_error("verify-state"));
			if (!receipt_accounting_valid_)
				return unexpected(workspace_error("receipt-counter-overflow"));
			auto main_digest = digest_exact(main_descriptor_, receipt_.main_database.byte_count);
			auto wal_digest =
				digest_exact(wal_descriptor_, receipt_.authoritative_wal_prefix.byte_count);
			if (!main_digest || !wal_digest || *main_digest != receipt_.main_database.sha256 ||
				*wal_digest != receipt_.authoritative_wal_prefix.sha256)
				return unexpected(workspace_error("sealed-object-drift"));
			return {};
		}

		int recovery_close(sqlite3_file* base) noexcept
		{
			auto* file = recovery_file(base);
			const auto closed = file->descriptor < 0 || ::close(file->descriptor) == 0;
			file->descriptor = -1;
			file->owner->open_file_count_.fetch_sub(1U, std::memory_order_acq_rel);
			file->~workspace_file();
			return closed ? sqlite_ok : sqlite_io_error;
		}

		int recovery_read(sqlite3_file* base,
						  void* output,
						  const int count,
						  const long long offset) noexcept
		{
			if (output == nullptr || count < 0 || offset < 0)
				return sqlite_io_error;
			const auto unsigned_offset = static_cast<std::uint64_t>(offset);
			const auto maximum_offset =
				static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
			if (unsigned_offset > maximum_offset ||
				static_cast<std::uint64_t>(count) > maximum_offset - unsigned_offset)
				return sqlite_io_error;
			auto* file = recovery_file(base);
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

		int recovery_write(sqlite3_file* base, const void*, int, long long) noexcept
		{
			try
			{
				auto* file = recovery_file(base);
				std::scoped_lock lock{file->owner->mutex_};
				if (!file->owner->receipt_accounting_valid_)
					return sqlite_io_error;
				auto& effects = file->owner->receipt_.effects;
				auto& counter = file->role == workspace_file_role::main_database
					? effects.denied_main_write_count
					: effects.denied_wal_write_count;
				if (!checked_increment(counter))
				{
					invalidate_receipt_accounting(*file->owner);
					return sqlite_io_error;
				}
				return sqlite_readonly;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int recovery_truncate(sqlite3_file* base, long long) noexcept
		{
			try
			{
				auto* file = recovery_file(base);
				std::scoped_lock lock{file->owner->mutex_};
				if (!file->owner->receipt_accounting_valid_)
					return sqlite_io_error;
				auto& effects = file->owner->receipt_.effects;
				auto& counter = file->role == workspace_file_role::main_database
					? effects.denied_main_truncate_count
					: effects.denied_wal_truncate_count;
				if (!checked_increment(counter))
				{
					invalidate_receipt_accounting(*file->owner);
					return sqlite_io_error;
				}
				return sqlite_readonly;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int recovery_sync(sqlite3_file* base, int) noexcept
		{
			try
			{
				auto* file = recovery_file(base);
				std::scoped_lock lock{file->owner->mutex_};
				if (!file->owner->receipt_accounting_valid_)
					return sqlite_io_error;
				auto& counter = file->role == workspace_file_role::main_database
					? file->owner->receipt_.effects.main_sync_request_count
					: file->owner->receipt_.effects.wal_sync_request_count;
				if (!checked_increment(counter))
				{
					invalidate_receipt_accounting(*file->owner);
					return sqlite_io_error;
				}
				return sqlite_ok;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int recovery_file_size(sqlite3_file* base, long long* output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = 0;
			struct stat observed
			{
			};
			if (::fstat(recovery_file(base)->descriptor, &observed) != 0 || observed.st_size < 0)
				return sqlite_io_error;
			*output = static_cast<long long>(observed.st_size);
			return sqlite_ok;
		}

		int recovery_lock(sqlite3_file* base, const int level) noexcept
		{
			auto* file = recovery_file(base);
			if (level < 0 || level > 4 || level < file->lock_level)
				return sqlite_io_error;
			file->lock_level = level;
			return sqlite_ok;
		}

		int recovery_unlock(sqlite3_file* base, const int level) noexcept
		{
			auto* file = recovery_file(base);
			if (level < 0 || level > 4 || level > file->lock_level)
				return sqlite_io_error;
			file->lock_level = level;
			return sqlite_ok;
		}

		int recovery_reserved(sqlite3_file* base, int* output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = recovery_file(base)->lock_level >= 2 ? 1 : 0;
			return sqlite_ok;
		}

		int recovery_control(sqlite3_file* base, const int operation, void* output) noexcept
		{
			if (output == nullptr)
				return sqlite_not_found;
			switch (operation)
			{
				case sqlite_file_control_lock_state:
					*static_cast<int*>(output) = recovery_file(base)->lock_level;
					return sqlite_ok;
				case sqlite_file_control_persist_wal:
					*static_cast<int*>(output) = 1;
					return sqlite_ok;
				case sqlite_file_control_powersafe_overwrite:
				case sqlite_file_control_has_moved:
					*static_cast<int*>(output) = 0;
					return sqlite_ok;
				default:
					return sqlite_not_found;
			}
		}

		int recovery_sector(sqlite3_file*) noexcept
		{
			return 4096;
		}

		int recovery_characteristics(sqlite3_file*) noexcept
		{
			return 0;
		}

		// SQLite fixes this callback ABI; the adjacent integer roles cannot use strong types.
		int recovery_shm_map(sqlite3_file* base,
							 const int page,
							 // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
							 const int page_size,
							 const int extend,
							 volatile void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			auto* file = recovery_file(base);
			if (file->role != workspace_file_role::main_database || page < 0 || page_size <= 0 ||
				(extend != 0 && extend != 1) || page_size > maximum_shm_region_bytes ||
				static_cast<std::size_t>(page) >= maximum_shm_region_count)
				return sqlite_io_error;
			try
			{
				std::scoped_lock lock{file->owner->mutex_};
				auto* owner = file->owner;
				if (!owner->receipt_accounting_valid_)
					return sqlite_io_error;
				if (!checked_increment(owner->receipt_.shm.map_request_count))
				{
					invalidate_receipt_accounting(*owner);
					return sqlite_io_error;
				}
				if (owner->shm_region_size_ != 0 && owner->shm_region_size_ != page_size)
					return sqlite_io_error;
				const auto index = static_cast<std::size_t>(page);
				const auto exists =
					owner->shm_regions_.size() > index && !owner->shm_regions_[index].empty();
				if (!exists)
				{
					if (extend == 0)
						return sqlite_ok;
					std::uint64_t next_region_count{};
					std::uint64_t next_bytes{};
					constexpr auto maximum_shm_bytes = static_cast<std::uint64_t>(
						maximum_shm_region_count *
						static_cast<std::size_t>(maximum_shm_region_bytes));
					if (!sqlite_checked_add_u64(
							owner->receipt_.shm.created_region_count, 1U, next_region_count) ||
						!sqlite_checked_add_u64(owner->receipt_.shm.private_byte_count,
												static_cast<std::uint64_t>(page_size),
												next_bytes) ||
						next_region_count > maximum_shm_region_count ||
						next_bytes > maximum_shm_bytes)
					{
						invalidate_receipt_accounting(*owner);
						return sqlite_io_error;
					}
					std::vector<std::byte> region(static_cast<std::size_t>(page_size), std::byte{});
					if (owner->shm_regions_.size() <= index)
						owner->shm_regions_.resize(index + 1U);
					owner->shm_regions_[index] = std::move(region);
					owner->receipt_.shm.created_region_count = next_region_count;
					owner->receipt_.shm.private_byte_count = next_bytes;
				}
				owner->shm_region_size_ = page_size;
				*output = owner->shm_regions_[index].data();
				return sqlite_ok;
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		// SQLite fixes this callback ABI; the adjacent integer roles cannot use strong types.
		int recovery_shm_lock(sqlite3_file* base,
							  const int offset,
							  // NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
							  const int count,
							  const int flags) noexcept
		{
			auto* file = recovery_file(base);
			if (file->role != workspace_file_role::main_database || offset < 0 || count <= 0 ||
				static_cast<std::size_t>(offset) >= sqlite_shm_lock_slot_count ||
				static_cast<std::size_t>(count) >
					sqlite_shm_lock_slot_count - static_cast<std::size_t>(offset))
				return sqlite_io_error;
			const auto action = flags & (sqlite_shm_lock | sqlite_shm_unlock);
			const auto mode = flags & (sqlite_shm_shared | sqlite_shm_exclusive);
			if ((action != sqlite_shm_lock && action != sqlite_shm_unlock) ||
				(mode != sqlite_shm_shared && mode != sqlite_shm_exclusive) ||
				flags != (action | mode) || (mode == sqlite_shm_shared && count != 1))
				return sqlite_io_error;
			try
			{
				std::scoped_lock lock{file->owner->mutex_};
				auto* owner = file->owner;
				if (!owner->receipt_accounting_valid_)
					return sqlite_io_error;
				auto& request_count = action == sqlite_shm_lock
					? owner->receipt_.shm.lock_request_count
					: owner->receipt_.shm.unlock_request_count;
				if (!checked_increment(request_count))
				{
					invalidate_receipt_accounting(*owner);
					return sqlite_io_error;
				}
				for (auto index = offset; index < offset + count; ++index)
				{
					auto& slot = owner->shm_locks_.at(static_cast<std::size_t>(index));
					if (action == sqlite_shm_lock)
					{
						if (slot == 0U && !checked_increment(owner->receipt_.shm.held_lock_count))
						{
							invalidate_receipt_accounting(*owner);
							return sqlite_io_error;
						}
						slot = mode == sqlite_shm_exclusive ? 2U : 1U;
					}
					else
					{
						if (slot != 0U)
						{
							if (owner->receipt_.shm.held_lock_count == 0U)
							{
								invalidate_receipt_accounting(*owner);
								return sqlite_io_error;
							}
							--owner->receipt_.shm.held_lock_count;
						}
						slot = 0U;
					}
				}
				return sqlite_ok;
			}
			catch (...)
			{
				return sqlite_busy;
			}
		}

		void recovery_shm_barrier(sqlite3_file* base) noexcept
		{
			std::atomic_thread_fence(std::memory_order_seq_cst);
			auto* file = recovery_file(base);
			std::scoped_lock lock{file->owner->mutex_};
			if (!file->owner->receipt_accounting_valid_ ||
				!checked_increment(file->owner->receipt_.shm.barrier_count))
				invalidate_receipt_accounting(*file->owner);
		}

		int recovery_shm_unmap(sqlite3_file* base, const int delete_flag) noexcept
		{
			if (delete_flag != 0 && delete_flag != 1)
				return sqlite_io_error;
			try
			{
				auto* file = recovery_file(base);
				std::scoped_lock lock{file->owner->mutex_};
				if (!file->owner->receipt_accounting_valid_ ||
					!checked_increment(file->owner->receipt_.shm.unmap_request_count))
				{
					invalidate_receipt_accounting(*file->owner);
					return sqlite_io_error;
				}
				return sqlite_ok;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int recovery_fetch(sqlite3_file*, long long, int, void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			return sqlite_ok;
		}

		int recovery_unfetch(sqlite3_file*, long long, void*) noexcept
		{
			return sqlite_ok;
		}

		int workspace_state::vfs_open(sqlite3_vfs* vfs,
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
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			if (!owner->sealed_.load(std::memory_order_acquire))
				return sqlite_cannot_open;
			const std::string_view path{name};
			const auto file_type = flags & sqlite_open_file_type_mask;
			const auto main_request =
				path == owner->path_ && file_type == sqlite_open_main_database;
			const auto wal_request = path == owner->wal_path_ &&
				file_type == sqlite_open_write_ahead_log && owner->wal_byte_count_ != 0U;
			const auto journal_request =
				path == owner->journal_path_ || file_type == sqlite_open_main_journal;
			try
			{
				std::scoped_lock lock{owner->mutex_};
				if (!owner->receipt_accounting_valid_)
					return sqlite_cannot_open;
				if (journal_request)
				{
					if (!checked_increment(
							owner->receipt_.effects.denied_rollback_journal_open_count))
						invalidate_receipt_accounting(*owner);
					return sqlite_cannot_open;
				}
				if (!main_request && !wal_request)
				{
					if (!checked_increment(owner->receipt_.effects.denied_other_open_count))
						invalidate_receipt_accounting(*owner);
					return sqlite_cannot_open;
				}
				auto& opens = owner->receipt_.opens;
				if (main_request)
				{
					if (!checked_increment(opens.main_attempt_count))
					{
						invalidate_receipt_accounting(*owner);
						return sqlite_cannot_open;
					}
					opens.last_main_input_flags = flags;
				}
				else
				{
					if (!checked_increment(opens.wal_attempt_count))
					{
						invalidate_receipt_accounting(*owner);
						return sqlite_cannot_open;
					}
					opens.last_wal_input_flags = flags;
				}
			}
			catch (...)
			{
				return sqlite_cannot_open;
			}

			if ((flags & sqlite_open_readwrite) == 0 || (flags & sqlite_open_readonly) != 0 ||
				(flags & (sqlite_open_delete_on_close | sqlite_open_uri)) != 0 ||
				(main_request && (flags & sqlite_open_create) != 0))
				return sqlite_cannot_open;
#if defined(__unix__) || defined(__APPLE__)
			const auto source_descriptor =
				main_request ? owner->main_descriptor_ : owner->wal_descriptor_;
			const auto duplicate = ::fcntl(source_descriptor, F_DUPFD_CLOEXEC, 0);
			if (duplicate < 0)
				return sqlite_cannot_open;
			auto* file = new (output) workspace_file{};
			file->owner = owner;
			file->descriptor = duplicate;
			file->role = main_request ? workspace_file_role::main_database
									  : workspace_file_role::write_ahead_log;
			owner->open_file_count_.fetch_add(1U, std::memory_order_acq_rel);
			const auto returned_flags = sqlite_open_readwrite |
				(main_request ? sqlite_open_main_database : sqlite_open_write_ahead_log);
			if (out_flags != nullptr)
				*out_flags = returned_flags;
			file->base.methods = &recovery_io_methods;
			try
			{
				std::scoped_lock lock{owner->mutex_};
				if (!owner->receipt_accounting_valid_)
					throw std::runtime_error{"receipt accounting invalid"};
				auto& opens = owner->receipt_.opens;
				if (main_request)
				{
					if (!checked_increment(opens.main_success_count))
					{
						invalidate_receipt_accounting(*owner);
						throw std::runtime_error{"receipt counter overflow"};
					}
					opens.last_main_output_flags = returned_flags;
					opens.main_readwrite_no_create = true;
				}
				else
				{
					if (!checked_increment(opens.wal_success_count))
					{
						invalidate_receipt_accounting(*owner);
						throw std::runtime_error{"receipt counter overflow"};
					}
					opens.last_wal_output_flags = returned_flags;
					opens.wal_was_preexisting = true;
				}
			}
			catch (...)
			{
				(void)::close(duplicate);
				owner->open_file_count_.fetch_sub(1U, std::memory_order_acq_rel);
				file->~workspace_file();
				output->methods = nullptr;
				return sqlite_cannot_open;
			}
			return sqlite_ok;
#else
			return sqlite_cannot_open;
#endif
		}

		int workspace_state::vfs_remove(sqlite3_vfs* vfs, const char*, int) noexcept
		{
			if (vfs != nullptr && vfs->app_data != nullptr)
				try
				{
					auto* owner = static_cast<workspace_state*>(vfs->app_data);
					std::scoped_lock lock{owner->mutex_};
					if (!owner->receipt_accounting_valid_ ||
						!checked_increment(owner->receipt_.effects.denied_delete_count))
					{
						invalidate_receipt_accounting(*owner);
						return sqlite_io_error;
					}
				}
				catch (...)
				{
					return sqlite_io_error;
				}
			return sqlite_readonly;
		}

		int
		workspace_state::vfs_access(sqlite3_vfs* vfs, const char* name, int, int* output) noexcept
		{
			if (output == nullptr)
				return sqlite_error;
			*output = 0;
			if (vfs == nullptr || vfs->app_data == nullptr || name == nullptr)
				return sqlite_ok;
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			const std::string_view path{name};
			if (owner->sealed_.load(std::memory_order_acquire) &&
				(path == owner->path_ ||
				 (path == owner->wal_path_ && owner->wal_byte_count_ != 0U)))
				*output = 1;
			return sqlite_ok;
		}

		int workspace_state::vfs_full_path(sqlite3_vfs* vfs,
										   const char* name,
										   const int size,
										   char* output) noexcept
		{
			if (output == nullptr || size <= 0)
				return sqlite_cannot_open;
			output[0] = '\0';
			if (vfs == nullptr || vfs->app_data == nullptr || name == nullptr)
				return sqlite_cannot_open;
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			const std::string_view input{name};
			if (input != owner->path_ && input != owner->wal_path_ && input != owner->journal_path_)
				return sqlite_cannot_open;
			if (input.size() + 1U > static_cast<std::size_t>(size))
				return sqlite_cannot_open;
			std::memcpy(output, input.data(), input.size());
			output[input.size()] = '\0';
			return sqlite_ok;
		}

		void* workspace_state::vfs_dl_open(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		void workspace_state::vfs_dl_error(sqlite3_vfs*, const int size, char* output) noexcept
		{
			if (output != nullptr && size > 0)
				output[0] = '\0';
		}

		void (*workspace_state::vfs_dl_sym(sqlite3_vfs*, void*, const char*) noexcept)(void)
		{
			return nullptr;
		}

		void workspace_state::vfs_dl_close(sqlite3_vfs*, void*) noexcept {}

		int workspace_state::vfs_randomness(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			return owner->runtime_->delegate->randomness(owner->runtime_->delegate, size, output);
		}

		int workspace_state::vfs_sleep(sqlite3_vfs* vfs, const int microseconds) noexcept
		{
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			return owner->runtime_->delegate->sleep(owner->runtime_->delegate, microseconds);
		}

		int workspace_state::vfs_current_time(sqlite3_vfs* vfs, double* output) noexcept
		{
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			return owner->runtime_->delegate->current_time(owner->runtime_->delegate, output);
		}

		int workspace_state::vfs_last_error(sqlite3_vfs*, const int size, char* output) noexcept
		{
			if (output != nullptr && size > 0)
				output[0] = '\0';
			return sqlite_not_found;
		}

		int workspace_state::vfs_current_time_int64(sqlite3_vfs* vfs, long long* output) noexcept
		{
			auto* owner = static_cast<workspace_state*>(vfs->app_data);
			return owner->runtime_->delegate->version >= 2 &&
					owner->runtime_->delegate->current_time_int64 != nullptr
				? owner->runtime_->delegate->current_time_int64(owner->runtime_->delegate, output)
				: sqlite_not_found;
		}

		int workspace_state::vfs_set_system_call(sqlite3_vfs*,
												 const char*,
												 sqlite3_syscall_ptr) noexcept
		{
			return sqlite_not_found;
		}

		sqlite3_syscall_ptr workspace_state::vfs_get_system_call(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		const char* workspace_state::vfs_next_system_call(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}
	} // namespace

	result<std::shared_ptr<sqlite_wal_recovery_workspace_builder>>
	make_sqlite_wal_recovery_workspace_builder(
		sqlite_backend_opaque_identity source_capability_token,
		sqlite_private_snapshot_registry_binding registry)
	{
		if (source_capability_token.profile.empty() || source_capability_token.bytes.empty())
			return unexpected(workspace_error("source-binding"));
		auto runtime = bind_runtime(std::move(registry));
		if (!runtime)
			return unexpected(std::move(runtime.error()));
		auto token = random_token();
		if (!token)
			return unexpected(std::move(token.error()));
#if defined(__linux__) && defined(SYS_memfd_create)
		const auto main_descriptor =
			static_cast<int>(::syscall(SYS_memfd_create,
									   "cxxlens-sqlite-wal-recovery-main-v1",
									   MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (main_descriptor < 0)
			return unexpected(workspace_error("create"));
		const auto wal_descriptor = static_cast<int>(::syscall(SYS_memfd_create,
															   "cxxlens-sqlite-wal-recovery-wal-v1",
															   MFD_CLOEXEC | MFD_ALLOW_SEALING));
		if (wal_descriptor < 0)
		{
			(void)::close(main_descriptor);
			return unexpected(workspace_error("create"));
		}
		try
		{
			auto output = std::make_shared<workspace_state>(
				std::move(*runtime),
				workspace_descriptors{main_descriptor, wal_descriptor},
				std::move(source_capability_token),
				std::move(*token));
			return std::static_pointer_cast<sqlite_wal_recovery_workspace_builder>(
				std::move(output));
		}
		catch (const std::bad_alloc&)
		{
			(void)::close(main_descriptor);
			(void)::close(wal_descriptor);
			return unexpected(workspace_error("allocation"));
		}
		catch (const std::length_error&)
		{
			(void)::close(main_descriptor);
			(void)::close(wal_descriptor);
			return unexpected(workspace_error("allocation"));
		}
#else
		return unexpected(workspace_error("platform"));
#endif
	}
} // namespace cxxlens::sdk
