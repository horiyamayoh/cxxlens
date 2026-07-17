#include <array>
#include <string>
#include <utility>

#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/company_lock_acquire.hpp>
#include <cxxlens/sdk.hpp>

namespace
{
	[[nodiscard]] cxxlens::sdk::result<cxxlens::sdk::relation_descriptor> source_span_descriptor()
	{
		cxxlens::sdk::relation_descriptor descriptor;
		descriptor.id = "source.span.v1";
		descriptor.name = "source.span";
		descriptor.version = {1U, 0U, 0U};
		descriptor.semantic_major = 1U;
		descriptor.semantics = "source.span/1";
		descriptor.owner_namespace = "cxxlens.standard.source";
		descriptor.columns = {{"source.span.v1.span",
							   "span",
							   {cxxlens::sdk::scalar_kind::typed_id, "source_span_id", false},
							   true,
							   cxxlens::sdk::column_role::claim_key}};
		descriptor.key_columns = {"source.span.v1.span"};
		descriptor.merge = cxxlens::sdk::merge_mode::set;
		auto digest = cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
													descriptor.contract_digest + "\n" +
														descriptor.canonical_form());
		if (!digest)
			return cxxlens::sdk::unexpected(std::move(digest.error()));
		descriptor.descriptor_digest = std::move(*digest);
		return descriptor;
	}

	[[nodiscard]] cxxlens::sdk::observation observed(cxxlens::sdk::detached_row row)
	{
		return {std::move(row),
				{"build.universe", {"all"}},
				"company.example.clang22",
				{"company.example.fake-provider", "company.example.lock-observation.v1"},
				{"sha256:0000000000000000000000000000000000000000000000000000000000000000"},
				"evidence:fake-provider-run",
				{"exact", "compile-unit", "assumptions:none", {"schema_validated"}}};
	}

	[[nodiscard]] cxxlens::sdk::detached_cell symbol(cxxlens::sdk::value_type type,
													 std::string value)
	{
		return {std::move(type),
				cxxlens::sdk::cell_state::present,
				cxxlens::sdk::scalar_value{std::move(value)},
				std::nullopt};
	}

	[[nodiscard]] cxxlens::sdk::detached_cell optional_entity(std::string value)
	{
		return {{cxxlens::sdk::scalar_kind::typed_id, "cc_entity_id", true},
				cxxlens::sdk::cell_state::present,
				cxxlens::sdk::scalar_value{std::move(value)},
				std::nullopt};
	}
} // namespace

int main()
{
	auto source_descriptor_value = source_span_descriptor();
	if (!source_descriptor_value)
		return 1;
	const auto source_descriptor = std::move(*source_descriptor_value);
	cxxlens::sdk::relation_registry registry;
	if (!registry.add(source_descriptor) ||
		!registry.add(cxxlens::cc::relations::entity::descriptor()) ||
		!registry.add(cxxlens::company::relations::lock_acquire::descriptor()))
		return 1;
	auto engine = registry.build("example-generation");
	if (!engine)
		return 2;

	cxxlens::sdk::row_builder source_builder{source_descriptor};
	if (!source_builder.set({source_descriptor.id,
							 "source.span.v1.span",
							 {cxxlens::sdk::scalar_kind::typed_id, "source_span_id", false}},
							cxxlens::sdk::detached_cell::typed("source_span_id", "source:42")))
		return 3;
	auto source_row = std::move(source_builder).finish();
	if (!source_row)
		return 4;
	auto source_claim = cxxlens::sdk::make_assertion(*engine, observed(std::move(*source_row)));
	if (!source_claim)
		return 5;

	using relation = cxxlens::company::relations::lock_acquire;
	relation::builder lock_builder;
	if (!lock_builder.set<relation::acquire>(
			cxxlens::sdk::detached_cell::typed("company_lock_acquire_id", "acquire:1")) ||
		!lock_builder.set<relation::lock>(
			cxxlens::sdk::detached_cell::typed("company_lock_id", "lock:cache")) ||
		!lock_builder.set<relation::function>(optional_entity("entity:not-materialized")) ||
		!lock_builder.set<relation::source>(
			cxxlens::sdk::detached_cell::typed("source_span_id", "source:42")) ||
		!lock_builder.set<relation::mode>(symbol(
			{cxxlens::sdk::scalar_kind::open_symbol, "company.lock-mode/1", false}, "exclusive")) ||
		!lock_builder.set<relation::ordinal>(cxxlens::sdk::detached_cell::unsigned_integer(0U)))
		return 6;
	auto lock_row = std::move(lock_builder).finish();
	if (!lock_row)
		return 7;
	cxxlens::sdk::claim_batch batch;
	if (!batch.add_observation(*engine, observed(std::move(*lock_row))))
		return 9;
	const std::array existing{*source_claim};
	auto committed = std::move(batch).commit(*engine, existing);
	return committed && committed->claims.size() == 1U && committed->unresolved.size() == 1U ? 0
																							 : 10;
}
