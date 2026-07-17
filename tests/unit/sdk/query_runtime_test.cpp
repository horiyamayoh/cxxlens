#include <algorithm>
#include <array>
#include <charconv>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

#include "../../../src/sdk/claim_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	namespace query = cxxlens::sdk::query;

	void require(const bool condition, const std::string& message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] relation_descriptor left_relation()
	{
		relation_descriptor value;
		value.id = "company.query.left.v1";
		value.name = "company.query.left";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.query.left/1";
		value.owner_namespace = "company.query";
		value.columns = {
			{value.id + ".key",
			 "key",
			 {scalar_kind::typed_id, "query_key_id", false},
			 true,
			 column_role::claim_key},
			{value.id + ".value",
			 "value",
			 {scalar_kind::signed_integer, {}, false},
			 true,
			 column_role::authoritative_payload},
			{value.id + ".note",
			 "note",
			 {scalar_kind::utf8_string, {}, true},
			 false,
			 column_role::auxiliary},
		};
		value.key_columns = {value.columns[0].id};
		value.merge = merge_mode::functional_assertion;
		value.conflict_columns = {value.columns[1].id};
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] std::size_t occurrences(const std::string_view text,
										  const std::string_view needle)
	{
		std::size_t count{};
		std::size_t offset{};
		while ((offset = text.find(needle, offset)) != std::string_view::npos)
		{
			++count;
			offset += needle.size();
		}
		return count;
	}

	[[nodiscard]] relation_descriptor right_relation()
	{
		relation_descriptor value;
		value.id = "company.query.right.v1";
		value.name = "company.query.right";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.query.right/1";
		value.owner_namespace = "company.query";
		value.columns = {
			{value.id + ".key",
			 "key",
			 {scalar_kind::typed_id, "query_key_id", false},
			 true,
			 column_role::claim_key},
			{value.id + ".label",
			 "label",
			 {scalar_kind::utf8_string, {}, false},
			 true,
			 column_role::authoritative_payload},
		};
		value.key_columns = {value.columns[0].id};
		value.merge = merge_mode::functional_assertion;
		value.conflict_columns = {value.columns[1].id};
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] relation_descriptor self_join_relation()
	{
		relation_descriptor value;
		value.id = "company.query.self.v1";
		value.name = "company.query.self";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.query.self/1";
		value.owner_namespace = "company.query";
		value.columns = {
			{value.id + ".key",
			 "key",
			 {scalar_kind::typed_id, "query_key_id", false},
			 true,
			 column_role::claim_key},
			{value.id + ".parent",
			 "parent",
			 {scalar_kind::typed_id, "query_key_id", false},
			 true,
			 column_role::authoritative_payload},
		};
		value.key_columns = {value.columns[0].id};
		value.merge = merge_mode::functional_assertion;
		value.conflict_columns = {value.columns[1].id};
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] detached_row
	self_join_row(const relation_descriptor& descriptor, std::string key, std::string parent)
	{
		row_builder builder{descriptor};
		require(builder
					.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 detached_cell::typed("query_key_id", std::move(key)))
					.has_value(),
				"self-join key rejected");
		require(builder
					.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 detached_cell::typed("query_key_id", std::move(parent)))
					.has_value(),
				"self-join parent rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "self-join row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] detached_row left_row(const relation_descriptor& descriptor,
										std::string key,
										const std::int64_t value,
										const bool unknown_note)
	{
		row_builder builder{descriptor};
		require(builder
					.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 detached_cell::typed("query_key_id", std::move(key)))
					.has_value(),
				"left key rejected");
		require(builder
					.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 detached_cell::signed_integer(value))
					.has_value(),
				"left value rejected");
		if (unknown_note)
			require(builder
						.set({descriptor.id, descriptor.columns[2].id, descriptor.columns[2].type},
							 detached_cell::unknown(descriptor.columns[2].type, "note-unresolved"))
						.has_value(),
					"unknown optional note rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "left row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] detached_row
	right_row(const relation_descriptor& descriptor, std::string key, std::string label)
	{
		row_builder builder{descriptor};
		require(builder
					.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 detached_cell::typed("query_key_id", std::move(key)))
					.has_value(),
				"right key rejected");
		require(builder
					.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 detached_cell::utf8(std::move(label)))
					.has_value(),
				"right label rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "right row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] relation_descriptor schema_relation(const std::uint32_t minor,
													  const bool include_value,
													  std::string key_parameter,
													  const bool include_note)
	{
		relation_descriptor value;
		value.id = "company.query.schema.v1";
		value.name = "company.query.schema";
		value.version = {1U, minor, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.query.schema/1";
		value.owner_namespace = "company.query";
		value.columns.push_back({value.id + ".key",
								 "key",
								 {scalar_kind::typed_id, std::move(key_parameter), false},
								 true,
								 column_role::claim_key});
		if (include_value)
			value.columns.push_back({value.id + ".value",
									 "value",
									 {scalar_kind::signed_integer, {}, false},
									 true,
									 column_role::auxiliary});
		if (include_note)
			value.columns.push_back({value.id + ".note",
									 "note",
									 {scalar_kind::utf8_string, {}, true},
									 false,
									 column_role::auxiliary});
		value.key_columns = {value.id + ".key"};
		value.merge = merge_mode::set;
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		require(value.validate().has_value(), "schema compatibility descriptor rejected");
		return value;
	}

	[[nodiscard]] detached_row schema_row(const relation_descriptor& descriptor)
	{
		row_builder builder{descriptor};
		const auto key = descriptor.column(descriptor.id + ".key");
		require(key.has_value(), "schema compatibility key missing");
		require(builder
					.set({descriptor.id, key->id, key->type},
						 detached_cell::typed(key->type.parameter, "key:schema"))
					.has_value(),
				"schema compatibility key rejected");
		if (const auto column = descriptor.column(descriptor.id + ".value"))
			require(builder
						.set({descriptor.id, column->id, column->type},
							 detached_cell::signed_integer(7))
						.has_value(),
					"schema compatibility value rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "schema compatibility row did not finish");
		return std::move(*row);
	}

	[[nodiscard]] relation_descriptor set_relation()
	{
		relation_descriptor value;
		value.id = "company.query.sets.v1";
		value.name = "company.query.sets";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.query.sets/1";
		value.owner_namespace = "company.query";
		value.columns = {
			{value.id + ".key",
			 "key",
			 {scalar_kind::typed_id, "query_set_key_id", false},
			 true,
			 column_role::claim_key},
			{value.id + ".qualifiers",
			 "qualifiers",
			 {scalar_kind::set, "open_symbol<cc.type-qualifier/1>", false},
			 true,
			 column_role::authoritative_payload},
		};
		value.key_columns = {value.columns[0].id};
		value.merge = merge_mode::set;
		value.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 value.contract_digest + "\n" + value.canonical_form());
		require(value.validate().has_value(), "set literal descriptor rejected");
		return value;
	}

	[[nodiscard]] detached_row set_row(const relation_descriptor& descriptor,
									   std::vector<std::byte> qualifiers)
	{
		row_builder builder{descriptor};
		require(builder
					.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 detached_cell::typed("query_set_key_id", "key:set"))
					.has_value(),
				"set literal key rejected");
		detached_cell set_value{descriptor.columns[1].type,
								cell_state::present,
								scalar_value{std::move(qualifiers)},
								std::nullopt};
		require(builder
					.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 std::move(set_value))
					.has_value(),
				"set literal value rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "set literal row did not finish");
		return std::move(*row);
	}

	constexpr std::string_view producer_semantics{
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
	constexpr std::string_view direct_basis{
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};

	[[nodiscard]] claim assertion(const relation_engine& engine,
								  detached_row row,
								  std::vector<std::string> fragments,
								  std::string interpretation = "company.query.domain",
								  claim_guarantee guarantee = {
									  "exact", "project", "assumptions:none", {"schema_validated"}})
	{
		auto value = make_assertion(engine,
									{std::move(row),
									 {"company.query.universe", std::move(fragments)},
									 std::move(interpretation),
									 {"company.query.provider", std::string{producer_semantics}},
									 {std::string{direct_basis}},
									 "evidence:query-runtime",
									 std::move(guarantee)});
		require(value.has_value(), "query fixture assertion failed");
		return std::move(*value);
	}

	struct fixture
	{
		relation_descriptor left;
		relation_descriptor right;
		relation_engine engine;
		std::vector<claim> claims;
		relation_descriptor self{};
	};

	[[nodiscard]] fixture make_fixture()
	{
		auto left = left_relation();
		auto right = right_relation();
		auto self = self_join_relation();
		relation_registry registry;
		require(registry.add(left).has_value(), "left relation rejected");
		require(registry.add(right).has_value(), "right relation rejected");
		require(registry.add(self).has_value(), "self-join relation rejected");
		auto engine = registry.build("query-runtime-generation");
		require(engine.has_value(), "query relation engine failed");
		std::vector<claim> claims;
		claims.push_back(
			assertion(*engine, left_row(left, "key:a", 2, false), {"debug", "release"}));
		claims.push_back(assertion(*engine, left_row(left, "key:b", 1, true), {"debug"}));
		claims.push_back(assertion(*engine, left_row(left, "key:a", 2, false), {"release"}));
		claims.push_back(assertion(*engine, right_row(right, "key:a", "alpha"), {"release"}));
		claims.push_back(assertion(*engine, right_row(right, "key:b", "beta"), {"debug"}));
		claims.push_back(assertion(
			*engine, right_row(right, "key:a", "other"), {"debug"}, "company.query.other-domain"));
		claims.push_back(assertion(*engine, self_join_row(self, "key:a", "key:root"), {"release"}));
		claims.push_back(assertion(*engine, self_join_row(self, "key:b", "key:a"), {"release"}));
		claims.push_back(assertion(*engine, self_join_row(self, "key:c", "key:a"), {"release"}));
		return {std::move(left),
				std::move(right),
				std::move(*engine),
				std::move(claims),
				std::move(self)};
	}

	[[nodiscard]] fixture make_side_channel_fixture()
	{
		auto left = left_relation();
		auto right = right_relation();
		relation_registry registry;
		require(registry.add(left).has_value(), "side-channel left relation rejected");
		require(registry.add(right).has_value(), "side-channel right relation rejected");
		auto engine = registry.build("query-side-channel-generation");
		require(engine.has_value(), "side-channel relation engine failed");
		std::vector<claim> claims;
		claims.push_back(
			assertion(*engine, left_row(left, "key:same-domain", 1, false), {"debug", "release"}));
		claims.push_back(
			assertion(*engine, left_row(left, "key:same-domain", 1, false), {"release"}));
		claims.push_back(
			assertion(*engine, left_row(left, "key:same-cross-domain", 2, false), {"release"}));
		claims.push_back(assertion(*engine,
								   left_row(left, "key:same-cross-domain", 2, false),
								   {"release"},
								   "company.query.other-domain"));
		claims.push_back(assertion(*engine, left_row(left, "key:conflict", 3, false), {"release"}));
		claims.push_back(assertion(*engine, left_row(left, "key:conflict", 4, false), {"release"}));
		claims.push_back(
			assertion(*engine, left_row(left, "key:differential", 5, false), {"release"}));
		claims.push_back(assertion(*engine,
								   left_row(left, "key:differential", 6, false),
								   {"release"},
								   "company.query.other-domain"));
		return {std::move(left), std::move(right), std::move(*engine), std::move(claims)};
	}

	[[nodiscard]] fixture make_budget_fixture()
	{
		auto left = left_relation();
		auto right = right_relation();
		relation_registry registry;
		require(registry.add(left).has_value(), "budget left relation rejected");
		require(registry.add(right).has_value(), "budget right relation rejected");
		auto engine = registry.build("query-budget-generation");
		require(engine.has_value(), "budget relation engine failed");
		std::vector<claim> claims;
		for (std::int64_t index = 0; index < 8; ++index)
		{
			claims.push_back(
				assertion(*engine, left_row(left, "key:budget", index, false), {"release"}));
			claims.push_back(
				assertion(*engine,
						  right_row(right, "key:budget", "label:" + std::to_string(index)),
						  {"release"}));
		}
		return {std::move(left), std::move(right), std::move(*engine), std::move(claims)};
	}

	[[nodiscard]] snapshot_draft draft(const relation_engine& engine)
	{
		return {{"catalog:query-runtime",
				 "stable",
				 std::string{engine.generation()},
				 "company.query.universe",
				 std::string{engine.registry_digest()},
				 "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
				 "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
				{1U, 0U, 0U},
				"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
				std::nullopt};
	}

	[[nodiscard]] partition_draft
	partition(const claim& value, const std::size_t ordinal, const bool unresolved = false)
	{
		auto basis = claim_input_basis_digest(value.input_basis);
		require(basis.has_value(), "claim basis digest failed");
		partition_draft output;
		output.relation_descriptor_id = value.descriptor;
		output.scope = "query-scope:" + std::to_string(ordinal);
		output.condition = value.presence;
		output.interpretation = value.interpretation;
		output.producer_semantics = value.producer.semantic_contract;
		output.producer_input_basis_digest = std::move(*basis);
		output.precision_profile = "exact";
		output.assumption_set_id = "assumptions:none";
		output.claims = {value};
		output.coverage = {{"query-scope", output.scope, "covered", {}}};
		if (unresolved)
			output.unresolved = {
				{value.assertion, value.descriptor, value.descriptor, {}, "fixture-unresolved"}};
		return output;
	}

	[[nodiscard]] snapshot_handle
	publish(snapshot_store& store, const fixture& data, const bool reverse, const bool unresolved)
	{
		auto writer = store.begin(draft(data.engine));
		require(writer.has_value(), "query snapshot writer failed");
		std::vector<std::size_t> order(data.claims.size());
		for (std::size_t index = 0U; index < order.size(); ++index)
			order[index] = index;
		if (reverse)
			std::ranges::reverse(order);
		for (const auto index : order)
			require(writer->stage(partition(data.claims[index], index, unresolved && index == 0U))
						.has_value(),
					"query partition stage failed");
		require(writer->validate().has_value(), "query snapshot validation failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "query snapshot publication failed");
		return std::move(*snapshot);
	}

	[[nodiscard]] snapshot_handle publish_schema_snapshot(relation_descriptor descriptor)
	{
		relation_registry registry;
		require(registry.add(descriptor).has_value(), "schema snapshot relation rejected");
		auto engine = registry.build("query-schema-compatibility-generation");
		require(engine.has_value(), "schema snapshot relation engine failed");
		auto store = make_in_memory_snapshot_store(*engine);
		require(store.has_value(), "schema snapshot store failed");
		auto writer = store->begin(draft(*engine));
		require(writer.has_value(), "schema snapshot writer failed");
		auto value = assertion(*engine, schema_row(descriptor), {"release"});
		require(writer->stage(partition(value, 0U)).has_value(),
				"schema snapshot partition rejected");
		require(writer->validate().has_value(), "schema snapshot validation failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "schema snapshot publication failed");
		return std::move(*snapshot);
	}

	[[nodiscard]] closure_candidate closure_for(const relation_engine& engine,
												const partition_draft& value)
	{
		auto manifest = make_partition_manifest(engine, value);
		require(manifest.has_value(), "closure fixture partition manifest failed");
		return {value.relation_descriptor_id,
				manifest->partition_id,
				manifest->content_digest,
				manifest->coverage_digest,
				"sha256:1111111111111111111111111111111111111111111111111111111111111111",
				value.condition,
				value.interpretation,
				value.assumption_set_id,
				"relation-key-enumeration",
				value.producer_semantics,
				"sha256:2222222222222222222222222222222222222222222222222222222222222222"};
	}

	[[nodiscard]] snapshot_handle
	publish_with_closures(snapshot_store& store,
						  const relation_engine& engine,
						  const std::vector<partition_draft>& partitions,
						  const std::span<const std::size_t> closed_partitions)
	{
		auto writer = store.begin(draft(engine));
		require(writer.has_value(), "closure snapshot writer failed");
		for (const auto& value : partitions)
			require(writer->stage(value).has_value(), "closure partition stage failed");
		for (const auto index : closed_partitions)
			require(writer->add_closure(closure_for(engine, partitions.at(index))).has_value(),
					"closure candidate stage failed");
		require(writer->validate().has_value(), "closure snapshot validation failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "closure snapshot publication failed");
		return std::move(*snapshot);
	}

	[[nodiscard]] std::vector<std::string> rows(const query::query_result& result)
	{
		auto cursor = result.rows();
		std::vector<std::string> output;
		while (true)
		{
			auto next = cursor.next();
			require(next.has_value(), "query result cursor failed");
			if (!*next)
				break;
			auto row = (*next)->copy();
			require(row.has_value(), "query result row copy failed");
			output.push_back(row->canonical_form());
		}
		return output;
	}

	[[nodiscard]] std::vector<query::annotated_row>
	annotated_rows(const query::query_result& result)
	{
		auto cursor = result.rows();
		std::vector<query::annotated_row> output;
		while (true)
		{
			auto next = cursor.next();
			require(next.has_value(), "annotated result cursor failed");
			if (!*next)
				break;
			auto row = (*next)->copy();
			require(row.has_value(), "annotated result row copy failed");
			output.push_back(std::move(*row));
		}
		return output;
	}

	[[nodiscard]] std::set<std::string, std::less<>> row_keys(const query::annotated_row& row)
	{
		std::set<std::string, std::less<>> output;
		for (const auto& [column, cell] : row.values)
		{
			(void)cell;
			output.insert(column);
		}
		return output;
	}

	[[nodiscard]] std::string present_string(const query::annotated_row& row,
											 const std::string_view column)
	{
		const auto found = row.values.find(column);
		require(found != row.values.end() && found->second.state == cell_state::present &&
					found->second.value.has_value(),
				"expected present query string cell missing");
		const auto value = std::get_if<std::string>(&*found->second.value);
		require(value != nullptr, "query cell did not contain a string value");
		return *value;
	}

	[[nodiscard]] std::uint64_t physical_metric(const std::string_view physical,
												const std::string_view anchor,
												const std::string_view metric)
	{
		const auto anchor_position = physical.find(anchor);
		require(anchor_position != std::string_view::npos, "physical metric anchor missing");
		const auto metric_position = physical.find(metric, anchor_position);
		require(metric_position != std::string_view::npos, "physical metric missing");
		const auto begin = metric_position + metric.size();
		std::uint64_t value{};
		const auto parsed =
			std::from_chars(physical.data() + begin, physical.data() + physical.size(), value);
		require(parsed.ec == std::errc{}, "physical metric was not numeric");
		return value;
	}

	[[nodiscard]] query::logical_query_ir scan_query(const relation_descriptor& descriptor)
	{
		auto builder = query::builder::from(descriptor);
		require(builder.has_value(), "scan builder failed");
		return std::move(*builder).finish();
	}

	void check_canonical_json_encoding(const fixture& data)
	{
		std::string text;
		for (unsigned int byte = 0U; byte < 0x20U; ++byte)
			text.push_back(static_cast<char>(byte));
		text += "\"\\雪";
		row_builder builder{data.left};
		require(builder
					.set({data.left.id, data.left.columns[0].id, data.left.columns[0].type},
						 detached_cell::typed("query_key_id", "key:json"))
					.has_value(),
				"JSON fixture key rejected");
		require(builder
					.set({data.left.id, data.left.columns[1].id, data.left.columns[1].type},
						 detached_cell::signed_integer(42))
					.has_value(),
				"JSON fixture value rejected");
		require(builder
					.set({data.left.id, data.left.columns[2].id, data.left.columns[2].type},
						 detached_cell{data.left.columns[2].type,
									   cell_state::present,
									   scalar_value{text},
									   std::nullopt})
					.has_value(),
				"valid UTF-8/control JSON fixture rejected");
		auto row = std::move(builder).finish();
		require(row.has_value(), "JSON fixture row did not finish");
		const auto value = assertion(data.engine, std::move(*row), {"release"});

		const auto publish_one = [&](snapshot_store& store)
		{
			auto writer = store.begin(draft(data.engine));
			require(writer.has_value(), "JSON fixture writer failed");
			require(writer->stage(partition(value, 0U)).has_value(),
					"JSON fixture partition rejected");
			require(writer->validate().has_value(), "JSON fixture snapshot validation failed");
			auto snapshot = writer->publish();
			require(snapshot.has_value(), "JSON fixture snapshot publication failed");
			return std::move(*snapshot);
		};

		auto memory_store = make_in_memory_snapshot_store(data.engine);
		require(memory_store.has_value(), "JSON fixture memory store failed");
		auto memory_snapshot = publish_one(*memory_store);
		const auto database =
			std::filesystem::temp_directory_path() / "cxxlens-ng-query-json.sqlite";
		std::filesystem::remove(database);
		std::filesystem::remove(database.string() + "-wal");
		std::filesystem::remove(database.string() + "-shm");
		auto sqlite_store = open_sqlite_snapshot_store(database.string(), data.engine);
		require(sqlite_store.has_value(), "JSON fixture SQLite store failed");
		auto sqlite_snapshot = publish_one(*sqlite_store);

		const auto execute = [&](const snapshot_handle& snapshot)
		{
			auto engine = query::reference_engine::bind(snapshot);
			require(engine.has_value(), "JSON fixture engine bind failed");
			auto result = engine->execute(scan_query(data.left));
			require(result.has_value(), "JSON fixture query failed");
			return result->canonical_form();
		};
		const auto memory_json = execute(memory_snapshot);
		const auto sqlite_json = execute(sqlite_snapshot);
		for (const auto byte : memory_json)
			require(static_cast<unsigned char>(byte) >= 0x20U,
					"canonical result contains a raw JSON control byte");
		for (const auto* escape : {"\\u0000",
								   "\\u0001",
								   "\\b",
								   "\\t",
								   "\\n",
								   "\\f",
								   "\\r",
								   "\\u000b",
								   "\\u001f",
								   "\\\"",
								   "\\\\"})
			require(memory_json.contains(escape),
					"canonical result omitted a required JSON escape");
		require(memory_json.contains("雪"), "non-ASCII UTF-8 did not round-trip");
		const auto row_projection = [](const std::string& serialized)
		{
			const auto begin = serialized.find("\"rows\":[");
			const auto end = serialized.find("],\"schema\"", begin);
			require(begin != std::string::npos && end != std::string::npos,
					"canonical result row projection missing");
			return serialized.substr(begin, end - begin + 1U);
		};
		require(row_projection(memory_json) == row_projection(sqlite_json),
				"memory/SQLite JSON row serialization diverged");

		const std::string invalid_utf8{"\xc0\xaf", 2U};
		auto invalid_value = detached_cell::utf8(invalid_utf8);
		auto invalid_value_result = invalid_value.validate();
		require(!invalid_value_result && invalid_value_result.error().detail == "invalid-utf8",
				"invalid UTF-8 string value was accepted");
		auto invalid_reason =
			detached_cell::unknown({scalar_kind::utf8_string, {}, true}, invalid_utf8);
		auto invalid_reason_result = invalid_reason.validate();
		require(!invalid_reason_result && invalid_reason_result.error().detail == "invalid-utf8",
				"invalid UTF-8 unknown reason was accepted");

		std::filesystem::remove(database);
		std::filesystem::remove(database.string() + "-wal");
		std::filesystem::remove(database.string() + "-shm");
	}

	[[nodiscard]] query::logical_query_ir
	condition_query(const relation_descriptor& descriptor,
					const std::span<const std::string> alternatives,
					const std::optional<std::string_view> interpretation = std::nullopt)
	{
		auto source = query::builder::from(descriptor);
		require(source.has_value(), "closure condition source failed");
		auto restricted =
			std::move(*source).condition_restrict("company.query.universe", alternatives);
		require(restricted.has_value(), "closure condition restriction failed");
		if (!interpretation)
			return std::move(*restricted).finish();
		auto interpreted = std::move(*restricted).interpretation_restrict(*interpretation);
		require(interpreted.has_value(), "closure interpretation restriction failed");
		return std::move(*interpreted).finish();
	}

	class ordinal_cancel final : public query::cancellation_probe
	{
	  public:
		explicit ordinal_cancel(const std::uint64_t ordinal) : ordinal_{ordinal} {}
		[[nodiscard]] bool
		stop_requested(const query::execution_checkpoint& checkpoint) const noexcept override
		{
			return checkpoint.current == query::execution_checkpoint::phase::before_publish_row &&
				checkpoint.ordinal == ordinal_;
		}

	  private:
		std::uint64_t ordinal_{};
	};

	class immediate_cancel final : public query::cancellation_probe
	{
	  public:
		[[nodiscard]] bool
		stop_requested(const query::execution_checkpoint& checkpoint) const noexcept override
		{
			return checkpoint.current == query::execution_checkpoint::phase::before_execution;
		}
	};

	void check_ir_validation(const fixture& data)
	{
		auto valid = scan_query(data.left);
		require(valid.validate().has_value(), "builder scan IR did not validate");

		const auto decode_limit = [](std::string arguments)
		{
			return query::decode_arguments(
				{"limit", "query.limit.v1", {"input"}, std::move(arguments)});
		};
		require(decode_limit(R"({"count":0})") && decode_limit(R"({"count":10})"),
				"valid JSON integer was rejected");
		for (const auto* invalid : {R"({"count":00})", R"({"count":01})", R"({"count":-01})"})
			require(!decode_limit(invalid), "leading-zero JSON integer was accepted");

		auto unicode_source = query::builder::from(data.left);
		require(unicode_source.has_value(), "Unicode query source failed");
		auto unicode_builder = std::move(*unicode_source).interpretation_restrict("domain-😀-雪");
		require(unicode_builder.has_value(), "Unicode query construction failed");
		auto canonical_unicode = std::move(*unicode_builder).finish();
		auto escaped_unicode = canonical_unicode;
		escaped_unicode.nodes.back().arguments =
			R"({"interpretation":"domain-\uD83D\uDE00-\u96ea"})";
		require(escaped_unicode.validate().has_value() &&
					escaped_unicode.digest() == canonical_unicode.digest(),
				"valid surrogate pair/BMP escape did not normalize to UTF-8 identity");
		for (const auto* invalid : {R"({"interpretation":"domain-\uD83D"})",
									R"({"interpretation":"domain-\uDE00"})",
									R"({"interpretation":"domain-\uD83D\u0041"})"})
		{
			query::ir_node node{"unicode", "query.interpretation_restrict.v1", {"input"}, invalid};
			require(!query::decode_arguments(node), "isolated or malformed surrogate was accepted");
		}
		for (const auto& invalid_utf8 : {std::string{"\xc0\xaf", 2U},
										 std::string{"\x80", 1U},
										 std::string{"\xe2\x82", 2U},
										 std::string{"\xed\xa0\x80", 3U},
										 std::string{"\xf4\x90\x80\x80", 4U}})
		{
			query::ir_node node{"utf8",
								"query.interpretation_restrict.v1",
								{"input"},
								"{\"interpretation\":\"domain-" + invalid_utf8 + "\"}"};
			require(!query::decode_arguments(node), "invalid raw UTF-8 JSON string was accepted");
		}
		for (const auto invalid_space : {'\v', '\f'})
		{
			query::ir_node node{"space",
								"query.limit.v1",
								{"input"},
								std::string{"{\"count\":"} + invalid_space + "0}"};
			require(!query::decode_arguments(node), "non-JSON whitespace was accepted");
		}

		auto canonical_scan_source = query::builder::from(data.left, "items");
		require(canonical_scan_source.has_value(), "canonical scan source failed");
		auto canonical_scan = std::move(*canonical_scan_source).finish();
		auto represented_scan = canonical_scan;
		represented_scan.nodes.front().arguments =
			" \n { \"descriptor_id\" : \"" + data.left.id + "\", \"alias\" : \"\\u0069tems\" } \t ";
		require(represented_scan.validate().has_value(),
				"equivalent whitespace/member-order/Unicode scan representation was rejected");
		require(represented_scan.digest() == canonical_scan.digest() &&
					represented_scan.canonical_form() == canonical_scan.canonical_form(),
				"scan JSON representation changed normalized logical identity");

		const auto predicate_key =
			column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		auto qualified_predicate_key = query::qualify(predicate_key, "company_query_left");
		require(qualified_predicate_key.has_value(), "canonical predicate qualification failed");
		auto first_predicate = query::equals_present(
			*qualified_predicate_key, query::literal::typed("query_key_id", "key:a"));
		auto second_predicate = query::equals_present(
			*qualified_predicate_key, query::literal::typed("query_key_id", "key:b"));
		require(first_predicate && second_predicate, "canonical predicate setup failed");
		const std::array predicate_terms{*first_predicate, *second_predicate};
		auto conjunction = query::all(predicate_terms);
		auto predicate_source = query::builder::from(data.left);
		require(conjunction && predicate_source, "canonical conjunction setup failed");
		auto filtered = std::move(*predicate_source).where(*conjunction);
		require(filtered.has_value(), "canonical conjunction query failed");
		auto canonical_predicate = std::move(*filtered).finish();
		auto represented_predicate = canonical_predicate;
		represented_predicate.nodes.back().arguments = "{\"predicate\":{\"operands\":[" +
			second_predicate->canonical + "," + first_predicate->canonical + "," +
			second_predicate->canonical + "],\"kind\":\"and\"}}";
		require(represented_predicate.validate().has_value() &&
					represented_predicate.digest() == canonical_predicate.digest(),
				"commutative predicate order or duplicate changed normalized identity");

		const std::array condition_values{std::string{"debug"}, std::string{"release"}};
		auto canonical_condition = condition_query(data.left, condition_values);
		auto represented_condition = canonical_condition;
		represented_condition.nodes.back().arguments =
			R"({"universe":"company.query.universe","alternatives":["release","debug","release"]})";
		require(represented_condition.validate().has_value(),
				"equivalent condition set representation was rejected");
		require(represented_condition.digest() == canonical_condition.digest(),
				"condition set order or duplicate changed normalized identity");

		auto interpretation_source = query::builder::from(data.left);
		require(interpretation_source.has_value(), "canonical interpretation source failed");
		auto interpretation_builder =
			std::move(*interpretation_source).interpretation_restrict("company/query");
		require(interpretation_builder.has_value(), "canonical interpretation query failed");
		auto canonical_interpretation = std::move(*interpretation_builder).finish();
		auto escaped_interpretation = canonical_interpretation;
		escaped_interpretation.nodes.back().arguments = R"({"interpretation":"company\/query"})";
		require(escaped_interpretation.validate().has_value() &&
					escaped_interpretation.digest() == canonical_interpretation.digest(),
				"equivalent escaped slash changed normalized identity");
		auto different_interpretation = canonical_interpretation;
		different_interpretation.nodes.back().arguments = R"({"interpretation":"company/other"})";
		require(different_interpretation.validate().has_value() &&
					different_interpretation.digest() != canonical_interpretation.digest(),
				"different typed argument retained the same normalized identity");
		require(represented_scan.canonical_form() == represented_scan.canonical_form(),
				"canonical form was not byte-stable on repeated encoding");

		auto type_mismatch = valid;
		type_mismatch.output_schema.front().type.scalar = scalar_kind::utf8_string;
		type_mismatch.output_schema.front().type.parameter.clear();
		auto rejected_type = type_mismatch.validate();
		require(!rejected_type && rejected_type.error().code == "sdk.query-output-schema-mismatch",
				"output scalar type mismatch was accepted");

		type_mismatch = valid;
		type_mismatch.output_schema.front().type.parameter = "different_query_key_id";
		auto rejected_parameter = type_mismatch.validate();
		require(!rejected_parameter &&
					rejected_parameter.error().code == "sdk.query-output-schema-mismatch",
				"output typed ID parameter mismatch was accepted");

		type_mismatch = valid;
		type_mismatch.output_schema.front().type.optional = true;
		auto rejected_optional = type_mismatch.validate();
		require(!rejected_optional &&
					rejected_optional.error().code == "sdk.query-output-schema-mismatch",
				"output optionality mismatch was accepted");

		type_mismatch = valid;
		type_mismatch.relation_requirements.push_back(data.right);
		type_mismatch.output_schema.front().descriptor_id = data.right.id;
		auto rejected_pair = type_mismatch.validate();
		require(!rejected_pair && rejected_pair.error().code == "sdk.query-output-schema-mismatch",
				"inconsistent output descriptor and column IDs were accepted");

		auto projection_source = query::builder::from(data.left);
		require(projection_source.has_value(), "typed projection validation source failed");
		const std::array projection_columns{valid.output_schema.front()};
		auto projection = std::move(*projection_source).project(projection_columns);
		require(projection.has_value(), "typed projection validation builder failed");
		auto projected = std::move(*projection).finish();
		require(projected.validate().has_value(), "builder projection IR did not validate");
		projected.output_schema.front().type.parameter = "different_query_key_id";
		auto rejected_projection = projected.validate();
		require(!rejected_projection &&
					rejected_projection.error().code == "sdk.query-output-schema-mismatch",
				"projected output type mismatch was accepted");

		auto malformed = valid;
		malformed.version = {2U, 0U, 0U};
		require(!malformed.validate() &&
					malformed.validate().error().code == "sdk.query-ir-invalid",
				"query schema version mismatch was accepted");
		malformed = valid;
		malformed.nodes.front().arguments.pop_back();
		malformed.nodes.front().arguments += ",\"index_name\":\"physical\"}";
		auto physical = malformed.validate();
		require(!physical && physical.error().code == "sdk.query-argument-invalid",
				"physical index leaked into logical IR");

		auto unreachable_scan = valid;
		unreachable_scan.nodes.push_back(
			{"n1", "query.scan.v1", {}, valid.nodes.front().arguments});
		require(unreachable_scan.digest() == valid.digest(),
				"unreachable scan unexpectedly changed rooted canonical identity");
		auto rejected_scan = unreachable_scan.validate();
		require(!rejected_scan && rejected_scan.error().code == "sdk.query-unreachable-node",
				"root-unreachable scan was accepted");

		auto hidden_filter_source = query::builder::from(data.left);
		require(hidden_filter_source.has_value(), "unreachable filter source failed");
		auto hidden_filter =
			std::move(*hidden_filter_source).where(query::is_present(valid.output_schema.front()));
		require(hidden_filter.has_value(), "unreachable filter construction failed");
		auto unreachable_filter = std::move(*hidden_filter).finish();
		unreachable_filter.root = "n0";
		auto rejected_filter = unreachable_filter.validate();
		require(!rejected_filter && rejected_filter.error().code == "sdk.query-unreachable-node",
				"root-unreachable filter was accepted");

		auto hidden_project_source = query::builder::from(data.left);
		require(hidden_project_source.has_value(), "unreachable project source failed");
		const std::array one_column{valid.output_schema.front()};
		auto hidden_project = std::move(*hidden_project_source).project(one_column);
		require(hidden_project.has_value(), "unreachable project construction failed");
		auto unreachable_project = std::move(*hidden_project).finish();
		unreachable_project.root = "n0";
		auto rejected_project = unreachable_project.validate();
		require(!rejected_project && rejected_project.error().code == "sdk.query-unreachable-node",
				"root-unreachable project was accepted");

		auto disconnected = unreachable_scan;
		auto disconnected_filter = unreachable_filter.nodes.back();
		disconnected_filter.id = "n2";
		disconnected_filter.inputs = {"n1"};
		disconnected.nodes.push_back(std::move(disconnected_filter));
		auto rejected_component = disconnected.validate();
		require(!rejected_component &&
					rejected_component.error().code == "sdk.query-unreachable-node",
				"disconnected logical query component was accepted");

		auto unused_requirement = valid;
		unused_requirement.relation_requirements.push_back(data.right);
		auto rejected_requirement = unused_requirement.validate();
		require(!rejected_requirement &&
					rejected_requirement.error().code == "sdk.query-unused-relation-requirement",
				"unused relation requirement was accepted");

		const auto key =
			column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		auto a = query::builder::from(data.left);
		auto b = query::builder::from(data.left);
		auto a_predicate =
			query::equals_present(key, query::literal::typed("query_key_id", "key:a"));
		auto b_predicate =
			query::equals_present(key, query::literal::typed("query_key_id", "key:b"));
		require(a && b && a_predicate && b_predicate, "union branches failed");
		auto a_filtered = std::move(*a).where(*a_predicate);
		auto b_filtered = std::move(*b).where(*b_predicate);
		require(a_filtered && b_filtered, "union branch filter failed");
		auto left_first = std::move(*a_filtered).union_with(*b_filtered);

		a = query::builder::from(data.left);
		b = query::builder::from(data.left);
		a_filtered = std::move(*a).where(*a_predicate);
		b_filtered = std::move(*b).where(*b_predicate);
		require(a_filtered && b_filtered, "reverse union branch filter failed");
		auto right_first = std::move(*b_filtered).union_with(*a_filtered);
		require(left_first && right_first &&
					left_first->ir().digest() == right_first->ir().digest(),
				"commutative union changed normalized IR digest");
	}

	void check_snapshot_schema_compatibility()
	{
		const auto execute =
			[](const snapshot_handle& snapshot, const query::logical_query_ir& logical)
		{
			auto engine = query::reference_engine::bind(snapshot);
			require(engine.has_value(), "schema compatibility reference engine bind failed");
			return engine->execute(logical);
		};

		const auto query_required = schema_relation(0U, true, "schema_query_key_id", false);
		const auto missing_required_snapshot =
			publish_schema_snapshot(schema_relation(2U, false, "schema_query_key_id", false));
		auto missing_required = execute(missing_required_snapshot, scan_query(query_required));
		require(!missing_required &&
					missing_required.error().code == "sdk.query-snapshot-schema-mismatch",
				"larger snapshot minor hid a missing required column");

		const auto mismatched_parameter_snapshot =
			publish_schema_snapshot(schema_relation(1U, true, "foreign_query_key_id", false));
		auto mismatched_parameter =
			execute(mismatched_parameter_snapshot, scan_query(query_required));
		require(!mismatched_parameter &&
					mismatched_parameter.error().code == "sdk.query-snapshot-schema-mismatch",
				"snapshot accepted an incompatible typed ID parameter");

		auto changed_role = schema_relation(1U, true, "schema_query_key_id", false);
		changed_role.columns[1].role = column_role::authoritative_payload;
		changed_role.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 changed_role.contract_digest + "\n" + changed_role.canonical_form());
		const auto changed_role_snapshot = publish_schema_snapshot(std::move(changed_role));
		auto mismatched_role = execute(changed_role_snapshot, scan_query(query_required));
		require(!mismatched_role &&
					mismatched_role.error().code == "sdk.query-snapshot-schema-mismatch",
				"snapshot accepted an incompatible column role");

		const auto query_optional = schema_relation(0U, true, "schema_query_key_id", true);
		const auto missing_optional_snapshot =
			publish_schema_snapshot(schema_relation(1U, true, "schema_query_key_id", false));
		auto required_optional = execute(missing_optional_snapshot, scan_query(query_optional));
		require(!required_optional &&
					required_optional.error().code == "sdk.query-snapshot-schema-mismatch",
				"required access accepted a missing optional column");

		const auto note = query_optional.column(query_optional.id + ".note");
		require(note.has_value(), "optional query note missing");
		auto optional_source = query::builder::from(query_optional);
		require(optional_source.has_value(), "optional projection source failed");
		const std::array optional_columns{column_ref{query_optional.id, note->id, note->type}};
		auto optional_projection = std::move(*optional_source).project(optional_columns);
		require(optional_projection.has_value(), "optional projection failed");
		auto optional_query = std::move(*optional_projection).finish();
		auto& arguments = optional_query.nodes.back().arguments;
		const std::string required_marker{"\"availability\":\"require\""};
		const auto availability = arguments.find(required_marker);
		require(availability != std::string::npos, "optional projection availability missing");
		arguments.replace(
			availability, required_marker.size(), "\"availability\":\"absent_if_schema_missing\"");
		require(optional_query.validate().has_value(),
				"optional absent-if-missing query did not validate");
		auto optional_result = execute(missing_optional_snapshot, optional_query);
		require(optional_result.has_value(),
				"absent-if-missing access rejected an additive optional column");
		auto optional_rows = annotated_rows(*optional_result);
		require(optional_rows.size() == 1U && optional_rows.front().values.size() == 1U,
				"absent-if-missing projection returned the wrong shape");
		const auto& optional_cell = optional_rows.front().values.begin()->second;
		require(optional_cell.state == cell_state::absent && optional_cell.type == note->type,
				"absent-if-missing did not synthesize a typed absent cell");

		const auto additive_snapshot =
			publish_schema_snapshot(schema_relation(1U, true, "schema_query_key_id", true));
		auto additive = execute(additive_snapshot, scan_query(query_required));
		require(additive.has_value() && annotated_rows(*additive).size() == 1U,
				"compatible additive snapshot minor was rejected");
	}

	void check_set_literal_round_trip()
	{
		const auto encode_set = [](const std::initializer_list<std::string_view> values)
		{
			std::vector<std::byte> output;
			for (const auto value : values)
			{
				const auto length = static_cast<std::uint32_t>(value.size());
				for (std::size_t byte{}; byte < sizeof(length); ++byte)
					output.push_back(static_cast<std::byte>(length >> (byte * 8U)));
				for (const auto character : value)
					output.push_back(static_cast<std::byte>(character));
			}
			return output;
		};
		const auto descriptor = set_relation();
		relation_registry registry;
		require(registry.add(descriptor).has_value(), "set literal relation rejected");
		auto relation_engine = registry.build("query-set-literal-generation");
		require(relation_engine.has_value(), "set literal relation engine failed");
		auto store = make_in_memory_snapshot_store(*relation_engine);
		require(store.has_value(), "set literal store failed");
		auto writer = store->begin(draft(*relation_engine));
		require(writer.has_value(), "set literal snapshot writer failed");
		const auto encoded = encode_set({"const", "volatile"});
		auto value = assertion(*relation_engine, set_row(descriptor, encoded), {"release"});
		require(writer->stage(partition(value, 0U)).has_value(), "set literal partition rejected");
		require(writer->validate().has_value(), "set literal snapshot validation failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "set literal snapshot publication failed");

		const auto qualifiers =
			column_ref{descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type};
		const auto make_query = [&](std::vector<std::byte> literal_bytes)
		{
			auto predicate = query::equals_present(
				qualifiers, query::literal::exact(qualifiers.type, scalar_value{literal_bytes}));
			auto source = query::builder::from(descriptor);
			require(predicate && source, "set literal query setup failed");
			auto filtered = std::move(*source).where(*predicate);
			require(filtered.has_value(), "set literal filter failed");
			return std::move(*filtered).finish();
		};

		auto matching = make_query(encoded);
		require(matching.validate().has_value(), "set literal logical IR rejected");
		auto decoded = query::decode_arguments(matching.nodes.back());
		require(decoded.has_value(), "set literal arguments did not decode");
		const auto& decoded_predicate = std::get<query::predicate_arguments>(*decoded).predicate;
		require(
			decoded_predicate.literal_value.has_value() &&
				decoded_predicate.literal_value->type == "set<open_symbol<cc.type-qualifier/1>>" &&
				std::get_if<std::vector<std::byte>>(&decoded_predicate.literal_value->value) !=
					nullptr &&
				std::get<std::vector<std::byte>>(decoded_predicate.literal_value->value) == encoded,
			"set literal decode lost its type parameter or byte-backed value");
		auto bytes_node = matching.nodes.back();
		auto& bytes_arguments = bytes_node.arguments;
		const auto set_type = bytes_arguments.find("set<open_symbol<cc.type-qualifier/1>>");
		require(set_type != std::string::npos, "bytes literal decoder fixture changed");
		bytes_arguments.replace(
			set_type, std::string_view{"set<open_symbol<cc.type-qualifier/1>>"}.size(), "bytes");
		auto decoded_bytes = query::decode_arguments(bytes_node);
		require(decoded_bytes.has_value(), "bytes literal regression did not decode");
		const auto& bytes_literal =
			*std::get<query::predicate_arguments>(*decoded_bytes).predicate.literal_value;
		require(std::get_if<std::vector<std::byte>>(&bytes_literal.value) != nullptr &&
					std::get<std::vector<std::byte>>(bytes_literal.value) == encoded,
				"bytes literal no longer uses byte-backed decoding");

		auto reference = query::reference_engine::bind(*snapshot);
		require(reference.has_value(), "set literal reference engine bind failed");
		auto matched = reference->execute(matching);
		require(matched.has_value() && rows(*matched).size() == 1U,
				"equal set literal did not match its row");
		auto unmatched = reference->execute(make_query(encode_set({"mutable"})));
		require(unmatched.has_value() && rows(*unmatched).empty(),
				"different set literal matched a row");

		auto empty = make_query({});
		auto empty_arguments = query::decode_arguments(empty.nodes.back());
		require(empty_arguments.has_value(), "empty set literal did not round-trip");
		const auto& empty_literal =
			*std::get<query::predicate_arguments>(*empty_arguments).predicate.literal_value;
		require(std::get_if<std::vector<std::byte>>(&empty_literal.value) != nullptr &&
					std::get<std::vector<std::byte>>(empty_literal.value).empty(),
				"empty set literal decoded as a string");

		constexpr std::string_view encoded_hex{"05000000636f6e737408000000766f6c6174696c65"};
		for (const auto* malformed : {"0", "0A", "0g"})
		{
			auto invalid = matching;
			auto& arguments = invalid.nodes.back().arguments;
			const auto encoded_value =
				arguments.find("\"value\":\"" + std::string{encoded_hex} + "\"");
			require(encoded_value != std::string::npos, "set literal fixture encoding changed");
			arguments.replace(encoded_value,
							  encoded_hex.size() + std::string_view{"\"value\":\"\""}.size(),
							  "\"value\":\"" + std::string{malformed} + "\"");
			require(!invalid.validate(), "malformed or noncanonical set hex was accepted");
		}

		for (auto invalid_bytes : {encode_set({"volatile", "const"}),
								   encode_set({"const", "const"}),
								   std::vector<std::byte>{std::byte{1U}}})
		{
			auto rejected = query::equals_present(
				qualifiers,
				query::literal::exact(qualifiers.type, scalar_value{std::move(invalid_bytes)}));
			require(!rejected && rejected.error().detail == "set-encoding",
					"noncanonical set bytes were accepted by the typed query builder");
		}

		auto noncanonical_dynamic = matching;
		auto& dynamic_arguments = noncanonical_dynamic.nodes.back().arguments;
		const auto dynamic_value =
			dynamic_arguments.find("\"value\":\"" + std::string{encoded_hex} + "\"");
		require(dynamic_value != std::string::npos, "dynamic set fixture encoding changed");
		dynamic_arguments.replace(dynamic_value,
								  encoded_hex.size() + std::string_view{"\"value\":\"\""}.size(),
								  "\"value\":\"08000000766f6c6174696c6505000000636f6e7374\"");
		require(!noncanonical_dynamic.validate(),
				"noncanonical set bytes were accepted by dynamic Logical Query IR");

		auto wrong_parameter = matching;
		auto& arguments = wrong_parameter.nodes.back().arguments;
		const auto parameter = arguments.find("open_symbol<cc.type-qualifier/1>");
		require(parameter != std::string::npos, "set literal parameter fixture changed");
		arguments.replace(parameter,
						  std::string_view{"open_symbol<cc.type-qualifier/1>"}.size(),
						  "open_symbol<cc.attribute/1>");
		require(!wrong_parameter.validate(), "mismatched set type parameter was accepted");
	}

	void check_runtime_matrix(const fixture& data,
							  const snapshot_handle& memory_snapshot,
							  const snapshot_handle& sqlite_snapshot)
	{
		auto memory_engine = query::reference_engine::bind(memory_snapshot);
		auto sqlite_engine = query::reference_engine::bind(sqlite_snapshot);
		require(memory_engine && sqlite_engine, "reference engine bind failed");
		auto foreign_schema = scan_query(data.left);
		foreign_schema.relation_requirements.front().columns.front().type.parameter =
			"foreign_query_key_id";
		foreign_schema.relation_requirements.front().descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 foreign_schema.relation_requirements.front().contract_digest + "\n" +
								 foreign_schema.relation_requirements.front().canonical_form());
		foreign_schema.output_schema.front().type.parameter = "foreign_query_key_id";
		require(foreign_schema.validate().has_value(),
				"self-consistent external IR schema did not validate");
		auto foreign_execution = memory_engine->execute(foreign_schema);
		require(!foreign_execution &&
					foreign_execution.error().code == "sdk.query-snapshot-schema-mismatch",
				"snapshot execution accepted an incompatible advertised output type");

		const auto left_key =
			column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		const auto left_value =
			column_ref{data.left.id, data.left.columns[1].id, data.left.columns[1].type};
		const auto left_note =
			column_ref{data.left.id, data.left.columns[2].id, data.left.columns[2].type};
		const auto right_key =
			column_ref{data.right.id, data.right.columns[0].id, data.right.columns[0].type};

		std::vector<query::logical_query_ir> queries;
		queries.push_back(scan_query(data.left));

		auto filtered = query::builder::from(data.left);
		require(filtered.has_value(), "filter source failed");
		auto unknown = std::move(*filtered).where(query::is_unknown(left_note));
		require(unknown.has_value(), "unknown filter failed");
		queries.push_back(std::move(*unknown).finish());

		auto absent = query::builder::from(data.left);
		require(absent.has_value(), "absent source failed");
		auto absent_filtered = std::move(*absent).where(query::is_absent(left_note));
		require(absent_filtered.has_value(), "absent filter failed");
		queries.push_back(std::move(*absent_filtered).finish());

		auto projected = query::builder::from(data.left);
		require(projected.has_value(), "project source failed");
		const std::array projected_columns{left_key, left_value};
		auto project = std::move(*projected).project(projected_columns);
		require(project.has_value(), "project failed");
		queries.push_back(std::move(*project).finish());

		auto joined_left = query::builder::from(data.left);
		auto joined_right = query::builder::from(data.right);
		auto join_predicate = query::equals_present(left_key, right_key);
		require(joined_left && joined_right && join_predicate, "join setup failed");
		auto joined = std::move(*joined_left).inner_join(std::move(*joined_right), *join_predicate);
		require(joined.has_value(), "inner join failed");
		queries.push_back(std::move(*joined).finish());

		auto semi_left = query::builder::from(data.left);
		auto semi_right = query::builder::from(data.right);
		require(semi_left && semi_right, "semi join setup failed");
		auto semi = std::move(*semi_left).semi_join(std::move(*semi_right), *join_predicate);
		require(semi.has_value(), "semi join failed");
		queries.push_back(std::move(*semi).finish());

		auto ordered_semi_left = query::builder::from(data.left);
		auto ordered_semi_right = query::builder::from(data.right);
		require(ordered_semi_left && ordered_semi_right, "ordered semi join setup failed");
		const std::array semi_order_columns{left_key};
		auto ordered_semi_source = std::move(*ordered_semi_left).order_by(semi_order_columns);
		require(ordered_semi_source.has_value(), "ordered semi join left order failed");
		auto ordered_semi = std::move(*ordered_semi_source)
								.semi_join(std::move(*ordered_semi_right), *join_predicate);
		require(ordered_semi.has_value(), "ordered semi join failed");
		auto limited_semi = std::move(*ordered_semi).limit(1U);
		require(limited_semi.has_value(), "semi join lost the left total order before limit");
		auto limited_semi_ir = std::move(*limited_semi).finish();
		require(limited_semi_ir.validate().has_value(),
				"builder semi join properties diverged from IR validation");

		auto unordered_semi_left = query::builder::from(data.left);
		auto unordered_semi_right = query::builder::from(data.right);
		require(unordered_semi_left && unordered_semi_right, "unordered semi join setup failed");
		auto unordered_semi = std::move(*unordered_semi_left)
								  .semi_join(std::move(*unordered_semi_right), *join_predicate);
		require(unordered_semi.has_value(), "unordered semi join failed");
		auto rejected_semi_limit = std::move(*unordered_semi).limit(1U);
		require(!rejected_semi_limit &&
					rejected_semi_limit.error().code == "sdk.query-limit-requires-order",
				"unordered semi join incorrectly acquired a total order");

		auto union_left = query::builder::from(data.left);
		auto union_right = query::builder::from(data.left);
		require(union_left && union_right, "union setup failed");
		auto united = std::move(*union_left).union_with(*union_right);
		require(united.has_value(), "union failed");
		auto distinct = std::move(*united).distinct();
		require(distinct.has_value(), "distinct failed");
		queries.push_back(std::move(*distinct).finish());

		auto ordered = query::builder::from(data.left);
		require(ordered.has_value(), "order source failed");
		const std::array order_columns{left_value};
		auto order = std::move(*ordered).order_by(order_columns);
		require(order.has_value(), "order by failed");
		auto limited = std::move(*order).limit(1U);
		require(limited.has_value(), "ordered limit failed");
		queries.push_back(std::move(*limited).finish());

		auto condition = query::builder::from(data.left);
		require(condition.has_value(), "condition source failed");
		const std::array alternatives{std::string{"debug"}};
		auto restricted =
			std::move(*condition).condition_restrict("company.query.universe", alternatives);
		require(restricted.has_value(), "condition restriction failed");
		queries.push_back(std::move(*restricted).finish());

		auto interpretation = query::builder::from(data.right);
		require(interpretation.has_value(), "interpretation source failed");
		auto domain = std::move(*interpretation).interpretation_restrict("company.query.domain");
		require(domain.has_value(), "interpretation restriction failed");
		queries.push_back(std::move(*domain).finish());

		for (const auto& logical : queries)
		{
			auto memory = memory_engine->execute(logical);
			auto sqlite = sqlite_engine->execute(logical);
			require(memory && sqlite, "query runtime execution failed");
			require(rows(*memory) == rows(*sqlite), "memory/SQLite semantic rows diverged");
			require(memory->execution() == sqlite->execution() &&
						memory->inputs_complete() == sqlite->inputs_complete() &&
						memory->summary_guarantee().approximation ==
							sqlite->summary_guarantee().approximation &&
						memory->summary_guarantee().fragment_set_digest ==
							sqlite->summary_guarantee().fragment_set_digest &&
						memory->summary_guarantee().condition_partition.canonical_form() ==
							sqlite->summary_guarantee().condition_partition.canonical_form() &&
						memory->summary_guarantee().interpretation_partitions ==
							sqlite->summary_guarantee().interpretation_partitions &&
						memory->summary_guarantee().assumptions ==
							sqlite->summary_guarantee().assumptions &&
						memory->summary_guarantee().verification_modalities ==
							sqlite->summary_guarantee().verification_modalities,
					"memory/SQLite query side channels diverged");
			require(memory->logical_ir_digest() == logical.digest(),
					"query result lost logical IR digest");
			require(memory->explain_physical().text.contains("backend=memory") &&
						sqlite->explain_physical().text.contains("backend=sqlite"),
					"physical explanation did not expose backend-only information");
			require(!memory->explain_logical().text.contains("backend="),
					"physical backend leaked into logical explanation");
			for (const auto& row : annotated_rows(*memory))
				for (const auto& [column, cell] : row.values)
				{
					(void)cell;
					require(
						column.starts_with("output.") &&
							memory->explain_logical().text.contains("\"id\":\"" + column + "\""),
						"runtime row key diverged from logical output schema");
				}
			require(occurrences(memory->explain_physical().text, ";node=") == logical.nodes.size(),
					"executor evaluation set diverged from validated rooted node set");
		}
		auto limited_semi_memory = memory_engine->execute(limited_semi_ir);
		auto limited_semi_sqlite = sqlite_engine->execute(limited_semi_ir);
		require(limited_semi_memory && limited_semi_sqlite && limited_semi_memory->ordered() &&
					limited_semi_sqlite->ordered() &&
					rows(*limited_semi_memory) == rows(*limited_semi_sqlite) &&
					rows(*limited_semi_memory).size() == 1U,
				"semi join execution did not preserve the ordered left filtered subsequence");

		auto implicit_scan = memory_engine->execute(queries.front());
		auto explicit_source = query::builder::from(data.left);
		require(implicit_scan && explicit_source, "implicit/explicit projection setup failed");
		std::vector<column_ref> all_left_columns;
		for (const auto& column : data.left.columns)
			all_left_columns.push_back({data.left.id, column.id, column.type});
		auto explicit_builder = std::move(*explicit_source).project(all_left_columns);
		require(explicit_builder.has_value(), "equivalent explicit projection failed");
		auto explicit_query = std::move(*explicit_builder).finish();
		auto explicit_result = memory_engine->execute(explicit_query);
		require(explicit_result && explicit_query.digest() == queries.front().digest() &&
					rows(*explicit_result) == rows(*implicit_scan),
				"equivalent implicit and explicit projections diverged");

		auto join_memory = memory_engine->execute(queries[4]);
		require(join_memory && rows(*join_memory).size() == 3U,
				"condition/interpretation-aware inner join diverged");
		const std::set<std::string, std::less<>> join_columns{"output.company_query_left_key",
															  "output.company_query_right_key",
															  "output.label",
															  "output.note",
															  "output.value"};
		const auto joined_rows = annotated_rows(*join_memory);
		require(!joined_rows.empty() && row_keys(joined_rows.front()) == join_columns,
				"implicit join projection did not use deterministic duplicate aliases");
		auto distinct_memory = memory_engine->execute(queries[6]);
		require(distinct_memory && rows(*distinct_memory).size() == 2U,
				"distinct did not merge conditions/evidence deterministically");
		auto limited_memory = memory_engine->execute(queries[7]);
		require(limited_memory && rows(*limited_memory).size() == 1U && limited_memory->ordered() &&
					limited_memory->summary_guarantee().approximation == "under_approximation",
				"ordered limit did not preserve total-order under-approximation");
		require(limited_memory->execution() == query::execution_status::complete &&
					limited_memory->inputs_complete() && !limited_memory->closed(),
				"success, input completeness, and closure were collapsed");
	}

	void check_self_join_occurrences(const fixture& data,
									 const snapshot_handle& memory_snapshot,
									 const snapshot_handle& sqlite_snapshot)
	{
		const column_ref key{data.self.id, data.self.columns[0].id, data.self.columns[0].type};
		const column_ref parent{data.self.id, data.self.columns[1].id, data.self.columns[1].type};
		auto left_key = query::qualify(key, "left");
		auto right_key = query::qualify(key, "right");
		auto left_parent = query::qualify(parent, "left");
		require(left_key && right_key && left_parent, "self-join occurrence qualification failed");
		require(!query::qualify(key, "Invalid-Alias"),
				"invalid query occurrence alias was accepted");

		auto ambiguous_left = query::builder::from(data.self, "left");
		auto ambiguous_right = query::builder::from(data.self, "right");
		auto ambiguous_predicate = query::equals_present(key, key);
		require(ambiguous_left && ambiguous_right && ambiguous_predicate,
				"ambiguous self-join setup failed");
		auto ambiguous = std::move(*ambiguous_left)
							 .inner_join(std::move(*ambiguous_right), *ambiguous_predicate);
		require(!ambiguous && ambiguous.error().code == "sdk.query-column-ambiguous",
				"unqualified self-join column did not fail as ambiguous");

		auto left = query::builder::from(data.self, "left");
		auto right = query::builder::from(data.self, "right");
		require(left.has_value(), "qualified self-join left scan failed");
		auto filtered = std::move(*left).where(query::is_present(*left_key));
		auto predicate = query::equals_present(*left_parent, *right_key);
		require(filtered && right && predicate, "qualified self-join setup failed");
		auto joined = std::move(*filtered).inner_join(std::move(*right), *predicate);
		require(joined.has_value(), "qualified self-join construction failed");
		const std::array order_columns{*right_key};
		auto ordered = std::move(*joined).order_by(order_columns);
		require(ordered.has_value(), "qualified self-join order failed");
		const std::array projection{*left_key, *right_key};
		auto projected = std::move(*ordered).project(projection);
		require(projected.has_value(), "qualified self-join projection failed");
		auto logical = std::move(*projected).finish();
		auto valid = logical.validate();
		require(valid.has_value(),
				"qualified self-join IR did not validate: " +
					(valid ? std::string{} : valid.error().code + ":" + valid.error().field));

		auto execute = [&](const snapshot_handle& snapshot)
		{
			auto engine = query::reference_engine::bind(snapshot);
			require(engine.has_value(), "self-join reference engine bind failed");
			return engine->execute(logical);
		};
		auto memory = execute(memory_snapshot);
		auto sqlite = execute(sqlite_snapshot);
		require(memory && sqlite && rows(*memory) == rows(*sqlite),
				"self-join memory/SQLite parity failed");
		const auto result_rows = annotated_rows(*memory);
		require(result_rows.size() == 2U && memory->ordered(),
				"self-join returned an unexpected row count or order guarantee");
		require(std::ranges::all_of(result_rows,
									[](const query::annotated_row& row)
									{
										return row.claim_contributors.size() == 2U;
									}),
				"self-join collapsed left/right claim contributors");
		const std::set<std::pair<std::string, std::string>> pairs{
			{present_string(result_rows[0], "output.left_key"),
			 present_string(result_rows[0], "output.right_key")},
			{present_string(result_rows[1], "output.left_key"),
			 present_string(result_rows[1], "output.right_key")},
		};
		require(pairs ==
					std::set<std::pair<std::string, std::string>>{{"key:b", "key:a"},
																  {"key:c", "key:a"}},
				"self-join collapsed the left and right occurrence values");

		auto left_scan = query::builder::from(data.self, "left");
		auto right_scan = query::builder::from(data.self, "right");
		require(left_scan && right_scan &&
					std::move(*left_scan).finish().digest() !=
						std::move(*right_scan).finish().digest(),
				"semantic scan occurrence aliases did not affect the logical digest");
	}

	void check_partiality(const fixture& data, const snapshot_handle& snapshot)
	{
		auto engine = query::reference_engine::bind(snapshot);
		require(engine.has_value(), "partiality engine bind failed");
		const auto logical = scan_query(data.left);

		query::execution_request budget;
		budget.budget.max_rows_output = 1U;
		auto truncated = engine->execute(logical, budget);
		require(truncated && truncated->execution() == query::execution_status::truncated &&
					rows(*truncated).size() == 1U && !truncated->unresolved_items().empty(),
				"output budget did not return deterministic unresolved prefix");

		ordinal_cancel cancel_after_one{1U};
		query::execution_request cancellation;
		cancellation.cancellation = &cancel_after_one;
		auto cancelled = engine->execute(logical, cancellation);
		require(cancelled &&
					cancelled->execution() == query::execution_status::cancelled_with_partial &&
					rows(*cancelled).size() == 1U && !cancelled->unresolved_items().empty(),
				"cancellation did not return deterministic sealed partial result");

		immediate_cancel cancel_now;
		cancellation.cancellation = &cancel_now;
		auto failed = engine->execute(logical, cancellation);
		require(failed && failed->execution() == query::execution_status::failed_before_result &&
					rows(*failed).empty(),
				"pre-execution cancellation published rows");

		auto cursor = truncated->rows();
		auto first = cursor.next();
		require(first && *first && (*first)->copy().has_value(),
				"query result row view unavailable");
		require(cursor.next().has_value(), "query result cursor advance failed");
		auto expired = (*first)->copy();
		require(!expired && expired.error().code == "sdk.query-row-view-expired",
				"advanced query cursor left prior row view live");
	}

	void check_canonical_union_execution(const fixture& data,
										 const snapshot_handle& memory_snapshot,
										 const snapshot_handle& sqlite_snapshot)
	{
		const auto make_union = [&](const bool reverse)
		{
			const auto key =
				column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
			auto a = query::builder::from(data.left);
			auto b = query::builder::from(data.left);
			auto a_predicate =
				query::equals_present(key, query::literal::typed("query_key_id", "key:a"));
			auto b_predicate =
				query::equals_present(key, query::literal::typed("query_key_id", "key:b"));
			require(a && b && a_predicate && b_predicate, "budgeted union setup failed");
			auto a_filtered = std::move(*a).where(*a_predicate);
			auto b_filtered = std::move(*b).where(*b_predicate);
			require(a_filtered && b_filtered, "budgeted union filters failed");
			auto united = reverse ? std::move(*b_filtered).union_with(*a_filtered)
								  : std::move(*a_filtered).union_with(*b_filtered);
			require(united.has_value(), "budgeted union construction failed");
			return std::move(*united).finish();
		};

		const auto left_first = make_union(false);
		const auto right_first = make_union(true);
		require(left_first.digest() == right_first.digest(),
				"union construction order changed logical identity");
		for (const auto* snapshot : {&memory_snapshot, &sqlite_snapshot})
		{
			auto engine = query::reference_engine::bind(*snapshot);
			require(engine.has_value(), "canonical union engine bind failed");
			std::array<query::execution_request, 4U> requests{};
			requests[1].budget.max_rows_scanned = 0U;
			requests[2].budget.max_rows_scanned = 1U;
			requests[3].budget.max_intermediate_rows = 1U;
			for (const auto& request : requests)
			{
				auto left_result = engine->execute(left_first, request);
				auto right_result = engine->execute(right_first, request);
				require(left_result && right_result,
						"canonical union execution returned an API error");
				require(left_result->canonical_form() == right_result->canonical_form(),
						"union branch order changed a budgeted canonical result");
			}
		}
	}

	void check_bounded_intermediate_budgets(const fixture& data)
	{
		auto store = make_in_memory_snapshot_store(data.engine);
		require(store.has_value(), "budget query store failed");
		auto snapshot = publish(*store, data, false, false);
		auto engine = query::reference_engine::bind(snapshot);
		require(engine.has_value(), "budget query engine bind failed");
		const auto left_key =
			column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		const auto right_key =
			column_ref{data.right.id, data.right.columns[0].id, data.right.columns[0].type};
		auto join_left = query::builder::from(data.left);
		auto join_right = query::builder::from(data.right);
		auto predicate = query::equals_present(left_key, right_key);
		require(join_left && join_right && predicate, "budget join setup failed");
		auto joined =
			std::move(*join_left).inner_join(std::move(*join_right), std::move(*predicate));
		require(joined.has_value(), "budget join construction failed");
		const auto join_query = std::move(*joined).finish();

		auto baseline = engine->execute(join_query);
		require(baseline && rows(*baseline).size() == 64U,
				"budget join fixture did not produce a 64-row product");
		const auto peak_bytes = physical_metric(
			baseline->explain_physical().text, "peak-logical-bytes=", "peak-logical-bytes=");
		require(peak_bytes > 1U, "budget accounting did not observe retained bytes");

		query::execution_request zero_rows;
		zero_rows.budget.max_intermediate_rows = 0U;
		auto zero = engine->execute(scan_query(data.left), zero_rows);
		require(zero && zero->execution() == query::execution_status::failed_before_result &&
					!zero->unresolved_items().empty() &&
					zero->unresolved_items().front().code == "sdk.query-intermediate-budget" &&
					physical_metric(zero->explain_physical().text,
									"peak-intermediate-rows=",
									"peak-intermediate-rows=") == 0U,
				"zero intermediate budget retained a row");
		query::execution_request zero_memory;
		zero_memory.budget.max_memory_bytes = 0U;
		auto no_memory = engine->execute(scan_query(data.left), zero_memory);
		require(no_memory &&
					no_memory->execution() == query::execution_status::failed_before_result &&
					!no_memory->unresolved_items().empty() &&
					no_memory->unresolved_items().front().code == "sdk.query-memory-budget" &&
					physical_metric(no_memory->explain_physical().text,
									"peak-logical-bytes=",
									"peak-logical-bytes=") == 0U,
				"zero memory budget retained logical payload");

		query::execution_request eight_rows;
		eight_rows.budget.max_intermediate_rows = 8U;
		auto bounded_join = engine->execute(join_query, eight_rows);
		require(bounded_join &&
					bounded_join->execution() == query::execution_status::failed_before_result &&
					!bounded_join->unresolved_items().empty() &&
					bounded_join->unresolved_items().front().code ==
						"sdk.query-intermediate-budget" &&
					bounded_join->unresolved_items().front().subject == join_query.root &&
					physical_metric(bounded_join->explain_physical().text,
									"peak-intermediate-rows=",
									"peak-intermediate-rows=") == 8U,
				"join materialized its 64-row product beyond an eight-row budget");
		auto reverse_store = make_in_memory_snapshot_store(data.engine);
		require(reverse_store.has_value(), "reverse budget query store failed");
		auto reverse_snapshot = publish(*reverse_store, data, true, false);
		auto reverse_engine = query::reference_engine::bind(reverse_snapshot);
		require(reverse_engine.has_value(), "reverse budget query engine bind failed");
		auto reverse_bounded = reverse_engine->execute(join_query, eight_rows);
		require(reverse_bounded && reverse_bounded->execution() == bounded_join->execution() &&
					reverse_bounded->unresolved_items().front().code ==
						bounded_join->unresolved_items().front().code &&
					reverse_bounded->unresolved_items().front().subject ==
						bounded_join->unresolved_items().front().subject,
				"input order changed budget exhaustion status or reason");

		query::execution_request exact_memory;
		exact_memory.budget.max_memory_bytes = peak_bytes;
		auto exact = engine->execute(join_query, exact_memory);
		require(exact && exact->execution() == query::execution_status::complete,
				"exact logical memory boundary was rejected");
		exact_memory.budget.max_memory_bytes = peak_bytes + 1U;
		auto below_limit = engine->execute(join_query, exact_memory);
		require(below_limit && below_limit->execution() == query::execution_status::complete,
				"logical memory usage below the limit was rejected");
		exact_memory.budget.max_memory_bytes = peak_bytes - 1U;
		auto over_limit = engine->execute(join_query, exact_memory);
		require(over_limit &&
					over_limit->execution() == query::execution_status::failed_before_result &&
					!over_limit->unresolved_items().empty() &&
					over_limit->unresolved_items().front().code == "sdk.query-memory-budget",
				"one byte beyond the logical memory budget was accepted");

		auto semi_left = query::builder::from(data.left);
		auto semi_right = query::builder::from(data.right);
		predicate = query::equals_present(left_key, right_key);
		require(semi_left && semi_right && predicate, "budget semi-join setup failed");
		auto semi = std::move(*semi_left).semi_join(std::move(*semi_right), std::move(*predicate));
		require(semi.has_value(), "budget semi-join construction failed");
		const auto semi_query = std::move(*semi).finish();
		auto semi_baseline = engine->execute(semi_query);
		require(semi_baseline && rows(*semi_baseline).size() == 8U,
				"budget semi-join fixture did not reduce witnesses");
		const auto retained_after_scans = physical_metric(
			semi_baseline->explain_physical().text, ";node=n1:", ",retained-bytes=");
		query::execution_request witness_budget;
		witness_budget.budget.max_memory_bytes = retained_after_scans;
		auto bounded_witnesses = engine->execute(semi_query, witness_budget);
		require(
			bounded_witnesses &&
				bounded_witnesses->execution() == query::execution_status::failed_before_result &&
				!bounded_witnesses->unresolved_items().empty() &&
				bounded_witnesses->unresolved_items().front().code == "sdk.query-memory-budget" &&
				bounded_witnesses->unresolved_items().front().subject == semi_query.root,
			"semi-join witness accumulation escaped the memory budget");
	}

	void check_side_channel_parity(const fixture& data)
	{
		claim_conflict base{"company.query.left",
							"key:conflict",
							"company.query.domain",
							{"debug", "release"},
							{"assertion:a", "assertion:b"},
							{"content:a", "content:b"}};
		auto reordered = base;
		std::ranges::reverse(reordered.overlap_fragments);
		std::ranges::reverse(reordered.assertions);
		std::ranges::reverse(reordered.contents);
		require(cxxlens::sdk::detail::canonical_claim_conflict_json(base) ==
					cxxlens::sdk::detail::canonical_claim_conflict_json(reordered),
				"conflict insertion order changed canonical JSON");
		auto changed_contents = base;
		changed_contents.contents.back() = "content:c";
		require(cxxlens::sdk::detail::canonical_claim_conflict_json(base) !=
					cxxlens::sdk::detail::canonical_claim_conflict_json(changed_contents),
				"conflict contents were omitted from canonical JSON");
		auto changed_overlap = base;
		changed_overlap.overlap_fragments.back() = "testing";
		require(cxxlens::sdk::detail::canonical_claim_conflict_json(base) !=
					cxxlens::sdk::detail::canonical_claim_conflict_json(changed_overlap),
				"conflict overlap was omitted from canonical JSON");

		auto store = make_in_memory_snapshot_store(data.engine);
		require(store.has_value(), "side-channel query store failed");
		auto snapshot = publish(*store, data, false, false);
		auto query_engine = query::reference_engine::bind(snapshot);
		require(query_engine.has_value(), "side-channel query engine bind failed");
		auto queried = query_engine->execute(scan_query(data.left));
		require(queried && queried->conflicts().size() == 1U &&
					queried->differential_disagreements().size() == 1U,
				"query side channel used claim content identity as functional payload identity");
		require(queried->summary_guarantee().approximation != "exact" &&
					std::ranges::any_of(queried->summary_guarantee().fragments,
										[](const query::query_guarantee_fragment& fragment)
										{
											return fragment.conflicting;
										}),
				"overlapping same-domain conflict did not block exact summary");
		const column_ref key{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		auto predicate =
			query::equals_present(key, query::literal::typed("query_key_id", "key:same-domain"));
		auto source = query::builder::from(data.left);
		require(predicate && source, "conflict scope filter setup failed");
		auto filtered_builder = std::move(*source).where(*predicate);
		require(filtered_builder.has_value(), "conflict scope filter rejected");
		auto filtered = query_engine->execute(std::move(*filtered_builder).finish());
		require(filtered &&
					std::ranges::none_of(filtered->summary_guarantee().fragments,
										 [](const query::query_guarantee_fragment& fragment)
										 {
											 return fragment.conflicting;
										 }),
				"filtered-out same-domain conflict contaminated surviving scope");

		claim_batch batch;
		for (const auto& value : data.claims)
			require(batch.add(value).has_value(), "side-channel kernel claim rejected");
		auto committed = std::move(batch).commit(data.engine);
		require(committed && committed->conflicts.size() == 1U &&
					committed->differential_disagreements.size() == 1U,
				"claim kernel side-channel fixture did not classify exact disagreements");

		const auto& query_conflict = queried->conflicts().front();
		const auto& kernel_conflict = committed->conflicts.front();
		require(query_conflict.relation == kernel_conflict.relation &&
					query_conflict.semantic_key == kernel_conflict.semantic_key &&
					query_conflict.interpretation == kernel_conflict.interpretation &&
					query_conflict.overlap_fragments == kernel_conflict.overlap_fragments &&
					query_conflict.assertions == kernel_conflict.assertions &&
					query_conflict.contents == kernel_conflict.contents,
				"claim kernel and query conflict classification diverged");
		const auto canonical_result = queried->canonical_form();
		require(canonical_result.contains("\"conflicts\":[{") &&
					canonical_result.contains("\"overlap_fragments\":[") &&
					canonical_result.contains("\"assertions\":[") &&
					canonical_result.contains("\"contents\":["),
				"canonical query result did not encode structured conflict fields");
		for (const auto& value : query_conflict.overlap_fragments)
			require(canonical_result.contains(value),
					"canonical query result lost a conflict overlap fragment");
		for (const auto& value : query_conflict.assertions)
			require(canonical_result.contains(value),
					"canonical query result lost a conflict assertion");
		for (const auto& value : query_conflict.contents)
			require(canonical_result.contains(value),
					"canonical query result lost a conflict content identity");

		const auto& query_differential = queried->differential_disagreements().front();
		const auto& kernel_differential = committed->differential_disagreements.front();
		require(query_differential.relation == kernel_differential.relation &&
					query_differential.semantic_key == kernel_differential.semantic_key &&
					query_differential.left_interpretation ==
						kernel_differential.left_interpretation &&
					query_differential.right_interpretation ==
						kernel_differential.right_interpretation &&
					query_differential.left_content == kernel_differential.left_content &&
					query_differential.right_content == kernel_differential.right_content &&
					query_differential.overlap_fragments == kernel_differential.overlap_fragments,
				"claim kernel and query differential classification diverged");
	}

	void check_claim_occurrence_aggregation(const fixture& data)
	{
		auto base = data.claims.front();
		auto provenance = base;
		provenance.provenance_root = "evidence:alternate";
		auto producer = base;
		producer.producer.id = "company.query.alternate-provider";
		auto guarantee = base;
		guarantee.guarantee.approximation = "under_approximation";
		auto occurrence_partition = partition(base, 900U);
		occurrence_partition.claims = {base, provenance, producer, guarantee, base};
		auto store = make_in_memory_snapshot_store(data.engine);
		require(store.has_value(), "query occurrence store unavailable");
		auto writer = store->begin(draft(data.engine));
		require(writer && writer->stage(std::move(occurrence_partition)) && writer->validate(),
				"query occurrence snapshot setup failed");
		auto snapshot = writer->publish();
		require(snapshot.has_value(), "query occurrence snapshot publication failed");
		auto engine = query::reference_engine::bind(std::move(*snapshot));
		require(engine.has_value(), "query occurrence engine bind failed");
		auto result = engine->execute(scan_query(data.left));
		require(result && result->producer_contracts().size() == 2U &&
					result->summary_guarantee().approximation == "under_approximation",
				"query result lost occurrence producer or conservative guarantee evidence");
		auto rows = result->rows();
		auto first = rows.next();
		require(first && *first, "query occurrence semantic row missing");
		auto aggregated = (*first)->copy();
		auto end = rows.next();
		require(aggregated && aggregated->producer_contracts.size() == 2U &&
					aggregated->provenance.size() == 2U &&
					aggregated->contributor_guarantees.size() == 2U && end && !*end,
				"query scan duplicated semantic content or dropped occurrence metadata");

		auto exact_row = *aggregated;
		exact_row.contributor_guarantees = {base.guarantee};
		auto under_row = exact_row;
		under_row.contributor_guarantees = {guarantee.guarantee};
		require(exact_row.validate() && under_row.validate() &&
					exact_row.canonical_form() != under_row.canonical_form(),
				"row-level guarantee did not affect canonical identity");
		auto forward = exact_row;
		forward.contributor_guarantees = {base.guarantee, guarantee.guarantee};
		auto reverse = exact_row;
		reverse.contributor_guarantees = {guarantee.guarantee, base.guarantee};
		require(forward.validate() && reverse.validate() &&
					forward.canonical_form() == reverse.canonical_form(),
				"guarantee insertion order changed canonical row identity");

		auto second_exact = exact_row;
		second_exact.values.at("output.value") = detached_cell::signed_integer(3);
		auto second_under = under_row;
		second_under.values.at("output.value") = detached_cell::signed_integer(3);
		const auto canonical_rows = [](std::vector<query::annotated_row> values)
		{
			std::vector<std::string> forms;
			forms.reserve(values.size());
			for (const auto& value : values)
				forms.push_back(value.canonical_form());
			std::ranges::sort(forms);
			return forms;
		};
		require(canonical_rows({exact_row, second_under}) !=
					canonical_rows({under_row, second_exact}),
				"swapping guarantees between result rows preserved canonical identity");

		const auto canonical_result = result->canonical_form();
		require(canonical_result.contains("\"contributor_guarantees\":[{") &&
					canonical_result.contains("\"approximation\":\"exact\"") &&
					canonical_result.contains("\"approximation\":\"under_approximation\"") &&
					canonical_result.contains("\"verification_modalities\":["),
				"query result canonical form lost structured row guarantees");

		const auto key =
			column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		auto left_key = query::qualify(key, "left");
		auto right_key = query::qualify(key, "right");
		require(left_key && right_key, "guarantee operator qualification failed");
		auto predicate = query::equals_present(*left_key, *right_key);
		require(predicate.has_value(), "guarantee operator predicate failed");
		for (const bool semi : {false, true})
		{
			auto left = query::builder::from(data.left, "left");
			auto right = query::builder::from(data.left, "right");
			require(left && right, "guarantee join source failed");
			auto joined = semi ? std::move(*left).semi_join(std::move(*right), *predicate)
							   : std::move(*left).inner_join(std::move(*right), *predicate);
			require(joined.has_value(), "guarantee join construction failed");
			auto joined_result = engine->execute(std::move(*joined).finish());
			require(joined_result.has_value(), "guarantee join execution failed");
			auto joined_rows = annotated_rows(*joined_result);
			require(
				joined_rows.size() == 1U &&
					joined_rows.front().contributor_guarantees.size() == 2U &&
					joined_rows.front().canonical_form().contains("\"contributor_guarantees\":[{"),
				"join/semi-join lost row-level guarantee attribution");
		}
		auto distinct_source = query::builder::from(data.left);
		require(distinct_source.has_value(), "guarantee distinct source failed");
		auto distinct = std::move(*distinct_source).distinct();
		require(distinct.has_value(), "guarantee distinct construction failed");
		auto distinct_result = engine->execute(std::move(*distinct).finish());
		require(distinct_result.has_value(), "guarantee distinct execution failed");
		auto distinct_rows = annotated_rows(*distinct_result);
		require(
			distinct_rows.size() == 1U &&
				distinct_rows.front().contributor_guarantees.size() == 2U &&
				distinct_rows.front().canonical_form().contains("\"contributor_guarantees\":[{"),
			"distinct lost row-level guarantee attribution");
	}

	void check_summary_guarantee_algebra(const fixture& data)
	{
		const auto publish_claims = [&](const std::vector<claim>& claims)
		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "summary guarantee store unavailable");
			auto writer = store->begin(draft(data.engine));
			require(writer.has_value(), "summary guarantee writer unavailable");
			for (std::size_t index = 0U; index < claims.size(); ++index)
				require(writer->stage(partition(claims[index], 1000U + index)).has_value(),
						"summary guarantee partition rejected");
			require(writer->validate().has_value(), "summary guarantee snapshot invalid");
			auto snapshot = writer->publish();
			require(snapshot.has_value(), "summary guarantee snapshot unpublished");
			return std::move(*snapshot);
		};
		const auto execute =
			[](const snapshot_handle& snapshot, const query::logical_query_ir& logical)
		{
			auto engine = query::reference_engine::bind(snapshot);
			require(engine.has_value(), "summary guarantee engine unavailable");
			auto result = engine->execute(logical);
			require(result.has_value(), "summary guarantee query failed");
			return std::move(*result);
		};

		auto exact = assertion(data.engine,
							   left_row(data.left, "key:a", 1, false),
							   {"release"},
							   "company.query.domain",
							   {"exact", "project", "assumption:a1", {"compiler_verified"}});
		auto weak_other =
			assertion(data.engine,
					  left_row(data.left, "key:weak-other", 2, false),
					  {"release"},
					  "company.query.other-domain",
					  {"unknown", "project", "assumption:weak", {"runtime_observed"}});
		auto domain_snapshot = publish_claims({exact, weak_other});
		auto unrestricted = execute(domain_snapshot, scan_query(data.left));
		require(unrestricted.summary_guarantee().approximation == "unknown",
				"accepted exact/unknown meet vector diverged");
		auto domain_source = query::builder::from(data.left);
		require(domain_source.has_value(), "summary domain source rejected");
		auto domain_builder =
			std::move(*domain_source).interpretation_restrict("company.query.domain");
		require(domain_builder.has_value(), "summary domain restriction rejected");
		auto restricted = execute(domain_snapshot, std::move(*domain_builder).finish());
		require(restricted.summary_guarantee().interpretation_partitions ==
						std::vector<std::string>{"company.query.domain"} &&
					std::ranges::none_of(restricted.summary_guarantee().fragments,
										 [](const query::query_guarantee_fragment& fragment)
										 {
											 return fragment.guarantee.approximation == "unknown";
										 }),
				"excluded interpretation downgraded selected-domain summary");

		auto weak_row = assertion(data.engine,
								  left_row(data.left, "key:weak", 3, false),
								  {"release"},
								  "company.query.domain",
								  {"unknown", "project", "assumption:weak", {"runtime_observed"}});
		auto filtered_snapshot = publish_claims({exact, weak_row});
		const column_ref key{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
		auto predicate = query::equals_present(key, query::literal::typed("query_key_id", "key:a"));
		auto filter_source = query::builder::from(data.left);
		require(predicate && filter_source, "summary filter setup rejected");
		auto filter_builder = std::move(*filter_source).where(*predicate);
		require(filter_builder.has_value(), "summary filter rejected");
		auto filtered = execute(filtered_snapshot, std::move(*filter_builder).finish());
		require(std::ranges::none_of(filtered.summary_guarantee().fragments,
									 [](const query::query_guarantee_fragment& fragment)
									 {
										 return fragment.guarantee.approximation == "unknown";
									 }),
				"filtered-out weak row downgraded surviving fragment summary");
		auto unresolved_store = make_in_memory_snapshot_store(data.engine);
		require(unresolved_store.has_value(), "unresolved scope store unavailable");
		auto unresolved_writer = unresolved_store->begin(draft(data.engine));
		require(unresolved_writer && unresolved_writer->stage(partition(exact, 1100U)) &&
					unresolved_writer->stage(partition(weak_row, 1101U, true)) &&
					unresolved_writer->validate(),
				"unresolved scope snapshot invalid");
		auto unresolved_snapshot = unresolved_writer->publish();
		require(unresolved_snapshot.has_value(), "unresolved scope snapshot unpublished");
		predicate = query::equals_present(key, query::literal::typed("query_key_id", "key:a"));
		filter_source = query::builder::from(data.left);
		require(predicate && filter_source, "unresolved scope filter setup rejected");
		filter_builder = std::move(*filter_source).where(*predicate);
		require(filter_builder.has_value(), "unresolved scope filter rejected");
		auto unresolved_filtered =
			execute(*unresolved_snapshot, std::move(*filter_builder).finish());
		require(!unresolved_filtered.inputs_complete() &&
					std::ranges::none_of(unresolved_filtered.summary_guarantee().fragments,
										 [](const query::query_guarantee_fragment& fragment)
										 {
											 return fragment.unresolved;
										 }),
				"filtered-out unresolved claim contaminated surviving fragment scope");

		auto runtime = assertion(data.engine,
								 left_row(data.left, "key:runtime", 4, false),
								 {"debug"},
								 "company.query.domain",
								 {"exact", "project", "assumption:a2", {"runtime_observed"}});
		auto modalities_snapshot = publish_claims({exact, runtime});
		auto composed = execute(modalities_snapshot, scan_query(data.left));
		const auto& summary = composed.summary_guarantee();
		require(
			summary.assumptions == std::vector<std::string>({"assumption:a1", "assumption:a2"}) &&
				summary.verification_modalities == std::vector<std::string>{"schema_validated"} &&
				summary.fragment_count == summary.fragments.size() &&
				summary.fragment_set_digest.starts_with("semantic-v2:sha256:") &&
				summary.drill_down_ref == "fragments:" + summary.fragment_set_digest,
			"summary assumption union, modality closure, or fragment index diverged");
		const auto canonical = composed.canonical_form();
		require(canonical.contains("\"condition_partition\":") &&
					canonical.contains("\"interpretation_partitions\":") &&
					canonical.contains("\"fragment_set_digest\":") &&
					canonical.contains("\"drill_down_ref\":") &&
					canonical.contains("\"fragments\":[{"),
				"summary required fields did not survive canonical round-trip");

		auto under =
			assertion(data.engine,
					  left_row(data.left, "key:under", 5, false),
					  {"debug"},
					  "company.query.domain",
					  {"under_approximation", "project", "assumption:a1", {"schema_validated"}});
		auto over =
			assertion(data.engine,
					  left_row(data.left, "key:over", 6, false),
					  {"release"},
					  "company.query.domain",
					  {"over_approximation", "project", "assumption:a2", {"schema_validated"}});
		auto component_snapshot = publish_claims({under, over});
		auto component = execute(component_snapshot, scan_query(data.left));
		require(component.summary_guarantee().approximation == "unknown",
				"accepted under/over component meet vector diverged");

		auto nonoverlap_left =
			assertion(data.engine, left_row(data.left, "key:nonoverlap", 7, false), {"debug"});
		auto nonoverlap_right =
			assertion(data.engine, left_row(data.left, "key:nonoverlap", 8, false), {"release"});
		auto nonoverlap_snapshot = publish_claims({nonoverlap_left, nonoverlap_right});
		auto nonoverlap = execute(nonoverlap_snapshot, scan_query(data.left));
		require(nonoverlap.conflicts().empty() &&
					std::ranges::none_of(nonoverlap.summary_guarantee().fragments,
										 [](const query::query_guarantee_fragment& fragment)
										 {
											 return fragment.conflicting;
										 }),
				"non-overlap payload difference blocked an unrelated condition scope");
	}

	void check_closure_applicability(const fixture& data)
	{
		const auto execute =
			[](const snapshot_handle& snapshot, const query::logical_query_ir& logical)
		{
			auto engine = query::reference_engine::bind(snapshot);
			require(engine.has_value(), "closure reference engine bind failed");
			auto result = engine->execute(logical);
			require(result.has_value(), "closure query execution failed");
			return std::move(*result);
		};

		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "unrelated closure store failed");
			const std::vector partitions{partition(data.claims[0], 100U),
										 partition(data.claims[3], 101U)};
			const std::array<std::size_t, 1U> closed{0U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			auto result = execute(snapshot, scan_query(data.right));
			require(result.inputs_complete() && !result.closed() && result.closure_ids().empty(),
					"unrelated relation closure made a query closed-world");
		}

		std::string exact_snapshot_id;
		const auto database =
			std::filesystem::temp_directory_path() / "cxxlens-ng-query-closure.sqlite";
		std::filesystem::remove(database);
		std::filesystem::remove(database.string() + "-wal");
		std::filesystem::remove(database.string() + "-shm");
		{
			auto store = open_sqlite_snapshot_store(database.string(), data.engine);
			require(store.has_value(), "exact SQLite closure store failed");
			const std::vector partitions{partition(data.claims[0], 110U),
										 partition(data.claims[3], 111U)};
			const std::array<std::size_t, 2U> closed{0U, 1U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			exact_snapshot_id = snapshot.id();
		}
		{
			auto reopened = open_sqlite_snapshot_store(database.string(), data.engine);
			require(reopened.has_value(), "exact SQLite closure reopen failed");
			auto snapshot = reopened->open(exact_snapshot_id);
			require(snapshot.has_value(), "exact SQLite closure snapshot missing");
			auto result = execute(*snapshot, scan_query(data.right));
			require(result.inputs_complete() && result.closed() &&
						result.closure_ids().size() == 1U &&
						snapshot->closure_certificates().size() == 2U,
					"exact partition closure was not persisted or applied selectively");
		}
		std::filesystem::remove(database);
		std::filesystem::remove(database.string() + "-wal");
		std::filesystem::remove(database.string() + "-shm");

		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "condition mismatch closure store failed");
			const std::vector partitions{partition(data.claims[4], 120U),
										 partition(data.claims[3], 121U)};
			const std::array<std::size_t, 1U> closed{0U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			const std::array alternatives{std::string{"release"}};
			auto result = execute(snapshot, condition_query(data.right, alternatives));
			require(result.inputs_complete() && !result.closed(),
					"closure for a different condition proved the requested condition");
		}

		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "interpretation mismatch closure store failed");
			const std::vector partitions{partition(data.claims[5], 130U),
										 partition(data.claims[4], 131U)};
			const std::array<std::size_t, 1U> closed{0U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			const std::array alternatives{std::string{"debug"}};
			auto result = execute(
				snapshot, condition_query(data.right, alternatives, "company.query.domain"));
			require(result.inputs_complete() && !result.closed(),
					"closure for a different interpretation proved the requested domain");
		}

		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "join closure store failed");
			const std::vector partitions{partition(data.claims[2], 140U),
										 partition(data.claims[3], 141U)};
			const std::array<std::size_t, 1U> closed{0U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			const auto left_key =
				column_ref{data.left.id, data.left.columns[0].id, data.left.columns[0].type};
			const auto right_key =
				column_ref{data.right.id, data.right.columns[0].id, data.right.columns[0].type};
			auto left = query::builder::from(data.left);
			auto right = query::builder::from(data.right);
			auto predicate = query::equals_present(left_key, right_key);
			require(left && right && predicate, "closure join setup failed");
			auto joined = std::move(*left).inner_join(std::move(*right), std::move(*predicate));
			require(joined.has_value(), "closure join construction failed");
			auto result = execute(snapshot, std::move(*joined).finish());
			require(result.inputs_complete() && !result.closed(),
					"one closed join input proved both required relations");
		}

		{
			auto store = make_in_memory_snapshot_store(data.engine);
			require(store.has_value(), "subset closure store failed");
			const std::vector partitions{partition(data.claims[0], 150U)};
			const std::array<std::size_t, 1U> closed{0U};
			auto snapshot = publish_with_closures(*store, data.engine, partitions, closed);
			const std::array alternatives{std::string{"release"}};
			auto result = execute(snapshot, condition_query(data.left, alternatives));
			require(result.inputs_complete() && result.closed() &&
						result.closure_ids().size() == 1U &&
						result.summary_guarantee().approximation == "exact",
					"exact superset partition closure did not prove an explicit subset query");
		}
	}
} // namespace

