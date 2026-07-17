#include <cstdlib>
#include <string>
#include <vector>

#include <cxxlens/sdk.hpp>

namespace
{
	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::relation_descriptor> relation()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "company.example.query_metric.v1";
		value.name = "company.example.query_metric";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.example.query-metric/1";
		value.owner_namespace = "company.example";
		value.columns = {
			{value.id + ".metric",
			 "metric",
			 {cxxlens::sdk::scalar_kind::typed_id, "query_metric_id", false},
			 true,
			 cxxlens::sdk::column_role::claim_key},
			{value.id + ".value",
			 "value",
			 {cxxlens::sdk::scalar_kind::signed_integer, {}, false},
			 true,
			 cxxlens::sdk::column_role::authoritative_payload},
		};
		value.key_columns = {value.columns.front().id};
		auto digest =
			cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										  value.contract_digest + "\n" + value.canonical_form());
		if (!digest)
			return cxxlens::sdk::unexpected(std::move(digest.error()));
		value.descriptor_digest = std::move(*digest);
		return value;
	}

	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::snapshot_handle>
	publish(cxxlens::sdk::snapshot_store& store,
			const cxxlens::sdk::relation_engine& engine,
			const cxxlens::sdk::claim& claim)
	{
		auto basis = cxxlens::sdk::claim_input_basis_digest(claim.input_basis);
		if (!basis)
			return cxxlens::sdk::unexpected(std::move(basis.error()));
		cxxlens::sdk::partition_draft partition;
		partition.relation_descriptor_id = claim.descriptor;
		partition.scope = "query-example";
		partition.condition = claim.presence;
		partition.interpretation = claim.interpretation;
		partition.producer_semantics = claim.producer.semantic_contract;
		partition.producer_input_basis_digest = std::move(*basis);
		partition.precision_profile = "exact";
		partition.assumption_set_id = "assumptions:none";
		partition.claims = {claim};
		partition.coverage = {{"project", "query-example", "covered", {}}};
		const cxxlens::sdk::snapshot_draft draft{
			{"catalog:query-example",
			 "stable",
			 std::string{engine.generation()},
			 claim.presence.universe,
			 std::string{engine.registry_digest()},
			 "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			 "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
			{1U, 0U, 0U},
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
			std::nullopt};
		auto writer = store.begin(draft);
		if (!writer)
			return cxxlens::sdk::unexpected(std::move(writer.error()));
		if (auto staged = writer->stage(std::move(partition)); !staged)
			return cxxlens::sdk::unexpected(std::move(staged.error()));
		if (auto validated = writer->validate(); !validated)
			return cxxlens::sdk::unexpected(std::move(validated.error()));
		return writer->publish();
	}

	[[nodiscard]] std::vector<std::string>
	result_rows(const cxxlens::sdk::query::query_result& result)
	{
		auto cursor = result.rows();
		std::vector<std::string> output;
		while (true)
		{
			auto next = cursor.next();
			if (!next || !*next)
				break;
			auto row = (*next)->copy();
			if (!row)
				return {};
			output.push_back(row->canonical_form());
		}
		return output;
	}
} // namespace

int main()
{
	auto descriptor_value = relation();
	if (!descriptor_value)
		return EXIT_FAILURE;
	const auto descriptor = std::move(*descriptor_value);
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(descriptor))
		return EXIT_FAILURE;
	auto engine = registry.build("query-example-engine");
	if (!engine)
		return EXIT_FAILURE;
	cxxlens::sdk::row_builder row_builder{descriptor};
	if (!row_builder.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 cxxlens::sdk::detached_cell::typed("query_metric_id", "metric:1")) ||
		!row_builder.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 cxxlens::sdk::detached_cell::signed_integer(42)))
		return EXIT_FAILURE;
	auto row = std::move(row_builder).finish();
	if (!row)
		return EXIT_FAILURE;
	auto claim = cxxlens::sdk::make_assertion(
		*engine,
		{std::move(*row),
		 {"query-example-universe", {"all"}},
		 "company.example.canonical",
		 {"company.example.provider",
		  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
		 {"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
		 "evidence:query-example",
		 {"exact", "project", "assumptions:none", {"schema_validated"}}});
	auto logical = cxxlens::sdk::query::builder::from(descriptor);
	auto memory = cxxlens::sdk::make_in_memory_snapshot_store(*engine);
	auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(":memory:", *engine);
	if (!claim || !logical || !memory || !sqlite)
		return EXIT_FAILURE;
	const auto query = std::move(*logical).finish();
	std::vector<std::string> expected;
	for (auto* store : {&*memory, &*sqlite})
	{
		auto snapshot = publish(*store, *engine, *claim);
		if (!snapshot)
			return EXIT_FAILURE;
		auto runtime = cxxlens::sdk::query::reference_engine::bind(std::move(*snapshot));
		if (!runtime)
			return EXIT_FAILURE;
		auto result = runtime->execute(query);
		if (!result || result->execution() != cxxlens::sdk::query::execution_status::complete ||
			!result->inputs_complete() || result->closed())
			return EXIT_FAILURE;
		auto actual = result_rows(*result);
		if (expected.empty())
			expected = actual;
		else if (actual != expected)
			return EXIT_FAILURE;
	}
	return expected.size() == 1U ? EXIT_SUCCESS : EXIT_FAILURE;
}
