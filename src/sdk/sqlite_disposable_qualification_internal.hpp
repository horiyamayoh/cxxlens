#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>

#include <cxxlens/sdk/common.hpp>

#if defined(__GNUC__) || defined(__clang__)
#define CXXLENS_SQLITE_QUALIFICATION_HIDDEN __attribute__((visibility("hidden")))
#else
#define CXXLENS_SQLITE_QUALIFICATION_HIDDEN
#endif

namespace cxxlens::detail::sqlite_qualification
{
	class sqlite_disposable_qualification_capability;
	enum class sqlite_disposable_qualification_verdict : std::uint8_t;
	class sqlite_disposable_raw_family_observer;
	struct sqlite_disposable_fz_post_cleanup_result;

	/** Exact opened-object identity retained by the disposable qualification harness. */
	struct sqlite_disposable_object_identity
	{
		std::uint64_t device{};
		std::uint64_t inode{};
		std::uint64_t kind{};
		std::uint64_t permissions{};
		std::uint64_t mount_id{};

		[[nodiscard]] bool operator==(const sqlite_disposable_object_identity&) const = default;
	};

	enum class sqlite_disposable_cleanup_policy : std::uint8_t
	{
		retain_private_root,
		remove_empty_private_root,
	};

	enum class sqlite_disposable_requested_effect : std::uint8_t
	{
		no_effect,
		classify_source,
		cleanup_source,
		recover_source,
		normalize_source,
	};

	/**
	 * Exact request-side bindings. This is not authority: the process-local capability must also
	 * be presented, remain live, and revalidate its retained filesystem objects.
	 */
	struct sqlite_disposable_qualification_request
	{
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
		sqlite_disposable_requested_effect requested_effect{
			sqlite_disposable_requested_effect::no_effect};
	};

	struct sqlite_disposable_qualification_bindings
	{
		/**
		 * Harness-supplied SHA-256 commitment over the exact selected runtime/VFS/device/build
		 * profile, harness build/toolchain, accepted proposal-review receipt, and candidate-report
		 * identity. This layer binds the exact bytes; it neither interprets this receipt nor lets
		 * the receipt substitute for the live capability and fresh run ID.
		 */
		std::string exact_profile_digest;
		/** SHA-256 of the exact family/run plan selected by the harness. */
		std::string family_plan_digest;
		/** SHA-256 of the closed allowed-effect and fault-boundary schedule. */
		std::string effect_fault_schedule_digest;
		sqlite_disposable_cleanup_policy cleanup_policy{
			sqlite_disposable_cleanup_policy::retain_private_root};
	};

	/**
	 * Authenticated retained parent-directory port. It duplicates the supplied descriptor and
	 * never retains or accepts a pathname.
	 */
	class CXXLENS_SQLITE_QUALIFICATION_HIDDEN sqlite_disposable_parent_directory final
	{
	  public:
		sqlite_disposable_parent_directory(const sqlite_disposable_parent_directory&) = delete;
		sqlite_disposable_parent_directory&
		operator=(const sqlite_disposable_parent_directory&) = delete;
		sqlite_disposable_parent_directory(sqlite_disposable_parent_directory&& other) noexcept;
		sqlite_disposable_parent_directory&
		operator=(sqlite_disposable_parent_directory&&) = delete;
		~sqlite_disposable_parent_directory();

	  private:
		sqlite_disposable_parent_directory(int descriptor,
										   sqlite_disposable_object_identity identity) noexcept;

		int descriptor_{-1};
		sqlite_disposable_object_identity identity_;

		friend cxxlens::sdk::result<sqlite_disposable_parent_directory>
		duplicate_sqlite_disposable_parent_directory(int directory_descriptor);
		friend class sqlite_disposable_qualification_capability;
		friend cxxlens::sdk::result<sqlite_disposable_qualification_capability>
		make_sqlite_disposable_qualification_capability(
			sqlite_disposable_parent_directory parent,
			std::string_view private_leaf,
			sqlite_disposable_qualification_bindings bindings);
	};

	/**
	 * Nonforgeable process-local disposable-fixture authority. It is deliberately move-only,
	 * opaque, non-default-constructible, and has no serialization or locator conversion surface.
	 */
	class CXXLENS_SQLITE_QUALIFICATION_HIDDEN sqlite_disposable_qualification_capability final
	{
	  public:
		sqlite_disposable_qualification_capability(
			const sqlite_disposable_qualification_capability&) = delete;
		sqlite_disposable_qualification_capability&
		operator=(const sqlite_disposable_qualification_capability&) = delete;
		sqlite_disposable_qualification_capability(
			sqlite_disposable_qualification_capability&& other) noexcept;
		sqlite_disposable_qualification_capability&
		operator=(sqlite_disposable_qualification_capability&&) = delete;
		~sqlite_disposable_qualification_capability();

		/** Produce a request receipt for the currently bound no-effect gate. */
		[[nodiscard]] sqlite_disposable_qualification_request no_effect_request() const;

