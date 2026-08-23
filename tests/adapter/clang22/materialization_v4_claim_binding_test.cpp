#include "llvm/clang22/materialization_v4_claim_binding.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

#include "llvm/clang22/source_closure.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::string digest(const char digit, const std::string_view prefix = "sha256:")
	{
		return std::string{prefix} + std::string(64U, digit);
	}

	[[nodiscard]] std::string semantic(const char digit)
	{
		return digest(digit, "semantic-v2:sha256:");
	}

	[[nodiscard]] sdk::relation_descriptor descriptor()
	{
		sdk::relation_descriptor value;
		value.id = "company.test.item.v1";
		value.name = "company.test.item";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.item/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.item.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_item_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.item.v1.value",
			 "value",
			 {sdk::scalar_kind::interpretation_domain_id, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.item.v1.key"};
		value.conflict_columns = {"company.test.item.v1.value"};
		value.references = {{
			{"company.test.item.v1.key"},
			"company.test.target",
			{"company.test.target.v1.key"},
			sdk::reference_strength::soft_semantic,
			false,
		}};
		value.merge = sdk::merge_mode::functional_assertion;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		auto added = registry.add(descriptor());
		if (!added)
		{
			std::cerr << added.error().code << ':' << added.error().field << ':'
					  << added.error().detail << '\n';
		}
		require(added.has_value(), "v4 relation descriptor rejected");
		auto built = registry.build("v4-claim-binding-test");
		require(built.has_value(), "v4 relation engine rejected");
		return std::move(*built);
	}

	[[nodiscard]] sdk::detached_row row(std::string key, std::string payload)
	{
		const auto relation = descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0].id, relation.columns[0].type},
						 sdk::detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"v4 claim key rejected");
		require(builder
					.set({relation.id, relation.columns[1].id, relation.columns[1].type},
						 {relation.columns[1].type,
						  sdk::cell_state::present,
						  sdk::scalar_value{std::move(payload)},
						  std::nullopt})
					.has_value(),
				"v4 claim payload rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "v4 claim row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] sdk::claim claim(const sdk::relation_engine& value,
								   std::string payload,
								   const std::string_view provenance = "evidence:task-v4")
	{
		sdk::observation observed{
			row("item:shared", std::move(payload)),
			{"universe-1", {"all"}},
			"company.test.interpretation-1",
			{"provider.test", digest('a')},
			{digest('b')},
			std::string{provenance},
			{"under_approximation", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto made = sdk::make_assertion(value, std::move(observed));
		require(made.has_value(), "v4 assertion rejected");
		return std::move(*made);
	}

	[[nodiscard]] source_closure_manifest manifest()
	{
		source_closure_manifest value;
		value.members = {{"", "project://src/main.cpp", "main", "utf8", 7U, digest('c'), true}};
		value.blobs = {{digest('c'), 7U}};
		auto file = source_closure_file_id(value.members.front().logical_path);
		require(file.has_value(), "v4 source file ID rejected");
		value.members.front().file_id = *file;
		auto closure = derive_source_closure_digest(value);
		require(closure.has_value(), "v4 closure digest rejected");
		value.closure_digest = *closure;
		value.closure_id = "source-closure:" + *closure;
		auto manifest_digest = derive_source_closure_manifest_digest(value);
		require(manifest_digest.has_value(), "v4 manifest digest rejected");
		value.manifest_digest = *manifest_digest;
		return value;
	}

	[[nodiscard]] provider_task_v4_base_task base(const source_closure_manifest& value)
	{
		provider_task_v4_base_task output;
		output.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'd');
		output.provider_execution_id = "provider-execution:v4-one";
		output.canonical_base_task_digest = digest('e');
		output.task_input_digest = digest('f');
		output.normalized_invocation_digest = semantic('1');
		output.toolchain_digest = semantic('2');
		output.environment_digest = digest('3');
		output.working_directory = "project://src";
		output.source = {"source-snapshot:v4-one",
						 value.members.front().file_id,
						 value.members.front().logical_path,
						 value.members.front().content_digest,
						 value.members.front().size_bytes,
						 value.members.front().encoding,
						 digest('4', "line-index:sha256:"),
						 true};
		return output;
	}

	[[nodiscard]] provider_task_v4 task(const source_closure_manifest& value,
										const provider_task_v4_base_task& base_value)
	{
		provider_task_v4 output;
		output.base_task_index = 0U;
		output.base_provider_task_id = base_value.provider_task_id;
		output.base_task_digest = base_value.canonical_base_task_digest;
		output.open_task = {base_value.task_input_digest,
							base_value.normalized_invocation_digest,
							base_value.toolchain_digest,
							base_value.environment_digest};
		output.source_closure = {value.closure_id,
								 value.closure_digest,
								 value.manifest_digest,
								 static_cast<std::uint64_t>(value.members.size()),
								 static_cast<std::uint64_t>(value.blobs.size()),
								 value.blobs.front().size_bytes};
		output.main_logical_path = base_value.source.logical_path;
		output.logical_working_directory = base_value.working_directory;
		auto task_digest = derive_provider_task_v4_digest(output);
		require(task_digest.has_value(), "v4 task digest rejected");
		output.task_v4_digest = *task_digest;
		output.task_id = "task:" + *task_digest;
		return output;
	}

	[[nodiscard]] materialization_v4_claim_binding binding()
	{
		auto closure = manifest();
		auto base_value = base(closure);
		auto task_value = task(closure, base_value);
		materialization_v4_claim_binding output;
		output.materialization_request_id = "materialization-request:v4-one";
		output.task_index = 0U;
		output.base_task = std::move(base_value);
		output.task = std::move(task_value);
		output.manifest = std::move(closure);
		output.provider_id = "provider.test";
		output.provider_semantic_contract_digest = digest('a');
		output.materializer_id = "cxxlens.clang22.materializer";
		output.materializer_semantic_contract_digest = semantic('5');
		output.direct_basis_digest = digest('6');
		output.canonical_adoption_transform_digest = digest('7');
		output.base_ingestion_transform_digest = digest('8');
		output.guarantee = {
			"under_approximation", "partition", "assumptions:none", {"schema_validated"}};
		output.assumption_set_id = output.guarantee.assumptions;
		output.relation_descriptor_id = descriptor().id;
		output.scope = "compile-unit:v4-one";
		output.interpretation = "company.test.interpretation-1";
		output.precision_profile = "under_approximation";
		return output;
	}

	[[nodiscard]] materialization_v4_claim_translation
	translation(const sdk::relation_engine& value)
	{
		auto authority = binding();
		auto first = claim(value, "one");
		auto second = claim(value, "two", "evidence:task-v4-alternate");
		const auto basis = sdk::claim_input_basis_digest(first.input_basis);
		require(basis.has_value(), "v4 direct basis digest rejected");
		authority.direct_basis_digest = *basis;

		sdk::claim_batch batch;
		require(batch.add(first).has_value(), "v4 first claim was not staged");
		require(batch.add(second).has_value(), "v4 second claim was not staged");
		auto committed = std::move(batch).commit(value);
		require(committed.has_value(), "v4 claim batch was not committed");
		require(committed->conflicts.size() == 1U, "v4 conflict was not retained");
		require(!committed->unresolved.empty(), "v4 unresolved references were not retained");

		sdk::partition_draft partition;
		partition.relation_descriptor_id = authority.relation_descriptor_id;
		partition.scope = authority.scope;
		partition.condition = {"universe-1", {"all"}};
		partition.interpretation = authority.interpretation;
		partition.producer_semantics = authority.provider_semantic_contract_digest;
		partition.producer_input_basis_digest = *basis;
		partition.precision_profile = authority.precision_profile;
		partition.assumption_set_id = authority.assumption_set_id;
		partition.claims = committed->claims;
		partition.coverage = {{"compile-unit", authority.scope, "unknown", "provider-partial"}};
		partition.unresolved = committed->unresolved;
		return {std::move(authority), std::move(*committed), std::move(partition)};
	}
} // namespace

int main()
{
	auto value = engine();
	auto input = translation(value);
	auto sealed = seal_materialization_v4_claim_translation(value, std::move(input));
	require(sealed.has_value(), "valid v4 claim translation was rejected");
	require(sealed->receipt.conflict_count == 1U, "v4 receipt lost conflict count");
	require(sealed->receipt.unresolved_count != 0U, "v4 receipt lost unresolved count");
	require(!sealed->receipt.complete, "partial v4 coverage was marked complete");
	require(validate_materialization_v4_claim_receipt(value, *sealed).has_value(),
			"valid v4 claim receipt did not replay");

	sealed->translation.partition.coverage.front().reason = "tampered";
	require(!validate_materialization_v4_claim_receipt(value, *sealed),
			"tampered v4 coverage was accepted");

	return 0;
}
