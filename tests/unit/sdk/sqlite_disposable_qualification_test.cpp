#include <array>
#include <concepts>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__linux__)
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk/store.hpp>

#include "../../support/sqlite_disposable_fixture.hpp"
#include "sdk/sqlite_disposable_normalization_internal.hpp"
#include "sdk/sqlite_disposable_qualification_internal.hpp"

namespace
{
	using namespace cxxlens::detail::sqlite_qualification;
	using cxxlens::test::sqlite_disposable_fixture;

	using capability = sqlite_disposable_qualification_capability;
	using entry_function = decltype(&enter_sqlite_disposable_qualification);
	using public_store_open_function = decltype(&cxxlens::sdk::open_sqlite_snapshot_store);

	static_assert(!std::default_initializable<capability>);
	static_assert(!std::copy_constructible<capability>);
	static_assert(!std::is_copy_assignable_v<capability>);
	static_assert(std::move_constructible<capability>);
	static_assert(!std::is_move_assignable_v<capability>);
	static_assert(!std::is_aggregate_v<capability>);
	static_assert(!std::is_trivially_copyable_v<capability>);
	static_assert(!std::constructible_from<capability, std::filesystem::path>);
	static_assert(!std::constructible_from<capability, std::string>);
	static_assert(!std::convertible_to<capability, std::string>);
	static_assert(std::is_invocable_r_v<sqlite_disposable_qualification_verdict,
										entry_function,
										capability&,
										const sqlite_disposable_qualification_request&>);
	static_assert(!std::is_invocable_v<entry_function,
									   const std::filesystem::path&,
									   const sqlite_disposable_qualification_request&>);
	static_assert(!std::is_invocable_v<entry_function,
									   std::string_view,
									   const sqlite_disposable_qualification_request&>);
	static_assert(!std::is_invocable_v<public_store_open_function,
									   capability&,
									   cxxlens::sdk::relation_engine>);

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] std::string digest(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] sqlite_disposable_qualification_bindings
	bindings(const sqlite_disposable_cleanup_policy cleanup =
				 sqlite_disposable_cleanup_policy::remove_empty_private_root)
	{
		return {digest('1'), digest('2'), digest('3'), cleanup};
	}

