#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/stat.h>
#if defined(__linux__)
#include <sys/inotify.h>
#include <sys/vfs.h>
#endif
#include <unistd.h>

#include "sqlite_default_observation_internal.hpp"
#include "sqlite_source_shm_readonly_preflight_internal.hpp"
#include "sqlite_wal_recovery_workspace_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::string_view observation_profile{"default-filesystem-v1"};
		constexpr std::string_view wal_suffix{"-wal"};
		constexpr std::string_view shm_suffix{"-shm"};
		constexpr std::string_view journal_suffix{"-journal"};
		constexpr std::size_t digest_buffer_bytes = std::size_t{64U} * 1024U;

		[[nodiscard]] error observation_io_error()
		{
			return {"store.sqlite-failure", "sqlite-sidecar-state", "observation-io-failure"};
		}

		[[nodiscard]] error observation_allocation_error()
		{
			return {"store.backend-unavailable", "sqlite", "vfs-observation-allocation"};
		}

		[[nodiscard]] error observation_unavailable_error()
		{
			return {"store.backend-unavailable", "sqlite", "vfs-observation"};
		}

		[[nodiscard]] error locator_binding_error()
		{
			return {"store.sqlite-failure", "sqlite-locator", "canonicalization-failed"};
		}

		[[nodiscard]] error concurrent_authority_error()
		{
			return {"store.sqlite-failure", "sqlite-initialization", "concurrent-authority-change"};
		}

		[[nodiscard]] error bootstrap_durability_error()
		{
			return {"store.sqlite-failure", "sqlite-initialization-bootstrap", "durability-opaque"};
		}

		[[nodiscard]] error object_kind_error()
		{
			return {"store.sqlite-failure", "sqlite-object-kind", "not-regular-or-equivalent"};
		}

		[[nodiscard]] error raw_create_error(const int value)
		{
			return {"store.sqlite-failure", "open", "posix-errno-" + std::to_string(value)};
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
				const auto output = value_;
				value_ = -1;
				return output;
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

		struct locator_parts
		{
			std::string parent;
			std::string leaf;
		};

		[[nodiscard]] result<locator_parts> split_absolute_locator(const std::string_view value)
		{
			if (value.size() < 2U || value.front() != '/' || value.back() == '/' ||
				value.contains('\0'))
				return unexpected(locator_binding_error());
			const auto separator = value.rfind('/');
			if (separator == std::string_view::npos || separator + 1U >= value.size())
				return unexpected(locator_binding_error());
			try
			{
				const auto root_prefix = value.substr(0U, separator + 1U);
				const auto parent = std::ranges::all_of(root_prefix,
														[](const char byte)
														{
															return byte == '/';
														})
					? std::string{root_prefix}
					: std::string{value.substr(0U, separator)};
				return locator_parts{
					parent,
					std::string{value.substr(separator + 1U)},
				};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(observation_allocation_error());
			}
			catch (const std::length_error&)
			{
				return unexpected(observation_allocation_error());
			}
		}

		[[nodiscard]] int open_directory(const char* path) noexcept
		{
			for (;;)
			{
				const auto descriptor =
					::open(path, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC);
				if (descriptor >= 0 || errno != EINTR)
					return descriptor;
			}
		}

		[[nodiscard]] int open_relative(const int parent,
										const char* leaf,
										const int flags,
										const mode_t mode = 0) noexcept
		{
			for (;;)
			{
				const auto descriptor =
					mode == 0 ? ::openat(parent, leaf, flags) : ::openat(parent, leaf, flags, mode);
				if (descriptor >= 0 || errno != EINTR)
					return descriptor;
			}
		}

		[[nodiscard]] int stat_relative(const int parent,
										const char* leaf,
										struct stat& output,
										const int flags) noexcept
		{
			for (;;)
			{
				const auto status = ::fstatat(parent, leaf, &output, flags);
				if (status == 0 || errno != EINTR)
					return status;
			}
		}

		[[nodiscard]] bool same_object(const struct stat& left, const struct stat& right) noexcept
		{
			return left.st_dev == right.st_dev && left.st_ino == right.st_ino &&
				(left.st_mode & S_IFMT) == (right.st_mode & S_IFMT);
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

		void append_pointer(std::vector<std::byte>& output, const void* value)
		{
			append_u64(output, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(value)));
		}

		void append_stat_object(std::vector<std::byte>& output, const struct stat& value)
		{
			append_u64(output, static_cast<std::uint64_t>(value.st_dev));
			append_u64(output, static_cast<std::uint64_t>(value.st_ino));
			append_u64(output, static_cast<std::uint64_t>(value.st_mode & S_IFMT));
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		make_stat_identity(const std::string_view profile, const struct stat& value)
		{
			sqlite_backend_opaque_identity output;
			output.profile = profile;
			output.bytes.reserve(24U);
			append_stat_object(output.bytes, value);
			return output;
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		make_entry_identity(const struct stat& parent,
							const std::string_view leaf,
							const struct stat& namespace_entry,
							const std::optional<struct stat>& resolved_object)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.directory-entry.v1";
			output.bytes.reserve(8U + leaf.size() + std::size_t{24U} * 3U + 1U);
			append_stat_object(output.bytes, parent);
			append_bytes(output.bytes, leaf);
			append_stat_object(output.bytes, namespace_entry);
			output.bytes.push_back(resolved_object ? std::byte{1U} : std::byte{});
			if (resolved_object)
				append_stat_object(output.bytes, *resolved_object);
			return output;
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_object_filesystem_profile(const int descriptor, const struct stat& object)
		{
#if defined(__linux__)
			struct statfs observed
			{
			};
			if (::fstatfs(descriptor, &observed) != 0)
				return std::nullopt;
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.object-filesystem.v1";
			output.bytes.reserve(56U);
			append_u64(output.bytes, static_cast<std::uint64_t>(object.st_dev));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_type));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_bsize));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_namelen));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_flags));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_fsid.__val[0]));
			append_u64(output.bytes, static_cast<std::uint64_t>(observed.f_fsid.__val[1]));
			return output;
#else
			(void)descriptor;
			(void)object;
			return std::nullopt;
#endif
		}

		[[nodiscard]] std::optional<sqlite_backend_opaque_identity>
		make_object_mount_identity(const int descriptor)
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			struct statx observed
			{
			};
			if (::statx(
					descriptor, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, STATX_MNT_ID, &observed) !=
					0 ||
				(observed.stx_mask & STATX_MNT_ID) == 0U)
				return std::nullopt;
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.linux-mount-id.v1";
			output.bytes.reserve(8U);
			append_u64(output.bytes, observed.stx_mnt_id);
			return output;
#else
			(void)descriptor;
			return std::nullopt;