int main()
{
	const auto data = make_fixture();
	auto memory_store = make_in_memory_snapshot_store(data.engine);
	require(memory_store.has_value(), "memory query store failed");
	auto memory_snapshot = publish(*memory_store, data, false, false);

	const auto database =
		std::filesystem::temp_directory_path() / "cxxlens-ng-query-runtime.sqlite";
	std::filesystem::remove(database);
	std::filesystem::remove(database.string() + "-wal");
	std::filesystem::remove(database.string() + "-shm");
	std::string sqlite_snapshot_id;
	{
		auto sqlite_store = open_sqlite_snapshot_store(database.string(), data.engine);
		require(sqlite_store.has_value(), "SQLite query store failed");
		auto snapshot = publish(*sqlite_store, data, true, false);
		sqlite_snapshot_id = snapshot.id();
	}
	auto reopened = open_sqlite_snapshot_store(database.string(), data.engine);
	require(reopened.has_value(), "SQLite query store reopen failed");
	auto sqlite_snapshot = reopened->open(sqlite_snapshot_id);
	require(sqlite_snapshot.has_value(), "SQLite query snapshot reopen failed");

	check_ir_validation(data);
	check_set_literal_round_trip();
	check_canonical_json_encoding(data);
	check_snapshot_schema_compatibility();
	check_runtime_matrix(data, memory_snapshot, *sqlite_snapshot);
	check_self_join_occurrences(data, memory_snapshot, *sqlite_snapshot);
	check_partiality(data, memory_snapshot);
	check_canonical_union_execution(data, memory_snapshot, *sqlite_snapshot);
	check_bounded_intermediate_budgets(make_budget_fixture());
	check_side_channel_parity(make_side_channel_fixture());
	check_claim_occurrence_aggregation(data);
	check_summary_guarantee_algebra(data);
	check_closure_applicability(data);

	auto incomplete_store = make_in_memory_snapshot_store(data.engine);
	require(incomplete_store.has_value(), "incomplete query store failed");
	auto incomplete_snapshot = publish(*incomplete_store, data, false, true);
	auto incomplete_engine = query::reference_engine::bind(incomplete_snapshot);
	require(incomplete_engine.has_value(), "incomplete query engine bind failed");
	auto incomplete = incomplete_engine->execute(scan_query(data.left));
	require(incomplete && incomplete->execution() == query::execution_status::complete &&
				!incomplete->inputs_complete() && !incomplete->closed() &&
				!incomplete->unresolved_items().empty() &&
				incomplete->summary_guarantee().approximation != "exact",
			"successful execution was confused with complete/closed input");

	std::filesystem::remove(database);
	std::filesystem::remove(database.string() + "-wal");
	std::filesystem::remove(database.string() + "-shm");
	return 0;
}
