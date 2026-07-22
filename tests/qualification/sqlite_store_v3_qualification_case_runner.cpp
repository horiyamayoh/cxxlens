#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>
#include <unistd.h>

#include "sdk/sqlite_limit_length_control_internal.hpp"
#include "sdk/sqlite_payload_streaming_internal.hpp"
#include "sdk/sqlite_store_fault_injection_internal.hpp"
#include "sqlite_store_fixture.hpp"
#include "sqlite_store_v3_scenario.hpp"

#if !defined(CXXLENS_SQLITE_QUALIFICATION_CONFIGURATION) || \
	!defined(CXXLENS_SQLITE_QUALIFICATION_REVISION) || \
	!defined(CXXLENS_SQLITE_QUALIFICATION_SOURCE_TREE)
#error "SQLite qualification runner requires compiled configuration/revision/source-tree binding"
#endif

namespace
{
	using cxxlens::sdk::sqlite_limit_length_boundary;
	using cxxlens::sdk::sqlite_limit_length_control_receipt;
	using cxxlens::sdk::sqlite_limit_length_control_scope;
	using cxxlens::sdk::sqlite_payload_resident_observation_receipt;
	using cxxlens::sdk::sqlite_payload_resident_observation_scope;
	using cxxlens::sdk::sqlite_store_fault_action;
	using cxxlens::sdk::sqlite_store_fault_boundary;
	using cxxlens::sdk::sqlite_store_fault_event;
	using cxxlens::sdk::sqlite_store_fault_plan;
	using cxxlens::sdk::sqlite_store_fault_scope;
	using cxxlens::sdk::sqlite_store_fault_timing;
	using cxxlens::sdk::sqlite_store_operation;
	using namespace cxxlens::test::sqlite_fixture;
	using namespace cxxlens::test::sqlite_v3_scenario;
	constexpr std::string_view compiled_configuration = CXXLENS_SQLITE_QUALIFICATION_CONFIGURATION;
	constexpr std::string_view compiled_revision = CXXLENS_SQLITE_QUALIFICATION_REVISION;
	constexpr std::string_view compiled_source_tree = CXXLENS_SQLITE_QUALIFICATION_SOURCE_TREE;

	constexpr std::string_view current_case = "current-v3.0.0-cold-reopen";
	constexpr std::string_view v2_case = "v2.6.0-read-only-zero-mutation";
	constexpr std::string_view migration_case = "compact-v2.6.0-to-v3.0.0-same-semantic-digest";
	constexpr std::string_view limit_case =
		"limit-length-exceeding-valid-canonical-v5-reopened-parity";

	struct arguments
	{
		std::string configuration;
		std::string case_id;
		std::filesystem::path work_directory;
		std::filesystem::path output;
		int event_fd{-1};
		int control_fd{-1};
	};

	struct observed_projection
	{
		std::string snapshot_id;
		std::string publication_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
		std::string canonical_export_digest;
		std::string query_projection_digest;
	};

	struct observed_open
	{
		std::string backend;
		std::string readable_format;
		bool direct_open{};
		bool migration_required{};
		observed_projection projection;
	};

	[[noreturn]] void fail(const std::string_view message)
	{
		throw std::runtime_error{std::string{message}};
	}

	[[noreturn]] void fail(const std::string_view operation, const cxxlens::sdk::error& value)
	{
		throw std::runtime_error{std::string{operation} + ": " + value.code + '/' + value.field +
								 value.detail};
	}

	[[nodiscard]] bool exact_lower_hex_revision(const std::string_view value) noexcept
	{
		if (value.size() != 40U)
			return false;
		for (const char byte : value)
			if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
				return false;
		return true;
	}

	[[nodiscard]] int parse_fd(const std::string_view spelling, const std::string_view option)
	{
		std::size_t consumed{};
		long value{};
		try
		{
			value = std::stol(std::string{spelling}, &consumed, 10);
		}
		catch (const std::exception&)
		{
			fail(std::string{option} + " is not a file descriptor");
		}
		if (consumed != spelling.size() || value < 0 || value > 1'048'576)
			fail(std::string{option} + " is outside the accepted descriptor range");
		return static_cast<int>(value);
	}

