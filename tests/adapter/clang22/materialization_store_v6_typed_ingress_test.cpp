#if defined(NDEBUG)
// This standalone contract binary uses assert expressions for its executable checks. Keep
// those checks active even when the install consumer build uses a release configuration.
#undef NDEBUG
#endif

#include "llvm/clang22/materialization_store_v6_typed_ingress.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <latch>
#include <limits>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>
#include <vector>

#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>

#include "llvm/clang22/observation_v2.hpp"
#include "llvm/clang22/provider_task_v4_authority_internal.hpp"
#include "llvm/clang22/source_closure.hpp"
#include "materialization_request_v2_2_fixture.hpp"
#include "sdk/provider_validation_internal.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] sdk::relation_descriptor descriptor(const std::size_t index)
	{
		switch (index)
		{
			case 0U:
				return cc::relations::call_direct_target::descriptor();
			case 1U:
				return cc::relations::call_site::descriptor();
			case 2U:
				return cc::relations::entity::descriptor();
			case 3U:
				return call_observation_v2_descriptor();
			case 4U:
				return entity_observation_v2_descriptor();
			case 5U:
				return type_observation_v2_descriptor();
			default:
				assert(false);
				return {};
		}
	}

	[[nodiscard]] std::array<sdk::relation_descriptor, task_v4_output_descriptor_ids.size()>
	descriptors()
	{
		std::array<sdk::relation_descriptor, task_v4_output_descriptor_ids.size()> output;
		for (std::size_t index{}; index < output.size(); ++index)
			output[index] = descriptor(index);
		return output;
	}

	[[nodiscard]] sdk::relation_descriptor claim_descriptor(const std::size_t index)
	{
		const auto production = descriptor(index);
		sdk::relation_descriptor output;
		output.id = production.id;
		output.name = production.name;
		output.version = production.version;
		output.semantic_major = production.semantic_major;
		output.semantics = production.semantics;
		output.owner_namespace = production.owner_namespace;
		output.columns = production.columns;
		output.key_columns = production.key_columns;
		output.merge = sdk::merge_mode::set;
		output.descriptor_digest =
			*sdk::semantic_digest("cxxlens.relation-descriptor-binding.v2",
								  output.contract_digest + "\n" + output.canonical_form());
		return output;
	}

	[[nodiscard]] sdk::relation_engine engine()
	{
		sdk::relation_registry registry;
		for (std::size_t index{}; index < task_v4_output_descriptor_ids.size(); ++index)
		{
			auto added = registry.add(claim_descriptor(index));
			if (!added)
				std::cerr << "descriptor " << index << ": " << added.error().code << " / "
						  << added.error().field << " / " << added.error().detail << '\n';
			assert(added);
		}
		auto output = registry.build("store-v6-typed-ingress-test");
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] sdk::detached_row row([[maybe_unused]] const std::string_view key)
	{
		using relation = cc::relations::call_direct_target;
		relation::builder builder;
		assert(
			builder.set<relation::call>(sdk::detached_cell::typed("cc_call_id", std::string{key})));
		assert(
			builder.set<relation::target>(sdk::detached_cell::typed("cc_entity_id", "entity:one")));
		assert(builder.set<relation::resolution>(sdk::detached_cell{
			{sdk::scalar_kind::open_symbol, "cc.direct-target-resolution/1", false},
			sdk::cell_state::present,
			sdk::scalar_value{std::string{"syntactic"}},
			std::nullopt}));
		auto output = std::move(builder).finish();
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] source_closure_manifest manifest()
	{
		source_closure_manifest value;
		value.members = {{"", "project://src/main.cpp", "main", "utf8", 7U, content('1'), true}};
		value.blobs = {{content('1'), 7U}};
		auto file = source_closure_file_id(value.members.front().logical_path);
		assert(file);
		value.members.front().file_id = *file;
		auto closure = derive_source_closure_digest(value);
		assert(closure);
		value.closure_digest = *closure;
		value.closure_id = "source-closure:" + *closure;
		auto digest = derive_source_closure_manifest_digest(value);
		assert(digest);
		value.manifest_digest = *digest;
		return value;
	}

	[[nodiscard]] provider_task_v4_base_task base_task(const source_closure_manifest& manifest)
	{
		provider_task_v4_base_task value;
		value.provider_task_id = "task:" + semantic('0');
		value.provider_execution_id = "provider-execution:typed-ingress";
		value.canonical_base_task_digest = content('2');
		value.task_input_digest = content('3');
		value.normalized_invocation_digest = semantic('4');
		value.toolchain_digest = semantic('5');
		value.environment_digest = content('6');
		value.working_directory = "project://src";
		value.source = {"source-snapshot:typed-ingress",
						manifest.members.front().file_id,
						manifest.members.front().logical_path,
						manifest.members.front().content_digest,
						manifest.members.front().size_bytes,
						manifest.members.front().encoding,
						"line-index:sha256:" + std::string(64U, '7'),
						true};
		return value;
	}

	[[nodiscard]] provider_task_v4 task_v4(const source_closure_manifest& manifest,
										   const provider_task_v4_base_task& base)
	{
		provider_task_v4 value;
		value.base_task_index = 0U;
		value.base_provider_task_id = base.provider_task_id;
		value.base_task_digest = base.canonical_base_task_digest;
		value.open_task = {base.task_input_digest,
						   base.normalized_invocation_digest,
						   base.toolchain_digest,
						   base.environment_digest};
		value.source_closure = {
			manifest.closure_id, manifest.closure_digest, manifest.manifest_digest, 1U, 1U, 7U};
		value.main_logical_path = base.source.logical_path;
		value.logical_working_directory = base.working_directory;
		auto digest = derive_provider_task_v4_digest(value);
		assert(digest);
		value.task_v4_digest = *digest;
		value.task_id = "task:" + *digest;
		return value;
	}

	[[nodiscard]] json_value inherited_authority(const std::string& request_id,
												 const std::string& semantic_digest)
	{
		auto document = cxxlens_test_materialization_request_v2_2_complete_document();
		auto fields = *document.as_object();
		auto request_value = json_value::string(request_id);
		auto digest_value = json_value::string(semantic_digest);
		assert(request_value && digest_value);
		fields.insert_or_assign("materialization_request_id", *request_value);
		fields.insert_or_assign("semantic_request_digest", *digest_value);
		auto output = json_value::object(std::move(fields));
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] materialization_request_v2_2
	request([[maybe_unused]] const source_closure_manifest& manifest,
			const provider_task_v4_base_task& base,
			const provider_task_v4& task)
	{
		materialization_request_v2_2 value;
		value.materialization_request_id = "materialization-authority:typed-ingress";
		value.semantic_request_digest = semantic('8');
		value.inherited_authority =
			inherited_authority(value.materialization_request_id, value.semantic_request_digest);
		value.base_tasks = {base};
		value.source_closures = {task.source_closure};
		value.task_extensions = {task};
		auto digest = derive_materialization_request_v2_2_digest(value);
		assert(digest && manifest.closure_id == task.source_closure.source_closure_id);
		value.request_digest = *digest;
		value.request_id = "materialization-request:" + *digest;
		return value;
	}

	[[nodiscard]] sdk::claim assertion(const sdk::relation_engine& engine,
									   const std::string_view provenance)
	{
		sdk::observation observed{
			row("call:one"),
			{"universe-typed-ingress", {"all"}},
			"typed-ingress-interpretation",
			{"provider.clang22", content('9')},
			{content('a')},
			std::string{provenance},
			{"under_approximation", "partition", "assumptions:none", {"schema_validated"}}};
		auto output = sdk::make_assertion(engine, std::move(observed));
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] std::string hidden_precursor_ref(const sdk::claim& value)
	{
		auto singleton =
			sdk::claim_batch_content_digest(std::span<const sdk::claim>{&value, 1U}, {}, {}, {});
		assert(singleton);
		auto output = sdk::canonical_identity_digest(
			"materialization-claim-envelope",
			std::array{sdk::canonical_value::from_string("hidden_precursor"),
					   sdk::canonical_value::from_string(*singleton)});
		assert(output);
		return *output;
	}

	struct translation_fixture
	{
		materialization_v4_claim_translation value;
		std::vector<materialization_store_v6_canonicalization_input> edges;
	};

	[[nodiscard]] translation_fixture translation(const sdk::relation_engine& engine,
												  const source_closure_manifest& manifest,
												  const provider_task_v4_base_task& base,
												  const provider_task_v4& task,
												  const bool distinct_final_row)
	{
		materialization_v4_claim_binding binding;
		binding.materialization_request_id = "materialization-authority:typed-ingress";
		binding.task_index = 0U;
		binding.base_task = base;
		binding.task = task;
		binding.manifest = manifest;
		binding.provider_id = "provider.clang22";
		binding.provider_semantic_contract_digest = content('9');
		binding.materializer_id = "cxxlens.clang22.materializer";
		binding.materializer_semantic_contract_digest = semantic('b');
		binding.canonical_adoption_transform_digest = content('c');
		binding.base_ingestion_transform_digest = content('d');
		binding.guarantee = {
			"under_approximation", "partition", "assumptions:none", {"schema_validated"}};
		binding.assumption_set_id = binding.guarantee.assumptions;
		binding.relation_descriptor_id = descriptor(0U).id;
		binding.scope = "compile-unit:typed-ingress";
		binding.interpretation = "typed-ingress-interpretation";
		binding.precision_profile = "under_approximation";

		auto first_precursor = assertion(engine, "evidence:occurrence-one");
		auto second_precursor = assertion(engine, "evidence:occurrence-two");
		assert(first_precursor.content == second_precursor.content);
		auto basis = sdk::claim_input_basis_digest(first_precursor.input_basis);
		assert(basis);
		binding.direct_basis_digest = *basis;
		const sdk::claim_producer materializer{binding.materializer_id,
											   binding.materializer_semantic_contract_digest};
		const auto final_row_key = distinct_final_row ? "call:two" : "call:one";
		auto first = sdk::make_canonical_claim(engine,
											   first_precursor,
											   materializer,
											   row(final_row_key),
											   binding.canonical_adoption_transform_digest);
		auto second = sdk::make_canonical_claim(engine,
												second_precursor,
												materializer,
												row(final_row_key),
												binding.canonical_adoption_transform_digest);
		assert(first && second && first->content == second->content);
		auto canonical_basis = sdk::claim_input_basis_digest(first->input_basis);
		auto second_basis = sdk::claim_input_basis_digest(second->input_basis);
		assert(canonical_basis && second_basis && *canonical_basis == *second_basis);

		std::vector<materialization_store_v6_canonicalization_input> edges;
		const std::array<std::tuple<std::uint64_t, const sdk::claim*, const sdk::claim*>, 2U>
			edge_inputs{{{0U, &first_precursor, &*first}, {0U, &second_precursor, &*second}}};
		for (const auto& [row_index, precursor, final] : edge_inputs)
		{
			auto final_ref = derive_materialization_store_v6_claim_ref(*final);
			assert(final_ref);
			edges.push_back({0U,
							 row_index,
							 {hidden_precursor_ref(*precursor),
							  *final_ref,
							  binding.canonical_adoption_transform_digest},
							 *precursor});
		}
		sdk::claim_batch batch;
		assert(batch.add(std::move(*first)));
		assert(batch.add(std::move(*second)));
		auto committed = std::move(batch).commit(engine);
		assert(committed);
		assert(committed->claims.size() == 2U);
		assert(committed->conflicts.empty());
		sdk::partition_draft partition;
		partition.relation_descriptor_id = binding.relation_descriptor_id;
		partition.scope = binding.scope;
		partition.condition = {"universe-typed-ingress", {"all"}};
		partition.interpretation = binding.interpretation;
		partition.producer_semantics = binding.materializer_semantic_contract_digest;
		partition.producer_input_basis_digest = *canonical_basis;
		partition.precision_profile = binding.precision_profile;
		partition.assumption_set_id = binding.assumption_set_id;
		partition.claims = committed->claims;
		partition.coverage = {{"compile-unit", binding.scope, "covered", ""}};
		partition.unresolved = committed->unresolved;
		return {{std::move(binding), std::move(*committed), std::move(partition)},
				std::move(edges)};
	}

	[[nodiscard]] provider_worker_v4_normalized_output
	normalized_output(const provider_task_v4& task)
	{
		provider_worker_v4_normalized_output output;
		output.task_id = task.task_id;
		output.task_v4_digest = task.task_v4_digest;
		output.compile_unit = "catalog-compile-unit:typed-ingress";
		for (std::size_t index{}; index < output.batches.size(); ++index)
		{
			const auto id = std::string{task_v4_output_descriptor_ids[index]};
			output.batches[index] = {id,
									 id.starts_with("cc.") ? "canonical" : "observation",
									 "clang22-atomic",
									 id + "-batch",
									 {}};
		}
		output.batches[0U].rows = {row("call:one")};
		assert(output.validate());
		return output;
	}

	[[nodiscard]] sdk::provider::detail::sealed_provider_transcript
	transcript(const provider_worker_v4_normalized_output& normalized)
	{
		const auto relations = descriptors();
		std::vector<sdk::provider::detail::sealed_provider_batch_replay> batches;
		batches.reserve(normalized.batches.size());
		for (std::size_t index{}; index < normalized.batches.size(); ++index)
		{
			const auto& source = normalized.batches[index];
			sdk::provider::detail::sealed_provider_batch_replay batch;
			batch.task_id = normalized.task_id;
			batch.descriptor_id = source.descriptor_id;
			batch.descriptor_digest = relations[index].descriptor_digest;
			batch.dependency_group_id = source.dependency_group_id;
			batch.atomic_output_group_id = source.atomic_output_group_id;
			batch.batch_id = source.batch_id;
			batch.rows = source.rows;
			for (const auto& column : relations[index].columns)
			{
				const auto present = batch.rows.empty() ? 0U : 1U;
				batch.columns.push_back({column.id, present, present});
				if (present != 0U)
					batch.ordered_chunk_digests.push_back(content('1'));
			}
			sdk::provider::columnar_batch_end end{batch.task_id,
												  batch.dependency_group_id,
												  batch.atomic_output_group_id,
												  batch.batch_id,
												  batch.descriptor_id,
												  batch.descriptor_digest,
												  static_cast<std::uint64_t>(batch.rows.size()),
												  batch.columns,
												  batch.ordered_chunk_digests,
												  {}};
			batch.batch_digest = sdk::provider::columnar_batch_digest(end);
			batches.push_back(std::move(batch));
		}
		auto output = sdk::provider::detail::rehydrate_provider_transcript(
			normalized.task_id,
			relations,
			std::move(batches),
			{{"task", normalized.task_id, "covered", ""}},
			{},
			{});
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] std::string process_binding(const provider_task_v4_authority_identity& value)
	{
		auto output = sdk::canonical_identity_digest(
			"process-channel",
			std::array{
				sdk::canonical_value::from_string(value.process_mode),
				sdk::canonical_value::from_string(value.task_id),
				sdk::canonical_value::from_string(value.session_id),
				sdk::canonical_value::from_string(value.task_v4_digest),
				sdk::canonical_value::from_string(value.closure_id),
				sdk::canonical_value::from_string(value.closure_digest),
				sdk::canonical_value::from_string(value.manifest_digest),
				sdk::canonical_value::from_string(value.transfer_digest),
				sdk::canonical_value::from_string(std::to_string(value.stream_id)),
				sdk::canonical_value::from_string(std::to_string(value.first_sequence)),
				sdk::canonical_value::from_integer(value.read_descriptor),
				sdk::canonical_value::from_integer(value.write_descriptor),
				sdk::canonical_value::from_string(std::to_string(value.read_device)),
				sdk::canonical_value::from_string(std::to_string(value.read_inode)),
				sdk::canonical_value::from_string(std::to_string(value.read_mode)),
				sdk::canonical_value::from_string(std::to_string(value.write_device)),
				sdk::canonical_value::from_string(std::to_string(value.write_inode)),
				sdk::canonical_value::from_string(std::to_string(value.write_mode)),
			});
		assert(output);
		return *output;
	}

	[[nodiscard]] provider_task_v4_authority_identity
	authority_identity(const materialization_request_v2_2& request,
					   const source_closure_manifest& manifest,
					   const provider_task_v4_base_task& base,
					   const provider_task_v4& task)
	{
		provider_task_v4_authority_identity value;
		value.authority_schema = std::string{provider_task_v4_authority_schema};
		value.request_schema = request.schema;
		value.request_version = request.request_version;
		value.request_id = request.request_id;
		value.request_digest = request.request_digest;
		value.materialization_request_id = request.materialization_request_id;
		value.semantic_request_digest = request.semantic_request_digest;
		value.protocol_major = request.protocol_major;
		value.protocol_minor = request.protocol_minor;
		value.required_features = request.required_features;
		value.task_count = 1U;
		value.task_index = 0U;
		value.provider_task_id = task.task_id;
		value.provider_execution_id = base.provider_execution_id;
		value.task_schema = std::string{provider_task_v4_authority_task_schema};
		value.task_id = task.task_id;
		value.task_v4_digest = task.task_v4_digest;
		value.base_task_digest = task.base_task_digest;
		value.main_logical_path = task.main_logical_path;
		value.logical_working_directory = task.logical_working_directory;
		value.task_input_digest = task.open_task.task_input_digest;
		value.normalized_invocation_digest = task.open_task.normalized_invocation_digest;
		value.environment_digest = task.open_task.environment_digest;
		value.session_id = "provider-session:sha256:" + std::string(64U, 'e');
		value.closure_id = manifest.closure_id;
		value.closure_digest = manifest.closure_digest;
		value.manifest_digest = manifest.manifest_digest;
		value.transfer_digest = semantic('f');
		value.process_mode = std::string{provider_task_v4_authority_process_mode};
		value.stream_id = 11U;
		value.first_sequence = 0U;
		value.read_descriptor = 4;
		value.write_descriptor = 5;
		value.read_device = 101U;
		value.read_inode = 102U;
		value.read_mode = 0100600U;
		value.write_device = 103U;
		value.write_inode = 104U;
		value.write_mode = 0100600U;
		value.process_binding_digest = process_binding(value);
		value.toolchain_digest = task.open_task.toolchain_digest;
		value.toolchain_family = "clang";
		value.toolchain_exact_version = "22.1.0";
		value.toolchain_target_triple = "x86_64-pc-linux-gnu";
		value.toolchain_executable = "toolchain://llvm-22/bin/clang++";
		value.toolchain_executable_digest = content('1');
		value.builtin_headers_digest = content('2');
		value.toolchain_sysroot = "sysroot://llvm-22";
		value.abi_digest = content('3');
		value.plugin_spec_digest = content('4');
		value.argument_count = 4U;
		value.longest_argument_bytes = 64U;
		value.root_count = 1U;
		value.longest_root_path_bytes = 64U;
		value.manifest_bytes = 256U;
		value.member_count = manifest.members.size();
		value.blob_count = manifest.blobs.size();
		value.blob_bytes = 7U;
		value.unique_blob_bytes = 7U;
		value.source_bytes = 7U;
		value.aggregate_source_bytes = 7U;
		value.output_group_count = task_v4_output_descriptor_ids.size();
		value.output_bytes = 4096U;
		value.resident_bytes = 4096U;
		return value;
	}

	[[nodiscard]] sdk::provider::detail::provider_runtime_receipt
	runtime_receipt(const provider_task_v4_authority_identity& identity,
					const materialization_v4_claim_sealed& sealed,
					const sdk::provider::detail::sealed_provider_transcript& transcript)
	{
		sdk::provider::frame frame;
		frame.type = sdk::provider::message_type::task_complete;
		frame.stream_id = identity.stream_id;
		frame.sequence = identity.first_sequence;
		frame.flags = static_cast<std::uint16_t>(sdk::provider::frame_flag::end_of_stream);
		sdk::provider::detail::provider_runtime_provenance provenance;
		provenance.provider_id = sealed.translation.binding.provider_id;
		provenance.provider_version = {1U, 0U, 0U};
		provenance.provider_binary_digest = content('5');
		provenance.provider_semantic_contract_digest =
			sealed.translation.binding.provider_semantic_contract_digest;
		provenance.protocol_session_id = identity.session_id;
		provenance.task_id = identity.task_id;
		provenance.task_input_digest = identity.task_input_digest;
		provenance.normalized_invocation_digest = identity.normalized_invocation_digest;
		provenance.toolchain_digest = identity.toolchain_digest;
		provenance.environment_digest = identity.environment_digest;
		provenance.sandbox_policy_digest = semantic('6');
		provenance.stream_id = identity.stream_id;
		const std::array frames{frame};
		auto output = sdk::provider::detail::make_provider_runtime_receipt(
			1U, content('7'), frames, std::move(provenance), "provider.success", transcript);
		assert(output);
		return std::move(*output);
	}

	[[nodiscard]] materialization_origin_association
	association(const sdk::claim& claim,
				const sdk::detached_row& worker_row,
				const provider_task_v4_authority_identity& identity,
				const materialization_v4_claim_sealed& sealed,
				const provider_worker_v4_normalized_output& normalized,
				std::optional<std::string> evidence)
	{
		auto claim_ref = derive_materialization_store_v6_claim_ref(claim);
		assert(claim_ref);
		const auto form = worker_row.canonical_form();
		const auto row_digest =
			sdk::content_digest(std::as_bytes(std::span{form.data(), form.size()}));
		materialization_semantic_task_context context{
			identity.provider_task_id,
			identity.task_input_digest,
			normalized.compile_unit,
			"compile-unit:typed-ingress",
			sealed.translation.partition.condition.universe,
			sealed.translation.partition.condition.id(),
			sealed.translation.partition.interpretation};
		auto context_value = sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string(context.provider_task_id),
			sdk::canonical_value::from_string(context.task_input_digest),
			sdk::canonical_value::from_string(context.selected_catalog_compile_unit_id),
			sdk::canonical_value::from_string(context.compile_unit_id),
			sdk::canonical_value::from_string(context.condition_universe_id),
			sdk::canonical_value::from_string(context.condition_id),
			sdk::canonical_value::from_string(context.interpretation_domain),
		});
		auto id = sdk::canonical_identity_digest(
			"materialization-claim-association",
			std::array{
				sdk::canonical_value::from_string(*claim_ref),
				std::move(context_value),
				sdk::canonical_value::from_string(row_digest),
				sdk::canonical_value::from_string(evidence.value_or("")),
			});
		assert(id);
		return {*id, *claim_ref, std::move(context), row_digest, std::move(evidence)};
	}

	void reseal_association_id(materialization_origin_association& value)
	{
		auto context_value = sdk::canonical_value::from_tuple({
			sdk::canonical_value::from_string(value.originating_task.provider_task_id),
			sdk::canonical_value::from_string(value.originating_task.task_input_digest),
			sdk::canonical_value::from_string(
				value.originating_task.selected_catalog_compile_unit_id),
			sdk::canonical_value::from_string(value.originating_task.compile_unit_id),
			sdk::canonical_value::from_string(value.originating_task.condition_universe_id),
			sdk::canonical_value::from_string(value.originating_task.condition_id),
			sdk::canonical_value::from_string(value.originating_task.interpretation_domain),
		});
		auto id = sdk::canonical_identity_digest(
			"materialization-claim-association",
			std::array{
				sdk::canonical_value::from_string(value.stored_claim_ref),
				std::move(context_value),
				sdk::canonical_value::from_string(value.sealed_row_digest),
				sdk::canonical_value::from_string(value.source_evidence_digest.value_or("")),
			});
		assert(id);
		value.association_id = std::move(*id);
	}

	enum class mutation
	{
		none,
		reverse_edges_and_origins,
		annotation,
		origin_row,
		edge_precursor,
		edge_final,
		edge_transform,
		edge_batch,
		edge_row,
		final_row,
		descriptor,
		worker_task,
		closure,
		journal,
		partition,
		record_bound,
		sort_arena,
		cancel,
	};

	[[nodiscard]] materialization_store_v6_ingress_input
	make_input(const sdk::relation_engine& engine, const mutation change = mutation::none)
	{
		auto manifest_value = manifest();
		auto base = base_task(manifest_value);
		auto task = task_v4(manifest_value, base);
		auto request_value = request(manifest_value, base, task);
		auto translated =
			translation(engine, manifest_value, base, task, change == mutation::final_row);
		auto edges = std::move(translated.edges);
		auto sealed =
			seal_materialization_v4_claim_translation(engine, std::move(translated.value));
		assert(sealed);
		const std::array<const materialization_v4_claim_sealed*, 1U> sealed_tasks{&*sealed};
		auto incremental = make_materialization_v4_incremental_receipt(engine, sealed_tasks);
		assert(incremental);
		auto journal_builder = materialization_v4_execution_journal::begin(
			request_value.materialization_request_id, 1U);
		assert(journal_builder);
		assert(journal_builder->record(sealed->receipt, false, 1U));
		auto journal = std::move(*journal_builder).finish(*incremental);
		assert(journal);

		materialization_v4_provider_output_authority output;
		output.materialization_request_id = request_value.materialization_request_id;
		output.publication = {semantic('8'), semantic('9'), semantic('a')};
		output.snapshot = {{"catalog:typed-ingress",
							"stable",
							std::string{engine.generation()},
							"universe-typed-ingress",
							std::string{engine.registry_digest()},
							content('b'),
							content('c')},
						   {1U, 0U, 0U},
						   content('d'),
						   std::nullopt};
		sdk::closure_candidate closure;
		closure.relation_descriptor_id = sealed->partition_manifest.relation_descriptor_id;
		closure.subject_partition_id = sealed->partition_manifest.partition_id;
		closure.partition_content_digest = sealed->partition_manifest.content_digest;
		closure.coverage_digest = sealed->partition_manifest.coverage_digest;
		closure.key_domain_digest = content('e');
		closure.condition = sealed->partition_binding.condition;
		closure.interpretation = sealed->partition_binding.interpretation;
		closure.assumption_set_id = sealed->partition_binding.assumption_set_id;
		closure.closure_kind = "relation-key-enumeration";
		closure.producer_semantics = sealed->partition_binding.producer_semantics;
		closure.evidence_digest = content('f');
		output.closures = {std::move(closure)};

		auto normalized = normalized_output(task);
		auto sealed_transcript = transcript(normalized);
		auto identity = authority_identity(request_value, manifest_value, base, task);
		auto runtime = runtime_receipt(identity, *sealed, sealed_transcript);
		auto authority =
			issue_provider_task_v4_authority(provider_task_v4_authority_identity{identity});
		assert(authority);
		provider_worker_v4_receipt worker{
			std::string{provider_worker_v4_receipt_schema},
			task.task_id,
			task.task_v4_digest,
			task.open_task.task_input_digest,
			manifest_value.closure_id,
			manifest_value.members.front().file_id,
			"translation-unit-executed",
			true,
			{{"analysis_recipe", "claim-output", "provided-by-materializer"},
			 {"output_plan", "claim-output", "provided-by-materializer"},
			 {"publication_target", "claim-output", "provided-by-materializer"}}};
		std::vector<materialization_store_v6_origin_input> origins;
		const std::array<std::size_t, 3U> origin_edges{0U, 0U, 1U};
		for (std::size_t index{}; index < origin_edges.size(); ++index)
		{
			const auto edge_index = origin_edges[index];
			const auto found = std::ranges::find_if(
				sealed->translation.partition.claims,
				[&](const sdk::claim& claim)
				{
					auto ref = derive_materialization_store_v6_claim_ref(claim);
					return ref && *ref == edges[edge_index].edge.final_claim_ref;
				});
			assert(found != sealed->translation.partition.claims.end());
			origins.push_back({edges[edge_index].batch_index,
							   edges[edge_index].row_index,
							   association(*found,
										   normalized.batches[0U].rows[0U],
										   identity,
										   *sealed,
										   normalized,
										   content(static_cast<char>('1' + index)))});
		}

		if (change == mutation::reverse_edges_and_origins)
		{
			std::ranges::reverse(edges);
			std::ranges::reverse(origins);
		}
		else if (change == mutation::annotation)
			origins.front().association.association_id = semantic('0');
		else if (change == mutation::origin_row)
			origins.front().row_index = 1U;
		else if (change == mutation::edge_precursor)
			edges.front().edge.precursor_claim_ref = semantic('0');
		else if (change == mutation::edge_final)
			edges.front().edge.final_claim_ref = semantic('0');
		else if (change == mutation::edge_transform)
			edges.front().edge.transform_semantics = content('0');
		else if (change == mutation::edge_batch)
			edges.front().batch_index = 3U;
		else if (change == mutation::edge_row)
			edges.front().row_index = 99U;
		else if (change == mutation::descriptor)
			normalized.batches[2U].descriptor_id = "cc.foreign.v1";
		else if (change == mutation::worker_task)
			worker.task_id += "-foreign";
		else if (change == mutation::closure)
			output.closures.front().coverage_digest = content('0');
		else if (change == mutation::journal)
			journal->execution_digest = semantic('0');
		else if (change == mutation::partition)
			sealed->translation.partition.coverage.front().state = "unknown";
		else if (change == mutation::record_bound)
			sealed->translation.partition.claims.front().content.assign(
				sdk::detail::bounded_store_v6_record_buffer_bytes + 1U, 'x');
		else if (change == mutation::sort_arena)
		{
			const auto seed = origins.front();
			origins.clear();
			for (std::size_t index{}; index < 16U; ++index)
			{
				auto value = seed;
				value.association.association_id.assign(192U * 1024U, 'a');
				value.association.association_id.back() = static_cast<char>('a' + index);
				origins.push_back(std::move(value));
			}
		}

		std::vector<materialization_store_v6_task_input> tasks;
		tasks.emplace_back(std::move(*authority),
						   std::move(worker),
						   std::move(normalized),
						   std::move(sealed_transcript),
						   std::move(runtime),
						   std::move(*sealed),
						   std::move(edges),
						   std::move(origins));
		materialization_store_v6_ingress_input input{
			std::move(request_value),
			materialization_request_v2_2_required_features(),
			std::move(*incremental),
			std::move(*journal),
			std::move(output),
			std::move(tasks),
			{}};
		if (change == mutation::cancel)
		{
			std::stop_source stop;
			stop.request_stop();
			input.cancellation = stop.get_token();
		}
		return input;
	}

	void positive_lossless_cursor_and_independent_expected()
	{
		auto value = engine();
		auto ingress = make_materialization_store_v6_typed_ingress(value, make_input(value));
		if (!ingress)
			std::cerr << ingress.error().code << " / " << ingress.error().field << " / "
					  << ingress.error().detail << '\n';
		assert(ingress);
		assert(ingress->structural_census().task_count == 1U);
		assert(ingress->structural_census().partition_count == 1U);
		assert(ingress->structural_census().claim_occurrence_count == 2U);
		assert(ingress->structural_census().unique_row_count == 1U);
		assert(ingress->structural_census().annotation_count == 3U);
		assert(ingress->structural_census().canonicalization_edge_count == 2U);
		assert(ingress->semantic_census().unique_claim_content_count == 1U);
		assert(ingress->semantic_census().normalized_row_count == 1U);
		assert(ingress->semantic_census().canonicalization_edge_count == 2U);
		assert(ingress->semantic_census().normalized_descriptor_batch_count == 6U);
		assert(!ingress->immutable_authority_binding().empty());
		assert(!ingress->take_expected_authority());

		auto task = ingress->take_task(0U);
		assert(task);
		assert(task->receipt().canonicalization_edge_count == 2U);
		assert(task->receipt().canonicalization_edge_bytes != 0U);
		assert(!task->next_canonicalization_edge());
		std::array<std::uint64_t, 8U> kinds{};
		for (;;)
		{
			auto event = task->next();
			assert(event);
			if (!*event)
				break;
			++kinds[static_cast<std::size_t>(std::visit(
				[](const auto& value) -> sdk::detail::bounded_store_v6_record_kind
				{
					using type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<type, materialization_store_v6_partition_begin>)
						return sdk::detail::bounded_store_v6_record_kind::partition_begin;
					if constexpr (std::is_same_v<type, materialization_store_v6_claim_occurrence>)
						return sdk::detail::bounded_store_v6_record_kind::claim_occurrence;
					if constexpr (std::is_same_v<type, materialization_store_v6_detached_row>)
						return sdk::detail::bounded_store_v6_record_kind::detached_row;
					if constexpr (std::is_same_v<type, materialization_store_v6_claim_annotation>)
						return sdk::detail::bounded_store_v6_record_kind::claim_annotation;
					if constexpr (std::is_same_v<type, materialization_store_v6_coverage>)
						return sdk::detail::bounded_store_v6_record_kind::coverage;
					if constexpr (std::is_same_v<type, materialization_store_v6_unresolved>)
						return sdk::detail::bounded_store_v6_record_kind::unresolved;
					return sdk::detail::bounded_store_v6_record_kind::partition_end;
				},
				**event))];
		}
		assert(task->authority_complete() && !*task->authority_complete());
		assert(kinds[2U] == 2U && kinds[3U] == 1U && kinds[4U] == 3U);
		for (std::uint64_t edge_index{}; edge_index < 2U; ++edge_index)
		{
			(void)edge_index;
			auto edge = task->next_canonicalization_edge();
			assert(edge && *edge);
			assert((**edge).batch_index == 0U && (**edge).row_index == 0U);
			assert((**edge).edge.precursor_claim_ref ==
				   hidden_precursor_ref((**edge).hidden_precursor));
			assert((**edge).edge.transform_semantics == content('c'));
		}
		auto edge_eof = task->next_canonicalization_edge();
		assert(edge_eof && !*edge_eof);
		assert(task->authority_complete() && *task->authority_complete());
		assert(!task->next());
		assert(!task->next_canonicalization_edge());

		auto expected = ingress->take_expected_authority();
		assert(expected);
		assert(!ingress->take_expected_authority());
		[[maybe_unused]] std::uint64_t expected_count{};
		for (;;)
		{
			auto record = expected->next_semantic_record();
			assert(record);
			if (!*record)
				break;
			++expected_count;
		}
		assert(expected->authority_complete() && *expected->authority_complete());
		assert(expected_count == ingress->structural_census().event_count);
		assert(!expected->next_semantic_record());
	}

	void deterministic_permutation_and_negative_authorities()
	{
		auto value = engine();
		auto first = make_materialization_store_v6_typed_ingress(value, make_input(value));
		auto reversed = make_materialization_store_v6_typed_ingress(
			value, make_input(value, mutation::reverse_edges_and_origins));
		assert(first && reversed);
		assert(first->structural_census() == reversed->structural_census());
		assert(first->semantic_census() == reversed->semantic_census());
		assert(first->immutable_authority_binding() == reversed->immutable_authority_binding());
		for (const auto change : {mutation::annotation,
								  mutation::origin_row,
								  mutation::edge_precursor,
								  mutation::edge_final,
								  mutation::edge_transform,
								  mutation::edge_batch,
								  mutation::edge_row,
								  mutation::final_row,
								  mutation::descriptor,
								  mutation::worker_task,
								  mutation::closure,
								  mutation::journal,
								  mutation::partition,
								  mutation::record_bound,
								  mutation::sort_arena,
								  mutation::cancel})
		{
			auto rejected =
				make_materialization_store_v6_typed_ingress(value, make_input(value, change));
			if (rejected)
				std::cerr << "mutation accepted: " << static_cast<int>(change) << '\n';
			assert(!rejected);
			if (change == mutation::record_bound || change == mutation::sort_arena)
				assert(rejected.error().code ==
					   "materialization.store-v6-typed-ingress-resource-exhausted");
		}
	}

	void hard_task_bound()
	{
		assert(validate_materialization_store_v6_task_count(4096U));
		assert(!validate_materialization_store_v6_task_count(4097U));
		assert(!validate_materialization_store_v6_task_count(0U));

		constexpr auto aggregate = sdk::detail::bounded_store_v6_max_aggregate_bytes;
		constexpr auto task = sdk::detail::bounded_store_v6_source_window_bytes;
		auto exact_task = checked_materialization_store_v6_spool_charge(0U, 0U, task);
		assert(exact_task && *exact_task == task);
		assert(!checked_materialization_store_v6_spool_charge(0U, 0U, task + 1U));
		auto exact_aggregate =
			checked_materialization_store_v6_spool_charge(aggregate - 1U, aggregate - 1U, 1U);
		assert(exact_aggregate && *exact_aggregate == aggregate);
		assert(!checked_materialization_store_v6_spool_charge(aggregate, aggregate, 1U));
		assert(!checked_materialization_store_v6_spool_charge(
			std::numeric_limits<std::uint64_t>::max() - 1U,
			std::numeric_limits<std::uint64_t>::max() - 1U,
			2U));
		assert(!checked_materialization_store_v6_spool_charge(1U, 2U, 0U));

		[[maybe_unused]] constexpr auto record = sdk::detail::bounded_store_v6_record_buffer_bytes;
		assert(validate_materialization_store_v6_record_source_bytes(record));
		assert(!validate_materialization_store_v6_record_source_bytes(record + 1U));
		constexpr auto sort = sdk::detail::bounded_store_v6_sort_arena_bytes;
		auto exact_sort = checked_materialization_store_v6_sort_arena_charge(0U, sort);
		assert(exact_sort && *exact_sort == sort);
		assert(!checked_materialization_store_v6_sort_arena_charge(sort, 1U));
		assert(!checked_materialization_store_v6_sort_arena_charge(
			std::numeric_limits<std::uint64_t>::max() - 1U, 2U));
	}

	void mid_task_cancellation_is_terminal()
	{
		auto value = engine();
		auto input = make_input(value);
		auto& origins = input.tasks.front().origins;
		const auto seed = origins.front();
		for (std::uint64_t index{}; index < 600U; ++index)
		{
			auto next = seed;
			const auto counter = std::to_string(index);
			next.association.source_evidence_digest =
				sdk::content_digest(std::as_bytes(std::span{counter.data(), counter.size()}));
			reseal_association_id(next.association);
			origins.push_back(std::move(next));
		}
		std::stop_source stop;
		input.cancellation = stop.get_token();
		std::latch entered{1U};
		std::optional<sdk::result<materialization_store_v6_typed_ingress>> outcome;
		std::jthread worker{[&]
							{
								entered.count_down();
								outcome.emplace(make_materialization_store_v6_typed_ingress(
									value, std::move(input)));
							}};
		entered.wait();
		std::this_thread::sleep_for(std::chrono::microseconds{200});
		stop.request_stop();
		worker.join();
		assert(outcome && !*outcome);
		assert(outcome->error().code == "materialization.store-v6-typed-ingress-cancelled");
	}
} // namespace

int main()
{
	positive_lossless_cursor_and_independent_expected();
	deterministic_permutation_and_negative_authorities();
	hard_task_bound();
	mid_task_cancellation_is_terminal();
	return 0;
}
