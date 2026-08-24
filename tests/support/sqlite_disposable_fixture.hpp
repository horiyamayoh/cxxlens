#pragma once

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "sdk/sqlite_disposable_normalization_internal.hpp"

#if defined(__linux__)
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace cxxlens::test
{
	using namespace cxxlens::detail::sqlite_qualification;

	/** Test-owned effect result. Raw product observations remain the only classification input. */
	struct sqlite_disposable_fixture_cleanup_result
	{
		sqlite_disposable_raw_family_observation before;
		sqlite_disposable_raw_family_observation after;
	};

	/**
	 * Filesystem authority owned by one disposable test fixture. This adapter deliberately lives
	 * outside the product sources: fixture writes, deterministic boundary signals, and injected
	 * sync failures cannot be reached through a product capability or symbol.
	 */
	class sqlite_disposable_fixture final
	{
	  public:
		sqlite_disposable_fixture(sqlite_disposable_qualification_capability& capability,
								  const int parent,
								  const std::string_view root_leaf,
								  sqlite_disposable_qualification_request binding)
			: capability_{&capability}, binding_{std::move(binding)}, root_leaf_{root_leaf}
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			parent_ = duplicate(parent);
			if (parent_ >= 0)
				root_ = open_directory(parent_, root_leaf_.c_str());
			if (parent_ < 0 || root_ < 0 || !capability_binding_live() || !root_binding_stable())
			{
				close_descriptors();
				throw std::runtime_error{"open bound SQLite disposable fixture"};
			}
#else
			(void)parent;
			throw std::runtime_error{
				"SQLite disposable fixtures require Linux statx mount identity"};
#endif
		}

		sqlite_disposable_fixture(const sqlite_disposable_fixture&) = delete;
		sqlite_disposable_fixture& operator=(const sqlite_disposable_fixture&) = delete;
		sqlite_disposable_fixture(sqlite_disposable_fixture&&) = delete;
		sqlite_disposable_fixture& operator=(sqlite_disposable_fixture&&) = delete;

		~sqlite_disposable_fixture()
		{
			close_descriptors();
		}

		[[nodiscard]] cxxlens::sdk::result<void>
		write(const sqlite_disposable_qualification_request& request,
			  const std::string_view leaf,
			  const std::span<const std::byte> bytes) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			if (request.requested_effect != sqlite_disposable_requested_effect::no_effect ||
				!request_binding_matches(request) || !capability_binding_live() ||
				!root_binding_stable())
				return cxxlens::sdk::unexpected(error("fixture-binding"));
			if (!valid_leaf(leaf) || bytes.size() > maximum_file_bytes)
				return cxxlens::sdk::unexpected(error("fixture-file"));

			std::string leaf_copy;
			try
			{
				leaf_copy = leaf;
			}
			catch (...)
			{
				return cxxlens::sdk::unexpected(error("fixture-allocation"));
			}
			const auto file = open_file(root_, leaf_copy.c_str());
			if (file < 0)
				return cxxlens::sdk::unexpected(error("fixture-file-open"));
			std::size_t offset{};
			while (offset < bytes.size())
			{
				ssize_t count{};
				for (;;)
				{
					count = ::write(file, bytes.data() + offset, bytes.size() - offset);
					if (count >= 0 || errno != EINTR)
						break;
				}
				if (count <= 0 || static_cast<std::size_t>(count) > bytes.size() - offset)
				{
					(void)::close(file);
					return cxxlens::sdk::unexpected(error("fixture-file-write"));
				}
				offset += static_cast<std::size_t>(count);
			}
			const auto synchronized = sync(file) && sync(root_);
			if (::close(file) != 0 || !synchronized)
				return cxxlens::sdk::unexpected(error("fixture-file-sync"));
			return {};
#else
			(void)request;
			(void)leaf;
			(void)bytes;
			return cxxlens::sdk::unexpected(error("unsupported-platform"));
#endif
		}

		void signal_before_cleanup(void (*signal)(void*) noexcept, void* context) noexcept
		{
			pre_cleanup_signal_ = signal;
			pre_cleanup_context_ = context;
		}

		void fail_cleanup_sync() noexcept
		{
			fail_sync_ = true;
		}

		[[nodiscard]] cxxlens::sdk::result<sqlite_disposable_fixture_cleanup_result>
		cleanup_fz_post_wal(const sqlite_disposable_qualification_request& request) noexcept
		{
#if defined(__linux__) && defined(STATX_MNT_ID)
			try
			{
				if (capability_ == nullptr || !capability_->live_in_current_process())
					return cxxlens::sdk::unexpected(error("normalization-capability-revoked"));
				if (cleanup_was_attempted(request.qualification_run_id))
					return cxxlens::sdk::unexpected(error("normalization-capability-consumed"));
				if (request.requested_effect !=
					sqlite_disposable_requested_effect::normalize_source)
					return cxxlens::sdk::unexpected(error("normalization-effect-not-authorized"));
				if (!request_binding_matches(request) || !root_binding_stable())
					return cxxlens::sdk::unexpected(error("normalization-capability-binding"));

				auto classify_request = request;
				classify_request.requested_effect =
					sqlite_disposable_requested_effect::classify_source;
				auto before =
					observe_sqlite_disposable_raw_empty_family(*capability_, classify_request);
				if (!before)
				{
					if (before.error().detail == "raw-capability-revoked")
						return cxxlens::sdk::unexpected(error("normalization-capability-revoked"));
					if (before.error().detail == "raw-capability-binding")
						return cxxlens::sdk::unexpected(error("normalization-capability-binding"));
					return cxxlens::sdk::unexpected(std::move(before.error()));
				}
				if (before->family.family !=
						sqlite_disposable_empty_family::exact_pre_or_post_zero_wal ||
					before->family.phase != sqlite_disposable_family_phase::post || !before->wal ||
					before->wal->byte_count != 0U)
					return cxxlens::sdk::unexpected(error("normalization-fz-post-required"));

				auto plan = plan_sqlite_disposable_empty_normalization(before->observation);
				if (!plan || plan->family != before->family ||
					plan->route !=
						sqlite_disposable_normalization_route::establish_rollback_empty_anchor ||
					plan->uses_existing_zero_byte_wal ||
					plan->may_handoff_to_ordinary_fresh_initialization)
					return cxxlens::sdk::unexpected(error("normalization-route-not-authorized"));

				const auto cleanup_claim = claim_cleanup_attempt(request.qualification_run_id);
				if (cleanup_claim == cleanup_claim_status::already_consumed)
					return cxxlens::sdk::unexpected(error("normalization-capability-consumed"));
				if (cleanup_claim == cleanup_claim_status::capacity_exhausted)
					return cxxlens::sdk::unexpected(
						error("normalization-fixture-attempt-capacity"));
				const auto signal = std::exchange(pre_cleanup_signal_, nullptr);
				auto* const signal_context = std::exchange(pre_cleanup_context_, nullptr);
				if (signal != nullptr)
					signal(signal_context);

				auto rechecked =
					observe_sqlite_disposable_raw_empty_family(*capability_, classify_request);
				if (!rechecked)
				{
					if (!leaf_matches("main", before->main))
						return cxxlens::sdk::unexpected(error("normalization-main-drift"));
					if (!leaf_matches("main-wal", *before->wal))
						return cxxlens::sdk::unexpected(error("normalization-wal-drift"));
					if (rechecked.error().detail == "raw-main-not-exact-empty")
						return cxxlens::sdk::unexpected(error("normalization-main-drift"));
					if (rechecked.error().detail == "raw-nonzero-wal-unresolved")
						return cxxlens::sdk::unexpected(error("normalization-wal-drift"));
					return cxxlens::sdk::unexpected(std::move(rechecked.error()));
				}
				if (rechecked->main != before->main)
					return cxxlens::sdk::unexpected(error("normalization-main-drift"));
				if (!rechecked->wal || *rechecked->wal != *before->wal)
					return cxxlens::sdk::unexpected(error("normalization-wal-drift"));
				if (!root_binding_stable())
					return cxxlens::sdk::unexpected(error("normalization-anchor-drift"));

				if (::unlinkat(root_, "main-wal", 0) != 0)
					return cxxlens::sdk::unexpected(error("normalization-wal-unlink-uncertain"));
				if (fail_sync_ || !sync(root_))
					return cxxlens::sdk::unexpected(error("normalization-parent-sync-uncertain"));

				auto after =
					observe_sqlite_disposable_raw_empty_family(*capability_, classify_request);
				if (!after)
					return cxxlens::sdk::unexpected(std::move(after.error()));
				if (after->family.family !=
						sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar ||
					after->family.phase != sqlite_disposable_family_phase::post || after->wal ||
					after->main != before->main)
					return cxxlens::sdk::unexpected(error("normalization-anchor-not-established"));

				return sqlite_disposable_fixture_cleanup_result{std::move(*before),
																std::move(*after)};
			}
			catch (const std::bad_alloc&)
			{
				return cxxlens::sdk::unexpected(error("normalization-allocation"));
			}
			catch (...)
			{
				return cxxlens::sdk::unexpected(error("normalization-exception"));
			}
#else
			(void)request;
			return cxxlens::sdk::unexpected(error("normalization-unsupported-platform"));
#endif
		}

	  private:
		static constexpr std::size_t maximum_file_bytes = 65'536U;

		[[nodiscard]] static cxxlens::sdk::error error(const std::string_view detail)
		{
			return {"store.backend-unavailable",
					"sqlite-disposable-qualification",
					std::string{detail}};
		}

		[[nodiscard]] static bool valid_leaf(const std::string_view leaf) noexcept
		{
			return !leaf.empty() && leaf != "." && leaf != ".." &&
				leaf.find('/') == std::string_view::npos &&
				leaf.find('\0') == std::string_view::npos;
		}

		[[nodiscard]] bool request_binding_matches(
			const sqlite_disposable_qualification_request& request) const noexcept
		{
			return request.creator_process_identity == binding_.creator_process_identity &&
				request.qualification_run_id == binding_.qualification_run_id &&
				request.parent_object == binding_.parent_object &&
				request.root_object == binding_.root_object &&
				request.root_entry == binding_.root_entry &&
				request.exact_profile_digest == binding_.exact_profile_digest &&
				request.family_plan_digest == binding_.family_plan_digest &&
				request.effect_fault_schedule_digest == binding_.effect_fault_schedule_digest &&
				request.cleanup_policy == binding_.cleanup_policy;
		}

		[[nodiscard]] bool capability_binding_live() const noexcept
		{
			try
			{
				return capability_ != nullptr && capability_->live_in_current_process() &&
					request_binding_matches(capability_->no_effect_request());
			}
			catch (...)
			{
				return false;
			}
		}

		struct cleanup_attempt_registry
		{
			std::mutex mutex;
			std::vector<std::uint64_t> run_ids;
		};

		enum class cleanup_claim_status : std::uint8_t
		{
			claimed,
			already_consumed,
			capacity_exhausted,
		};

		[[nodiscard]] static cleanup_attempt_registry& cleanup_attempts()
		{
			static cleanup_attempt_registry registry;
			return registry;
		}

		[[nodiscard]] static bool cleanup_was_attempted(const std::uint64_t run_id)
		{
			auto& registry = cleanup_attempts();
			const std::scoped_lock lock{registry.mutex};
			return std::ranges::find(registry.run_ids, run_id) != registry.run_ids.end();
		}

		[[nodiscard]] static cleanup_claim_status claim_cleanup_attempt(const std::uint64_t run_id)
		{
			auto& registry = cleanup_attempts();
			const std::scoped_lock lock{registry.mutex};
			if (std::ranges::find(registry.run_ids, run_id) != registry.run_ids.end())
				return cleanup_claim_status::already_consumed;
			if (registry.run_ids.size() >= maximum_cleanup_attempts)
				return cleanup_claim_status::capacity_exhausted;
			registry.run_ids.push_back(run_id);
			return cleanup_claim_status::claimed;
		}

