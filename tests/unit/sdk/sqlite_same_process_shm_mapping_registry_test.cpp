#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iostream>

#include <sys/wait.h>
#include <unistd.h>

#include "../../support/sqlite_same_process_shm_product_fixture.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::test_support;

	void verify_product_alias_family_lifecycle_and_bounds()
	{
		sqlite_same_process_shm_product_fixture fixture{10U};
		auto& registry = fixture.registry();
		const auto initial = registry.snapshot();
		require_shm(initial.process_live && !initial.registry_quarantined &&
						initial.registered_alias_count == 1U && initial.active_family_count == 1U &&
						initial.active_family_pin_count == 1U && initial.cohort_count == 1U &&
						initial.generation_source_count == 1U &&
						initial.reader_lifecycle_sequence_source_count == 1U,
					"production alias installs one bounded family and identity source");

		for (std::size_t index = 0; index < 64U; ++index)
		{
			sqlite_same_process_shm_product_fixture joined{static_cast<std::uint8_t>(100U + index),
														   10U};
			require_shm(joined.family() == fixture.family(), "second alias joins exact family key");
			const auto live = registry.snapshot();
			require_shm(live.family_record_count == 1U && live.active_family_count == 1U &&
							live.active_family_pin_count == 2U,
						"family joins share one coordinator and add one bounded pin");
		}

		const auto bounded = registry.snapshot();
		require_shm(bounded.family_record_count == 1U && bounded.active_family_count == 1U &&
						bounded.active_family_pin_count == 1U &&
						bounded.registered_alias_count == initial.registered_alias_count,
					"repeated joins leave no registry growth or partial visibility");
		const auto family = registry.family_snapshot(fixture.family());
		require_shm(family.exact_active_match_count == 1U && family.coordinator_present &&
						family.lookup_visible && family.alias_pin_count == 1U,
					"family snapshot exposes only the exact singleton coordinator");
	}

	void verify_cross_binding_rejection_is_zero_effect()
	{
		sqlite_same_process_shm_product_fixture fixture{11U};
		auto& registry = fixture.registry();
		const auto before = registry.snapshot();
		auto foreign = fixture.family();
		foreign.process_instance = shm_identity("test.registry.foreign-process", 11U);
		auto rejected = sqlite_same_process_shm_vfs_alias_registration_port::install_or_join_family(
			fixture.alias(), foreign);
		require_shm(!rejected.has_value(), "reject foreign process/family binding");
		const auto after = registry.snapshot();
		require_shm(after.active_family_count == before.active_family_count &&
						after.active_family_pin_count == before.active_family_pin_count &&
						after.family_record_count == before.family_record_count &&
						after.registered_alias_count == before.registered_alias_count &&
						fixture.native_register_calls() == 1U &&
						fixture.native_unregister_calls() == 0U,
					"cross-binding rejection has no native or partially visible family effect");
	}

	void verify_unregister_waits_for_exact_custody_drain()
	{
		sqlite_same_process_shm_product_fixture fixture{12U};
		auto waiting = fixture.alias().unregister_alias();
		require_shm(
			!waiting.has_value() &&
				waiting.error().reason == sqlite_shm_lease_rejection_reason::retiring &&
				waiting.error().action ==
					sqlite_shm_lease_recovery_action::await_complete_attachment_gate_boundary &&
				fixture.alias().valid() && fixture.native_unregister_calls() == 0U,
			"live family custody prevents native unregister without consuming the owner");
		require_shm(fixture.registry().release_family(fixture.family_pin()).has_value(),
					"release exact family custody");
		require_shm(fixture.alias().unregister_alias().has_value() &&
						fixture.native_unregister_calls() == 1U && !fixture.alias().valid(),
					"drained alias performs its one native unregister");
		auto replay = fixture.alias().unregister_alias();
		require_shm(!replay.has_value() && fixture.native_unregister_calls() == 1U,
					"unregister completion is nonreplayable");
		const auto final = fixture.registry().snapshot();
		require_shm(final.active_family_count == 0U && final.registered_alias_count == 0U &&
						final.detached_alias_tombstone_count >= 1U,
					"released family and alias are visible only as retired identity");
	}

	void verify_alias_epoch_prevents_aba_reuse()
	{
		std::optional<sqlite_backend_opaque_identity> old_alias;
		std::optional<sqlite_backend_opaque_identity> old_registration;
		{
			sqlite_same_process_shm_product_fixture first{13U};
			old_alias = first.alias().alias_lifetime();
			old_registration = first.alias().registration_epoch();
		}
		{
			sqlite_same_process_shm_product_fixture successor{13U};
			require_shm(successor.alias().alias_lifetime() != *old_alias &&
							successor.alias().registration_epoch() != *old_registration,
						"same name/address lifecycle receives fresh alias and registration epochs");
			const auto snapshot = successor.registry().snapshot();
			require_shm(snapshot.registered_alias_count == 1U &&
							snapshot.detached_alias_tombstone_count >= 1U &&
							snapshot.active_family_count == 1U,
						"fresh alias is active without resurrecting retired ABA identity");
		}
	}

	void verify_fork_stales_inherited_registry_owners()
	{
		sqlite_same_process_shm_product_fixture fixture{14U};
		const auto child = ::fork();
		require_shm(child >= 0, "fork registry owners");
		if (child == 0)
		{
			::alarm(5U);
			auto rejected =
				sqlite_same_process_shm_vfs_alias_registration_port::install_or_join_family(
					fixture.alias(), fixture.family());
			int stale_code{};
			if (fixture.process().valid())
				stale_code |= 1;
			if (rejected.has_value())
				stale_code |= 2;
			if (fixture.native_unregister_calls() != 0U)
				stale_code |= 4;
			{
				auto stale_alias = std::move(fixture.alias());
				auto stale_family = std::move(fixture.family_pin());
				(void)stale_alias;
				(void)stale_family;
			}
			::alarm(0U);
			::_exit(stale_code);
		}

		int status{};
		require_shm(::waitpid(child, &status, 0) == child, "wait registry child");
		require_shm(WIFEXITED(status) && WEXITSTATUS(status) == 0,
					"child stale code " +
						std::to_string(WIFEXITED(status) ? WEXITSTATUS(status) : 255) + " signal " +
						std::to_string(WIFSIGNALED(status) ? WTERMSIG(status) : 0));
		const auto parent = fixture.registry().snapshot();
		require_shm(fixture.process().valid() && fixture.alias().valid() &&
						fixture.family_pin().valid() && parent.process_live &&
						parent.active_family_count == 1U,
					"child invalidation leaves parent registry ownership unchanged");
	}
} // namespace

int main()
{
	try
	{
		verify_product_alias_family_lifecycle_and_bounds();
		verify_cross_binding_rejection_is_zero_effect();
		verify_unregister_waits_for_exact_custody_drain();
		verify_alias_epoch_prevents_aba_reuse();
		verify_fork_stales_inherited_registry_owners();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
