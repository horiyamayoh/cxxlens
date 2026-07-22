#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <link.h>

#include "sqlite_backend_effect_gate_internal.hpp"
#include "sqlite_default_forwarding_vfs_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		constexpr int sqlite_ok = 0;
		constexpr int sqlite_error = 1;
		constexpr int sqlite_no_memory = 7;
		constexpr int sqlite_readonly = 8;
		constexpr int sqlite_readonly_cannot_initialize = sqlite_readonly | (5 << 8);
		constexpr int sqlite_io_error = 10;
		constexpr int sqlite_not_found = 12;
		constexpr int sqlite_cannot_open = 14;
		constexpr int sqlite_open_memory = 0x00000080;
		constexpr int sqlite_open_read_only = 0x00000001;
		constexpr int sqlite_open_read_write = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_uri = 0x00000040;
		constexpr int sqlite_open_full_mutex = 0x00010000;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_private_cache = 0x00040000;
		constexpr int sqlite_open_main_journal = 0x00000800;
		constexpr int sqlite_open_write_ahead_log = 0x00080000;
		constexpr int sqlite_open_file_type_mask = 0x000fff00;
		constexpr int sqlite_file_control_has_moved = 20;
		constexpr int sqlite_lock_exclusive = 4;
		constexpr std::array effectful_file_controls{
			5,	// SQLITE_FCNTL_SIZE_HINT
			6,	// SQLITE_FCNTL_CHUNK_SIZE
			10, // SQLITE_FCNTL_PERSIST_WAL
			11, // SQLITE_FCNTL_OVERWRITE
			22, // SQLITE_FCNTL_COMMIT_PHASETWO
			31, // SQLITE_FCNTL_BEGIN_ATOMIC_WRITE
			32, // SQLITE_FCNTL_COMMIT_ATOMIC_WRITE
			33, // SQLITE_FCNTL_ROLLBACK_ATOMIC_WRITE
			37, // SQLITE_FCNTL_CKPT_DONE
			38, // SQLITE_FCNTL_RESERVE_BYTES
		};
		constexpr int sqlite_shm_unlock = 1;
		constexpr int sqlite_shm_lock = 2;
		constexpr int sqlite_shm_shared = 4;
		constexpr int sqlite_shm_exclusive = 8;
		constexpr std::size_t maximum_pathname_bytes = std::size_t{1024U} * 1024U;
		constexpr std::size_t maximum_open_observations = 64U;
		constexpr std::size_t maximum_shm_lock_observations = 64U;
		constexpr std::size_t maximum_shm_map_observations = 64U;
		constexpr std::string_view journal_suffix{"-journal"};
		constexpr std::string_view wal_suffix{"-wal"};
		constexpr std::string_view forwarding_profile{"default-forwarding-vfs-v1"};
		constexpr std::string_view filesystem_profile{"default-filesystem-v1"};
		constexpr std::string_view ephemeral_profile{"default-ephemeral-v1"};
		constexpr std::string_view source_shm_profile{"sqlite-source-shm-readonly-unix-uri-v1"};
		constexpr std::string_view source_shm_qualification_profile{
			"sqlite-source-shm-readonly-qualification-candidate-v1"};
		constexpr int source_shm_open_flags = sqlite_open_read_only | sqlite_open_uri |
			sqlite_open_private_cache | sqlite_open_full_mutex;
		constexpr int source_shm_main_xopen_flags =
			sqlite_open_read_only | sqlite_open_uri | sqlite_open_main_database;
		constexpr int qualification_fixture_main_xopen_flags =
			sqlite_open_read_write | sqlite_open_create | sqlite_open_main_database;

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

		[[nodiscard]] error forwarding_error(std::string detail)
		{
			return {"store.backend-unavailable", "sqlite", std::move(detail)};
		}

		[[nodiscard]] error canonicalization_error()
		{
			return {"store.sqlite-failure", "sqlite-locator", "canonicalization-failed"};
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
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		}

		void append_pointer(std::vector<std::byte>& output, const void* value)
		{
			append_u64(output, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
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

		[[nodiscard]] std::string strict_source_shm_uri(const std::string_view canonical_locator)
		{
			constexpr std::string_view hexadecimal{"0123456789ABCDEF"};
			std::string output{"file:"};
			output.reserve(5U + canonical_locator.size() * 3U + 45U);
			for (const auto raw : canonical_locator)
			{
				const auto value = static_cast<unsigned char>(raw);
				if (uri_unreserved(value))
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

		[[nodiscard]] constexpr std::optional<std::size_t>
		checked_align_up(const std::size_t value, const std::size_t alignment) noexcept
		{
			if (alignment == 0U || (alignment & (alignment - 1U)) != 0U)
				return std::nullopt;
			const auto remainder = value & (alignment - 1U);
			const auto padding = remainder == 0U ? 0U : alignment - remainder;
			if (value > std::numeric_limits<std::size_t>::max() - padding)
				return std::nullopt;
			return value + padding;
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

		struct image_segment_query
		{
			std::uintptr_t code{};
			std::uintptr_t data_begin{};
			std::uintptr_t data_end{};
			bool found{};
		};

		int find_bound_image_segments(dl_phdr_info* information, std::size_t, void* opaque) noexcept
		{
			auto& query = *static_cast<image_segment_query*>(opaque);
			bool executable_code{};
			bool readable_data{};
			for (std::size_t index{}; index < information->dlpi_phnum; ++index)
			{
				const auto& header = information->dlpi_phdr[index];
				if (header.p_type != PT_LOAD ||
					static_cast<std::uintptr_t>(information->dlpi_addr) >
						std::numeric_limits<std::uintptr_t>::max() - header.p_vaddr)
					continue;
				const auto segment_begin = static_cast<std::uintptr_t>(information->dlpi_addr) +
					static_cast<std::uintptr_t>(header.p_vaddr);
				if (static_cast<std::uintptr_t>(header.p_memsz) >
					std::numeric_limits<std::uintptr_t>::max() - segment_begin)
					continue;
				const auto segment_end =
					segment_begin + static_cast<std::uintptr_t>(header.p_memsz);
				if ((header.p_flags & PF_X) != 0U && query.code >= segment_begin &&
					query.code < segment_end)
					executable_code = true;
				if ((header.p_flags & PF_R) != 0U && query.data_begin >= segment_begin &&
					query.data_end <= segment_end)
					readable_data = true;
			}
			if (executable_code && readable_data)
			{
				query.found = true;
				return 1;
			}
			return 0;
		}

		[[nodiscard]] bool readable_range_bound_to_code(const void* address,
														const std::size_t size,
														const std::size_t alignment,
														const void* code,
														const void* image) noexcept
		{
			if (address == nullptr || code == nullptr || image == nullptr || size == 0U ||
				alignment == 0U || reinterpret_cast<std::uintptr_t>(address) % alignment != 0U)
				return false;
			Dl_info data_information{};
			Dl_info code_information{};
			if (::dladdr(address, &data_information) == 0 || data_information.dli_fbase != image ||
				::dladdr(code, &code_information) == 0 || code_information.dli_fbase != image)
				return false;
			const auto begin = reinterpret_cast<std::uintptr_t>(address);
			if (size > std::numeric_limits<std::uintptr_t>::max() - begin)
				return false;
			image_segment_query query{
				reinterpret_cast<std::uintptr_t>(code), begin, begin + size, false};
			(void)::dl_iterate_phdr(find_bound_image_segments, &query);
			return query.found;
		}

		[[nodiscard]] bool exact_suffix_path(const std::string_view value,
											 const std::string_view base,
											 const std::string_view suffix) noexcept
		{
			return value.size() == base.size() + suffix.size() && value.starts_with(base) &&
				value.substr(base.size()) == suffix;
		}

		[[nodiscard]] bool exact_proc_self_fd_locator(const std::string_view value,
													  const std::string_view descendant) noexcept
		{
			constexpr std::string_view prefix{"/proc/self/fd/"};
			if (!value.starts_with(prefix) || descendant.empty())
				return false;
			const auto fd_begin = prefix.size();
			const auto separator = value.find('/', fd_begin);
			if (separator == std::string_view::npos || separator == fd_begin ||
				(separator - fd_begin > 1U && value[fd_begin] == '0') ||
				value.substr(separator + 1U) != descendant)
				return false;
			return std::ranges::all_of(value.substr(fd_begin, separator - fd_begin),
									   [](const char byte)
									   {
										   return byte >= '0' && byte <= '9';
									   });
		}

		[[nodiscard]] bool qualification_candidate_locator(const std::string_view value) noexcept
		{
			return exact_proc_self_fd_locator(value, "cold/main.db") ||
				exact_proc_self_fd_locator(value, "active/main.db");
		}

		[[nodiscard]] bool exact_sqlite_family_path(const std::string_view value,
													const std::string_view main) noexcept
		{
			return value == main || exact_suffix_path(value, main, journal_suffix) ||
				exact_suffix_path(value, main, wal_suffix) ||
				exact_suffix_path(value, main, "-shm");
		}

		class default_forwarding_state;

		struct default_connection_observation final : sqlite_backend_connection_observation_scope
		{
			mutable std::mutex mutex;
			sqlite_backend_opaque_identity capability_token_value;
			sqlite_backend_opaque_identity connection_token_value;
			std::string canonical_locator;
			std::thread::id originating_thread;
			std::vector<sqlite_backend_open_observation> open_events;
			std::optional<sqlite_backend_opaque_identity> shared_memory_object_identity;
			std::optional<sqlite_backend_opaque_identity> shared_memory_entry_identity;
			std::vector<sqlite_backend_shm_lock_observation> held_shm_locks;
			std::vector<sqlite_backend_shm_map_observation> shm_map_events;
			std::optional<sqlite_source_shm_qualified_open_plan> source_shm_open_plan;
			std::optional<sqlite_source_shm_qualification_open_plan>
				source_shm_qualification_open_plan;
			std::optional<sqlite_source_shm_qualification_fixture_fullpath_plan>
				source_shm_qualification_fixture_fullpath_plan;
			std::optional<sqlite_source_shm_qualification_fixture_fullpath_plan>
				source_shm_qualification_fixture_pending_open_plan;
			std::optional<sqlite_source_shm_open_callback_receipt> source_shm_open_callback_receipt;
			std::weak_ptr<default_forwarding_state> owner;
			std::string profile;
			bool complete{};
			bool invalid{};
			bool source_shm_open_rejected{};
			bool source_shm_qualification_fullpath_preserved{};
			bool source_shm_qualification_fixture_main_accepted{};
			bool source_shm_target_fullpath_projected{};
			bool main_proven{};
			bool main_claimed{};
			bool main_handle_open{};
			bool main_handle_read_only{};
			std::unique_ptr<sqlite_backend_effect_gate_state> effect_gate;

			[[nodiscard]] const sqlite_backend_opaque_identity& token() const noexcept override
			{
				return connection_token_value;
			}

			[[nodiscard]] result<sqlite_backend_connection_observation> snapshot() const override
			{
				try
				{
					std::scoped_lock lock{mutex};
					return sqlite_backend_connection_observation{
						profile,
						capability_token_value,
						connection_token_value,
						open_events,
						shared_memory_object_identity,
						shared_memory_entry_identity,
						held_shm_locks,
						complete,
						main_handle_open,
						source_shm_open_callback_receipt,
						shm_map_events,
					};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
			}

			[[nodiscard]] sqlite_backend_effect_gate* effect_gate_port() noexcept override
			{
				return effect_gate.get();
			}

			[[nodiscard]] result<void>
			arm_source_shm_readonly_profile(sqlite_source_shm_qualified_open_plan plan) override;

			[[nodiscard]] result<void> arm_source_shm_readonly_qualification_candidate(
				sqlite_source_shm_qualification_open_plan plan) override;

			[[nodiscard]] result<void> arm_source_shm_qualification_fixture_fullpath(
				sqlite_source_shm_qualification_fixture_fullpath_plan plan) override;

			[[nodiscard]] bool permits_persistent_effect(const bool shm_coordination) const noexcept
			{
				return effect_gate != nullptr &&
					effect_gate->permits_persistent_effect(shm_coordination);
			}

			[[nodiscard]] bool permits_existing_read_only_sidecars() const noexcept
			{
				try
				{
					{
						std::scoped_lock lock{mutex};
						if (invalid || !main_proven || !main_handle_open ||
							!main_handle_read_only || effect_gate == nullptr)
							return false;
					}
					if (effect_gate->stage() != sqlite_backend_effect_stage::denied)
						return false;
					auto receipt = effect_gate->latest_receipt();
					return receipt.has_value() &&
						receipt->stage == sqlite_backend_effect_stage::denied;
				}
				catch (...)
				{
					return false;
				}
			}
		};

		struct opened_object_identities
		{
			sqlite_backend_opaque_identity object;
			sqlite_backend_opaque_identity entry;
		};

		struct native_file_node
		{
			native_file_node(const std::size_t storage_bytes,
							 std::shared_ptr<default_forwarding_state> owner_pin,
							 std::shared_ptr<void> runtime_pin,
							 std::shared_ptr<default_connection_observation> observation_pin,
							 std::shared_ptr<sqlite_source_shm_target_namespace_epoch> epoch_pin,
							 sqlite3_vfs* underlying_vfs,
							 const void* underlying_app_data,
							 const void* underlying_image,
							 const void* underlying_open_callback)
				: storage_count{(storage_bytes + sizeof(std::max_align_t) - 1U) /
								sizeof(std::max_align_t)},
				  storage{std::make_unique<std::max_align_t[]>(storage_count)},
				  owner{std::move(owner_pin)}, runtime_lifetime{std::move(runtime_pin)},
				  observation{std::move(observation_pin)},
				  target_namespace_epoch{std::move(epoch_pin)}, underlying{underlying_vfs},
				  underlying_vfs_identity{underlying_vfs},
				  underlying_app_data_identity{underlying_app_data},
				  underlying_image_identity{underlying_image},
				  underlying_open_callback_address{underlying_open_callback}
			{
				std::memset(storage.get(), 0, storage_count * sizeof(std::max_align_t));
			}

			[[nodiscard]] sqlite3_file* file() noexcept
			{
				return reinterpret_cast<sqlite3_file*>(storage.get());
			}

			std::size_t storage_count{};
			std::unique_ptr<std::max_align_t[]> storage;
			std::shared_ptr<default_forwarding_state> owner;
			std::shared_ptr<void> runtime_lifetime;
			std::shared_ptr<default_connection_observation> observation;
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
			sqlite3_vfs* underlying{};
			const void* underlying_vfs_identity{};
			const void* underlying_app_data_identity{};
			const void* underlying_image_identity{};
			const void* underlying_open_callback_address{};
			sqlite3_io_methods trusted_methods{};
			bool trusted_methods_ready{};
			int (*trusted_close)(sqlite3_file*){};
			std::shared_ptr<native_file_node> quarantine_self;
			bool close_attempted{};
		};

		void quarantine_native_file(std::shared_ptr<native_file_node>& node) noexcept
		{
			// `quarantine_self` was armed before native xOpen. Dropping the external owner is
			// allocation-free and leaves the whole opaque file and every pin alive for process
			// life.
			node.reset();
		}

		void release_known_safe_native_file(std::shared_ptr<native_file_node>& node) noexcept
		{
			if (node)
				node->quarantine_self.reset();
			node.reset();
		}

		struct forwarding_file
		{
			sqlite3_file base{};
			std::shared_ptr<default_forwarding_state> owner;
			std::shared_ptr<default_connection_observation> connection_observation;
			sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
			bool observed_role{};
			bool main_handle{};
			bool shm_readonly_cannot_initialize{};
			bool source_shm_readonly_qualified{};
			bool source_shm_qualification_candidate{};
			bool source_shm_readonly_family_seen{};
			bool source_shm_terminal_failure{};
			std::optional<opened_object_identities> expected_source_shm_identity;
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
			std::shared_ptr<native_file_node> native;
		};

		[[nodiscard]] forwarding_file* forwarding(sqlite3_file* value) noexcept
		{
			return reinterpret_cast<forwarding_file*>(value);
		}

		[[nodiscard]] sqlite3_file* underlying_file(forwarding_file& value) noexcept;
		[[nodiscard]] const sqlite3_io_methods* underlying_methods(forwarding_file& value) noexcept;

		int forwarding_close(sqlite3_file* base) noexcept;
		int forwarding_read(sqlite3_file* base, void* output, int count, long long offset) noexcept;
		int forwarding_write(sqlite3_file* base,
							 const void* input,
							 int count,
							 long long offset) noexcept;
		int forwarding_truncate(sqlite3_file* base, long long size) noexcept;
		int forwarding_sync(sqlite3_file* base, int flags) noexcept;
		int forwarding_file_size(sqlite3_file* base, long long* output) noexcept;
		int forwarding_lock(sqlite3_file* base, int level) noexcept;
		int forwarding_unlock(sqlite3_file* base, int level) noexcept;
		int forwarding_reserved(sqlite3_file* base, int* output) noexcept;
		int forwarding_control(sqlite3_file* base, int operation, void* value) noexcept;
		int forwarding_sector(sqlite3_file* base) noexcept;
		int forwarding_characteristics(sqlite3_file* base) noexcept;
		int forwarding_shm_map(sqlite3_file* base,
							   int page,
							   int page_size,
							   int extend,
							   volatile void** output) noexcept;
		int forwarding_shm_lock(sqlite3_file* base, int offset, int count, int flags) noexcept;
		void forwarding_shm_barrier(sqlite3_file* base) noexcept;
		int forwarding_shm_unmap(sqlite3_file* base, int remove_file) noexcept;
		int
		forwarding_fetch(sqlite3_file* base, long long offset, int count, void** output) noexcept;
		int forwarding_unfetch(sqlite3_file* base, long long offset, void* value) noexcept;

		const sqlite3_io_methods forwarding_io_v1{
			1,
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
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
			nullptr,
		};

		const sqlite3_io_methods forwarding_io_v2{
			2,
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
			nullptr,
			nullptr,
		};

		const sqlite3_io_methods forwarding_io_v3{
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

		int forwarding_vfs_open(sqlite3_vfs* vfs,
								const char* name,
								sqlite3_file* output,
								int flags,
								int* out_flags) noexcept;
		int forwarding_vfs_remove(sqlite3_vfs* vfs, const char* name, int sync_directory) noexcept;
		int
		forwarding_vfs_access(sqlite3_vfs* vfs, const char* name, int flags, int* output) noexcept;
		int forwarding_vfs_full_pathname(sqlite3_vfs* vfs,
										 const char* name,
										 int size,
										 char* output) noexcept;
		void* forwarding_vfs_dl_open(sqlite3_vfs* vfs, const char* name) noexcept;
		void forwarding_vfs_dl_error(sqlite3_vfs* vfs, int size, char* output) noexcept;
		void (*forwarding_vfs_dl_sym(sqlite3_vfs* vfs,
									 void* handle,
									 const char* name) noexcept)(void);
		void forwarding_vfs_dl_close(sqlite3_vfs* vfs, void* handle) noexcept;
		int forwarding_vfs_randomness(sqlite3_vfs* vfs, int size, char* output) noexcept;
		int forwarding_vfs_sleep(sqlite3_vfs* vfs, int microseconds) noexcept;
		int forwarding_vfs_current_time(sqlite3_vfs* vfs, double* output) noexcept;
		int forwarding_vfs_last_error(sqlite3_vfs* vfs, int size, char* output) noexcept;
		int forwarding_vfs_current_time_int64(sqlite3_vfs* vfs, long long* output) noexcept;
		int forwarding_vfs_set_system_call(sqlite3_vfs* vfs,
										   const char* name,
										   sqlite3_syscall_ptr function) noexcept;
		sqlite3_syscall_ptr forwarding_vfs_get_system_call(sqlite3_vfs* vfs,
														   const char* name) noexcept;
		const char* forwarding_vfs_next_system_call(sqlite3_vfs* vfs, const char* name) noexcept;

		std::mutex forwarding_registration_mutex;
		std::atomic<std::uint64_t> next_forwarding_name{1U};

		class default_connection_observation_port final
			: public sqlite_default_connection_observation_port
		{
		  public:
			default_connection_observation_port(
				std::weak_ptr<default_forwarding_state> owner,
				sqlite_default_forwarding_observation_binding binding)
				: owner_{std::move(owner)}, name_{binding.registered_vfs_name},
				  forwarding_identity_{binding.forwarding_vfs_identity},
				  underlying_identity_{binding.pinned_underlying_vfs_identity},
				  underlying_app_data_identity_{binding.pinned_underlying_vfs_app_data_identity},
				  backend_identity_{binding.backend_lifetime_identity}
			{
			}

			[[nodiscard]] sqlite_default_forwarding_observation_binding
			binding() const noexcept override
			{
				return {
					name_,
					forwarding_identity_,
					underlying_identity_,
					underlying_app_data_identity_,
					backend_identity_,
				};
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_connection_observation(
				std::string_view canonical_vfs_locator,
				const sqlite_backend_opaque_identity& source_capability_token) const override;
			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_ephemeral_connection_observation(
				const sqlite_backend_opaque_identity& source_capability_token) const override;
			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_source_shm_qualification_observation(
				std::string_view scratch_canonical_vfs_locator,
				const sqlite_backend_opaque_identity& source_capability_token) const override;

		  private:
			std::weak_ptr<default_forwarding_state> owner_;
			std::string name_;
			const void* forwarding_identity_{};
			const void* underlying_identity_{};
			const void* underlying_app_data_identity_{};
			const void* backend_identity_{};
		};

		struct open_association
		{
			std::shared_ptr<default_connection_observation> observation;
			sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
			bool observed_role{};
			bool main_handle{};
			bool qualification_fixture{};
			bool rejected{};
		};

		enum class qualification_full_path_result : std::uint8_t
		{
			delegate,
			preserved,
			rejected,
		};

		enum class source_shm_open_validation : std::uint8_t
		{
			generic,
			accepted,
			rejected,
		};

		[[nodiscard]] bool same_identities(const opened_object_identities& left,
										   const opened_object_identities& right) noexcept
		{
			return left.object == right.object && left.entry == right.entry;
		}

		class default_forwarding_state final
			: public sqlite_default_forwarding_vfs,
			  public std::enable_shared_from_this<default_forwarding_state>
		{
		  public:
			static result<std::shared_ptr<default_forwarding_state>>
			create(sqlite_private_snapshot_registry_binding registry)
			{
				if (registry.runtime_identity == nullptr ||
					registry.pinned_default_vfs == nullptr || registry.find == nullptr ||
					registry.register_vfs == nullptr || registry.unregister_vfs == nullptr ||
					!registry.runtime_lifetime)
					return unexpected(forwarding_error("forwarding-vfs-lifetime"));
				auto* underlying = static_cast<sqlite3_vfs*>(registry.pinned_default_vfs);
				if (underlying->version < 1 ||
					underlying->os_file_bytes < static_cast<int>(sizeof(sqlite3_file)) ||
					underlying->maximum_pathname <= 0 ||
					std::cmp_greater(underlying->maximum_pathname, maximum_pathname_bytes) ||
					underlying->name == nullptr || underlying->name[0] == '\0' ||
					underlying->app_data == nullptr || underlying->open == nullptr ||
					underlying->full_pathname == nullptr)
					return unexpected(forwarding_error("forwarding-vfs-delegate"));
				Dl_info underlying_image{};
				if (::dladdr(function_address(underlying->open), &underlying_image) == 0 ||
					underlying_image.dli_fbase == nullptr)
					return unexpected(forwarding_error("forwarding-vfs-delegate-image"));
				const auto file_offset =
					checked_align_up(sizeof(forwarding_file), alignof(std::max_align_t));
				if (!file_offset ||
					*file_offset > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
					static_cast<std::size_t>(underlying->os_file_bytes) >
						static_cast<std::size_t>(std::numeric_limits<int>::max()) - *file_offset)
					return unexpected(forwarding_error("forwarding-vfs-file-size"));

				std::shared_ptr<default_forwarding_state> output;
				try
				{
					output = std::shared_ptr<default_forwarding_state>(
						new default_forwarding_state(std::move(registry),
													 underlying,
													 underlying->app_data,
													 underlying_image.dli_fbase,
													 *file_offset));
					output->initialize_wrapper();
					auto registered = output->register_alias();
					if (!registered)
						return unexpected(std::move(registered.error()));
					output->connection_port_ =
						std::make_shared<default_connection_observation_port>(
							output,
							sqlite_default_forwarding_observation_binding{
								output->registered_name_,
								&output->wrapper_,
								underlying,
								underlying->app_data,
								output.get(),
							});
					return output;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			~default_forwarding_state() override
			{
				if (open_file_count_.load(std::memory_order_acquire) != 0U)
					std::terminate();
				if (!registered_)
					return;
				std::scoped_lock lock{forwarding_registration_mutex};
				if (registry_.find(registered_name_.c_str()) != &wrapper_ ||
					registry_.unregister_vfs(&wrapper_) != sqlite_ok ||
					registry_.find(registered_name_.c_str()) != nullptr)
					std::terminate();
				registered_ = false;
			}

			[[nodiscard]] std::string_view registered_vfs_name() const noexcept override
			{
				return registered_name_;
			}

			[[nodiscard]] const void* vfs_implementation_identity() const noexcept override
			{
				return &wrapper_;
			}

			[[nodiscard]] const void* pinned_underlying_vfs_identity() const noexcept override
			{
				return underlying_;
			}

			[[nodiscard]] const void*
			pinned_underlying_vfs_app_data_identity() const noexcept override
			{
				return underlying_app_data_identity_;
			}

			[[nodiscard]] const void* runtime_identity() const noexcept override
			{
				return registry_.runtime_identity;
			}

			[[nodiscard]] const void* backend_lifetime_identity() const noexcept override
			{
				return this;
			}

			[[nodiscard]] result<std::string>
			canonicalize(const std::string_view raw_path) const override
			{
				if (raw_path.empty() || raw_path == ":memory:" || raw_path.contains('\0') ||
					raw_path.starts_with("file:") || raw_path.contains('?') ||
					raw_path.contains('#'))
					return unexpected(canonicalization_error());
				try
				{
					std::string terminated{raw_path};
					const auto buffer_size =
						static_cast<std::size_t>(underlying_->maximum_pathname) + 1U;
					std::vector<char> buffer(buffer_size, '\0');
					const auto status = underlying_->full_pathname(underlying_,
																   terminated.c_str(),
																   static_cast<int>(buffer.size()),
																   buffer.data());
					if (status != sqlite_ok)
						return unexpected(canonicalization_error());
					const auto end = std::ranges::find(buffer, '\0');
					if (end == buffer.end() || end == buffer.begin() || buffer.front() != '/')
						return unexpected(canonicalization_error());
					return std::string(buffer.begin(), end);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			[[nodiscard]] sqlite3_vfs* underlying() const noexcept
			{
				return underlying_;
			}

			[[nodiscard]] std::size_t file_offset() const noexcept
			{
				return file_offset_;
			}

			[[nodiscard]] const void* underlying_image_identity() const noexcept
			{
				return underlying_image_identity_;
			}

			[[nodiscard]] const void* underlying_open_callback_address() const noexcept
			{
				return underlying_open_callback_address_;
			}

			[[nodiscard]] const sqlite_private_snapshot_registry_binding& registry() const noexcept
			{
				return registry_;
			}

			[[nodiscard]] const std::shared_ptr<const sqlite_default_connection_observation_port>&
			connection_port() const noexcept
			{
				return connection_port_;
			}

			[[nodiscard]] result<void>
			arm_source_shm_readonly_profile(default_connection_observation& observation,
											sqlite_source_shm_qualified_open_plan plan)
			{
				try
				{
					if (!valid_source_shm_runtime_binding(plan.runtime) ||
						!valid_source_shm_open_tuple(plan.canonical_vfs_locator,
													 plan.application_generated_uri,
													 plan.registered_vfs_name,
													 plan.open_flags) ||
						observation.profile == source_shm_qualification_profile ||
						observation.canonical_locator != canonical_locator_ ||
						plan.canonical_vfs_locator != observation.canonical_locator ||
						plan.qualification.profile != source_shm_profile ||
						plan.qualification.filesystem_profile.empty() ||
						plan.qualification.runtime_identity != registry_.runtime_identity ||
						plan.qualification.runtime_image_identity !=
							plan.runtime.runtime_image_identity ||
						plan.qualification.runtime_lifetime_identity !=
							registry_.runtime_lifetime.get() ||
						plan.qualification.forwarding_vfs_identity != &wrapper_ ||
						plan.qualification.pinned_underlying_vfs_identity != underlying_ ||
						plan.qualification.pinned_underlying_vfs_app_data_identity !=
							underlying_->app_data ||
						plan.qualification.backend_lifetime_identity != this ||
						plan.qualification.observation_capability_token !=
							observation.capability_token_value ||
						plan.qualification.parent_namespace_identity.profile.empty() ||
						plan.qualification.parent_namespace_identity.bytes.empty() ||
						plan.qualification.expected_shared_memory_object_identity.profile.empty() ||
						plan.qualification.expected_shared_memory_object_identity.bytes.empty() ||
						plan.qualification.expected_shared_memory_entry_identity.profile.empty() ||
						plan.qualification.expected_shared_memory_entry_identity.bytes.empty() ||
						plan.qualification.target_namespace_epoch_identity.profile.empty() ||
						plan.qualification.target_namespace_epoch_identity.bytes.empty() ||
						!plan.qualification.target_namespace_epoch ||
						plan.qualification.target_namespace_epoch->identity() !=
							plan.qualification.target_namespace_epoch_identity ||
						plan.qualification.target_namespace_epoch->logical_main_locator() !=
							plan.canonical_vfs_locator ||
						plan.qualification.target_namespace_epoch->anchored_main_locator() !=
							plan.delegated_vfs_locator ||
						plan.qualification.sealed_qualification_token.profile.empty() ||
						plan.qualification.sealed_qualification_token.bytes.empty() ||
						!plan.qualification.first_map_nonmutating ||
						!plan.qualification.later_map_nonmutating ||
						!plan.qualification.cantinit_heap_wal_index_route_proven ||
						!plan.qualification.readonly_mapped_wal_index_retry_route_proven)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					const auto* source_id = plan.runtime.source_id();
					if (source_id == nullptr || source_id[0] == '\0' ||
						plan.qualification.sqlite_source_id != source_id)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					if (auto epoch = plan.qualification.target_namespace_epoch->recheck(); !epoch)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					auto current_shared_memory =
						plan.qualification.target_namespace_epoch->retained_entry(
							sqlite_backend_file_role::shared_memory);
					if (!current_shared_memory || !current_shared_memory->object_identity ||
						!current_shared_memory->directory_entry_identity ||
						*current_shared_memory->object_identity !=
							plan.qualification.expected_shared_memory_object_identity ||
						*current_shared_memory->directory_entry_identity !=
							plan.qualification.expected_shared_memory_entry_identity)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || observation.main_claimed ||
						observation.main_handle_open || observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan ||
						observation.source_shm_qualification_fixture_fullpath_plan ||
						observation.source_shm_qualification_fixture_pending_open_plan ||
						observation.source_shm_open_callback_receipt)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					observation.source_shm_open_plan.emplace(std::move(plan));
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			[[nodiscard]] result<void> arm_source_shm_readonly_qualification_candidate(
				default_connection_observation& observation,
				sqlite_source_shm_qualification_open_plan plan)
			{
				try
				{
					if (observation.profile != source_shm_qualification_profile ||
						observation.canonical_locator == canonical_locator_ ||
						plan.canonical_vfs_locator != observation.canonical_locator ||
						!qualification_candidate_locator(plan.canonical_vfs_locator) ||
						plan.filesystem_profile.empty() ||
						!valid_source_shm_runtime_binding(plan.runtime) ||
						!valid_source_shm_open_tuple(plan.canonical_vfs_locator,
													 plan.application_generated_uri,
													 plan.registered_vfs_name,
													 plan.open_flags) ||
						plan.forwarding_vfs_identity != &wrapper_ ||
						plan.pinned_underlying_vfs_identity != underlying_ ||
						plan.pinned_underlying_vfs_app_data_identity != underlying_->app_data ||
						plan.backend_lifetime_identity != this ||
						plan.observation_capability_token != observation.capability_token_value)
						return unexpected(forwarding_error("source-shm-qualification-arm"));
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || observation.main_claimed ||
						observation.main_handle_open || observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan ||
						observation.source_shm_qualification_fixture_fullpath_plan ||
						observation.source_shm_qualification_fixture_pending_open_plan ||
						observation.source_shm_open_callback_receipt)
						return unexpected(forwarding_error("source-shm-qualification-arm"));
					observation.source_shm_qualification_open_plan.emplace(std::move(plan));
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			[[nodiscard]] result<void> arm_source_shm_qualification_fixture_fullpath(
				default_connection_observation& observation,
				sqlite_source_shm_qualification_fixture_fullpath_plan plan)
			{
				try
				{
					if (observation.profile != source_shm_qualification_profile ||
						observation.canonical_locator == canonical_locator_ ||
						plan.canonical_vfs_locator != observation.canonical_locator ||
						!exact_proc_self_fd_locator(plan.canonical_vfs_locator,
													"producer/main.db") ||
						plan.registered_vfs_name != registered_name_ ||
						plan.forwarding_vfs_identity != &wrapper_ ||
						plan.pinned_underlying_vfs_identity != underlying_ ||
						plan.pinned_underlying_vfs_app_data_identity != underlying_->app_data ||
						plan.backend_lifetime_identity != this ||
						plan.observation_capability_token != observation.capability_token_value ||
						observation.effect_gate == nullptr ||
						observation.effect_gate->stage() != sqlite_backend_effect_stage::denied ||
						!observation.effect_gate->enforcement_active())
						return unexpected(
							forwarding_error("source-shm-qualification-fixture-fullpath-arm"));
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || observation.main_claimed ||
						observation.main_handle_open || observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan ||
						observation.source_shm_qualification_fixture_fullpath_plan ||
						observation.source_shm_qualification_fixture_pending_open_plan ||
						observation.source_shm_open_callback_receipt ||
						observation.source_shm_qualification_fullpath_preserved ||
						observation.source_shm_qualification_fixture_main_accepted)
						return unexpected(
							forwarding_error("source-shm-qualification-fixture-fullpath-arm"));
					observation.source_shm_qualification_fixture_fullpath_plan.emplace(
						std::move(plan));
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			[[nodiscard]] qualification_full_path_result
			preserve_qualified_full_path(const char* name, int size, char* output) noexcept;

			[[nodiscard]] result<void> attach_observation(
				std::string canonical_locator,
				std::string profile,
				const std::shared_ptr<sqlite_backend_observation_capability>& capability)
			{
				if (canonical_locator.empty() || profile.empty() || !capability)
					return unexpected(forwarding_error("vfs-observation-binding"));
				const auto binding = capability->binding();
				if (binding.vfs_implementation_identity != &wrapper_ ||
					binding.registered_vfs_name != registered_name_ ||
					binding.backend_lifetime_identity != this ||
					binding.runtime_identity != registry_.runtime_identity ||
					binding.runtime_lifetime_identity != registry_.runtime_lifetime.get())
					return unexpected(forwarding_error("vfs-observation-binding"));
				std::scoped_lock lock{connection_observations_mutex_};
				if (!canonical_locator_.empty() || !observation_capability_.expired())
					return unexpected(forwarding_error("vfs-observation-binding"));
				canonical_locator_ = std::move(canonical_locator);
				observation_profile_ = std::move(profile);
				observation_capability_ = capability;
				return {};
			}

			[[nodiscard]] source_shm_open_validation
			validate_source_shm_open_callback(default_connection_observation& observation,
											  const char* name,
											  const int flags) noexcept
			{
				try
				{
					std::scoped_lock lock{observation.mutex};
					const auto reject = [&]() noexcept
					{
						observation.invalid = true;
						observation.complete = false;
						observation.source_shm_open_rejected = true;
						return source_shm_open_validation::rejected;
					};
					const auto* qualified = observation.source_shm_open_plan
						? &*observation.source_shm_open_plan
						: nullptr;
					const auto* candidate = observation.source_shm_qualification_open_plan
						? &*observation.source_shm_qualification_open_plan
						: nullptr;
					if (observation.invalid)
						return reject();
					if (qualified == nullptr && candidate == nullptr)
						return (flags & sqlite_open_uri) != 0 ? reject()
															  : source_shm_open_validation::generic;
					if ((qualified == nullptr) == (candidate == nullptr) || name == nullptr ||
						observation.source_shm_open_callback_receipt)
						return reject();

					const auto& runtime =
						qualified != nullptr ? qualified->runtime : candidate->runtime;
					const auto& canonical_locator = qualified != nullptr
						? qualified->canonical_vfs_locator
						: candidate->canonical_vfs_locator;
					const auto& delegated_locator = qualified != nullptr
						? qualified->delegated_vfs_locator
						: candidate->canonical_vfs_locator;
					const auto& application_generated_uri = qualified != nullptr
						? qualified->application_generated_uri
						: candidate->application_generated_uri;
					const auto& registered_vfs_name = qualified != nullptr
						? qualified->registered_vfs_name
						: candidate->registered_vfs_name;
					if (flags != source_shm_main_xopen_flags ||
						std::string_view{name} != delegated_locator ||
						runtime.uri_parameter == nullptr || runtime.uri_key == nullptr)
						return reject();

					constexpr std::array expected_keys{"mode", "cache", "readonly_shm"};
					constexpr std::array expected_values{"ro", "private", "1"};
					for (std::size_t index{}; index < expected_keys.size(); ++index)
					{
						const auto* key = runtime.uri_key(name, static_cast<int>(index));
						const auto* value = runtime.uri_parameter(name, expected_keys[index]);
						if (key == nullptr || value == nullptr ||
							std::string_view{key} != expected_keys[index] ||
							std::string_view{value} != expected_values[index])
							return reject();
					}
					if (runtime.uri_key(name, static_cast<int>(expected_keys.size())) != nullptr ||
						runtime.uri_parameter(name, "vfs") != nullptr ||
						runtime.uri_parameter(name, "immutable") != nullptr)
						return reject();

					sqlite_source_shm_open_callback_receipt receipt;
					receipt.profile = qualified != nullptr
						? std::string{source_shm_profile}
						: std::string{source_shm_qualification_profile};
					receipt.connection_token = observation.connection_token_value;
					receipt.qualification_token = qualified != nullptr
						? qualified->qualification.sealed_qualification_token
						: candidate->observation_capability_token;
					receipt.target_namespace_epoch_identity = qualified != nullptr
						? qualified->qualification.target_namespace_epoch_identity
						: sqlite_backend_opaque_identity{};
					receipt.canonical_vfs_locator = canonical_locator;
					receipt.delegated_vfs_locator = delegated_locator;
					receipt.application_generated_uri = application_generated_uri;
					receipt.registered_vfs_name = registered_vfs_name;
					receipt.mode = expected_values[0];
					receipt.cache = expected_values[1];
					receipt.readonly_shm = expected_values[2];
					receipt.input_flags = flags;
					receipt.runtime_identity = runtime.runtime_identity;
					receipt.forwarding_vfs_identity = &wrapper_;
					receipt.pinned_underlying_vfs_identity = underlying_;
					receipt.pinned_underlying_vfs_app_data_identity = underlying_->app_data;
					observation.source_shm_open_callback_receipt.emplace(std::move(receipt));
					return source_shm_open_validation::accepted;
				}
				catch (...)
				{
					std::scoped_lock lock{observation.mutex};
					observation.invalid = true;
					observation.complete = false;
					return source_shm_open_validation::rejected;
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_connection_observation(const std::string_view canonical_locator,
										 const sqlite_backend_opaque_identity& capability_token)
			{
				try
				{
					auto capability = observation_capability_.lock();
					if (!capability || capability->capability_token() != capability_token)
						return unexpected(forwarding_error("vfs-observation-binding"));
					auto observation = std::make_shared<default_connection_observation>();
					observation->owner = shared_from_this();
					observation->capability_token_value = capability_token;
					observation->canonical_locator = std::string{canonical_locator};
					observation->originating_thread = std::this_thread::get_id();
					observation->open_events.reserve(maximum_open_observations);
					observation->held_shm_locks.reserve(maximum_shm_lock_observations);
					observation->shm_map_events.reserve(maximum_shm_map_observations);
					observation->profile = observation_profile_;
					if (observation->profile == ephemeral_profile)
					{
						// SQLite's dedicated in-memory pager intentionally bypasses VFS xOpen. The
						// profile plus an empty event set is the complete typed not-applicable
						// receipt; the caller separately owns the successful sqlite3_open_v2
						// handle.
						observation->main_proven = true;
						observation->complete = true;
					}
					observation->connection_token_value.profile = forwarding_profile.data();
					append_pointer(observation->connection_token_value.bytes, this);
					append_pointer(observation->connection_token_value.bytes, capability.get());
					append_u64(
						observation->connection_token_value.bytes,
						next_connection_observation_.fetch_add(1U, std::memory_order_relaxed));
					append_bytes(observation->connection_token_value.bytes, canonical_locator);
					observation->effect_gate = std::make_unique<sqlite_backend_effect_gate_state>(
						*observation,
						observation->capability_token_value,
						observation->connection_token_value,
						observation->canonical_locator,
						std::string{forwarding_profile},
						observation->profile != ephemeral_profile);

					std::scoped_lock owner_lock{connection_observations_mutex_};
					if (canonical_locator != canonical_locator_ || observation_profile_.empty())
						return unexpected(forwarding_error("vfs-observation-binding"));
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto existing = iterator->lock();
						if (!existing)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						std::scoped_lock observation_lock{existing->mutex};
						if (existing->canonical_locator == canonical_locator &&
							existing->originating_thread == observation->originating_thread &&
							(!existing->main_claimed || existing->main_handle_open))
							return unexpected(forwarding_error("vfs-observation-overlap"));
						++iterator;
					}
					connection_observations_.emplace_back(observation);
					return std::static_pointer_cast<sqlite_backend_connection_observation_scope>(
						std::move(observation));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_ephemeral_connection_observation(
				const sqlite_backend_opaque_identity& capability_token)
			{
				try
				{
					auto capability = observation_capability_.lock();
					if (!capability || capability->capability_token() != capability_token)
						return unexpected(forwarding_error("vfs-observation-binding"));
					auto observation = std::make_shared<default_connection_observation>();
					observation->owner = shared_from_this();
					observation->capability_token_value = capability_token;
					observation->canonical_locator = ":memory:";
					observation->originating_thread = std::this_thread::get_id();
					observation->open_events.reserve(maximum_open_observations);
					observation->held_shm_locks.reserve(maximum_shm_lock_observations);
					observation->shm_map_events.reserve(maximum_shm_map_observations);
					observation->profile = ephemeral_profile;
					observation->main_proven = true;
					observation->complete = true;
					observation->connection_token_value.profile = forwarding_profile.data();
					append_pointer(observation->connection_token_value.bytes, this);
					append_pointer(observation->connection_token_value.bytes, capability.get());
					append_u64(
						observation->connection_token_value.bytes,
						next_connection_observation_.fetch_add(1U, std::memory_order_relaxed));
					append_bytes(observation->connection_token_value.bytes, ":memory:");
					observation->effect_gate = std::make_unique<sqlite_backend_effect_gate_state>(
						*observation,
						observation->capability_token_value,
						observation->connection_token_value,
						observation->canonical_locator,
						std::string{forwarding_profile},
						false);
					return std::static_pointer_cast<sqlite_backend_connection_observation_scope>(
						std::move(observation));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("vfs-observation-allocation"));
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_source_shm_qualification_observation(
				const std::string_view scratch_canonical_vfs_locator,
				const sqlite_backend_opaque_identity& capability_token)
			{
				try
				{
					auto capability = observation_capability_.lock();
					if (!capability || capability->capability_token() != capability_token ||
						scratch_canonical_vfs_locator.empty() ||
						scratch_canonical_vfs_locator.front() != '/' ||
						scratch_canonical_vfs_locator.contains('\0') ||
						scratch_canonical_vfs_locator == canonical_locator_)
						return unexpected(
							forwarding_error("source-shm-qualification-observation-binding"));
					auto observation = std::make_shared<default_connection_observation>();
					observation->owner = shared_from_this();
					observation->capability_token_value = capability_token;
					observation->canonical_locator = std::string{scratch_canonical_vfs_locator};
					observation->originating_thread = std::this_thread::get_id();
					observation->open_events.reserve(maximum_open_observations);
					observation->held_shm_locks.reserve(maximum_shm_lock_observations);
					observation->shm_map_events.reserve(maximum_shm_map_observations);
					observation->profile = source_shm_qualification_profile;
					observation->connection_token_value.profile = forwarding_profile.data();
					append_pointer(observation->connection_token_value.bytes, this);
					append_pointer(observation->connection_token_value.bytes, capability.get());
					append_u64(
						observation->connection_token_value.bytes,
						next_connection_observation_.fetch_add(1U, std::memory_order_relaxed));
					append_bytes(observation->connection_token_value.bytes,
								 scratch_canonical_vfs_locator);
					observation->effect_gate = std::make_unique<sqlite_backend_effect_gate_state>(
						*observation,
						observation->capability_token_value,
						observation->connection_token_value,
						observation->canonical_locator,
						std::string{forwarding_profile},
						true);

					std::scoped_lock owner_lock{connection_observations_mutex_};
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto existing = iterator->lock();
						if (!existing)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						std::scoped_lock observation_lock{existing->mutex};
						if (existing->originating_thread == observation->originating_thread &&
							(!existing->main_claimed || existing->main_handle_open))
							return unexpected(
								forwarding_error("source-shm-qualification-observation-overlap"));
						++iterator;
					}
					connection_observations_.emplace_back(observation);
					return std::static_pointer_cast<sqlite_backend_connection_observation_scope>(
						std::move(observation));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
				catch (const std::length_error&)
				{
					return unexpected(forwarding_error("forwarding-vfs-allocation"));
				}
			}

			[[nodiscard]] open_association associate_open(const char* name,
														  const int flags) noexcept
			{
				const auto type = flags & sqlite_open_file_type_mask;
				const auto path = name == nullptr ? std::string_view{} : std::string_view{name};
				std::scoped_lock owner_lock{connection_observations_mutex_};
				const auto ephemeral_main = canonical_locator_ == ":memory:" &&
					type == sqlite_open_main_database && (flags & sqlite_open_memory) != 0 &&
					(name == nullptr || path == canonical_locator_);
				if (ephemeral_main)
					return claim_main_locked(path);
				if (canonical_locator_ != ":memory:" && type == sqlite_open_main_database)
					return claim_main_locked(path);
				if (name != nullptr && type == sqlite_open_main_journal)
					return associate_sidecar_locked(
						path, sqlite_backend_file_role::rollback_journal, journal_suffix);
				if (name != nullptr && type == sqlite_open_write_ahead_log)
					return associate_sidecar_locked(
						path, sqlite_backend_file_role::write_ahead_log, wal_suffix);
				return {};
			}

			[[nodiscard]] std::optional<opened_object_identities>
			observe_stable_existing_entry(const sqlite_backend_file_role role) const noexcept
			{
				if (canonical_locator_ == ":memory:")
					return std::nullopt;
				try
				{
					auto capability = observation_capability_.lock();
					if (!capability)
						return std::nullopt;
					auto before = observe_sqlite_default_entry_identity_without_open(
						*capability, canonical_locator_, role);
					if (!before)
						return std::nullopt;
					auto after = observe_sqlite_default_entry_identity_without_open(
						*capability, canonical_locator_, role);
					if (!after || before->role != after->role ||
						before->object_identity != after->object_identity ||
						before->directory_entry_identity != after->directory_entry_identity)
						return std::nullopt;
					return opened_object_identities{std::move(after->object_identity),
													std::move(after->directory_entry_identity)};
				}
				catch (...)
				{
					return std::nullopt;
				}
			}

			[[nodiscard]] std::optional<opened_object_identities>
			observe_opened_object(forwarding_file& file) const noexcept
			{
				if (!file.observed_role || canonical_locator_ == ":memory:")
					return std::nullopt;
				try
				{
					if (file.connection_observation)
					{
						std::scoped_lock lock{file.connection_observation->mutex};
						if (file.connection_observation->profile ==
							source_shm_qualification_profile)
							return std::nullopt;
					}
					auto capability = observation_capability_.lock();
					auto* raw = underlying_file(file);
					const auto* methods = underlying_methods(file);
					if (!capability || raw == nullptr || methods == nullptr ||
						methods->file_control == nullptr)
						return std::nullopt;
					auto before = observe_sqlite_default_entry_identity_without_open(
						*capability, canonical_locator_, file.role);
					if (!before)
						return std::nullopt;
					int moved = 1;
					if (methods->file_control(raw, sqlite_file_control_has_moved, &moved) !=
							sqlite_ok ||
						moved != 0)
						return std::nullopt;
					auto after = observe_sqlite_default_entry_identity_without_open(
						*capability, canonical_locator_, file.role);
					if (!after || before->role != after->role ||
						before->object_identity != after->object_identity ||
						before->directory_entry_identity != after->directory_entry_identity)
						return std::nullopt;
					return opened_object_identities{std::move(after->object_identity),
													std::move(after->directory_entry_identity)};
				}
				catch (...)
				{
					return std::nullopt;
				}
			}

			[[nodiscard]] std::optional<opened_object_identities>
			observe_shared_memory() const noexcept
			{
				return observe_stable_existing_entry(sqlite_backend_file_role::shared_memory);
			}

			[[nodiscard]] bool permits_path_effect(const char* name) noexcept
			{
				try
				{
					if (name == nullptr)
						return true;
					const std::string_view path{name};
					std::scoped_lock owner_lock{connection_observations_mutex_};
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto observation = iterator->lock();
						if (!observation)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						++iterator;
						bool claims_namespace{};
						bool matches_namespace{};
						bool writable_fixture{};
						{
							std::scoped_lock observation_lock{observation->mutex};
							claims_namespace = observation->source_shm_open_plan.has_value() ||
								observation->source_shm_qualification_fixture_main_accepted ||
								!observation->main_claimed || observation->main_handle_open;
							matches_namespace =
								exact_sqlite_family_path(path, observation->canonical_locator);
							if (observation->source_shm_open_plan)
								matches_namespace =
									matches_namespace ||
									exact_sqlite_family_path(
										path,
										observation->source_shm_open_plan->delegated_vfs_locator);
							writable_fixture =
								observation->source_shm_qualification_fixture_main_accepted &&
								observation->main_handle_open;
						}
						if (!claims_namespace || !matches_namespace)
							continue;
						if (writable_fixture)
							return true;
						if (!observation->permits_persistent_effect(false))
							return false;
					}
					return true;
				}
				catch (...)
				{
					return false;
				}
			}

			[[nodiscard]] bool denies_logical_source_access(const char* name) noexcept
			{
				if (name == nullptr)
					return false;
				try
				{
					const std::string_view path{name};
					std::scoped_lock owner_lock{connection_observations_mutex_};
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto observation = iterator->lock();
						if (!observation)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						++iterator;
						std::scoped_lock observation_lock{observation->mutex};
						if (observation->source_shm_open_plan)
						{
							if (exact_sqlite_family_path(path, observation->canonical_locator))
								return true;
							if (!observation->main_handle_open &&
								exact_sqlite_family_path(
									path, observation->source_shm_open_plan->delegated_vfs_locator))
								return true;
						}
					}
					return false;
				}
				catch (...)
				{
					return true;
				}
			}

			void increment_open_file_count() noexcept
			{
				open_file_count_.fetch_add(1U, std::memory_order_acq_rel);
			}

			void decrement_open_file_count() noexcept
			{
				open_file_count_.fetch_sub(1U, std::memory_order_acq_rel);
			}

		  private:
			[[nodiscard]] bool valid_source_shm_runtime_binding(
				const sqlite_source_shm_runtime_binding& runtime) const noexcept
			{
				return runtime.runtime_identity == registry_.runtime_identity &&
					runtime.runtime_image_identity != nullptr &&
					runtime.runtime_lifetime_identity == registry_.runtime_lifetime.get() &&
					runtime.runtime_lifetime &&
					runtime.runtime_lifetime.get() == registry_.runtime_lifetime.get() &&
					runtime.open_v2 != nullptr && runtime.close_v2 != nullptr &&
					runtime.exec != nullptr && runtime.errmsg != nullptr &&
					runtime.free_memory != nullptr && runtime.source_id != nullptr &&
					runtime.uri_parameter != nullptr && runtime.uri_key != nullptr &&
					runtime.vfs_find == registry_.find &&
					runtime.vfs_register == registry_.register_vfs &&
					runtime.vfs_unregister == registry_.unregister_vfs &&
					runtime.vfs_find(registered_name_.c_str()) == &wrapper_;
			}

			[[nodiscard]] bool
			valid_source_shm_open_tuple(const std::string_view canonical_locator,
										const std::string_view application_generated_uri,
										const std::string_view registered_vfs_name,
										const int open_flags) const
			{
				return !canonical_locator.empty() && canonical_locator.front() == '/' &&
					!canonical_locator.contains('\0') && registered_vfs_name == registered_name_ &&
					open_flags == source_shm_open_flags &&
					application_generated_uri == strict_source_shm_uri(canonical_locator);
			}

			default_forwarding_state(sqlite_private_snapshot_registry_binding registry,
									 sqlite3_vfs* underlying,
									 const void* underlying_app_data_identity,
									 const void* underlying_image_identity,
									 const std::size_t file_offset)
				: registry_{std::move(registry)}, underlying_{underlying},
				  underlying_app_data_identity_{underlying_app_data_identity},
				  underlying_image_identity_{underlying_image_identity},
				  underlying_open_callback_address_{function_address(underlying->open)},
				  file_offset_{file_offset}
			{
			}

			void initialize_wrapper() noexcept
			{
				const auto version = std::min(underlying_->version, 3);
				wrapper_ = sqlite3_vfs{
					version,
					static_cast<int>(file_offset_ +
									 static_cast<std::size_t>(underlying_->os_file_bytes)),
					underlying_->maximum_pathname,
					nullptr,
					nullptr,
					this,
					forwarding_vfs_open,
					underlying_->remove != nullptr ? forwarding_vfs_remove : nullptr,
					underlying_->access != nullptr ? forwarding_vfs_access : nullptr,
					forwarding_vfs_full_pathname,
					underlying_->dl_open != nullptr ? forwarding_vfs_dl_open : nullptr,
					underlying_->dl_error != nullptr ? forwarding_vfs_dl_error : nullptr,
					underlying_->dl_sym != nullptr ? forwarding_vfs_dl_sym : nullptr,
					underlying_->dl_close != nullptr ? forwarding_vfs_dl_close : nullptr,
					underlying_->randomness != nullptr ? forwarding_vfs_randomness : nullptr,
					underlying_->sleep != nullptr ? forwarding_vfs_sleep : nullptr,
					underlying_->current_time != nullptr ? forwarding_vfs_current_time : nullptr,
					underlying_->get_last_error != nullptr ? forwarding_vfs_last_error : nullptr,
					version >= 2 && underlying_->current_time_int64 != nullptr
						? forwarding_vfs_current_time_int64
						: nullptr,
					version >= 3 && underlying_->set_system_call != nullptr
						? forwarding_vfs_set_system_call
						: nullptr,
					version >= 3 && underlying_->get_system_call != nullptr
						? forwarding_vfs_get_system_call
						: nullptr,
					version >= 3 && underlying_->next_system_call != nullptr
						? forwarding_vfs_next_system_call
						: nullptr,
				};
			}

			[[nodiscard]] result<void> register_alias()
			{
				std::scoped_lock lock{forwarding_registration_mutex};
				for (std::size_t attempt{}; attempt < 32U; ++attempt)
				{
					const auto value =
						next_forwarding_name.fetch_add(1U, std::memory_order_relaxed);
					registered_name_ = "cxxlens-default-forwarding-v1-" +
						std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
						std::to_string(value);
					wrapper_.name = registered_name_.c_str();
					if (registry_.find(registered_name_.c_str()) != nullptr)
						continue;
					if (registry_.register_vfs(&wrapper_, 0) != sqlite_ok)
						return unexpected(forwarding_error("forwarding-vfs-register"));
					if (registry_.find(registered_name_.c_str()) != &wrapper_)
					{
						if (registry_.unregister_vfs(&wrapper_) != sqlite_ok ||
							registry_.find(registered_name_.c_str()) != nullptr)
							std::terminate();
						return unexpected(forwarding_error("forwarding-vfs-register"));
					}
					registered_ = true;
					return {};
				}
				return unexpected(forwarding_error("forwarding-vfs-name-collision"));
			}

			[[nodiscard]] open_association claim_main_locked(const std::string_view path) noexcept
			{
				std::vector<std::shared_ptr<default_connection_observation>> candidates;
				std::vector<std::shared_ptr<default_connection_observation>> guarded_candidates;
				std::vector<std::shared_ptr<default_connection_observation>> thread_candidates;
				try
				{
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto observation = iterator->lock();
						if (!observation)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						std::scoped_lock observation_lock{observation->mutex};
						const auto source_armed = observation->source_shm_open_plan ||
							observation->source_shm_qualification_open_plan ||
							observation->source_shm_qualification_fixture_fullpath_plan ||
							observation->source_shm_qualification_fixture_pending_open_plan;
						std::string_view expected_path = observation->canonical_locator;
						bool source_ready = true;
						if (observation->source_shm_open_plan)
						{
							expected_path =
								observation->source_shm_open_plan->delegated_vfs_locator;
							source_ready = observation->source_shm_target_fullpath_projected;
						}
						else if (observation->source_shm_qualification_open_plan)
							source_ready = observation->source_shm_qualification_fullpath_preserved;
						else if (observation->source_shm_qualification_fixture_fullpath_plan)
							source_ready = false;
						else if (observation->source_shm_qualification_fixture_pending_open_plan)
							source_ready = observation->source_shm_qualification_fullpath_preserved;
						const auto consumed_fixture_retry =
							observation->source_shm_qualification_fixture_main_accepted &&
							expected_path == path;
						const auto guarded = source_armed || consumed_fixture_retry ||
							observation->source_shm_open_rejected ||
							observation->source_shm_open_callback_receipt.has_value();
						if (guarded &&
							observation->originating_thread != std::this_thread::get_id() &&
							(path == expected_path || path == observation->canonical_locator))
						{
							observation->invalid = true;
							observation->complete = false;
							observation->source_shm_open_rejected = true;
							return {nullptr,
									sqlite_backend_file_role::main_database,
									true,
									true,
									false,
									true};
						}
						if (observation->originating_thread == std::this_thread::get_id() &&
							(!observation->main_claimed || guarded))
						{
							thread_candidates.push_back(observation);
							if (!observation->main_claimed && source_ready && expected_path == path)
								candidates.push_back(observation);
							if (guarded)
								guarded_candidates.push_back(observation);
						}
						++iterator;
					}
					if (thread_candidates.size() > 1U)
					{
						for (const auto& candidate : thread_candidates)
						{
							std::scoped_lock lock{candidate->mutex};
							candidate->invalid = true;
							candidate->complete = false;
						}
						return {nullptr,
								sqlite_backend_file_role::main_database,
								true,
								true,
								false,
								true};
					}
					if (candidates.size() == 1U)
					{
						std::scoped_lock lock{candidates.front()->mutex};
						const auto fixture =
							candidates.front()
								->source_shm_qualification_fixture_pending_open_plan.has_value();
						if (fixture)
						{
							candidates.front()
								->source_shm_qualification_fixture_pending_open_plan.reset();
							candidates.front()->source_shm_qualification_fixture_main_accepted =
								true;
						}
						candidates.front()->main_claimed = true;
						return {candidates.front(),
								sqlite_backend_file_role::main_database,
								!fixture,
								true,
								fixture};
					}
					for (const auto& candidate : guarded_candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
						candidate->source_shm_open_rejected = true;
					}
				}
				catch (...)
				{
					for (const auto& candidate : guarded_candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
					}
				}
				if (path == canonical_locator_ || !guarded_candidates.empty())
					return {
						nullptr, sqlite_backend_file_role::main_database, true, true, false, true};
				return {};
			}

			[[nodiscard]] open_association
			associate_sidecar_locked(const std::string_view path,
									 const sqlite_backend_file_role role,
									 const std::string_view suffix) noexcept
			{
				std::vector<std::shared_ptr<default_connection_observation>> candidates;
				std::vector<std::shared_ptr<default_connection_observation>> same_thread;
				std::vector<std::shared_ptr<default_connection_observation>> guarded_candidates;
				try
				{
					for (auto iterator = connection_observations_.begin();
						 iterator != connection_observations_.end();)
					{
						auto observation = iterator->lock();
						if (!observation)
						{
							iterator = connection_observations_.erase(iterator);
							continue;
						}
						std::scoped_lock observation_lock{observation->mutex};
						std::string_view family_locator = observation->canonical_locator;
						const auto qualified = observation->source_shm_open_plan.has_value();
						if (qualified)
							family_locator =
								observation->source_shm_open_plan->delegated_vfs_locator;
						const auto guarded_match = qualified &&
							(exact_suffix_path(path, observation->canonical_locator, suffix) ||
							 exact_suffix_path(path, family_locator, suffix));
						if (observation->main_handle_open &&
							exact_suffix_path(path, family_locator, suffix))
						{
							candidates.push_back(observation);
							if (observation->originating_thread == std::this_thread::get_id())
								same_thread.push_back(observation);
						}
						else if (guarded_match)
							guarded_candidates.push_back(observation);
						++iterator;
					}
					if (same_thread.size() == 1U)
					{
						std::scoped_lock lock{same_thread.front()->mutex};
						const auto fixture =
							same_thread.front()->source_shm_qualification_fixture_main_accepted;
						return {same_thread.front(), role, !fixture, false, fixture};
					}
					if (same_thread.empty() && candidates.size() == 1U)
					{
						std::scoped_lock lock{candidates.front()->mutex};
						const auto fixture =
							candidates.front()->source_shm_qualification_fixture_main_accepted;
						return {candidates.front(), role, !fixture, false, fixture};
					}
					for (const auto& candidate : candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
					}
					for (const auto& candidate : guarded_candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
						candidate->source_shm_open_rejected = true;
					}
				}
				catch (...)
				{
					for (const auto& candidate : candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
					}
					for (const auto& candidate : guarded_candidates)
					{
						std::scoped_lock lock{candidate->mutex};
						candidate->invalid = true;
						candidate->complete = false;
					}
				}
				if (exact_suffix_path(path, canonical_locator_, suffix) ||
					!guarded_candidates.empty())
					return {nullptr, role, true, false, false, true};
				return {};
			}

			sqlite_private_snapshot_registry_binding registry_;
			sqlite3_vfs* underlying_{};
			const void* underlying_app_data_identity_{};
			const void* underlying_image_identity_{};
			const void* underlying_open_callback_address_{};
			std::size_t file_offset_{};
			sqlite3_vfs wrapper_{};
			std::string registered_name_;
			std::shared_ptr<const sqlite_default_connection_observation_port> connection_port_;
			std::mutex connection_observations_mutex_;
			std::vector<std::weak_ptr<default_connection_observation>> connection_observations_;
			std::weak_ptr<sqlite_backend_observation_capability> observation_capability_;
			std::string canonical_locator_;
			std::string observation_profile_;
			std::atomic<std::uint64_t> next_connection_observation_{1U};
			std::atomic<std::size_t> open_file_count_;
			bool registered_{};
		};

		qualification_full_path_result default_forwarding_state::preserve_qualified_full_path(
			const char* name, const int size, char* output) noexcept
		{
			if (name == nullptr || output == nullptr || size <= 0)
				return qualification_full_path_result::rejected;
			try
			{
				enum class selected_kind : std::uint8_t
				{
					candidate,
					fixture,
					target,
				};
				const std::string_view input{name};
				std::shared_ptr<default_connection_observation> selected;
				selected_kind kind{selected_kind::candidate};
				std::scoped_lock owner_lock{connection_observations_mutex_};
				for (auto iterator = connection_observations_.begin();
					 iterator != connection_observations_.end();)
				{
					auto observation = iterator->lock();
					if (!observation)
					{
						iterator = connection_observations_.erase(iterator);
						continue;
					}
					++iterator;
					std::scoped_lock lock{observation->mutex};
					const auto candidate =
						observation->source_shm_qualification_open_plan.has_value();
					const auto fixture =
						observation->source_shm_qualification_fixture_fullpath_plan.has_value();
					const auto fixture_pending =
						observation->source_shm_qualification_fixture_pending_open_plan.has_value();
					const auto target = observation->source_shm_open_plan.has_value();
					const auto exact_input = input == observation->canonical_locator;
					const auto armed = !observation->main_claimed &&
						(candidate || fixture || fixture_pending || target);
					const auto replay = exact_input &&
						(observation->source_shm_qualification_fullpath_preserved ||
						 observation->source_shm_target_fullpath_projected);
					const auto reject = [&]() noexcept
					{
						observation->invalid = true;
						observation->complete = false;
						observation->source_shm_open_rejected = true;
						return qualification_full_path_result::rejected;
					};
					if (target && observation->main_claimed &&
						exact_sqlite_family_path(input, observation->canonical_locator))
						return reject();
					if (observation->originating_thread != std::this_thread::get_id())
					{
						if ((armed || replay) && exact_input)
							return reject();
						continue;
					}
					if (!armed && !replay)
						continue;
					if (observation->invalid || replay || !exact_input || fixture_pending ||
						selected)
						return reject();
					selected = observation;
					kind = target ? selected_kind::target
						: fixture ? selected_kind::fixture
								  : selected_kind::candidate;
				}
				if (!selected)
					return qualification_full_path_result::delegate;

				std::scoped_lock lock{selected->mutex};
				std::string_view preserved;
				if (kind == selected_kind::target)
				{
					auto& plan = *selected->source_shm_open_plan;
					if (!plan.qualification.target_namespace_epoch ||
						!plan.qualification.target_namespace_epoch->recheck())
					{
						selected->invalid = true;
						selected->complete = false;
						selected->source_shm_open_rejected = true;
						return qualification_full_path_result::rejected;
					}
					preserved = plan.delegated_vfs_locator;
					selected->source_shm_target_fullpath_projected = true;
				}
				else
				{
					preserved = selected->canonical_locator;
					selected->source_shm_qualification_fullpath_preserved = true;
					if (kind == selected_kind::fixture)
					{
						selected->source_shm_qualification_fixture_pending_open_plan =
							std::move(selected->source_shm_qualification_fixture_fullpath_plan);
						selected->source_shm_qualification_fixture_fullpath_plan.reset();
					}
				}
				if (preserved.size() + 1U > static_cast<std::size_t>(size))
				{
					selected->invalid = true;
					selected->complete = false;
					selected->source_shm_open_rejected = true;
					return qualification_full_path_result::rejected;
				}
				std::memcpy(output, preserved.data(), preserved.size());
				output[preserved.size()] = '\0';
				return qualification_full_path_result::preserved;
			}
			catch (...)
			{
				return qualification_full_path_result::rejected;
			}
		}

		result<void> default_connection_observation::arm_source_shm_readonly_profile(
			sqlite_source_shm_qualified_open_plan plan)
		{
			auto retained_owner = owner.lock();
			if (!retained_owner)
				return unexpected(forwarding_error("source-shm-readonly-arm"));
			return retained_owner->arm_source_shm_readonly_profile(*this, std::move(plan));
		}

		result<void>
		default_connection_observation::arm_source_shm_readonly_qualification_candidate(
			sqlite_source_shm_qualification_open_plan plan)
		{
			auto retained_owner = owner.lock();
			if (!retained_owner)
				return unexpected(forwarding_error("source-shm-qualification-arm"));
			return retained_owner->arm_source_shm_readonly_qualification_candidate(*this,
																				   std::move(plan));
		}

		result<void> default_connection_observation::arm_source_shm_qualification_fixture_fullpath(
			sqlite_source_shm_qualification_fixture_fullpath_plan plan)
		{
			auto retained_owner = owner.lock();
			if (!retained_owner)
				return unexpected(
					forwarding_error("source-shm-qualification-fixture-fullpath-arm"));
			return retained_owner->arm_source_shm_qualification_fixture_fullpath(*this,
																				 std::move(plan));
		}

		result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		default_connection_observation_port::begin_connection_observation(
			const std::string_view canonical_vfs_locator,
			const sqlite_backend_opaque_identity& source_capability_token) const
		{
			auto owner = owner_.lock();
			if (!owner)
				return unexpected(forwarding_error("vfs-observation"));
			return owner->begin_connection_observation(canonical_vfs_locator,
													   source_capability_token);
		}

		result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		default_connection_observation_port::begin_ephemeral_connection_observation(
			const sqlite_backend_opaque_identity& source_capability_token) const
		{
			auto owner = owner_.lock();
			if (!owner)
				return unexpected(forwarding_error("vfs-observation"));
			return owner->begin_ephemeral_connection_observation(source_capability_token);
		}

		result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
		default_connection_observation_port::begin_source_shm_qualification_observation(
			const std::string_view scratch_canonical_vfs_locator,
			const sqlite_backend_opaque_identity& source_capability_token) const
		{
			auto owner = owner_.lock();
			if (!owner)
				return unexpected(forwarding_error("vfs-observation"));
			return owner->begin_source_shm_qualification_observation(scratch_canonical_vfs_locator,
																	 source_capability_token);
		}

		[[nodiscard]] sqlite3_file* underlying_file(forwarding_file& value) noexcept
		{
			return value.native ? value.native->file() : nullptr;
		}

		[[nodiscard]] const sqlite3_io_methods* underlying_methods(forwarding_file& value) noexcept
		{
			return value.native && value.native->trusted_methods_ready
				? &value.native->trusted_methods
				: nullptr;
		}

		struct native_method_inspection
		{
			int (*trusted_close)(sqlite3_file*){};
			const sqlite3_io_methods* forwarding_methods{};
			sqlite3_io_methods callbacks{};
		};

		[[nodiscard]] native_method_inspection
		inspect_native_methods(const native_file_node& node) noexcept
		{
			native_method_inspection output{};
			const auto* raw = const_cast<native_file_node&>(node).file();
			const auto* methods = raw != nullptr ? raw->methods : nullptr;
			if (!readable_range_bound_to_code(methods,
											  sizeof(int),
											  alignof(sqlite3_io_methods),
											  node.underlying_open_callback_address,
											  node.underlying_image_identity))
				return output;
			const auto advertised_version = methods->version;
			constexpr auto version_one_bytes = offsetof(sqlite3_io_methods, shm_map);
			constexpr auto version_two_bytes = offsetof(sqlite3_io_methods, fetch);
			const auto known_prefix_bytes = advertised_version >= 3 ? sizeof(sqlite3_io_methods)
				: advertised_version >= 2							? version_two_bytes
																	: version_one_bytes;
			if (!readable_range_bound_to_code(methods,
											  known_prefix_bytes,
											  alignof(sqlite3_io_methods),
											  node.underlying_open_callback_address,
											  node.underlying_image_identity))
				return output;
			if (function_from_image(methods->close, node.underlying_image_identity))
				output.trusted_close = methods->close;
			if (advertised_version < 1 || output.trusted_close == nullptr ||
				!function_from_image(methods->read, node.underlying_image_identity) ||
				!function_from_image(methods->write, node.underlying_image_identity) ||
				!function_from_image(methods->truncate, node.underlying_image_identity) ||
				!function_from_image(methods->sync, node.underlying_image_identity) ||
				!function_from_image(methods->file_size, node.underlying_image_identity) ||
				!function_from_image(methods->lock, node.underlying_image_identity) ||
				!function_from_image(methods->unlock, node.underlying_image_identity) ||
				!function_from_image(methods->check_reserved_lock,
									 node.underlying_image_identity) ||
				!function_from_image(methods->file_control, node.underlying_image_identity) ||
				!function_from_image(methods->sector_size, node.underlying_image_identity) ||
				!function_from_image(methods->device_characteristics,
									 node.underlying_image_identity))
				return output;

			output.callbacks = sqlite3_io_methods{
				1,
				methods->close,
				methods->read,
				methods->write,
				methods->truncate,
				methods->sync,
				methods->file_size,
				methods->lock,
				methods->unlock,
				methods->check_reserved_lock,
				methods->file_control,
				methods->sector_size,
				methods->device_characteristics,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
				nullptr,
			};
			output.forwarding_methods = &forwarding_io_v1;
			bool trusted_shm_group{};
			if (advertised_version >= 2)
			{
				if ((methods->shm_lock != nullptr &&
					 !function_from_image(methods->shm_lock, node.underlying_image_identity)) ||
					(methods->shm_barrier != nullptr &&
					 !function_from_image(methods->shm_barrier, node.underlying_image_identity)) ||
					(methods->shm_unmap != nullptr &&
					 !function_from_image(methods->shm_unmap, node.underlying_image_identity)))
				{
					output.forwarding_methods = nullptr;
					return output;
				}
				if (methods->shm_map != nullptr)
				{
					if (methods->shm_lock == nullptr || methods->shm_barrier == nullptr ||
						methods->shm_unmap == nullptr ||
						!function_from_image(methods->shm_map, node.underlying_image_identity))
					{
						output.forwarding_methods = nullptr;
						return output;
					}
					trusted_shm_group = true;
					output.callbacks.version = 2;
					output.callbacks.shm_map = methods->shm_map;
					output.callbacks.shm_lock = methods->shm_lock;
					output.callbacks.shm_barrier = methods->shm_barrier;
					output.callbacks.shm_unmap = methods->shm_unmap;
					output.forwarding_methods = &forwarding_io_v2;
				}
			}
			if (advertised_version >= 3)
			{
				if (methods->unfetch != nullptr &&
					!function_from_image(methods->unfetch, node.underlying_image_identity))
				{
					output.forwarding_methods = nullptr;
					return output;
				}
				if (methods->fetch != nullptr &&
					(methods->unfetch == nullptr ||
					 !function_from_image(methods->fetch, node.underlying_image_identity)))
				{
					output.forwarding_methods = nullptr;
					return output;
				}
				if (methods->fetch != nullptr)
				{
					output.callbacks.fetch = methods->fetch;
					output.callbacks.unfetch = methods->unfetch;
				}
				if (trusted_shm_group && methods->fetch != nullptr)
				{
					output.callbacks.version = 3;
					output.forwarding_methods = &forwarding_io_v3;
				}
			}
			return output;
		}

		[[nodiscard]] int close_native_file(std::shared_ptr<native_file_node>& node,
											int (*close_callback)(sqlite3_file*)) noexcept
		{
			if (!node || close_callback == nullptr || node->close_attempted)
			{
				quarantine_native_file(node);
				return sqlite_io_error;
			}
			node->close_attempted = true;
			try
			{
				const auto status = close_callback(node->file());
				if (status == sqlite_ok)
				{
					release_known_safe_native_file(node);
					return sqlite_ok;
				}
				quarantine_native_file(node);
				return status;
			}
			catch (...)
			{
				// A throwing callback has an unknown cleanup outcome and cannot be retried.
			}
			quarantine_native_file(node);
			return sqlite_io_error;
		}

		void cleanup_failed_forwarding_open(forwarding_file& file) noexcept
		{
			file.base.methods = nullptr;
			if (file.native)
				(void)close_native_file(file.native, file.native->trusted_close);
		}

		[[nodiscard]] std::optional<std::size_t>
		record_open_attempt(const std::shared_ptr<default_connection_observation>& observation,
							const sqlite_backend_file_role role,
							const int flags) noexcept
		{
			if (!observation)
				return std::nullopt;
			try
			{
				std::scoped_lock lock{observation->mutex};
				if (observation->open_events.size() >= maximum_open_observations)
				{
					observation->invalid = true;
					observation->complete = false;
					return std::nullopt;
				}
				const auto index = observation->open_events.size();
				observation->open_events.push_back(
					{role, flags, sqlite_backend_open_outcome::attempted, {}, {}, {}});
				observation->complete = false;
				return index;
			}
			catch (...)
			{
				std::scoped_lock lock{observation->mutex};
				observation->invalid = true;
				observation->complete = false;
				return std::nullopt;
			}
		}

		void record_open_failure(const std::shared_ptr<default_connection_observation>& observation,
								 const std::optional<std::size_t> index) noexcept
		{
			if (!observation || !index)
				return;
			std::scoped_lock lock{observation->mutex};
			if (*index >= observation->open_events.size())
			{
				observation->invalid = true;
				observation->complete = false;
				return;
			}
			observation->open_events[*index].outcome = sqlite_backend_open_outcome::failed;
			observation->complete = observation->main_proven && !observation->invalid;
		}

		[[nodiscard]] bool
		record_open_success(const std::shared_ptr<default_connection_observation>& observation,
							const std::optional<std::size_t> index,
							const int returned_flags,
							std::optional<opened_object_identities> identities) noexcept
		{
			if (!observation)
				return true;
			try
			{
				std::scoped_lock lock{observation->mutex};
				if (!index || *index >= observation->open_events.size())
				{
					observation->invalid = true;
					observation->complete = false;
					return false;
				}
				auto& event = observation->open_events[*index];
				event.outcome = sqlite_backend_open_outcome::succeeded;
				event.returned_flags = returned_flags;
				const auto is_main = event.role == sqlite_backend_file_role::main_database;
				if (identities)
				{
					event.object_identity = std::move(identities->object);
					event.directory_entry_identity = std::move(identities->entry);
					if (is_main)
						observation->main_proven = true;
				}
				else if ((observation->canonical_locator == ":memory:" ||
						  observation->profile == source_shm_qualification_profile) &&
						 is_main)
					observation->main_proven = true;
				else if (observation->profile == source_shm_qualification_profile)
				{
					// The qualification producer independently seals scratch inode and namespace
					// identities before and after the query. The target-bound observation companion
					// must not project those scratch paths through its target capability.
				}
				else
				{
					observation->invalid = true;
					observation->complete = false;
				}
				if (!observation->invalid)
					observation->complete = observation->main_proven;
				return !observation->invalid;
			}
			catch (...)
			{
				std::scoped_lock lock{observation->mutex};
				observation->invalid = true;
				observation->complete = false;
				return false;
			}
		}

		void
		mark_incomplete(const std::shared_ptr<default_connection_observation>& observation) noexcept
		{
			if (!observation)
				return;
			std::scoped_lock lock{observation->mutex};
			observation->invalid = true;
			observation->complete = false;
		}

		[[nodiscard]] bool
		record_shm_map_event(const std::shared_ptr<default_connection_observation>& observation,
							 sqlite_backend_shm_map_observation event) noexcept
		{
			if (!observation)
				return true;
			try
			{
				std::scoped_lock lock{observation->mutex};
				if (observation->shm_map_events.size() >= maximum_shm_map_observations)
				{
					observation->invalid = true;
					observation->complete = false;
					return false;
				}
				observation->shm_map_events.push_back(std::move(event));
				return true;
			}
			catch (...)
			{
				mark_incomplete(observation);
				return false;
			}
		}

		[[nodiscard]] bool record_shared_memory_identity(
			const std::shared_ptr<default_connection_observation>& observation,
			const opened_object_identities& identity) noexcept
		{
			if (!observation)
				return false;
			try
			{
				std::scoped_lock lock{observation->mutex};
				observation->shared_memory_object_identity = identity.object;
				observation->shared_memory_entry_identity = identity.entry;
				return true;
			}
			catch (...)
			{
				mark_incomplete(observation);
				return false;
			}
		}

		[[nodiscard]] bool persistent_effect_permitted(const forwarding_file& file,
													   const bool shm_coordination = false) noexcept
		{
			if (!file.observed_role)
				return true;
			return file.connection_observation != nullptr &&
				file.connection_observation->permits_persistent_effect(shm_coordination);
		}

		[[nodiscard]] bool native_operation_permitted(const forwarding_file& file) noexcept
		{
			return !file.source_shm_readonly_qualified || !file.source_shm_terminal_failure;
		}

		void mark_source_shm_terminal_failure(forwarding_file& file) noexcept
		{
			if (file.source_shm_readonly_qualified)
				file.source_shm_terminal_failure = true;
			mark_incomplete(file.connection_observation);
		}

		[[nodiscard]] bool effectful_file_control(const int operation) noexcept
		{
			return std::ranges::find(effectful_file_controls, operation) !=
				effectful_file_controls.end();
		}

		int forwarding_close(sqlite3_file* base) noexcept
		{
			if (base == nullptr)
				return sqlite_io_error;
			auto* file = forwarding(base);
			file->base.methods = nullptr;
			auto owner = file->owner;
			auto observation = file->connection_observation;
			const auto close_callback = file->native ? file->native->trusted_close : nullptr;
			const auto status = close_native_file(file->native, close_callback);
			if (file->main_handle && observation)
			{
				std::scoped_lock lock{observation->mutex};
				observation->main_handle_open = false;
				observation->main_handle_read_only = false;
				observation->held_shm_locks.clear();
				observation->shared_memory_object_identity.reset();
				observation->shared_memory_entry_identity.reset();
			}
			if (owner)
				owner->decrement_open_file_count();
			file->~forwarding_file();
			return status;
		}

		int forwarding_read(sqlite3_file* base,
							void* output,
							const int count,
							const long long offset) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->read != nullptr
					? methods->read(raw, output, count, offset)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_write(sqlite3_file* base,
							 const void* input,
							 const int count,
							 const long long offset) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				if (!persistent_effect_permitted(*file))
					return sqlite_readonly;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->write != nullptr
					? methods->write(raw, input, count, offset)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_truncate(sqlite3_file* base, const long long size) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				if (!persistent_effect_permitted(*file))
					return sqlite_readonly;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->truncate != nullptr
					? methods->truncate(raw, size)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_sync(sqlite3_file* base, const int flags) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->sync != nullptr
					? methods->sync(raw, flags)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_file_size(sqlite3_file* base, long long* output) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->file_size != nullptr
					? methods->file_size(raw, output)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_lock(sqlite3_file* base, const int level) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				if (raw == nullptr || methods == nullptr || methods->lock == nullptr)
					return sqlite_io_error;
				const auto status = methods->lock(raw, level);
				if (status != sqlite_ok || level < sqlite_lock_exclusive || !file->main_handle ||
					!file->connection_observation || !file->connection_observation->effect_gate ||
					!file->connection_observation->effect_gate->has_pending_exclusive_arm())
					return status;

				int moved = 1;
				if (methods->file_control == nullptr ||
					methods->file_control(raw, sqlite_file_control_has_moved, &moved) !=
						sqlite_ok ||
					moved != 0)
				{
					mark_incomplete(file->connection_observation);
					return sqlite_io_error;
				}
				auto armed =
					file->connection_observation->effect_gate->apply_pending_exclusive_arm();
				if (!armed || !*armed)
				{
					mark_incomplete(file->connection_observation);
					return sqlite_io_error;
				}
				return status;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_unlock(sqlite3_file* base, const int level) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->unlock != nullptr
					? methods->unlock(raw, level)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_reserved(sqlite3_file* base, int* output) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr &&
						methods->check_reserved_lock != nullptr
					? methods->check_reserved_lock(raw, output)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_control(sqlite3_file* base, const int operation, void* value) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				if (effectful_file_control(operation) && !persistent_effect_permitted(*file))
					return sqlite_readonly;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->file_control != nullptr
					? methods->file_control(raw, operation, value)
					: sqlite_not_found;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_sector(sqlite3_file* base) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return 0;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->sector_size != nullptr
					? methods->sector_size(raw)
					: 0;
			}
			catch (...)
			{
				return 0;
			}
		}

		int forwarding_characteristics(sqlite3_file* base) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return 0;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr &&
						methods->device_characteristics != nullptr
					? methods->device_characteristics(raw)
					: 0;
			}
			catch (...)
			{
				return 0;
			}
		}

		int forwarding_shm_map(sqlite3_file* base,
							   const int page,
							   const int page_size,
							   const int extend,
							   volatile void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				if (file->source_shm_readonly_qualified)
				{
					auto* raw = underlying_file(*file);
					const auto* methods = underlying_methods(*file);
					if (raw == nullptr || methods == nullptr || methods->shm_map == nullptr)
						return sqlite_io_error;
					const auto expected_identity = file->source_shm_qualification_candidate
						? std::optional<opened_object_identities>{}
						: file->expected_source_shm_identity;
					if (!file->source_shm_qualification_candidate &&
						(!expected_identity || !file->target_namespace_epoch ||
						 !file->target_namespace_epoch->recheck()))
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					const auto readonly_family_seen_before = file->source_shm_readonly_family_seen;
					volatile void* native_mapping{};
					bool native_unmap_attempted{};
					bool native_unmap_succeeded{true};
					const auto release_native_mapping = [&]() noexcept
					{
						if (native_mapping == nullptr || native_unmap_attempted)
							return native_unmap_succeeded;
						native_unmap_attempted = true;
						if (methods->shm_unmap == nullptr)
							native_unmap_succeeded = false;
						else
						{
							try
							{
								native_unmap_succeeded = methods->shm_unmap(raw, 0) == sqlite_ok;
							}
							catch (...)
							{
								native_unmap_succeeded = false;
							}
						}
						if (!native_unmap_succeeded)
							mark_source_shm_terminal_failure(*file);
						return native_unmap_succeeded;
					};
					int native_status{};
					try
					{
						native_status = methods->shm_map(raw, page, page_size, 0, &native_mapping);
					}
					catch (...)
					{
						(void)release_native_mapping();
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					const auto native_nonnull = native_mapping != nullptr;
					int returned_status = native_status;
					volatile void* returned_mapping = native_mapping;
					bool protocol_violation{};
					if (native_status == sqlite_ok)
					{
						protocol_violation = true;
						returned_status = sqlite_io_error;
						returned_mapping = nullptr;
					}
					else if (native_status == sqlite_readonly_cannot_initialize)
					{
						file->source_shm_readonly_family_seen = true;
						if (native_nonnull)
						{
							protocol_violation = true;
							returned_status = sqlite_io_error;
							returned_mapping = nullptr;
						}
					}
					else if (native_status == sqlite_readonly)
					{
						file->source_shm_readonly_family_seen = true;
						if (!native_nonnull)
						{
							returned_status = sqlite_readonly_cannot_initialize;
							returned_mapping = nullptr;
						}
					}
					else if ((native_status & 0xff) == sqlite_readonly)
					{
						file->source_shm_readonly_family_seen = true;
						protocol_violation = true;
						returned_status = sqlite_io_error;
						returned_mapping = nullptr;
					}
					else if (native_nonnull)
					{
						protocol_violation = true;
						returned_status = sqlite_io_error;
						returned_mapping = nullptr;
					}

					bool observed_identity{};
					if (!file->source_shm_qualification_candidate)
					{
						if (!file->target_namespace_epoch->recheck())
						{
							protocol_violation = true;
							returned_status = sqlite_io_error;
							returned_mapping = nullptr;
						}
						else
							observed_identity = true;
					}
					if (protocol_violation)
						(void)release_native_mapping();
					if (protocol_violation)
						mark_source_shm_terminal_failure(*file);
					else if (observed_identity &&
							 !record_shared_memory_identity(file->connection_observation,
															*expected_identity))
					{
						protocol_violation = true;
						returned_status = sqlite_io_error;
						returned_mapping = nullptr;
						(void)release_native_mapping();
						mark_source_shm_terminal_failure(*file);
					}
					if (!record_shm_map_event(
							file->connection_observation,
							{page,
							 page_size,
							 extend,
							 0,
							 native_status,
							 returned_status,
							 native_nonnull,
							 returned_mapping != nullptr,
							 readonly_family_seen_before,
							 file->source_shm_readonly_family_seen,
							 file->owner->pinned_underlying_vfs_identity(),
							 file->owner->pinned_underlying_vfs_app_data_identity()}))
					{
						(void)release_native_mapping();
						mark_source_shm_terminal_failure(*file);
						*output = nullptr;
						return sqlite_io_error;
					}
					if (!native_unmap_succeeded)
					{
						*output = nullptr;
						return sqlite_io_error;
					}
					*output = returned_mapping;
					return returned_status;
				}
				if (file->shm_readonly_cannot_initialize)
					return sqlite_readonly_cannot_initialize;
				int delegated_extend = extend;
				std::optional<opened_object_identities> expected_existing_identity;
				if (extend != 0 && !persistent_effect_permitted(*file, true))
				{
					if (!file->main_handle || !file->connection_observation ||
						!file->connection_observation->permits_existing_read_only_sidecars())
					{
						file->shm_readonly_cannot_initialize = true;
						return sqlite_readonly_cannot_initialize;
					}
					expected_existing_identity = file->owner->observe_shared_memory();
					if (!expected_existing_identity)
					{
						file->shm_readonly_cannot_initialize = true;
						return sqlite_readonly_cannot_initialize;
					}
					delegated_extend = 0;
				}
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				if (raw == nullptr || methods == nullptr || methods->shm_map == nullptr)
					return sqlite_io_error;
				const auto status =
					methods->shm_map(raw, page, page_size, delegated_extend, output);
				if (extend != 0 && delegated_extend == 0 && (status & 0xff) == sqlite_readonly)
				{
					*output = nullptr;
					file->shm_readonly_cannot_initialize = true;
					return sqlite_readonly_cannot_initialize;
				}
				if ((status & 0xff) == sqlite_readonly)
					file->shm_readonly_cannot_initialize = true;
				if (status == sqlite_ok && extend != 0 && *output == nullptr)
				{
					if (delegated_extend == 0)
					{
						file->shm_readonly_cannot_initialize = true;
						return sqlite_readonly_cannot_initialize;
					}
					mark_incomplete(file->connection_observation);
					return sqlite_io_error;
				}
				if (status != sqlite_ok || !file->connection_observation)
					return status;
				auto identities = file->owner->observe_shared_memory();
				if (!identities ||
					(expected_existing_identity &&
					 !same_identities(*expected_existing_identity, *identities)))
				{
					mark_incomplete(file->connection_observation);
					if (expected_existing_identity)
					{
						if (output != nullptr)
							*output = nullptr;
						if (methods->shm_unmap != nullptr)
							(void)methods->shm_unmap(raw, 0);
						return sqlite_io_error;
					}
					return status;
				}
				try
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					file->connection_observation->shared_memory_object_identity =
						std::move(identities->object);
					file->connection_observation->shared_memory_entry_identity =
						std::move(identities->entry);
				}
				catch (...)
				{
					mark_incomplete(file->connection_observation);
				}
				return status;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_shm_lock(sqlite3_file* base,
								const int offset,
								const int count,
								const int flags) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				if (raw == nullptr || methods == nullptr || methods->shm_lock == nullptr)
					return sqlite_io_error;
				int status{};
				try
				{
					status = methods->shm_lock(raw, offset, count, flags);
				}
				catch (...)
				{
					mark_source_shm_terminal_failure(*file);
					return sqlite_io_error;
				}
				if (status != sqlite_ok || !file->connection_observation)
					return status;
				const auto action = flags & (sqlite_shm_unlock | sqlite_shm_lock);
				const auto mode = flags & (sqlite_shm_shared | sqlite_shm_exclusive);
				if (offset < 0 || count <= 0 ||
					static_cast<long long>(offset) + static_cast<long long>(count) >
						std::numeric_limits<int>::max() ||
					(action != sqlite_shm_unlock && action != sqlite_shm_lock) ||
					(mode != sqlite_shm_shared && mode != sqlite_shm_exclusive))
				{
					if (file->source_shm_readonly_qualified)
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					mark_incomplete(file->connection_observation);
					return status;
				}
				try
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					auto& held = file->connection_observation->held_shm_locks;
					const auto overlaps = [&](const sqlite_backend_shm_lock_observation& value)
					{
						const auto value_end = static_cast<long long>(value.offset) + value.count;
						const auto requested_end = static_cast<long long>(offset) + count;
						return value.offset < requested_end && offset < value_end;
					};
					held.erase(std::remove_if(held.begin(), held.end(), overlaps), held.end());
					if (action == sqlite_shm_lock)
					{
						if (held.size() >= maximum_shm_lock_observations)
						{
							file->connection_observation->invalid = true;
							file->connection_observation->complete = false;
							if (file->source_shm_readonly_qualified)
							{
								file->source_shm_terminal_failure = true;
								return sqlite_io_error;
							}
							return status;
						}
						held.push_back({offset,
										count,
										mode == sqlite_shm_exclusive
											? sqlite_backend_shm_lock_mode::exclusive
											: sqlite_backend_shm_lock_mode::shared});
						std::ranges::sort(held, {}, &sqlite_backend_shm_lock_observation::offset);
					}
				}
				catch (...)
				{
					mark_source_shm_terminal_failure(*file);
					return sqlite_io_error;
				}
				return status;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		void forwarding_shm_barrier(sqlite3_file* base) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				if (raw != nullptr && methods != nullptr && methods->shm_barrier != nullptr)
				{
					try
					{
						methods->shm_barrier(raw);
					}
					catch (...)
					{
						mark_source_shm_terminal_failure(*file);
					}
				}
			}
			catch (...)
			{
				return;
			}
		}

		int forwarding_shm_unmap(sqlite3_file* base, const int remove_file) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				const auto delegated_remove = file->source_shm_readonly_qualified ? 0 : remove_file;
				if (!file->source_shm_readonly_qualified && remove_file != 0 &&
					!persistent_effect_permitted(*file))
					return sqlite_readonly;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				if (raw == nullptr || methods == nullptr || methods->shm_unmap == nullptr)
					return sqlite_io_error;
				int status{};
				try
				{
					status = methods->shm_unmap(raw, delegated_remove);
				}
				catch (...)
				{
					mark_source_shm_terminal_failure(*file);
					return sqlite_io_error;
				}
				if (file->source_shm_readonly_qualified && status != sqlite_ok)
					mark_source_shm_terminal_failure(*file);
				if (status == sqlite_ok)
					file->shm_readonly_cannot_initialize = false;
				if (status == sqlite_ok)
					file->source_shm_readonly_family_seen = false;
				if (status == sqlite_ok && file->connection_observation)
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					file->connection_observation->held_shm_locks.clear();
					file->connection_observation->shared_memory_object_identity.reset();
					file->connection_observation->shared_memory_entry_identity.reset();
				}
				return status;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_fetch(sqlite3_file* base,
							 const long long offset,
							 const int count,
							 void** output) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->fetch != nullptr
					? methods->fetch(raw, offset, count, output)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_unfetch(sqlite3_file* base, const long long offset, void* value) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				auto* raw = underlying_file(*file);
				const auto* methods = underlying_methods(*file);
				return raw != nullptr && methods != nullptr && methods->unfetch != nullptr
					? methods->unfetch(raw, offset, value)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_vfs_open(sqlite3_vfs* vfs,
								const char* name,
								sqlite3_file* output,
								const int flags,
								int* out_flags) noexcept
		{
			if (out_flags != nullptr)
				*out_flags = 0;
			if (vfs == nullptr || vfs->app_data == nullptr || output == nullptr)
				return sqlite_cannot_open;
			output->methods = nullptr;
			auto* raw_owner = static_cast<default_forwarding_state*>(vfs->app_data);
			if (raw_owner->vfs_implementation_identity() != vfs)
				return sqlite_cannot_open;
			std::shared_ptr<default_forwarding_state> owner;
			open_association association;
			std::optional<std::size_t> event_index;
			std::optional<opened_object_identities> expected_existing_identity;
			forwarding_file* file{};
			bool native_open_invoked{};
			bool native_open_returned{};
			auto source_shm_validation = source_shm_open_validation::generic;
			try
			{
				owner = raw_owner->shared_from_this();
				association = owner->associate_open(name, flags);
				if (association.rejected)
					return sqlite_cannot_open;
				event_index = record_open_attempt(association.observation, association.role, flags);
				if (association.observation && !event_index)
				{
					mark_incomplete(association.observation);
					return sqlite_io_error;
				}
				if (association.qualification_fixture && association.main_handle &&
					flags != qualification_fixture_main_xopen_flags)
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					return sqlite_cannot_open;
				}
				if (association.main_handle && association.observation &&
					!association.qualification_fixture)
				{
					source_shm_validation = owner->validate_source_shm_open_callback(
						*association.observation, name, flags);
					if (source_shm_validation == source_shm_open_validation::rejected)
					{
						record_open_failure(association.observation, event_index);
						return sqlite_cannot_open;
					}
				}
				int delegated_flags = flags;
				if ((flags & sqlite_open_create) != 0 && association.observed_role &&
					!association.observation)
				{
					record_open_failure(association.observation, event_index);
					return sqlite_readonly;
				}
				if ((flags & sqlite_open_create) != 0 && association.observation &&
					!association.qualification_fixture &&
					!association.observation->permits_persistent_effect(false))
				{
					const auto coordination_wal_create =
						association.role == sqlite_backend_file_role::write_ahead_log &&
						flags ==
							(sqlite_open_read_write | sqlite_open_create |
							 sqlite_open_write_ahead_log) &&
						association.observation->effect_gate != nullptr &&
						association.observation->effect_gate->stage() ==
							sqlite_backend_effect_stage::wal_shm_coordination_only &&
						association.observation->permits_persistent_effect(true);
					const auto existing_read_only_wal =
						association.role == sqlite_backend_file_role::write_ahead_log &&
						flags ==
							(sqlite_open_read_write | sqlite_open_create |
							 sqlite_open_write_ahead_log) &&
						association.observation->permits_existing_read_only_sidecars();
					if (!coordination_wal_create && !existing_read_only_wal &&
						!association.main_handle)
					{
						record_open_failure(association.observation, event_index);
						return sqlite_readonly;
					}
					if (existing_read_only_wal)
					{
						bool qualification_candidate{};
						{
							std::scoped_lock lock{association.observation->mutex};
							qualification_candidate = association.observation->profile ==
								source_shm_qualification_profile;
						}
						if (!qualification_candidate)
							expected_existing_identity = owner->observe_stable_existing_entry(
								sqlite_backend_file_role::write_ahead_log);
						if (!qualification_candidate && !expected_existing_identity)
						{
							record_open_failure(association.observation, event_index);
							return sqlite_readonly;
						}
						delegated_flags = sqlite_open_read_only | sqlite_open_write_ahead_log;
					}
					// A pre-created main file may be opened while denied, but the underlying
					// delegate must not retain authority to recreate a concurrently removed path.
					if (!coordination_wal_create && !existing_read_only_wal)
						delegated_flags &= ~sqlite_open_create;
				}
				static_assert(alignof(forwarding_file) <= alignof(std::max_align_t));
				file = new (output) forwarding_file{};
				file->owner = owner;
				file->connection_observation = association.observation;
				file->role = association.role;
				file->observed_role = association.observed_role;
				file->main_handle = association.main_handle;
				file->source_shm_readonly_qualified =
					source_shm_validation == source_shm_open_validation::accepted;
				if (file->source_shm_readonly_qualified && association.observation)
				{
					std::scoped_lock lock{association.observation->mutex};
					file->source_shm_qualification_candidate =
						association.observation->source_shm_qualification_open_plan.has_value();
					if (!file->source_shm_qualification_candidate &&
						association.observation->source_shm_open_plan)
					{
						const auto& qualification =
							association.observation->source_shm_open_plan->qualification;
						file->expected_source_shm_identity = opened_object_identities{
							qualification.expected_shared_memory_object_identity,
							qualification.expected_shared_memory_entry_identity,
						};
						file->target_namespace_epoch = qualification.target_namespace_epoch;
					}
				}
				if (association.observation && !file->target_namespace_epoch)
				{
					std::scoped_lock lock{association.observation->mutex};
					if (association.observation->source_shm_open_plan)
						file->target_namespace_epoch = association.observation->source_shm_open_plan
														   ->qualification.target_namespace_epoch;
				}
				if (file->target_namespace_epoch && !file->target_namespace_epoch->recheck())
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					file->~forwarding_file();
					return sqlite_cannot_open;
				}
				if (owner->underlying()->app_data !=
						owner->pinned_underlying_vfs_app_data_identity() ||
					function_address(owner->underlying()->open) !=
						owner->underlying_open_callback_address())
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					file->~forwarding_file();
					return sqlite_cannot_open;
				}
				file->native = std::make_shared<native_file_node>(
					static_cast<std::size_t>(owner->underlying()->os_file_bytes),
					owner,
					owner->registry().runtime_lifetime,
					association.observation,
					file->target_namespace_epoch,
					owner->underlying(),
					owner->pinned_underlying_vfs_app_data_identity(),
					owner->underlying_image_identity(),
					owner->underlying_open_callback_address());
				// Copying a shared_ptr is noexcept and cannot allocate. Pre-arm the self-cycle
				// before native xOpen so every uncertain callback outcome can retain the opaque
				// allocation.
				static_assert(std::is_nothrow_copy_assignable_v<std::shared_ptr<native_file_node>>);
				file->native->quarantine_self = file->native;
				auto* raw = underlying_file(*file);
				if (raw == nullptr)
					throw std::bad_alloc{};
				int local_out_flags{};
				native_open_invoked = true;
				const auto status = owner->underlying()->open(
					owner->underlying(), name, raw, delegated_flags, &local_out_flags);
				native_open_returned = true;
				if (status != sqlite_ok && raw->methods == nullptr)
				{
					release_known_safe_native_file(file->native);
					record_open_failure(association.observation, event_index);
					file->~forwarding_file();
					return status;
				}
				if (raw->methods == nullptr)
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					quarantine_native_file(file->native);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				const auto inspection = inspect_native_methods(*file->native);
				file->native->trusted_close = inspection.trusted_close;
				if (status != sqlite_ok)
				{
					record_open_failure(association.observation, event_index);
					(void)close_native_file(file->native, inspection.trusted_close);
					file->~forwarding_file();
					return status;
				}
				if (inspection.forwarding_methods == nullptr)
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					cleanup_failed_forwarding_open(*file);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				file->native->trusted_methods = inspection.callbacks;
				file->native->trusted_methods_ready = true;
				if (file->target_namespace_epoch && !file->target_namespace_epoch->recheck())
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					cleanup_failed_forwarding_open(*file);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				std::optional<opened_object_identities> identities;
				if (file->target_namespace_epoch)
				{
					auto retained = file->target_namespace_epoch->retained_entry(file->role);
					if (!retained || retained->state != sqlite_backend_entry_state::held_regular ||
						!retained->object_identity || !retained->directory_entry_identity)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						cleanup_failed_forwarding_open(*file);
						file->~forwarding_file();
						return sqlite_io_error;
					}
					identities = opened_object_identities{*retained->object_identity,
														  *retained->directory_entry_identity};
				}
				else
					identities = owner->observe_opened_object(*file);
				if (expected_existing_identity &&
					((local_out_flags & sqlite_open_read_only) == 0 ||
					 (local_out_flags & sqlite_open_read_write) != 0 || !identities ||
					 !same_identities(*expected_existing_identity, *identities)))
				{
					mark_incomplete(association.observation);
					record_open_failure(association.observation, event_index);
					cleanup_failed_forwarding_open(*file);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				if (!record_open_success(association.observation,
										 event_index,
										 local_out_flags,
										 std::move(identities)))
				{
					cleanup_failed_forwarding_open(*file);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				if (association.main_handle && association.observation)
				{
					std::scoped_lock lock{association.observation->mutex};
					association.observation->main_handle_open = true;
					association.observation->main_handle_read_only =
						(delegated_flags & sqlite_open_read_only) != 0 &&
						(delegated_flags & sqlite_open_read_write) == 0 &&
						(local_out_flags & sqlite_open_read_only) != 0 &&
						(local_out_flags & sqlite_open_read_write) == 0;
				}
				if (out_flags != nullptr)
					*out_flags = local_out_flags;
				owner->increment_open_file_count();
				file->base.methods = inspection.forwarding_methods;
				return status;
			}
			catch (...)
			{
				mark_incomplete(association.observation);
				record_open_failure(association.observation, event_index);
				if (file != nullptr)
				{
					file->base.methods = nullptr;
					if (file->native)
					{
						if (native_open_invoked && !native_open_returned)
							quarantine_native_file(file->native);
						else if (native_open_returned)
							cleanup_failed_forwarding_open(*file);
						else
							release_known_safe_native_file(file->native);
					}
					file->~forwarding_file();
				}
				return sqlite_no_memory;
			}
		}

		[[nodiscard]] default_forwarding_state* forwarding_owner(sqlite3_vfs* vfs) noexcept
		{
			if (vfs == nullptr || vfs->app_data == nullptr)
				return nullptr;
			auto* owner = static_cast<default_forwarding_state*>(vfs->app_data);
			return owner->vfs_implementation_identity() == vfs ? owner : nullptr;
		}

		int
		forwarding_vfs_remove(sqlite3_vfs* vfs, const char* name, const int sync_directory) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				if (owner != nullptr && !owner->permits_path_effect(name))
					return sqlite_readonly;
				return owner != nullptr && owner->underlying()->remove != nullptr
					? owner->underlying()->remove(owner->underlying(), name, sync_directory)
					: sqlite_io_error;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int forwarding_vfs_access(sqlite3_vfs* vfs,
								  const char* name,
								  const int flags,
								  int* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				if (owner != nullptr && owner->denies_logical_source_access(name))
				{
					if (output != nullptr)
						*output = 0;
					return sqlite_ok;
				}
				return owner != nullptr && owner->underlying()->access != nullptr
					? owner->underlying()->access(owner->underlying(), name, flags, output)
					: sqlite_error;
			}
			catch (...)
			{
				return sqlite_error;
			}
		}

		int forwarding_vfs_full_pathname(sqlite3_vfs* vfs,
										 const char* name,
										 const int size,
										 char* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				if (owner == nullptr)
					return sqlite_cannot_open;
				switch (owner->preserve_qualified_full_path(name, size, output))
				{
					case qualification_full_path_result::preserved:
						return sqlite_ok;
					case qualification_full_path_result::rejected:
						return sqlite_cannot_open;
					case qualification_full_path_result::delegate:
						break;
				}
				return owner != nullptr && owner->underlying()->full_pathname != nullptr
					? owner->underlying()->full_pathname(owner->underlying(), name, size, output)
					: sqlite_cannot_open;
			}
			catch (...)
			{
				return sqlite_cannot_open;
			}
		}

		void* forwarding_vfs_dl_open(sqlite3_vfs* vfs, const char* name) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->dl_open != nullptr
					? owner->underlying()->dl_open(owner->underlying(), name)
					: nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		void forwarding_vfs_dl_error(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				if (owner != nullptr && owner->underlying()->dl_error != nullptr)
					owner->underlying()->dl_error(owner->underlying(), size, output);
			}
			catch (...)
			{
				return;
			}
		}

		void (*forwarding_vfs_dl_sym(sqlite3_vfs* vfs,
									 void* handle,
									 const char* name) noexcept)(void)
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->dl_sym != nullptr
					? owner->underlying()->dl_sym(owner->underlying(), handle, name)
					: nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		void forwarding_vfs_dl_close(sqlite3_vfs* vfs, void* handle) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				if (owner != nullptr && owner->underlying()->dl_close != nullptr)
					owner->underlying()->dl_close(owner->underlying(), handle);
			}
			catch (...)
			{
				return;
			}
		}

		int forwarding_vfs_randomness(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->randomness != nullptr
					? owner->underlying()->randomness(owner->underlying(), size, output)
					: 0;
			}
			catch (...)
			{
				return 0;
			}
		}

		int forwarding_vfs_sleep(sqlite3_vfs* vfs, const int microseconds) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->sleep != nullptr
					? owner->underlying()->sleep(owner->underlying(), microseconds)
					: 0;
			}
			catch (...)
			{
				return 0;
			}
		}

		int forwarding_vfs_current_time(sqlite3_vfs* vfs, double* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->current_time != nullptr
					? owner->underlying()->current_time(owner->underlying(), output)
					: sqlite_error;
			}
			catch (...)
			{
				return sqlite_error;
			}
		}

		int forwarding_vfs_last_error(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->get_last_error != nullptr
					? owner->underlying()->get_last_error(owner->underlying(), size, output)
					: sqlite_not_found;
			}
			catch (...)
			{
				return sqlite_not_found;
			}
		}

		int forwarding_vfs_current_time_int64(sqlite3_vfs* vfs, long long* output) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->current_time_int64 != nullptr
					? owner->underlying()->current_time_int64(owner->underlying(), output)
					: sqlite_not_found;
			}
			catch (...)
			{
				return sqlite_not_found;
			}
		}

		int forwarding_vfs_set_system_call(sqlite3_vfs* vfs,
										   const char* name,
										   const sqlite3_syscall_ptr function) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->set_system_call != nullptr
					? owner->underlying()->set_system_call(owner->underlying(), name, function)
					: sqlite_not_found;
			}
			catch (...)
			{
				return sqlite_not_found;
			}
		}

		sqlite3_syscall_ptr forwarding_vfs_get_system_call(sqlite3_vfs* vfs,
														   const char* name) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->get_system_call != nullptr
					? owner->underlying()->get_system_call(owner->underlying(), name)
					: nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		const char* forwarding_vfs_next_system_call(sqlite3_vfs* vfs, const char* name) noexcept
		{
			try
			{
				auto* owner = forwarding_owner(vfs);
				return owner != nullptr && owner->underlying()->next_system_call != nullptr
					? owner->underlying()->next_system_call(owner->underlying(), name)
					: nullptr;
			}
			catch (...)
			{
				return nullptr;
			}
		}

		class ephemeral_observation_capability final : public sqlite_backend_observation_capability
		{
		  public:
			ephemeral_observation_capability(
				std::string registered_name,
				const void* forwarding_identity,
				std::shared_ptr<void> backend_lifetime,
				const void* runtime_identity,
				std::shared_ptr<void> runtime_lifetime,
				std::shared_ptr<const sqlite_default_connection_observation_port> connection_port)
				: registered_name_{std::move(registered_name)},
				  forwarding_identity_{forwarding_identity},
				  backend_lifetime_{std::move(backend_lifetime)},
				  runtime_identity_{runtime_identity},
				  runtime_lifetime_{std::move(runtime_lifetime)},
				  connection_port_{std::move(connection_port)}
			{
				capability_token_.profile = "default-ephemeral-v1.capability.v1";
				capability_token_.bytes.reserve(96U + registered_name_.size());
				append_bytes(capability_token_.bytes, ephemeral_profile);
				append_bytes(capability_token_.bytes, registered_name_);
				append_pointer(capability_token_.bytes, forwarding_identity_);
				append_pointer(capability_token_.bytes, backend_lifetime_.get());
				append_pointer(capability_token_.bytes, runtime_identity_);
				append_pointer(capability_token_.bytes, runtime_lifetime_.get());
				append_pointer(capability_token_.bytes, connection_port_.get());
			}

			[[nodiscard]] sqlite_backend_vfs_binding binding() const noexcept override
			{
				return {
					ephemeral_profile,
					registered_name_,
					forwarding_identity_,
					backend_lifetime_.get(),
					this,
					runtime_identity_,
					runtime_lifetime_.get(),
				};
			}

			[[nodiscard]] const sqlite_backend_opaque_identity&
			capability_token() const noexcept override
			{
				return capability_token_;
			}

			[[nodiscard]] result<sqlite_backend_namespace_census>
			capture_namespace(std::string_view) const override
			{
				return unexpected(forwarding_error("ephemeral-namespace-observation"));
			}

			[[nodiscard]] result<bool> recheck_namespace(const sqlite_backend_namespace_census&,
														 std::string_view) const override
			{
				return unexpected(forwarding_error("ephemeral-namespace-observation"));
			}

			[[nodiscard]] result<sqlite_backend_zero_main_receipt>
			exclusive_create_sync_zero_main(std::string_view) override
			{
				return unexpected(forwarding_error("ephemeral-bootstrap"));
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot_builder>>
			create_private_snapshot() override
			{
				return unexpected(forwarding_error("ephemeral-private-snapshot"));
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_connection_observation(const std::string_view canonical_vfs_locator) override
			{
				if (canonical_vfs_locator != ":memory:" || !connection_port_)
					return unexpected(forwarding_error("vfs-observation-binding"));
				return connection_port_->begin_connection_observation(canonical_vfs_locator,
																	  capability_token_);
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_ephemeral_connection_observation() override
			{
				return begin_connection_observation(":memory:");
			}

		  private:
			std::string registered_name_;
			const void* forwarding_identity_{};
			std::shared_ptr<void> backend_lifetime_;
			const void* runtime_identity_{};
			std::shared_ptr<void> runtime_lifetime_;
			std::shared_ptr<const sqlite_default_connection_observation_port> connection_port_;
			sqlite_backend_opaque_identity capability_token_;
		};

		[[nodiscard]] result<std::shared_ptr<default_forwarding_state>>
		make_forwarding_state(sqlite_private_snapshot_registry_binding registry)
		{
			try
			{
				return default_forwarding_state::create(std::move(registry));
			}
			catch (const std::exception&)
			{
				return unexpected(forwarding_error("forwarding-vfs-register"));
			}
			catch (...)
			{
				return unexpected(forwarding_error("forwarding-vfs-register"));
			}
		}
	} // namespace

	result<std::shared_ptr<sqlite_default_forwarding_vfs>>
	make_sqlite_default_forwarding_vfs(sqlite_private_snapshot_registry_binding registry)
	{
		auto state = make_forwarding_state(std::move(registry));
		if (!state)
			return unexpected(std::move(state.error()));
		return std::static_pointer_cast<sqlite_default_forwarding_vfs>(std::move(*state));
	}

	result<sqlite_default_forwarding_store_bundle>
	make_sqlite_default_forwarding_store_bundle(const std::string_view raw_path,
												sqlite_private_snapshot_registry_binding registry)
	{
		auto state = make_forwarding_state(std::move(registry));
		if (!state)
			return unexpected(std::move(state.error()));
		auto canonical = (*state)->canonicalize(raw_path);
		if (!canonical)
			return unexpected(std::move(canonical.error()));
		auto backend_lifetime = std::static_pointer_cast<void>(*state);
		auto observation =
			make_sqlite_default_observation_capability(sqlite_default_observation_binding{
				*canonical,
				std::string{(*state)->registered_vfs_name()},
				(*state)->vfs_implementation_identity(),
				(*state)->pinned_underlying_vfs_identity(),
				(*state)->pinned_underlying_vfs_app_data_identity(),
				backend_lifetime,
				(*state)->registry(),
				(*state)->connection_port(),
			});
		if (!observation)
			return unexpected(std::move(observation.error()));
		if (auto attached = (*state)->attach_observation(
				*canonical, std::string{filesystem_profile}, *observation);
			!attached)
			return unexpected(std::move(attached.error()));
		return sqlite_default_forwarding_store_bundle{
			std::static_pointer_cast<sqlite_default_forwarding_vfs>(*state),
			std::move(*canonical),
			std::move(*observation),
			(*state)->connection_port(),
			(*state)->runtime_identity(),
			(*state)->registry().runtime_lifetime,
		};
	}

	result<sqlite_default_ephemeral_store_bundle>
	make_sqlite_default_ephemeral_store_bundle(sqlite_private_snapshot_registry_binding registry)
	{
		auto state = make_forwarding_state(std::move(registry));
		if (!state)
			return unexpected(std::move(state.error()));
		try
		{
			auto backend_lifetime = std::static_pointer_cast<void>(*state);
			auto capability = std::make_shared<ephemeral_observation_capability>(
				std::string{(*state)->registered_vfs_name()},
				(*state)->vfs_implementation_identity(),
				backend_lifetime,
				(*state)->runtime_identity(),
				(*state)->registry().runtime_lifetime,
				(*state)->connection_port());
			auto observation =
				std::static_pointer_cast<sqlite_backend_observation_capability>(capability);
			if (auto attached = (*state)->attach_observation(
					":memory:", std::string{ephemeral_profile}, observation);
				!attached)
				return unexpected(std::move(attached.error()));
			return sqlite_default_ephemeral_store_bundle{
				std::static_pointer_cast<sqlite_default_forwarding_vfs>(*state),
				":memory:",
				std::move(observation),
				(*state)->connection_port(),
				(*state)->runtime_identity(),
				(*state)->registry().runtime_lifetime,
			};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(forwarding_error("forwarding-vfs-allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(forwarding_error("forwarding-vfs-allocation"));
		}
	}
} // namespace cxxlens::sdk
