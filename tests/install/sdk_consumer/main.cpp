#include <string>
#include <utility>

#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	[[nodiscard]] cxxlens::sdk::relation_descriptor store_descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "consumer.store_probe.v1";
		value.name = "consumer.store_probe";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "consumer.store-probe/1";
		value.owner_namespace = "consumer";
		value.columns = {{"consumer.store_probe.v1.key",
						  "key",
						  {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
						  true,
						  cxxlens::sdk::column_role::claim_key}};
		value.key_columns = {value.columns.front().id};
		value.descriptor_digest = *cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor.v1",
																 value.canonical_form());
		return value;
	}

	[[nodiscard]] int execute_store(cxxlens::sdk::snapshot_store& store,
									const cxxlens::sdk::relation_engine& engine,
									const cxxlens::sdk::relation_descriptor& descriptor)
	{
		cxxlens::sdk::row_builder row_builder{descriptor};
		if (!row_builder.set(
				{descriptor.id, descriptor.columns.front().id, descriptor.columns.front().type},
				cxxlens::sdk::detached_cell::utf8("installed")))
			return 1;
		auto row = std::move(row_builder).finish();
		if (!row)
			return 2;
		auto claim = cxxlens::sdk::make_assertion(
			engine,
			{std::move(*row),
			 {"consumer-universe", {"all"}},
			 "consumer-domain",
			 {"consumer-provider",
			  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
			 {"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
			 "evidence:installed",
			 {"exact", "installed", "assumptions:none", {"schema_validated"}}});
		if (!claim)
			return 3;
		auto basis = cxxlens::sdk::claim_input_basis_digest(claim->input_basis);
		if (!basis)
			return 4;
		cxxlens::sdk::partition_draft partition;
		partition.relation_descriptor_id = descriptor.id;
		partition.scope = "installed";
		partition.condition = claim->presence;
		partition.interpretation = claim->interpretation;
		partition.producer_semantics = claim->producer.semantic_contract;
		partition.producer_input_basis_digest = std::move(*basis);
		partition.precision_profile = "exact";
		partition.assumption_set_id = "assumptions:none";
		partition.claims = {*claim};
		partition.coverage = {{"project", "installed", "covered", {}}};
		const cxxlens::sdk::snapshot_draft draft{
			{"consumer-catalog",
			 "stable",
			 std::string{engine.generation()},
			 "consumer-universe",
			 std::string{engine.registry_digest()},
			 "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			 "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
			{1U, 0U, 0U},
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
			std::nullopt};
		auto writer = store.begin(draft);
		if (!writer)
			return 5;
		if (!writer->stage(std::move(partition)))
			return 6;
		if (!writer->validate())
			return 7;
		auto snapshot = writer->publish();
		auto query = cxxlens::sdk::query::builder::from(descriptor);
		if (!snapshot || !query)
			return 8;
		auto runtime = cxxlens::sdk::query::reference_engine::bind(std::move(*snapshot));
		if (!runtime)
			return 9;
		auto result = runtime->execute(std::move(*query).finish());
		if (!result || result->execution() != cxxlens::sdk::query::execution_status::complete ||
			!result->inputs_complete())
			return 10;
		auto cursor = result->rows();
		auto first = cursor.next();
		if (!first || !*first || !(*first)->copy())
			return 11;
		auto end = cursor.next();
		return end && !*end ? 0 : 12;
	}
} // namespace

int main()
{
	auto query = cxxlens::sdk::query::from<cxxlens::cc::relations::call_site>();
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(store_descriptor()))
		return 2;
	auto engine = registry.build("installed-consumer");
	if (!engine)
		return 3;
	auto memory = cxxlens::sdk::make_in_memory_snapshot_store(*engine);
	auto sqlite = cxxlens::sdk::open_sqlite_snapshot_store(":memory:", *engine);
	if (!query || !query->ir().validate())
		return 4;
	if (!memory || memory->compatibility().backend != "memory")
		return 5;
	if (!sqlite || sqlite->compatibility().backend != "sqlite")
		return 6;
	if (const auto code = execute_store(*memory, *engine, store_descriptor()); code != 0)
		return 10 + code;
	if (const auto code = execute_store(*sqlite, *engine, store_descriptor()); code != 0)
		return 30 + code;
	return 0;
}
