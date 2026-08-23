#include "llvm/clang22/provider_worker_v4_claim_bridge.hpp"

#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

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

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::vector<std::string> effective_arguments()
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			"project://src/main.cpp",
		};
#else
		return {"/usr/bin/clang++", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
#endif
	}

	[[nodiscard]] std::vector<std::string> qualified_read_roots()
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {CXXLENS_TEST_CLANG22_ROOT};
#else
		return {"/usr"};
#endif
	}

	[[nodiscard]] sdk::relation_descriptor relation_descriptor()
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
		value.merge = sdk::merge_mode::functional_assertion;
		value.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  value.contract_digest + "\n" + value.canonical_form());
		return value;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		require(registry.add(relation_descriptor()).has_value(), "bridge relation rejected");
		auto built = registry.build("worker-v4-claim-bridge-test");
		require(built.has_value(), "bridge relation engine rejected");
		return std::move(*built);
	}

	[[nodiscard]] sdk::detached_row row(std::string key, std::string payload)
	{
		const auto descriptor = relation_descriptor();
		sdk::row_builder builder{descriptor};
		require(builder
					.set({descriptor.id, descriptor.columns[0].id, descriptor.columns[0].type},
						 sdk::detached_cell::typed("company_item_id", std::move(key)))
					.has_value(),
				"bridge claim key rejected");
		require(builder
					.set({descriptor.id, descriptor.columns[1].id, descriptor.columns[1].type},
						 {descriptor.columns[1].type,
						  sdk::cell_state::present,
						  sdk::scalar_value{std::move(payload)},
						  std::nullopt})
					.has_value(),
				"bridge claim payload rejected");
		auto finished = std::move(builder).finish();
		require(finished.has_value(), "bridge claim row did not finish");
		return std::move(*finished);
	}

	[[nodiscard]] source_closure_task_v4_decoded decoded_fixture()
	{
		auto closure = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
		});
		require(closure.has_value(), "bridge closure fixture was rejected");
		source_closure_task_v4_input input;
		input.base_task_index = 0U;
		input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		input.task_input_digest = digest('2');
		input.logical_working_directory = "project://src";
		auto arguments = effective_arguments();
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, arguments);
		require(invocation.has_value(), "bridge invocation digest was rejected");
		input.normalized_invocation_digest = std::move(*invocation);
		input.toolchain_digest = semantic('4');
		input.environment_digest = digest('5');
		input.closure = std::move(*closure);
		input.main_logical_path = "project://src/main.cpp";
		auto identity = derive_source_closure_task_v4_identity(input);
		require(identity.has_value(), "bridge task identity was rejected");
		return {std::move(input), std::move(*identity)};
	}

	[[nodiscard]] provider_task_v4_input_authority
	input_authority(const source_closure_task_v4_input& input)
	{
		auto arguments = effective_arguments();
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, arguments);
		require(invocation.has_value(), "bridge authority digest was rejected");
		return {std::move(*invocation),
				input.logical_working_directory,
				std::move(arguments),
				qualified_read_roots()};
	}

	[[nodiscard]] source_closure_manifest manifest(const source_closure_snapshot& closure)
	{
		source_closure_manifest output;
		output.closure_id = closure.snapshot_id;
		output.closure_digest = closure.closure_digest;
		for (const auto& member : closure.members)
			output.members.push_back({member.file_id,
									  member.logical_path,
									  "main",
									  "utf8",
									  member.size_bytes,
									  member.content_digest,
									  member.read_only});
		for (const auto& blob : closure.blobs)
			output.blobs.push_back({blob.content_digest, blob.size_bytes});
		auto digest_value = source_closure_manifest_digest(closure);
		require(digest_value.has_value(), "bridge manifest digest was rejected");
		output.manifest_digest = std::move(*digest_value);
		return output;
	}

	[[nodiscard]] provider_task_v4_base_task
	base_task(const source_closure_task_v4_decoded& metadata)
	{
		provider_task_v4_base_task output;
		output.provider_task_id = metadata.input.base_provider_task_id;
		output.provider_execution_id = "provider-execution:v4-one";
		output.canonical_base_task_digest = metadata.identity.base_task_digest;
		output.task_input_digest = metadata.input.task_input_digest;
		output.normalized_invocation_digest = metadata.input.normalized_invocation_digest;
		output.toolchain_digest = metadata.input.toolchain_digest;
		output.environment_digest = metadata.input.environment_digest;
		output.working_directory = metadata.input.logical_working_directory;
		const auto* main = metadata.input.closure.find_member(metadata.input.main_logical_path);
		require(main != nullptr, "bridge base task lost main member");
		output.source = {"source-snapshot:v4-one",
						 main->file_id,
						 main->logical_path,
						 main->content_digest,
						 main->size_bytes,
						 "utf8",
						 digest('4', "line-index:sha256:"),
						 true};
		return output;
	}

	[[nodiscard]] provider_task_v4 task(const source_closure_task_v4_decoded& metadata,
										const provider_task_v4_base_task& base)
	{
		provider_task_v4 output;
		output.base_task_index = metadata.input.base_task_index;
		output.base_provider_task_id = base.provider_task_id;
		output.base_task_digest = base.canonical_base_task_digest;
		output.open_task = {base.task_input_digest,
							base.normalized_invocation_digest,
							base.toolchain_digest,
							base.environment_digest};
		auto manifest_value = manifest(metadata.input.closure);
		output.source_closure = {metadata.input.closure.snapshot_id,
								 metadata.input.closure.closure_digest,
								 manifest_value.manifest_digest,
								 static_cast<std::uint64_t>(manifest_value.members.size()),
								 static_cast<std::uint64_t>(manifest_value.blobs.size()),
								 manifest_value.blobs.front().size_bytes};
		output.main_logical_path = metadata.input.main_logical_path;
		output.logical_working_directory = metadata.input.logical_working_directory;
		auto task_digest = derive_provider_task_v4_digest(output);
		require(task_digest.has_value(), "bridge task digest was rejected");
		output.task_v4_digest = *task_digest;
		output.task_id = "task:" + *task_digest;
		require(output.task_id == metadata.identity.task_id &&
					output.task_v4_digest == metadata.identity.task_v4_digest,
				"bridge task identity diverged from worker metadata");
		return output;
	}

	[[nodiscard]] sdk::claim make_claim(const sdk::relation_engine& value,
										std::string key,
										std::string payload,
										const std::string_view provenance)
	{
		sdk::observation observed{
			row(std::move(key), std::move(payload)),
			{"universe-1", {"all"}},
			"company.test.interpretation-1",
			{"provider.test", digest('a')},
			{digest('b')},
			std::string{provenance},
			{"under_approximation", "partition", "assumptions:none", {"schema_validated"}},
		};
		auto output = sdk::make_assertion(value, std::move(observed));
		require(output.has_value(), "bridge assertion was rejected");
		return std::move(*output);
	}

	[[nodiscard]] [[maybe_unused]] materialization_v4_claim_translation
	translation(const sdk::relation_engine& value, const source_closure_task_v4_decoded& metadata)
	{
		auto base = base_task(metadata);
		auto task_value = task(metadata, base);
		auto manifest_value = manifest(metadata.input.closure);
		materialization_v4_claim_binding binding_value;
		binding_value.materialization_request_id = "materialization-request:v4-one";
		binding_value.task_index = metadata.input.base_task_index;
		binding_value.base_task = std::move(base);
		binding_value.task = std::move(task_value);
		binding_value.manifest = std::move(manifest_value);
		binding_value.provider_id = "provider.test";
		binding_value.provider_semantic_contract_digest = digest('a');
		binding_value.materializer_id = "cxxlens.clang22.materializer";
		binding_value.materializer_semantic_contract_digest = semantic('5');
		binding_value.canonical_adoption_transform_digest = digest('7');
		binding_value.base_ingestion_transform_digest = digest('8');
		binding_value.guarantee = {
			"under_approximation", "partition", "assumptions:none", {"schema_validated"}};
		binding_value.assumption_set_id = binding_value.guarantee.assumptions;
		binding_value.relation_descriptor_id = relation_descriptor().id;
		binding_value.scope = "compile-unit:v4-one";
		binding_value.interpretation = "company.test.interpretation-1";
		binding_value.precision_profile = "under_approximation";

		auto first = make_claim(value, "item:one", "one", "evidence:task-v4");
		auto basis = sdk::claim_input_basis_digest(first.input_basis);
		require(basis.has_value(), "bridge basis digest was rejected");
		binding_value.direct_basis_digest = *basis;
		sdk::claim_batch batch;
		require(batch.add(first).has_value(), "bridge claim was not staged");
		auto committed = std::move(batch).commit(value);
		require(committed.has_value(), "bridge claim batch was not committed");

		sdk::partition_draft partition;
		partition.relation_descriptor_id = binding_value.relation_descriptor_id;
		partition.scope = binding_value.scope;
		partition.condition = {"universe-1", {"all"}};
		partition.interpretation = binding_value.interpretation;
		partition.producer_semantics = binding_value.provider_semantic_contract_digest;
		partition.producer_input_basis_digest = *basis;
		partition.precision_profile = binding_value.precision_profile;
		partition.assumption_set_id = binding_value.assumption_set_id;
		partition.claims = committed->claims;
		partition.coverage = {{"compile-unit", binding_value.scope, "covered", ""}};
		partition.unresolved = committed->unresolved;
		return {std::move(binding_value), std::move(*committed), std::move(partition)};
	}

	[[nodiscard]] provider_worker_v4_output_authority
	output_authority(const sdk::relation_engine& value)
	{
		return {value,
				{"semantic-v2:sha256:" + std::string(64U, '9'),
				 "semantic-v2:sha256:" + std::string(64U, 'a'),
				 "publication-target:v4-test"}};
	}
} // namespace