	[[nodiscard]] arguments parse_arguments(const int argc, char** argv)
	{
		arguments output;
		for (int index = 1; index < argc; ++index)
		{
			const std::string_view option{argv[index]};
			if (index + 1 >= argc)
				fail("qualification case runner option is missing its value");
			const std::string_view value{argv[++index]};
			if (option == "--configuration")
				output.configuration = value;
			else if (option == "--case")
				output.case_id = value;
			else if (option == "--work-directory")
				output.work_directory = value;
			else if (option == "--output")
				output.output = value;
			else if (option == "--event-fd")
				output.event_fd = parse_fd(value, option);
			else if (option == "--control-fd")
				output.control_fd = parse_fd(value, option);
			else
				fail(std::string{"unknown qualification case runner option: "} +
					 std::string{option});
		}
		if ((output.configuration != "static" && output.configuration != "shared") ||
			(output.case_id != current_case && output.case_id != v2_case &&
			 output.case_id != migration_case && output.case_id != limit_case) ||
			output.work_directory.empty() || output.output.empty() || output.event_fd < 0 ||
			output.control_fd < 0)
			fail("qualification case runner arguments are incomplete");
		if (output.configuration != compiled_configuration ||
			(compiled_configuration != "static" && compiled_configuration != "shared") ||
			!exact_lower_hex_revision(compiled_revision) ||
			!exact_lower_hex_revision(compiled_source_tree))
			fail("qualification runner compiled provenance binding is invalid");
		return output;
	}

	void write_all(const int descriptor, const std::string_view bytes)
	{
		std::size_t offset{};
		while (offset < bytes.size())
		{
			const auto result = ::write(descriptor, bytes.data() + offset, bytes.size() - offset);
			if (result < 0 && errno == EINTR)
				continue;
			if (result <= 0)
				fail("qualification checkpoint event write failed");
			offset += static_cast<std::size_t>(result);
		}
	}

	void checkpoint(const arguments& options, const std::string_view name)
	{
		write_all(options.event_fd, std::string{name} + '\n');
		char acknowledgement{};
		for (;;)
		{
			const auto result = ::read(options.control_fd, &acknowledgement, 1U);
			if (result < 0 && errno == EINTR)
				continue;
			if (result != 1 || acknowledgement != 'c')
				fail("qualification parent did not acknowledge the checkpoint");
			return;
		}
	}

	[[nodiscard]] std::string json_string(const std::string_view value)
	{
		auto output = cxxlens::sdk::canonical_json_text(value);
		if (!output)
			fail("qualification JSON string", output.error());
		return std::move(*output);
	}

	[[nodiscard]] std::string digest_text(const std::string_view value)
	{
		return cxxlens::sdk::content_digest(std::as_bytes(std::span{value.data(), value.size()}));
	}

	[[nodiscard]] std::string version_text(const cxxlens::sdk::semantic_version& version)
	{
		return std::to_string(version.major) + '.' + std::to_string(version.minor) + '.' +
			std::to_string(version.patch);
	}

	void append_frame(std::string& output, const std::string_view value)
	{
		output += std::to_string(value.size());
		output.push_back(':');
		output.append(value);
	}

	[[nodiscard]] std::string query_projection_digest(const cxxlens::sdk::relation_engine& engine,
													  const cxxlens::sdk::snapshot_handle& snapshot)
	{
		if (snapshot.manifest().partitions.empty())
			fail("qualification snapshot has no relation partition");
		const auto descriptor_id = snapshot.manifest().partitions.front().relation_descriptor_id;
		auto relation = engine.require_id(descriptor_id);
		if (!relation)
			fail("qualification relation lookup", relation.error());
		auto cursor = snapshot.open(*relation);
		if (!cursor)
			fail("qualification query open", cursor.error());

		std::string digest_projection{"cxxlens.sqlite-store-v3-query-projection.v1"};
		std::uint64_t count{};
		for (;;)
		{
			auto next = cursor->next();
			if (!next)
				fail("qualification query next", next.error());
			if (!*next)
				break;
			auto row = (**next).copy();
			if (!row)
				fail("qualification query row copy", row.error());
			const auto canonical = row->canonical_form();
			append_frame(digest_projection, digest_text(canonical));
			++count;
		}
		append_frame(digest_projection, std::to_string(count));
		return digest_text(digest_projection);
	}

