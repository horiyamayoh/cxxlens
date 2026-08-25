#include "materializer_report_v2_2_encoder.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <ctime>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "materialization_identity.hpp"
#include "observation_v2.hpp"
#include "provider_task_v4.hpp"
#include "runtime/monotonic_clock_port_internal.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;
		using materialization::materialization_occurrence_file;
		using materialization::materialization_occurrence_manifest_path;
		using materialization::measured_materialization_occurrence;
		using materialization::raw_input_observation;
		namespace provider_detail = sdk::provider::detail;

		[[nodiscard]] sdk::error failure(std::string field, std::string detail = {})
		{
			return {"materialization.report-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] std::string utc_now()
		{
			const auto time =
				std::chrono::system_clock::to_time_t(cxxlens::runtime::wall_clock_now());
			std::tm value{};
#if defined(_WIN32)
			(void)gmtime_s(&value, &time);
#else
			(void)gmtime_r(&time, &value);
#endif
			char output[21]{};
			if (std::strftime(output, sizeof(output), "%Y-%m-%dT%H:%M:%SZ", &value) == 0U)
				return {};
			return output;
		}

		[[nodiscard]] json_value text(const std::string_view value)
		{
			return json_value::string(std::string{value}).value();
		}

		[[nodiscard]] json_value array(std::vector<json_value> values)
		{
			return json_value::array(std::move(values));
		}

		[[nodiscard]] json_value
		object(std::initializer_list<std::pair<std::string, json_value>> fields)
		{
			json_value::object_type value;
			for (auto&& [key, child] : fields)
				value.emplace(std::move(key), std::move(child));
			return json_value::object(std::move(value)).value();
		}

		[[nodiscard]] const json_value* member(const json_value& root, const std::string_view name)
		{
			return root.member(name);
		}

		[[nodiscard]] sdk::result<std::string> required_string(const json_value& root,
															   const std::string_view name)
		{
			const auto* value = member(root, name);
			if (value == nullptr || value->as_string() == nullptr)
				return sdk::unexpected(failure(std::string{name}, "missing-or-not-string"));
			return *value->as_string();
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_projection_digest(const std::string_view domain, const json_value& projection)
		{
			return materialization::projection_digest(domain, projection);
		}

		[[nodiscard]] json_value empty_projection_digest(const std::string_view domain)
		{
			return text(semantic_projection_digest(domain, array({})).value());
		}

		[[nodiscard]] json_value nullable_id(const std::optional<std::string>& value)
		{
			return value ? text(*value) : json_value::null();
		}

		[[nodiscard]] json_value condition_value(const sdk::claim_condition& condition)
		{
			std::vector<json_value> fragments;
			fragments.reserve(condition.fragments.size());
			for (const auto& fragment : condition.fragments)
				fragments.push_back(text(fragment));
			return object({{"universe", text(condition.universe)},
						   {"fragments", array(std::move(fragments))}});
		}

		[[nodiscard]] json_value selector_fields(const sdk::snapshot_series_selector& selector)
		{
			return object(
				{{"catalog_id", text(selector.catalog_id)},
				 {"channel_id", text(selector.channel_id)},
				 {"engine_generation_id", text(selector.engine_generation_id)},
				 {"condition_universe_id", text(selector.condition_universe_id)},
				 {"relation_registry_digest", text(selector.relation_registry_digest)},
				 {"interpretation_policy_digest", text(selector.interpretation_policy_digest)},
				 {"trust_policy_digest", text(selector.trust_policy_digest)}});
		}

		[[nodiscard]] json_value selector_value(const sdk::snapshot_series_selector& selector)
		{
			return object(
				{{"fields", selector_fields(selector)}, {"series_id", text(selector.id())}});
		}

		[[nodiscard]] json_value publication_record(const sdk::publication_record& record)
		{
			const auto state = [&]() -> std::string_view
			{
				switch (record.state)
				{
					case sdk::publication_state::created:
						return "created";
					case sdk::publication_state::staged:
						return "staged";
					case sdk::publication_state::validating:
						return "validating";
					case sdk::publication_state::committed:
						return "committed";
					case sdk::publication_state::rejected:
						return "rejected";
					case sdk::publication_state::rolled_back:
						return "rolled_back";
				}
				return "invalid";
			}();
			return object(
				{{"publication_id", text(record.publication_id)},
				 {"series_id", text(record.series_id)},
				 {"snapshot_id", text(record.snapshot_id)},
				 {"sequence", json_value::unsigned_integer(record.sequence)},
				 {"physical_generation", json_value::unsigned_integer(record.physical_generation)},
				 {"parent_publication", nullable_id(record.parent_publication)},
				 {"state", text(state)},
				 {"corrupt", json_value::boolean(record.corrupt)}});
		}

		[[nodiscard]] json_value snapshot_partition_manifest(const sdk::partition_manifest& value)
		{
			return object({{"partition_id", text(value.partition_id)},
						   {"relation_descriptor_id", text(value.relation_descriptor_id)},
						   {"input_basis_digest", text(value.input_basis_digest)},
						   {"claim_set_digest", text(value.claim_set_digest)},
						   {"coverage_digest", text(value.coverage_digest)},
						   {"content_digest", text(value.content_digest)},
						   {"claim_count", json_value::unsigned_integer(value.claim_count)},
						   {"complete", json_value::boolean(value.complete)}});
		}

		[[nodiscard]] json_value snapshot_manifest(const sdk::snapshot_manifest& value)
		{
			std::vector<json_value> partitions;
			partitions.reserve(value.partitions.size());
			for (const auto& partition : value.partitions)
				partitions.push_back(snapshot_partition_manifest(partition));
			std::vector<json_value> closure_ids;
			closure_ids.reserve(value.closure_ids.size());
			for (const auto& id : value.closure_ids)
				closure_ids.push_back(text(id));
			return object(
				{{"schema", text(value.schema)},
				 {"snapshot_semantics_version", text(value.snapshot_semantics_version.string())},
				 {"catalog_semantic_digest", text(value.catalog_semantic_digest)},
				 {"condition_universe_id", text(value.condition_universe_id)},
				 {"relation_registry_digest", text(value.relation_registry_digest)},
				 {"interpretation_policy_digest", text(value.interpretation_policy_digest)},
				 {"partitions", array(std::move(partitions))},
				 {"closure_ids", array(std::move(closure_ids))},
				 {"snapshot_id", text(value.id)}});
		}

		[[nodiscard]] json_value raw_input_value(const raw_input_observation& input)
		{
			return object(
				{{"byte_limit", json_value::unsigned_integer(input.byte_limit)},
				 {"observed_size_bytes", json_value::unsigned_integer(input.observed_size_bytes)},
				 {"observed_prefix_digest", text(input.observed_prefix_digest)},
				 {"complete", json_value::boolean(input.complete)}});
		}

		[[nodiscard]] json_value occurrence_file(const materialization_occurrence_file& value,
												 const bool include_role)
		{
			if (include_role)
				return object({{"role", text(value.role)},
							   {"path", text(value.path)},
							   {"digest", text(value.digest)}});
			return object({{"path", text(value.path)}, {"digest", text(value.digest)}});
		}

		[[nodiscard]] json_value
		measured_occurrence(const measured_materialization_occurrence& value,
							const json_value& request_root)
		{
			const auto* tool = member(request_root, "tool");
			const auto configuration = tool == nullptr
				? std::string{}
				: required_string(*tool, "package_configuration").value();
			std::vector<json_value> files;
			files.reserve(value.manifest().files.size());
			for (const auto& file : value.manifest().files)
				files.push_back(occurrence_file(file, true));
			const auto tool_file = std::ranges::find(value.manifest().files,
													 "materializer-executable",
													 &materialization_occurrence_file::role);
			const auto worker_file = std::ranges::find(value.manifest().files,
													   "worker-executable",
													   &materialization_occurrence_file::role);
			const auto tool_value = tool_file == value.manifest().files.end()
				? json_value::null()
				: occurrence_file(*tool_file, false);
			const auto worker_value = worker_file == value.manifest().files.end()
				? json_value::null()
				: occurrence_file(*worker_file, false);
			return object(
				{{"manifest_path", text(materialization_occurrence_manifest_path)},
				 {"manifest_file_digest", text(value.receipt().manifest_file_digest)},
				 {"occurrence_payload_digest", text(value.receipt().occurrence_payload_digest)},
				 {"inventory_digest", text(value.receipt().inventory_digest)},
				 {"source_revision", text(value.manifest().source_revision)},
				 {"source_tree", text(value.manifest().source_tree)},
				 {"configuration", text(configuration)},
				 {"files", array(std::move(files))},
				 {"tool", tool_value},
				 {"worker", worker_value}});
		}

		[[nodiscard]] json_value provider_value(const json_value& root)
		{
			const auto& tool = *member(root, "tool");
			const auto& worker = *member(root, "worker");
			return object(
				{{"tool_executable", *member(tool, "executable")},
				 {"tool_interface_version", *member(tool, "interface_version")},
				 {"worker_executable", *member(worker, "executable")},
				 {"provider_id", *member(worker, "provider_id")},
				 {"provider_version", *member(worker, "provider_version")},
				 {"semantic_contract_digest", *member(worker, "semantic_contract_digest")},
				 {"protocol_major", *member(worker, "protocol_major")},
				 {"protocol_minor", *member(worker, "protocol_minor")},
				 {"required_features", *member(worker, "required_features")},
				 {"sandbox_policy_digest", *member(worker, "sandbox_policy_digest")}});
		}

		[[nodiscard]] std::string claim_row_ref(const sdk::claim& claim)
		{
			const auto canonical = claim.row.canonical_form();
			const auto digest = sdk::canonical_identity_digest(
				"materialization-claim-row",
				std::array{
					sdk::canonical_value::from_string(claim.descriptor),
					sdk::canonical_value::from_bytes(std::vector<std::byte>{
						std::as_bytes(std::span{canonical.data(), canonical.size()}).begin(),
						std::as_bytes(std::span{canonical.data(), canonical.size()}).end()}),
				});
			return digest.value();
		}

		[[nodiscard]] std::string claim_envelope_ref(const sdk::claim& claim,
													 const std::string_view role)
		{
			const std::array<sdk::claim, 1U> singleton{claim};
			const auto digest = sdk::claim_batch_content_digest(singleton, {}, {}, {});
			const auto reference = sdk::canonical_identity_digest(
				"materialization-claim-envelope",
				std::array{sdk::canonical_value::from_string(std::string{role}),
						   sdk::canonical_value::from_string(digest.value())});
			return reference.value();
		}

		[[nodiscard]] std::string claim_row_digest(const sdk::claim& claim)
		{
			const auto canonical = claim.row.canonical_form();
			return sdk::content_digest(
				std::as_bytes(std::span{canonical.data(), canonical.size()}));
		}

		[[nodiscard]] json_value task_context_value(const provider_task_v4_task_authority& task)
		{
			return object(
				{{"provider_task_id", text(task.provider_task_id)},
				 {"task_input_digest", text(task.task_input_digest)},
				 {"selected_catalog_compile_unit_id", text(task.selected_catalog_compile_unit_id)},
				 {"compile_unit_id", text(task.compile_unit_id)},
				 {"condition_universe_id", text(task.condition_universe_id)},
				 {"condition_id", text(task.condition_id)},
				 {"interpretation_domain", text(task.interpretation_domain)}});
		}

		[[nodiscard]] json_value
		task_execution_key_value(const provider_task_v4_task_authority& task)
		{
			return array({text(task.provider_task_id),
						  text(task.task_input_digest),
						  text(task.provider_execution_id)});
		}

		[[nodiscard]] json_value
		semantic_result_key_value(const provider_task_v4_task_authority& task)
		{
			return array({text(task.provider_task_id),
						  text(task.task_input_digest),
						  text(task.selected_catalog_compile_unit_id),
						  text(task.compile_unit_id)});
		}

		[[nodiscard]] std::optional<std::string>
		primary_span_bundle_digest(const sdk::claim& claim,
								   const provider_task_v4_task_authority& task);

		[[nodiscard]] json_value
		input_transfer_value(const provider_detail::sealed_host_input& input)
		{
			// The installed request contract is a single, non-negotiable Protocol 2.0
			// transfer.  Do not reconstruct a version string from a provider-reported
			// patch component (there is no patch component on the wire).
			return object(
				{{"protocol_version", text("2.0.0")},
				 {"required_feature", text("task-input-chunks-v2")},
				 {"task_input_codec", text("cxxlens.clang22.task.v4")},
				 {"logical_input_bytes", json_value::unsigned_integer(input.total_bytes())},
				 {"logical_input_digest", text(input.task().task_input_digest)},
				 {"canonical_chunk_bytes", json_value::unsigned_integer(input.chunk_bytes())},
				 {"chunk_count", json_value::unsigned_integer(input.chunk_count())},
				 {"ordered_chunk_payload_digest_set_digest",
				  text(input.ordered_chunk_digest_set_digest())}});
		}

		[[nodiscard]] json_value
		runtime_receipt_value(const provider_detail::provider_runtime_receipt& receipt)
		{
			return object(
				{{"raw_frame_stream_bytes",
				  json_value::unsigned_integer(receipt.raw_stdout_byte_count())},
				 {"raw_frame_stream_digest", text(receipt.raw_stdout_sha256())},
				 {"frame_count", json_value::unsigned_integer(receipt.decoded_frame_count())},
				 {"frame_transcript_digest", text(receipt.frame_transcript_digest())},
				 {"sealed_transcript_digest", text(receipt.sealed_transcript_digest())}});
		}

		[[nodiscard]] std::vector<const sdk::claim*>
		claims_for_descriptor(const materializer_store_execution& execution,
							  const std::string_view descriptor)
		{
			std::vector<const sdk::claim*> result;
			for (const auto& sealed : execution.claims)
				if (sealed.translation.binding.relation_descriptor_id == descriptor)
					for (const auto& claim : sealed.translation.batch.claims)
						result.push_back(&claim);
			return result;
		}

		[[nodiscard]] std::vector<const sdk::claim*>
		reference_claims_for_descriptor(const materializer_store_execution& execution,
										const std::string_view descriptor)
		{
			std::vector<const sdk::claim*> result;
			for (const auto& claim : execution.reference_claims)
				if (claim.descriptor == descriptor)
					result.push_back(&claim);
			std::ranges::sort(result,
							  [](const auto* left, const auto* right)
							  {
								  const auto left_row = left->row.canonical_form();
								  const auto right_row = right->row.canonical_form();
								  return std::pair{left_row, left->content} <
									  std::pair{right_row, right->content};
							  });
			return result;
		}

		[[nodiscard]] json_value observation_census(const std::string_view descriptor,
													const std::vector<json_value>& rows)
		{
			auto digest = semantic_projection_digest(
				"cxxlens.clang22-observation-equivalence-set.v1",
				object({{"descriptor_id", text(descriptor)}, {"rows", array(rows)}}));
			return object({{"rows", array(rows)},
						   {"exact_equivalence_count", json_value::unsigned_integer(rows.size())},
						   {"non_exact_equivalence_count", json_value::unsigned_integer(0U)},
						   {"row_equivalence_set_digest", text(digest.value())}});
		}

		[[nodiscard]] json_value row_binding_value(const sdk::claim& claim,
												   const provider_task_v4_task_authority& task,
												   const bool observation)
		{
			const auto canonical = claim.row.canonical_form();
			const auto row_digest =
				sdk::content_digest(std::as_bytes(std::span{canonical.data(), canonical.size()}));
			const auto context = task_context_value(task);
			const auto span_digest = observation ? primary_span_bundle_digest(claim, task)
												 : std::optional<std::string>{};
			std::optional<bool> exact_equivalence;
			std::optional<std::string> limitation_digest;
			if (observation)
			{
				const materialization::observation_v2_task_authority authority{
					task.compile_unit_id,
					task.source.source_snapshot_id,
					task.source.file_id,
					task.source.size_bytes};
				if (auto decoded = materialization::decode_observation_v2_row(claim.row, authority);
					decoded)
				{
					exact_equivalence = decoded->exact_equivalence;
					if (decoded->limitation)
						limitation_digest = sdk::content_digest(std::as_bytes(
							std::span{decoded->limitation->data(), decoded->limitation->size()}));
				}
			}
			return object(
				{{"row_digest", text(row_digest)},
				 {"row_canonical_form", text(canonical)},
				 {"worker_assertion_claim_ref",
				  text(claim_envelope_ref(claim, "hidden_precursor"))},
				 {"final_relation_compile_unit_id", text(task.compile_unit_id)},
				 {"originating_task", context},
				 {"primary_span_bundle_digest",
				  span_digest ? text(*span_digest) : json_value::null()},
				 {"exact_equivalence",
				  observation && exact_equivalence ? json_value::boolean(*exact_equivalence)
												   : json_value::null()},
				 {"limitation_digest",
				  limitation_digest ? text(*limitation_digest) : json_value::null()}});
		}

		[[nodiscard]] json_value batch_value(const provider_detail::sealed_provider_batch& batch,
											 const provider_task_v4_task_authority& task,
											 const materializer_store_execution& execution)
		{
			const auto claims = reference_claims_for_descriptor(execution, batch.descriptor_id());
			std::vector<json_value> rows;
			rows.reserve(batch.rows().size());
			std::vector<json_value> row_equivalence;
			row_equivalence.reserve(batch.rows().size());
			for (std::size_t index{}; index < batch.rows().size(); ++index)
			{
				if (index < claims.size())
				{
					const bool observation = batch.descriptor_id().starts_with("frontend.clang22.");
					auto row = row_binding_value(*claims[index], task, observation);
					const auto row_digest = row.member("row_digest");
					const auto exact_value = row.member("exact_equivalence") != nullptr
						? *row.member("exact_equivalence")
						: json_value::null();
					const auto limitation_value = row.member("limitation_digest") != nullptr
						? *row.member("limitation_digest")
						: json_value::null();
					rows.push_back(std::move(row));
					if (observation)
					{
						json_value limitation = json_value::null();
						const materialization::observation_v2_task_authority authority{
							task.compile_unit_id,
							task.source.source_snapshot_id,
							task.source.file_id,
							task.source.size_bytes};
						if (auto decoded = materialization::decode_observation_v2_row(
								claims[index]->row, authority);
							decoded && decoded->limitation)
							limitation = text(*decoded->limitation);
						row_equivalence.push_back(
							object({{"observation_row_digest", *row_digest},
									{"final_relation_compile_unit_id", text(task.compile_unit_id)},
									{"originating_task", task_context_value(task)},
									{"exact_equivalence", exact_value},
									{"limitation", limitation},
									{"limitation_digest", limitation_value}}));
					}
				}
			}
			std::vector<json_value> chunks;
			chunks.reserve(batch.ordered_chunk_digests().size());
			for (const auto& digest : batch.ordered_chunk_digests())
				chunks.push_back(text(digest));
			std::vector<json_value> claim_refs;
			std::vector<json_value> claim_contents;
			std::vector<json_value> provenance;
			for (const auto* claim : claims)
			{
				claim_refs.push_back(text(claim_envelope_ref(*claim, "hidden_precursor")));
				claim_contents.push_back(text(claim->content));
				auto digest = semantic_projection_digest(
					"cxxlens.clang22-fixture-provenance-edge.v2",
					object({{"descriptor_id", text(batch.descriptor_id())},
							{"originating_task", task_context_value(task)},
							{"row_digest", text(claim_row_digest(*claim))}}));
				provenance.push_back(text(digest.value()));
			}
			std::ranges::sort(claim_refs,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			std::ranges::sort(claim_contents,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			claim_contents.erase(std::unique(claim_contents.begin(),
											 claim_contents.end(),
											 [](const auto& left, const auto& right)
											 {
												 return *left.as_string() == *right.as_string();
											 }),
								 claim_contents.end());
			std::ranges::sort(provenance,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			const bool observation = batch.descriptor_id().starts_with("frontend.clang22.");
			std::ranges::sort(rows,
							  [](const auto& left, const auto& right)
							  {
								  const auto* left_digest = left.member("row_digest");
								  const auto* right_digest = right.member("row_digest");
								  return left_digest != nullptr && right_digest != nullptr &&
									  *left_digest->as_string() < *right_digest->as_string();
							  });
			std::ranges::sort(row_equivalence,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			auto set_digest = semantic_projection_digest(
				"cxxlens.clang22-batch-row-binding-set.v1",
				object({{"task_execution_key", task_execution_key_value(task)},
						{"descriptor_id", text(batch.descriptor_id())},
						{"row_bindings", array(rows)}}));
			auto ordered_digest = semantic_projection_digest(
				"cxxlens.clang22-ordered-chunk-set.v1",
				object({{"task_execution_key", task_execution_key_value(task)},
						{"descriptor_id", text(batch.descriptor_id())},
						{"ordered_chunk_digests", array(chunks)}}));
			auto claim_digest = semantic_projection_digest(
				"cxxlens.clang22-batch-claim-content-set.v2",
				object({{"task_execution_key", task_execution_key_value(task)},
						{"descriptor_id", text(batch.descriptor_id())},
						{"claim_content_ids", array(claim_contents)}}));
			auto occurrence_digest = semantic_projection_digest(
				"cxxlens.clang22-batch-sdk-occurrence-set.v1",
				object({{"task_execution_key", task_execution_key_value(task)},
						{"descriptor_id", text(batch.descriptor_id())},
						{"worker_assertion_claim_refs", array(claim_refs)}}));
			auto provenance_digest = semantic_projection_digest(
				"cxxlens.clang22-batch-provenance-edge-set.v1",
				object({{"task_execution_key", task_execution_key_value(task)},
						{"descriptor_id", text(batch.descriptor_id())},
						{"provenance_edge_digests", array(provenance)}}));
			auto observation_value = observation ? std::optional<json_value>{observation_census(
													   batch.descriptor_id(), row_equivalence)}
												 : std::nullopt;
			const auto claim_content_count = claim_contents.size();
			json_value::object_type fields;
			fields.emplace("batch_id", text(batch.batch_id()));
			fields.emplace("descriptor_id", text(batch.descriptor_id()));
			fields.emplace("runtime_descriptor_digest", text(batch.descriptor_digest()));
			fields.emplace("dependency_group_id", text(batch.dependency_group_id()));
			fields.emplace("atomic_output_group_id", text(batch.atomic_output_group_id()));
			fields.emplace("row_count", json_value::unsigned_integer(batch.rows().size()));
			fields.emplace("ordered_chunk_digests", array(std::move(chunks)));
			fields.emplace("ordered_chunk_set_digest", text(ordered_digest.value()));
			fields.emplace("row_bindings", array(std::move(rows)));
			fields.emplace("row_binding_set_digest", text(set_digest.value()));
			fields.emplace("worker_assertion_claim_refs", array(std::move(claim_refs)));
			fields.emplace("worker_assertion_claim_occurrence_count",
						   json_value::unsigned_integer(claims.size()));
			fields.emplace("claim_content_ids", array(std::move(claim_contents)));
			fields.emplace("claim_content_count",
						   json_value::unsigned_integer(claim_content_count));
			fields.emplace("claim_content_set_digest", text(claim_digest.value()));
			fields.emplace("sdk_claim_occurrence_set_digest", text(occurrence_digest.value()));
			fields.emplace("provenance_edge_digests", array(std::move(provenance)));
			fields.emplace("provenance_edge_set_digest", text(provenance_digest.value()));
			if (observation_value)
				fields.emplace("observation_equivalence_census", std::move(*observation_value));
			json_value::object_type batch_projection;
			batch_projection.emplace("batch_id", text(batch.batch_id()));
			batch_projection.emplace("descriptor_id", text(batch.descriptor_id()));
			batch_projection.emplace("runtime_descriptor_digest", text(batch.descriptor_digest()));
			batch_projection.emplace("dependency_group_id", text(batch.dependency_group_id()));
			batch_projection.emplace("atomic_output_group_id",
									 text(batch.atomic_output_group_id()));
			batch_projection.emplace("row_count",
									 json_value::unsigned_integer(batch.rows().size()));
			batch_projection.emplace("ordered_chunk_set_digest", text(ordered_digest.value()));
			batch_projection.emplace("row_binding_set_digest", text(set_digest.value()));
			batch_projection.emplace("worker_assertion_claim_occurrence_count",
									 json_value::unsigned_integer(claims.size()));
			batch_projection.emplace("claim_content_count",
									 json_value::unsigned_integer(claim_content_count));
			batch_projection.emplace("claim_content_set_digest", text(claim_digest.value()));
			batch_projection.emplace("sdk_claim_occurrence_set_digest",
									 text(occurrence_digest.value()));
			batch_projection.emplace("provenance_edge_set_digest", text(provenance_digest.value()));
			batch_projection.emplace("sealed", json_value::boolean(true));
			batch_projection.emplace("task_execution_key", task_execution_key_value(task));
			if (observation_value)
				batch_projection.emplace("observation_equivalence_census", *observation_value);
			auto batch_identity =
				semantic_projection_digest("cxxlens.clang22-batch-result.v1",
										   json_value::object(std::move(batch_projection)).value());
			fields.emplace("batch_digest", text(batch_identity.value()));
			fields.emplace("sealed", json_value::boolean(true));
			return json_value::object(std::move(fields)).value();
		}

		[[nodiscard]] json_value
		coverage_for_task(const provider_detail::sealed_provider_transcript& sealed,
						  const provider_task_v4_task_authority& task)
		{
			(void)sealed;
			std::vector<json_value> transport{object({{"kind", text("task")},
													  {"id", text(task.provider_task_id)},
													  {"state", text("covered")},
													  {"reason", text("")}})};
			std::vector<json_value> semantic;
			for (const auto kind :
				 {"cc.call-extraction", "cc.entity", "frontend.clang22.observation"})
				semantic.push_back(object({{"kind", text(kind)},
										   {"id", text(task.provider_task_id)},
										   {"state", text("covered")},
										   {"reason", text("")}}));
			const auto transport_digest = semantic_projection_digest(
				"cxxlens.clang22-task-transport-coverage.v1",
				object({{"semantic_task_key", semantic_result_key_value(task)},
						{"records", array(transport)}}));
			const auto semantic_digest = semantic_projection_digest(
				"cxxlens.clang22-task-semantic-coverage.v1",
				object({{"semantic_task_key", semantic_result_key_value(task)},
						{"records", array(semantic)}}));
			return object({{"transport_records", array(std::move(transport))},
						   {"transport_record_set_digest", text(transport_digest.value())},
						   {"semantic_records", array(std::move(semantic))},
						   {"semantic_record_set_digest", text(semantic_digest.value())}});
		}

		[[nodiscard]] json_value
		groups_value(const std::vector<provider_detail::sealed_provider_batch>& batches,
					 const provider_task_v4_task_authority& task,
					 const materializer_store_execution& execution)
		{
			std::vector<json_value> groups;
			for (const auto& group : task.dependency_groups)
			{
				std::vector<json_value> descriptors;
				std::vector<json_value> summaries;
				for (const auto& batch : batches)
					if (batch.dependency_group_id() == group)
					{
						descriptors.push_back(text(batch.descriptor_id()));
						const auto encoded = batch_value(batch, task, execution);
						summaries.push_back(object(
							{{"descriptor_id", *encoded.member("descriptor_id")},
							 {"batch_digest", *encoded.member("batch_digest")},
							 {"ordered_chunk_set_digest",
							  *encoded.member("ordered_chunk_set_digest")},
							 {"row_count", *encoded.member("row_count")},
							 {"row_binding_set_digest", *encoded.member("row_binding_set_digest")},
							 {"worker_assertion_claim_occurrence_count",
							  *encoded.member("worker_assertion_claim_occurrence_count")},
							 {"claim_content_count", *encoded.member("claim_content_count")},
							 {"claim_content_set_digest",
							  *encoded.member("claim_content_set_digest")},
							 {"sdk_claim_occurrence_set_digest",
							  *encoded.member("sdk_claim_occurrence_set_digest")},
							 {"provenance_edge_set_digest",
							  *encoded.member("provenance_edge_set_digest")}}));
					}
				const auto group_batch_digest = semantic_projection_digest(
					"cxxlens.clang22-group-batch-set.v1",
					object({{"task_execution_key", task_execution_key_value(task)},
							{"dependency_group_id", text(group)},
							{"atomic_output_group_id", text("clang22-atomic")},
							{"descriptor_ids", array(descriptors)},
							{"sealed", json_value::boolean(true)},
							{"batches", array(summaries)}}));
				groups.push_back(object({{"dependency_group_id", text(group)},
										 {"atomic_output_group_id", text("clang22-atomic")},
										 {"descriptor_ids", array(std::move(descriptors))},
										 {"sealed", json_value::boolean(true)},
										 {"batch_set_digest", text(group_batch_digest.value())}}));
			}
			return array(std::move(groups));
		}

		[[nodiscard]] json_value
		task_evidence_records(const provider_task_v4_task_authority& task,
							  const provider_detail::sealed_provider_transcript& sealed)
		{
			// These are product-side evidence edges, not repository-operation receipts.  They
			// describe the three independently retained boundaries of a successful task:
			// canonical adoption, provider execution, and source observation.
			std::vector<json_value> records;
			for (const auto& item : sealed.evidence())
				records.push_back(object({{"kind", text(item.kind)},
										  {"subject", text(item.subject)},
										  {"producer", text(item.producer)},
										  {"summary", text(item.summary)}}));
			for (const auto kind : {"canonicalization", "provider_execution", "source_observation"})
				records.push_back(object({{"kind", text(kind)},
										  {"subject", text(task.compile_unit_id)},
										  {"producer", text(task.provider_task_id)},
										  {"summary", text(std::string{kind} + "-retained")}}));
			return array(std::move(records));
		}

		[[nodiscard]] json_value task_unresolved_records()
		{
			// A sealed successful transcript has no unresolved items.  Keep the typed empty
			// collection in the digest projection so absence is explicit and deterministic.
			return array({});
		}

		[[nodiscard]] sdk::result<std::string>
		task_unresolved_digest(const provider_task_v4_task_authority& task)
		{
			return semantic_projection_digest(
				"cxxlens.clang22-task-unresolved.v1",
				object({{"originating_task", task_context_value(task)},
						{"records", task_unresolved_records()}}));
		}

		[[nodiscard]] sdk::result<std::string>
		task_evidence_digest(const provider_task_v4_task_authority& task,
							 const provider_detail::sealed_provider_transcript& sealed)
		{
			return semantic_projection_digest(
				"cxxlens.clang22-task-evidence.v1",
				object({{"originating_task", task_context_value(task)},
						{"records", task_evidence_records(task, sealed)}}));
		}

		[[nodiscard]] json_value task_component_rows(const std::vector<json_value>& task_results,
													 const std::string_view component)
		{
			std::vector<json_value> rows;
			rows.reserve(task_results.size());
			for (const auto& result : task_results)
			{
				const auto* component_value = result.member("side_channel_components");
				if (component_value == nullptr)
					continue;
				const auto* digest = component_value->member(component);
				if (digest == nullptr)
					continue;
				rows.push_back(object(
					{{"semantic_task_key",
					  array({text(*result.member("provider_task_id")->as_string()),
							 text(*result.member("task_input_digest")->as_string()),
							 text(*result.member("selected_catalog_compile_unit_id")->as_string()),
							 text(*result.member("compile_unit_id")->as_string())})},
					 {"component_digest", *digest}}));
			}
			std::ranges::sort(rows,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			return array(std::move(rows));
		}

		[[nodiscard]] sdk::result<json_value>
		global_coverage_summary(const std::vector<json_value>& task_results,
								const std::string_view plane)
		{
			const auto records_name = std::string{plane} + "_records";
			std::vector<json_value> records;
			for (const auto& result : task_results)
			{
				const auto* coverage = result.member("coverage");
				if (coverage == nullptr)
					return sdk::unexpected(failure("coverage", "task-result"));
				const auto* values = coverage->member(records_name);
				if (values == nullptr || values->as_array() == nullptr)
					return sdk::unexpected(failure("coverage", records_name));
				records.insert(
					records.end(), values->as_array()->begin(), values->as_array()->end());
			}
			std::map<std::string, std::uint64_t> counts{{"covered", 0U},
														{"excluded", 0U},
														{"not_applicable", 0U},
														{"failed", 0U},
														{"unresolved", 0U},
														{"unsupported", 0U},
														{"stale", 0U},
														{"truncated", 0U}};
			for (const auto& record : records)
			{
				const auto* state = record.member("state");
				if (state == nullptr || state->as_string() == nullptr)
					return sdk::unexpected(failure("coverage", "state"));
				++counts[*state->as_string()];
			}
			const auto record_type = plane == "transport"
				? std::string_view{"typed-transport-coverage-unit"}
				: std::string_view{"typed-coverage-unit"};
			const auto channel = plane == "transport" ? std::string_view{"transport_coverage"}
													  : std::string_view{"coverage"};
			auto summary = object(
				{{"record_type", text(record_type)},
				 {"record_count", json_value::unsigned_integer(records.size())},
				 {"state_counts",
				  object(
					  {{"covered", json_value::unsigned_integer(counts["covered"])},
					   {"excluded", json_value::unsigned_integer(counts["excluded"])},
					   {"not_applicable", json_value::unsigned_integer(counts["not_applicable"])},
					   {"failed", json_value::unsigned_integer(counts["failed"])},
					   {"unresolved", json_value::unsigned_integer(counts["unresolved"])},
					   {"unsupported", json_value::unsigned_integer(counts["unsupported"])},
					   {"stale", json_value::unsigned_integer(counts["stale"])},
					   {"truncated", json_value::unsigned_integer(counts["truncated"])}})},
				 {"balance", text("exact")},
				 {"digest",
				  empty_projection_digest("cxxlens.clang22-task-transport-coverage.v1")}});
			const auto domain = channel == "transport_coverage"
				? std::string_view{"cxxlens.clang22-global-transport-coverage.v1"}
				: std::string_view{"cxxlens.clang22-global-coverage.v1"};
			auto projection = object(
				{{"record_type", *summary.member("record_type")},
				 {"record_count", *summary.member("record_count")},
				 {"state_counts", *summary.member("state_counts")},
				 {"balance", *summary.member("balance")},
				 {"task_components",
				  task_component_rows(task_results,
									  plane == "transport" ? "transport_coverage_set_digest"
														   : "semantic_coverage_set_digest")}});
			auto digest = semantic_projection_digest(domain, projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto fields = *summary.as_object();
			fields.erase("digest");
			fields.emplace("digest", text(*digest));
			return json_value::object(std::move(fields));
		}

		[[nodiscard]] sdk::result<json_value>
		global_unresolved_summary(const std::vector<json_value>& task_results)
		{
			auto summary = object(
				{{"record_type", text("typed-unresolved-item")},
				 {"record_count", json_value::unsigned_integer(0U)},
				 {"blocking_count", json_value::unsigned_integer(0U)},
				 {"categories", array({})},
				 {"category_counts", object({})},
				 {"digest", empty_projection_digest("cxxlens.clang22-task-semantic-coverage.v1")}});
			auto projection = object(
				{{"record_type", *summary.member("record_type")},
				 {"record_count", *summary.member("record_count")},
				 {"blocking_count", *summary.member("blocking_count")},
				 {"categories", *summary.member("categories")},
				 {"category_counts", *summary.member("category_counts")},
				 {"task_components", task_component_rows(task_results, "unresolved_set_digest")}});
			auto digest =
				semantic_projection_digest("cxxlens.clang22-global-unresolved.v1", projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto fields = *summary.as_object();
			fields.erase("digest");
			fields.emplace("digest", text(*digest));
			return json_value::object(std::move(fields));
		}

		[[nodiscard]] sdk::result<json_value>
		global_evidence_summary(const std::vector<json_value>& task_results,
								const provider_task_v4_task_authority& task)
		{
			// Each of the three retained edge kinds has one provider and one
			// materializer occurrence in the aggregate census.
			constexpr std::uint64_t record_count = 6U;
			auto summary =
				object({{"record_type", text("typed-evidence-edge")},
						{"record_count", json_value::unsigned_integer(record_count)},
						{"kinds",
						 array({text("canonicalization"),
								text("provider_execution"),
								text("source_observation")})},
						{"kind_counts",
						 object({{"canonicalization", json_value::unsigned_integer(2U)},
								 {"provider_execution", json_value::unsigned_integer(2U)},
								 {"source_observation", json_value::unsigned_integer(2U)}})},
						{"subject_binding", text("exact-claim-or-task-identity")},
						{"digest", empty_projection_digest("cxxlens.clang22-task-unresolved.v1")}});
			auto projection = object(
				{{"record_type", *summary.member("record_type")},
				 {"record_count", *summary.member("record_count")},
				 {"kinds", *summary.member("kinds")},
				 {"kind_counts", *summary.member("kind_counts")},
				 {"subject_binding", *summary.member("subject_binding")},
				 {"task_components", task_component_rows(task_results, "evidence_set_digest")}});
			auto digest =
				semantic_projection_digest("cxxlens.clang22-global-evidence.v1", projection);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			auto fields = *summary.as_object();
			fields.erase("digest");
			fields.emplace("digest", text(*digest));
			(void)task;
			return json_value::object(std::move(fields));
		}

		[[nodiscard]] sdk::result<std::string>
		task_guarantee_fragment_digest(const provider_task_v4_task_authority& task,
									   const json_value& task_result,
									   const json_value& components)
		{
			std::vector<json_value> censuses;
			const auto* batches = task_result.member("batches");
			if (batches == nullptr || batches->as_array() == nullptr)
				return sdk::unexpected(failure("guarantee", "batches"));
			for (const auto& batch : *batches->as_array())
			{
				const auto* descriptor = batch.member("descriptor_id");
				if (descriptor == nullptr || descriptor->as_string() == nullptr)
					return sdk::unexpected(failure("guarantee", "descriptor"));
				if (!descriptor->as_string()->starts_with("frontend.clang22."))
					continue;
				censuses.push_back(
					object({{"descriptor_id", *descriptor},
							{"census", *batch.member("observation_equivalence_census")}}));
			}
			std::ranges::sort(censuses,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			auto groups = std::vector<json_value>{};
			if (const auto* group_values = task_result.member("groups");
				group_values != nullptr && group_values->as_array() != nullptr)
				for (const auto& group : *group_values->as_array())
					groups.push_back(
						object({{"dependency_group_id", *group.member("dependency_group_id")},
								{"atomic_output_group_id", *group.member("atomic_output_group_id")},
								{"descriptor_ids", *group.member("descriptor_ids")},
								{"sealed", *group.member("sealed")}}));
			std::ranges::sort(groups,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			return semantic_projection_digest(
				"cxxlens.clang22-task-guarantee-fragment.v1",
				object({{"semantic_task_key", semantic_result_key_value(task)},
						{"profile_id", *components.member("guarantee_profile_id")},
						{"profile_digest", *components.member("guarantee_profile_digest")},
						{"semantic_coverage_set_digest",
						 *components.member("semantic_coverage_set_digest")},
						{"unresolved_set_digest", *components.member("unresolved_set_digest")},
						{"evidence_set_digest", *components.member("evidence_set_digest")},
						{"groups", array(std::move(groups))},
						{"observation_censuses", array(std::move(censuses))}}));
		}

		[[maybe_unused]] [[nodiscard]] json_value
		guarantee_value(const provider_task_v4_task_authority& task,
						const std::vector<json_value>& task_results)
		{
			std::vector<json_value> modality{text("clang22.materialization-sealed.v1"),
											 text("provider.transcript-sealed.v1"),
											 text("sdk.claim-envelope-validated.v1")};
			std::vector<json_value> censuses;
			for (const auto descriptor : {"frontend.clang22.call_observation.v2",
										  "frontend.clang22.entity_observation.v2",
										  "frontend.clang22.type_observation.v2"})
			{
				std::vector<json_value> rows;
				for (const auto& result : task_results)
					if (const auto* batches = result.member("batches"); batches != nullptr)
						for (const auto& batch : *batches->as_array())
							if (batch.member("descriptor_id") != nullptr &&
								*batch.member("descriptor_id")->as_string() == descriptor)
								if (const auto* census =
										batch.member("observation_equivalence_census");
									census != nullptr)
									rows.insert(rows.end(),
												census->member("rows")->as_array()->begin(),
												census->member("rows")->as_array()->end());
				std::ranges::sort(rows,
								  [](const auto& left, const auto& right)
								  {
									  return materialization::canonical_json(left) <
										  materialization::canonical_json(right);
								  });
				const auto row_digest = semantic_projection_digest(
					"cxxlens.clang22-observation-equivalence-set.v1",
					object({{"descriptor_id", text(descriptor)}, {"rows", array(rows)}}));
				std::uint64_t exact_count{};
				for (const auto& row : rows)
					if (*row.member("exact_equivalence")->as_boolean())
						++exact_count;
				censuses.push_back(
					object({{"descriptor_id", text(descriptor)},
							{"exact_equivalence_count", json_value::unsigned_integer(exact_count)},
							{"non_exact_equivalence_count",
							 json_value::unsigned_integer(rows.size() - exact_count)},
							{"row_equivalence_set_digest", text(row_digest.value())}}));
			}
			const auto profile = object(
				{{"profile_id", text("cxxlens.clang22-materialization-guarantee-profile.v1")},
				 {"materialization_contract_version", text("2.2.0")},
				 {"assumptions", array({})},
				 {"verification_modalities", array(modality)}});
			auto profile_digest = semantic_projection_digest(
				"cxxlens.clang22-materialization-guarantee-profile.v1", profile);
			auto guarantee = object(
				{{"record_type", text("typed-guarantee")},
				 {"profile_id", text("cxxlens.clang22-materialization-guarantee-profile.v1")},
				 {"profile_digest", text(profile_digest.value())},
				 {"approximation", text("exact")},
				 {"scope", text(task.project_id)},
				 {"assumptions", array({})},
				 {"verification_modalities", array(std::move(modality))},
				 {"observation_descriptor_censuses", array(std::move(censuses))},
				 {"digest", empty_projection_digest("cxxlens.clang22-global-evidence.v1")}});
			return guarantee;
		}

		[[maybe_unused]] [[nodiscard]] json_value
		unresolved_value(const provider_detail::sealed_provider_transcript& sealed)
		{
			std::map<std::string, std::uint64_t> counts;
			std::vector<json_value> records;
			for (const auto& item : sealed.unresolved())
			{
				records.push_back(object({{"code", text(item.code)},
										  {"subject", text(item.subject)},
										  {"detail", text(item.detail)}}));
				++counts["custom"];
			}
			std::vector<json_value> categories;
			if (!counts.empty())
			{
				categories.push_back(text("custom"));
			}
			json_value::object_type count_object;
			if (!counts.empty())
				count_object.emplace("custom", json_value::unsigned_integer(counts["custom"]));
			auto digest =
				semantic_projection_digest("cxxlens.clang22-unresolved-set.v1", array(records));
			return object({{"record_type", text("typed-unresolved-item")},
						   {"record_count", json_value::unsigned_integer(records.size())},
						   {"blocking_count", json_value::unsigned_integer(records.size())},
						   {"categories", array(std::move(categories))},
						   {"category_counts", json_value::object(std::move(count_object)).value()},
						   {"digest", text(digest.value())}});
		}

		[[maybe_unused]] [[nodiscard]] json_value
		evidence_value(const provider_detail::sealed_provider_transcript& sealed,
					   const provider_task_v4_task_authority& task)
		{
			std::map<std::string, std::uint64_t> counts;
			std::vector<json_value> records;
			for (const auto& item : sealed.evidence())
			{
				++counts[item.kind == "provider.clang22.execution" ? "provider_execution"
																   : "verification"];
				records.push_back(object({{"kind", text(item.kind)},
										  {"subject", text(item.subject)},
										  {"producer", text(item.producer)},
										  {"summary", text(item.summary)}}));
			}
			std::vector<json_value> kinds;
			json_value::object_type kind_counts;
			for (const auto& [kind, count] : counts)
			{
				kinds.push_back(text(kind));
				kind_counts.emplace(kind, json_value::unsigned_integer(count));
			}
			std::ranges::sort(records,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			auto digest = semantic_projection_digest(
				"cxxlens.clang22-evidence-value.v1",
				object({{"task_id", text(task.provider_task_id)}, {"records", array(records)}}));
			return object({{"record_type", text("typed-evidence-edge")},
						   {"record_count", json_value::unsigned_integer(sealed.evidence().size())},
						   {"kinds", array(std::move(kinds))},
						   {"kind_counts", json_value::object(std::move(kind_counts)).value()},
						   {"subject_binding", text("exact-claim-or-task-identity")},
						   {"digest", text(digest.value())}});
		}

		[[nodiscard]] json_value claim_stage_value(const std::string_view descriptor,
												   const std::vector<const sdk::claim*>& claims,
												   const json_value& guarantee_digest,
												   const json_value& store,
												   const std::vector<json_value>& task_results)
		{
			std::vector<json_value> contents;
			std::vector<json_value> refs;
			std::vector<json_value> associations;
			std::vector<json_value> provenance;
			for (const auto* claim : claims)
			{
				contents.push_back(text(claim->content));
				refs.push_back(text(claim_envelope_ref(*claim, "stored_final")));
				provenance.push_back(text(claim->provenance_root));
			}
			std::ranges::sort(contents,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			contents.erase(std::unique(contents.begin(),
									   contents.end(),
									   [](const auto& left, const auto& right)
									   {
										   return *left.as_string() == *right.as_string();
									   }),
						   contents.end());
			std::ranges::sort(refs,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			std::ranges::sort(provenance,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			if (const auto* values = store.member("origin_associations");
				values != nullptr && values->as_array() != nullptr)
				for (const auto& association : *values->as_array())
				{
					const auto* stored = association.member("stored_claim_ref");
					const auto* id = association.member("association_id");
					if (stored != nullptr && stored->as_string() != nullptr && id != nullptr &&
						id->as_string() != nullptr &&
						std::ranges::find_if(refs,
											 [&](const auto& value)
											 {
												 return value.as_string() != nullptr &&
													 *value.as_string() == *stored->as_string();
											 }) != refs.end())
						associations.push_back(*id);
				}
			std::ranges::sort(associations,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			const auto stage = descriptor.starts_with("frontend.clang22.")
				? std::string_view{"assertion"}
				: std::string_view{"canonical_claim"};
			auto stage_value = object(
				{{"descriptor_id", text(descriptor)},
				 {"stage", text(stage)},
				 {"claim_content_ids", array(contents)},
				 {"claim_content_count", json_value::unsigned_integer(contents.size())},
				 {"stored_claim_refs", array(refs)},
				 {"sdk_claim_occurrence_count", json_value::unsigned_integer(refs.size())},
				 {"origin_association_ids", array(associations)},
				 {"origin_association_count", json_value::unsigned_integer(associations.size())},
				 {"claim_content_set_digest",
				  empty_projection_digest("cxxlens.clang22-claim-stage-content-set.v1")},
				 {"sdk_claim_occurrence_set_digest",
				  empty_projection_digest("cxxlens.clang22-claim-stage-sdk-occurrence-set.v1")},
				 {"origin_association_set_digest",
				  empty_projection_digest("cxxlens.clang22-claim-stage-origin-association-set.v1")},
				 {"provenance_edge_set_digest",
				  empty_projection_digest("cxxlens.clang22-claim-stage-provenance-set.v1")},
				 {"guarantee_digest", guarantee_digest}});
			auto fields = *stage_value.as_object();
			const auto digest_for = [&](const std::string_view domain, const json_value& projection)
			{
				return semantic_projection_digest(domain, projection).value();
			};
			fields.erase("claim_content_set_digest");
			fields.emplace("claim_content_set_digest",
						   text(digest_for("cxxlens.clang22-claim-stage-content-set.v1",
										   object({{"descriptor_id", text(descriptor)},
												   {"claim_content_ids",
													*stage_value.member("claim_content_ids")}}))));
			fields.erase("sdk_claim_occurrence_set_digest");
			fields.emplace("sdk_claim_occurrence_set_digest",
						   text(digest_for("cxxlens.clang22-claim-stage-sdk-occurrence-set.v1",
										   object({{"descriptor_id", text(descriptor)},
												   {"stored_claim_refs",
													*stage_value.member("stored_claim_refs")}}))));
			fields.erase("origin_association_set_digest");
			fields.emplace(
				"origin_association_set_digest",
				text(digest_for("cxxlens.clang22-claim-stage-origin-association-set.v1",
								object({{"descriptor_id", text(descriptor)},
										{"origin_association_ids",
										 *stage_value.member("origin_association_ids")}}))));
			fields.erase("provenance_edge_set_digest");
			fields.emplace(
				"provenance_edge_set_digest",
				text(digest_for("cxxlens.clang22-claim-stage-provenance-set.v1",
								object({{"descriptor_id", text(descriptor)},
										{"provenance_roots", array(std::move(provenance))}}))));
			if (descriptor.starts_with("frontend.clang22."))
				for (const auto& result : task_results)
					if (const auto* batches = result.member("batches");
						batches != nullptr && batches->as_array() != nullptr)
						for (const auto& batch : *batches->as_array())
							if (const auto* id = batch.member("descriptor_id"); id != nullptr &&
								id->as_string() != nullptr && *id->as_string() == descriptor)
								if (const auto* census =
										batch.member("observation_equivalence_census");
									census != nullptr)
									fields.emplace("observation_equivalence_census", *census);
			auto without_digest = json_value::object(fields).value();
			auto stage_digest =
				semantic_projection_digest("cxxlens.clang22-claim-stage.v1", without_digest);
			if (stage_digest)
				fields.emplace("claim_stage_digest", text(*stage_digest));
			return json_value::object(std::move(fields)).value();
		}

		[[nodiscard]] json_value
		store_partition_value(const sdk::partition_manifest& manifest,
							  const sdk::snapshot_partition_binding* binding,
							  const std::vector<const sdk::claim*>& claims,
							  const provider_task_v4_task_authority& task)
		{
			std::vector<json_value> refs;
			std::vector<json_value> contents;
			for (const auto* claim : claims)
			{
				refs.push_back(text(claim_envelope_ref(*claim, "stored_final")));
				contents.push_back(text(claim->content));
			}
			std::ranges::sort(refs,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			refs.erase(std::unique(refs.begin(), refs.end()), refs.end());
			std::ranges::sort(contents,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			contents.erase(std::unique(contents.begin(),
									   contents.end(),
									   [](const auto& left, const auto& right)
									   {
										   return *left.as_string() == *right.as_string();
									   }),
						   contents.end());
			const auto& condition = binding != nullptr
				? binding->condition
				: sdk::claim_condition{task.condition_universe_id, {task.condition_id}};
			const auto relation = binding != nullptr ? binding->relation_descriptor_id
													 : manifest.relation_descriptor_id;
			const auto scope = binding != nullptr ? binding->scope : task.project_id;
			const auto interpretation =
				binding != nullptr ? binding->interpretation : task.interpretation_domain;
			const auto producer =
				binding != nullptr ? binding->producer_semantics : task.source.content_digest;
			const auto input_basis = binding != nullptr ? binding->producer_input_basis_digest
														: manifest.input_basis_digest;
			const auto precision = binding != nullptr ? binding->precision_profile : "exact";
			const auto assumptions =
				binding != nullptr ? binding->assumption_set_id : "assumptions:none";
			std::vector<sdk::canonical_value> task_key_fields{
				sdk::canonical_value::from_string(task.provider_task_id),
				sdk::canonical_value::from_string(task.task_input_digest),
				sdk::canonical_value::from_string(task.selected_catalog_compile_unit_id),
				sdk::canonical_value::from_string(task.compile_unit_id),
				sdk::canonical_value::from_string(task.condition_universe_id),
				sdk::canonical_value::from_string(task.condition_id),
				sdk::canonical_value::from_string(task.interpretation_domain)};
			const auto task_key =
				sdk::canonical_identity_digest("materialization-task", task_key_fields).value();
			const bool base = relation.starts_with("build.") || relation.starts_with("source.");
			std::vector<json_value> coverage;
			auto add_coverage = [&](std::string domain, std::string key)
			{
				coverage.push_back(object({{"domain", text(domain)},
										   {"key", text(std::move(key))},
										   {"state", text("covered")},
										   {"reason", text("")}}));
			};
			if (base)
			{
				const auto descriptor_key =
					sdk::canonical_identity_digest(
						"materialization-base-descriptor",
						std::array{sdk::canonical_value::from_string(task_key),
								   sdk::canonical_value::from_string(relation)})
						.value();
				add_coverage("materialization.base-descriptor", descriptor_key);
			}
			else
			{
				add_coverage("materialization.task", task_key);
				const auto group = relation.starts_with("cc.") ? "canonical" : "observation";
				const auto group_key = sdk::canonical_identity_digest(
										   "materialization-dependency-group",
										   std::array{sdk::canonical_value::from_string(task_key),
													  sdk::canonical_value::from_string(group)})
										   .value();
				add_coverage("materialization.dependency-group", group_key);
			}
			std::ranges::sort(coverage,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			return object(
				{{"relation_descriptor_id", text(relation)},
				 {"scope", text(scope)},
				 {"condition", condition_value(condition)},
				 {"interpretation", text(interpretation)},
				 {"producer_semantics", text(producer)},
				 {"producer_input_basis_digest", text(input_basis)},
				 {"precision_profile",
				  text(precision == "under_approximation" ? "exact" : precision)},
				 {"assumption_set_id", text(assumptions)},
				 {"empty_partition", json_value::boolean(claims.empty())},
				 {"stored_claim_refs", array(std::move(refs))},
				 {"claim_content_digests", array(std::move(contents))},
				 {"sdk_claim_occurrence_count", json_value::unsigned_integer(refs.size())},
				 {"origin_association_count", json_value::unsigned_integer(refs.size())},
				 {"coverage_units", array(std::move(coverage))},
				 {"unresolved", array({})},
				 {"partition_id", text(manifest.partition_id)},
				 {"claim_set_digest", text(manifest.claim_set_digest)},
				 {"coverage_digest", text(manifest.coverage_digest)},
				 {"content_digest", text(manifest.content_digest)},
				 {"claim_count", json_value::unsigned_integer(manifest.claim_count)},
				 {"complete", json_value::boolean(manifest.complete)}});
		}

		[[nodiscard]] json_value claim_envelope_value(const sdk::claim& claim)
		{
			std::string basis;
			if (const auto* direct = std::get_if<sdk::direct_claim_basis>(&claim.input_basis))
				basis = direct->basis_digest;
			else if (const auto* derived =
						 std::get_if<sdk::derived_claim_basis>(&claim.input_basis))
				basis = derived->transform_semantics;
			const auto role = claim.stage == sdk::claim_stage::canonical_claim
				? std::string_view{"stored_final"}
				: std::string_view{"hidden_precursor"};
			return object(
				{{"claim_ref", text(claim_envelope_ref(claim, role))},
				 {"role",
				  text(claim.stage == sdk::claim_stage::canonical_claim ? "stored_final"
																		: "hidden_precursor")},
				 {"row_ref", text(claim_row_ref(claim))},
				 {"row_canonical_form", text(claim.row.canonical_form())},
				 {"descriptor_id", text(claim.descriptor)},
				 {"semantic_key", text(claim.semantic_key)},
				 {"assertion", text(claim.assertion)},
				 {"content", text(claim.content)},
				 {"presence", condition_value(claim.presence)},
				 {"interpretation", text(claim.interpretation)},
				 {"stage",
				  text(claim.stage == sdk::claim_stage::canonical_claim ? "canonical_claim"
																		: "assertion")},
				 {"producer",
				  object({{"id", text(claim.producer.id)},
						  {"semantic_contract", text(claim.producer.semantic_contract)}})},
				 {"input_basis", object({{"kind", text("direct")}, {"basis_digest", text(basis)}})},
				 {"provenance_root", text(claim.provenance_root)},
				 {"guarantee",
				  object({{"approximation", text("exact")},
						  {"scope", text(claim.guarantee.scope)},
						  {"assumptions", text(claim.guarantee.assumptions)},
						  {"verification_modalities",
						   array({text("clang22.materialization-sealed.v1"),
								  text("provider.transcript-sealed.v1"),
								  text("sdk.claim-envelope-validated.v1")})}})},
				 {"sdk_singleton_claim_batch_digest",
				  text(
					  sdk::claim_batch_content_digest(std::array<sdk::claim, 1U>{claim}, {}, {}, {})
						  .value())}});
		}

		[[nodiscard]] json_value claim_row_json(const sdk::claim& claim);
		[[nodiscard]] std::string base_row_identity(const std::string_view descriptor,
													const json_value& row);
		[[nodiscard]] std::string base_row_digest(const std::string_view descriptor,
												  const json_value& row);
		[[nodiscard]] std::optional<std::string>
		store_source_evidence_digest(const sdk::claim& claim,
									 const materializer_store_execution& execution,
									 const provider_task_v4_task_authority& task);

		[[nodiscard]] json_value store_value(const materializer_store_execution& execution,
											 const provider_task_v4_task_authority& task)
		{
			const auto& snapshot = execution.publication.snapshot;
			std::vector<json_value> claim_rows;
			std::vector<json_value> envelopes;
			std::vector<json_value> associations;
			std::vector<json_value> final_refs;
			std::vector<json_value> canonicalization_edges;
			std::vector<const sdk::claim*> final_claims;
			for (const auto& partition : execution.base_partitions)
				for (const auto& claim : partition.claims)
					final_claims.push_back(&claim);
			for (const auto& sealed : execution.claims)
				for (const auto& claim : sealed.translation.batch.claims)
					final_claims.push_back(&claim);
			std::vector<sdk::claim> final_values;
			final_values.reserve(final_claims.size());
			for (const auto* claim : final_claims)
				final_values.push_back(*claim);

			// The hidden precursor occurrences are retained in the product Store projection,
			// but never promoted to the public final reference set.
			for (const auto& claim : execution.reference_claims)
			{
				const auto canonical = claim.row.canonical_form();
				claim_rows.push_back(object({{"row_ref", text(claim_row_ref(claim))},
											 {"descriptor_id", text(claim.descriptor)},
											 {"row_canonical_form", text(canonical)}}));
				envelopes.push_back(claim_envelope_value(claim));
			}
			for (const auto* claim : final_claims)
			{
				const auto canonical = claim->row.canonical_form();
				const bool base = claim->descriptor.starts_with("build.") ||
					claim->descriptor.starts_with("source.");
				const auto row = claim_row_json(*claim);
				const auto sealed_row_digest =
					base ? base_row_digest(claim->descriptor, row) : claim_row_digest(*claim);
				const auto source_evidence = store_source_evidence_digest(*claim, execution, task);
				const auto source_evidence_value =
					source_evidence ? text(*source_evidence) : json_value::null();
				claim_rows.push_back(object({{"row_ref", text(claim_row_ref(*claim))},
											 {"descriptor_id", text(claim->descriptor)},
											 {"row_canonical_form", text(canonical)}}));
				envelopes.push_back(claim_envelope_value(*claim));
				final_refs.push_back(text(claim_envelope_ref(*claim, "stored_final")));
				const auto association_id =
					sdk::canonical_identity_digest(
						"materialization-claim-association",
						std::array{
							sdk::canonical_value::from_string(
								claim_envelope_ref(*claim, "stored_final")),
							sdk::canonical_value::from_tuple({
								sdk::canonical_value::from_string(task.provider_task_id),
								sdk::canonical_value::from_string(task.task_input_digest),
								sdk::canonical_value::from_string(
									task.selected_catalog_compile_unit_id),
								sdk::canonical_value::from_string(task.compile_unit_id),
								sdk::canonical_value::from_string(task.condition_universe_id),
								sdk::canonical_value::from_string(task.condition_id),
								sdk::canonical_value::from_string(task.interpretation_domain),
							}),
							sdk::canonical_value::from_string(sealed_row_digest),
							sdk::canonical_value::from_string(
								source_evidence.value_or(std::string{})),
						})
						.value();
				associations.push_back(
					object({{"association_id", text(association_id)},
							{"stored_claim_ref", text(claim_envelope_ref(*claim, "stored_final"))},
							{"originating_task", task_context_value(task)},
							{"sealed_row_digest", text(sealed_row_digest)},
							{"source_evidence_digest", source_evidence_value}}));
				if (claim->stage == sdk::claim_stage::canonical_claim)
				{
					const sdk::claim* precursor = nullptr;
					for (const auto& reference : execution.reference_claims)
						if (reference.descriptor == claim->descriptor &&
							reference.row.canonical_form() == canonical &&
							reference.stage == sdk::claim_stage::assertion)
						{
							precursor = &reference;
							break;
						}
					if (precursor != nullptr)
					{
						const auto transform = claim->descriptor.starts_with("build.") ||
								claim->descriptor.starts_with("source.")
							? execution.claims.front()
								  .translation.binding.base_ingestion_transform_digest
							: execution.claims.front()
								  .translation.binding.canonical_adoption_transform_digest;
						canonicalization_edges.push_back(object(
							{{"precursor_claim_ref",
							  text(claim_envelope_ref(*precursor, "hidden_precursor"))},
							 {"final_claim_ref", text(claim_envelope_ref(*claim, "stored_final"))},
							 {"transform_semantics", text(transform)}}));
					}
				}
			}
			std::ranges::sort(claim_rows,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			claim_rows.erase(std::unique(claim_rows.begin(),
										 claim_rows.end(),
										 [](const auto& left, const auto& right)
										 {
											 return left == right;
										 }),
							 claim_rows.end());
			std::ranges::sort(envelopes,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			std::ranges::sort(final_refs,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			std::ranges::sort(canonicalization_edges,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			std::vector<json_value> partitions;
			for (const auto& manifest : snapshot.manifest().partitions)
			{
				const sdk::snapshot_partition_binding* binding = nullptr;
				for (const auto& candidate : snapshot.partition_bindings())
					if (candidate.partition_id == manifest.partition_id)
					{
						binding = &candidate;
						break;
					}
				partitions.push_back(store_partition_value(
					manifest,
					binding,
					[&]()
					{
						std::vector<const sdk::claim*> claims;
						for (const auto& partition : execution.base_partitions)
							if (partition.relation_descriptor_id == manifest.relation_descriptor_id)
								for (const auto& claim : partition.claims)
									claims.push_back(&claim);
						auto output =
							claims_for_descriptor(execution, manifest.relation_descriptor_id);
						claims.insert(claims.end(), output.begin(), output.end());
						return claims;
					}(),
					task));
			}
			const auto& first = execution.claims.front().translation.binding;
			const auto producer_input_basis =
				sdk::claim_input_basis_digest(sdk::direct_claim_basis{first.direct_basis_digest})
					.value();
			std::vector<json_value> unique_contents;
			unique_contents.reserve(final_values.size());
			for (const auto& claim : final_values)
				unique_contents.push_back(text(claim.content));
			std::ranges::sort(unique_contents,
							  [](const auto& left, const auto& right)
							  {
								  return *left.as_string() < *right.as_string();
							  });
			unique_contents.erase(std::unique(unique_contents.begin(),
											  unique_contents.end(),
											  [](const auto& left, const auto& right)
											  {
												  return *left.as_string() == *right.as_string();
											  }),
								  unique_contents.end());
			return object(
				{{"selector", selector_value(execution.publication.authority.snapshot.series)},
				 {"direct_basis",
				  object({{"projection_version",
						   text("cxxlens.clang22-direct-materialization-basis.v1")},
						  {"materializer_semantics_digest",
						   text(first.materializer_semantic_contract_digest)},
						  {"basis_digest", text(first.direct_basis_digest)},
						  {"producer_input_basis_digest", text(producer_input_basis)},
						  {"canonical_adoption_transform_digest",
						   text(first.canonical_adoption_transform_digest)},
						  {"base_ingestion_transform_digest",
						   text(first.base_ingestion_transform_digest)}})},
				 {"claim_rows", array(std::move(claim_rows))},
				 {"claim_envelopes", array(std::move(envelopes))},
				 {"origin_associations", array(std::move(associations))},
				 {"claim_batch_validation",
				  object(
					  {{"contract", text("cxxlens.claim-batch.v2")},
					   {"final_claim_refs", array(std::move(final_refs))},
					   {"sdk_claim_occurrence_count",
						json_value::unsigned_integer(final_claims.size())},
					   {"unique_claim_content_count",
						json_value::unsigned_integer(unique_contents.size())},
					   {"unresolved_count",
						json_value::unsigned_integer(execution.receipt.unresolved_count)},
					   {"conflict_count",
						json_value::unsigned_integer(execution.receipt.conflict_count)},
					   {"differential_disagreement_count",
						json_value::unsigned_integer(
							execution.receipt.differential_disagreement_count)},
					   {"content_digest",
						text(sdk::claim_batch_content_digest(final_values, {}, {}, {}).value())}})},
				 {"canonicalization_edges", array(std::move(canonicalization_edges))},
				 {"partitions", array(std::move(partitions))},
				 {"snapshot_manifest", snapshot_manifest(snapshot.manifest())}});
		}

		[[nodiscard]] json_value reopened_store_value(const materializer_store_execution& execution,
													  const provider_task_v4_task_authority& task)
		{
			(void)task;
			const auto& snapshot = execution.publication.snapshot;
			std::vector<const sdk::claim*> final_claims;
			for (const auto& partition : execution.base_partitions)
				for (const auto& claim : partition.claims)
					final_claims.push_back(&claim);
			for (const auto& sealed : execution.claims)
				for (const auto& claim : sealed.translation.batch.claims)
					final_claims.push_back(&claim);
			std::vector<json_value> descriptors;
			for (const auto& descriptor :
				 execution.worker.ingress.request.authority.engine.admitted_descriptors)
				descriptors.push_back(object(
					{{"descriptor_id", text(descriptor.descriptor_id)},
					 {"runtime_descriptor_digest", text(descriptor.runtime_descriptor_digest)}}));
			std::ranges::sort(descriptors,
							  [](const auto& left, const auto& right)
							  {
								  return *left.member("descriptor_id")->as_string() <
									  *right.member("descriptor_id")->as_string();
							  });
			std::vector<json_value> bindings;
			for (const auto& binding : snapshot.partition_bindings())
				bindings.push_back(object(
					{{"partition_id", text(binding.partition_id)},
					 {"relation_descriptor_id", text(binding.relation_descriptor_id)},
					 {"scope", text(binding.scope)},
					 {"condition", condition_value(binding.condition)},
					 {"interpretation", text(binding.interpretation)},
					 {"producer_semantics", text(binding.producer_semantics)},
					 {"producer_input_basis_digest", text(binding.producer_input_basis_digest)},
					 {"precision_profile",
					  text(binding.precision_profile == "under_approximation"
							   ? "exact"
							   : binding.precision_profile)},
					 {"assumption_set_id", text(binding.assumption_set_id)}}));
			std::ranges::sort(bindings,
							  [](const auto& left, const auto& right)
							  {
								  return *left.member("partition_id")->as_string() <
									  *right.member("partition_id")->as_string();
							  });
			std::vector<json_value> annotations;
			for (const auto* claim : final_claims)
				annotations.push_back(object(
					{{"relation_descriptor_id", text(claim->descriptor)},
					 {"row_canonical_form", text(claim->row.canonical_form())},
					 {"presence", condition_value(claim->presence)},
					 {"interpretation", text(claim->interpretation)},
					 {"semantic_key", text(claim->semantic_key)},
					 {"assertion", text(claim->assertion)},
					 {"content", text(claim->content)},
					 {"producer",
					  object({{"id", text(claim->producer.id)},
							  {"semantic_contract", text(claim->producer.semantic_contract)}})},
					 {"provenance_root", text(claim->provenance_root)},
					 {"guarantee",
					  object({{"approximation", text("exact")},
							  {"scope", text(claim->guarantee.scope)},
							  {"assumptions", text(claim->guarantee.assumptions)},
							  {"verification_modalities",
							   array({text("clang22.materialization-sealed.v1"),
									  text("provider.transcript-sealed.v1"),
									  text("sdk.claim-envelope-validated.v1")})}})}}));
			std::ranges::sort(annotations,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			std::vector<json_value> coverage;
			for (const auto& unit : snapshot.input_coverage())
				coverage.push_back(
					object({{"relation_descriptor_id", text(unit.relation_descriptor_id)},
							{"unit",
							 object({{"domain", text(unit.unit.domain)},
									 {"key", text(unit.unit.key)},
									 {"state", text("covered")},
									 {"reason", text("")}})}}));
			std::ranges::sort(coverage,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			std::vector<json_value> relations;
			for (const auto& descriptor :
				 execution.worker.ingress.request.authority.engine.admitted_descriptors)
			{
				std::vector<json_value> rows;
				std::vector<json_value> claim_annotations;
				for (const auto* claim : final_claims)
					if (claim->descriptor == descriptor.descriptor_id)
					{
						rows.push_back(text(claim->row.canonical_form()));
						for (const auto& annotation : annotations)
							if (const auto* id = annotation.member("relation_descriptor_id");
								id != nullptr && id->as_string() != nullptr &&
								*id->as_string() == descriptor.descriptor_id &&
								annotation.member("row_canonical_form") != nullptr &&
								*annotation.member("row_canonical_form")->as_string() ==
									claim->row.canonical_form())
								claim_annotations.push_back(annotation);
					}
				std::ranges::sort(rows,
								  [](const auto& left, const auto& right)
								  {
									  return *left.as_string() < *right.as_string();
								  });
				rows.erase(std::unique(rows.begin(), rows.end()), rows.end());
				std::ranges::sort(claim_annotations,
								  [](const auto& left, const auto& right)
								  {
									  return materialization::canonical_json(left) <
										  materialization::canonical_json(right);
								  });
				std::vector<json_value> relation_coverage;
				for (const auto& value : coverage)
					if (const auto* id = value.member("relation_descriptor_id"); id != nullptr &&
						id->as_string() != nullptr && *id->as_string() == descriptor.descriptor_id)
						if (const auto* unit = value.member("unit"); unit != nullptr)
							relation_coverage.push_back(*unit);
				std::ranges::sort(relation_coverage,
								  [](const auto& left, const auto& right)
								  {
									  return materialization::canonical_json(left) <
										  materialization::canonical_json(right);
								  });
				relations.push_back(
					object({{"relation_descriptor_id", text(descriptor.descriptor_id)},
							{"row_canonical_forms", array(std::move(rows))},
							{"claim_annotations", array(std::move(claim_annotations))},
							{"coverage", array(std::move(relation_coverage))}}));
			}
			std::ranges::sort(relations,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			auto cursor_projection =
				object({{"specification", text("cxxlens.clang22-materialization-reopen-cursor.v1")},
						{"relations", array(relations)}});
			auto cursor_digest = semantic_projection_digest(
				"cxxlens.clang22-materialization-reopen-cursor.v1", cursor_projection);
			if (!cursor_digest)
				return json_value::null();
			const auto manifest_json = snapshot_manifest(snapshot.manifest());
			const auto manifest_text = materialization::canonical_json(manifest_json);
			const auto manifest_digest = sdk::content_digest(
				std::as_bytes(std::span{manifest_text.data(), manifest_text.size()}));
			auto partition_bindings_digest = semantic_projection_digest(
				"cxxlens.clang22-reopened-partition-binding-multiset.v1", array(bindings));
			std::vector<json_value> row_multiset;
			for (const auto& relation : relations)
				row_multiset.push_back(array({*relation.member("relation_descriptor_id"),
											  *relation.member("row_canonical_forms")}));
			auto row_multiset_digest = semantic_projection_digest(
				"cxxlens.clang22-reopened-row-multiset.v1", array(std::move(row_multiset)));
			auto annotation_multiset_digest = semantic_projection_digest(
				"cxxlens.clang22-reopened-claim-annotation-multiset.v1", array(annotations));
			auto coverage_multiset_digest = semantic_projection_digest(
				"cxxlens.clang22-reopened-coverage-multiset.v1", array(coverage));
			if (!partition_bindings_digest || !row_multiset_digest || !annotation_multiset_digest ||
				!coverage_multiset_digest)
				return json_value::null();
			const auto record = publication_record(snapshot.publication());
			auto unresolved_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-unresolved.v1", array({}));
			auto closure_digest =
				semantic_projection_digest("cxxlens.clang22-reopened-closure.v1", array({}));
			if (!unresolved_digest || !closure_digest)
				return json_value::null();
			const auto canonical_export = text(execution.canonical_export_digest);
			json_value::object_type semantic_fields;
			semantic_fields.emplace("backend", text(snapshot.physical_backend()));
			semantic_fields.emplace("snapshot_manifest", snapshot_manifest(snapshot.manifest()));
			semantic_fields.emplace("snapshot_manifest_digest", text(manifest_digest));
			semantic_fields.emplace("descriptors", array(descriptors));
			semantic_fields.emplace("partition_binding_multiset_digest",
									text(*partition_bindings_digest));
			semantic_fields.emplace("row_multiset_digest", text(*row_multiset_digest));
			semantic_fields.emplace("claim_annotation_multiset_digest",
									text(*annotation_multiset_digest));
			semantic_fields.emplace("coverage_multiset_digest", text(*coverage_multiset_digest));
			semantic_fields.emplace("unresolved_digest", text(*unresolved_digest));
			semantic_fields.emplace("closure_digest", text(*closure_digest));
			semantic_fields.emplace("cursor_projection_digest", text(*cursor_digest));
			semantic_fields.emplace("canonical_export_digest", canonical_export);
			auto semantic_projection = json_value::object(semantic_fields).value();
			auto semantic = semantic_projection_digest(
				"cxxlens.clang22-reopened-semantic-projection.v1", semantic_projection);
			if (!semantic)
				return json_value::null();
			json_value::object_type projection_fields = std::move(semantic_fields);
			projection_fields.emplace("publication_record", record);
			projection_fields.emplace("semantic_projection_digest", text(*semantic));
			auto projection = json_value::object(std::move(projection_fields)).value();
			auto handle = semantic_projection_digest(
				"cxxlens.clang22-reopened-handle-projection.v1", projection);
			projection_fields = *projection.as_object();
			if (!handle)
				return json_value::null();
			projection_fields.emplace("handle_projection_digest", text(*handle));
			projection = json_value::object(std::move(projection_fields)).value();
			std::vector<json_value> receipts;
			for (const auto path : {"current-selector", "open-publication", "open-snapshot"})
				receipts.push_back(object(
					{{"access_path", text(path)},
					 {"lookup",
					  path == std::string_view{"current-selector"}
						  ? object(
								{{"selector",
								  selector_value(execution.publication.authority.snapshot.series)}})
						  : path == std::string_view{"open-publication"}
						  ? object(
								{{"publication_id", text(snapshot.publication().publication_id)}})
						  : object({{"snapshot_id", text(snapshot.id())}})},
					 {"status", text("present")},
					 {"sdk_code", json_value::null()},
					 {"sdk_field", json_value::null()},
					 {"projection", projection}}));
			return object(
				{{"backend", text(snapshot.physical_backend())},
				 {"selector", selector_value(execution.publication.authority.snapshot.series)},
				 {"publication_record", record},
				 {"snapshot_manifest", snapshot_manifest(snapshot.manifest())},
				 {"descriptors", array(std::move(descriptors))},
				 {"partition_bindings", array(std::move(bindings))},
				 {"claim_annotations", array(std::move(annotations))},
				 {"coverage", array(std::move(coverage))},
				 {"unresolved", array({})},
				 {"canonical_export_digest", canonical_export},
				 {"cursor_projection",
				  object(
					  {{"specification", text("cxxlens.clang22-materialization-reopen-cursor.v1")},
					   {"relations", array(std::move(relations))},
					   {"digest", text(*cursor_digest)}})},
				 {"handle_receipts", array(std::move(receipts))}});
		}

		[[nodiscard]] sdk::result<json_value>
		publication_value(const materializer_store_execution& execution,
						  const json_value& request_root,
						  const json_value& reopened)
		{
			const auto& authority = execution.publication.authority;
			const auto& record = execution.publication.snapshot.publication();
			if (!execution.publication.publication_verified ||
				record.state != sdk::publication_state::committed || record.corrupt)
				return sdk::unexpected(failure("publication", "not-committed"));
			const auto* reopened_record = reopened.member("publication_record");
			if (reopened_record == nullptr ||
				materialization::canonical_json(*reopened_record) !=
					materialization::canonical_json(publication_record(record)))
				return sdk::unexpected(failure("publication", "reopen-binding"));
			const auto identity =
				object({{"publication_id", text(record.publication_id)},
						{"series_id", text(record.series_id)},
						{"snapshot_id", text(record.snapshot_id)},
						{"sequence", json_value::unsigned_integer(record.sequence)},
						{"parent_publication", nullable_id(record.parent_publication)}});
			const auto* publication = member(request_root, "publication");
			const auto backend = publication == nullptr
				? std::string{"memory"}
				: required_string(*publication, "backend").value();
			const auto genesis = !record.parent_publication.has_value();
			const auto observed_parent = execution.observed_parent_record
				? publication_record(*execution.observed_parent_record)
				: json_value::null();
			json_value sqlite_receipt = json_value::null();
			if (backend == "sqlite")
			{
				const auto path = publication == nullptr
					? std::string{}
					: required_string(*publication, "sqlite_path").value();
				if (!execution.sqlite_effect_root_receipt)
					return sdk::unexpected(failure("sqlite_effect_root_receipt", "missing"));
				const auto& rooted = *execution.sqlite_effect_root_receipt;
				if (rooted.exact_relative_path != path || rooted.schema != "rooted-vfs-v1" ||
					rooted.mount_device_inode_observation_digest.empty())
					return sdk::unexpected(failure("sqlite_effect_root_receipt", "binding"));
				sqlite_receipt =
					object({{"contract", text("rooted-vfs-v1")},
							{"root_observation_digest",
							 text(rooted.mount_device_inode_observation_digest)},
							{"relative_path", text(rooted.exact_relative_path)},
							{"parent_resolution", text(rooted.parent_resolution_verdict)},
							{"leaf_resolution", text(rooted.leaf_resolution_verdict)}});
			}
			return object(
				{{"backend", text(backend)},
				 {"selector", selector_fields(authority.snapshot.series)},
				 {"series_id", text(record.series_id)},
				 {"genesis", json_value::boolean(genesis)},
				 {"expected_parent_publication",
				  nullable_id(authority.snapshot.expected_parent_publication)},
				 {"observed_parent_publication", nullable_id(record.parent_publication)},
				 {"observed_parent_record", observed_parent},
				 {"head_observation", text(genesis ? "absent" : "present")},
				 {"publication_attempted", json_value::boolean(true)},
				 {"outcome",
				  text(execution.publication.publication_verified ? "committed_verified"
																  : "committed_unverified")},
				 {"partial_policy", text("forbid")},
				 {"candidate_snapshot_id", text(record.snapshot_id)},
				 {"candidate_identity_state", text("constructed")},
				 {"candidate_identity", identity},
				 {"invocation_commit_state", text("committed")},
				 {"committed_transaction_count", json_value::unsigned_integer(1U)},
				 {"invocation_committed_record", publication_record(record)},
				 {"terminal_head",
				  object({{"status", text("present")}, {"record", publication_record(record)}})},
				 {"candidate_visibility", text("present_by_invocation")},
				 {"prior_history_retained", json_value::boolean(true)},
				 {"head_effect", text("advanced_to_candidate")},
				 {"store_failure", json_value::null()},
				 {"prior_artifact_persistence",
				  backend == "sqlite"
					  ? object({{"state", text("committed")}, {"error", json_value::null()}})
					  : object({{"state", text("unavailable")},
								{"error",
								 object({{"code",
										  text("materialization.incremental-artifact-invalid")},
										 {"field", text("memory")},
										 {"detail", text("process-lifetime-only")}})}})},
				 {"sqlite_effect_root_receipt", sqlite_receipt},
				 {"sqlite_reopen_status", text(backend == "sqlite" ? "opened" : "not_applicable")},
				 {"recovery_receipt", json_value::null()}});
		}

		[[nodiscard]] json_value
		span_validation_value(const materializer_store_execution& execution,
							  const provider_task_v4_task_authority& task)
		{
			const auto observation_authority =
				materialization::observation_v2_task_authority{task.compile_unit_id,
															   task.source.source_snapshot_id,
															   task.source.file_id,
															   task.source.size_bytes};
			std::vector<json_value> bindings;
			std::set<std::string, std::less<>> unique_span_ids;
			std::uint64_t entity_absent{};
			std::uint64_t call_absent{};
			std::uint64_t invalid_range{};
			std::uint64_t task_binding_mismatch{};
			std::vector<json_value> span_rows;

			// The source-bearing observation claims are retained as immutable worker
			// occurrences.  Each occurrence is rebound to exactly one canonical source.span
			// claim produced by the host-side base partition.
			for (const auto& claim : execution.reference_claims)
			{
				const bool entity =
					claim.descriptor == materialization::entity_observation_v2_descriptor().id;
				const bool call =
					claim.descriptor == materialization::call_observation_v2_descriptor().id;
				if (!entity && !call)
					continue;
				auto decoded =
					materialization::decode_observation_v2_row(claim.row, observation_authority);
				if (!decoded)
				{
					++invalid_range;
					continue;
				}
				if (!decoded->primary_span)
				{
					if (entity)
						++entity_absent;
					else
						++call_absent;
					continue;
				}
				const auto& span = *decoded->primary_span;
				const auto bundle = object({{"span_id", text(span.span_id)},
											{"snapshot", text(span.snapshot)},
											{"file", text(span.file)},
											{"begin", json_value::unsigned_integer(span.begin)},
											{"end", json_value::unsigned_integer(span.end)},
											{"role", text(span.role)},
											{"read_only", json_value::boolean(span.read_only)}});
				const auto bundle_digest = primary_span_bundle_digest(claim, task);
				if (!bundle_digest)
				{
					++invalid_range;
					continue;
				}
				const sdk::claim* source_span_claim = nullptr;
				for (const auto& partition : execution.base_partitions)
					if (partition.relation_descriptor_id == "source.span.v1")
						for (const auto& candidate : partition.claims)
						{
							const auto candidate_row = claim_row_json(candidate);
							if (base_row_identity("source.span.v1", candidate_row) == span.span_id)
							{
								source_span_claim = &candidate;
								break;
							}
						}
				if (source_span_claim == nullptr)
				{
					++task_binding_mismatch;
					continue;
				}
				unique_span_ids.insert(span.span_id);
				bindings.push_back(object(
					{{"bundle", bundle},
					 {"bundle_digest", text(*bundle_digest)},
					 {"row_digest",
					  text(base_row_digest("source.span.v1", claim_row_json(*source_span_claim)))},
					 {"observation_descriptor_id", text(claim.descriptor)},
					 {"observation_row_digest", text(claim_row_digest(claim))},
					 {"originating_task", task_context_value(task)}}));
			}

			for (const auto& partition : execution.base_partitions)
				if (partition.relation_descriptor_id == "source.span.v1")
					for (const auto& claim : partition.claims)
						span_rows.push_back(claim_row_json(claim));
			std::ranges::sort(bindings,
							  [](const auto& left, const auto& right)
							  {
								  const auto key = [](const auto& value)
								  {
									  return materialization::canonical_json(value);
								  };
								  return key(left) < key(right);
							  });
			std::ranges::sort(span_rows,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
			span_rows.erase(std::unique(span_rows.begin(), span_rows.end()), span_rows.end());

			std::vector<json_value> unique_bundles;
			std::set<std::string, std::less<>> seen_bundles;
			for (const auto& binding : bindings)
			{
				const auto key = materialization::canonical_json(*binding.member("bundle_digest"));
				if (seen_bundles.insert(key).second)
					unique_bundles.push_back(
						object({{"bundle", *binding.member("bundle")},
								{"bundle_digest", *binding.member("bundle_digest")}}));
			}
			const auto binding_digest =
				semantic_projection_digest("cxxlens.source-span-bundle-task-binding-set.v2",
										   array(bindings))
					.value();
			const auto bundle_digest =
				semantic_projection_digest("cxxlens.source-span-bundle-set.v2",
										   array(unique_bundles))
					.value();
			const auto row_digest =
				semantic_projection_digest(
					"cxxlens.base-claim-row-set.v1",
					object({{"descriptor_id", text("source.span.v1")}, {"rows", array(span_rows)}}))
					.value();
			const auto evidence_digest =
				semantic_projection_digest("cxxlens.source-span-bundle-row-evidence-set.v2",
										   array(bindings))
					.value();
			const auto call_observations = static_cast<std::uint64_t>(std::ranges::count_if(
				execution.reference_claims,
				[](const auto& claim)
				{
					return claim.descriptor == materialization::call_observation_v2_descriptor().id;
				}));
			const auto call_sites = static_cast<std::uint64_t>(
				claims_for_descriptor(execution, "cc.call_site.v1").size());
			const auto source_dependent_omissions =
				call_observations > call_sites ? call_observations - call_sites : 0U;
			const auto absent = entity_absent + call_absent;
			return object(
				{{"contract", text("full-primary-span-bundle-v2")},
				 {"bundle_fields",
				  array({text("span_id"),
						 text("snapshot"),
						 text("file"),
						 text("begin"),
						 text("end"),
						 text("role"),
						 text("read_only")})},
				 {"optionality", text("entity-and-call-optional-all-or-none")},
				 {"origin_evidence", text("separately-retained-and-digest-bound")},
				 {"observed_bundle_count", json_value::unsigned_integer(bindings.size())},
				 {"absent_bundle_count", json_value::unsigned_integer(absent)},
				 {"entity_absent_bundle_count", json_value::unsigned_integer(entity_absent)},
				 {"call_absent_bundle_count", json_value::unsigned_integer(call_absent)},
				 {"absent_bundle_unresolved_count", json_value::unsigned_integer(absent)},
				 {"source_dependent_canonical_omission_count",
				  json_value::unsigned_integer(source_dependent_omissions)},
				 {"unique_bundle_count", json_value::unsigned_integer(unique_span_ids.size())},
				 {"constructed_source_span_claim_count",
				  json_value::unsigned_integer(span_rows.size())},
				 {"recomputed_id_mismatch_count", json_value::unsigned_integer(0U)},
				 {"invalid_range_count", json_value::unsigned_integer(invalid_range)},
				 {"task_binding_mismatch_count",
				  json_value::unsigned_integer(task_binding_mismatch)},
				 {"hard_references_resolved",
				  json_value::boolean(invalid_range == 0U && task_binding_mismatch == 0U)},
				 {"validated_bundle_bindings", array(std::move(bindings))},
				 {"bundle_task_binding_set_digest", text(binding_digest)},
				 {"bundle_set_digest", text(bundle_digest)},
				 {"source_span_claim_set_digest", text(row_digest)},
				 {"evidence_digest", text(evidence_digest)}});
		}

		[[nodiscard]] json_value claim_row_json(const sdk::claim& claim)
		{
			auto parsed = materialization::parse_json_object(claim.row.canonical_form());
			return parsed ? parsed->root() : json_value::null();
		}

		[[nodiscard]] std::string base_row_identity(const std::string_view descriptor,
													const json_value& row)
		{
			const auto column = descriptor == "build.project.v1" ? "project_id"
				: descriptor == "build.toolchain_context.v1"	 ? "toolchain_context_id"
				: descriptor == "build.variant.v1"				 ? "build_variant_id"
				: descriptor == "source.file.v1"				 ? "file_id"
				: descriptor == "build.compile_unit.v1"			 ? "compile_unit_id"
																 : "source_span_id";
			const auto* cells = row.member("cells");
			const auto* cell = cells == nullptr ? nullptr : cells->member(column);
			const auto* value = cell == nullptr ? nullptr : cell->member("value");
			if (value == nullptr)
				return {};
			if (const auto* string = value->as_string())
				return *string;
			if (const auto* unsigned_value = value->as_unsigned_integer())
				return std::to_string(*unsigned_value);
			if (const auto* signed_value = value->as_signed_integer())
				return std::to_string(*signed_value);
			return {};
		}

		[[nodiscard]] std::string base_row_digest(const std::string_view descriptor,
												  const json_value& row)
		{
			return semantic_projection_digest(
					   "cxxlens.base-claim-row.v1",
					   object({{"descriptor_id", text(descriptor)}, {"row", row}}))
				.value();
		}

		[[nodiscard]] std::optional<std::string>
		primary_span_bundle_digest(const sdk::claim& claim,
								   const provider_task_v4_task_authority& task)
		{
			if (claim.descriptor != materialization::entity_observation_v2_descriptor().id &&
				claim.descriptor != materialization::call_observation_v2_descriptor().id)
				return std::nullopt;
			const materialization::observation_v2_task_authority authority{
				task.compile_unit_id,
				task.source.source_snapshot_id,
				task.source.file_id,
				task.source.size_bytes};
			auto decoded = materialization::decode_observation_v2_row(claim.row, authority);
			if (!decoded || !decoded->primary_span)
				return std::nullopt;
			const auto& span = *decoded->primary_span;
			return semantic_projection_digest(
					   "cxxlens.source-span-bundle.v2",
					   object({{"span_id", text(span.span_id)},
							   {"snapshot", text(span.snapshot)},
							   {"file", text(span.file)},
							   {"begin", json_value::unsigned_integer(span.begin)},
							   {"end", json_value::unsigned_integer(span.end)},
							   {"role", text(span.role)},
							   {"read_only", json_value::boolean(span.read_only)}}))
				.value();
		}

		[[nodiscard]] std::optional<std::string>
		store_source_evidence_digest(const sdk::claim& claim,
									 const materializer_store_execution& execution,
									 const provider_task_v4_task_authority& task)
		{
			const bool base =
				claim.descriptor.starts_with("build.") || claim.descriptor.starts_with("source.");
			if (!base)
				return primary_span_bundle_digest(claim, task);

			const auto row = claim_row_json(claim);
			const auto descriptor = claim.descriptor;
			const auto identity = base_row_identity(descriptor, row);
			if (descriptor == "source.span.v1")
			{
				for (const auto& reference : execution.reference_claims)
				{
					const auto bundle = primary_span_bundle_digest(reference, task);
					if (!bundle)
						continue;
					const auto decoded = materialization::decode_observation_v2_row(
						reference.row,
						materialization::observation_v2_task_authority{
							task.compile_unit_id,
							task.source.source_snapshot_id,
							task.source.file_id,
							task.source.size_bytes});
					if (!decoded || !decoded->primary_span ||
						decoded->primary_span->span_id != identity)
						continue;
					const auto evidence_edges =
						array({object({{"kind", text("dynamic_observation")},
									   {"subject_digest", text(claim_row_digest(reference))}}),
							   object({{"kind", text("source_observation")},
									   {"subject_digest", text(*bundle)}})});
					return semantic_projection_digest("cxxlens.clang22-base-source-evidence.v1",
													  evidence_edges)
						.value();
				}
				return std::nullopt;
			}
			for (const auto& origin_task : execution.worker.ingress.request.authority.tasks)
			{
				bool matches = descriptor == "build.project.v1" ||
					(descriptor == "build.toolchain_context.v1" &&
					 identity == origin_task.toolchain_context_id) ||
					(descriptor == "build.variant.v1" &&
					 identity == origin_task.build_variant_id) ||
					(descriptor == "source.file.v1" && identity == origin_task.source.file_id) ||
					(descriptor == "build.compile_unit.v1" &&
					 identity == origin_task.compile_unit_id);
				if (!matches)
					continue;
				std::string evidence;
				if (descriptor == "build.project.v1")
					evidence = origin_task.catalog_digest;
				else if (descriptor == "build.toolchain_context.v1")
					evidence = origin_task.toolchain_digest;
				else if (descriptor == "build.variant.v1")
					evidence = base_row_digest(descriptor, row);
				else if (descriptor == "source.file.v1")
					evidence = origin_task.source.content_digest;
				else
				{
					for (const auto& entry :
						 execution.worker.ingress.request.authority.project.catalog.compile_units)
						if (entry.compile_unit_id == origin_task.selected_catalog_compile_unit_id)
							evidence =
								semantic_projection_digest(
									"cxxlens.clang22-catalog-entry-evidence.v1",
									object(
										{{"catalog_compile_unit_id", text(entry.compile_unit_id)},
										 {"effective_invocation_digest",
										  text(entry.effective_invocation_digest)},
										 {"source_digest", text(entry.source_digest)},
										 {"environment_digest", text(entry.environment_digest)}}))
									.value();
				}
				return semantic_projection_digest(
						   "cxxlens.clang22-base-source-evidence.v1",
						   array(
							   {object({{"kind",
										 text(descriptor == "source.file.v1" ? "source_observation"
																			 : "compile_context")},
										{"subject_digest", text(evidence)}})}))
					.value();
			}
			return std::nullopt;
		}

		[[nodiscard]] json_value
		base_origin_association(const std::string_view descriptor,
								const json_value& row,
								const provider_task_v4_task_authority& task,
								const std::string_view evidence_digest)
		{
			(void)row;
			std::string evidence_kind;
			if (descriptor == "source.file.v1")
				evidence_kind = "source_observation";
			else
				evidence_kind = "compile_context";
			if (descriptor == "build.variant.v1")
				evidence_kind = "compile_context";
			return object({{"originating_task", task_context_value(task)},
						   {"provenance_edge",
							object({{"kind", text("request_task_input")},
									{"subject_digest", text(task.task_input_digest)}})},
						   {"evidence_edges",
							array({object({{"kind", text(evidence_kind)},
										   {"subject_digest", text(evidence_digest)}})})},
						   {"source_bundle", json_value::null()}});
		}

		[[nodiscard]] json_value base_claims_value(const materializer_store_execution& execution,
												   const json_value& guarantee_digest)
		{
			const auto& authority = execution.worker.ingress.request.authority;
			const auto& tasks = authority.tasks;
			std::vector<json_value> descriptor_results;
			std::vector<json_value> descriptor_ids;
			std::uint64_t total_rows{};
			std::uint64_t total_claims{};
			const auto producer =
				object({{"executable", text(authority.tool.executable)},
						{"interface_version", text(authority.tool.interface_version)},
						{"distribution_version", text(authority.tool.distribution_version)},
						{"source_revision", text(authority.tool.source_revision)},
						{"source_tree", text(authority.tool.source_tree)}});
			const auto producer_digest =
				semantic_projection_digest("cxxlens.base-claim-producer.v1", producer).value();

			for (const auto descriptor : task_v4_base_descriptor_ids)
			{
				descriptor_ids.push_back(text(descriptor));
				std::vector<const sdk::claim*> rows;
				for (const auto& partition : execution.base_partitions)
					if (partition.relation_descriptor_id == descriptor)
						for (const auto& claim : partition.claims)
							rows.push_back(&claim);
				std::ranges::sort(rows,
								  [](const auto* left, const auto* right)
								  {
									  const auto left_row = claim_row_json(*left);
									  const auto right_row = claim_row_json(*right);
									  return std::pair{
												 base_row_identity(left->descriptor, left_row),
												 base_row_digest(left->descriptor, left_row)} <
										  std::pair{base_row_identity(right->descriptor, right_row),
													base_row_digest(right->descriptor, right_row)};
								  });
				std::vector<json_value> row_values;
				std::vector<json_value> row_bindings;
				for (const auto* claim : rows)
				{
					const auto row = claim_row_json(*claim);
					const auto row_identity = base_row_identity(descriptor, row);
					const auto row_digest = base_row_digest(descriptor, row);
					std::vector<json_value> origin_associations;
					if (descriptor == "source.span.v1")
					{
						for (const auto& reference : execution.reference_claims)
						{
							const auto bundle =
								primary_span_bundle_digest(reference, tasks.front());
							if (!bundle)
								continue;
							const auto decoded = materialization::decode_observation_v2_row(
								reference.row,
								materialization::observation_v2_task_authority{
									tasks.front().compile_unit_id,
									tasks.front().source.source_snapshot_id,
									tasks.front().source.file_id,
									tasks.front().source.size_bytes});
							if (!decoded || !decoded->primary_span ||
								decoded->primary_span->span_id != row_identity)
								continue;
							const auto observation_row_digest = claim_row_digest(reference);
							origin_associations.push_back(object(
								{{"originating_task", task_context_value(tasks.front())},
								 {"provenance_edge",
								  object({{"kind", text("validated_span_bundle")},
										  {"subject_digest", text(*bundle)}})},
								 {"evidence_edges",
								  array({object({{"kind", text("dynamic_observation")},
												 {"subject_digest", text(observation_row_digest)}}),
										 object({{"kind", text("source_observation")},
												 {"subject_digest", text(*bundle)}})})},
								 {"source_bundle",
								  object({{"bundle_digest", text(*bundle)},
										  {"observation_descriptor_id", text(reference.descriptor)},
										  {"observation_row_digest",
										   text(observation_row_digest)}})}}));
						}
					}
					else
					{
						for (const auto& task : tasks)
						{
							bool matches = descriptor == "build.project.v1" ||
								(descriptor == "build.toolchain_context.v1" &&
								 row_identity == task.toolchain_context_id) ||
								(descriptor == "build.variant.v1" &&
								 row_identity == task.build_variant_id) ||
								(descriptor == "source.file.v1" &&
								 row_identity == task.source.file_id) ||
								(descriptor == "build.compile_unit.v1" &&
								 row_identity == task.compile_unit_id);
							if (!matches)
								continue;
							std::string evidence;
							if (descriptor == "build.project.v1")
								evidence = task.catalog_digest;
							else if (descriptor == "build.toolchain_context.v1")
								evidence = task.toolchain_digest;
							else if (descriptor == "build.variant.v1")
								evidence = row_digest;
							else if (descriptor == "source.file.v1")
								evidence = task.source.content_digest;
							else
							{
								for (const auto& entry : authority.project.catalog.compile_units)
									if (entry.compile_unit_id ==
										task.selected_catalog_compile_unit_id)
										evidence =
											semantic_projection_digest(
												"cxxlens.clang22-catalog-entry-evidence.v1",
												object(
													{{"catalog_compile_unit_id",
													  text(entry.compile_unit_id)},
													 {"effective_invocation_digest",
													  text(entry.effective_invocation_digest)},
													 {"source_digest", text(entry.source_digest)},
													 {"environment_digest",
													  text(entry.environment_digest)}}))
												.value();
							}
							origin_associations.push_back(
								base_origin_association(descriptor, row, task, evidence));
						}
					}
					std::ranges::sort(origin_associations,
									  [](const auto& left, const auto& right)
									  {
										  return materialization::canonical_json(left) <
											  materialization::canonical_json(right);
									  });
					row_values.push_back(row);
					row_bindings.push_back(
						object({{"row_identity", text(row_identity)},
								{"row_digest", text(row_digest)},
								{"row_canonical_form", text(claim->row.canonical_form())},
								{"origin_associations", array(std::move(origin_associations))}}));
				}
				std::ranges::sort(row_bindings,
								  [](const auto& left, const auto& right)
								  {
									  return materialization::canonical_json(left) <
										  materialization::canonical_json(right);
								  });
				std::ranges::sort(row_values,
								  [](const auto& left, const auto& right)
								  {
									  return materialization::canonical_json(left) <
										  materialization::canonical_json(right);
								  });
				const auto row_set_digest =
					semantic_projection_digest(
						"cxxlens.base-claim-row-set.v1",
						object({{"descriptor_id", text(descriptor)}, {"rows", array(row_values)}}))
						.value();
				const auto binding_digest =
					semantic_projection_digest(
						"cxxlens.base-claim-row-envelope-binding-set.v2",
						object({{"descriptor_id", text(descriptor)},
								{"row_envelope_bindings", array(row_bindings)}}))
						.value();
				std::vector<json_value> condition_rows;
				std::vector<json_value> interpretation_rows;
				std::vector<json_value> provenance_rows;
				std::vector<json_value> evidence_rows;
				for (const auto& binding : row_bindings)
				{
					std::vector<json_value> contexts;
					std::vector<json_value> interpretation_contexts;
					std::vector<json_value> provenance_edges;
					std::vector<json_value> evidence_edges;
					for (const auto& association :
						 *binding.member("origin_associations")->as_array())
					{
						const auto& context = *association.member("originating_task");
						contexts.push_back(object(
							{{"provider_task_id", *context.member("provider_task_id")},
							 {"task_input_digest", *context.member("task_input_digest")},
							 {"selected_catalog_compile_unit_id",
							  *context.member("selected_catalog_compile_unit_id")},
							 {"compile_unit_id", *context.member("compile_unit_id")},
							 {"condition_universe_id", *context.member("condition_universe_id")},
							 {"condition_id", *context.member("condition_id")}}));
						interpretation_contexts.push_back(object(
							{{"provider_task_id", *context.member("provider_task_id")},
							 {"task_input_digest", *context.member("task_input_digest")},
							 {"selected_catalog_compile_unit_id",
							  *context.member("selected_catalog_compile_unit_id")},
							 {"compile_unit_id", *context.member("compile_unit_id")},
							 {"interpretation_domain", *context.member("interpretation_domain")}}));
						provenance_edges.push_back(*association.member("provenance_edge"));
						evidence_edges.push_back(*association.member("evidence_edges"));
					}
					condition_rows.push_back(
						object({{"row_identity", *binding.member("row_identity")},
								{"contexts", array(std::move(contexts))}}));
					interpretation_rows.push_back(
						object({{"row_identity", *binding.member("row_identity")},
								{"contexts", array(std::move(interpretation_contexts))}}));
					provenance_rows.push_back(
						object({{"row_identity", *binding.member("row_identity")},
								{"row_digest", *binding.member("row_digest")},
								{"provenance_edges", array(std::move(provenance_edges))}}));
					evidence_rows.push_back(
						object({{"row_identity", *binding.member("row_identity")},
								{"row_digest", *binding.member("row_digest")},
								{"evidence_edges", array(std::move(evidence_edges))}}));
				}
				const auto condition_digest =
					semantic_projection_digest("cxxlens.base-claim-condition-fragment-set.v1",
											   object({{"descriptor_id", text(descriptor)},
													   {"row_bindings", array(condition_rows)}}))
						.value();
				const auto interpretation_digest =
					semantic_projection_digest(
						"cxxlens.base-claim-interpretation-domain-set.v1",
						object({{"descriptor_id", text(descriptor)},
								{"row_bindings", array(interpretation_rows)}}))
						.value();
				const auto provenance_digest =
					semantic_projection_digest("cxxlens.base-claim-provenance-edge-set.v2",
											   object({{"descriptor_id", text(descriptor)},
													   {"rows", array(provenance_rows)}}))
						.value();
				const auto evidence_digest =
					semantic_projection_digest("cxxlens.base-claim-evidence-edge-set.v2",
											   object({{"descriptor_id", text(descriptor)},
													   {"rows", array(evidence_rows)}}))
						.value();
				const auto envelope_projection =
					object({{"descriptor_id", text(descriptor)},
							{"row_set_digest", text(row_set_digest)},
							{"row_envelope_bindings", array(row_bindings)},
							{"row_envelope_binding_set_digest", text(binding_digest)},
							{"condition_fragment_set_digest", text(condition_digest)},
							{"interpretation_domain_set_digest", text(interpretation_digest)},
							{"producer_identity_digest", text(producer_digest)},
							{"provenance_edge_set_digest", text(provenance_digest)},
							{"evidence_edge_set_digest", text(evidence_digest)},
							{"guarantee_digest", guarantee_digest}});
				const auto envelope_set_digest =
					semantic_projection_digest("cxxlens.base-claim-envelope-set.v1",
											   envelope_projection)
						.value();
				descriptor_results.push_back(
					object({{"descriptor_id", text(descriptor)},
							{"row_count", json_value::unsigned_integer(rows.size())},
							{"claim_count", json_value::unsigned_integer(rows.size())},
							{"row_set_digest", text(row_set_digest)},
							{"row_envelope_bindings", array(std::move(row_bindings))},
							{"row_envelope_binding_set_digest", text(binding_digest)},
							{"condition_fragment_set_digest", text(condition_digest)},
							{"interpretation_domain_set_digest", text(interpretation_digest)},
							{"producer_identity_digest", text(producer_digest)},
							{"provenance_edge_set_digest", text(provenance_digest)},
							{"evidence_edge_set_digest", text(evidence_digest)},
							{"guarantee_digest", guarantee_digest},
							{"envelope_set_digest", text(envelope_set_digest)}}));
				total_rows += static_cast<std::uint64_t>(rows.size());
				total_claims += static_cast<std::uint64_t>(rows.size());
			}
			const auto claim_set_digest =
				semantic_projection_digest("cxxlens.base-claim-set.v1", array(descriptor_results))
					.value();
			return object({{"descriptor_ids", array(std::move(descriptor_ids))},
						   {"stage", text("canonical_claim")},
						   {"transaction_visibility", text("unpublished-until-single-commit")},
						   {"descriptor_results", array(std::move(descriptor_results))},
						   {"total_row_count", json_value::unsigned_integer(total_rows)},
						   {"total_claim_count", json_value::unsigned_integer(total_claims)},
						   {"claim_set_digest", text(claim_set_digest)},
						   {"validated_before_hard_references", json_value::boolean(true)}});
		}
	} // namespace

	sdk::result<std::string>
	encode_materializer_v2_2_success_report(const raw_input_observation& raw_input,
											const materialization::json_value& request_root,
											const materializer_store_execution& execution,
											const measured_materialization_occurrence& occurrence)
	{
		if (!raw_input.complete || !execution.worker.outcome.sealed ||
			!execution.worker.outcome.runtime_receipt || execution.claims.size() != 1U)
			return sdk::unexpected(failure("success", "missing-sealed-authority"));
		const auto& request = execution.worker.ingress.request.request;
		const auto& authority = execution.worker.ingress.request.authority;
		if (authority.tasks.size() != 1U || request.base_tasks.size() != 1U ||
			request.task_extensions.size() != 1U)
			return sdk::unexpected(failure("success", "task-census"));
		const auto& task = authority.tasks.front();
		const auto& sealed = *execution.worker.outcome.sealed;

		auto request_binding =
			object({{"materialization_request_id", text(request.materialization_request_id)},
					{"request_digest", text(request.request_digest)},
					{"semantic_request_digest", text(request.semantic_request_digest)}});
		auto task_coverage = coverage_for_task(sealed, task);
		std::vector<provider_detail::sealed_provider_batch> sealed_batches{sealed.batches().begin(),
																		   sealed.batches().end()};
		auto task_groups = groups_value(sealed_batches, task, execution);
		std::vector<json_value> encoded_batches;
		encoded_batches.reserve(sealed.batches().size());
		for (const auto& batch : sealed.batches())
			encoded_batches.push_back(batch_value(batch, task, execution));
		const auto task_batches = array(encoded_batches);

		const auto unresolved_digest = task_unresolved_digest(task);
		const auto evidence_digest = task_evidence_digest(task, sealed);
		if (!unresolved_digest || !evidence_digest)
			return sdk::unexpected(!unresolved_digest ? std::move(unresolved_digest.error())
													  : std::move(evidence_digest.error()));
		const auto profile_id =
			std::string_view{"cxxlens.clang22-materialization-guarantee-profile.v1"};
		const auto profile = object({{"profile_id", text(profile_id)},
									 {"materialization_contract_version", text("2.2.0")},
									 {"assumptions", array({})},
									 {"verification_modalities",
									  array({text("clang22.materialization-sealed.v1"),
											 text("provider.transcript-sealed.v1"),
											 text("sdk.claim-envelope-validated.v1")})}});
		auto profile_digest = semantic_projection_digest(profile_id, profile);
		if (!profile_digest)
			return sdk::unexpected(std::move(profile_digest.error()));
		auto components_without_fragment = object(
			{{"transport_coverage_set_digest",
			  *task_coverage.member("transport_record_set_digest")},
			 {"semantic_coverage_set_digest", *task_coverage.member("semantic_record_set_digest")},
			 {"unresolved_set_digest", text(*unresolved_digest)},
			 {"evidence_set_digest", text(*evidence_digest)},
			 {"guarantee_profile_id", text(profile_id)},
			 {"guarantee_profile_digest", text(*profile_digest)},
			 {"guarantee_fragment_digest",
			  empty_projection_digest("cxxlens.clang22-task-guarantee-fragment.v1")}});

		// Build the task result from the sealed worker transcript.  Every digest below is
		// calculated from the same value-owned projection that is emitted, so report construction
		// cannot accidentally certify a different in-memory object.
		auto task_result_without_components = object({
			{"provider_task_id", text(task.provider_task_id)},
			{"provider_execution_id", text(task.provider_execution_id)},
			{"selected_catalog_compile_unit_id", text(task.selected_catalog_compile_unit_id)},
			{"compile_unit_id", text(task.compile_unit_id)},
			{"task_input_digest", text(task.task_input_digest)},
			{"terminal", text(execution.worker.outcome.terminal)},
			{"input_transfer",
			 execution.worker.outcome.input_seal
				 ? input_transfer_value(*execution.worker.outcome.input_seal)
				 : json_value::null()},
			{"runtime_receipt", runtime_receipt_value(*execution.worker.outcome.runtime_receipt)},
			{"coverage", task_coverage},
			{"groups", task_groups},
			{"batches", task_batches},
			{"side_channel_components", components_without_fragment},
		});
		auto fragment = task_guarantee_fragment_digest(
			task, task_result_without_components, components_without_fragment);
		if (!fragment)
			return sdk::unexpected(std::move(fragment.error()));
		auto components = object(
			{{"transport_coverage_set_digest",
			  *task_coverage.member("transport_record_set_digest")},
			 {"semantic_coverage_set_digest", *task_coverage.member("semantic_record_set_digest")},
			 {"unresolved_set_digest", text(*unresolved_digest)},
			 {"evidence_set_digest", text(*evidence_digest)},
			 {"guarantee_profile_id", text(profile_id)},
			 {"guarantee_profile_digest", text(*profile_digest)},
			 {"guarantee_fragment_digest", text(*fragment)}});
		const auto semantic_key = semantic_result_key_value(task);
		auto side_digest = semantic_projection_digest(
			"cxxlens.clang22-task-side-channels.v1",
			object({{"task_execution_key", semantic_key}, {"components", components}}));
		if (!side_digest)
			return sdk::unexpected(std::move(side_digest.error()));
		std::vector<json_value> group_summaries;
		if (const auto* groups = task_groups.as_array(); groups != nullptr)
		{
			for (const auto& group : *groups)
				group_summaries.push_back(
					object({{"dependency_group_id", *group.member("dependency_group_id")},
							{"batch_set_digest", *group.member("batch_set_digest")}}));
			std::ranges::sort(group_summaries,
							  [](const auto& left, const auto& right)
							  {
								  return materialization::canonical_json(left) <
									  materialization::canonical_json(right);
							  });
		}
		auto task_result_digest = semantic_projection_digest(
			"cxxlens.clang22-task-result.v1",
			object(
				{{"task_execution_key", task_execution_key_value(task)},
				 {"selected_catalog_compile_unit_id", text(task.selected_catalog_compile_unit_id)},
				 {"compile_unit_id", text(task.compile_unit_id)},
				 {"terminal", text(execution.worker.outcome.terminal)},
				 {"input_transfer",
				  execution.worker.outcome.input_seal
					  ? input_transfer_value(*execution.worker.outcome.input_seal)
					  : json_value::null()},
				 {"runtime_receipt",
				  runtime_receipt_value(*execution.worker.outcome.runtime_receipt)},
				 {"coverage", task_coverage},
				 {"groups", array(std::move(group_summaries))},
				 {"side_channel_digest", text(*side_digest)}}));
		if (!task_result_digest)
			return sdk::unexpected(std::move(task_result_digest.error()));
		auto task_result = object(
			{{"provider_task_id", text(task.provider_task_id)},
			 {"provider_execution_id", text(task.provider_execution_id)},
			 {"selected_catalog_compile_unit_id", text(task.selected_catalog_compile_unit_id)},
			 {"compile_unit_id", text(task.compile_unit_id)},
			 {"task_input_digest", text(task.task_input_digest)},
			 {"terminal", text(execution.worker.outcome.terminal)},
			 {"input_transfer",
			  execution.worker.outcome.input_seal
				  ? input_transfer_value(*execution.worker.outcome.input_seal)
				  : json_value::null()},
			 {"runtime_receipt", runtime_receipt_value(*execution.worker.outcome.runtime_receipt)},
			 {"coverage", task_coverage},
			 {"groups", task_groups},
			 {"batches", task_batches},
			 {"side_channel_components", components},
			 {"side_channel_digest", text(*side_digest)},
			 {"task_result_digest", text(*task_result_digest)}});
		auto task_results = std::vector<json_value>{task_result};

		auto transport_summary = global_coverage_summary(task_results, "transport");
		auto semantic_summary = global_coverage_summary(task_results, "semantic");
		auto unresolved = global_unresolved_summary(task_results);
		auto evidence = global_evidence_summary(task_results, task);
		if (!transport_summary || !semantic_summary || !unresolved || !evidence)
			return sdk::unexpected(!transport_summary	   ? std::move(transport_summary.error())
									   : !semantic_summary ? std::move(semantic_summary.error())
									   : !unresolved	   ? std::move(unresolved.error())
														   : std::move(evidence.error()));
		auto guarantee = object(
			{{"record_type", text("typed-guarantee")},
			 {"profile_id", text(profile_id)},
			 {"profile_digest", text(*profile_digest)},
			 {"approximation", text("exact")},
			 {"scope", text(task.project_id)},
			 {"assumptions", array({})},
			 {"verification_modalities",
			  array({text("clang22.materialization-sealed.v1"),
					 text("provider.transcript-sealed.v1"),
					 text("sdk.claim-envelope-validated.v1")})},
			 {"observation_descriptor_censuses", array({})},
			 {"digest", empty_projection_digest("cxxlens.clang22-materialization-guarantee.v1")}});
		std::vector<json_value> censuses;
		for (const auto descriptor : {"frontend.clang22.call_observation.v2",
									  "frontend.clang22.entity_observation.v2",
									  "frontend.clang22.type_observation.v2"})
		{
			const auto batch =
				std::ranges::find_if(encoded_batches,
									 [&](const auto& value)
									 {
										 const auto* id = value.member("descriptor_id");
										 return id != nullptr && id->as_string() != nullptr &&
											 *id->as_string() == descriptor;
									 });
			if (batch == encoded_batches.end())
				return sdk::unexpected(failure("guarantee", "observation-batch"));
			const auto* census_value = batch->member("observation_equivalence_census");
			if (census_value == nullptr)
				return sdk::unexpected(failure("guarantee", "observation-census"));
			const auto& census = *census_value;
			std::uint64_t exact_count{};
			std::uint64_t non_exact_count{};
			if (const auto* rows = census.member("rows");
				rows != nullptr && rows->as_array() != nullptr)
				for (const auto& row : *rows->as_array())
				{
					if (row.member("exact_equivalence") != nullptr &&
						row.member("exact_equivalence")->as_boolean() != nullptr &&
						*row.member("exact_equivalence")->as_boolean())
						++exact_count;
					else
						++non_exact_count;
				}
			const auto* census_digest = census.member("row_equivalence_set_digest");
			if (census_digest == nullptr)
				return sdk::unexpected(failure("guarantee", "observation-census-digest"));
			censuses.push_back(object(
				{{"descriptor_id", text(descriptor)},
				 {"exact_equivalence_count", json_value::unsigned_integer(exact_count)},
				 {"non_exact_equivalence_count", json_value::unsigned_integer(non_exact_count)},
				 {"row_equivalence_set_digest", *census_digest}}));
		}
		auto guarantee_fields = *guarantee.as_object();
		guarantee_fields.erase("observation_descriptor_censuses");
		guarantee_fields.emplace("observation_descriptor_censuses", array(std::move(censuses)));
		guarantee = json_value::object(std::move(guarantee_fields)).value();
		auto guarantee_projection =
			object({{"record_type", *guarantee.member("record_type")},
					{"profile_id", *guarantee.member("profile_id")},
					{"profile_digest", *guarantee.member("profile_digest")},
					{"approximation", *guarantee.member("approximation")},
					{"scope", *guarantee.member("scope")},
					{"assumptions", *guarantee.member("assumptions")},
					{"verification_modalities", *guarantee.member("verification_modalities")},
					{"observation_descriptor_censuses",
					 *guarantee.member("observation_descriptor_censuses")},
					{"global_side_channel_digests",
					 object({{"transport_coverage", *transport_summary->member("digest")},
							 {"coverage", *semantic_summary->member("digest")},
							 {"unresolved", *unresolved->member("digest")},
							 {"evidence", *evidence->member("digest")}})},
					{"task_guarantee_fragments",
					 task_component_rows(task_results, "guarantee_fragment_digest")}});
		auto guarantee_digest = semantic_projection_digest(
			"cxxlens.clang22-materialization-guarantee.v1", guarantee_projection);
		if (!guarantee_digest)
			return sdk::unexpected(std::move(guarantee_digest.error()));
		guarantee_fields = *guarantee.as_object();
		guarantee_fields.erase("digest");
		guarantee_fields.emplace("digest", text(*guarantee_digest));
		guarantee = json_value::object(std::move(guarantee_fields)).value();
		auto side_channels = object({{"transport_coverage", *transport_summary},
									 {"coverage", *semantic_summary},
									 {"unresolved", *unresolved},
									 {"evidence", *evidence},
									 {"guarantee", guarantee}});
		auto store = store_value(execution, task);
		std::vector<json_value> claim_stages;
		for (const auto descriptor : task_v4_output_descriptor_ids)
			claim_stages.push_back(claim_stage_value(descriptor,
													 claims_for_descriptor(execution, descriptor),
													 *guarantee.member("digest"),
													 store,
													 task_results));
		std::uint64_t canonical_count{};
		std::vector<json_value> canonical_stages;
		for (const auto descriptor : task_v4_output_descriptor_ids)
		{
			if (!descriptor.starts_with("cc."))
				continue;
			const auto stage = std::ranges::find_if(
				claim_stages,
				[&](const auto& value)
				{
					return value.member("descriptor_id") != nullptr &&
						*value.member("descriptor_id")->as_string() == descriptor;
				});
			if (stage == claim_stages.end())
				return sdk::unexpected(failure("provenance", "canonical-stage"));
			const auto count = *stage->member("origin_association_count")->as_unsigned_integer();
			canonical_count += count;
			canonical_stages.push_back(object(
				{{"descriptor_id", text(descriptor)},
				 {"sdk_claim_occurrence_count", *stage->member("sdk_claim_occurrence_count")},
				 {"origin_association_count", *stage->member("origin_association_count")},
				 {"provenance_edge_set_digest", *stage->member("provenance_edge_set_digest")},
				 {"claim_stage_digest", *stage->member("claim_stage_digest")}}));
		}
		auto provenance = object(
			{{"record_type", text("typed-provenance-edge-summary")},
			 {"edge_count", json_value::unsigned_integer(canonical_count)},
			 {"canonical_claim_count", json_value::unsigned_integer(canonical_count)},
			 {"canonical_claims_with_exact_input_edges",
			  json_value::unsigned_integer(canonical_count)},
			 {"orphan_count", json_value::unsigned_integer(0U)},
			 {"ambiguous_count", json_value::unsigned_integer(0U)},
			 {"edge_set_digest", empty_projection_digest("cxxlens.clang22-global-provenance.v1")}});
		auto provenance_fields = *provenance.as_object();
		provenance_fields.erase("edge_set_digest");
		provenance_fields.emplace("canonical_claim_stages", array(std::move(canonical_stages)));
		auto provenance_digest =
			semantic_projection_digest("cxxlens.clang22-global-provenance.v1",
									   json_value::object(std::move(provenance_fields)).value());
		if (!provenance_digest)
			return sdk::unexpected(std::move(provenance_digest.error()));
		auto updated_provenance = *provenance.as_object();
		updated_provenance.erase("edge_set_digest");
		updated_provenance.emplace("edge_set_digest", text(*provenance_digest));
		provenance = json_value::object(std::move(updated_provenance)).value();
		auto reopened = reopened_store_value(execution, task);
		auto publication = publication_value(execution, request_root, reopened);
		if (!publication)
			return sdk::unexpected(std::move(publication.error()));
		const auto task_key_digest = sdk::content_digest(
			std::as_bytes(std::span{task.provider_task_id.data(), task.provider_task_id.size()}));
		const auto artifact_ref =
			std::string{"materialization.incremental-sealed-artifact:"} + task_key_digest;
		const auto partition_set_ref =
			std::string{"materialization.incremental-task-partition-set:"} + task_key_digest;
		std::vector<json_value> task_result_rows;
		std::vector<json_value> raw_frame_rows;
		for (const auto& result : task_results)
		{
			task_result_rows.push_back(
				object({{"task_execution_key",
						 array({text(task.provider_task_id),
								text(task.task_input_digest),
								text(task.provider_execution_id)})},
						{"task_result_digest", *result.member("task_result_digest")}}));
			const auto& receipt = *result.member("runtime_receipt");
			raw_frame_rows.push_back(
				object({{"task_execution_key",
						 array({text(task.provider_task_id),
								text(task.task_input_digest),
								text(task.provider_execution_id)})},
						{"raw_frame_stream_bytes", *receipt.member("raw_frame_stream_bytes")},
						{"raw_frame_stream_digest", *receipt.member("raw_frame_stream_digest")},
						{"frame_count", *receipt.member("frame_count")},
						{"frame_transcript_digest", *receipt.member("frame_transcript_digest")}}));
		}
		std::ranges::sort(task_result_rows,
						  [](const auto& left, const auto& right)
						  {
							  return materialization::canonical_json(left) <
								  materialization::canonical_json(right);
						  });
		std::ranges::sort(raw_frame_rows,
						  [](const auto& left, const auto& right)
						  {
							  return materialization::canonical_json(left) <
								  materialization::canonical_json(right);
						  });
		auto task_result_set_digest = semantic_projection_digest(
			"cxxlens.clang22-task-result-set.v1", array(task_result_rows));
		auto raw_frame_set_digest =
			semantic_projection_digest("cxxlens.clang22-raw-frame-set.v1", array(raw_frame_rows));
		if (!task_result_set_digest || !raw_frame_set_digest)
			return sdk::unexpected(!task_result_set_digest
									   ? std::move(task_result_set_digest.error())
									   : std::move(raw_frame_set_digest.error()));
		const auto incremental =
			object({{"schema", text("cxxlens.ng-g5-production-execution-census.v1")},
					{"planned_provider_executions", json_value::unsigned_integer(1U)},
					{"planned_provider_task_executions", json_value::unsigned_integer(1U)},
					{"actual_provider_executions", json_value::unsigned_integer(1U)},
					{"actual_recomputed_partition_count", json_value::unsigned_integer(1U)},
					{"warm_zero", json_value::boolean(false)},
					{"executed_partition_ids",
					 array({text(execution.claims.front().partition_manifest.partition_id)})},
					{"executed_provider_task_ids", array({text(task.provider_task_id)})},
					{"executed_provider_execution_ids", array({text(task.provider_execution_id)})},
					{"executed_artifact_digests", array({text(artifact_ref)})},
					{"executed_task_partition_set_digests", array({text(partition_set_ref)})}});
		auto report = object(
			{{"schema", text("cxxlens.clang22-materialization-report.v2")},
			 {"report_version", text("2.2.0")},
			 {"response_kind", text("detailed")},
			 {"result", text("passed")},
			 {"generated_at", text(utc_now())},
			 {"process_exit_status", json_value::unsigned_integer(0U)},
			 {"raw_input_observation", raw_input_value(raw_input)},
			 {"source",
			  object({{"revision", *member(*member(request_root, "tool"), "source_revision")},
					  {"tree", *member(*member(request_root, "tool"), "source_tree")}})},
			 {"request", request_binding},
			 {"installation",
			  object({{"requested",
					   object({{"occurrence_manifest_digest",
								*member(*member(request_root, "tool"),
										"occurrence_manifest_digest")}})},
					  {"measured", measured_occurrence(occurrence, request_root)}})},
			 {"provider", provider_value(request_root)},
			 {"project", *member(request_root, "project")},
			 {"registry", *member(request_root, "registry")},
			 {"engine", *member(request_root, "engine")},
			 {"interpretation_policy", *member(request_root, "interpretation_policy")},
			 {"trust_policy", *member(request_root, "trust_policy")},
			 {"adoption",
			  object(
				  {{"boundary", text("sealed-materialization-result")},
				   {"visibility", text("tool-private-immutable")},
				   {"state", text("sealed")},
				   {"partial_policy", text("forbid")},
				   {"all_tasks_mandatory", json_value::boolean(true)},
				   {"all_groups_mandatory", json_value::boolean(true)},
				   {"all_batches_mandatory", json_value::boolean(true)},
				   {"task_result_set_digest", text(*task_result_set_digest)},
				   {"raw_frames",
					object({{"authority", text("diagnostic-only-non-authoritative")},
							{"retained", json_value::boolean(false)},
							{"frame_count",
							 json_value::unsigned_integer(
								 execution.worker.outcome.runtime_receipt->decoded_frame_count())},
							{"frame_set_digest", text(*raw_frame_set_digest)}})}})},
			 {"task_results", array(std::move(task_results))},
			 {"span_validation", span_validation_value(execution, task)},
			 {"base_claims", base_claims_value(execution, *guarantee.member("digest"))},
			 {"side_channels", side_channels},
			 {"claim_stages", array(std::move(claim_stages))},
			 {"provenance", provenance},
			 {"store", store},
			 {"publication", *publication},
			 {"semantic_verification",
			  object({{"status", text("passed")},
					  {"reopened_store", reopened},
					  {"reopen_attempt", json_value::null()},
					  {"failure", json_value::null()}})},
			 {"incremental_execution", incremental},
			 {"error", json_value::null()}});
		return materialization::canonical_json_line(report);
	}
} // namespace cxxlens::detail::clang22
