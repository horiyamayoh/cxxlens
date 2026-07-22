#include "materialization_rooted_vfs.hpp"

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
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include "materialization_io.hpp"
#include "materialization_sqlite_abi.hpp"
#include "sdk/sqlite_backend_effect_gate_internal.hpp"
#include "sdk/sqlite_backend_observation_internal.hpp"
#include "sdk/sqlite_private_snapshot_internal.hpp"
#include "sdk/sqlite_wal_recovery_workspace_internal.hpp"
#include "sdk/store_backend_lifetime_internal.hpp"

#if defined(__linux__)
#include <linux/memfd.h>
#include <linux/openat2.h>
#include <sys/syscall.h>
#endif

#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22::materialization
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
		constexpr int sqlite_open_read_write = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_delete_on_close = 0x00000008;
		constexpr int sqlite_open_exclusive = 0x00000010;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_temporary_database = 0x00000200;
		constexpr int sqlite_open_transient_database = 0x00000400;
		constexpr int sqlite_open_main_journal = 0x00000800;
		constexpr int sqlite_open_temporary_journal = 0x00001000;
		constexpr int sqlite_open_subjournal = 0x00002000;
		constexpr int sqlite_open_super_journal = 0x00004000;
		constexpr int sqlite_open_write_ahead_log = 0x00080000;
		constexpr int sqlite_open_file_type_mask = sqlite_open_main_database |
			sqlite_open_temporary_database | sqlite_open_transient_database |
			sqlite_open_main_journal | sqlite_open_temporary_journal | sqlite_open_subjournal |
			sqlite_open_super_journal | sqlite_open_write_ahead_log;
		constexpr int sqlite_access_exists = 0;
		constexpr int sqlite_access_read_write = 1;
		constexpr int sqlite_shm_unlock = 1;
		constexpr int sqlite_shm_lock = 2;
		constexpr int sqlite_shm_shared = 4;
		constexpr int sqlite_shm_exclusive = 8;
		constexpr int sqlite_lock_none = 0;
		constexpr int sqlite_lock_shared = 1;
		constexpr int sqlite_lock_reserved = 2;
		constexpr int sqlite_lock_pending = 3;
		constexpr int sqlite_lock_exclusive = 4;
		constexpr int sqlite_file_control_lock_state = 1;
		constexpr int sqlite_file_control_last_errno = 4;
		constexpr int sqlite_file_control_size_hint = 5;
		constexpr int sqlite_file_control_powersafe_overwrite = 13;
		constexpr int sqlite_file_control_has_moved = 20;
		constexpr int sqlite_sync_data_only = 0x10;
		constexpr off_t sqlite_pending_byte = 0x40000000;
		constexpr off_t sqlite_reserved_byte = sqlite_pending_byte + 1;
		constexpr off_t sqlite_shared_first = sqlite_pending_byte + 2;
		constexpr off_t sqlite_shared_size = 510;
		constexpr off_t sqlite_shm_lock_base = 120;
		constexpr std::string_view rooted_name_prefix{"/cxxlens-rooted-vfs-v1/"};
		constexpr std::size_t maximum_sqlite_request_path_bytes = 4095U;
		constexpr std::string_view sqlite_journal_suffix{"-journal"};
		constexpr std::string_view sqlite_wal_suffix{"-wal"};
		constexpr std::string_view sqlite_shm_suffix{"-shm"};

		[[nodiscard]] bool sqlite_offset_range_valid(const long long offset,
													 const int count) noexcept
		{
			if (offset < 0 || count < 0)
				return false;
			const auto maximum = static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max());
			const auto begin = static_cast<std::uintmax_t>(offset);
			const auto length = static_cast<std::uintmax_t>(count);
			return begin <= maximum && length <= maximum - begin;
		}

		enum class rooted_sqlite_path_role
		{
			database,
			journal,
			write_ahead_log,
			known_sidecar_or_database,
		};

		struct rooted_sqlite_vfs;
		class rooted_held_object;

		struct rooted_connection_observation final
			: sdk::sqlite_backend_connection_observation_scope
		{
			mutable std::mutex mutex;
			std::string profile{"rooted-vfs-v1"};
			sdk::sqlite_backend_opaque_identity capability_token_value;
			sdk::sqlite_backend_opaque_identity connection_token_value;
			std::string relative_path;
			std::string canonical_locator;
			std::thread::id originating_thread;
			std::vector<sdk::sqlite_backend_open_observation> open_events;
			std::optional<sdk::sqlite_backend_opaque_identity> shared_memory_object_identity;
			std::optional<sdk::sqlite_backend_opaque_identity> shared_memory_entry_identity;
			std::vector<sdk::sqlite_backend_shm_lock_observation> held_shm_locks;
			bool complete{true};
			bool main_claimed{};
			bool main_handle_open{};
			std::unique_ptr<sdk::sqlite_backend_effect_gate_state> effect_gate;

			[[nodiscard]] const sdk::sqlite_backend_opaque_identity& token() const noexcept override
			{
				return connection_token_value;
			}

			[[nodiscard]] sdk::result<sdk::sqlite_backend_connection_observation>
			snapshot() const override
			{
				try
				{
					std::scoped_lock lock{mutex};
					return sdk::sqlite_backend_connection_observation{
						profile,
						capability_token_value,
						connection_token_value,
						open_events,
						shared_memory_object_identity,
						shared_memory_entry_identity,
						held_shm_locks,
						complete,
						main_handle_open,
					};
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(sdk::error{
						"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
				}
				catch (const std::length_error&)
				{
					return sdk::unexpected(sdk::error{
						"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
				}
			}

			[[nodiscard]] sdk::sqlite_backend_effect_gate* effect_gate_port() noexcept override
			{
				return effect_gate.get();
			}
		};

		struct rooted_mapping
		{
			void* address{};
			std::size_t size{};
		};

		struct rooted_sqlite_file
		{
			sqlite3_file base{};
			rooted_sqlite_vfs* owner{};
			int authenticated_descriptor{-1};
			int shared_memory_descriptor{-1};
			int lock_level{sqlite_lock_none};
			int last_errno{};
			std::string relative_path;
			std::string authority_database_path;
			std::vector<rooted_mapping> mappings;
			std::shared_ptr<rooted_connection_observation> connection_observation;
			bool authenticates_database_path{};
			bool powersafe_overwrite{};
		};

		struct authenticated_database_path_entry
		{
			std::string path;
			std::size_t active_open_count{};
			std::size_t pending_open_count{};
			std::size_t authority_lease_count{};
		};

		struct sqlite_vfs_api
		{
			using find_function = sqlite3_vfs* (*)(const char*);
			using register_function = int (*)(sqlite3_vfs*, int);
			using unregister_function = int (*)(sqlite3_vfs*);
			find_function find{};
			register_function register_vfs{};
			unregister_function unregister_vfs{};
		};

		std::mutex rooted_vfs_registration_mutex;
		bool rooted_vfs_registration_active{};

		struct sqlite_library_handle
		{
			sqlite_library_handle() noexcept = default;
			sqlite_library_handle(const sqlite_library_handle&) = delete;
			sqlite_library_handle& operator=(const sqlite_library_handle&) = delete;
			void* value{};

			~sqlite_library_handle()
			{
				if (value != nullptr)
					(void)::dlclose(value);
			}

			[[nodiscard]] void* get() const noexcept
			{
				return value;
			}
		};

		struct rooted_sqlite_vfs final : sdk::sqlite_backend_observation_capability,
										 std::enable_shared_from_this<rooted_sqlite_vfs>
		{
			// Members are destroyed in reverse order: the captured root closes before dlclose.
			sqlite_library_handle sqlite_library;
			materialization_owned_fd root;
			materialization_file_identity root_identity;
			std::string root_digest;
			sdk::sqlite_backend_opaque_identity capability_token_value;
			sqlite_vfs_api api;
			sqlite3_vfs* delegate{};
			sqlite3_vfs wrapper{};
			std::string wrapper_name{"cxxlens-rooted-vfs-v1"};
			std::mutex authenticated_paths_mutex;
			std::vector<authenticated_database_path_entry> authenticated_database_paths;
			std::mutex connection_observations_mutex;
			std::vector<std::weak_ptr<rooted_connection_observation>> connection_observations;
			std::atomic<std::uint64_t> next_connection_observation{1U};
			bool registered{};

			[[nodiscard]] sdk::sqlite_backend_vfs_binding binding() const noexcept override;
			[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
			capability_token() const noexcept override;
			[[nodiscard]] sdk::result<sdk::sqlite_backend_namespace_census>
			capture_namespace(std::string_view canonical_vfs_locator) const override;
			[[nodiscard]] sdk::result<sdk::sqlite_backend_stat_only_entry_observation>
			observe_entry_state_without_open(std::string_view canonical_vfs_locator,
											 sdk::sqlite_backend_file_role role) const override;
			[[nodiscard]] sdk::result<bool>
			recheck_namespace(const sdk::sqlite_backend_namespace_census& prior,
							  std::string_view canonical_vfs_locator) const override;
			[[nodiscard]] sdk::result<sdk::sqlite_backend_zero_main_receipt>
			exclusive_create_sync_zero_main(std::string_view canonical_vfs_locator) override;
			[[nodiscard]]
			sdk::result<std::shared_ptr<sdk::sqlite_backend_private_snapshot_builder>>
			create_private_snapshot() override;
			[[nodiscard]]
			sdk::result<std::shared_ptr<sdk::sqlite_wal_recovery_workspace_builder>>
			create_wal_recovery_workspace() override;
			[[nodiscard]]
			sdk::result<std::shared_ptr<sdk::sqlite_backend_connection_observation_scope>>
			begin_connection_observation(std::string_view canonical_vfs_locator) override;
			[[nodiscard]]
			sdk::result<std::shared_ptr<sdk::sqlite_backend_connection_observation_scope>>
			begin_ephemeral_connection_observation() override;

			~rooted_sqlite_vfs()
			{
				if (registered)
				{
					std::scoped_lock lock{rooted_vfs_registration_mutex};
					if (!rooted_vfs_registration_active ||
						api.find(wrapper_name.c_str()) != &wrapper ||
						api.unregister_vfs(&wrapper) != sqlite_ok)
						std::terminate();
					rooted_vfs_registration_active = false;
					registered = false;
				}
			}
		};

		class rooted_held_object final : public sdk::sqlite_backend_held_object
		{
		  public:
			rooted_held_object(sdk::sqlite_backend_file_role role,
							   materialization_owned_fd object,
							   materialization_owned_fd parent,
							   std::string leaf,
							   sdk::sqlite_backend_opaque_identity object_identity,
							   sdk::sqlite_backend_opaque_identity entry_identity);

			[[nodiscard]] sdk::sqlite_backend_file_role role() const noexcept override;
			[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
			object_identity() const noexcept override;
			[[nodiscard]] const sdk::sqlite_backend_opaque_identity&
			directory_entry_identity() const noexcept override;
			[[nodiscard]] sdk::result<std::uint64_t> size() const override;
			[[nodiscard]] sdk::result<void> read_exact(std::uint64_t offset,
													   std::span<std::byte> output) const override;
			[[nodiscard]] sdk::result<std::string> sha256() const override;
			[[nodiscard]] sdk::result<std::shared_ptr<sdk::sqlite_backend_private_snapshot>>
			copy_exact(sdk::sqlite_backend_private_snapshot_builder& builder,
					   std::span<std::byte> scratch) const override;
			[[nodiscard]] sdk::result<sdk::sqlite_backend_replacement_state>
			recheck_current_entry() const override;

			[[nodiscard]] int descriptor() const noexcept;
			[[nodiscard]] int parent_descriptor() const noexcept;
			[[nodiscard]] const std::string& leaf() const noexcept;

		  private:
			sdk::sqlite_backend_file_role role_;
			materialization_owned_fd object_;
			materialization_owned_fd parent_;
			std::string leaf_;
			sdk::sqlite_backend_opaque_identity object_identity_;
			sdk::sqlite_backend_opaque_identity entry_identity_;
		};

		[[nodiscard]] rooted_sqlite_file* rooted_file(sqlite3_file* file) noexcept
		{
			return reinterpret_cast<rooted_sqlite_file*>(file);
		}

		[[nodiscard]] bool reserve_database_path(rooted_sqlite_vfs& owner,
												 const std::string_view path)
		{
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing != owner.authenticated_database_paths.end())
			{
				if (existing->pending_open_count >=
					std::numeric_limits<std::size_t>::max() - existing->active_open_count)
					return false;
				++existing->pending_open_count;
				return true;
			}
			try
			{
				owner.authenticated_database_paths.emplace_back(std::string{path}, 0U, 1U, 0U);
				return true;
			}
			catch (const std::bad_alloc&)
			{
				return false;
			}
			catch (const std::length_error&)
			{
				return false;
			}
		}

		void cancel_database_path_reservation(rooted_sqlite_vfs& owner,
											  const std::string_view path) noexcept
		{
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing == owner.authenticated_database_paths.end() ||
				existing->pending_open_count == 0U)
				std::terminate();
			--existing->pending_open_count;
			if (existing->active_open_count == 0U && existing->pending_open_count == 0U &&
				existing->authority_lease_count == 0U)
				owner.authenticated_database_paths.erase(existing);
		}

		void commit_database_path_reservation(rooted_sqlite_vfs& owner,
											  const std::string_view path) noexcept
		{
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing == owner.authenticated_database_paths.end() ||
				existing->pending_open_count == 0U ||
				existing->active_open_count == std::numeric_limits<std::size_t>::max())
				std::terminate();
			--existing->pending_open_count;
			++existing->active_open_count;
		}

		void deauthenticate_database_path(rooted_sqlite_vfs& owner, const std::string_view path)
		{
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing == owner.authenticated_database_paths.end() ||
				existing->active_open_count == 0U)
				return;
			--existing->active_open_count;
			if (existing->active_open_count == 0U && existing->pending_open_count == 0U &&
				existing->authority_lease_count == 0U)
				owner.authenticated_database_paths.erase(existing);
		}

		void release_database_path_authority(rooted_sqlite_vfs& owner,
											 const std::string_view path) noexcept
		{
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing == owner.authenticated_database_paths.end() ||
				existing->authority_lease_count == 0U)
				std::terminate();
			--existing->authority_lease_count;
			if (existing->active_open_count == 0U && existing->pending_open_count == 0U &&
				existing->authority_lease_count == 0U)
				owner.authenticated_database_paths.erase(existing);
		}

		class database_path_authority_lease
		{
		  public:
			database_path_authority_lease() noexcept = default;
			database_path_authority_lease(rooted_sqlite_vfs& owner, std::string path) noexcept
				: owner_{&owner}, path_{std::move(path)}
			{
			}
			database_path_authority_lease(const database_path_authority_lease&) = delete;
			database_path_authority_lease& operator=(const database_path_authority_lease&) = delete;
			database_path_authority_lease(database_path_authority_lease&& other) noexcept
				: owner_{std::exchange(other.owner_, nullptr)}, path_{std::move(other.path_)}
			{
			}
			database_path_authority_lease& operator=(database_path_authority_lease&&) = delete;
			~database_path_authority_lease()
			{
				if (owner_ != nullptr)
					release_database_path_authority(*owner_, path_);
			}

			[[nodiscard]] std::string_view database_path() const noexcept
			{
				return path_;
			}

		  private:
			rooted_sqlite_vfs* owner_{};
			std::string path_;
		};

		static_assert(std::is_nothrow_move_constructible_v<database_path_authority_lease>);

		[[nodiscard]] std::optional<database_path_authority_lease>
		acquire_database_path_authority(rooted_sqlite_vfs& owner, const std::string_view path)
		{
			std::string stable_path{path};
			std::scoped_lock lock{owner.authenticated_paths_mutex};
			const auto existing = std::ranges::find(
				owner.authenticated_database_paths, path, &authenticated_database_path_entry::path);
			if (existing == owner.authenticated_database_paths.end() ||
				existing->active_open_count == 0U ||
				existing->authority_lease_count == std::numeric_limits<std::size_t>::max())
				return std::nullopt;
			++existing->authority_lease_count;
			return database_path_authority_lease{owner, std::move(stable_path)};
		}

		struct rooted_file_effect_authority
		{
			std::optional<database_path_authority_lease> lease;
			int failure{sqlite_ok};
		};

		[[nodiscard]] rooted_file_effect_authority
		acquire_file_effect_authority(rooted_sqlite_file& file) noexcept
		{
			if (file.authority_database_path.empty())
				return {};
			try
			{
				auto acquired =
					acquire_database_path_authority(*file.owner, file.authority_database_path);
				if (!acquired)
					return {std::nullopt, sqlite_io_error};
				rooted_file_effect_authority output;
				output.lease.emplace(std::move(*acquired));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return {std::nullopt, sqlite_no_memory};
			}
			catch (const std::length_error&)
			{
				return {std::nullopt, sqlite_no_memory};
			}
			catch (...)
			{
				return {std::nullopt, sqlite_io_error};
			}
		}

		[[nodiscard]] bool
		rooted_persistent_effect_permitted(const rooted_sqlite_file& file,
										   const bool shm_coordination = false) noexcept
		{
			if (file.authority_database_path.empty())
				return true;
			return file.connection_observation != nullptr &&
				file.connection_observation->effect_gate != nullptr &&
				file.connection_observation->effect_gate->permits_persistent_effect(
					shm_coordination);
		}

		[[nodiscard]] sdk::result<void> validate_rooted_sidecar(const std::string_view value,
																const std::string_view suffix)
		{
			if (!value.ends_with(suffix) || value.size() == suffix.size())
				return sdk::unexpected(sdk::error{
					"materialization.identity-mismatch", "sqlite-vfs", "sidecar-suffix"});
			const auto database = value.substr(0U, value.size() - suffix.size());
			if (auto valid = validate_materialization_sqlite_path(database); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		rooted_relative_name(const char* name, const rooted_sqlite_path_role role)
		{
			if (name == nullptr)
				return sdk::unexpected(sdk::error{
					"materialization.identity-mismatch", "sqlite-vfs", "anonymous-file"});
			std::string_view value{name};
			if (!value.starts_with(rooted_name_prefix))
				return sdk::unexpected(sdk::error{
					"materialization.identity-mismatch", "sqlite-vfs", "reserved-namespace"});
			value.remove_prefix(rooted_name_prefix.size());

			sdk::result<void> valid;
			switch (role)
			{
				case rooted_sqlite_path_role::database:
					valid = validate_materialization_sqlite_path(value);
					break;
				case rooted_sqlite_path_role::journal:
					valid = validate_rooted_sidecar(value, sqlite_journal_suffix);
					break;
				case rooted_sqlite_path_role::write_ahead_log:
					valid = validate_rooted_sidecar(value, sqlite_wal_suffix);
					break;
				case rooted_sqlite_path_role::known_sidecar_or_database:
					valid = validate_materialization_sqlite_path(value);
					if (!valid)
						for (const auto suffix :
							 {sqlite_journal_suffix, sqlite_wal_suffix, sqlite_shm_suffix})
						{
							valid = validate_rooted_sidecar(value, suffix);
							if (valid)
								break;
						}
					break;
			}
			if (!valid)
				return sdk::unexpected(std::move(valid.error()));
			return std::string{value};
		}

		[[nodiscard]] std::optional<database_path_authority_lease>
		acquire_named_path_authority(rooted_sqlite_vfs& owner,
									 const std::string_view relative,
									 const rooted_sqlite_path_role role)
		{
			auto acquire_sidecar =
				[&](const std::string_view suffix) -> std::optional<database_path_authority_lease>
			{
				if (auto valid = validate_rooted_sidecar(relative, suffix); !valid)
					return std::nullopt;
				return acquire_database_path_authority(
					owner, relative.substr(0U, relative.size() - suffix.size()));
			};

			switch (role)
			{
				case rooted_sqlite_path_role::database:
					return acquire_database_path_authority(owner, relative);
				case rooted_sqlite_path_role::journal:
					return acquire_sidecar(sqlite_journal_suffix);
				case rooted_sqlite_path_role::write_ahead_log:
					return acquire_sidecar(sqlite_wal_suffix);
				case rooted_sqlite_path_role::known_sidecar_or_database:
					if (auto direct = acquire_database_path_authority(owner, relative))
						return direct;
					for (const auto suffix :
						 {sqlite_journal_suffix, sqlite_wal_suffix, sqlite_shm_suffix})
						if (auto sidecar = acquire_sidecar(suffix))
							return sidecar;
					return std::nullopt;
			}
			return std::nullopt;
		}

		[[nodiscard]] std::optional<rooted_sqlite_path_role>
		rooted_open_path_role(const int flags) noexcept
		{
			switch (flags & sqlite_open_file_type_mask)
			{
				case sqlite_open_main_database:
					return rooted_sqlite_path_role::database;
				case sqlite_open_main_journal:
					return rooted_sqlite_path_role::journal;
				case sqlite_open_write_ahead_log:
					return rooted_sqlite_path_role::write_ahead_log;
				default:
					return std::nullopt;
			}
		}

		[[nodiscard]] bool rooted_anonymous_open_allowed(const int flags) noexcept
		{
			switch (flags & sqlite_open_file_type_mask)
			{
				case sqlite_open_temporary_database:
				case sqlite_open_transient_database:
				case sqlite_open_temporary_journal:
				case sqlite_open_subjournal:
					return true;
				default:
					return false;
			}
		}

		[[nodiscard]] int sqlite_flags_to_open(const int sqlite_flags) noexcept
		{
			int output =
				((sqlite_flags & sqlite_open_read_write) != 0 ? O_RDWR : O_RDONLY) | O_NONBLOCK;
			if ((sqlite_flags & sqlite_open_create) != 0)
				output |= O_CREAT;
			if ((sqlite_flags & sqlite_open_exclusive) != 0)
				output |= O_EXCL;
			return output;
		}

		[[nodiscard]] sdk::result<materialization_owned_fd> rooted_parent(
			const rooted_sqlite_vfs& owner, const std::string_view relative, std::string& leaf)
		{
			const auto separator = relative.rfind('/');
			if (separator == std::string_view::npos)
			{
				leaf = relative;
				const auto descriptor = ::fcntl(owner.root.get(), F_DUPFD_CLOEXEC, 0);
				if (descriptor < 0)
					return sdk::unexpected(
						sdk::error{"materialization.identity-mismatch", "sqlite-vfs", "root-dup"});
				return materialization_owned_fd{descriptor};
			}
			leaf = relative.substr(separator + 1U);
			return open_materialization_beneath(
				owner.root.get(), relative.substr(0U, separator), O_RDONLY | O_DIRECTORY);
		}

		[[nodiscard]] sdk::result<materialization_owned_fd>
		rooted_open(const rooted_sqlite_vfs& owner,
					const std::string_view relative,
					const int flags,
					const std::uint32_t creation_mode = 0U)
		{
			std::string leaf;
			auto parent = rooted_parent(owner, relative, leaf);
			if (!parent)
				return sdk::unexpected(std::move(parent.error()));
			return open_materialization_beneath(parent->get(), leaf, flags, creation_mode);
		}

		[[nodiscard]] bool rooted_regular_file(const int descriptor) noexcept
		{
			struct stat observed{};
			return descriptor >= 0 && ::fstat(descriptor, &observed) == 0 &&
				S_ISREG(observed.st_mode);
		}

		[[nodiscard]] sdk::error observation_error(const std::string_view detail)
		{
			return {"store.sqlite-failure", "sqlite-sidecar-state", std::string{detail}};
		}

		void append_identity_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (std::uint32_t shift = 56U;; shift -= 8U)
			{
				output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
				if (shift == 0U)
					break;
			}
		}

		void append_identity_bytes(std::vector<std::byte>& output, const std::string_view value)
		{
			append_identity_u64(output, static_cast<std::uint64_t>(value.size()));
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		[[nodiscard]] materialization_file_identity identity_from_stat(const struct stat& value)
		{
			return materialization_file_identity{
				static_cast<std::uint64_t>(value.st_dev),
				static_cast<std::uint64_t>(value.st_ino),
				value.st_size < 0 ? 0U : static_cast<std::uint64_t>(value.st_size),
				static_cast<std::uint64_t>(value.st_mode),
				static_cast<std::int64_t>(value.st_mtim.tv_sec),
				static_cast<std::int64_t>(value.st_mtim.tv_nsec),
				static_cast<std::int64_t>(value.st_ctim.tv_sec),
				static_cast<std::int64_t>(value.st_ctim.tv_nsec),
			};
		}

		[[nodiscard]] sdk::sqlite_backend_opaque_identity
		make_opaque_identity(const std::string_view profile,
							 const materialization_file_identity& value,
							 const std::string_view name = {})
		{
			sdk::sqlite_backend_opaque_identity output;
			output.profile = profile;
			output.bytes.reserve(8U * 9U + name.size());
			append_identity_bytes(output.bytes, name);
			append_identity_u64(output.bytes, value.device);
			append_identity_u64(output.bytes, value.inode);
			append_identity_u64(output.bytes, value.size_bytes);
			append_identity_u64(output.bytes, value.mode);
			append_identity_u64(output.bytes, static_cast<std::uint64_t>(value.modified_seconds));
			append_identity_u64(output.bytes,
								static_cast<std::uint64_t>(value.modified_nanoseconds));
			append_identity_u64(output.bytes, static_cast<std::uint64_t>(value.changed_seconds));
			append_identity_u64(output.bytes,
								static_cast<std::uint64_t>(value.changed_nanoseconds));
			return output;
		}

		void append_stable_object_identity(std::vector<std::byte>& output,
									  const materialization_file_identity& value)
		{
			append_identity_u64(output, value.device);
			append_identity_u64(output, value.inode);
			append_identity_u64(
				output, value.mode & static_cast<std::uint64_t>(S_IFMT));
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_opaque_identity>
		opaque_parent_namespace_identity(const int descriptor)
		{
			auto observed = materialization_fd_identity(descriptor, false);
			if (!observed)
				return sdk::unexpected(observation_error("observation-io-failure"));
			try
			{
				sdk::sqlite_backend_opaque_identity output;
				output.profile = "rooted-vfs-v1.parent-namespace.v1";
				output.bytes.reserve(24U);
				append_stable_object_identity(output.bytes, *observed);
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_opaque_identity>
		opaque_fd_identity(const int descriptor, const std::string_view profile)
		{
			auto observed = materialization_fd_identity(descriptor, false);
			if (!observed)
				return sdk::unexpected(observation_error("observation-io-failure"));
			try
			{
				return make_opaque_identity(profile, *observed);
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_opaque_identity> opaque_entry_identity(
			const int parent, const std::string_view leaf, const struct stat& entry)
		{
			auto parent_identity = materialization_fd_identity(parent, false);
			if (!parent_identity)
				return sdk::unexpected(observation_error("observation-io-failure"));
			try
			{
				sdk::sqlite_backend_opaque_identity output;
				output.profile = "rooted-vfs-v1.directory-entry.v1";
				output.bytes.reserve(96U + leaf.size());
				append_stable_object_identity(output.bytes, *parent_identity);
				append_identity_bytes(output.bytes, leaf);
				const auto leaf_identity = identity_from_stat(entry);
				append_identity_u64(output.bytes, leaf_identity.device);
				append_identity_u64(output.bytes, leaf_identity.inode);
				append_identity_u64(output.bytes, leaf_identity.size_bytes);
				append_identity_u64(output.bytes, leaf_identity.mode);
				append_identity_u64(output.bytes,
									static_cast<std::uint64_t>(leaf_identity.modified_seconds));
				append_identity_u64(output.bytes,
									static_cast<std::uint64_t>(leaf_identity.modified_nanoseconds));
				append_identity_u64(output.bytes,
									static_cast<std::uint64_t>(leaf_identity.changed_seconds));
				append_identity_u64(output.bytes,
									static_cast<std::uint64_t>(leaf_identity.changed_nanoseconds));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		[[nodiscard]] sdk::result<materialization_owned_fd>
		duplicate_descriptor(const int descriptor)
		{
			const auto duplicate = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
			if (duplicate < 0)
				return sdk::unexpected(observation_error("observation-io-failure"));
			return materialization_owned_fd{duplicate};
		}

		[[nodiscard]] sdk::result<sdk::sqlite_backend_entry_observation> observe_rooted_entry(
			const int parent, const std::string_view leaf, const sdk::sqlite_backend_file_role role)
		{
			struct stat entry{};
			if (::fstatat(parent, std::string{leaf}.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT)
					return sdk::sqlite_backend_entry_observation{
						role, sdk::sqlite_backend_entry_state::absent, {}, {}, {}};
				return sdk::unexpected(observation_error("observation-io-failure"));
			}
			auto entry_identity = opaque_entry_identity(parent, leaf, entry);
			if (!entry_identity)
				return sdk::unexpected(std::move(entry_identity.error()));
			if (!S_ISREG(entry.st_mode))
				return sdk::sqlite_backend_entry_observation{
					role,
					sdk::sqlite_backend_entry_state::unsupported_kind,
					{},
					std::move(*entry_identity),
					{},
				};

			auto opened = open_materialization_beneath(parent, leaf, O_RDONLY | O_NONBLOCK);
			if (!opened)
			{
				struct stat rechecked{};
				if (::fstatat(parent, std::string{leaf}.c_str(), &rechecked, AT_SYMLINK_NOFOLLOW) !=
						0 ||
					rechecked.st_dev != entry.st_dev || rechecked.st_ino != entry.st_ino ||
					rechecked.st_mode != entry.st_mode)
					return sdk::unexpected(observation_error("observation-io-failure"));
				return sdk::sqlite_backend_entry_observation{
					role,
					sdk::sqlite_backend_entry_state::present_unreadable,
					{},
					std::move(*entry_identity),
					{},
				};
			}
			auto object_identity =
				opaque_fd_identity(opened->get(), "rooted-vfs-v1.open-object.v1");
			if (!object_identity)
				return sdk::unexpected(std::move(object_identity.error()));
			struct stat held_stat{};
			struct stat current_entry{};
			if (::fstat(opened->get(), &held_stat) != 0 || !S_ISREG(held_stat.st_mode) ||
				::fstatat(parent, std::string{leaf}.c_str(), &current_entry, AT_SYMLINK_NOFOLLOW) !=
					0 ||
				held_stat.st_dev != current_entry.st_dev ||
				held_stat.st_ino != current_entry.st_ino)
				return sdk::unexpected(observation_error("observation-io-failure"));
			auto retained_parent = duplicate_descriptor(parent);
			if (!retained_parent)
				return sdk::unexpected(std::move(retained_parent.error()));
			try
			{
				auto held = std::make_shared<rooted_held_object>(role,
																 std::move(*opened),
																 std::move(*retained_parent),
																 std::string{leaf},
																 *object_identity,
																 *entry_identity);
				return sdk::sqlite_backend_entry_observation{
					role,
					sdk::sqlite_backend_entry_state::held_regular,
					std::move(*object_identity),
					std::move(*entry_identity),
					std::move(held),
				};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		[[nodiscard]] bool
		same_entry_observation(const sdk::sqlite_backend_entry_observation& left,
							   const sdk::sqlite_backend_entry_observation& right)
		{
			return left.role == right.role && left.state == right.state &&
				left.object_identity == right.object_identity &&
				left.directory_entry_identity == right.directory_entry_identity;
		}

		rooted_held_object::rooted_held_object(const sdk::sqlite_backend_file_role role,
											   materialization_owned_fd object,
											   materialization_owned_fd parent,
											   std::string leaf,
											   sdk::sqlite_backend_opaque_identity object_identity,
											   sdk::sqlite_backend_opaque_identity entry_identity)
			: role_{role}, object_{std::move(object)}, parent_{std::move(parent)},
			  leaf_{std::move(leaf)}, object_identity_{std::move(object_identity)},
			  entry_identity_{std::move(entry_identity)}
		{
		}

		sdk::sqlite_backend_file_role rooted_held_object::role() const noexcept
		{
			return role_;
		}

		const sdk::sqlite_backend_opaque_identity&
		rooted_held_object::object_identity() const noexcept
		{
			return object_identity_;
		}

		const sdk::sqlite_backend_opaque_identity&
		rooted_held_object::directory_entry_identity() const noexcept
		{
			return entry_identity_;
		}

		int rooted_held_object::descriptor() const noexcept
		{
			return object_.get();
		}

		int rooted_held_object::parent_descriptor() const noexcept
		{
			return parent_.get();
		}

		const std::string& rooted_held_object::leaf() const noexcept
		{
			return leaf_;
		}

		sdk::result<std::uint64_t> rooted_held_object::size() const
		{
			struct stat observed{};
			if (::fstat(object_.get(), &observed) != 0 || observed.st_size < 0 ||
				!S_ISREG(observed.st_mode))
				return sdk::unexpected(observation_error("observation-io-failure"));
			try
			{
				if (make_opaque_identity("rooted-vfs-v1.open-object.v1",
										 identity_from_stat(observed)) != object_identity_)
					return sdk::unexpected(observation_error("concurrent-source-change"));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			return static_cast<std::uint64_t>(observed.st_size);
		}

		sdk::result<void> rooted_held_object::read_exact(const std::uint64_t offset,
														 const std::span<std::byte> output) const
		{
			const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
			if (offset > maximum || output.size() > maximum - offset)
				return sdk::unexpected(observation_error("observation-io-failure"));
			std::size_t consumed{};
			while (consumed < output.size())
			{
				const auto count = ::pread(object_.get(),
										   output.data() + consumed,
										   output.size() - consumed,
										   static_cast<off_t>(offset + consumed));
				if (count > 0)
				{
					consumed += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return sdk::unexpected(observation_error("observation-io-failure"));
			}
			return {};
		}

		sdk::result<std::string> rooted_held_object::sha256() const
		{
			auto byte_count = size();
			if (!byte_count)
				return sdk::unexpected(std::move(byte_count.error()));
			if (*byte_count > std::numeric_limits<std::uint64_t>::max() / 8U)
				return sdk::unexpected(observation_error("observation-byte-count-overflow"));
			auto digest = make_materialization_sha256_accumulator();
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			std::uint64_t offset{};
			while (offset < *byte_count)
			{
				const auto count = static_cast<std::size_t>(
					std::min<std::uint64_t>(buffer.size(), *byte_count - offset));
				if (auto read = read_exact(offset, std::span{buffer}.first(count)); !read)
					return sdk::unexpected(std::move(read.error()));
				if (auto updated = digest->update(std::span{buffer}.first(count)); !updated)
					return sdk::unexpected(observation_error("observation-io-failure"));
				offset += count;
			}
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(observation_error("observation-io-failure"));
			if (auto after = size(); !after || *after != *byte_count)
				return sdk::unexpected(observation_error("concurrent-source-change"));
			return std::move(*finished);
		}

		sdk::result<std::shared_ptr<sdk::sqlite_backend_private_snapshot>>
		rooted_held_object::copy_exact(sdk::sqlite_backend_private_snapshot_builder& builder,
									   const std::span<std::byte> scratch) const
		{
			if (scratch.empty())
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "vfs-observation-buffer"});
			auto replacement = recheck_current_entry();
			if (!replacement ||
				*replacement != sdk::sqlite_backend_replacement_state::exact_same_entry_and_object)
				return sdk::unexpected(observation_error("concurrent-source-change"));
			auto byte_count = size();
			if (!byte_count)
				return sdk::unexpected(std::move(byte_count.error()));
			if (*byte_count > std::numeric_limits<std::uint64_t>::max() / 8U)
				return sdk::unexpected(observation_error("observation-byte-count-overflow"));
			auto digest = make_materialization_sha256_accumulator();
			std::uint64_t offset{};
			while (offset < *byte_count)
			{
				const auto count = static_cast<std::size_t>(
					std::min<std::uint64_t>(scratch.size(), *byte_count - offset));
				auto window = scratch.first(count);
				if (auto read = read_exact(offset, window); !read)
					return sdk::unexpected(std::move(read.error()));
				if (auto updated = digest->update(window); !updated)
					return sdk::unexpected(observation_error("observation-io-failure"));
				if (auto appended = builder.append(window); !appended)
					return sdk::unexpected(std::move(appended.error()));
				offset += count;
			}
			auto finished = digest->finish();
			if (!finished)
				return sdk::unexpected(observation_error("observation-io-failure"));
			replacement = recheck_current_entry();
			if (!replacement ||
				*replacement != sdk::sqlite_backend_replacement_state::exact_same_entry_and_object)
				return sdk::unexpected(observation_error("concurrent-source-change"));
			return builder.seal(*byte_count, *finished);
		}

		sdk::result<sdk::sqlite_backend_replacement_state>
		rooted_held_object::recheck_current_entry() const
		{
			struct stat entry{};
			if (::fstatat(parent_.get(), leaf_.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT)
					return sdk::sqlite_backend_replacement_state::absent;
				if (errno == EACCES || errno == EPERM)
					return sdk::sqlite_backend_replacement_state::unreadable;
				return sdk::unexpected(observation_error("observation-io-failure"));
			}
			if (!S_ISREG(entry.st_mode))
				return sdk::sqlite_backend_replacement_state::unsupported_kind;
			struct stat held{};
			if (::fstat(object_.get(), &held) != 0 || !S_ISREG(held.st_mode))
				return sdk::unexpected(observation_error("observation-io-failure"));
			if (held.st_dev != entry.st_dev || held.st_ino != entry.st_ino)
				return sdk::sqlite_backend_replacement_state::replaced;
			auto current_object = opaque_fd_identity(object_.get(), "rooted-vfs-v1.open-object.v1");
			auto current_entry = opaque_entry_identity(parent_.get(), leaf_, entry);
			if (!current_object || !current_entry)
				return sdk::unexpected(observation_error("observation-io-failure"));
			return *current_object == object_identity_ && *current_entry == entry_identity_
				? sdk::sqlite_backend_replacement_state::exact_same_entry_and_object
				: sdk::sqlite_backend_replacement_state::replaced;
		}

		[[nodiscard]] sdk::result<std::string>
		rooted_observation_relative_locator(const std::string_view canonical_vfs_locator)
		{
			try
			{
				const std::string terminated{canonical_vfs_locator};
				return rooted_relative_name(terminated.c_str(), rooted_sqlite_path_role::database);
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		sdk::sqlite_backend_vfs_binding rooted_sqlite_vfs::binding() const noexcept
		{
			return {
				"rooted-vfs-v1",
				wrapper_name,
				&wrapper,
				this,
				static_cast<const sdk::sqlite_backend_observation_capability*>(this),
				sqlite_library.get(),
				this,
			};
		}

		const sdk::sqlite_backend_opaque_identity&
		rooted_sqlite_vfs::capability_token() const noexcept
		{
			return capability_token_value;
		}

		sdk::result<sdk::sqlite_backend_namespace_census>
		rooted_sqlite_vfs::capture_namespace(const std::string_view canonical_vfs_locator) const
		{
			auto relative = rooted_observation_relative_locator(canonical_vfs_locator);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			std::string leaf;
			auto parent = rooted_parent(*this, *relative, leaf);
			if (!parent)
				return sdk::unexpected(observation_error("observation-io-failure"));
			auto parent_identity = opaque_parent_namespace_identity(parent->get());
			if (!parent_identity)
				return sdk::unexpected(std::move(parent_identity.error()));
			try
			{
				const std::array roles{
					std::pair{sdk::sqlite_backend_file_role::main_database, std::string_view{}},
					std::pair{sdk::sqlite_backend_file_role::write_ahead_log, sqlite_wal_suffix},
					std::pair{sdk::sqlite_backend_file_role::shared_memory, sqlite_shm_suffix},
					std::pair{sdk::sqlite_backend_file_role::rollback_journal,
							  sqlite_journal_suffix},
				};
				sdk::sqlite_backend_namespace_census output;
				output.profile = "rooted-vfs-v1.namespace-census.v1";
				output.capability_token = capability_token_value;
				output.parent_namespace_identity = std::move(*parent_identity);
				for (std::size_t index{}; index < roles.size(); ++index)
				{
					std::string observed_leaf{leaf};
					observed_leaf.append(roles[index].second);
					auto observed =
						observe_rooted_entry(parent->get(), observed_leaf, roles[index].first);
					if (!observed)
						return sdk::unexpected(std::move(observed.error()));
					output.entries[index] = std::move(*observed);
				}
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		sdk::result<sdk::sqlite_backend_stat_only_entry_observation>
		rooted_sqlite_vfs::observe_entry_state_without_open(
			const std::string_view canonical_vfs_locator,
			const sdk::sqlite_backend_file_role role) const
		{
			auto relative = rooted_observation_relative_locator(canonical_vfs_locator);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			std::string leaf;
			auto parent = rooted_parent(*this, *relative, leaf);
			if (!parent)
				return sdk::unexpected(observation_error("observation-io-failure"));
			auto parent_identity = opaque_parent_namespace_identity(parent->get());
			if (!parent_identity)
				return sdk::unexpected(std::move(parent_identity.error()));
			try
			{
				switch (role)
				{
					case sdk::sqlite_backend_file_role::main_database:
						break;
					case sdk::sqlite_backend_file_role::write_ahead_log:
						leaf.append(sqlite_wal_suffix);
						break;
					case sdk::sqlite_backend_file_role::shared_memory:
						leaf.append(sqlite_shm_suffix);
						break;
					case sdk::sqlite_backend_file_role::rollback_journal:
						leaf.append(sqlite_journal_suffix);
						break;
				}

				struct stat before{};
				if (::fstatat(parent->get(), leaf.c_str(), &before, AT_SYMLINK_NOFOLLOW) != 0)
				{
					const auto first_error = errno;
					struct stat retry{};
					if (::fstatat(parent->get(), leaf.c_str(), &retry, AT_SYMLINK_NOFOLLOW) == 0 ||
						errno != first_error)
						return sdk::unexpected(observation_error("concurrent-source-change"));
					if (first_error == ENOENT)
						return sdk::sqlite_backend_stat_only_entry_observation{
							role,
							sdk::sqlite_backend_entry_state::absent,
							std::move(*parent_identity),
							{},
							{},
						};
					if (first_error == EACCES || first_error == EPERM)
						return sdk::sqlite_backend_stat_only_entry_observation{
							role,
							sdk::sqlite_backend_entry_state::present_unreadable,
							std::move(*parent_identity),
							{},
							{},
						};
					return sdk::unexpected(observation_error("observation-io-failure"));
				}

				struct stat after{};
				if (::fstatat(parent->get(), leaf.c_str(), &after, AT_SYMLINK_NOFOLLOW) != 0 ||
					identity_from_stat(before) != identity_from_stat(after))
					return sdk::unexpected(observation_error("concurrent-source-change"));
				auto entry_identity = opaque_entry_identity(parent->get(), leaf, after);
				if (!entry_identity)
					return sdk::unexpected(std::move(entry_identity.error()));
				if (!S_ISREG(after.st_mode))
					return sdk::sqlite_backend_stat_only_entry_observation{
						role,
						sdk::sqlite_backend_entry_state::unsupported_kind,
						std::move(*parent_identity),
						{},
						std::move(*entry_identity),
					};
				return sdk::sqlite_backend_stat_only_entry_observation{
					role,
					sdk::sqlite_backend_entry_state::held_regular,
					std::move(*parent_identity),
					make_opaque_identity("rooted-vfs-v1.open-object.v1", identity_from_stat(after)),
					std::move(*entry_identity),
				};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		sdk::result<bool>
		rooted_sqlite_vfs::recheck_namespace(const sdk::sqlite_backend_namespace_census& prior,
											 const std::string_view canonical_vfs_locator) const
		{
			auto current = capture_namespace(canonical_vfs_locator);
			if (!current)
				return sdk::unexpected(std::move(current.error()));
			if (prior.profile != current->profile ||
				prior.capability_token != current->capability_token ||
				prior.parent_namespace_identity != current->parent_namespace_identity)
				return false;
			for (std::size_t index{}; index < prior.entries.size(); ++index)
				if (!same_entry_observation(prior.entries[index], current->entries[index]))
					return false;
			return true;
		}

		sdk::result<sdk::sqlite_backend_zero_main_receipt>
		rooted_sqlite_vfs::exclusive_create_sync_zero_main(
			const std::string_view canonical_vfs_locator)
		{
			auto relative = rooted_observation_relative_locator(canonical_vfs_locator);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			std::string leaf;
			auto parent = rooted_parent(*this, *relative, leaf);
			if (!parent)
				return sdk::unexpected(observation_error("observation-io-failure"));
			auto parent_identity = opaque_parent_namespace_identity(parent->get());
			if (!parent_identity)
				return sdk::unexpected(std::move(parent_identity.error()));
			auto created = open_materialization_beneath(
				parent->get(), leaf, O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK, 0600U);
			if (!created)
				return sdk::unexpected(sdk::error{
					"store.sqlite-failure", "open", "exclusive-bootstrap-create-failed"});
			struct stat object_stat{};
			if (::fstat(created->get(), &object_stat) != 0 || !S_ISREG(object_stat.st_mode) ||
				object_stat.st_size != 0)
				return sdk::unexpected(sdk::error{
					"store.sqlite-failure", "sqlite-object-kind", "not-regular-or-equivalent"});
			if (::fsync(created->get()) != 0 || ::fsync(parent->get()) != 0)
				return sdk::unexpected(sdk::error{"store.sqlite-failure",
												  "sqlite-initialization-bootstrap",
												  "durability-opaque"});
			struct stat entry_stat{};
			struct stat object_after{};
			if (::fstat(created->get(), &object_after) != 0 || object_after.st_size != 0 ||
				::fstatat(parent->get(), leaf.c_str(), &entry_stat, AT_SYMLINK_NOFOLLOW) != 0 ||
				!S_ISREG(entry_stat.st_mode) || object_after.st_dev != entry_stat.st_dev ||
				object_after.st_ino != entry_stat.st_ino)
				return sdk::unexpected(sdk::error{"store.sqlite-failure",
												  "sqlite-initialization",
												  "concurrent-authority-change"});
			auto object_identity =
				opaque_fd_identity(created->get(), "rooted-vfs-v1.open-object.v1");
			auto entry_identity = opaque_entry_identity(parent->get(), leaf, entry_stat);
			if (!object_identity || !entry_identity)
				return sdk::unexpected(observation_error("observation-io-failure"));
			try
			{
				auto held = std::make_shared<rooted_held_object>(
					sdk::sqlite_backend_file_role::main_database,
					std::move(*created),
					std::move(*parent),
					std::move(leaf),
					*object_identity,
					*entry_identity);
				const auto empty_digest = sdk::content_digest(std::span<const std::byte>{});
				return sdk::sqlite_backend_zero_main_receipt{
					"rooted-vfs-v1.zero-main.v1",
					capability_token_value,
					std::move(*parent_identity),
					std::move(*entry_identity),
					std::move(*object_identity),
					std::move(held),
					0U,
					empty_digest,
					true,
					true,
				};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		sdk::result<std::shared_ptr<sdk::sqlite_backend_private_snapshot_builder>>
		rooted_sqlite_vfs::create_private_snapshot()
		{
			sdk::sqlite_private_snapshot_registry_binding registry;
			registry.runtime_identity = sqlite_library.get();
			registry.pinned_default_vfs = delegate;
			registry.find =
				reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::find_function>(
					api.find);
			registry.register_vfs =
				reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::register_function>(
					api.register_vfs);
			registry.unregister_vfs = reinterpret_cast<
				sdk::sqlite_private_snapshot_registry_binding::unregister_function>(
				api.unregister_vfs);
			registry.runtime_lifetime = shared_from_this();
			return sdk::make_sqlite_private_snapshot_builder(capability_token_value,
															 std::move(registry));
		}

		sdk::result<std::shared_ptr<sdk::sqlite_wal_recovery_workspace_builder>>
		rooted_sqlite_vfs::create_wal_recovery_workspace()
		{
			sdk::sqlite_private_snapshot_registry_binding registry;
			registry.runtime_identity = sqlite_library.get();
			registry.pinned_default_vfs = delegate;
			registry.find =
				reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::find_function>(
					api.find);
			registry.register_vfs =
				reinterpret_cast<sdk::sqlite_private_snapshot_registry_binding::register_function>(
					api.register_vfs);
			registry.unregister_vfs = reinterpret_cast<
				sdk::sqlite_private_snapshot_registry_binding::unregister_function>(
				api.unregister_vfs);
			registry.runtime_lifetime = shared_from_this();
			return sdk::make_sqlite_wal_recovery_workspace_builder(capability_token_value,
																   std::move(registry));
		}

		sdk::result<std::shared_ptr<sdk::sqlite_backend_connection_observation_scope>>
		rooted_sqlite_vfs::begin_connection_observation(
			const std::string_view canonical_vfs_locator)
		{
			auto relative = rooted_observation_relative_locator(canonical_vfs_locator);
			if (!relative)
				return sdk::unexpected(std::move(relative.error()));
			try
			{
				auto observation = std::make_shared<rooted_connection_observation>();
				observation->capability_token_value = capability_token_value;
				observation->relative_path = std::move(*relative);
				observation->canonical_locator = canonical_vfs_locator;
				observation->originating_thread = std::this_thread::get_id();
				observation->open_events.reserve(8U);
				observation->held_shm_locks.reserve(64U);
				const auto token_value =
					next_connection_observation.fetch_add(1U, std::memory_order_relaxed);
				observation->connection_token_value.profile =
					"rooted-vfs-v1.connection-observation.v1";
				append_identity_u64(observation->connection_token_value.bytes, token_value);
				observation->effect_gate = std::make_unique<sdk::sqlite_backend_effect_gate_state>(
					*observation,
					observation->capability_token_value,
					observation->connection_token_value,
					observation->canonical_locator,
					"rooted-vfs-v1",
					true);
				std::scoped_lock lock{connection_observations_mutex};
				for (auto iterator = connection_observations.begin();
					 iterator != connection_observations.end();)
				{
					auto existing = iterator->lock();
					if (!existing)
					{
						iterator = connection_observations.erase(iterator);
						continue;
					}
					std::scoped_lock existing_lock{existing->mutex};
					if (existing->relative_path == observation->relative_path &&
						existing->originating_thread == observation->originating_thread &&
						(existing->main_handle_open || !existing->main_claimed))
						return sdk::unexpected(sdk::error{
							"store.backend-unavailable", "sqlite", "vfs-observation-overlap"});
					++iterator;
				}
				connection_observations.emplace_back(observation);
				return std::static_pointer_cast<sdk::sqlite_backend_connection_observation_scope>(
					std::move(observation));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		sdk::result<std::shared_ptr<sdk::sqlite_backend_connection_observation_scope>>
		rooted_sqlite_vfs::begin_ephemeral_connection_observation()
		{
			try
			{
				auto observation = std::make_shared<rooted_connection_observation>();
				observation->profile = "rooted-ephemeral-v1";
				observation->capability_token_value = capability_token_value;
				observation->canonical_locator = ":memory:";
				observation->originating_thread = std::this_thread::get_id();
				const auto token_value =
					next_connection_observation.fetch_add(1U, std::memory_order_relaxed);
				observation->connection_token_value.profile =
					"rooted-vfs-v1.ephemeral-connection-observation.v1";
				append_identity_u64(observation->connection_token_value.bytes, token_value);
				observation->effect_gate = std::make_unique<sdk::sqlite_backend_effect_gate_state>(
					*observation,
					observation->capability_token_value,
					observation->connection_token_value,
					observation->canonical_locator,
					"rooted-vfs-v1",
					false);
				return std::static_pointer_cast<sdk::sqlite_backend_connection_observation_scope>(
					std::move(observation));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
			catch (const std::length_error&)
			{
				return sdk::unexpected(sdk::error{
					"store.backend-unavailable", "sqlite", "vfs-observation-allocation"});
			}
		}

		[[nodiscard]] int rooted_remove(rooted_sqlite_vfs& owner,
										const std::string_view relative,
										const bool synchronize)
		{
			std::string leaf;
			auto parent = rooted_parent(owner, relative, leaf);
			if (!parent)
				return sqlite_cannot_open;
			struct stat missing_or_rebound{};
#if defined(O_PATH)
			constexpr auto identity_open_flags = O_PATH;
#else
			constexpr auto identity_open_flags = O_RDONLY | O_NONBLOCK;
#endif
			auto authenticated =
				open_materialization_beneath(parent->get(), leaf, identity_open_flags);
			if (!authenticated)
			{
				if (::fstatat(
						parent->get(), leaf.c_str(), &missing_or_rebound, AT_SYMLINK_NOFOLLOW) !=
						0 &&
					errno == ENOENT)
					return sqlite_ok;
				return sqlite_cannot_open;
			}
			struct stat authenticated_identity{};
			struct stat immediate_identity{};
			if (::fstat(authenticated->get(), &authenticated_identity) != 0 ||
				!S_ISREG(authenticated_identity.st_mode) ||
				::fstatat(parent->get(), leaf.c_str(), &immediate_identity, AT_SYMLINK_NOFOLLOW) !=
					0 ||
				!S_ISREG(immediate_identity.st_mode) ||
				authenticated_identity.st_dev != immediate_identity.st_dev ||
				authenticated_identity.st_ino != immediate_identity.st_ino)
				return sqlite_cannot_open;
			if (::unlinkat(parent->get(), leaf.c_str(), 0) != 0 && errno != ENOENT)
				return sqlite_io_error;
			if (synchronize && ::fsync(parent->get()) != 0)
				return sqlite_io_error;
			return sqlite_ok;
		}

		int rooted_close(sqlite3_file* base) noexcept;
		int rooted_read(sqlite3_file* base, void* output, int count, long long offset) noexcept;
		int
		rooted_write(sqlite3_file* base, const void* input, int count, long long offset) noexcept;
		int rooted_truncate(sqlite3_file* base, long long size) noexcept;
		int rooted_sync(sqlite3_file* base, int flags) noexcept;
		int rooted_file_size(sqlite3_file* base, long long* size) noexcept;
		int rooted_lock(sqlite3_file* base, int level) noexcept;
		int rooted_unlock(sqlite3_file* base, int level) noexcept;
		int rooted_reserved(sqlite3_file* base, int* output) noexcept;
		int rooted_control(sqlite3_file* base, int operation, void* value) noexcept;
		int rooted_sector(sqlite3_file* base) noexcept;
		int rooted_characteristics(sqlite3_file* base) noexcept;
		struct opened_object_identities
		{
			sdk::sqlite_backend_opaque_identity object;
			sdk::sqlite_backend_opaque_identity entry;
		};
		[[nodiscard]] sdk::result<opened_object_identities> observe_opened_object(
			const rooted_sqlite_vfs& owner, std::string_view relative, int descriptor);
		int rooted_shm_map(
			sqlite3_file* base, int page, int page_size, int extend, volatile void** out) noexcept;
		int rooted_shm_lock(sqlite3_file* base, int offset, int count, int flags) noexcept;
		void rooted_shm_barrier(sqlite3_file* base) noexcept;
		int rooted_shm_unmap(sqlite3_file* base, int remove_file) noexcept;
		int rooted_fetch(sqlite3_file* base, long long offset, int count, void** output) noexcept;
		int rooted_unfetch(sqlite3_file* base, long long offset, void* value) noexcept;

		const sqlite3_io_methods rooted_io_methods{
			3,
			rooted_close,
			rooted_read,
			rooted_write,
			rooted_truncate,
			rooted_sync,
			rooted_file_size,
			rooted_lock,
			rooted_unlock,
			rooted_reserved,
			rooted_control,
			rooted_sector,
			rooted_characteristics,
			rooted_shm_map,
			rooted_shm_lock,
			rooted_shm_barrier,
			rooted_shm_unmap,
			rooted_fetch,
			rooted_unfetch,
		};

		void release_shm_resources(rooted_sqlite_file& file) noexcept
		{
			for (const auto& mapping : file.mappings)
				if (mapping.address != nullptr)
					(void)::munmap(mapping.address, mapping.size);
			file.mappings.clear();
			if (file.shared_memory_descriptor >= 0)
			{
				(void)::close(file.shared_memory_descriptor);
				file.shared_memory_descriptor = -1;
			}
		}

		int rooted_close(sqlite3_file* base) noexcept
		{
			auto* file = rooted_file(base);
			release_shm_resources(*file);
			auto result =
				file->authenticated_descriptor < 0 || ::close(file->authenticated_descriptor) == 0
				? sqlite_ok
				: sqlite_io_error;
			file->authenticated_descriptor = -1;
			try
			{
				if (file->authenticates_database_path && file->connection_observation)
				{
					std::scoped_lock observation_lock{file->connection_observation->mutex};
					file->connection_observation->main_handle_open = false;
					file->connection_observation->held_shm_locks.clear();
				}
				if (file->authenticates_database_path)
					deauthenticate_database_path(*file->owner, file->relative_path);
			}
			catch (...)
			{
				result = sqlite_io_error;
			}
			file->~rooted_sqlite_file();
			return result;
		}

		int rooted_read(sqlite3_file* base,
						void* output,
						const int count,
						const long long offset) noexcept
		{
			auto* file = rooted_file(base);
			if (output == nullptr || !sqlite_offset_range_valid(offset, count))
				return sqlite_io_error;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			auto* destination = static_cast<std::byte*>(output);
			std::size_t consumed{};
			while (consumed < static_cast<std::size_t>(count))
			{
				const auto read_count =
					::pread(file->authenticated_descriptor,
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
				{
					file->last_errno = errno;
					return sqlite_io_error;
				}
				std::memset(destination + consumed, 0, static_cast<std::size_t>(count) - consumed);
				return sqlite_io_error_short_read;
			}
			return sqlite_ok;
		}

		int rooted_write(sqlite3_file* base,
						 const void* input,
						 const int count,
						 const long long offset) noexcept
		{
			auto* file = rooted_file(base);
			if (input == nullptr || !sqlite_offset_range_valid(offset, count))
				return sqlite_io_error;
			if (!rooted_persistent_effect_permitted(*file))
				return sqlite_readonly;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			const auto* source = static_cast<const std::byte*>(input);
			std::size_t consumed{};
			while (consumed < static_cast<std::size_t>(count))
			{
				const auto write_count =
					::pwrite(file->authenticated_descriptor,
							 source + consumed,
							 static_cast<std::size_t>(count) - consumed,
							 static_cast<off_t>(offset) + static_cast<off_t>(consumed));
				if (write_count > 0)
				{
					consumed += static_cast<std::size_t>(write_count);
					continue;
				}
				if (write_count < 0 && errno == EINTR)
					continue;
				file->last_errno = write_count < 0 ? errno : EIO;
				return sqlite_io_error;
			}
			return sqlite_ok;
		}

		int rooted_truncate(sqlite3_file* base, const long long size) noexcept
		{
			auto* file = rooted_file(base);
			if (size < 0 ||
				static_cast<std::uintmax_t>(size) >
					static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
				return sqlite_io_error;
			if (!rooted_persistent_effect_permitted(*file))
				return sqlite_readonly;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			if (::ftruncate(file->authenticated_descriptor, static_cast<off_t>(size)) != 0)
			{
				file->last_errno = errno;
				return sqlite_io_error;
			}
			return sqlite_ok;
		}

		int rooted_sync(sqlite3_file* base, const int flags) noexcept
		{
			auto* file = rooted_file(base);
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			const auto result = (flags & sqlite_sync_data_only) != 0
				? ::fdatasync(file->authenticated_descriptor)
				: ::fsync(file->authenticated_descriptor);
			if (result != 0)
			{
				file->last_errno = errno;
				return sqlite_io_error;
			}
			return sqlite_ok;
		}

		int rooted_file_size(sqlite3_file* base, long long* size) noexcept
		{
			auto* file = rooted_file(base);
			if (size == nullptr)
				return sqlite_io_error;
			*size = 0;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			struct stat observed{};
			if (::fstat(file->authenticated_descriptor, &observed) != 0 || observed.st_size < 0)
			{
				file->last_errno = errno;
				return sqlite_io_error;
			}
			*size = static_cast<long long>(observed.st_size);
			return sqlite_ok;
		}

		[[nodiscard]] int rooted_set_lock(rooted_sqlite_file& file,
										  const short type,
										  const off_t start,
										  const off_t length) noexcept
		{
			struct flock lock{};
			lock.l_type = type;
			lock.l_whence = SEEK_SET;
			lock.l_start = start;
			lock.l_len = length;
#if defined(F_OFD_SETLK)
			const auto operation = F_OFD_SETLK;
#else
			const auto operation = F_SETLK;
#endif
			if (::fcntl(file.authenticated_descriptor, operation, &lock) == 0)
				return sqlite_ok;
			file.last_errno = errno;
			return errno == EACCES || errno == EAGAIN ? sqlite_busy : sqlite_io_error;
		}

		[[nodiscard]] int apply_rooted_pending_effect_arm(rooted_sqlite_file& file) noexcept
		{
			if (!file.authenticates_database_path || !file.connection_observation ||
				!file.connection_observation->effect_gate ||
				!file.connection_observation->effect_gate->has_pending_exclusive_arm())
				return sqlite_ok;
			try
			{
				auto armed =
					file.connection_observation->effect_gate->apply_pending_exclusive_arm();
				if (armed && *armed)
					return sqlite_ok;
				std::scoped_lock observation_lock{file.connection_observation->mutex};
				file.connection_observation->complete = false;
				return sqlite_io_error;
			}
			catch (...)
			{
				std::scoped_lock observation_lock{file.connection_observation->mutex};
				file.connection_observation->complete = false;
				return sqlite_io_error;
			}
		}

		int rooted_lock(sqlite3_file* base, const int level) noexcept
		{
			auto* file = rooted_file(base);
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			if (level <= file->lock_level)
			{
				return level >= sqlite_lock_exclusive
					? apply_rooted_pending_effect_arm(*file)
					: sqlite_ok;
			}
			if (level < sqlite_lock_shared || level > sqlite_lock_exclusive)
				return sqlite_io_error;

			if (file->lock_level < sqlite_lock_shared)
			{
				if (auto locked =
						rooted_set_lock(*file, static_cast<short>(F_RDLCK), sqlite_pending_byte, 1);
					locked != sqlite_ok)
					return locked;
				const auto shared = rooted_set_lock(
					*file, static_cast<short>(F_RDLCK), sqlite_shared_first, sqlite_shared_size);
				const auto released =
					rooted_set_lock(*file, static_cast<short>(F_UNLCK), sqlite_pending_byte, 1);
				if (shared != sqlite_ok)
					return shared;
				if (released != sqlite_ok)
					return released;
				file->lock_level = sqlite_lock_shared;
			}
			if (level >= sqlite_lock_reserved && file->lock_level < sqlite_lock_reserved)
			{
				const auto locked =
					rooted_set_lock(*file, static_cast<short>(F_WRLCK), sqlite_reserved_byte, 1);
				if (locked != sqlite_ok)
					return locked;
				file->lock_level = sqlite_lock_reserved;
			}
			if (level >= sqlite_lock_pending && file->lock_level < sqlite_lock_pending)
			{
				const auto locked =
					rooted_set_lock(*file, static_cast<short>(F_WRLCK), sqlite_pending_byte, 1);
				if (locked != sqlite_ok)
					return locked;
				file->lock_level = sqlite_lock_pending;
			}
			if (level >= sqlite_lock_exclusive && file->lock_level < sqlite_lock_exclusive)
			{
				const auto locked = rooted_set_lock(
					*file, static_cast<short>(F_WRLCK), sqlite_shared_first, sqlite_shared_size);
				if (locked != sqlite_ok)
					return locked;
				file->lock_level = sqlite_lock_exclusive;
			}
			return level >= sqlite_lock_exclusive ? apply_rooted_pending_effect_arm(*file) : sqlite_ok;
		}

		int rooted_unlock(sqlite3_file* base, const int level) noexcept
		{
			auto* file = rooted_file(base);
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			if (level != sqlite_lock_none && level != sqlite_lock_shared)
				return sqlite_io_error;
			if (level == sqlite_lock_shared)
			{
				if (file->lock_level > sqlite_lock_shared)
				{
					if (auto shared = rooted_set_lock(*file,
													  static_cast<short>(F_RDLCK),
													  sqlite_shared_first,
													  sqlite_shared_size);
						shared != sqlite_ok)
						return shared;
					if (auto released = rooted_set_lock(
							*file, static_cast<short>(F_UNLCK), sqlite_pending_byte, 2);
						released != sqlite_ok)
						return released;
				}
				file->lock_level = sqlite_lock_shared;
				return sqlite_ok;
			}
			const auto released = rooted_set_lock(
				*file, static_cast<short>(F_UNLCK), sqlite_pending_byte, 2 + sqlite_shared_size);
			if (released == sqlite_ok)
				file->lock_level = sqlite_lock_none;
			return released;
		}

		int rooted_reserved(sqlite3_file* base, int* output) noexcept
		{
			auto* file = rooted_file(base);
			if (output == nullptr)
				return sqlite_io_error;
			*output = 0;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			if (file->lock_level >= sqlite_lock_reserved)
			{
				*output = 1;
				return sqlite_ok;
			}
			struct flock lock{};
			lock.l_type = F_WRLCK;
			lock.l_whence = SEEK_SET;
			lock.l_start = sqlite_reserved_byte;
			lock.l_len = 1;
#if defined(F_OFD_GETLK)
			const auto operation = F_OFD_GETLK;
#else
			const auto operation = F_GETLK;
#endif
			if (::fcntl(file->authenticated_descriptor, operation, &lock) != 0)
			{
				file->last_errno = errno;
				return sqlite_io_error;
			}
			*output = lock.l_type == F_UNLCK ? 0 : 1;
			return sqlite_ok;
		}

		int rooted_control(sqlite3_file* base, const int operation, void* value) noexcept
		{
			auto* file = rooted_file(base);
			if (value == nullptr)
				return sqlite_not_found;
			switch (operation)
			{
				case sqlite_file_control_lock_state:
					if (auto authority = acquire_file_effect_authority(*file);
						authority.failure != sqlite_ok)
						return authority.failure;
					*static_cast<int*>(value) = file->lock_level;
					return sqlite_ok;
				case sqlite_file_control_last_errno:
					if (auto authority = acquire_file_effect_authority(*file);
						authority.failure != sqlite_ok)
						return authority.failure;
					*static_cast<int*>(value) = file->last_errno;
					return sqlite_ok;
				case sqlite_file_control_size_hint:
				{
					if (!rooted_persistent_effect_permitted(*file))
						return sqlite_readonly;
					auto authority = acquire_file_effect_authority(*file);
					if (authority.failure != sqlite_ok)
						return authority.failure;
					const auto requested = *static_cast<const long long*>(value);
					long long current{};
					if (requested > 0 && rooted_file_size(base, &current) == sqlite_ok &&
						requested > current)
						return rooted_truncate(base, requested);
					return sqlite_ok;
				}
				case sqlite_file_control_powersafe_overwrite:
				{
					auto authority = acquire_file_effect_authority(*file);
					if (authority.failure != sqlite_ok)
						return authority.failure;
					auto& requested = *static_cast<int*>(value);
					if (requested >= 0)
						file->powersafe_overwrite = requested != 0;
					requested = file->powersafe_overwrite ? 1 : 0;
					return sqlite_ok;
				}
				case sqlite_file_control_has_moved:
				{
					auto& moved = *static_cast<int*>(value);
					moved = 1;
					try
					{
						if (file->authenticates_database_path && !file->relative_path.empty())
						{
							auto authority =
								acquire_named_path_authority(*file->owner,
															 file->relative_path,
															 rooted_sqlite_path_role::database);
							if (!authority)
								return sqlite_ok;
							struct stat retained{};
#if defined(O_PATH)
							constexpr auto identity_open_flags = O_PATH;
#else
							constexpr auto identity_open_flags = O_RDONLY | O_NONBLOCK;
#endif
							auto reopened =
								rooted_open(*file->owner, file->relative_path, identity_open_flags);
							struct stat observed{};
							const auto retained_ok =
								::fstat(file->authenticated_descriptor, &retained) == 0;
							const auto observed_ok =
								reopened && ::fstat(reopened->get(), &observed) == 0;
							if (retained_ok && observed_ok && S_ISREG(observed.st_mode) &&
								retained.st_dev == observed.st_dev &&
								retained.st_ino == observed.st_ino)
								moved = 0;
						}
					}
					catch (...)
					{
						// A failure to reauthenticate the path is conservatively a move.
					}
					return sqlite_ok;
				}
				default:
					return sqlite_not_found;
			}
		}

		int rooted_sector(sqlite3_file*) noexcept
		{
			return 4096;
		}

		int rooted_characteristics(sqlite3_file*) noexcept
		{
			return 0;
		}

		int rooted_shm_map(sqlite3_file* base,
						   const int page,
						   const int page_size,
						   const int extend,
						   volatile void** output) noexcept
		{
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			if (page < 0 || page_size <= 0)
				return sqlite_io_error;
			try
			{
				auto* file = rooted_file(base);
				if (!file->authenticates_database_path || file->relative_path.empty())
					return sqlite_cannot_open;
				const auto coordination_permitted = rooted_persistent_effect_permitted(*file, true);
				if (extend != 0 && !coordination_permitted)
					return sqlite_readonly;
				auto authority = acquire_named_path_authority(
					*file->owner, file->relative_path, rooted_sqlite_path_role::database);
				if (!authority)
					return sqlite_cannot_open;

				const auto page_index = static_cast<std::size_t>(page);
				if (file->mappings.size() <= page_index)
					file->mappings.resize(page_index + 1U);

				if (file->shared_memory_descriptor < 0)
				{
					std::string path;
					path.reserve(file->relative_path.size() + sqlite_shm_suffix.size());
					path.append(file->relative_path);
					path.append(sqlite_shm_suffix);
					const auto create_flags = extend != 0 || coordination_permitted ? O_CREAT : 0;
					auto opened = rooted_open(*file->owner,
											  path,
											  O_RDWR | O_NONBLOCK | create_flags,
											  create_flags != 0 ? 0600U : 0U);
					if (!opened)
					{
						if (extend == 0)
						{
							std::string leaf;
							auto parent = rooted_parent(*file->owner, path, leaf);
							struct stat absent{};
							if (parent &&
								::fstatat(
									parent->get(), leaf.c_str(), &absent, AT_SYMLINK_NOFOLLOW) !=
									0 &&
								errno == ENOENT)
								return sqlite_ok;
						}
						return sqlite_cannot_open;
					}
					if (!rooted_regular_file(opened->get()))
						return sqlite_cannot_open;
					if (file->connection_observation)
					{
						auto identities = observe_opened_object(*file->owner, path, opened->get());
						if (!identities)
							return sqlite_io_error;
						try
						{
							std::scoped_lock observation_lock{file->connection_observation->mutex};
							file->connection_observation->shared_memory_object_identity =
								std::move(identities->object);
							file->connection_observation->shared_memory_entry_identity =
								std::move(identities->entry);
						}
						catch (...)
						{
							std::scoped_lock observation_lock{file->connection_observation->mutex};
							file->connection_observation->complete = false;
							return sqlite_no_memory;
						}
					}
					file->shared_memory_descriptor = opened->release();
				}
				auto& mapping = file->mappings[page_index];
				if (mapping.address == nullptr)
				{
					const auto byte_offset_unsigned =
						static_cast<std::uintmax_t>(page) * static_cast<std::uintmax_t>(page_size);
					const auto required_unsigned =
						byte_offset_unsigned + static_cast<std::uintmax_t>(page_size);
					if (required_unsigned >
						static_cast<std::uintmax_t>(std::numeric_limits<off_t>::max()))
						return sqlite_io_error;
					const auto byte_offset = static_cast<off_t>(byte_offset_unsigned);
					const auto required = static_cast<off_t>(required_unsigned);
					struct stat observed{};
					if (::fstat(file->shared_memory_descriptor, &observed) != 0)
						return sqlite_io_error;
					if (observed.st_size < required)
					{
						if (extend == 0)
							return sqlite_ok;
						if (::ftruncate(file->shared_memory_descriptor, required) != 0)
							return sqlite_io_error;
					}
					auto* address = ::mmap(nullptr,
										   static_cast<std::size_t>(page_size),
										   PROT_READ | PROT_WRITE,
										   MAP_SHARED,
										   file->shared_memory_descriptor,
										   byte_offset);
					if (address == MAP_FAILED)
						return sqlite_io_error;
					mapping = {address, static_cast<std::size_t>(page_size)};
				}
				*output = mapping.address;
				return sqlite_ok;
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int rooted_shm_lock(sqlite3_file* base,
							const int offset,
							const int count,
							const int flags) noexcept
		{
			auto* file = rooted_file(base);
			if (file->shared_memory_descriptor < 0 || offset < 0 || count <= 0)
				return sqlite_io_error;
			auto authority = acquire_file_effect_authority(*file);
			if (authority.failure != sqlite_ok)
				return authority.failure;
			if (file->connection_observation && (flags & sqlite_shm_lock) != 0)
			{
				std::scoped_lock observation_lock{file->connection_observation->mutex};
				const auto already_present =
					std::ranges::any_of(file->connection_observation->held_shm_locks,
										[&](const sdk::sqlite_backend_shm_lock_observation& value)
										{
											return value.offset == offset && value.count == count;
										});
				if (!already_present && file->connection_observation->held_shm_locks.size() >= 64U)
				{
					file->connection_observation->complete = false;
					return sqlite_io_error;
				}
			}
			struct flock lock{};
			lock.l_whence = SEEK_SET;
			lock.l_start = static_cast<off_t>(sqlite_shm_lock_base) + static_cast<off_t>(offset);
			lock.l_len = count;
			if ((flags & sqlite_shm_unlock) != 0)
				lock.l_type = F_UNLCK;
			else if ((flags & sqlite_shm_lock) != 0 && (flags & sqlite_shm_exclusive) != 0)
				lock.l_type = F_WRLCK;
			else if ((flags & sqlite_shm_lock) != 0 && (flags & sqlite_shm_shared) != 0)
				lock.l_type = F_RDLCK;
			else
				return sqlite_io_error;
#if defined(F_OFD_SETLK)
			constexpr auto operation = F_OFD_SETLK;
#else
			constexpr auto operation = F_SETLK;
#endif
			if (::fcntl(file->shared_memory_descriptor, operation, &lock) != 0)
				return sqlite_busy;
			if (file->connection_observation)
			{
				try
				{
					std::scoped_lock observation_lock{file->connection_observation->mutex};
					auto& held = file->connection_observation->held_shm_locks;
					const auto overlaps = [&](const sdk::sqlite_backend_shm_lock_observation& value)
					{
						const auto value_end = static_cast<long long>(value.offset) + value.count;
						const auto request_end = static_cast<long long>(offset) + count;
						return value.offset < request_end && offset < value_end;
					};
					if ((flags & sqlite_shm_unlock) != 0)
						held.erase(std::remove_if(held.begin(), held.end(), overlaps), held.end());
					else
					{
						held.erase(std::remove_if(held.begin(), held.end(), overlaps), held.end());
						held.push_back({offset,
										count,
										(flags & sqlite_shm_exclusive) != 0
											? sdk::sqlite_backend_shm_lock_mode::exclusive
											: sdk::sqlite_backend_shm_lock_mode::shared});
					}
				}
				catch (...)
				{
					std::scoped_lock observation_lock{file->connection_observation->mutex};
					file->connection_observation->complete = false;
					return sqlite_io_error;
				}
			}
			return sqlite_ok;
		}

		void rooted_shm_barrier(sqlite3_file*) noexcept
		{
			std::atomic_thread_fence(std::memory_order_seq_cst);
		}

		int rooted_shm_unmap(sqlite3_file* base, const int remove_file) noexcept
		{
			auto* file = rooted_file(base);
			if (!file->authenticates_database_path || file->relative_path.empty())
				return sqlite_cannot_open;
			if (remove_file == 0)
			{
				release_shm_resources(*file);
				return sqlite_ok;
			}
			if (!rooted_persistent_effect_permitted(*file))
				return sqlite_readonly;
			try
			{
				auto authority = acquire_named_path_authority(
					*file->owner, file->relative_path, rooted_sqlite_path_role::database);
				if (!authority)
					return sqlite_cannot_open;
				std::string remove_path;
				remove_path.reserve(file->relative_path.size() + sqlite_shm_suffix.size());
				remove_path.append(file->relative_path);
				remove_path.append(sqlite_shm_suffix);
				release_shm_resources(*file);
				return rooted_remove(*file->owner, remove_path, false);
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int rooted_fetch(sqlite3_file* base, const long long, const int, void** output) noexcept
		{
			(void)base;
			if (output == nullptr)
				return sqlite_io_error;
			*output = nullptr;
			return sqlite_ok;
		}

		int rooted_unfetch(sqlite3_file*, const long long, void*) noexcept
		{
			return sqlite_ok;
		}

		[[nodiscard]] std::shared_ptr<rooted_connection_observation>
		claim_main_connection_observation(rooted_sqlite_vfs& owner,
										  const std::string_view relative) noexcept
		{
			std::scoped_lock owner_lock{owner.connection_observations_mutex};
			for (auto iterator = owner.connection_observations.begin();
				 iterator != owner.connection_observations.end();)
			{
				auto observation = iterator->lock();
				if (!observation)
				{
					iterator = owner.connection_observations.erase(iterator);
					continue;
				}
				std::scoped_lock observation_lock{observation->mutex};
				if (!observation->main_claimed && observation->relative_path == relative &&
					observation->originating_thread == std::this_thread::get_id())
				{
					observation->main_claimed = true;
					return observation;
				}
				++iterator;
			}
			return {};
		}

		[[nodiscard]] std::shared_ptr<rooted_connection_observation>
		active_connection_observation(rooted_sqlite_vfs& owner,
									  const std::string_view database_relative) noexcept
		{
			std::vector<std::shared_ptr<rooted_connection_observation>> candidates;
			std::vector<std::shared_ptr<rooted_connection_observation>> same_thread;
			try
			{
				std::scoped_lock owner_lock{owner.connection_observations_mutex};
				for (auto iterator = owner.connection_observations.begin();
					 iterator != owner.connection_observations.end();)
				{
					auto observation = iterator->lock();
					if (!observation)
					{
						iterator = owner.connection_observations.erase(iterator);
						continue;
					}
					{
						std::scoped_lock observation_lock{observation->mutex};
						if (observation->main_handle_open &&
							observation->relative_path == database_relative)
						{
							candidates.push_back(observation);
							if (observation->originating_thread == std::this_thread::get_id())
								same_thread.push_back(observation);
						}
					}
					++iterator;
				}
			}
			catch (...)
			{
				for (const auto& candidate : candidates)
				{
					std::scoped_lock lock{candidate->mutex};
					candidate->complete = false;
				}
				return {};
			}
			if (same_thread.size() == 1U)
				return same_thread.front();
			if (same_thread.empty() && candidates.size() == 1U)
				return candidates.front();
			for (const auto& candidate : candidates)
			{
				std::scoped_lock lock{candidate->mutex};
				candidate->complete = false;
			}
			return {};
		}

		[[nodiscard]] std::optional<std::size_t>
		record_open_attempt(const std::shared_ptr<rooted_connection_observation>& observation,
							const sdk::sqlite_backend_file_role role,
							const int flags) noexcept
		{
			if (!observation)
				return std::nullopt;
			try
			{
				std::scoped_lock lock{observation->mutex};
				if (observation->open_events.size() >= 8U)
				{
					observation->complete = false;
					return std::nullopt;
				}
				const auto index = observation->open_events.size();
				observation->open_events.push_back(
					{role, flags, sdk::sqlite_backend_open_outcome::attempted, {}, {}, {}});
				return index;
			}
			catch (...)
			{
				std::scoped_lock lock{observation->mutex};
				observation->complete = false;
				return std::nullopt;
			}
		}

		void record_open_failure(const std::shared_ptr<rooted_connection_observation>& observation,
								 const std::optional<std::size_t> index) noexcept
		{
			if (!observation || !index)
				return;
			std::scoped_lock lock{observation->mutex};
			if (*index >= observation->open_events.size())
			{
				observation->complete = false;
				return;
			}
			observation->open_events[*index].outcome = sdk::sqlite_backend_open_outcome::failed;
		}

		[[nodiscard]] sdk::result<opened_object_identities> observe_opened_object(
			const rooted_sqlite_vfs& owner, const std::string_view relative, const int descriptor)
		{
			std::string leaf;
			auto parent = rooted_parent(owner, relative, leaf);
			if (!parent)
				return sdk::unexpected(observation_error("observation-io-failure"));
			struct stat held{};
			struct stat entry{};
			if (::fstat(descriptor, &held) != 0 || !S_ISREG(held.st_mode) ||
				::fstatat(parent->get(), leaf.c_str(), &entry, AT_SYMLINK_NOFOLLOW) != 0 ||
				!S_ISREG(entry.st_mode) || held.st_dev != entry.st_dev ||
				held.st_ino != entry.st_ino)
				return sdk::unexpected(observation_error("observation-io-failure"));
			auto object = opaque_fd_identity(descriptor, "rooted-vfs-v1.open-object.v1");
			auto directory_entry = opaque_entry_identity(parent->get(), leaf, entry);
			if (!object || !directory_entry)
				return sdk::unexpected(observation_error("observation-io-failure"));
			return opened_object_identities{std::move(*object), std::move(*directory_entry)};
		}

		[[nodiscard]] bool
		record_open_success(const std::shared_ptr<rooted_connection_observation>& observation,
							const std::optional<std::size_t> index,
							const int returned_flags,
							opened_object_identities identities) noexcept
		{
			if (!observation)
				return true;
			if (!index)
				return false;
			try
			{
				std::scoped_lock lock{observation->mutex};
				if (*index >= observation->open_events.size())
				{
					observation->complete = false;
					return false;
				}
				auto& event = observation->open_events[*index];
				event.outcome = sdk::sqlite_backend_open_outcome::succeeded;
				event.returned_flags = returned_flags;
				event.object_identity = std::move(identities.object);
				event.directory_entry_identity = std::move(identities.entry);
				return true;
			}
			catch (...)
			{
				std::scoped_lock lock{observation->mutex};
				observation->complete = false;
				return false;
			}
		}

		int rooted_vfs_open(sqlite3_vfs* vfs,
							const char* name,
							sqlite3_file* output,
							const int flags,
							int* output_flags) noexcept
		{
			if (output_flags != nullptr)
				*output_flags = 0;
			if (output == nullptr)
				return sqlite_cannot_open;
			output->methods = nullptr;
			if (vfs == nullptr || vfs->app_data == nullptr)
				return sqlite_cannot_open;
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			std::string relative;
			std::string authority_database_path;
			std::shared_ptr<rooted_connection_observation> connection_observation;
			std::optional<std::size_t> open_observation_index;
			bool database_path_reserved{};
			int delegated_sqlite_flags = flags;
			try
			{
				materialization_owned_fd authenticated;
				std::optional<database_path_authority_lease> authority;
				if (name == nullptr)
				{
					if (!rooted_anonymous_open_allowed(flags))
						return sqlite_cannot_open;
#if defined(__linux__) && defined(SYS_memfd_create)
					const auto descriptor = static_cast<int>(
						::syscall(SYS_memfd_create, "cxxlens-sqlite-temp", MFD_CLOEXEC));
					if (descriptor < 0)
						return sqlite_cannot_open;
					authenticated = materialization_owned_fd{descriptor};
#else
					return sqlite_cannot_open;
#endif
				}
				else
				{
					if ((flags & sqlite_open_delete_on_close) != 0)
						return sqlite_cannot_open;
					const auto role = rooted_open_path_role(flags);
					if (!role)
						return sqlite_cannot_open;
					auto parsed = rooted_relative_name(name, *role);
					if (!parsed)
					{
						return sqlite_cannot_open;
					}
					relative = std::move(*parsed);
					if (*role == rooted_sqlite_path_role::database)
					{
						authority_database_path = relative;
						connection_observation =
							claim_main_connection_observation(*owner, relative);
						if (!reserve_database_path(*owner, relative))
							return sqlite_no_memory;
						database_path_reserved = true;
					}
					else
					{
						const auto suffix = *role == rooted_sqlite_path_role::journal
							? sqlite_journal_suffix
							: sqlite_wal_suffix;
						authority_database_path.assign(relative.data(),
													   relative.size() - suffix.size());
						connection_observation =
							active_connection_observation(*owner, authority_database_path);
						auto acquired =
							acquire_database_path_authority(*owner, authority_database_path);
						if (!acquired)
							return sqlite_cannot_open;
						authority.emplace(std::move(*acquired));
					}
					const auto observed_role = *role == rooted_sqlite_path_role::database
						? sdk::sqlite_backend_file_role::main_database
						: *role == rooted_sqlite_path_role::journal
						? sdk::sqlite_backend_file_role::rollback_journal
						: sdk::sqlite_backend_file_role::write_ahead_log;
					open_observation_index =
						record_open_attempt(connection_observation, observed_role, flags);
					if (connection_observation && !open_observation_index)
					{
						if (database_path_reserved)
						{
							cancel_database_path_reservation(*owner, relative);
							database_path_reserved = false;
						}
						return sqlite_no_memory;
					}
					if ((flags & sqlite_open_create) != 0)
					{
						if (!connection_observation || !connection_observation->effect_gate)
						{
							record_open_failure(connection_observation, open_observation_index);
							if (database_path_reserved)
							{
								cancel_database_path_reservation(*owner, relative);
								database_path_reserved = false;
							}
							return sqlite_readonly;
						}
						if (!connection_observation->effect_gate->permits_persistent_effect(false))
						{
							const auto coordination_wal_create =
								*role == rooted_sqlite_path_role::write_ahead_log &&
								flags ==
									(sqlite_open_read_write | sqlite_open_create |
									 sqlite_open_write_ahead_log) &&
								connection_observation->effect_gate->stage() ==
									sdk::sqlite_backend_effect_stage::wal_shm_coordination_only &&
								connection_observation->effect_gate->permits_persistent_effect(
									true);
							if (!coordination_wal_create &&
								*role != rooted_sqlite_path_role::database)
							{
								record_open_failure(connection_observation, open_observation_index);
								return sqlite_readonly;
							}
							// The bootstrap owns creation. A denied main xOpen may authenticate the
							// existing object but cannot recreate a concurrently removed entry.
							if (!coordination_wal_create)
								delegated_sqlite_flags &= ~sqlite_open_create;
						}
					}
					const auto creation_mode = (delegated_sqlite_flags & sqlite_open_create) != 0
						? std::uint32_t{0600U}
						: std::uint32_t{};
					auto opened = rooted_open(*owner,
											  relative,
											  sqlite_flags_to_open(delegated_sqlite_flags),
											  creation_mode);
					if (!opened || !rooted_regular_file(opened->get()))
					{
						record_open_failure(connection_observation, open_observation_index);
						if (database_path_reserved)
						{
							cancel_database_path_reservation(*owner, relative);
							database_path_reserved = false;
						}
						return sqlite_cannot_open;
					}
					if (connection_observation)
					{
						auto identities = observe_opened_object(*owner, relative, opened->get());
						if (!identities ||
							!record_open_success(connection_observation,
												 open_observation_index,
												 flags,
												 identities ? std::move(*identities)
															: opened_object_identities{}))
						{
							record_open_failure(connection_observation, open_observation_index);
							if (database_path_reserved)
							{
								cancel_database_path_reservation(*owner, relative);
								database_path_reserved = false;
							}
							return sqlite_io_error;
						}
					}
					authenticated = std::move(*opened);
				}

				static_assert(std::is_nothrow_default_constructible_v<rooted_sqlite_file>);
				static_assert(std::is_nothrow_move_assignable_v<std::string>);
				auto* file = new (output) rooted_sqlite_file{};
				file->relative_path = std::move(relative);
				file->authority_database_path = std::move(authority_database_path);
				file->connection_observation = std::move(connection_observation);
				if (database_path_reserved)
				{
					commit_database_path_reservation(*owner, file->relative_path);
					database_path_reserved = false;
				}
				file->owner = owner;
				file->authenticated_descriptor = authenticated.release();
				file->authenticates_database_path =
					(flags & sqlite_open_file_type_mask) == sqlite_open_main_database;
				if (file->authenticates_database_path && file->connection_observation)
				{
					std::scoped_lock observation_lock{file->connection_observation->mutex};
					file->connection_observation->main_handle_open = true;
				}
				if (output_flags != nullptr)
					*output_flags = flags;
				file->base.methods = &rooted_io_methods;
				return sqlite_ok;
			}
			catch (const std::bad_alloc&)
			{
				if (database_path_reserved)
					cancel_database_path_reservation(*owner, relative);
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				if (database_path_reserved)
					cancel_database_path_reservation(*owner, relative);
				return sqlite_no_memory;
			}
			catch (...)
			{
				if (database_path_reserved)
					cancel_database_path_reservation(*owner, relative);
				return sqlite_cannot_open;
			}
		}

		int rooted_vfs_remove(sqlite3_vfs* vfs, const char* name, const int synchronize) noexcept
		{
			try
			{
				if (vfs == nullptr || vfs->app_data == nullptr)
					return sqlite_cannot_open;
				auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
				auto relative =
					rooted_relative_name(name, rooted_sqlite_path_role::known_sidecar_or_database);
				if (!relative)
					return sqlite_cannot_open;
				auto authority = acquire_named_path_authority(
					*owner, *relative, rooted_sqlite_path_role::known_sidecar_or_database);
				if (!authority)
					return sqlite_cannot_open;
				auto observation =
					active_connection_observation(*owner, authority->database_path());
				if (!observation || !observation->effect_gate ||
					!observation->effect_gate->permits_persistent_effect(false))
					return sqlite_readonly;
				return rooted_remove(*owner, *relative, synchronize != 0);
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int
		rooted_vfs_access(sqlite3_vfs* vfs, const char* name, const int flags, int* output) noexcept
		{
			if (output == nullptr)
				return sqlite_error;
			*output = 0;
			try
			{
				if (vfs == nullptr || vfs->app_data == nullptr)
					return sqlite_ok;
				auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
				auto relative =
					rooted_relative_name(name, rooted_sqlite_path_role::known_sidecar_or_database);
				if (!relative)
					return sqlite_ok;
				auto authority = acquire_named_path_authority(
					*owner, *relative, rooted_sqlite_path_role::known_sidecar_or_database);
				if (!authority)
					return sqlite_ok;
#if defined(O_PATH)
				const auto open_flags = flags == sqlite_access_exists
					? O_PATH
					: (flags == sqlite_access_read_write ? O_RDWR : O_RDONLY) | O_NONBLOCK;
#else
				const auto open_flags =
					(flags == sqlite_access_read_write ? O_RDWR : O_RDONLY) | O_NONBLOCK;
#endif
				auto opened = rooted_open(*owner, *relative, open_flags);
				*output = opened && rooted_regular_file(opened->get()) ? 1 : 0;
				return sqlite_ok;
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_io_error;
			}
		}

		int rooted_vfs_full_path(sqlite3_vfs* vfs,
								 const char* name,
								 const int output_size,
								 char* output) noexcept
		{
			if (output == nullptr || output_size <= 0)
				return sqlite_cannot_open;
			output[0] = '\0';
			try
			{
				if (vfs == nullptr || vfs->app_data == nullptr)
					return sqlite_cannot_open;
				auto relative = rooted_relative_name(name, rooted_sqlite_path_role::database);
				if (!relative ||
					rooted_name_prefix.size() + relative->size() + 1U >
						static_cast<std::size_t>(output_size))
					return sqlite_cannot_open;
				std::memcpy(output, rooted_name_prefix.data(), rooted_name_prefix.size());
				std::memcpy(output + rooted_name_prefix.size(), relative->data(), relative->size());
				output[rooted_name_prefix.size() + relative->size()] = '\0';
				return sqlite_ok;
			}
			catch (const std::bad_alloc&)
			{
				return sqlite_no_memory;
			}
			catch (const std::length_error&)
			{
				return sqlite_no_memory;
			}
			catch (...)
			{
				return sqlite_cannot_open;
			}
		}

		void* rooted_dl_open(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		void rooted_dl_error(sqlite3_vfs*, const int size, char* output) noexcept
		{
			if (output != nullptr && size > 0)
				output[0] = '\0';
		}

		void (*rooted_dl_sym(sqlite3_vfs*, void*, const char*) noexcept)(void)
		{
			return nullptr;
		}

		void rooted_dl_close(sqlite3_vfs*, void*) noexcept {}

		int rooted_randomness(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			return owner->delegate->randomness(owner->delegate, size, output);
		}

		int rooted_sleep(sqlite3_vfs* vfs, const int microseconds) noexcept
		{
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			return owner->delegate->sleep(owner->delegate, microseconds);
		}

		int rooted_current_time(sqlite3_vfs* vfs, double* output) noexcept
		{
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			return owner->delegate->current_time(owner->delegate, output);
		}

		int rooted_last_error(sqlite3_vfs* vfs, const int size, char* output) noexcept
		{
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			return owner->delegate->get_last_error != nullptr
				? owner->delegate->get_last_error(owner->delegate, size, output)
				: sqlite_not_found;
		}

		int rooted_current_time_int64(sqlite3_vfs* vfs, long long* output) noexcept
		{
			auto* owner = static_cast<rooted_sqlite_vfs*>(vfs->app_data);
			return owner->delegate->version >= 2 && owner->delegate->current_time_int64 != nullptr
				? owner->delegate->current_time_int64(owner->delegate, output)
				: sqlite_not_found;
		}

		int rooted_set_system_call(sqlite3_vfs*, const char*, sqlite3_syscall_ptr) noexcept
		{
			return sqlite_not_found;
		}

		sqlite3_syscall_ptr rooted_get_system_call(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		const char* rooted_next_system_call(sqlite3_vfs*, const char*) noexcept
		{
			return nullptr;
		}

		template <class Function>
		[[nodiscard]] bool sqlite_symbol(void* library, const char* name, Function& output)
		{
			const auto symbol = ::dlsym(library, name);
			if (symbol == nullptr)
				return false;
			output = reinterpret_cast<Function>(symbol);
			return true;
		}

		[[nodiscard]] sdk::result<std::shared_ptr<rooted_sqlite_vfs>>
		make_rooted_sqlite_vfs(const materialization_effect_root& effect_root)
		{
			auto duplicate = effect_root.duplicate_directory();
			if (!duplicate)
				return sdk::unexpected(std::move(duplicate.error()));
			std::shared_ptr<rooted_sqlite_vfs> output;
			try
			{
				output = std::make_shared<rooted_sqlite_vfs>();
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					sdk::error{"materialization.resource-limit", "rooted-vfs", "allocation"});
			}
			output->root = std::move(*duplicate);
			output->root_identity = effect_root.identity();
			output->root_digest = effect_root.observation_digest();
#if defined(__APPLE__)
			constexpr std::array candidates{"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
#else
			constexpr std::array candidates{"libsqlite3.so.0", "libsqlite3.so"};
#endif
			for (const auto* candidate : candidates)
			{
				output->sqlite_library.value = ::dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
				if (output->sqlite_library.get() != nullptr)
					break;
			}
			if (output->sqlite_library.get() == nullptr ||
				!sqlite_symbol(
					output->sqlite_library.get(), "sqlite3_vfs_find", output->api.find) ||
				!sqlite_symbol(output->sqlite_library.get(),
							   "sqlite3_vfs_register",
							   output->api.register_vfs) ||
				!sqlite_symbol(output->sqlite_library.get(),
							   "sqlite3_vfs_unregister",
							   output->api.unregister_vfs))
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "rooted-vfs-symbols"});
			output->delegate = output->api.find(nullptr);
			if (output->delegate == nullptr || output->delegate->version < 3 ||
				output->delegate->os_file_bytes <= 0 || output->delegate->open == nullptr)
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "rooted-vfs-delegate"});
			const auto total_file_bytes = sizeof(rooted_sqlite_file);
			if (total_file_bytes > static_cast<std::size_t>(std::numeric_limits<int>::max()))
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "rooted-vfs-file-size"});
			output->wrapper = sqlite3_vfs{
				3,
				static_cast<int>(total_file_bytes),
				8192,
				nullptr,
				output->wrapper_name.c_str(),
				output.get(),
				rooted_vfs_open,
				rooted_vfs_remove,
				rooted_vfs_access,
				rooted_vfs_full_path,
				rooted_dl_open,
				rooted_dl_error,
				rooted_dl_sym,
				rooted_dl_close,
				rooted_randomness,
				rooted_sleep,
				rooted_current_time,
				rooted_last_error,
				rooted_current_time_int64,
				rooted_set_system_call,
				rooted_get_system_call,
				rooted_next_system_call,
			};
			try
			{
				output->capability_token_value =
					make_opaque_identity("rooted-vfs-v1.observation-capability.v1",
										 output->root_identity,
										 output->wrapper_name);
				append_identity_u64(
					output->capability_token_value.bytes,
					static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(&output->wrapper)));
				append_identity_u64(
					output->capability_token_value.bytes,
					static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(output->delegate)));
				append_identity_u64(output->capability_token_value.bytes,
									static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(
										output->sqlite_library.get())));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(
					sdk::error{"store.backend-unavailable", "sqlite", "vfs-observation"});
			}
			{
				std::scoped_lock lock{rooted_vfs_registration_mutex};
				if (rooted_vfs_registration_active ||
					output->api.find(output->wrapper_name.c_str()) != nullptr ||
					output->api.register_vfs(&output->wrapper, 0) != sqlite_ok)
					return sdk::unexpected(
						sdk::error{"store.backend-unavailable", "sqlite", "rooted-vfs-register"});
				output->registered = true;
				rooted_vfs_registration_active = true;
				if (output->api.find(output->wrapper_name.c_str()) != &output->wrapper)
					std::terminate();
			}
			return output;
		}

		[[nodiscard]] sdk::error rooted_error(std::string field, std::string detail)
		{
			return {"materialization.identity-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<materialization_owned_fd>
		openat2_exact(const int directory,
					  const std::string_view path,
					  const int flags,
					  const std::uint32_t creation_mode,
					  const std::uint64_t resolution)
		{
#if defined(__linux__) && defined(SYS_openat2)
			if (path.empty() || path.contains('\0'))
				return sdk::unexpected(rooted_error("path", "empty-or-nul"));
			std::string terminated{path};
			open_how how{};
			how.flags = static_cast<std::uint64_t>(flags | O_CLOEXEC);
			how.mode = creation_mode;
			how.resolve = resolution;
			for (;;)
			{
				const auto descriptor = static_cast<int>(
					::syscall(SYS_openat2, directory, terminated.c_str(), &how, sizeof(how)));
				if (descriptor >= 0)
					return materialization_owned_fd{descriptor};
				if (errno == EINTR)
					continue;
				return sdk::unexpected(rooted_error(
					"openat2", errno == ENOSYS ? "unsupported-kernel" : std::to_string(errno)));
			}
#else
			(void)directory;
			(void)path;
			(void)flags;
			(void)creation_mode;
			(void)resolution;
			return sdk::unexpected(rooted_error("openat2", "unsupported-platform"));
#endif
		}

		[[nodiscard]] std::string
		root_observation_digest(const materialization_file_identity& identity)
		{
			const auto projection = std::string{"rooted-vfs-v1\0", 14U} +
				std::to_string(identity.device) + "\0" + std::to_string(identity.inode) + "\0" +
				std::to_string(identity.mode);
			return sdk::content_digest(std::as_bytes(std::span{projection}));
		}
	} // namespace

	materialization_owned_fd::materialization_owned_fd(const int descriptor) noexcept
		: descriptor_{descriptor}
	{
	}

	materialization_owned_fd::materialization_owned_fd(materialization_owned_fd&& other) noexcept
		: descriptor_{other.release()}
	{
	}

	materialization_owned_fd&
	materialization_owned_fd::operator=(materialization_owned_fd&& other) noexcept
	{
		if (this != &other)
		{
			if (descriptor_ >= 0)
				(void)::close(descriptor_);
			descriptor_ = other.release();
		}
		return *this;
	}

	materialization_owned_fd::~materialization_owned_fd()
	{
		if (descriptor_ >= 0)
			(void)::close(descriptor_);
	}

	int materialization_owned_fd::get() const noexcept
	{
		return descriptor_;
	}

	materialization_owned_fd::operator bool() const noexcept
	{
		return descriptor_ >= 0;
	}

	int materialization_owned_fd::release() noexcept
	{
		const auto output = descriptor_;
		descriptor_ = -1;
		return output;
	}

	sdk::result<materialization_file_identity>
	materialization_fd_identity(const int descriptor, const bool require_regular)
	{
		struct stat observed{};
		if (descriptor < 0 || ::fstat(descriptor, &observed) != 0)
			return sdk::unexpected(rooted_error("fstat", std::to_string(errno)));
		if (require_regular && !S_ISREG(observed.st_mode))
			return sdk::unexpected(rooted_error("fstat", "not-regular"));
		if (observed.st_size < 0)
			return sdk::unexpected(rooted_error("fstat", "negative-size"));
		return materialization_file_identity{
			static_cast<std::uint64_t>(observed.st_dev),
			static_cast<std::uint64_t>(observed.st_ino),
			static_cast<std::uint64_t>(observed.st_size),
			static_cast<std::uint64_t>(observed.st_mode),
			static_cast<std::int64_t>(observed.st_mtim.tv_sec),
			static_cast<std::int64_t>(observed.st_mtim.tv_nsec),
			static_cast<std::int64_t>(observed.st_ctim.tv_sec),
			static_cast<std::int64_t>(observed.st_ctim.tv_nsec),
		};
	}

	sdk::result<void> validate_materialization_relative_path(const std::string_view value,
															 const std::size_t maximum_utf8_bytes,
															 const bool require_nfc)
	{
		if (value.empty() || value.size() > maximum_utf8_bytes || value.front() == '/' ||
			value.contains('\0') || value.contains('\\') ||
			(value.size() >= 2U &&
			 ((value[0U] >= 'A' && value[0U] <= 'Z') || (value[0U] >= 'a' && value[0U] <= 'z')) &&
			 value[1U] == ':') ||
			!sdk::validate_utf8_text(value))
			return sdk::unexpected(rooted_error("path", "lexical"));
		std::size_t begin{};
		while (begin <= value.size())
		{
			const auto end = value.find('/', begin);
			const auto component = value.substr(
				begin, end == std::string_view::npos ? value.size() - begin : end - begin);
			if (component.empty() || component == "." || component == "..")
				return sdk::unexpected(rooted_error("path", "component"));
			if (end == std::string_view::npos)
				break;
			begin = end + 1U;
		}
		if (require_nfc)
		{
			auto normalized = is_nfc_utf8(value);
			if (!normalized)
				return sdk::unexpected(std::move(normalized.error()));
			if (!*normalized)
				return sdk::unexpected(rooted_error("path", "normalization-change"));
		}
		return {};
	}

	sdk::result<void> validate_materialization_sqlite_path(const std::string_view value)
	{
		if (!value.empty() && (value.front() == ':' || value.starts_with("file:")))
			return sdk::unexpected(rooted_error("sqlite-path", "reserved-name"));
		return validate_materialization_relative_path(
			value, maximum_sqlite_request_path_bytes, true);
	}

	sdk::result<materialization_owned_fd>
	open_materialization_beneath(const int directory_descriptor,
								 const std::string_view relative_path,
								 const int flags,
								 const std::uint32_t creation_mode)
	{
		if (directory_descriptor < 0)
			return sdk::unexpected(rooted_error("root-directory", "invalid-descriptor"));
		if (auto valid = validate_materialization_relative_path(relative_path, 4095U, true); !valid)
			return sdk::unexpected(std::move(valid.error()));
#if defined(__linux__)
		constexpr auto resolution = static_cast<std::uint64_t>(
			RESOLVE_BENEATH | RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
		return openat2_exact(directory_descriptor, relative_path, flags, creation_mode, resolution);
#else
		(void)flags;
		(void)creation_mode;
		return sdk::unexpected(rooted_error("openat2", "unsupported-platform"));
#endif
	}

	materialization_effect_root::materialization_effect_root(materialization_owned_fd directory,
															 materialization_file_identity identity,
															 std::string observation_digest)
		: directory_{std::move(directory)}, identity_{identity},
		  observation_digest_{std::move(observation_digest)}
	{
	}

	sdk::result<materialization_effect_root> materialization_effect_root::capture_startup()
	{
		const auto descriptor = ::open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
		if (descriptor < 0)
			return sdk::unexpected(rooted_error("effect-root", std::to_string(errno)));
		materialization_owned_fd directory{descriptor};
		auto identity = materialization_fd_identity(directory.get(), false);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		if ((identity->mode & static_cast<std::uint64_t>(S_IFMT)) !=
			static_cast<std::uint64_t>(S_IFDIR))
			return sdk::unexpected(rooted_error("effect-root", "not-directory"));
		return materialization_effect_root{
			std::move(directory), *identity, root_observation_digest(*identity)};
	}

	sdk::result<materialization_owned_fd>
	materialization_effect_root::open_beneath(const std::string_view relative_path,
											  const int flags,
											  const std::uint32_t creation_mode) const
	{
		return open_materialization_beneath(directory_.get(), relative_path, flags, creation_mode);
	}

	sdk::result<materialization_owned_fd> materialization_effect_root::duplicate_directory() const
	{
		const auto descriptor = ::fcntl(directory_.get(), F_DUPFD_CLOEXEC, 0);
		if (descriptor < 0)
			return sdk::unexpected(rooted_error("effect-root", std::to_string(errno)));
		return materialization_owned_fd{descriptor};
	}

	const materialization_file_identity& materialization_effect_root::identity() const noexcept
	{
		return identity_;
	}

	const std::string& materialization_effect_root::observation_digest() const noexcept
	{
		return observation_digest_;
	}

	sdk::result<materialization_owned_fd>
	open_materialization_absolute_no_symlinks(const std::string_view absolute_path, const int flags)
	{
		if (absolute_path.empty() || absolute_path.front() != '/' ||
			!sdk::validate_utf8_text(absolute_path) || absolute_path.contains('\0'))
			return sdk::unexpected(rooted_error("absolute-path", "lexical"));
#if defined(__linux__)
		constexpr auto resolution =
			static_cast<std::uint64_t>(RESOLVE_NO_SYMLINKS | RESOLVE_NO_MAGICLINKS);
		return openat2_exact(AT_FDCWD, absolute_path, flags, 0U, resolution);
#else
		(void)flags;
		return sdk::unexpected(rooted_error("openat2", "unsupported-platform"));
#endif
	}

	struct materialization_rooted_store_opener::state
	{
		std::shared_ptr<rooted_sqlite_vfs> vfs;
		std::optional<materialization_rooted_vfs_receipt> receipt;
		std::optional<std::string> exact_sqlite_path;
	};

	materialization_rooted_store_opener::materialization_rooted_store_opener(
		std::unique_ptr<state> state_value)
		: state_{std::move(state_value)}
	{
	}

	materialization_rooted_store_opener::~materialization_rooted_store_opener() = default;

	sdk::result<std::unique_ptr<materialization_rooted_store_opener>>
	materialization_rooted_store_opener::create(const materialization_effect_root& root)
	{
		auto vfs = make_rooted_sqlite_vfs(root);
		if (!vfs)
			return sdk::unexpected(std::move(vfs.error()));
		try
		{
			auto state_value = std::make_unique<state>();
			state_value->vfs = std::move(*vfs);
			return std::unique_ptr<materialization_rooted_store_opener>{
				new materialization_rooted_store_opener{std::move(state_value)}};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.resource-limit", "rooted-vfs", "allocation"});
		}
	}

	sdk::result<sdk::snapshot_store>
	materialization_rooted_store_opener::open_memory(sdk::relation_engine engine)
	{
		return sdk::make_in_memory_snapshot_store(std::move(engine));
	}

	sdk::result<sdk::snapshot_store>
	materialization_rooted_store_opener::open_sqlite(const std::string& exact_path,
													 sdk::relation_engine engine)
	{
		if (auto valid = validate_materialization_sqlite_path(exact_path); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (state_->exact_sqlite_path && *state_->exact_sqlite_path != exact_path)
			return sdk::unexpected(sdk::error{"materialization.identity-mismatch",
											  "sqlite-path",
											  "changed-between-open-and-reopen"});

		std::string rooted_sqlite_path;
		std::optional<std::string> prepared_exact_path;
		std::optional<materialization_rooted_vfs_receipt> prepared_receipt;
		try
		{
			rooted_sqlite_path.reserve(rooted_name_prefix.size() + exact_path.size());
			rooted_sqlite_path.append(rooted_name_prefix);
			rooted_sqlite_path.append(exact_path);
			prepared_exact_path.emplace(exact_path);
			prepared_receipt.emplace(materialization_rooted_vfs_receipt{
				"rooted-vfs-v1", state_->vfs->root_digest, exact_path});
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.resource-limit", "rooted-vfs", "open-allocation"});
		}
		catch (const std::length_error&)
		{
			return sdk::unexpected(
				sdk::error{"materialization.resource-limit", "rooted-vfs", "open-allocation"});
		}

		auto opened = sdk::snapshot_store_backend_lifetime_access::open_sqlite(
			rooted_sqlite_path,
			std::move(engine),
			state_->vfs->wrapper_name,
			sdk::sqlite_backend_runtime_binding{
				state_->vfs->sqlite_library.get(), state_->vfs->sqlite_library.get(), state_->vfs},
			state_->vfs,
			state_->vfs);
		if (!opened)
			return sdk::unexpected(std::move(opened.error()));
		static_assert(std::is_nothrow_move_constructible_v<std::string>);
		static_assert(std::is_nothrow_move_assignable_v<std::string>);
		static_assert(std::is_nothrow_move_constructible_v<materialization_rooted_vfs_receipt>);
		static_assert(std::is_nothrow_move_assignable_v<materialization_rooted_vfs_receipt>);
		static_assert(
			noexcept(std::declval<std::optional<std::string>&>() = std::declval<std::string&&>()));
		static_assert(noexcept(std::declval<std::optional<materialization_rooted_vfs_receipt>&>() =
								   std::declval<materialization_rooted_vfs_receipt&&>()));
		state_->exact_sqlite_path = std::move(*prepared_exact_path);
		state_->receipt = std::move(*prepared_receipt);
		return opened;
	}

	const std::optional<materialization_rooted_vfs_receipt>&
	materialization_rooted_store_opener::receipt() const noexcept
	{
		return state_->receipt;
	}

	sdk::sqlite_backend_observation_capability&
	materialization_rooted_store_opener::observation_capability() noexcept
	{
		return *state_->vfs;
	}
} // namespace cxxlens::detail::clang22::materialization
