#include <array>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/materialization_v4_claim_binding.hpp"
#include "llvm/clang22/materialization_v4_incremental_ingress.hpp"
#include "llvm/clang22/materialization_v4_store_source.hpp"
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
		value.id = "company.test.completeitem.v1";
		value.name = "company.test.completeitem";
		value.version = {1U, 0U, 0U};
		value.semantic_major = 1U;
		value.semantics = "company.test.completeitem/1";
		value.owner_namespace = "company.test";
		value.columns = {
			{"company.test.completeitem.v1.key",
			 "key",
			 {sdk::scalar_kind::typed_id, "company_item_id", false},
			 true,
			 sdk::column_role::claim_key},
			{"company.test.completeitem.v1.value",
			 "value",
			 {sdk::scalar_kind::utf8_string, {}, false},
			 true,
			 sdk::column_role::authoritative_payload},
		};
		value.key_columns = {"company.test.completeitem.v1.key"};
		value.merge = sdk::merge_mode::set;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		auto added = registry.add(descriptor());
		require(added.has_value(), "positive relation descriptor rejected");
		auto built = registry.build("v4-store-positive-test");
		require(built.has_value(), "positive relation engine rejected");
		return std::move(*built);
	}

	[[nodiscard]] sdk::detached_row row(std::string key, std::string payload)
	{
		const auto relation = descriptor();
		sdk::row_builder builder{relation};
		require(builder
					.set({relation.id, relation.columns[0U].id, relation.columns[0U].type},
						 sdk::detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"positive claim key rejected");
		require(builder
					.set({relation.id, relation.columns[1U].id, relation.columns[1U].type},
						 sdk::detached_cell::utf8(std::move(payload)))
					.has_value(),
				"positive claim payload rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "positive claim row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] source_closure_manifest manifest()
	{
		source_closure_manifest value;
		value.members = {{"", "project://src/main.cpp", "main", "utf8", 7U, digest('c'), true}};
		value.blobs = {{digest('c'), 7U}};
		auto file = source_closure_file_id(value.members.front().logical_path);
		require(file.has_value(), "positive source file ID rejected");
		value.members.front().file_id = *file;
		auto closure = derive_source_closure_digest(value);
		require(closure.has_value(), "positive closure digest rejected");
		value.closure_digest = *closure;
		value.closure_id = "source-closure:" + *closure;
		auto manifest_digest = derive_source_closure_manifest_digest(value);
		require(manifest_digest.has_value(), "positive manifest digest rejected");
		value.manifest_digest = *manifest_digest;
		return value;
	}

	[[nodiscard]] provider_task_v4_base_task base(const source_closure_manifest& value)
	{
		provider_task_v4_base_task output;
		output.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'd');
		output.provider_execution_id = "provider-execution:v4-positive";
		output.canonical_base_task_digest = digest('e');
		output.task_input_digest = digest('f');
		output.normalized_invocation_digest = semantic('1');
		output.toolchain_digest = semantic('2');
		output.environment_digest = digest('3');
		output.working_directory = "project://src";
		output.source = {"source-snapshot:v4-positive",
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
		require(task_digest.has_value(), "positive task digest rejected");
		output.task_v4_digest = *task_digest;
		output.task_id = "task:" + *task_digest;
		return output;
	}

	[[nodiscard]] sdk::claim claim(const sdk::relation_engine& value)
	{
		sdk::observation observed{
			row("item:positive", "payload"),
			{"universe-complete", {"all"}},
			"company.test.complete-interpretation-1",
			{"provider.complete", digest('a')},
			{digest('b')},
			"evidence:v4-store-positive",
			{"under_approximation", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto made = sdk::make_assertion(value, std::move(observed));
		require(made.has_value(), "positive assertion rejected");
		return std::move(*made);
	}

	[[nodiscard]] materialization_v4_claim_translation
	translation(const sdk::relation_engine& value)
	{
		auto closure = manifest();
		auto base_value = base(closure);
		auto task_value = task(closure, base_value);
		materialization_v4_claim_binding authority;
		authority.materialization_request_id = "materialization-request:v4-positive";
		authority.task_index = 0U;
		authority.base_task = std::move(base_value);
		authority.task = std::move(task_value);
		authority.manifest = std::move(closure);
		authority.provider_id = "provider.complete";
		authority.provider_semantic_contract_digest = digest('a');
		authority.materializer_id = "cxxlens.clang22.materializer";
		authority.materializer_semantic_contract_digest = semantic('5');
		authority.direct_basis_digest = digest('6');
		authority.canonical_adoption_transform_digest = digest('7');
		authority.base_ingestion_transform_digest = digest('8');
		authority.guarantee = {
			"under_approximation", "partition", "assumptions:none", {"schema_validated"}};
		authority.assumption_set_id = authority.guarantee.assumptions;
		authority.relation_descriptor_id = descriptor().id;
		authority.scope = "compile-unit:v4-positive";
		authority.interpretation = "company.test.complete-interpretation-1";
		authority.precision_profile = "under_approximation";

		auto claim_value = claim(value);
		auto basis = sdk::claim_input_basis_digest(claim_value.input_basis);
		require(basis.has_value(), "positive claim basis rejected");
		authority.direct_basis_digest = *basis;
		sdk::claim_batch batch;
		require(batch.add(claim_value).has_value(), "positive claim was not staged");
		auto committed = std::move(batch).commit(value);
		require(committed.has_value() && committed->unresolved.empty() &&
					committed->conflicts.empty() && committed->differential_disagreements.empty(),
				"positive claim batch was not complete");

		sdk::partition_draft partition;
		partition.relation_descriptor_id = authority.relation_descriptor_id;
		partition.scope = authority.scope;
		partition.condition = {"universe-complete", {"all"}};
		partition.interpretation = authority.interpretation;
		partition.producer_semantics = authority.provider_semantic_contract_digest;
		partition.producer_input_basis_digest = *basis;
		partition.precision_profile = authority.precision_profile;
		partition.assumption_set_id = authority.assumption_set_id;
		partition.claims = committed->claims;
		partition.coverage = {{"compile-unit", authority.scope, "covered", ""}};
		partition.unresolved = committed->unresolved;
		return {std::move(authority), std::move(*committed), std::move(partition)};
	}

	[[nodiscard]] sdk::snapshot_series_selector selector(const sdk::relation_engine& value)
	{
		return {"catalog-v4-store-positive",
				"stable",
				std::string{value.generation()},
				"universe-complete",
				std::string{value.registry_digest()},
				digest('b'),
				digest('c')};
	}

	void positive_v4_store_publication()
	{
		auto value = engine();
		auto sealed = seal_materialization_v4_claim_translation(value, translation(value));
		require(sealed.has_value(), "positive v4 translation was not sealed");
		const std::array<const materialization_v4_claim_sealed*, 1U> sealed_tasks{&*sealed};
		auto receipt = make_materialization_v4_incremental_receipt(value, sealed_tasks);
		require(receipt && receipt->complete, "positive v4 receipt was not complete");
		const auto selector_value = selector(value);
		materialization_v4_provider_output_authority authority;
		authority.materialization_request_id = receipt->materialization_request_id;
		authority.publication = {"semantic-v2:sha256:" + std::string(64U, '9'),
								 "semantic-v2:sha256:" + std::string(64U, 'a'),
								 "publication-target:v4-positive"};
		authority.snapshot = {
			selector_value,
			{1U, 0U, 0U},
			digest('9'),
			std::nullopt,
		};
		sdk::closure_candidate closure;
		closure.relation_descriptor_id = sealed->translation.partition.relation_descriptor_id;
		closure.subject_partition_id = sealed->partition_manifest.partition_id;
		closure.partition_content_digest = sealed->partition_manifest.content_digest;
		closure.coverage_digest = sealed->partition_manifest.coverage_digest;
		closure.key_domain_digest = digest('d');
		closure.condition = sealed->partition_binding.condition;
		closure.interpretation = sealed->partition_binding.interpretation;
		closure.assumption_set_id = sealed->partition_binding.assumption_set_id;
		closure.closure_kind = "relation-key-enumeration";
		closure.producer_semantics = sealed->partition_binding.producer_semantics;
		closure.evidence_digest = digest('e');
		authority.closures = {std::move(closure)};

		auto source =
			make_materialization_v4_store_source(value, *receipt, sealed_tasks, authority);
		require(source.has_value(), "positive v4 Store source was rejected");
		auto store = sdk::make_in_memory_snapshot_store(value);
		require(store.has_value(), "positive v4 Store backend was not created");
		auto published = publish_materialization_v4_store_source(value, *store, std::move(*source));
		require(published.has_value(), "positive v4 Store source did not publish");
		require(published->snapshot.publication().state == sdk::publication_state::committed &&
					!published->snapshot.publication().corrupt &&
					!published->snapshot.manifest().partitions.empty() &&
					published->snapshot.manifest().partitions.front().complete &&
					published->snapshot.manifest().closure_ids.size() == 1U &&
					published->authority.publication == authority.publication &&
					published->receipt == *receipt && published->output_receipt_digest.empty() &&
					published->output_batch_count == 0U && published->publication_verified,
				"positive v4 publication lost authority or was not committed");

		sealed->translation.partition.coverage.front().state = "unknown";
		sealed->translation.partition.coverage.front().reason = "tampered";
		auto rejected =
			make_materialization_v4_store_source(value, *receipt, sealed_tasks, authority);
		require(!rejected, "tampered v4 output crossed the Store source boundary");
	}
} // namespace

int main()
{
	positive_v4_store_publication();
	return 0;
}
