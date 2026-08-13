#include <filesystem>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>

#include <cxxlens/sdk.hpp>

#include "../../support/sqlite_store_fixture.hpp"
#include "../../support/sqlite_store_v3_scenario.hpp"
#include "sdk/sqlite_limit_length_control_internal.hpp"

namespace
{
	using cxxlens::sdk::sqlite_limit_length_boundary;
	using cxxlens::sdk::sqlite_limit_length_control_scope;
	using namespace cxxlens::test::sqlite_fixture;
	using namespace cxxlens::test::sqlite_v3_scenario;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	template <class T>
	void require_error(const cxxlens::sdk::result<T>& value,
					   const std::string_view code,
					   const std::string_view field,
					   const std::string_view detail,
					   const std::string_view label)
	{
		require(!value, std::string{label} + " unexpectedly succeeded");
		require(value.error().code == code && value.error().field == field &&
					value.error().detail == detail,
				std::string{label} + " returned a different error tuple");
	}

	void require_limit_observation(const sqlite_limit_length_control_scope& scope,
								   const sqlite_limit_length_boundary boundary,
								   const std::string_view label)
	{
		const auto receipt = scope.observation();
		const auto expected = cxxlens::sdk::sqlite_limit_length_boundary_value(boundary);
		require(scope.active(), std::string{label} + " control scope was not active");
		require(receipt.requested_limit_length == expected,
				std::string{label} + " requested a different SQLite limit");
		require(receipt.observed_connection_count > 0U,
				std::string{label} + " did not observe an actual Store connection");
		require(!receipt.observation_count_overflow && receipt.all_actual_limits_exact &&
					receipt.minimum_actual_limit == expected &&
					receipt.maximum_actual_limit == expected,
				std::string{label} + " did not install the exact per-connection SQLite limit");
	}

	void require_no_limit_observation(const sqlite_limit_length_control_scope& scope,
									  const std::string_view label)
	{
		const auto receipt = scope.observation();
		require(scope.active(), std::string{label} + " control scope was not active");
		require(receipt.observed_connection_count == 0U && !receipt.observation_count_overflow,
				std::string{label} + " observed a SQLite v3 limit before its authority gate");
	}

	void require_floor_error(const auto& value, const std::string_view label)
	{
		require_error(value, "store.backend-unavailable", "sqlite-limit-length", "16777216", label);
	}

	void check_fresh_precedence()
	{
		const auto engine = make_engine();
		temporary_directory directory{"sqlite-limit-fresh"};
		const auto rejected_path = directory.path() / "below.sqlite";
		const auto before = capture_files(rejected_path);
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(rejected_path.string(), engine);
			require_floor_error(opened, "fresh one-below-floor open");
			require_limit_observation(
				scope, sqlite_limit_length_boundary::one_below_minimum, "fresh one-below-floor");
		}
		require(capture_files(rejected_path) == before,
				"fresh below-floor scratch gate crossed into target bootstrap effects");

