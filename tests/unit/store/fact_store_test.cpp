#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <barrier>

#include "runtime/filesystem_port.hpp"
#include "runtime/hash_port.hpp"
#include "store/binary_codec.hpp"
#include "store/fact_store_access.hpp"
#include "store/ng_legacy_fact_store_adapter.hpp"
#include "store/sqlite/sqlite_api.hpp"
#include "store/store_port.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail;

	[[nodiscard]] std::string id(const std::string_view prefix, const char fill)
	{
		return std::string{prefix} + std::string(64U, fill);
	}

	const compile_unit_id compile_unit{id("cu_", 'a')};
	const build_variant_id variant{id("variant_", 'b')};
	const file_id file{id("file_", 'c')};
	const symbol_id caller{id("symbol_", 'd')};
	const symbol_id callee{id("symbol_", 'e')};
	const symbol_id base_symbol_id{id("symbol_", 'f')};
	const type_id receiver{id("type_", '1')};

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] source_span span(const std::uint64_t offset)
	{
		source_span value;
		value.primary = {
			source_point::at(file, offset, 1U, static_cast<std::uint32_t>(offset + 1U)),
			source_point::at(file, offset + 1U, 1U, static_cast<std::uint32_t>(offset + 2U)),
			source_range_kind::token};
		value.origin = source_origin::directly_spelled;
		value.digest = {"fnv1a64", 1U, "0123456789abcdef"};
		return value;
	}

	[[nodiscard]] facts::detached_fact_record
	fact(const fact_kind kind,
		 const char fill,
		 std::string key,
		 std::map<std::string, std::string> payload,
		 const std::optional<source_span>& source = std::nullopt)
	{
		facts::detached_fact_record value;
		value.id = fact_id{id("fact_", fill)};
		value.kind = kind;
		value.stable_key = std::move(key);
		value.source = source;
		value.origin.compile_units = {compile_unit};
		value.origin.variants = {variant};
		value.origin.extractor_id = "fixture.semantic";
		value.origin.extractor_version = "1.0.0";
		value.payload_version = 1U;
		value.payload = std::move(payload);
		return value;
	}

	[[nodiscard]] facts::reduction_result fixture()
	{
		facts::reduction_result value;
		auto symbol = fact(fact_kind::symbol,
						   '1',
						   "symbol:caller",
						   {{"symbol.id", std::string{caller.value()}},
							{"symbol.kind", "function"},
							{"symbol.linkage", "external"},
							{"symbol.name", "caller"}});
		symbol.name = facts::name_identity{"fixture::caller", "c:@N@fixture@F@caller#", {}, {}, {}};
		auto callee_symbol = fact(fact_kind::symbol,
								  '2',
								  "symbol:callee",
								  {{"symbol.id", std::string{callee.value()}},
								   {"symbol.kind", "function"},
								   {"symbol.linkage", "external"},
								   {"symbol.name", "callee"}});
		callee_symbol.name =
			facts::name_identity{"fixture::callee", "c:@N@fixture@F@callee#", {}, {}, {}};
		auto base_symbol = fact(fact_kind::symbol,
								'3',
								"symbol:base",
								{{"symbol.id", std::string{base_symbol_id.value()}},
								 {"symbol.kind", "struct"},
								 {"symbol.linkage", "external"},
								 {"symbol.name", "base"}});
		base_symbol.name = facts::name_identity{"fixture::base", "c:@N@fixture@S@base", {}, {}, {}};
		auto type = fact(fact_kind::type,
						 '4',
						 "type:receiver",
						 {{"type.id", std::string{receiver.value()}},
						  {"type.kind", "builtin"},
						  {"type.const", "true"},
						  {"type.volatile", "false"},
						  {"type.indirection", "false"},
						  {"type.reference", "false"}});
		type.type = facts::type_identity{"const int", "q(c--):builtin(17)", {}, {}, true};
		value.facts = {
			std::move(symbol),
			std::move(callee_symbol),
			std::move(base_symbol),
			std::move(type),
			fact(fact_kind::declaration,
				 '5',
				 "declaration:caller",
				 {{"symbol.id", std::string{caller.value()}}},
				 span(1U)),
			fact(fact_kind::definition,
				 '6',
				 "definition:caller",
				 {{"symbol.id", std::string{caller.value()}}, {"definition.canonical", "true"}},
				 span(2U)),
			fact(fact_kind::reference,
				 '7',
				 "reference:callee",
				 {{"reference.target", std::string{callee.value()}},
				  {"reference.role", "read"},
				  {"reference.from_macro", "false"}},
				 span(3U)),
			fact(fact_kind::call,
				 '8',
				 "call:callee",
				 {{"call.kind", "direct_function"},
				  {"call.caller", std::string{caller.value()}},
				  {"call.direct_callee", std::string{callee.value()}},
				  {"call.possible_callees", std::string{callee.value()}},
				  {"call.receiver_static_type", std::string{receiver.value()}},
				  {"call.dispatch", "direct_exact"}},
				 span(4U)),
			fact(fact_kind::inheritance,
				 '9',
				 "inheritance:edge",
				 {{"inheritance.derived", std::string{caller.value()}},
				  {"inheritance.base", std::string{base_symbol_id.value()}},
				  {"inheritance.access", "public"},
				  {"inheritance.virtual", "false"}},
				 span(5U)),
			fact(fact_kind::override_relation,
				 'a',
				 "override:edge",
				 {{"override.overriding", std::string{caller.value()}},
				  {"override.overridden", std::string{callee.value()}}},
				 span(6U)),
			fact(fact_kind::include_relation,
				 'b',
				 "include:vector",
				 {{"include.spelling", "vector"},
				  {"include.angled", "true"},
				  {"include.system", "true"},
				  {"conditional.context", "active"}},
				 span(7U)),
			fact(fact_kind::macro_expansion,
				 'c',
				 "macro:fixture",
				 {{"macro.name", "FIXTURE"}, {"macro.function_like", "true"}},
				 span(8U)),
		};
		std::ranges::sort(value.facts,
						  [](const auto& left, const auto& right)
						  {
							  return std::tuple{left.kind, left.stable_key, left.id.value()} <
								  std::tuple{right.kind, right.stable_key, right.id.value()};
						  });
		value.coverage.request({"compile-unit", std::string{compile_unit.value()}})
			.classify(
				{"compile-unit", std::string{compile_unit.value()}, coverage_state::covered, {}});
		require(value.validate().has_value(), "store fixture is invalid");
		return value;
	}

	[[nodiscard]] store::snapshot_metadata metadata()
	{
		store::snapshot_metadata value;
		value.workspace_key = "workspace_fixture";
		value.extractor_versions.emplace("fixture.semantic", "1.0.0");
		return value;
	}

	void cleanup(const std::filesystem::path& database)
	{
		runtime::standard_filesystem_adapter files;
		runtime::request_context context;
		context.operation = "store.test.cleanup";
		(void)files.remove(database, context);
		(void)files.remove(database.string() + "-wal", context);
		(void)files.remove(database.string() + "-shm", context);
	}

	[[nodiscard]] std::shared_ptr<store::fact_store_port>
	open_sqlite(const std::filesystem::path& database, const store::snapshot_metadata& expected)
	{
		auto opened = store::open_sqlite_store(database, expected);
		require(opened.has_value(), "SQLite backend did not open");
		return std::move(opened.value());
	}

	void commit_fixture(const std::shared_ptr<store::fact_store_port>& backend,
						const store::snapshot_metadata& expected,
						const store::transaction_fault fault = store::transaction_fault::none)
	{
		auto transaction = backend->begin(expected, fault);
		require(transaction.has_value(), "transaction did not begin");
		auto reduction = fixture();
		require(transaction.value()->stage(reduction).has_value(), "transaction stage failed");
		require(transaction.value()->validate().has_value(), "transaction validate failed");
		require(transaction.value()->commit().has_value(), "transaction commit failed");
	}

	void check_public_projection(const std::shared_ptr<const store::snapshot_data>& snapshot)
	{
		auto public_store = fact_store_access::make_store(snapshot);
		auto all = public_store.find(fact_query::all());
		require(all && all.value().size() == fixture().facts.size(), "find lost detached facts");
		require(public_store.find(fact_query::all().kind(fact_kind::call)).value().size() == 1U,
				"kind query failed");
		require(public_store.find(fact_query::all().file(file)).value().size() == 8U,
				"file query failed");
		require(public_store.find(fact_query::all().owner(callee)).value().size() >= 2U,
				"owner query failed");
		require(public_store.find(fact_query::all().variant(variant)).value().size() ==
					all.value().size(),
				"variant query failed");
		require(public_store.symbols().value().size() == 3U, "symbol projection failed");
		require(public_store.references(callee).value().size() == 1U,
				"reference projection failed");
		require(public_store.calls().value().size() == 1U, "call projection failed");
		require(public_store.inheritance().value().size() == 1U, "inheritance projection failed");
		require(public_store.overrides().value().size() == 1U, "override projection failed");
		require(public_store.includes().value().size() == 1U, "include projection failed");
		require(public_store.macros().value().size() == 1U, "macro projection failed");
		require(public_store.coverage(fact_profile::full()).validate().has_value(),
				"coverage projection became unbalanced");
	}

	void check_transaction_contract(const std::shared_ptr<store::fact_store_port>& backend,
									const store::snapshot_metadata& expected)
	{
		auto original = backend->read();
		require(original && original.value()->facts.empty(), "new store was not empty");
		auto first = backend->begin(expected);
		require(first.has_value(), "first writer failed to begin");
		auto second = backend->begin(expected);
		require(!second && second.error().attributes.at("reason") == "writer-lock-conflict",
				"second writer did not receive stable lock conflict");
		auto reduction = fixture();
		require(first.value()->stage(reduction) && first.value()->validate(),
				"writer preparation failed");
		auto before_commit = backend->read();
		require(before_commit && before_commit.value()->facts.empty(),
				"reader observed a partially staged snapshot");
		require(first.value()->commit().has_value(), "writer commit failed");
		auto after_commit = backend->read();
		require(after_commit && after_commit.value()->facts.size() == reduction.facts.size(),
				"reader did not observe the complete committed snapshot");
		require(before_commit.value()->facts.empty(), "old reader changed after commit");

		auto concurrent = backend->begin(expected);
		require(concurrent.has_value(), "concurrent-reader transaction did not begin");
		require(concurrent.value()->stage(reduction) && concurrent.value()->validate(),
				"concurrent-reader transaction preparation failed");
		std::barrier start{9};
		std::atomic<std::uint32_t> ready{};
		std::atomic<bool> stop{};
		std::atomic<bool> invalid_snapshot{};
		std::vector<std::jthread> readers;
		readers.reserve(8U);
		for (std::uint32_t index{}; index < 8U; ++index)
		{
			readers.emplace_back(
				[&]
				{
					start.arrive_and_wait();
					do
					{
						auto observed = backend->read();
						if (!observed || observed.value()->facts.size() != reduction.facts.size() ||
							(observed.value()->metadata.generation !=
								 after_commit.value()->metadata.generation &&
							 observed.value()->metadata.generation !=
								 after_commit.value()->metadata.generation + 1U))
							invalid_snapshot = true;
						++ready;
					} while (!stop.load());
				});
		}
		start.arrive_and_wait();
		while (ready.load() < 8U)
			std::this_thread::yield();
		require(concurrent.value()->commit().has_value(), "concurrent-reader commit failed");
		stop = true;
		readers.clear();
		require(!invalid_snapshot.load(), "concurrent reader observed a partial snapshot");
		after_commit = backend->read();
		require(after_commit && after_commit.value()->metadata.generation == 2U,
				"concurrent readers prevented complete atomic publication");

		for (const auto fault : std::array{store::transaction_fault::after_begin,
										   store::transaction_fault::after_stage,
										   store::transaction_fault::after_validate,
										   store::transaction_fault::before_publish,
										   store::transaction_fault::during_publish})
		{
			auto transaction = backend->begin(expected, fault);
			if (fault == store::transaction_fault::after_begin)
			{
				require(!transaction, "after-begin fault was not injected");
			}
			else
			{
				require(transaction.has_value(), "fault transaction did not begin");
				auto staged = transaction.value()->stage(reduction);
				if (fault == store::transaction_fault::after_stage)
					require(!staged, "after-stage fault was not injected");
				else
				{
					require(staged.has_value(), "fault transaction failed too early");
					auto validated = transaction.value()->validate();
					if (fault == store::transaction_fault::after_validate)
						require(!validated, "after-validate fault was not injected");
					else
					{
						require(validated.has_value(), "fault transaction validation failed early");
						require(!transaction.value()->commit(), "publish fault was not injected");
					}
				}
			}
			auto retained = backend->read();
			require(retained &&
						retained.value()->metadata.generation ==
							after_commit.value()->metadata.generation &&
						retained.value()->facts.size() == after_commit.value()->facts.size(),
					"fault exposed a partial snapshot or lost the prior snapshot");
		}
		check_public_projection(after_commit.value());
	}

	[[nodiscard]] std::int64_t scalar_query(const std::filesystem::path& database,
											const std::string_view sql)
	{
		auto loaded = store::sqlite::load_api();
		require(loaded.has_value(), "SQLite runtime unavailable for schema inspection");
		store::sqlite::database* raw{};
		const auto native = database.string();
		require(
			loaded.value()->open_v2(native.c_str(),
									&raw,
									store::sqlite::open_read_write | store::sqlite::open_full_mutex,
									nullptr) == store::sqlite::ok,
			"schema inspection could not open database");
		store::sqlite::statement* statement{};
		const std::string stable_sql{sql};
		require(loaded.value()->prepare_v2(raw, stable_sql.c_str(), -1, &statement, nullptr) ==
					store::sqlite::ok,
				"schema inspection query did not prepare");
		require(loaded.value()->step(statement) == store::sqlite::row,
				"schema inspection query returned no row");
		const auto result = loaded.value()->column_int64(statement, 0);
		require(loaded.value()->finalize(statement) == store::sqlite::ok,
				"schema inspection statement did not finalize");
		require(loaded.value()->close_v2(raw) == store::sqlite::ok,
				"schema inspection database did not close");
		return result;
	}

	void check_sql_schema(const std::filesystem::path& database)
	{
		require(
			scalar_query(database,
						 "SELECT count(*) FROM sqlite_master WHERE name IN ("
						 "'cxxlens_store_metadata','cxxlens_snapshots','cxxlens_fact_index',"
						 "'cxxlens_edge_index','cxxlens_coverage_index','cxxlens_current_snapshot',"
						 "'cxxlens_fact_kind_order','cxxlens_fact_owner_order',"
						 "'cxxlens_fact_file_order','cxxlens_edge_forward_order',"
						 "'cxxlens_edge_reverse_order')") == 11,
			"declared SQLite tables or indexes are missing");
		require(scalar_query(database,
							 "SELECT count(*) FROM cxxlens_fact_index WHERE generation=(SELECT "
							 "generation FROM cxxlens_current_snapshot WHERE singleton=1)") == 12,
				"fact index did not cover the committed fixture");
		require(scalar_query(database,
							 "SELECT count(*) FROM cxxlens_edge_index WHERE generation=(SELECT "
							 "generation FROM cxxlens_current_snapshot WHERE singleton=1)") == 4,
				"forward/reverse edge index did not cover relations");
		require(scalar_query(database,
							 "SELECT count(*) FROM cxxlens_coverage_index WHERE generation=(SELECT "
							 "generation FROM cxxlens_current_snapshot WHERE singleton=1)") == 1,
				"coverage metadata was not indexed");
	}

	void check_backend_parity(const std::filesystem::path& first_database,
							  const std::filesystem::path& relocated_database)
	{
		const auto expected = metadata();
		cleanup(first_database);
		cleanup(relocated_database);
		auto memory = store::make_in_memory_store(expected);
		require(memory.has_value(), "in-memory backend did not open");
		auto sqlite = open_sqlite(first_database, expected);
		check_transaction_contract(memory.value(), expected);
		check_transaction_contract(sqlite, expected);
		check_sql_schema(first_database);
		auto memory_snapshot = memory.value()->read().value();
		auto sqlite_snapshot = sqlite->read().value();
		auto memory_bytes = store::encode_snapshot(*memory_snapshot);
		auto sqlite_bytes = store::encode_snapshot(*sqlite_snapshot);
		require(memory_bytes && sqlite_bytes && memory_bytes.value() == sqlite_bytes.value(),
				"backend snapshots were not byte-equivalent");
		auto competing_connection = open_sqlite(first_database, expected);
		auto held_writer = sqlite->begin(expected);
		require(held_writer.has_value(), "cross-connection writer did not begin");
		auto blocked_writer = competing_connection->begin(expected);
		require(!blocked_writer &&
					blocked_writer.error().attributes.at("reason") == "writer-lock-conflict",
				"cross-connection second writer did not receive stable lock conflict");
		held_writer.value()->rollback();
		competing_connection.reset();

		require(sqlite->compact().has_value(), "SQLite VACUUM failed");
		sqlite.reset();
		auto reopened = open_sqlite(first_database, expected);
		require(store::encode_snapshot(*reopened->read().value()).value() == memory_bytes.value(),
				"VACUUM/reopen changed snapshot order or values");

		auto relocated = open_sqlite(relocated_database, expected);
		commit_fixture(relocated, expected);
		commit_fixture(relocated, expected);
		require(store::encode_snapshot(*relocated->read().value()).value() == memory_bytes.value(),
				"database/root relocation changed semantic snapshot bytes");
		cleanup(relocated_database);

		std::vector<store::snapshot_metadata> mismatches;
		auto schema_mismatch = expected;
		schema_mismatch.schema_version = "cxxlens.fact-snapshot.v2";
		mismatches.push_back(schema_mismatch);
		auto semantics_mismatch = expected;
		semantics_mismatch.semantics_version = "2.0.0";
		mismatches.push_back(semantics_mismatch);
		auto adapter_mismatch = expected;
		adapter_mismatch.adapter_version = "2.0.0";
		mismatches.push_back(adapter_mismatch);
		auto extractor_mismatch = expected;
		extractor_mismatch.extractor_versions["fixture.semantic"] = "2.0.0";
		mismatches.push_back(extractor_mismatch);
		for (const auto& mismatch : mismatches)
		{
			auto rejected = open_sqlite(first_database, mismatch);
			require(rejected->compatibility() == store::compatibility_state::rebuild_required &&
						!rejected->read(),
					"one metadata mismatch axis was silently accepted");
		}
		auto incompatible_metadata = semantics_mismatch;
		auto incompatible = open_sqlite(first_database, incompatible_metadata);
		require(incompatible->compatibility() == store::compatibility_state::rebuild_required &&
					!incompatible->read(),
				"metadata mismatch did not require explicit rebuild");
		require(incompatible->rebuild(incompatible_metadata) &&
					incompatible->compatibility() == store::compatibility_state::compatible &&
					incompatible->read().value()->facts.empty(),
				"explicit incompatible-cache rebuild failed");
		incompatible.reset();
		reopened.reset();
		cleanup(first_database);
	}

	void corrupt_payload(const std::filesystem::path& database)
	{
		auto loaded = store::sqlite::load_api();
		require(loaded.has_value(), "SQLite runtime unavailable for corruption fixture");
		store::sqlite::database* raw{};
		const auto native = database.string();
		require(
			loaded.value()->open_v2(native.c_str(),
									&raw,
									store::sqlite::open_read_write | store::sqlite::open_full_mutex,
									nullptr) == store::sqlite::ok,
			"corruption fixture could not open database");
		char* message{};
		const int status = loaded.value()->exec(
			raw,
			"UPDATE cxxlens_snapshots SET payload=x'00' WHERE generation=(SELECT generation FROM "
			"cxxlens_current_snapshot WHERE singleton=1)",
			nullptr,
			nullptr,
			&message);
		if (message != nullptr)
			loaded.value()->free_memory(message);
		require(status == store::sqlite::ok, "corruption fixture update failed");
		require(loaded.value()->close_v2(raw) == store::sqlite::ok,
				"corruption fixture close failed");
	}

	void check_corruption(const std::filesystem::path& corrupt_database,
						  const std::filesystem::path& truncated_database)
	{
		const auto expected = metadata();
		cleanup(corrupt_database);
		{
			auto backend = open_sqlite(corrupt_database, expected);
			commit_fixture(backend, expected);
		}
		corrupt_payload(corrupt_database);
		auto corrupt = open_sqlite(corrupt_database, expected);
		require(corrupt->compatibility() == store::compatibility_state::corrupt &&
					!corrupt->read() && corrupt->read().error().code.value == "facts.store-corrupt",
				"corrupted payload masqueraded as an empty store");
		corrupt.reset();
		cleanup(corrupt_database);

		cleanup(truncated_database);
		{
			std::ofstream output{truncated_database, std::ios::binary | std::ios::trunc};
			output.write("SQLite", 6);
		}
		auto truncated = open_sqlite(truncated_database, expected);
		require(truncated->compatibility() == store::compatibility_state::corrupt &&
					!truncated->read(),
				"truncated database masqueraded as an empty store");
		truncated.reset();
		cleanup(truncated_database);
	}

	[[nodiscard]] std::string snapshot_golden()
	{
		store::snapshot_data snapshot;
		snapshot.metadata = metadata();
		snapshot.metadata.generation = 2U;
		auto reduction = fixture();
		snapshot.facts = std::move(reduction.facts);
		snapshot.coverage = std::move(reduction.coverage);
		auto encoded = store::encode_snapshot(snapshot);
		require(encoded.has_value(), "golden snapshot did not encode");
		runtime::fnv1a_hash_adapter hashes;
		runtime::hash_request request{
			"fnv1a64", 1U, "cxxlens.test.fact-store-binary.v1", encoded.value()};
		runtime::request_context context;
		context.operation = "store.test.golden";
		auto digest = hashes.calculate(request, context);
		require(digest.has_value(), "golden snapshot did not hash");
		return digest.value().hexadecimal;
	}

	[[nodiscard]] sdk::relation_descriptor legacy_bridge_descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "migration.legacy_fact.v1";
		value.name = "migration.legacy_fact";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "migration.legacy-fact/1";
		value.owner_namespace = "cxxlens.migration";
		value.columns = {
			{"migration.legacy_fact.v1.fact",
			 "fact",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::claim_key},
			{"migration.legacy_fact.v1.stable_key",
			 "stable_key",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {value.columns.front().id};
		value.descriptor_digest =
			sdk::semantic_digest("cxxlens.relation-descriptor.v1", value.canonical_form());
		return value;
	}

	class fixture_legacy_mapper final : public store::legacy_fact_mapper
	{
	  public:
		explicit fixture_legacy_mapper(const sdk::relation_engine& engine) : engine_{engine} {}
		[[nodiscard]] sdk::result<sdk::claim>
		map(const facts::detached_fact_record& fact) const override
		{
			const auto descriptor = legacy_bridge_descriptor();
			sdk::row_builder builder{descriptor};
			if (auto added = builder.set(
					{descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
					sdk::detached_cell::utf8(std::string{fact.id.value()}));
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = builder.set(
					{descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
					sdk::detached_cell::utf8(fact.stable_key));
				!added)
				return sdk::unexpected(std::move(added.error()));
			auto row = std::move(builder).finish();
			if (!row)
				return sdk::unexpected(std::move(row.error()));
			return sdk::make_assertion(
				engine_,
				{std::move(*row),
				 {"migration-universe", {"all"}},
				 "migration.legacy-v1",
				 {"migration.legacy-adapter",
				  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
				 {"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
				 "evidence:legacy-store",
				 {"unknown", "legacy-fact-store", "migration-only", {"schema_validated"}}});
		}

	  private:
		const sdk::relation_engine& engine_;
	};

	void check_ng_one_way_legacy_adapter()
	{
		sdk::relation_registry registry;
		require(registry.add(legacy_bridge_descriptor()).has_value(),
				"legacy bridge descriptor rejected");
		auto engine = registry.build("legacy-migration-test");
		require(engine.has_value(), "legacy bridge engine rejected");
		auto reduction = fixture();
		store::snapshot_data source{metadata(), reduction.facts, reduction.coverage};
		fixture_legacy_mapper mapper{*engine};
		store::legacy_migration_request request{
			legacy_bridge_descriptor().id,
			"legacy-workspace",
			{"migration-universe", {"all"}},
			"migration.legacy-v1",
			"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa",
			{},
			"unknown",
			"legacy-assumptions"};
		const sdk::claim_input_basis direct{sdk::direct_claim_basis{
			"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"}};
		auto basis = sdk::claim_input_basis_digest(direct);
		require(basis.has_value(), "legacy bridge basis rejected");
		request.producer_input_basis_digest = std::move(*basis);
		auto adapted = store::adapt_legacy_fact_snapshot(source, request, mapper);
		require(adapted && adapted->claims.size() == source.facts.size() &&
					sdk::make_partition_manifest(*engine, *adapted).has_value(),
				"legacy v1 facts did not traverse the NG partition validator");
	}
} // namespace

int main(const int argument_count, const char* const* arguments)
{
	if (argument_count == 2 && std::string_view{arguments[1]} == "--emit")
	{
		std::cout << snapshot_golden() << '\n';
		return 0;
	}
	const std::filesystem::path test_directory{CXXLENS_STORE_TEST_DIRECTORY};
	check_backend_parity(test_directory / "facts.sqlite",
						 test_directory / "relocated-facts.sqlite");
	check_corruption(test_directory / "corrupt-facts.sqlite",
					 test_directory / "truncated-facts.sqlite");
	check_ng_one_way_legacy_adapter();
	return 0;
}
