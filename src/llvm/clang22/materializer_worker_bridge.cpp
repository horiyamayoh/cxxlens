#include "materializer_worker_bridge.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <iterator>
#include <limits>
#include <map>
#include <memory>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <cxxlens/relations/build_compile_unit.hpp>
#include <cxxlens/relations/build_project.hpp>
#include <cxxlens/relations/build_toolchain_context.hpp>
#include <cxxlens/relations/build_variant.hpp>
#include <cxxlens/relations/cc_call_direct_target.hpp>
#include <cxxlens/relations/cc_call_site.hpp>
#include <cxxlens/relations/cc_entity.hpp>
#include <cxxlens/relations/source_file.hpp>
#include <cxxlens/relations/source_span.hpp>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#include "materialization_json.hpp"
#include "materialization_rooted_vfs.hpp"
#include "materialization_v4_claim_binding.hpp"
#include "observation_v2.hpp"
#include "protocol_v2/closure.hpp"
#include "provider_task_v4.hpp"
#include "provider_worker_v4.hpp"
#include "provider_worker_v4_output_normalizer.hpp"
#include "source_closure_task_v4.hpp"
#include "source_closure_transport.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;
		namespace protocol = ::cxxlens::protocol_v2;

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::result<json_value> text(std::string_view value)
		{
			return json_value::string(std::string{value});
		}

		[[nodiscard]] sdk::result<json_value> object(json_value::object_type fields)
		{
			return json_value::object(std::move(fields));
		}

		[[nodiscard]] std::string role_name(source_closure_role value)
		{
			switch (value)
			{
				case source_closure_role::main:
					return "main";
				case source_closure_role::header:
					return "header";
				case source_closure_role::generated:
					return "generated";
				case source_closure_role::forced_include:
					return "forced-include";
				case source_closure_role::macro_file:
					return "macro-file";
			}
			return {};
		}

		[[nodiscard]] std::string encoding_name(source_closure_encoding value)
		{
			switch (value)
			{
				case source_closure_encoding::utf8:
					return "utf8";
				case source_closure_encoding::utf16le:
					return "utf16le";
				case source_closure_encoding::utf16be:
					return "utf16be";
				case source_closure_encoding::locale_dependent:
					return "locale_dependent";
				case source_closure_encoding::binary_or_unknown:
					return "binary_or_unknown";
			}
			return {};
		}

		[[nodiscard]] sdk::result<json_value>
		manifest_value(const source_closure_snapshot& snapshot)
		{
			std::vector<json_value> members;
			members.reserve(snapshot.members.size());
			for (const auto& member : snapshot.members)
			{
				json_value::object_type fields;
				fields.emplace("content_digest", text(member.content_digest).value());
				fields.emplace("encoding", text(encoding_name(member.encoding)).value());
				fields.emplace("file_id", text(member.file_id).value());
				fields.emplace("logical_path", text(member.logical_path).value());
				fields.emplace("read_only", json_value::boolean(member.read_only));
				fields.emplace("role", text(role_name(member.role)).value());
				fields.emplace("size_bytes", json_value::unsigned_integer(member.size_bytes));
				members.push_back(object(std::move(fields)).value());
			}
			std::vector<json_value> blobs;
			blobs.reserve(snapshot.blobs.size());
			for (const auto& blob : snapshot.blobs)
			{
				json_value::object_type fields;
				fields.emplace("content_digest", text(blob.content_digest).value());
				fields.emplace("size_bytes", json_value::unsigned_integer(blob.size_bytes));
				blobs.push_back(object(std::move(fields)).value());
			}
			json_value::object_type root;
			root.emplace("blobs", json_value::array(std::move(blobs)));
			root.emplace("closure_digest", text(snapshot.closure_digest).value());
			root.emplace("closure_id", text(snapshot.snapshot_id).value());
			root.emplace("members", json_value::array(std::move(members)));
			root.emplace("schema", text(source_closure_manifest_schema).value());
			return object(std::move(root));
		}

		[[nodiscard]] sdk::result<json_value>
		base_task_value(const provider_task_v4_base_task& base)
		{
			json_value::object_type source;
			source.emplace("content_digest", text(base.source.content_digest).value());
			source.emplace("encoding", text(base.source.encoding).value());
			source.emplace("file_id", text(base.source.file_id).value());
			source.emplace("line_index_id", text(base.source.line_index_id).value());
			source.emplace("logical_path", text(base.source.logical_path).value());
			source.emplace("read_only", json_value::boolean(base.source.read_only));
			source.emplace("size_bytes", json_value::unsigned_integer(base.source.size_bytes));
			source.emplace("source_snapshot_id", text(base.source.source_snapshot_id).value());
			return object({
				{"environment_digest", text(base.environment_digest).value()},
				{"normalized_invocation_digest", text(base.normalized_invocation_digest).value()},
				{"provider_execution_id", text(base.provider_execution_id).value()},
				{"provider_task_id", text(base.provider_task_id).value()},
				{"source", object(std::move(source)).value()},
				{"task_input_digest", text(base.task_input_digest).value()},
				{"toolchain_digest", text(base.toolchain_digest).value()},
				{"working_directory", text(base.working_directory).value()},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		array_strings(const std::span<const std::string> values)
		{
			std::vector<json_value> encoded;
			encoded.reserve(values.size());
			for (const auto& value : values)
				encoded.push_back(text(value).value());
			return json_value::array(std::move(encoded));
		}

		[[nodiscard]] sdk::result<json_value>
		closure_binding_value(const source_closure_transfer_binding& binding,
							  const std::string_view transfer,
							  const std::uint64_t stream_id)
		{
			return object({
				{"closure_digest", text(binding.closure_digest).value()},
				{"closure_id", text(binding.closure_id).value()},
				{"expected_transfer_digest", text(transfer).value()},
				{"first_sequence", json_value::unsigned_integer(binding.first_sequence)},
				{"manifest_digest", text(binding.manifest_digest).value()},
				{"session_id", text(binding.session_id).value()},
				{"stream_id", json_value::unsigned_integer(stream_id)},
				{"task_id", text(binding.task_id).value()},
				{"task_v4_digest", text(binding.task_v4_digest).value()},
			});
		}

		[[nodiscard]] std::array<const sdk::relation_descriptor*, 6U> output_descriptors()
		{
			return {&cc::relations::call_direct_target::descriptor(),
					&cc::relations::call_site::descriptor(),
					&cc::relations::entity::descriptor(),
					&materialization::call_observation_v2_descriptor(),
					&materialization::entity_observation_v2_descriptor(),
					&materialization::type_observation_v2_descriptor()};
		}

		[[nodiscard]] sdk::result<json_value>
		worker_output_value(const provider_task_v4_task_authority& task,
							const sdk::detail::build_capture_draft& capture,
							const sdk::provider::manifest& manifest,
							const std::string_view semantic_contract_digest,
							const std::array<const sdk::relation_descriptor*, 6U>& descriptors)
		{
			std::vector<json_value> descriptor_ids;
			descriptor_ids.reserve(task_v4_output_descriptor_ids.size());
			std::vector<json_value> descriptor_digests;
			descriptor_digests.reserve(descriptors.size());
			for (const auto& id : task_v4_output_descriptor_ids)
				descriptor_ids.push_back(text(id).value());
			for (const auto* descriptor : descriptors)
				descriptor_digests.push_back(text(descriptor->descriptor_digest).value());
			std::vector<json_value> groups;
			groups.reserve(task_v4_dependency_groups.size());
			for (const auto& group : task_v4_dependency_groups)
				groups.push_back(text(group).value());
			const auto maximum_rows = std::min<std::uint64_t>(task.budget.rows, 100000U);
			const auto maximum_bytes = std::min<std::uint64_t>(task.budget.output_bytes,
															   std::uint64_t{16U} * 1024U * 1024U);
			return object({
				{"compile_unit_id", text(capture.compile_unit_id).value()},
				{"dependency_groups", json_value::array(std::move(groups))},
				{"descriptor_digests", json_value::array(std::move(descriptor_digests))},
				{"maximum_output_bytes", json_value::unsigned_integer(maximum_bytes)},
				{"maximum_rows", json_value::unsigned_integer(maximum_rows)},
				{"provider_id", text(manifest.provider_id).value()},
				{"provider_version", text(manifest.provider_version.string()).value()},
				{"requested_descriptor_ids", json_value::array(std::move(descriptor_ids))},
				{"semantic_contract_digest", text(semantic_contract_digest).value()},
				{"toolchain_context_id", text(capture.toolchain_context_id).value()},
			});
		}

		struct materializer_basis_authority
		{
			std::string materializer_semantics_digest;
			std::string basis_digest;
			std::string producer_input_basis_digest;
			std::string canonical_adoption_transform_digest;
			std::string base_ingestion_transform_digest;
			std::string assumption_set_id;
			sdk::claim_guarantee guarantee;
		};

		[[nodiscard]] sdk::canonical_value canonical_text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value canonical_version(const sdk::semantic_version value)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.major)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.minor)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.patch)),
			});
		}

		[[nodiscard]] sdk::canonical_value
		canonical_strings(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> encoded;
			encoded.reserve(values.size());
			for (const auto& value : values)
				encoded.push_back(canonical_text(value));
			return sdk::canonical_value::from_tuple(std::move(encoded));
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_digest_projection(const std::string_view domain,
								   const sdk::canonical_value& projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			std::string bytes;
			bytes.reserve(encoded->size());
			for (const auto byte : *encoded)
				bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			return sdk::semantic_digest(domain, bytes);
		}

		[[nodiscard]] sdk::result<std::string>
		direct_input_basis_digest(const std::string_view basis_digest)
		{
			auto identity = sdk::canonical_identity_digest(
				"producer-input-direct", std::array{canonical_text(basis_digest)});
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			const auto separator = identity->find(':');
			if (separator == std::string::npos)
				return sdk::unexpected(
					failure("materialization.identity-mismatch", "direct-basis"));
			return identity->substr(separator + 1U);
		}

		[[nodiscard]] sdk::result<materializer_basis_authority> make_materializer_basis_authority(
			const provider_task_v4_request_authority& authority,
			const std::span<const sdk::detail::validated_build_capture> captures)
		{
			const auto& tool = authority.tool;
			auto materializer_semantics = semantic_digest_projection(
				"cxxlens.clang22-materializer-semantics.v1",
				sdk::canonical_value::from_tuple({canonical_text(tool.executable),
												  canonical_text(tool.interface_version),
												  canonical_text(tool.distribution_version),
												  canonical_text(tool.source_revision),
												  canonical_text(tool.source_tree)}));
			if (!materializer_semantics)
				return sdk::unexpected(std::move(materializer_semantics.error()));

			std::vector<sdk::canonical_value> admitted;
			admitted.reserve(authority.engine.admitted_descriptors.size());
			for (const auto& descriptor : authority.engine.admitted_descriptors)
				admitted.push_back(sdk::canonical_value::from_tuple({
					canonical_text(descriptor.descriptor_id),
					canonical_text(descriptor.runtime_descriptor_digest),
				}));

			std::vector<sdk::canonical_value> semantic_tasks;
			if (authority.tasks.empty() || authority.tasks.size() != captures.size())
				return sdk::unexpected(
					failure("materialization.task-census-invalid", "build-captures"));
			semantic_tasks.reserve(authority.tasks.size());
			for (std::size_t index{}; index < authority.tasks.size(); ++index)
			{
				const auto& task = authority.tasks[index];
				const auto& capture = captures[index].value();
				auto context = sdk::canonical_value::from_tuple({
					canonical_text(task.provider_task_id),
					canonical_text(task.task_input_digest),
					canonical_text(capture.selected_catalog_compile_unit_id),
					canonical_text(capture.compile_unit_id),
					canonical_text(task.condition_universe_id),
					canonical_text(task.condition_id),
					canonical_text(task.interpretation_domain),
				});
				semantic_tasks.push_back(sdk::canonical_value::from_tuple(
					{std::move(context), canonical_text(task.task_input_digest)}));
			}
			std::ranges::sort(semantic_tasks,
							  [](const auto& left, const auto& right)
							  {
								  return sdk::canonical_binary(left).value() <
									  sdk::canonical_binary(right).value();
							  });
			const auto projection = sdk::canonical_value::from_tuple({
				canonical_text("cxxlens.clang22-direct-materialization-basis.v1"),
				canonical_text(*materializer_semantics),
				sdk::canonical_value::from_tuple({
					canonical_text(authority.worker.provider_id),
					canonical_version(authority.worker.provider_version),
					canonical_text(authority.worker.semantic_contract_digest),
					sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(authority.worker.protocol_major)),
					sdk::canonical_value::from_integer(
						static_cast<std::int64_t>(authority.worker.protocol_minor)),
					canonical_strings(authority.worker.required_features),
				}),
				sdk::canonical_value::from_tuple(
					{canonical_text(captures.front().value().project_id),
					 canonical_text(captures.front().value().catalog.catalog_id),
					 canonical_text(captures.front().value().catalog.catalog_digest)}),
				sdk::canonical_value::from_tuple(
					{canonical_text(authority.engine.generation_contract),
					 canonical_text(authority.engine.engine_generation_id),
					 canonical_text(authority.engine.engine_registry_digest),
					 sdk::canonical_value::from_tuple(std::move(admitted))}),
				sdk::canonical_value::from_tuple(std::move(semantic_tasks)),
			});
			auto basis = semantic_digest_projection(
				"cxxlens.clang22-direct-materialization-basis.v1", projection);
			if (!basis)
				return sdk::unexpected(std::move(basis.error()));
			auto producer_input = direct_input_basis_digest(*basis);
			if (!producer_input)
				return sdk::unexpected(std::move(producer_input.error()));
			auto canonical_transform = semantic_digest_projection(
				"cxxlens.clang22-canonical-adoption-transform.v1",
				sdk::canonical_value::from_tuple(
					{canonical_text("cxxlens.clang22-canonical-adoption-transform.v1"),
					 canonical_text(*materializer_semantics),
					 canonical_text(authority.engine.engine_registry_digest)}));
			if (!canonical_transform)
				return sdk::unexpected(std::move(canonical_transform.error()));
			auto base_transform = semantic_digest_projection(
				"cxxlens.clang22-base-ingestion-transform.v1",
				sdk::canonical_value::from_tuple(
					{canonical_text("cxxlens.clang22-base-ingestion-transform.v1"),
					 canonical_text(*materializer_semantics),
					 canonical_text(authority.engine.engine_registry_digest)}));
			if (!base_transform)
				return sdk::unexpected(std::move(base_transform.error()));
			auto assumption_digest = semantic_digest_projection(
				"cxxlens.clang22-assumption-set.v1",
				sdk::canonical_value::from_tuple(std::vector<sdk::canonical_value>{}));
			if (!assumption_digest)
				return sdk::unexpected(std::move(assumption_digest.error()));
			return materializer_basis_authority{*materializer_semantics,
												*basis,
												*producer_input,
												*canonical_transform,
												*base_transform,
												"assumption-set:" + *assumption_digest,
												{"exact",
												 captures.front().value().project_id,
												 "assumption-set:" + *assumption_digest,
												 {"clang22.materialization-sealed.v1",
												  "provider.transcript-sealed.v1",
												  "sdk.claim-envelope-validated.v1"}}};
		}

		[[nodiscard]] sdk::result<sdk::detail::validated_materialization_task>
		make_generic_materialization_task(
			const installed_materializer_source_closure_result& ingress,
			const sdk::relation_engine& engine,
			const materializer_basis_authority& basis,
			const std::string_view measured_worker_digest,
			const std::string_view provider_input_digest)
		{
			const auto& authority = ingress.request.authority;
			if (authority.tasks.size() != 1U || ingress.request.build_captures.size() != 1U)
				return sdk::unexpected(
					failure("materialization.task-census-invalid", "generic-task", "one-task"));
			const auto& frontend_task = authority.tasks.front();
			auto generic_provider_id = authority.worker.provider_id;
			std::ranges::replace(generic_provider_id, ':', '.');
			const auto& capture = ingress.request.build_captures.front();
			const auto& capture_value = capture.value();
			const auto descriptor_pointers = output_descriptors();
			std::vector<sdk::relation_descriptor> descriptors;
			descriptors.reserve(descriptor_pointers.size());
			for (const auto* descriptor : descriptor_pointers)
				descriptors.push_back(*descriptor);

			sdk::detail::generic_materialization_task_request request{
				ingress.request.request.materialization_request_id,
				std::string{provider_input_digest},
				capture,
				{generic_provider_id,
				 authority.worker.provider_version,
				 authority.worker.semantic_contract_digest,
				 descriptors,
				 {},
				 {frontend_task.interpretation_domain},
				 "capture",
				 "observation"},
				descriptors,
				frontend_task.condition_universe_id,
				frontend_task.condition_id,
				frontend_task.interpretation_domain,
				frontend_task.dependency_groups,
				"clang22-task-v4-output-normalizer.v1",
				basis.assumption_set_id,
				"exact",
				{"compile-unit", capture_value.project_id, "covered"},
				{},
				{generic_provider_id,
				 authority.worker.provider_version,
				 std::string{measured_worker_digest},
				 authority.worker.semantic_contract_digest,
				 authority.trust_policy.required_qualification,
				 authority.trust_policy.trust_policy_digest,
				 frontend_task.sandbox,
				 frontend_task.budget},
				{{authority.publication.selector,
				  {1U, 0U, 0U},
				  capture_value.catalog.catalog_digest,
				  authority.publication.expected_parent_publication},
				 authority.publication.recipe_digest,
				 authority.publication.output_plan_digest,
				 authority.publication.publication_target}};
			return sdk::detail::make_generic_materialization_task(engine, std::move(request));
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		closure_transcript(const source_closure_transfer_binding& binding,
						   const source_closure_snapshot& snapshot,
						   const std::string_view transfer,
						   const std::uint64_t stream_id)
		{
			auto manifest = manifest_value(snapshot);
			if (!manifest)
				return sdk::unexpected(std::move(manifest.error()));
			const auto manifest_bytes = materialization::canonical_json(*manifest);
			const auto limits = protocol::closure_limits{};
			if (manifest_bytes.empty() || manifest_bytes.size() > limits.maximum_manifest_bytes)
				return sdk::unexpected(failure("source-closure.limit-exceeded", "manifest"));
			std::vector<std::byte> output;
			std::uint64_t sequence{binding.first_sequence};
			auto append = [&](const sdk::provider::message_type type,
							  const protocol::closure_control& control,
							  const std::span<const std::byte> payload) -> sdk::result<void>
			{
				auto encoded = protocol::encode_closure_control(
					static_cast<protocol::message_type>(static_cast<std::uint16_t>(type)),
					control,
					limits);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				sdk::provider::frame frame;
				frame.type = type;
				frame.stream_id = stream_id;
				frame.sequence = sequence++;
				frame.control = std::move(*encoded);
				frame.payload.assign(payload.begin(), payload.end());
				auto wire = sdk::provider::encode_frame(frame);
				if (!wire)
					return sdk::unexpected(std::move(wire.error()));
				output.insert(output.end(), wire->begin(), wire->end());
				return {};
			};
			const auto manifest_chunk_bytes =
				std::min<std::size_t>(limits.maximum_chunk_payload_bytes, manifest_bytes.size());
			const auto manifest_chunks =
				(manifest_bytes.size() + manifest_chunk_bytes - 1U) / manifest_chunk_bytes;
			auto descriptor =
				protocol::source_closure_manifest_descriptor{protocol::manifest_kind::descriptor,
															 binding.session_id,
															 binding.task_id,
															 binding.task_v4_digest,
															 binding.closure_id,
															 binding.closure_digest,
															 binding.manifest_digest,
															 manifest_bytes.size(),
															 manifest_chunk_bytes,
															 manifest_chunks};
			if (auto result =
					append(sdk::provider::message_type::source_closure_manifest, descriptor, {});
				!result)
				return sdk::unexpected(std::move(result.error()));
			for (std::size_t index{}; index < manifest_chunks; ++index)
			{
				const auto offset = index * manifest_chunk_bytes;
				const auto count = std::min(manifest_chunk_bytes, manifest_bytes.size() - offset);
				auto control =
					protocol::source_closure_manifest_chunk{protocol::manifest_kind::chunk,
															binding.session_id,
															binding.task_id,
															binding.manifest_digest,
															index,
															offset,
															count};
				if (auto result =
						append(sdk::provider::message_type::source_closure_manifest,
							   control,
							   std::as_bytes(std::span{manifest_bytes.data() + offset, count}));
					!result)
					return sdk::unexpected(std::move(result.error()));
			}
			std::vector<source_closure_blob_receipt> receipts;
			receipts.reserve(snapshot.blobs.size());
			std::uint64_t total_bytes{};
			for (std::size_t ordinal{}; ordinal < snapshot.blobs.size(); ++ordinal)
			{
				const auto& blob = snapshot.blobs[ordinal];
				if (!blob.content || blob.content->size() != blob.size_bytes ||
					blob.size_bytes == 0U || blob.size_bytes > limits.maximum_blob_bytes)
					return sdk::unexpected(failure("source-closure.blob-invalid", "content"));
				const auto chunk_bytes =
					std::min<std::size_t>(limits.maximum_chunk_payload_bytes, blob.content->size());
				const auto chunks = (blob.content->size() + chunk_bytes - 1U) / chunk_bytes;
				auto blob_control = protocol::source_closure_blob_descriptor{binding.session_id,
																			 binding.task_id,
																			 binding.closure_digest,
																			 ordinal,
																			 blob.content_digest,
																			 blob.size_bytes,
																			 chunk_bytes,
																			 chunks};
				if (auto result =
						append(sdk::provider::message_type::source_closure_blob, blob_control, {});
					!result)
					return sdk::unexpected(std::move(result.error()));
				for (std::size_t index{}; index < chunks; ++index)
				{
					const auto offset = index * chunk_bytes;
					const auto count = std::min(chunk_bytes, blob.content->size() - offset);
					auto control = protocol::source_closure_chunk{binding.session_id,
																  binding.task_id,
																  ordinal,
																  blob.content_digest,
																  index,
																  offset,
																  count};
					if (auto result =
							append(sdk::provider::message_type::source_closure_chunk,
								   control,
								   std::as_bytes(std::span{blob.content->data() + offset, count}));
						!result)
						return sdk::unexpected(std::move(result.error()));
				}
				receipts.push_back({ordinal, blob.content_digest, blob.size_bytes});
				if (total_bytes > std::numeric_limits<std::uint64_t>::max() - blob.size_bytes)
					return sdk::unexpected(failure("source-closure.limit-exceeded", "blob-bytes"));
				total_bytes += blob.size_bytes;
			}
			auto receipt_digest = source_closure_blob_receipts_digest(receipts);
			if (!receipt_digest)
				return sdk::unexpected(std::move(receipt_digest.error()));
			auto expected = source_closure_transfer_digest(
				binding, *receipt_digest, receipts.size(), total_bytes);
			if (!expected || *expected != transfer)
				return sdk::unexpected(failure("source-closure.digest-mismatch", "transfer"));
			auto seal = protocol::source_closure_seal{binding.session_id,
													  binding.task_id,
													  binding.task_v4_digest,
													  binding.manifest_digest,
													  *receipt_digest,
													  receipts.size(),
													  total_bytes,
													  binding.closure_digest,
													  std::string{transfer}};
			if (auto result = append(sdk::provider::message_type::source_closure_seal, seal, {});
				!result)
				return sdk::unexpected(std::move(result.error()));
			return output;
		}

		struct channel_endpoints
		{
			int child_read{-1};
			int host_write{-1};
			int child_write{-1};
			int host_read{-1};

			channel_endpoints() = default;
			channel_endpoints(const channel_endpoints&) = delete;
			channel_endpoints& operator=(const channel_endpoints&) = delete;
			channel_endpoints(channel_endpoints&& other) noexcept
				: child_read{std::exchange(other.child_read, -1)},
				  host_write{std::exchange(other.host_write, -1)},
				  child_write{std::exchange(other.child_write, -1)},
				  host_read{std::exchange(other.host_read, -1)}
			{
			}
			channel_endpoints& operator=(channel_endpoints&& other) noexcept
			{
				if (this != &other)
				{
					for (const auto descriptor : {child_read, host_write, child_write, host_read})
						if (descriptor >= 0)
							(void)::close(descriptor);
					child_read = std::exchange(other.child_read, -1);
					host_write = std::exchange(other.host_write, -1);
					child_write = std::exchange(other.child_write, -1);
					host_read = std::exchange(other.host_read, -1);
				}
				return *this;
			}

			~channel_endpoints()
			{
				for (const auto descriptor : {child_read, host_write, child_write, host_read})
					if (descriptor >= 0)
						(void)::close(descriptor);
			}
		};

		[[nodiscard]] sdk::result<channel_endpoints> make_channels()
		{
			std::array<int, 2U> input{-1, -1};
			std::array<int, 2U> output{-1, -1};
			if (::socketpair(AF_UNIX, SOCK_STREAM, 0, input.data()) != 0 ||
				::socketpair(AF_UNIX, SOCK_STREAM, 0, output.data()) != 0)
			{
				for (const auto descriptor :
					 {input.at(0U), input.at(1U), output.at(0U), output.at(1U)})
					if (descriptor >= 0)
						(void)::close(descriptor);
				return sdk::unexpected(failure("provider.process-channel-invalid", "socketpair"));
			}
			channel_endpoints result;
			result.child_read = input.at(0U);
			result.host_write = input.at(1U);
			result.child_write = output.at(0U);
			result.host_read = output.at(1U);
			for (int* endpoint : {&result.child_read, &result.child_write, &result.host_read})
			{
				if (*endpoint < 4)
				{
					const auto duplicated = ::fcntl(*endpoint, F_DUPFD, 4);
					if (duplicated < 4)
						return sdk::unexpected(
							failure("provider.process-channel-invalid", "descriptor", "duplicate"));
					(void)::close(*endpoint);
					*endpoint = duplicated;
				}
				const auto flags = ::fcntl(*endpoint, F_GETFL);
				const auto descriptor_flags = ::fcntl(*endpoint, F_GETFD);
				if (flags < 0 || descriptor_flags < 0 ||
					::fcntl(*endpoint, F_SETFL, flags | O_NONBLOCK) != 0 ||
					::fcntl(*endpoint, F_SETFD, descriptor_flags & ~FD_CLOEXEC) != 0)
					return sdk::unexpected(
						failure("provider.process-channel-invalid", "descriptor", "flags"));
			}
			return std::move(result);
		}

		[[nodiscard]] sdk::result<void> write_all(const int descriptor,
												  const std::span<const std::byte> bytes)
		{
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count =
					::write(descriptor, bytes.data() + offset, bytes.size() - offset);
				if (count > 0)
				{
					offset += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
				{
					struct pollfd poll_value{descriptor, POLLOUT, 0};
					if (::poll(&poll_value, 1, 5000) > 0)
						continue;
				}
				return sdk::unexpected(failure("provider.process-channel-io", "write"));
			}
			return {};
		}

		void drain(const int descriptor) noexcept
		{
			std::array<std::byte, 4096U> buffer{};
			for (;;)
			{
				const auto count = ::read(descriptor, buffer.data(), buffer.size());
				if (count > 0)
					continue;
				if (count < 0 && errno == EINTR)
					continue;
				break;
			}
		}

		/**
		 * Measure the exact executable selected by the request before provider selection.
		 *
		 * The process runtime repeats this check at exec time, but selection must not be based on
		 * a caller-provided digest alone.  Keep the read bounded and reject replacement between
		 * the two identity observations; the measured digest is then the only value used to build
		 * the candidate and its sandbox evidence.
		 */
		// The selected absolute path and its expected digest form one ordered identity check.
		[[nodiscard]] sdk::result<std::string> measure_worker_executable(
			const std::string_view path, // NOLINT(bugprone-easily-swappable-parameters)
			const std::string_view expected)
		{
			if (path.empty() || path.front() != '/')
				return sdk::unexpected(
					failure("security.provider-path-invalid", "worker.executable"));
			const int descriptor = ::open(std::string{path}.c_str(), O_RDONLY | O_CLOEXEC);
			if (descriptor < 0)
				return sdk::unexpected(
					failure("security.provider-binary-unavailable", "worker.executable"));
			struct stat before{};
			if (::fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode) ||
				before.st_size < 0 || (before.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH)) == 0)
			{
				(void)::close(descriptor);
				return sdk::unexpected(
					failure("security.provider-binary-invalid", "worker.executable", "identity"));
			}
			constexpr std::uint64_t maximum_worker_bytes = std::uint64_t{256U} * 1024U * 1024U;
			const auto size = static_cast<std::uint64_t>(before.st_size);
			if (size == 0U || size > maximum_worker_bytes ||
				size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
			{
				(void)::close(descriptor);
				return sdk::unexpected(
					failure("security.provider-binary-invalid", "worker.executable", "size"));
			}
			std::vector<std::byte> bytes(static_cast<std::size_t>(size));
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count = ::read(descriptor, bytes.data() + offset, bytes.size() - offset);
				if (count > 0)
				{
					offset += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				(void)::close(descriptor);
				return sdk::unexpected(
					failure("security.provider-binary-unavailable", "worker.executable", "read"));
			}
			struct stat after{};
			const bool stable = ::fstat(descriptor, &after) == 0 && after.st_dev == before.st_dev &&
				after.st_ino == before.st_ino && after.st_size == before.st_size &&
				after.st_mtime == before.st_mtime && after.st_ctime == before.st_ctime;
			(void)::close(descriptor);
			if (!stable)
				return sdk::unexpected(
					failure("security.provider-binary-replaced", "worker.executable"));
			auto measured = sdk::content_digest(bytes);
			if (measured != expected)
				return sdk::unexpected(
					failure("security.provider-binary-mismatch", "worker.executable", "digest"));
			return measured;
		}

		[[nodiscard]] sdk::result<sdk::provider::manifest>
		make_manifest(const provider_task_v4_worker_authority& authority)
		{
			constexpr std::string_view semantic_prefix{"semantic-v2:"};
			sdk::provider::manifest manifest;
			manifest.provider_id = authority.provider_id;
			if (!manifest.provider_id.contains('.'))
				std::ranges::replace(manifest.provider_id, ':', '.');
			manifest.provider_version = authority.provider_version;
			manifest.package_identity = authority.provider_id + ".package";
			manifest.publisher = "cxxlens";
			manifest.license = "Apache-2.0";
			manifest.protocol = {authority.protocol_major,
								 authority.protocol_minor,
								 authority.protocol_minor,
								 authority.required_features,
								 {}};
			manifest.platform_tuples = {"linux-glibc"};
			manifest.provider_binary_digest = authority.installed_binary_digest;
			// The provider runtime identity uses the wire-level sha256 spelling, while task-v4
			// authority retains the semantic-v2 domain prefix.  Both spellings carry the same
			// digest; the envelope keeps the authority spelling for the worker-side cross-bind.
			manifest.provider_semantic_contract_digest =
				authority.semantic_contract_digest.starts_with(semantic_prefix)
				? authority.semantic_contract_digest.substr(semantic_prefix.size())
				: authority.semantic_contract_digest;
			manifest.offered_relations = {
				"cc.call_direct_target@1",
				"cc.call_site@1",
				"cc.entity@1",
				"frontend.clang22.call_observation@2",
				"frontend.clang22.entity_observation@2",
				"frontend.clang22.type_observation@2",
			};
			manifest.interpretation_domains = {"cc.clang22-canonical-1"};
			const auto invalidation_contract = std::string{
				"cxxlens.clang22.provider-invalidation.v2|source-closure|task-v4|sealed-output"};
			const auto determinism_contract = std::string{
				"cxxlens.clang22.provider-determinism.v2|canonical-cbor|ordered-six-batches"};
			manifest.invalidation_contract = sdk::content_digest(std::as_bytes(
				std::span{invalidation_contract.data(), invalidation_contract.size()}));
			manifest.determinism_contract = sdk::content_digest(
				std::as_bytes(std::span{determinism_contract.data(), determinism_contract.size()}));
			manifest.resource_class = "provider.clang22";
			manifest.sandbox_minimum = "enforced";
			manifest.requested_qualifications = {
				"canonical-semantic-qualified", "sandbox-qualified", "schema-conformant"};
			if (auto valid = manifest.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return manifest;
		}

		[[nodiscard]] sdk::result<sdk::relation_engine>
		make_materializer_relation_engine(const provider_task_v4_request_authority& authority)
		{
			sdk::relation_registry registry;
			const std::array<const sdk::relation_descriptor*, 12U> descriptors{
				&build::relations::project::descriptor(),
				&build::relations::toolchain_context::descriptor(),
				&build::relations::variant::descriptor(),
				&build::relations::compile_unit::descriptor(),
				&source::relations::file::descriptor(),
				&source::relations::span::descriptor(),
				&cc::relations::call_direct_target::descriptor(),
				&cc::relations::call_site::descriptor(),
				&cc::relations::entity::descriptor(),
				&materialization::call_observation_v2_descriptor(),
				&materialization::entity_observation_v2_descriptor(),
				&materialization::type_observation_v2_descriptor(),
			};
			for (const auto* descriptor : descriptors)
				if (auto added = registry.add(*descriptor); !added)
					return sdk::unexpected(std::move(added.error()));
			auto engine = registry.build(authority.engine.engine_generation_id);
			if (!engine)
				return sdk::unexpected(std::move(engine.error()));
			if (engine->registry_digest() != authority.engine.engine_registry_digest)
				return sdk::unexpected(failure(
					"materialization.relation-engine", "registry-digest", "authority-mismatch"));
			return std::move(*engine);
		}

		[[nodiscard]] sdk::result<materialization::source_closure_manifest>
		make_materializer_manifest(const source_closure_snapshot& snapshot)
		{
			auto digest = source_closure_manifest_digest(snapshot);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			materialization::source_closure_manifest manifest;
			manifest.closure_id = snapshot.snapshot_id;
			manifest.closure_digest = snapshot.closure_digest;
			manifest.manifest_digest = *digest;
			manifest.members.reserve(snapshot.members.size());
			for (const auto& member : snapshot.members)
				manifest.members.push_back({member.file_id,
											member.logical_path,
											role_name(member.role),
											encoding_name(member.encoding),
											member.size_bytes,
											member.content_digest,
											member.read_only});
			manifest.blobs.reserve(snapshot.blobs.size());
			for (const auto& blob : snapshot.blobs)
				manifest.blobs.push_back({blob.content_digest, blob.size_bytes});
			if (auto valid = manifest.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return manifest;
		}

		[[nodiscard]] sdk::result<std::vector<sdk::claim>>
		make_worker_assertions(const materializer_worker_execution& execution,
							   const sdk::relation_engine& engine,
							   const provider_task_v4_task_authority& task,
							   const std::string_view descriptor_id,
							   const materializer_basis_authority& basis)
		{
			if (!execution.outcome.sealed || !execution.outcome.runtime_receipt)
				return sdk::unexpected(failure("provider.transcript-invalid", "receipt"));
			const sdk::provider::detail::sealed_provider_batch* selected = nullptr;
			for (const auto& batch : execution.outcome.sealed->batches())
				if (batch.descriptor_id() == descriptor_id)
				{
					selected = &batch;
					break;
				}
			if (selected == nullptr)
				return sdk::unexpected(failure(
					"materialization.group-incomplete", "descriptor", std::string{descriptor_id}));
			std::vector<sdk::claim> output;
			output.reserve(selected->rows().size());
			for (const auto& row : selected->rows())
			{
				sdk::observation observation{
					row,
					{task.condition_universe_id, {task.condition_id}},
					task.interpretation_domain,
					{execution.ingress.request.authority.worker.provider_id,
					 execution.ingress.request.authority.worker.semantic_contract_digest},
					{basis.basis_digest},
					std::string{execution.outcome.runtime_receipt->sealed_transcript_digest()},
					basis.guarantee,
				};
				auto claim = sdk::make_assertion(engine, std::move(observation));
				if (!claim)
					return sdk::unexpected(std::move(claim.error()));
				output.push_back(std::move(*claim));
			}
			return output;
		}

		[[nodiscard]] sdk::result<materialization::materialization_v4_claim_sealed>
		make_worker_claim(const materializer_worker_execution& execution,
						  const sdk::relation_engine& engine,
						  const provider_task_v4_task_authority& task,
						  const sdk::detail::build_capture_draft& capture,
						  const provider_task_v4_base_task& base,
						  const provider_task_v4& extension,
						  const materialization::source_closure_manifest& manifest,
						  const std::string_view descriptor_id,
						  const materializer_basis_authority& basis,
						  const std::span<const sdk::claim> existing)
		{
			if (!execution.outcome.sealed || !execution.outcome.runtime_receipt)
				return sdk::unexpected(failure("provider.transcript-invalid", "receipt"));
			auto assertions = make_worker_assertions(execution, engine, task, descriptor_id, basis);
			if (!assertions)
				return sdk::unexpected(std::move(assertions.error()));

			materialization::materialization_v4_claim_binding binding;
			binding.materialization_request_id =
				execution.ingress.request.request.materialization_request_id;
			binding.task_index = extension.base_task_index;
			binding.base_task = base;
			binding.task = extension;
			binding.manifest = manifest;
			binding.provider_id = execution.ingress.request.authority.worker.provider_id;
			binding.provider_semantic_contract_digest =
				execution.ingress.request.authority.worker.semantic_contract_digest;
			binding.materializer_id = "cxxlens.clang22.materializer";
			binding.materializer_semantic_contract_digest = basis.materializer_semantics_digest;
			binding.canonical_adoption_transform_digest = basis.canonical_adoption_transform_digest;
			binding.base_ingestion_transform_digest = basis.base_ingestion_transform_digest;
			binding.direct_basis_digest = basis.basis_digest;
			binding.guarantee = basis.guarantee;
			binding.assumption_set_id = basis.assumption_set_id;
			binding.relation_descriptor_id = std::string{descriptor_id};
			// Store partitions are project-scoped; compile-unit identity remains in the
			// task association and coverage records.
			binding.scope = capture.project_id;
			binding.interpretation = task.interpretation_domain;
			binding.precision_profile = "exact";

			const bool canonical_stage = descriptor_id == "cc.call_direct_target.v1" ||
				descriptor_id == "cc.call_site.v1" || descriptor_id == "cc.entity.v1";
			std::vector<sdk::claim> adopted;
			adopted.reserve(assertions->size());
			for (const auto& assertion : *assertions)
			{
				if (canonical_stage)
				{
					auto canonical = sdk::make_canonical_claim(
						engine,
						assertion,
						{"cxxlens.clang22.materializer", basis.materializer_semantics_digest},
						assertion.row,
						basis.canonical_adoption_transform_digest);
					if (!canonical)
						return sdk::unexpected(std::move(canonical.error()));
					adopted.push_back(std::move(*canonical));
				}
				else
					adopted.push_back(assertion);
			}

			sdk::claim_batch claim_batch;
			for (auto& claim : adopted)
			{
				if (auto added = claim_batch.add(std::move(claim)); !added)
					return sdk::unexpected(std::move(added.error()));
			}
			auto committed = std::move(claim_batch).commit(engine, existing);
			if (!committed)
				return sdk::unexpected(std::move(committed.error()));
			std::string partition_basis = basis.producer_input_basis_digest;
			if (!committed->claims.empty())
			{
				auto claim_basis =
					sdk::claim_input_basis_digest(committed->claims.front().input_basis);
				if (!claim_basis)
					return sdk::unexpected(std::move(claim_basis.error()));
				partition_basis = std::move(*claim_basis);
			}

			materialization::materialization_v4_claim_translation translation{
				binding,
				std::move(*committed),
				{binding.relation_descriptor_id,
				 binding.scope,
				 {task.condition_universe_id, {task.condition_id}},
				 binding.interpretation,
				 canonical_stage ? binding.materializer_semantic_contract_digest
								 : binding.provider_semantic_contract_digest,
				 partition_basis,
				 binding.precision_profile,
				 binding.assumption_set_id,
				 {},
				 {{"compile-unit", binding.scope, "covered", {}}},
				 {}}};
			translation.partition.claims = translation.batch.claims;
			translation.partition.unresolved = translation.batch.unresolved;
			return materialization::seal_materialization_v4_claim_translation(
				sdk::relation_engine{engine}, std::move(translation), existing);
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		make_worker_envelope(const installed_materializer_source_closure_result& ingress,
							 const std::size_t task_index,
							 const sdk::provider::manifest& manifest,
							 const std::uint64_t stream_id,
							 const std::string_view transfer)
		{
			const auto& request = ingress.request.request;
			const auto& authority = ingress.request.authority;
			if (task_index >= request.base_tasks.size() || task_index >= authority.tasks.size() ||
				task_index >= ingress.request.build_captures.size())
				return sdk::unexpected(failure("provider.worker-v4-input-invalid", "task-index"));
			const auto& base = request.base_tasks[task_index];
			const auto& extension = request.task_extensions[task_index];
			const auto& task = authority.tasks[task_index];
			const auto& validated_capture = ingress.request.build_captures[task_index];
			if (auto valid = materialization::validate_provider_task_v4_build_capture_binding(
					task, validated_capture);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& capture = validated_capture.value();
			const auto& invocation = capture.invocation;
			if (extension.source_closure.source_closure_id != ingress.binding.closure_id)
				return sdk::unexpected(
					failure("source-closure.task-binding-mismatch", "task", "closure"));
			auto base_value = base_task_value(base);
			if (!base_value)
				return sdk::unexpected(std::move(base_value.error()));
			const auto base_text = materialization::canonical_json(*base_value);
			const auto base_digest =
				sdk::content_digest(std::as_bytes(std::span{base_text.data(), base_text.size()}));
			if (base_digest != extension.base_task_digest)
				return sdk::unexpected(
					failure("source-closure.task-v4-binding-mismatch", "base-task-projection"));
			source_closure_task_v4_input input;
			input.base_task_index = extension.base_task_index;
			input.base_provider_task_id = extension.base_provider_task_id;
			input.base_task_projection = std::vector<std::byte>{
				std::as_bytes(std::span{base_text.data(), base_text.size()}).begin(),
				std::as_bytes(std::span{base_text.data(), base_text.size()}).end()};
			input.task_input_digest = task.task_input_digest;
			input.normalized_invocation_digest = invocation.effective_invocation_digest;
			input.toolchain_digest = capture.toolchain_digest;
			input.environment_digest = invocation.environment_digest;
			input.closure = ingress.receiver.snapshot;
			input.main_logical_path = extension.main_logical_path;
			input.logical_working_directory = extension.logical_working_directory;
			auto identity = derive_source_closure_task_v4_identity(input);
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			if (identity->task_id != extension.task_id ||
				identity->task_v4_digest != extension.task_v4_digest ||
				identity->base_task_digest != extension.base_task_digest)
				return sdk::unexpected(
					failure("source-closure.task-v4-binding-mismatch", "identity"));
			auto task_document = materialization::parse_json_object(
				std::string{reinterpret_cast<const char*>(identity->input_payload.data()),
							identity->input_payload.size()});
			if (!task_document)
				return sdk::unexpected(std::move(task_document.error()));
			auto authority_value = object({
				{"effective_arguments",
				 array_strings(*invocation.effective_replay_arguments.value).value()},
				{"logical_working_directory", text(invocation.logical_working_directory).value()},
				{"normalized_invocation_digest",
				 text(invocation.effective_invocation_digest).value()},
				{"qualified_read_roots", array_strings(invocation.qualified_read_roots).value()},
			});
			auto output_value = worker_output_value(task,
													capture,
													manifest,
													authority.worker.semantic_contract_digest,
													output_descriptors());
			if (!authority_value || !output_value)
				return sdk::unexpected(!authority_value ? std::move(authority_value.error())
														: std::move(output_value.error()));
			auto base_document = materialization::parse_json_object(base_text);
			if (!base_document)
				return sdk::unexpected(std::move(base_document.error()));
			auto closure_value = closure_binding_value(ingress.binding, transfer, stream_id);
			if (!closure_value)
				return sdk::unexpected(std::move(closure_value.error()));
			auto root = object({
				{"base_task_projection", base_document->root()},
				{"closure_binding", std::move(*closure_value)},
				{"expected_base_task_digest", text(extension.base_task_digest).value()},
				{"expected_task_v4_input_digest", text(identity->task_v4_input_digest).value()},
				{"input_authority", std::move(*authority_value)},
				{"output_authority", std::move(*output_value)},
				{"stream_id", json_value::unsigned_integer(stream_id)},
				{"task_v4_payload", task_document->root()},
				{"schema", text("cxxlens.clang22.worker-ingress.v4").value()},
			});
			if (!root)
				return sdk::unexpected(std::move(root.error()));
			const auto encoded = materialization::canonical_json(*root);
			return std::vector<std::byte>{
				std::as_bytes(std::span{encoded.data(), encoded.size()}).begin(),
				std::as_bytes(std::span{encoded.data(), encoded.size()}).end()};
		}

		[[nodiscard]] sdk::canonical_value
		receipt_projection(const materialization::materialization_v4_claim_receipt& value)
		{
			return sdk::canonical_value::from_tuple(std::vector<sdk::canonical_value>{
				sdk::canonical_value::from_string(value.schema),
				sdk::canonical_value::from_string(value.binding_digest),
				sdk::canonical_value::from_string(value.materialization_request_id),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_index)),
				sdk::canonical_value::from_string(value.task_id),
				sdk::canonical_value::from_string(value.task_v4_digest),
				sdk::canonical_value::from_string(value.provider_execution_id),
				sdk::canonical_value::from_string(value.source_closure_id),
				sdk::canonical_value::from_string(value.source_closure_digest),
				sdk::canonical_value::from_string(value.manifest_digest),
				sdk::canonical_value::from_string(value.task_input_digest),
				sdk::canonical_value::from_string(value.claim_batch_content_digest),
				sdk::canonical_value::from_string(value.partition_id),
				sdk::canonical_value::from_string(value.partition_content_digest),
				sdk::canonical_value::from_string(value.coverage_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.claim_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.conflict_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				sdk::canonical_value::from_boolean(value.complete),
				sdk::canonical_value::from_string(value.receipt_digest),
			});
		}

		[[nodiscard]] sdk::result<std::string>
		output_receipt_digest(const materializer_task_output_receipt& value)
		{
			if (value.batch_receipts.size() != task_v4_output_descriptor_ids.size())
				return sdk::unexpected(
					failure("materialization.group-incomplete", "receipt.batch-count"));
			std::vector<sdk::canonical_value> batches;
			batches.reserve(value.batch_receipts.size());
			for (const auto& receipt : value.batch_receipts)
				batches.push_back(receipt_projection(receipt));
			const std::vector<sdk::canonical_value> fields{
				sdk::canonical_value::from_string(value.schema),
				sdk::canonical_value::from_string(value.materialization_request_id),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.task_index)),
				sdk::canonical_value::from_string(value.task_id),
				sdk::canonical_value::from_string(value.task_v4_digest),
				sdk::canonical_value::from_tuple(std::move(batches)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.claim_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.unresolved_count)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.conflict_count)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.differential_disagreement_count)),
				sdk::canonical_value::from_boolean(value.complete),
			};
			return sdk::canonical_identity_digest(
				"cxxlens.clang22.materializer-task-output-receipt.v4", fields);
		}

		[[nodiscard]] sdk::result<materializer_task_output_receipt> make_task_output_receipt(
			const sdk::relation_engine& engine,
			const std::vector<materialization::materialization_v4_claim_sealed>& claims,
			const std::span<const sdk::claim> existing)
		{
			if (claims.size() != task_v4_output_descriptor_ids.size())
				return sdk::unexpected(
					failure("materialization.group-incomplete", "descriptor-count"));
			materializer_task_output_receipt output;
			output.batch_receipts.reserve(claims.size());
			for (std::size_t index{}; index < claims.size(); ++index)
			{
				const auto& claim = claims[index];
				if (auto valid = materialization::validate_materialization_v4_claim_receipt(
						engine, claim, existing);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				if (claim.translation.binding.relation_descriptor_id !=
					task_v4_output_descriptor_ids.at(index))
					return sdk::unexpected(failure(
						"materialization.descriptor-binding-mismatch", "descriptor", "order"));
				if (index == 0U)
				{
					output.materialization_request_id = claim.receipt.materialization_request_id;
					output.task_index = claim.receipt.task_index;
					output.task_id = claim.receipt.task_id;
					output.task_v4_digest = claim.receipt.task_v4_digest;
				}
				else if (claim.receipt.materialization_request_id !=
							 output.materialization_request_id ||
						 claim.receipt.task_index != output.task_index ||
						 claim.receipt.task_id != output.task_id ||
						 claim.receipt.task_v4_digest != output.task_v4_digest)
					return sdk::unexpected(
						failure("materialization.task-binding-mismatch", "descriptor", "task"));
				output.batch_receipts.push_back(claim.receipt);
				const auto add = [&](std::uint64_t& total,
									 const std::uint64_t value,
									 const std::string_view field) -> sdk::result<void>
				{
					if (value > std::numeric_limits<std::uint64_t>::max() - total)
						return sdk::unexpected(
							failure("materialization.group-overflow", std::string{field}));
					total += value;
					return {};
				};
				for (const auto [field, value] :
					 {std::pair{std::string_view{"claim-count"}, claim.receipt.claim_count},
					  std::pair{std::string_view{"unresolved-count"},
								claim.receipt.unresolved_count},
					  std::pair{std::string_view{"conflict-count"}, claim.receipt.conflict_count},
					  std::pair{std::string_view{"differential-count"},
								claim.receipt.differential_disagreement_count}})
				{
					auto& total = field == "claim-count" ? output.claim_count
						: field == "unresolved-count"	 ? output.unresolved_count
						: field == "conflict-count"		 ? output.conflict_count
														 : output.differential_disagreement_count;
					if (auto added = add(total, value, field); !added)
						return sdk::unexpected(std::move(added.error()));
				}
				output.complete = index == 0U ? claim.receipt.complete
											  : output.complete && claim.receipt.complete;
			}
			auto digest = output_receipt_digest(output);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			output.receipt_digest = std::move(*digest);
			return output;
		}

		[[nodiscard]] sdk::detached_cell
		base_symbol_cell(const sdk::scalar_kind kind, std::string parameter, std::string value)
		{
			return {{kind, std::move(parameter), false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::detached_cell base_optional_typed_cell(std::string parameter,
																  std::string value)
		{
			auto output = sdk::detached_cell::typed(std::move(parameter), std::move(value));
			output.type.optional = true;
			return output;
		}

		[[nodiscard]] sdk::detached_cell base_digest_cell(std::string value)
		{
			return {{sdk::scalar_kind::digest, {}, false},
					sdk::cell_state::present,
					sdk::scalar_value{std::move(value)},
					std::nullopt};
		}

		[[nodiscard]] sdk::result<sdk::claim>
		make_base_claim(const sdk::relation_engine& engine,
						const sdk::detached_row& row,
						const provider_task_v4_task_authority& task,
						const materializer_worker_execution& execution,
						const materializer_basis_authority& basis)
		{
			if (!execution.outcome.runtime_receipt)
				return sdk::unexpected(
					failure("materialization.transcript-invalid", "runtime-receipt"));
			sdk::observation observation{
				row,
				{task.condition_universe_id, {task.condition_id}},
				task.interpretation_domain,
				{"cxxlens.clang22.materializer", basis.materializer_semantics_digest},
				{basis.basis_digest},
				std::string{execution.outcome.runtime_receipt->sealed_transcript_digest()},
				basis.guarantee,
			};
			return sdk::make_assertion(engine, std::move(observation));
		}

		[[nodiscard]] sdk::result<sdk::partition_draft>
		make_base_partition(const sdk::relation_engine& engine,
							const std::string_view relation_descriptor_id,
							const std::vector<sdk::detached_row>& rows,
							const provider_task_v4_task_authority& task,
							const sdk::detail::build_capture_draft& capture,
							const materializer_worker_execution& execution,
							const materializer_basis_authority& basis,
							std::vector<sdk::claim>* reference_claims)
		{
			if (rows.empty())
				return sdk::unexpected(failure("materialization.base-partition-empty",
											   "relation",
											   std::string{relation_descriptor_id}));
			sdk::partition_draft output;
			output.relation_descriptor_id = std::string{relation_descriptor_id};
			output.scope = capture.project_id;
			output.condition = {task.condition_universe_id, {task.condition_id}};
			output.interpretation = task.interpretation_domain;
			output.producer_semantics = basis.materializer_semantics_digest;
			output.precision_profile = "exact";
			output.assumption_set_id = basis.assumption_set_id;
			output.coverage = {{"compile-unit", capture.compile_unit_id, "covered", {}}};
			output.claims.reserve(rows.size());
			for (const auto& row : rows)
			{
				if (row.descriptor_id != relation_descriptor_id)
					return sdk::unexpected(failure(
						"materialization.base-partition-invalid", "descriptor", row.descriptor_id));
				auto claim = make_base_claim(engine, row, task, execution, basis);
				if (!claim)
					return sdk::unexpected(std::move(claim.error()));
				if (reference_claims != nullptr)
					reference_claims->push_back(*claim);
				auto canonical = sdk::make_canonical_claim(
					engine,
					*claim,
					{"cxxlens.clang22.materializer", basis.materializer_semantics_digest},
					row,
					basis.base_ingestion_transform_digest);
				if (!canonical)
					return sdk::unexpected(std::move(canonical.error()));
				output.claims.push_back(std::move(*canonical));
			}
			auto claim_basis = sdk::claim_input_basis_digest(output.claims.front().input_basis);
			if (!claim_basis)
				return sdk::unexpected(std::move(claim_basis.error()));
			output.producer_input_basis_digest = std::move(*claim_basis);
			return output;
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_project_row(const sdk::detail::build_capture_draft& capture)
		{
			using relation = build::relations::project;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::project_column>(
						 sdk::detached_cell::typed("project_id", capture.project_id)),
					 builder.set<relation::catalog>(
						 sdk::detached_cell::typed("catalog_id", capture.catalog.catalog_id)),
					 builder.set<relation::catalog_digest>(
						 base_digest_cell(capture.catalog.catalog_digest)),
					 builder.set<relation::logical_root>(sdk::detached_cell::typed(
						 "logical_path_id", capture.catalog.logical_root)),
					 builder.set<relation::environment_digest>(
						 base_digest_cell(capture.catalog.environment_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_toolchain_row(const sdk::detail::build_capture_draft& capture)
		{
			using relation = build::relations::toolchain_context;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::toolchain>(sdk::detached_cell::typed(
						 "toolchain_context_id", capture.toolchain_context_id)),
					 builder.set<relation::family>(base_symbol_cell(sdk::scalar_kind::open_symbol,
																	"build.toolchain-family/1",
																	capture.toolchain.family)),
					 builder.set<relation::exact_version>(
						 sdk::detached_cell::utf8(capture.toolchain.exact_version)),
					 builder.set<relation::target_triple>(
						 sdk::detached_cell::utf8(capture.toolchain.target_triple)),
					 builder.set<relation::builtin_headers_digest>(
						 base_digest_cell(capture.toolchain.builtin_headers_digest)),
					 builder.set<relation::abi_digest>(
						 base_digest_cell(capture.toolchain.abi_digest)),
					 builder.set<relation::plugin_spec_digest>(
						 base_digest_cell(capture.toolchain.plugin_spec_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			if (capture.toolchain.sysroot)
			{
				auto result = builder.set<relation::sysroot>(
					base_optional_typed_cell("logical_path_id", *capture.toolchain.sysroot));
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			}
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_variant_row(const sdk::detail::build_capture_draft& capture)
		{
			using relation = build::relations::variant;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::variant_column>(
						 sdk::detached_cell::typed("build_variant_id", capture.build_variant_id)),
					 builder.set<relation::project>(
						 sdk::detached_cell::typed("project_id", capture.project_id)),
					 builder.set<relation::toolchain>(sdk::detached_cell::typed(
						 "toolchain_context_id", capture.toolchain_context_id)),
					 builder.set<relation::language>(base_symbol_cell(sdk::scalar_kind::open_symbol,
																	  "build.language/1",
																	  capture.variant.language)),
					 builder.set<relation::language_standard>(
						 base_symbol_cell(sdk::scalar_kind::open_symbol,
										  "build.language-standard/1",
										  capture.variant.language_standard)),
					 builder.set<relation::target_triple>(
						 sdk::detached_cell::utf8(capture.variant.target_triple)),
					 builder.set<relation::predefined_macros_digest>(
						 base_digest_cell(capture.variant.predefined_macros_digest)),
					 builder.set<relation::include_search_digest>(
						 base_digest_cell(capture.variant.include_search_digest)),
					 builder.set<relation::semantic_flags_digest>(
						 base_digest_cell(capture.variant.semantic_flags_digest)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_source_file_row(const sdk::detail::build_capture_draft& capture)
		{
			using relation = source::relations::file;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::snapshot>(sdk::detached_cell::typed(
						 "source_snapshot_id", capture.source.source_snapshot_id)),
					 builder.set<relation::file_column>(
						 sdk::detached_cell::typed("file_id", capture.source.file_id)),
					 builder.set<relation::project>(
						 sdk::detached_cell::typed("project_id", capture.project_id)),
					 builder.set<relation::logical_path>(
						 sdk::detached_cell::typed("logical_path_id", capture.source.logical_path)),
					 builder.set<relation::content>(
						 base_digest_cell(capture.source.content_digest)),
					 builder.set<relation::size>(
						 sdk::detached_cell::unsigned_integer(capture.source.size_bytes)),
					 builder.set<relation::encoding>(base_symbol_cell(sdk::scalar_kind::open_symbol,
																	  "source.encoding/1",
																	  capture.source.encoding)),
					 builder.set<relation::line_index>(
						 sdk::detached_cell::typed("line_index_id", capture.source.line_index_id)),
					 builder.set<relation::read_only>(
						 sdk::detached_cell::boolean(capture.source.read_only)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_compile_unit_row(const sdk::detail::build_capture_draft& capture)
		{
			using relation = build::relations::compile_unit;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::compile_unit_column>(
						 sdk::detached_cell::typed("compile_unit_id", capture.compile_unit_id)),
					 builder.set<relation::project>(
						 sdk::detached_cell::typed("project_id", capture.project_id)),
					 builder.set<relation::main_source>(sdk::detached_cell::typed(
						 "source_snapshot_id", capture.source.source_snapshot_id)),
					 builder.set<relation::variant>(
						 sdk::detached_cell::typed("build_variant_id", capture.build_variant_id)),
					 builder.set<relation::toolchain>(sdk::detached_cell::typed(
						 "toolchain_context_id", capture.toolchain_context_id)),
					 builder.set<relation::effective_invocation_digest>(
						 base_digest_cell(capture.invocation.effective_invocation_digest)),
					 builder.set<relation::language>(base_symbol_cell(sdk::scalar_kind::open_symbol,
																	  "build.language/1",
																	  capture.invocation.language)),
					 builder.set<relation::working_directory>(sdk::detached_cell::typed(
						 "logical_path_id", capture.invocation.logical_working_directory)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<sdk::detached_row>
		make_base_source_span_row(const materialization::observation_v2_primary_span& value)
		{
			using relation = source::relations::span;
			relation::builder builder;
			for (auto result : {
					 builder.set<relation::span_column>(
						 sdk::detached_cell::typed("source_span_id", value.span_id)),
					 builder.set<relation::snapshot>(
						 sdk::detached_cell::typed("source_snapshot_id", value.snapshot)),
					 builder.set<relation::file>(sdk::detached_cell::typed("file_id", value.file)),
					 builder.set<relation::begin>(
						 sdk::detached_cell::unsigned_integer(value.begin)),
					 builder.set<relation::end>(sdk::detached_cell::unsigned_integer(value.end)),
					 builder.set<relation::role>(base_symbol_cell(
						 sdk::scalar_kind::open_symbol, "source.range-role/1", value.role)),
					 builder.set<relation::read_only>(sdk::detached_cell::boolean(value.read_only)),
				 })
				if (!result)
					return sdk::unexpected(std::move(result.error()));
			return std::move(builder).finish();
		}

		[[nodiscard]] sdk::result<std::vector<sdk::partition_draft>>
		make_base_partitions(const sdk::relation_engine& engine,
							 const provider_task_v4_task_authority& task,
							 const sdk::detail::build_capture_draft& capture,
							 const materializer_worker_execution& execution,
							 const std::span<const sdk::claim> claims,
							 const materializer_basis_authority& basis,
							 std::vector<sdk::claim>* reference_claims)
		{
			std::vector<sdk::partition_draft> output;
			output.reserve(task_v4_base_descriptor_ids.size());
			auto add = [&](const std::string_view descriptor_id,
						   sdk::result<sdk::detached_row> row) -> sdk::result<void>
			{
				if (!row)
					return sdk::unexpected(std::move(row.error()));
				std::vector<sdk::detached_row> rows;
				rows.push_back(std::move(*row));
				auto partition = make_base_partition(
					engine, descriptor_id, rows, task, capture, execution, basis, reference_claims);
				if (!partition)
					return sdk::unexpected(std::move(partition.error()));
				output.push_back(std::move(*partition));
				return {};
			};
			if (auto value =
					add(build::relations::project::descriptor().id, make_base_project_row(capture));
				!value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value = add(build::relations::toolchain_context::descriptor().id,
								 make_base_toolchain_row(capture));
				!value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value =
					add(build::relations::variant::descriptor().id, make_base_variant_row(capture));
				!value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value = add(source::relations::file::descriptor().id,
								 make_base_source_file_row(capture));
				!value)
				return sdk::unexpected(std::move(value.error()));
			if (auto value = add(build::relations::compile_unit::descriptor().id,
								 make_base_compile_unit_row(capture));
				!value)
				return sdk::unexpected(std::move(value.error()));

			std::map<std::string, materialization::observation_v2_primary_span, std::less<>> spans;
			const materialization::observation_v2_task_authority observation_authority{
				capture.compile_unit_id,
				capture.source.source_snapshot_id,
				capture.source.file_id,
				capture.source.size_bytes};
			for (const auto& claim : claims)
			{
				if (claim.descriptor != materialization::entity_observation_v2_descriptor().id &&
					claim.descriptor != materialization::call_observation_v2_descriptor().id)
					continue;
				auto decoded =
					materialization::decode_observation_v2_row(claim.row, observation_authority);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				if (!decoded->primary_span)
					continue;
				auto [it, inserted] =
					spans.emplace(decoded->primary_span->span_id, *decoded->primary_span);
				if (!inserted && it->second != *decoded->primary_span)
					return sdk::unexpected(
						failure("materialization.source-span-conflict", "span", it->first));
			}
			if (!spans.empty())
			{
				std::vector<sdk::detached_row> rows;
				rows.reserve(spans.size());
				for (const auto& [id, span] : spans)
				{
					(void)id;
					auto row = make_base_source_span_row(span);
					if (!row)
						return sdk::unexpected(std::move(row.error()));
					rows.push_back(std::move(*row));
				}
				auto partition = make_base_partition(engine,
													 source::relations::span::descriptor().id,
													 rows,
													 task,
													 capture,
													 execution,
													 basis,
													 reference_claims);
				if (!partition)
					return sdk::unexpected(std::move(partition.error()));
				output.push_back(std::move(*partition));
			}
			return output;
		}
	} // namespace

	sdk::result<void> materializer_task_output_receipt::validate() const
	{
		if (schema != "cxxlens.clang22.materializer-task-output-receipt.v4" ||
			batch_receipts.size() != task_v4_output_descriptor_ids.size())
			return sdk::unexpected(
				failure("materialization.receipt-invalid", "task-output", "shape"));
		for (const auto [field, value] :
			 {std::pair{std::string_view{"request"}, std::string_view{materialization_request_id}},
			  std::pair{std::string_view{"task"}, std::string_view{task_id}},
			  std::pair{std::string_view{"task-v4"}, std::string_view{task_v4_digest}},
			  std::pair{std::string_view{"receipt"}, std::string_view{receipt_digest}}})
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(
					failure("materialization.receipt-invalid", std::string{field}, "strong-id"));
		auto expected = output_receipt_digest(*this);
		if (!expected || *expected != receipt_digest)
			return sdk::unexpected(
				failure("materialization.receipt-invalid", "task-output", "digest"));
		return {};
	}

	sdk::result<materializer_worker_execution>
	run_materializer_worker(installed_materializer_source_closure_result ingress)
	{
		production_provider_trust_issuer issuer;
		return run_materializer_worker(std::move(ingress), issuer);
	}

	sdk::result<materializer_worker_execution>
	run_materializer_worker(installed_materializer_source_closure_result ingress,
							provider_trust_issuer_port& issuer)
	{
		if (ingress.request.authority.tasks.empty() || ingress.request.build_captures.empty() ||
			ingress.request.authority.tasks.size() != ingress.request.build_captures.size())
			return sdk::unexpected(failure("provider.worker-v4-input-invalid", "tasks", "empty"));
		const auto& task = ingress.request.authority.tasks.front();
		const auto& capture = ingress.request.build_captures.front().value();
		auto manifest = make_manifest(ingress.request.authority.worker);
		if (!manifest)
			return sdk::unexpected(std::move(manifest.error()));
		const auto stream_id = ingress.receiver.stream_id;
		const auto transfer = ingress.receiver.credentials.transfer_digest;
		auto closure =
			closure_transcript(ingress.binding, ingress.receiver.snapshot, transfer, stream_id);
		if (!closure)
			return sdk::unexpected(std::move(closure.error()));
		auto envelope = make_worker_envelope(ingress, 0U, *manifest, stream_id, transfer);
		if (!envelope)
			return sdk::unexpected(std::move(envelope.error()));
		auto channels = make_channels();
		if (!channels)
			return sdk::unexpected(std::move(channels.error()));
		auto binding = sdk::provider::detail::make_process_inherited_channel_binding(
			channels->child_read,
			channels->child_write,
			ingress.binding.task_id,
			ingress.binding.session_id,
			ingress.binding.task_v4_digest,
			ingress.binding.closure_id,
			ingress.binding.closure_digest,
			ingress.binding.manifest_digest,
			transfer,
			stream_id,
			ingress.receiver.first_sequence);
		if (!binding)
			return sdk::unexpected(std::move(binding.error()));
		sdk::provider::provider_candidate candidate;
		candidate.description = *manifest;
		candidate.source = sdk::provider::discovery_source::explicit_path;
		candidate.executable_argv = {ingress.request.authority.worker.executable};
		candidate.authoritative_path = true;
		auto measured_worker =
			measure_worker_executable(ingress.request.authority.worker.executable,
									  ingress.request.authority.worker.installed_binary_digest);
		if (!measured_worker)
			return sdk::unexpected(std::move(measured_worker.error()));
		auto issuance = issuer.issue(
			candidate.description, *measured_worker, ingress.request.authority.trust_policy);
		if (!issuance)
			return sdk::unexpected(std::move(issuance.error()));
		if (auto valid =
				issuance->validate(candidate.description.provider_id,
								   candidate.description.provider_version,
								   *measured_worker,
								   ingress.request.authority.trust_policy.required_qualification);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		candidate.trust_valid = issuance->trust_valid;
		candidate.certified_qualifications = std::move(issuance->certified_qualifications);
		candidate.certification_valid = issuance->certification_valid;
		auto policy = sdk::provider::resolve_sandbox_policy(
			ingress.request.authority.worker.sandbox_policy_digest);
		if (!policy)
			return sdk::unexpected(std::move(policy.error()));
		if (policy->policy_digest() != ingress.request.authority.worker.sandbox_policy_digest)
			return sdk::unexpected(
				failure("security.sandbox-policy-mismatch", "worker.sandbox_policy_digest"));
		auto sandbox_evidence =
			sdk::provider::sandbox_evidence_digest(*policy,
												   task.budget,
												   sdk::provider::sandbox_assurance::enforced,
												   policy->mechanisms,
												   *measured_worker);
		if (!sandbox_evidence)
			return sdk::unexpected(std::move(sandbox_evidence.error()));
		candidate.sandbox = {"linux-glibc",
							 policy->mechanisms,
							 sdk::provider::sandbox_assurance::enforced,
							 ingress.request.authority.worker.sandbox_policy_digest,
							 *sandbox_evidence};
		sdk::provider::provider_selection_request selection_request{
			candidate.description.provider_id,
			candidate.description.provider_version,
			candidate.description.provider_binary_digest,
			candidate.description.provider_semantic_contract_digest,
			{sdk::provider::sandbox_assurance::enforced,
			 ingress.request.authority.worker.sandbox_policy_digest},
			true,
			std::nullopt};
		auto selection =
			sdk::provider::select_provider(selection_request, std::span{&candidate, 1U});
		if (!selection)
			return sdk::unexpected(std::move(selection.error()));
		const auto descriptors = output_descriptors();
		std::vector<sdk::relation_descriptor> descriptor_values;
		descriptor_values.reserve(descriptors.size());
		for (const auto* descriptor : descriptors)
			descriptor_values.push_back(*descriptor);
		const auto envelope_digest = sdk::content_digest(*envelope);
		auto engine = make_materializer_relation_engine(ingress.request.authority);
		if (!engine)
			return sdk::unexpected(std::move(engine.error()));
		auto basis = make_materializer_basis_authority(ingress.request.authority,
													   ingress.request.build_captures);
		if (!basis)
			return sdk::unexpected(std::move(basis.error()));
		auto generic_task = make_generic_materialization_task(
			ingress, *engine, *basis, *measured_worker, envelope_digest);
		if (!generic_task)
			return sdk::unexpected(std::move(generic_task.error()));
		sdk::provider::process_task_request request;
		request.selection = std::move(*selection);
		request.output_descriptors = std::move(descriptor_values);
		request.task_id = ingress.binding.task_id;
		request.payload = std::move(*envelope);
		request.task_input_digest = envelope_digest;
		request.normalized_invocation_digest = capture.invocation.effective_invocation_digest;
		request.toolchain_digest = capture.toolchain_digest;
		request.environment_digest = capture.invocation.environment_digest;
		request.sandbox = {sdk::provider::sandbox_assurance::enforced,
						   ingress.request.authority.worker.sandbox_policy_digest};
		request.budget = task.budget;
		request.limits.protocol_major = ingress.request.request.protocol_major;
		request.limits.minimum_minor = ingress.request.request.protocol_minor;
		request.limits.maximum_minor = ingress.request.request.protocol_minor;
		request.output_credit = {std::uint64_t{64U} * 1024U * 1024U, 65536U};
		request.inherited_channel = std::move(*binding);
		const auto sender_bytes = std::move(*closure);
		const auto sender_descriptor = channels->host_write;
		std::thread sender(
			[sender_descriptor, sender_bytes]()
			{
				(void)write_all(sender_descriptor, sender_bytes);
				(void)::close(sender_descriptor);
			});
		auto process_port = sdk::provider::make_system_provider_process_port();
		if (!process_port)
		{
			sender.join();
			return sdk::unexpected(failure("provider.runtime-unavailable", "process-port"));
		}
		auto outcome = sdk::provider::detail::execute_provider_process(*process_port, request);
		sender.join();
		drain(channels->host_read);
		if (!outcome)
			return sdk::unexpected(std::move(outcome.error()));
		if (!outcome->succeeded())
			return sdk::unexpected(
				failure("provider.transcript-invalid", "worker", outcome->terminal));
		return materializer_worker_execution{std::move(ingress),
											 std::move(*generic_task),
											 std::move(*outcome),
											 std::move(*issuance)};
	}

	sdk::result<materializer_store_execution>
	publish_materializer_worker(materializer_worker_execution execution)
	{
		if (!execution.outcome.succeeded() || !execution.outcome.sealed ||
			!execution.outcome.runtime_receipt)
			return sdk::unexpected(failure("materialization.transcript-invalid", "worker"));
		auto& request = execution.ingress.request.request;
		auto& authority = execution.ingress.request.authority;
		if (request.task_extensions.size() != 1U || authority.tasks.size() != 1U ||
			execution.ingress.request.build_captures.size() != 1U)
			return sdk::unexpected(
				failure("materialization.task-census-invalid", "tasks", "one-task-ingress"));
		auto engine = make_materializer_relation_engine(authority);
		if (!engine)
			return sdk::unexpected(std::move(engine.error()));
		auto manifest = make_materializer_manifest(execution.ingress.receiver.snapshot);
		if (!manifest)
			return sdk::unexpected(std::move(manifest.error()));
		const auto& base = request.base_tasks.front();
		const auto& extension = request.task_extensions.front();
		const auto& task = authority.tasks.front();
		const auto& validated_capture = execution.ingress.request.build_captures.front();
		if (auto valid = materialization::validate_provider_task_v4_build_capture_binding(
				task, validated_capture);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto& capture = validated_capture.value();
		auto basis =
			make_materializer_basis_authority(authority, execution.ingress.request.build_captures);
		if (!basis)
			return sdk::unexpected(std::move(basis.error()));
		std::vector<sdk::claim> raw_output_claims;
		for (const auto descriptor_id : task_v4_output_descriptor_ids)
		{
			auto assertions =
				make_worker_assertions(execution, *engine, task, descriptor_id, *basis);
			if (!assertions)
				return sdk::unexpected(std::move(assertions.error()));
			raw_output_claims.insert(raw_output_claims.end(),
									 std::make_move_iterator(assertions->begin()),
									 std::make_move_iterator(assertions->end()));
		}
		std::vector<sdk::claim> base_reference_claims;
		auto base_partitions = make_base_partitions(
			*engine, task, capture, execution, raw_output_claims, *basis, &base_reference_claims);
		if (!base_partitions)
			return sdk::unexpected(std::move(base_partitions.error()));
		std::vector<sdk::claim> reference_claims;
		reference_claims.insert(
			reference_claims.end(), base_reference_claims.begin(), base_reference_claims.end());
		reference_claims.insert(
			reference_claims.end(), raw_output_claims.begin(), raw_output_claims.end());
		for (const auto& reference : reference_claims)
			if (auto valid = sdk::validate_claim(*engine, reference); !valid)
				return sdk::unexpected(failure("materialization.reference-claim-invalid",
											   "descriptor",
											   valid.error().code + ":" + valid.error().field +
												   ":" + valid.error().detail));
		std::vector<materialization::materialization_v4_claim_sealed> claims;
		claims.reserve(task_v4_output_descriptor_ids.size());
		for (const auto descriptor_id : task_v4_output_descriptor_ids)
		{
			auto claim = make_worker_claim(execution,
										   *engine,
										   task,
										   capture,
										   base,
										   extension,
										   *manifest,
										   descriptor_id,
										   *basis,
										   reference_claims);
			if (!claim)
				return sdk::unexpected(std::move(claim.error()));
			claims.push_back(std::move(*claim));
		}
		auto receipt = make_task_output_receipt(*engine, claims, reference_claims);
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		if (auto valid = receipt->validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto key_domain = sdk::semantic_digest("cxxlens.clang22.materializer-key-domain.v1",
											   capture.compile_unit_id);
		if (!key_domain)
			return sdk::unexpected(std::move(key_domain.error()));
		std::vector<sdk::closure_candidate> closures;
		closures.reserve(claims.size());
		for (auto& claim : claims)
		{
			sdk::closure_candidate closure;
			closure.relation_descriptor_id = claim.translation.partition.relation_descriptor_id;
			closure.subject_partition_id = claim.partition_manifest.partition_id;
			closure.partition_content_digest = claim.partition_manifest.content_digest;
			closure.coverage_digest = claim.partition_manifest.coverage_digest;
			closure.key_domain_digest = *key_domain;
			closure.condition = claim.partition_binding.condition;
			closure.interpretation = claim.partition_binding.interpretation;
			closure.assumption_set_id = claim.partition_binding.assumption_set_id;
			closure.closure_kind = "relation-key-enumeration";
			closure.producer_semantics = claim.partition_binding.producer_semantics;
			closure.evidence_digest =
				std::string{execution.outcome.runtime_receipt->raw_stdout_sha256()};
			closures.push_back(std::move(closure));
		}
		sdk::detail::materialization_result_draft generic_result;
		generic_result.terminal = receipt->complete
			? sdk::detail::materialization_terminal::complete
			: sdk::detail::materialization_terminal::partial;
		generic_result.task_id = execution.task.id();
		generic_result.task_input_digest = execution.task.input_binding_digest();
		generic_result.runtime = sdk::detail::materialization_runtime_binding{
			execution.task.value().provider.provider_id,
			execution.task.value().provider.provider_version,
			execution.outcome.measured_executable_digest,
			authority.worker.semantic_contract_digest,
			execution.outcome.task_input_digest,
			std::string{execution.outcome.runtime_receipt->sealed_transcript_digest()}};
		generic_result.partitions.reserve(claims.size());
		for (const auto& claim : claims)
		{
			generic_result.partitions.push_back(claim.translation.partition);
			for (const auto& unresolved : claim.translation.batch.unresolved)
				generic_result.unresolved.push_back(
					{unresolved.reason,
					 unresolved.source_assertion,
					 unresolved.source_relation + "->" + unresolved.target_relation});
			generic_result.conflicts.insert(generic_result.conflicts.end(),
											claim.translation.batch.conflicts.begin(),
											claim.translation.batch.conflicts.end());
			generic_result.differential_disagreements.insert(
				generic_result.differential_disagreements.end(),
				claim.translation.batch.differential_disagreements.begin(),
				claim.translation.batch.differential_disagreements.end());
		}
		generic_result.coverage = {{"compile-unit",
									capture.project_id,
									receipt->complete ? "covered" : "unresolved",
									receipt->complete ? std::string{} : "provider-partial"}};
		if (receipt->complete)
			generic_result.closures = closures;
		auto validated_generic_result = sdk::detail::validate_materialization_result(
			*engine, execution.task, std::move(generic_result));
		if (!validated_generic_result)
			return sdk::unexpected(std::move(validated_generic_result.error()));
		// The compiler-neutral writer accepts only the validated task/result pair. The v4 receipts
		// remain frontend provenance and are bound as an opaque strong source receipt; they no
		// longer constitute a second Store publication authority.
		auto source =
			sdk::detail::make_materialization_publication_source(*engine,
																 execution.task,
																 *validated_generic_result,
																 *base_partitions,
																 receipt->receipt_digest);
		if (!source)
			return sdk::unexpected(std::move(source.error()));
		// snapshot_store takes ownership of its engine. Retain an immutable validation copy for the
		// publish-side revalidation; passing the moved-from engine would erase the registry
		// authority precisely at the final effect boundary.
		auto publication_engine = *engine;
		sdk::result<sdk::snapshot_store> store = sdk::unexpected(
			sdk::error{"materialization.store-open-failed", "backend", "unsupported"});
		std::optional<materialization::materialization_rooted_vfs_receipt>
			sqlite_effect_root_receipt;
		if (authority.publication.backend == "sqlite")
		{
			if (!authority.publication.sqlite_path)
				return sdk::unexpected(
					failure("materialization.store-open-failed", "sqlite_path", "missing"));
			auto effect_root = materialization::materialization_effect_root::capture_startup();
			if (!effect_root)
				return sdk::unexpected(std::move(effect_root.error()));
			auto rooted_opener =
				materialization::materialization_rooted_store_opener::create(*effect_root);
			if (!rooted_opener)
				return sdk::unexpected(std::move(rooted_opener.error()));
			store = (*rooted_opener)
						->open_sqlite(*authority.publication.sqlite_path, std::move(*engine));
			if (!store)
				return sdk::unexpected(std::move(store.error()));
			if (!(*rooted_opener)->receipt())
				return sdk::unexpected(
					failure("materialization.store-open-failed", "rooted-vfs", "receipt-missing"));
			sqlite_effect_root_receipt = *(*rooted_opener)->receipt();
		}
		else if (authority.publication.backend == "memory")
			store = sdk::make_in_memory_snapshot_store(std::move(*engine));
		else
			return sdk::unexpected(
				failure("materialization.store-open-failed", "backend", "unsupported"));
		if (!store)
			return sdk::unexpected(std::move(store.error()));
		std::optional<sdk::publication_record> observed_parent_record;
		if (authority.publication.expected_parent_publication)
		{
			auto parent =
				store->open_publication(*authority.publication.expected_parent_publication);
			if (!parent)
				return sdk::unexpected(std::move(parent.error()));
			observed_parent_record = parent->publication();
		}
		auto published_source = sdk::detail::publish_materialization_source(
			publication_engine,
			*store,
			std::move(*source),
			authority.publication.backend == "sqlite" && authority.publication.sqlite_path
				? authority.publication.sqlite_path
				: std::nullopt);
		if (!published_source)
			return sdk::unexpected(std::move(published_source.error()));
		auto& snapshot = published_source->snapshot;
		// A committed handle is not sufficient for the installed success boundary.  Re-open the
		// current selector, publication, and snapshot while the backend is still owned so a caller
		// cannot report success for a commit that is immediately unreadable or bound to another
		// series.  The three paths must expose the same immutable identity and manifest.
		auto reopened_publication = store->open_publication(snapshot.publication().publication_id);
		if (!reopened_publication)
			return sdk::unexpected(std::move(reopened_publication.error()));
		auto reopened_snapshot = store->open(snapshot.id());
		if (!reopened_snapshot)
			return sdk::unexpected(std::move(reopened_snapshot.error()));
		auto current = store->current(authority.publication.selector);
		if (!current)
			return sdk::unexpected(std::move(current.error()));
		if (reopened_publication->id() != snapshot.id() ||
			reopened_snapshot->id() != snapshot.id() || current->id() != snapshot.id() ||
			reopened_publication->publication() != snapshot.publication() ||
			reopened_snapshot->manifest() != snapshot.manifest() ||
			current->manifest() != snapshot.manifest())
			return sdk::unexpected(failure(
				"materialization.store-verification-failed", "reopen", "identity-mismatch"));
		auto canonical_export = store->canonical_export(snapshot.id());
		if (!canonical_export)
			return sdk::unexpected(std::move(canonical_export.error()));
		const auto canonical_export_digest = sdk::content_digest(
			std::as_bytes(std::span{canonical_export->data(), canonical_export->size()}));
		return materializer_store_execution{std::move(execution),
											std::move(claims),
											std::move(*base_partitions),
											std::move(reference_claims),
											std::move(*receipt),
											std::move(*validated_generic_result),
											std::move(*published_source),
											std::move(observed_parent_record),
											canonical_export_digest,
											std::move(sqlite_effect_root_receipt)};
	}
} // namespace cxxlens::detail::clang22
