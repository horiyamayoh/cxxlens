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

#include "llvm/clang22/materialization_store.hpp"
#include "llvm/clang22/materialization_v4_claim_binding.hpp"
#include "llvm/clang22/materialization_v4_incremental_ingress.hpp"
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

	class sealed_partition_source final : public materialization_store_partition_replay_source
	{
	  public:
		sealed_partition_source(const sdk::relation_engine& engine,
								const materialization_v4_store_ingress& ingress,
								std::vector<const materialization_v4_claim_sealed*> tasks)
			: engine_{engine}, ingress_{&ingress}, tasks_{std::move(tasks)}
		{
		}

		sdk::result<void> replay(const materialization_store_partition_consumer& consumer) override
		{
			if (!consumer)
				return sdk::unexpected({"materialization.v4-store-source", "consumer", "missing"});
			const std::span<const materialization_v4_claim_sealed* const> sealed{tasks_};
			if (auto valid = validate_materialization_v4_incremental_receipt(
					engine_, ingress_->receipt, sealed);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			for (const auto* task : tasks_)
			{
				if (task == nullptr)
					return sdk::unexpected({"materialization.v4-store-source", "task", "null"});
				auto partition = task->translation.partition;
				if (auto consumed = consumer(std::move(partition)); !consumed)
					return consumed;
			}
			++replay_count;
			return {};
		}

		std::size_t replay_count{};

	  private:
		const sdk::relation_engine& engine_;
		const materialization_v4_store_ingress* ingress_{};
		std::vector<const materialization_v4_claim_sealed*> tasks_;
	};

	void positive_v4_store_publication()
	{
		auto value = engine();
		auto sealed = seal_materialization_v4_claim_translation(value, translation(value));
		require(sealed.has_value(), "positive v4 translation was not sealed");
		const std::array<const materialization_v4_claim_sealed*, 1U> sealed_tasks{&*sealed};
		auto receipt = make_materialization_v4_incremental_receipt(value, sealed_tasks);
		require(receipt && receipt->complete, "positive v4 receipt was not complete");
		const materialization_v4_store_publication_authority authority{
			"semantic-v2:sha256:" + std::string(64U, '9'),
			"semantic-v2:sha256:" + std::string(64U, 'a'),
			"publication-target:v4-positive"};
		auto ingress =
			admit_materialization_v4_store_ingress(value, *receipt, sealed_tasks, authority);
		require(ingress.has_value(), "positive v4 Store ingress was rejected");

		const auto selector_value = selector(value);
		const validated_publication_request publication{
			"memory", selector_value, selector_value.id(), true, std::nullopt, std::nullopt};
		streaming_prepared_store_transaction prepared{
			{selector_value,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 std::nullopt},
			{},
			{}};
		sealed_partition_source source{value, *ingress, {&*sealed}};
		auto observed = execute_materialization_store_streaming(
			value, publication, std::move(prepared), source);
		require(!observed.first_issue && observed.publication_attempted &&
					observed.publish_returned_record && observed.publish_returned_handle &&
					observed.candidate_manifest &&
					observed.candidate_manifest->partitions.front().complete &&
					source.replay_count == 2U,
				"positive v4 result did not publish through bounded Store");
		require(observed.publish_returned_record->state == sdk::publication_state::committed &&
					!observed.publish_returned_record->corrupt,
				"positive v4 publication was not committed and noncorrupt");

		sealed->translation.partition.coverage.front().state = "unknown";
		sealed->translation.partition.coverage.front().reason = "tampered";
		sealed_partition_source tampered_source{value, *ingress, {&*sealed}};
		streaming_prepared_store_transaction tampered_prepared{
			{selector_value,
			 {1U, 0U, 0U},
			 "sha256:6666666666666666666666666666666666666666666666666666666666666666",
			 std::nullopt},
			{},
			{}};
		auto rejected = execute_materialization_store_streaming(
			value, publication, std::move(tampered_prepared), tampered_source);
		require(rejected.first_issue && !rejected.publication_attempted,
				"tampered v4 result crossed the Store publication boundary");
	}
} // namespace

int main()
{
	positive_v4_store_publication();
	return 0;
}