	[[nodiscard]] observed_projection
	capture_observed_projection(cxxlens::sdk::snapshot_store& store,
								const cxxlens::sdk::relation_engine& engine,
								const cxxlens::sdk::snapshot_handle& snapshot)
	{
		auto exported = store.canonical_export(snapshot.id());
		if (!exported)
			fail("qualification canonical export", exported.error());
		return {std::string{snapshot.id()},
				std::string{snapshot.publication().publication_id},
				snapshot.publication().sequence,
				snapshot.publication().physical_generation,
				digest_text(*exported),
				query_projection_digest(engine, snapshot)};
	}

	[[nodiscard]] observed_open capture_current(cxxlens::sdk::snapshot_store& store,
												const cxxlens::sdk::relation_engine& engine)
	{
		auto current = store.current(selector(engine));
		if (!current)
			fail("qualification Store current", current.error());
		const auto compatibility = store.compatibility();
		return {compatibility.backend,
				version_text(compatibility.readable_format),
				compatibility.direct_open,
				compatibility.migration_required,
				capture_observed_projection(store, engine, *current)};
	}

	[[nodiscard]] std::string projection_json(const observed_projection& value)
	{
		return "{\"snapshot_id\":" + json_string(value.snapshot_id) +
			",\"publication_id\":" + json_string(value.publication_id) +
			",\"sequence\":" + std::to_string(value.sequence) +
			",\"physical_generation\":" + std::to_string(value.physical_generation) +
			",\"canonical_export_digest\":" + json_string(value.canonical_export_digest) +
			",\"query_projection_digest\":" + json_string(value.query_projection_digest) + '}';
	}

	[[nodiscard]] std::string open_json(const observed_open& value)
	{
		return "{\"backend\":" + json_string(value.backend) +
			",\"readable_format\":" + json_string(value.readable_format) +
			",\"direct_open\":" + (value.direct_open ? "true" : "false") +
			",\"migration_required\":" + (value.migration_required ? "true" : "false") +
			",\"projection\":" + projection_json(value.projection) + '}';
	}

	void write_result(const arguments& options,
					  const std::string_view observations,
					  const std::string_view receipts)
	{
		const auto relative_database = std::filesystem::path{"database"} / "database.sqlite";
		const std::string document =
			"{\"schema\":\"cxxlens.sqlite-store-v3-case-result.v1\",\"configuration\":" +
			json_string(options.configuration) + ",\"case_id\":" + json_string(options.case_id) +
			",\"compiled_provenance\":{\"configuration\":" + json_string(compiled_configuration) +
			",\"revision\":" + json_string(compiled_revision) +
			",\"source_tree\":" + json_string(compiled_source_tree) + '}' +
			",\"database_relative_path\":" + json_string(relative_database.generic_string()) +
			",\"observations\":" + std::string{observations} +
			",\"production_receipts\":" + std::string{receipts} + "}\n";

		std::ofstream stream{options.output, std::ios::binary | std::ios::trunc};
		if (!stream)
			fail("qualification child result could not be opened");
		stream.write(document.data(), static_cast<std::streamsize>(document.size()));
		stream.flush();
		if (!stream)
			fail("qualification child result could not be written");
	}

	[[nodiscard]] std::filesystem::path prepare_database_path(const arguments& options)
	{
		const auto database_directory = options.work_directory / "database";
		if (!std::filesystem::create_directory(database_directory))
			fail("qualification database directory already exists or could not be created");
		return database_directory / "database.sqlite";
	}

