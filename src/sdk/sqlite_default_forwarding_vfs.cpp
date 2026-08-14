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
#include "sqlite_same_process_shm_identity_issuer_internal.hpp"
#include "sqlite_same_process_shm_vfs_alias_registration_internal.hpp"
#include "sqlite_source_shm_readonly_preflight_internal.hpp"
#include "sqlite_vfs_abi_internal.hpp"
#include "sqlite_writer_shm_mapping_epoch_internal.hpp"
#include "sqlite_writer_shm_mapping_semantics_internal.hpp"

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
		constexpr std::string_view shm_suffix{"-shm"};
		constexpr std::string_view forwarding_profile{"default-forwarding-vfs-v1"};
		constexpr std::string_view filesystem_profile{"default-filesystem-v1"};
		constexpr std::string_view ephemeral_profile{"default-ephemeral-v1"};
		constexpr std::string_view source_shm_profile{"sqlite-source-shm-readonly-unix-uri-v1"};
		constexpr std::string_view source_shm_qualification_profile{
			"sqlite-source-shm-readonly-qualification-candidate-v1"};
		// The internal lease/attachment machinery is intentionally retained as a proposed,
		// fail-closed implementation boundary. The accepted authority still requires a
		// distinct exact implementation review and complete counterexample matrix before the
		// qualified source-SHM native-OK exception may be activated.
		constexpr bool source_shm_native_ok_projection_production_activation = false;

		constexpr int source_shm_open_flags = sqlite_open_read_only | sqlite_open_uri |
			sqlite_open_private_cache | sqlite_open_full_mutex;
		constexpr int source_shm_main_xopen_flags =
			sqlite_open_read_only | sqlite_open_uri | sqlite_open_main_database;
		constexpr int qualification_fixture_main_xopen_flags =
			sqlite_open_read_write | sqlite_open_create | sqlite_open_main_database;

		// Callback and native-lifetime identities can outlive their forwarding state in the
		// process-global SHM registry.  A state-local counter would therefore permit pointer ABA:
		// a later forwarding state could receive the same native_file_node address and mint the
		// same identity after its counter restarted at one.  Keep one checked sequence domain for
		// every forwarding alias in this process; the registry still binds each resulting receipt
		// to its exact process/family/alias scope.
		std::atomic<std::uint64_t> next_native_lifetime_sequence{1U};

		using sqlite3_file = sqlite_vfs_abi::file;
		using sqlite3_io_methods = sqlite_vfs_abi::io_methods;
		using sqlite3_vfs = sqlite_vfs_abi::vfs;
		using sqlite3_syscall_ptr = sqlite_vfs_abi::syscall_ptr;

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
		struct native_file_node;

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
			std::size_t native_shm_map_attempt_count{};
			bool writer_shm_mapping_epoch_requested{};
			std::optional<sqlite_source_shm_qualified_open_plan> source_shm_open_plan;
			std::optional<sqlite_source_shm_qualification_open_plan>
				source_shm_qualification_open_plan;
			std::optional<sqlite_source_shm_qualification_fixture_fullpath_plan>
				source_shm_qualification_fixture_fullpath_plan;
			std::optional<sqlite_source_shm_qualification_fixture_fullpath_plan>
				source_shm_qualification_fixture_pending_open_plan;
			std::optional<sqlite_source_shm_open_callback_receipt> source_shm_open_callback_receipt;
			std::optional<sqlite_backend_opaque_identity> main_native_file_receipt;
			std::optional<sqlite_backend_opaque_identity> main_native_xopen_receipt;
			std::optional<sqlite_backend_opaque_identity> main_callback_cohort;
			std::optional<sqlite_backend_opaque_identity> main_open_epoch;
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> writer_target_namespace_epoch;
			std::optional<sqlite_backend_opaque_identity> writer_sqlite_source_id;
			std::optional<sqlite_backend_opaque_identity> writer_effect_gate_receipt;
			std::optional<sqlite_backend_opaque_identity> writer_effect_receipt;
			std::optional<sqlite_shm_writer_eligibility> writer_eligibility;
			std::weak_ptr<default_forwarding_state> owner;
			std::weak_ptr<native_file_node> main_native_node;
			std::weak_ptr<native_file_node> wal_native_node;
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

			[[nodiscard]] bool requires_source_shm_writer_mapping_epoch() const noexcept override
			{
				return true;
			}

			[[nodiscard]] bool native_shm_map_attempted() const noexcept override
			{
				std::scoped_lock lock{mutex};
				return native_shm_map_attempt_count != 0U;
			}

			[[nodiscard]] result<void> request_writer_shm_mapping_epoch() override
			{
				std::scoped_lock lock{mutex};
				if (invalid || main_handle_open || writer_shm_mapping_epoch_requested)
					return unexpected(forwarding_error("source-shm-writer-epoch-request"));
				writer_shm_mapping_epoch_requested = true;
				return {};
			}

			[[nodiscard]] bool writer_shm_mapping_epoch_armed() const noexcept override
			{
				std::scoped_lock lock{mutex};
				return writer_target_namespace_epoch != nullptr &&
					writer_sqlite_source_id.has_value();
			}

			[[nodiscard]] result<void> install_current_v3_writer_eligibility() override;

			[[nodiscard]] result<void> arm_writer_shm_mapping_epoch(
				std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch,
				sqlite_backend_opaque_identity sqlite_source_id) override;

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

		/** Stable source-private view of the installed family while one VFS owner is alive. */
		struct source_shm_reader_registry_context
		{
			sqlite_same_process_shm_mapping_registry* registry{};
			sqlite_shm_registry_family_pin* family{};
			const sqlite_shm_lease_family_binding* family_binding{};
			const sqlite_backend_opaque_identity* alias_lifetime{};
			const sqlite_backend_opaque_identity* registration_epoch{};
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
				  underlying_vfs_version{underlying_vfs != nullptr ? underlying_vfs->version : 0},
				  underlying_app_data_identity{underlying_app_data},
				  underlying_image_identity{underlying_image},
				  underlying_open_callback_address{underlying_open_callback},
				  underlying_full_pathname_callback_address{
					  function_address(underlying_vfs->full_pathname)}
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
			std::optional<sqlite_backend_opaque_identity> registration_epoch;
			bool writer_target_namespace_epoch_owner{};
			sqlite3_vfs* underlying{};
			const void* underlying_vfs_identity{};
			int underlying_vfs_version{};
			const void* underlying_app_data_identity{};
			const void* underlying_image_identity{};
			const void* underlying_open_callback_address{};
			const void* underlying_full_pathname_callback_address{};
			const sqlite3_io_methods* underlying_methods_identity{};
			int underlying_methods_version{};
			sqlite3_io_methods trusted_methods{};
			bool trusted_methods_ready{};
			int (*trusted_close)(sqlite3_file*){};
			// The source is deliberately declared before the revoker so destruction revokes the
			// close epoch before the source's weak owner is released.
			std::optional<sqlite_writer_shm_native_lifetime_source> writer_lifetime_source;
			std::optional<sqlite_writer_shm_native_lifetime_revoker> writer_lifetime_revoker;
			std::optional<sqlite_writer_shm_native_lifetime_source> writer_retained_parent_source;
			std::optional<sqlite_writer_shm_native_lifetime_revoker> writer_retained_parent_revoker;
			std::optional<sqlite_backend_opaque_identity> writer_retained_parent_receipt;
			std::optional<sqlite_writer_shm_native_lifetime_source> writer_shm_attachment_source;
			std::optional<sqlite_writer_shm_native_lifetime_revoker> writer_shm_attachment_revoker;
			std::optional<sqlite_backend_opaque_identity> writer_shm_attachment_receipt;
			std::optional<sqlite_backend_opaque_identity> writer_native_file_receipt;
			std::optional<sqlite_backend_opaque_identity> writer_native_xopen_receipt;
			std::optional<sqlite_backend_opaque_identity> writer_callback_cohort;
			std::optional<sqlite_backend_opaque_identity> writer_open_epoch;
			// One writer attachment spans every page map on this native main handle. The
			// identity must not be reminted for a later page, otherwise the registry sees a
			// same-family lineage with a different attachment epoch and rejects the extension.
			std::optional<sqlite_backend_opaque_identity> writer_attachment_epoch;
			std::vector<sqlite_shm_pending_mapping> writer_pending;
			std::optional<sqlite_shm_native_attachment_identity> writer_pending_attachment;
			std::vector<sqlite_shm_writer_holder> writer_holders;
			std::mutex writer_lifecycle_mutex;
			std::optional<sqlite_shm_reader_open_authority> reader_open_authority;
			std::mutex reader_lifecycle_mutex;
			std::optional<sqlite_shm_reader_session_request> reader_session_request;
			std::optional<sqlite_shm_reader_session> reader_session;
			std::optional<sqlite_shm_reader_handoff> reader_handoff;
			std::uint64_t reader_generation{};
			bool reader_existing_route_active{};
			bool reader_lifecycle_failed{};
			std::shared_ptr<native_file_node> quarantine_self;
			std::atomic_bool quarantine_enqueued{false};
			// Non-owning link; quarantine_self remains the owner for process lifetime.
			native_file_node* quarantine_next{};
			bool close_attempted{};
		};

		struct native_file_quarantine_sink
		{
			static_assert(std::atomic<native_file_node*>::is_always_lock_free);
			std::atomic<native_file_node*> head{nullptr};
		};
		native_file_quarantine_sink native_file_quarantine_sink_storage;

		void quarantine_native_file(std::shared_ptr<native_file_node>& node) noexcept
		{
			// `quarantine_self` was armed before native xOpen. Keep the quarantined node reachable
			// through a process-lifetime, lock-free sink instead of leaving an unreachable
			// self-cycle behind. The link is non-owning because quarantine_self remains the owner.
			if (!node)
				return;
			bool expected = false;
			if (!node->quarantine_enqueued.compare_exchange_strong(
					expected, true, std::memory_order_acq_rel, std::memory_order_acquire))
			{
				node.reset();
				return;
			}
			auto* previous =
				native_file_quarantine_sink_storage.head.load(std::memory_order_acquire);
			do
			{
				node->quarantine_next = previous;
			} while (!native_file_quarantine_sink_storage.head.compare_exchange_weak(
				previous, node.get(), std::memory_order_release, std::memory_order_acquire));
			node.reset();
		}

		void release_known_safe_native_file(std::shared_ptr<native_file_node>& node) noexcept
		{
			if (node && !node->quarantine_enqueued.load(std::memory_order_acquire))
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

		void append_opaque_identity(std::vector<std::byte>& output,
									const sqlite_backend_opaque_identity& identity)
		{
			append_bytes(output, identity.profile);
			append_u64(output, static_cast<std::uint64_t>(identity.bytes.size()));
			output.insert(output.end(), identity.bytes.begin(), identity.bytes.end());
		}

		[[nodiscard]] bool
		valid_opaque_identity(const sqlite_backend_opaque_identity& identity) noexcept
		{
			return !identity.profile.empty() && !identity.bytes.empty();
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_sqlite_source_id_identity(const char* source_id) noexcept
		{
			if (source_id == nullptr || source_id[0] == '\0')
				return std::nullopt;
			try
			{
				sqlite_backend_opaque_identity output{"cxxlens.sqlite-source-id.v1", {}};
				const auto value = std::string_view{source_id};
				output.bytes.reserve(value.size());
				for (const auto character : value)
					output.bytes.push_back(
						static_cast<std::byte>(static_cast<unsigned char>(character)));
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		struct native_lifetime_receipts
		{
			sqlite_backend_opaque_identity lifetime;
			sqlite_backend_opaque_identity semantic;
			sqlite_backend_opaque_identity xopen;
			sqlite_backend_opaque_identity callback_cohort;
			sqlite_backend_opaque_identity open_epoch;
		};

		[[nodiscard]] std::optional<native_lifetime_receipts>
		make_native_lifetime_receipts(const default_forwarding_state& owner,
									  const native_file_node& node,
									  const sqlite_backend_file_role role,
									  const std::size_t event_index,
									  const int input_flags,
									  const int delegated_flags,
									  const int returned_flags,
									  const opened_object_identities& identities,
									  const std::uint64_t lifetime_sequence) noexcept;

		[[nodiscard]] bool source_shm_runtime_receipt_present(
			const sqlite_source_shm_runtime_binding& runtime) noexcept
		{
			return runtime.runtime_identity != nullptr ||
				runtime.runtime_image_identity != nullptr ||
				runtime.runtime_lifetime_identity != nullptr ||
				runtime.runtime_lifetime != nullptr || runtime.open_v2 != nullptr ||
				runtime.close_v2 != nullptr || runtime.exec != nullptr ||
				runtime.errmsg != nullptr || runtime.free_memory != nullptr ||
				runtime.source_id != nullptr || runtime.uri_parameter != nullptr ||
				runtime.uri_key != nullptr || runtime.vfs_find != nullptr ||
				runtime.vfs_register != nullptr || runtime.vfs_unregister != nullptr;
		}

		[[nodiscard]] bool complete_source_shm_runtime_receipt(
			const sqlite_source_shm_runtime_binding& runtime) noexcept
		{
			return runtime.runtime_identity != nullptr &&
				runtime.runtime_image_identity != nullptr &&
				runtime.runtime_lifetime_identity != nullptr && runtime.runtime_lifetime &&
				runtime.runtime_lifetime_identity == runtime.runtime_lifetime.get() &&
				runtime.open_v2 != nullptr && runtime.close_v2 != nullptr &&
				runtime.exec != nullptr && runtime.errmsg != nullptr &&
				runtime.free_memory != nullptr && runtime.source_id != nullptr &&
				runtime.uri_parameter != nullptr && runtime.uri_key != nullptr &&
				runtime.vfs_find != nullptr && runtime.vfs_register != nullptr &&
				runtime.vfs_unregister != nullptr;
		}

		class default_forwarding_state;

		struct default_forwarding_state_deleter
		{
			void operator()(default_forwarding_state* state) const noexcept;
		};

		void drain_deferred_forwarding_states() noexcept;
		void defer_forwarding_state(default_forwarding_state* state) noexcept;

		class default_forwarding_state final
			: public sqlite_default_forwarding_vfs,
			  public std::enable_shared_from_this<default_forwarding_state>
		{
		  public:
			static result<std::shared_ptr<default_forwarding_state>>
			create(sqlite_private_snapshot_registry_binding registry)
			{
				drain_deferred_forwarding_states();
				const auto source_runtime_present =
					source_shm_runtime_receipt_present(registry.source_shm_runtime);
				if (registry.runtime_identity == nullptr ||
					registry.pinned_default_vfs == nullptr || registry.find == nullptr ||
					registry.register_vfs == nullptr || registry.unregister_vfs == nullptr ||
					!registry.runtime_lifetime ||
					(source_runtime_present &&
					 !complete_source_shm_runtime_receipt(registry.source_shm_runtime)) ||
					(source_runtime_present &&
					 (registry.source_shm_runtime.runtime_identity != registry.runtime_identity ||
					  registry.source_shm_runtime.runtime_lifetime.get() !=
						  registry.runtime_lifetime.get() ||
					  registry.source_shm_runtime.vfs_find != registry.find ||
					  registry.source_shm_runtime.vfs_register != registry.register_vfs ||
					  registry.source_shm_runtime.vfs_unregister != registry.unregister_vfs)))
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
					underlying_image.dli_fbase == nullptr ||
					(source_runtime_present &&
					 underlying_image.dli_fbase !=
						 registry.source_shm_runtime.runtime_image_identity))
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
													 *file_offset),
						default_forwarding_state_deleter{});
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
				if (registered_alias_ || source_shm_family_ || registered_)
					std::terminate();
			}

			[[nodiscard]] bool finalize_lifetime() noexcept
			{
				if (open_file_count_.load(std::memory_order_acquire) != 0U)
					std::terminate();
				if (!registered_alias_)
				{
					if (!registered_)
						return true;
					std::scoped_lock lock{forwarding_registration_mutex};
					if (registry_.find(registered_name_.c_str()) != &wrapper_ ||
						registry_.unregister_vfs(&wrapper_) != sqlite_ok ||
						registry_.find(registered_name_.c_str()) != nullptr)
						std::terminate();
					registered_ = false;
					return true;
				}
				if (source_shm_family_)
				{
					if (!registered_alias_ || !registered_alias_->valid() ||
						registered_alias_->registry() == nullptr)
						std::terminate();
					auto released =
						registered_alias_->registry()->release_family(*source_shm_family_);
					if (!released)
					{
						if (released.error().reason == sqlite_shm_lease_rejection_reason::retiring)
							return false;
						std::terminate();
					}
					source_shm_family_.reset();
				}
				source_shm_family_binding_.reset();
				auto unregistered = registered_alias_->unregister_alias();
				if (!unregistered)
				{
					if (unregistered.error().reason == sqlite_shm_lease_rejection_reason::retiring)
						return false;
					std::terminate();
				}
				registered_alias_.reset();
				registered_ = false;
				return true;
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

			[[nodiscard]] std::optional<std::uint64_t> mint_native_lifetime_sequence() noexcept
			{
				const auto sequence =
					next_native_lifetime_sequence.fetch_add(1U, std::memory_order_relaxed);
				if (sequence == 0U || sequence == std::numeric_limits<std::uint64_t>::max())
					return std::nullopt;
				return sequence;
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
			ensure_source_shm_family(const sqlite_shm_lease_family_binding& family)
			{
				std::scoped_lock lock{source_shm_family_mutex_};
				if (!registered_alias_ || !registered_alias_->valid())
					return unexpected(forwarding_error("source-shm-readonly-family"));
				if (source_shm_family_)
				{
					if (!source_shm_family_binding_ || *source_shm_family_binding_ != family)
						return unexpected(forwarding_error("source-shm-readonly-family"));
					return {};
				}
				auto pinned =
					sqlite_same_process_shm_vfs_alias_registration_port::install_or_join_family(
						*registered_alias_, family);
				if (!pinned)
					return unexpected(forwarding_error("source-shm-readonly-family"));
				source_shm_family_.emplace(std::move(*pinned));
				source_shm_family_binding_ = family;
				return {};
			}

			[[nodiscard]] result<void> install_current_v3_writer_eligibility(
				default_connection_observation& observation,
				const sqlite_backend_effect_arm_receipt& effect_receipt);
			[[nodiscard]] result<void>
			promote_current_v3_writer_pending(default_connection_observation& observation);
			[[nodiscard]] result<void> arm_writer_shm_mapping_epoch(
				default_connection_observation& observation,
				std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch,
				sqlite_backend_opaque_identity sqlite_source_id);
			[[nodiscard]] result<void> arm_requested_writer_epoch_before_native_map(
				default_connection_observation& observation);
			[[nodiscard]] int remove_writer_shm_sidecar() noexcept;
			[[nodiscard]] result<void> ensure_writer_file_family(
				const sqlite_source_shm_target_namespace_epoch& target_namespace_epoch);
			[[nodiscard]] result<void>
			revoke_writer_eligibility(default_connection_observation& observation) noexcept;
			[[nodiscard]] result<void> acquire_source_reader_open(
				default_connection_observation& observation,
				native_file_node& node,
				std::optional<sqlite_shm_reader_attachment_target_identity> target_identity);
			[[nodiscard]] result<void> release_source_reader_open(native_file_node& node) noexcept;
			[[nodiscard]] std::optional<source_shm_reader_registry_context>
			source_shm_reader_context() noexcept
			{
				try
				{
					std::scoped_lock lock{source_shm_family_mutex_};
					if (!source_shm_family_ || !source_shm_family_binding_ || !registered_alias_ ||
						!registered_alias_->valid() || registered_alias_->registry() == nullptr ||
						!source_shm_family_->valid())
						return std::nullopt;
					return source_shm_reader_registry_context{
						registered_alias_->registry(),
						&*source_shm_family_,
						&*source_shm_family_binding_,
						&registered_alias_->alias_lifetime(),
						&registered_alias_->registration_epoch()};
				}
				catch (...)
				{
					return std::nullopt;
				}
			}

			[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
			source_shm_registration_epoch() noexcept
			{
				try
				{
					std::scoped_lock lock{source_shm_family_mutex_};
					if (!registered_alias_ || !registered_alias_->valid())
						return std::nullopt;
					return registered_alias_->registration_epoch();
				}
				catch (...)
				{
					return std::nullopt;
				}
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
						plan.qualification.exact_file_family.profile.empty() ||
						plan.qualification.exact_file_family.bytes.empty() ||
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
					std::array<sqlite_backend_entry_observation, 4U> family_entries;
					constexpr std::array family_roles{
						sqlite_backend_file_role::main_database,
						sqlite_backend_file_role::write_ahead_log,
						sqlite_backend_file_role::shared_memory,
						sqlite_backend_file_role::rollback_journal,
					};
					for (std::size_t index{}; index < family_roles.size(); ++index)
					{
						auto retained = plan.qualification.target_namespace_epoch->retained_entry(
							family_roles[index]);
						if (!retained)
							return unexpected(forwarding_error("source-shm-readonly-arm"));
						family_entries[index] = std::move(*retained);
					}
					auto exact_file_family = seal_sqlite_source_shm_exact_file_family(
						plan.canonical_vfs_locator,
						plan.qualification.parent_namespace_identity,
						source_id,
						family_entries);
					if (!exact_file_family ||
						*exact_file_family != plan.qualification.exact_file_family)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					if (!registered_alias_)
						return unexpected(forwarding_error("source-shm-readonly-arm"));
					const sqlite_shm_lease_family_binding family{
						registered_alias_->process_instance(),
						registered_alias_->shared_runtime_vfs_cohort(),
						std::move(*exact_file_family),
					};
					if (auto installed = ensure_source_shm_family(family); !installed)
						return installed;
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
						const auto active_writer_overlap =
							existing->canonical_locator == canonical_locator &&
							existing->main_handle_open && !existing->invalid &&
							existing->effect_gate != nullptr &&
							existing->effect_gate->stage() ==
								sqlite_backend_effect_stage::fully_armed &&
							!existing->source_shm_open_plan &&
							!existing->source_shm_qualification_open_plan &&
							!existing->source_shm_qualification_fixture_fullpath_plan &&
							!existing->source_shm_qualification_fixture_pending_open_plan;
						if (existing->canonical_locator == canonical_locator &&
							existing->originating_thread == observation->originating_thread &&
							(!existing->main_claimed || existing->main_handle_open) &&
							!active_writer_overlap)
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
						const auto active_writer_overlap =
							existing->canonical_locator == canonical_locator_ &&
							existing->main_handle_open && !existing->invalid &&
							existing->effect_gate != nullptr &&
							existing->effect_gate->stage() ==
								sqlite_backend_effect_stage::fully_armed &&
							!existing->source_shm_open_plan &&
							!existing->source_shm_qualification_open_plan &&
							!existing->source_shm_qualification_fixture_fullpath_plan &&
							!existing->source_shm_qualification_fixture_pending_open_plan;
						if (existing->originating_thread == observation->originating_thread &&
							(!existing->main_claimed || existing->main_handle_open) &&
							!active_writer_overlap)
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
				const auto source_runtime_present =
					source_shm_runtime_receipt_present(registry_.source_shm_runtime);
				if (!source_runtime_present)
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
				if (registry_.source_shm_runtime.runtime_image_identity !=
					underlying_image_identity_)
					return unexpected(forwarding_error("forwarding-vfs-register"));
				auto process = sqlite_same_process_shm_process_port::acquire();
				if (!process)
					return unexpected(forwarding_error("forwarding-vfs-process-registry"));
				for (std::size_t attempt{}; attempt < 32U; ++attempt)
				{
					const auto value =
						next_forwarding_name.fetch_add(1U, std::memory_order_relaxed);
					registered_name_ = "cxxlens-default-forwarding-v1-" +
						std::to_string(reinterpret_cast<std::uintptr_t>(this)) + "-" +
						std::to_string(value);
					wrapper_.name = registered_name_.c_str();
					auto sealed = sqlite_same_process_shm_vfs_alias_identity_sealer::seal(
						sqlite_shm_vfs_alias_identity_sealing_input{
							*process,
							registry_.source_shm_runtime,
							underlying_,
							underlying_app_data_identity_,
							underlying_open_callback_address_,
							this,
							registered_name_,
							&wrapper_,
						});
					if (!sealed)
						return unexpected(forwarding_error("forwarding-vfs-register"));
					auto registered =
						sqlite_same_process_shm_vfs_alias_registration_port::register_alias(
							std::move(*sealed));
					if (!registered)
					{
						if (registered.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request)
							continue;
						return unexpected(forwarding_error("forwarding-vfs-register"));
					}
					registered_alias_.emplace(std::move(*registered));
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
			std::optional<sqlite_shm_registered_vfs_alias> registered_alias_;
			std::mutex source_shm_family_mutex_;
			std::optional<sqlite_shm_registry_family_pin> source_shm_family_;
			std::optional<sqlite_shm_lease_family_binding> source_shm_family_binding_;
			bool registered_{};
		};

		struct deferred_forwarding_state_queue
		{
			std::mutex mutex;
			std::vector<default_forwarding_state*> pending;
			bool draining{};
		};

		[[nodiscard]] deferred_forwarding_state_queue& deferred_forwarding_states() noexcept
		{
			static auto* queue = new deferred_forwarding_state_queue;
			return *queue;
		}

		void defer_forwarding_state(default_forwarding_state* const state) noexcept
		{
			if (state == nullptr)
				std::terminate();
			try
			{
				auto& queue = deferred_forwarding_states();
				std::scoped_lock lock{queue.mutex};
				queue.pending.push_back(state);
			}
			catch (...)
			{
				std::terminate();
			}
		}

		void drain_deferred_forwarding_states() noexcept
		{
			auto& queue = deferred_forwarding_states();
			{
				std::scoped_lock lock{queue.mutex};
				if (queue.draining)
					return;
				queue.draining = true;
			}

			for (;;)
			{
				std::vector<default_forwarding_state*> current;
				{
					std::scoped_lock lock{queue.mutex};
					if (queue.pending.empty())
					{
						queue.draining = false;
						return;
					}
					current.swap(queue.pending);
				}

				std::vector<default_forwarding_state*> retry;
				retry.reserve(current.size());
				bool progress{};
				for (auto* const state : current)
				{
					if (state->finalize_lifetime())
					{
						delete state;
						progress = true;
					}
					else
						retry.push_back(state);
				}
				{
					std::scoped_lock lock{queue.mutex};
					queue.pending.insert(queue.pending.end(), retry.begin(), retry.end());
					if (!progress)
					{
						queue.draining = false;
						return;
					}
				}
			}
		}

		void default_forwarding_state_deleter::operator()(
			default_forwarding_state* const state) const noexcept
		{
			if (state == nullptr)
				return;
			if (state->finalize_lifetime())
			{
				delete state;
				drain_deferred_forwarding_states();
				return;
			}
			defer_forwarding_state(state);
			drain_deferred_forwarding_states();
		}

		[[nodiscard]] std::optional<native_lifetime_receipts>
		make_native_lifetime_receipts(const default_forwarding_state& owner,
									  const native_file_node& node,
									  const sqlite_backend_file_role role,
									  const std::size_t event_index,
									  const int input_flags,
									  const int delegated_flags,
									  const int returned_flags,
									  const opened_object_identities& identities,
									  const std::uint64_t lifetime_sequence) noexcept
		{
			if ((role != sqlite_backend_file_role::main_database &&
				 role != sqlite_backend_file_role::write_ahead_log) ||
				lifetime_sequence == 0U)
				return std::nullopt;
			try
			{
				const auto role_value = static_cast<std::uint8_t>(role);
				sqlite_backend_opaque_identity lifetime{"cxxlens.sqlite-native-file-lifetime.v1",
														{}};
				lifetime.bytes.reserve(256U + identities.object.bytes.size() +
									   identities.entry.bytes.size());
				append_u64(lifetime.bytes, lifetime_sequence);
				append_pointer(lifetime.bytes, owner.vfs_implementation_identity());
				append_pointer(lifetime.bytes, owner.underlying());
				append_pointer(lifetime.bytes, owner.underlying_image_identity());
				append_pointer(lifetime.bytes, owner.underlying_open_callback_address());
				append_pointer(lifetime.bytes, node.underlying_app_data_identity);
				append_pointer(lifetime.bytes, &node);
				append_u64(lifetime.bytes, static_cast<std::uint64_t>(event_index));
				lifetime.bytes.push_back(std::byte{role_value});
				append_opaque_identity(lifetime.bytes, identities.object);
				append_opaque_identity(lifetime.bytes, identities.entry);

				sqlite_backend_opaque_identity semantic{"cxxlens.sqlite-native-file-semantic.v1",
														{}};
				semantic.bytes.reserve(128U + identities.object.bytes.size() +
									   identities.entry.bytes.size());
				semantic.bytes.push_back(std::byte{role_value});
				append_u64(semantic.bytes, static_cast<std::uint64_t>(input_flags));
				append_u64(semantic.bytes, static_cast<std::uint64_t>(returned_flags));
				append_opaque_identity(semantic.bytes, identities.object);
				append_opaque_identity(semantic.bytes, identities.entry);

				sqlite_backend_opaque_identity xopen{"cxxlens.sqlite-native-xopen-receipt.v1", {}};
				xopen.bytes.reserve(160U);
				append_u64(xopen.bytes, static_cast<std::uint64_t>(event_index));
				append_u64(xopen.bytes, static_cast<std::uint64_t>(input_flags));
				append_u64(xopen.bytes, static_cast<std::uint64_t>(delegated_flags));
				append_u64(xopen.bytes, static_cast<std::uint64_t>(returned_flags));
				xopen.bytes.push_back(std::byte{role_value});
				append_pointer(xopen.bytes, owner.underlying());
				append_pointer(xopen.bytes, owner.underlying_image_identity());
				append_pointer(xopen.bytes, owner.underlying_open_callback_address());
				append_pointer(xopen.bytes, node.underlying_app_data_identity);

				sqlite_backend_opaque_identity callback_cohort{
					"cxxlens.sqlite-native-callback-cohort.v1", {}};
				callback_cohort.bytes.reserve(96U);
				append_pointer(callback_cohort.bytes, owner.vfs_implementation_identity());
				append_pointer(callback_cohort.bytes, owner.underlying());
				append_pointer(callback_cohort.bytes, owner.underlying_open_callback_address());
				append_pointer(callback_cohort.bytes, node.underlying_app_data_identity);
				append_pointer(callback_cohort.bytes, &node);
				append_u64(callback_cohort.bytes, static_cast<std::uint64_t>(event_index));

				sqlite_backend_opaque_identity open_epoch{"cxxlens.sqlite-native-open-epoch.v1",
														  {}};
				open_epoch.bytes.reserve(lifetime.bytes.size() + xopen.bytes.size());
				append_opaque_identity(open_epoch.bytes, lifetime);
				append_opaque_identity(open_epoch.bytes, xopen);
				return native_lifetime_receipts{std::move(lifetime),
												std::move(semantic),
												std::move(xopen),
												std::move(callback_cohort),
												std::move(open_epoch)};
			}
			catch (const std::bad_alloc&)
			{
				return std::nullopt;
			}
			catch (const std::length_error&)
			{
				return std::nullopt;
			}
		}

		result<void> default_forwarding_state::install_current_v3_writer_eligibility(
			default_connection_observation& observation,
			const sqlite_backend_effect_arm_receipt& effect_receipt)
		{
			try
			{
				std::scoped_lock family_lock{source_shm_family_mutex_};
				// A newly-created database has no authenticated four-file family until the
				// first WAL qualification cut. It remains on the existing fail-closed route;
				// no synthetic eligibility is installed for it.
				if (!source_shm_family_ || !source_shm_family_binding_)
					return {};
				if (!registered_alias_ || !registered_alias_->valid() ||
					registered_alias_->registry() == nullptr)
					return unexpected(forwarding_error("source-shm-writer-eligibility"));

				std::optional<sqlite_backend_opaque_identity> open_epoch;
				sqlite_backend_opaque_identity connection_token;
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.writer_eligibility)
						return {};
					open_epoch = observation.main_open_epoch;
					connection_token = observation.connection_token_value;
					if (!open_epoch || connection_token.profile.empty() ||
						connection_token.bytes.empty())
						return unexpected(forwarding_error("source-shm-writer-eligibility"));
				}

				auto sealed = sqlite_shm_writer_eligibility_receipt_production_factory::seal(
					*source_shm_family_binding_,
					std::move(connection_token),
					std::move(*open_epoch),
					effect_receipt);
				if (!sealed)
				{
					return unexpected(forwarding_error("source-shm-writer-eligibility"));
				}
				auto installed = registered_alias_->registry()->install_writer_eligibility(
					*source_shm_family_, *sealed);
				if (!installed)
				{
					return unexpected(forwarding_error("source-shm-writer-eligibility"));
				}
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.writer_eligibility)
					{
						(void)registered_alias_->registry()->revoke_writer_eligibility(
							*source_shm_family_, *installed);
						return unexpected(forwarding_error("source-shm-writer-eligibility"));
					}
					observation.writer_eligibility.emplace(std::move(*installed));
					observation.writer_effect_gate_receipt = effect_receipt.prerequisite_receipt;
					observation.writer_effect_receipt = effect_receipt.validation_receipt;
				}
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-writer-eligibility"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-writer-eligibility"));
			}
		}

		result<void> default_forwarding_state::promote_current_v3_writer_pending(
			default_connection_observation& observation)
		{
			try
			{
				auto node = observation.main_native_node.lock();
				if (!node)
					return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
				std::unique_lock lifecycle_lock{node->writer_lifecycle_mutex, std::try_to_lock};
				if (!lifecycle_lock.owns_lock() || node->writer_holders.size() != 0U)
					return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
				if (node->writer_pending.empty())
					return {};

				const auto context = source_shm_reader_context();
				if (!context || !context->registry || !context->family ||
					!context->family_binding || !context->alias_lifetime ||
					!node->writer_pending_attachment)
					return unexpected(forwarding_error("source-shm-writer-pending-promotion"));

				std::vector<sqlite_shm_pending_mapping*> pending;
				pending.reserve(node->writer_pending.size());
				for (auto& mapping : node->writer_pending)
				{
					if (!mapping.valid())
						return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
					pending.push_back(&mapping);
				}
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || !observation.main_handle_open ||
						!observation.writer_eligibility ||
						!observation.writer_target_namespace_epoch ||
						observation.writer_target_namespace_epoch != node->target_namespace_epoch)
						return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
					auto promoted = context->registry->advance_positive_writer_attachment_gate(
						*context->family,
						*node->writer_pending_attachment,
						std::span<sqlite_shm_pending_mapping*>{pending.data(), pending.size()},
						*observation.writer_eligibility);
					if (!promoted ||
						promoted->progress !=
							sqlite_shm_positive_writer_attachment_gate_progress::complete ||
						promoted->holders.size() != pending.size())
					{
						return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
					}
					node->writer_holders.swap(promoted->holders);
					node->writer_pending.clear();
					node->writer_pending_attachment.reset();
				}
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
			}
			catch (...)
			{
				return unexpected(forwarding_error("source-shm-writer-pending-promotion"));
			}
		}

		result<void> default_forwarding_state::arm_writer_shm_mapping_epoch(
			default_connection_observation& observation,
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch,
			sqlite_backend_opaque_identity sqlite_source_id)
		{
			try
			{
				const auto reject = []() -> result<void>
				{
					return unexpected(forwarding_error("source-shm-writer-epoch-arm"));
				};
				if (!target_namespace_epoch || !valid_opaque_identity(sqlite_source_id) ||
					target_namespace_epoch->logical_main_locator() !=
						observation.canonical_locator ||
					target_namespace_epoch->anchored_main_locator().empty() ||
					!valid_opaque_identity(target_namespace_epoch->identity()) ||
					!valid_opaque_identity(target_namespace_epoch->parent_namespace_identity()) ||
					!target_namespace_epoch->recheck())
					return reject();
				const auto expected_source_id =
					make_sqlite_source_id_identity(registry_.source_shm_runtime.source_id());
				if (!expected_source_id || sqlite_source_id != *expected_source_id)
					return reject();
				std::shared_ptr<native_file_node> main_node;
				std::shared_ptr<native_file_node> wal_node;
				bool attach_to_open_connection{};
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid)
						return reject();
					if (observation.writer_target_namespace_epoch)
						return reject();
					if (observation.writer_sqlite_source_id)
						return reject();
					attach_to_open_connection = observation.main_handle_open;
					if (attach_to_open_connection)
					{
						if (observation.main_handle_read_only ||
							observation.shm_map_events.size() != 0U ||
							!observation.main_native_file_receipt ||
							!observation.main_native_xopen_receipt ||
							!observation.main_callback_cohort || !observation.main_open_epoch ||
							!observation.main_native_node.lock())
							return reject();
						main_node = observation.main_native_node.lock();
						wal_node = observation.wal_native_node.lock();
					}
				}
				if (auto family = ensure_writer_file_family(*target_namespace_epoch); !family)
					return family;
				if (attach_to_open_connection)
				{
					if (!main_node || (wal_node && main_node == wal_node))
						return reject();
					std::unique_lock main_lock{main_node->writer_lifecycle_mutex};
					if (main_node->target_namespace_epoch || !main_node->writer_lifetime_source ||
						!main_node->writer_lifetime_source->valid())
						return reject();
					std::optional<std::unique_lock<std::mutex>> wal_lock;
					if (wal_node)
					{
						wal_lock.emplace(wal_node->writer_lifecycle_mutex);
						if (wal_node->target_namespace_epoch || !wal_node->writer_lifetime_source ||
							!wal_node->writer_lifetime_source->valid())
							return reject();
					}
					main_node->target_namespace_epoch = target_namespace_epoch;
					main_node->writer_target_namespace_epoch_owner = true;
					if (wal_node)
						wal_node->target_namespace_epoch = target_namespace_epoch;
				}
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || observation.writer_target_namespace_epoch ||
						observation.writer_sqlite_source_id ||
						(attach_to_open_connection != observation.main_handle_open))
						return reject();
					observation.writer_target_namespace_epoch = std::move(target_namespace_epoch);
					observation.writer_sqlite_source_id = std::move(sqlite_source_id);
				}
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-arm"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-arm"));
			}
			catch (...)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-arm"));
			}
		}

		result<void> default_forwarding_state::arm_requested_writer_epoch_before_native_map(
			default_connection_observation& observation)
		{
			try
			{
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || !observation.writer_shm_mapping_epoch_requested ||
						observation.writer_target_namespace_epoch ||
						observation.writer_sqlite_source_id || !observation.main_handle_open ||
						observation.main_handle_read_only || observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan ||
						observation.source_shm_qualification_fixture_fullpath_plan ||
						observation.source_shm_qualification_fixture_pending_open_plan)
						return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
				}
				auto capability = observation_capability_.lock();
				if (!capability || canonical_locator_.empty())
					return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
				auto census = capability->capture_namespace(canonical_locator_);
				if (!census)
					return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
				auto target_namespace_epoch =
					make_sqlite_source_shm_target_namespace_epoch(canonical_locator_, *census);
				if (!target_namespace_epoch)
					return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
				auto sqlite_source_id =
					make_sqlite_source_id_identity(registry_.source_shm_runtime.source_id());
				if (!sqlite_source_id)
					return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
				return arm_writer_shm_mapping_epoch(
					observation, std::move(*target_namespace_epoch), std::move(*sqlite_source_id));
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
			}
			catch (...)
			{
				return unexpected(forwarding_error("source-shm-writer-epoch-pre-map"));
			}
		}

		int default_forwarding_state::remove_writer_shm_sidecar() noexcept
		{
			try
			{
				if (canonical_locator_.empty() || canonical_locator_.contains('\0'))
					return sqlite_io_error;
				std::string shared_memory_path{canonical_locator_};
				shared_memory_path.append(shm_suffix);
				return forwarding_vfs_remove(&wrapper_, shared_memory_path.c_str(), 1);
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		result<void> default_forwarding_state::ensure_writer_file_family(
			const sqlite_source_shm_target_namespace_epoch& target_namespace_epoch)
		{
			try
			{
				std::scoped_lock lock{source_shm_family_mutex_};
				if (source_shm_family_ && source_shm_family_binding_)
					return {};
				if (!registered_alias_ || !registered_alias_->valid() ||
					registered_alias_->registry() == nullptr ||
					registry_.source_shm_runtime.source_id == nullptr)
					return unexpected(forwarding_error("source-shm-writer-family"));
				constexpr std::array roles{
					sqlite_backend_file_role::main_database,
					sqlite_backend_file_role::write_ahead_log,
					sqlite_backend_file_role::shared_memory,
					sqlite_backend_file_role::rollback_journal,
				};
				std::array<sqlite_backend_entry_observation, roles.size()> entries;
				for (std::size_t index{}; index < roles.size(); ++index)
				{
					auto retained = target_namespace_epoch.retained_entry(roles[index]);
					if (!retained)
						return unexpected(forwarding_error("source-shm-writer-family"));
					entries[index] = std::move(*retained);
				}
				auto exact_file_family = seal_sqlite_source_shm_exact_file_family(
					canonical_locator_,
					target_namespace_epoch.parent_namespace_identity(),
					registry_.source_shm_runtime.source_id(),
					std::span<const sqlite_backend_entry_observation>{entries});
				if (!exact_file_family)
					return unexpected(forwarding_error("source-shm-writer-family"));
				const sqlite_shm_lease_family_binding family{
					registered_alias_->process_instance(),
					registered_alias_->shared_runtime_vfs_cohort(),
					std::move(*exact_file_family)};
				if (!registered_alias_->registry())
					return unexpected(forwarding_error("source-shm-writer-family"));
				auto installed =
					sqlite_same_process_shm_vfs_alias_registration_port::install_or_join_family(
						*registered_alias_, family);
				if (!installed)
					return unexpected(forwarding_error("source-shm-writer-family"));
				source_shm_family_.emplace(std::move(*installed));
				source_shm_family_binding_ = family;
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-writer-family"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-writer-family"));
			}
		}

		result<void> default_forwarding_state::revoke_writer_eligibility(
			default_connection_observation& observation) noexcept
		{
			try
			{
				std::scoped_lock family_lock{source_shm_family_mutex_};
				std::optional<sqlite_shm_writer_eligibility> eligibility;
				{
					std::scoped_lock lock{observation.mutex};
					if (!observation.writer_eligibility)
						return {};
					eligibility.emplace(std::move(*observation.writer_eligibility));
					observation.writer_eligibility.reset();
				}
				if (!source_shm_family_ || !registered_alias_ || !registered_alias_->valid() ||
					registered_alias_->registry() == nullptr)
				{
					return unexpected(forwarding_error("source-shm-writer-eligibility-revoke"));
				}
				auto revoked = registered_alias_->registry()->revoke_writer_eligibility(
					*source_shm_family_, *eligibility);
				if (!revoked)
				{
					return unexpected(forwarding_error("source-shm-writer-eligibility-revoke"));
				}
				return {};
			}
			catch (...)
			{
				return unexpected(forwarding_error("source-shm-writer-eligibility-revoke"));
			}
		}

		result<void> default_forwarding_state::acquire_source_reader_open(
			default_connection_observation& observation,
			native_file_node& node,
			std::optional<sqlite_shm_reader_attachment_target_identity> target_identity)
		{
			try
			{
				std::scoped_lock family_lock{source_shm_family_mutex_};
				{
					std::scoped_lock lock{observation.mutex};
					if (!observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan)
						return {};
					if (observation.invalid || node.reader_open_authority ||
						!observation.main_native_file_receipt ||
						!observation.main_native_xopen_receipt ||
						!observation.main_callback_cohort || !observation.main_open_epoch)
						return unexpected(forwarding_error("source-shm-reader-open"));
				}
				if (!source_shm_family_ || !source_shm_family_binding_ || !registered_alias_ ||
					!registered_alias_->valid() || registered_alias_->registry() == nullptr ||
					!source_shm_family_->valid())
					return unexpected(forwarding_error("source-shm-reader-open"));
				const auto registration_epoch = registered_alias_->registration_epoch();
				if (!node.registration_epoch || *node.registration_epoch != registration_epoch)
					return unexpected(forwarding_error("source-shm-reader-open"));

				sqlite_shm_reader_open_binding binding;
				{
					std::scoped_lock lock{observation.mutex};
					binding = {*source_shm_family_binding_,
							   registered_alias_->alias_lifetime(),
							   observation.connection_token_value,
							   *observation.main_native_file_receipt,
							   *observation.main_native_xopen_receipt,
							   *observation.main_open_epoch,
							   *observation.main_callback_cohort,
							   std::move(target_identity),
							   registration_epoch};
				}
				auto acquired = sqlite_shm_reader_open_production_factory::acquire(
					*registered_alias_->registry(), *source_shm_family_, binding);
				if (!acquired)
					return unexpected(forwarding_error("source-shm-reader-open"));
				node.reader_open_authority.emplace(std::move(*acquired));
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(forwarding_error("source-shm-reader-open"));
			}
			catch (const std::length_error&)
			{
				return unexpected(forwarding_error("source-shm-reader-open"));
			}
		}

		result<void>
		default_forwarding_state::release_source_reader_open(native_file_node& node) noexcept
		{
			try
			{
				if (!node.reader_open_authority)
					return {};
				std::scoped_lock family_lock{source_shm_family_mutex_};
				if (!source_shm_family_ || !registered_alias_ || !registered_alias_->valid() ||
					registered_alias_->registry() == nullptr)
					return unexpected(forwarding_error("source-shm-reader-open-release"));
				auto released =
					registered_alias_->registry()->release_reader_open(*node.reader_open_authority);
				if (!released)
					return unexpected(forwarding_error("source-shm-reader-open-release"));
				node.reader_open_authority.reset();
				return {};
			}
			catch (...)
			{
				return unexpected(forwarding_error("source-shm-reader-open-release"));
			}
		}

		result<void> default_connection_observation::install_current_v3_writer_eligibility()
		{
			auto owner_pin = owner.lock();
			if (!owner_pin || effect_gate == nullptr)
				return unexpected(forwarding_error("source-shm-writer-eligibility"));
			auto effect_receipt = effect_gate->latest_receipt();
			if (!effect_receipt ||
				effect_receipt->stage != sqlite_backend_effect_stage::fully_armed)
				return unexpected(forwarding_error("source-shm-writer-eligibility"));
			if (auto installed =
					owner_pin->install_current_v3_writer_eligibility(*this, *effect_receipt);
				!installed)
				return installed;
			return owner_pin->promote_current_v3_writer_pending(*this);
		}

		result<void> default_connection_observation::arm_writer_shm_mapping_epoch(
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch,
			sqlite_backend_opaque_identity sqlite_source_id)
		{
			auto owner_pin = owner.lock();
			if (!owner_pin)
				return unexpected(forwarding_error("source-shm-writer-epoch-arm"));
			return owner_pin->arm_writer_shm_mapping_epoch(
				*this, std::move(target_namespace_epoch), std::move(sqlite_source_id));
		}

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

		/**
		 * Revalidate the complete native callback boundary immediately around a qualified SHM
		 * delegation.  The forwarding VFS intentionally caches the trusted io-method table so a
		 * malformed native table cannot be re-read later, but that cache must not hide a live VFS,
		 * registration, or callback replacement.  A changed raw methods pointer is likewise a
		 * terminal identity drift even when its replacement happens to point into the same image.
		 */
		[[nodiscard]] bool
		native_shm_callback_identity_valid(native_file_node& node,
										   const bool allow_closed_file = false) noexcept
		{
			try
			{
				if (!node.owner || !node.trusted_methods_ready || node.underlying == nullptr)
					return false;
				const auto current_registration_epoch = node.owner->source_shm_registration_epoch();
				if (!current_registration_epoch)
					return false;
				if (!node.registration_epoch ||
					*node.registration_epoch != *current_registration_epoch)
					return false;
				if (node.underlying_vfs_identity != node.underlying ||
					node.underlying != node.owner->underlying() ||
					node.underlying->version != node.underlying_vfs_version ||
					node.underlying->app_data != node.underlying_app_data_identity ||
					node.underlying->open == nullptr ||
					function_address(node.underlying->open) !=
						node.underlying_open_callback_address ||
					node.underlying->full_pathname == nullptr ||
					function_address(node.underlying->full_pathname) !=
						node.underlying_full_pathname_callback_address)
					return false;

				const auto* wrapper =
					static_cast<const sqlite3_vfs*>(node.owner->vfs_implementation_identity());
				const auto& registry = node.owner->registry();
				const auto expected_version = std::min(node.underlying_vfs_version, 3);
				if (wrapper == nullptr || wrapper->version != expected_version ||
					wrapper->app_data != node.owner->backend_lifetime_identity() ||
					wrapper->name == nullptr ||
					std::string_view{wrapper->name} != node.owner->registered_vfs_name() ||
					registry.find == nullptr ||
					registry.find(node.owner->registered_vfs_name().data()) != wrapper ||
					wrapper->open != forwarding_vfs_open ||
					wrapper->full_pathname != forwarding_vfs_full_pathname ||
					wrapper->remove !=
						(node.underlying->remove != nullptr ? forwarding_vfs_remove : nullptr) ||
					wrapper->access !=
						(node.underlying->access != nullptr ? forwarding_vfs_access : nullptr) ||
					wrapper->dl_open !=
						(node.underlying->dl_open != nullptr ? forwarding_vfs_dl_open : nullptr) ||
					wrapper->dl_error !=
						(node.underlying->dl_error != nullptr ? forwarding_vfs_dl_error
															  : nullptr) ||
					wrapper->dl_sym !=
						(node.underlying->dl_sym != nullptr ? forwarding_vfs_dl_sym : nullptr) ||
					wrapper->dl_close !=
						(node.underlying->dl_close != nullptr ? forwarding_vfs_dl_close
															  : nullptr) ||
					wrapper->randomness !=
						(node.underlying->randomness != nullptr ? forwarding_vfs_randomness
																: nullptr) ||
					wrapper->sleep !=
						(node.underlying->sleep != nullptr ? forwarding_vfs_sleep : nullptr) ||
					wrapper->current_time !=
						(node.underlying->current_time != nullptr ? forwarding_vfs_current_time
																  : nullptr) ||
					wrapper->get_last_error !=
						(node.underlying->get_last_error != nullptr ? forwarding_vfs_last_error
																	: nullptr) ||
					wrapper->current_time_int64 !=
						(expected_version >= 2 && node.underlying->current_time_int64 != nullptr
							 ? forwarding_vfs_current_time_int64
							 : nullptr) ||
					wrapper->set_system_call !=
						(expected_version >= 3 && node.underlying->set_system_call != nullptr
							 ? forwarding_vfs_set_system_call
							 : nullptr) ||
					wrapper->get_system_call !=
						(expected_version >= 3 && node.underlying->get_system_call != nullptr
							 ? forwarding_vfs_get_system_call
							 : nullptr) ||
					wrapper->next_system_call !=
						(expected_version >= 3 && node.underlying->next_system_call != nullptr
							 ? forwarding_vfs_next_system_call
							 : nullptr))
					return false;

				const auto* raw = const_cast<native_file_node&>(node).file();
				if (raw == nullptr)
					return false;
				const auto* methods = raw->methods;
				if (methods == nullptr)
					return allow_closed_file;
				const auto inspected = inspect_native_methods(node);
				if (methods != node.underlying_methods_identity ||
					methods->version != node.underlying_methods_version ||
					methods->close != node.trusted_methods.close ||
					methods->close != node.trusted_close ||
					inspected.forwarding_methods == nullptr ||
					methods->shm_map != node.trusted_methods.shm_map ||
					methods->shm_lock != node.trusted_methods.shm_lock ||
					methods->shm_barrier != node.trusted_methods.shm_barrier ||
					methods->shm_unmap != node.trusted_methods.shm_unmap)
					return false;
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] int close_source_reader_native(std::shared_ptr<native_file_node>& node,
													 int (*close_callback)(sqlite3_file*)) noexcept;

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
				if (!node->writer_holders.empty() || !node->writer_pending.empty())
				{
					// xClose cannot revoke a live or pending writer attachment. The caller must
					// first complete the exact xShmUnmap cleanup boundary.
					quarantine_native_file(node);
					return sqlite_io_error;
				}
				if (node->writer_lifetime_revoker && !node->writer_lifetime_revoker->revoke())
				{
					// Native close is never entered after the source-private lifetime cut
					// became ambiguous. The node remains quarantined and cannot be retried.
					quarantine_native_file(node);
					return sqlite_io_error;
				}
				if (node->writer_retained_parent_revoker &&
					!node->writer_retained_parent_revoker->revoke())
				{
					quarantine_native_file(node);
					return sqlite_io_error;
				}
				if (node->writer_shm_attachment_revoker &&
					!node->writer_shm_attachment_revoker->revoke())
				{
					quarantine_native_file(node);
					return sqlite_io_error;
				}
				node->writer_lifetime_source.reset();
				node->writer_retained_parent_source.reset();
				node->writer_shm_attachment_source.reset();
				if (node->reader_open_authority)
				{
					const auto status = close_source_reader_native(node, close_callback);
					if (status != sqlite_ok)
						quarantine_native_file(node);
					else
						release_known_safe_native_file(node);
					return status;
				}
				const auto status = close_callback(node->file());
				if (status == sqlite_ok)
				{
					if (node->writer_target_namespace_epoch_owner)
					{
						if (!node->target_namespace_epoch ||
							!node->target_namespace_epoch->finish())
						{
							quarantine_native_file(node);
							return sqlite_io_error;
						}
						node->writer_target_namespace_epoch_owner = false;
						node->target_namespace_epoch.reset();
					}
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
			if (file->main_handle && owner && observation)
			{
				const auto revoked = owner->revoke_writer_eligibility(*observation);
				if (!revoked)
				{
					mark_incomplete(observation);
					quarantine_native_file(file->native);
					if (owner)
						owner->decrement_open_file_count();
					file->~forwarding_file();
					return sqlite_io_error;
				}
			}
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

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_source_reader_identity(default_forwarding_state& owner,
									const native_file_node& node,
									const std::string_view purpose) noexcept
		{
			try
			{
				const auto sequence = owner.mint_native_lifetime_sequence();
				if (!sequence)
					return std::nullopt;
				sqlite_backend_opaque_identity identity;
				identity.profile = std::string{source_shm_profile};
				append_pointer(identity.bytes, &node);
				append_pointer(identity.bytes, owner.underlying());
				append_u64(identity.bytes, *sequence);
				append_bytes(identity.bytes, purpose);
				return identity;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_source_reader_lifecycle_request_seal(default_forwarding_state& owner,
												  const native_file_node& node,
												  const std::string_view purpose,
												  const std::uint64_t owner_token,
												  const std::uint64_t generation) noexcept
		{
			try
			{
				auto identity = make_source_reader_identity(owner, node, purpose);
				if (!identity)
					return std::nullopt;
				append_u64(identity->bytes, owner_token);
				append_u64(identity->bytes, generation);
				return identity;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool
		complete_source_reader_session(default_forwarding_state& owner,
									   native_file_node& node,
									   const source_shm_reader_registry_context& context,
									   const sqlite_shm_reader_session_terminal_kind kind,
									   const bool authority_read_closed,
									   const bool no_live_shm_lock) noexcept
		{
			const auto has_session =
				node.reader_session.has_value() || node.reader_session_request.has_value();
			if (!has_session)
				return true;
			if (!node.reader_session || !node.reader_session_request ||
				!node.reader_session->valid() || context.registry == nullptr ||
				context.family == nullptr)
				return false;
			try
			{
				const auto request = *node.reader_session_request;
				auto request_seal = make_source_reader_lifecycle_request_seal(
					owner,
					node,
					"reader-session-terminal",
					request.attachment.registry_open_token(),
					request.attachment.writer_mapping_generation());
				if (!request_seal)
					return false;
				auto scope = sqlite_shm_reader_lifecycle_production_factory::seal_session_scope(
					*context.registry,
					*context.family,
					*node.reader_session,
					request,
					*request_seal);
				if (!scope.valid())
					return false;
				auto issuer = sqlite_shm_reader_lifecycle_production_factory::identity_issuer(
					*context.registry);
				if (!issuer.valid())
					return false;

				sqlite_shm_reader_session_terminal_identity_role role;
				switch (kind)
				{
					case sqlite_shm_reader_session_terminal_kind::success:
						role = sqlite_shm_reader_session_terminal_identity_role::success;
						break;
					case sqlite_shm_reader_session_terminal_kind::failure:
						role = sqlite_shm_reader_session_terminal_identity_role::failure;
						break;
					case sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read:
						role = sqlite_shm_reader_session_terminal_identity_role::
							cancelled_before_authority_read;
						break;
				}
				auto terminal_result = issuer.issue_session_terminal(scope, role);
				if (!terminal_result)
					return false;
				auto terminal = std::move(*terminal_result);
				if (!issuer.validate_session_terminal(scope, terminal, role))
					return false;
				auto receipt =
					sqlite_shm_reader_lifecycle_production_factory::make_session_terminal(
						request,
						kind,
						terminal.identity(),
						authority_read_closed,
						no_live_shm_lock);
				if (!context.registry->complete_reader_session(
						*context.family, *node.reader_session, receipt))
					return false;
				if (!issuer.retire_session_terminal(scope, terminal, role) ||
					!issuer.retire_scope(scope))
					return false;
				node.reader_session.reset();
				node.reader_session_request.reset();
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] int forwarding_source_shm_unmap(forwarding_file& file,
													  const int remove_file) noexcept;

		[[nodiscard]] std::optional<sqlite_shm_callback_execution_receipt>
		make_source_reader_callback(default_forwarding_state& owner,
									const native_file_node& node) noexcept
		{
			try
			{
				const auto thread_identity =
					make_source_reader_identity(owner, node, "reader-callback-thread");
				const auto invocation_token =
					make_source_reader_identity(owner, node, "reader-callback-invocation");
				if (!thread_identity || !invocation_token)
					return std::nullopt;
				return sqlite_shm_callback_execution_receipt{
					std::move(*thread_identity), 0U, std::move(*invocation_token)};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_shm_reader_attachment_target_identity>
		observe_source_reader_target_identity(const forwarding_file& file) noexcept
		{
			try
			{
				if (!file.target_namespace_epoch || !file.expected_source_shm_identity ||
					!file.target_namespace_epoch->recheck())
					return std::nullopt;
				auto retained = file.target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::shared_memory);
				if (!retained || !retained->object_identity ||
					!retained->directory_entry_identity || !retained->held_object ||
					*retained->object_identity != file.expected_source_shm_identity->object ||
					*retained->directory_entry_identity != file.expected_source_shm_identity->entry)
					return std::nullopt;

				std::optional<sqlite_backend_opaque_identity> filesystem;
				if (retained->object_filesystem_profile)
					filesystem = *retained->object_filesystem_profile;
				else if (retained->held_object->object_filesystem_profile())
					filesystem = *retained->held_object->object_filesystem_profile();
				if (!filesystem || !retained->held_object->object_mount_identity())
					return std::nullopt;
				auto size = retained->held_object->size();
				if (!size || *size == 0U)
					return std::nullopt;
				return sqlite_shm_reader_attachment_target_identity{
					file.target_namespace_epoch->identity(),
					file.target_namespace_epoch->parent_namespace_identity(),
					*retained->object_identity,
					*retained->directory_entry_identity,
					std::move(*filesystem),
					*retained->held_object->object_mount_identity(),
					*size};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_shm_reader_pre_sqlite_session_request>
		make_source_reader_session_request(
			const forwarding_file& file,
			default_forwarding_state& owner,
			const native_file_node& node,
			const default_connection_observation& observation,
			const source_shm_reader_registry_context& context) noexcept
		{
			try
			{
				const auto target_identity = observe_source_reader_target_identity(file);
				const auto callback = make_source_reader_callback(owner, node);
				const auto transaction =
					make_source_reader_identity(owner, node, "reader-transaction-epoch");
				const auto decode =
					make_source_reader_identity(owner, node, "reader-decode-attempt");
				const auto authority =
					make_source_reader_identity(owner, node, "reader-authority-read");
				if (!callback || !transaction || !decode || !authority ||
					context.family_binding == nullptr || context.alias_lifetime == nullptr ||
					context.registration_epoch == nullptr)
					return std::nullopt;
				sqlite_shm_reader_pre_sqlite_session_request pre_request;
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || !observation.source_shm_open_plan ||
						observation.source_shm_qualification_open_plan ||
						!observation.main_native_file_receipt ||
						!observation.main_native_xopen_receipt ||
						!observation.main_callback_cohort || !observation.main_open_epoch)
						return std::nullopt;
					pre_request = {*context.family_binding,
								   *context.alias_lifetime,
								   observation.connection_token_value,
								   *observation.main_native_file_receipt,
								   *observation.main_native_xopen_receipt,
								   *observation.main_open_epoch,
								   *observation.main_callback_cohort,
								   std::move(*callback),
								   std::move(*transaction),
								   std::move(*decode),
								   std::move(*authority),
								   std::move(target_identity),
								   *context.registration_epoch};
				}
				return pre_request;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool
		ensure_source_reader_session(forwarding_file& file,
									 native_file_node& node,
									 const source_shm_reader_registry_context& context) noexcept
		{
			if (node.reader_lifecycle_failed || node.reader_open_authority == std::nullopt ||
				!node.reader_open_authority->valid() || context.registry == nullptr ||
				context.family == nullptr || context.family_binding == nullptr ||
				context.alias_lifetime == nullptr || !file.connection_observation)
			{
				return false;
			}
			if (node.reader_existing_route_active)
				return true;
			if (node.reader_session && node.reader_session->valid() && node.reader_session_request)
				return true;
			if (node.reader_session || node.reader_session_request)
				return false;
			const auto pre_request = make_source_reader_session_request(
				file, *file.owner, node, *file.connection_observation, context);
			if (!pre_request)
			{
				return false;
			}
			try
			{
				auto admitted = context.registry->admit_reader_session_before_sqlite(
					*context.family, *node.reader_open_authority, *pre_request);
				if (!admitted)
				{
					return false;
				}
				if (!admitted->proposal_request())
				{
					if (admitted->kind() !=
						sqlite_shm_reader_session_admission_kind::
							existing_or_ordinary_predecessor_zero_proposal_custody)
						return false;
					node.reader_existing_route_active = true;
					return true;
				}
				auto session = admitted->take_session();
				if (!session || !session->valid())
					return false;
				node.reader_session_request = *admitted->proposal_request();
				node.reader_session.emplace(std::move(*session));
				return node.reader_session->valid();
			}
			catch (...)
			{
				return false;
			}
		}

		struct source_reader_observed_attachment
		{
			sqlite_backend_opaque_identity object;
			sqlite_backend_opaque_identity entry;
			sqlite_backend_opaque_identity device;
			sqlite_backend_opaque_identity mount;
			sqlite_backend_opaque_identity namespace_epoch;
			sqlite_backend_opaque_identity parent_namespace;
			std::uint64_t shm_size{};
		};

		/**
		 * Seal the narrow DF-0205 native-OK projection against the exact map event that was just
		 * committed. A connection-wide boolean is deliberately insufficient: another page, a stale
		 * generation, or a reused native pointer must never inherit a neighbouring map's authority.
		 */
		[[nodiscard]] bool authorize_source_shm_native_ok_projection(
			default_connection_observation& observation,
			const native_file_node& node,
			const sqlite_shm_mapping_tuple& mapping,
			const sqlite_backend_opaque_identity& native_effect,
			const sqlite_shm_callback_execution_receipt& callback,
			const int native_status,
			const volatile void* native_mapping,
			const int page,
			const int page_size,
			const int caller_extend,
			const int delegated_extend) noexcept
		{
			if (!source_shm_native_ok_projection_production_activation ||
				native_status != sqlite_ok || native_mapping == nullptr || page < 0 ||
				page_size <= 0 || !node.reader_open_authority ||
				!node.reader_open_authority->valid() || !node.reader_session ||
				!node.reader_session->valid() || !node.reader_handoff ||
				!node.reader_handoff->valid() || node.reader_generation == 0U ||
				!valid_opaque_identity(native_effect) ||
				!valid_opaque_identity(callback.invocation_token) || mapping.page_number != page ||
				mapping.page_size != page_size || mapping.native_mapping != native_mapping)
				return false;
			const auto page_number = static_cast<std::uint64_t>(page);
			const auto size = static_cast<std::uint64_t>(page_size);
			if (page_number > std::numeric_limits<std::uint64_t>::max() / size)
				return false;
			const auto byte_offset = page_number * size;
			if (byte_offset > std::numeric_limits<std::uint64_t>::max() - size ||
				mapping.byte_offset != byte_offset || mapping.byte_count != size ||
				mapping.sealed_shm_size < byte_offset + size)
				return false;
			try
			{
				std::scoped_lock lock{observation.mutex};
				if (observation.invalid || !observation.source_shm_open_plan ||
					!observation.source_shm_open_callback_receipt ||
					observation.source_shm_qualification_open_plan ||
					observation.shm_map_events.empty())
					return false;
				auto& event = observation.shm_map_events.back();
				if (event.page != page || event.page_size != page_size ||
					event.caller_extend != caller_extend ||
					event.delegated_extend != delegated_extend ||
					event.native_status != sqlite_ok || event.returned_status != sqlite_readonly ||
					!event.native_mapping_nonnull || !event.returned_mapping_nonnull ||
					event.native_mapping_identity != native_mapping ||
					event.native_ok_projection_receipt.has_value())
					return false;
				event.native_ok_projection_receipt.emplace(
					sqlite_backend_shm_map_projection_receipt{node.reader_generation,
															  page,
															  page_size,
															  caller_extend,
															  delegated_extend,
															  native_mapping,
															  mapping.byte_offset,
															  mapping.byte_count,
															  mapping.sealed_shm_size,
															  callback.invocation_token,
															  native_effect});
				return true;
			}
			catch (...)
			{
				return false;
			}
		}

		void clear_source_shm_projection_receipts(
			const std::shared_ptr<default_connection_observation>& observation) noexcept
		{
			if (!observation)
				return;
			try
			{
				std::scoped_lock lock{observation->mutex};
				for (auto& event : observation->shm_map_events)
					event.native_ok_projection_receipt.reset();
			}
			catch (...)
			{
				mark_incomplete(observation);
			}
		}

		[[nodiscard]] std::optional<source_reader_observed_attachment>
		observe_source_reader_attachment(const forwarding_file& file) noexcept
		{
			try
			{
				auto target = observe_source_reader_target_identity(file);
				if (!target)
					return std::nullopt;
				return source_reader_observed_attachment{target->shm_object,
														 target->shm_entry,
														 target->filesystem,
														 target->mount,
														 target->namespace_epoch,
														 target->parent_namespace,
														 target->sealed_shm_size};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] int forwarding_source_shm_map(forwarding_file& file,
													const int page,
													const int page_size,
													const int extend,
													volatile void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			if (extend != 0 && extend != 1)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			auto node = file.native;
			if (!node || !file.owner || !file.connection_observation ||
				file.source_shm_qualification_candidate)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			const auto context = file.owner->source_shm_reader_context();
			if (!context || context->registry == nullptr || context->family == nullptr ||
				context->family_binding == nullptr || context->alias_lifetime == nullptr)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			std::unique_lock lifecycle_lock{node->reader_lifecycle_mutex, std::try_to_lock};
			if (!lifecycle_lock.owns_lock())
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			try
			{
				if (!ensure_source_reader_session(file, *node, *context))
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (node->reader_existing_route_active && !node->reader_session)
				{
					auto* raw = underlying_file(file);
					const auto* methods = underlying_methods(file);
					if (raw == nullptr || methods == nullptr || methods->shm_map == nullptr ||
						!file.expected_source_shm_identity || !file.target_namespace_epoch ||
						!file.target_namespace_epoch->recheck())
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (!native_shm_callback_identity_valid(*node))
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					const auto readonly_family_seen_before = file.source_shm_readonly_family_seen;
					volatile void* native_mapping{};
					int native_status{};
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
						return native_unmap_succeeded;
					};
					try
					{
						native_status = methods->shm_map(raw, page, page_size, 0, &native_mapping);
					}
					catch (...)
					{
						(void)release_native_mapping();
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (!native_shm_callback_identity_valid(*node))
					{
						if (native_mapping != nullptr && methods->shm_unmap != nullptr)
						{
							try
							{
								if (methods->shm_unmap(raw, 0) != sqlite_ok)
									mark_source_shm_terminal_failure(file);
							}
							catch (...)
							{
								mark_source_shm_terminal_failure(file);
							}
						}
						mark_source_shm_terminal_failure(file);
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
						file.source_shm_readonly_family_seen = true;
						if (native_nonnull)
						{
							protocol_violation = true;
							returned_status = sqlite_io_error;
							returned_mapping = nullptr;
						}
					}
					else if (native_status == sqlite_readonly)
					{
						file.source_shm_readonly_family_seen = true;
						if (!native_nonnull)
						{
							returned_status = sqlite_readonly_cannot_initialize;
							returned_mapping = nullptr;
						}
					}
					else if ((native_status & 0xff) == sqlite_readonly)
					{
						file.source_shm_readonly_family_seen = true;
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

					if (!file.target_namespace_epoch->recheck())
					{
						protocol_violation = true;
						returned_status = sqlite_io_error;
						returned_mapping = nullptr;
					}
					if (protocol_violation && !release_native_mapping())
						mark_source_shm_terminal_failure(file);
					if (protocol_violation)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (!record_shared_memory_identity(file.connection_observation,
													   *file.expected_source_shm_identity) ||
						!record_shm_map_event(
							file.connection_observation,
							{page,
							 page_size,
							 extend,
							 0,
							 native_status,
							 returned_status,
							 native_nonnull,
							 returned_mapping != nullptr,
							 readonly_family_seen_before,
							 file.source_shm_readonly_family_seen,
							 file.owner->pinned_underlying_vfs_identity(),
							 file.owner->pinned_underlying_vfs_app_data_identity(),
							 native_mapping,
							 std::nullopt}))
					{
						if (!release_native_mapping())
							mark_source_shm_terminal_failure(file);
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (!native_unmap_succeeded)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					*output = returned_mapping;
					return returned_status;
				}
				const auto& session_request = *node->reader_session_request;
				const sqlite_shm_reader_attachment_map_pre_request pre_request{
					session_request.attachment.family(),
					session_request.attachment.alias_lifetime(),
					session_request.attachment.connection_token(),
					session_request.attachment,
					page,
					page_size,
					extend};
				auto prepared = context->registry->prepare_reader_map_identity(
					*context->family, *node->reader_session, pre_request);
				if (!prepared)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto scope = context->registry->claim_reader_map_identity_scope(
					*context->family, *prepared, pre_request);
				if (!scope)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto issuer = sqlite_shm_reader_lifecycle_production_factory::identity_issuer(
					*context->registry);
				const auto thread_identity =
					make_source_reader_identity(*file.owner, *node, "reader-map-thread");
				if (!thread_identity)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto permit = issuer.reserve_callback(*scope,
													  sqlite_shm_reader_callback_identity_role::map,
													  std::move(*thread_identity),
													  0U);
				if (!permit)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto callback = issuer.seal_callback(
					*permit, *scope, sqlite_shm_reader_callback_identity_role::map);
				if (!callback)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto inflight = context->registry->bind_reader_map_identity(
					*context->family, *node->reader_session, *prepared, *scope, *callback);
				if (!inflight)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				const sqlite_shm_reader_attachment_map_request map_request{
					pre_request.family,
					pre_request.alias_lifetime,
					pre_request.connection_token,
					pre_request.expected_attachment,
					callback->receipt(),
					page,
					page_size,
					extend};
				std::optional<sqlite_shm_reader_native_ok_projection_permit> projection_permit;
				if (source_shm_native_ok_projection_production_activation)
				{
					auto prepared_projection =
						context->registry->prepare_reader_native_ok_projection(
							*context->family, *inflight, map_request);
					if (!prepared_projection)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					// Stage B can recheck the retained namespace.  Do not hold the registry mutex
					// across that filesystem work: stage A sealed its one-shot reservation and
					// stage C will revalidate the process/family pin before publication.
					auto borrowed =
						context->registry->mint_reader_native_ok_projection(*prepared_projection);
					if (!borrowed)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					auto attached = context->registry->attach_reader_native_ok_projection(
						*context->family, *prepared_projection, std::move(*borrowed));
					if (!attached)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					projection_permit.emplace(std::move(*attached));
				}

				auto* raw = underlying_file(file);
				const auto* methods = underlying_methods(file);
				if (raw == nullptr || methods == nullptr || methods->shm_map == nullptr)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (!native_shm_callback_identity_valid(*node))
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				const auto& expected_target = session_request.attachment.target_identity();
				std::optional<sqlite_shm_reader_attachment_target_identity> pre_native_target;
				if (expected_target)
					pre_native_target = observe_source_reader_target_identity(file);
				else if (!file.target_namespace_epoch || !file.target_namespace_epoch->recheck())
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				// A target-bearing reader session is the only route eligible for the native-OK
				// projection. Re-observe the complete retained object before entering SQLite so an
				// open-after SHM replacement or size change cannot cross the native callback
				// boundary. Targetless ordinary readers remain fail-closed for the projection, but
				// retain their existing namespace-only admission until that projection is
				// activated.
				if ((expected_target &&
					 (!pre_native_target || *pre_native_target != *expected_target)) ||
					(source_shm_native_ok_projection_production_activation && !expected_target))
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				volatile void* native_mapping{};
				int native_status{};
				try
				{
					native_status = methods->shm_map(raw, page, page_size, 0, &native_mapping);
				}
				catch (...)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (!native_shm_callback_identity_valid(*node))
				{
					if (native_mapping != nullptr && methods->shm_unmap != nullptr)
					{
						try
						{
							if (methods->shm_unmap(raw, 0) != sqlite_ok)
								mark_source_shm_terminal_failure(file);
						}
						catch (...)
						{
							mark_source_shm_terminal_failure(file);
						}
					}
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				const auto native_nonnull = native_mapping != nullptr;
				const auto release_native_mapping = [&]() noexcept
				{
					if (!native_nonnull)
						return true;
					if (methods->shm_unmap == nullptr)
						return false;
					try
					{
						return methods->shm_unmap(raw, 0) == sqlite_ok;
					}
					catch (...)
					{
						return false;
					}
				};

				if (native_status != sqlite_ok || !native_nonnull)
				{
					const auto effect_role =
						sqlite_shm_reader_effect_identity_role::zero_attachment_result;
					auto effect = issuer.issue_effect(*scope, *callback, effect_role);
					if (!effect)
					{
						if (!release_native_mapping())
							mark_source_shm_terminal_failure(file);
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					auto zero = sqlite_shm_reader_lifecycle_production_factory::
						validate_zero_attachment_effect(*context->registry,
														*context->family,
														*inflight,
														*scope,
														*callback,
														*effect,
														native_status,
														native_mapping,
														0);
					if (!zero)
					{
						if (!release_native_mapping())
							mark_source_shm_terminal_failure(file);
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					auto completed = context->registry->complete_reader_zero_attachment_map(
						*context->family, *inflight, *zero, *node->reader_session);
					if (!completed)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (node->reader_session && !node->reader_session->valid())
					{
						node->reader_session.reset();
						node->reader_session_request.reset();
					}
					if (!record_shm_map_event(
							file.connection_observation,
							{page,
							 page_size,
							 extend,
							 0,
							 native_status,
							 completed->kind() ==
									 sqlite_shm_reader_attachment_zero_effect_kind::
										 exact_no_attachment_change
								 ? native_status
								 : sqlite_io_error,
							 native_nonnull,
							 false,
							 false,
							 file.source_shm_readonly_family_seen,
							 file.owner->pinned_underlying_vfs_identity(),
							 file.owner->pinned_underlying_vfs_app_data_identity(),
							 native_mapping,
							 std::nullopt}))
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					return completed->kind() ==
							sqlite_shm_reader_attachment_zero_effect_kind::
								exact_no_attachment_change
						? native_status
						: sqlite_io_error;
				}
				if (!source_shm_native_ok_projection_production_activation)
				{
					if (!release_native_mapping() ||
						!record_shared_memory_identity(file.connection_observation,
													   *file.expected_source_shm_identity) ||
						!record_shm_map_event(
							file.connection_observation,
							{page,
							 page_size,
							 extend,
							 0,
							 native_status,
							 sqlite_io_error,
							 native_nonnull,
							 false,
							 file.source_shm_readonly_family_seen,
							 file.source_shm_readonly_family_seen,
							 file.owner->pinned_underlying_vfs_identity(),
							 file.owner->pinned_underlying_vfs_app_data_identity(),
							 native_mapping,
							 std::nullopt}))
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				auto observed = observe_source_reader_attachment(file);
				if (!observed)
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto effect = issuer.issue_effect(
					*scope, *callback, sqlite_shm_reader_effect_identity_role::mapped_result);
				if (!effect)
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				const auto projection_effect_identity = effect->identity();
				auto observed_native =
					sqlite_shm_reader_lifecycle_production_factory::make_mapped_observation(
						*inflight,
						map_request,
						native_status,
						native_mapping,
						0,
						std::move(observed->object),
						std::move(observed->entry),
						std::move(observed->device),
						std::move(observed->mount),
						std::move(observed->namespace_epoch),
						std::move(observed->parent_namespace),
						observed->shm_size);
				if (!observed_native)
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (!record_shared_memory_identity(file.connection_observation,
												   *file.expected_source_shm_identity) ||
					!record_shm_map_event(file.connection_observation,
										  {page,
										   page_size,
										   extend,
										   0,
										   native_status,
										   sqlite_readonly,
										   native_nonnull,
										   true,
										   file.source_shm_readonly_family_seen,
										   true,
										   file.owner->pinned_underlying_vfs_identity(),
										   file.owner->pinned_underlying_vfs_app_data_identity(),
										   native_mapping,
										   std::nullopt}))
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto validated = sqlite_shm_reader_lifecycle_production_factory::
					validate_mapped_attachment_effect(*context->registry,
													  *context->family,
													  *inflight,
													  *scope,
													  *callback,
													  *effect,
													  std::move(*observed_native));
				if (!validated)
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto committed = projection_permit
					? context->registry->commit_reader_map_with_native_ok_projection(
						  *context->family,
						  *inflight,
						  *validated,
						  *node->reader_session,
						  *projection_permit)
					: context->registry->commit_reader_map(
						  *context->family, *inflight, *validated, *node->reader_session);
				if (!committed)
				{
					if (!release_native_mapping())
						mark_source_shm_terminal_failure(file);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (auto handoff = committed->take_handoff(); handoff)
				{
					node->reader_generation = handoff->generation();
					node->reader_handoff.emplace(std::move(*handoff));
				}
				else if (node->reader_handoff)
					node->reader_generation = node->reader_handoff->generation();
				const auto projection_authorized =
					authorize_source_shm_native_ok_projection(*file.connection_observation,
															  *node,
															  committed->mapping(),
															  projection_effect_identity,
															  callback->receipt(),
															  native_status,
															  native_mapping,
															  page,
															  page_size,
															  extend,
															  0);
				if (!projection_authorized)
				{
					// A committed native map without its exact per-map receipt is terminal. The
					// later xShmUnmap path remains available solely to consume the already-owned
					// handoff.
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				file.source_shm_readonly_family_seen = true;
				*output = native_mapping;
				return sqlite_readonly;
			}
			catch (...)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_writer_identity(default_forwarding_state& owner,
							 const native_file_node& node,
							 const std::string_view purpose,
							 const std::string_view profile) noexcept
		{
			try
			{
				auto output = make_source_reader_identity(owner, node, purpose);
				if (!output)
					return std::nullopt;
				output->profile = std::string{profile};
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_writer_expected_shm_leaf(const native_file_node& node) noexcept
		{
			try
			{
				if (!node.target_namespace_epoch ||
					!valid_opaque_identity(node.target_namespace_epoch->identity()) ||
					!valid_opaque_identity(
						node.target_namespace_epoch->parent_namespace_identity()) ||
					node.target_namespace_epoch->anchored_main_locator().empty())
					return std::nullopt;
				sqlite_backend_opaque_identity output{"cxxlens.sqlite-writer-expected-shm-leaf.v1",
													  {}};
				append_opaque_identity(output.bytes, node.target_namespace_epoch->identity());
				append_opaque_identity(output.bytes,
									   node.target_namespace_epoch->parent_namespace_identity());
				append_bytes(output.bytes, node.target_namespace_epoch->anchored_main_locator());
				append_bytes(output.bytes, shm_suffix);
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool ensure_writer_epoch_attachment_sources(default_forwarding_state& owner,
																  native_file_node& node) noexcept
		{
			try
			{
				if (node.writer_retained_parent_source && node.writer_retained_parent_revoker &&
					node.writer_retained_parent_source->valid() &&
					node.writer_shm_attachment_source && node.writer_shm_attachment_revoker &&
					node.writer_shm_attachment_source->valid() &&
					node.writer_retained_parent_receipt && node.writer_shm_attachment_receipt)
					return true;
				if (!node.target_namespace_epoch || !node.writer_lifetime_source ||
					!node.writer_lifetime_source->valid() || !node.quarantine_self ||
					!node.target_namespace_epoch->recheck())
					return false;
				auto main_retained = node.target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::main_database);
				auto retained = node.target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::shared_memory);
				if (!main_retained ||
					main_retained->state != sqlite_backend_entry_state::held_regular ||
					!main_retained->direct_regular_entry || !main_retained->object_identity ||
					!main_retained->directory_entry_identity ||
					!main_retained->object_filesystem_profile || !main_retained->held_object ||
					!main_retained->held_object->object_mount_identity() || !retained ||
					(retained->state != sqlite_backend_entry_state::held_regular &&
					 retained->state != sqlite_backend_entry_state::absent))
					return false;
				auto expected_leaf = make_writer_expected_shm_leaf(node);
				if (!expected_leaf)
					return false;
				const auto filesystem = retained->state == sqlite_backend_entry_state::held_regular
					? retained->object_filesystem_profile
					: main_retained->object_filesystem_profile;
				const auto mount = retained->state == sqlite_backend_entry_state::held_regular
					? retained->held_object->object_mount_identity()
					: main_retained->held_object->object_mount_identity();
				if (!filesystem || !mount)
					return false;
				auto parent_lifetime =
					make_writer_identity(owner,
										 node,
										 "writer-retained-parent-lifetime",
										 "cxxlens.sqlite-writer-retained-parent-lifetime.v1");
				auto parent_receipt =
					make_writer_identity(owner,
										 node,
										 "writer-retained-parent-semantic",
										 "cxxlens.sqlite-writer-retained-parent-receipt.v1");
				auto shm_lifetime =
					make_writer_identity(owner,
										 node,
										 "writer-shm-attachment-lifetime",
										 "cxxlens.sqlite-writer-shm-attachment-lifetime.v1");
				auto shm_receipt =
					make_writer_identity(owner,
										 node,
										 "writer-shm-attachment-semantic",
										 "cxxlens.sqlite-writer-shm-attachment-receipt.v1");
				if (!parent_lifetime || !parent_receipt || !shm_lifetime || !shm_receipt)
					return false;
				append_opaque_identity(parent_receipt->bytes,
									   node.target_namespace_epoch->parent_namespace_identity());
				append_opaque_identity(parent_receipt->bytes, *main_retained->object_identity);
				append_opaque_identity(parent_receipt->bytes,
									   *main_retained->directory_entry_identity);
				append_opaque_identity(shm_receipt->bytes, *expected_leaf);
				append_opaque_identity(shm_receipt->bytes, *filesystem);
				append_opaque_identity(shm_receipt->bytes, *mount);
				if (retained->object_identity)
					append_opaque_identity(shm_receipt->bytes, *retained->object_identity);
				if (retained->directory_entry_identity)
					append_opaque_identity(shm_receipt->bytes, *retained->directory_entry_identity);
				auto parent_semantic = *parent_receipt;
				auto shm_semantic = *shm_receipt;
				const auto owner_pin = std::static_pointer_cast<void>(node.quarantine_self);
				auto parent = sqlite_writer_shm_native_lifetime_production_factory::create_source(
					sqlite_writer_shm_native_lifetime_role::retained_parent,
					std::move(*parent_lifetime),
					std::move(*parent_receipt),
					std::nullopt,
					owner_pin);
				auto shm = sqlite_writer_shm_native_lifetime_production_factory::create_source(
					sqlite_writer_shm_native_lifetime_role::shared_memory_attachment,
					std::move(*shm_lifetime),
					std::move(*shm_receipt),
					std::nullopt,
					owner_pin);
				if (!parent || !shm)
					return false;
				node.writer_retained_parent_revoker.emplace(std::move(parent->first));
				node.writer_retained_parent_source.emplace(std::move(parent->second));
				node.writer_retained_parent_receipt.emplace(std::move(parent_semantic));
				node.writer_shm_attachment_revoker.emplace(std::move(shm->first));
				node.writer_shm_attachment_source.emplace(std::move(shm->second));
				node.writer_shm_attachment_receipt.emplace(std::move(shm_semantic));
				return node.writer_retained_parent_source->valid() &&
					node.writer_shm_attachment_source->valid();
			}
			catch (...)
			{
				return false;
			}
		}

		[[nodiscard]] std::optional<sqlite_shm_callback_execution_receipt>
		make_writer_callback(default_forwarding_state& owner, const native_file_node& node) noexcept
		{
			try
			{
				auto thread = make_writer_identity(
					owner, node, "writer-map-thread", "cxxlens.sqlite-writer-callback-thread.v1");
				auto invocation =
					make_writer_identity(owner,
										 node,
										 "writer-map-invocation",
										 "cxxlens.sqlite-writer-callback-invocation.v1");
				if (!thread || !invocation)
					return std::nullopt;
				return sqlite_shm_callback_execution_receipt{
					std::move(*thread), 0U, std::move(*invocation)};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_writer_attachment_epoch(default_forwarding_state& owner,
									 const native_file_node& node,
									 const sqlite_backend_opaque_identity& connection,
									 const sqlite_backend_opaque_identity& open_epoch) noexcept
		{
			try
			{
				auto output = make_writer_identity(owner,
												   node,
												   "writer-attachment-epoch",
												   "cxxlens.sqlite-writer-shm-attachment-epoch.v1");
				if (!output || !node.target_namespace_epoch)
					return std::nullopt;
				append_opaque_identity(output->bytes, connection);
				append_opaque_identity(output->bytes, open_epoch);
				append_opaque_identity(output->bytes, node.target_namespace_epoch->identity());
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		/**
		 * Seal the wrapper-visible WAL coordination observation for one writer map.
		 *
		 * The zero-zero preexisting-SHM route is intentionally allowed before SQLite has
		 * acquired the WAL write lock: its effect proof is the unchanged direct SHM census,
		 * not a WAL write.  The one-one routes still require the exact exclusive lock
		 * observation.  Both cases retain the complete observed lock set so the receipt
		 * cannot be reconstructed from the caller's extend flag alone.
		 */
		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_writer_wal_lock_receipt(const default_connection_observation& observation,
									 const native_file_node& node,
									 const bool require_exclusive_lock) noexcept
		{
			try
			{
				std::vector<sqlite_backend_shm_lock_observation> locks;
				{
					std::scoped_lock lock{observation.mutex};
					if (observation.invalid || !observation.main_handle_open ||
						(require_exclusive_lock && observation.held_shm_locks.empty()))
						return std::nullopt;
					locks = observation.held_shm_locks;
				}
				std::ranges::sort(locks,
								  [](const auto& left, const auto& right)
								  {
									  if (left.offset != right.offset)
										  return left.offset < right.offset;
									  if (left.count != right.count)
										  return left.count < right.count;
									  return static_cast<int>(left.mode) <
										  static_cast<int>(right.mode);
								  });
				const bool has_exclusive_write_lock = std::ranges::any_of(
					locks,
					[](const auto& lock)
					{
						return lock.offset == 0 && lock.count == 1 &&
							lock.mode == sqlite_backend_shm_lock_mode::exclusive;
					});
				if (require_exclusive_lock && !has_exclusive_write_lock)
					return std::nullopt;
				sqlite_backend_opaque_identity receipt{
					require_exclusive_lock ? "cxxlens.sqlite-writer-wal-write-lock.v1"
										   : "cxxlens.sqlite-wal-coordination-observation.v1",
					{}};
				append_pointer(receipt.bytes, &node);
				append_opaque_identity(receipt.bytes, observation.connection_token_value);
				append_u64(receipt.bytes, require_exclusive_lock ? 1U : 0U);
				append_u64(receipt.bytes, has_exclusive_write_lock ? 1U : 0U);
				append_u64(receipt.bytes, static_cast<std::uint64_t>(locks.size()));
				for (const auto& lock : locks)
				{
					if (lock.offset < 0 || lock.count <= 0)
						return std::nullopt;
					append_u64(receipt.bytes, static_cast<std::uint64_t>(lock.offset));
					append_u64(receipt.bytes, static_cast<std::uint64_t>(lock.count));
					receipt.bytes.push_back(static_cast<std::byte>(lock.mode));
				}
				return receipt;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_writer_route_seal(default_forwarding_state& owner,
							   const native_file_node& node,
							   const sqlite_shm_writer_map_request& request,
							   const std::string_view label) noexcept
		{
			try
			{
				auto output =
					make_writer_identity(owner, node, label, "cxxlens.sqlite-writer-route-seal.v1");
				if (!output)
					return std::nullopt;
				append_opaque_identity(output->bytes, request.family.process_instance);
				append_opaque_identity(output->bytes, request.family.shared_runtime_vfs_cohort);
				append_opaque_identity(output->bytes, request.family.exact_file_family);
				append_opaque_identity(output->bytes, request.alias_lifetime);
				append_opaque_identity(output->bytes, request.connection_token);
				append_opaque_identity(output->bytes, request.attachment.attachment_epoch());
				return output;
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		/**
		 * Bind the first writer map to the still-denied effect boundary.
		 *
		 * SQLite may call xShmMap while opening the actual current-v3 writer, before Store can
		 * complete its full effect arm.  The denied validation is the only gate evidence available
		 * at that point.  The second identity is an attempt-local source transcript; it is never
		 * consumed by persistent-effect permission and cannot install writer eligibility by itself.
		 */
		[[nodiscard]] std::optional<
			std::pair<sqlite_backend_opaque_identity, sqlite_backend_opaque_identity>>
		make_writer_pre_gate_effect_receipts(
			const default_connection_observation& observation,
			const native_file_node& node,
			const sqlite_backend_opaque_identity& source_id,
			const sqlite_backend_opaque_identity& callback_cohort,
			const sqlite_backend_opaque_identity& open_epoch) noexcept
		{
			try
			{
				if (observation.effect_gate == nullptr)
					return std::nullopt;
				auto latest = observation.effect_gate->latest_receipt();
				if (!latest || latest->sequence == 0U ||
					latest->capability_token != observation.capability_token_value ||
					latest->connection_token != observation.connection_token_value ||
					latest->canonical_vfs_locator != observation.canonical_locator ||
					!valid_opaque_identity(latest->validation_receipt) ||
					!valid_opaque_identity(source_id) || !valid_opaque_identity(callback_cohort) ||
					!valid_opaque_identity(open_epoch) || !node.target_namespace_epoch ||
					!valid_opaque_identity(node.target_namespace_epoch->identity()))
					return std::nullopt;
				const auto effect_stage = latest->stage;
				if (effect_stage != sqlite_backend_effect_stage::denied &&
					effect_stage != sqlite_backend_effect_stage::wal_shm_coordination_only &&
					effect_stage != sqlite_backend_effect_stage::fully_armed)
					return std::nullopt;
				const auto gate_receipt = effect_stage == sqlite_backend_effect_stage::denied
					? latest->validation_receipt
					: latest->prerequisite_receipt;
				if (!valid_opaque_identity(gate_receipt))
					return std::nullopt;

				sqlite_backend_opaque_identity transcript{
					"cxxlens.sqlite-writer-pre-gate-coordination-transcript.v1", {}};
				append_opaque_identity(transcript.bytes, gate_receipt);
				append_opaque_identity(transcript.bytes, latest->validation_receipt);
				append_opaque_identity(transcript.bytes, source_id);
				append_opaque_identity(transcript.bytes, callback_cohort);
				append_opaque_identity(transcript.bytes, open_epoch);
				append_opaque_identity(transcript.bytes, node.target_namespace_epoch->identity());
				append_u64(transcript.bytes, latest->sequence);
				transcript.bytes.push_back(static_cast<std::byte>(effect_stage));
				if (!valid_opaque_identity(transcript) || transcript == gate_receipt ||
					transcript == latest->validation_receipt)
					return std::nullopt;
				return std::pair{gate_receipt, std::move(transcript)};
			}
			catch (...)
			{
				return std::nullopt;
			}
		}

		[[nodiscard]] bool
		complete_writer_native_cleanup(const source_shm_reader_registry_context& context,
									   sqlite_shm_writer_attachment_cleanup& cleanup,
									   const sqlite_shm_callback_execution_receipt& callback,
									   int native_status,
									   bool native_known) noexcept
		{
			if (!context.registry || !context.family)
				return false;
			const auto outcome = !native_known ? sqlite_shm_native_cleanup_outcome::unknown
				: native_status == sqlite_ok ? sqlite_shm_native_cleanup_outcome::confirmed_success
											 : sqlite_shm_native_cleanup_outcome::non_ok;
			return static_cast<bool>(context.registry->complete_writer_cleanup(
				*context.family, cleanup, callback, outcome));
		}

		[[nodiscard]] int
		fail_writer_post_native(forwarding_file& file,
								const source_shm_reader_registry_context& context,
								sqlite_shm_writer_post_native_mapping& post_native,
								const sqlite_shm_callback_execution_receipt& callback,
								volatile void* native_mapping) noexcept
		{
			auto cleanup = context.registry && context.family
				? context.registry->begin_writer_cleanup(*context.family, post_native, callback)
				: sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>{
					  {sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					   sqlite_shm_lease_recovery_action::quarantine_no_retry}};
			int status{};
			bool known{};
			auto* raw = underlying_file(file);
			const auto* methods = underlying_methods(file);
			if (native_mapping != nullptr && raw != nullptr && methods != nullptr &&
				methods->shm_unmap != nullptr)
			{
				try
				{
					status = methods->shm_unmap(raw, 0);
					known = true;
				}
				catch (...)
				{
					known = false;
				}
			}
			if (!cleanup ||
				!complete_writer_native_cleanup(context, *cleanup, callback, status, known))
				mark_source_shm_terminal_failure(file);
			return sqlite_io_error;
		}

		[[nodiscard]] int
		fail_writer_pending(forwarding_file& file,
							const source_shm_reader_registry_context& context,
							sqlite_shm_pending_mapping& pending,
							const sqlite_shm_callback_execution_receipt& callback) noexcept
		{
			auto cleanup = context.registry && context.family
				? context.registry->begin_writer_cleanup(*context.family, pending, callback)
				: sqlite_shm_lease_result<sqlite_shm_writer_attachment_cleanup>{
					  {sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
					   sqlite_shm_lease_recovery_action::quarantine_no_retry}};
			int status{};
			bool known{};
			auto* raw = underlying_file(file);
			const auto* methods = underlying_methods(file);
			if (raw != nullptr && methods != nullptr && methods->shm_unmap != nullptr)
			{
				try
				{
					status = methods->shm_unmap(raw, 0);
					known = true;
				}
				catch (...)
				{
					known = false;
				}
			}
			if (!cleanup ||
				!complete_writer_native_cleanup(context, *cleanup, callback, status, known))
				mark_source_shm_terminal_failure(file);
			return sqlite_io_error;
		}

		[[nodiscard]] int forwarding_writer_shm_map(forwarding_file& file,
													int page,
													int page_size,
													int extend,
													volatile void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			try
			{
				if (!file.owner || !file.connection_observation || !file.native ||
					file.role != sqlite_backend_file_role::main_database || !file.main_handle ||
					!file.native->target_namespace_epoch)
					return sqlite_io_error;
				auto node = file.native;
				std::unique_lock lifecycle_lock{node->writer_lifecycle_mutex, std::try_to_lock};
				if (!lifecycle_lock.owns_lock() ||
					!ensure_writer_epoch_attachment_sources(*file.owner, *node))
					return sqlite_io_error;
				const auto context = file.owner->source_shm_reader_context();
				if (!context || !context->registry || !context->family ||
					!context->family_binding || !context->alias_lifetime)
					return sqlite_io_error;

				sqlite_backend_opaque_identity connection_token;
				sqlite_backend_opaque_identity main_receipt;
				sqlite_backend_opaque_identity main_xopen;
				sqlite_backend_opaque_identity main_open_epoch;
				sqlite_backend_opaque_identity callback_cohort;
				sqlite_backend_opaque_identity source_id;
				sqlite_backend_opaque_identity effect_gate_receipt;
				sqlite_backend_opaque_identity effect_receipt;
				bool eligibility_installed{};
				{
					std::scoped_lock lock{file.connection_observation->mutex};
					const auto& observation = *file.connection_observation;
					if (observation.invalid || !observation.main_handle_open ||
						!observation.writer_target_namespace_epoch ||
						observation.writer_target_namespace_epoch != node->target_namespace_epoch ||
						!observation.writer_sqlite_source_id ||
						!observation.main_native_file_receipt ||
						!observation.main_native_xopen_receipt ||
						!observation.main_callback_cohort || !observation.main_open_epoch)
					{
						return sqlite_io_error;
					}
					eligibility_installed = observation.writer_eligibility.has_value();
					connection_token = observation.connection_token_value;
					main_receipt = *observation.main_native_file_receipt;
					main_xopen = *observation.main_native_xopen_receipt;
					main_open_epoch = *observation.main_open_epoch;
					callback_cohort = *observation.main_callback_cohort;
					source_id = *observation.writer_sqlite_source_id;
					if (eligibility_installed)
					{
						if (!observation.writer_effect_gate_receipt ||
							!observation.writer_effect_receipt)
							return sqlite_io_error;
						effect_gate_receipt = *observation.writer_effect_gate_receipt;
						effect_receipt = *observation.writer_effect_receipt;
					}
					else
					{
						auto pre_gate = make_writer_pre_gate_effect_receipts(
							observation, *node, source_id, callback_cohort, main_open_epoch);
						if (!pre_gate)
						{
							return sqlite_io_error;
						}
						effect_gate_receipt = std::move(pre_gate->first);
						effect_receipt = std::move(pre_gate->second);
					}
				}
				if (!node->writer_native_file_receipt || !node->writer_native_xopen_receipt ||
					!node->writer_open_epoch || !node->writer_callback_cohort ||
					*node->writer_native_file_receipt != main_receipt ||
					*node->writer_native_xopen_receipt != main_xopen ||
					*node->writer_open_epoch != main_open_epoch ||
					*node->writer_callback_cohort != callback_cohort ||
					!node->writer_retained_parent_receipt || !node->writer_shm_attachment_receipt)
				{
					return sqlite_io_error;
				}

				auto wal_node = file.connection_observation->wal_native_node.lock();
				if (!wal_node || wal_node.get() == node.get() ||
					!wal_node->writer_lifetime_source ||
					!wal_node->writer_lifetime_source->valid() ||
					!wal_node->writer_native_file_receipt ||
					!wal_node->writer_native_xopen_receipt || !wal_node->target_namespace_epoch ||
					wal_node->target_namespace_epoch != node->target_namespace_epoch)
				{
					return sqlite_io_error;
				}
				auto wal_lock =
					make_writer_wal_lock_receipt(*file.connection_observation, *node, extend != 0);
				auto callback = make_writer_callback(*file.owner, *node);
				if (!node->writer_attachment_epoch)
					node->writer_attachment_epoch = make_writer_attachment_epoch(
						*file.owner, *node, connection_token, main_open_epoch);
				if (!wal_lock || !callback || !node->writer_attachment_epoch)
				{
					return sqlite_io_error;
				}
				auto attachment =
					sqlite_shm_native_attachment_identity::bind(*context->family_binding,
																*context->alias_lifetime,
																connection_token,
																main_receipt,
																main_xopen,
																main_open_epoch,
																callback_cohort,
																*node->writer_attachment_epoch);
				if (!attachment)
				{
					return sqlite_io_error;
				}
				std::optional<sqlite_shm_native_attachment_identity> pending_attachment;
				if (!eligibility_installed)
				{
					if (node->writer_pending_attachment &&
						*node->writer_pending_attachment != *attachment)
						return sqlite_io_error;
					if (!node->writer_pending_attachment)
						pending_attachment.emplace(*attachment);
				}

				auto main_retained = node->target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::main_database);
				auto retained = node->target_namespace_epoch->retained_entry(
					sqlite_backend_file_role::shared_memory);
				if (!main_retained ||
					main_retained->state != sqlite_backend_entry_state::held_regular ||
					!main_retained->direct_regular_entry ||
					!main_retained->object_filesystem_profile || !main_retained->held_object ||
					!main_retained->held_object->object_mount_identity() || !retained ||
					(retained->state != sqlite_backend_entry_state::held_regular &&
					 retained->state != sqlite_backend_entry_state::absent))
				{
					return sqlite_io_error;
				}
				auto expected_leaf = make_writer_expected_shm_leaf(*node);
				if (!expected_leaf)
				{
					return sqlite_io_error;
				}
				const auto absent_filesystem = retained->state == sqlite_backend_entry_state::absent
					? main_retained->object_filesystem_profile
					: retained->object_filesystem_profile;
				const auto absent_mount = retained->state == sqlite_backend_entry_state::absent
					? main_retained->held_object->object_mount_identity()
					: retained->held_object->object_mount_identity();
				if (!absent_filesystem || !absent_mount)
				{
					return sqlite_io_error;
				}
				const sqlite_shm_writer_map_request map_request{*context->family_binding,
																*context->alias_lifetime,
																connection_token,
																*attachment,
																*callback,
																page,
																page_size,
																extend};
				const sqlite_writer_shm_mapping_epoch_binding binding{
					map_request,
					extend,
					*expected_leaf,
					*node->writer_retained_parent_receipt,
					*wal_node->writer_native_file_receipt,
					*wal_node->writer_native_xopen_receipt,
					*node->writer_shm_attachment_receipt,
					node->target_namespace_epoch->identity()};

				if (node->writer_holders.capacity() < node->writer_holders.size() + 1U)
					node->writer_holders.reserve(node->writer_holders.size() + 1U);
				if (node->writer_pending.capacity() < node->writer_pending.size() + 1U)
					node->writer_pending.reserve(node->writer_pending.size() + 1U);
				auto parent_pin = node->writer_retained_parent_source->mint_pin();
				auto main_pin = node->writer_lifetime_source->mint_pin();
				auto wal_pin = wal_node->writer_lifetime_source->mint_pin();
				auto shm_pin = node->writer_shm_attachment_source->mint_pin();
				if (!parent_pin || !main_pin || !wal_pin || !shm_pin)
				{
					return sqlite_io_error;
				}
				sqlite_writer_shm_mapping_epoch_platform_binding platform{
					node->target_namespace_epoch,
					node->target_namespace_epoch->parent_namespace_identity(),
					source_id,
					*wal_lock,
					effect_gate_receipt,
					effect_receipt,
					absent_filesystem,
					absent_mount};
				sqlite_writer_shm_mapping_epoch_request epoch_request{binding,
																	  std::move(*parent_pin),
																	  std::move(*main_pin),
																	  std::move(*wal_pin),
																	  std::move(*shm_pin)};
				sqlite_retained_namespace_writer_shm_mapping_epoch_port epoch_port{
					std::move(platform)};
				auto activation = epoch_port.arm(std::move(epoch_request));
				if (!activation)
				{
					return sqlite_io_error;
				}
				auto observer = activation->take_observer();
				auto arm = activation->take_arm();
				auto inflight = context->registry->begin_writer_map(
					*context->family, std::move(arm), map_request);
				if (!inflight)
				{
					return sqlite_io_error;
				}

				auto* raw = underlying_file(file);
				const auto* methods = underlying_methods(file);
				if (!raw || !methods || !methods->shm_map)
				{
					(void)context->registry->resolve_writer_map_failure(*context->family,
																		*inflight);
					return sqlite_io_error;
				}
				volatile void* native_mapping{};
				int native_status{};
				bool native_threw{};
				try
				{
					native_status = methods->shm_map(raw, page, page_size, extend, &native_mapping);
				}
				catch (...)
				{
					native_threw = true;
					native_status = sqlite_io_error;
				}
				auto native_receipt =
					native_mapping != nullptr && (native_status != sqlite_ok || native_threw)
					? sqlite_writer_shm_native_map_receipt_validator::validate_mapped_failure(
						  *inflight, native_status, native_mapping)
					: sqlite_writer_shm_native_map_receipt_validator::validate(
						  *inflight, native_status, native_mapping);
				if (native_receipt && native_status != sqlite_ok)
				{
					auto post_native = context->registry->record_writer_native_mapping(
						*context->family, *inflight, *native_receipt);
					if (post_native)
						return fail_writer_post_native(
							file, *context, *post_native, *callback, native_mapping);
					native_receipt =
						sqlite_shm_lease_result<sqlite_shm_verified_writer_native_map_receipt>{
							{sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
							 sqlite_shm_lease_recovery_action::quarantine_no_retry}};
				}
				if (!native_receipt)
				{
					if (native_mapping == nullptr)
					{
						auto resolved = context->registry->resolve_writer_map_failure(
							*context->family, *inflight);
						if (resolved && native_status != sqlite_ok && !native_threw)
							return native_status;
					}
					else if (methods->shm_unmap)
					{
						try
						{
							if (methods->shm_unmap(raw, 0) != sqlite_ok)
								mark_source_shm_terminal_failure(file);
						}
						catch (...)
						{
							mark_source_shm_terminal_failure(file);
						}
					}
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				auto post_native = context->registry->record_writer_native_mapping(
					*context->family, *inflight, *native_receipt);
				if (!post_native)
				{
					if (methods->shm_unmap)
					{
						try
						{
							(void)methods->shm_unmap(raw, 0);
						}
						catch (...)
						{
						}
					}
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				auto epoch = seal_sqlite_writer_shm_mapping_epoch(observer, native_mapping);
				if (!epoch)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				auto audit = validate_sqlite_writer_shm_mapping_semantics_for_audit(*epoch);
				if (!audit)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				auto auth_seal = make_writer_route_seal(
					*file.owner, *node, map_request, "authenticated-owned-forwarding-rw-main");
				auto route_seal =
					make_writer_route_seal(*file.owner, *node, map_request, "route-validation");
				if (!auth_seal || !route_seal)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				auto proof = sqlite_shm_writer_route_proof_production_factory::seal(
					audit->route,
					map_request,
					extend,
					std::move(*auth_seal),
					main_receipt,
					main_xopen,
					source_id,
					map_request.callback.invocation_token,
					*wal_lock,
					effect_gate_receipt,
					std::move(*route_seal));
				if (!proof)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				auto verified =
					sqlite_writer_shm_mapping_receipt_validator::validate(*epoch, *proof);
				if (!verified)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				const auto& mapped_shm = epoch->post_observation().stat;
				if (mapped_shm.state != sqlite_writer_shm_entry_state::direct_regular ||
					!mapped_shm.object_identity || !mapped_shm.directory_entry_identity ||
					!record_shared_memory_identity(
						file.connection_observation,
						{*mapped_shm.object_identity, *mapped_shm.directory_entry_identity}))
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);

				const bool gate_before_map = !node->writer_holders.empty();
				if (gate_before_map)
				{
					auto holder =
						context->registry->complete_gate_winning_writer_map_before_callback_return(
							*context->family, *post_native, *verified);
					if (!holder)
					{
						return fail_writer_post_native(
							file, *context, *post_native, *callback, native_mapping);
					}
					node->writer_holders.push_back(std::move(*holder));
					*output = native_mapping;
					return sqlite_ok;
				}

				auto pending = context->registry->install_writer_pending(
					*context->family, *post_native, *verified);
				if (!pending)
					return fail_writer_post_native(
						file, *context, *post_native, *callback, native_mapping);
				if (!eligibility_installed)
				{
					if (node->writer_pending_attachment &&
						*node->writer_pending_attachment != map_request.attachment)
						return fail_writer_pending(file, *context, *pending, *callback);
					if (!node->writer_pending_attachment)
					{
						if (!pending_attachment)
							return fail_writer_pending(file, *context, *pending, *callback);
						node->writer_pending_attachment.emplace(std::move(*pending_attachment));
					}
					node->writer_pending.push_back(std::move(*pending));
					*output = native_mapping;
					return sqlite_ok;
				}
				std::array<sqlite_shm_pending_mapping*, 1U> group{&*pending};
				auto gate = [&]()
					-> sqlite_shm_lease_result<sqlite_shm_positive_writer_attachment_gate_result>
				{
					std::scoped_lock lock{file.connection_observation->mutex};
					if (!file.connection_observation->writer_eligibility)
						return sqlite_shm_lease_result<
							sqlite_shm_positive_writer_attachment_gate_result>{
							sqlite_shm_lease_rejection{
								sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
								sqlite_shm_lease_recovery_action::quarantine_no_retry}};
					return context->registry->advance_positive_writer_attachment_gate(
						*context->family,
						map_request.attachment,
						group,
						*file.connection_observation->writer_eligibility);
				}();
				if (!gate ||
					gate->progress !=
						sqlite_shm_positive_writer_attachment_gate_progress::complete ||
					gate->holders.size() != 1U)
				{
					return fail_writer_pending(file, *context, *pending, *callback);
				}
				node->writer_holders = std::move(gate->holders);
				*output = native_mapping;
				return sqlite_ok;
			}
			catch (...)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
		}

		[[nodiscard]] int forwarding_writer_shm_unmap(forwarding_file& file,
													  const int remove_file) noexcept
		{
			if (!file.owner || !file.native || !file.connection_observation ||
				file.role != sqlite_backend_file_role::main_database)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			auto node = file.native;
			std::unique_lock lifecycle_lock{node->writer_lifecycle_mutex, std::try_to_lock};
			if (!lifecycle_lock.owns_lock())
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			const auto context = file.owner->source_shm_reader_context();
			const bool has_pending = !node->writer_pending.empty();
			const bool has_holders = !node->writer_holders.empty();
			if (!context || !context->registry || !context->family || !context->family_binding ||
				!context->alias_lifetime || (!has_pending && !has_holders) ||
				(has_pending && has_holders))
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}

			struct released_writer
			{
				sqlite_shm_writer_release release;
				sqlite_shm_callback_execution_receipt callback;
			};

			try
			{
				if (has_pending)
				{
					auto callback = make_writer_callback(*file.owner, *node);
					if (!callback)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					auto cleanup = context->registry->begin_writer_cleanup(
						*context->family, node->writer_pending.front(), *callback);
					int native_status{};
					bool native_known{};
					auto* raw = underlying_file(file);
					const auto* methods = underlying_methods(file);
					if (raw != nullptr && methods != nullptr && methods->shm_unmap != nullptr)
					{
						try
						{
							native_status = methods->shm_unmap(raw, 0);
							native_known = true;
						}
						catch (...)
						{
							native_known = false;
						}
					}
					if (!cleanup ||
						!complete_writer_native_cleanup(
							*context, *cleanup, *callback, native_status, native_known) ||
						native_status != sqlite_ok)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					if (remove_file != 0 && file.owner->remove_writer_shm_sidecar() != sqlite_ok)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					node->writer_pending.clear();
					node->writer_pending_attachment.reset();
					{
						std::scoped_lock lock{file.connection_observation->mutex};
						file.connection_observation->held_shm_locks.clear();
						file.connection_observation->shared_memory_object_identity.reset();
						file.connection_observation->shared_memory_entry_identity.reset();
					}
					file.shm_readonly_cannot_initialize = false;
					file.source_shm_readonly_family_seen = false;
					return sqlite_ok;
				}

				std::vector<released_writer> releases;
				releases.reserve(node->writer_holders.size());
				bool release_failed{};
				for (auto& holder : node->writer_holders)
				{
					auto callback = make_writer_callback(*file.owner, *node);
					if (!callback)
					{
						release_failed = true;
						break;
					}
					auto released = context->registry->release_writer_holder(
						*context->family, holder, *callback);
					if (!released)
					{
						release_failed = true;
						break;
					}
					releases.push_back(released_writer{std::move(*released), std::move(*callback)});
				}

				const auto native_cleanup = [&]() noexcept
				{
					int status{};
					bool known{};
					auto* raw = underlying_file(file);
					const auto* methods = underlying_methods(file);
					if (raw != nullptr && methods != nullptr && methods->shm_unmap != nullptr)
					{
						try
						{
							status = methods->shm_unmap(raw, 0);
							known = true;
						}
						catch (...)
						{
							known = false;
						}
					}
					return std::pair<int, bool>{status, known};
				};

				bool retirement_blocked{};
				bool retirement_quarantined{};
				for (auto& item : releases)
				{
					const auto decision = item.release.decision();
					if (decision == sqlite_shm_writer_retirement_decision::wait_for_inflight)
					{
						const auto polled = context->registry->poll_writer_retirement(
							*context->family, item.release.cleanup(), item.callback);
						if (!polled ||
							polled->decision != sqlite_shm_writer_retirement_decision::ready)
						{
							(void)context->registry->fail_writer_retirement_wait(
								*context->family,
								item.release.cleanup(),
								item.callback,
								sqlite_shm_retirement_wait_failure::timeout);
							retirement_blocked = true;
						}
					}
					else if (decision ==
								 sqlite_shm_writer_retirement_decision::quarantine_same_thread ||
							 decision == sqlite_shm_writer_retirement_decision::quarantined)
						retirement_quarantined = true;
				}

				if (release_failed || retirement_blocked || retirement_quarantined)
				{
					(void)native_cleanup();
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				const auto [native_status, native_known] = native_cleanup();
				bool cleanup_confirmed = native_known && native_status == sqlite_ok;
				for (auto& item : releases)
				{
					cleanup_confirmed = complete_writer_native_cleanup(*context,
																	   item.release.cleanup(),
																	   item.callback,
																	   native_status,
																	   native_known) &&
						cleanup_confirmed;
				}
				if (!cleanup_confirmed)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (remove_file != 0 && file.owner->remove_writer_shm_sidecar() != sqlite_ok)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				node->writer_holders.clear();
				{
					std::scoped_lock lock{file.connection_observation->mutex};
					file.connection_observation->held_shm_locks.clear();
					file.connection_observation->shared_memory_object_identity.reset();
					file.connection_observation->shared_memory_entry_identity.reset();
				}
				file.shm_readonly_cannot_initialize = false;
				file.source_shm_readonly_family_seen = false;
				return sqlite_ok;
			}
			catch (...)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
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
				if (file->connection_observation)
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					++file->connection_observation->native_shm_map_attempt_count;
				}
				if (!native_operation_permitted(*file))
					return sqlite_io_error;
				if (file->role == sqlite_backend_file_role::main_database && file->main_handle &&
					file->native && file->connection_observation)
				{
					bool writer_epoch_requested{};
					bool writer_epoch_armed{};
					bool writer_read_only{};
					bool writer_wal_lock_observed{};
					{
						std::scoped_lock lock{file->connection_observation->mutex};
						writer_epoch_requested =
							file->connection_observation->writer_shm_mapping_epoch_requested;
						writer_epoch_armed =
							file->connection_observation->writer_target_namespace_epoch != nullptr;
						writer_read_only = file->connection_observation->main_handle_read_only;
						writer_wal_lock_observed = std::ranges::any_of(
							file->connection_observation->held_shm_locks,
							[](const auto& shm_lock)
							{
								return shm_lock.offset == 0 && shm_lock.count == 1 &&
									shm_lock.mode == sqlite_backend_shm_lock_mode::exclusive;
							});
					}
					if (writer_epoch_requested && !writer_epoch_armed && !writer_read_only &&
						writer_wal_lock_observed)
					{
						if (auto armed = file->owner->arm_requested_writer_epoch_before_native_map(
								*file->connection_observation);
							!armed)
						{
							mark_source_shm_terminal_failure(*file);
							return sqlite_io_error;
						}
					}
				}
				bool writer_route{};
				bool writer_epoch{};
				bool writer_epoch_match{};
				if (file->role == sqlite_backend_file_role::main_database && file->main_handle &&
					file->native && file->connection_observation)
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					writer_epoch =
						file->connection_observation->writer_target_namespace_epoch != nullptr;
					writer_epoch_match = writer_epoch &&
						file->connection_observation->writer_target_namespace_epoch ==
							file->native->target_namespace_epoch;
					// The epoch is armed before SQLite's opening xShmMap.  Eligibility may be
					// installed only after that callback returns, so route every exact epoch
					// attempt through the writer pending state machine.
					writer_route = writer_epoch_match;
				}
				if (writer_route)
				{
					return forwarding_writer_shm_map(*file, page, page_size, extend, output);
				}
				if (file->source_shm_readonly_qualified)
				{
					if (extend != 0 && extend != 1)
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					if (!file->source_shm_qualification_candidate && file->native &&
						file->native->reader_open_authority)
					{
						const auto result =
							forwarding_source_shm_map(*file, page, page_size, extend, output);
						return result;
					}
					auto* raw = underlying_file(*file);
					const auto* methods = underlying_methods(*file);
					if (raw == nullptr || methods == nullptr || methods->shm_map == nullptr)
						return sqlite_io_error;
					if (!file->native || !native_shm_callback_identity_valid(*file->native))
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
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
					if (!file->native || !native_shm_callback_identity_valid(*file->native))
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
							 file->owner->pinned_underlying_vfs_app_data_identity(),
							 native_mapping,
							 std::nullopt}))
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
				if (file->source_shm_readonly_qualified)
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

		[[nodiscard]] int close_source_reader_native(std::shared_ptr<native_file_node>& node,
													 int (*close_callback)(sqlite3_file*)) noexcept
		{
			if (!node || !node->owner || !node->reader_open_authority || close_callback == nullptr)
				return sqlite_io_error;
			auto owner = node->owner;
			const auto context = owner->source_shm_reader_context();
			if (!context || context->registry == nullptr || context->family == nullptr)
				return sqlite_io_error;
			std::unique_lock lifecycle_lock{node->reader_lifecycle_mutex, std::try_to_lock};
			if (!lifecycle_lock.owns_lock())
				return sqlite_io_error;
			try
			{
				const auto open_view =
					sqlite_shm_reader_lifecycle_production_factory::open_epoch_view(
						*context->registry, *node->reader_open_authority);
				if (!open_view)
					return sqlite_io_error;
				auto issuer = sqlite_shm_reader_lifecycle_production_factory::identity_issuer(
					*context->registry);
				if (!issuer.valid())
					return sqlite_io_error;

				auto issue_callback = [&](sqlite_shm_reader_lifecycle_identity_scope& scope,
										  const sqlite_shm_reader_callback_identity_role role,
										  const std::string_view purpose)
					-> std::optional<sqlite_shm_issued_reader_callback_identity>
				{
					try
					{
						auto thread_identity = make_source_reader_identity(*owner, *node, purpose);
						if (!thread_identity)
							return std::nullopt;
						auto permit =
							issuer.reserve_callback(scope, role, std::move(*thread_identity), 0U);
						if (!permit)
							return std::nullopt;
						auto sealed = issuer.seal_callback(*permit, scope, role);
						if (!sealed)
							return std::nullopt;
						return std::optional<sqlite_shm_issued_reader_callback_identity>{
							std::move(*sealed)};
					}
					catch (...)
					{
						return std::nullopt;
					}
				};

				if (node->reader_handoff)
				{
					if (!complete_source_reader_session(
							*owner,
							*node,
							*context,
							sqlite_shm_reader_session_terminal_kind::success,
							true,
							true))
						return sqlite_io_error;
					auto& handoff = *node->reader_handoff;
					auto unmap_request_seal =
						make_source_reader_lifecycle_request_seal(*owner,
																  *node,
																  "reader-live-close-unmap",
																  open_view->registry_open_token,
																  handoff.generation());
					auto close_request_seal =
						make_source_reader_lifecycle_request_seal(*owner,
																  *node,
																  "reader-live-close-close",
																  open_view->close_owner_token,
																  node->reader_generation);
					if (!unmap_request_seal || !close_request_seal)
						return sqlite_io_error;
					auto unmap_scope =
						sqlite_shm_reader_lifecycle_production_factory::seal_handoff_scope(
							*context->registry,
							*context->family,
							handoff,
							open_view->registry_open_token,
							open_view->binding.callback_cohort,
							*unmap_request_seal);
					auto close_scope = sqlite_shm_reader_lifecycle_production_factory::seal_scope(
						*context->registry,
						*context->family,
						open_view->binding.callback_cohort,
						*close_request_seal,
						open_view->registry_open_token,
						sqlite_shm_reader_lifecycle_owner_kind::close,
						open_view->close_owner_token,
						node->reader_generation);
					if (!unmap_scope.valid() || !close_scope.valid())
						return sqlite_io_error;
					auto unmap_callback =
						issue_callback(unmap_scope,
									   sqlite_shm_reader_callback_identity_role::attachment_unmap,
									   "reader-live-close-unmap-thread");
					auto close_callback_identity =
						issue_callback(close_scope,
									   sqlite_shm_reader_callback_identity_role::close,
									   "reader-live-close-thread");
					if (!unmap_callback || !close_callback_identity ||
						unmap_callback->receipt().invocation_token ==
							close_callback_identity->receipt().invocation_token)
						return sqlite_io_error;
					auto begun = context->registry->begin_reader_live_close(
						*context->family,
						*node->reader_open_authority,
						handoff,
						{unmap_callback->receipt(), 0, 0},
						{close_callback_identity->receipt()});
					if (!begun)
						return sqlite_io_error;
					node->reader_handoff.reset();
					auto live_close = std::move(*begun);
					const auto quarantine_live_close_unmap = [&]() noexcept
					{
						auto terminal = sqlite_shm_reader_lifecycle_production_factory::
							make_live_close_unmap_terminal(
								live_close,
								unmap_callback->receipt(),
								sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
								std::nullopt,
								0,
								0,
								std::nullopt,
								std::nullopt);
						auto unmapped = context->registry->complete_reader_live_close_unmap(
							*context->family, *node->reader_open_authority, live_close, terminal);
						if (!unmapped)
							return false;
						const auto retired_unmap =
							static_cast<bool>(issuer.retire_callback(
								unmap_scope,
								*unmap_callback,
								sqlite_shm_reader_callback_identity_role::attachment_unmap)) &&
							static_cast<bool>(issuer.retire_scope(unmap_scope));
						const auto retired_close =
							static_cast<bool>(issuer.retire_callback(
								close_scope,
								*close_callback_identity,
								sqlite_shm_reader_callback_identity_role::close)) &&
							static_cast<bool>(issuer.retire_scope(close_scope));
						return retired_unmap && retired_close &&
							unmapped->kind() ==
							sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined;
					};
					auto cut = context->registry->poll_reader_live_close_unmap_cut(
						*context->family,
						*node->reader_open_authority,
						live_close,
						close_callback_identity->receipt());
					if (!cut ||
						cut->progress != sqlite_shm_reader_unmap_cut_progress::native_effect_ready)
					{
						if (cut)
							(void)context->registry->fail_reader_live_close_unmap_cut_wait(
								*context->family,
								*node->reader_open_authority,
								live_close,
								close_callback_identity->receipt(),
								sqlite_shm_retirement_wait_failure::timeout);
						return sqlite_io_error;
					}
					auto* raw = node->file();
					const auto& methods = node->trusted_methods;
					if (raw == nullptr || methods.shm_unmap == nullptr ||
						!native_shm_callback_identity_valid(*node))
					{
						(void)quarantine_live_close_unmap();
						return sqlite_io_error;
					}
					int native_unmap_status{};
					bool native_unmap_known{};
					try
					{
						native_unmap_status = methods.shm_unmap(raw, 0);
						native_unmap_known = true;
					}
					catch (...)
					{
						native_unmap_known = false;
					}
					if (!native_shm_callback_identity_valid(*node))
					{
						(void)quarantine_live_close_unmap();
						return sqlite_io_error;
					}
					std::optional<sqlite_shm_issued_reader_effect_identity> unmap_effect;
					std::optional<sqlite_shm_issued_reader_effect_identity> unmap_latch;
					if (native_unmap_known)
					{
						auto effect = issuer.issue_effect(
							unmap_scope,
							*unmap_callback,
							sqlite_shm_reader_effect_identity_role::native_unmap);
						if (!effect)
							return sqlite_io_error;
						unmap_effect.emplace(std::move(*effect));
						if (native_unmap_status == sqlite_ok)
						{
							auto latch = issuer.issue_effect(
								unmap_scope,
								*unmap_callback,
								sqlite_shm_reader_effect_identity_role::latch_reset);
							if (!latch)
								return sqlite_io_error;
							unmap_latch.emplace(std::move(*latch));
						}
					}
					auto unmap_terminal = sqlite_shm_reader_lifecycle_production_factory::
						make_live_close_unmap_terminal(
							live_close,
							unmap_callback->receipt(),
							native_unmap_known
								? sqlite_shm_reader_unmap_evidence_kind::exact_native_result
								: sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
							native_unmap_known ? std::optional<int>{native_unmap_status}
											   : std::nullopt,
							0,
							0,
							unmap_effect
								? std::optional<sqlite_backend_opaque_identity>{unmap_effect
																					->identity()}
								: std::nullopt,
							unmap_latch
								? std::optional<sqlite_backend_opaque_identity>{unmap_latch
																					->identity()}
								: std::nullopt);
					auto unmapped = context->registry->complete_reader_live_close_unmap(
						*context->family, *node->reader_open_authority, live_close, unmap_terminal);
					if (!unmapped)
						return sqlite_io_error;
					bool retire_unmap_ok = true;
					if (unmap_latch)
						retire_unmap_ok = static_cast<bool>(issuer.retire_effect(
							unmap_scope,
							*unmap_callback,
							*unmap_latch,
							sqlite_shm_reader_effect_identity_role::latch_reset));
					if (unmap_effect)
						retire_unmap_ok = retire_unmap_ok &&
							static_cast<bool>(issuer.retire_effect(
								unmap_scope,
								*unmap_callback,
								*unmap_effect,
								sqlite_shm_reader_effect_identity_role::native_unmap));
					retire_unmap_ok = retire_unmap_ok &&
						static_cast<bool>(issuer.retire_callback(
							unmap_scope,
							*unmap_callback,
							sqlite_shm_reader_callback_identity_role::attachment_unmap));
					retire_unmap_ok =
						retire_unmap_ok && static_cast<bool>(issuer.retire_scope(unmap_scope));
					if (!retire_unmap_ok ||
						unmapped->kind() !=
							sqlite_shm_reader_unmap_terminal_kind::retired_confirmed)
						return sqlite_io_error;
					const auto quarantine_live_close = [&]() noexcept
					{
						auto terminal = sqlite_shm_reader_lifecycle_production_factory::
							make_live_close_terminal(
								live_close,
								close_callback_identity->receipt(),
								sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
								std::nullopt,
								std::nullopt);
						auto closed = context->registry->complete_reader_live_close(
							*context->family, *node->reader_open_authority, live_close, terminal);
						if (!closed)
							return false;
						const auto retired =
							static_cast<bool>(issuer.retire_callback(
								close_scope,
								*close_callback_identity,
								sqlite_shm_reader_callback_identity_role::close)) &&
							static_cast<bool>(issuer.retire_scope(close_scope));
						return retired &&
							closed->kind() ==
							sqlite_shm_reader_close_terminal_kind::terminal_quarantined;
					};
					if (!native_shm_callback_identity_valid(*node))
					{
						(void)quarantine_live_close();
						return sqlite_io_error;
					}

					int native_close_status{};
					bool native_close_known{};
					try
					{
						native_close_status = close_callback(raw);
						native_close_known = true;
					}
					catch (...)
					{
						native_close_known = false;
					}
					if (!native_shm_callback_identity_valid(*node, true))
					{
						(void)quarantine_live_close();
						return sqlite_io_error;
					}
					std::optional<sqlite_shm_issued_reader_effect_identity> close_effect;
					if (native_close_known)
					{
						auto effect = issuer.issue_effect(
							close_scope,
							*close_callback_identity,
							sqlite_shm_reader_effect_identity_role::native_close);
						if (!effect)
							return sqlite_io_error;
						close_effect.emplace(std::move(*effect));
					}
					auto close_terminal =
						sqlite_shm_reader_lifecycle_production_factory::make_live_close_terminal(
							live_close,
							close_callback_identity->receipt(),
							native_close_known
								? sqlite_shm_reader_close_evidence_kind::exact_native_result
								: sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
							native_close_known ? std::optional<int>{native_close_status}
											   : std::nullopt,
							close_effect
								? std::optional<sqlite_backend_opaque_identity>{close_effect
																					->identity()}
								: std::nullopt);
					auto closed = context->registry->complete_reader_live_close(
						*context->family, *node->reader_open_authority, live_close, close_terminal);
					if (!closed)
						return sqlite_io_error;
					bool retire_close_ok = true;
					if (close_effect)
						retire_close_ok = static_cast<bool>(issuer.retire_effect(
							close_scope,
							*close_callback_identity,
							*close_effect,
							sqlite_shm_reader_effect_identity_role::native_close));
					retire_close_ok = retire_close_ok &&
						static_cast<bool>(issuer.retire_callback(
							close_scope,
							*close_callback_identity,
							sqlite_shm_reader_callback_identity_role::close));
					retire_close_ok =
						retire_close_ok && static_cast<bool>(issuer.retire_scope(close_scope));
					if (!retire_close_ok ||
						closed->kind() != sqlite_shm_reader_close_terminal_kind::closed)
						return sqlite_io_error;
					if (!owner->release_source_reader_open(*node))
						return sqlite_io_error;
					node->reader_generation = 0U;
					clear_source_shm_projection_receipts(node->observation);
					return sqlite_ok;
				}

				if (node->reader_session &&
					node->reader_session->phase() !=
						sqlite_shm_reader_session_phase::reserved_for_first_map)
					return sqlite_io_error;
				if (!complete_source_reader_session(
						*owner,
						*node,
						*context,
						sqlite_shm_reader_session_terminal_kind::cancelled_before_authority_read,
						false,
						true))
					return sqlite_io_error;
				auto close_request_seal = make_source_reader_lifecycle_request_seal(
					*owner, *node, "reader-close", open_view->close_owner_token, 0U);
				if (!close_request_seal)
					return sqlite_io_error;
				auto close_scope = sqlite_shm_reader_lifecycle_production_factory::seal_scope(
					*context->registry,
					*context->family,
					open_view->binding.callback_cohort,
					*close_request_seal,
					open_view->registry_open_token,
					sqlite_shm_reader_lifecycle_owner_kind::close,
					open_view->close_owner_token,
					0U);
				if (!close_scope.valid())
					return sqlite_io_error;
				auto close_callback_identity =
					issue_callback(close_scope,
								   sqlite_shm_reader_callback_identity_role::close,
								   "reader-close-thread");
				if (!close_callback_identity)
					return sqlite_io_error;
				auto begun =
					context->registry->begin_reader_close(*context->family,
														  *node->reader_open_authority,
														  {close_callback_identity->receipt()});
				if (!begun)
					return sqlite_io_error;
				auto close = std::move(*begun);
				const auto quarantine_close = [&]() noexcept
				{
					auto terminal =
						sqlite_shm_reader_lifecycle_production_factory::make_close_terminal(
							close,
							close_callback_identity->receipt(),
							sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
							std::nullopt,
							std::nullopt);
					auto closed = context->registry->complete_reader_close(
						*context->family, *node->reader_open_authority, close, terminal);
					if (!closed)
						return false;
					const auto retired = static_cast<bool>(issuer.retire_callback(
											 close_scope,
											 *close_callback_identity,
											 sqlite_shm_reader_callback_identity_role::close)) &&
						static_cast<bool>(issuer.retire_scope(close_scope));
					return retired &&
						closed->kind() ==
						sqlite_shm_reader_close_terminal_kind::terminal_quarantined;
				};
				if (!native_shm_callback_identity_valid(*node))
				{
					(void)quarantine_close();
					return sqlite_io_error;
				}
				if (!close.native_effect_ready())
				{
					auto cut = context->registry->poll_reader_close_cut(
						*context->family,
						*node->reader_open_authority,
						close,
						close_callback_identity->receipt());
					if (!(cut &&
						  cut->progress ==
							  sqlite_shm_reader_close_cut_progress::native_effect_ready))
					{
						if (cut)
							(void)context->registry->fail_reader_close_cut_wait(
								*context->family,
								*node->reader_open_authority,
								close,
								close_callback_identity->receipt(),
								sqlite_shm_retirement_wait_failure::timeout);
						return sqlite_io_error;
					}
				}
				if (!native_shm_callback_identity_valid(*node))
				{
					(void)quarantine_close();
					return sqlite_io_error;
				}
				int native_status{};
				bool native_known{};
				try
				{
					native_status = close_callback(node->file());
					native_known = true;
				}
				catch (...)
				{
					native_known = false;
				}
				if (!native_shm_callback_identity_valid(*node, true))
				{
					(void)quarantine_close();
					return sqlite_io_error;
				}
				std::optional<sqlite_shm_issued_reader_effect_identity> effect_identity;
				if (native_known)
				{
					auto effect =
						issuer.issue_effect(close_scope,
											*close_callback_identity,
											sqlite_shm_reader_effect_identity_role::native_close);
					if (!effect)
						return sqlite_io_error;
					effect_identity.emplace(std::move(*effect));
				}
				auto terminal = sqlite_shm_reader_lifecycle_production_factory::make_close_terminal(
					close,
					close_callback_identity->receipt(),
					native_known ? sqlite_shm_reader_close_evidence_kind::exact_native_result
								 : sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
					native_known ? std::optional<int>{native_status} : std::nullopt,
					effect_identity
						? std::optional<sqlite_backend_opaque_identity>{effect_identity->identity()}
						: std::nullopt);
				auto closed = context->registry->complete_reader_close(
					*context->family, *node->reader_open_authority, close, terminal);
				if (!closed)
					return sqlite_io_error;
				bool retire_ok = true;
				if (effect_identity)
					retire_ok = static_cast<bool>(
						issuer.retire_effect(close_scope,
											 *close_callback_identity,
											 *effect_identity,
											 sqlite_shm_reader_effect_identity_role::native_close));
				retire_ok = retire_ok &&
					static_cast<bool>(issuer.retire_callback(
						close_scope,
						*close_callback_identity,
						sqlite_shm_reader_callback_identity_role::close));
				retire_ok = retire_ok && static_cast<bool>(issuer.retire_scope(close_scope));
				if (!retire_ok || closed->kind() != sqlite_shm_reader_close_terminal_kind::closed)
					return sqlite_io_error;
				if (!owner->release_source_reader_open(*node))
					return sqlite_io_error;
				return sqlite_ok;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		[[nodiscard]] int forwarding_source_shm_unmap(forwarding_file& file,
													  const int remove_file) noexcept
		{
			if (!file.source_shm_readonly_qualified || file.source_shm_qualification_candidate ||
				file.native == nullptr || !file.owner || !file.native->reader_open_authority ||
				!file.native->reader_handoff || remove_file != 0)
				return sqlite_io_error;
			const auto context = file.owner->source_shm_reader_context();
			if (!context || context->registry == nullptr || context->family == nullptr ||
				context->family_binding == nullptr || context->alias_lifetime == nullptr)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			std::unique_lock lifecycle_lock{file.native->reader_lifecycle_mutex, std::try_to_lock};
			if (!lifecycle_lock.owns_lock())
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
			try
			{
				const auto open_view =
					sqlite_shm_reader_lifecycle_production_factory::open_epoch_view(
						*context->registry, *file.native->reader_open_authority);
				if (!open_view ||
					!complete_source_reader_session(
						*file.owner,
						*file.native,
						*context,
						sqlite_shm_reader_session_terminal_kind::success,
						true,
						true))
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				auto& handoff = *file.native->reader_handoff;
				auto request_seal =
					make_source_reader_lifecycle_request_seal(*file.owner,
															  *file.native,
															  "reader-unmap",
															  open_view->registry_open_token,
															  handoff.generation());
				if (!request_seal)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto scope = sqlite_shm_reader_lifecycle_production_factory::seal_handoff_scope(
					*context->registry,
					*context->family,
					handoff,
					open_view->registry_open_token,
					open_view->binding.callback_cohort,
					*request_seal);
				if (!scope.valid())
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto issuer = sqlite_shm_reader_lifecycle_production_factory::identity_issuer(
					*context->registry);
				if (!issuer.valid())
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto thread_identity =
					make_source_reader_identity(*file.owner, *file.native, "reader-unmap-thread");
				if (!thread_identity)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto permit = issuer.reserve_callback(
					scope,
					sqlite_shm_reader_callback_identity_role::attachment_unmap,
					std::move(*thread_identity),
					0U);
				if (!permit)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto callback_result = issuer.seal_callback(
					*permit, scope, sqlite_shm_reader_callback_identity_role::attachment_unmap);
				if (!callback_result)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				auto callback = std::move(*callback_result);
				auto begun = context->registry->begin_reader_unmap(
					*context->family, handoff, callback.receipt());
				if (!begun)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				file.native->reader_handoff.reset();
				auto unmap = std::move(*begun);
				const auto quarantine_unmap = [&]() noexcept
				{
					auto terminal =
						sqlite_shm_reader_lifecycle_production_factory::make_unmap_terminal(
							unmap,
							callback.receipt(),
							sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
							std::nullopt,
							0,
							0,
							std::nullopt,
							std::nullopt);
					auto completed =
						context->registry->complete_reader_unmap(*context->family, unmap, terminal);
					if (!completed)
						return false;
					const auto retired =
						static_cast<bool>(issuer.retire_callback(
							scope,
							callback,
							sqlite_shm_reader_callback_identity_role::attachment_unmap)) &&
						static_cast<bool>(issuer.retire_scope(scope));
					return retired &&
						completed->kind() ==
						sqlite_shm_reader_unmap_terminal_kind::terminal_quarantined;
				};
				auto cut = context->registry->poll_reader_unmap_cut(
					*context->family, unmap, callback.receipt());
				if (!cut ||
					cut->progress != sqlite_shm_reader_unmap_cut_progress::native_effect_ready)
				{
					if (cut)
						(void)context->registry->fail_reader_unmap_cut_wait(
							*context->family,
							unmap,
							callback.receipt(),
							sqlite_shm_retirement_wait_failure::timeout);
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}

				auto* raw = underlying_file(file);
				const auto* methods = underlying_methods(file);
				if (raw == nullptr || methods == nullptr || methods->shm_unmap == nullptr ||
					!file.native || !native_shm_callback_identity_valid(*file.native))
				{
					(void)quarantine_unmap();
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				int native_status{};
				bool native_known{};
				try
				{
					native_status = methods->shm_unmap(raw, 0);
					native_known = true;
				}
				catch (...)
				{
					native_known = false;
				}
				if (!native_shm_callback_identity_valid(*file.native))
				{
					(void)quarantine_unmap();
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				std::optional<sqlite_shm_issued_reader_effect_identity> native_effect;
				std::optional<sqlite_shm_issued_reader_effect_identity> latch_reset;
				if (native_known)
				{
					auto issued = issuer.issue_effect(
						scope, callback, sqlite_shm_reader_effect_identity_role::native_unmap);
					if (!issued)
					{
						mark_source_shm_terminal_failure(file);
						return sqlite_io_error;
					}
					native_effect.emplace(std::move(*issued));
					if (native_status == sqlite_ok)
					{
						auto reset = issuer.issue_effect(
							scope, callback, sqlite_shm_reader_effect_identity_role::latch_reset);
						if (!reset)
						{
							mark_source_shm_terminal_failure(file);
							return sqlite_io_error;
						}
						latch_reset.emplace(std::move(*reset));
					}
				}
				auto terminal = sqlite_shm_reader_lifecycle_production_factory::make_unmap_terminal(
					unmap,
					callback.receipt(),
					native_known ? sqlite_shm_reader_unmap_evidence_kind::exact_native_result
								 : sqlite_shm_reader_unmap_evidence_kind::throw_or_unknown,
					native_known ? std::optional<int>{native_status} : std::nullopt,
					0,
					0,
					native_effect
						? std::optional<sqlite_backend_opaque_identity>{native_effect->identity()}
						: std::nullopt,
					latch_reset
						? std::optional<sqlite_backend_opaque_identity>{latch_reset->identity()}
						: std::nullopt);
				auto completed =
					context->registry->complete_reader_unmap(*context->family, unmap, terminal);
				if (!completed)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				bool retire_ok = true;
				if (latch_reset)
					retire_ok = static_cast<bool>(
						issuer.retire_effect(scope,
											 callback,
											 *latch_reset,
											 sqlite_shm_reader_effect_identity_role::latch_reset));
				if (native_effect)
					retire_ok = retire_ok &&
						static_cast<bool>(issuer.retire_effect(
							scope,
							callback,
							*native_effect,
							sqlite_shm_reader_effect_identity_role::native_unmap));
				retire_ok = retire_ok &&
					static_cast<bool>(issuer.retire_callback(
						scope,
						callback,
						sqlite_shm_reader_callback_identity_role::attachment_unmap));
				retire_ok = retire_ok && static_cast<bool>(issuer.retire_scope(scope));
				if (!retire_ok)
				{
					mark_source_shm_terminal_failure(file);
					return sqlite_io_error;
				}
				if (completed->kind() != sqlite_shm_reader_unmap_terminal_kind::retired_confirmed)
				{
					mark_source_shm_terminal_failure(file);
					return completed->outward_status();
				}
				file.native->reader_generation = 0U;
				file.shm_readonly_cannot_initialize = false;
				file.source_shm_readonly_family_seen = false;
				if (file.connection_observation)
				{
					std::scoped_lock lock{file.connection_observation->mutex};
					for (auto& event : file.connection_observation->shm_map_events)
						event.native_ok_projection_receipt.reset();
					file.connection_observation->held_shm_locks.clear();
					file.connection_observation->shared_memory_object_identity.reset();
					file.connection_observation->shared_memory_entry_identity.reset();
				}
				return sqlite_ok;
			}
			catch (...)
			{
				mark_source_shm_terminal_failure(file);
				return sqlite_io_error;
			}
		}

		int forwarding_shm_unmap(sqlite3_file* base, const int remove_file) noexcept
		{
			try
			{
				auto* file = forwarding(base);
				if (file->source_shm_readonly_qualified &&
					!file->source_shm_qualification_candidate)
				{
					if (file->native && file->native->reader_handoff)
					{
						const auto result = forwarding_source_shm_unmap(*file, remove_file);
						return result;
					}
					if (!native_operation_permitted(*file))
						return sqlite_io_error;
					if (file->source_shm_terminal_failure)
						return sqlite_io_error;
					auto* raw = underlying_file(*file);
					const auto* methods = underlying_methods(*file);
					if (raw == nullptr || methods == nullptr || methods->shm_unmap == nullptr ||
						!file->native || !native_shm_callback_identity_valid(*file->native))
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					int status{};
					try
					{
						status = methods->shm_unmap(raw, 0);
					}
					catch (...)
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					if (!native_shm_callback_identity_valid(*file->native))
					{
						mark_source_shm_terminal_failure(*file);
						return sqlite_io_error;
					}
					if (status != sqlite_ok)
						mark_source_shm_terminal_failure(*file);
					else
					{
						file->shm_readonly_cannot_initialize = false;
						file->source_shm_readonly_family_seen = false;
					}
					return status;
				}
				bool writer_route{};
				if (file->role == sqlite_backend_file_role::main_database && file->native &&
					file->connection_observation)
				{
					std::scoped_lock lock{file->connection_observation->mutex};
					writer_route = file->connection_observation->writer_target_namespace_epoch &&
						file->connection_observation->writer_target_namespace_epoch ==
							file->native->target_namespace_epoch;
				}
				if (!writer_route && file->role == sqlite_backend_file_role::main_database &&
					file->native)
				{
					std::unique_lock lifecycle_lock{file->native->writer_lifecycle_mutex,
													std::try_to_lock};
					if (lifecycle_lock.owns_lock())
						writer_route = !file->native->writer_pending.empty() ||
							!file->native->writer_holders.empty();
				}
				if (writer_route)
				{
					const auto status = forwarding_writer_shm_unmap(*file, remove_file);
					return status;
				}
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
					else if (association.observation->writer_target_namespace_epoch)
					{
						file->target_namespace_epoch =
							association.observation->writer_target_namespace_epoch;
					}
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
				if (file->source_shm_readonly_qualified)
				{
					const auto registration_epoch = owner->source_shm_registration_epoch();
					if (!registration_epoch)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						file->~forwarding_file();
						return sqlite_cannot_open;
					}
					file->native->registration_epoch = *registration_epoch;
				}
				if (association.observation && association.main_handle)
				{
					std::scoped_lock lock{association.observation->mutex};
					file->native->writer_target_namespace_epoch_owner =
						association.observation->writer_target_namespace_epoch != nullptr &&
						association.observation->writer_target_namespace_epoch ==
							file->target_namespace_epoch;
				}
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
				file->native->underlying_methods_identity = raw->methods;
				file->native->underlying_methods_version = raw->methods->version;
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
				auto lifetime_identities = identities;
				if (!record_open_success(association.observation,
										 event_index,
										 local_out_flags,
										 std::move(identities)))
				{
					cleanup_failed_forwarding_open(*file);
					file->~forwarding_file();
					return sqlite_io_error;
				}
				if (lifetime_identities && !association.qualification_fixture &&
					(association.role == sqlite_backend_file_role::main_database ||
					 association.role == sqlite_backend_file_role::write_ahead_log))
				{
					const auto lifetime_sequence = owner->mint_native_lifetime_sequence();
					if (!lifetime_sequence)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						cleanup_failed_forwarding_open(*file);
						file->~forwarding_file();
						return sqlite_io_error;
					}
					const auto sealed_receipts = make_native_lifetime_receipts(*owner,
																			   *file->native,
																			   association.role,
																			   *event_index,
																			   flags,
																			   delegated_flags,
																			   local_out_flags,
																			   *lifetime_identities,
																			   *lifetime_sequence);
					if (!sealed_receipts)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						cleanup_failed_forwarding_open(*file);
						file->~forwarding_file();
						return sqlite_io_error;
					}
					auto produced =
						sqlite_writer_shm_native_lifetime_production_factory::create_source(
							association.role == sqlite_backend_file_role::main_database
								? sqlite_writer_shm_native_lifetime_role::main_database
								: sqlite_writer_shm_native_lifetime_role::write_ahead_log,
							sealed_receipts->lifetime,
							sealed_receipts->semantic,
							sealed_receipts->xopen,
							std::static_pointer_cast<void>(file->native));
					if (!produced)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						cleanup_failed_forwarding_open(*file);
						file->~forwarding_file();
						return sqlite_io_error;
					}
					file->native->writer_lifetime_revoker.emplace(std::move(produced->first));
					file->native->writer_lifetime_source.emplace(std::move(produced->second));
					file->native->writer_native_file_receipt.emplace(sealed_receipts->semantic);
					file->native->writer_native_xopen_receipt.emplace(sealed_receipts->xopen);
					file->native->writer_callback_cohort.emplace(sealed_receipts->callback_cohort);
					file->native->writer_open_epoch.emplace(sealed_receipts->open_epoch);
					if (association.main_handle && association.observation)
					{
						std::scoped_lock lock{association.observation->mutex};
						association.observation->main_native_file_receipt =
							file->native->writer_native_file_receipt;
						association.observation->main_native_xopen_receipt =
							file->native->writer_native_xopen_receipt;
						association.observation->main_callback_cohort =
							file->native->writer_callback_cohort;
						association.observation->main_open_epoch = file->native->writer_open_epoch;
					}
				}
				if (association.main_handle && association.observation &&
					file->source_shm_readonly_qualified)
				{
					if (auto admitted = owner->acquire_source_reader_open(
							*association.observation,
							*file->native,
							observe_source_reader_target_identity(*file));
						!admitted)
					{
						mark_incomplete(association.observation);
						record_open_failure(association.observation, event_index);
						cleanup_failed_forwarding_open(*file);
						file->~forwarding_file();
						return sqlite_io_error;
					}
				}
				if (association.main_handle && association.observation)
				{
					std::scoped_lock lock{association.observation->mutex};
					if (association.role == sqlite_backend_file_role::main_database)
						association.observation->main_native_node = file->native;
					else if (association.role == sqlite_backend_file_role::write_ahead_log)
						association.observation->wal_native_node = file->native;
					association.observation->main_handle_open = true;
					association.observation->main_handle_read_only =
						(delegated_flags & sqlite_open_read_only) != 0 &&
						(delegated_flags & sqlite_open_read_write) == 0 &&
						(local_out_flags & sqlite_open_read_only) != 0 &&
						(local_out_flags & sqlite_open_read_write) == 0;
				}
				if (association.observation &&
					association.role == sqlite_backend_file_role::write_ahead_log)
				{
					std::scoped_lock lock{association.observation->mutex};
					association.observation->wal_native_node = file->native;
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
				const auto status = owner != nullptr && owner->underlying()->remove != nullptr
					? owner->underlying()->remove(owner->underlying(), name, sync_directory)
					: sqlite_io_error;
				return status;
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

	bool sqlite_source_shm_native_ok_projection_production_activation_enabled() noexcept
	{
		return source_shm_native_ok_projection_production_activation;
	}

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
