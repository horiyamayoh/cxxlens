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
		value.descriptor_digest =
			cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor.v1", value.canonical_form());
		return value;
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
	return query && query->ir().validate() && memory && sqlite &&
			memory->compatibility().backend == "memory" &&
			sqlite->compatibility().backend == "sqlite"
		? 0
		: 1;
}