#if defined(__linux__) && defined(STATX_MNT_ID)
	class test_parent
	{
	  public:
		test_parent()
		{
			std::array pattern{'/', 't', 'm', 'p', '/', 'c', 'x', 'x', 'l', 'e', 'n',
							   's', '-', 'd', 'i', 's', 'p', 'o', 's', 'a', 'b', 'l',
							   'e', '-', 'X', 'X', 'X', 'X', 'X', 'X', '\0'};
			auto* created = ::mkdtemp(pattern.data());
			require(created != nullptr, "create test parent");
			path_ = created;
			reopen();
		}

		test_parent(const test_parent&) = delete;
		test_parent& operator=(const test_parent&) = delete;

		~test_parent()
		{
			if (descriptor_ < 0 && !path_.empty())
				descriptor_ =
					::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			if (descriptor_ >= 0)
			{
				for (const auto* leaf : {"qualification-root",
										 "qualification-root-two",
										 "retained-root",
										 "preexisting-empty",
										 "preexisting-nonempty",
										 "preexisting-symlink",
										 "preexisting-regular",
										 "rebound-root",
										 "renamed-original",
										 "renamed-main",
										 "renamed-wal",
										 "nonempty-after-mint"})
				{
					const auto child = ::openat(
						descriptor_, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
					if (child >= 0)
					{
						(void)::unlinkat(child, "payload", 0);
						(void)::unlinkat(child, "unexpected", 0);
						(void)::unlinkat(child, "main", 0);
						(void)::unlinkat(child, "main-wal", 0);
						(void)::unlinkat(child, "main-shm", 0);
						(void)::unlinkat(child, "main-journal", 0);
						(void)::close(child);
						(void)::unlinkat(descriptor_, leaf, AT_REMOVEDIR);
					}
					else
						(void)::unlinkat(descriptor_, leaf, 0);
				}
				(void)::close(descriptor_);
			}
			if (!path_.empty())
				(void)::rmdir(path_.c_str());
		}

		[[nodiscard]] int descriptor() const noexcept
		{
			return descriptor_;
		}

		void close_descriptor()
		{
			require(descriptor_ >= 0, "test parent descriptor is open");
			require(::close(descriptor_) == 0, "close test parent descriptor");
			descriptor_ = -1;
		}

		void reopen()
		{
			require(descriptor_ < 0, "test parent descriptor is closed");
			descriptor_ = ::open(path_.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			require(descriptor_ >= 0, "open test parent");
		}

		void make_directory(const char* leaf) const
		{
			require(::mkdirat(descriptor_, leaf, 0700) == 0, "create test directory");
		}

		void create_file(const int parent, const char* leaf) const
		{
			const auto file =
				::openat(parent, leaf, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
			require(file >= 0, "create test file");
			require(::close(file) == 0, "close test file");
		}

		void create_file(const char* leaf) const
		{
			create_file(descriptor_, leaf);
		}

		[[nodiscard]] int open_directory(const char* leaf) const
		{
			const auto output =
				::openat(descriptor_, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
			require(output >= 0, "open test directory");
			return output;
		}

		void remove_directory(const char* leaf, const char* child = nullptr) const
		{
			if (child != nullptr)
			{
				const auto directory = open_directory(leaf);
				require(::unlinkat(directory, child, 0) == 0, "remove test child");
				require(::close(directory) == 0, "close test child directory");
			}
			require(::unlinkat(descriptor_, leaf, AT_REMOVEDIR) == 0, "remove test directory");
		}

		[[nodiscard]] bool entry_absent(const char* leaf) const
		{
			struct stat observed{};
			errno = 0;
			return ::fstatat(descriptor_, leaf, &observed, AT_SYMLINK_NOFOLLOW) != 0 &&
				errno == ENOENT;
		}

	  private:
		std::string path_;
		int descriptor_{-1};
	};

	class scoped_umask
	{
	  public:
		explicit scoped_umask(const mode_t value) noexcept : previous_{::umask(value)} {}
		scoped_umask(const scoped_umask&) = delete;
		scoped_umask& operator=(const scoped_umask&) = delete;
		~scoped_umask()
		{
			(void)::umask(previous_);
		}

	  private:
		mode_t previous_{};
	};

	[[nodiscard]] cxxlens::sdk::result<capability>
	mint(test_parent& parent,
		 const std::string_view leaf,
		 sqlite_disposable_qualification_bindings exact_bindings = bindings())
	{
		auto authenticated = duplicate_sqlite_disposable_parent_directory(parent.descriptor());
		require(authenticated.has_value(), "authenticate test parent");
		return make_sqlite_disposable_qualification_capability(
			std::move(*authenticated), leaf, std::move(exact_bindings));
	}

	void exercise_move_revoke_and_fresh_run()
	{
		test_parent parent;
		auto first = mint(parent, "qualification-root");
		require(first.has_value(), "mint first capability");
		auto first_request = first->no_effect_request();
		require(first_request.qualification_run_id != 0U, "nonzero run id");
		require(enter_sqlite_disposable_qualification(*first, first_request) ==
					sqlite_disposable_qualification_verdict::effects_denied_ready,
				"exact no-effect entry");

		capability moved{std::move(*first)};
		require(enter_sqlite_disposable_qualification(*first, first_request) ==
					sqlite_disposable_qualification_verdict::capability_revoked_or_stale,
				"moved-from capability is stale");
		require(enter_sqlite_disposable_qualification(moved, first_request) ==
					sqlite_disposable_qualification_verdict::effects_denied_ready,
				"move retains authority");
		require(moved.revoke().has_value(), "explicit revoke");
		require(moved.revoke().has_value(), "idempotent explicit revoke");
		require(enter_sqlite_disposable_qualification(moved, first_request) ==
					sqlite_disposable_qualification_verdict::capability_revoked_or_stale,
				"revoked capability rejected");
		require(moved.no_effect_request().qualification_run_id == 0U,
				"revoked capability cannot reproduce a live request binding");
		require(parent.entry_absent("qualification-root"), "revoke removed exact empty root");

		auto second = mint(parent, "qualification-root-two");
		require(second.has_value(), "mint second capability");
		const auto second_request = second->no_effect_request();
		require(second_request.qualification_run_id != first_request.qualification_run_id,
				"fresh run id is not reused");
		require(enter_sqlite_disposable_qualification(*second, first_request) ==
					sqlite_disposable_qualification_verdict::wrong_run,
				"stale prior-run request rejected");
		require(enter_sqlite_disposable_qualification(moved, second_request) ==
					sqlite_disposable_qualification_verdict::capability_revoked_or_stale,
				"revoked capability remains stale after a new mint and descriptor reuse");
	}

	void exercise_new_leaf_requirement()
	{
		test_parent parent;
		auto retained = mint(parent,
							 "retained-root",
							 bindings(sqlite_disposable_cleanup_policy::retain_private_root));
		require(retained.has_value(), "mint retained capability");
		auto reused = mint(parent, "retained-root");
		require(!reused.has_value(), "reused live leaf rejected");
		require(reused.error().detail == "private-root-not-fresh",
				"reused live leaf rejection is exact");
		require(retained->revoke().has_value(), "revoke retained capability");
		parent.remove_directory("retained-root");

		parent.make_directory("preexisting-empty");
		require(!mint(parent, "preexisting-empty"), "preexisting empty directory rejected");

		parent.make_directory("preexisting-nonempty");
		const auto nonempty = parent.open_directory("preexisting-nonempty");
		parent.create_file(nonempty, "payload");
		require(::close(nonempty) == 0, "close nonempty preexisting directory");
		require(!mint(parent, "preexisting-nonempty"), "preexisting nonempty directory rejected");

		parent.create_file("preexisting-regular");
		require(!mint(parent, "preexisting-regular"), "preexisting regular leaf rejected");
		require(::symlinkat("preexisting-empty", parent.descriptor(), "preexisting-symlink") == 0,
				"create preexisting symlink");
		require(!mint(parent, "preexisting-symlink"), "preexisting symlink leaf rejected");

		require(!mint(parent, "."), "dot leaf rejected");
		require(!mint(parent, "../qualification-root"), "multi-component leaf rejected");
	}

	void exercise_binding_rejections()
	{
		test_parent parent;
		auto minted = mint(parent, "qualification-root");
		require(minted.has_value(), "mint binding capability");
		const auto exact = minted->no_effect_request();

		auto changed = exact;
		++changed.creator_process_identity;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_creator_process,
				"request process binding rejected");
		changed = exact;
		++changed.qualification_run_id;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_run,
				"request run binding rejected");
		changed = exact;
		changed.exact_profile_digest = digest('4');
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_profile,
				"profile binding rejected");
		changed = exact;
		changed.family_plan_digest = digest('4');
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_family_plan,
				"family-plan binding rejected");
		changed = exact;
		changed.effect_fault_schedule_digest = digest('4');
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_effect_fault_schedule,
				"effect/fault schedule binding rejected");
		changed = exact;
		changed.cleanup_policy = sqlite_disposable_cleanup_policy::retain_private_root;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_cleanup_policy,
				"cleanup-policy binding rejected");
		changed = exact;
		changed.parent_object.inode ^= 1U;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_parent_binding,
				"parent object binding rejected");
		changed = exact;
		changed.root_object.inode ^= 1U;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_root_object_binding,
				"root object binding rejected");
		changed = exact;
		changed.root_entry.inode ^= 1U;
		require(enter_sqlite_disposable_qualification(*minted, changed) ==
					sqlite_disposable_qualification_verdict::wrong_root_entry_binding,
				"root entry binding rejected");

		const auto child = ::fork();
		require(child >= 0, "fork wrong-process probe");
		if (child == 0)
		{
			const auto verdict = enter_sqlite_disposable_qualification(*minted, exact);
			::_exit(verdict == sqlite_disposable_qualification_verdict::wrong_creator_process ? 0
																							  : 1);
		}
		int child_status{};
		require(::waitpid(child, &child_status, 0) == child, "wait wrong-process probe");
		require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
				"forked process cannot use capability");
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::effects_denied_ready,
				"creator retains capability after fork probe");

		const auto revoke_child = ::fork();
		require(revoke_child >= 0, "fork wrong-process revoke probe");
		if (revoke_child == 0)
		{
			const auto revoked = minted->revoke();
			const auto root_preserved = !parent.entry_absent("qualification-root");
			::_exit(!revoked && revoked.error().detail == "revoke-wrong-process" && root_preserved
						? 0
						: 1);
		}
		child_status = 0;
		require(::waitpid(revoke_child, &child_status, 0) == revoke_child,
				"wait wrong-process revoke probe");
		require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
				"forked revoke closes only and preserves creator root");

		const auto destructor_child = ::fork();
		require(destructor_child >= 0, "fork wrong-process destructor probe");
		if (destructor_child == 0)
		{
			{
				capability child_capability{std::move(*minted)};
			}
			::_exit(!parent.entry_absent("qualification-root") ? 0 : 1);
		}
		child_status = 0;
		require(::waitpid(destructor_child, &child_status, 0) == destructor_child,
				"wait wrong-process destructor probe");
		require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
				"forked destructor closes only and preserves creator root");
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::effects_denied_ready,
				"creator capability survives child revoke and destructor copies");
	}

	void exercise_binding_input_validation()
	{
		test_parent parent;
		auto invalid = bindings();
		invalid.exact_profile_digest.clear();
		require(!mint(parent, "qualification-root", invalid), "empty profile digest rejected");
		invalid = bindings();
		invalid.family_plan_digest.clear();
		require(!mint(parent, "qualification-root", invalid), "empty family plan rejected");
		invalid = bindings();
		invalid.effect_fault_schedule_digest.clear();
		require(!mint(parent, "qualification-root", invalid), "empty schedule rejected");
		invalid = bindings();
		invalid.exact_profile_digest = "sha256:" + std::string(64U, 'A');
		require(!mint(parent, "qualification-root", invalid),
				"noncanonical profile digest rejected");
		invalid = bindings();
		invalid.cleanup_policy = static_cast<sqlite_disposable_cleanup_policy>(255U);
		require(!mint(parent, "qualification-root", invalid), "unknown cleanup policy rejected");
		require(parent.entry_absent("qualification-root"), "invalid bindings have no root effect");

		{
			scoped_umask restrictive{0777};
			auto failed_after_create = mint(parent, "qualification-root");
			require(!failed_after_create, "nonprivate post-mkdir root rejected");
			require(failed_after_create.error().detail == "private-root-entry",
					"post-mkdir rejection reports successful exact rollback");
		}
		require(parent.entry_absent("qualification-root"),
				"post-mkdir failure rolls back and confirms absence");
		auto retry = mint(parent, "qualification-root");
		require(retry.has_value(), "rolled-back leaf is fresh for the next mint");
	}

	void exercise_effect_denial_and_contamination()
	{
		test_parent parent;
		auto minted = mint(parent, "nonempty-after-mint");
		require(minted.has_value(), "mint effect-denial capability");
		const auto exact = minted->no_effect_request();
		for (const auto effect : {sqlite_disposable_requested_effect::classify_source,
								  sqlite_disposable_requested_effect::cleanup_source,
								  sqlite_disposable_requested_effect::recover_source,
								  sqlite_disposable_requested_effect::normalize_source})
		{
			auto request = exact;
			request.requested_effect = effect;
			require(enter_sqlite_disposable_qualification(*minted, request) ==
						sqlite_disposable_qualification_verdict::effect_not_authorized,
					"qualification entry rejects source effect");
		}
		auto unknown_effect = exact;
		unknown_effect.requested_effect = static_cast<sqlite_disposable_requested_effect>(255U);
		require(enter_sqlite_disposable_qualification(*minted, unknown_effect) ==
					sqlite_disposable_qualification_verdict::effect_not_authorized,
				"unknown source effect fails closed");
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::effects_denied_ready,
				"source-effect requests do not arm the gate");

		const auto root = parent.open_directory("nonempty-after-mint");
		parent.create_file(root, "unexpected");
		require(::close(root) == 0, "close contaminated root");
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::root_not_empty,
				"post-mint root contamination rejected");
		require(!minted->revoke(), "cleanup refuses unknown root entry");
		parent.remove_directory("nonempty-after-mint", "unexpected");
	}

	void exercise_root_rebind()
	{
		test_parent parent;
		auto minted = mint(parent, "rebound-root");
		require(minted.has_value(), "mint rebind capability");
		const auto exact = minted->no_effect_request();
		require(::renameat(
					parent.descriptor(), "rebound-root", parent.descriptor(), "renamed-original") ==
					0,
				"rename original root");
		parent.make_directory("rebound-root");
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::root_entry_rebound,
				"renamed/rebound root rejected");
		require(!minted->revoke(), "revoke refuses rebound entry");
		require(!parent.entry_absent("rebound-root"), "replacement root preserved");
		require(!parent.entry_absent("renamed-original"), "original root preserved");
		parent.remove_directory("rebound-root");
		parent.remove_directory("renamed-original");
	}

	void exercise_retained_parent_lifetime_and_destructor()
	{
		test_parent parent;
		{
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint lifetime capability");
			const auto exact = minted->no_effect_request();
			parent.close_descriptor();
			require(enter_sqlite_disposable_qualification(*minted, exact) ==
						sqlite_disposable_qualification_verdict::effects_denied_ready,
					"capability retains authenticated parent and root");
		}
		parent.reopen();
		require(parent.entry_absent("qualification-root"),
				"capability destructor closes and removes unchanged empty root");
	}

	void exercise_fixture_adapter_capability_liveness()
	{
		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint revoked fixture capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			require(minted->revoke().has_value(), "revoke fixture capability");
			require(!fixture.write(setup, "main", {}),
					"fixture adapter rejects a revoked capability");
			require(parent.entry_absent("qualification-root"),
					"revoked fixture adapter cannot recreate its removed root");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint forked fixture capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			const auto child = ::fork();
			require(child >= 0, "fork fixture adapter probe");
			if (child == 0)
				::_exit(!fixture.write(setup, "main", {}) ? 0 : 1);
			int child_status{};
			require(::waitpid(child, &child_status, 0) == child, "wait fixture adapter fork probe");
			require(WIFEXITED(child_status) && WEXITSTATUS(child_status) == 0,
					"forked fixture adapter cannot use creator capability");
			const auto root = parent.open_directory("qualification-root");
			struct stat absent{};
			errno = 0;
			require(::fstatat(root, "main", &absent, AT_SYMLINK_NOFOLLOW) != 0 && errno == ENOENT,
					"forked fixture adapter leaves creator root unchanged");
			require(::close(root) == 0, "close forked fixture adapter root");
		}
	}

	[[nodiscard]] std::vector<std::byte> canonical_empty_main(const std::uint8_t header_version)
	{
		std::vector<std::byte> bytes(4096U);
		constexpr std::string_view magic{"SQLite format 3\0", 16U};
		std::memcpy(bytes.data(), magic.data(), magic.size());
		const auto put_u16 = [&bytes](const std::size_t offset, const std::uint16_t value)
		{
			bytes[offset] = static_cast<std::byte>((value >> 8U) & 0xffU);
			bytes[offset + 1U] = static_cast<std::byte>(value & 0xffU);
		};
		const auto put_u32 = [&bytes](const std::size_t offset, const std::uint32_t value)
		{
			bytes[offset] = static_cast<std::byte>((value >> 24U) & 0xffU);
			bytes[offset + 1U] = static_cast<std::byte>((value >> 16U) & 0xffU);
			bytes[offset + 2U] = static_cast<std::byte>((value >> 8U) & 0xffU);
			bytes[offset + 3U] = static_cast<std::byte>(value & 0xffU);
		};
		put_u16(16U, 4096U);
		bytes[18U] = static_cast<std::byte>(header_version);
		bytes[19U] = static_cast<std::byte>(header_version);
		bytes[21U] = std::byte{64U};
		bytes[22U] = std::byte{32U};
		bytes[23U] = std::byte{32U};
		put_u32(24U, 1U);
		put_u32(28U, 1U);
		put_u32(92U, 1U);
		put_u32(96U, 3'045'001U);
		bytes[100U] = std::byte{0x0dU};
		put_u16(105U, 4096U);
		return bytes;
	}

	void exercise_raw_empty_family_observation()
	{
		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint F0 raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto wrong_setup = setup;
			++wrong_setup.qualification_run_id;
			require(!fixture.write(wrong_setup, "main", {}),
					"fixture adapter rejects a different qualification run");
			require(!fixture.write(setup, "../main", {}),
					"fixture adapter rejects a multi-component leaf");
			require(!fixture.write(setup, "oversized", std::vector<std::byte>(65'537U)),
					"fixture adapter rejects oversized bytes");
			const auto main = canonical_empty_main(2U);
			require(static_cast<bool>(fixture.write(setup, "main", main)), "write F0 main fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			auto observed = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(observed.has_value(), "observe F0 raw family");
			require(
				observed->family.family == sqlite_disposable_empty_family::exact_pre_no_sidecar &&
					observed->family.phase == sqlite_disposable_family_phase::pre &&
					!observed->wal && observed->main.byte_count == main.size() &&
					observed->main.sha256 ==
						"sha256:e3ba06536f7dbba337dee3c1c5f01b43660ce276abb54c5cee2d5defc5b970aa",
				"F0 raw identity/bytes digest");
			require(observed->observation.source_anchor_stable &&
						observed->observation.main_identity_stable &&
						observed->observation.main_entry_stable &&
						observed->observation.exact_logical_empty &&
						observed->observation.wal == sqlite_disposable_wal_state::absent &&
						!observed->observation.shared_memory_present &&
						observed->observation.journal == sqlite_disposable_journal_state::absent &&
						!observed->observation.other_sidecar_present,
					"F0 retained-FD observation has exact empty no-sidecar topology");

			const auto reobserved = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(reobserved.has_value(), "reobserve F0 raw family from retained FD");
			require(*reobserved == *observed,
					"F0 retained-FD identity/size/bytes/namespace remain unchanged with no effect");

			const auto denied = observe_sqlite_disposable_raw_empty_family(*minted, setup);
			require(!denied && denied.error().detail == "raw-effect-not-authorized",
					"raw classifier requires explicit classify effect");
			const auto invalid = [&]()
			{
				auto mutated = main;
				mutated[18U] = std::byte{3U};
				require(static_cast<bool>(fixture.write(setup, "main", mutated)),
						"write invalid F0 header");
				return observe_sqlite_disposable_raw_empty_family(*minted, classify);
			}();
			require(!invalid && invalid.error().detail == "raw-main-not-exact-empty",
					"invalid F0 header fails closed");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint FZ raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			const auto main = canonical_empty_main(2U);
			require(static_cast<bool>(fixture.write(setup, "main", main)), "write FZ main fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write zero-byte FZ WAL fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			auto observed = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(observed.has_value() &&
						observed->family.family ==
							sqlite_disposable_empty_family::exact_pre_or_post_zero_wal &&
						observed->family.phase == sqlite_disposable_family_phase::pre &&
						observed->wal && observed->wal->byte_count == 0U,
					"FZ-pre raw family");
			require(
				static_cast<bool>(fixture.write(setup, "main-wal", std::array{std::byte{0x01U}})),
				"write nonzero WAL mutation");
			const auto rejected = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(!rejected && rejected.error().detail == "raw-nonzero-wal-unresolved",
					"nonzero WAL is not silently classified");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint FZ-post raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			const auto main = canonical_empty_main(1U);
			require(static_cast<bool>(fixture.write(setup, "main", main)),
					"write FZ-post main fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write FZ-post zero-byte WAL fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			auto observed = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(observed.has_value() &&
						observed->family.family ==
							sqlite_disposable_empty_family::exact_pre_or_post_zero_wal &&
						observed->family.phase == sqlite_disposable_family_phase::post &&
						observed->observation.main_header ==
							sqlite_disposable_main_header_state::rollback_empty &&
						observed->wal && observed->wal->byte_count == 0U,
					"FZ-post raw family preserves rollback-empty header and zero WAL");
			auto planned = plan_sqlite_disposable_empty_normalization(observed->observation);
			require(
				planned.has_value() &&
					planned->route ==
						sqlite_disposable_normalization_route::establish_rollback_empty_anchor &&
					!planned->uses_existing_zero_byte_wal &&
					!planned->may_handoff_to_ordinary_fresh_initialization,
				"FZ-post raw family selects only a rollback-empty anchor route");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint FO raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			const auto main = canonical_empty_main(1U);
			require(static_cast<bool>(fixture.write(setup, "main", main)), "write FO main fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			auto observed = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(observed.has_value() &&
						observed->family.family ==
							sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar &&
						observed->family.phase == sqlite_disposable_family_phase::post,
					"FO raw family");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint mixed raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			const auto main = canonical_empty_main(2U);
			require(static_cast<bool>(fixture.write(setup, "main", main)),
					"write mixed main fixture");
			require(
				static_cast<bool>(fixture.write(setup, "unexpected", std::array{std::byte{0x01U}})),
				"write unexpected sidecar fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			const auto mixed = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(!mixed && mixed.error().detail == "raw-family-unresolved-topology",
					"extra topology fails closed");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint orphan raw capability");
			auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write orphan WAL fixture");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			const auto orphan = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(!orphan && orphan.error().detail == "raw-orphan-sidecar",
					"orphan sidecar fails closed");
		}
	}

	struct wal_unlink_boundary_swap
	{
		int root{-1};
		int rename_status{-1};
		int create_status{-1};
	};

	void swap_wal_at_unlink_boundary(void* context) noexcept
	{
		auto& swap = *static_cast<wal_unlink_boundary_swap*>(context);
		swap.rename_status = ::renameat(swap.root, "main-wal", swap.root, "renamed-wal");
		const auto replacement = ::openat(
			swap.root, "main-wal", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (replacement < 0)
		{
			swap.create_status = -1;
			return;
		}
		swap.create_status = ::close(replacement);
	}

	struct main_unlink_boundary_swap
	{
		int root{-1};
		int rename_status{-1};
		int create_status{-1};
	};

	void swap_main_at_unlink_boundary(void* context) noexcept
	{
		auto& swap = *static_cast<main_unlink_boundary_swap*>(context);
		swap.rename_status = ::renameat(swap.root, "main", swap.root, "renamed-main");
		const auto replacement =
			::openat(swap.root, "main", O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
		if (replacement < 0)
		{
			swap.create_status = -1;
			return;
		}
		swap.create_status = ::close(replacement);
	}

	struct main_byte_drift
	{
		int root{-1};
		ssize_t write_count{-1};
		int close_status{-1};
	};

	void mutate_main_bytes_before_cleanup(void* context) noexcept
	{
		auto& drift = *static_cast<main_byte_drift*>(context);
		const auto main = ::openat(drift.root, "main", O_WRONLY | O_NOFOLLOW | O_CLOEXEC);
		if (main < 0)
			return;
		const std::byte invalid_header_version{3U};
		drift.write_count = ::pwrite(main, &invalid_header_version, 1U, 18);
		drift.close_status = ::close(main);
	}

	void exercise_fz_post_fixture_cleanup()
	{
		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint FZ-post normalization capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(enter_sqlite_disposable_qualification(*minted, normalize) ==
						sqlite_disposable_qualification_verdict::effect_not_authorized,
					"general qualification gate still rejects source normalization");
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write FZ-post main fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write FZ-post zero-byte WAL fixture");

			auto cleaned = fixture.cleanup_fz_post_wal(normalize);
			require(cleaned.has_value(),
					"FZ-post fixture cleanup succeeds only after exact checks");
			require(cleaned->before.family.family ==
							sqlite_disposable_empty_family::exact_pre_or_post_zero_wal &&
						cleaned->before.family.phase == sqlite_disposable_family_phase::post &&
						cleaned->before.wal && cleaned->before.wal->byte_count == 0U,
					"FZ-post cleanup records the exact zero-byte WAL precondition");
			require(cleaned->after.family.family ==
							sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar &&
						cleaned->after.family.phase == sqlite_disposable_family_phase::post &&
						!cleaned->after.wal && cleaned->after.main == cleaned->before.main,
					"FZ-post cleanup returns a stable rollback-empty anchor observation");
			const auto repeated = fixture.cleanup_fz_post_wal(normalize);
			require(!repeated && repeated.error().detail == "normalization-capability-consumed",
					"successful cleanup consumes the normalization capability");
			sqlite_disposable_fixture second_fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			require(static_cast<bool>(second_fixture.write(setup, "main-wal", {})),
					"recreate zero WAL to probe cross-adapter replay");
			const auto cross_adapter_replay = second_fixture.cleanup_fz_post_wal(normalize);
			require(!cross_adapter_replay &&
						cross_adapter_replay.error().detail == "normalization-capability-consumed",
					"qualification run remains consumed across fixture adapters");
			const auto revoked = minted->revoke();
			require(!revoked && revoked.error().detail == "revoke-root-not-empty",
					"revocation closes a consumed capability while fixture files remain");
			const auto replay_after_revoke = second_fixture.cleanup_fz_post_wal(normalize);
			require(!replay_after_revoke &&
						replay_after_revoke.error().detail == "normalization-capability-revoked",
					"revoked capability takes precedence over consumed fixture state");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint parent-sync fault capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write parent-sync fault main fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write parent-sync fault zero-byte WAL fixture");
			fixture.fail_cleanup_sync();
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			require(!rejected && rejected.error().detail == "normalization-parent-sync-uncertain",
					"parent sync failure is an opaque post-effect result");
			const auto repeated = fixture.cleanup_fz_post_wal(normalize);
			require(!repeated && repeated.error().detail == "normalization-capability-consumed",
					"parent sync uncertainty cannot be retried");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint FZ-pre normalization negative capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(2U))),
					"write FZ-pre normalization negative main");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write FZ-pre normalization negative WAL");
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			require(!rejected && rejected.error().detail == "normalization-fz-post-required",
					"FZ-pre never enters the FZ-post cleanup edge");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			const auto preserved = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(preserved && preserved->family.phase == sqlite_disposable_family_phase::pre &&
						preserved->wal && preserved->wal->byte_count == 0U,
					"FZ-pre rejection leaves the source fixture unchanged");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint nonzero WAL normalization negative capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write nonzero WAL normalization negative main");
			require(
				static_cast<bool>(fixture.write(setup, "main-wal", std::array{std::byte{0x01U}})),
				"write nonzero WAL normalization negative WAL");
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			require(!rejected && rejected.error().detail == "raw-nonzero-wal-unresolved",
					"nonzero WAL never enters the cleanup edge");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint mixed topology normalization negative capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write mixed topology normalization negative main");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write mixed topology normalization negative WAL");
			require(
				static_cast<bool>(fixture.write(setup, "unexpected", std::array{std::byte{0x01U}})),
				"write mixed topology normalization negative sidecar");
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			require(!rejected && rejected.error().detail == "raw-family-unresolved-topology",
					"mixed topology never enters the cleanup edge");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint binding normalization negative capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write binding normalization negative main");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write binding normalization negative WAL");
			normalize.exact_profile_digest = digest('4');
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			require(!rejected && rejected.error().detail == "normalization-capability-binding",
					"binding drift prevents any normalization effect");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			require(observe_sqlite_disposable_raw_empty_family(*minted, classify).has_value(),
					"binding rejection leaves exact source fixture readable");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint unlink-boundary normalization negative capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write unlink-boundary normalization main");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write unlink-boundary normalization WAL");
			wal_unlink_boundary_swap swap;
			swap.root = parent.open_directory("qualification-root");
			fixture.signal_before_cleanup(swap_wal_at_unlink_boundary, &swap);
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			struct stat renamed_wal = {};
			require(::fstatat(swap.root, "renamed-wal", &renamed_wal, AT_SYMLINK_NOFOLLOW) == 0,
					"renamed WAL remains observable after pre-effect rejection");
			require(::close(swap.root) == 0, "close unlink-boundary WAL fixture root");
			require(!rejected && rejected.error().detail == "normalization-wal-drift",
					"WAL rebind is rejected before the unlink boundary");
			require(swap.rename_status == 0 && swap.create_status == 0,
					"WAL rebind negative signal executed");
			auto classify = setup;
			classify.requested_effect = sqlite_disposable_requested_effect::classify_source;
			const auto unresolved = observe_sqlite_disposable_raw_empty_family(*minted, classify);
			require(!unresolved && unresolved.error().detail == "raw-family-unresolved-topology",
					"pre-effect WAL rebind remains explicitly unresolved without retry");
			const auto repeated = fixture.cleanup_fz_post_wal(normalize);
			require(!repeated && repeated.error().detail == "normalization-capability-consumed",
					"WAL drift consumes the normalization capability without retry");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint unlink-boundary main drift capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write unlink-boundary main drift fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write unlink-boundary main drift WAL");
			main_unlink_boundary_swap swap;
			swap.root = parent.open_directory("qualification-root");
			fixture.signal_before_cleanup(swap_main_at_unlink_boundary, &swap);
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			struct stat renamed_main = {};
			struct stat preserved_wal = {};
			require(::fstatat(swap.root, "renamed-main", &renamed_main, AT_SYMLINK_NOFOLLOW) == 0 &&
						::fstatat(swap.root, "main-wal", &preserved_wal, AT_SYMLINK_NOFOLLOW) == 0,
					"main drift preserves both the renamed source and WAL");
			require(::close(swap.root) == 0, "close unlink-boundary main drift root");
			require(!rejected && rejected.error().detail == "normalization-main-drift",
					"main replacement is rejected before unlinking the WAL");
			require(swap.rename_status == 0 && swap.create_status == 0,
					"main rebind negative signal executed");
			const auto repeated = fixture.cleanup_fz_post_wal(normalize);
			require(!repeated && repeated.error().detail == "normalization-capability-consumed",
					"main drift consumes the normalization capability without retry");
		}

		{
			test_parent parent;
			auto minted = mint(parent, "qualification-root");
			require(minted.has_value(), "mint same-size main drift capability");
			const auto setup = minted->no_effect_request();
			sqlite_disposable_fixture fixture{
				*minted, parent.descriptor(), "qualification-root", setup};
			auto normalize = setup;
			normalize.requested_effect = sqlite_disposable_requested_effect::normalize_source;
			require(static_cast<bool>(fixture.write(setup, "main", canonical_empty_main(1U))),
					"write same-size main drift fixture");
			require(static_cast<bool>(fixture.write(setup, "main-wal", {})),
					"write same-size main drift WAL");
			main_byte_drift drift;
			drift.root = parent.open_directory("qualification-root");
			fixture.signal_before_cleanup(mutate_main_bytes_before_cleanup, &drift);
			const auto rejected = fixture.cleanup_fz_post_wal(normalize);
			struct stat wal = {};
			require(::fstatat(drift.root, "main-wal", &wal, AT_SYMLINK_NOFOLLOW) == 0,
					"same-size main drift preserves the WAL");
			require(::close(drift.root) == 0, "close same-size main drift root");
			require(drift.write_count == 1 && drift.close_status == 0,
					"same-size main byte drift executed");
			require(!rejected && rejected.error().detail == "normalization-main-drift",
					"same-size main byte drift is phase-authentic");
			const auto repeated = fixture.cleanup_fz_post_wal(normalize);
			require(!repeated && repeated.error().detail == "normalization-capability-consumed",
					"same-size main drift consumes the normalization capability");
		}
	}

	[[nodiscard]] sqlite_disposable_empty_family_observation
	family_observation(const sqlite_disposable_main_header_state header,
					   const sqlite_disposable_wal_state wal,
					   const sqlite_disposable_journal_state journal)
	{
		return {true, true, true, true, header, wal, false, journal, false};
	}

	void exercise_receiptless_family_partition_and_routes()
	{
		const auto require_plan = [](const sqlite_disposable_empty_family_observation& observation,
									 const sqlite_disposable_empty_family expected_family,
									 const sqlite_disposable_family_phase expected_phase,
									 const sqlite_disposable_normalization_route expected_route,
									 const bool expected_zero_wal,
									 const std::string_view label)
		{
			auto classified = classify_sqlite_disposable_empty_family(observation);
			require(classified && classified->family == expected_family &&
						classified->phase == expected_phase,
					std::string{label} + " family classification");
			auto planned = plan_sqlite_disposable_empty_normalization(observation);
			require(planned && planned->family == *classified && planned->route == expected_route &&
						planned->uses_existing_zero_byte_wal == expected_zero_wal &&
						!planned->may_handoff_to_ordinary_fresh_initialization,
					std::string{label} + " qualification-only route");
		};
		const auto require_rejected =
			[](const sqlite_disposable_empty_family_observation& observation,
			   const std::string_view label)
		{
			require(!classify_sqlite_disposable_empty_family(observation),
					std::string{label} + " classifier accepted an unrecognized family");
			require(!plan_sqlite_disposable_empty_normalization(observation),
					std::string{label} + " planner selected a route after rejection");
		};

		const auto f0 = family_observation(sqlite_disposable_main_header_state::wal_empty,
										   sqlite_disposable_wal_state::absent,
										   sqlite_disposable_journal_state::absent);
		require_plan(f0,
					 sqlite_disposable_empty_family::exact_pre_no_sidecar,
					 sqlite_disposable_family_phase::pre,
					 sqlite_disposable_normalization_route::start_new_live_receipted_normalizer,
					 false,
					 "F0");

		const auto fz_pre = family_observation(sqlite_disposable_main_header_state::wal_empty,
											   sqlite_disposable_wal_state::readable_zero_byte,
											   sqlite_disposable_journal_state::absent);
		require_plan(fz_pre,
					 sqlite_disposable_empty_family::exact_pre_or_post_zero_wal,
					 sqlite_disposable_family_phase::pre,
					 sqlite_disposable_normalization_route::start_new_live_receipted_normalizer,
					 true,
					 "FZ-pre");

		const auto fz_post = family_observation(sqlite_disposable_main_header_state::rollback_empty,
												sqlite_disposable_wal_state::readable_zero_byte,
												sqlite_disposable_journal_state::absent);
		require_plan(fz_post,
					 sqlite_disposable_empty_family::exact_pre_or_post_zero_wal,
					 sqlite_disposable_family_phase::post,
					 sqlite_disposable_normalization_route::establish_rollback_empty_anchor,
					 false,
					 "FZ-post");

		const auto fp = family_observation(sqlite_disposable_main_header_state::wal_empty,
										   sqlite_disposable_wal_state::absent,
										   sqlite_disposable_journal_state::nonhot_prefix);
		require_plan(fp,
					 sqlite_disposable_empty_family::exact_pre_nonhot_journal_prefix,
					 sqlite_disposable_family_phase::pre,
					 sqlite_disposable_normalization_route::start_new_live_receipted_normalizer,
					 false,
					 "FP");

		for (const auto header : {sqlite_disposable_main_header_state::wal_empty,
								  sqlite_disposable_main_header_state::rollback_empty})
		{
			auto input =
				family_observation(header,
								   sqlite_disposable_wal_state::absent,
								   sqlite_disposable_journal_state::hot_with_exact_preimages);
			require_plan(input,
						 sqlite_disposable_empty_family::valid_hot_journal_with_exact_preimages,
						 sqlite_disposable_family_phase::pre_or_post,
						 sqlite_disposable_normalization_route::start_new_live_receipted_normalizer,
						 false,
						 header == sqlite_disposable_main_header_state::wal_empty ? "FH-pre"
																				  : "FH-post");
		}

		const auto fi =
			family_observation(sqlite_disposable_main_header_state::rollback_empty,
							   sqlite_disposable_wal_state::absent,
							   sqlite_disposable_journal_state::invalidated_with_exact_post);
		require_plan(fi,
					 sqlite_disposable_empty_family::invalidated_journal_with_exact_post,
					 sqlite_disposable_family_phase::post,
					 sqlite_disposable_normalization_route::establish_rollback_empty_anchor,
					 false,
					 "FI");

		const auto fo = family_observation(sqlite_disposable_main_header_state::rollback_empty,
										   sqlite_disposable_wal_state::absent,
										   sqlite_disposable_journal_state::absent);
		require_plan(fo,
					 sqlite_disposable_empty_family::complete_rollback_empty_no_sidecar,
					 sqlite_disposable_family_phase::post,
					 sqlite_disposable_normalization_route::establish_rollback_empty_anchor,
					 false,
					 "FO");

		auto rejected = f0;
		rejected.source_anchor_stable = false;
		require_rejected(rejected, "unstable source anchor");
		rejected = f0;
		rejected.main_identity_stable = false;
		require_rejected(rejected, "main identity drift");
		rejected = f0;
		rejected.main_entry_stable = false;
		require_rejected(rejected, "main entry drift");
		rejected = f0;
		rejected.exact_logical_empty = false;
		require_rejected(rejected, "non-empty logical projection");
		rejected = f0;
		rejected.shared_memory_present = true;
		require_rejected(rejected, "mixed SHM topology");
		rejected = f0;
		rejected.other_sidecar_present = true;
		require_rejected(rejected, "mixed sidecar topology");
		rejected = f0;
		rejected.wal = sqlite_disposable_wal_state::invalid_or_unknown;
		require_rejected(rejected, "unknown WAL state");
		rejected = f0;
		rejected.wal = sqlite_disposable_wal_state::readable_nonzero;
		require_rejected(rejected, "non-empty WAL state");
		rejected = f0;
		rejected.journal = sqlite_disposable_journal_state::invalid_or_unknown;
		require_rejected(rejected, "unknown journal state");
		rejected = family_observation(sqlite_disposable_main_header_state::rollback_empty,
									  sqlite_disposable_wal_state::absent,
									  sqlite_disposable_journal_state::nonhot_prefix);
		require_rejected(rejected, "nonhot journal with rollback header");
		rejected = f0;
		rejected.wal = sqlite_disposable_wal_state::readable_zero_byte;
		rejected.journal = sqlite_disposable_journal_state::nonhot_prefix;
		require_rejected(rejected, "mixed zero WAL and journal");
		rejected = f0;
		rejected.wal = sqlite_disposable_wal_state::absent;
		rejected.journal = sqlite_disposable_journal_state::invalidated_with_exact_post;
		require_rejected(rejected, "invalidated journal with WAL header");
		rejected = fz_pre;
		rejected.main_header = static_cast<sqlite_disposable_main_header_state>(255U);
		require_rejected(rejected, "unknown main header with zero-byte WAL");
		rejected = family_observation(static_cast<sqlite_disposable_main_header_state>(255U),
									  sqlite_disposable_wal_state::absent,
									  sqlite_disposable_journal_state::hot_with_exact_preimages);
		require_rejected(rejected, "unknown main header with hot journal");
		rejected = f0;
		rejected.main_header = static_cast<sqlite_disposable_main_header_state>(255U);
		require_rejected(rejected, "unknown main header state");
	}
#endif
} // namespace

int main()
{
#if defined(__linux__) && defined(STATX_MNT_ID)
	exercise_move_revoke_and_fresh_run();
	exercise_new_leaf_requirement();
	exercise_binding_rejections();
	exercise_binding_input_validation();
	exercise_effect_denial_and_contamination();
	exercise_root_rebind();
	exercise_retained_parent_lifetime_and_destructor();
	exercise_fixture_adapter_capability_liveness();
	exercise_raw_empty_family_observation();
	exercise_fz_post_fixture_cleanup();
	exercise_receiptless_family_partition_and_routes();
#else
	auto unavailable = duplicate_sqlite_disposable_parent_directory(-1);
	require(!unavailable, "unsupported platform fails closed");
#endif
	std::cout << "sqlite disposable qualification tests passed\n";
	return 0;
}
