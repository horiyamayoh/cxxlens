#include "materialization_incremental_receipt.hpp"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error receipt_error(const std::string_view field,
											   const std::string_view detail)
		{
			return {"materialization.incremental-receipt-invalid",
					std::string{field},
					std::string{detail}};
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value bytes(const std::span<const std::byte> value)
		{
			return sdk::canonical_value::from_bytes(
				std::vector<std::byte>{value.begin(), value.end()});
		}

		[[nodiscard]] sdk::canonical_value tuple(std::vector<sdk::canonical_value> values)
		{
			return sdk::canonical_value::from_tuple(std::move(values));
		}

		[[nodiscard]] sdk::canonical_value u64_bytes(const std::uint64_t value)
		{
			std::vector<std::byte> raw(sizeof(value));
			for (std::size_t index{}; index < raw.size(); ++index)
				raw[index] = static_cast<std::byte>(
					(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
			return bytes(raw);
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_projection(const std::string_view domain, const sdk::canonical_value& value)
		{
			auto encoded = sdk::canonical_binary(value);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			const std::string payload{reinterpret_cast<const char*>(encoded->data()),
									  encoded->size()};
			return sdk::semantic_digest(domain, payload);
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		encode_value(const sdk::canonical_value& value)
		{
			auto encoded = sdk::canonical_binary(value);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return std::move(*encoded);
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		canonical_bytes_value(const sdk::canonical_value& value)
		{
			auto encoded = encode_value(value);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::canonical_value::from_bytes(std::move(*encoded));
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		event_tuple(std::vector<sdk::canonical_value> values)
		{
			return encode_value(tuple(std::move(values)));
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		ordered_canonical_bytes(const std::string_view value)
		{
			auto item = canonical_bytes_value(text(value));
			if (!item)
				return sdk::unexpected(std::move(item.error()));
			return tuple({std::move(*item)});
		}

		[[nodiscard]] sdk::canonical_value provider_transcript_projection(
			const sdk::provider::detail::sealed_provider_transcript& value)
		{
			std::vector<sdk::canonical_value> batches;
			batches.reserve(value.batches().size());
			for (const auto& batch : value.batches())
			{
				std::vector<sdk::canonical_value> chunks;
				chunks.reserve(batch.ordered_chunk_digests().size());
				for (const auto& digest : batch.ordered_chunk_digests())
					chunks.push_back(text(digest));
				std::vector<sdk::canonical_value> rows;
				rows.reserve(batch.rows().size());
				for (const auto& row : batch.rows())
					rows.push_back(text(row.canonical_form()));
				batches.push_back(tuple({text(batch.task_id()),
										 text(batch.descriptor_id()),
										 text(batch.descriptor_digest()),
										 text(batch.dependency_group_id()),
										 text(batch.atomic_output_group_id()),
										 text(batch.batch_id()),
										 text(batch.batch_digest()),
										 tuple(std::move(chunks)),
										 tuple(std::move(rows))}));
			}
			std::vector<sdk::canonical_value> coverage;
			coverage.reserve(value.coverage().size());
			for (const auto& item : value.coverage())
				coverage.push_back(
					tuple({text(item.kind), text(item.id), text(item.state), text(item.reason)}));
			std::vector<sdk::canonical_value> unresolved;
			unresolved.reserve(value.unresolved().size());
			for (const auto& item : value.unresolved())
				unresolved.push_back(
					tuple({text(item.code), text(item.subject), text(item.detail)}));
			std::vector<sdk::canonical_value> evidence;
			evidence.reserve(value.evidence().size());
			for (const auto& item : value.evidence())
				evidence.push_back(tuple({text(item.kind),
										  text(item.subject),
										  text(item.producer),
										  text(item.summary)}));
			return tuple({tuple(std::move(batches)),
						  tuple(std::move(coverage)),
						  tuple(std::move(unresolved)),
						  tuple(std::move(evidence))});
		}

		[[nodiscard]] sdk::canonical_value
		observation_projection(const sealed_observation_v2_row& row)
		{
			const auto& observation = row.observation;
			std::vector<sdk::canonical_value> primary;
			if (observation.primary_span)
			{
				const auto& span = *observation.primary_span;
				primary = {text(span.span_id),
						   text(span.snapshot),
						   text(span.file),
						   u64_bytes(span.begin),
						   u64_bytes(span.end),
						   text(span.role),
						   sdk::canonical_value::from_boolean(span.read_only)};
			}
			std::vector<sdk::canonical_value> origins;
			origins.reserve(observation.origin_chain.size());
			for (const auto& origin : observation.origin_chain)
				origins.push_back(tuple({text(origin.kind),
										 text(origin.logical_path),
										 u64_bytes(static_cast<std::uint64_t>(origin.begin)),
										 u64_bytes(static_cast<std::uint64_t>(origin.end)),
										 sdk::canonical_value::from_boolean(origin.read_only)}));
			return tuple({u64_bytes(static_cast<std::uint64_t>(row.batch_index)),
						  u64_bytes(static_cast<std::uint64_t>(row.row_index)),
						  u64_bytes(static_cast<std::uint8_t>(observation.kind)),
						  text(observation.final_relation_compile_unit_id),
						  text(observation.semantic_key),
						  text(observation.payload_digest),
						  observation.primary_span ? tuple(std::move(primary))
												   : sdk::canonical_value::null(),
						  tuple(std::move(origins)),
						  sdk::canonical_value::from_boolean(observation.exact_equivalence),
						  observation.limitation ? text(*observation.limitation)
												 : sdk::canonical_value::null()});
		}

		[[nodiscard]] sdk::canonical_value row_projection(const sdk::detached_row& row)
		{
			return text(row.canonical_form());
		}

		[[nodiscard]] bool content_digest(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char character)
									{
										return (character >= '0' && character <= '9') ||
											(character >= 'a' && character <= 'f');
									});
		}

		[[nodiscard]] bool semantic_digest(const std::string_view value) noexcept
		{
			return value.size() == 83U && value.starts_with("semantic-v2:sha256:") &&
				std::ranges::all_of(value.substr(19U),
									[](const char character)
									{
										return (character >= '0' && character <= '9') ||
											(character >= 'a' && character <= 'f');
									});
		}

		[[nodiscard]] sdk::result<std::string>
		request_id_of(const validated_materialization_request& request)
		{
			const auto* value = request.document.root().member("materialization_request_id");
			if (value == nullptr || value->as_string() == nullptr ||
				!sdk::validate_strong_id(*value->as_string()))
				return sdk::unexpected(receipt_error("materialization-request-id", "invalid"));
			return *value->as_string();
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		full_event_projection_bytes(const materialization_partition_event_kind kind,
									const std::span<const std::byte> key,
									const std::span<const std::byte> payload)
		{
			if (auto valid =
					validate_materialization_partition_event_projection(kind, key, payload);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			std::vector<std::byte> kind_bytes{static_cast<std::byte>(kind)};
			return sdk::canonical_binary(tuple({bytes(kind_bytes), bytes(key), bytes(payload)}));
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		full_event_projection(const materialization_incremental_event_projection& event)
		{
			if (event.task_id.empty() || event.partition_id.empty() ||
				!sdk::validate_strong_id(event.task_id) ||
				!sdk::validate_strong_id(event.partition_id))
				return sdk::unexpected(receipt_error("event.identity", "strong-id"));
			auto decoded_key = sdk::canonical_binary_decode(event.key);
			if (!decoded_key || decoded_key->type != sdk::canonical_value::kind::ordered_tuple ||
				decoded_key->tuple.size() < 2U ||
				decoded_key->tuple[0].type != sdk::canonical_value::kind::utf8_string ||
				decoded_key->tuple[1].type != sdk::canonical_value::kind::utf8_string ||
				decoded_key->tuple[0].text != event.task_id ||
				decoded_key->tuple[1].text != event.partition_id)
				return sdk::unexpected(
					receipt_error("event.identity", "task-or-partition-mismatch"));
			return full_event_projection_bytes(event.kind, event.key, event.payload);
		}

		struct ordered_event
		{
			std::string partition_id;
			materialization_partition_event_kind kind;
			std::vector<std::byte> full_projection;
		};

		[[nodiscard]] sdk::result<materialization_incremental_receipt_component>
		component_digest(const std::string_view domain,
						 const std::string_view task_id,
						 const std::vector<std::vector<std::byte>>& projections)
		{
			std::vector<sdk::canonical_value> values;
			values.reserve(projections.size());
			for (const auto& projection : projections)
				values.push_back(bytes(projection));
			std::vector<sdk::canonical_value> fields;
			fields.reserve(3U);
			fields.push_back(text(task_id));
			fields.push_back(u64_bytes(static_cast<std::uint64_t>(projections.size())));
			fields.push_back(tuple(std::move(values)));
			auto digest = semantic_projection(domain, tuple(std::move(fields)));
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return materialization_incremental_receipt_component{
				static_cast<std::uint64_t>(projections.size()), std::move(*digest)};
		}

		[[nodiscard]] sdk::result<std::string>
		seal_task_receipt_projection(const materialization_incremental_task_receipt& receipt)
		{
			std::vector<sdk::canonical_value> fields;
			fields.reserve(16U);
			fields.push_back(text(receipt.materialization_request_id));
			fields.push_back(text(receipt.selected_request_entry_binding_digest));
			fields.push_back(text(receipt.task_id));
			fields.push_back(u64_bytes(receipt.canonical_task_ordinal));
			fields.push_back(sdk::canonical_value::from_boolean(receipt.successful_seal));
			fields.push_back(u64_bytes(receipt.provider_stdout_byte_count));
			fields.push_back(text(receipt.provider_stdout_sha256));
			fields.push_back(u64_bytes(receipt.decoded_provider_frame_count));
			fields.push_back(text(receipt.provider_frame_transcript_digest));
			fields.push_back(text(receipt.provider_sealed_transcript_digest));
			const auto component = [](const materialization_incremental_receipt_component& value)
			{
				return tuple({u64_bytes(value.count), text(value.full_projection_digest)});
			};
			fields.push_back(component(receipt.partition));
			fields.push_back(component(receipt.event));
			fields.push_back(component(receipt.claim));
			fields.push_back(component(receipt.row));
			fields.push_back(component(receipt.coverage));
			fields.push_back(component(receipt.unresolved));
			return semantic_projection("cxxlens.df-0200.pre-encoder-task-receipt.v1",
									   tuple(std::move(fields)));
		}

		[[nodiscard]] sdk::result<void>
		validate_component(const materialization_incremental_receipt_component& value,
						   const std::string_view field,
						   const bool allow_empty = true)
		{
			if ((!allow_empty && value.count == 0U) ||
				!semantic_digest(value.full_projection_digest))
				return sdk::unexpected(receipt_error(field, "count-or-digest"));
			return {};
		}
	} // namespace

	sdk::result<std::vector<std::byte>> materialization_incremental_full_event_projection(
		const materialization_partition_event_kind kind,
		const std::span<const std::byte> key,
		const std::span<const std::byte> payload)
	{
		return full_event_projection_bytes(kind, key, payload);
	}

	sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const sealed_materialization_result& result,
		const std::span<const std::string> partition_ids)
	{
		try
		{
			const std::string task_id{result.provider_task_id()};
			if (!sdk::validate_strong_id(task_id) || partition_ids.empty() ||
				!std::ranges::is_sorted(partition_ids) ||
				std::ranges::adjacent_find(partition_ids) != partition_ids.end())
				return sdk::unexpected(receipt_error("result-oracle", "identity-or-order"));
			for (const auto& partition_id : partition_ids)
				if (!sdk::validate_strong_id(partition_id))
					return sdk::unexpected(receipt_error("result-oracle", "partition-id"));

			auto provider_digest =
				semantic_projection("cxxlens.df-0200.independent-provider-result.v1",
									provider_transcript_projection(result.provider_seal()));
			if (!provider_digest)
				return sdk::unexpected(std::move(provider_digest.error()));

			std::vector<materialization_incremental_event_projection> output;
			const auto append_event =
				[&](const std::string_view partition_id,
					const materialization_partition_event_kind kind,
					std::vector<sdk::canonical_value> key_values,
					std::vector<sdk::canonical_value> payload_values) -> sdk::result<void>
			{
				auto key = event_tuple(std::move(key_values));
				if (!key)
					return sdk::unexpected(std::move(key.error()));
				auto payload = event_tuple(std::move(payload_values));
				if (!payload)
					return sdk::unexpected(std::move(payload.error()));
				output.push_back({task_id,
								  std::string{partition_id},
								  kind,
								  std::move(*key),
								  std::move(*payload)});
				return {};
			};

			for (const auto& partition_id : partition_ids)
			{
				const auto partition_output_begin = output.size();
				std::set<std::vector<std::byte>> claim_occurrence_identities;
				const auto& transcript = result.provider_seal();
				if (auto valid =
						append_event(partition_id,
									 materialization_partition_event_kind::partition_begin,
									 {text(task_id), text(partition_id)},
									 {text(result.task_input_digest()),
									  text(result.provider_execution_id()),
									  text(result.selected_catalog_compile_unit_id()),
									  text(result.final_relation_compile_unit_id()),
									  text(*provider_digest),
									  text(std::to_string(transcript.batches().size())),
									  text(std::to_string(result.base_claim_rows().size())),
									  text(std::to_string(result.observation_rows().size()))});
					!valid)
					return sdk::unexpected(std::move(valid.error()));

				std::uint64_t claim_count{};
				std::uint64_t row_count{};
				std::uint64_t coverage_count{};
				std::uint64_t unresolved_count{};
				for (const auto& batch : transcript.batches())
				{
					for (const auto& row : batch.rows())
					{
						const auto row_value = row_projection(row);
						const auto metadata_value = tuple({text(batch.batch_id()),
														   text(batch.descriptor_digest()),
														   text(batch.dependency_group_id()),
														   text(batch.atomic_output_group_id())});
						const auto occurrence_value =
							tuple({text(batch.descriptor_id()), metadata_value, row_value});
						auto occurrence_identity = encode_value(occurrence_value);
						if (!occurrence_identity)
							return sdk::unexpected(std::move(occurrence_identity.error()));
						if (!claim_occurrence_identities.insert(*occurrence_identity).second)
							continue;

						auto descriptor = canonical_bytes_value(text(batch.descriptor_id()));
						if (!descriptor)
							return sdk::unexpected(std::move(descriptor.error()));
						auto row_bytes = canonical_bytes_value(row_value);
						if (!row_bytes)
							return sdk::unexpected(std::move(row_bytes.error()));
						auto metadata_bytes = canonical_bytes_value(metadata_value);
						if (!metadata_bytes)
							return sdk::unexpected(std::move(metadata_bytes.error()));
						auto occurrence_bytes = canonical_bytes_value(occurrence_value);
						if (!occurrence_bytes)
							return sdk::unexpected(std::move(occurrence_bytes.error()));
						auto hard = ordered_canonical_bytes(std::string{"hard:"} +
															std::string{batch.descriptor_id()});
						auto soft = ordered_canonical_bytes(std::string{"soft:"} +
															std::string{batch.descriptor_id()});
						auto functional = ordered_canonical_bytes(std::string{"functional:"} +
																  std::string{batch.batch_id()});
						auto differential = ordered_canonical_bytes(std::string{"differential:"} +
																	std::string{batch.batch_id()});
						if (!hard || !soft || !functional || !differential)
							return sdk::unexpected(receipt_error("result-oracle", "claim-law"));
						if (auto valid =
								append_event(partition_id,
											 materialization_partition_event_kind::claim_occurrence,
											 {text(task_id),
											  text(partition_id),
											  std::move(*descriptor),
											  std::move(*occurrence_bytes)},
											 {std::move(*row_bytes),
											  std::move(*metadata_bytes),
											  std::move(*hard),
											  std::move(*soft),
											  std::move(*functional),
											  std::move(*differential)});
							!valid)
							return sdk::unexpected(std::move(valid.error()));
						++claim_count;
					}
				}

				const auto append_detached_row =
					[&](const sdk::detached_row& row) -> sdk::result<void>
				{
					auto row_bytes = canonical_bytes_value(row_projection(row));
					if (!row_bytes)
						return sdk::unexpected(std::move(row_bytes.error()));
					auto key_row = canonical_bytes_value(row_projection(row));
					if (!key_row)
						return sdk::unexpected(std::move(key_row.error()));
					if (auto valid =
							append_event(partition_id,
										 materialization_partition_event_kind::detached_row,
										 {text(task_id),
										  text(partition_id),
										  text(row.descriptor_id),
										  std::move(*key_row)},
										 {std::move(*row_bytes)});
						!valid)
						return valid;
					++row_count;
					return {};
				};
				for (const auto& row : result.base_claim_rows())
					if (auto valid = append_detached_row(row); !valid)
						return sdk::unexpected(std::move(valid.error()));
				for (const auto& row : result.source_span_claim_rows())
					if (auto valid = append_detached_row(row); !valid)
						return sdk::unexpected(std::move(valid.error()));

				for (const auto& observation : result.observation_rows())
				{
					auto observation_bytes =
						canonical_bytes_value(observation_projection(observation));
					if (!observation_bytes)
						return sdk::unexpected(std::move(observation_bytes.error()));
					auto key_observation =
						canonical_bytes_value(observation_projection(observation));
					if (!key_observation)
						return sdk::unexpected(std::move(key_observation.error()));
					if (auto valid =
							append_event(partition_id,
										 materialization_partition_event_kind::claim_annotation,
										 {text(task_id),
										  text(partition_id),
										  text("observation"),
										  std::move(*key_observation)},
										 {std::move(*observation_bytes)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				for (const auto& item : transcript.coverage())
				{
					const auto item_value = tuple(
						{text(item.kind), text(item.id), text(item.state), text(item.reason)});
					auto item_bytes = canonical_bytes_value(item_value);
					if (!item_bytes)
						return sdk::unexpected(std::move(item_bytes.error()));
					auto key_item = canonical_bytes_value(item_value);
					if (!key_item)
						return sdk::unexpected(std::move(key_item.error()));
					if (auto valid =
							append_event(partition_id,
										 materialization_partition_event_kind::coverage,
										 {text(task_id), text(partition_id), std::move(*key_item)},
										 {std::move(*item_bytes)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
					++coverage_count;
				}

				for (const auto& item : transcript.unresolved())
				{
					const auto item_value =
						tuple({text(item.code), text(item.subject), text(item.detail)});
					auto item_bytes = canonical_bytes_value(item_value);
					if (!item_bytes)
						return sdk::unexpected(std::move(item_bytes.error()));
					auto key_item = canonical_bytes_value(item_value);
					if (!key_item)
						return sdk::unexpected(std::move(key_item.error()));
					if (auto valid =
							append_event(partition_id,
										 materialization_partition_event_kind::unresolved,
										 {text(task_id), text(partition_id), std::move(*key_item)},
										 {std::move(*item_bytes)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
					++unresolved_count;
				}

				const auto frame_count =
					static_cast<std::uint64_t>(output.size() - partition_output_begin + 1U);
				if (auto valid = append_event(partition_id,
											  materialization_partition_event_kind::partition_end,
											  {text(task_id), text(partition_id)},
											  {u64_bytes(frame_count),
											   u64_bytes(claim_count),
											   u64_bytes(row_count),
											   u64_bytes(coverage_count),
											   u64_bytes(unresolved_count),
											   text(*provider_digest),
											   text(result.provider_execution_id()),
											   text(result.selected_catalog_compile_unit_id()),
											   text(result.final_relation_compile_unit_id()),
											   text(result.task_input_digest()),
											   text(task_id)});
					!valid)
					return sdk::unexpected(std::move(valid.error()));
			}

			std::vector<
				std::pair<std::vector<std::byte>, materialization_incremental_event_projection>>
				ordered;
			ordered.reserve(output.size());
			for (auto& event : output)
			{
				auto full = full_event_projection(event);
				if (!full)
					return sdk::unexpected(std::move(full.error()));
				ordered.emplace_back(std::move(*full), std::move(event));
			}
			std::ranges::sort(ordered,
							  [](const auto& left, const auto& right)
							  {
								  return std::tie(left.second.partition_id, left.first) <
									  std::tie(right.second.partition_id, right.first);
							  });
			for (std::size_t index{1U}; index < ordered.size(); ++index)
				if (ordered[index - 1U].second.partition_id == ordered[index].second.partition_id &&
					ordered[index - 1U].first == ordered[index].first)
					return sdk::unexpected(receipt_error("result-oracle", "duplicate-final-event"));
			output.clear();
			output.reserve(ordered.size());
			for (auto& entry : ordered)
				output.push_back(std::move(entry.second));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(receipt_error("result-oracle", "allocation"));
		}
	}

	sdk::result<std::string>
	materialization_incremental_request_id(const validated_materialization_request& request)
	{
		return request_id_of(request);
	}

	sdk::result<std::string> seal_materialization_incremental_selected_request_entry_binding(
		const validated_materialization_request& request, const std::size_t task_index)
	{
		if (task_index >= request.tasks.size())
			return sdk::unexpected(receipt_error("task-index", "out-of-range"));
		auto request_id = request_id_of(request);
		if (!request_id)
			return sdk::unexpected(std::move(request_id.error()));
		const auto& task = request.tasks[task_index];
		if (task_index > std::numeric_limits<std::uint64_t>::max() ||
			!content_digest(task.worker_input.source_content_digest))
			return sdk::unexpected(receipt_error("selected-request-entry", "task-binding"));
		return semantic_projection("cxxlens.df-0200.selected-request-entry-binding.v1",
								   tuple({text(*request_id),
										  text(task.provider_task_id),
										  u64_bytes(static_cast<std::uint64_t>(task_index)),
										  text(task.worker_input.source_content_digest),
										  u64_bytes(task.worker_input.budget.output_bytes),
										  u64_bytes(task.worker_input.budget.rows)}));
	}

	sdk::result<materialization_incremental_task_receipt>
	make_materialization_incremental_task_receipt(
		const validated_materialization_request& request,
		const std::size_t task_index,
		const std::uint64_t provider_stdout_byte_count,
		std::string provider_stdout_sha256,
		const std::uint64_t decoded_provider_frame_count,
		std::string provider_frame_transcript_digest,
		std::string provider_sealed_transcript_digest,
		const std::span<const materialization_incremental_event_projection> events)
	{
		if (task_index >= request.tasks.size() || events.empty() ||
			provider_stdout_byte_count == 0U || decoded_provider_frame_count == 0U ||
			!content_digest(provider_stdout_sha256) ||
			!semantic_digest(provider_frame_transcript_digest) ||
			!semantic_digest(provider_sealed_transcript_digest))
			return sdk::unexpected(receipt_error("task-receipt", "runtime-fields"));
		auto request_id = request_id_of(request);
		if (!request_id)
			return sdk::unexpected(std::move(request_id.error()));
		auto selected =
			seal_materialization_incremental_selected_request_entry_binding(request, task_index);
		if (!selected)
			return sdk::unexpected(std::move(selected.error()));
		const auto task_id = request.tasks[task_index].provider_task_id;
		std::vector<ordered_event> ordered;
		ordered.reserve(events.size());
		for (const auto& event : events)
		{
			if (event.task_id != task_id)
				return sdk::unexpected(receipt_error("event.task-id", "task-mismatch"));
			auto full = full_event_projection(event);
			if (!full)
				return sdk::unexpected(std::move(full.error()));
			ordered.push_back({event.partition_id, event.kind, std::move(*full)});
		}
		std::ranges::sort(ordered,
						  [](const ordered_event& left, const ordered_event& right)
						  {
							  return std::tie(left.partition_id, left.full_projection) <
								  std::tie(right.partition_id, right.full_projection);
						  });
		for (std::size_t index{1U}; index < ordered.size(); ++index)
			if (ordered[index - 1U].partition_id == ordered[index].partition_id &&
				ordered[index - 1U].full_projection == ordered[index].full_projection)
				return sdk::unexpected(receipt_error("events", "duplicate-final-projection"));

		std::map<std::string, std::vector<std::vector<std::byte>>, std::less<>> by_partition;
		std::map<std::string, std::pair<std::uint64_t, std::uint64_t>, std::less<>> boundaries;
		std::vector<std::vector<std::byte>> all_events;
		std::vector<std::vector<std::byte>> claims;
		std::vector<std::vector<std::byte>> rows;
		std::vector<std::vector<std::byte>> coverage;
		std::vector<std::vector<std::byte>> unresolved;
		all_events.reserve(ordered.size());
		for (const auto& event : ordered)
		{
			by_partition[event.partition_id].push_back(event.full_projection);
			all_events.push_back(event.full_projection);
			if (event.kind == materialization_partition_event_kind::partition_begin)
				++boundaries[event.partition_id].first;
			if (event.kind == materialization_partition_event_kind::partition_end)
				++boundaries[event.partition_id].second;
			switch (event.kind)
			{
				case materialization_partition_event_kind::claim_occurrence:
					claims.push_back(event.full_projection);
					break;
				case materialization_partition_event_kind::detached_row:
					rows.push_back(event.full_projection);
					break;
				case materialization_partition_event_kind::coverage:
					coverage.push_back(event.full_projection);
					break;
				case materialization_partition_event_kind::unresolved:
					unresolved.push_back(event.full_projection);
					break;
				case materialization_partition_event_kind::partition_begin:
				case materialization_partition_event_kind::claim_annotation:
				case materialization_partition_event_kind::partition_end:
					break;
			}
		}
		for (const auto& [partition_id, boundary] : boundaries)
			if (boundary != std::pair<std::uint64_t, std::uint64_t>{1U, 1U})
				return sdk::unexpected(receipt_error("events", "partition-boundary-census"));

		std::vector<sdk::canonical_value> partition_rows;
		partition_rows.reserve(by_partition.size());
		for (const auto& [partition_id, projections] : by_partition)
		{
			auto digest = semantic_projection(
				"cxxlens.df-0200.partition-event-full-projection.v1",
				tuple({text(partition_id),
					   u64_bytes(static_cast<std::uint64_t>(projections.size())),
					   [&]
					   {
						   std::vector<sdk::canonical_value> values;
						   values.reserve(projections.size());
						   for (const auto& projection : projections)
							   values.push_back(bytes(projection));
						   return tuple(std::move(values));
					   }()}));
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			partition_rows.push_back(tuple({text(partition_id), text(*digest)}));
		}
		std::vector<sdk::canonical_value> partition_set_fields;
		partition_set_fields.push_back(text(task_id));
		partition_set_fields.push_back(u64_bytes(static_cast<std::uint64_t>(by_partition.size())));
		partition_set_fields.push_back(tuple(std::move(partition_rows)));
		auto partition_digest =
			semantic_projection("cxxlens.df-0200.task-partition-full-projection.v1",
								tuple(std::move(partition_set_fields)));
		if (!partition_digest)
			return sdk::unexpected(std::move(partition_digest.error()));
		auto event_component =
			component_digest("cxxlens.df-0200.task-event-full-projection.v1", task_id, all_events);
		if (!event_component)
			return sdk::unexpected(std::move(event_component.error()));
		auto claim_component = component_digest(
			"cxxlens.df-0200.task-claim-occurrence-full-projection.v1", task_id, claims);
		if (!claim_component)
			return sdk::unexpected(std::move(claim_component.error()));
		auto row_component =
			component_digest("cxxlens.df-0200.task-detached-row-full-projection.v1", task_id, rows);
		if (!row_component)
			return sdk::unexpected(std::move(row_component.error()));
		auto coverage_component =
			component_digest("cxxlens.df-0200.task-coverage-full-projection.v1", task_id, coverage);
		if (!coverage_component)
			return sdk::unexpected(std::move(coverage_component.error()));
		auto unresolved_component = component_digest(
			"cxxlens.df-0200.task-unresolved-full-projection.v1", task_id, unresolved);
		if (!unresolved_component)
			return sdk::unexpected(std::move(unresolved_component.error()));

		materialization_incremental_task_receipt receipt;
		receipt.materialization_request_id = std::move(*request_id);
		receipt.selected_request_entry_binding_digest = std::move(*selected);
		receipt.task_id = task_id;
		receipt.canonical_task_ordinal = static_cast<std::uint64_t>(task_index);
		receipt.successful_seal = true;
		receipt.provider_stdout_byte_count = provider_stdout_byte_count;
		receipt.provider_stdout_sha256 = std::move(provider_stdout_sha256);
		receipt.decoded_provider_frame_count = decoded_provider_frame_count;
		receipt.provider_frame_transcript_digest = std::move(provider_frame_transcript_digest);
		receipt.provider_sealed_transcript_digest = std::move(provider_sealed_transcript_digest);
		receipt.partition = {static_cast<std::uint64_t>(by_partition.size()),
							 std::move(*partition_digest)};
		receipt.event = std::move(*event_component);
		receipt.claim = std::move(*claim_component);
		receipt.row = std::move(*row_component);
		receipt.coverage = std::move(*coverage_component);
		receipt.unresolved = std::move(*unresolved_component);
		auto seal = seal_task_receipt_projection(receipt);
		if (!seal)
			return sdk::unexpected(std::move(seal.error()));
		receipt.pre_encoder_task_receipt_seal_digest = std::move(*seal);
		return receipt;
	}

	sdk::result<void> validate_materialization_incremental_task_receipt(
		const validated_materialization_request& request,
		const std::size_t task_index,
		const materialization_incremental_task_receipt& receipt)
	{
		if (task_index >= request.tasks.size() || !receipt.successful_seal ||
			receipt.canonical_task_ordinal != task_index)
			return sdk::unexpected(receipt_error("task-receipt", "ordinal-or-success"));
		auto request_id = request_id_of(request);
		if (!request_id || receipt.materialization_request_id != *request_id ||
			receipt.task_id != request.tasks[task_index].provider_task_id)
			return sdk::unexpected(receipt_error("task-receipt", "request-or-task-binding"));
		auto selected =
			seal_materialization_incremental_selected_request_entry_binding(request, task_index);
		if (!selected || receipt.selected_request_entry_binding_digest != *selected)
			return sdk::unexpected(receipt_error("task-receipt", "selected-entry-binding"));
		if (receipt.provider_stdout_byte_count == 0U ||
			!content_digest(receipt.provider_stdout_sha256) ||
			receipt.decoded_provider_frame_count == 0U ||
			!semantic_digest(receipt.provider_frame_transcript_digest) ||
			!semantic_digest(receipt.provider_sealed_transcript_digest) ||
			!validate_component(receipt.partition, "partition", false) ||
			!validate_component(receipt.event, "event") ||
			!validate_component(receipt.claim, "claim") ||
			!validate_component(receipt.row, "row") ||
			!validate_component(receipt.coverage, "coverage") ||
			!validate_component(receipt.unresolved, "unresolved"))
			return sdk::unexpected(receipt_error("task-receipt", "field-validation"));
		auto seal = seal_task_receipt_projection(receipt);
		if (!seal || receipt.pre_encoder_task_receipt_seal_digest != *seal)
			return sdk::unexpected(receipt_error("task-receipt", "seal-mismatch"));
		return {};
	}

	sdk::result<materialization_incremental_execution_journal_receipt>
	seal_materialization_incremental_execution_journal(
		std::string materialization_request_id,
		const std::span<const materialization_incremental_task_receipt> task_receipts)
	{
		if (task_receipts.empty() || !sdk::validate_strong_id(materialization_request_id))
			return sdk::unexpected(receipt_error("execution-journal", "identity"));
		std::vector<std::string> task_ids;
		std::vector<std::string> seals;
		task_ids.reserve(task_receipts.size());
		seals.reserve(task_receipts.size());
		for (std::size_t index{}; index < task_receipts.size(); ++index)
		{
			const auto& receipt = task_receipts[index];
			if (receipt.materialization_request_id != materialization_request_id ||
				receipt.canonical_task_ordinal != index || !receipt.successful_seal ||
				!sdk::validate_strong_id(receipt.task_id))
				return sdk::unexpected(receipt_error("execution-journal", "task-order"));
			auto seal = seal_task_receipt_projection(receipt);
			if (!seal || receipt.pre_encoder_task_receipt_seal_digest != *seal)
				return sdk::unexpected(receipt_error("execution-journal", "task-seal"));
			task_ids.push_back(receipt.task_id);
			seals.push_back(std::move(*seal));
		}
		std::vector<sdk::canonical_value> task_id_values;
		std::vector<sdk::canonical_value> seal_values;
		task_id_values.reserve(task_ids.size());
		seal_values.reserve(seals.size());
		for (const auto& task_id : task_ids)
			task_id_values.push_back(text(task_id));
		for (const auto& seal : seals)
			seal_values.push_back(text(seal));
		auto digest =
			semantic_projection("cxxlens.df-0200.execution-journal-receipt-set.v1",
								tuple({text(materialization_request_id),
									   u64_bytes(static_cast<std::uint64_t>(task_ids.size())),
									   tuple(std::move(task_id_values)),
									   tuple(std::move(seal_values))}));
		if (!digest)
			return sdk::unexpected(std::move(digest.error()));
		return materialization_incremental_execution_journal_receipt{
			std::move(materialization_request_id),
			static_cast<std::uint64_t>(task_ids.size()),
			std::move(task_ids),
			std::move(seals),
			std::move(*digest)};
	}
} // namespace cxxlens::detail::clang22::materialization
