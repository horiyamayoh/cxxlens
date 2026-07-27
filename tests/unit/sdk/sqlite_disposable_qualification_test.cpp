#include <array>
#include <concepts>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#if defined(__linux__)
#include <cerrno>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cxxlens/sdk/store.hpp>

#include "sdk/sqlite_disposable_qualification_internal.hpp"

namespace
{
	using namespace cxxlens::detail::sqlite_qualification;

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
										 "nonempty-after-mint"})
				{
					const auto child = ::openat(
						descriptor_, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
					if (child >= 0)
					{
						(void)::unlinkat(child, "payload", 0);
						(void)::unlinkat(child, "unexpected", 0);
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

	void exercise_process_instance_invalidation()
	{
		test_parent parent;
		auto minted = mint(parent, "qualification-root");
		require(minted.has_value(), "mint process-instance capability");
		const auto exact = minted->no_effect_request();
		invalidate_sqlite_disposable_process_instance_for_testing(*minted);
		require(enter_sqlite_disposable_qualification(*minted, exact) ==
					sqlite_disposable_qualification_verdict::wrong_creator_process,
				"lost process-instance proof rejects entry even when PID still matches");
		const auto revoked = minted->revoke();
		require(!revoked && revoked.error().detail == "revoke-wrong-process",
				"lost process-instance proof cannot clean by PID alone");
		require(!parent.entry_absent("qualification-root"),
				"process-instance failure preserves creator root");
		parent.remove_directory("qualification-root");
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
					"slice one rejects source effect");
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

	struct unlink_boundary_swap
	{
		int parent{-1};
		int rename_status{-1};
		int create_status{-1};
	};

	void swap_root_at_unlink_boundary(void* context) noexcept
	{
		auto& swap = *static_cast<unlink_boundary_swap*>(context);
		swap.rename_status =
			::renameat(swap.parent, "rebound-root", swap.parent, "renamed-original");
		swap.create_status = ::mkdirat(swap.parent, "rebound-root", 0700);
	}

	void exercise_unlink_boundary_rebind()
	{
		test_parent parent;
		auto minted = mint(parent, "rebound-root");
		require(minted.has_value(), "mint unlink-boundary capability");
		unlink_boundary_swap swap{parent.descriptor()};
		set_sqlite_disposable_pre_remove_signal_for_testing(
			*minted, swap_root_at_unlink_boundary, &swap);
		const auto revoked = minted->revoke();
		require(!revoked, "unlink-boundary replacement never reports cleanup success");
		require(revoked.error().detail == "revoke-remove-identity-opaque",
				"unlink-boundary replacement is post-effect identity opaque");
		require(swap.rename_status == 0 && swap.create_status == 0,
				"unlink-boundary swap executed");
		require(parent.entry_absent("rebound-root"),
				"replacement path effect is observed but not treated as exact cleanup");
		require(!parent.entry_absent("renamed-original"),
				"held original remains preserved after unlink-boundary swap");
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
#endif
} // namespace

int main()
{
#if defined(__linux__) && defined(STATX_MNT_ID)
	exercise_move_revoke_and_fresh_run();
	exercise_new_leaf_requirement();
	exercise_binding_rejections();
	exercise_binding_input_validation();
	exercise_process_instance_invalidation();
	exercise_effect_denial_and_contamination();
	exercise_root_rebind();
	exercise_unlink_boundary_rebind();
	exercise_retained_parent_lifetime_and_destructor();
#else
	auto unavailable = duplicate_sqlite_disposable_parent_directory(-1);
	require(!unavailable, "unsupported platform fails closed");
#endif
	std::cout << "sqlite disposable qualification tests passed\n";
	return 0;
}