#if defined(__linux__) && defined(STATX_MNT_ID)
		[[nodiscard]] static int duplicate(const int descriptor) noexcept
		{
			for (;;)
			{
				const auto output = ::fcntl(descriptor, F_DUPFD_CLOEXEC, 0);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] static int open_directory(const int parent, const char* leaf) noexcept
		{
			for (;;)
			{
				const auto output =
					::openat(parent, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] static int open_file(const int parent, const char* leaf) noexcept
		{
			for (;;)
			{
				const auto output = ::openat(
					parent, leaf, O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW | O_CLOEXEC, 0600);
				if (output >= 0 || errno != EINTR)
					return output;
			}
		}

		[[nodiscard]] static bool sync(const int descriptor) noexcept
		{
			for (;;)
			{
				if (::fsync(descriptor) == 0)
					return true;
				if (errno != EINTR)
					return false;
			}
		}

		[[nodiscard]] static sqlite_disposable_object_identity
		identity(const struct statx& observed) noexcept
		{
			const auto device = (static_cast<std::uint64_t>(observed.stx_dev_major) << 32U) |
				static_cast<std::uint64_t>(observed.stx_dev_minor);
			return {device,
					observed.stx_ino,
					static_cast<std::uint64_t>(observed.stx_mode & S_IFMT),
					static_cast<std::uint64_t>(observed.stx_mode & 07777),
					observed.stx_mnt_id};
		}

		[[nodiscard]] static bool observe_fd(const int descriptor,
											 sqlite_disposable_object_identity& output) noexcept
		{
			constexpr unsigned int mask = STATX_TYPE | STATX_MODE | STATX_INO | STATX_MNT_ID;
			struct statx observed{};
			for (;;)
			{
				if (::statx(descriptor, "", AT_EMPTY_PATH | AT_SYMLINK_NOFOLLOW, mask, &observed) ==
					0)
					break;
				if (errno != EINTR)
					return false;
			}
			if ((observed.stx_mask & mask) != mask)
				return false;
			output = identity(observed);
			return true;
		}

		[[nodiscard]] static bool observe_entry(const int parent,
												const char* leaf,
												sqlite_disposable_object_identity& output,
												std::uint64_t* size = nullptr) noexcept
		{
			constexpr unsigned int mask =
				STATX_TYPE | STATX_MODE | STATX_INO | STATX_MNT_ID | STATX_SIZE;
			struct statx observed{};
			for (;;)
			{
				if (::statx(parent, leaf, AT_SYMLINK_NOFOLLOW, mask, &observed) == 0)
					break;
				if (errno != EINTR)
					return false;
			}
			if ((observed.stx_mask & mask) != mask)
				return false;
			output = identity(observed);
			if (size != nullptr)
				*size = observed.stx_size;
			return true;
		}

		[[nodiscard]] bool root_binding_stable() const noexcept
		{
			sqlite_disposable_object_identity parent;
			sqlite_disposable_object_identity root;
			sqlite_disposable_object_identity entry;
			return parent_ >= 0 && root_ >= 0 && observe_fd(parent_, parent) &&
				observe_fd(root_, root) && observe_entry(parent_, root_leaf_.c_str(), entry) &&
				parent == binding_.parent_object && root == binding_.root_object &&
				entry == binding_.root_entry && root == entry;
		}

		[[nodiscard]] bool
		leaf_matches(const char* leaf,
					 const sqlite_disposable_raw_file_observation& expected) const noexcept
		{
			sqlite_disposable_object_identity entry;
			std::uint64_t size{};
			return observe_entry(root_, leaf, entry, &size) && entry == expected.entry &&
				size == expected.byte_count;
		}
#endif

		void close_descriptors() noexcept
		{
#if defined(__linux__)
			if (root_ >= 0)
				(void)::close(root_);
			if (parent_ >= 0)
				(void)::close(parent_);
#endif
			root_ = -1;
			parent_ = -1;
		}

		static constexpr std::size_t maximum_cleanup_attempts = 4096U;

		sqlite_disposable_qualification_capability* capability_{};
		sqlite_disposable_qualification_request binding_;
		std::string root_leaf_;
		int parent_{-1};
		int root_{-1};
		void (*pre_cleanup_signal_)(void*) noexcept {};
		void* pre_cleanup_context_{};
		bool fail_sync_{};
	};
} // namespace cxxlens::test
