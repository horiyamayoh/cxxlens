#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <map>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdk/sqlite_backend_effect_gate_internal.hpp"
#include "sdk/sqlite_default_forwarding_vfs_internal.hpp"
#include "sdk/sqlite_exact_empty_normalization_effect_internal.hpp"
#include "sdk/sqlite_private_snapshot_internal.hpp"
#include "sdk/sqlite_source_shm_readonly_preflight_internal.hpp"
#include "sdk/sqlite_vfs_abi_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	constexpr int sqlite_ok = 0;
	constexpr int sqlite_no_memory = 7;
	constexpr int sqlite_readonly = 8;
	constexpr int sqlite_readonly_cannot_initialize = sqlite_readonly | (5 << 8);
	constexpr int sqlite_io_error = 10;
	constexpr int sqlite_cannot_open = 14;
	constexpr int sqlite_not_found = 12;
	constexpr int sqlite_open_read_only = 0x1;
	constexpr int sqlite_open_read_write = 0x2;
	constexpr int sqlite_open_create = 0x4;
	constexpr int sqlite_open_uri = 0x40;
	constexpr int sqlite_open_full_mutex = 0x00010000;
	constexpr int sqlite_open_main_database = 0x100;
	constexpr int sqlite_open_private_cache = 0x00040000;
	constexpr int sqlite_open_main_journal = 0x800;
	constexpr int sqlite_open_write_ahead_log = 0x80000;
	constexpr int sqlite_file_control_has_moved = 20;
	constexpr int sqlite_file_control_persist_wal = 10;
	constexpr int sqlite_file_control_size_hint = 5;
	constexpr int sqlite_file_control_sync = 21;
	constexpr int sqlite_lock_exclusive = 4;
	constexpr int sqlite_shm_unlock = 1;
	constexpr int sqlite_shm_lock = 2;
	constexpr int sqlite_shm_exclusive = 8;
	constexpr off_t sqlite_unix_shm_deadman_switch_offset = 128;

	using sqlite3_file = sqlite_vfs_abi::file;
	using sqlite3_io_methods = sqlite_vfs_abi::io_methods;
	using sqlite3_vfs = sqlite_vfs_abi::vfs;
	struct sqlite3;
	using sqlite3_syscall_ptr = sqlite_vfs_abi::syscall_ptr;

	struct alignas(std::max_align_t) fake_file
	{
		sqlite3_file base{};
		std::uint64_t marker{};
	};

	int app_data_sentinel{};
	int runtime_sentinel{};
	sqlite3_vfs original_vfs{};
	std::map<std::string, void*, std::less<>> registry;
	bool original_pointer_ok{true};
	bool original_app_data_ok{true};
	bool trailing_alignment_ok{true};
	bool fail_next_open{};
	bool throw_on_fake_open{};
	bool override_next_open{};
	int next_open_status{sqlite_ok};
	const sqlite3_io_methods* next_open_methods{};
	int fake_close_result{sqlite_ok};
	bool throw_on_fake_close{};
	bool exclusive_lock_seen{};
	int null_find_calls{};
	int open_calls{};
	int close_calls{};
	int read_calls{};
	int write_calls{};
	int truncate_calls{};
	int remove_calls{};
	int shm_map_calls{};
	int shm_lock_calls{};
	int shm_unmap_calls{};
	int absent_map_trailing_lock_calls{};
	int absent_map_trailing_barrier_calls{};
	int absent_map_trailing_unmap_calls{};
	int absent_fetch_trailing_unfetch_calls{};
	int shm_unmap_result{sqlite_ok};
	int last_shm_remove{-1};
	int last_open_flags{};
	int last_shm_extend{-1};
	int shm_map_result{sqlite_ok};
	bool replace_open_path_on_next_open{};
	bool replace_shm_path_on_next_map{};
	bool mutate_vfs_identity_on_next_shm_map{};
	bool mutate_full_pathname_on_next_shm_map{};
	bool mutate_registry_lookup_on_next_shm_map{};
	bool mutate_close_callback_on_next_shm_map{};
	bool mutate_io_methods_on_next_shm_map{};
	const sqlite3_io_methods* replacement_io_methods_on_next_shm_map{};
	sqlite3_vfs* registry_find_override{};
	bool return_null_shm_mapping_without_extension{};
	bool replacement_succeeded{};
	std::string replacement_path;
	std::array<std::byte, 4096U> shm_page{};
	constexpr int source_shm_application_open_flags = sqlite_open_read_only | sqlite_open_uri |
		sqlite_open_full_mutex | sqlite_open_private_cache;
	constexpr int source_shm_main_xopen_flags =
		sqlite_open_read_only | sqlite_open_uri | sqlite_open_main_database;
	int fake_replacement_close(sqlite3_file* base);
	void mutate_full_methods_close();

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << "FAIL: " << message << '\n';
			std::exit(1);
		}
	}

	class temporary_test_directory
	{
	  public:
		explicit temporary_test_directory(std::string_view label)
		{
			static std::atomic<std::uint64_t> sequence{};
			path_ = std::filesystem::temp_directory_path() /
				("cxxlens-" + std::string{label} + '-' + std::to_string(::getpid()) + '-' +
				 std::to_string(sequence.fetch_add(1U)));
			if (!std::filesystem::create_directory(path_))
				throw std::runtime_error{"test temporary directory creation failed"};
		}

		temporary_test_directory(const temporary_test_directory&) = delete;
		temporary_test_directory& operator=(const temporary_test_directory&) = delete;
		~temporary_test_directory()
		{
			std::error_code error;
			std::filesystem::remove_all(path_, error);
		}

		[[nodiscard]] const std::filesystem::path& path() const noexcept
		{
			return path_;
		}

	  private:
		std::filesystem::path path_;
	};

	class sqlite_close_unknown_port final : public store_operation_port
	{
	  public:
		[[nodiscard]] store_write_outcome
		write_exact(const int descriptor, const std::span<const std::byte> bytes) noexcept override
		{
			return delegate_.write_exact(descriptor, bytes);
		}
		[[nodiscard]] store_sync_outcome
		synchronize(const int descriptor, const store_sync_target target) noexcept override
		{
			return delegate_.synchronize(descriptor, target);
		}
		[[nodiscard]] store_close_outcome close_descriptor(const int descriptor) noexcept override
		{
			return delegate_.close_descriptor(descriptor);
		}
		[[nodiscard]] store_close_outcome
		close_sqlite(store_sqlite_operation_binding) noexcept override
		{
			++sqlite_close_calls_;
			return {store_close_state::outcome_unknown, EIO, true};
		}
		[[nodiscard]] store_commit_outcome
		commit_sqlite(const store_sqlite_operation_binding binding) noexcept override
		{
			return delegate_.commit_sqlite(binding);
		}
		[[nodiscard]] std::size_t sqlite_close_calls() const noexcept
		{
			return sqlite_close_calls_;
		}

	  private:
		default_store_operation_port delegate_;
		std::size_t sqlite_close_calls_{};
	};

	[[nodiscard]] sqlite_backend_opaque_identity test_receipt(const std::string_view label)
	{
		sqlite_backend_opaque_identity output{"test.effect-receipt.v1", {}};
		for (const auto value : label)
			output.bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
		if (output.bytes.empty())
			output.bytes.push_back(std::byte{0U});
		return output;
	}

	class test_target_namespace_epoch final : public sqlite_source_shm_target_namespace_epoch
	{
	  public:
		test_target_namespace_epoch(std::string logical,
									std::string anchored,
									sqlite_backend_namespace_census census)
			: logical_{std::move(logical)}, anchored_{std::move(anchored)},
			  identity_{test_receipt("target-epoch")}, census_{std::move(census)}
		{
		}

		[[nodiscard]] std::string_view logical_main_locator() const noexcept override
		{
			return logical_;
		}
		[[nodiscard]] std::string_view anchored_main_locator() const noexcept override
		{
			return anchored_;
		}
		[[nodiscard]] const sqlite_backend_opaque_identity& identity() const noexcept override
		{
			return identity_;
		}
		[[nodiscard]] const sqlite_backend_opaque_identity&
		parent_namespace_identity() const noexcept override
		{
			return census_.parent_namespace_identity;
		}
		[[nodiscard]] result<sqlite_backend_entry_observation>
		retained_entry(const sqlite_backend_file_role role) const override
		{
			++recheck_calls;
			if (!valid_)
				return unexpected(error{"test.epoch", "sqlite", "invalid"});
			for (const auto& entry : census_.entries)
				if (entry.role == role)
					return entry;
			return unexpected(error{"test.epoch", "sqlite", "missing-role"});
		}
		[[nodiscard]] result<void> recheck() const override
		{
			++recheck_calls;
			return valid_ ? result<void>{} : result<void>{error{"test.epoch", "sqlite", "invalid"}};
		}
		[[nodiscard]] result<void> finish() override
		{
			++finish_calls;
			return recheck();
		}

		void invalidate() noexcept
		{
			valid_ = false;
		}

		mutable int recheck_calls{};
		int finish_calls{};

	  private:
		std::string logical_;
		std::string anchored_;
		sqlite_backend_opaque_identity identity_;
		sqlite_backend_namespace_census census_;
		bool valid_{true};
	};

	class test_arm_authority final : public sqlite_backend_effect_arm_authority
	{
	  public:
		test_arm_authority(const bool succeeds, const bool requires_exclusive) noexcept
			: succeeds_{succeeds}, requires_exclusive_{requires_exclusive}
		{
		}

		[[nodiscard]] result<sqlite_backend_opaque_identity> recheck_and_seal(
			const sqlite_backend_effect_arm_request& request,
			const sqlite_backend_connection_observation_scope& connection) const override
		{
			++calls;
			if (!succeeds_ || request.connection_token != connection.token() ||
				(requires_exclusive_ && !exclusive_lock_seen))
				return unexpected(error{"test.effect-validation", "gate", "recheck-failed"});
			return test_receipt("validated");
		}

		mutable int calls{};

	  private:
		bool succeeds_{};
		bool requires_exclusive_{};
	};

	[[nodiscard]] sqlite_backend_effect_arm_request
	make_arm_request(const sqlite_backend_observation_capability& capability,
					 const sqlite_backend_connection_observation_scope& connection,
					 const std::string_view locator,
					 const sqlite_backend_effect_stage stage,
					 std::shared_ptr<const sqlite_backend_effect_arm_authority> authority,
					 const std::string_view label)
	{
		return {
			stage,
			capability.capability_token(),
			connection.token(),
			std::string{locator},
			test_receipt(label),
			std::move(authority),
		};
	}

	void check_original(sqlite3_vfs* value)
	{
		original_pointer_ok = original_pointer_ok && value == &original_vfs;
		original_app_data_ok =
			original_app_data_ok && value != nullptr && value->app_data == &app_data_sentinel;
	}

	void replace_regular_file(const std::string& path) noexcept
	{
		replacement_succeeded = false;
		try
		{
			const auto staged = path + ".identity-replacement";
			if (::unlink(staged.c_str()) != 0 && errno != ENOENT)
				return;
			const auto descriptor =
				::open(staged.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
			if (descriptor < 0)
				return;
			if (::close(descriptor) != 0)
			{
				(void)::unlink(staged.c_str());
				return;
			}
			if (::rename(staged.c_str(), path.c_str()) != 0)
			{
				(void)::unlink(staged.c_str());
				return;
			}
			replacement_succeeded = true;
		}
		catch (...)
		{
			// Allocation failure leaves the original fixture untouched and reports failure.
		}
	}

	int fake_close(sqlite3_file* base)
	{
		++close_calls;
		if (throw_on_fake_close)
		{
			throw_on_fake_close = false;
			throw std::runtime_error{"fake close failure"};
		}
		base->methods = nullptr;
		return fake_close_result;
	}

	int fake_replacement_close(sqlite3_file* base)
	{
		++close_calls;
		base->methods = nullptr;
		return fake_close_result;
	}

	int fake_read(sqlite3_file*, void* output, const int count, long long)
	{
		++read_calls;
		if (output != nullptr && count > 0)
			std::memset(output, 0, static_cast<std::size_t>(count));
		return sqlite_ok;
	}

	int fake_write(sqlite3_file*, const void*, int, long long)
	{
		++write_calls;
		return sqlite_ok;
	}
	int fake_truncate(sqlite3_file*, long long)
	{
		++truncate_calls;
		return sqlite_ok;
	}
	int fake_sync(sqlite3_file*, int)
	{
		return sqlite_ok;
	}
	int fake_size(sqlite3_file*, long long* output)
	{
		if (output != nullptr)
			*output = 0;
		return sqlite_ok;
	}
	int fake_lock(sqlite3_file*, const int level)
	{
		if (level >= sqlite_lock_exclusive)
			exclusive_lock_seen = true;
		return sqlite_ok;
	}
	int fake_unlock(sqlite3_file*, int)
	{
		return sqlite_ok;
	}
	int fake_reserved(sqlite3_file*, int* output)
	{
		if (output != nullptr)
			*output = 0;
		return sqlite_ok;
	}
	int fake_control(sqlite3_file*, const int operation, void* output)
	{
		if (operation == sqlite_file_control_has_moved && output != nullptr)
		{
			*static_cast<int*>(output) = 0;
			return sqlite_ok;
		}
		return 12;
	}
	int fake_sector(sqlite3_file*)
	{
		return 4096;
	}
	int fake_characteristics(sqlite3_file*)
	{
		return 0;
	}
	int fake_replacement_full_path(sqlite3_vfs* vfs, const char* name, int size, char* output);
	int fake_shm_map(sqlite3_file* base, int, int, const int extend, volatile void** output)
	{
		++shm_map_calls;
		last_shm_extend = extend;
		if (replace_shm_path_on_next_map)
		{
			replace_shm_path_on_next_map = false;
			replace_regular_file(replacement_path);
		}
		if (output != nullptr)
			*output = return_null_shm_mapping_without_extension && extend == 0 ? nullptr
																			   : shm_page.data();
		if (mutate_vfs_identity_on_next_shm_map)
		{
			mutate_vfs_identity_on_next_shm_map = false;
			original_vfs.app_data = &runtime_sentinel;
		}
		if (mutate_full_pathname_on_next_shm_map)
		{
			mutate_full_pathname_on_next_shm_map = false;
			original_vfs.full_pathname = fake_replacement_full_path;
		}
		if (mutate_registry_lookup_on_next_shm_map)
		{
			mutate_registry_lookup_on_next_shm_map = false;
			registry_find_override = &original_vfs;
		}
		if (mutate_close_callback_on_next_shm_map)
		{
			mutate_close_callback_on_next_shm_map = false;
			mutate_full_methods_close();
		}
		if (mutate_io_methods_on_next_shm_map)
		{
			mutate_io_methods_on_next_shm_map = false;
			// A native callback that replaces the live table after returning is an identity drift,
			// even when the forwarding VFS retained a previously inspected trusted copy.
			// The production guard must reject before publishing the native pointer.
			base->methods = replacement_io_methods_on_next_shm_map;
			replacement_io_methods_on_next_shm_map = nullptr;
		}
		return shm_map_result;
	}
	int fake_shm_lock(sqlite3_file*, int, int, int)
	{
		++shm_lock_calls;
		return sqlite_ok;
	}
	void fake_shm_barrier(sqlite3_file*) {}
	int fake_shm_unmap(sqlite3_file*, const int remove_file)
	{
		++shm_unmap_calls;
		last_shm_remove = remove_file;
		return shm_unmap_result;
	}
	int fake_fetch(sqlite3_file*, long long, int, void** output)
	{
		if (output != nullptr)
			*output = nullptr;
		return sqlite_ok;
	}
	int fake_unfetch(sqlite3_file*, long long, void*)
	{
		return sqlite_ok;
	}
	int absent_map_trailing_lock(sqlite3_file*, int, int, int)
	{
		++absent_map_trailing_lock_calls;
		return sqlite_ok;
	}
	void absent_map_trailing_barrier(sqlite3_file*)
	{
		++absent_map_trailing_barrier_calls;
	}
	int absent_map_trailing_unmap(sqlite3_file*, int)
	{
		++absent_map_trailing_unmap_calls;
		return sqlite_ok;
	}
	int absent_fetch_trailing_unfetch(sqlite3_file*, long long, void*)
	{
		++absent_fetch_trailing_unfetch_calls;
		return sqlite_ok;
	}

	sqlite3_io_methods full_methods{3,
									fake_close,
									fake_read,
									fake_write,
									fake_truncate,
									fake_sync,
									fake_size,
									fake_lock,
									fake_unlock,
									fake_reserved,
									fake_control,
									fake_sector,
									fake_characteristics,
									fake_shm_map,
									fake_shm_lock,
									fake_shm_barrier,
									fake_shm_unmap,
									fake_fetch,
									fake_unfetch};
	void mutate_full_methods_close()
	{
		full_methods.close = fake_replacement_close;
	}
	const sqlite3_io_methods journal_methods{3,
											 fake_close,
											 fake_read,
											 fake_write,
											 fake_truncate,
											 fake_sync,
											 fake_size,
											 fake_lock,
											 fake_unlock,
											 fake_reserved,
											 fake_control,
											 fake_sector,
											 fake_characteristics,
											 nullptr,
											 nullptr,
											 nullptr,
											 nullptr,
											 nullptr,
											 nullptr};
	const sqlite3_io_methods absent_shm_lead_trusted_trailing_methods{3,
																	  fake_close,
																	  fake_read,
																	  fake_write,
																	  fake_truncate,
																	  fake_sync,
																	  fake_size,
																	  fake_lock,
																	  fake_unlock,
																	  fake_reserved,
																	  fake_control,
																	  fake_sector,
																	  fake_characteristics,
																	  nullptr,
																	  absent_map_trailing_lock,
																	  absent_map_trailing_barrier,
																	  absent_map_trailing_unmap,
																	  nullptr,
																	  nullptr};
	const sqlite3_io_methods absent_fetch_lead_trusted_trailing_methods{
		3,
		fake_close,
		fake_read,
		fake_write,
		fake_truncate,
		fake_sync,
		fake_size,
		fake_lock,
		fake_unlock,
		fake_reserved,
		fake_control,
		fake_sector,
		fake_characteristics,
		fake_shm_map,
		fake_shm_lock,
		fake_shm_barrier,
		fake_shm_unmap,
		nullptr,
		absent_fetch_trailing_unfetch};
	const sqlite3_io_methods missing_shm_trailing_methods{3,
														  fake_close,
														  fake_read,
														  fake_write,
														  fake_truncate,
														  fake_sync,
														  fake_size,
														  fake_lock,
														  fake_unlock,
														  fake_reserved,
														  fake_control,
														  fake_sector,
														  fake_characteristics,
														  fake_shm_map,
														  nullptr,
														  fake_shm_barrier,
														  fake_shm_unmap,
														  nullptr,
														  nullptr};
	const sqlite3_io_methods missing_fetch_trailing_methods{3,
															fake_close,
															fake_read,
															fake_write,
															fake_truncate,
															fake_sync,
															fake_size,
															fake_lock,
															fake_unlock,
															fake_reserved,
															fake_control,
															fake_sector,
															fake_characteristics,
															fake_shm_map,
															fake_shm_lock,
															fake_shm_barrier,
															fake_shm_unmap,
															fake_fetch,
															nullptr};
	sqlite3_io_methods foreign_trailing_methods{3,
												fake_close,
												fake_read,
												fake_write,
												fake_truncate,
												fake_sync,
												fake_size,
												fake_lock,
												fake_unlock,
												fake_reserved,
												fake_control,
												fake_sector,
												fake_characteristics,
												nullptr,
												nullptr,
												fake_shm_barrier,
												fake_shm_unmap,
												nullptr,
												nullptr};
	int fake_open(
		sqlite3_vfs* vfs, const char* name, sqlite3_file* output, const int flags, int* out_flags)
	{
		check_original(vfs);
		++open_calls;
		last_open_flags = flags;
		trailing_alignment_ok = trailing_alignment_ok &&
			reinterpret_cast<std::uintptr_t>(output) % alignof(fake_file) == 0U;
		if (throw_on_fake_open)
		{
			throw_on_fake_open = false;
			override_next_open = false;
			throw std::runtime_error{"fake open failure"};
		}
		if (fail_next_open)
		{
			fail_next_open = false;
			if (out_flags != nullptr)
				*out_flags = 0x5a5a;
			output->methods = nullptr;
			return sqlite_cannot_open;
		}
		if (override_next_open)
		{
			override_next_open = false;
			output->methods = next_open_methods;
			if (out_flags != nullptr)
				*out_flags = next_open_status == sqlite_ok ? flags : 0x5a5a;
			return next_open_status;
		}
		if (replace_open_path_on_next_open && name != nullptr)
		{
			replace_open_path_on_next_open = false;
			replace_regular_file(name);
		}
		auto* file = new (output) fake_file{};
		file->marker = 0xc0ffeeU;
		file->base.methods =
			(flags & 0x000fff00) == sqlite_open_main_journal ? &journal_methods : &full_methods;
		if (out_flags != nullptr)
			*out_flags = (flags & sqlite_open_read_only) != 0 ? flags : flags | 0x2;
		return sqlite_ok;
	}

	int fake_remove(sqlite3_vfs* vfs, const char*, int)
	{
		check_original(vfs);
		++remove_calls;
		return sqlite_ok;
	}
	int fake_access(sqlite3_vfs* vfs, const char*, int, int* output)
	{
		check_original(vfs);
		if (output != nullptr)
			*output = 1;
		return sqlite_ok;
	}
	int fake_full_path(sqlite3_vfs* vfs, const char* name, const int size, char* output)
	{
		check_original(vfs);
		if (name == nullptr || output == nullptr || size <= 0 ||
			std::strlen(name) + 1U > static_cast<std::size_t>(size))
			return sqlite_cannot_open;
		std::memcpy(output, name, std::strlen(name) + 1U);
		return sqlite_ok;
	}
	int fake_replacement_full_path(sqlite3_vfs* vfs, const char* name, const int size, char* output)
	{
		return fake_full_path(vfs, name, size, output);
	}
	void* fake_dl_open(sqlite3_vfs* vfs, const char*)
	{
		check_original(vfs);
		return &runtime_sentinel;
	}
	void fake_dl_error(sqlite3_vfs* vfs, int size, char* output)
	{
		check_original(vfs);
		if (output != nullptr && size > 0)
			output[0] = '\0';
	}
	void fake_symbol() {}
	void (*fake_dl_sym(sqlite3_vfs* vfs, void*, const char*))(void)
	{
		check_original(vfs);
		return fake_symbol;
	}
	void fake_dl_close(sqlite3_vfs* vfs, void*)
	{
		check_original(vfs);
	}
	int fake_randomness(sqlite3_vfs* vfs, int, char*)
	{
		check_original(vfs);
		return 0;
	}
	int fake_sleep(sqlite3_vfs* vfs, const int value)
	{
		check_original(vfs);
		return value;
	}
	int fake_current_time(sqlite3_vfs* vfs, double* output)
	{
		check_original(vfs);
		if (output != nullptr)
			*output = 1.0;
		return sqlite_ok;
	}
	int fake_last_error(sqlite3_vfs* vfs, int, char*)
	{
		check_original(vfs);
		return 0;
	}
	int fake_current_time_int64(sqlite3_vfs* vfs, long long* output)
	{
		check_original(vfs);
		if (output != nullptr)
			*output = 1;
		return sqlite_ok;
	}
	int fake_set_system_call(sqlite3_vfs* vfs, const char*, sqlite3_syscall_ptr)
	{
		check_original(vfs);
		return sqlite_ok;
	}
	sqlite3_syscall_ptr fake_get_system_call(sqlite3_vfs* vfs, const char*)
	{
		check_original(vfs);
		return fake_symbol;
	}
	const char* fake_next_system_call(sqlite3_vfs* vfs, const char*)
	{
		check_original(vfs);
		return "fake";
	}

	void* fake_find(const char* name)
	{
		if (registry_find_override != nullptr)
		{
			auto* result = registry_find_override;
			registry_find_override = nullptr;
			return result;
		}
		if (name == nullptr)
		{
			++null_find_calls;
			return &original_vfs;
		}
		const auto found = registry.find(name);
		return found == registry.end() ? nullptr : found->second;
	}
	int fake_register(void* value, int)
	{
		auto* vfs = static_cast<sqlite3_vfs*>(value);
		if (vfs == nullptr || vfs->name == nullptr || registry.contains(vfs->name))
			return 1;
		registry.emplace(vfs->name, value);
		return sqlite_ok;
	}
	int fake_unregister(void* value)
	{
		auto* vfs = static_cast<sqlite3_vfs*>(value);
		if (vfs == nullptr || vfs->name == nullptr)
			return 1;
		const auto found = registry.find(vfs->name);
		if (found == registry.end() || found->second != value)
			return 1;
		registry.erase(found);
		return sqlite_ok;
	}

	int fake_runtime_open_v2(const char*, void**, int, const char*)
	{
		return sqlite_cannot_open;
	}
	int fake_runtime_close_v2(void*)
	{
		return sqlite_ok;
	}
	int fake_runtime_exec(
		void*, const char*, sqlite_source_shm_runtime_binding::exec_callback, void*, char**)
	{
		return sqlite_ok;
	}
	const char* fake_runtime_errmsg(void*)
	{
		return "fake";
	}
	void fake_runtime_free(void*) {}
	const char* fake_runtime_source_id()
	{
		return "fake-source-id-v1";
	}

	const char* fake_uri_key(const char* filename, const int requested)
	{
		if (filename == nullptr || requested < 0)
			return nullptr;
		auto* cursor = filename + std::strlen(filename) + 1U;
		for (int index{}; *cursor != '\0'; ++index)
		{
			auto* key = cursor;
			cursor += std::strlen(cursor) + 1U;
			if (*cursor == '\0')
				return nullptr;
			if (index == requested)
				return key;
			cursor += std::strlen(cursor) + 1U;
		}
		return nullptr;
	}

	const char* fake_uri_parameter(const char* filename, const char* requested)
	{
		if (filename == nullptr || requested == nullptr)
			return nullptr;
		auto* cursor = filename + std::strlen(filename) + 1U;
		while (*cursor != '\0')
		{
			auto* key = cursor;
			cursor += std::strlen(cursor) + 1U;
			if (*cursor == '\0')
				return nullptr;
			auto* value = cursor;
			cursor += std::strlen(cursor) + 1U;
			if (std::string_view{key} == requested)
				return value;
		}
		return nullptr;
	}

	std::vector<char> sqlite_uri_filename(
		const std::string_view canonical_locator,
		const std::vector<std::pair<std::string_view, std::string_view>>& parameters)
	{
		std::vector<char> output;
		const auto append = [&](const std::string_view value)
		{
			output.insert(output.end(), value.begin(), value.end());
			output.push_back('\0');
		};
		append(canonical_locator);
		for (const auto& [key, value] : parameters)
		{
			append(key);
			append(value);
		}
		output.push_back('\0');
		return output;
	}

	std::string strict_source_shm_uri(const std::string_view canonical_locator)
	{
		constexpr std::string_view hexadecimal{"0123456789ABCDEF"};
		const auto unreserved = [](const unsigned char value)
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
		};
		std::string output{"file:"};
		for (const auto raw : canonical_locator)
		{
			const auto value = static_cast<unsigned char>(raw);
			if (unreserved(value))
				output.push_back(static_cast<char>(value));
			else
			{
				output.push_back('%');
				output.push_back(hexadecimal[(value >> 4U) & 0x0fU]);
				output.push_back(hexadecimal[value & 0x0fU]);
			}
		}
		output.append("?mode=ro&cache=private&readonly_shm=1");
		return output;
	}

	sqlite_source_shm_runtime_binding
	fake_source_shm_runtime(const std::shared_ptr<void>& runtime_lifetime)
	{
		return {
			&runtime_sentinel,
			&runtime_sentinel,
			runtime_lifetime.get(),
			runtime_lifetime,
			fake_runtime_open_v2,
			fake_runtime_close_v2,
			fake_runtime_exec,
			fake_runtime_errmsg,
			fake_runtime_free,
			fake_runtime_source_id,
			fake_uri_parameter,
			fake_uri_key,
			fake_find,
			fake_register,
			fake_unregister,
		};
	}

	cxxlens::sdk::sqlite_private_snapshot_registry_binding registry_binding()
	{
		auto runtime_lifetime = std::shared_ptr<void>{std::make_shared<int>(7)};
		auto runtime = fake_source_shm_runtime(runtime_lifetime);
		Dl_info image{};
		require(::dladdr(std::bit_cast<const void*>(original_vfs.open), &image) != 0 &&
					image.dli_fbase != nullptr,
				"resolve fake VFS image identity");
		runtime.runtime_image_identity = image.dli_fbase;
		return {
			&runtime_sentinel,
			&original_vfs,
			fake_find,
			fake_register,
			fake_unregister,
			runtime_lifetime,
			runtime,
		};
	}

	void exercise_private_snapshot_digest_binding()
	{
		require(sqlite_private_snapshot_digest_failure_detail(
					sqlite_private_snapshot_digest_failure::source_read) ==
						"private-snapshot-seal-binding" &&
					sqlite_private_snapshot_digest_failure_detail(
						sqlite_private_snapshot_digest_failure::source_drift) ==
						"private-snapshot-seal-binding" &&
					sqlite_private_snapshot_digest_failure_detail(
						sqlite_private_snapshot_digest_failure::hash_update) ==
						"private-snapshot-seal-binding" &&
					sqlite_private_snapshot_digest_failure_detail(
						sqlite_private_snapshot_digest_failure::hash_finalize) ==
						"private-snapshot-allocation" &&
					sqlite_private_snapshot_digest_failure_detail(
						static_cast<sqlite_private_snapshot_digest_failure>(0xffU)) ==
						"private-snapshot-seal-binding",
				"private snapshot typed digest failure projection");
		const auto initial_registry_size = registry.size();
		{
			auto builder = make_sqlite_private_snapshot_builder(
				{"test.sqlite-private-snapshot-source.v1",
				 {std::byte{0x73U}, std::byte{0x6eU}, std::byte{0x61U}, std::byte{0x70U}}},
				registry_binding());
			require(builder.has_value(), "private snapshot builder");
			const std::array payload{std::byte{0x61U}, std::byte{0x62U}, std::byte{0x63U}};
			require((*builder)->append(payload).has_value(), "private snapshot append");

			const std::string mismatched_digest = "sha256:" + std::string(64U, '0');
			auto mismatch = (*builder)->seal(payload.size(), mismatched_digest);
			require(!mismatch.has_value() &&
						mismatch.error() ==
							error{"store.backend-unavailable",
								  "sqlite",
								  "private-snapshot-seal-binding"},
					"private snapshot digest mismatch exact error");

			constexpr std::string_view expected_digest =
				"sha256:ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
			auto sealed = (*builder)->seal(payload.size(), expected_digest);
			require(
				sealed.has_value() &&
					(*sealed)->receipt() ==
						sqlite_backend_copy_receipt{payload.size(), std::string{expected_digest}},
				"private snapshot digest binding and receipt");
			require((*sealed)->application_generated_uri().starts_with(
						"file:/cxxlens-sqlite-private-v1/") &&
						(*sealed)->application_generated_uri().ends_with(
							"?mode=ro&cache=private&immutable=1"),
					"private snapshot strict private URI");
		}
		require(registry.size() == initial_registry_size,
				"private snapshot VFS unregistered with owned lifetime");
	}

	void create_file(const std::string& path)
	{
		const auto descriptor = ::open(path.c_str(), O_CREAT | O_TRUNC | O_RDWR | O_CLOEXEC, 0600);
		require(descriptor >= 0, "create fixture");
		require(::close(descriptor) == 0, "close fixture");
	}

	std::vector<std::max_align_t> file_storage(const sqlite3_vfs& vfs)
	{
		return std::vector<std::max_align_t>(
			(static_cast<std::size_t>(vfs.os_file_bytes) + sizeof(std::max_align_t) - 1U) /
			sizeof(std::max_align_t));
	}

	template <class Function>
	[[nodiscard]] Function load_function(void* handle, const char* name)
	{
		static_assert(std::is_pointer_v<Function>);
		auto* raw = ::dlsym(handle, name);
		require(raw != nullptr, name);
		static_assert(sizeof(Function) == sizeof(raw));
		return std::bit_cast<Function>(raw);
	}

	using real_find_function = sqlite3_vfs* (*)(const char*);
	using real_open_function = int (*)(const char*, sqlite3**, int, const char*);
	using real_close_function = int (*)(sqlite3*);
	using real_exec_callback = int (*)(void*, int, char**, char**);
	using real_exec_function = int (*)(sqlite3*, const char*, real_exec_callback, void*, char**);
	using real_file_control_function = int (*)(sqlite3*, const char*, int, void*);
	using real_free_function = void (*)(void*);
	using real_libversion_number_function = int (*)();
	using real_compile_option_get_function = const char* (*)(int);
	using real_db_readonly_function = int (*)(sqlite3*, const char*);

	real_find_function real_find{};

	void exercise_real_sqlite()
	{
		auto* raw_library = ::dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
		require(raw_library != nullptr, "load real SQLite");
		auto library = std::shared_ptr<void>{raw_library,
											 [](void* value)
											 {
												 (void)::dlclose(value);
											 }};
		real_find = load_function<real_find_function>(raw_library, "sqlite3_vfs_find");
		const auto real_find_opaque =
			load_function<sqlite_source_shm_runtime_binding::vfs_find_function>(raw_library,
																				"sqlite3_vfs_find");
		const auto real_register_opaque =
			load_function<sqlite_source_shm_runtime_binding::vfs_register_function>(
				raw_library, "sqlite3_vfs_register");
		const auto real_unregister_opaque =
			load_function<sqlite_source_shm_runtime_binding::vfs_unregister_function>(
				raw_library, "sqlite3_vfs_unregister");
		const auto real_open = load_function<real_open_function>(raw_library, "sqlite3_open_v2");
		const auto real_close = load_function<real_close_function>(raw_library, "sqlite3_close");
		const auto source_open = load_function<sqlite_source_shm_runtime_binding::open_v2_function>(
			raw_library, "sqlite3_open_v2");
		const auto source_close =
			load_function<sqlite_source_shm_runtime_binding::close_v2_function>(raw_library,
																				"sqlite3_close_v2");
		const auto real_exec = load_function<real_exec_function>(raw_library, "sqlite3_exec");
		const auto real_file_control =
			load_function<real_file_control_function>(raw_library, "sqlite3_file_control");
		const auto real_free = load_function<real_free_function>(raw_library, "sqlite3_free");
		const auto real_libversion_number = load_function<real_libversion_number_function>(
			raw_library, "sqlite3_libversion_number");
		const auto real_compile_option_get = load_function<real_compile_option_get_function>(
			raw_library, "sqlite3_compileoption_get");
		const auto real_db_readonly =
			load_function<real_db_readonly_function>(raw_library, "sqlite3_db_readonly");
		const auto source_exec = load_function<sqlite_source_shm_runtime_binding::exec_function>(
			raw_library, "sqlite3_exec");
		const auto source_errmsg =
			load_function<sqlite_source_shm_runtime_binding::errmsg_function>(raw_library,
																			  "sqlite3_errmsg");
		const auto source_free = load_function<sqlite_source_shm_runtime_binding::free_function>(
			raw_library, "sqlite3_free");
		const auto source_id = load_function<sqlite_source_shm_runtime_binding::source_id_function>(
			raw_library, "sqlite3_sourceid");
		const auto uri_parameter =
			load_function<sqlite_source_shm_runtime_binding::uri_parameter_function>(
				raw_library, "sqlite3_uri_parameter");
		const auto uri_key = load_function<sqlite_source_shm_runtime_binding::uri_key_function>(
			raw_library, "sqlite3_uri_key");
		Dl_info runtime_image_information{};
		require(::dladdr(std::bit_cast<const void*>(source_open), &runtime_image_information) !=
						0 &&
					runtime_image_information.dli_fbase != nullptr,
				"resolve real SQLite image identity");
		auto real_runtime = sqlite_source_shm_runtime_binding{
			raw_library,
			runtime_image_information.dli_fbase,
			library.get(),
			library,
			source_open,
			source_close,
			source_exec,
			source_errmsg,
			source_free,
			source_id,
			uri_parameter,
			uri_key,
			real_find_opaque,
			real_register_opaque,
			real_unregister_opaque,
		};
		auto* real_default = real_find(nullptr);
		require(real_default != nullptr, "resolve real default VFS once");

		temporary_test_directory target_directory_guard{"default-forwarding-real"};
		const std::string target_directory = target_directory_guard.path().string();
		const std::string normalization_path =
			(target_directory_guard.path() / "normalization.sqlite").string();
		{
			sqlite3* fixture{};
			constexpr int fixture_flags = sqlite_open_read_write | sqlite_open_create |
				sqlite_open_full_mutex | sqlite_open_private_cache;
			require(real_open(normalization_path.c_str(), &fixture, fixture_flags, nullptr) ==
						sqlite_ok,
					"create exact-empty WAL normalization fixture");
			char* message{};
			const auto selected_wal =
				real_exec(fixture, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &message);
			if (message != nullptr)
				real_free(message);
			require(selected_wal == sqlite_ok && real_close(fixture) == sqlite_ok,
					"seal exact-empty WAL normalization fixture");
		}
		require(!std::filesystem::exists(normalization_path + "-shm") &&
					!std::filesystem::exists(normalization_path + "-journal"),
				"normalization fixture starts without SHM or rollback journal");
		{
			auto normalization_bundle = make_sqlite_default_forwarding_store_bundle(
				normalization_path,
				sqlite_private_snapshot_registry_binding{
					raw_library,
					real_default,
					real_find_opaque,
					real_register_opaque,
					real_unregister_opaque,
					library,
					real_runtime,
				});
			require(normalization_bundle.has_value(), "normalization forwarding bundle");
			auto census = normalization_bundle->observation->capture_namespace(normalization_path);
			require(census.has_value() && census->source_shm_guard,
					census ? std::string{"normalization retained-parent source census"}
						   : std::string{"normalization retained-parent source census: "} +
							census.error().field + '/' + census.error().detail);
			std::shared_ptr<const sqlite_backend_held_object> held_main;
			for (const auto& entry : census->entries)
				if (entry.role == sqlite_backend_file_role::main_database)
					held_main = entry.held_object;
			require(held_main != nullptr, "normalization held main source anchor");
			auto operation_port = std::make_shared<default_store_operation_port>();
			auto input = sqlite_exact_empty_normalization_input{
				sqlite_exact_empty_normalization_runtime{
					real_runtime,
					real_libversion_number,
					real_compile_option_get,
					std::bit_cast<sqlite_exact_empty_normalization_runtime::db_readonly_function>(
						real_db_readonly),
				},
				std::move(*census),
				normalization_path,
				operation_port,
			};
			auto wrong_anchor = seal_sqlite_logical_read_receipt(
				std::make_shared<const int>(7), true, true, 0U, true);
			require(wrong_anchor.has_value(), "normalization wrong-anchor receipt fixture");
			auto pre_negative_digest = held_main->sha256();
			auto rejected = run_sqlite_exact_empty_normalization_effect(
				std::move(*wrong_anchor), input, normalization_bundle->observation);
			auto post_negative_digest = held_main->sha256();
			require(!rejected &&
						rejected.error() ==
							error{"store.backend-unavailable",
								  "sqlite",
								  "exact-empty-normalization-entry"},
					"wrong #201 anchor rejects before normalization arm");
			require(pre_negative_digest && post_negative_digest &&
						*post_negative_digest == *pre_negative_digest &&
						!std::filesystem::exists(normalization_path + "-wal") &&
						!std::filesystem::exists(normalization_path + "-journal") &&
						!std::filesystem::exists(normalization_path + "-shm"),
					"wrong #201 anchor is zero-effect");
			auto logical_read = seal_sqlite_logical_read_receipt(
				std::static_pointer_cast<const void>(held_main), true, true, 0U, true);
			require(logical_read.has_value(), "normalization exact #201 logical-read receipt");
			auto normalized = run_sqlite_exact_empty_normalization_effect(
				std::move(*logical_read), input, normalization_bundle->observation);
			require(normalized.has_value(),
					normalized ? std::string{"real exact-empty normalization effect"}
							   : std::string{"real exact-empty normalization effect: "} +
							normalized.error().detail);
			require(
				(*normalized)->confirmed_close() && (*normalized)->post_close_exact_projection() &&
					(*normalized)->post_close_logical_empty() &&
					(*normalized)->post_close_sidecars_absent() && (*normalized)->custody_drained(),
				"real normalization completed physical and logical edge");

			auto cold_post =
				normalization_bundle->observation->capture_namespace(normalization_path);
			require(cold_post && cold_post->source_shm_guard,
					"cold post-normalization retained-parent census");
			std::shared_ptr<const sqlite_backend_held_object> cold_post_main;
			for (const auto& entry : cold_post->entries)
				if (entry.role == sqlite_backend_file_role::main_database)
					cold_post_main = entry.held_object;
			require(cold_post_main != nullptr, "cold post-normalization held main");
			auto cold_post_digest = cold_post_main->sha256();
			auto replay_receipt = seal_sqlite_logical_read_receipt(
				std::static_pointer_cast<const void>(cold_post_main), true, true, 0U, true);
			require(replay_receipt.has_value(), "cold replay receipt fixture");
			auto replay_input = sqlite_exact_empty_normalization_input{
				input.runtime,
				std::move(*cold_post),
				normalization_path,
				operation_port,
			};
			auto replay = run_sqlite_exact_empty_normalization_effect(
				std::move(*replay_receipt), replay_input, normalization_bundle->observation);
			auto replay_post_digest = cold_post_main->sha256();
			require(!replay && cold_post_digest && replay_post_digest &&
						*replay_post_digest == *cold_post_digest &&
						!std::filesystem::exists(normalization_path + "-wal") &&
						!std::filesystem::exists(normalization_path + "-journal") &&
						!std::filesystem::exists(normalization_path + "-shm"),
					"cold FO replay remains zero-effect and cannot repeat normalization");
		}
		require(::unlink(normalization_path.c_str()) == 0,
				"remove real exact-empty normalization fixture");

		const std::string close_unknown_path =
			(target_directory_guard.path() / "normalization-close-unknown.sqlite").string();
		{
			sqlite3* fixture{};
			constexpr int fixture_flags = sqlite_open_read_write | sqlite_open_create |
				sqlite_open_full_mutex | sqlite_open_private_cache;
			require(real_open(close_unknown_path.c_str(), &fixture, fixture_flags, nullptr) ==
						sqlite_ok,
					"create close-unknown normalization fixture");
			char* message{};
			const auto selected_wal =
				real_exec(fixture, "PRAGMA journal_mode=WAL;", nullptr, nullptr, &message);
			if (message != nullptr)
				real_free(message);
			require(selected_wal == sqlite_ok && real_close(fixture) == sqlite_ok,
					"seal close-unknown normalization fixture");

			auto fault_bundle = make_sqlite_default_forwarding_store_bundle(
				close_unknown_path,
				sqlite_private_snapshot_registry_binding{
					raw_library,
					real_default,
					real_find_opaque,
					real_register_opaque,
					real_unregister_opaque,
					library,
					real_runtime,
				});
			require(fault_bundle.has_value(), "close-unknown forwarding bundle");
			auto census = fault_bundle->observation->capture_namespace(close_unknown_path);
			require(census && census->source_shm_guard,
					"close-unknown retained-parent source census");
			std::shared_ptr<const sqlite_backend_held_object> held_main;
			for (const auto& entry : census->entries)
				if (entry.role == sqlite_backend_file_role::main_database)
					held_main = entry.held_object;
			require(held_main != nullptr, "close-unknown held main");
			auto logical_read = seal_sqlite_logical_read_receipt(
				std::static_pointer_cast<const void>(held_main), true, true, 0U, true);
			require(logical_read.has_value(), "close-unknown #201 receipt");
			auto fault_port = std::make_shared<sqlite_close_unknown_port>();
			auto input = sqlite_exact_empty_normalization_input{
				sqlite_exact_empty_normalization_runtime{
					real_runtime,
					real_libversion_number,
					real_compile_option_get,
					std::bit_cast<sqlite_exact_empty_normalization_runtime::db_readonly_function>(
						real_db_readonly),
				},
				std::move(*census),
				close_unknown_path,
				fault_port,
			};
			auto faulted = run_sqlite_exact_empty_normalization_effect(
				std::move(*logical_read), input, fault_bundle->observation);
			require(!faulted &&
						faulted.error() ==
							error{"store.backend-unavailable",
								  "sqlite",
								  "exact-empty-normalization-close-opaque"} &&
						fault_port->sqlite_close_calls() == 1U,
					"ambiguous SQLite close is quarantined once with no completed edge");
			require(!std::filesystem::exists(close_unknown_path + "-wal") &&
						std::filesystem::is_regular_file(close_unknown_path + "-journal") &&
						std::filesystem::file_size(close_unknown_path + "-journal") > 28U &&
						!std::filesystem::exists(close_unknown_path + "-shm"),
					"close ambiguity preserves an unresolved invalidated journal without success");
		}
		require(::unlink((close_unknown_path + "-journal").c_str()) == 0,
				"unlink quarantined disposable close-unknown journal");
		require(::unlink(close_unknown_path.c_str()) == 0,
				"unlink quarantined disposable close-unknown fixture");
		const std::string path = (target_directory_guard.path() / "main.sqlite").string();
		const auto real_shm_path = path + "-shm";
		create_file(path);
		{
			auto bundle = make_sqlite_default_forwarding_store_bundle(
				path,
				sqlite_private_snapshot_registry_binding{
					raw_library,
					real_default,
					real_find_opaque,
					real_register_opaque,
					real_unregister_opaque,
					library,
					real_runtime,
				});
			require(bundle.has_value(), "real filesystem forwarding bundle");
			const std::string alias{bundle->forwarding_vfs->registered_vfs_name()};

			auto scratch = bundle->observation->begin_ephemeral_connection_observation();
			require(scratch.has_value(), "real same-alias ephemeral scope");
			auto* scratch_gate = (*scratch)->effect_gate_port();
			require(scratch_gate != nullptr, "real scratch gate");
			auto scratch_activation = scratch_gate->activate_denied(
				bundle->observation->capability_token(), (*scratch)->token(), ":memory:");
			require(scratch_activation.has_value(), "real scratch deny receipt");
			sqlite3* scratch_database{};
			constexpr int real_open_readwrite = 0x2;
			constexpr int real_open_create = 0x4;
			constexpr int real_open_fullmutex = 0x00010000;
			constexpr int real_open_privatecache = 0x00040000;
			const auto real_flags = real_open_readwrite | real_open_create | real_open_fullmutex |
				real_open_privatecache;
			require(real_open(":memory:", &scratch_database, real_flags, alias.c_str()) ==
						sqlite_ok,
					"real same-alias memory open");
			auto scratch_snapshot = (*scratch)->snapshot();
			require(scratch_snapshot.has_value() && scratch_snapshot->complete &&
						!scratch_snapshot->main_handle_open &&
						scratch_snapshot->open_events.empty(),
					"real memory pager bypass is typed not-applicable");
			require(real_close(scratch_database) == sqlite_ok, "real memory close");

			auto scope = bundle->observation->begin_connection_observation(path);
			require(scope.has_value(), "real filesystem scope");
			require((*scope)->requires_source_shm_writer_mapping_epoch(),
					"default forwarding VFS did not require the source SHM epoch route");
			auto* gate = (*scope)->effect_gate_port();
			require(gate != nullptr, "real filesystem gate");
			auto activation = gate->activate_denied(
				bundle->observation->capability_token(), (*scope)->token(), path);
			require(activation.has_value(), "real filesystem deny receipt");
			sqlite3* database{};
			require(real_open(path.c_str(), &database, real_flags, alias.c_str()) == sqlite_ok,
					"real filesystem open through alias");
			auto observed = (*scope)->snapshot();
			require(observed.has_value() && observed->complete && observed->main_handle_open &&
						observed->open_events.size() == 1U,
					"real filesystem main xOpen proof");
			auto authority = std::make_shared<test_arm_authority>(true, false);
			auto armed = gate->arm_now(make_arm_request(*bundle->observation,
														**scope,
														path,
														sqlite_backend_effect_stage::fully_armed,
														authority,
														"real"));
			require(armed.has_value() && armed->sequence == 2U,
					"real filesystem fully armed receipt");
			char* message{};
			const auto executed = real_exec(database,
											"PRAGMA journal_mode=WAL;"
											"PRAGMA wal_autocheckpoint=0;"
											"CREATE TABLE effect_gate_probe(value INTEGER);"
											"INSERT INTO effect_gate_probe VALUES(7);",
											nullptr,
											nullptr,
											&message);
			std::string execution_message = message != nullptr ? message : "";
			if (message != nullptr)
				real_free(message);
			if (executed != sqlite_ok)
			{
				auto failed_observation = (*scope)->snapshot();
				if (failed_observation)
				{
					execution_message.append("; open-events=");
					execution_message.append(
						std::to_string(failed_observation->open_events.size()));
					for (const auto& event : failed_observation->open_events)
					{
						execution_message.push_back('/');
						execution_message.append(std::to_string(static_cast<int>(event.role)));
						execution_message.push_back(':');
						execution_message.append(std::to_string(static_cast<int>(event.outcome)));
					}
				}
			}
			require(executed == sqlite_ok,
					execution_message.empty()
						? std::string{"real WAL filesystem write after arm"}
						: std::string{"real WAL filesystem write after arm: "} + execution_message);
			int persist_wal = 1;
			require(real_file_control(
						database, nullptr, sqlite_file_control_persist_wal, &persist_wal) ==
						sqlite_ok,
					"retain real WAL fixture after writer close");
			const auto external_dms_holder = ::open(real_shm_path.c_str(), O_RDONLY | O_CLOEXEC);
			require(external_dms_holder >= 0, "open real active WAL DMS holder");
			struct flock deadman_lock{};
			deadman_lock.l_type = F_RDLCK;
			deadman_lock.l_whence = SEEK_SET;
			deadman_lock.l_start = sqlite_unix_shm_deadman_switch_offset;
			deadman_lock.l_len = 1;
			require(::fcntl(external_dms_holder, F_OFD_SETLK, &deadman_lock) == 0,
					"hold real active WAL epoch outside SQLite's in-process SHM node");
			require(real_close(database) == sqlite_ok, "real filesystem close");

			{
				auto unrelated = bundle->observation->capture_namespace(path);
				require(unrelated.has_value() && unrelated->source_shm_guard,
						"capture unrelated-event namespace guard");
				const auto sibling = target_directory + "/unrelated.tmp";
				create_file(sibling);
				require(::unlink(sibling.c_str()) == 0 && unrelated->source_shm_guard->recheck(),
						"unrelated sibling namespace events do not invalidate source guard");
			}
			{
				auto metadata = bundle->observation->capture_namespace(path);
				require(metadata.has_value() && metadata->source_shm_guard,
						"capture metadata-event namespace guard");
				require(::chmod(path.c_str(), 0640) == 0 && metadata->source_shm_guard->recheck() &&
							::chmod(path.c_str(), 0600) == 0 &&
							metadata->source_shm_guard->recheck(),
						"metadata-only source events remain outside namespace guard");
				const auto content = ::open(path.c_str(), O_RDWR | O_CLOEXEC);
				require(content >= 0, "open source for same-byte content event");
				std::byte first{};
				require(::pread(content, &first, 1, 0) == 1 &&
							::pwrite(content, &first, 1, 0) == 1 && ::fdatasync(content) == 0 &&
							::close(content) == 0 && metadata->source_shm_guard->recheck(),
						"content modify event alone remains outside namespace guard");
			}
			{
				auto leaf_create = bundle->observation->capture_namespace(path);
				require(leaf_create.has_value() && leaf_create->source_shm_guard,
						"capture leaf-create namespace guard");
				const auto transient_journal = path + "-journal";
				create_file(transient_journal);
				require(::unlink(transient_journal.c_str()) == 0 &&
							!leaf_create->source_shm_guard->recheck(),
						"source leaf create-delete invalidates continuous guard");
			}
			for (const auto& source_leaf :
				 {path, path + std::string{"-wal"}, path + std::string{"-shm"}})
			{
				auto leaf_swap = bundle->observation->capture_namespace(path);
				require(leaf_swap.has_value() && leaf_swap->source_shm_guard,
						"capture main/WAL/SHM leaf-swap namespace guard");
				const auto moved_leaf = source_leaf + ".swap";
				require(::rename(source_leaf.c_str(), moved_leaf.c_str()) == 0 &&
							::rename(moved_leaf.c_str(), source_leaf.c_str()) == 0 &&
							!leaf_swap->source_shm_guard->recheck(),
						"each main/WAL/SHM A-to-B-to-A invalidates continuous guard");
			}
			{
				auto ancestor_swap = bundle->observation->capture_namespace(path);
				require(ancestor_swap.has_value() && ancestor_swap->source_shm_guard,
						"capture ancestor-swap namespace guard");
				const auto moved_directory = target_directory + ".swap";
				require(::rename(target_directory.c_str(), moved_directory.c_str()) == 0 &&
							::rename(moved_directory.c_str(), target_directory.c_str()) == 0 &&
							!ancestor_swap->source_shm_guard->recheck(),
						"source ancestor A-to-B-to-A invalidates continuous guard");
			}
			{
				auto lifecycle = bundle->observation->capture_namespace(path);
				require(lifecycle.has_value() && lifecycle->source_shm_guard,
						"capture guard lifecycle census");
				require(!lifecycle->source_shm_guard->finish(),
						"unclaimed census guard cannot be finished");
				require(lifecycle->source_shm_guard->claim_target_epoch() &&
							lifecycle->source_shm_guard->finish() &&
							!lifecycle->source_shm_guard->recheck() &&
							!lifecycle->source_shm_guard->claim_target_epoch(),
						"guard lifecycle is sealed, one-shot claimed, finished, then unusable");
			}

			auto source_census = bundle->observation->capture_namespace(path);
			require(source_census.has_value(), "capture real active WAL source census");
			const auto observation_binding = bundle->observation->binding();
			auto qualification_request = sqlite_source_shm_qualification_request{
				real_runtime,
				path,
				*source_census,
				source_census->parent_namespace_identity,
				alias,
				observation_binding.vfs_implementation_identity,
				observation_binding.pinned_underlying_vfs_identity,
				observation_binding.pinned_underlying_vfs_app_data_identity,
				observation_binding.backend_lifetime_identity,
				bundle->observation->capability_token(),
			};
			auto* source_shm_port = bundle->observation->source_shm_readonly_port();
			require(source_shm_port != nullptr, "real source SHM preflight port");
			auto missing_guard_request = qualification_request;
			missing_guard_request.source_census.source_shm_guard.reset();
			auto missing_guard = source_shm_port->qualify(std::move(missing_guard_request));
			require(!missing_guard &&
						missing_guard.error() ==
							error{"store.backend-unavailable",
								  "sqlite",
								  "source-shm-readonly-qualification"},
					"active source without continuous guard fails with exact qualification tuple");
			auto replay_request = qualification_request;
			auto qualified_plan = source_shm_port->qualify(std::move(qualification_request));
			require(qualified_plan.has_value(),
					qualified_plan.has_value()
						? std::string{"real target-independent SHM preflight"}
						: std::string{"real target-independent SHM preflight: "} +
							qualified_plan.error().detail);
			require(!source_shm_port->qualify(std::move(replay_request)),
					"census namespace guard transfers to exactly one target epoch");

			auto source_scope = bundle->observation->begin_connection_observation(path);
			require(source_scope.has_value(), "real qualified source scope");
			auto* source_gate = (*source_scope)->effect_gate_port();
			require(source_gate != nullptr &&
						source_gate
							->activate_denied(bundle->observation->capability_token(),
											  (*source_scope)->token(),
											  path)
							.has_value(),
					"real qualified source deny receipt");
			require((*source_scope)->arm_source_shm_readonly_profile(*qualified_plan).has_value(),
					"real sealed source SHM target arm");
			sqlite3* source_database{};
			require(real_open(qualified_plan->application_generated_uri.c_str(),
							  &source_database,
							  qualified_plan->open_flags,
							  qualified_plan->registered_vfs_name.c_str()) == sqlite_ok,
					"real strict readonly-SHM URI open");
			message = nullptr;
			const auto source_read = real_exec(source_database,
											   "SELECT count(*) FROM effect_gate_probe;",
											   nullptr,
											   nullptr,
											   &message);
			if (message != nullptr)
				real_free(message);
			require(source_read == sqlite_ok, "real qualified source reads active WAL");
			auto source_observation = (*source_scope)->snapshot();
			require(source_observation.has_value() && source_observation->complete &&
						source_observation->source_shm_open_callback_receipt.has_value() &&
						!source_observation->shm_map_events.empty(),
					"real qualified xOpen and xShmMap receipts");
			require(real_close(source_database) == sqlite_ok, "real qualified source close");
			require(::close(external_dms_holder) == 0, "close real active WAL DMS holder");
		}
		require(::unlink(path.c_str()) == 0, "remove real SQLite fixture");
		const auto real_wal_path = path + "-wal";
		require((::unlink(real_wal_path.c_str()) == 0 || errno == ENOENT) &&
					(::unlink(real_shm_path.c_str()) == 0 || errno == ENOENT),
				"remove any retained real SQLite sidecars");
		require(::rmdir(target_directory.c_str()) == 0, "remove real SQLite target directory");
		real_find = nullptr;
	}
} // namespace

