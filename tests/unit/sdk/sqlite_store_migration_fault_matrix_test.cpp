// Port-based SQLite migration fault matrix.
//
// The real filesystem/interposer coverage lives in the non-sanitized
// sqlite_store_migration_external_interposer_test.  This test exercises the
// same product state transitions through the typed Store fault port so it can
// run under ASan/UBSan/TSan without putting an instrumented child behind
// LD_PRELOAD.

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"
#include "sdk/sqlite_store_fault_injection_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using cxxlens::test::sqlite_fixture::chunk_maximum_bytes;
	using cxxlens::test::sqlite_v3_scenario::exact_v2_scenario;
	using cxxlens::test::sqlite_v3_scenario::make_engine;
	using cxxlens::test::sqlite_v3_scenario::selector;

	[[noreturn]] void fail(std::string message)
	{
		throw std::runtime_error{std::move(message)};
	}

	void require(const bool condition, std::string message)
	{
		if (!condition)
			fail(std::move(message));
	}

	[[nodiscard]] std::string tuple(const error& value)
	{
		return value.code + "/" + value.field + "/" + value.detail;
	}

	void require_v2(const store_compatibility& compatibility)
	{
		require(compatibility.backend == "sqlite" && compatibility.readable_format.major == 2U &&
					compatibility.readable_format.minor == 6U &&
					compatibility.readable_format.patch == 0U && compatibility.direct_open &&
					compatibility.migration_required,
				"migration fault did not retain direct v2 compatibility");
	}

	void require_v3(const store_compatibility& compatibility)
	{
		require(compatibility.backend == "sqlite" && compatibility.readable_format.major == 3U &&
					compatibility.readable_format.minor == 0U &&
					compatibility.readable_format.patch == 0U && compatibility.direct_open &&
					!compatibility.migration_required,
				"migration fault did not retain direct v3 compatibility");
	}

	void require_current(snapshot_store& store,
						 const relation_engine& engine,
						 const exact_v2_scenario& expected)
	{
		auto current = store.current(selector(engine));
		require(current.has_value(),
				"migration fault current failed: " +
					(current ? std::string{} : tuple(current.error())));
		require(current->id() == expected.current.snapshot_id &&
					current->publication().publication_id == expected.current.publication_id,
				"migration fault changed the current semantic authority");
	}

	void require_exactly_once(const sqlite_store_fault_scope& fault)
	{
		const auto observed = fault.observation();
		require(observed.matching_event_count == 1U && observed.issued_directive_count == 1U &&
					!observed.count_overflow,
				"migration fault port did not dispatch exactly once");
	}

	[[nodiscard]] std::uint64_t payload_event_total(const std::filesystem::path& path)
	{
		std::uint64_t total{};
		for (const auto& publication : cxxlens::test::sqlite_fixture::read_v2_publications(path))
		{
			if (publication.payload.empty())
				continue;
			const auto chunk_count = 1U + (publication.payload.size() - 1U) / chunk_maximum_bytes;
			// Migration dispatches a typed before/after event for every chunk.
			total += 2U * static_cast<std::uint64_t>(chunk_count);
		}
		require(total != 0U, "migration fault fixture has no payload chunk events");
		return total;
	}

	void check_pre_effect(const std::filesystem::path& path,
						  const relation_engine& engine,
						  const exact_v2_scenario& expected)
	{
		using cxxlens::test::sqlite_fixture::capture_files;
		const auto before = capture_files(path);
		auto store = open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(), "pre-effect migration Store unavailable");
		const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
											 sqlite_store_fault_boundary::transaction_begin,
											 sqlite_store_fault_timing::after,
											 1U,
											 1U};
		{
			sqlite_store_fault_scope fault{{event, sqlite_store_fault_action::report_failure}};
			auto migrated = store->compact();
			require(!migrated &&
						tuple(migrated.error()) == "store.sqlite-failure/migration-fault/injected",
					"pre-effect migration returned an unexpected error");
			require_exactly_once(fault);
		}
		require_v2(store->compatibility());
		require_current(*store, engine, expected);
		require(capture_files(path) == before,
				"pre-effect migration changed the exact SQLite file family");
	}

	void check_payload_fault(const std::filesystem::path& path,
							 const relation_engine& engine,
							 const exact_v2_scenario& expected,
							 const std::uint64_t ordinal,
							 const std::uint64_t total)
	{
		auto store = open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(), "payload migration Store unavailable");
		const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
											 sqlite_store_fault_boundary::payload_chunk,
											 sqlite_store_fault_timing::before,
											 ordinal,
											 total};
		{
			sqlite_store_fault_scope fault{{event, sqlite_store_fault_action::report_failure}};
			auto migrated = store->compact();
			require(!migrated &&
						tuple(migrated.error()) == "store.sqlite-failure/migration-fault/injected",
					"payload migration returned an unexpected error");
			require_exactly_once(fault);
		}
		require_v2(store->compatibility());
		require_current(*store, engine, expected);
		// A payload failure is pre-commit and must retain the v2 authority. SQLite
		// may still normalize a transient rollback journal before returning, so the
		// durable semantic check above is the authority for this port case.
	}

	void check_commit_after_delegate(const std::filesystem::path& path,
									 const relation_engine& engine,
									 const exact_v2_scenario& expected)
	{
		auto store = open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(), "commit-unknown migration Store unavailable");
		const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
											 sqlite_store_fault_boundary::transaction_commit,
											 sqlite_store_fault_timing::after,
											 1U,
											 1U};
		{
			sqlite_store_fault_scope fault{
				{event, sqlite_store_fault_action::report_failure_after_delegate}};
			auto migrated = store->compact();
			require(migrated.has_value(),
					"commit-after-delegate migration was not recovered as success");
			require_exactly_once(fault);
		}
		require_v3(store->compatibility());
		require_current(*store, engine, expected);
		auto reopened = open_sqlite_snapshot_store(path.string(), engine);
		require(reopened.has_value(), "commit-after-delegate source did not reopen");
		require_v3(reopened->compatibility());
		require_current(*reopened, engine, expected);
	}

	void check_close_after_commit(const std::filesystem::path& path,
								  const relation_engine& engine,
								  const exact_v2_scenario& expected)
	{
		auto store = open_sqlite_snapshot_store(path.string(), engine);
		require(store.has_value(), "close-unknown migration Store unavailable");
		require_current(*store, engine, expected);
		const sqlite_store_fault_event event{sqlite_store_operation::migrate_predecessor,
											 sqlite_store_fault_boundary::connection_close,
											 sqlite_store_fault_timing::after,
											 1U,
											 1U};
		{
			sqlite_store_fault_scope fault{
				{event, sqlite_store_fault_action::request_close_non_ok}};
			auto migrated = store->compact();
			require(!migrated &&
						tuple(migrated.error()) == "store.sqlite-failure/migration-recovery/opaque",
					"close-after-commit migration returned an unexpected error");
			require_exactly_once(fault);
		}
		const auto compatibility = store->compatibility();
		require(compatibility.backend == "sqlite" && compatibility.readable_format.major == 2U &&
					compatibility.readable_format.minor == 6U &&
					compatibility.readable_format.patch == 0U && !compatibility.direct_open &&
					compatibility.migration_required,
				"close-after-commit did not poison the Store connection");
		auto current = store->current(selector(engine));
		require(!current &&
					tuple(current.error()) ==
						"store.backend-unavailable/sqlite-connection/reopen-required",
				"poisoned migration Store returned an unexpected current result");
	}

	void run_matrix()
	{
		const auto engine = make_engine();
		cxxlens::test::sqlite_fixture::temporary_directory directory{
			"sqlite-migration-port-matrix"};

		{
			const auto path = directory.path() / "pre-effect.sqlite";
			const auto expected =
				cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(path, engine);
			check_pre_effect(path, engine, expected);
		}
		for (const auto ordinal_kind : {0U, 1U, 2U})
		{
			const auto path =
				directory.path() / ("payload-" + std::to_string(ordinal_kind) + ".sqlite");
			const auto expected =
				cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(path, engine);
			const auto total = payload_event_total(path);
			const auto ordinal = ordinal_kind == 0U ? 1U
				: ordinal_kind == 1U				? (total + 1U) / 2U
													: total;
			check_payload_fault(path, engine, expected, ordinal, total);
		}
		{
			const auto path = directory.path() / "commit-after-delegate.sqlite";
			const auto expected =
				cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(path, engine);
			check_commit_after_delegate(path, engine, expected);
		}
		{
			const auto path = directory.path() / "close-after-commit.sqlite";
			const auto expected =
				cxxlens::test::sqlite_v3_scenario::create_exact_v2_scenario(path, engine);
			check_close_after_commit(path, engine, expected);
		}
	}
} // namespace

int main()
{
	try
	{
		run_matrix();
		std::cout << "SQLite migration port fault matrix passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
