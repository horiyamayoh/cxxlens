#include <cstdlib>
#include <string>

#include <cxxlens/sdk.hpp>

namespace
{
	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::relation_descriptor> relation()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "company.example.metric.v1";
		value.name = "company.example.metric";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.example.metric/1";
		value.owner_namespace = "company.example";
		value.columns = {
			{"company.example.metric.v1.metric",
			 "metric",
			 {cxxlens::sdk::scalar_kind::typed_id, "company_metric_id", false},
			 true,
			 cxxlens::sdk::column_role::claim_key},
			{"company.example.metric.v1.value",
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
	auto engine = registry.build("example-engine-1");
	if (!engine)
		return EXIT_FAILURE;

	cxxlens::sdk::row_builder builder{descriptor};
	if (!builder.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
					 cxxlens::sdk::detached_cell::typed("company_metric_id", "metric:1")) ||
		!builder.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
					 cxxlens::sdk::detached_cell::signed_integer(42)))
		return EXIT_FAILURE;
	auto row = std::move(builder).finish();
	if (!row)
		return EXIT_FAILURE;

	const std::string producer =
		"sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
	auto assertion = cxxlens::sdk::make_assertion(
		*engine,
		{std::move(*row),
		 {"example-universe", {"all"}},
		 "company.example.canonical-1",
		 {"company.example.provider", producer},
		 {"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
		 "evidence:example",
		 {"exact", "project", "assumptions:none", {"schema_validated"}}});
	if (!assertion)
		return EXIT_FAILURE;

	cxxlens::sdk::partition_draft partition;
	partition.relation_descriptor_id = descriptor.id;
	partition.scope = "compile-unit:1";
	partition.condition = assertion->presence;
	partition.interpretation = assertion->interpretation;
	partition.producer_semantics = producer;
	auto input_basis = cxxlens::sdk::claim_input_basis_digest(assertion->input_basis);
	if (!input_basis)
		return EXIT_FAILURE;
	partition.producer_input_basis_digest = std::move(*input_basis);
	partition.precision_profile = "exact";
	partition.assumption_set_id = "assumptions-empty";
	partition.claims = {*assertion};
	partition.coverage = {{"compile-unit", "compile-unit:1", "covered", {}}};
	auto partition_manifest = cxxlens::sdk::make_partition_manifest(*engine, partition);
	if (!partition_manifest)
		return EXIT_FAILURE;
	const cxxlens::sdk::closure_candidate closure{
		descriptor.id,
		partition_manifest->partition_id,
		partition_manifest->content_digest,
		partition_manifest->coverage_digest,
		"sha256:1111111111111111111111111111111111111111111111111111111111111111",
		partition.condition,
		partition.interpretation,
		partition.assumption_set_id,
		"relation-key-enumeration",
		partition.producer_semantics,
		"sha256:2222222222222222222222222222222222222222222222222222222222222222"};

	const cxxlens::sdk::snapshot_series_selector selector{
		"catalog:example",
		"stable",
		std::string{engine->generation()},
		"example-universe",
		std::string{engine->registry_digest()},
		"sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
		"sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"};
	const cxxlens::sdk::snapshot_draft draft{
		selector,
		{1U, 0U, 0U},
		"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
		std::nullopt};
	auto memory = cxxlens::sdk::make_in_memory_snapshot_store(*engine);
	auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(":memory:", *engine);
	if (!memory || !sqlite)
		return EXIT_FAILURE;

	std::string expected;
	for (auto* store : {&*memory, &*sqlite})
	{
		auto writer = store->begin(draft);
		if (!writer || !writer->stage(partition) || !writer->add_closure(closure) ||
			!writer->validate())
			return EXIT_FAILURE;
		auto published = writer->publish();
		if (!published)
			return EXIT_FAILURE;
		if (expected.empty())
			expected = published->id();
		if (published->id() != expected)
			return EXIT_FAILURE;
		if (published->partition_bindings().size() != 1U ||
			published->closure_certificates().size() != 1U)
			return EXIT_FAILURE;
	}
	return EXIT_SUCCESS;
}
