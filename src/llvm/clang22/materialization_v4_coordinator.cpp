#include "materialization_v4_coordinator.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <new>
#include <ranges>
#include <set>
#include <string>
#include <utility>

#include "sdk/provider_runtime_internal.hpp"
#include "source_closure_task_v4.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.v4-coordinator-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error limit(std::string field, std::string detail = {})
		{
			return {"materialization.v4-coordinator-limit", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error cancelled(std::string phase)
		{
			return {"materialization.cancelled", std::move(phase), "stop-requested"};
		}

		[[nodiscard]] sdk::result<void> stop_before(const std::stop_token& stop,
													const std::string_view phase)
		{
			if (stop.stop_requested())
				return sdk::unexpected(cancelled(std::string{phase}));
			return {};
		}

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		[[nodiscard]] sdk::result<void> strong(const std::string_view value,
											   const std::string_view field)
		{
			if (auto result = sdk::validate_strong_id(value); !result)
				return sdk::unexpected(invalid(std::string{field}, "identity"));
			return {};
		}

		[[nodiscard]] sdk::result<void> checked_add(std::uint64_t& total,
													const std::uint64_t value,
													const std::uint64_t maximum,
													const std::string_view field)
		{
			if (total > maximum || value > maximum - total)
				return sdk::unexpected(limit(std::string{field}, "overflow-or-maximum"));
			total += value;
			return {};
		}

		[[nodiscard]] constexpr std::string_view role_name(const source_closure_role value) noexcept
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

		[[nodiscard]] constexpr std::string_view
		encoding_name(const source_closure_encoding value) noexcept
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

		[[nodiscard]] sdk::result<source_closure_manifest>
		manifest_from_snapshot(const source_closure_snapshot& snapshot,
							   const provider_task_v4_limits limits)
		{
			if (auto valid = snapshot.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto manifest_digest = source_closure_manifest_digest(snapshot);
			if (!manifest_digest)
				return sdk::unexpected(std::move(manifest_digest.error()));

			source_closure_manifest manifest;
			manifest.closure_id = snapshot.snapshot_id;
			manifest.closure_digest = snapshot.closure_digest;
			manifest.manifest_digest = std::move(*manifest_digest);
			try
			{
				manifest.members.reserve(snapshot.members.size());
				for (const auto& member : snapshot.members)
					manifest.members.push_back({member.file_id,
												member.logical_path,
												std::string{role_name(member.role)},
												std::string{encoding_name(member.encoding)},
												member.size_bytes,
												member.content_digest,
												member.read_only});
				manifest.blobs.reserve(snapshot.blobs.size());
				for (const auto& blob : snapshot.blobs)
					manifest.blobs.push_back({blob.content_digest, blob.size_bytes});
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(limit("closure.manifest", "allocation"));
			}
			if (auto valid = manifest.validate(limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return manifest;
		}

		[[nodiscard]] std::optional<std::size_t>
		first_task_for_closure(const materialization_v4_validated_request& request,
							   const std::string_view closure_id)
		{
			for (std::size_t index{}; index < request.task_extensions.size(); ++index)
				if (request.task_extensions[index].source_closure.source_closure_id == closure_id)
					return index;
			return std::nullopt;
		}

		[[nodiscard]] sdk::result<void>
		validate_authority(const materialization_v4_validated_request& request,
						   const provider_task_v4_request_authority& authority,
						   const std::string_view authority_digest,
						   const sdk::relation_engine& engine)
		{
			if (auto valid = authority.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto recomputed = authority.authority_digest();
			if (!recomputed)
				return sdk::unexpected(std::move(recomputed.error()));
			if (*recomputed != authority_digest)
				return sdk::unexpected(invalid("request-authority-digest", "recomputed-mismatch"));
			if (engine.registry_digest() != authority.engine.engine_registry_digest ||
				engine.generation() != authority.engine.engine_generation_id)
				return sdk::unexpected(invalid("relation-engine", "authority-mismatch"));

			const auto& value = request;
			if (value.base_tasks.size() != authority.tasks.size() ||
				value.task_extensions.size() != authority.tasks.size())
				return sdk::unexpected(invalid("authority.tasks", "census"));
			for (std::size_t index{}; index < authority.tasks.size(); ++index)
			{
				const auto& base = value.base_tasks[index];
				const auto& extension = value.task_extensions[index];
				const auto& task = authority.tasks[index];
				if (task.provider_task_id != base.provider_task_id ||
					task.provider_execution_id != base.provider_execution_id ||
					task.task_input_digest != base.task_input_digest ||
					task.normalized_invocation_digest != base.normalized_invocation_digest ||
					task.toolchain_digest != base.toolchain_digest ||
					task.environment_digest != base.environment_digest ||
					task.working_directory != base.working_directory ||
					task.source != base.source || extension.base_task_index != index ||
					extension.base_provider_task_id != task.provider_task_id ||
					extension.open_task.task_input_digest != task.task_input_digest ||
					extension.open_task.normalized_invocation_digest !=
						task.normalized_invocation_digest ||
					extension.open_task.toolchain_digest != task.toolchain_digest ||
					extension.open_task.environment_digest != task.environment_digest ||
					extension.main_logical_path != task.source.logical_path ||
					extension.logical_working_directory != task.working_directory)
					return sdk::unexpected(invalid("authority.tasks", std::to_string(index)));
				if (auto valid = task.input_authority.validate(
						extension.main_logical_path,
						extension.logical_working_directory,
						extension.open_task.normalized_invocation_digest);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
			}
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_request_token(const materialization_v4_validated_request& request)
		{
			if (request.schema != materialization_request_v2_2_schema ||
				request.request_version != materialization_request_v2_2_version ||
				request.protocol_major != materialization_protocol_v2_major ||
				request.protocol_minor != materialization_protocol_v2_minor ||
				request.required_features.size() != 2U ||
				request.required_features[0] != "task-input-chunks-v2" ||
				request.required_features[1] != "task-source-closure-v2" ||
				request.negotiated_features != request.required_features ||
				request.base_tasks.empty() || request.source_closures.empty() ||
				request.base_tasks.size() != request.task_extensions.size())
				return sdk::unexpected(invalid("validated-request", "closed-shape"));
			for (const auto& [field, value] : {
					 std::pair{std::string_view{"validated-request.request-id"},
							   std::string_view{request.request_id}},
					 std::pair{std::string_view{"validated-request.request-digest"},
							   std::string_view{request.request_digest}},
					 std::pair{std::string_view{"validated-request.materialization-id"},
							   std::string_view{request.materialization_request_id}},
					 std::pair{std::string_view{"validated-request.semantic-request"},
							   std::string_view{request.semantic_request_digest}},
				 })
				if (auto valid = strong(value, field); !valid)
					return valid;
			if (request.request_id != "materialization-request:" + request.request_digest)
				return sdk::unexpected(invalid("validated-request.request-id", "digest-binding"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_worker_result(const materialization_v4_task_launch& launch,
							   const materialization_v4_worker_result& result)
		{
			const auto& request = launch.request;
			const auto& authority = launch.request_authority;
			const auto& task = authority.tasks[launch.task_index];
			const auto& extension = request.task_extensions[launch.task_index];
			const auto& closure = launch.closure;
			const auto& binding = result.binding;
			if (binding.request_authority_digest != launch.request_authority_digest ||
				binding.task_index != launch.task_index || binding.task_id != extension.task_id ||
				binding.task_v4_digest != extension.task_v4_digest ||
				binding.source_closure_id != extension.source_closure.source_closure_id ||
				binding.source_closure_digest != extension.source_closure.source_closure_digest ||
				binding.manifest_digest != extension.source_closure.manifest_digest ||
				binding.closure_receipt_digest != closure.receiver.credentials.spool_receipt ||
				binding.provider_id != authority.worker.provider_id ||
				binding.provider_version != authority.worker.provider_version ||
				binding.provider_binary_digest != authority.worker.installed_binary_digest ||
				binding.provider_semantic_contract_digest !=
					authority.worker.semantic_contract_digest ||
				binding.launch_input_digest != task.task_input_digest ||
				binding.normalized_invocation_digest != task.normalized_invocation_digest ||
				binding.toolchain_digest != task.toolchain_digest ||
				binding.environment_digest != task.environment_digest ||
				binding.output_plan_digest != authority.publication.output_plan_digest)
				return sdk::unexpected(invalid("worker.binding", extension.task_id));

			for (const auto& [field, value] : {
					 std::pair{std::string_view{"worker.runtime-receipt"},
							   std::string_view{binding.runtime_receipt_digest}},
					 std::pair{std::string_view{"worker.transcript-receipt"},
							   std::string_view{binding.sealed_transcript_digest}},
				 })
				if (auto valid = strong(value, field); !valid)
					return valid;

			const auto batches = result.transcript.batches();
			if (batches.size() != authority.registry.descriptors.size())
				return sdk::unexpected(invalid("worker.batches", "descriptor-census"));
			for (std::size_t index{}; index < batches.size(); ++index)
			{
				const auto& batch = batches[index];
				const auto& descriptor = authority.registry.descriptors[index];
				if (batch.task_id() != extension.task_id ||
					batch.descriptor_id() != descriptor.descriptor_id ||
					batch.descriptor_digest() != descriptor.runtime_descriptor_digest ||
					batch.dependency_group_id() != descriptor.dependency_group_id ||
					batch.atomic_output_group_id() != descriptor.atomic_output_group_id ||
					batch.batch_id() != descriptor.batch_id)
					return sdk::unexpected(invalid("worker.batches", std::to_string(index)));
			}
			auto digest = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				extension.task_id, "provider.success", result.transcript);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			if (*digest != binding.sealed_transcript_digest)
				return sdk::unexpected(invalid("worker.transcript-receipt", "recomputed-mismatch"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_host_rows(const sdk::relation_engine& engine,
						   const provider_task_v4_request_authority& authority,
						   const materialization_v4_host_claim_result& result)
		{
			auto digest = materialization_v4_base_row_set_digest(result.base_rows);
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			if (*digest != result.base_row_set_digest)
				return sdk::unexpected(invalid("claim.base-rows", "digest-mismatch"));
			if (result.base_rows.empty())
				return sdk::unexpected(invalid("claim.base-rows", "empty"));

			std::size_t current{};
			std::vector<bool> observed(authority.registry.base_descriptors.size(), false);
			std::string prior;
			for (const auto& row : result.base_rows)
			{
				while (current < authority.registry.base_descriptors.size() &&
					   authority.registry.base_descriptors[current].descriptor_id !=
						   row.descriptor_id)
				{
					if (observed[current])
						prior.clear();
					++current;
				}
				if (current == authority.registry.base_descriptors.size())
					return sdk::unexpected(invalid("claim.base-rows", "descriptor-order"));
				observed[current] = true;
				auto relation = engine.require_id(row.descriptor_id);
				if (!relation)
					return sdk::unexpected(std::move(relation.error()));
				const auto& descriptor = relation->descriptor();
				if (descriptor.descriptor_digest !=
					authority.registry.base_descriptors[current].runtime_descriptor_digest)
					return sdk::unexpected(invalid("claim.base-rows", "descriptor-digest"));
				if (auto valid = sdk::validate_row(descriptor, row); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (descriptor.domain_identity.result_column)
					if (auto valid = sdk::validate_domain_identity(descriptor, row); !valid)
						return sdk::unexpected(std::move(valid.error()));
				const auto canonical = row.canonical_form();
				if (!prior.empty() && canonical <= prior)
					return sdk::unexpected(invalid("claim.base-rows", "non-canonical-order"));
				prior = canonical;
			}
			if (std::ranges::find(observed, false) != observed.end())
				return sdk::unexpected(invalid("claim.base-rows", "descriptor-census"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_claim_result(const materialization_v4_task_launch& launch,
							  const materialization_v4_worker_result_binding& worker,
							  const sdk::relation_engine& engine,
							  const materialization_v4_host_claim_result& result)
		{
			if (auto valid = validate_materialization_v4_claim_receipt(engine, result.claim);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& request = launch.request;
			const auto& extension = request.task_extensions[launch.task_index];
			const auto& base = request.base_tasks[launch.task_index];
			const auto& binding = result.claim.translation.binding;
			const auto& receipt = result.claim.receipt;
			if (binding.materialization_request_id != request.materialization_request_id ||
				binding.task_index != launch.task_index || binding.base_task != base ||
				binding.task != extension ||
				binding.manifest.closure_id != worker.source_closure_id ||
				binding.manifest.closure_digest != worker.source_closure_digest ||
				binding.manifest.manifest_digest != worker.manifest_digest ||
				binding.provider_id != worker.provider_id ||
				binding.provider_semantic_contract_digest !=
					worker.provider_semantic_contract_digest ||
				receipt.materialization_request_id != request.materialization_request_id ||
				receipt.task_index != launch.task_index || receipt.task_id != worker.task_id ||
				receipt.task_v4_digest != worker.task_v4_digest ||
				receipt.source_closure_id != worker.source_closure_id ||
				receipt.source_closure_digest != worker.source_closure_digest ||
				receipt.manifest_digest != worker.manifest_digest ||
				receipt.task_input_digest != worker.launch_input_digest)
				return sdk::unexpected(invalid("claim.binding", worker.task_id));
			if (std::ranges::none_of(launch.request_authority.registry.descriptors,
									 [&](const auto& descriptor)
									 {
										 return descriptor.descriptor_id ==
											 binding.relation_descriptor_id;
									 }))
				return sdk::unexpected(invalid("claim.relation", binding.relation_descriptor_id));
			return validate_host_rows(engine, launch.request_authority, result);
		}

		[[nodiscard]] sdk::result<void>
		validate_publication(const materialization_v4_store_publication_result& result,
							 const std::string_view preparation_digest)
		{
			if (!is_valid(result.terminal) ||
				result.terminal == materialization_report2_2_store_terminal::not_attempted ||
				result.publish_call_count != 1U ||
				result.store_preparation_digest != preparation_digest)
				return sdk::unexpected(invalid("store.terminal", "matrix-or-preparation"));
			if (auto valid = strong(result.store_result_digest, "store.result-digest"); !valid)
				return valid;
			const bool committed =
				result.terminal == materialization_report2_2_store_terminal::committed_unverified ||
				result.terminal == materialization_report2_2_store_terminal::committed_verified;
			if (committed != result.publication_id.has_value())
				return sdk::unexpected(invalid("store.publication-id", "terminal-matrix"));
			if (result.publication_id)
				return strong(*result.publication_id, "store.publication-id");
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		canonical_digest(const std::string_view domain, const sdk::canonical_value& projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			std::string bytes;
			try
			{
				bytes.reserve(encoded->size());
				for (const auto byte : *encoded)
					bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(limit("digest", "allocation"));
			}
			return sdk::semantic_digest(domain, bytes);
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::result<std::string> closure_receipt_set_digest(
			const std::span<const materialization_v4_admitted_closure> closures)
		{
			std::vector<sdk::canonical_value> values;
			try
			{
				values.reserve(closures.size());
				for (const auto& closure : closures)
					values.push_back(sdk::canonical_value::from_tuple({
						sdk::canonical_value::from_integer(
							static_cast<std::int64_t>(closure.canonical_closure_index)),
						text(closure.ingress_binding.closure_id),
						text(closure.ingress_binding.manifest_digest),
						text(closure.receiver.credentials.transfer_digest),
						text(closure.receiver.credentials.spool_receipt),
						text(closure.receiver.credentials.cleanup_owner),
					}));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(limit("closure-receipt-set", "allocation"));
			}
			return canonical_digest("cxxlens.clang22.materialization-v4-closure-receipt-set.v1",
									sdk::canonical_value::from_tuple(std::move(values)));
		}

		[[nodiscard]] sdk::result<std::string>
		worker_result_set_digest(const std::span<const materialization_v4_worker_result> workers)
		{
			std::vector<sdk::canonical_value> values;
			try
			{
				values.reserve(workers.size());
				for (const auto& worker : workers)
				{
					const auto& value = worker.binding;
					values.push_back(sdk::canonical_value::from_tuple({
						text(value.request_authority_digest),
						sdk::canonical_value::from_integer(
							static_cast<std::int64_t>(value.task_index)),
						text(value.task_id),
						text(value.task_v4_digest),
						text(value.source_closure_id),
						text(value.source_closure_digest),
						text(value.manifest_digest),
						text(value.closure_receipt_digest),
						text(value.provider_id),
						text(value.provider_version.string()),
						text(value.provider_binary_digest),
						text(value.provider_semantic_contract_digest),
						text(value.launch_input_digest),
						text(value.normalized_invocation_digest),
						text(value.toolchain_digest),
						text(value.environment_digest),
						text(value.output_plan_digest),
						text(value.runtime_receipt_digest),
						text(value.sealed_transcript_digest),
						sdk::canonical_value::from_integer(
							static_cast<std::int64_t>(value.retained_bytes)),
					}));
				}
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(limit("worker-result-set", "allocation"));
			}
			return canonical_digest("cxxlens.clang22.materialization-v4-worker-result-set.v1",
									sdk::canonical_value::from_tuple(std::move(values)));
		}

		[[nodiscard]] sdk::result<std::string> base_row_result_set_digest(
			const std::span<const materialization_v4_host_claim_result> results)
		{
			std::vector<sdk::canonical_value> values;
			try
			{
				values.reserve(results.size());
				for (std::size_t index{}; index < results.size(); ++index)
					values.push_back(sdk::canonical_value::from_tuple({
						sdk::canonical_value::from_integer(static_cast<std::int64_t>(index)),
						text(results[index].claim.receipt.task_id),
						text(results[index].base_row_set_digest),
					}));
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(limit("base-row-result-set", "allocation"));
			}
			return canonical_digest("cxxlens.clang22.materialization-v4-base-row-result-set.v1",
									sdk::canonical_value::from_tuple(std::move(values)));
		}
	} // namespace

	sdk::result<materialization_v4_validated_request>
	make_materialization_v4_validated_request(validated_materialization_request_v2_2&& request)
	{
		try
		{
			const auto advertised = request.negotiated_features;
			const auto observed_unique_blob_bytes = request.unique_blob_bytes;
			auto rebound = validate_materialization_request_v2_2(
				std::move(request.request), advertised, materialization_request_v2_2_limits{});
			if (!rebound)
				return sdk::unexpected(std::move(rebound.error()));
			if (rebound->negotiated_features != request.negotiated_features ||
				rebound->unique_blob_bytes != observed_unique_blob_bytes)
				return sdk::unexpected(invalid("validated-request", "decoder-result-mismatch"));

			auto& value = rebound->request;
			materialization_v4_validated_request output;
			output.schema = std::move(value.schema);
			output.request_version = std::move(value.request_version);
			output.protocol_major = value.protocol_major;
			output.protocol_minor = value.protocol_minor;
			output.request_id = std::move(value.request_id);
			output.request_digest = std::move(value.request_digest);
			output.required_features = std::move(value.required_features);
			output.materialization_request_id = std::move(value.materialization_request_id);
			output.semantic_request_digest = std::move(value.semantic_request_digest);
			output.base_tasks = std::move(value.base_tasks);
			output.source_closures = std::move(value.source_closures);
			output.task_extensions = std::move(value.task_extensions);
			output.negotiated_features = std::move(rebound->negotiated_features);
			output.unique_blob_bytes = rebound->unique_blob_bytes;
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("validated-request", "allocation"));
		}
	}

	const materialization_v4_store_preparation_projection&
	materialization_v4_store_preparation::projection() const noexcept
	{
		return projection_;
	}

	std::string_view materialization_v4_store_preparation::preparation_digest() const noexcept
	{
		return preparation_digest_;
	}

	std::unique_ptr<materialization_v4_store_prepared_state>
	materialization_v4_store_preparation::take_state() && noexcept
	{
		return std::move(state_);
	}

	materialization_v4_store_preparation::materialization_v4_store_preparation(
		std::unique_ptr<materialization_v4_store_prepared_state> state,
		materialization_v4_store_preparation_projection projection,
		std::string preparation_digest)
		: state_{std::move(state)}, projection_{std::move(projection)},
		  preparation_digest_{std::move(preparation_digest)}
	{
	}

	sdk::result<materialization_v4_store_preparation> make_materialization_v4_store_preparation(
		std::unique_ptr<materialization_v4_store_prepared_state> state,
		materialization_v4_store_preparation_projection projection,
		const materialization_v4_coordinator_limits& limits)
	{
		if (!state || projection.task_count == 0U || projection.task_count > limits.maximum_tasks ||
			projection.source_bytes > limits.maximum_retained_result_bytes ||
			projection.expected_projection_digest != projection.actual_projection_digest)
			return sdk::unexpected(invalid("store.preparation", "state-count-bound-or-projection"));
		for (const auto& [field, value] : {
				 std::pair{std::string_view{"store.materialization-request-id"},
						   std::string_view{projection.materialization_request_id}},
				 std::pair{std::string_view{"store.task-receipt"},
						   std::string_view{projection.task_receipt_digest}},
				 std::pair{std::string_view{"store.source"},
						   std::string_view{projection.store_source_digest}},
				 std::pair{std::string_view{"store.sealed-input-replay"},
						   std::string_view{projection.sealed_input_replay_digest}},
				 std::pair{std::string_view{"store.expected"},
						   std::string_view{projection.expected_projection_digest}},
				 std::pair{std::string_view{"store.backend-staged-cursor"},
						   std::string_view{projection.backend_staged_cursor_digest}},
				 std::pair{std::string_view{"store.actual"},
						   std::string_view{projection.actual_projection_digest}},
				 std::pair{std::string_view{"store.journal"},
						   std::string_view{projection.journal_digest}},
			 })
			if (auto valid = strong(value, field); !valid)
				return sdk::unexpected(std::move(valid.error()));

		auto digest = canonical_digest("cxxlens.clang22.materialization-v4-store-preparation.v1",
									   sdk::canonical_value::from_tuple({
										   text(projection.materialization_request_id),
										   text(projection.task_receipt_digest),
										   sdk::canonical_value::from_integer(
											   static_cast<std::int64_t>(projection.task_count)),
										   sdk::canonical_value::from_integer(
											   static_cast<std::int64_t>(projection.source_bytes)),
										   text(projection.store_source_digest),
										   text(projection.sealed_input_replay_digest),
										   text(projection.expected_projection_digest),
										   text(projection.backend_staged_cursor_digest),
										   text(projection.actual_projection_digest),
										   text(projection.journal_digest),
									   }));
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		return materialization_v4_store_preparation{
			std::move(state), std::move(projection), std::move(*digest)};
	}

	sdk::result<std::string>
	materialization_v4_base_row_set_digest(const std::span<const sdk::detached_row> rows)
	{
		if (rows.empty())
			return sdk::unexpected(invalid("base-rows", "empty"));
		try
		{
			auto chain = sdk::semantic_digest(
				"cxxlens.clang22.materialization-v4-base-rows-chain.v1", "seed");
			if (!chain)
				return sdk::unexpected(std::move(chain.error()));
			for (std::size_t index{}; index < rows.size(); ++index)
			{
				auto row = sdk::semantic_digest("cxxlens.clang22.materialization-v4-base-row.v1",
												rows[index].canonical_form());
				if (!row)
					return sdk::unexpected(std::move(row.error()));
				chain =
					sdk::semantic_digest("cxxlens.clang22.materialization-v4-base-rows-chain.v1",
										 *chain + "\n" + std::to_string(index) + "\n" + *row);
				if (!chain)
					return sdk::unexpected(std::move(chain.error()));
			}
			return sdk::semantic_digest("cxxlens.clang22.materialization-v4-base-rows.v1",
										std::to_string(rows.size()) + "\n" + *chain);
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("base-rows", "allocation"));
		}
	}

	sdk::result<materialization_v4_coordinator_outcome>
	run_materialization_v4_coordinator(materialization_v4_coordinator_input input,
									   materialization_v4_coordinator_ports ports)
	{
		if (input.limits.maximum_tasks == 0U || input.limits.maximum_closures == 0U ||
			input.limits.maximum_tasks > materialization_v4_incremental_max_tasks ||
			input.limits.maximum_closures > materialization_v4_incremental_max_tasks ||
			input.limits.maximum_unique_blob_bytes == 0U ||
			input.limits.maximum_retained_result_bytes == 0U ||
			input.limits.report.maximum_report_bytes == 0U ||
			input.limits.report.maximum_terminal_bytes == 0U ||
			input.limits.report.maximum_terminal_bytes >= input.limits.report.maximum_report_bytes)
			return sdk::unexpected(invalid("limits", "invalid"));
		if (auto stop = stop_before(input.cancellation, "before-authority"); !stop)
			return sdk::unexpected(std::move(stop.error()));
		if (auto valid = validate_request_token(input.request); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_authority(
				input.request, input.authority, input.authority_digest, input.engine);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (input.request.task_extensions.size() > input.limits.maximum_tasks ||
			input.request.source_closures.size() > input.limits.maximum_closures ||
			input.request.unique_blob_bytes > input.limits.maximum_unique_blob_bytes)
			return sdk::unexpected(limit("request.census", "maximum"));
		if (auto stop = stop_before(input.cancellation, "before-closure-receive"); !stop)
			return sdk::unexpected(std::move(stop.error()));

		auto closures = ports.closures.receive_all(input.request, input.cancellation);
		if (!closures)
			return sdk::unexpected(std::move(closures.error()));
		if (auto stop = stop_before(input.cancellation, "after-closure-receive"); !stop)
			return sdk::unexpected(std::move(stop.error()));
		if (closures->size() != input.request.source_closures.size())
			return sdk::unexpected(invalid("closures", "census"));

		std::vector<source_closure_manifest> manifests;
		std::uint64_t closure_bytes{};
		try
		{
			manifests.reserve(closures->size());
			for (std::size_t index{}; index < closures->size(); ++index)
			{
				auto& admitted = (*closures)[index];
				const auto& summary = input.request.source_closures[index];
				const auto authority_index =
					first_task_for_closure(input.request, summary.source_closure_id);
				if (!authority_index || admitted.canonical_closure_index != index ||
					admitted.authority_task_index != *authority_index ||
					admitted.receiver.snapshot.snapshot_id != summary.source_closure_id ||
					admitted.receiver.snapshot.closure_digest != summary.source_closure_digest ||
					admitted.ingress_binding.closure_id != summary.source_closure_id ||
					admitted.ingress_binding.closure_digest != summary.source_closure_digest ||
					admitted.ingress_binding.manifest_digest != summary.manifest_digest ||
					admitted.ingress_binding.task_id !=
						input.request.task_extensions[*authority_index].task_id ||
					admitted.ingress_binding.task_v4_digest !=
						input.request.task_extensions[*authority_index].task_v4_digest ||
					admitted.expected_transfer_digest !=
						admitted.receiver.credentials.transfer_digest)
					return sdk::unexpected(invalid("closures.binding", std::to_string(index)));
				for (const auto& [field, value] : {
						 std::pair{std::string_view{"closures.session"},
								   std::string_view{admitted.ingress_binding.session_id}},
						 std::pair{std::string_view{"closures.transfer"},
								   std::string_view{admitted.receiver.credentials.transfer_digest}},
						 std::pair{std::string_view{"closures.spool-receipt"},
								   std::string_view{admitted.receiver.credentials.spool_receipt}},
						 std::pair{std::string_view{"closures.cleanup-owner"},
								   std::string_view{admitted.receiver.credentials.cleanup_owner}},
					 })
					if (auto valid = strong(value, field); !valid)
						return sdk::unexpected(std::move(valid.error()));

				std::uint64_t retained{};
				for (const auto& blob : admitted.receiver.snapshot.blobs)
					if (auto added = checked_add(retained,
												 blob.size_bytes,
												 input.limits.maximum_unique_blob_bytes,
												 "closure.bytes");
						!added)
						return sdk::unexpected(std::move(added.error()));
				if (retained != admitted.retained_bytes || retained != summary.unique_blob_bytes)
					return sdk::unexpected(
						invalid("closures.retained-bytes", std::to_string(index)));
				if (auto added = checked_add(closure_bytes,
											 admitted.retained_bytes,
											 input.limits.maximum_unique_blob_bytes,
											 "closures.bytes");
					!added)
					return sdk::unexpected(std::move(added.error()));
				auto manifest =
					manifest_from_snapshot(admitted.receiver.snapshot, provider_task_v4_limits{});
				if (!manifest)
					return sdk::unexpected(std::move(manifest.error()));
				if (manifest->manifest_digest != summary.manifest_digest)
					return sdk::unexpected(invalid("closures.manifest", std::to_string(index)));
				manifests.push_back(std::move(*manifest));
			}
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("closures", "allocation"));
		}
		if (closure_bytes != input.request.unique_blob_bytes)
			return sdk::unexpected(invalid("closures.bytes", "request-mismatch"));

		for (std::size_t index{}; index < manifests.size(); ++index)
			if (auto bound = bind_source_closure_summary(input.request.source_closures[index],
														 manifests[index]);
				!bound)
				return sdk::unexpected(std::move(bound.error()));
		for (std::size_t index{}; index < input.request.task_extensions.size(); ++index)
		{
			const auto& extension = input.request.task_extensions[index];
			const auto manifest = std::ranges::find_if(
				manifests,
				[&](const auto& candidate)
				{
					return candidate.closure_id == extension.source_closure.source_closure_id;
				});
			if (manifest == manifests.end())
				return sdk::unexpected(invalid("request.manifest", extension.task_id));
			if (auto bound = bind_provider_task_v4_main_member(
					input.request.base_tasks[index], extension, *manifest);
				!bound)
				return sdk::unexpected(std::move(bound.error()));
			const auto admitted = std::ranges::find_if(
				*closures,
				[&](const auto& candidate)
				{
					return candidate.receiver.snapshot.snapshot_id == manifest->closure_id;
				});
			if (admitted == closures->end() ||
				input.request.base_tasks[index].source.source_snapshot_id !=
					admitted->receiver.snapshot.snapshot_id)
				return sdk::unexpected(invalid("request.source-snapshot", extension.task_id));
			if (auto authenticated =
					ports.source_identity.validate_main(input.request.base_tasks[index],
														extension,
														admitted->receiver.snapshot,
														input.cancellation);
				!authenticated)
				return sdk::unexpected(std::move(authenticated.error()));
			if (auto stop = stop_before(input.cancellation, "after-source-identity"); !stop)
				return sdk::unexpected(std::move(stop.error()));
		}

		std::vector<materialization_v4_worker_result> worker_results;
		std::vector<materialization_v4_host_claim_result> host_results;
		std::vector<const materialization_v4_claim_sealed*> sealed_tasks;
		std::vector<const materialization_v4_host_claim_result*> host_result_views;
		std::uint64_t retained_results{};
		try
		{
			worker_results.reserve(input.request.task_extensions.size());
			host_results.reserve(input.request.task_extensions.size());
			sealed_tasks.reserve(input.request.task_extensions.size());
			host_result_views.reserve(input.request.task_extensions.size());
			for (std::size_t index{}; index < input.request.task_extensions.size(); ++index)
			{
				if (auto stop = stop_before(input.cancellation, "before-worker"); !stop)
					return sdk::unexpected(std::move(stop.error()));
				const auto& extension = input.request.task_extensions[index];
				const auto closure =
					std::ranges::find_if(*closures,
										 [&](const auto& candidate)
										 {
											 return candidate.receiver.snapshot.snapshot_id ==
												 extension.source_closure.source_closure_id;
										 });
				if (closure == closures->end())
					return sdk::unexpected(invalid("task.closure", extension.task_id));
				materialization_v4_task_launch launch{input.request,
													  input.authority,
													  input.authority_digest,
													  index,
													  *closure,
													  input.limits,
													  input.cancellation};
				auto worker = ports.worker.execute(launch);
				if (!worker)
					return sdk::unexpected(std::move(worker.error()));
				if (auto valid = validate_worker_result(launch, *worker); !valid)
					return sdk::unexpected(std::move(valid.error()));
				if (auto added = checked_add(retained_results,
											 worker->binding.retained_bytes,
											 input.limits.maximum_retained_result_bytes,
											 "worker.retained-bytes");
					!added)
					return sdk::unexpected(std::move(added.error()));
				if (auto stop = stop_before(input.cancellation, "after-worker"); !stop)
					return sdk::unexpected(std::move(stop.error()));

				auto host = ports.claims.translate(
					launch, worker->binding, worker->transcript, input.engine);
				if (!host)
					return sdk::unexpected(std::move(host.error()));
				if (auto valid =
						validate_claim_result(launch, worker->binding, input.engine, *host);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				if (auto added = checked_add(retained_results,
											 host->retained_bytes,
											 input.limits.maximum_retained_result_bytes,
											 "claim.retained-bytes");
					!added)
					return sdk::unexpected(std::move(added.error()));
				if (auto stop = stop_before(input.cancellation, "after-claim"); !stop)
					return sdk::unexpected(std::move(stop.error()));
				worker_results.push_back(std::move(*worker));
				host_results.push_back(std::move(*host));
			}
			for (const auto& host : host_results)
			{
				sealed_tasks.push_back(&host.claim);
				host_result_views.push_back(&host);
			}
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(limit("task-results", "allocation"));
		}

		auto receipt = make_materialization_v4_incremental_receipt(input.engine, sealed_tasks);
		if (!receipt)
			return sdk::unexpected(std::move(receipt.error()));
		if (auto valid = validate_materialization_v4_incremental_receipt(
				input.engine, *receipt, sealed_tasks);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (!receipt->complete ||
			receipt->materialization_request_id != input.request.materialization_request_id ||
			receipt->task_count != input.request.task_extensions.size())
			return sdk::unexpected(invalid("task-receipt", "incomplete-or-census"));
		if (auto stop = stop_before(input.cancellation, "before-store-prepare"); !stop)
			return sdk::unexpected(std::move(stop.error()));

		auto preparation = ports.store.prepare(input.request,
											   input.authority,
											   input.authority_digest,
											   input.engine,
											   *receipt,
											   sealed_tasks,
											   host_result_views,
											   input.limits,
											   input.cancellation);
		if (!preparation)
			return sdk::unexpected(std::move(preparation.error()));
		if (preparation->projection().materialization_request_id !=
				input.request.materialization_request_id ||
			preparation->projection().task_receipt_digest != receipt->receipt_digest ||
			preparation->projection().task_count != receipt->task_count ||
			preparation->projection().source_bytes > input.limits.maximum_retained_result_bytes ||
			preparation->projection().expected_projection_digest !=
				preparation->projection().actual_projection_digest)
			return sdk::unexpected(invalid("store.preparation", "coordinator-binding"));
		if (auto stop = stop_before(input.cancellation, "after-store-prepare"); !stop)
			return sdk::unexpected(std::move(stop.error()));

		auto report_storage = ports.report_storage.create(input.limits.report, input.cancellation);
		if (!report_storage)
			return sdk::unexpected(std::move(report_storage.error()));
		const auto preparation_projection = preparation->projection();
		const std::string preparation_digest{preparation->preparation_digest()};
		auto closure_receipts = closure_receipt_set_digest(*closures);
		auto worker_receipts = worker_result_set_digest(worker_results);
		auto base_row_receipts = base_row_result_set_digest(host_results);
		if (!closure_receipts || !worker_receipts || !base_row_receipts)
			return sdk::unexpected(!closure_receipts	  ? std::move(closure_receipts.error())
									   : !worker_receipts ? std::move(worker_receipts.error())
														  : std::move(base_row_receipts.error()));
		materialization_report2_2_prepublication_projection report_projection{
			input.request.materialization_request_id,
			input.request.request_digest,
			input.request.semantic_request_digest,
			input.authority_digest,
			static_cast<std::uint64_t>(input.request.task_extensions.size()),
			static_cast<std::uint64_t>(input.request.source_closures.size()),
			input.request.unique_blob_bytes,
			std::move(*closure_receipts),
			std::move(*worker_receipts),
			std::move(*base_row_receipts),
			receipt->receipt_digest,
			preparation_projection.store_source_digest,
			preparation_digest,
			preparation_projection.expected_projection_digest,
			preparation_projection.actual_projection_digest,
			preparation_projection.journal_digest,
		};
		auto report = materialization_report2_2_builder::prepare(std::move(*report_storage),
																 std::move(report_projection),
																 ports.report_projection,
																 input.limits.report,
																 input.cancellation);
		if (!report)
			return sdk::unexpected(std::move(report.error()));
		if (!report->terminal_space_reserved())
			return sdk::unexpected(invalid("report", "terminal-not-reserved"));
		if (auto stop = stop_before(input.cancellation, "before-store-publish"); !stop)
			return sdk::unexpected(std::move(stop.error()));

		auto publication = ports.store.publish_once(std::move(*preparation), input.cancellation);
		if (auto valid = validate_publication(publication, preparation_digest); !valid)
			return materialization_v4_coordinator_outcome{
				std::in_place_type<materialization_v4_coordinator_postpublication_failure>,
				std::move(publication),
				std::move(valid.error())};

		try
		{
			materialization_report2_2_terminal_projection terminal{
				publication.terminal,
				publication.store_preparation_digest,
				publication.store_result_digest,
				publication.publication_id,
				publication.publish_call_count,
			};
			auto sealed_report = std::move(*report).finalize(terminal);
			if (!sealed_report)
				return materialization_v4_coordinator_outcome{
					std::in_place_type<materialization_v4_coordinator_postpublication_failure>,
					std::move(publication),
					std::move(sealed_report.error())};
			return materialization_v4_coordinator_outcome{
				std::in_place_type<materialization_v4_coordinator_completed>,
				std::move(*receipt),
				std::move(publication),
				std::move(*sealed_report)};
		}
		catch (const std::bad_alloc&)
		{
			return materialization_v4_coordinator_outcome{
				std::in_place_type<materialization_v4_coordinator_postpublication_failure>,
				std::move(publication),
				invalid("report", "postpublication-allocation")};
		}
		catch (...)
		{
			return materialization_v4_coordinator_outcome{
				std::in_place_type<materialization_v4_coordinator_postpublication_failure>,
				std::move(publication),
				invalid("report", "postpublication-exception")};
		}
	}
} // namespace cxxlens::detail::clang22::materialization
