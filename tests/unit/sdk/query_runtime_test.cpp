#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

#include <cxxlens/sdk.hpp>

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
		value.descriptor_digest =
			semantic_digest("cxxlens.relation-descriptor.v1", value.canonical_form());
		return value;
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
		value.descriptor_digest =
			semantic_digest("cxxlens.relation-descriptor.v1", value.canonical_form());
		return value;
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

	constexpr std::string_view producer_semantics{
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"};
	constexpr std::string_view direct_basis{
		"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"};

	[[nodiscard]] claim assertion(const relation_engine& engine,
								  detached_row row,
								  std::vector<std::string> fragments,
								  std::string interpretation = "company.query.domain")
	{
		auto value =
			make_assertion(engine,
						   {std::move(row),
							{"company.query.universe", std::move(fragments)},
							std::move(interpretation),
							{"company.query.provider", std::string{producer_semantics}},
							{std::string{direct_basis}},
							"evidence:query-runtime",
							{"exact", "project", "assumptions:none", {"schema_validated"}}});
		require(value.has_value(), "query fixture assertion failed");
		return std::move(*value);
	}

	struct fixture
	{
		relation_descriptor left;
		relation_descriptor right;
		relation_engine engine;
		std::vector<claim> claims;
	};

	[[nodiscard]] fixture make_fixture()
	{
		auto left = left_relation();
		auto right = right_relation();
		relation_registry registry;
		require(registry.add(left).has_value(), "left relation rejected");
		require(registry.add(right).has_value(), "right relation rejected");
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

	[[nodiscard]] query::logical_query_ir scan_query(const relation_descriptor& descriptor)
	{
		auto builder = query::builder::from(descriptor);
		require(builder.has_value(), "scan builder failed");
		return std::move(*builder).finish();
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

	void check_runtime_matrix(const fixture& data,
							  const snapshot_handle& memory_snapshot,
							  const snapshot_handle& sqlite_snapshot)
	{
		auto memory_engine = query::reference_engine::bind(memory_snapshot);
		auto sqlite_engine = query::reference_engine::bind(sqlite_snapshot);
		require(memory_engine && sqlite_engine, "reference engine bind failed");

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
							sqlite->summary_guarantee().approximation,
					"memory/SQLite query side channels diverged");
			require(memory->logical_ir_digest() == logical.digest(),
					"query result lost logical IR digest");
			require(memory->explain_physical().text.contains("backend=memory") &&
						sqlite->explain_physical().text.contains("backend=sqlite"),
					"physical explanation did not expose backend-only information");
			require(!memory->explain_logical().text.contains("backend="),
					"physical backend leaked into logical explanation");
		}

		auto join_memory = memory_engine->execute(queries[4]);
		require(join_memory && rows(*join_memory).size() == 3U,
				"condition/interpretation-aware inner join diverged");
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
	check_runtime_matrix(data, memory_snapshot, *sqlite_snapshot);
	check_partiality(data, memory_snapshot);

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