int main()
{
	original_vfs = sqlite3_vfs{
		3,
		static_cast<int>(sizeof(fake_file)),
		4096,
		nullptr,
		"fake-original",
		&app_data_sentinel,
		fake_open,
		fake_remove,
		fake_access,
		fake_full_path,
		fake_dl_open,
		fake_dl_error,
		fake_dl_sym,
		fake_dl_close,
		fake_randomness,
		fake_sleep,
		fake_current_time,
		fake_last_error,
		fake_current_time_int64,
		fake_set_system_call,
		fake_get_system_call,
		fake_next_system_call,
	};
	registry.emplace(original_vfs.name, &original_vfs);
	exercise_private_snapshot_digest_binding();
	{
		auto incomplete = registry_binding();
		incomplete.source_shm_runtime.source_id = nullptr;
		auto rejected = cxxlens::sdk::make_sqlite_default_forwarding_vfs(std::move(incomplete));
		require(!rejected && registry.size() == 1U,
				"partial source runtime receipt rejects before native alias registration");
	}

	const std::string main_path =
		"/tmp/cxxlens-default-forwarding-smoke-" + std::to_string(::getpid()) + ".sqlite";
	const auto shm_path = main_path + "-shm";
	const auto journal_path = main_path + "-journal";
	const auto wal_path = main_path + "-wal";
	create_file(main_path);
	create_file(shm_path);
	create_file(journal_path);
	create_file(wal_path);

	auto bundle =
		cxxlens::sdk::make_sqlite_default_forwarding_store_bundle(main_path, registry_binding());
	require(bundle.has_value(), "filesystem bundle");
	require(bundle->canonical_vfs_locator == main_path, "canonical locator exact");
	require(bundle->runtime_identity == &runtime_sentinel, "runtime identity exact");
	require(bundle->forwarding_vfs->pinned_underlying_vfs_identity() == &original_vfs,
			"underlying identity exact");
	require(bundle->forwarding_vfs->pinned_underlying_vfs_app_data_identity() == &app_data_sentinel,
			"pAppData identity exact");
	require(!bundle->forwarding_vfs->canonicalize("file:/tmp/rejected"), "URI rejected");
	require(!bundle->forwarding_vfs->canonicalize("/tmp/rejected?query"), "query rejected");
	require(!bundle->observation->begin_connection_observation(main_path + ".wrong"),
			"wrong locator rejected");
	auto wrong_token = bundle->observation->capability_token();
	wrong_token.bytes.front() ^= std::byte{1U};
	require(
		!bundle->connection_observation_port->begin_connection_observation(main_path, wrong_token),
		"wrong capability token rejected");
	{
		auto wal_recovery = bundle->observation->create_wal_recovery_workspace();
		require(wal_recovery.has_value() && *wal_recovery,
				"filesystem observation creates a capability-bound WAL recovery workspace");
	}

	auto* wrapper = const_cast<sqlite3_vfs*>(
		static_cast<const sqlite3_vfs*>(bundle->forwarding_vfs->vfs_implementation_identity()));
	require(fake_find(std::string{bundle->forwarding_vfs->registered_vfs_name()}.c_str()) ==
				wrapper,
			"alias registered");
	const std::string source_shm_alias{bundle->forwarding_vfs->registered_vfs_name()};
	const auto exact_uri_parameters = std::vector<std::pair<std::string_view, std::string_view>>{
		{"mode", "ro"}, {"cache", "private"}, {"readonly_shm", "1"}};
	const auto make_candidate_plan = [&](const std::string& locator)
	{
		return sqlite_source_shm_qualification_open_plan{
			fake_source_shm_runtime(bundle->runtime_lifetime),
			locator,
			"test-filesystem-profile-v1",
			strict_source_shm_uri(locator),
			source_shm_alias,
			wrapper,
			&original_vfs,
			&app_data_sentinel,
			bundle->forwarding_vfs->backend_lifetime_identity(),
			bundle->observation->capability_token(),
			source_shm_application_open_flags,
		};
	};

#if defined(__linux__)
	{
		require(::unlink(shm_path.c_str()) == 0, "remove initial writer stat-only SHM fixture");
		require(::unlink(journal_path.c_str()) == 0,
				"remove initial writer stat-only journal fixture");
		auto absent_census = bundle->observation->capture_namespace(main_path);
		require(absent_census.has_value() && absent_census->source_shm_guard,
				"writer stat-only absent census guard");
		auto absent_stat = absent_census->source_shm_guard->observe_writer_shm_stat(false);
		require(absent_stat.has_value() &&
					absent_stat->state == sqlite_backend_entry_state::absent &&
					!absent_stat->object_identity && !absent_stat->directory_entry_identity &&
					absent_stat->byte_count == 0U,
				"writer stat-only absent observation");
		const auto created = ::open(shm_path.c_str(), O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
		require(created >= 0 && ::ftruncate(created, 4096) == 0 && ::close(created) == 0,
				"create writer stat-only SHM fixture");
		auto post_stat = absent_census->source_shm_guard->observe_writer_shm_stat(true);
		require(post_stat.has_value() &&
					post_stat->state == sqlite_backend_entry_state::held_regular &&
					post_stat->object_identity && post_stat->directory_entry_identity &&
					post_stat->byte_count == 4096U,
				"writer stat-only exact create observation");
		auto repeated_post_stat = absent_census->source_shm_guard->observe_writer_shm_stat(true);
		require(repeated_post_stat.has_value() && *repeated_post_stat == *post_stat,
				"writer stat-only repeated post observation is stable");
		require(::unlink(shm_path.c_str()) == 0, "remove writer stat-only SHM fixture");
		create_file(shm_path);
		create_file(journal_path);
	}
#endif

	{
		const std::string scratch_path{"/proc/self/fd/701/cold/main.db"};
		auto qualified_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				scratch_path, bundle->observation->capability_token());
		require(qualified_scope.has_value(), "source SHM qualification scope");
		auto* qualified_gate = (*qualified_scope)->effect_gate_port();
		require(qualified_gate != nullptr &&
					qualified_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*qualified_scope)->token(),
										  scratch_path)
						.has_value(),
				"source SHM qualification deny receipt");
		require(
			(*qualified_scope)
				->arm_source_shm_readonly_qualification_candidate(make_candidate_plan(scratch_path))
				.has_value(),
			"source SHM qualification candidate arm");
		std::array<char, 256U> qualified_full_path{};
		require(wrapper->full_pathname(wrapper,
									   scratch_path.c_str(),
									   static_cast<int>(qualified_full_path.size()),
									   qualified_full_path.data()) == sqlite_ok &&
					std::string_view{qualified_full_path.data()} == scratch_path,
				"qualification xFull preserves the exact procfd locator");
		auto filename = sqlite_uri_filename(scratch_path, exact_uri_parameters);
		auto qualified_storage = file_storage(*wrapper);
		auto* qualified_file = reinterpret_cast<sqlite3_file*>(qualified_storage.data());
		int qualified_out{};
		const auto opens_before_qualified = open_calls;
		require(wrapper->open(wrapper,
							  filename.data(),
							  qualified_file,
							  source_shm_main_xopen_flags,
							  &qualified_out) == sqlite_ok &&
					open_calls == opens_before_qualified + 1,
				"qualified callback receipt precedes native xOpen delegation");

		const auto maps_before_qualified = shm_map_calls;
		shm_map_result = sqlite_readonly_cannot_initialize;
		return_null_shm_mapping_without_extension = true;
		volatile void* mapping = shm_page.data();
		require(
			qualified_file->methods->shm_map(qualified_file, 0, 4096, 1, &mapping) ==
					sqlite_readonly_cannot_initialize &&
				mapping == nullptr && shm_map_calls == maps_before_qualified + 1 &&
				last_shm_extend == 0,
			"qualified caller extend one delegates native extend zero and preserves CANTINIT/null");

		shm_map_result = sqlite_readonly;
		return_null_shm_mapping_without_extension = false;
		mapping = nullptr;
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 0, &mapping) ==
						sqlite_readonly &&
					mapping == shm_page.data() && shm_map_calls == maps_before_qualified + 2,
				"qualified later map delegates after CANTINIT and preserves READONLY/non-null");
		const auto unmaps_before_family_reset = shm_unmap_calls;
		require(qualified_file->methods->shm_unmap(qualified_file, 1) == sqlite_ok &&
					shm_unmap_calls == unmaps_before_family_reset + 1 && last_shm_remove == 0,
				"successful qualified unmap is non-removing and resets transient map-family state");

		return_null_shm_mapping_without_extension = true;
		mapping = shm_page.data();
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 1, &mapping) ==
						sqlite_readonly_cannot_initialize &&
					mapping == nullptr && shm_map_calls == maps_before_qualified + 3 &&
					last_shm_extend == 0,
				"qualified READONLY/null normalizes to CANTINIT without suppressing delegation");

		shm_map_result = sqlite_readonly | (1 << 8);
		mapping = shm_page.data();
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 0, &mapping) ==
						sqlite_io_error &&
					mapping == nullptr && shm_map_calls == maps_before_qualified + 4,
				"qualified unexpected READONLY-family result is a protocol violation");

		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		mapping = shm_page.data();
		const auto reads_before_terminal = read_calls;
		const auto locks_before_terminal = shm_lock_calls;
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 1, &mapping) ==
						sqlite_io_error &&
					mapping == nullptr && shm_map_calls == maps_before_qualified + 4,
				"qualified terminal latch denies later native map delegation");
		std::byte read_output{};
		require(qualified_file->methods->read(qualified_file, &read_output, 1, 0) ==
						sqlite_io_error &&
					read_calls == reads_before_terminal &&
					qualified_file->methods->shm_lock(
						qualified_file, 0, 1, sqlite_shm_lock | sqlite_shm_exclusive) ==
						sqlite_io_error &&
					shm_lock_calls == locks_before_terminal,
				"qualified terminal latch denies later native read and lock delegation");
		shm_unmap_result = sqlite_io_error;
		const auto unmaps_before_terminal_cleanup = shm_unmap_calls;
		require(qualified_file->methods->shm_unmap(qualified_file, 1) == sqlite_io_error &&
					shm_unmap_calls == unmaps_before_terminal_cleanup + 1 && last_shm_remove == 0,
				"qualified terminal cleanup forces remove zero and preserves the latch on failure");
		shm_map_result = sqlite_readonly_cannot_initialize;
		return_null_shm_mapping_without_extension = true;
		mapping = shm_page.data();
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 0, &mapping) ==
						sqlite_io_error &&
					mapping == nullptr && shm_map_calls == maps_before_qualified + 4,
				"map after failed cleanup remains terminal without native delegation");
		shm_unmap_result = sqlite_ok;
		require(qualified_file->methods->shm_unmap(qualified_file, 1) == sqlite_ok &&
					shm_unmap_calls == unmaps_before_terminal_cleanup + 2 && last_shm_remove == 0,
				"successful qualified cleanup forces remove zero");
		mapping = shm_page.data();
		require(qualified_file->methods->shm_map(qualified_file, 0, 4096, 1, &mapping) ==
						sqlite_io_error &&
					mapping == nullptr && shm_map_calls == maps_before_qualified + 4,
				"successful cleanup does not clear the qualified terminal latch");
		auto qualified_snapshot = (*qualified_scope)->snapshot();
		require(qualified_snapshot.has_value() &&
					qualified_snapshot->source_shm_open_callback_receipt.has_value() &&
					qualified_snapshot->source_shm_open_callback_receipt->profile ==
						"sqlite-source-shm-readonly-qualification-candidate-v1" &&
					qualified_snapshot->source_shm_open_callback_receipt->mode == "ro" &&
					qualified_snapshot->source_shm_open_callback_receipt->cache == "private" &&
					qualified_snapshot->source_shm_open_callback_receipt->readonly_shm == "1" &&
					qualified_snapshot->source_shm_open_callback_receipt
							->pinned_underlying_vfs_identity == &original_vfs &&
					qualified_snapshot->source_shm_open_callback_receipt
							->pinned_underlying_vfs_app_data_identity == &app_data_sentinel &&
					qualified_snapshot->shm_map_events.size() == 4U &&
					qualified_snapshot->shm_map_events[0].caller_extend == 1 &&
					qualified_snapshot->shm_map_events[0].delegated_extend == 0 &&
					!qualified_snapshot->shm_map_events[0].readonly_family_seen_before &&
					qualified_snapshot->shm_map_events[0].readonly_family_seen_after &&
					qualified_snapshot->shm_map_events[1].native_status == sqlite_readonly &&
					qualified_snapshot->shm_map_events[1].native_mapping_nonnull &&
					!qualified_snapshot->shm_map_events[2].readonly_family_seen_before &&
					qualified_snapshot->shm_map_events[2].readonly_family_seen_after &&
					qualified_snapshot->shm_map_events[3].native_status ==
						(sqlite_readonly | (1 << 8)) &&
					qualified_snapshot->shm_map_events[3].returned_status == sqlite_io_error,
				"qualified callback and map transition receipts are exact");
		require(qualified_file->methods->close(qualified_file) == sqlite_ok,
				"qualified terminal handle closes through the trusted native callback");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
	}

	{
		const std::string full_path_drift_path{"/proc/self/fd/713/active/main.db"};
		auto full_path_drift_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				full_path_drift_path, bundle->observation->capability_token());
		require(full_path_drift_scope.has_value() &&
					(*full_path_drift_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(full_path_drift_path))
						.has_value(),
				"qualified full-path callback drift scope and arm");
		std::array<char, 256U> full_path_drift_full_path{};
		require(wrapper->full_pathname(wrapper,
									   full_path_drift_path.c_str(),
									   static_cast<int>(full_path_drift_full_path.size()),
									   full_path_drift_full_path.data()) == sqlite_ok,
				"qualified full-path callback drift xFullPathname");
		auto full_path_drift_storage = file_storage(*wrapper);
		auto* full_path_drift_file =
			reinterpret_cast<sqlite3_file*>(full_path_drift_storage.data());
		int full_path_drift_out{};
		auto full_path_drift_filename =
			sqlite_uri_filename(full_path_drift_path, exact_uri_parameters);
		require(wrapper->open(wrapper,
							  full_path_drift_filename.data(),
							  full_path_drift_file,
							  source_shm_main_xopen_flags,
							  &full_path_drift_out) == sqlite_ok,
				"qualified full-path callback drift open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		mutate_full_pathname_on_next_shm_map = true;
		volatile void* full_path_drift_mapping = shm_page.data();
		const auto full_path_drift_maps_before = shm_map_calls;
		const auto full_path_drift_unmaps_before = shm_unmap_calls;
		require(full_path_drift_file->methods->shm_map(
					full_path_drift_file, 0, 4096, 1, &full_path_drift_mapping) ==
						sqlite_io_error &&
					full_path_drift_mapping == nullptr &&
					shm_map_calls == full_path_drift_maps_before + 1 &&
					shm_unmap_calls == full_path_drift_unmaps_before + 1,
				"qualified full-path callback drift is rejected after native map and unmapped");
		original_vfs.full_pathname = fake_full_path;
		auto full_path_drift_snapshot = (*full_path_drift_scope)->snapshot();
		require(full_path_drift_snapshot.has_value() && !full_path_drift_snapshot->complete,
				"qualified full-path callback drift invalidates the observation receipt");
		require(full_path_drift_file->methods->close(full_path_drift_file) == sqlite_ok,
				"qualified full-path callback drift handle closes through trusted close");
	}

	{
		const std::string registry_drift_path{"/proc/self/fd/714/active/main.db"};
		auto registry_drift_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				registry_drift_path, bundle->observation->capability_token());
		require(registry_drift_scope.has_value() &&
					(*registry_drift_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(registry_drift_path))
						.has_value(),
				"qualified registry lookup drift scope and arm");
		std::array<char, 256U> registry_drift_full_path{};
		require(wrapper->full_pathname(wrapper,
									   registry_drift_path.c_str(),
									   static_cast<int>(registry_drift_full_path.size()),
									   registry_drift_full_path.data()) == sqlite_ok,
				"qualified registry lookup drift xFullPathname");
		auto registry_drift_storage = file_storage(*wrapper);
		auto* registry_drift_file = reinterpret_cast<sqlite3_file*>(registry_drift_storage.data());
		int registry_drift_out{};
		auto registry_drift_filename =
			sqlite_uri_filename(registry_drift_path, exact_uri_parameters);
		require(wrapper->open(wrapper,
							  registry_drift_filename.data(),
							  registry_drift_file,
							  source_shm_main_xopen_flags,
							  &registry_drift_out) == sqlite_ok,
				"qualified registry lookup drift open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		mutate_registry_lookup_on_next_shm_map = true;
		volatile void* registry_drift_mapping = shm_page.data();
		const auto registry_drift_maps_before = shm_map_calls;
		const auto registry_drift_unmaps_before = shm_unmap_calls;
		require(registry_drift_file->methods->shm_map(
					registry_drift_file, 0, 4096, 1, &registry_drift_mapping) == sqlite_io_error &&
					registry_drift_mapping == nullptr &&
					shm_map_calls == registry_drift_maps_before + 1 &&
					shm_unmap_calls == registry_drift_unmaps_before + 1,
				"qualified registry lookup drift is rejected after native map and unmapped");
		auto registry_drift_snapshot = (*registry_drift_scope)->snapshot();
		require(registry_drift_snapshot.has_value() && !registry_drift_snapshot->complete,
				"qualified registry lookup drift invalidates the observation receipt");
		require(registry_drift_file->methods->close(registry_drift_file) == sqlite_ok,
				"qualified registry lookup drift handle closes through trusted close");
	}

	{
		const std::string ok_null_path{"/proc/self/fd/708/active/main.db"};
		auto ok_null_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				ok_null_path, bundle->observation->capability_token());
		require(ok_null_scope.has_value() &&
					(*ok_null_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(ok_null_path))
						.has_value(),
				"qualified native OK/null fresh scope and arm");
		std::array<char, 256U> ok_null_full_path{};
		require(wrapper->full_pathname(wrapper,
									   ok_null_path.c_str(),
									   static_cast<int>(ok_null_full_path.size()),
									   ok_null_full_path.data()) == sqlite_ok,
				"qualified native OK/null xFullPathname");
		auto ok_null_filename = sqlite_uri_filename(ok_null_path, exact_uri_parameters);
		auto ok_null_storage = file_storage(*wrapper);
		auto* ok_null_file = reinterpret_cast<sqlite3_file*>(ok_null_storage.data());
		int ok_null_out{};
		require(wrapper->open(wrapper,
							  ok_null_filename.data(),
							  ok_null_file,
							  source_shm_main_xopen_flags,
							  &ok_null_out) == sqlite_ok,
				"qualified native OK/null fresh handle open");

		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = true;
		shm_unmap_result = sqlite_ok;
		volatile void* ok_null_mapping = shm_page.data();
		const auto maps_before_ok_null = shm_map_calls;
		const auto unmaps_before_ok_null = shm_unmap_calls;
		const auto reads_before_ok_null = read_calls;
		require(ok_null_file->methods->shm_map(ok_null_file, 0, 4096, 1, &ok_null_mapping) ==
						sqlite_io_error &&
					ok_null_mapping == nullptr && shm_map_calls == maps_before_ok_null + 1 &&
					shm_unmap_calls == unmaps_before_ok_null,
				"qualified native OK/null is a directly reached protocol violation");
		ok_null_mapping = shm_page.data();
		require(ok_null_file->methods->shm_map(ok_null_file, 0, 4096, 0, &ok_null_mapping) ==
						sqlite_io_error &&
					ok_null_mapping == nullptr && shm_map_calls == maps_before_ok_null + 1 &&
					ok_null_file->methods->read(ok_null_file, nullptr, 0, 0) == sqlite_io_error &&
					read_calls == reads_before_ok_null,
				"qualified native OK/null terminal latch suppresses later native operations");
		auto ok_null_snapshot = (*ok_null_scope)->snapshot();
		require(ok_null_snapshot.has_value() && ok_null_snapshot->shm_map_events.size() == 1U &&
					ok_null_snapshot->shm_map_events[0].native_status == sqlite_ok &&
					!ok_null_snapshot->shm_map_events[0].native_mapping_nonnull &&
					ok_null_snapshot->shm_map_events[0].returned_status == sqlite_io_error,
				"qualified native OK/null receipt records the fresh protocol violation");
		require(ok_null_file->methods->close(ok_null_file) == sqlite_ok,
				"qualified native OK/null terminal handle closes");
		return_null_shm_mapping_without_extension = false;
	}

	{
		const std::string vfs_drift_path{"/proc/self/fd/711/active/main.db"};
		auto vfs_drift_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				vfs_drift_path, bundle->observation->capability_token());
		require(vfs_drift_scope.has_value() &&
					(*vfs_drift_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(vfs_drift_path))
						.has_value(),
				"qualified VFS identity drift scope and arm");
		std::array<char, 256U> vfs_drift_full_path{};
		require(wrapper->full_pathname(wrapper,
									   vfs_drift_path.c_str(),
									   static_cast<int>(vfs_drift_full_path.size()),
									   vfs_drift_full_path.data()) == sqlite_ok,
				"qualified VFS identity drift xFullPathname");
		auto vfs_drift_storage = file_storage(*wrapper);
		auto* vfs_drift_file = reinterpret_cast<sqlite3_file*>(vfs_drift_storage.data());
		int vfs_drift_out{};
		require(wrapper->open(wrapper,
							  sqlite_uri_filename(vfs_drift_path, exact_uri_parameters).data(),
							  vfs_drift_file,
							  source_shm_main_xopen_flags,
							  &vfs_drift_out) == sqlite_ok,
				"qualified VFS identity drift open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		mutate_vfs_identity_on_next_shm_map = true;
		volatile void* vfs_drift_mapping = shm_page.data();
		const auto vfs_drift_maps_before = shm_map_calls;
		const auto vfs_drift_unmaps_before = shm_unmap_calls;
		const auto vfs_drift_status =
			vfs_drift_file->methods->shm_map(vfs_drift_file, 0, 4096, 1, &vfs_drift_mapping);
		require(vfs_drift_status == sqlite_io_error && vfs_drift_mapping == nullptr &&
					shm_map_calls == vfs_drift_maps_before + 1 &&
					shm_unmap_calls == vfs_drift_unmaps_before + 1,
				"qualified VFS app-data drift is rejected after native map and unmapped");
		original_vfs.app_data = &app_data_sentinel;
		auto vfs_drift_snapshot = (*vfs_drift_scope)->snapshot();
		require(vfs_drift_snapshot.has_value() && !vfs_drift_snapshot->complete,
				"qualified VFS app-data drift invalidates the observation receipt");
		require(vfs_drift_file->methods->close(vfs_drift_file) == sqlite_ok,
				"qualified VFS app-data drift handle closes through trusted close");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
	}

	{
		const std::string methods_drift_path{"/proc/self/fd/712/active/main.db"};
		auto methods_drift_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				methods_drift_path, bundle->observation->capability_token());
		require(methods_drift_scope.has_value() &&
					(*methods_drift_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(methods_drift_path))
						.has_value(),
				"qualified io-method identity drift scope and arm");
		std::array<char, 256U> methods_drift_full_path{};
		require(wrapper->full_pathname(wrapper,
									   methods_drift_path.c_str(),
									   static_cast<int>(methods_drift_full_path.size()),
									   methods_drift_full_path.data()) == sqlite_ok,
				"qualified io-method identity drift xFullPathname");
		auto methods_drift_storage = file_storage(*wrapper);
		auto* methods_drift_file = reinterpret_cast<sqlite3_file*>(methods_drift_storage.data());
		int methods_drift_out{};
		auto methods_drift_filename = sqlite_uri_filename(methods_drift_path, exact_uri_parameters);
		require(wrapper->open(wrapper,
							  methods_drift_filename.data(),
							  methods_drift_file,
							  source_shm_main_xopen_flags,
							  &methods_drift_out) == sqlite_ok,
				"qualified io-method identity drift open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		replacement_io_methods_on_next_shm_map = &journal_methods;
		mutate_io_methods_on_next_shm_map = true;
		volatile void* methods_drift_mapping = shm_page.data();
		const auto methods_drift_maps_before = shm_map_calls;
		const auto methods_drift_unmaps_before = shm_unmap_calls;
		require(methods_drift_file->methods->shm_map(
					methods_drift_file, 0, 4096, 1, &methods_drift_mapping) == sqlite_io_error &&
					methods_drift_mapping == nullptr &&
					shm_map_calls == methods_drift_maps_before + 1 &&
					shm_unmap_calls == methods_drift_unmaps_before + 1,
				"qualified io-method identity drift is rejected after native map and unmapped");
		auto methods_drift_snapshot = (*methods_drift_scope)->snapshot();
		require(methods_drift_snapshot.has_value() && !methods_drift_snapshot->complete,
				"qualified io-method identity drift invalidates the observation receipt");
		require(methods_drift_file->methods->close(methods_drift_file) == sqlite_ok,
				"qualified io-method identity drift handle closes through trusted close");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
	}

	{
		const std::string close_drift_path{"/proc/self/fd/715/active/main.db"};
		auto close_drift_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				close_drift_path, bundle->observation->capability_token());
		require(close_drift_scope.has_value() &&
					(*close_drift_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(close_drift_path))
						.has_value(),
				"qualified close callback drift scope and arm");
		std::array<char, 256U> close_drift_full_path{};
		require(wrapper->full_pathname(wrapper,
									   close_drift_path.c_str(),
									   static_cast<int>(close_drift_full_path.size()),
									   close_drift_full_path.data()) == sqlite_ok,
				"qualified close callback drift xFullPathname");
		auto close_drift_storage = file_storage(*wrapper);
		auto* close_drift_file = reinterpret_cast<sqlite3_file*>(close_drift_storage.data());
		int close_drift_out{};
		auto close_drift_filename = sqlite_uri_filename(close_drift_path, exact_uri_parameters);
		require(wrapper->open(wrapper,
							  close_drift_filename.data(),
							  close_drift_file,
							  source_shm_main_xopen_flags,
							  &close_drift_out) == sqlite_ok,
				"qualified close callback drift open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		mutate_close_callback_on_next_shm_map = true;
		volatile void* close_drift_mapping = shm_page.data();
		const auto close_drift_maps_before = shm_map_calls;
		const auto close_drift_unmaps_before = shm_unmap_calls;
		require(close_drift_file->methods->shm_map(
					close_drift_file, 0, 4096, 1, &close_drift_mapping) == sqlite_io_error &&
					close_drift_mapping == nullptr &&
					shm_map_calls == close_drift_maps_before + 1 &&
					shm_unmap_calls == close_drift_unmaps_before + 1,
				"qualified close callback drift is rejected after native map and unmapped");
		full_methods.close = fake_close;
		auto close_drift_snapshot = (*close_drift_scope)->snapshot();
		require(close_drift_snapshot.has_value() && !close_drift_snapshot->complete,
				"qualified close callback drift invalidates the observation receipt");
		require(close_drift_file->methods->close(close_drift_file) == sqlite_ok,
				"qualified close callback drift handle closes through trusted close");
	}

	{
		const std::string invalid_extend_path{"/proc/self/fd/710/active/main.db"};
		auto invalid_extend_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				invalid_extend_path, bundle->observation->capability_token());
		require(invalid_extend_scope.has_value() &&
					(*invalid_extend_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(invalid_extend_path))
						.has_value(),
				"qualified unknown extend fresh scope and arm");
		std::array<char, 256U> invalid_extend_full_path{};
		require(wrapper->full_pathname(wrapper,
									   invalid_extend_path.c_str(),
									   static_cast<int>(invalid_extend_full_path.size()),
									   invalid_extend_full_path.data()) == sqlite_ok,
				"qualified unknown extend xFullPathname");
		auto invalid_extend_filename =
			sqlite_uri_filename(invalid_extend_path, exact_uri_parameters);
		auto invalid_extend_storage = file_storage(*wrapper);
		auto* invalid_extend_file = reinterpret_cast<sqlite3_file*>(invalid_extend_storage.data());
		int invalid_extend_out{};
		require(wrapper->open(wrapper,
							  invalid_extend_filename.data(),
							  invalid_extend_file,
							  source_shm_main_xopen_flags,
							  &invalid_extend_out) == sqlite_ok,
				"qualified unknown extend fresh handle open");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		volatile void* invalid_extend_mapping = shm_page.data();
		const auto maps_before_invalid_extend = shm_map_calls;
		require(invalid_extend_file->methods->shm_map(
					invalid_extend_file, 0, 4096, 2, &invalid_extend_mapping) == sqlite_io_error &&
					invalid_extend_mapping == nullptr &&
					shm_map_calls == maps_before_invalid_extend,
				"qualified unknown caller extend is rejected before native delegation");
		require(invalid_extend_file->methods->close(invalid_extend_file) == sqlite_ok,
				"qualified unknown extend terminal handle closes");
	}

	{
		const std::string ok_nonnull_path{"/proc/self/fd/709/active/main.db"};
		auto ok_nonnull_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				ok_nonnull_path, bundle->observation->capability_token());
		require(ok_nonnull_scope.has_value() &&
					(*ok_nonnull_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(ok_nonnull_path))
						.has_value(),
				"qualified native OK/non-null fresh scope and arm");
		std::array<char, 256U> ok_nonnull_full_path{};
		require(wrapper->full_pathname(wrapper,
									   ok_nonnull_path.c_str(),
									   static_cast<int>(ok_nonnull_full_path.size()),
									   ok_nonnull_full_path.data()) == sqlite_ok,
				"qualified native OK/non-null xFullPathname");
		auto ok_nonnull_filename = sqlite_uri_filename(ok_nonnull_path, exact_uri_parameters);
		auto ok_nonnull_storage = file_storage(*wrapper);
		auto* ok_nonnull_file = reinterpret_cast<sqlite3_file*>(ok_nonnull_storage.data());
		int ok_nonnull_out{};
		require(wrapper->open(wrapper,
							  ok_nonnull_filename.data(),
							  ok_nonnull_file,
							  source_shm_main_xopen_flags,
							  &ok_nonnull_out) == sqlite_ok,
				"qualified native OK/non-null fresh handle open");

		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		shm_unmap_result = sqlite_ok;
		volatile void* ok_nonnull_mapping{};
		const auto maps_before_ok_nonnull = shm_map_calls;
		const auto unmaps_before_ok_nonnull = shm_unmap_calls;
		const auto locks_before_ok_nonnull = shm_lock_calls;
		require(ok_nonnull_file->methods->shm_map(
					ok_nonnull_file, 0, 4096, 0, &ok_nonnull_mapping) == sqlite_io_error &&
					ok_nonnull_mapping == nullptr && shm_map_calls == maps_before_ok_nonnull + 1 &&
					shm_unmap_calls == unmaps_before_ok_nonnull + 1 && last_shm_remove == 0,
				"qualified native OK/non-null releases mapping once and fails terminally");
		ok_nonnull_mapping = shm_page.data();
		require(ok_nonnull_file->methods->shm_map(
					ok_nonnull_file, 0, 4096, 0, &ok_nonnull_mapping) == sqlite_io_error &&
					ok_nonnull_mapping == nullptr && shm_map_calls == maps_before_ok_nonnull + 1 &&
					shm_unmap_calls == unmaps_before_ok_nonnull + 1 &&
					ok_nonnull_file->methods->shm_lock(
						ok_nonnull_file, 0, 1, sqlite_shm_lock | sqlite_shm_exclusive) ==
						sqlite_io_error &&
					shm_lock_calls == locks_before_ok_nonnull,
				"qualified native OK/non-null terminal latch suppresses later delegation");
		auto ok_nonnull_snapshot = (*ok_nonnull_scope)->snapshot();
		require(ok_nonnull_snapshot.has_value() &&
					ok_nonnull_snapshot->shm_map_events.size() == 1U &&
					ok_nonnull_snapshot->shm_map_events[0].native_status == sqlite_ok &&
					ok_nonnull_snapshot->shm_map_events[0].native_mapping_nonnull &&
					ok_nonnull_snapshot->shm_map_events[0].returned_status == sqlite_io_error,
				"qualified native OK/non-null receipt records the fresh protocol violation");
		require(ok_nonnull_file->methods->close(ok_nonnull_file) == sqlite_ok,
				"qualified native OK/non-null terminal handle closes");
	}

	{
		const std::string capacity_path{"/proc/self/fd/707/active/main.db"};
		auto capacity_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				capacity_path, bundle->observation->capability_token());
		require(capacity_scope.has_value() &&
					(*capacity_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(capacity_path))
						.has_value(),
				"qualified map receipt capacity scope and arm");
		std::array<char, 256U> capacity_full_path{};
		require(wrapper->full_pathname(wrapper,
									   capacity_path.c_str(),
									   static_cast<int>(capacity_full_path.size()),
									   capacity_full_path.data()) == sqlite_ok,
				"qualified map receipt capacity xFullPathname");
		auto capacity_filename = sqlite_uri_filename(capacity_path, exact_uri_parameters);
		auto capacity_storage = file_storage(*wrapper);
		auto* capacity_file = reinterpret_cast<sqlite3_file*>(capacity_storage.data());
		int capacity_out{};
		require(wrapper->open(wrapper,
							  capacity_filename.data(),
							  capacity_file,
							  source_shm_main_xopen_flags,
							  &capacity_out) == sqlite_ok,
				"qualified map receipt capacity open");
		shm_map_result = sqlite_readonly_cannot_initialize;
		return_null_shm_mapping_without_extension = true;
		for (int index{}; index < 64; ++index)
		{
			volatile void* capacity_mapping = shm_page.data();
			require(
				capacity_file->methods->shm_map(capacity_file, index, 4096, 0, &capacity_mapping) ==
						sqlite_readonly_cannot_initialize &&
					capacity_mapping == nullptr,
				"qualified map receipt fills each bounded slot");
		}
		shm_map_result = sqlite_readonly;
		return_null_shm_mapping_without_extension = false;
		shm_unmap_result = sqlite_io_error;
		volatile void* saturated_mapping{};
		const auto maps_before_saturation = shm_map_calls;
		const auto unmaps_before_saturation = shm_unmap_calls;
		require(capacity_file->methods->shm_map(capacity_file, 64, 4096, 0, &saturated_mapping) ==
						sqlite_io_error &&
					saturated_mapping == nullptr && shm_map_calls == maps_before_saturation + 1 &&
					shm_unmap_calls == unmaps_before_saturation + 1 && last_shm_remove == 0,
				"receipt saturation releases native mapping once and latches cleanup uncertainty");
		const auto maps_after_saturation = shm_map_calls;
		saturated_mapping = shm_page.data();
		require(capacity_file->methods->shm_map(capacity_file, 0, 4096, 0, &saturated_mapping) ==
						sqlite_io_error &&
					saturated_mapping == nullptr && shm_map_calls == maps_after_saturation,
				"receipt saturation terminal latch denies later map delegation");
		shm_unmap_result = sqlite_ok;
		require(capacity_file->methods->shm_unmap(capacity_file, 1) == sqlite_ok &&
					last_shm_remove == 0 &&
					capacity_file->methods->close(capacity_file) == sqlite_ok,
				"receipt saturation cleanup remains non-removing and closeable");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
	}

	{
		const std::string injection_path{"/proc/self/fd/702/active/main.db"};
		auto injection_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				injection_path, bundle->observation->capability_token());
		require(injection_scope.has_value() &&
					(*injection_scope)
						->arm_source_shm_readonly_qualification_candidate(
							make_candidate_plan(injection_path))
						.has_value(),
				"URI injection scope and candidate arm");
		std::array<char, 256U> injection_full_path{};
		require(wrapper->full_pathname(wrapper,
									   injection_path.c_str(),
									   static_cast<int>(injection_full_path.size()),
									   injection_full_path.data()) == sqlite_ok,
				"URI injection arm consumes exact procfd xFull");
		auto injected = sqlite_uri_filename(
			injection_path,
			{{"mode", "ro"}, {"cache", "private"}, {"readonly_shm", "1"}, {"immutable", "1"}});
		auto injection_storage = file_storage(*wrapper);
		auto* injection_file = reinterpret_cast<sqlite3_file*>(injection_storage.data());
		int injection_out{0x7f7f};
		const auto opens_before_injection = open_calls;
		require(wrapper->open(wrapper,
							  injected.data(),
							  injection_file,
							  source_shm_main_xopen_flags,
							  &injection_out) == sqlite_cannot_open &&
					injection_out == 0 && injection_file->methods == nullptr &&
					open_calls == opens_before_injection,
				"unknown or prohibited URI parameter fails before native xOpen");
		auto exact_after_rejection = sqlite_uri_filename(injection_path, exact_uri_parameters);
		require(wrapper->open(wrapper,
							  exact_after_rejection.data(),
							  injection_file,
							  source_shm_main_xopen_flags,
							  &injection_out) == sqlite_cannot_open &&
					open_calls == opens_before_injection,
				"rejected callback scope cannot retry with a corrected URI");
	}

	{
		const std::string encoding_path{"/proc/self/fd/703/cold/main.db"};
		auto encoding_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				encoding_path, bundle->observation->capability_token());
		require(encoding_scope.has_value(), "strict URI encoding scope");
		auto invalid_plan = make_candidate_plan(encoding_path);
		const auto uppercase_escape = invalid_plan.application_generated_uri.find("%2F");
		require(uppercase_escape != std::string::npos, "strict URI has encoded slash");
		invalid_plan.application_generated_uri[uppercase_escape + 2U] = 'f';
		require(!(*encoding_scope)
					 ->arm_source_shm_readonly_qualification_candidate(std::move(invalid_plan)),
				"noncanonical lowercase URI escape fails at arm time");
	}

	{
		const std::string unarmed_path{"/proc/self/fd/704/active/main.db"};
		auto unarmed_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				unarmed_path, bundle->observation->capability_token());
		require(unarmed_scope.has_value(), "unarmed source SHM scope");
		auto filename = sqlite_uri_filename(unarmed_path, exact_uri_parameters);
		auto unarmed_storage = file_storage(*wrapper);
		auto* unarmed_file = reinterpret_cast<sqlite3_file*>(unarmed_storage.data());
		int unarmed_out{0x7f7f};
		const auto opens_before_unarmed = open_calls;
		require(wrapper->open(wrapper,
							  filename.data(),
							  unarmed_file,
							  source_shm_main_xopen_flags,
							  &unarmed_out) == sqlite_cannot_open &&
					unarmed_out == 0 && unarmed_file->methods == nullptr &&
					open_calls == opens_before_unarmed,
				"source SHM URI is deny-by-default without an arm");
		require(wrapper->open(wrapper,
							  filename.data(),
							  unarmed_file,
							  source_shm_main_xopen_flags,
							  &unarmed_out) == sqlite_cannot_open &&
					open_calls == opens_before_unarmed,
				"unarmed source SHM URI retry remains fail closed");
	}

	{
		const std::string fixture_path{"/proc/self/fd/706/producer/main.db"};
		auto fixture_scope =
			bundle->connection_observation_port->begin_source_shm_qualification_observation(
				fixture_path, bundle->observation->capability_token());
		require(fixture_scope.has_value(), "qualification fixture retry scope");
		auto* fixture_gate = (*fixture_scope)->effect_gate_port();
		require(fixture_gate != nullptr &&
					fixture_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*fixture_scope)->token(),
										  fixture_path)
						.has_value(),
				"qualification fixture retry deny receipt");
		require((*fixture_scope)
					->arm_source_shm_qualification_fixture_fullpath(
						{fixture_path,
						 source_shm_alias,
						 wrapper,
						 &original_vfs,
						 &app_data_sentinel,
						 bundle->forwarding_vfs->backend_lifetime_identity(),
						 bundle->observation->capability_token()})
					.has_value(),
				"qualification fixture retry fullpath arm");
		std::array<char, 256U> fixture_full_path{};
		require(wrapper->full_pathname(wrapper,
									   fixture_path.c_str(),
									   static_cast<int>(fixture_full_path.size()),
									   fixture_full_path.data()) == sqlite_ok &&
					std::string_view{fixture_full_path.data()} == fixture_path,
				"qualification fixture exact xFullPathname claim");
		auto fixture_storage = file_storage(*wrapper);
		auto* fixture_file = reinterpret_cast<sqlite3_file*>(fixture_storage.data());
		int fixture_out{0x7f7f};
		const auto opens_before_fixture = open_calls;
		fail_next_open = true;
		constexpr auto fixture_open_flags =
			sqlite_open_read_write | sqlite_open_create | sqlite_open_main_database;
		require(
			wrapper->open(
				wrapper, fixture_path.c_str(), fixture_file, fixture_open_flags, &fixture_out) ==
					sqlite_cannot_open &&
				open_calls == opens_before_fixture + 1 && fixture_file->methods == nullptr,
			"qualification fixture native xOpen failure consumes its one-shot claim");
		fixture_out = 0x7f7f;
		require(
			wrapper->open(
				wrapper, fixture_path.c_str(), fixture_file, fixture_open_flags, &fixture_out) ==
					sqlite_cannot_open &&
				open_calls == opens_before_fixture + 1 && fixture_out == 0 &&
				fixture_file->methods == nullptr,
			"qualification fixture retry cannot escape into generic native xOpen");
	}

	{
		require(::unlink(journal_path.c_str()) == 0,
				"remove rollback journal before exact WAL family qualification");
		auto target_census = bundle->observation->capture_namespace(main_path);
		require(target_census.has_value(), "qualified target source census");
		const sqlite_backend_entry_observation* target_shm{};
		for (const auto& entry : target_census->entries)
			if (entry.role == sqlite_backend_file_role::shared_memory)
				target_shm = &entry;
		require(target_shm != nullptr && target_shm->object_identity &&
					target_shm->directory_entry_identity,
				"qualified target SHM identities");
		const std::string anchored_target{"/proc/self/fd/705/main.db"};
		auto target_epoch = std::make_shared<test_target_namespace_epoch>(
			main_path, anchored_target, *target_census);
		auto target_scope = bundle->observation->begin_connection_observation(main_path);
		require(target_scope.has_value(), "qualified target scope");
		auto* target_gate = (*target_scope)->effect_gate_port();
		require(target_gate != nullptr &&
					target_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*target_scope)->token(),
										  main_path)
						.has_value(),
				"qualified target deny receipt");
		sqlite_source_shm_qualification_receipt qualification;
		qualification.profile = "sqlite-source-shm-readonly-unix-uri-v1";
		qualification.sqlite_source_id = fake_runtime_source_id();
		qualification.filesystem_profile = "test-filesystem-profile-v1";
		qualification.runtime_identity = &runtime_sentinel;
		qualification.runtime_image_identity = &runtime_sentinel;
		qualification.runtime_lifetime_identity = bundle->runtime_lifetime.get();
		qualification.forwarding_vfs_identity = wrapper;
		qualification.pinned_underlying_vfs_identity = &original_vfs;
		qualification.pinned_underlying_vfs_app_data_identity = &app_data_sentinel;
		qualification.backend_lifetime_identity =
			bundle->forwarding_vfs->backend_lifetime_identity();
		qualification.observation_capability_token = bundle->observation->capability_token();
		qualification.parent_namespace_identity = test_receipt("qualified-parent");
		auto exact_file_family =
			seal_sqlite_source_shm_exact_file_family(main_path,
													 qualification.parent_namespace_identity,
													 fake_runtime_source_id(),
													 target_census->entries);
		require(exact_file_family.has_value(), "qualified exact file-family identity");
		qualification.exact_file_family = std::move(*exact_file_family);
		qualification.expected_shared_memory_object_identity = *target_shm->object_identity;
		qualification.expected_shared_memory_entry_identity = *target_shm->directory_entry_identity;
		qualification.target_namespace_epoch_identity = target_epoch->identity();
		qualification.target_namespace_epoch = target_epoch;
		qualification.sealed_qualification_token = test_receipt("qualified-target");
		qualification.first_map_nonmutating = true;
		qualification.later_map_nonmutating = true;
		qualification.cantinit_heap_wal_index_route_proven = true;
		qualification.readonly_mapped_wal_index_retry_route_proven = true;
		sqlite_source_shm_qualified_open_plan plan{
			fake_source_shm_runtime(bundle->runtime_lifetime),
			std::move(qualification),
			main_path,
			anchored_target,
			strict_source_shm_uri(main_path),
			source_shm_alias,
			source_shm_application_open_flags,
		};
		require((*target_scope)->arm_source_shm_readonly_profile(std::move(plan)).has_value(),
				"sealed four-proof target arm");
		std::array<char, 256U> target_full_path{};
		require(wrapper->full_pathname(wrapper,
									   main_path.c_str(),
									   static_cast<int>(target_full_path.size()),
									   target_full_path.data()) == sqlite_ok &&
					std::string_view{target_full_path.data()} == anchored_target,
				"qualified target xFull projects to retained-parent anchor");
		auto filename = sqlite_uri_filename(anchored_target, exact_uri_parameters);
		auto target_storage = file_storage(*wrapper);
		auto* target_file = reinterpret_cast<sqlite3_file*>(target_storage.data());
		int target_out{};
		require(
			wrapper->open(
				wrapper, filename.data(), target_file, source_shm_main_xopen_flags, &target_out) ==
				sqlite_ok,
			"sealed target strict URI callback open");
		const auto target_shm_descriptor = ::open(shm_path.c_str(), O_RDWR | O_CLOEXEC);
		require(target_shm_descriptor >= 0 && ::ftruncate(target_shm_descriptor, 8192) == 0,
				"sealed target SHM size drift fixture");
		shm_map_result = sqlite_readonly_cannot_initialize;
		return_null_shm_mapping_without_extension = true;
		volatile void* target_mapping = shm_page.data();
		const auto target_maps_before = shm_map_calls;
		require(target_file->methods->shm_map(target_file, 0, 4096, 1, &target_mapping) ==
						sqlite_io_error &&
					target_mapping == nullptr && shm_map_calls == target_maps_before,
				"sealed target SHM size drift is rejected before native map");
		require(::ftruncate(target_shm_descriptor, 4096) == 0 &&
					::close(target_shm_descriptor) == 0,
				"sealed target SHM size drift fixture restore");
		auto target_snapshot = (*target_scope)->snapshot();
		require(target_snapshot.has_value() && !target_snapshot->complete,
				"sealed target size-drift invalidates the observation receipt");
		require(target_file->methods->close(target_file) == sqlite_ok,
				"sealed target size-drift handle closes");
		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
	}
	create_file(journal_path);
	int access_result{};
	std::array<char, 4097U> path_buffer{};
	std::array<char, 8U> error_buffer{};
	double current_time{};
	long long current_time_int64{};
	require(wrapper->access(wrapper, main_path.c_str(), 0, &access_result) == sqlite_ok,
			"access delegated");
	require(wrapper->full_pathname(wrapper,
								   main_path.c_str(),
								   static_cast<int>(path_buffer.size()),
								   path_buffer.data()) == sqlite_ok,
			"full pathname delegated");
	require(wrapper->remove(wrapper, "/tmp/nonexistent-fake", 0) == sqlite_ok, "remove delegated");
	auto* dl_handle = wrapper->dl_open(wrapper, "fake");
	wrapper->dl_error(wrapper, static_cast<int>(error_buffer.size()), error_buffer.data());
	require(wrapper->dl_sym(wrapper, dl_handle, "fake") == fake_symbol, "dl sym delegated");
	wrapper->dl_close(wrapper, dl_handle);
	require(wrapper->randomness(wrapper, 0, nullptr) == 0, "randomness delegated");
	require(wrapper->sleep(wrapper, 7) == 7, "sleep delegated");
	require(wrapper->current_time(wrapper, &current_time) == sqlite_ok, "time delegated");
	require(wrapper->get_last_error(wrapper, 0, nullptr) == 0, "last error delegated");
	require(wrapper->current_time_int64(wrapper, &current_time_int64) == sqlite_ok,
			"time64 delegated");
	require(wrapper->set_system_call(wrapper, "fake", fake_symbol) == sqlite_ok,
			"set syscall delegated");
	require(wrapper->get_system_call(wrapper, "fake") == fake_symbol, "get syscall delegated");
	require(std::string_view{wrapper->next_system_call(wrapper, nullptr)} == "fake",
			"next syscall delegated");

	{
		auto quarantine_bundle =
			cxxlens::sdk::make_sqlite_default_ephemeral_store_bundle(registry_binding());
		require(quarantine_bundle.has_value(), "quarantine test forwarding bundle");
		auto* quarantine_wrapper = const_cast<sqlite3_vfs*>(static_cast<const sqlite3_vfs*>(
			quarantine_bundle->forwarding_vfs->vfs_implementation_identity()));
		const auto memory_flags = sqlite_open_read_only | sqlite_open_main_database;
		const auto overridden_open = [&](const int status,
										 const sqlite3_io_methods* methods,
										 sqlite3_file& output,
										 int& returned_flags)
		{
			override_next_open = true;
			next_open_status = status;
			next_open_methods = methods;
			return quarantine_wrapper->open(quarantine_wrapper,
											"/tmp/cxxlens-vfs-quarantine-probe.db",
											&output,
											memory_flags,
											&returned_flags);
		};

		auto throwing_open_storage = file_storage(*quarantine_wrapper);
		auto* throwing_open_file = reinterpret_cast<sqlite3_file*>(throwing_open_storage.data());
		int throwing_open_out{0x7f7f};
		const auto opens_before_throw = open_calls;
		auto closes_before_quarantine = close_calls;
		throw_on_fake_open = true;
		require(quarantine_wrapper->open(quarantine_wrapper,
										 "/tmp/cxxlens-vfs-quarantine-probe.db",
										 throwing_open_file,
										 memory_flags,
										 &throwing_open_out) == sqlite_no_memory &&
					throwing_open_file->methods == nullptr && throwing_open_out == 0 &&
					open_calls == opens_before_throw + 1 && close_calls == closes_before_quarantine,
				"throwing native xOpen quarantines without an unproven close attempt");

		auto failed_with_methods_storage = file_storage(*quarantine_wrapper);
		auto* failed_with_methods =
			reinterpret_cast<sqlite3_file*>(failed_with_methods_storage.data());
		int failed_with_methods_out{0x7f7f};
		closes_before_quarantine = close_calls;
		require(overridden_open(sqlite_cannot_open,
								&full_methods,
								*failed_with_methods,
								failed_with_methods_out) == sqlite_cannot_open &&
					failed_with_methods->methods == nullptr && failed_with_methods_out == 0 &&
					close_calls == closes_before_quarantine + 1,
				"failed native xOpen with trusted pMethods closes exactly once");

		fake_close_result = sqlite_io_error;
		closes_before_quarantine = close_calls;
		failed_with_methods_out = 0x7f7f;
		require(overridden_open(sqlite_cannot_open,
								&full_methods,
								*failed_with_methods,
								failed_with_methods_out) == sqlite_cannot_open &&
					failed_with_methods->methods == nullptr && failed_with_methods_out == 0 &&
					close_calls == closes_before_quarantine + 1,
				"failed native xOpen preserves status and quarantines a failed close once");
		fake_close_result = sqlite_ok;
		auto retry_storage = file_storage(*quarantine_wrapper);
		auto* retry_file = reinterpret_cast<sqlite3_file*>(retry_storage.data());
		int retry_out{};
		require(quarantine_wrapper->open(quarantine_wrapper,
										 "/tmp/cxxlens-vfs-quarantine-probe.db",
										 retry_file,
										 memory_flags,
										 &retry_out) == sqlite_ok &&
					retry_file->methods->close(retry_file) == sqlite_ok &&
					close_calls == closes_before_quarantine + 2,
				"later open never retries the quarantined failed-close node");

		auto malformed_storage = file_storage(*quarantine_wrapper);
		auto* malformed_file = reinterpret_cast<sqlite3_file*>(malformed_storage.data());
		int malformed_out{0x7f7f};
		closes_before_quarantine = close_calls;
		require(overridden_open(sqlite_ok,
								reinterpret_cast<const sqlite3_io_methods*>(std::uintptr_t{1U}),
								*malformed_file,
								malformed_out) == sqlite_io_error &&
					malformed_file->methods == nullptr && malformed_out == 0 &&
					close_calls == closes_before_quarantine,
				"unreadable pMethods is never dereferenced or closed");

		malformed_out = 0x7f7f;
		require(overridden_open(sqlite_ok, nullptr, *malformed_file, malformed_out) ==
						sqlite_io_error &&
					malformed_file->methods == nullptr && malformed_out == 0 &&
					close_calls == closes_before_quarantine,
				"successful native xOpen without pMethods is quarantined");

		auto libc = std::shared_ptr<void>{::dlopen("libc.so.6", RTLD_NOW | RTLD_LOCAL),
										  [](void* value)
										  {
											  if (value != nullptr)
												  (void)::dlclose(value);
										  }};
		require(libc != nullptr, "load libc for foreign callback fixture");
		using foreign_shm_lock_function = int (*)(sqlite3_file*, int, int, int);
		void* const foreign_shm_lock_raw = ::dlsym(libc.get(), "close");
		require(foreign_shm_lock_raw != nullptr, "resolve libc close for foreign callback fixture");
		static_assert(sizeof(foreign_shm_lock_function) == sizeof(foreign_shm_lock_raw));
		const auto foreign_shm_lock =
			std::bit_cast<foreign_shm_lock_function>(foreign_shm_lock_raw);
		Dl_info foreign_image{};
		Dl_info test_image{};
		require(::dladdr(foreign_shm_lock_raw, &foreign_image) != 0 &&
					foreign_image.dli_fbase != nullptr &&
					::dladdr(std::bit_cast<const void*>(&fake_open), &test_image) != 0 &&
					test_image.dli_fbase != nullptr &&
					foreign_image.dli_fbase != test_image.dli_fbase,
				"foreign callback fixture resolves outside the test image");
		foreign_trailing_methods.shm_lock = foreign_shm_lock;
		for (const auto* malformed_methods :
			 {&missing_shm_trailing_methods,
			  &missing_fetch_trailing_methods,
			  static_cast<const sqlite3_io_methods*>(&foreign_trailing_methods)})
		{
			malformed_out = 0x7f7f;
			const auto closes_before_malformed = close_calls;
			require(overridden_open(sqlite_ok, malformed_methods, *malformed_file, malformed_out) ==
							sqlite_io_error &&
						malformed_file->methods == nullptr && malformed_out == 0 &&
						close_calls == closes_before_malformed + 1,
					"incomplete or foreign extension callback table closes and fails");
		}

		malformed_out = 0;
		require(overridden_open(sqlite_ok, &journal_methods, *malformed_file, malformed_out) ==
						sqlite_ok &&
					malformed_file->methods != nullptr && malformed_file->methods->version == 1 &&
					malformed_file->methods->shm_map == nullptr &&
					malformed_file->methods->fetch == nullptr &&
					malformed_file->methods->close(malformed_file) == sqlite_ok,
				"journal table without extensions downgrades to v1");

		malformed_out = 0;
		require(overridden_open(sqlite_ok,
								&absent_shm_lead_trusted_trailing_methods,
								*malformed_file,
								malformed_out) == sqlite_ok &&
					malformed_file->methods != nullptr && malformed_file->methods->version == 1 &&
					malformed_file->methods->shm_map == nullptr &&
					malformed_file->methods->shm_lock == nullptr &&
					malformed_file->methods->shm_barrier == nullptr &&
					malformed_file->methods->shm_unmap == nullptr &&
					malformed_file->methods->fetch == nullptr &&
					malformed_file->methods->unfetch == nullptr &&
					malformed_file->methods->close(malformed_file) == sqlite_ok &&
					absent_map_trailing_lock_calls == 0 && absent_map_trailing_barrier_calls == 0 &&
					absent_map_trailing_unmap_calls == 0,
				"absent xShmMap lead hides same-image trusted trailing callbacks at v1");

		malformed_out = 0;
		require(overridden_open(sqlite_ok,
								&absent_fetch_lead_trusted_trailing_methods,
								*malformed_file,
								malformed_out) == sqlite_ok &&
					malformed_file->methods != nullptr && malformed_file->methods->version == 2 &&
					malformed_file->methods->shm_map != nullptr &&
					malformed_file->methods->fetch == nullptr &&
					malformed_file->methods->unfetch == nullptr &&
					malformed_file->methods->close(malformed_file) == sqlite_ok &&
					absent_fetch_trailing_unfetch_calls == 0,
				"absent xFetch lead hides same-image trusted xUnfetch at v2");

		auto close_failure_storage = file_storage(*quarantine_wrapper);
		auto* close_failure_file = reinterpret_cast<sqlite3_file*>(close_failure_storage.data());
		int close_failure_out{};
		require(overridden_open(sqlite_ok, &full_methods, *close_failure_file, close_failure_out) ==
					sqlite_ok,
				"normal close-failure fixture open");
		fake_close_result = sqlite_io_error;
		closes_before_quarantine = close_calls;
		require(close_failure_file->methods->close(close_failure_file) == sqlite_io_error &&
					close_calls == closes_before_quarantine + 1,
				"normal xClose failure quarantines without retry");
		fake_close_result = sqlite_ok;

		auto close_throw_storage = file_storage(*quarantine_wrapper);
		auto* close_throw_file = reinterpret_cast<sqlite3_file*>(close_throw_storage.data());
		int close_throw_out{};
		require(overridden_open(sqlite_ok, &full_methods, *close_throw_file, close_throw_out) ==
					sqlite_ok,
				"throwing close fixture open");
		throw_on_fake_close = true;
		closes_before_quarantine = close_calls;
		require(close_throw_file->methods->close(close_throw_file) == sqlite_io_error &&
					close_calls == closes_before_quarantine + 1,
				"throwing xClose quarantines without unwinding or retry");

		Dl_info fake_image_information{};
		require(::dladdr(std::bit_cast<const void*>(&fake_open), &fake_image_information) != 0 &&
					fake_image_information.dli_fbase != nullptr,
				"origin-probe fake runtime image");
		auto origin_runtime = fake_source_shm_runtime(quarantine_bundle->runtime_lifetime);
		origin_runtime.runtime_image_identity = fake_image_information.dli_fbase;
		closes_before_quarantine = close_calls;
		override_next_open = true;
		next_open_status = sqlite_ok;
		next_open_methods = &full_methods;
		auto origin_backend_pin = std::static_pointer_cast<void>(std::make_shared<int>(17));
		require(validate_sqlite_source_shm_readonly_origin_probe(
					&original_vfs, origin_runtime, origin_backend_pin, "/tmp/origin-probe.db")
						.has_value() &&
					close_calls == closes_before_quarantine + 1,
				"origin probe closes one fully trusted native file");

		auto malformed_origin_backend = std::static_pointer_cast<void>(std::make_shared<int>(18));
		std::weak_ptr<void> malformed_origin_backend_weak = malformed_origin_backend;
		override_next_open = true;
		next_open_status = sqlite_ok;
		next_open_methods = reinterpret_cast<const sqlite3_io_methods*>(std::uintptr_t{1U});
		closes_before_quarantine = close_calls;
		require(
			!validate_sqlite_source_shm_readonly_origin_probe(
				&original_vfs, origin_runtime, malformed_origin_backend, "/tmp/origin-probe.db") &&
				close_calls == closes_before_quarantine,
			"origin probe never dereferences or closes pMethods 0x1");
		malformed_origin_backend.reset();
		require(!malformed_origin_backend_weak.expired(),
				"origin probe quarantine retains backend and runtime pins");

		auto throwing_origin_backend = std::static_pointer_cast<void>(std::make_shared<int>(20));
		std::weak_ptr<void> throwing_origin_backend_weak = throwing_origin_backend;
		throw_on_fake_open = true;
		closes_before_quarantine = close_calls;
		require(
			!validate_sqlite_source_shm_readonly_origin_probe(
				&original_vfs, origin_runtime, throwing_origin_backend, "/tmp/origin-probe.db") &&
				close_calls == closes_before_quarantine,
			"throwing origin xOpen quarantines without an unproven close attempt");
		throwing_origin_backend.reset();
		require(!throwing_origin_backend_weak.expired(),
				"throwing origin xOpen retains all preallocated pins");

		auto failed_origin_backend = std::static_pointer_cast<void>(std::make_shared<int>(19));
		std::weak_ptr<void> failed_origin_backend_weak = failed_origin_backend;
		override_next_open = true;
		next_open_status = sqlite_cannot_open;
		next_open_methods = &full_methods;
		fake_close_result = sqlite_io_error;
		closes_before_quarantine = close_calls;
		require(!validate_sqlite_source_shm_readonly_origin_probe(
					&original_vfs, origin_runtime, failed_origin_backend, "/tmp/origin-probe.db") &&
					close_calls == closes_before_quarantine + 1,
				"failed origin xOpen preserves exactly one failed close attempt");
		fake_close_result = sqlite_ok;
		failed_origin_backend.reset();
		require(!failed_origin_backend_weak.expired(),
				"failed origin close quarantines all preallocated pins without retry");

		std::weak_ptr<sqlite_default_forwarding_vfs> quarantined_owner =
			quarantine_bundle->forwarding_vfs;
		quarantine_bundle->observation.reset();
		quarantine_bundle->connection_observation_port.reset();
		quarantine_bundle->forwarding_vfs.reset();
		require(!quarantined_owner.expired(),
				"process quarantine retains the forwarding backend and runtime pins");
	}

	auto failed_scope = bundle->observation->begin_connection_observation(main_path);
	require(failed_scope.has_value(), "failed scope begin");
	auto failed_storage = file_storage(*wrapper);
	auto* failed_file = reinterpret_cast<sqlite3_file*>(failed_storage.data());
	int failed_out_flags = 0x7f7f;
	fail_next_open = true;
	require(wrapper->open(wrapper,
						  main_path.c_str(),
						  failed_file,
						  sqlite_open_main_database | 0x2,
						  &failed_out_flags) == sqlite_cannot_open,
			"failed open preserved");
	require(failed_out_flags == 0, "failed pOutFlags not adopted");
	require(failed_file->methods == nullptr, "failed wrapper methods null");
	auto failed_snapshot = (*failed_scope)->snapshot();
	require(failed_snapshot.has_value() && !failed_snapshot->complete &&
				failed_snapshot->open_events.size() == 1U &&
				failed_snapshot->open_events.front().outcome ==
					cxxlens::sdk::sqlite_backend_open_outcome::failed,
			"failed observation exact");

	const auto read_only_main_flags = sqlite_open_read_only | sqlite_open_main_database;
	const auto requested_existing_wal_flags =
		sqlite_open_read_write | sqlite_open_create | sqlite_open_write_ahead_log;
	const auto delegated_existing_wal_flags = sqlite_open_read_only | sqlite_open_write_ahead_log;
	{
		auto read_only_scope = bundle->observation->begin_connection_observation(main_path);
		require(read_only_scope.has_value(), "read-only recovery scope begin");
		auto* read_only_gate = (*read_only_scope)->effect_gate_port();
		require(read_only_gate != nullptr &&
					read_only_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*read_only_scope)->token(),
										  main_path)
						.has_value(),
				"read-only recovery deny receipt");
		auto read_only_storage = file_storage(*wrapper);
		auto* read_only_file = reinterpret_cast<sqlite3_file*>(read_only_storage.data());
		int read_only_out{};
		require(
			wrapper->open(
				wrapper, main_path.c_str(), read_only_file, read_only_main_flags, &read_only_out) ==
					sqlite_ok &&
				read_only_out == read_only_main_flags && last_open_flags == read_only_main_flags,
			"exact read-only main open");

		auto existing_wal_storage = file_storage(*wrapper);
		auto* existing_wal = reinterpret_cast<sqlite3_file*>(existing_wal_storage.data());
		int existing_wal_out{};
		const auto opens_before_existing_wal = open_calls;
		require(
			wrapper->open(wrapper,
						  wal_path.c_str(),
						  existing_wal,
						  requested_existing_wal_flags,
						  &existing_wal_out) == sqlite_ok &&
				open_calls == opens_before_existing_wal + 1 &&
				last_open_flags == delegated_existing_wal_flags &&
				existing_wal_out == delegated_existing_wal_flags &&
				existing_wal->methods != nullptr,
			"denied read-only main reopens only an exact existing WAL without create authority");
		require(existing_wal->methods->write(existing_wal, nullptr, 0, 0) == sqlite_readonly &&
					existing_wal->methods->close(existing_wal) == sqlite_ok,
				"read-only WAL handle cannot regain persistent effects");
		auto read_only_wal_snapshot = (*read_only_scope)->snapshot();
		require(read_only_wal_snapshot.has_value() && read_only_wal_snapshot->complete &&
					read_only_wal_snapshot->open_events.size() == 2U &&
					read_only_wal_snapshot->open_events.back().input_flags ==
						requested_existing_wal_flags &&
					read_only_wal_snapshot->open_events.back().returned_flags ==
						delegated_existing_wal_flags &&
					read_only_wal_snapshot->open_events.back().object_identity.has_value() &&
					read_only_wal_snapshot->open_events.back().directory_entry_identity.has_value(),
				"read-only WAL downgrade retained exact input/output flags and identities");

		volatile void* read_only_mapping{};
		const auto shm_maps_before_read_only = shm_map_calls;
		require(read_only_file->methods->shm_map(read_only_file, 0, 4096, 1, &read_only_mapping) ==
						sqlite_ok &&
					read_only_mapping != nullptr &&
					shm_map_calls == shm_maps_before_read_only + 1 && last_shm_extend == 0,
				"denied read-only main maps only an exact existing SHM without extension");
		auto read_only_shm_snapshot = (*read_only_scope)->snapshot();
		require(read_only_shm_snapshot.has_value() && read_only_shm_snapshot->complete &&
					read_only_shm_snapshot->shared_memory_object_identity.has_value() &&
					read_only_shm_snapshot->shared_memory_entry_identity.has_value(),
				"read-only SHM downgrade retained exact identities");

		require(::unlink(wal_path.c_str()) == 0, "missing read-only WAL fixture unlink");
		auto missing_wal_storage = file_storage(*wrapper);
		auto* missing_wal = reinterpret_cast<sqlite3_file*>(missing_wal_storage.data());
		int missing_wal_out{0x7f7f};
		const auto opens_before_missing_wal = open_calls;
		require(wrapper->open(wrapper,
							  wal_path.c_str(),
							  missing_wal,
							  requested_existing_wal_flags,
							  &missing_wal_out) == sqlite_readonly &&
					missing_wal_out == 0 && missing_wal->methods == nullptr &&
					open_calls == opens_before_missing_wal,
				"missing WAL fails before read-only downgrade delegation");
		create_file(wal_path);

		require(::unlink(shm_path.c_str()) == 0, "missing read-only SHM fixture unlink");
		volatile void* missing_shm_mapping = shm_page.data();
		const auto shm_maps_before_missing = shm_map_calls;
		require(
			read_only_file->methods->shm_map(read_only_file, 0, 4096, 1, &missing_shm_mapping) ==
					sqlite_readonly_cannot_initialize &&
				missing_shm_mapping == nullptr && shm_map_calls == shm_maps_before_missing,
			"missing SHM fails before non-extending delegate callback");
		create_file(shm_path);
		require(read_only_file->methods->close(read_only_file) == sqlite_ok,
				"read-only recovery main close");
	}

	{
		auto null_mapping_scope = bundle->observation->begin_connection_observation(main_path);
		require(null_mapping_scope.has_value(), "null SHM mapping scope begin");
		auto* null_mapping_gate = (*null_mapping_scope)->effect_gate_port();
		require(null_mapping_gate != nullptr &&
					null_mapping_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*null_mapping_scope)->token(),
										  main_path)
						.has_value(),
				"null SHM mapping deny receipt");
		auto null_mapping_storage = file_storage(*wrapper);
		auto* null_mapping_file = reinterpret_cast<sqlite3_file*>(null_mapping_storage.data());
		int null_mapping_out{};
		require(wrapper->open(wrapper,
							  main_path.c_str(),
							  null_mapping_file,
							  read_only_main_flags,
							  &null_mapping_out) == sqlite_ok,
				"null SHM mapping read-only main open");

		return_null_shm_mapping_without_extension = true;
		volatile void* null_mapping = shm_page.data();
		const auto maps_before_null = shm_map_calls;
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 1, &null_mapping) ==
						sqlite_readonly_cannot_initialize &&
					null_mapping == nullptr && shm_map_calls == maps_before_null + 1 &&
					last_shm_extend == 0,
				"OK/null non-extending SHM map becomes READONLY_CANTINIT");
		null_mapping = shm_page.data();
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 0, &null_mapping) ==
						sqlite_readonly_cannot_initialize &&
					null_mapping == nullptr && shm_map_calls == maps_before_null + 1,
				"READONLY_CANTINIT remains latched until SHM unmap");
		require(null_mapping_file->methods->shm_unmap(null_mapping_file, 0) == sqlite_ok,
				"READONLY_CANTINIT latch SHM unmap");

		return_null_shm_mapping_without_extension = false;
		shm_map_result = sqlite_readonly;
		null_mapping = shm_page.data();
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 1, &null_mapping) ==
						sqlite_readonly_cannot_initialize &&
					null_mapping == nullptr && shm_map_calls == maps_before_null + 2,
				"READONLY/non-null suppressed SHM map becomes READONLY_CANTINIT");
		null_mapping = shm_page.data();
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 0, &null_mapping) ==
						sqlite_readonly_cannot_initialize &&
					null_mapping == nullptr && shm_map_calls == maps_before_null + 2,
				"READONLY/non-null result remains latched without delegation");
		require(null_mapping_file->methods->shm_unmap(null_mapping_file, 0) == sqlite_ok,
				"READONLY/non-null latch SHM unmap");

		return_null_shm_mapping_without_extension = true;
		null_mapping = shm_page.data();
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 1, &null_mapping) ==
						sqlite_readonly_cannot_initialize &&
					null_mapping == nullptr && shm_map_calls == maps_before_null + 3,
				"READONLY/null suppressed SHM map becomes READONLY_CANTINIT");
		require(null_mapping_file->methods->shm_unmap(null_mapping_file, 0) == sqlite_ok,
				"READONLY/null latch SHM unmap");

		shm_map_result = sqlite_ok;
		return_null_shm_mapping_without_extension = false;
		require(null_mapping_file->methods->shm_map(null_mapping_file, 0, 4096, 0, &null_mapping) ==
						sqlite_ok &&
					null_mapping == shm_page.data() && shm_map_calls == maps_before_null + 4,
				"SHM unmap clears READONLY_CANTINIT latch");
		require(null_mapping_file->methods->close(null_mapping_file) == sqlite_ok,
				"null SHM mapping main close");
	}

	{
		auto replacement_scope = bundle->observation->begin_connection_observation(main_path);
		require(replacement_scope.has_value(), "WAL replacement scope begin");
		auto* replacement_gate = (*replacement_scope)->effect_gate_port();
		require(replacement_gate != nullptr &&
					replacement_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*replacement_scope)->token(),
										  main_path)
						.has_value(),
				"WAL replacement deny receipt");
		auto replacement_main_storage = file_storage(*wrapper);
		auto* replacement_main = reinterpret_cast<sqlite3_file*>(replacement_main_storage.data());
		int replacement_main_out{};
		require(wrapper->open(wrapper,
							  main_path.c_str(),
							  replacement_main,
							  read_only_main_flags,
							  &replacement_main_out) == sqlite_ok,
				"WAL replacement read-only main open");
		auto replaced_wal_storage = file_storage(*wrapper);
		auto* replaced_wal = reinterpret_cast<sqlite3_file*>(replaced_wal_storage.data());
		int replaced_wal_out{0x7f7f};
		replacement_path = wal_path;
		replacement_succeeded = false;
		replace_open_path_on_next_open = true;
		const auto closes_before_replaced_wal = close_calls;
		require(wrapper->open(wrapper,
							  wal_path.c_str(),
							  replaced_wal,
							  requested_existing_wal_flags,
							  &replaced_wal_out) == sqlite_io_error &&
					replacement_succeeded && replaced_wal_out == 0 &&
					replaced_wal->methods == nullptr &&
					close_calls == closes_before_replaced_wal + 1,
				"replaced WAL fails closed after delegate identity recheck");
		auto replaced_wal_snapshot = (*replacement_scope)->snapshot();
		require(replaced_wal_snapshot.has_value() && !replaced_wal_snapshot->complete &&
					!replaced_wal_snapshot->open_events.empty() &&
					replaced_wal_snapshot->open_events.back().outcome ==
						sqlite_backend_open_outcome::failed,
				"WAL replacement invalidates the observation receipt");
		require(replacement_main->methods->close(replacement_main) == sqlite_ok,
				"WAL replacement main close");
	}

	{
		auto replacement_scope = bundle->observation->begin_connection_observation(main_path);
		require(replacement_scope.has_value(), "SHM replacement scope begin");
		auto* replacement_gate = (*replacement_scope)->effect_gate_port();
		require(replacement_gate != nullptr &&
					replacement_gate
						->activate_denied(bundle->observation->capability_token(),
										  (*replacement_scope)->token(),
										  main_path)
						.has_value(),
				"SHM replacement deny receipt");
		auto replacement_main_storage = file_storage(*wrapper);
		auto* replacement_main = reinterpret_cast<sqlite3_file*>(replacement_main_storage.data());
		int replacement_main_out{};
		require(wrapper->open(wrapper,
							  main_path.c_str(),
							  replacement_main,
							  read_only_main_flags,
							  &replacement_main_out) == sqlite_ok,
				"SHM replacement read-only main open");
		replacement_path = shm_path;
		replacement_succeeded = false;
		replace_shm_path_on_next_map = true;
		volatile void* replaced_shm_mapping = shm_page.data();
		const auto unmaps_before_replaced_shm = shm_unmap_calls;
		require(replacement_main->methods->shm_map(
					replacement_main, 0, 4096, 1, &replaced_shm_mapping) == sqlite_io_error &&
					replacement_succeeded && replaced_shm_mapping == nullptr &&
					last_shm_extend == 0 && shm_unmap_calls == unmaps_before_replaced_shm + 1,
				"replaced SHM fails closed and releases the delegated mapping");
		auto replaced_shm_snapshot = (*replacement_scope)->snapshot();
		require(replaced_shm_snapshot.has_value() && !replaced_shm_snapshot->complete,
				"SHM replacement invalidates the observation receipt");
		require(replacement_main->methods->close(replacement_main) == sqlite_ok,
				"SHM replacement main close");
	}

	auto forgotten_scope = bundle->observation->begin_connection_observation(main_path);
	require(forgotten_scope.has_value(), "forgotten-activation scope begin");
	require((*forgotten_scope)->token() != (*failed_scope)->token(), "connection token unique");
	auto* forgotten_gate = (*forgotten_scope)->effect_gate_port();
	require(forgotten_gate != nullptr && forgotten_gate->enforcement_active() &&
				forgotten_gate->stage() == sqlite_backend_effect_stage::denied &&
				!forgotten_gate->latest_receipt(),
			"scope construction is deny-by-default");
	auto forgotten_storage = file_storage(*wrapper);
	auto* forgotten_file = reinterpret_cast<sqlite3_file*>(forgotten_storage.data());
	int forgotten_out{};
	const auto create_main_flags = sqlite_open_main_database | 0x2 | sqlite_open_create;
	require(wrapper->open(
				wrapper, main_path.c_str(), forgotten_file, create_main_flags, &forgotten_out) ==
				sqlite_ok,
			"pre-created main opens while unsealed");
	require((last_open_flags & sqlite_open_create) == 0,
			"unsealed main strips delegate create authority");
	require(forgotten_file->methods->write(forgotten_file, nullptr, 0, 0) == sqlite_readonly &&
				forgotten_file->methods->truncate(forgotten_file, 0) == sqlite_readonly,
			"forgotten activation denies write and truncate");
	long long forgotten_size_hint{};
	require(forgotten_file->methods->file_control(forgotten_file,
												  sqlite_file_control_size_hint,
												  &forgotten_size_hint) == sqlite_readonly &&
				forgotten_file->methods->file_control(
					forgotten_file, sqlite_file_control_sync, nullptr) == sqlite_not_found,
			"denied gate blocks size mutation but delegates pre-sync notification");
	const auto removes_before_forgotten = remove_calls;
	require(wrapper->remove(wrapper, main_path.c_str(), 0) == sqlite_readonly &&
				remove_calls == removes_before_forgotten,
			"forgotten activation denies delete before delegate");
	auto forgotten_journal_storage = file_storage(*wrapper);
	auto* forgotten_journal = reinterpret_cast<sqlite3_file*>(forgotten_journal_storage.data());
	int forgotten_journal_out{0x7f7f};
	const auto opens_before_forgotten_sidecar = open_calls;
	require(wrapper->open(wrapper,
						  journal_path.c_str(),
						  forgotten_journal,
						  sqlite_open_main_journal | 0x2 | sqlite_open_create,
						  &forgotten_journal_out) == sqlite_readonly &&
				forgotten_journal_out == 0 && forgotten_journal->methods == nullptr &&
				open_calls == opens_before_forgotten_sidecar,
			"forgotten activation denies sidecar create before delegate");
	volatile void* forgotten_mapping{};
	const auto shm_maps_before_forgotten = shm_map_calls;
	require(forgotten_file->methods->shm_map(forgotten_file, 0, 4096, 1, &forgotten_mapping) ==
					sqlite_readonly_cannot_initialize &&
				shm_map_calls == shm_maps_before_forgotten,
			"forgotten activation denies SHM extension");
	const auto closes_before_forgotten = close_calls;
	require(forgotten_file->methods->close(forgotten_file) == sqlite_ok &&
				close_calls == closes_before_forgotten + 1,
			"xClose is always delegated while denied");

	auto rejected_scope = bundle->observation->begin_connection_observation(main_path);
	require(rejected_scope.has_value(), "failed-recheck scope begin");
	auto rejected_storage = file_storage(*wrapper);
	auto* rejected_file = reinterpret_cast<sqlite3_file*>(rejected_storage.data());
	int rejected_out{};
	const auto main_flags = sqlite_open_main_database | 0x2;
	require(wrapper->open(wrapper, main_path.c_str(), rejected_file, main_flags, &rejected_out) ==
				sqlite_ok,
			"failed-recheck main open");
	auto* rejected_gate = (*rejected_scope)->effect_gate_port();
	require(rejected_gate != nullptr, "failed-recheck gate");
	auto rejected_activation = rejected_gate->activate_denied(
		bundle->observation->capability_token(), (*rejected_scope)->token(), main_path);
	require(rejected_activation.has_value() && rejected_activation->sequence == 1U,
			"failed-recheck deny receipt");
	auto rejecting_authority = std::make_shared<test_arm_authority>(false, true);
	require(rejected_gate
				->install_arm_on_exclusive_lock(
					make_arm_request(*bundle->observation,
									 **rejected_scope,
									 main_path,
									 sqlite_backend_effect_stage::fully_armed,
									 rejecting_authority,
									 "reject"))
				.has_value(),
			"install rejected exclusive plan");
	exclusive_lock_seen = false;
	require(rejected_file->methods->lock(rejected_file, sqlite_lock_exclusive) == sqlite_io_error &&
				exclusive_lock_seen && rejecting_authority->calls == 1 &&
				rejected_gate->stage() == sqlite_backend_effect_stage::denied,
			"exclusive callback recheck failure stays denied");
	auto rejected_latest = rejected_gate->latest_receipt();
	require(rejected_latest.has_value() && rejected_latest->sequence == 1U &&
				rejected_latest->stage == sqlite_backend_effect_stage::denied &&
				rejected_file->methods->write(rejected_file, nullptr, 0, 0) == sqlite_readonly,
			"failed arm preserves only deny receipt");
	require(rejected_file->methods->close(rejected_file) == sqlite_ok,
			"failed arm still permits close");

	auto scope = bundle->observation->begin_connection_observation(main_path);
	require(scope.has_value(), "matrix scope begin");
	auto storage = file_storage(*wrapper);
	auto* file = reinterpret_cast<sqlite3_file*>(storage.data());
	int returned_flags{};
	require(wrapper->open(wrapper, main_path.c_str(), file, main_flags, &returned_flags) ==
				sqlite_ok,
			"matrix main open");
	require(returned_flags == (main_flags | 0x2), "success pOutFlags exact");
	require(file->methods != nullptr && file->methods->version == 3, "main methods v3");
	auto main_snapshot = (*scope)->snapshot();
	require(main_snapshot.has_value() && main_snapshot->complete &&
				main_snapshot->main_handle_open && main_snapshot->open_events.size() == 1U &&
				main_snapshot->open_events.front().returned_flags == returned_flags &&
				main_snapshot->open_events.front().object_identity.has_value() &&
				main_snapshot->open_events.front().directory_entry_identity.has_value(),
			"main observation proof");

	auto* gate = (*scope)->effect_gate_port();
	require(gate != nullptr, "matrix effect gate");
	auto activation = gate->activate_denied(
		bundle->observation->capability_token(), (*scope)->token(), main_path);
	require(activation.has_value() && activation->sequence == 1U &&
				activation->stage == sqlite_backend_effect_stage::denied &&
				activation->capability_token == bundle->observation->capability_token() &&
				activation->connection_token == (*scope)->token() &&
				activation->canonical_vfs_locator == main_path &&
				!activation->validation_receipt.bytes.empty(),
			"typed deny receipt exact");
	require(!gate->activate_denied(
				bundle->observation->capability_token(), (*scope)->token(), main_path),
			"deny activation seals exactly once");

	auto coordination_authority = std::make_shared<test_arm_authority>(true, false);
	sqlite_backend_effect_gate_state exhausted_gate{
		**scope,
		bundle->observation->capability_token(),
		(*scope)->token(),
		main_path,
		"sequence-exhaustion-test-v1",
		true,
		std::numeric_limits<std::uint64_t>::max() - 1U,
	};
	auto exhaustion_activation = exhausted_gate.activate_denied(
		bundle->observation->capability_token(), (*scope)->token(), main_path);
	require(exhaustion_activation.has_value() &&
				exhaustion_activation->sequence == std::numeric_limits<std::uint64_t>::max() - 1U,
			"sequence exhaustion fixture reaches final incrementable receipt");
	auto exhaustion_authority = std::make_shared<test_arm_authority>(true, false);
	require(!exhausted_gate.arm_now(
				make_arm_request(*bundle->observation,
								 **scope,
								 main_path,
								 sqlite_backend_effect_stage::wal_shm_coordination_only,
								 exhaustion_authority,
								 "sequence-exhausted")) &&
				exhaustion_authority->calls == 0 &&
				exhausted_gate.stage() == sqlite_backend_effect_stage::denied &&
				exhausted_gate.latest_receipt().has_value() &&
				exhausted_gate.latest_receipt()->sequence ==
					std::numeric_limits<std::uint64_t>::max() - 1U,
			"UINT64_MAX next sequence fails closed before authority callback");
	auto coordination =
		gate->arm_now(make_arm_request(*bundle->observation,
									   **scope,
									   main_path,
									   sqlite_backend_effect_stage::wal_shm_coordination_only,
									   coordination_authority,
									   "coordination"));
	require(coordination.has_value() && coordination->sequence == 2U &&
				coordination->stage == sqlite_backend_effect_stage::wal_shm_coordination_only &&
				!coordination->armed_after_underlying_exclusive_lock &&
				coordination_authority->calls == 1,
			"coordination-only receipt");
	volatile void* mapped{};
	require(file->methods->shm_map(file, 0, 4096, 1, &mapped) == sqlite_ok && mapped != nullptr,
			"coordination-only permits SHM extension");
	auto coordination_wal_storage = file_storage(*wrapper);
	auto* coordination_wal = reinterpret_cast<sqlite3_file*>(coordination_wal_storage.data());
	int coordination_wal_out{};
	const auto coordination_wal_flags =
		sqlite_open_read_write | sqlite_open_create | sqlite_open_write_ahead_log;
	const auto opens_before_coordination_wal = open_calls;
	require(wrapper->open(wrapper,
						  wal_path.c_str(),
						  coordination_wal,
						  coordination_wal_flags,
						  &coordination_wal_out) == sqlite_ok &&
				open_calls == opens_before_coordination_wal + 1 &&
				last_open_flags == coordination_wal_flags && coordination_wal->methods != nullptr &&
				coordination_wal->methods->close(coordination_wal) == sqlite_ok,
			"coordination-only permits only the exact WAL create callback");
	auto coordination_snapshot = (*scope)->snapshot();
	require(coordination_snapshot.has_value() && coordination_snapshot->complete &&
				!coordination_snapshot->open_events.empty() &&
				coordination_snapshot->open_events.back().role ==
					sqlite_backend_file_role::write_ahead_log &&
				coordination_snapshot->open_events.back().input_flags == coordination_wal_flags &&
				coordination_snapshot->open_events.back().outcome ==
					sqlite_backend_open_outcome::succeeded &&
				coordination_snapshot->open_events.back().returned_flags.has_value() &&
				coordination_snapshot->open_events.back().object_identity.has_value() &&
				coordination_snapshot->open_events.back().directory_entry_identity.has_value(),
			"coordination WAL success retained exact flags and identities");
	auto wrong_wal_storage = file_storage(*wrapper);
	auto* wrong_wal = reinterpret_cast<sqlite3_file*>(wrong_wal_storage.data());
	int wrong_wal_out{0x7f7f};
	const auto opens_before_wrong_wal = open_calls;
	require(wrapper->open(wrapper,
						  wal_path.c_str(),
						  wrong_wal,
						  0x1 | sqlite_open_create | sqlite_open_write_ahead_log,
						  &wrong_wal_out) == sqlite_readonly &&
				wrong_wal_out == 0 && wrong_wal->methods == nullptr &&
				open_calls == opens_before_wrong_wal,
			"coordination-only rejects non-exact WAL create flags before delegation");
	require(file->methods->write(file, nullptr, 0, 0) == sqlite_readonly &&
				file->methods->truncate(file, 0) == sqlite_readonly,
			"coordination-only denies persistent file effects");
	const auto coordination_removes = remove_calls;
	require(wrapper->remove(wrapper, main_path.c_str(), 0) == sqlite_readonly &&
				remove_calls == coordination_removes,
			"coordination-only denies delete");
	auto denied_journal_storage = file_storage(*wrapper);
	auto* denied_journal = reinterpret_cast<sqlite3_file*>(denied_journal_storage.data());
	int denied_journal_out{};
	const auto coordination_opens = open_calls;
	require(wrapper->open(wrapper,
						  journal_path.c_str(),
						  denied_journal,
						  sqlite_open_main_journal | 0x2 | sqlite_open_create,
						  &denied_journal_out) == sqlite_readonly &&
				open_calls == coordination_opens,
			"coordination-only denies sidecar create");

	auto full_authority = std::make_shared<test_arm_authority>(true, true);
	require(gate->install_arm_on_exclusive_lock(
					make_arm_request(*bundle->observation,
									 **scope,
									 main_path,
									 sqlite_backend_effect_stage::fully_armed,
									 full_authority,
									 "full"))
				.has_value(),
			"install exclusive full-arm plan");
	exclusive_lock_seen = false;
	require(file->methods->lock(file, sqlite_lock_exclusive) == sqlite_ok && exclusive_lock_seen &&
				full_authority->calls == 1 &&
				gate->stage() == sqlite_backend_effect_stage::fully_armed,
			"exclusive lock success rechecks then arms");
	auto full_receipt = gate->latest_receipt();
	require(full_receipt.has_value() && full_receipt->sequence == 3U &&
				full_receipt->stage == sqlite_backend_effect_stage::fully_armed &&
				full_receipt->armed_after_underlying_exclusive_lock &&
				full_receipt->capability_token == bundle->observation->capability_token() &&
				full_receipt->connection_token == (*scope)->token() &&
				full_receipt->prerequisite_receipt == test_receipt("full") &&
				full_receipt->validation_receipt == test_receipt("validated"),
			"exclusive full-arm receipt exact");
	const auto writes_before_full = write_calls;
	const auto truncates_before_full = truncate_calls;
	require(file->methods->write(file, nullptr, 0, 0) == sqlite_ok &&
				file->methods->truncate(file, 0) == sqlite_ok &&
				write_calls == writes_before_full + 1 &&
				truncate_calls == truncates_before_full + 1,
			"fully armed delegates write and truncate");
	auto journal_storage = file_storage(*wrapper);
	auto* journal_file = reinterpret_cast<sqlite3_file*>(journal_storage.data());
	int journal_out{};
	require(wrapper->open(wrapper,
						  journal_path.c_str(),
						  journal_file,
						  sqlite_open_main_journal | 0x2 | sqlite_open_create,
						  &journal_out) == sqlite_ok,
			"fully armed delegates journal create");
	require(journal_file->methods != nullptr && journal_file->methods->version == 1,
			"journal methods downgraded");
	require(journal_file->methods->close(journal_file) == sqlite_ok, "journal close");
	require(wrapper->remove(wrapper, main_path.c_str(), 0) == sqlite_ok,
			"fully armed delegates delete");
	require(file->methods->shm_lock(file, 3, 1, sqlite_shm_lock | sqlite_shm_exclusive) ==
				sqlite_ok,
			"shm lock");
	auto locked = (*scope)->snapshot();
	require(locked.has_value() && locked->held_shm_locks.size() == 1U &&
				locked->shared_memory_object_identity.has_value(),
			"shm lock observed");
	require(file->methods->shm_lock(file, 3, 1, sqlite_shm_unlock | sqlite_shm_exclusive) ==
				sqlite_ok,
			"shm unlock");
	auto unlocked = (*scope)->snapshot();
	require(unlocked.has_value() && unlocked->held_shm_locks.empty(),
			"shm unlock shrinks held set");
	auto scratch_scope = bundle->observation->begin_ephemeral_connection_observation();
	require(scratch_scope.has_value(), "filesystem capability same-alias scratch scope");
	auto scratch_snapshot = (*scratch_scope)->snapshot();
	require(scratch_snapshot.has_value() && scratch_snapshot->profile == "default-ephemeral-v1" &&
				scratch_snapshot->complete && !scratch_snapshot->main_handle_open &&
				scratch_snapshot->open_events.empty(),
			"scratch scope is typed no-VFS-open receipt");
	auto* scratch_gate = (*scratch_scope)->effect_gate_port();
	require(scratch_gate != nullptr &&
				scratch_gate
					->activate_denied(bundle->observation->capability_token(),
									  (*scratch_scope)->token(),
									  ":memory:")
					.has_value(),
			"scratch deny receipt uses filesystem capability token");

	const std::string alias_name{bundle->forwarding_vfs->registered_vfs_name()};
	bundle->observation.reset();
	bundle->connection_observation_port.reset();
	bundle->forwarding_vfs.reset();
	require(fake_find(alias_name.c_str()) == wrapper, "open file retains alias");
	require(file->methods->close(file) == sqlite_ok, "main close");
	require(fake_find(alias_name.c_str()) == nullptr, "close releases and unregisters alias");

	auto ephemeral = cxxlens::sdk::make_sqlite_default_ephemeral_store_bundle(registry_binding());
	require(ephemeral.has_value() && ephemeral->sqlite_locator == ":memory:", "ephemeral bundle");
	auto ephemeral_scope = ephemeral->observation->begin_connection_observation(":memory:");
	require(ephemeral_scope.has_value(), "ephemeral scope");
	auto ephemeral_snapshot = (*ephemeral_scope)->snapshot();
	require(ephemeral_snapshot.has_value() && ephemeral_snapshot->complete &&
				!ephemeral_snapshot->main_handle_open && ephemeral_snapshot->open_events.empty(),
			"ephemeral zero-event not-applicable proof");
	auto ephemeral_wal_recovery = ephemeral->observation->create_wal_recovery_workspace();
	require(!ephemeral_wal_recovery &&
				ephemeral_wal_recovery.error().code == "store.backend-unavailable" &&
				ephemeral_wal_recovery.error().field == "sqlite-observation" &&
				ephemeral_wal_recovery.error().detail == "wal-recovery-workspace-unavailable",
			"ephemeral observation WAL recovery factory fails closed with a typed result");

	require(original_pointer_ok, "every VFS callback receives original pointer");
	require(original_app_data_ok, "every VFS callback preserves pAppData");
	require(trailing_alignment_ok, "underlying trailing storage aligned");
	require(null_find_calls == 0, "find(nullptr) never used");
	require(::unlink(main_path.c_str()) == 0, "remove main fixture");
	require(::unlink(shm_path.c_str()) == 0, "remove shm fixture");
	require(::unlink(journal_path.c_str()) == 0, "remove journal fixture");
	require(::unlink(wal_path.c_str()) == 0, "remove WAL fixture");
	exercise_real_sqlite();
	std::cout << "sqlite default forwarding VFS tests passed\n";
}