	void run_current(const arguments& options, const std::filesystem::path& path)
	{
		const auto engine = make_engine();
		observed_open initial;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification current-v3 create", store.error());
			auto snapshot = publish_small(*store, engine);
			const auto compatibility = store->compatibility();
			initial = {compatibility.backend,
					   version_text(compatibility.readable_format),
					   compatibility.direct_open,
					   compatibility.migration_required,
					   capture_observed_projection(*store, engine, snapshot)};
		}
		quiesce_wal_sidecars(path);
		checkpoint(options, "current-initial");

		observed_open cold;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification current-v3 cold reopen", store.error());
			cold = capture_current(*store, engine);
		}
		quiesce_wal_sidecars(path);
		checkpoint(options, "current-cold");
		write_result(options,
					 "{\"initial_open\":" + open_json(initial) +
						 ",\"cold_reopen\":" + open_json(cold) + '}',
					 "{}");
	}

	void run_v2(const arguments& options, const std::filesystem::path& path)
	{
		const auto engine = make_engine();
		(void)create_exact_v2_scenario(path, engine);
		checkpoint(options, "v2-before");

		observed_open opened;
		cxxlens::sdk::error begin_error;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification exact-v2 direct open", store.error());
			opened = capture_current(*store, engine);
			auto begun = store->begin(snapshot_draft(engine));
			if (begun)
				fail("qualification exact-v2 begin unexpectedly succeeded");
			begin_error = begun.error();
		}
		checkpoint(options, "v2-after");

		write_result(options,
					 "{\"open\":" + open_json(opened) +
						 ",\"begin_result\":{\"code\":" + json_string(begin_error.code) +
						 ",\"field\":" + json_string(begin_error.field) +
						 ",\"detail\":" + json_string(begin_error.detail) + "}}",
					 "{}");
	}

	void run_migration(const arguments& options, const std::filesystem::path& path)
	{
		const auto engine = make_engine();
		(void)create_exact_v2_scenario(path, engine);
		std::uint64_t migration_begin_immediate_count{};

		observed_open source;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification migration source open", store.error());
			source = capture_current(*store, engine);
		}
		checkpoint(options, "migration-source");

		observed_open target;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification migration trigger open", store.error());
			const sqlite_store_fault_event begin_immediate_after{
				.operation = sqlite_store_operation::migrate_predecessor,
				.boundary = sqlite_store_fault_boundary::transaction_begin,
				.timing = sqlite_store_fault_timing::after,
				.ordinal = 1U,
				.total = 1U,
			};
			sqlite_store_fault_scope observation_scope{sqlite_store_fault_plan{
				.event = begin_immediate_after,
				.action = sqlite_store_fault_action::observe_only,
			}};
			if (!observation_scope.active())
				fail("qualification migration fault observation scope is inactive");
			if (auto migrated = store->compact(); !migrated)
				fail("qualification snapshot-store compact", migrated.error());
			const auto observation = observation_scope.observation();
			if (observation.observed_event_count != 1U || observation.matching_event_count != 1U ||
				observation.issued_directive_count != 0U || !observation.has_last_observed_event ||
				observation.last_observed_event != begin_immediate_after ||
				!observation.has_matched_event ||
				observation.matched_event != begin_immediate_after || observation.count_overflow ||
				observation.invalid_plan || observation.invalid_event_observed ||
				observation.nested_scope_suppressed)
				fail("qualification migration BEGIN IMMEDIATE observation is not exact");
			migration_begin_immediate_count = observation.matching_event_count;
			target = capture_current(*store, engine);
		}
		quiesce_wal_sidecars(path);
		checkpoint(options, "migration-target");

		observed_open cold;
		{
			auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
			if (!store)
				fail("qualification migrated cold reopen", store.error());
			cold = capture_current(*store, engine);
		}
		quiesce_wal_sidecars(path);
		checkpoint(options, "migration-cold");

		write_result(options,
					 "{\"source\":" + open_json(source) + ",\"target\":" + open_json(target) +
						 ",\"cold_reopen\":" + open_json(cold) + '}',
					 "{\"migration_begin_immediate_count\":" +
						 std::to_string(migration_begin_immediate_count) + '}');
	}

	[[nodiscard]] std::string limit_receipt_json(const sqlite_limit_length_control_receipt& value)
	{
		return "{\"requested_limit_length\":" + std::to_string(value.requested_limit_length) +
			",\"observed_connection_count\":" + std::to_string(value.observed_connection_count) +
			",\"minimum_actual_limit\":" + std::to_string(value.minimum_actual_limit) +
			",\"maximum_actual_limit\":" + std::to_string(value.maximum_actual_limit) +
			",\"all_actual_limits_exact\":" + (value.all_actual_limits_exact ? "true" : "false") +
			",\"observation_count_overflow\":" +
			(value.observation_count_overflow ? "true" : "false") + '}';
	}

	[[nodiscard]] std::string
	resident_receipt_json(const sqlite_payload_resident_observation_receipt& value)
	{
		return "{\"maximum_resident_payload_buffer_bytes\":" +
			std::to_string(value.maximum_resident_payload_buffer_bytes) +
			",\"observation_count\":" + std::to_string(value.observation_count) +
			",\"chunk_framer_instance_count\":" +
			std::to_string(value.chunk_framer_instance_count) +
			",\"validating_source_instance_count\":" +
			std::to_string(value.validating_source_instance_count) +
			",\"observation_count_overflow\":" +
			(value.observation_count_overflow ? "true" : "false") + '}';
	}

	void run_limit(const arguments& options, const std::filesystem::path& path)
	{
		const auto engine = make_engine();
		sqlite_payload_resident_observation_scope resident_scope;
		if (!resident_scope.active())
			fail("qualification payload resident observation scope was not active");
		observed_open memory;
		{
			auto store = cxxlens::sdk::make_in_memory_snapshot_store(engine);
			if (!store)
				fail("qualification memory Store", store.error());
			auto snapshot = publish_bounded_large_v5(*store, engine);
			const auto compatibility = store->compatibility();
			memory = {compatibility.backend,
					  version_text(compatibility.readable_format),
					  compatibility.direct_open,
					  compatibility.migration_required,
					  capture_observed_projection(*store, engine, snapshot)};
		}

		observed_open sqlite;
		observed_open cold;
		sqlite_limit_length_control_receipt limit_receipt;
		{
			sqlite_limit_length_control_scope scope{sqlite_limit_length_boundary::exact_minimum};
			if (!scope.active())
				fail("qualification SQLite limit scope was not active");
			{
				auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
				if (!store)
					fail("qualification limit SQLite Store", store.error());
				auto snapshot = publish_bounded_large_v5(*store, engine);
				const auto compatibility = store->compatibility();
				sqlite = {compatibility.backend,
						  version_text(compatibility.readable_format),
						  compatibility.direct_open,
						  compatibility.migration_required,
						  capture_observed_projection(*store, engine, snapshot)};
			}
			quiesce_wal_sidecars(path);
			checkpoint(options, "limit-sqlite");
			{
				auto store = cxxlens::sdk::open_sqlite_snapshot_store(path.string(), engine);
				if (!store)
					fail("qualification limit SQLite cold reopen", store.error());
				cold = capture_current(*store, engine);
			}
			quiesce_wal_sidecars(path);
			checkpoint(options, "limit-cold");
			limit_receipt = scope.observation();
		}
		const auto resident_receipt = resident_scope.observation();

		write_result(options,
					 "{\"memory\":" + open_json(memory) + ",\"sqlite\":" + open_json(sqlite) +
						 ",\"cold_reopen_sqlite\":" + open_json(cold) + '}',
					 "{\"sqlite_limit_length\":" + limit_receipt_json(limit_receipt) +
						 ",\"payload_resident\":" + resident_receipt_json(resident_receipt) + '}');
	}
} // namespace

int main(const int argc, char** argv)
{
	try
	{
		const auto options = parse_arguments(argc, argv);
		const auto path = prepare_database_path(options);
		if (options.case_id == current_case)
			run_current(options, path);
		else if (options.case_id == v2_case)
			run_v2(options, path);
		else if (options.case_id == migration_case)
			run_migration(options, path);
		else
			run_limit(options, path);
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
