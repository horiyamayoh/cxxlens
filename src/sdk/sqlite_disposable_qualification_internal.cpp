#include "sqlite_disposable_qualification_internal.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sqlite_disposable_normalization_internal.hpp"
#include "sqlite_payload_streaming_internal.hpp"

#if defined(__linux__)
#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace cxxlens::detail::sqlite_qualification
{
	namespace
	{
		[[nodiscard]] cxxlens::sdk::error qualification_error(const std::string_view detail)
		{
			return {"store.backend-unavailable",
					"sqlite-disposable-qualification",
					std::string{detail}};
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

		[[nodiscard]] bool
		valid_cleanup_policy(const sqlite_disposable_cleanup_policy value) noexcept
		{
			return value == sqlite_disposable_cleanup_policy::retain_private_root ||
				value == sqlite_disposable_cleanup_policy::remove_empty_private_root;
		}

		[[nodiscard]] bool valid_leaf(const std::string_view leaf) noexcept
		{
			return !leaf.empty() && leaf != "." && leaf != ".." &&
				leaf.find('/') == std::string_view::npos &&
				leaf.find('\0') == std::string_view::npos;
		}

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
			void reset() noexcept
			{
#if defined(__linux__)
				if (value_ >= 0)
					(void)::close(value_);
#endif
				value_ = -1;
			}

		  private:
			int value_{-1};
		};

#if defined(__linux__) && defined(STATX_MNT_ID)
		struct object_observation
		{
			sqlite_disposable_object_identity identity;
			std::uint64_t link_count{};
			std::uint64_t size{};
			std::int64_t modification_seconds{};
			std::uint32_t modification_nanoseconds{};
			std::int64_t change_seconds{};
			std::uint32_t change_nanoseconds{};

			[[nodiscard]] bool operator==(const object_observation&) const = default;
		};

		constexpr unsigned int required_statx_mask = STATX_TYPE | STATX_MODE | STATX_NLINK |
			STATX_INO | STATX_MNT_ID | STATX_SIZE | STATX_MTIME | STATX_CTIME;

		[[nodiscard]] sqlite_disposable_object_identity
		make_identity(const struct statx& observed) noexcept
		{
			const auto device = (static_cast<std::uint64_t>(observed.stx_dev_major) << 32U) |
				static_cast<std::uint64_t>(observed.stx_dev_minor);
			return {
				device,
				observed.stx_ino,
				static_cast<std::uint64_t>(observed.stx_mode & S_IFMT),
				static_cast<std::uint64_t>(observed.stx_mode & 07777),
				observed.stx_mnt_id,
			};
		}

		[[nodiscard]] bool observe_fd(const int descriptor, object_observation& output) noexcept
		{
			struct statx observed{};
			for (;;)
			{
				if (::statx(descriptor,
							"",
							AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW,
							required_statx_mask,
							&observed) == 0)
					break;
				if (errno != EINTR)
					return false;
			}
			if ((observed.stx_mask & required_statx_mask) != required_statx_mask)
				return false;
			output = {make_identity(observed),
					  observed.stx_nlink,
					  observed.stx_size,
					  observed.stx_mtime.tv_sec,
					  observed.stx_mtime.tv_nsec,
					  observed.stx_ctime.tv_sec,
					  observed.stx_ctime.tv_nsec};
			return true;
		}

		[[nodiscard]] bool
		observe_entry(const int parent, const char* leaf, object_observation& output) noexcept
		{
			struct statx observed{};
			for (;;)
			{
				if (::statx(parent, leaf, AT_SYMLINK_NOFOLLOW, required_statx_mask, &observed) == 0)
					break;
				if (errno != EINTR)
					return false;
			}
			if ((observed.stx_mask & required_statx_mask) != required_statx_mask)
				return false;
			output = {make_identity(observed),
					  observed.stx_nlink,
					  observed.stx_size,
					  observed.stx_mtime.tv_sec,
					  observed.stx_mtime.tv_nsec,
					  observed.stx_ctime.tv_sec,
					  observed.stx_ctime.tv_nsec};
			return true;
		}

		[[nodiscard]] bool identity_for_fd(const int descriptor,
										   sqlite_disposable_object_identity& identity) noexcept
		{
			object_observation observed;
			if (!observe_fd(descriptor, observed))
				return false;
			identity = observed.identity;
			return true;
		}

		[[nodiscard]] bool identity_for_entry(const int parent,
											  const char* leaf,
											  sqlite_disposable_object_identity& identity) noexcept
		{
			object_observation observed;
			if (!observe_entry(parent, leaf, observed))
				return false;
			identity = observed.identity;
			return true;
		}

		[[nodiscard]] bool
		directory_identity(const sqlite_disposable_object_identity& identity,
						   const std::uint64_t required_permissions =
							   std::numeric_limits<std::uint64_t>::max()) noexcept
		{
			return identity.kind == static_cast<std::uint64_t>(S_IFDIR) &&
				(required_permissions == std::numeric_limits<std::uint64_t>::max() ||
				 identity.permissions == required_permissions);
		}

		[[nodiscard]] int duplicate_cloexec(const int descriptor) noexcept
		{
			for (;;)
			{
				const auto output = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] int open_self_process_instance() noexcept
		{
#if defined(SYS_pidfd_open)
			for (;;)
			{
				const auto output = ::syscall(SYS_pidfd_open, ::getpid(), 0U);
				if (output >= 0 && output <= static_cast<long>(std::numeric_limits<int>::max()))
					return static_cast<int>(output);
				if (output < 0 && errno == EINTR)
					continue;
				if (output >= 0)
					(void)::close(static_cast<int>(output));
				return -1;
			}
#else
			return -1;
#endif
		}

		[[nodiscard]] bool process_instance_live(const int descriptor) noexcept
		{
			if (descriptor < 0)
				return false;
			struct pollfd observation{descriptor, POLLIN, 0};
			for (;;)
			{
				const auto status = ::poll(&observation, 1U, 0);
				if (status == 0)
					return observation.revents == 0;
				if (status > 0)
					return false;
				if (errno != EINTR)
					return false;
			}
		}

		[[nodiscard]] int open_private_root(const int parent, const char* leaf) noexcept
		{
			for (;;)
			{
				const auto output =
					::openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] int open_private_root_identity(const int parent, const char* leaf) noexcept
		{
			for (;;)
			{
				const auto output =
					::openat(parent, leaf, O_PATH | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		enum class empty_census_status : std::uint8_t
		{
			empty,
			nonempty,
			unavailable,
		};

		[[nodiscard]] empty_census_status empty_census(const int directory) noexcept
		{
			const auto census_descriptor =
				::openat(directory, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (census_descriptor < 0)
				return empty_census_status::unavailable;
			auto* stream = ::fdopendir(census_descriptor);
			if (stream == nullptr)
			{
				(void)::close(census_descriptor);
				return empty_census_status::unavailable;
			}

			auto outcome = empty_census_status::empty;
			for (;;)
			{
				errno = 0;
				const auto* entry = ::readdir(stream);
				if (entry == nullptr)
				{
					if (errno != 0)
						outcome = empty_census_status::unavailable;
					break;
				}
				const std::string_view name{entry->d_name};
				if (name != "." && name != "..")
				{
					outcome = empty_census_status::nonempty;
					break;
				}
			}
			if (::closedir(stream) != 0)
				return empty_census_status::unavailable;
			return outcome;
		}

		[[nodiscard]] bool fsync_exact(const int descriptor) noexcept
		{
			for (;;)
			{
				if (::fsync(descriptor) == 0)
					return true;
				if (errno != EINTR)
					return false;
			}
		}

		[[nodiscard]] bool entry_absent(const int parent, const char* leaf) noexcept
		{
			object_observation observed;
			errno = 0;
			return !observe_entry(parent, leaf, observed) && errno == ENOENT;
		}

		/**
		 * Best-effort rollback after this invocation has created a root but before capability
		 * minting. Success requires proof that unlink removed the held object itself. A
		 * final-check-to-unlink rebind may already have caused a path effect, but can never be
		 * reported as a successful rollback.
		 */
		[[nodiscard]] bool
		rollback_created_root(const int parent,
							  const int held_root,
							  const char* leaf,
							  const sqlite_disposable_object_identity& expected_parent,
							  const sqlite_disposable_object_identity& expected_root) noexcept
		{
			object_observation current_parent;
			object_observation current_root;
			object_observation current_entry;
			if (!observe_fd(parent, current_parent) || !observe_fd(held_root, current_root) ||
				!observe_entry(parent, leaf, current_entry) ||
				current_parent.identity != expected_parent ||
				current_root.identity != expected_root || current_entry.identity != expected_root)
				return false;
			if (::unlinkat(parent, leaf, AT_REMOVEDIR) != 0)
				return false;
			const auto parent_synced = fsync_exact(parent);
			object_observation removed_root;
			object_observation parent_after;
			const auto held_object_removed = observe_fd(held_root, removed_root) &&
				removed_root.identity == expected_root && removed_root.link_count == 0U;
			const auto parent_stable =
				observe_fd(parent, parent_after) && parent_after.identity == expected_parent;
			return parent_synced && entry_absent(parent, leaf) && held_object_removed &&
				parent_stable;
		}
#endif

		std::atomic<std::uint64_t> next_qualification_run_id{1U};

		[[nodiscard]] bool fresh_run_id(std::uint64_t& output) noexcept
		{
			auto current = next_qualification_run_id.load(std::memory_order_relaxed);
			for (;;)
			{
				if (current == 0U || current == std::numeric_limits<std::uint64_t>::max())
					return false;
				if (next_qualification_run_id.compare_exchange_weak(
						current, current + 1U, std::memory_order_relaxed))
				{
					output = current;
					return true;
				}
			}
		}

		enum class live_root_status : std::uint8_t
		{
			live_empty,
			rebound,
			nonempty,
		};
	} // namespace

	struct sqlite_disposable_qualification_capability::state
	{
		owned_descriptor creator_process;
		owned_descriptor parent;
		owned_descriptor root;
		std::string private_leaf;
		std::uint64_t creator_process_identity{};
		std::uint64_t qualification_run_id{};
		sqlite_disposable_object_identity parent_object;
		sqlite_disposable_object_identity root_object;
		sqlite_disposable_object_identity root_entry;
		std::string exact_profile_digest;
		std::string family_plan_digest;
		std::string effect_fault_schedule_digest;
		sqlite_disposable_cleanup_policy cleanup_policy{
			sqlite_disposable_cleanup_policy::retain_private_root};
		void (*pre_remove_signal)(void*) noexcept {};
		void* pre_remove_signal_context{};
		bool active{true};
	};

	namespace
	{
		template <class State>
		[[nodiscard]] bool revalidate_live_root_objects(const State& state) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			if (!state.parent || !state.root)
				return false;
			sqlite_disposable_object_identity current_parent;
			sqlite_disposable_object_identity current_root;
			sqlite_disposable_object_identity current_entry;
			return identity_for_fd(state.parent.get(), current_parent) &&
				identity_for_fd(state.root.get(), current_root) &&
				identity_for_entry(state.parent.get(), state.private_leaf.c_str(), current_entry) &&
				current_parent == state.parent_object && current_root == state.root_object &&
				current_entry == state.root_entry && current_root == current_entry &&
				directory_identity(current_root, 0700U) && directory_identity(current_entry, 0700U);
#else
			(void)state;
			return false;
#endif
		}

		template <class State>
		[[nodiscard]] live_root_status revalidate_live_root(const State& state) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			if (!revalidate_live_root_objects(state))
				return live_root_status::rebound;
			const auto census = empty_census(state.root.get());
			if (census == empty_census_status::empty)
				return live_root_status::live_empty;
			if (census == empty_census_status::nonempty)
				return live_root_status::nonempty;
			return live_root_status::rebound;
#else
			(void)state;
			return live_root_status::rebound;
#endif
		}

		enum class revoke_status : std::uint8_t
		{
			complete,
			already_revoked,
			wrong_process,
			root_drift,
			root_not_empty,
			remove_failed,
			remove_identity_opaque,
			parent_sync_failed,
			absence_unconfirmed,
		};

		template <class State>
		void close_state_descriptors(State& state) noexcept
		{
			state.root.reset();
			state.parent.reset();
			state.creator_process.reset();
		}

		template <class State>
		[[nodiscard]] revoke_status revoke_state(State& state) noexcept
		{
			if (!state.active)
				return revoke_status::already_revoked;
			state.active = false;

#if defined(__linux__) && defined(STATX_MNT_ID)
			const auto current_process = static_cast<std::uint64_t>(::getpid());
			if (current_process != state.creator_process_identity ||
				!process_instance_live(state.creator_process.get()))
			{
				close_state_descriptors(state);
				return revoke_status::wrong_process;
			}
			if (state.cleanup_policy == sqlite_disposable_cleanup_policy::retain_private_root)
			{
				close_state_descriptors(state);
				return revoke_status::complete;
			}

			const auto live = revalidate_live_root(state);
			if (live != live_root_status::live_empty)
			{
				close_state_descriptors(state);
				return live == live_root_status::nonempty ? revoke_status::root_not_empty
														  : revoke_status::root_drift;
			}
			const auto signal = std::exchange(state.pre_remove_signal, nullptr);
			auto* const signal_context = std::exchange(state.pre_remove_signal_context, nullptr);
			if (signal != nullptr)
				signal(signal_context);
			if (::unlinkat(state.parent.get(), state.private_leaf.c_str(), AT_REMOVEDIR) != 0)
			{
				close_state_descriptors(state);
				return revoke_status::remove_failed;
			}
			const auto parent_synced = fsync_exact(state.parent.get());
			const auto absence_confirmed =
				entry_absent(state.parent.get(), state.private_leaf.c_str());
			object_observation removed_root;
			object_observation current_parent;
			const auto held_object_removed = observe_fd(state.root.get(), removed_root) &&
				removed_root.identity == state.root_object && removed_root.link_count == 0U;
			const auto parent_stable = observe_fd(state.parent.get(), current_parent) &&
				current_parent.identity == state.parent_object;
			close_state_descriptors(state);
			if (!held_object_removed || !parent_stable)
				return revoke_status::remove_identity_opaque;
			if (!parent_synced)
				return revoke_status::parent_sync_failed;
			return absence_confirmed ? revoke_status::complete : revoke_status::absence_unconfirmed;
#else
			close_state_descriptors(state);
			return revoke_status::root_drift;
#endif
		}

		[[nodiscard]] std::string_view revoke_detail(const revoke_status status) noexcept
		{
			switch (status)
			{
				case revoke_status::complete:
				case revoke_status::already_revoked:
					return {};
				case revoke_status::wrong_process:
					return "revoke-wrong-process";
				case revoke_status::root_drift:
					return "revoke-root-drift";
				case revoke_status::root_not_empty:
					return "revoke-root-not-empty";
				case revoke_status::remove_failed:
					return "revoke-remove-failed";
				case revoke_status::remove_identity_opaque:
					return "revoke-remove-identity-opaque";
				case revoke_status::parent_sync_failed:
					return "revoke-parent-sync-failed";
				case revoke_status::absence_unconfirmed:
					return "revoke-absence-unconfirmed";
			}
			return "revoke-invalid-status";
		}

		[[nodiscard]] cxxlens::sdk::error raw_family_error(const std::string_view detail)
		{
			return {"store.sqlite-failure", "sqlite-initialization-recovery", std::string{detail}};
		}

		struct raw_namespace_census
		{
			bool main{};
			bool wal{};
			bool shared_memory{};
			bool journal{};
			bool other{};
			bool unavailable{};

			[[nodiscard]] bool operator==(const raw_namespace_census&) const = default;
		};

#if defined(__linux__) && defined(STATX_MNT_ID)
		constexpr std::uint64_t raw_fixture_maximum_file_bytes = 65'536U;
		constexpr std::uint64_t sqlite_header_byte_count = 100U;

		struct raw_file_read
		{
			sqlite_disposable_raw_file_observation observation;
			std::vector<std::byte> bytes;
		};

		[[nodiscard]] raw_namespace_census enumerate_raw_namespace(const int root) noexcept
		{
			raw_namespace_census output;
			const auto descriptor =
				::openat(root, ".", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (descriptor < 0)
			{
				output.unavailable = true;
				return output;
			}
			auto* stream = ::fdopendir(descriptor);
			if (stream == nullptr)
			{
				(void)::close(descriptor);
				output.unavailable = true;
				return output;
			}

			for (;;)
			{
				errno = 0;
				const auto* entry = ::readdir(stream);
				if (entry == nullptr)
				{
					if (errno != 0)
						output.unavailable = true;
					break;
				}
				const std::string_view name{entry->d_name};
				if (name == "." || name == "..")
					continue;

				object_observation observed;
				if (!observe_entry(root, entry->d_name, observed))
				{
					output.unavailable = true;
					continue;
				}
				const auto regular = observed.identity.kind == static_cast<std::uint64_t>(S_IFREG);
				if (name == "main")
					output.main = true;
				else if (name == "main-wal")
					output.wal = true;
				else if (name == "main-shm")
					output.shared_memory = true;
				else if (name == "main-journal")
					output.journal = true;
				else
					output.other = true;
				if (!regular)
					output.unavailable = true;
			}

			if (::closedir(stream) != 0)
				output.unavailable = true;
			return output;
		}

		[[nodiscard]] std::uint16_t read_big_endian_u16(const std::span<const std::byte> bytes,
														const std::size_t offset) noexcept
		{
			return static_cast<std::uint16_t>(
				(std::to_integer<std::uint16_t>(bytes[offset]) << 8U) |
				std::to_integer<std::uint16_t>(bytes[offset + 1U]));
		}

		[[nodiscard]] std::uint32_t read_big_endian_u32(const std::span<const std::byte> bytes,
														const std::size_t offset) noexcept
		{
			return (std::to_integer<std::uint32_t>(bytes[offset]) << 24U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 1U]) << 16U) |
				(std::to_integer<std::uint32_t>(bytes[offset + 2U]) << 8U) |
				std::to_integer<std::uint32_t>(bytes[offset + 3U]);
		}

		[[nodiscard]] bool all_zero(const std::span<const std::byte> bytes) noexcept
		{
			return std::ranges::all_of(bytes,
									   [](const std::byte value)
									   {
										   return value == std::byte{};
									   });
		}

		[[nodiscard]] bool valid_sqlite_page_size(const std::uint16_t encoded,
												  std::uint32_t& output) noexcept
		{
			output = encoded == 1U ? 65'536U : encoded;
			return output >= 512U && output <= 65'536U && (output & (output - 1U)) == 0U;
		}

		[[nodiscard]] std::optional<sqlite_disposable_main_header_state>
		parse_exact_empty_main(const std::span<const std::byte> bytes) noexcept
		{
			if (bytes.size() < sqlite_header_byte_count ||
				std::memcmp(bytes.data(), "SQLite format 3\0", 16U) != 0)
				return std::nullopt;

			std::uint32_t page_size{};
			if (!valid_sqlite_page_size(read_big_endian_u16(bytes, 16U), page_size) ||
				bytes.size() != page_size)
				return std::nullopt;

			const auto write_version = std::to_integer<std::uint8_t>(bytes[18U]);
			const auto read_version = std::to_integer<std::uint8_t>(bytes[19U]);
			if (write_version != read_version || (write_version != 1U && write_version != 2U) ||
				std::to_integer<std::uint8_t>(bytes[20U]) != 0U ||
				std::to_integer<std::uint8_t>(bytes[21U]) != 64U ||
				std::to_integer<std::uint8_t>(bytes[22U]) != 32U ||
				std::to_integer<std::uint8_t>(bytes[23U]) != 32U)
				return std::nullopt;

			const auto schema_format = read_big_endian_u32(bytes, 44U);
			const auto text_encoding = read_big_endian_u32(bytes, 56U);
			const auto page_one_content_offset = read_big_endian_u16(bytes, 105U);
			const auto canonical_content_offset =
				page_size == 65'536U ? 0U : static_cast<std::uint16_t>(page_size);
			if (read_big_endian_u32(bytes, 28U) != 1U || read_big_endian_u32(bytes, 32U) != 0U ||
				read_big_endian_u32(bytes, 36U) != 0U || read_big_endian_u32(bytes, 40U) != 0U ||
				(schema_format != 0U && schema_format != 4U) ||
				read_big_endian_u32(bytes, 52U) != 0U ||
				(text_encoding != 0U && text_encoding != 1U) ||
				read_big_endian_u32(bytes, 60U) != 0U || read_big_endian_u32(bytes, 64U) != 0U ||
				read_big_endian_u32(bytes, 68U) != 0U || !all_zero(bytes.subspan(72U, 20U)) ||
				std::to_integer<std::uint8_t>(bytes[100U]) != 0x0dU ||
				read_big_endian_u16(bytes, 101U) != 0U || read_big_endian_u16(bytes, 103U) != 0U ||
				page_one_content_offset != canonical_content_offset ||
				std::to_integer<std::uint8_t>(bytes[107U]) != 0U || !all_zero(bytes.subspan(108U)))
				return std::nullopt;

			return write_version == 2U ? sqlite_disposable_main_header_state::wal_empty
									   : sqlite_disposable_main_header_state::rollback_empty;
		}

		[[nodiscard]] cxxlens::sdk::result<raw_file_read>
		read_raw_regular_file(const int root, const std::string_view leaf)
		{
			std::string leaf_copy;
			try
			{
				leaf_copy = leaf;
			}
			catch (const std::bad_alloc&)
			{
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-allocation"));
			}

			object_observation before_entry;
			if (!observe_entry(root, leaf_copy.c_str(), before_entry) ||
				before_entry.identity.kind != static_cast<std::uint64_t>(S_IFREG))
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-not-regular"));
			if (before_entry.size > raw_fixture_maximum_file_bytes)
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-too-large"));

			owned_descriptor file{
				openat(root, leaf_copy.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC)};
			if (!file)
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-open"));
			object_observation before_file;
			if (!observe_fd(file.get(), before_file) ||
				before_file.identity != before_entry.identity ||
				before_file.size != before_entry.size ||
				before_file.identity.kind != static_cast<std::uint64_t>(S_IFREG))
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-identity"));

			std::vector<std::byte> bytes;
			try
			{
				bytes.resize(static_cast<std::size_t>(before_file.size));
			}
			catch (const std::bad_alloc&)
			{
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-allocation"));
			}

			cxxlens::sdk::sqlite_incremental_sha256 digest;
			std::uint64_t offset{};
			while (offset < before_file.size)
			{
				const auto remaining = before_file.size - offset;
				const auto requested =
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 4096U));
				ssize_t count{};
				for (;;)
				{
					if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
						return cxxlens::sdk::unexpected(raw_family_error("raw-file-offset"));
					count = ::pread(file.get(),
									bytes.data() + static_cast<std::size_t>(offset),
									requested,
									static_cast<off_t>(offset));
					if (count >= 0 || errno != EINTR)
						break;
				}
				if (count <= 0 || static_cast<std::size_t>(count) > requested)
					return cxxlens::sdk::unexpected(raw_family_error("raw-file-short-read"));
				const auto count_size = static_cast<std::size_t>(count);
				if (auto updated = digest.update(std::span<const std::byte>{
						bytes.data() + static_cast<std::size_t>(offset), count_size});
					!updated)
					return cxxlens::sdk::unexpected(raw_family_error("raw-file-digest"));
				offset += static_cast<std::uint64_t>(count_size);
			}

			// A same-size replacement can preserve identity and size. Re-read the complete bounded
			// byte sequence before accepting the first digest, then also compare statx change times
			// below. Either byte drift or metadata drift remains unresolved.
			std::vector<std::byte> verification;
			try
			{
				verification.resize(bytes.size());
			}
			catch (const std::bad_alloc&)
			{
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-allocation"));
			}
			offset = 0U;
			while (offset < before_file.size)
			{
				const auto remaining = before_file.size - offset;
				const auto requested =
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, 4096U));
				ssize_t count{};
				for (;;)
				{
					if (offset > static_cast<std::uint64_t>(std::numeric_limits<off_t>::max()))
						return cxxlens::sdk::unexpected(raw_family_error("raw-file-offset"));
					count = ::pread(file.get(),
									verification.data() + static_cast<std::size_t>(offset),
									requested,
									static_cast<off_t>(offset));
					if (count >= 0 || errno != EINTR)
						break;
				}
				if (count <= 0 || static_cast<std::size_t>(count) > requested)
					return cxxlens::sdk::unexpected(raw_family_error("raw-file-short-read"));
				offset += static_cast<std::uint64_t>(count);
			}
			if (!std::ranges::equal(bytes, verification))
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-byte-drift"));

			object_observation after_file;
			object_observation after_entry;
			if (!observe_fd(file.get(), after_file) ||
				!observe_entry(root, leaf_copy.c_str(), after_entry) || after_file != before_file ||
				after_entry != before_entry)
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-drift"));
			auto sha256 = digest.finish();
			if (!sha256)
				return cxxlens::sdk::unexpected(raw_family_error("raw-file-digest"));

			return raw_file_read{
				{before_file.identity, before_entry.identity, before_file.size, std::move(*sha256)},
				std::move(bytes)};
		}
#endif

		template <class State>
		[[nodiscard]] bool
		raw_request_matches(const State& state,
							const sqlite_disposable_qualification_request& request) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			return state.active &&
				static_cast<std::uint64_t>(::getpid()) == state.creator_process_identity &&
				process_instance_live(state.creator_process.get()) &&
				request.creator_process_identity == state.creator_process_identity &&
				request.qualification_run_id == state.qualification_run_id &&
				request.parent_object == state.parent_object &&
				request.root_object == state.root_object &&
				request.root_entry == state.root_entry &&
				request.exact_profile_digest == state.exact_profile_digest &&
				request.family_plan_digest == state.family_plan_digest &&
				request.effect_fault_schedule_digest == state.effect_fault_schedule_digest &&
				request.cleanup_policy == state.cleanup_policy &&
				revalidate_live_root_objects(state);
#else
			(void)state;
			(void)request;
			return false;
#endif
		}
	} // namespace

	class sqlite_disposable_raw_family_observer
	{
	  public:
		[[nodiscard]] static cxxlens::sdk::result<sqlite_disposable_raw_family_observation>
		observe(sqlite_disposable_qualification_capability& capability,
				const sqlite_disposable_qualification_request& request)
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			auto* const state = capability.state_.get();
			if (state == nullptr || !state->active)
				return cxxlens::sdk::unexpected(raw_family_error("raw-capability-revoked"));
			if (request.requested_effect != sqlite_disposable_requested_effect::classify_source)
				return cxxlens::sdk::unexpected(raw_family_error("raw-effect-not-authorized"));
			if (!raw_request_matches(*state, request))
				return cxxlens::sdk::unexpected(raw_family_error("raw-capability-binding"));

			object_observation parent_before;
			object_observation root_before;
			if (!observe_fd(state->parent.get(), parent_before) ||
				!observe_fd(state->root.get(), root_before))
				return cxxlens::sdk::unexpected(raw_family_error("raw-anchor-observation"));
			const auto before = enumerate_raw_namespace(state->root.get());
			if (before.unavailable)
				return cxxlens::sdk::unexpected(raw_family_error("raw-census-unavailable"));
			if (!before.main)
				return cxxlens::sdk::unexpected(raw_family_error(
					before.wal || before.shared_memory || before.journal || before.other
						? "raw-orphan-sidecar"
						: "raw-main-missing"));
			if (before.shared_memory || before.journal || before.other)
				return cxxlens::sdk::unexpected(raw_family_error("raw-family-unresolved-topology"));

			auto main = read_raw_regular_file(state->root.get(), "main");
			if (!main)
				return cxxlens::sdk::unexpected(std::move(main.error()));
			const auto main_header = parse_exact_empty_main(main->bytes);
			if (!main_header)
				return cxxlens::sdk::unexpected(raw_family_error("raw-main-not-exact-empty"));

			std::optional<sqlite_disposable_raw_file_observation> wal_observation;
			if (before.wal)
			{
				auto wal = read_raw_regular_file(state->root.get(), "main-wal");
				if (!wal)
					return cxxlens::sdk::unexpected(std::move(wal.error()));
				if (wal->observation.byte_count != 0U)
					return cxxlens::sdk::unexpected(raw_family_error("raw-nonzero-wal-unresolved"));
				wal_observation = std::move(wal->observation);
			}

			const auto after = enumerate_raw_namespace(state->root.get());
			object_observation parent_after;
			object_observation root_after;
			if (after.unavailable || after != before ||
				!observe_fd(state->parent.get(), parent_after) ||
				!observe_fd(state->root.get(), root_after) ||
				parent_after.identity != parent_before.identity ||
				root_after.identity != root_before.identity)
				return cxxlens::sdk::unexpected(raw_family_error("raw-namespace-drift"));

			const sqlite_disposable_empty_family_observation family_observation{
				true,
				main->observation.object == main->observation.entry,
				main->observation.object == main->observation.entry,
				true,
				*main_header,
				before.wal ? sqlite_disposable_wal_state::readable_zero_byte
						   : sqlite_disposable_wal_state::absent,
				false,
				sqlite_disposable_journal_state::absent,
				false};
			auto family = classify_sqlite_disposable_empty_family(family_observation);
			if (!family)
				return cxxlens::sdk::unexpected(std::move(family.error()));

			return sqlite_disposable_raw_family_observation{family_observation,
															*family,
															std::move(main->observation),
															std::move(wal_observation)};
#else
			(void)capability;
			(void)request;
			return cxxlens::sdk::unexpected(raw_family_error("raw-unsupported-platform"));
#endif
		}
	};

	sqlite_disposable_parent_directory::sqlite_disposable_parent_directory(
		const int descriptor, const sqlite_disposable_object_identity identity) noexcept
		: descriptor_{descriptor}, identity_{identity}
	{
	}

	sqlite_disposable_parent_directory::sqlite_disposable_parent_directory(
		sqlite_disposable_parent_directory&& other) noexcept
		: descriptor_{std::exchange(other.descriptor_, -1)}, identity_{other.identity_}
	{
	}

	sqlite_disposable_parent_directory::~sqlite_disposable_parent_directory()
	{
#if defined(__linux__)
		if (descriptor_ >= 0)
			(void)::close(descriptor_);
#endif
	}

	sqlite_disposable_qualification_capability::sqlite_disposable_qualification_capability(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}

	sqlite_disposable_qualification_capability::sqlite_disposable_qualification_capability(
		sqlite_disposable_qualification_capability&& other) noexcept
		: state_{std::move(other.state_)}
	{
	}

	sqlite_disposable_qualification_capability::~sqlite_disposable_qualification_capability()
	{
		if (state_)
			(void)revoke_state(*state_);
	}

	sqlite_disposable_qualification_request
	sqlite_disposable_qualification_capability::no_effect_request() const
	{
		if (!state_)
			return {};
		return {
			state_->creator_process_identity,
			state_->qualification_run_id,
			state_->parent_object,
			state_->root_object,
			state_->root_entry,
			state_->exact_profile_digest,
			state_->family_plan_digest,
			state_->effect_fault_schedule_digest,
			state_->cleanup_policy,
			sqlite_disposable_requested_effect::no_effect,
		};
	}

	cxxlens::sdk::result<void> sqlite_disposable_qualification_capability::revoke()
	{
		if (!state_)
			return cxxlens::sdk::unexpected(qualification_error("revoke-moved-capability"));
		const auto status = revoke_state(*state_);
		const auto detail = revoke_detail(status);
		if (detail.empty())
			return {};
		return cxxlens::sdk::unexpected(qualification_error(detail));
	}

	cxxlens::sdk::result<sqlite_disposable_parent_directory>
	duplicate_sqlite_disposable_parent_directory(const int directory_descriptor)
	{
#if defined(__linux__) && defined(STATX_MNT_ID)
		sqlite_disposable_object_identity identity;
		if (!identity_for_fd(directory_descriptor, identity) || !directory_identity(identity))
			return cxxlens::sdk::unexpected(qualification_error("parent-not-directory"));
		const auto duplicate = duplicate_cloexec(directory_descriptor);
		if (duplicate < 0)
			return cxxlens::sdk::unexpected(qualification_error("parent-duplicate"));
		sqlite_disposable_object_identity duplicate_identity;
		if (!identity_for_fd(duplicate, duplicate_identity) || duplicate_identity != identity)
		{
			(void)::close(duplicate);
			return cxxlens::sdk::unexpected(qualification_error("parent-identity"));
		}
		return sqlite_disposable_parent_directory{duplicate, identity};
#else
		(void)directory_descriptor;
		return cxxlens::sdk::unexpected(qualification_error("unsupported-platform"));
#endif
	}

	cxxlens::sdk::result<sqlite_disposable_qualification_capability>
	make_sqlite_disposable_qualification_capability(
		sqlite_disposable_parent_directory parent,
		const std::string_view private_leaf,
		sqlite_disposable_qualification_bindings bindings)
	{
#if defined(__linux__) && defined(STATX_MNT_ID)
		if (parent.descriptor_ < 0)
			return cxxlens::sdk::unexpected(qualification_error("parent-revoked"));
		if (!valid_leaf(private_leaf))
			return cxxlens::sdk::unexpected(qualification_error("private-leaf"));
		errno = 0;
		const auto maximum_leaf = ::fpathconf(parent.descriptor_, _PC_NAME_MAX);
		if (maximum_leaf <= 0 ||
			static_cast<std::uint64_t>(private_leaf.size()) >
				static_cast<std::uint64_t>(maximum_leaf))
			return cxxlens::sdk::unexpected(qualification_error("private-leaf-length"));
		if (!canonical_sha256(bindings.exact_profile_digest) ||
			!canonical_sha256(bindings.family_plan_digest) ||
			!canonical_sha256(bindings.effect_fault_schedule_digest) ||
			!valid_cleanup_policy(bindings.cleanup_policy))
			return cxxlens::sdk::unexpected(qualification_error("binding"));

		std::unique_ptr<sqlite_disposable_qualification_capability::state> state;
		std::string leaf;
		try
		{
			state = std::make_unique<sqlite_disposable_qualification_capability::state>();
			leaf = private_leaf;
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(qualification_error("allocation"));
		}

		owned_descriptor retained_parent{std::exchange(parent.descriptor_, -1)};
		sqlite_disposable_object_identity current_parent;
		if (!identity_for_fd(retained_parent.get(), current_parent) ||
			current_parent != parent.identity_ || !directory_identity(current_parent))
			return cxxlens::sdk::unexpected(qualification_error("parent-identity"));

		const auto process_identity = static_cast<std::uint64_t>(::getpid());
		owned_descriptor creator_process{open_self_process_instance()};
		if (process_identity == 0U || !creator_process ||
			!process_instance_live(creator_process.get()) ||
			static_cast<std::uint64_t>(::getpid()) != process_identity)
			return cxxlens::sdk::unexpected(qualification_error("creator-process-instance"));

		if (::mkdirat(retained_parent.get(), leaf.c_str(), 0700) != 0)
			return cxxlens::sdk::unexpected(qualification_error(
				errno == EEXIST ? "private-root-not-fresh" : "private-root-create"));

		owned_descriptor held_root{open_private_root_identity(retained_parent.get(), leaf.c_str())};
		if (!held_root)
			return cxxlens::sdk::unexpected(qualification_error("private-root-rollback-opaque"));

		object_observation held_root_observation;
		object_observation first_entry_observation;
		if (!observe_fd(held_root.get(), held_root_observation))
			return cxxlens::sdk::unexpected(qualification_error("private-root-rollback-opaque"));

		const auto created_failure = [&](const std::string_view detail)
		{
			const auto rolled_back = rollback_created_root(retained_parent.get(),
														   held_root.get(),
														   leaf.c_str(),
														   current_parent,
														   held_root_observation.identity);
			return cxxlens::sdk::result<sqlite_disposable_qualification_capability>{
				cxxlens::sdk::unexpected(
					qualification_error(rolled_back ? detail : "private-root-rollback-opaque"))};
		};

		if (!observe_entry(retained_parent.get(), leaf.c_str(), first_entry_observation) ||
			first_entry_observation.identity != held_root_observation.identity ||
			!directory_identity(held_root_observation.identity, 0700U) ||
			!directory_identity(first_entry_observation.identity, 0700U))
			return created_failure("private-root-entry");

		owned_descriptor root{open_private_root(retained_parent.get(), leaf.c_str())};
		if (!root)
			return created_failure("private-root-open");

		sqlite_disposable_object_identity root_object;
		sqlite_disposable_object_identity root_entry;
		if (!identity_for_fd(root.get(), root_object) ||
			!identity_for_entry(retained_parent.get(), leaf.c_str(), root_entry) ||
			root_object != held_root_observation.identity ||
			root_entry != held_root_observation.identity ||
			!directory_identity(root_object, 0700U) || !directory_identity(root_entry, 0700U))
			return created_failure("private-root-rebound");
		if (empty_census(root.get()) != empty_census_status::empty)
			return created_failure("private-root-not-empty");

		std::uint64_t run_id{};
		if (!fresh_run_id(run_id))
			return created_failure("run-id-exhausted");

		state->creator_process = std::move(creator_process);
		state->parent = std::move(retained_parent);
		state->root = std::move(root);
		state->private_leaf = std::move(leaf);
		state->creator_process_identity = process_identity;
		state->qualification_run_id = run_id;
		state->parent_object = current_parent;
		state->root_object = root_object;
		state->root_entry = root_entry;
		state->exact_profile_digest = std::move(bindings.exact_profile_digest);
		state->family_plan_digest = std::move(bindings.family_plan_digest);
		state->effect_fault_schedule_digest = std::move(bindings.effect_fault_schedule_digest);
		state->cleanup_policy = bindings.cleanup_policy;
		return sqlite_disposable_qualification_capability{std::move(state)};
#else
		(void)parent;
		(void)private_leaf;
		(void)bindings;
		return cxxlens::sdk::unexpected(qualification_error("unsupported-platform"));
#endif
	}

	sqlite_disposable_qualification_verdict enter_sqlite_disposable_qualification(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request) noexcept
	{
		auto* state = capability.state_.get();
		if (state == nullptr || !state->active)
			return sqlite_disposable_qualification_verdict::capability_revoked_or_stale;
#if defined(__linux__) && defined(STATX_MNT_ID)
		if (static_cast<std::uint64_t>(::getpid()) != state->creator_process_identity ||
			!process_instance_live(state->creator_process.get()) ||
			request.creator_process_identity != state->creator_process_identity)
			return sqlite_disposable_qualification_verdict::wrong_creator_process;
		if (request.qualification_run_id != state->qualification_run_id)
			return sqlite_disposable_qualification_verdict::wrong_run;
		if (request.exact_profile_digest != state->exact_profile_digest)
			return sqlite_disposable_qualification_verdict::wrong_profile;
		if (request.family_plan_digest != state->family_plan_digest)
			return sqlite_disposable_qualification_verdict::wrong_family_plan;
		if (request.effect_fault_schedule_digest != state->effect_fault_schedule_digest)
			return sqlite_disposable_qualification_verdict::wrong_effect_fault_schedule;
		if (request.cleanup_policy != state->cleanup_policy)
			return sqlite_disposable_qualification_verdict::wrong_cleanup_policy;
		if (request.parent_object != state->parent_object)
			return sqlite_disposable_qualification_verdict::wrong_parent_binding;
		if (request.root_object != state->root_object)
			return sqlite_disposable_qualification_verdict::wrong_root_object_binding;
		if (request.root_entry != state->root_entry)
			return sqlite_disposable_qualification_verdict::wrong_root_entry_binding;

		const auto live = revalidate_live_root(*state);
		if (live == live_root_status::rebound)
			return sqlite_disposable_qualification_verdict::root_entry_rebound;
		if (live == live_root_status::nonempty)
			return sqlite_disposable_qualification_verdict::root_not_empty;
		if (request.requested_effect != sqlite_disposable_requested_effect::no_effect)
			return sqlite_disposable_qualification_verdict::effect_not_authorized;
		return sqlite_disposable_qualification_verdict::effects_denied_ready;
#else
		(void)request;
		return sqlite_disposable_qualification_verdict::root_entry_rebound;
#endif
	}

	void set_sqlite_disposable_pre_remove_signal_for_testing(
		sqlite_disposable_qualification_capability& capability,
		void (*signal)(void*) noexcept,
		void* context) noexcept
	{
		if (capability.state_ == nullptr || !capability.state_->active)
			return;
		capability.state_->pre_remove_signal = signal;
		capability.state_->pre_remove_signal_context = context;
	}

	void invalidate_sqlite_disposable_process_instance_for_testing(
		sqlite_disposable_qualification_capability& capability) noexcept
	{
		if (capability.state_ != nullptr)
			capability.state_->creator_process.reset();
	}

	cxxlens::sdk::result<sqlite_disposable_raw_family_observation>
	observe_sqlite_disposable_raw_empty_family(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request)
	{
		return sqlite_disposable_raw_family_observer::observe(capability, request);
	}

	cxxlens::sdk::result<sqlite_disposable_fz_post_cleanup_result>
	cleanup_sqlite_disposable_fz_post_wal_for_testing(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request) noexcept
	{
#if defined(__linux__) && defined(STATX_MNT_ID)
		try
		{
			auto* const state = capability.state_.get();
			if (state == nullptr || !state->active)
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-capability-revoked"));
			if (request.requested_effect != sqlite_disposable_requested_effect::normalize_source)
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-effect-not-authorized"));
			if (!raw_request_matches(*state, request))
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-capability-binding"));

			auto classify_request = request;
			classify_request.requested_effect = sqlite_disposable_requested_effect::classify_source;
			auto before =
				sqlite_disposable_raw_family_observer::observe(capability, classify_request);
			if (!before)
				return cxxlens::sdk::unexpected(std::move(before.error()));
			if (before->family.family !=
					sqlite_disposable_empty_family::exact_pre_or_post_zero_wal ||
				before->family.phase != sqlite_disposable_family_phase::post || !before->wal ||
				before->wal->byte_count != 0U)
				return cxxlens::sdk::unexpected(raw_family_error("normalization-fz-post-required"));

			auto plan = plan_sqlite_disposable_empty_normalization(before->observation);
			if (!plan)
				return cxxlens::sdk::unexpected(std::move(plan.error()));
			if (plan->family != before->family ||
				plan->route !=
					sqlite_disposable_normalization_route::establish_rollback_empty_anchor ||
				plan->uses_existing_zero_byte_wal ||
				plan->may_handoff_to_ordinary_fresh_initialization)
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-route-not-authorized"));

			if (!revalidate_live_root_objects(*state))
				return cxxlens::sdk::unexpected(raw_family_error("normalization-anchor-drift"));
			auto current_wal = read_raw_regular_file(state->root.get(), "main-wal");
			if (!current_wal || current_wal->observation.byte_count != 0U ||
				current_wal->observation != *before->wal)
				return cxxlens::sdk::unexpected(raw_family_error("normalization-wal-drift"));
			if (!revalidate_live_root_objects(*state))
				return cxxlens::sdk::unexpected(raw_family_error("normalization-anchor-drift"));

			// This is the only mutation boundary. A test may rebind the known leaf here; the
			// subsequent census must reject the result, and this function must never retry.
			const auto signal = std::exchange(state->pre_remove_signal, nullptr);
			auto* const signal_context = std::exchange(state->pre_remove_signal_context, nullptr);
			if (signal != nullptr)
				signal(signal_context);
			if (::unlinkat(state->root.get(), "main-wal", 0) != 0)
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-wal-unlink-uncertain"));
			if (!fsync_exact(state->root.get()))
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-parent-sync-uncertain"));

			auto after =
				sqlite_disposable_raw_family_observer::observe(capability, classify_request);
			if (!after)
				return cxxlens::sdk::unexpected(std::move(after.error()));
			if (after->family.family !=
					sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar ||
				after->family.phase != sqlite_disposable_family_phase::post || after->wal ||
				after->main != before->main)
				return cxxlens::sdk::unexpected(
					raw_family_error("normalization-anchor-not-established"));

			return sqlite_disposable_fz_post_cleanup_result{std::move(*before), std::move(*after)};
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(raw_family_error("normalization-allocation"));
		}
		catch (...)
		{
			return cxxlens::sdk::unexpected(raw_family_error("normalization-exception"));
		}
#else
		(void)capability;
		(void)request;
		return cxxlens::sdk::unexpected(raw_family_error("normalization-unsupported-platform"));
#endif
	}

	cxxlens::sdk::result<void> write_sqlite_disposable_fixture_file_for_testing(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request,
		const std::string_view leaf,
		const std::span<const std::byte> bytes) noexcept
	{
#if defined(__linux__) && defined(STATX_MNT_ID)
		auto* const state = capability.state_.get();
		if (state == nullptr || !state->active)
			return cxxlens::sdk::unexpected(qualification_error("fixture-capability-revoked"));
		if (request.requested_effect != sqlite_disposable_requested_effect::no_effect ||
			!raw_request_matches(*state, request))
			return cxxlens::sdk::unexpected(qualification_error("fixture-capability-binding"));
		if (!valid_leaf(leaf) || bytes.size() > 65'536U)
			return cxxlens::sdk::unexpected(qualification_error("fixture-file"));

		std::string leaf_copy;
		try
		{
			leaf_copy = leaf;
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(qualification_error("fixture-allocation"));
		}

		owned_descriptor file{::openat(state->root.get(),
									   leaf_copy.c_str(),
									   O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC,
									   0600)};
		if (!file)
			return cxxlens::sdk::unexpected(qualification_error("fixture-file-open"));
		std::size_t offset{};
		while (offset < bytes.size())
		{
			ssize_t count{};
			for (;;)
			{
				count = ::write(file.get(), bytes.data() + offset, bytes.size() - offset);
				if (count >= 0 || errno != EINTR)
					break;
			}
			if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
				return cxxlens::sdk::unexpected(qualification_error("fixture-file-write"));
			offset += static_cast<std::size_t>(count);
		}
		if (!fsync_exact(file.get()) || !fsync_exact(state->root.get()))
			return cxxlens::sdk::unexpected(qualification_error("fixture-file-sync"));
		return {};
#else
		(void)capability;
		(void)request;
		(void)leaf;
		(void)bytes;
		return cxxlens::sdk::unexpected(qualification_error("unsupported-platform"));
#endif
	}
} // namespace cxxlens::detail::sqlite_qualification
