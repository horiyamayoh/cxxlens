#include <cstdint>
#include <exception>
#include <iostream>
#include <optional>

#include <sys/wait.h>
#include <unistd.h>

#include "../../support/sqlite_same_process_shm_product_fixture.hpp"
#include "sdk/sqlite_same_process_shm_identity_issuer_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::test_support;

	[[nodiscard]] sqlite_shm_callback_execution_receipt callback(const std::uint8_t marker)
	{
		return {shm_identity("test.lease.thread", marker),
				0U,
				shm_identity("test.lease.invocation", marker)};
	}

	[[nodiscard]] sqlite_shm_reader_open_binding
	open_binding(sqlite_same_process_shm_product_fixture& fixture, const std::uint8_t marker)
	{
		return {fixture.family(),
				fixture.alias().alias_lifetime(),
				shm_identity("test.lease.connection", marker),
				shm_identity("test.lease.main-native-file", marker),
				shm_identity("test.lease.main-xopen", marker),
				shm_identity("test.lease.open-epoch", marker),
				shm_identity("test.lease.callback-cohort", marker),
				std::nullopt,
				fixture.alias().registration_epoch()};
	}

	[[nodiscard]] sqlite_shm_reader_open_authority
	acquire_open(sqlite_same_process_shm_product_fixture& fixture, const std::uint8_t marker)
	{
		auto open = sqlite_shm_reader_open_production_factory::acquire(
			fixture.registry(), fixture.family_pin(), open_binding(fixture, marker));
		require_shm(open.has_value(), "acquire reader open through production factory");
		return std::move(*open);
	}

	void verify_open_close_receipt_and_late_replay()
	{
		sqlite_same_process_shm_product_fixture fixture{20U};
		auto open = acquire_open(fixture, 20U);
		const auto view = sqlite_shm_reader_lifecycle_production_factory::open_epoch_view(
			fixture.registry(), open);
		require_shm(view.has_value() && view->registry_open_token != 0U &&
						view->binding.family == fixture.family() &&
						view->binding.alias_lifetime == fixture.alias().alias_lifetime() &&
						view->binding.registration_epoch == fixture.alias().registration_epoch(),
					"reader-open view preserves the exact family/alias/registration epoch");
		const auto active = fixture.registry().family_snapshot(fixture.family());
		require_shm(active.coordinator.reader_registry_open_count == 1U &&
						active.coordinator.reader_open_close_owner_count == 1U &&
						fixture.registry().snapshot().active_reader_open_count == 1U,
					"open authority is visible exactly once");
		const auto active_census = fixture.registry().reader_lifecycle_census(fixture.family());
		require_shm(active_census.has_value() && !active_census->all_drained() &&
						active_census->observed().cleanup_custody_count == 1U &&
						active_census->observed().registry_activity_count == 0U &&
						active_census->observed().liveness_uncertainty_count == 0U,
					"an open connection is production-visible as undrained close custody");

		const auto close_callback = callback(21U);
		auto close = fixture.registry().begin_reader_close(
			fixture.family_pin(), open, sqlite_shm_reader_close_request{close_callback});
		require_shm(close.has_value() && close->valid() &&
						close->route() == sqlite_shm_reader_close_route::close_without_group,
					"unmapped reader open admits direct close");
		auto terminal = sqlite_shm_reader_lifecycle_production_factory::make_close_terminal(
			*close,
			close_callback,
			sqlite_shm_reader_close_evidence_kind::exact_native_result,
			0,
			shm_identity("test.lease.close-effect", 21U));
		auto completed =
			fixture.registry().complete_reader_close(fixture.family_pin(), open, *close, terminal);
		require_shm(completed.has_value() &&
						completed->kind() == sqlite_shm_reader_close_terminal_kind::closed &&
						completed->route() == sqlite_shm_reader_close_route::close_without_group &&
						!close->valid(),
					"exact close receipt commits one terminal result");
		require_shm(fixture.registry().release_reader_open(open).has_value() && !open.valid(),
					"release closed reader-open authority");

		auto late = fixture.registry().begin_reader_close(
			fixture.family_pin(), open, sqlite_shm_reader_close_request{callback(22U)});
		auto replay =
			fixture.registry().complete_reader_close(fixture.family_pin(), open, *close, terminal);
		require_shm(!late.has_value() && !replay.has_value(),
					"late close and terminal replay cannot recreate authority");
		const auto retired = fixture.registry().family_snapshot(fixture.family());
		const auto drained = fixture.registry().reader_lifecycle_census(fixture.family());
		require_shm(retired.coordinator.reader_registry_open_count == 0U &&
						retired.coordinator.reader_open_close_owner_count == 0U &&
						retired.coordinator.reader_open_close_tombstone_count == 1U &&
						drained.has_value() && drained->all_drained() &&
						drained->observed().open_epoch_close_compact_tombstone_count == 1U,
					"closed open census " +
						std::to_string(retired.coordinator.reader_registry_open_count) + "/" +
						std::to_string(retired.coordinator.reader_open_close_owner_count) + "/" +
						std::to_string(retired.coordinator.reader_close_terminal_count) + "/" +
						std::to_string(retired.coordinator.reader_open_close_tombstone_count));
	}

	void verify_open_rejection_and_unknown_close_are_atomic()
	{
		{
			sqlite_same_process_shm_product_fixture fixture{23U};
			auto malformed = open_binding(fixture, 23U);
			malformed.registration_epoch = shm_identity("test.lease.stale-registration", 23U);
			const auto before = fixture.registry().snapshot();
			const auto before_census = fixture.registry().reader_lifecycle_census(fixture.family());
			auto rejected = sqlite_shm_reader_open_production_factory::acquire(
				fixture.registry(), fixture.family_pin(), malformed);
			const auto after = fixture.registry().snapshot();
			const auto after_census = fixture.registry().reader_lifecycle_census(fixture.family());
			require_shm(!rejected.has_value() &&
							after.active_reader_open_count == before.active_reader_open_count &&
							after.active_activity_pin_count == before.active_activity_pin_count &&
							before_census.has_value() && after_census == before_census &&
							after_census->all_drained(),
						"stale VFS registration epoch rejects before any reader-open effect");
		}

		{
			sqlite_same_process_shm_product_fixture fixture{24U};
			auto open = acquire_open(fixture, 24U);
			const auto close_callback = callback(24U);
			auto close = fixture.registry().begin_reader_close(
				fixture.family_pin(), open, sqlite_shm_reader_close_request{close_callback});
			require_shm(close.has_value(), "begin reader close before unknown native outcome");
			auto unknown = sqlite_shm_reader_lifecycle_production_factory::make_close_terminal(
				*close,
				close_callback,
				sqlite_shm_reader_close_evidence_kind::throw_or_unknown,
				std::nullopt,
				std::nullopt);
			auto terminal = fixture.registry().complete_reader_close(
				fixture.family_pin(), open, *close, unknown);
			require_shm(terminal.has_value() &&
							terminal->kind() ==
								sqlite_shm_reader_close_terminal_kind::terminal_quarantined &&
							!close->valid(),
						"unknown native close becomes one fail-closed terminal, never success");
			auto replay = fixture.registry().complete_reader_close(
				fixture.family_pin(), open, *close, unknown);
			const auto census = fixture.registry().reader_lifecycle_census(fixture.family());
			require_shm(!replay.has_value() && (!census || !census->all_drained()),
						"unknown close is nonreplayable and cannot certify drained custody");
			(void)fixture.registry().release_reader_open(open);
		}
	}

	void verify_zero_effect_shape_is_exact()
	{
		require_shm(classify_sqlite_shm_writer_extend_pair(0, 0) ==
						sqlite_shm_writer_extend_pair::zero_zero,
					"zero/zero is the only no-resize effect pair");
		require_shm(classify_sqlite_shm_writer_extend_pair(1, 1) ==
						sqlite_shm_writer_extend_pair::one_one,
					"one/one preserves the delegated resize effect");
		require_shm(!classify_sqlite_shm_writer_extend_pair(0, 1).has_value() &&
						!classify_sqlite_shm_writer_extend_pair(1, 0).has_value() &&
						!classify_sqlite_shm_writer_extend_pair(-1, 0).has_value(),
					"mismatched or invalid resize pairs never project a zero effect");
	}

	void verify_fork_stales_open_and_cleanup_is_lock_free()
	{
		sqlite_same_process_shm_product_fixture fixture{25U};
		auto open = acquire_open(fixture, 25U);
		const auto child = ::fork();
		require_shm(child >= 0, "fork live reader open");
		if (child == 0)
		{
			::alarm(5U);
			const auto rejected = fixture.process().valid() || open.valid();
			{
				auto stale_open = std::move(open);
				(void)stale_open;
			}
			::alarm(0U);
			::_exit(rejected ? 1 : 0);
		}
		int status{};
		require_shm(::waitpid(child, &status, 0) == child && WIFEXITED(status) &&
						WEXITSTATUS(status) == 0,
					"child rejects inherited open epoch and destroys it without inherited locks");
		require_shm(fixture.process().valid() && open.valid(),
					"fork child does not mutate the parent open owner");
	}
} // namespace

int main()
{
	try
	{
		verify_open_close_receipt_and_late_replay();
		verify_open_rejection_and_unknown_close_are_atomic();
		verify_zero_effect_shape_is_exact();
		verify_fork_stales_open_and_cleanup_is_lock_free();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