		/** Revoke authority, safely remove an unchanged empty root when requested, and close FDs.
		 */
		[[nodiscard]] cxxlens::sdk::result<void> revoke();

	  private:
		struct state;
		explicit sqlite_disposable_qualification_capability(std::unique_ptr<state> state) noexcept;

		std::unique_ptr<state> state_;

		friend cxxlens::sdk::result<sqlite_disposable_qualification_capability>
		make_sqlite_disposable_qualification_capability(
			sqlite_disposable_parent_directory parent,
			std::string_view private_leaf,
			sqlite_disposable_qualification_bindings bindings);
		friend sqlite_disposable_qualification_verdict enter_sqlite_disposable_qualification(
			sqlite_disposable_qualification_capability& capability,
			const sqlite_disposable_qualification_request& request) noexcept;
		friend void set_sqlite_disposable_pre_remove_signal_for_testing(
			sqlite_disposable_qualification_capability& capability,
			void (*signal)(void*) noexcept,
			void* context) noexcept;
		friend void invalidate_sqlite_disposable_process_instance_for_testing(
			sqlite_disposable_qualification_capability& capability) noexcept;
		friend cxxlens::sdk::result<void> write_sqlite_disposable_fixture_file_for_testing(
			sqlite_disposable_qualification_capability& capability,
			const sqlite_disposable_qualification_request& request,
			std::string_view leaf,
			std::span<const std::byte> bytes) noexcept;
		friend cxxlens::sdk::result<sqlite_disposable_fz_post_cleanup_result>
		cleanup_sqlite_disposable_fz_post_wal_for_testing(
			sqlite_disposable_qualification_capability& capability,
			const sqlite_disposable_qualification_request& request) noexcept;
		friend class sqlite_disposable_raw_family_observer;
	};

	enum class sqlite_disposable_qualification_verdict : std::uint8_t
	{
		effects_denied_ready,
		capability_revoked_or_stale,
		wrong_creator_process,
		wrong_run,
		wrong_profile,
		wrong_family_plan,
		wrong_effect_fault_schedule,
		wrong_cleanup_policy,
		wrong_parent_binding,
		wrong_root_object_binding,
		wrong_root_entry_binding,
		root_entry_rebound,
		root_not_empty,
		effect_not_authorized,
	};

	/** Duplicate and authenticate one harness-owned parent directory descriptor. */
	[[nodiscard]] CXXLENS_SQLITE_QUALIFICATION_HIDDEN
		cxxlens::sdk::result<sqlite_disposable_parent_directory>
		duplicate_sqlite_disposable_parent_directory(int directory_descriptor);

	/**
	 * Create exactly one new private leaf, verify its retained object/entry identity and empty
	 * census, then mint the capability. No pathname or public Store locator is accepted.
	 */
	[[nodiscard]] CXXLENS_SQLITE_QUALIFICATION_HIDDEN
		cxxlens::sdk::result<sqlite_disposable_qualification_capability>
		make_sqlite_disposable_qualification_capability(
			sqlite_disposable_parent_directory parent,
			std::string_view private_leaf,
			sqlite_disposable_qualification_bindings bindings);

	/**
	 * Qualification-only entry gate. Slice 1 accepts only `no_effect` and returns
	 * `effects_denied_ready`; every source-effect request remains rejected.
	 */
	[[nodiscard]] CXXLENS_SQLITE_QUALIFICATION_HIDDEN sqlite_disposable_qualification_verdict
	enter_sqlite_disposable_qualification(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request) noexcept;

	/**
	 * Deterministic internal test signal at the final-check-to-unlink boundary. It discloses no
	 * retained descriptor, locator, leaf, or binding; a test must use its own fixture authority.
	 * This seam is not linked into any production component.
	 */
	CXXLENS_SQLITE_QUALIFICATION_HIDDEN void set_sqlite_disposable_pre_remove_signal_for_testing(
		sqlite_disposable_qualification_capability& capability,
		void (*signal)(void* context) noexcept,
		void* context) noexcept;

	/** Invalidate only the retained creator-process proof for a focused fail-closed test. */
	CXXLENS_SQLITE_QUALIFICATION_HIDDEN void
	invalidate_sqlite_disposable_process_instance_for_testing(
		sqlite_disposable_qualification_capability& capability) noexcept;

	/**
	 * Test-harness setup only. Write one direct regular file below the capability's retained root;
	 * the production Store and VFS do not link this BUILD_TESTING-only target. The helper accepts
	 * no pathname outside the already authenticated root and is never a classification or success
	 * authority.
	 */
	[[nodiscard]] CXXLENS_SQLITE_QUALIFICATION_HIDDEN cxxlens::sdk::result<void>
	write_sqlite_disposable_fixture_file_for_testing(
		sqlite_disposable_qualification_capability& capability,
		const sqlite_disposable_qualification_request& request,
		std::string_view leaf,
		std::span<const std::byte> bytes) noexcept;
} // namespace cxxlens::detail::sqlite_qualification

#undef CXXLENS_SQLITE_QUALIFICATION_HIDDEN