		const auto accepted_path = directory.path() / "exact.sqlite";
		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(accepted_path.string(), engine);
			require(opened.has_value(), "fresh exact-floor Store did not open");
			require_limit_observation(
				scope, sqlite_limit_length_boundary::exact_minimum, "fresh exact-floor");
		}
		require_wal_header_and_quiescent_sidecars(accepted_path);
	}

	void check_current_v3_precedence()
	{
		const auto engine = make_engine();
		temporary_directory directory{"sqlite-limit-current"};
		const auto path = directory.path() / "current.sqlite";
		const auto expected = create_current_v3_scenario(path, engine);
		quiesce_wal_sidecars(path);
		const auto before = capture_files(path);
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			require_floor_error(opened, "current-v3 one-below-floor reopen");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::one_below_minimum,
									  "current-v3 one-below-floor");
		}
		require(capture_files(path) == before,
				"current-v3 below-floor gate changed the source file family");

		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			require(opened.has_value(), "current-v3 exact-floor Store did not reopen");
			auto current = opened->current(selector(engine));
			require(current && current->id() == expected.snapshot_id &&
						current->publication().publication_id == expected.publication_id,
					"current-v3 exact-floor reopen changed the semantic projection");
			require_limit_observation(
				scope, sqlite_limit_length_boundary::exact_minimum, "current-v3 exact-floor");
		}
	}

	void check_wal_only_precedence()
	{
		const auto engine = make_engine();
		temporary_directory directory{"sqlite-limit-wal-only"};
		const auto source_path = directory.path() / "source.sqlite";
		const auto expected = create_current_v3_scenario(source_path, engine);
		quiesce_wal_sidecars(source_path);

		const auto rejected_path = directory.path() / "below-open.sqlite";
		auto rejected = make_cold_wal_only_copy(source_path, rejected_path);
		const auto rejected_before = capture_files(rejected.path());
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(rejected.path().string(), engine);
			require_floor_error(opened, "WAL-only one-below-floor private recovery");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::one_below_minimum,
									  "WAL-only one-below-floor private recovery");
		}
		require(capture_files(rejected.path()) == rejected_before,
				"WAL-only below-floor private recovery changed the source file family");

		const auto deferred_path = directory.path() / "below-mutation.sqlite";
		auto deferred_fixture = make_cold_wal_only_copy(source_path, deferred_path);
		std::optional<cxxlens::sdk::snapshot_store> deferred;
		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(deferred_fixture.path().string(), engine);
			require(opened.has_value(), "WAL-only exact-floor eager Store did not open");
			deferred.emplace(std::move(*opened));
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::exact_minimum,
									  "WAL-only exact-floor eager open");
		}
		const auto deferred_before = capture_files(deferred_fixture.path());
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto writer =
				deferred->begin(snapshot_draft(engine, std::string{expected.publication_id}));
			require(writer.has_value(), "WAL-only mutation writer did not begin");
			require(writer->stage(small_partition(engine, true)).has_value(),
					"WAL-only mutation writer did not stage");
			require(writer->validate().has_value(), "WAL-only mutation writer did not validate");
			auto published = writer->publish();
			require_floor_error(published, "WAL-only one-below-floor first mutation");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::one_below_minimum,
									  "WAL-only one-below-floor first mutation");
		}
		deferred.reset();
		require(capture_files(deferred_fixture.path()) == deferred_before,
				"WAL-only below-floor first mutation changed the source file family");

		const auto accepted_path = directory.path() / "exact.sqlite";
		auto accepted = make_cold_wal_only_copy(source_path, accepted_path);
		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			auto opened =
				cxxlens::sdk::open_sqlite_snapshot_store(accepted.path().string(), engine);
			require(opened.has_value(), "WAL-only exact-floor Store did not open");
			auto current = opened->current(selector(engine));
			require(current && current->id() == expected.snapshot_id,
					"WAL-only exact-floor eager projection changed");
			auto next = publish_small(*opened, engine, true, std::string{expected.publication_id});
			require(next.publication().sequence == expected.sequence + 1U,
					"WAL-only exact-floor first mutation did not publish its descendant");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::exact_minimum,
									  "WAL-only exact-floor first mutation");
		}
	}

	void check_v2_compact_precedence()
	{
		const auto engine = make_engine();
		temporary_directory directory{"sqlite-limit-v2-compact"};

		const auto rejected_path = directory.path() / "below.sqlite";
		const auto expected = create_exact_v2_scenario(rejected_path, engine);
		const auto before = capture_files(rejected_path);
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(rejected_path.string(), engine);
			require(opened.has_value(), "exact-v2 below-floor diagnostic Store did not open");
			require_no_limit_observation(scope, "exact-v2 diagnostic open");
			auto current = opened->current(selector(engine));
			require(current && current->id() == expected.current.snapshot_id,
					"exact-v2 below-floor diagnostic projection changed");

			auto invalid = snapshot_draft(engine);
			invalid.series.channel_id.clear();
			auto invalid_begin = opened->begin(std::move(invalid));
			require_error(invalid_begin,
						  "store.selection-authority-incomplete",
						  "selector",
						  "",
						  "exact-v2 invalid draft precedence");
			auto migration_begin = opened->begin(snapshot_draft(engine));
			require_error(migration_begin,
						  "store.migration-required",
						  "sqlite-physical-format",
						  "cxxlens.sqlite-semantic-store.v2-to-v3",
						  "exact-v2 migration-required precedence");
			require_no_limit_observation(scope, "exact-v2 read and begin");

			auto compacted = opened->compact();
			require_floor_error(compacted, "exact-v2 one-below-floor compact");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::one_below_minimum,
									  "exact-v2 one-below-floor compact");
		}
		require(capture_files(rejected_path) == before,
				"exact-v2 below-floor compact changed the source file family");

		const auto corrupt_path = directory.path() / "corrupt.sqlite";
		const auto corrupt = create_exact_v2_scenario(corrupt_path, engine);
		rewrite_v2_checksum(
			corrupt_path,
			corrupt.current.publication_id,
			"sha256:0000000000000000000000000000000000000000000000000000000000000000");
		const auto corrupt_before = capture_files(corrupt_path);
		{
			sqlite_limit_length_control_scope scope{
				sqlite_limit_length_boundary::one_below_minimum};
			auto opened = cxxlens::sdk::open_sqlite_snapshot_store(corrupt_path.string(), engine);
			require(opened.has_value(), "corrupt exact-v2 Store did not open for diagnosis");
			auto compacted = opened->compact();
			require_error(compacted,
						  "store.compact-validation-failed",
						  corrupt.current.publication_id,
						  "",
						  "corrupt exact-v2 compact preflight");
			require_no_limit_observation(scope, "corrupt exact-v2 compact preflight");
		}
		require(capture_files(corrupt_path) == corrupt_before,
				"corrupt exact-v2 compact preflight changed the source file family");

		const auto accepted_path = directory.path() / "exact.sqlite";
		const auto accepted = create_exact_v2_scenario(accepted_path, engine);
		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			{
				auto opened =
					cxxlens::sdk::open_sqlite_snapshot_store(accepted_path.string(), engine);
				require(opened.has_value(), "exact-v2 exact-floor Store did not open");
				require_no_limit_observation(scope, "exact-v2 exact-floor diagnostic open");
				require(opened->compact().has_value(),
						"exact-v2 exact-floor compact did not migrate");
			}
			quiesce_wal_sidecars(accepted_path);
			auto reopened =
				cxxlens::sdk::open_sqlite_snapshot_store(accepted_path.string(), engine);
			require(reopened.has_value(), "migrated exact-floor Store did not cold reopen");
			auto current = reopened->current(selector(engine));
			auto exported = reopened->canonical_export(accepted.current.snapshot_id);
			require(current && current->id() == accepted.current.snapshot_id && exported &&
						*exported == accepted.current_canonical_export,
					"exact-floor migration changed the semantic/export projection");
			require_limit_observation(scope,
									  sqlite_limit_length_boundary::exact_minimum,
									  "exact-v2 exact-floor migration and reopen");
		}
	}
} // namespace

int main()
{
	try
	{
		check_fresh_precedence();
		check_current_v3_precedence();
		check_wal_only_precedence();
		check_v2_compact_precedence();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
	std::cout << "SQLite limit-length precedence tests passed\n";
	return 0;
}
