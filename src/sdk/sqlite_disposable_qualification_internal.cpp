#include "sqlite_disposable_qualification_internal.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <utility>

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
		};

		constexpr unsigned int required_statx_mask =
			STATX_TYPE | STATX_MODE | STATX_NLINK | STATX_INO | STATX_MNT_ID;

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
			output = {make_identity(observed), observed.stx_nlink};
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
			output = {make_identity(observed), observed.stx_nlink};
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
		[[nodiscard]] live_root_status revalidate_live_root(const State& state) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			if (!state.parent || !state.root)
				return live_root_status::rebound;
			sqlite_disposable_object_identity current_parent;
			sqlite_disposable_object_identity current_root;
			sqlite_disposable_object_identity current_entry;
			if (!identity_for_fd(state.parent.get(), current_parent) ||
				!identity_for_fd(state.root.get(), current_root) ||
				!identity_for_entry(
					state.parent.get(), state.private_leaf.c_str(), current_entry) ||
				current_parent != state.parent_object || current_root != state.root_object ||
				current_entry != state.root_entry || current_root != current_entry ||
				!directory_identity(current_root, 0700U) ||
				!directory_identity(current_entry, 0700U))
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
	} // namespace

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
} // namespace cxxlens::detail::sqlite_qualification
