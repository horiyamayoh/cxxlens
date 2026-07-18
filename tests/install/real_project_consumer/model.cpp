#include "model.hpp"

#include <string>
#include <utility>

namespace real_project
{
	cxxlens::sdk::relation_descriptor descriptor()
	{
		cxxlens::sdk::relation_descriptor value;
		value.id = "consumer.real_project.v1";
		value.name = "consumer.real_project";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "consumer.real-project/1";
		value.owner_namespace = "consumer";
		value.columns = {{"consumer.real_project.v1.key",
						  "key",
						  {cxxlens::sdk::scalar_kind::utf8_string, {}, false},
						  true,
						  cxxlens::sdk::column_role::claim_key}};
		value.key_columns = {value.columns.front().id};
		value.descriptor_digest =
			*cxxlens::sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
										   value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	int qualify(cxxlens::sdk::snapshot_store& store, const cxxlens::sdk::relation_engine& engine)
	{
		auto relation = descriptor();
		cxxlens::sdk::row_builder builder{relation};
		if (!builder.set({relation.id, relation.columns.front().id, relation.columns.front().type},
						 cxxlens::sdk::detached_cell::utf8("real-project")))
			return 1;
		auto row = std::move(builder).finish();
		if (!row)
			return 2;
		auto claim = cxxlens::sdk::make_assertion(
			engine,
			{std::move(*row),
			 {"real-project-universe", {"all"}},
			 "real-project-domain",
			 {"real-project-provider",
			  "sha256:aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"},
			 {"sha256:bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"},
			 "evidence:real-project",
			 {"exact", "real-project", "assumptions:none", {"schema_validated"}}});
		if (!claim)
			return 3;
		auto basis = cxxlens::sdk::claim_input_basis_digest(claim->input_basis);
		if (!basis)
			return 4;
		cxxlens::sdk::partition_draft partition;
		partition.relation_descriptor_id = relation.id;
		partition.scope = "real-project";
		partition.condition = claim->presence;
		partition.interpretation = claim->interpretation;
		partition.producer_semantics = claim->producer.semantic_contract;
		partition.producer_input_basis_digest = std::move(*basis);
		partition.precision_profile = "exact";
		partition.assumption_set_id = "assumptions:none";
		partition.claims = {*claim};
		partition.coverage = {{"project", "real-project", "covered", {}}};
		const cxxlens::sdk::snapshot_draft draft{
			{"real-project-catalog",
			 "stable",
			 std::string{engine.generation()},
			 "real-project-universe",
			 std::string{engine.registry_digest()},
			 "sha256:cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc",
			 "sha256:dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd"},
			{1U, 0U, 0U},
			"sha256:eeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeeee",
			std::nullopt};
		auto writer = store.begin(draft);
		if (!writer || !writer->stage(std::move(partition)) || !writer->validate())
			return 5;
		auto snapshot = writer->publish();
		auto query = cxxlens::sdk::query::builder::from(relation);
		if (!snapshot || !query)
			return 6;
		auto runtime = cxxlens::sdk::query::reference_engine::bind(std::move(*snapshot));
		if (!runtime)
			return 7;
		auto result = runtime->execute(std::move(*query).finish());
		if (!result || result->execution() != cxxlens::sdk::query::execution_status::complete ||
			!result->inputs_complete())
			return 8;
		auto rows = result->rows();
		auto first = rows.next();
		if (!first || !*first || !(*first)->copy())
			return 9;
		auto end = rows.next();
		return end && !*end ? 0 : 9;
	}
} // namespace real_project