#endif
		}

		struct content_epoch
		{
			std::uint64_t device{};
			std::uint64_t inode{};
			std::uint64_t size{};
			std::int64_t modified_seconds{};
			std::int64_t modified_nanoseconds{};
			std::int64_t changed_seconds{};
			std::int64_t changed_nanoseconds{};
			std::uint64_t kind{};

			[[nodiscard]] bool operator==(const content_epoch&) const = default;
		};

		[[nodiscard]] std::optional<content_epoch>
		epoch_from_stat(const struct stat& value) noexcept
		{
			if (value.st_size < 0 || !S_ISREG(value.st_mode))
				return std::nullopt;
#if defined(__APPLE__)
			return content_epoch{
				static_cast<std::uint64_t>(value.st_dev),
				static_cast<std::uint64_t>(value.st_ino),
				static_cast<std::uint64_t>(value.st_size),
				static_cast<std::int64_t>(value.st_mtimespec.tv_sec),
				static_cast<std::int64_t>(value.st_mtimespec.tv_nsec),
				static_cast<std::int64_t>(value.st_ctimespec.tv_sec),
				static_cast<std::int64_t>(value.st_ctimespec.tv_nsec),
				static_cast<std::uint64_t>(value.st_mode & S_IFMT),
			};
#else
			return content_epoch{
				static_cast<std::uint64_t>(value.st_dev),
				static_cast<std::uint64_t>(value.st_ino),
				static_cast<std::uint64_t>(value.st_size),
				static_cast<std::int64_t>(value.st_mtim.tv_sec),
				static_cast<std::int64_t>(value.st_mtim.tv_nsec),
				static_cast<std::int64_t>(value.st_ctim.tv_sec),
				static_cast<std::int64_t>(value.st_ctim.tv_nsec),
				static_cast<std::uint64_t>(value.st_mode & S_IFMT),
			};
#endif
		}

		class incremental_sha256
		{
		  public:
			void update(std::span<const std::byte> input) noexcept
			{
				total_bytes_ += static_cast<std::uint64_t>(input.size());
				if (pending_size_ != 0U)
				{
					const auto count = std::min(input.size(), block_bytes - pending_size_);
					std::ranges::copy(input.first(count), pending_.begin() + pending_size_);
					pending_size_ += count;
					input = input.subspan(count);
					if (pending_size_ != block_bytes)
						return;
					transform(pending_);
					pending_size_ = 0U;
				}
				while (input.size() >= block_bytes)
				{
					transform(input.first(block_bytes));
					input = input.subspan(block_bytes);
				}
				std::ranges::copy(input, pending_.begin());
				pending_size_ = input.size();
			}

			[[nodiscard]] std::string finish()
			{
				const auto bit_count = total_bytes_ * 8U;
				pending_.at(pending_size_++) = std::byte{0x80U};
				if (pending_size_ > 56U)
				{
					std::fill(pending_.begin() + pending_size_, pending_.end(), std::byte{});
					transform(pending_);
					pending_size_ = 0U;
				}
				std::fill(pending_.begin() + pending_size_, pending_.begin() + 56U, std::byte{});
				for (std::size_t index{}; index < 8U; ++index)
					pending_.at(56U + index) = static_cast<std::byte>(
						(bit_count >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
				transform(pending_);
				constexpr std::string_view digits{"0123456789abcdef"};
				std::string output{"sha256:"};
				output.reserve(71U);
				for (const auto word : state_)
					for (std::uint32_t shift = 28U;; shift -= 4U)
					{
						output.push_back(digits[(word >> shift) & 0x0fU]);
						if (shift == 0U)
							break;
					}
				return output;
			}

		  private:
			static constexpr std::size_t block_bytes = 64U;
			static constexpr std::array<std::uint32_t, 64U> round_constants{
				0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U,
				0x923f82a4U, 0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U,
				0x72be5d74U, 0x80deb1feU, 0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U,
				0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU, 0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU,
				0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U, 0xc6e00bf3U, 0xd5a79147U,
				0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU, 0x53380d13U,
				0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
				0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U,
				0x19a4c116U, 0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU,
				0x5b9cca4fU, 0x682e6ff3U, 0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U,
				0x90befffaU, 0xa4506cebU, 0xbef9a3f7U, 0xc67178f2U,
			};

			void transform(const std::span<const std::byte> block) noexcept
			{
				std::array<std::uint32_t, 64U> schedule{};
				for (std::size_t index{}; index < 16U; ++index)
				{
					const auto offset = index * 4U;
					schedule.at(index) = (std::to_integer<std::uint32_t>(block[offset]) << 24U) |
						(std::to_integer<std::uint32_t>(block[offset + 1U]) << 16U) |
						(std::to_integer<std::uint32_t>(block[offset + 2U]) << 8U) |
						std::to_integer<std::uint32_t>(block[offset + 3U]);
				}
				for (std::size_t index = 16U; index < schedule.size(); ++index)
				{
					const auto small_zero = std::rotr(schedule.at(index - 15U), 7) ^
						std::rotr(schedule.at(index - 15U), 18) ^ (schedule.at(index - 15U) >> 3U);
					const auto small_one = std::rotr(schedule.at(index - 2U), 17) ^
						std::rotr(schedule.at(index - 2U), 19) ^ (schedule.at(index - 2U) >> 10U);
					schedule.at(index) =
						schedule.at(index - 16U) + small_zero + schedule.at(index - 7U) + small_one;
				}
				auto [a, b, c, d, e, f, g, h] = state_;
				for (std::size_t index{}; index < schedule.size(); ++index)
				{
					const auto big_one = std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25);
					const auto choose = (e & f) ^ (~e & g);
					const auto first =
						h + big_one + choose + round_constants.at(index) + schedule.at(index);
					const auto big_zero = std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22);
					const auto majority = (a & b) ^ (a & c) ^ (b & c);
					const auto second = big_zero + majority;
					h = g;
					g = f;
					f = e;
					e = d + first;
					d = c;
					c = b;
					b = a;
					a = first + second;
				}
				state_[0U] += a;
				state_[1U] += b;
				state_[2U] += c;
				state_[3U] += d;
				state_[4U] += e;
				state_[5U] += f;
				state_[6U] += g;
				state_[7U] += h;
			}

			std::array<std::uint32_t, 8U> state_{0x6a09e667U,
												 0xbb67ae85U,
												 0x3c6ef372U,
												 0xa54ff53aU,
												 0x510e527fU,
												 0x9b05688cU,
												 0x1f83d9abU,
												 0x5be0cd19U};
			std::array<std::byte, block_bytes> pending_{};
			std::size_t pending_size_{};
			std::uint64_t total_bytes_{};
		};

		struct parent_context
		{
			owned_descriptor descriptor;
			std::string path;
			struct stat status
			{
			};
			sqlite_backend_opaque_identity identity;
		};