int main()
{
	auto metadata = decoded_fixture();
	auto closure = metadata.input.closure;
	auto relation_engine = engine();
	auto authority = input_authority(metadata.input);

	// Missing detached output authority/callback is rejected before the compiler callback can be
	// reached.  The ordinary execution-only path remains separate and fail-closed.
	auto rejected = execute_provider_worker_v4_with_claim_output(
		{{metadata, closure, authority, {}}, output_authority(relation_engine), {}});
	require(!rejected && rejected.error().code == "provider-worker-v4.output-invalid",
			"v4 claim bridge accepted a missing output callback");

	auto invalid_authority = output_authority(relation_engine);
	invalid_authority.publication.publication_target.clear();
	rejected = execute_provider_worker_v4_with_claim_output(
		{{metadata, closure, authority, {}},
		 std::move(invalid_authority),
		 [](provider::clang22::borrowed_translation_unit&)
			 -> sdk::result<materialization_v4_claim_translation>
		 {
			 return sdk::unexpected(sdk::error{"test.callback-not-reached", "callback", {}});
		 }});
	require(!rejected && rejected.error().code == "provider-worker-v4.output-authority-invalid",
			"v4 claim bridge accepted invalid output authority");

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	const auto expected_task = metadata.identity.task_id;
	const auto expected_closure = closure.snapshot_id;
	auto detached = translation(relation_engine, metadata);
	bool callback_ran = false;
	auto positive = execute_provider_worker_v4_with_claim_output(
		{{std::move(metadata), std::move(closure), authority, {}},
		 output_authority(relation_engine),
		 [detached = std::move(detached),
		  &callback_ran](provider::clang22::borrowed_translation_unit& unit) mutable
			 -> sdk::result<materialization_v4_claim_translation>
		 {
			 callback_ran = true;
			 (void)unit.ast();
			 (void)unit.source_manager();
			 return std::move(detached);
		 }});
	if (!positive)
		std::cerr << "v4 claim bridge positive failed: " << positive.error().code << " / "
				  << positive.error().field << " / " << positive.error().detail << '\n';
	require(positive.has_value(), "v4 claim bridge did not seal positive detached output");
	require(callback_ran, "v4 claim bridge callback did not run");
	require(positive->execution.task_id == expected_task,
			"v4 claim bridge execution identity drifted");
	require(positive->claim.receipt.task_id == expected_task,
			"v4 claim bridge claim identity drifted");
	require(positive->store_ingress.receipt.complete,
			"v4 claim bridge issued ingress for incomplete output");

	// A detached translation bound to another task cannot cross the worker authority boundary.
	auto foreign_metadata = decoded_fixture();
	auto foreign_closure = foreign_metadata.input.closure;
	auto foreign_translation = translation(relation_engine, foreign_metadata);
	foreign_translation.binding.task.task_id.back() =
		foreign_translation.binding.task.task_id.back() == '0' ? '1' : '0';
	auto foreign = execute_provider_worker_v4_with_claim_output(
		{{std::move(foreign_metadata), std::move(foreign_closure), authority, {}},
		 output_authority(relation_engine),
		 [foreign_translation =
			  std::move(foreign_translation)](provider::clang22::borrowed_translation_unit&) mutable
			 -> sdk::result<materialization_v4_claim_translation>
		 {
			 return std::move(foreign_translation);
		 }});
	require(!foreign && foreign.error().code == "source-closure.task-v4-binding-mismatch",
			"v4 claim bridge accepted a foreign detached translation");
#else
	(void)authority;
#endif
	return 0;
}
