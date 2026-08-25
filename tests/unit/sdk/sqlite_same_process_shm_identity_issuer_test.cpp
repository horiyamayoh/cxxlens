#include <cstdint>
#include <exception>
#include <iostream>

#include <sys/wait.h>
#include <unistd.h>

#include "../../support/sqlite_same_process_shm_product_fixture.hpp"
#include "sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::test_support;

	[[nodiscard]] sqlite_shm_reader_lifecycle_identity_scope
	make_scope(sqlite_same_process_shm_product_fixture& fixture,
			   const sqlite_shm_reader_lifecycle_owner_kind kind,
			   const std::uint64_t owner_token,
			   const std::uint8_t marker)
	{
		return sqlite_shm_reader_lifecycle_production_factory::seal_scope(
			fixture.registry(),
			fixture.family_pin(),
			shm_identity("test.identity-issuer.callback-cohort", marker),
			shm_identity("test.identity-issuer.request", marker),
			1U,
			kind,
			owner_token,
			1U);
	}

	[[nodiscard]] sqlite_shm_issued_reader_callback_identity
	issue_callback(sqlite_shm_process_global_identity_issuer& issuer,
				   sqlite_shm_reader_lifecycle_identity_scope& scope,
				   const std::uint8_t marker)
	{
		auto permit = issuer.reserve_callback(scope,
											  sqlite_shm_reader_callback_identity_role::map,
											  shm_identity("test.identity-issuer.thread", marker),
											  0U);
		require_shm(permit.has_value(), "reserve callback identity");
		auto callback =
			issuer.seal_callback(*permit, scope, sqlite_shm_reader_callback_identity_role::map);
		require_shm(callback.has_value(), "seal callback identity");
		return std::move(*callback);
	}

	void verify_nonreplayable_cross_bound_identity()
	{
		sqlite_same_process_shm_product_fixture fixture{1U};
		auto issuer =
			sqlite_shm_reader_lifecycle_production_factory::identity_issuer(fixture.registry());
		auto scope = make_scope(fixture, sqlite_shm_reader_lifecycle_owner_kind::map, 1U, 1U);
		auto foreign = make_scope(fixture, sqlite_shm_reader_lifecycle_owner_kind::map, 2U, 2U);
		require_shm(issuer.valid() && scope.valid() && foreign.valid(),
					"production issuer and scopes are live");

		auto callback = issue_callback(issuer, scope, 1U);
		auto effect = issuer.issue_effect(
			scope, callback, sqlite_shm_reader_effect_identity_role::mapped_result);
		require_shm(effect.has_value(), "issue mapped effect identity");
		require_shm(
			issuer
				.validate_effect(
					scope, callback, *effect, sqlite_shm_reader_effect_identity_role::mapped_result)
				.has_value(),
			"validate exact effect lineage");
		require_shm(!issuer
						 .validate_effect(foreign,
										  callback,
										  *effect,
										  sqlite_shm_reader_effect_identity_role::mapped_result)
						 .has_value(),
					"reject cross-owner effect presentation");
		require_shm(!issuer
						 .issue_effect(
							 scope, callback, sqlite_shm_reader_effect_identity_role::mapped_result)
						 .has_value(),
					"reject duplicate effect role");
		require_shm(
			!issuer.retire_callback(scope, callback, sqlite_shm_reader_callback_identity_role::map)
				 .has_value(),
			"retain callback custody while effect is live");
		require_shm(
			issuer.retire_effect(scope,
								 callback,
								 *effect,
								 sqlite_shm_reader_effect_identity_role::mapped_result)
					.has_value() &&
				issuer
					.retire_callback(scope, callback, sqlite_shm_reader_callback_identity_role::map)
					.has_value() &&
				issuer.retire_scope(scope).has_value(),
			"drain effect, callback, and scope in dependency order");
		require_shm(!scope.valid() && !callback.valid() && !effect->valid(),
					"retirement invalidates every presenter");
		require_shm(
			!issuer
				 .validate_callback(scope, callback, sqlite_shm_reader_callback_identity_role::map)
				 .has_value(),
			"retired callback cannot replay");
		require_shm(issuer.retire_scope(foreign).has_value(), "retire independent scope");
	}

	void verify_zero_effect_rejection_and_drop_custody()
	{
		sqlite_same_process_shm_product_fixture fixture{2U};
		auto issuer =
			sqlite_shm_reader_lifecycle_production_factory::identity_issuer(fixture.registry());
		auto scope = make_scope(fixture, sqlite_shm_reader_lifecycle_owner_kind::map, 3U, 3U);
		auto rejected =
			issuer.reserve_callback(scope, sqlite_shm_reader_callback_identity_role::map, {}, 0U);
		require_shm(!rejected.has_value(), "reject missing thread identity before minting");
		{
			auto callback = issue_callback(issuer, scope, 3U);
			auto zero = issuer.issue_effect(
				scope, callback, sqlite_shm_reader_effect_identity_role::zero_attachment_result);
			require_shm(zero.has_value(), "valid zero-effect identity follows rejected input");
		}
		require_shm(issuer.retire_scope(scope).has_value(),
					"presenter drop drains callback and zero-effect custody");

		auto session = make_scope(fixture, sqlite_shm_reader_lifecycle_owner_kind::session, 4U, 4U);
		auto terminal = issuer.issue_session_terminal(
			session, sqlite_shm_reader_session_terminal_identity_role::success);
		require_shm(terminal.has_value() &&
						issuer
							.validate_session_terminal(
								session,
								*terminal,
								sqlite_shm_reader_session_terminal_identity_role::success)
							.has_value(),
					"session terminal is exact and independently typed");
		require_shm(
			issuer.retire_session_terminal(
					  session, *terminal, sqlite_shm_reader_session_terminal_identity_role::success)
					.has_value() &&
				issuer.retire_scope(session).has_value(),
			"retire session terminal exactly once");
	}

	void verify_fork_stales_child_without_mutating_parent()
	{
		sqlite_same_process_shm_product_fixture fixture{5U};
		auto issuer =
			sqlite_shm_reader_lifecycle_production_factory::identity_issuer(fixture.registry());
		auto scope = make_scope(fixture, sqlite_shm_reader_lifecycle_owner_kind::map, 5U, 5U);
		auto callback = issue_callback(issuer, scope, 5U);
		auto effect = issuer.issue_effect(
			scope, callback, sqlite_shm_reader_effect_identity_role::mapped_result);
		require_shm(effect.has_value(), "issue live identity graph before fork");

		const auto child = ::fork();
		require_shm(child >= 0, "fork identity graph");
		if (child == 0)
		{
			::alarm(5U);
			const bool stale = !fixture.process().valid() && !issuer.valid() && !scope.valid() &&
				!callback.valid() && !effect->valid();
			{
				auto stale_scope = std::move(scope);
				auto stale_callback = std::move(callback);
				auto stale_effect = std::move(*effect);
				(void)stale_scope;
				(void)stale_callback;
				(void)stale_effect;
			}
			::alarm(0U);
			::_exit(stale ? 0 : 1);
		}

		int status{};
		require_shm(::waitpid(child, &status, 0) == child, "wait identity child");
		require_shm(WIFEXITED(status) && WEXITSTATUS(status) == 0,
					"child rejects inherited PID/epoch graph and destroys stale owners");
		require_shm(issuer.valid() && scope.valid() && callback.valid() && effect->valid(),
					"child invalidation leaves parent custody live");
		require_shm(
			issuer.retire_effect(scope,
								 callback,
								 *effect,
								 sqlite_shm_reader_effect_identity_role::mapped_result)
					.has_value() &&
				issuer
					.retire_callback(scope, callback, sqlite_shm_reader_callback_identity_role::map)
					.has_value() &&
				issuer.retire_scope(scope).has_value(),
			"parent drains exact custody after child exit");
	}
} // namespace

int main()
{
	try
	{
		verify_nonreplayable_cross_bound_identity();
		verify_zero_effect_rejection_and_drop_custody();
		verify_fork_stales_child_without_mutating_parent();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