#if defined(__linux__)
		struct source_shm_ancestry_watch
		{
			owned_descriptor directory;
			struct stat directory_status
			{
			};
			std::string child_leaf;
			struct stat child_status
			{
			};
			int watch{-1};
		};

		class default_source_shm_namespace_guard final : public sqlite_source_shm_namespace_guard
		{
		  public:
			static result<std::shared_ptr<default_source_shm_namespace_guard>>
			create(std::shared_ptr<const parent_context> parent,
				   const std::string_view logical_main_locator,
				   const std::string_view main_leaf)
			{
				try
				{
					if (!parent || !parent->descriptor || parent->path.empty() ||
						parent->path.front() != '/' || logical_main_locator.empty() ||
						main_leaf.empty() || main_leaf.contains('/') || main_leaf.contains('\0'))
						return unexpected(observation_io_error());
					owned_descriptor event_queue{::inotify_init1(IN_NONBLOCK | IN_CLOEXEC)};
					auto parent_mount_identity =
						make_object_mount_identity(parent->descriptor.get());
					if (!event_queue || !parent_mount_identity)
						return unexpected(observation_io_error());
					constexpr std::uint32_t watch_mask = IN_CREATE | IN_DELETE | IN_MOVED_FROM |
						IN_MOVED_TO | IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_UNMOUNT |
						IN_Q_OVERFLOW | IN_ONLYDIR;
					const auto add_watch = [&](const int descriptor)
					{
						const auto proc_path = "/proc/self/fd/" + std::to_string(descriptor);
						return ::inotify_add_watch(
							event_queue.get(), proc_path.c_str(), watch_mask);
					};

					owned_descriptor current{open_directory("/")};
					if (!current)
						return unexpected(observation_io_error());
					std::vector<source_shm_ancestry_watch> ancestry;
					std::string_view remainder{parent->path};
					while (!remainder.empty() && remainder.front() == '/')
						remainder.remove_prefix(1U);
					while (!remainder.empty())
					{
						const auto separator = remainder.find('/');
						const auto component = remainder.substr(0U, separator);
						if (component.empty() || component == "." || component == "..")
							return unexpected(observation_io_error());
						struct stat current_status
						{
						};
						if (::fstat(current.get(), &current_status) != 0 ||
							!S_ISDIR(current_status.st_mode))
							return unexpected(observation_io_error());
						const auto watch = add_watch(current.get());
						if (watch < 0 ||
							std::ranges::any_of(ancestry,
												[&](const source_shm_ancestry_watch& existing)
												{
													return existing.watch == watch;
												}))
							return unexpected(observation_io_error());
						std::string child_leaf{component};
						owned_descriptor child{open_relative(current.get(),
															 child_leaf.c_str(),
															 O_RDONLY | O_DIRECTORY | O_NONBLOCK |
																 O_NOFOLLOW | O_CLOEXEC)};
						struct stat child_status
						{
						};
						if (!child || ::fstat(child.get(), &child_status) != 0 ||
							!S_ISDIR(child_status.st_mode))
							return unexpected(observation_io_error());
						ancestry.push_back(source_shm_ancestry_watch{std::move(current),
																	 current_status,
																	 std::move(child_leaf),
																	 child_status,
																	 watch});
						current = std::move(child);
						if (separator == std::string_view::npos)
							break;
						remainder.remove_prefix(separator + 1U);
						while (!remainder.empty() && remainder.front() == '/')
							return unexpected(observation_io_error());
					}
					struct stat reached_parent
					{
					};
					if (::fstat(current.get(), &reached_parent) != 0 ||
						!same_object(reached_parent, parent->status))
						return unexpected(observation_io_error());
					const auto parent_watch = add_watch(parent->descriptor.get());
					if (parent_watch < 0 ||
						std::ranges::any_of(ancestry,
											[&](const source_shm_ancestry_watch& existing)
											{
												return existing.watch == parent_watch;
											}))
						return unexpected(observation_io_error());

					auto anchored = "/proc/self/fd/" + std::to_string(parent->descriptor.get()) +
						"/" + std::string{main_leaf};
					return std::shared_ptr<default_source_shm_namespace_guard>(
						new default_source_shm_namespace_guard(std::move(parent),
															   std::move(event_queue),
															   parent_watch,
															   std::move(ancestry),
															   std::move(*parent_mount_identity),
															   std::string{logical_main_locator},
															   std::move(anchored),
															   std::string{main_leaf}));
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<void>
			seal(const std::array<sqlite_backend_entry_observation, 4U>& entries)
			{
				try
				{
					std::scoped_lock lock{mutex_};
					if (sealed_ || finished_)
						return unexpected(observation_io_error());
					entries_ = entries;
					identity_.profile =
						"default-filesystem-v1.source-shm-continuous-namespace-guard.v1";
					append_bytes(identity_.bytes, logical_main_locator_);
					append_bytes(identity_.bytes, anchored_main_locator_);
					append_bytes(identity_.bytes, parent_->identity.profile);
					identity_.bytes.insert(identity_.bytes.end(),
										   parent_->identity.bytes.begin(),
										   parent_->identity.bytes.end());
					append_bytes(identity_.bytes, parent_mount_identity_.profile);
					identity_.bytes.insert(identity_.bytes.end(),
										   parent_mount_identity_.bytes.begin(),
										   parent_mount_identity_.bytes.end());
					for (const auto& entry : entries_)
					{
						append_u64(identity_.bytes, static_cast<std::uint64_t>(entry.role));
						append_u64(identity_.bytes, static_cast<std::uint64_t>(entry.state));
						const auto active_role =
							entry.role != sqlite_backend_file_role::rollback_journal;
						const auto mount = entry.held_object
							? entry.held_object->object_mount_identity()
							: std::optional<sqlite_backend_opaque_identity>{};
						if (active_role &&
							(!entry.object_filesystem_profile || !mount ||
							 *mount != parent_mount_identity_))
							return unexpected(observation_io_error());
						if (entry.object_identity)
						{
							append_bytes(identity_.bytes, entry.object_identity->profile);
							identity_.bytes.insert(identity_.bytes.end(),
												   entry.object_identity->bytes.begin(),
												   entry.object_identity->bytes.end());
						}
						if (entry.directory_entry_identity)
						{
							append_bytes(identity_.bytes, entry.directory_entry_identity->profile);
							identity_.bytes.insert(identity_.bytes.end(),
												   entry.directory_entry_identity->bytes.begin(),
												   entry.directory_entry_identity->bytes.end());
						}
						if (entry.object_filesystem_profile)
						{
							append_bytes(identity_.bytes, entry.object_filesystem_profile->profile);
							identity_.bytes.insert(identity_.bytes.end(),
												   entry.object_filesystem_profile->bytes.begin(),
												   entry.object_filesystem_profile->bytes.end());
						}
						if (mount)
						{
							append_bytes(identity_.bytes, mount->profile);
							identity_.bytes.insert(
								identity_.bytes.end(), mount->bytes.begin(), mount->bytes.end());
						}
					}
					sealed_ = true;
					return recheck_locked();
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

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
				for (const auto& entry : entries_)
					if (entry.role == role)
						return entry;
				return unexpected(observation_io_error());
			}
			[[nodiscard]] result<void> recheck() const override
			{
				std::scoped_lock lock{mutex_};
				return recheck_locked();
			}
			[[nodiscard]] result<void> claim_target_epoch() override
			{
				std::scoped_lock lock{mutex_};
				if (claimed_)
					return unexpected(observation_io_error());
				claimed_ = true;
				if (auto checked = recheck_locked(); !checked)
					return checked;
				return {};
			}
			[[nodiscard]] result<void> finish() override
			{
				std::scoped_lock lock{mutex_};
				if (!claimed_)
					return unexpected(observation_io_error());
				if (auto checked = recheck_locked(); !checked)
					return checked;
				event_queue_ = owned_descriptor{};
				finished_ = true;
				return {};
			}

		  private:
			default_source_shm_namespace_guard(std::shared_ptr<const parent_context> parent,
											   owned_descriptor event_queue,
											   const int parent_watch,
											   std::vector<source_shm_ancestry_watch> ancestry,
											   sqlite_backend_opaque_identity parent_mount_identity,
											   std::string logical_main_locator,
											   std::string anchored_main_locator,
											   std::string main_leaf)
				: parent_{std::move(parent)}, event_queue_{std::move(event_queue)},
				  parent_watch_{parent_watch}, ancestry_{std::move(ancestry)},
				  parent_mount_identity_{std::move(parent_mount_identity)},
				  logical_main_locator_{std::move(logical_main_locator)},
				  anchored_main_locator_{std::move(anchored_main_locator)},
				  main_leaf_{std::move(main_leaf)}
			{
			}

			[[nodiscard]] bool source_family_leaf(const std::string_view value) const noexcept
			{
				const auto exact_suffix = [&](const std::string_view suffix) noexcept
				{
					return value.size() == main_leaf_.size() + suffix.size() &&
						value.starts_with(main_leaf_) && value.ends_with(suffix);
				};
				return value == main_leaf_ || exact_suffix(wal_suffix) ||
					exact_suffix(shm_suffix) || exact_suffix(journal_suffix);
			}

			[[nodiscard]] result<void> require_no_relevant_events_locked() const
			{
				std::array<std::byte, 4096U> buffer{};
				for (;;)
				{
					ssize_t count{};
					do
					{
						count = ::read(event_queue_.get(), buffer.data(), buffer.size());
					} while (count < 0 && errno == EINTR);
					if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
						return {};
					if (count <= 0)
						return unexpected(observation_io_error());
					std::size_t offset{};
					while (offset < static_cast<std::size_t>(count))
					{
						if (static_cast<std::size_t>(count) - offset < sizeof(inotify_event))
							return unexpected(observation_io_error());
						const auto* event =
							reinterpret_cast<const inotify_event*>(buffer.data() + offset);
						const auto event_size = sizeof(inotify_event) + event->len;
						if (event_size > static_cast<std::size_t>(count) - offset)
							return unexpected(observation_io_error());
						constexpr std::uint32_t unconditional =
							IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED | IN_UNMOUNT | IN_Q_OVERFLOW;
						if ((event->mask & unconditional) != 0U)
							return unexpected(observation_io_error());
						const auto name_size = event->len == 0U
							? 0U
							: ::strnlen(event->name, static_cast<std::size_t>(event->len));
						const std::string_view name{event->name, name_size};
						bool relevant = event->wd == parent_watch_ &&
							(event->len == 0U || source_family_leaf(name));
						for (const auto& ancestor : ancestry_)
							if (event->wd == ancestor.watch)
							{
								relevant = event->len == 0U || name == ancestor.child_leaf;
								break;
							}
						if (relevant)
							return unexpected(observation_io_error());
						offset += event_size;
					}
				}
			}

			[[nodiscard]] result<void> recheck_locked() const
			{
				if (!sealed_ || finished_ || !event_queue_ || !parent_ ||
					!require_no_relevant_events_locked())
					return unexpected(observation_io_error());
				struct stat current_parent
				{
				};
				const auto current_parent_mount =
					make_object_mount_identity(parent_->descriptor.get());
				if (::fstat(parent_->descriptor.get(), &current_parent) != 0 ||
					!same_object(parent_->status, current_parent) || !current_parent_mount ||
					*current_parent_mount != parent_mount_identity_ ||
					make_stat_identity("default-filesystem-v1.parent-namespace.v1",
									   current_parent) != parent_->identity)
					return unexpected(observation_io_error());
				for (const auto& ancestor : ancestry_)
				{
					struct stat current_directory
					{
					};
					struct stat current_child
					{
					};
					if (::fstat(ancestor.directory.get(), &current_directory) != 0 ||
						!same_object(current_directory, ancestor.directory_status) ||
						stat_relative(ancestor.directory.get(),
									  ancestor.child_leaf.c_str(),
									  current_child,
									  AT_SYMLINK_NOFOLLOW) != 0 ||
						!S_ISDIR(current_child.st_mode) ||
						!same_object(current_child, ancestor.child_status))
						return unexpected(observation_io_error());
				}
				for (const auto& expected : entries_)
				{
					std::string leaf = main_leaf_;
					if (expected.role == sqlite_backend_file_role::write_ahead_log)
						leaf.append(wal_suffix);
					else if (expected.role == sqlite_backend_file_role::shared_memory)
						leaf.append(shm_suffix);
					else if (expected.role == sqlite_backend_file_role::rollback_journal)
						leaf.append(journal_suffix);
					struct stat namespace_entry
					{
					};
					if (expected.state == sqlite_backend_entry_state::absent)
					{
						if (stat_relative(parent_->descriptor.get(),
										  leaf.c_str(),
										  namespace_entry,
										  AT_SYMLINK_NOFOLLOW) == 0 ||
							errno != ENOENT)
							return unexpected(observation_io_error());
						continue;
					}
					struct stat resolved
					{
					};
					owned_descriptor current_object{open_relative(
						parent_->descriptor.get(), leaf.c_str(), O_PATH | O_NOFOLLOW | O_CLOEXEC)};
					const auto current_stat_ok =
						current_object && ::fstat(current_object.get(), &resolved) == 0;
					const auto current_filesystem = current_stat_ok
						? make_object_filesystem_profile(current_object.get(), resolved)
						: std::optional<sqlite_backend_opaque_identity>{};
					const auto current_mount = current_stat_ok
						? make_object_mount_identity(current_object.get())
						: std::optional<sqlite_backend_opaque_identity>{};
					if (expected.state != sqlite_backend_entry_state::held_regular ||
						!expected.direct_regular_entry || !expected.object_identity ||
						!expected.directory_entry_identity || !expected.object_filesystem_profile ||
						!expected.held_object || !expected.held_object->object_mount_identity() ||
						!current_stat_ok ||
						stat_relative(parent_->descriptor.get(),
									  leaf.c_str(),
									  namespace_entry,
									  AT_SYMLINK_NOFOLLOW) != 0 ||
						!S_ISREG(namespace_entry.st_mode) || !S_ISREG(resolved.st_mode) ||
						!same_object(namespace_entry, resolved) ||
						make_stat_identity("default-filesystem-v1.open-object.v1", resolved) !=
							*expected.object_identity ||
						make_entry_identity(parent_->status,
											leaf,
											namespace_entry,
											std::optional<struct stat>{resolved}) !=
							*expected.directory_entry_identity ||
						!current_filesystem ||
						*current_filesystem != *expected.object_filesystem_profile ||
						!current_mount ||
						*current_mount != *expected.held_object->object_mount_identity() ||
						!expected.held_object->recheck_retained_object())
						return unexpected(observation_io_error());
				}
				if (!require_no_relevant_events_locked())
					return unexpected(observation_io_error());
				return {};
			}

			std::shared_ptr<const parent_context> parent_;
			mutable owned_descriptor event_queue_;
			int parent_watch_{-1};
			std::vector<source_shm_ancestry_watch> ancestry_;
			sqlite_backend_opaque_identity parent_mount_identity_;
			std::string logical_main_locator_;
			std::string anchored_main_locator_;
			std::string main_leaf_;
			std::array<sqlite_backend_entry_observation, 4U> entries_;
			sqlite_backend_opaque_identity identity_;
			mutable std::mutex mutex_;
			bool sealed_{};
			bool claimed_{};
			bool finished_{};
		};
#endif

		[[nodiscard]] result<std::shared_ptr<const parent_context>>
		open_parent_context(const std::string& path)
		{
			const auto raw = open_directory(path.c_str());
			if (raw < 0)
				return unexpected(observation_io_error());
			owned_descriptor descriptor{raw};
			struct stat observed
			{
			};
			if (::fstat(descriptor.get(), &observed) != 0 || !S_ISDIR(observed.st_mode))
				return unexpected(observation_io_error());
			try
			{
				return std::make_shared<const parent_context>(parent_context{
					std::move(descriptor),
					path,
					observed,
					make_stat_identity("default-filesystem-v1.parent-namespace.v1", observed),
				});
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(observation_allocation_error());
			}
			catch (const std::length_error&)
			{
				return unexpected(observation_allocation_error());
			}
		}

		class default_held_object final : public sqlite_backend_held_object
		{
		  public:
			default_held_object(sqlite_backend_file_role role,
								owned_descriptor object,
								std::shared_ptr<const parent_context> parent,
								std::string leaf,
								sqlite_backend_opaque_identity object_identity,
								sqlite_backend_opaque_identity entry_identity,
								sqlite_backend_opaque_identity object_filesystem_profile,
								std::optional<sqlite_backend_opaque_identity> object_mount_identity)
				: role_{role}, object_{std::move(object)}, parent_{std::move(parent)},
				  leaf_{std::move(leaf)}, object_identity_{std::move(object_identity)},
				  entry_identity_{std::move(entry_identity)},
				  object_filesystem_profile_{std::move(object_filesystem_profile)},
				  object_mount_identity_{std::move(object_mount_identity)}
			{
			}

			[[nodiscard]] sqlite_backend_file_role role() const noexcept override
			{
				return role_;
			}
			[[nodiscard]] const sqlite_backend_opaque_identity&
			object_identity() const noexcept override
			{
				return object_identity_;
			}
			[[nodiscard]] const sqlite_backend_opaque_identity&
			directory_entry_identity() const noexcept override
			{
				return entry_identity_;
			}
			[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
			object_filesystem_profile() const noexcept override
			{
				return object_filesystem_profile_;
			}
			[[nodiscard]] const std::optional<sqlite_backend_opaque_identity>&
			object_mount_identity() const noexcept override
			{
				return object_mount_identity_;
			}
			[[nodiscard]] result<void> recheck_retained_object() const override
			{
				struct stat held
				{
				};
				if (::fstat(object_.get(), &held) != 0 || !S_ISREG(held.st_mode))
					return unexpected(observation_io_error());
				try
				{
					auto filesystem = make_object_filesystem_profile(object_.get(), held);
					auto mount = make_object_mount_identity(object_.get());
					if (make_stat_identity("default-filesystem-v1.open-object.v1", held) !=
							object_identity_ ||
						!filesystem || !object_filesystem_profile_ ||
						*filesystem != *object_filesystem_profile_ ||
						(object_mount_identity_ && (!mount || *mount != *object_mount_identity_)))
						return unexpected(observation_io_error());
					return {};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<std::uint64_t> size() const override
			{
				struct stat observed
				{
				};
				if (::fstat(object_.get(), &observed) != 0 || observed.st_size < 0 ||
					!S_ISREG(observed.st_mode))
					return unexpected(observation_io_error());
				try
				{
					if (make_stat_identity("default-filesystem-v1.open-object.v1", observed) !=
						object_identity_)
						return unexpected(observation_io_error());
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				return static_cast<std::uint64_t>(observed.st_size);
			}

			[[nodiscard]] result<void> read_exact(const std::uint64_t offset,
												  const std::span<std::byte> output) const override
			{
				const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
				if (offset > maximum || output.size() > maximum - offset)
					return unexpected(observation_io_error());
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
					return unexpected(observation_io_error());
				}
				return {};
			}

			[[nodiscard]] result<std::string> sha256() const override
			{
				struct stat before
				{
				};
				if (::fstat(object_.get(), &before) != 0)
					return unexpected(observation_io_error());
				const auto before_epoch = epoch_from_stat(before);
				if (!before_epoch)
					return unexpected(observation_io_error());
				try
				{
					incremental_sha256 digest;
					std::array<std::byte, digest_buffer_bytes> buffer{};
					std::uint64_t offset{};
					while (offset < before_epoch->size)
					{
						const auto count = static_cast<std::size_t>(
							std::min<std::uint64_t>(buffer.size(), before_epoch->size - offset));
						const auto window = std::span{buffer}.first(count);
						if (auto read = read_exact(offset, window); !read)
							return unexpected(std::move(read.error()));
						digest.update(window);
						offset += count;
					}
					struct stat after
					{
					};
					if (::fstat(object_.get(), &after) != 0 ||
						epoch_from_stat(after) != before_epoch)
						return unexpected(observation_io_error());
					return digest.finish();
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot>>
			copy_exact(sqlite_backend_private_snapshot_builder& builder,
					   const std::span<std::byte> scratch) const override
			{
				if (scratch.empty())
					return unexpected(
						error{"store.backend-unavailable", "sqlite", "vfs-observation-buffer"});
				auto replacement = recheck_current_entry();
				if (!replacement)
					return unexpected(std::move(replacement.error()));
				if (*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object)
					return unexpected(observation_io_error());
				struct stat before
				{
				};
				if (::fstat(object_.get(), &before) != 0)
					return unexpected(observation_io_error());
				const auto before_epoch = epoch_from_stat(before);
				if (!before_epoch)
					return unexpected(observation_io_error());
				try
				{
					incremental_sha256 digest;
					std::uint64_t offset{};
					while (offset < before_epoch->size)
					{
						const auto count = static_cast<std::size_t>(
							std::min<std::uint64_t>(scratch.size(), before_epoch->size - offset));
						auto window = scratch.first(count);
						if (auto read = read_exact(offset, window); !read)
							return unexpected(std::move(read.error()));
						digest.update(window);
						if (auto appended = builder.append(window); !appended)
							return unexpected(std::move(appended.error()));
						offset += count;
					}
					struct stat after
					{
					};
					if (::fstat(object_.get(), &after) != 0 ||
						epoch_from_stat(after) != before_epoch)
						return unexpected(observation_io_error());
					replacement = recheck_current_entry();
					if (!replacement)
						return unexpected(std::move(replacement.error()));
					if (*replacement !=
						sqlite_backend_replacement_state::exact_same_entry_and_object)
						return unexpected(observation_io_error());
					return builder.seal(before_epoch->size, digest.finish());
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<sqlite_backend_replacement_state>
			recheck_current_entry() const override;

		  private:
			sqlite_backend_file_role role_;
			owned_descriptor object_;
			std::shared_ptr<const parent_context> parent_;
			std::string leaf_;
			sqlite_backend_opaque_identity object_identity_;
			sqlite_backend_opaque_identity entry_identity_;
			std::optional<sqlite_backend_opaque_identity> object_filesystem_profile_;
			std::optional<sqlite_backend_opaque_identity> object_mount_identity_;
		};

		[[nodiscard]] result<sqlite_backend_entry_observation>
		observe_entry(const std::shared_ptr<const parent_context>& parent,
					  const std::string_view leaf_view,
					  const sqlite_backend_file_role role)
		{
			try
			{
				const std::string leaf{leaf_view};
				struct stat namespace_entry
				{
				};
				if (stat_relative(parent->descriptor.get(),
								  leaf.c_str(),
								  namespace_entry,
								  AT_SYMLINK_NOFOLLOW) != 0)
				{
					if (errno == ENOENT)
						return sqlite_backend_entry_observation{
							role, sqlite_backend_entry_state::absent, {}, {}, {}, {}, false};
					return unexpected(observation_io_error());
				}

				std::optional<struct stat> resolved;
				struct stat resolved_value
				{
				};
				if (stat_relative(parent->descriptor.get(), leaf.c_str(), resolved_value, 0) != 0)
				{
					if (errno == EACCES || errno == EPERM || errno == ENOENT || errno == ELOOP)
						return sqlite_backend_entry_observation{
							role,
							sqlite_backend_entry_state::present_unreadable,
							{},
							make_entry_identity(parent->status, leaf, namespace_entry, {}),
							{},
							{},
							false,
						};
					return unexpected(observation_io_error());
				}
				resolved.emplace(resolved_value);
				auto entry_identity =
					make_entry_identity(parent->status, leaf, namespace_entry, resolved);
				if (!S_ISREG(resolved_value.st_mode))
					return sqlite_backend_entry_observation{
						role,
						sqlite_backend_entry_state::unsupported_kind,
						{},
						std::move(entry_identity),
						{},
						{},
						false,
					};

				const auto raw = open_relative(
					parent->descriptor.get(), leaf.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
				if (raw < 0)
				{
					if (errno != EACCES && errno != EPERM)
						return unexpected(observation_io_error());
					struct stat current_namespace
					{
					};
					struct stat current_resolved
					{
					};
					if (stat_relative(parent->descriptor.get(),
									  leaf.c_str(),
									  current_namespace,
									  AT_SYMLINK_NOFOLLOW) != 0 ||
						stat_relative(
							parent->descriptor.get(), leaf.c_str(), current_resolved, 0) != 0 ||
						!same_object(namespace_entry, current_namespace) ||
						!same_object(resolved_value, current_resolved))
						return unexpected(observation_io_error());
					return sqlite_backend_entry_observation{
						role,
						sqlite_backend_entry_state::present_unreadable,
						{},
						std::move(entry_identity),
						{},
						{},
						false,
					};
				}
				owned_descriptor object{raw};
				struct stat held
				{
				};
				struct stat current_namespace
				{
				};
				struct stat current_resolved
				{
				};
				if (::fstat(object.get(), &held) != 0 || !S_ISREG(held.st_mode) ||
					!same_object(held, resolved_value) ||
					stat_relative(parent->descriptor.get(),
								  leaf.c_str(),
								  current_namespace,
								  AT_SYMLINK_NOFOLLOW) != 0 ||
					stat_relative(parent->descriptor.get(), leaf.c_str(), current_resolved, 0) !=
						0 ||
					!same_object(namespace_entry, current_namespace) ||
					!same_object(resolved_value, current_resolved))
					return unexpected(observation_io_error());
				auto object_identity =
					make_stat_identity("default-filesystem-v1.open-object.v1", held);
				auto object_filesystem_profile = make_object_filesystem_profile(object.get(), held);
				auto object_mount_identity = make_object_mount_identity(object.get());
				if (!object_filesystem_profile)
					return unexpected(observation_io_error());
				auto held_object =
					std::make_shared<default_held_object>(role,
														  std::move(object),
														  parent,
														  leaf,
														  object_identity,
														  entry_identity,
														  *object_filesystem_profile,
														  std::move(object_mount_identity));
				return sqlite_backend_entry_observation{
					role,
					sqlite_backend_entry_state::held_regular,
					std::move(object_identity),
					std::move(entry_identity),
					std::move(held_object),
					std::move(object_filesystem_profile),
					S_ISREG(namespace_entry.st_mode) &&
						same_object(namespace_entry, resolved_value),
				};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(observation_allocation_error());
			}
			catch (const std::length_error&)
			{
				return unexpected(observation_allocation_error());
			}
		}

		[[nodiscard]] result<sqlite_default_entry_state_observation>
		observe_entry_state_from_parent_without_open(const parent_context& parent,
													 const std::string_view leaf_view,
													 const sqlite_backend_file_role role)
		{
			try
			{
				const std::string leaf{leaf_view};
				struct stat namespace_before
				{
				};
				if (stat_relative(parent.descriptor.get(),
								  leaf.c_str(),
								  namespace_before,
								  AT_SYMLINK_NOFOLLOW) != 0)
				{
					const auto first_error = errno;
					struct stat namespace_retry
					{
					};
					if (stat_relative(parent.descriptor.get(),
									  leaf.c_str(),
									  namespace_retry,
									  AT_SYMLINK_NOFOLLOW) == 0 ||
						errno != first_error)
						return unexpected(observation_io_error());
					if (first_error == ENOENT)
						return sqlite_default_entry_state_observation{
							role,
							sqlite_backend_entry_state::absent,
							parent.identity,
							{},
							{},
						};
					if (first_error == EACCES || first_error == EPERM)
						return sqlite_default_entry_state_observation{
							role,
							sqlite_backend_entry_state::present_unreadable,
							parent.identity,
							{},
							{},
						};
					return unexpected(observation_io_error());
				}

				struct stat object_before
				{
				};
				struct stat namespace_after
				{
				};
				struct stat object_after
				{
				};
				if (stat_relative(parent.descriptor.get(), leaf.c_str(), object_before, 0) != 0)
				{
					const auto first_error = errno;
					if (stat_relative(parent.descriptor.get(),
									  leaf.c_str(),
									  namespace_after,
									  AT_SYMLINK_NOFOLLOW) != 0 ||
						!same_object(namespace_before, namespace_after))
						return unexpected(observation_io_error());
					struct stat object_retry
					{
					};
					if (stat_relative(parent.descriptor.get(), leaf.c_str(), object_retry, 0) ==
							0 ||
						errno != first_error)
						return unexpected(observation_io_error());
					if (first_error != EACCES && first_error != EPERM && first_error != ENOENT &&
						first_error != ELOOP)
						return unexpected(observation_io_error());
					return sqlite_default_entry_state_observation{
						role,
						sqlite_backend_entry_state::present_unreadable,
						parent.identity,
						{},
						make_entry_identity(parent.status, leaf, namespace_after, {}),
					};
				}

				if (stat_relative(parent.descriptor.get(),
								  leaf.c_str(),
								  namespace_after,
								  AT_SYMLINK_NOFOLLOW) != 0 ||
					stat_relative(parent.descriptor.get(), leaf.c_str(), object_after, 0) != 0 ||
					!same_object(namespace_before, namespace_after) ||
					!same_object(object_before, object_after))
					return unexpected(observation_io_error());
				auto entry_identity = make_entry_identity(
					parent.status, leaf, namespace_after, std::optional<struct stat>{object_after});
				if (!S_ISREG(object_after.st_mode))
					return sqlite_default_entry_state_observation{
						role,
						sqlite_backend_entry_state::unsupported_kind,
						parent.identity,
						{},
						std::move(entry_identity),
					};
				return sqlite_default_entry_state_observation{
					role,
					sqlite_backend_entry_state::held_regular,
					parent.identity,
					make_stat_identity("default-filesystem-v1.open-object.v1", object_after),
					std::move(entry_identity),
				};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(observation_allocation_error());
			}
			catch (const std::length_error&)
			{
				return unexpected(observation_allocation_error());
			}
		}

		[[nodiscard]] result<sqlite_default_entry_identity_observation>
		observe_entry_identity_without_open(const parent_context& parent,
											const std::string_view leaf_view,
											const sqlite_backend_file_role role)
		{
			auto observed = observe_entry_state_from_parent_without_open(parent, leaf_view, role);
			if (!observed || observed->state != sqlite_backend_entry_state::held_regular ||
				!observed->object_identity || !observed->directory_entry_identity)
				return unexpected(observed ? observation_io_error() : std::move(observed.error()));
			return sqlite_default_entry_identity_observation{
				role,
				std::move(*observed->object_identity),
				std::move(*observed->directory_entry_identity),
			};
		}

		result<sqlite_backend_replacement_state> default_held_object::recheck_current_entry() const
		{
			const auto raw_parent = open_directory(parent_->path.c_str());
			if (raw_parent < 0)
			{
				if (errno == ENOENT || errno == ENOTDIR)
					return sqlite_backend_replacement_state::absent;
				if (errno == EACCES || errno == EPERM)
					return sqlite_backend_replacement_state::unreadable;
				return unexpected(observation_io_error());
			}
			owned_descriptor current_parent_descriptor{raw_parent};
			struct stat current_parent_status
			{
			};
			if (::fstat(current_parent_descriptor.get(), &current_parent_status) != 0 ||
				!S_ISDIR(current_parent_status.st_mode))
				return unexpected(observation_io_error());
			if (!same_object(parent_->status, current_parent_status))
				return sqlite_backend_replacement_state::replaced;
			try
			{
				struct stat namespace_entry
				{
				};
				if (stat_relative(current_parent_descriptor.get(),
								  leaf_.c_str(),
								  namespace_entry,
								  AT_SYMLINK_NOFOLLOW) != 0)
				{
					if (errno == ENOENT)
						return sqlite_backend_replacement_state::absent;
					if (errno == EACCES || errno == EPERM)
						return sqlite_backend_replacement_state::unreadable;
					return unexpected(observation_io_error());
				}
				struct stat resolved
				{
				};
				if (stat_relative(current_parent_descriptor.get(), leaf_.c_str(), resolved, 0) != 0)
				{
					if (errno == EACCES || errno == EPERM)
						return sqlite_backend_replacement_state::unreadable;
					return sqlite_backend_replacement_state::replaced;
				}
				if (!S_ISREG(resolved.st_mode))
					return sqlite_backend_replacement_state::unsupported_kind;
				struct stat namespace_after
				{
				};
				struct stat resolved_after
				{
				};
				if (stat_relative(current_parent_descriptor.get(),
								  leaf_.c_str(),
								  namespace_after,
								  AT_SYMLINK_NOFOLLOW) != 0 ||
					stat_relative(
						current_parent_descriptor.get(), leaf_.c_str(), resolved_after, 0) != 0 ||
					!same_object(namespace_entry, namespace_after) ||
					!same_object(resolved, resolved_after))
					return sqlite_backend_replacement_state::replaced;
				struct stat held
				{
				};
				if (::fstat(object_.get(), &held) != 0 || !S_ISREG(held.st_mode))
					return unexpected(observation_io_error());
				const auto current_object =
					make_stat_identity("default-filesystem-v1.open-object.v1", resolved_after);
				const auto held_object =
					make_stat_identity("default-filesystem-v1.open-object.v1", held);
				const auto current_entry =
					make_entry_identity(current_parent_status,
										leaf_,
										namespace_after,
										std::optional<struct stat>{resolved_after});
				return current_object == object_identity_ && held_object == object_identity_ &&
						current_entry == entry_identity_
					? sqlite_backend_replacement_state::exact_same_entry_and_object
					: sqlite_backend_replacement_state::replaced;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(observation_allocation_error());
			}
			catch (const std::length_error&)
			{
				return unexpected(observation_allocation_error());
			}
			return unexpected(observation_io_error());
		}

		[[nodiscard]] bool same_entry_observation(const sqlite_backend_entry_observation& left,
												  const sqlite_backend_entry_observation& right)
		{
			return left.role == right.role && left.state == right.state &&
				left.object_identity == right.object_identity &&
				left.directory_entry_identity == right.directory_entry_identity &&
				left.object_filesystem_profile == right.object_filesystem_profile &&
				left.direct_regular_entry == right.direct_regular_entry;
		}

		class default_observation_capability final : public sqlite_backend_observation_capability
		{
		  public:
			explicit default_observation_capability(
				sqlite_default_observation_binding binding,
				locator_parts locator,
				sqlite_backend_opaque_identity capability_token,
				std::shared_ptr<sqlite_source_shm_readonly_port> source_shm_readonly_port)
				: canonical_locator_{std::move(binding.canonical_vfs_locator)},
				  locator_{std::move(locator)},
				  registered_vfs_name_{std::move(binding.registered_vfs_name)},
				  forwarding_vfs_identity_{binding.forwarding_vfs_identity},
				  pinned_underlying_vfs_app_data_identity_{
					  binding.pinned_underlying_vfs_app_data_identity},
				  backend_lifetime_{std::move(binding.backend_lifetime)},
				  private_snapshot_registry_{std::move(binding.private_snapshot_registry)},
				  connection_observation_port_{std::move(binding.connection_observation_port)},
				  capability_token_{std::move(capability_token)},
				  source_shm_readonly_port_{std::move(source_shm_readonly_port)}
			{
			}

			[[nodiscard]] sqlite_backend_vfs_binding binding() const noexcept override
			{
				return sqlite_backend_vfs_binding{
					observation_profile,
					registered_vfs_name_,
					forwarding_vfs_identity_,
					backend_lifetime_.get(),
					this,
					private_snapshot_registry_.runtime_identity,
					private_snapshot_registry_.runtime_lifetime.get(),
					private_snapshot_registry_.pinned_default_vfs,
					pinned_underlying_vfs_app_data_identity_,
				};
			}

			[[nodiscard]] const sqlite_backend_opaque_identity&
			capability_token() const noexcept override
			{
				return capability_token_;
			}

			[[nodiscard]] sqlite_source_shm_readonly_port*
			source_shm_readonly_port() noexcept override
			{
				return source_shm_readonly_port_.get();
			}

			[[nodiscard]] result<sqlite_default_entry_identity_observation>
			observe_entry_identity_no_open(const std::string_view canonical_vfs_locator,
										   const sqlite_backend_file_role role) const
			{
				if (canonical_vfs_locator != canonical_locator_)
					return unexpected(locator_binding_error());
				try
				{
					std::string leaf{locator_.leaf};
					switch (role)
					{
						case sqlite_backend_file_role::main_database:
							break;
						case sqlite_backend_file_role::write_ahead_log:
							leaf.append(wal_suffix);
							break;
						case sqlite_backend_file_role::shared_memory:
							leaf.append(shm_suffix);
							break;
						case sqlite_backend_file_role::rollback_journal:
							leaf.append(journal_suffix);
							break;
					}
					auto parent = open_parent_context(locator_.parent);
					if (!parent)
						return unexpected(std::move(parent.error()));
					return observe_entry_identity_without_open(**parent, leaf, role);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<sqlite_default_entry_state_observation>
			observe_entry_state_without_open(const std::string_view canonical_vfs_locator,
											 const sqlite_backend_file_role role) const override
			{
				if (canonical_vfs_locator != canonical_locator_)
					return unexpected(locator_binding_error());
				try
				{
					std::string leaf{locator_.leaf};
					switch (role)
					{
						case sqlite_backend_file_role::main_database:
							break;
						case sqlite_backend_file_role::write_ahead_log:
							leaf.append(wal_suffix);
							break;
						case sqlite_backend_file_role::shared_memory:
							leaf.append(shm_suffix);
							break;
						case sqlite_backend_file_role::rollback_journal:
							leaf.append(journal_suffix);
							break;
					}
					auto parent = open_parent_context(locator_.parent);
					if (!parent)
						return unexpected(std::move(parent.error()));
					return observe_entry_state_from_parent_without_open(**parent, leaf, role);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<sqlite_backend_namespace_census>
			capture_namespace(const std::string_view canonical_vfs_locator) const override
			{
				if (canonical_vfs_locator != canonical_locator_)
					return unexpected(locator_binding_error());
				auto parent = open_parent_context(locator_.parent);
				if (!parent)
					return unexpected(std::move(parent.error()));
				try
				{
#if defined(__linux__)
					std::shared_ptr<default_source_shm_namespace_guard> source_shm_guard;
					if (auto created_guard = default_source_shm_namespace_guard::create(
							*parent, canonical_locator_, locator_.leaf);
						created_guard)
						source_shm_guard = std::move(*created_guard);
#endif
					const std::array<std::pair<sqlite_backend_file_role, std::string>, 4U> names{
						std::pair{sqlite_backend_file_role::main_database, locator_.leaf},
						std::pair{sqlite_backend_file_role::write_ahead_log,
								  locator_.leaf + std::string{wal_suffix}},
						std::pair{sqlite_backend_file_role::shared_memory,
								  locator_.leaf + std::string{shm_suffix}},
						std::pair{sqlite_backend_file_role::rollback_journal,
								  locator_.leaf + std::string{journal_suffix}},
					};
					std::array<sqlite_backend_entry_observation, 4U> entries;
					for (std::size_t index{}; index < names.size(); ++index)
					{
						auto observed =
							observe_entry(*parent, names.at(index).second, names.at(index).first);
						if (!observed)
							return unexpected(std::move(observed.error()));
						entries.at(index) = std::move(*observed);
					}
#if defined(__linux__)
					const auto guardable_source =
						entries[0U].state == sqlite_backend_entry_state::held_regular &&
						entries[0U].direct_regular_entry && entries[0U].held_object &&
						entries[0U].held_object->object_mount_identity() &&
						entries[1U].state == sqlite_backend_entry_state::held_regular &&
						entries[1U].direct_regular_entry && entries[1U].held_object &&
						entries[1U].held_object->object_mount_identity() &&
						entries[2U].state == sqlite_backend_entry_state::held_regular &&
						entries[2U].direct_regular_entry && entries[2U].held_object &&
						entries[2U].held_object->object_mount_identity() &&
						entries[3U].state == sqlite_backend_entry_state::absent;
					if (guardable_source && source_shm_guard)
					{
						if (auto sealed = source_shm_guard->seal(entries); !sealed)
							source_shm_guard.reset();
					}
#endif
					return sqlite_backend_namespace_census{
						std::string{observation_profile},
						capability_token_,
						(*parent)->identity,
						std::move(entries),
#if defined(__linux__)
						guardable_source ? std::move(source_shm_guard) : nullptr,
#else
						{},
#endif
					};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<bool>
			recheck_namespace(const sqlite_backend_namespace_census& prior,
							  const std::string_view canonical_vfs_locator) const override
			{
				if (canonical_vfs_locator != canonical_locator_ ||
					prior.profile != observation_profile ||
					prior.capability_token != capability_token_ ||
					(prior.source_shm_guard && !prior.source_shm_guard->recheck()))
					return unexpected(observation_unavailable_error());
				auto current = capture_namespace(canonical_vfs_locator);
				if (!current)
					return unexpected(std::move(current.error()));
				if (prior.parent_namespace_identity != current->parent_namespace_identity)
					return false;
				for (std::size_t index{}; index < prior.entries.size(); ++index)
					if (!same_entry_observation(prior.entries.at(index),
												current->entries.at(index)))
						return false;
				return true;
			}

			[[nodiscard]] result<sqlite_backend_zero_main_receipt>
			exclusive_create_sync_zero_main(const std::string_view canonical_vfs_locator) override
			{
				if (canonical_vfs_locator != canonical_locator_)
					return unexpected(locator_binding_error());
				auto before = capture_namespace(canonical_vfs_locator);
				if (!before)
					return unexpected(std::move(before.error()));
				if (std::ranges::any_of(before->entries,
										[](const sqlite_backend_entry_observation& value)
										{
											return value.state !=
												sqlite_backend_entry_state::absent;
										}))
					return unexpected(concurrent_authority_error());
				auto stable = recheck_namespace(*before, canonical_vfs_locator);
				if (!stable)
					return unexpected(std::move(stable.error()));
				if (!*stable)
					return unexpected(concurrent_authority_error());
				auto parent = open_parent_context(locator_.parent);
				if (!parent)
					return unexpected(std::move(parent.error()));
				if ((*parent)->identity != before->parent_namespace_identity)
					return unexpected(concurrent_authority_error());

				const auto raw = open_relative((*parent)->descriptor.get(),
											   locator_.leaf.c_str(),
											   O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC,
											   static_cast<mode_t>(0644));
				if (raw < 0)
				{
					if (errno == EEXIST || errno == ENOENT || errno == ENOTDIR)
						return unexpected(concurrent_authority_error());
					return unexpected(raw_create_error(errno));
				}
				owned_descriptor created{raw};
				struct stat held_status
				{
				};
				if (::fstat(created.get(), &held_status) != 0 || !S_ISREG(held_status.st_mode) ||
					held_status.st_size != 0)
					return unexpected(object_kind_error());
				if (::fdatasync(created.get()) != 0 || ::fsync((*parent)->descriptor.get()) != 0)
					return unexpected(bootstrap_durability_error());

				try
				{
					struct stat namespace_entry
					{
					};
					struct stat resolved
					{
					};
					if (stat_relative((*parent)->descriptor.get(),
									  locator_.leaf.c_str(),
									  namespace_entry,
									  AT_SYMLINK_NOFOLLOW) != 0 ||
						stat_relative(
							(*parent)->descriptor.get(), locator_.leaf.c_str(), resolved, 0) != 0 ||
						!same_object(held_status, namespace_entry) ||
						!same_object(held_status, resolved))
						return unexpected(bootstrap_durability_error());
					auto object_identity =
						make_stat_identity("default-filesystem-v1.open-object.v1", held_status);
					auto entry_identity = make_entry_identity(
						(*parent)->status, locator_.leaf, namespace_entry, resolved);
					auto object_filesystem_profile =
						make_object_filesystem_profile(created.get(), held_status);
					auto object_mount_identity = make_object_mount_identity(created.get());
					if (!object_filesystem_profile)
						return unexpected(bootstrap_durability_error());
					auto held = std::make_shared<default_held_object>(
						sqlite_backend_file_role::main_database,
						std::move(created),
						*parent,
						locator_.leaf,
						object_identity,
						entry_identity,
						*object_filesystem_profile,
						std::move(object_mount_identity));
					auto post = capture_namespace(canonical_vfs_locator);
					if (!post ||
						post->parent_namespace_identity != before->parent_namespace_identity ||
						post->entries[0U].state != sqlite_backend_entry_state::held_regular ||
						post->entries[0U].object_identity != object_identity ||
						post->entries[0U].directory_entry_identity != entry_identity ||
						post->entries[0U].object_filesystem_profile != object_filesystem_profile ||
						!post->entries[0U].direct_regular_entry ||
						std::ranges::any_of(post->entries | std::views::drop(1U),
											[](const sqlite_backend_entry_observation& value)
											{
												return value.state !=
													sqlite_backend_entry_state::absent;
											}))
						return unexpected(bootstrap_durability_error());
					auto held_size = held->size();
					auto held_digest = held->sha256();
					auto replacement = held->recheck_current_entry();
					if (!held_size || *held_size != 0U || !held_digest ||
						*held_digest !=
							"sha256:"
							"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855" ||
						!replacement ||
						*replacement !=
							sqlite_backend_replacement_state::exact_same_entry_and_object)
						return unexpected(bootstrap_durability_error());
					return sqlite_backend_zero_main_receipt{
						std::string{observation_profile},
						capability_token_,
						before->parent_namespace_identity,
						std::move(entry_identity),
						std::move(object_identity),
						std::move(held),
						0U,
						std::move(*held_digest),
						true,
						true,
					};
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(bootstrap_durability_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(bootstrap_durability_error());
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_private_snapshot_builder>>
			create_private_snapshot() override
			{
				try
				{
					return make_sqlite_private_snapshot_builder(capability_token_,
																private_snapshot_registry_);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_wal_recovery_workspace_builder>>
			create_wal_recovery_workspace() override
			{
				try
				{
					return make_sqlite_wal_recovery_workspace_builder(capability_token_,
																	  private_snapshot_registry_);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_connection_observation(const std::string_view canonical_vfs_locator) override
			{
				if (canonical_vfs_locator != canonical_locator_)
					return unexpected(locator_binding_error());
				if (!connection_observation_port_)
					return unexpected(observation_unavailable_error());
				try
				{
					auto scope = connection_observation_port_->begin_connection_observation(
						canonical_vfs_locator, capability_token_);
					if (!scope || !*scope || (*scope)->token().profile.empty() ||
						(*scope)->token().bytes.empty())
						return unexpected(observation_unavailable_error());
					return scope;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

			[[nodiscard]] result<std::shared_ptr<sqlite_backend_connection_observation_scope>>
			begin_ephemeral_connection_observation() override
			{
				if (!connection_observation_port_)
					return unexpected(observation_unavailable_error());
				try
				{
					auto scope =
						connection_observation_port_->begin_ephemeral_connection_observation(
							capability_token_);
					if (!scope || !*scope || (*scope)->token().profile.empty() ||
						(*scope)->token().bytes.empty())
						return unexpected(observation_unavailable_error());
					return scope;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(observation_allocation_error());
				}
				catch (const std::length_error&)
				{
					return unexpected(observation_allocation_error());
				}
			}

		  private:
			std::string canonical_locator_;
			locator_parts locator_;
			std::string registered_vfs_name_;
			const void* forwarding_vfs_identity_{};
			const void* pinned_underlying_vfs_app_data_identity_{};
			std::shared_ptr<void> backend_lifetime_;
			sqlite_private_snapshot_registry_binding private_snapshot_registry_;
			std::shared_ptr<const sqlite_default_connection_observation_port>
				connection_observation_port_;
			sqlite_backend_opaque_identity capability_token_;
			std::shared_ptr<sqlite_source_shm_readonly_port> source_shm_readonly_port_;
		};

		[[nodiscard]] bool
		connection_port_matches(const sqlite_default_observation_binding& binding) noexcept
		{
			if (!binding.connection_observation_port)
				return true;
			const auto observed = binding.connection_observation_port->binding();
			return observed.registered_vfs_name == binding.registered_vfs_name &&
				observed.forwarding_vfs_identity == binding.forwarding_vfs_identity &&
				observed.pinned_underlying_vfs_identity == binding.pinned_underlying_vfs_identity &&
				observed.pinned_underlying_vfs_app_data_identity ==
				binding.pinned_underlying_vfs_app_data_identity &&
				observed.backend_lifetime_identity == binding.backend_lifetime.get();
		}

		[[nodiscard]] sqlite_backend_opaque_identity
		make_capability_token(const sqlite_default_observation_binding& binding)
		{
			sqlite_backend_opaque_identity output;
			output.profile = "default-filesystem-v1.capability.v1";
			output.bytes.reserve(128U + binding.canonical_vfs_locator.size() +
								 binding.registered_vfs_name.size());
			append_bytes(output.bytes, observation_profile);
			append_bytes(output.bytes, binding.canonical_vfs_locator);
			append_bytes(output.bytes, binding.registered_vfs_name);
			append_pointer(output.bytes, binding.forwarding_vfs_identity);
			append_pointer(output.bytes, binding.pinned_underlying_vfs_identity);
			append_pointer(output.bytes, binding.pinned_underlying_vfs_app_data_identity);
			append_pointer(output.bytes, binding.backend_lifetime.get());
			append_pointer(output.bytes, binding.private_snapshot_registry.runtime_identity);
			append_pointer(output.bytes, binding.private_snapshot_registry.pinned_default_vfs);
			append_pointer(output.bytes, binding.private_snapshot_registry.runtime_lifetime.get());
			append_pointer(output.bytes, binding.connection_observation_port.get());
			return output;
		}
	} // namespace

	result<std::shared_ptr<sqlite_backend_observation_capability>>
	make_sqlite_default_observation_capability(sqlite_default_observation_binding binding)
	{
		if (binding.registered_vfs_name.empty() || binding.registered_vfs_name.contains('\0') ||
			binding.forwarding_vfs_identity == nullptr ||
			binding.pinned_underlying_vfs_identity == nullptr ||
			binding.pinned_underlying_vfs_app_data_identity == nullptr ||
			!binding.backend_lifetime ||
			binding.private_snapshot_registry.runtime_identity == nullptr ||
			binding.private_snapshot_registry.pinned_default_vfs == nullptr ||
			binding.private_snapshot_registry.find == nullptr ||
			binding.private_snapshot_registry.register_vfs == nullptr ||
			binding.private_snapshot_registry.unregister_vfs == nullptr ||
			!binding.private_snapshot_registry.runtime_lifetime ||
			binding.private_snapshot_registry.pinned_default_vfs !=
				binding.pinned_underlying_vfs_identity ||
			!connection_port_matches(binding))
			return unexpected(observation_unavailable_error());
		if (binding.private_snapshot_registry.find(binding.registered_vfs_name.c_str()) !=
			binding.forwarding_vfs_identity)
			return unexpected(observation_unavailable_error());
		auto locator = split_absolute_locator(binding.canonical_vfs_locator);
		if (!locator)
			return unexpected(std::move(locator.error()));
		try
		{
			auto capability_token = make_capability_token(binding);
			auto source_shm_readonly =
				make_sqlite_source_shm_readonly_preflight(binding, capability_token);
			if (!source_shm_readonly)
				return unexpected(std::move(source_shm_readonly.error()));
			auto capability =
				std::make_shared<default_observation_capability>(std::move(binding),
																 std::move(*locator),
																 std::move(capability_token),
																 std::move(*source_shm_readonly));
			return std::static_pointer_cast<sqlite_backend_observation_capability>(
				std::move(capability));
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(observation_allocation_error());
		}
		catch (const std::length_error&)
		{
			return unexpected(observation_allocation_error());
		}
	}

	result<sqlite_default_entry_identity_observation>
	observe_sqlite_default_entry_identity_without_open(
		const sqlite_backend_observation_capability& capability,
		const std::string_view canonical_vfs_locator,
		const sqlite_backend_file_role role)
	{
		const auto* platform = dynamic_cast<const default_observation_capability*>(&capability);
		if (platform == nullptr)
			return unexpected(observation_unavailable_error());
		return platform->observe_entry_identity_no_open(canonical_vfs_locator, role);
	}

	result<sqlite_default_entry_state_observation> observe_sqlite_default_entry_state_without_open(
		const sqlite_backend_observation_capability& capability,
		const std::string_view canonical_vfs_locator,
		const sqlite_backend_file_role role)
	{
		const auto* platform = dynamic_cast<const default_observation_capability*>(&capability);
		if (platform == nullptr)
			return unexpected(observation_unavailable_error());
		return platform->observe_entry_state_without_open(canonical_vfs_locator, role);
	}
} // namespace cxxlens::sdk
