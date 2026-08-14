#include "materialization_incremental_receipt.hpp"

#include <algorithm>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

#include "materialization_claims.hpp"
#include "sdk/claim_internal.hpp"

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

		[[nodiscard]] sdk::result<sdk::canonical_value>
		ordered_canonical_bytes(std::vector<std::vector<std::byte>> values)
		{
			std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> ordered;
			ordered.reserve(values.size());
			for (auto& value : values)
			{
				auto ordering = encode_value(sdk::canonical_value::from_bytes(value));
				if (!ordering)
					return sdk::unexpected(std::move(ordering.error()));
				ordered.emplace_back(std::move(value), std::move(*ordering));
			}
			std::ranges::sort(ordered,
							  [](const auto& left, const auto& right)
							  {
								  return left.second < right.second;
							  });
			for (std::size_t index{1U}; index < ordered.size(); ++index)
				if (ordered[index - 1U].second == ordered[index].second)
					return sdk::unexpected(
						receipt_error("result-oracle", "reference-target-duplicate"));
			std::vector<sdk::canonical_value> output;
			output.reserve(ordered.size());
			for (auto& value : ordered)
				output.push_back(sdk::canonical_value::from_bytes(std::move(value.first)));
			return tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value
		claim_basis_projection(const sdk::claim_input_basis& basis)
		{
			if (const auto* direct = std::get_if<sdk::direct_claim_basis>(&basis))
				return tuple({text("direct"), text(direct->basis_digest)});
			const auto& derived = std::get<sdk::derived_claim_basis>(basis);
			std::vector<sdk::canonical_value> consumed;
			consumed.reserve(derived.consumed_partition_content_digests.size());
			for (const auto& digest : derived.consumed_partition_content_digests)
				consumed.push_back(text(digest));
			return tuple({text("derived"),
						  text(derived.input_snapshot),
						  tuple(std::move(consumed)),
						  text(derived.transform_semantics)});
		}

		[[nodiscard]] sdk::canonical_value
		claim_occurrence_metadata_projection(const sdk::claim& value)
		{
			std::vector<sdk::canonical_value> modalities;
			modalities.reserve(value.guarantee.verification_modalities.size());
			for (const auto& modality : value.guarantee.verification_modalities)
				modalities.push_back(text(modality));
			return tuple({text(value.descriptor),
						  text(value.semantic_key),
						  text(value.interpretation),
						  text(value.assertion),
						  text(value.row.canonical_form()),
						  text(value.presence.canonical_form()),
						  u64_bytes(static_cast<std::uint64_t>(value.stage)),
						  text(value.producer.id),
						  text(value.producer.semantic_contract),
						  claim_basis_projection(value.input_basis),
						  text(value.provenance_root),
						  text(value.guarantee.approximation),
						  text(value.guarantee.scope),
						  text(value.guarantee.assumptions),
						  tuple(std::move(modalities))});
		}

		[[nodiscard]] sdk::canonical_value detached_cell_projection(const sdk::detached_cell& cell)
		{
			if (cell.state == sdk::cell_state::absent)
				return sdk::canonical_value::null();
			if (cell.state == sdk::cell_state::unknown)
				return tuple({text("unknown"), text(cell.unknown_reason.value_or("unspecified"))});
			if (!cell.value)
				return sdk::canonical_value::null();
			return std::visit(
				[](const auto& value) -> sdk::canonical_value
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type, bool>)
						return sdk::canonical_value::from_boolean(value);
					else if constexpr (std::is_same_v<value_type, std::int64_t>)
						return sdk::canonical_value::from_integer(value);
					else if constexpr (std::is_same_v<value_type, std::uint64_t>)
						return tuple({text("unsigned"), text(std::to_string(value))});
					else if constexpr (std::is_same_v<value_type, std::string>)
						return text(value);
					else
						return sdk::canonical_value::from_bytes(value);
				},
				*cell.value);
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		claim_content_projection(const sdk::claim& value, const sdk::relation_engine& engine)
		{
			auto relation = engine.require_id(value.descriptor);
			if (!relation)
				return sdk::unexpected(std::move(relation.error()));
			std::vector<std::string> payload_columns;
			for (const auto& column : relation->descriptor().columns)
				if (column.role == sdk::column_role::authoritative_payload)
					payload_columns.push_back(column.id);
			std::ranges::sort(payload_columns);
			std::vector<sdk::canonical_value> payload;
			payload.reserve(payload_columns.size());
			for (const auto& column : payload_columns)
			{
				const auto found = value.row.cells.find(column);
				payload.push_back(found == value.row.cells.end()
									  ? sdk::canonical_value::null()
									  : detached_cell_projection(found->second));
			}
			return tuple({text(value.assertion), tuple(std::move(payload))});
		}

		[[nodiscard]] sdk::canonical_value annotation_projection(const sdk::claim& value)
		{
			std::vector<sdk::canonical_value> modalities;
			modalities.reserve(value.guarantee.verification_modalities.size());
			for (const auto& modality : value.guarantee.verification_modalities)
				modalities.push_back(text(modality));
			return tuple({text(value.row.canonical_form()),
						  text(value.presence.canonical_form()),
						  text(value.interpretation),
						  text(value.semantic_key),
						  text(value.assertion),
						  text(value.content),
						  text(value.producer.id),
						  text(value.producer.semantic_contract),
						  text(value.provenance_root),
						  text(value.guarantee.approximation),
						  text(value.guarantee.scope),
						  text(value.guarantee.assumptions),
						  tuple(std::move(modalities))});
		}

		[[nodiscard]] sdk::canonical_value
		coverage_projection(const sdk::snapshot_coverage_unit& value)
		{
			return tuple(
				{text(value.domain), text(value.key), text(value.state), text(value.reason)});
		}

		[[nodiscard]] sdk::canonical_value
		unresolved_projection(const sdk::unresolved_reference& value)
		{
			std::vector<sdk::canonical_value> columns;
			columns.reserve(value.source_columns.size());
			for (const auto& column : value.source_columns)
				columns.push_back(text(column));
			return tuple({text(value.source_assertion),
						  text(value.source_relation),
						  text(value.target_relation),
						  tuple(std::move(columns)),
						  text(value.reason)});
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		decode_reference_elements(const sdk::claim& source,
								  const sdk::relation_reference_descriptor& reference)
		{
			if (reference.source_columns.empty())
				return sdk::unexpected(receipt_error("result-oracle", "reference-source-columns"));
			const auto found = source.row.cells.find(reference.source_columns.front());
			if (found == source.row.cells.end() || !found->second.value)
				return std::vector<std::string>{};
			const auto* encoded = std::get_if<std::vector<std::byte>>(&*found->second.value);
			if (encoded == nullptr)
				return sdk::unexpected(receipt_error("result-oracle", "reference-container-type"));
			std::vector<std::string> output;
			for (std::size_t offset{}; offset < encoded->size();)
			{
				if (encoded->size() - offset < sizeof(std::uint32_t))
					return sdk::unexpected(
						receipt_error("result-oracle", "reference-container-frame"));
				std::uint32_t length{};
				for (std::size_t index{}; index < sizeof(length); ++index)
					length |= std::to_integer<std::uint32_t>((*encoded)[offset + index])
						<< static_cast<unsigned>(index * 8U);
				offset += sizeof(length);
				if (length == 0U || length > encoded->size() - offset)
					return sdk::unexpected(
						receipt_error("result-oracle", "reference-container-frame"));
				output.emplace_back(reinterpret_cast<const char*>(encoded->data() + offset),
									length);
				offset += length;
			}
			return output;
		}

		[[nodiscard]] bool reference_matches(const sdk::claim& source,
											 const sdk::claim& target,
											 const sdk::relation_reference_descriptor& reference,
											 const std::optional<std::string_view> element,
											 const sdk::relation_engine& engine)
		{
			auto target_descriptor = engine.require_id(target.descriptor);
			if (!target_descriptor ||
				target_descriptor->descriptor().name != reference.target_relation ||
				source.interpretation != target.interpretation ||
				source.presence.universe != target.presence.universe ||
				!std::ranges::includes(target.presence.fragments, source.presence.fragments))
				return false;
			if (reference.source_columns.size() != reference.target_columns.size())
				return false;
			for (std::size_t index{}; index < reference.source_columns.size(); ++index)
			{
				const auto left = source.row.cells.find(reference.source_columns[index]);
				const auto right = target.row.cells.find(reference.target_columns[index]);
				if (left == source.row.cells.end() || right == target.row.cells.end() ||
					left->second.state != sdk::cell_state::present ||
					right->second.state != sdk::cell_state::present || !left->second.value ||
					!right->second.value)
					return false;
				if (reference.container_elements)
				{
					if (!element)
						return false;
					const auto* target_value = std::get_if<std::string>(&*right->second.value);
					if (target_value == nullptr || *target_value != *element)
						return false;
				}
				else if (*left->second.value != *right->second.value)
					return false;
			}
			return true;
		}

		[[nodiscard]] sdk::result<std::vector<std::vector<std::byte>>>
		reference_targets(const sdk::claim& source,
						  const sdk::relation_reference_descriptor& reference,
						  const std::span<const sdk::claim* const> claims,
						  const sdk::relation_engine& engine)
		{
			if (std::ranges::any_of(reference.source_columns,
									[&](const auto& column)
									{
										const auto found = source.row.cells.find(column);
										return found == source.row.cells.end() ||
											found->second.state == sdk::cell_state::absent;
									}))
				return std::vector<std::vector<std::byte>>{};

			std::vector<std::optional<std::string_view>> elements;
			if (reference.container_elements)
			{
				auto decoded = decode_reference_elements(source, reference);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				for (const auto& value : *decoded)
					elements.emplace_back(value);
				if (elements.empty())
					return sdk::unexpected(
						receipt_error("result-oracle", "reference-container-empty"));
			}
			else
				elements.emplace_back(std::nullopt);

			std::vector<std::vector<std::byte>> output;
			for (const auto element : elements)
			{
				const auto found = std::ranges::find_if(
					claims,
					[&](const sdk::claim* target)
					{
						return reference_matches(source, *target, reference, element, engine);
					});
				if (found == claims.end())
					return sdk::unexpected(
						receipt_error("result-oracle", "reference-target-missing"));
				auto occurrence = sdk::detail::claim_occurrence_projection(**found);
				if (!occurrence)
					return sdk::unexpected(std::move(occurrence.error()));
				output.push_back(std::move(*occurrence));
			}
			std::ranges::sort(output);
			if (std::ranges::adjacent_find(output) != output.end())
				return sdk::unexpected(
					receipt_error("result-oracle", "reference-target-duplicate"));
			return output;
		}

		[[nodiscard]] sdk::result<
			std::pair<std::vector<std::vector<std::byte>>, std::vector<std::vector<std::byte>>>>
		claim_reference_targets(const sdk::claim& source,
								const std::span<const sdk::claim* const> claims,
								const sdk::relation_engine& engine)
		{
			auto descriptor = engine.require_id(source.descriptor);
			if (!descriptor)
				return sdk::unexpected(std::move(descriptor.error()));
			std::vector<std::vector<std::byte>> hard;
			std::vector<std::vector<std::byte>> soft;
			for (const auto& reference : descriptor->descriptor().references)
			{
				auto targets = reference_targets(source, reference, claims, engine);
				if (!targets)
					return sdk::unexpected(std::move(targets.error()));
				auto& destination =
					reference.strength == sdk::reference_strength::hard ? hard : soft;
				destination.insert(destination.end(),
								   std::make_move_iterator(targets->begin()),
								   std::make_move_iterator(targets->end()));
			}
			return std::pair{std::move(hard), std::move(soft)};
		}

		[[nodiscard]] sdk::result<std::string>
		exact_partition_digest(const std::string_view domain,
							   const std::string_view partition_id,
							   const std::vector<std::vector<std::byte>>& projections)
		{
			std::vector<sdk::canonical_value> values;
			values.reserve(projections.size());
			for (const auto& projection : projections)
				values.push_back(bytes(projection));
			return semantic_projection(
				domain,
				tuple({text(partition_id),
					   u64_bytes(static_cast<std::uint64_t>(projections.size())),
					   tuple(std::move(values))}));
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

	sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const validated_materialization_request& request,
		const std::size_t task_index,
		const sealed_materialization_result& result,
		const std::span<const std::string> partition_ids,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		try
		{
			if (task_index >= request.tasks.size() ||
				(!partition_ids.empty() &&
				 (!std::ranges::is_sorted(partition_ids) ||
				  std::ranges::adjacent_find(partition_ids) != partition_ids.end())))
				return sdk::unexpected(receipt_error("bounded-result-oracle", "identity-or-order"));
			for (const auto& partition_id : partition_ids)
				if (!sdk::validate_strong_id(partition_id))
					return sdk::unexpected(receipt_error("bounded-result-oracle", "partition-id"));

			auto bounded = construct_materialization_bounded_task_claims(
				request, task_index, result, producer_authority, guarantee_authority);
			if (!bounded)
				return sdk::unexpected(std::move(bounded.error()));
			const auto task_id = request.tasks[task_index].provider_task_id;
			if (result.provider_task_id() != task_id)
				return sdk::unexpected(receipt_error("bounded-result-oracle", "task-id"));

			std::map<std::string, const materialization_claim_partition*, std::less<>> partitions;
			std::vector<const sdk::claim*> all_claims;
			std::map<std::string, std::pair<std::string, std::string>, std::less<>> claim_locations;
			std::map<std::string, const sdk::claim*, std::less<>> claims_by_ref;
			for (const auto& partition : bounded->partitions)
			{
				if (!partitions.emplace(partition.manifest.partition_id, &partition).second)
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "duplicate-partition"));
				if (partition.stored_claim_refs.size() != partition.draft.claims.size())
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "claim-reference-census"));
				for (std::size_t index{}; index < partition.draft.claims.size(); ++index)
				{
					const auto& claim = partition.draft.claims[index];
					all_claims.push_back(&claim);
					const auto& claim_ref = partition.stored_claim_refs[index];
					const auto [found, inserted] = claim_locations.emplace(
						claim_ref, std::pair{partition.manifest.partition_id, claim.content});
					if (!inserted &&
						found->second != std::pair{partition.manifest.partition_id, claim.content})
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "claim-reference-alias"));
					claims_by_ref.emplace(claim_ref, &claim);
				}
			}
			std::vector<std::string> actual_partition_ids;
			actual_partition_ids.reserve(partitions.size());
			for (const auto& [partition_id, partition] : partitions)
			{
				(void)partition;
				actual_partition_ids.push_back(partition_id);
			}
			std::vector<std::string> selected_partition_ids;
			if (partition_ids.empty())
			{
				selected_partition_ids = actual_partition_ids;
			}
			else
				selected_partition_ids.assign(partition_ids.begin(), partition_ids.end());
			if (selected_partition_ids.empty() ||
				std::ranges::adjacent_find(selected_partition_ids) !=
					selected_partition_ids.end() ||
				selected_partition_ids != actual_partition_ids)
				return sdk::unexpected(receipt_error("bounded-result-oracle", "partition-census"));
			for (const auto& partition_id : selected_partition_ids)
				if (!partitions.contains(partition_id))
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "partition-missing"));

			const std::span<const sdk::claim*> claim_span{all_claims};
			std::map<std::string,
					 std::vector<const materialization_origin_association*>,
					 std::less<>>
				associations_by_partition;
			for (const auto& association : bounded->origin_associations)
			{
				const auto found = claim_locations.find(association.stored_claim_ref);
				if (found == claim_locations.end())
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "association-orphan"));
				associations_by_partition[found->second.first].push_back(&association);
			}

			std::vector<materialization_incremental_event_projection> output;
			for (const auto& partition_id : selected_partition_ids)
			{
				const auto& partition = *partitions.at(partition_id);
				std::vector<materialization_incremental_event_projection> partition_events;
				const auto append_event =
					[&](const materialization_partition_event_kind kind,
						std::vector<sdk::canonical_value> key_values,
						std::vector<sdk::canonical_value> payload_values) -> sdk::result<void>
				{
					auto key = event_tuple(std::move(key_values));
					if (!key)
						return sdk::unexpected(std::move(key.error()));
					auto payload = event_tuple(std::move(payload_values));
					if (!payload)
						return sdk::unexpected(std::move(payload.error()));
					partition_events.push_back({task_id,
												std::string{partition_id},
												kind,
												std::move(*key),
												std::move(*payload)});
					return {};
				};

				if (auto valid = append_event(materialization_partition_event_kind::partition_begin,
											  {text(task_id), text(partition_id)},
											  {text(partition.draft.relation_descriptor_id),
											   text(partition.draft.scope),
											   text(partition.draft.condition.canonical_form()),
											   text(partition.draft.interpretation),
											   text(partition.draft.producer_semantics),
											   text(partition.draft.producer_input_basis_digest),
											   text(partition.draft.precision_profile),
											   text(partition.draft.assumption_set_id)});
					!valid)
					return sdk::unexpected(std::move(valid.error()));

				std::vector<std::pair<std::vector<std::byte>, const sdk::claim*>> ordered_claims;
				ordered_claims.reserve(partition.draft.claims.size());
				for (const auto& claim : partition.draft.claims)
				{
					auto occurrence = sdk::detail::claim_occurrence_projection(claim);
					if (!occurrence)
						return sdk::unexpected(std::move(occurrence.error()));
					ordered_claims.emplace_back(std::move(*occurrence), &claim);
				}
				std::ranges::sort(ordered_claims,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [occurrence, claim] : ordered_claims)
				{
					auto content_projection = claim_content_projection(*claim, request.engine);
					if (!content_projection)
						return sdk::unexpected(std::move(content_projection.error()));
					auto claim_key = canonical_bytes_value(*content_projection);
					auto claim_content = canonical_bytes_value(*content_projection);
					auto metadata =
						canonical_bytes_value(claim_occurrence_metadata_projection(*claim));
					auto targets = claim_reference_targets(*claim, claim_span, request.engine);
					if (!claim_key || !claim_content || !metadata || !targets)
						return sdk::unexpected(!claim_key ? std::move(claim_key.error())
												   : !claim_content
												   ? std::move(claim_content.error())
												   : !metadata ? std::move(metadata.error())
															   : std::move(targets.error()));
					auto hard = ordered_canonical_bytes(std::move(targets->first));
					auto soft = ordered_canonical_bytes(std::move(targets->second));
					auto functional =
						ordered_canonical_bytes(std::vector<std::vector<std::byte>>{});
					auto differential =
						ordered_canonical_bytes(std::vector<std::vector<std::byte>>{});
					if (!hard || !soft || !functional || !differential)
						return sdk::unexpected(receipt_error("bounded-result-oracle", "claim-law"));
					if (auto valid =
							append_event(materialization_partition_event_kind::claim_occurrence,
										 {text(task_id),
										  text(partition_id),
										  sdk::canonical_value::from_bytes(occurrence),
										  std::move(*claim_key)},
										 {std::move(*claim_content),
										  std::move(*metadata),
										  std::move(*hard),
										  std::move(*soft),
										  std::move(*functional),
										  std::move(*differential)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::set<std::vector<std::byte>> row_identities;
				for (const auto& claim : partition.draft.claims)
				{
					auto row = canonical_bytes_value(text(claim.row.canonical_form()));
					if (!row)
						return sdk::unexpected(std::move(row.error()));
					if (!row_identities.insert(row->byte_string).second)
						continue;
					if (auto valid =
							append_event(materialization_partition_event_kind::detached_row,
										 {text(task_id),
										  text(partition_id),
										  text(claim.row.descriptor_id),
										  *row},
										 {*row});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::vector<const materialization_origin_association*> associations =
					associations_by_partition[std::string{partition_id}];
				std::vector<
					std::pair<std::vector<std::byte>, const materialization_origin_association*>>
					ordered_associations;
				ordered_associations.reserve(associations.size());
				for (const auto* association : associations)
				{
					const auto claim = claims_by_ref.find(association->stored_claim_ref);
					if (claim == claims_by_ref.end())
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "annotation-claim"));
					auto annotation = canonical_bytes_value(annotation_projection(*claim->second));
					if (!annotation)
						return sdk::unexpected(std::move(annotation.error()));
					ordered_associations.emplace_back(std::move(annotation->byte_string),
													  association);
				}
				std::ranges::sort(ordered_associations,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [annotation, association] : ordered_associations)
				{
					const auto claim = claims_by_ref.find(association->stored_claim_ref);
					if (claim == claims_by_ref.end())
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "annotation-claim"));
					auto order_key = canonical_bytes_value(
						tuple({bytes(annotation), text(association->association_id)}));
					if (!order_key)
						return sdk::unexpected(std::move(order_key.error()));
					if (auto valid =
							append_event(materialization_partition_event_kind::claim_annotation,
										 {text(task_id),
										  text(partition_id),
										  text(claim->second->content),
										  std::move(*order_key)},
										 {sdk::canonical_value::from_bytes(annotation)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::vector<std::pair<std::vector<std::byte>, sdk::snapshot_coverage_unit>>
					coverage;
				coverage.reserve(partition.draft.coverage.size());
				for (const auto& item : partition.draft.coverage)
				{
					auto encoded = canonical_bytes_value(coverage_projection(item));
					if (!encoded)
						return sdk::unexpected(std::move(encoded.error()));
					coverage.emplace_back(std::move(encoded->byte_string), item);
				}
				std::ranges::sort(coverage,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [encoded, item] : coverage)
					if (auto valid = append_event(materialization_partition_event_kind::coverage,
												  {text(task_id),
												   text(partition_id),
												   sdk::canonical_value::from_bytes(encoded)},
												  {sdk::canonical_value::from_bytes(encoded)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));

				std::vector<std::pair<std::vector<std::byte>, sdk::unresolved_reference>>
					unresolved;
				unresolved.reserve(partition.draft.unresolved.size());
				for (const auto& item : partition.draft.unresolved)
				{
					auto encoded = canonical_bytes_value(unresolved_projection(item));
					if (!encoded)
						return sdk::unexpected(std::move(encoded.error()));
					unresolved.emplace_back(std::move(encoded->byte_string), item);
				}
				std::ranges::sort(unresolved,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [encoded, item] : unresolved)
					if (auto valid = append_event(materialization_partition_event_kind::unresolved,
												  {text(task_id),
												   text(partition_id),
												   sdk::canonical_value::from_bytes(encoded)},
												  {sdk::canonical_value::from_bytes(encoded)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));

				std::vector<
					std::pair<std::vector<std::byte>, materialization_incremental_event_projection>>
					ordered_pre_end;
				ordered_pre_end.reserve(partition_events.size());
				std::vector<std::vector<std::byte>> claim_projections;
				std::vector<std::vector<std::byte>> row_projections;
				std::vector<std::vector<std::byte>> coverage_projections;
				std::vector<std::vector<std::byte>> unresolved_projections;
				for (auto& event : partition_events)
				{
					auto full = full_event_projection(event);
					if (!full)
						return sdk::unexpected(std::move(full.error()));
					if (event.kind == materialization_partition_event_kind::claim_occurrence)
						claim_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::detached_row)
						row_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::coverage)
						coverage_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::unresolved)
						unresolved_projections.push_back(*full);
					ordered_pre_end.emplace_back(std::move(*full), std::move(event));
				}
				std::ranges::sort(ordered_pre_end,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				std::vector<std::vector<std::byte>> ordered_full;
				ordered_full.reserve(ordered_pre_end.size());
				for (const auto& entry : ordered_pre_end)
					ordered_full.push_back(entry.first);
				auto event_digest =
					exact_partition_digest("cxxlens.df-0200.partition-event-full-projection.v1",
										   partition_id,
										   ordered_full);
				auto claim_digest =
					exact_partition_digest("cxxlens.df-0200.claim-occurrence-full-projection.v1",
										   partition_id,
										   claim_projections);
				auto row_digest =
					exact_partition_digest("cxxlens.df-0200.detached-row-full-projection.v1",
										   partition_id,
										   row_projections);
				auto coverage_digest =
					exact_partition_digest("cxxlens.df-0200.coverage-full-projection.v1",
										   partition_id,
										   coverage_projections);
				auto unresolved_digest =
					exact_partition_digest("cxxlens.df-0200.unresolved-full-projection.v1",
										   partition_id,
										   unresolved_projections);
				if (!event_digest || !claim_digest || !row_digest || !coverage_digest ||
					!unresolved_digest)
					return sdk::unexpected(receipt_error("bounded-result-oracle", "digest"));
				const auto event_count = ordered_pre_end.size() + 1U;
				if (event_count > std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(receipt_error("bounded-result-oracle", "event-count"));
				if (auto valid = append_event(
						materialization_partition_event_kind::partition_end,
						{text(task_id), text(partition_id)},
						{u64_bytes(static_cast<std::uint64_t>(event_count)),
						 u64_bytes(static_cast<std::uint64_t>(claim_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(row_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(coverage_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(unresolved_projections.size())),
						 text(*event_digest),
						 text(*claim_digest),
						 text(*row_digest),
						 text(*coverage_digest),
						 text(*unresolved_digest),
						 text(partition.manifest.content_digest)});
					!valid)
					return sdk::unexpected(std::move(valid.error()));

				for (auto& entry : ordered_pre_end)
					output.push_back(std::move(entry.second));
				output.push_back(std::move(partition_events.back()));
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
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "duplicate-final-event"));
			output.clear();
			output.reserve(ordered.size());
			for (auto& entry : ordered)
				output.push_back(std::move(entry.second));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(receipt_error("bounded-result-oracle", "allocation"));
		}
	}

	sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const materialization_v2_1_claim_authority& authority,
		const std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const sealed_materialization_result& result,
		const std::span<const std::string> partition_ids)
	{
		try
		{
			if (task_index >= authority.task_count() || task.metadata.task_index != task_index ||
				(!partition_ids.empty() &&
				 (!std::ranges::is_sorted(partition_ids) ||
				  std::ranges::adjacent_find(partition_ids) != partition_ids.end())))
				return sdk::unexpected(receipt_error("bounded-result-oracle", "identity-or-order"));
			for (const auto& partition_id : partition_ids)
				if (!sdk::validate_strong_id(partition_id))
					return sdk::unexpected(receipt_error("bounded-result-oracle", "partition-id"));
			auto bounded =
				construct_materialization_bounded_task_claims(authority, task_index, task, result);
			if (!bounded)
				return sdk::unexpected(std::move(bounded.error()));
			const auto task_id = task.metadata.provider_task_id;
			if (result.provider_task_id() != task_id || authority.engine() == nullptr)
				return sdk::unexpected(receipt_error("bounded-result-oracle", "task-id-or-engine"));

			std::map<std::string, const materialization_claim_partition*, std::less<>> partitions;
			std::vector<const sdk::claim*> all_claims;
			std::map<std::string, std::pair<std::string, std::string>, std::less<>> claim_locations;
			std::map<std::string, const sdk::claim*, std::less<>> claims_by_ref;
			for (const auto& partition : bounded->partitions)
			{
				if (!partitions.emplace(partition.manifest.partition_id, &partition).second)
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "duplicate-partition"));
				if (partition.stored_claim_refs.size() != partition.draft.claims.size())
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "claim-reference-census"));
				for (std::size_t index{}; index < partition.draft.claims.size(); ++index)
				{
					const auto& claim = partition.draft.claims[index];
					all_claims.push_back(&claim);
					const auto& claim_ref = partition.stored_claim_refs[index];
					const auto [found, inserted] = claim_locations.emplace(
						claim_ref, std::pair{partition.manifest.partition_id, claim.content});
					if (!inserted &&
						found->second != std::pair{partition.manifest.partition_id, claim.content})
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "claim-reference-alias"));
					claims_by_ref.emplace(claim_ref, &claim);
				}
			}
			std::vector<std::string> actual_partition_ids;
			actual_partition_ids.reserve(partitions.size());
			for (const auto& [partition_id, partition] : partitions)
			{
				(void)partition;
				actual_partition_ids.push_back(partition_id);
			}
			std::vector<std::string> selected_partition_ids;
			if (partition_ids.empty())
				selected_partition_ids = actual_partition_ids;
			else
				selected_partition_ids.assign(partition_ids.begin(), partition_ids.end());
			if (selected_partition_ids.empty() ||
				std::ranges::adjacent_find(selected_partition_ids) !=
					selected_partition_ids.end() ||
				selected_partition_ids != actual_partition_ids)
				return sdk::unexpected(receipt_error("bounded-result-oracle", "partition-census"));
			const std::span<const sdk::claim*> claim_span{all_claims};
			std::map<std::string,
					 std::vector<const materialization_origin_association*>,
					 std::less<>>
				associations_by_partition;
			for (const auto& association : bounded->origin_associations)
			{
				const auto found = claim_locations.find(association.stored_claim_ref);
				if (found == claim_locations.end())
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "association-orphan"));
				associations_by_partition[found->second.first].push_back(&association);
			}

			std::vector<materialization_incremental_event_projection> output;
			for (const auto& partition_id : selected_partition_ids)
			{
				const auto& partition = *partitions.at(partition_id);
				std::vector<materialization_incremental_event_projection> partition_events;
				const auto append_event =
					[&](const materialization_partition_event_kind kind,
						std::vector<sdk::canonical_value> key_values,
						std::vector<sdk::canonical_value> payload_values) -> sdk::result<void>
				{
					auto key = event_tuple(std::move(key_values));
					if (!key)
						return sdk::unexpected(std::move(key.error()));
					auto payload = event_tuple(std::move(payload_values));
					if (!payload)
						return sdk::unexpected(std::move(payload.error()));
					partition_events.push_back({task_id,
												std::string{partition_id},
												kind,
												std::move(*key),
												std::move(*payload)});
					return {};
				};

				if (auto valid = append_event(materialization_partition_event_kind::partition_begin,
											  {text(task_id), text(partition_id)},
											  {text(partition.draft.relation_descriptor_id),
											   text(partition.draft.scope),
											   text(partition.draft.condition.canonical_form()),
											   text(partition.draft.interpretation),
											   text(partition.draft.producer_semantics),
											   text(partition.draft.producer_input_basis_digest),
											   text(partition.draft.precision_profile),
											   text(partition.draft.assumption_set_id)});
					!valid)
					return sdk::unexpected(std::move(valid.error()));

				std::vector<std::pair<std::vector<std::byte>, const sdk::claim*>> ordered_claims;
				ordered_claims.reserve(partition.draft.claims.size());
				for (const auto& claim : partition.draft.claims)
				{
					auto occurrence = sdk::detail::claim_occurrence_projection(claim);
					if (!occurrence)
						return sdk::unexpected(std::move(occurrence.error()));
					ordered_claims.emplace_back(std::move(*occurrence), &claim);
				}
				std::ranges::sort(ordered_claims,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [occurrence, claim] : ordered_claims)
				{
					auto content_projection = claim_content_projection(*claim, *authority.engine());
					if (!content_projection)
						return sdk::unexpected(std::move(content_projection.error()));
					auto claim_key = canonical_bytes_value(*content_projection);
					auto claim_content = canonical_bytes_value(*content_projection);
					auto metadata =
						canonical_bytes_value(claim_occurrence_metadata_projection(*claim));
					auto targets = claim_reference_targets(*claim, claim_span, *authority.engine());
					if (!claim_key || !claim_content || !metadata || !targets)
						return sdk::unexpected(!claim_key ? std::move(claim_key.error())
												   : !claim_content
												   ? std::move(claim_content.error())
												   : !metadata ? std::move(metadata.error())
															   : std::move(targets.error()));
					auto hard = ordered_canonical_bytes(std::move(targets->first));
					auto soft = ordered_canonical_bytes(std::move(targets->second));
					auto functional =
						ordered_canonical_bytes(std::vector<std::vector<std::byte>>{});
					auto differential =
						ordered_canonical_bytes(std::vector<std::vector<std::byte>>{});
					if (!hard || !soft || !functional || !differential)
						return sdk::unexpected(receipt_error("bounded-result-oracle", "claim-law"));
					if (auto valid =
							append_event(materialization_partition_event_kind::claim_occurrence,
										 {text(task_id),
										  text(partition_id),
										  sdk::canonical_value::from_bytes(occurrence),
										  std::move(*claim_key)},
										 {std::move(*claim_content),
										  std::move(*metadata),
										  std::move(*hard),
										  std::move(*soft),
										  std::move(*functional),
										  std::move(*differential)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::set<std::vector<std::byte>> row_identities;
				for (const auto& claim : partition.draft.claims)
				{
					auto row = canonical_bytes_value(text(claim.row.canonical_form()));
					if (!row)
						return sdk::unexpected(std::move(row.error()));
					if (!row_identities.insert(row->byte_string).second)
						continue;
					if (auto valid =
							append_event(materialization_partition_event_kind::detached_row,
										 {text(task_id),
										  text(partition_id),
										  text(claim.row.descriptor_id),
										  *row},
										 {*row});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::vector<const materialization_origin_association*> associations =
					associations_by_partition[std::string{partition_id}];
				std::vector<
					std::pair<std::vector<std::byte>, const materialization_origin_association*>>
					ordered_associations;
				ordered_associations.reserve(associations.size());
				for (const auto* association : associations)
				{
					const auto claim = claims_by_ref.find(association->stored_claim_ref);
					if (claim == claims_by_ref.end())
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "annotation-claim"));
					auto annotation = canonical_bytes_value(annotation_projection(*claim->second));
					if (!annotation)
						return sdk::unexpected(std::move(annotation.error()));
					ordered_associations.emplace_back(std::move(annotation->byte_string),
													  association);
				}
				std::ranges::sort(ordered_associations,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [annotation, association] : ordered_associations)
				{
					const auto claim = claims_by_ref.find(association->stored_claim_ref);
					if (claim == claims_by_ref.end())
						return sdk::unexpected(
							receipt_error("bounded-result-oracle", "annotation-claim"));
					auto order_key = canonical_bytes_value(
						tuple({bytes(annotation), text(association->association_id)}));
					if (!order_key)
						return sdk::unexpected(std::move(order_key.error()));
					if (auto valid =
							append_event(materialization_partition_event_kind::claim_annotation,
										 {text(task_id),
										  text(partition_id),
										  text(claim->second->content),
										  std::move(*order_key)},
										 {sdk::canonical_value::from_bytes(annotation)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));
				}

				std::vector<std::pair<std::vector<std::byte>, sdk::snapshot_coverage_unit>>
					coverage;
				coverage.reserve(partition.draft.coverage.size());
				for (const auto& item : partition.draft.coverage)
				{
					auto encoded = canonical_bytes_value(coverage_projection(item));
					if (!encoded)
						return sdk::unexpected(std::move(encoded.error()));
					coverage.emplace_back(std::move(encoded->byte_string), item);
				}
				std::ranges::sort(coverage,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [encoded, item] : coverage)
					if (auto valid = append_event(materialization_partition_event_kind::coverage,
												  {text(task_id),
												   text(partition_id),
												   sdk::canonical_value::from_bytes(encoded)},
												  {sdk::canonical_value::from_bytes(encoded)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));

				std::vector<std::pair<std::vector<std::byte>, sdk::unresolved_reference>>
					unresolved;
				unresolved.reserve(partition.draft.unresolved.size());
				for (const auto& item : partition.draft.unresolved)
				{
					auto encoded = canonical_bytes_value(unresolved_projection(item));
					if (!encoded)
						return sdk::unexpected(std::move(encoded.error()));
					unresolved.emplace_back(std::move(encoded->byte_string), item);
				}
				std::ranges::sort(unresolved,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				for (const auto& [encoded, item] : unresolved)
					if (auto valid = append_event(materialization_partition_event_kind::unresolved,
												  {text(task_id),
												   text(partition_id),
												   sdk::canonical_value::from_bytes(encoded)},
												  {sdk::canonical_value::from_bytes(encoded)});
						!valid)
						return sdk::unexpected(std::move(valid.error()));

				std::vector<
					std::pair<std::vector<std::byte>, materialization_incremental_event_projection>>
					ordered_pre_end;
				ordered_pre_end.reserve(partition_events.size());
				std::vector<std::vector<std::byte>> claim_projections;
				std::vector<std::vector<std::byte>> row_projections;
				std::vector<std::vector<std::byte>> coverage_projections;
				std::vector<std::vector<std::byte>> unresolved_projections;
				for (auto& event : partition_events)
				{
					auto full = full_event_projection(event);
					if (!full)
						return sdk::unexpected(std::move(full.error()));
					if (event.kind == materialization_partition_event_kind::claim_occurrence)
						claim_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::detached_row)
						row_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::coverage)
						coverage_projections.push_back(*full);
					else if (event.kind == materialization_partition_event_kind::unresolved)
						unresolved_projections.push_back(*full);
					ordered_pre_end.emplace_back(std::move(*full), std::move(event));
				}
				std::ranges::sort(ordered_pre_end,
								  [](const auto& left, const auto& right)
								  {
									  return left.first < right.first;
								  });
				std::vector<std::vector<std::byte>> ordered_full;
				ordered_full.reserve(ordered_pre_end.size());
				for (const auto& entry : ordered_pre_end)
					ordered_full.push_back(entry.first);
				auto event_digest =
					exact_partition_digest("cxxlens.df-0200.partition-event-full-projection.v1",
										   partition_id,
										   ordered_full);
				auto claim_digest =
					exact_partition_digest("cxxlens.df-0200.claim-occurrence-full-projection.v1",
										   partition_id,
										   claim_projections);
				auto row_digest =
					exact_partition_digest("cxxlens.df-0200.detached-row-full-projection.v1",
										   partition_id,
										   row_projections);
				auto coverage_digest =
					exact_partition_digest("cxxlens.df-0200.coverage-full-projection.v1",
										   partition_id,
										   coverage_projections);
				auto unresolved_digest =
					exact_partition_digest("cxxlens.df-0200.unresolved-full-projection.v1",
										   partition_id,
										   unresolved_projections);
				if (!event_digest || !claim_digest || !row_digest || !coverage_digest ||
					!unresolved_digest)
					return sdk::unexpected(receipt_error("bounded-result-oracle", "digest"));
				const auto event_count = ordered_pre_end.size() + 1U;
				if (event_count > std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(receipt_error("bounded-result-oracle", "event-count"));
				if (auto valid = append_event(
						materialization_partition_event_kind::partition_end,
						{text(task_id), text(partition_id)},
						{u64_bytes(static_cast<std::uint64_t>(event_count)),
						 u64_bytes(static_cast<std::uint64_t>(claim_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(row_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(coverage_projections.size())),
						 u64_bytes(static_cast<std::uint64_t>(unresolved_projections.size())),
						 text(*event_digest),
						 text(*claim_digest),
						 text(*row_digest),
						 text(*coverage_digest),
						 text(*unresolved_digest),
						 text(partition.manifest.content_digest)});
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				for (auto& entry : ordered_pre_end)
					output.push_back(std::move(entry.second));
				output.push_back(std::move(partition_events.back()));
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
					return sdk::unexpected(
						receipt_error("bounded-result-oracle", "duplicate-final-event"));
			output.clear();
			output.reserve(ordered.size());
			for (auto& entry : ordered)
				output.push_back(std::move(entry.second));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(receipt_error("bounded-result-oracle", "allocation"));
		}
	}

	sdk::result<std::string>
	materialization_incremental_request_id(const validated_materialization_request& request)
	{
		return request_id_of(request);
	}

	sdk::result<std::string>
	materialization_incremental_request_id(const materialization_v2_1_claim_authority& authority)
	{
		if (!sdk::validate_strong_id(authority.materialization_request_id()))
			return sdk::unexpected(receipt_error("materialization-request-id", "invalid"));
		return authority.materialization_request_id();
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

	sdk::result<std::string> seal_materialization_incremental_selected_request_entry_binding(
		const materialization_v2_1_claim_authority& authority,
		const std::size_t task_index,
		const materialization_v2_1_task_execution& task)
	{
		if (task_index >= authority.task_count() || task.metadata.task_index != task_index ||
			task.metadata.provider_task_id.empty() ||
			task.metadata.source_content_digest != task.input.source_content_digest ||
			task_index > std::numeric_limits<std::uint64_t>::max() ||
			!content_digest(task.input.source_content_digest))
			return sdk::unexpected(receipt_error("selected-request-entry", "task-binding"));
		return semantic_projection("cxxlens.df-0200.selected-request-entry-binding.v1",
								   tuple({text(authority.materialization_request_id()),
										  text(task.metadata.provider_task_id),
										  u64_bytes(static_cast<std::uint64_t>(task_index)),
										  text(task.input.source_content_digest),
										  u64_bytes(task.input.budget.output_bytes),
										  u64_bytes(task.input.budget.rows)}));
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

	sdk::result<materialization_incremental_task_receipt>
	make_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		const std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const std::uint64_t provider_stdout_byte_count,
		std::string provider_stdout_sha256,
		const std::uint64_t decoded_provider_frame_count,
		std::string provider_frame_transcript_digest,
		std::string provider_sealed_transcript_digest,
		const std::span<const materialization_incremental_event_projection> events)
	{
		if (task_index >= authority.task_count() || task.metadata.task_index != task_index ||
			events.empty() || provider_stdout_byte_count == 0U ||
			decoded_provider_frame_count == 0U || !content_digest(provider_stdout_sha256) ||
			!semantic_digest(provider_frame_transcript_digest) ||
			!semantic_digest(provider_sealed_transcript_digest))
			return sdk::unexpected(receipt_error("task-receipt", "runtime-fields"));
		auto request_id = materialization_incremental_request_id(authority);
		if (!request_id)
			return sdk::unexpected(std::move(request_id.error()));
		auto selected = seal_materialization_incremental_selected_request_entry_binding(
			authority, task_index, task);
		if (!selected)
			return sdk::unexpected(std::move(selected.error()));
		const auto task_id = task.metadata.provider_task_id;
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

	sdk::result<void> validate_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		const std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const materialization_incremental_task_receipt& receipt)
	{
		if (task_index >= authority.task_count() || task.metadata.task_index != task_index ||
			!receipt.successful_seal || receipt.canonical_task_ordinal != task_index)
			return sdk::unexpected(receipt_error("task-receipt", "ordinal-or-success"));
		if (receipt.materialization_request_id != authority.materialization_request_id() ||
			receipt.task_id != task.metadata.provider_task_id)
			return sdk::unexpected(receipt_error("task-receipt", "request-or-task-binding"));
		auto selected = seal_materialization_incremental_selected_request_entry_binding(
			authority, task_index, task);
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
