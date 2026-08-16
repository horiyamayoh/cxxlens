#include "materialization_incremental_ingress.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "sdk/provider_runtime_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		constexpr std::string_view semantic_marker{"cxxlens-semantic-digest-v2"};
		constexpr std::string_view task_event_domain{
			"cxxlens.df-0200.task-event-full-projection.v1"};
		constexpr std::string_view task_partition_domain{
			"cxxlens.df-0200.task-partition-full-projection.v1"};
		constexpr std::array<std::string_view, 4U> component_domains{
			"cxxlens.df-0200.task-claim-occurrence-full-projection.v1",
			"cxxlens.df-0200.task-detached-row-full-projection.v1",
			"cxxlens.df-0200.task-coverage-full-projection.v1",
			"cxxlens.df-0200.task-unresolved-full-projection.v1",
		};

		[[nodiscard]] sdk::error ingress_error(const std::string_view field,
											   const std::string_view detail)
		{
			return {"materialization.incremental-ingress-invalid",
					std::string{field},
					std::string{detail}};
		}

		[[nodiscard]] bool checked_add(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return false;
			output = left + right;
			return true;
		}

		[[nodiscard]] bool checked_mul(const std::uint64_t left,
									   const std::uint64_t right,
									   std::uint64_t& output) noexcept
		{
			if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
				return false;
			output = left * right;
			return true;
		}

		[[nodiscard]] sdk::result<std::uint64_t> encoded_string_size(const std::string_view value)
		{
			if (value.size() > std::numeric_limits<std::uint64_t>::max())
				return sdk::unexpected(ingress_error("digest", "length-overflow"));
			return 9U + static_cast<std::uint64_t>(value.size());
		}

		[[nodiscard]] sdk::result<std::uint64_t> encoded_bytes_size(const std::uint64_t value_size)
		{
			if (value_size > std::numeric_limits<std::uint64_t>::max() - 9U)
				return sdk::unexpected(ingress_error("digest", "length-overflow"));
			return 9U + value_size;
		}

		[[nodiscard]] sdk::result<std::uint64_t> encoded_tuple_size(const std::uint64_t item_count,
																	const std::uint64_t item_bytes)
		{
			std::uint64_t framed_items{};
			if (!checked_mul(item_count, 8U, framed_items) ||
				!checked_add(9U, framed_items, framed_items) ||
				!checked_add(framed_items, item_bytes, framed_items))
				return sdk::unexpected(ingress_error("digest", "length-overflow"));
			return framed_items;
		}

		class semantic_projection_digest_builder
		{
		  public:
			[[nodiscard]] static sdk::result<semantic_projection_digest_builder>
			begin(const std::string_view domain,
				  const std::string_view task_id,
				  const std::uint64_t expected_count,
				  const std::uint64_t expected_projection_bytes)
			{
				try
				{
					semantic_projection_digest_builder output;
					output.domain_ = std::string{domain};
					output.expected_count_ = expected_count;
					output.expected_projection_bytes_ = expected_projection_bytes;
					output.accumulator_ = make_materialization_sha256_accumulator();
					if (!output.accumulator_)
						return sdk::unexpected(ingress_error("digest", "unavailable"));

					auto marker_size = encoded_string_size(semantic_marker);
					auto domain_size = encoded_string_size(domain);
					auto task_size = encoded_string_size(task_id);
					if (!marker_size || !domain_size || !task_size)
						return sdk::unexpected(ingress_error("digest", "length-overflow"));
					std::uint64_t projection_item_bytes{};
					if (!checked_mul(expected_count, 9U, projection_item_bytes) ||
						!checked_add(projection_item_bytes,
									 expected_projection_bytes,
									 projection_item_bytes))
						return sdk::unexpected(ingress_error("digest", "length-overflow"));
					auto projection_tuple_size =
						encoded_tuple_size(expected_count, projection_item_bytes);
					if (!projection_tuple_size)
						return sdk::unexpected(std::move(projection_tuple_size.error()));
					std::uint64_t inner_items{};
					if (!checked_add(*task_size, 17U, inner_items) ||
						!checked_add(inner_items, *projection_tuple_size, inner_items))
						return sdk::unexpected(ingress_error("digest", "length-overflow"));
					auto inner_tuple_size = encoded_tuple_size(3U, inner_items);
					if (!inner_tuple_size)
						return sdk::unexpected(std::move(inner_tuple_size.error()));
					auto payload_size = encoded_bytes_size(*inner_tuple_size);
					if (!payload_size)
						return sdk::unexpected(std::move(payload_size.error()));
					std::uint64_t outer_items{};
					if (!checked_add(*marker_size, *domain_size, outer_items) ||
						!checked_add(outer_items, *payload_size, outer_items))
						return sdk::unexpected(ingress_error("digest", "length-overflow"));
					auto outer_tuple_size = encoded_tuple_size(3U, outer_items);
					if (!outer_tuple_size)
						return sdk::unexpected(std::move(outer_tuple_size.error()));

					auto valid = output.update_byte(0x05U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(3U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*marker_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_string(semantic_marker);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*domain_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_string(domain);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*payload_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x03U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*inner_tuple_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x05U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(3U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*task_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_string(task_id);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(17U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x03U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(8U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(expected_count);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(*projection_tuple_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x05U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(expected_count);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					(void)outer_tuple_size;
					output.open_ = true;
					return output;
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(ingress_error("allocation", "unavailable"));
				}
			}

			[[nodiscard]] sdk::result<void> append(const std::span<const std::byte> projection)
			{
				if (!open_ || actual_count_ >= expected_count_)
					return sdk::unexpected(ingress_error("digest", "projection-count"));
				if (projection.size() > std::numeric_limits<std::uint64_t>::max())
					return sdk::unexpected(ingress_error("digest", "length-overflow"));
				std::uint64_t next_bytes{};
				if (!checked_add(actual_projection_bytes_,
								 static_cast<std::uint64_t>(projection.size()),
								 next_bytes))
					return sdk::unexpected(ingress_error("digest", "length-overflow"));
				if (auto valid = update_u64(9U + static_cast<std::uint64_t>(projection.size()));
					!valid)
					return valid;
				if (auto valid = update_byte(0x03U); !valid)
					return valid;
				if (auto valid = update_u64(static_cast<std::uint64_t>(projection.size())); !valid)
					return valid;
				if (auto valid = update(projection); !valid)
					return valid;
				actual_projection_bytes_ = next_bytes;
				++actual_count_;
				return {};
			}

			[[nodiscard]] sdk::result<std::string> finish() &&
			{
				if (!open_ || actual_count_ != expected_count_ ||
					actual_projection_bytes_ != expected_projection_bytes_)
					return sdk::unexpected(ingress_error("digest", "projection-census"));
				auto raw = accumulator_->finish();
				if (!raw || raw->size() != 71U || !raw->starts_with("sha256:"))
					return sdk::unexpected(ingress_error("digest", "finalize"));
				return "semantic-v2:" + *raw;
			}

		  private:
			[[nodiscard]] sdk::result<void> update(const std::span<const std::byte> bytes)
			{
				if (!accumulator_)
					return sdk::unexpected(ingress_error("digest", "unavailable"));
				if (auto updated = accumulator_->update(bytes); !updated)
					return sdk::unexpected(ingress_error("digest", "update"));
				return {};
			}

			[[nodiscard]] sdk::result<void> update_byte(const std::uint8_t value)
			{
				const std::array<std::byte, 1U> bytes{static_cast<std::byte>(value)};
				return update(bytes);
			}

			[[nodiscard]] sdk::result<void> update_u64(const std::uint64_t value)
			{
				std::array<std::byte, 8U> bytes{};
				for (std::size_t index{}; index < bytes.size(); ++index)
					bytes[index] = static_cast<std::byte>(
						(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
				return update(bytes);
			}

			[[nodiscard]] sdk::result<void> update_string(const std::string_view value)
			{
				if (auto valid = update_byte(0x04U); !valid)
					return valid;
				if (auto valid = update_u64(static_cast<std::uint64_t>(value.size())); !valid)
					return valid;
				return update(std::as_bytes(std::span{value.data(), value.size()}));
			}

			std::string domain_;
			std::unique_ptr<materialization_digest_accumulator> accumulator_;
			std::uint64_t expected_count_{};
			std::uint64_t expected_projection_bytes_{};
			std::uint64_t actual_count_{};
			std::uint64_t actual_projection_bytes_{};
			bool open_{};
		};

		struct component_measurement
		{
			std::uint64_t count{};
			std::uint64_t projection_bytes{};
		};

		struct stream_measurement
		{
			std::string partition_id;
			std::uint64_t frame_count{};
			component_measurement event;
			std::array<component_measurement, 4U> components{};
		};

		[[nodiscard]] sdk::result<std::pair<std::string, std::string>>
		event_identity(const std::span<const std::byte> key)
		{
			auto decoded = sdk::canonical_binary_decode(key);
			if (!decoded || decoded->type != sdk::canonical_value::kind::ordered_tuple ||
				decoded->tuple.size() < 2U ||
				decoded->tuple[0].type != sdk::canonical_value::kind::utf8_string ||
				decoded->tuple[1].type != sdk::canonical_value::kind::utf8_string ||
				!sdk::validate_strong_id(decoded->tuple[0].text) ||
				!sdk::validate_strong_id(decoded->tuple[1].text))
				return sdk::unexpected(ingress_error("event.identity", "task-or-partition"));
			return std::pair{decoded->tuple[0].text, decoded->tuple[1].text};
		}

		[[nodiscard]] std::optional<std::size_t>
		component_index(const materialization_partition_event_kind kind) noexcept
		{
			switch (kind)
			{
				case materialization_partition_event_kind::claim_occurrence:
					return 0U;
				case materialization_partition_event_kind::detached_row:
					return 1U;
				case materialization_partition_event_kind::coverage:
					return 2U;
				case materialization_partition_event_kind::unresolved:
					return 3U;
				case materialization_partition_event_kind::partition_begin:
				case materialization_partition_event_kind::claim_annotation:
				case materialization_partition_event_kind::partition_end:
					return std::nullopt;
			}
			return std::nullopt;
		}

		[[nodiscard]] sdk::result<void> add_measurement(component_measurement& output,
														const std::size_t projection_size)
		{
			if (projection_size > std::numeric_limits<std::uint64_t>::max() ||
				!checked_add(output.count, 1U, output.count) ||
				!checked_add(output.projection_bytes,
							 static_cast<std::uint64_t>(projection_size),
							 output.projection_bytes))
				return sdk::unexpected(ingress_error("event", "census-overflow"));
			return {};
		}

		[[nodiscard]] sdk::result<stream_measurement>
		inspect_stream(materialization_replayable_spool& spool,
					   const std::string_view request_id,
					   const std::string_view task_id)
		{
			try
			{
				auto receipt = validate_materialization_partition_event_stream(spool, request_id);
				if (!receipt)
					return sdk::unexpected(std::move(receipt.error()));
				stream_measurement output;
				std::optional<std::string> partition_id;
				auto replay = replay_materialization_partition_event_stream(
					spool,
					request_id,
					[&](const std::uint64_t,
						const materialization_partition_event_kind kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload) -> sdk::result<void>
					{
						auto identity = event_identity(key);
						if (!identity || identity->first != task_id)
							return sdk::unexpected(
								ingress_error("event.identity", "task-mismatch"));
						if (!partition_id)
							partition_id = identity->second;
						else if (*partition_id != identity->second)
							return sdk::unexpected(
								ingress_error("event.identity", "interleaved-partition"));
						auto projection =
							materialization_incremental_full_event_projection(kind, key, payload);
						if (!projection)
							return sdk::unexpected(std::move(projection.error()));
						if (auto valid = add_measurement(output.event, projection->size()); !valid)
							return valid;
						if (const auto component = component_index(kind))
						{
							if (auto valid = add_measurement(output.components[*component],
															 projection->size());
								!valid)
								return valid;
						}
						return {};
					});
				if (!replay || !partition_id)
					return sdk::unexpected(replay ? ingress_error("event", "partition-id")
												  : std::move(replay.error()));
				output.partition_id = std::move(*partition_id);
				output.frame_count = output.event.count;
				if (output.frame_count != receipt->actual_frame_count)
					return sdk::unexpected(ingress_error("event", "stream-census"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(ingress_error("allocation", "unavailable"));
			}
		}

		[[nodiscard]] sdk::result<std::string>
		semantic_value_digest(const std::string_view domain, const sdk::canonical_value& value)
		{
			auto encoded = sdk::canonical_binary(value);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				domain,
				std::string{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] sdk::result<void>
		replay_into_builders(materialization_replayable_spool& spool,
							 const std::string_view request_id,
							 const std::string_view task_id,
							 const std::string_view partition_id,
							 semantic_projection_digest_builder& event_builder,
							 std::array<semantic_projection_digest_builder, 4U>& component_builders,
							 semantic_projection_digest_builder& partition_builder)
		{
			auto replay = replay_materialization_partition_event_stream(
				spool,
				request_id,
				[&](const std::uint64_t,
					const materialization_partition_event_kind kind,
					const std::span<const std::byte> key,
					const std::span<const std::byte> payload) -> sdk::result<void>
				{
					auto identity = event_identity(key);
					if (!identity || identity->first != task_id || identity->second != partition_id)
						return sdk::unexpected(ingress_error("event.identity", "binding"));
					auto projection =
						materialization_incremental_full_event_projection(kind, key, payload);
					if (!projection)
						return sdk::unexpected(std::move(projection.error()));
					if (auto valid = event_builder.append(*projection); !valid)
						return valid;
					if (const auto component = component_index(kind))
						if (auto valid = component_builders[*component].append(*projection); !valid)
							return valid;
					return partition_builder.append(*projection);
				});
			if (!replay)
				return sdk::unexpected(std::move(replay.error()));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_task_streams(
			const validated_materialization_request& request,
			const std::size_t task_index,
			const std::span<const std::string> expected_partition_ids,
			const sealed_materialization_result& result,
			const materialization_incremental_task_receipt& receipt,
			std::vector<std::unique_ptr<materialization_replayable_spool>>& spools,
			const materialization_producer_authority* producer_authority,
			const materialization_guarantee_authority* guarantee_authority,
			const bool dynamic_partition_ids,
			std::vector<std::string>* discovered_partition_ids)
		{
			if (spools.empty() ||
				(!dynamic_partition_ids && spools.size() != expected_partition_ids.size()))
				return sdk::unexpected(ingress_error("partitions", "exact-census"));
			const auto task_id = request.tasks[task_index].provider_task_id;
			auto expected_events = (producer_authority != nullptr && guarantee_authority != nullptr)
				? materialization_incremental_result_event_projections(request,
																	   task_index,
																	   result,
																	   expected_partition_ids,
																	   *producer_authority,
																	   *guarantee_authority)
				: materialization_incremental_result_event_projections(result,
																	   expected_partition_ids);
			if (!expected_events)
				return sdk::unexpected(std::move(expected_events.error()));
			std::vector<std::string> oracle_partition_ids;
			for (const auto& event : *expected_events)
				if (oracle_partition_ids.empty() ||
					oracle_partition_ids.back() != event.partition_id)
					oracle_partition_ids.push_back(event.partition_id);
			if (oracle_partition_ids.empty() || !std::ranges::is_sorted(oracle_partition_ids) ||
				std::ranges::adjacent_find(oracle_partition_ids) != oracle_partition_ids.end())
				return sdk::unexpected(ingress_error("partitions", "oracle-order"));
			if (dynamic_partition_ids)
			{
				if (spools.size() != oracle_partition_ids.size())
					return sdk::unexpected(ingress_error("partitions", "exact-census"));
				if (discovered_partition_ids != nullptr)
					*discovered_partition_ids = oracle_partition_ids;
			}
			auto expected_receipt = make_materialization_incremental_task_receipt(
				request,
				task_index,
				receipt.provider_stdout_byte_count,
				receipt.provider_stdout_sha256,
				receipt.decoded_provider_frame_count,
				receipt.provider_frame_transcript_digest,
				receipt.provider_sealed_transcript_digest,
				std::span<const materialization_incremental_event_projection>{*expected_events});
			if (!expected_receipt)
				return sdk::unexpected(std::move(expected_receipt.error()));
			if (expected_receipt->partition != receipt.partition ||
				expected_receipt->event != receipt.event ||
				expected_receipt->claim != receipt.claim || expected_receipt->row != receipt.row ||
				expected_receipt->coverage != receipt.coverage ||
				expected_receipt->unresolved != receipt.unresolved ||
				expected_receipt->pre_encoder_task_receipt_seal_digest !=
					receipt.pre_encoder_task_receipt_seal_digest)
				return sdk::unexpected(ingress_error("result-oracle", "receipt-binding"));
			auto expected_provider_seal =
				sdk::provider::detail::provider_sealed_transcript_receipt_digest(
					result.provider_task_id(), "provider.success", result.provider_seal());
			if (!expected_provider_seal ||
				receipt.provider_sealed_transcript_digest != *expected_provider_seal)
				return sdk::unexpected(ingress_error("result-oracle", "provider-seal-binding"));
			std::vector<stream_measurement> summaries;
			summaries.reserve(spools.size());
			std::array<component_measurement, 4U> total_components{};
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			for (std::size_t index{}; index < spools.size(); ++index)
			{
				if (!spools[index])
					return sdk::unexpected(ingress_error("partitions", "null-spool"));
				auto summary = inspect_stream(*spools[index], *request_id, task_id);
				if (!summary)
					return sdk::unexpected(std::move(summary.error()));
				const auto& bound_partition_ids =
					dynamic_partition_ids ? oracle_partition_ids : expected_partition_ids;
				if (summary->partition_id != bound_partition_ids[index] ||
					(index != 0U && summaries.back().partition_id >= summary->partition_id))
					return sdk::unexpected(ingress_error("partitions", "order-or-binding"));
				auto expected_begin = std::ranges::find_if(
					*expected_events,
					[&](const materialization_incremental_event_projection& event)
					{
						return event.partition_id == summary->partition_id;
					});
				if (expected_begin == expected_events->end())
					return sdk::unexpected(ingress_error("result-oracle", "partition-missing"));
				auto expected_end = expected_begin;
				while (expected_end != expected_events->end() &&
					   expected_end->partition_id == summary->partition_id)
					++expected_end;
				const std::span<const materialization_incremental_event_projection>
					expected_partition{&*expected_begin,
									   static_cast<std::size_t>(expected_end - expected_begin)};
				std::size_t expected_index{};
				auto exact_replay = replay_materialization_partition_event_stream(
					*spools[index],
					*request_id,
					[&](const std::uint64_t,
						const materialization_partition_event_kind kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload) -> sdk::result<void>
					{
						if (expected_index >= expected_partition.size())
							return sdk::unexpected(
								ingress_error("result-oracle", "stream-extra-event"));
						const auto& expected = expected_partition[expected_index];
						if (expected.kind != kind || expected.key.size() != key.size() ||
							expected.payload.size() != payload.size() ||
							!std::ranges::equal(expected.key, key) ||
							!std::ranges::equal(expected.payload, payload))
							return sdk::unexpected(
								ingress_error("result-oracle", "stream-event-mismatch"));
						++expected_index;
						return {};
					});
				if (!exact_replay || expected_index != expected_partition.size())
					return sdk::unexpected(
						exact_replay ? ingress_error("result-oracle", "stream-event-missing")
									 : std::move(exact_replay.error()));
				for (std::size_t component{}; component < total_components.size(); ++component)
				{
					if (!checked_add(total_components[component].count,
									 summary->components[component].count,
									 total_components[component].count) ||
						!checked_add(total_components[component].projection_bytes,
									 summary->components[component].projection_bytes,
									 total_components[component].projection_bytes))
						return sdk::unexpected(ingress_error("event", "census-overflow"));
				}
				summaries.push_back(std::move(*summary));
			}
			component_measurement total_event;
			for (const auto& summary : summaries)
			{
				if (!checked_add(total_event.count, summary.event.count, total_event.count) ||
					!checked_add(total_event.projection_bytes,
								 summary.event.projection_bytes,
								 total_event.projection_bytes))
					return sdk::unexpected(ingress_error("event", "census-overflow"));
			}
			if (receipt.partition.count != summaries.size() ||
				receipt.event.count != total_event.count ||
				receipt.claim.count != total_components[0U].count ||
				receipt.row.count != total_components[1U].count ||
				receipt.coverage.count != total_components[2U].count ||
				receipt.unresolved.count != total_components[3U].count)
				return sdk::unexpected(ingress_error("receipt", "count-mismatch"));

			auto event_builder = semantic_projection_digest_builder::begin(
				task_event_domain, task_id, receipt.event.count, total_event.projection_bytes);
			if (!event_builder)
				return sdk::unexpected(std::move(event_builder.error()));
			auto claim_builder =
				semantic_projection_digest_builder::begin(component_domains[0U],
														  task_id,
														  receipt.claim.count,
														  total_components[0U].projection_bytes);
			auto row_builder =
				semantic_projection_digest_builder::begin(component_domains[1U],
														  task_id,
														  receipt.row.count,
														  total_components[1U].projection_bytes);
			auto coverage_builder =
				semantic_projection_digest_builder::begin(component_domains[2U],
														  task_id,
														  receipt.coverage.count,
														  total_components[2U].projection_bytes);
			auto unresolved_builder =
				semantic_projection_digest_builder::begin(component_domains[3U],
														  task_id,
														  receipt.unresolved.count,
														  total_components[3U].projection_bytes);
			if (!claim_builder || !row_builder || !coverage_builder || !unresolved_builder)
				return sdk::unexpected(ingress_error("digest", "builder"));
			std::array<semantic_projection_digest_builder, 4U> component_builders{
				std::move(*claim_builder),
				std::move(*row_builder),
				std::move(*coverage_builder),
				std::move(*unresolved_builder),
			};
			std::vector<semantic_projection_digest_builder> partition_builders;
			partition_builders.reserve(summaries.size());
			for (const auto& summary : summaries)
			{
				auto builder = semantic_projection_digest_builder::begin(
					"cxxlens.df-0200.partition-event-full-projection.v1",
					summary.partition_id,
					summary.event.count,
					summary.event.projection_bytes);
				if (!builder)
					return sdk::unexpected(std::move(builder.error()));
				partition_builders.push_back(std::move(*builder));
			}
			for (std::size_t index{}; index < spools.size(); ++index)
			{
				if (auto valid = replay_into_builders(*spools[index],
													  *request_id,
													  task_id,
													  summaries[index].partition_id,
													  *event_builder,
													  component_builders,
													  partition_builders[index]);
					!valid)
					return valid;
			}
			auto event_digest = std::move(*event_builder).finish();
			if (!event_digest || *event_digest != receipt.event.full_projection_digest)
				return sdk::unexpected(ingress_error("receipt.event", "digest-mismatch"));
			const std::array<const std::string*, 4U> expected_digests{
				&receipt.claim.full_projection_digest,
				&receipt.row.full_projection_digest,
				&receipt.coverage.full_projection_digest,
				&receipt.unresolved.full_projection_digest,
			};
			for (std::size_t index{}; index < component_builders.size(); ++index)
			{
				auto digest = std::move(component_builders[index]).finish();
				if (!digest || *digest != *expected_digests[index])
					return sdk::unexpected(ingress_error("receipt.component", "digest-mismatch"));
			}
			std::vector<sdk::canonical_value> partition_rows;
			partition_rows.reserve(partition_builders.size());
			for (std::size_t index{}; index < partition_builders.size(); ++index)
			{
				auto digest = std::move(partition_builders[index]).finish();
				if (!digest)
					return sdk::unexpected(std::move(digest.error()));
				partition_rows.push_back(sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(summaries[index].partition_id),
					sdk::canonical_value::from_string(std::move(*digest)),
				}));
			}
			std::vector<std::byte> partition_count_bytes(8U);
			for (std::size_t index{}; index < partition_count_bytes.size(); ++index)
				partition_count_bytes[index] =
					static_cast<std::byte>((static_cast<std::uint64_t>(summaries.size()) >>
											(56U - static_cast<unsigned>(index * 8U))) &
										   0xffU);
			auto partition_digest = semantic_value_digest(
				task_partition_domain,
				sdk::canonical_value::from_tuple({
					sdk::canonical_value::from_string(task_id),
					sdk::canonical_value::from_bytes(std::move(partition_count_bytes)),
					sdk::canonical_value::from_tuple(std::move(partition_rows)),
				}));
			if (!partition_digest || *partition_digest != receipt.partition.full_projection_digest)
				return sdk::unexpected(ingress_error("receipt.partition", "digest-mismatch"));
			return {};
		}
		[[nodiscard]] sdk::result<void> validate_task_streams_v2(
			const materialization_v2_1_claim_authority& authority,
			const materialization_incremental_selected_request_binding_set& binding_set,
			const std::size_t task_index,
			const std::span<const std::string> expected_partition_ids,
			const materialization_v2_1_task_execution& task,
			const sealed_materialization_result& result,
			const materialization_incremental_task_receipt& receipt,
			std::vector<std::unique_ptr<materialization_replayable_spool>>& spools,
			const bool dynamic_partition_ids,
			std::vector<std::string>* discovered_partition_ids)
		{
			if (spools.empty() ||
				(!dynamic_partition_ids && spools.size() != expected_partition_ids.size()))
				return sdk::unexpected(ingress_error("partitions", "exact-census"));
			auto expected_events = materialization_incremental_receipt_event_projections(
				authority, task_index, task, result, expected_partition_ids);
			if (!expected_events)
				return sdk::unexpected(std::move(expected_events.error()));
			auto expected_receipt = make_materialization_incremental_task_receipt(
				authority,
				binding_set,
				task_index,
				task,
				receipt.provider_stdout_byte_count,
				receipt.provider_stdout_sha256,
				receipt.decoded_provider_frame_count,
				receipt.provider_frame_transcript_digest,
				receipt.provider_sealed_transcript_digest,
				std::span<const materialization_incremental_event_projection>{*expected_events});
			if (!expected_receipt || expected_receipt->partition != receipt.partition ||
				expected_receipt->event != receipt.event ||
				expected_receipt->claim != receipt.claim || expected_receipt->row != receipt.row ||
				expected_receipt->coverage != receipt.coverage ||
				expected_receipt->unresolved != receipt.unresolved ||
				expected_receipt->pre_encoder_task_receipt_seal_digest !=
					receipt.pre_encoder_task_receipt_seal_digest)
				return sdk::unexpected(ingress_error("result-oracle", "receipt-binding"));
			auto expected_provider_seal =
				sdk::provider::detail::provider_sealed_transcript_receipt_digest(
					result.provider_task_id(), "provider.success", result.provider_seal());
			if (!expected_provider_seal ||
				receipt.provider_sealed_transcript_digest != *expected_provider_seal)
				return sdk::unexpected(ingress_error("result-oracle", "provider-seal-binding"));
			std::vector<std::string> oracle_partition_ids;
			for (const auto& event : *expected_events)
				if (oracle_partition_ids.empty() ||
					oracle_partition_ids.back() != event.partition_id)
					oracle_partition_ids.push_back(event.partition_id);
			if (oracle_partition_ids.empty() || !std::ranges::is_sorted(oracle_partition_ids) ||
				std::ranges::adjacent_find(oracle_partition_ids) != oracle_partition_ids.end())
				return sdk::unexpected(ingress_error("partitions", "oracle-order"));
			if (dynamic_partition_ids)
			{
				if (spools.size() != oracle_partition_ids.size())
					return sdk::unexpected(ingress_error("partitions", "exact-census"));
				if (discovered_partition_ids != nullptr)
					*discovered_partition_ids = oracle_partition_ids;
			}
			const auto request_id = authority.materialization_request_id();
			const auto task_id = task.metadata.provider_task_id;
			for (std::size_t index{}; index < spools.size(); ++index)
			{
				if (!spools[index])
					return sdk::unexpected(ingress_error("partitions", "null-spool"));
				const auto& partition_ids =
					dynamic_partition_ids ? oracle_partition_ids : expected_partition_ids;
				if (index >= partition_ids.size())
					return sdk::unexpected(ingress_error("partitions", "exact-census"));
				std::size_t expected_index{};
				auto expected_begin = std::ranges::find_if(
					*expected_events,
					[&](const materialization_incremental_event_projection& event)
					{
						return event.partition_id == partition_ids[index];
					});
				if (expected_begin == expected_events->end())
					return sdk::unexpected(ingress_error("result-oracle", "partition-missing"));
				auto expected_end = expected_begin;
				while (expected_end != expected_events->end() &&
					   expected_end->partition_id == partition_ids[index])
					++expected_end;
				const std::span<const materialization_incremental_event_projection>
					expected_partition{&*expected_begin,
									   static_cast<std::size_t>(expected_end - expected_begin)};
				auto replay = replay_materialization_partition_event_stream(
					*spools[index],
					request_id,
					[&](const std::uint64_t,
						const materialization_partition_event_kind kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload) -> sdk::result<void>
					{
						if (expected_index >= expected_partition.size())
							return sdk::unexpected(
								ingress_error("result-oracle", "stream-extra-event"));
						const auto& expected = expected_partition[expected_index];
						if (expected.task_id != task_id || expected.kind != kind ||
							expected.key.size() != key.size() ||
							expected.payload.size() != payload.size() ||
							!std::ranges::equal(expected.key, key) ||
							!std::ranges::equal(expected.payload, payload))
							return sdk::unexpected(
								ingress_error("result-oracle", "stream-event-mismatch"));
						++expected_index;
						return {};
					});
				if (!replay || expected_index != expected_partition.size())
					return sdk::unexpected(
						replay ? ingress_error("result-oracle", "stream-event-missing")
							   : std::move(replay.error()));
			}
			return {};
		}
	} // namespace

	materialization_incremental_ingress::materialization_incremental_ingress(
		const validated_materialization_request& request,
		std::string request_id,
		std::vector<std::vector<std::string>> expected_partition_ids,
		const materialization_producer_authority* producer_authority,
		const materialization_guarantee_authority* guarantee_authority,
		const bool dynamic_partition_ids,
		const validated_materialization_request_v2_1* v2_request,
		const materialization_v2_1_claim_authority* claim_authority,
		const std::size_t v2_task_count)
		: request_{&request}, request_id_{std::move(request_id)},
		  expected_partition_ids_{std::move(expected_partition_ids)},
		  producer_authority_{producer_authority}, guarantee_authority_{guarantee_authority},
		  dynamic_partition_ids_{dynamic_partition_ids}, v2_request_{v2_request},
		  claim_authority_{claim_authority}, v2_task_count_{v2_task_count}
	{
	}

	materialization_incremental_ingress::materialization_incremental_ingress(
		std::string request_id,
		const std::size_t task_count,
		const validated_materialization_request_v2_1& request,
		const materialization_v2_1_claim_authority& claim_authority,
		const materialization_incremental_selected_request_binding_set& binding_set)
		: request_{}, request_id_{std::move(request_id)}, expected_partition_ids_(task_count),
		  producer_authority_{}, guarantee_authority_{}, dynamic_partition_ids_{true},
		  v2_request_{&request}, claim_authority_{&claim_authority}, binding_set_{&binding_set},
		  v2_task_count_{task_count}
	{
	}

	sdk::result<materialization_incremental_ingress> materialization_incremental_ingress::begin(
		const validated_materialization_request& request,
		std::vector<std::vector<std::string>> expected_partition_ids)
	{
		try
		{
			if (request.tasks.empty() || expected_partition_ids.size() != request.tasks.size())
				return sdk::unexpected(ingress_error("tasks", "exact-census"));
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			std::set<std::string, std::less<>> all_partitions;
			for (auto& partition_ids : expected_partition_ids)
			{
				if (partition_ids.empty() || !std::ranges::is_sorted(partition_ids) ||
					std::ranges::adjacent_find(partition_ids) != partition_ids.end())
					return sdk::unexpected(ingress_error("partitions", "order-or-duplicate"));
				for (const auto& partition_id : partition_ids)
				{
					if (!sdk::validate_strong_id(partition_id) ||
						!all_partitions.insert(partition_id).second)
						return sdk::unexpected(
							ingress_error("partitions", "identity-or-duplicate"));
				}
			}
			return materialization_incremental_ingress{
				request, std::move(*request_id), std::move(expected_partition_ids)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_incremental_ingress> materialization_incremental_ingress::begin(
		const validated_materialization_request& request,
		std::vector<std::vector<std::string>> expected_partition_ids,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		try
		{
			if (request.tasks.empty() || expected_partition_ids.size() != request.tasks.size())
				return sdk::unexpected(ingress_error("tasks", "exact-census"));
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			std::set<std::string, std::less<>> all_partitions;
			for (const auto& partition_ids : expected_partition_ids)
			{
				if (partition_ids.empty() || !std::ranges::is_sorted(partition_ids) ||
					std::ranges::adjacent_find(partition_ids) != partition_ids.end())
					return sdk::unexpected(ingress_error("partitions", "order-or-duplicate"));
				for (const auto& partition_id : partition_ids)
					if (!sdk::validate_strong_id(partition_id) ||
						!all_partitions.insert(partition_id).second)
						return sdk::unexpected(
							ingress_error("partitions", "identity-or-duplicate"));
			}
			return materialization_incremental_ingress{request,
													   std::move(*request_id),
													   std::move(expected_partition_ids),
													   &producer_authority,
													   &guarantee_authority};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_incremental_ingress>
	materialization_incremental_ingress::begin_dynamic(
		const validated_materialization_request& request,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority)
	{
		try
		{
			if (request.tasks.empty())
				return sdk::unexpected(ingress_error("tasks", "exact-census"));
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			return materialization_incremental_ingress{
				request,
				std::move(*request_id),
				std::vector<std::vector<std::string>>(request.tasks.size()),
				&producer_authority,
				&guarantee_authority,
				true};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_incremental_ingress>
	materialization_incremental_ingress::begin_dynamic(
		validated_materialization_request_v2_1& request,
		const materialization_v2_1_claim_authority& claim_authority,
		const materialization_incremental_selected_request_binding_set& binding_set)
	{
		try
		{
			const auto task_count = request.request().task_count();
			if (task_count == 0U || task_count > std::numeric_limits<std::size_t>::max() ||
				claim_authority.request() != &request || claim_authority.task_count() != task_count)
				return sdk::unexpected(ingress_error("tasks", "exact-census"));
			auto expected_binding_set =
				seal_materialization_incremental_selected_request_binding_set(claim_authority);
			if (!expected_binding_set || *expected_binding_set != binding_set)
				return sdk::unexpected(ingress_error("selected-request-set", "binding"));
			auto request_id = materialization_incremental_request_id(claim_authority);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			return materialization_incremental_ingress{std::move(*request_id),
													   static_cast<std::size_t>(task_count),
													   request,
													   claim_authority,
													   binding_set};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
	}

	sdk::result<void> materialization_incremental_ingress::consume_task(
		materialization_incremental_task_ingress task) &&
	{
		try
		{
			const auto request_task_index = next_task_index_;
			if (request_ == nullptr || request_task_index >= request_->tasks.size())
				return sdk::unexpected(ingress_error("tasks", "not-next-or-missing"));
			if (auto valid = validate_materialization_incremental_task_receipt(
					*request_, request_task_index, task.receipt);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& request_task = request_->tasks[request_task_index];
			const auto& result = task.result;
			if (result.provider_task_id() != request_task.provider_task_id ||
				result.task_input_digest() != request_task.task_input_digest ||
				result.provider_execution_id() != request_task.provider_execution_id ||
				result.selected_catalog_compile_unit_id() !=
					request_task.worker_input.selected_catalog_compile_unit ||
				result.final_relation_compile_unit_id() != request_task.worker_input.compile_unit)
				return sdk::unexpected(ingress_error("task-result", "identity-mismatch"));
			std::vector<std::string> discovered_partition_ids;
			if (auto valid = validate_task_streams(
					*request_,
					request_task_index,
					std::span<const std::string>{expected_partition_ids_[request_task_index]},
					result,
					task.receipt,
					task.partition_spools,
					producer_authority_,
					guarantee_authority_,
					dynamic_partition_ids_,
					&discovered_partition_ids);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			claim_stream_tasks_.emplace_back(std::move(task.receipt),
											 std::move(task.partition_spools));
			++next_task_index_;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
		catch (...)
		{
			return sdk::unexpected(ingress_error("ingress", "unexpected"));
		}
	}

	sdk::result<void> materialization_incremental_ingress::consume_task(
		materialization_v2_1_task_execution& task_execution,
		materialization_incremental_task_ingress task) &&
	{
		try
		{
			const auto request_task_index = next_task_index_;
			if (v2_request_ == nullptr || claim_authority_ == nullptr || binding_set_ == nullptr ||
				request_task_index >= v2_task_count_)
				return sdk::unexpected(ingress_error("tasks", "not-next-or-missing"));
			if (auto valid = validate_materialization_incremental_task_receipt(*claim_authority_,
																			   *binding_set_,
																			   request_task_index,
																			   task_execution,
																			   task.receipt);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const auto& result = task.result;
			const auto& metadata = task_execution.metadata;
			if (result.provider_task_id() != metadata.provider_task_id ||
				result.task_input_digest() != metadata.task_input_digest ||
				result.provider_execution_id() != metadata.provider_execution_id ||
				result.selected_catalog_compile_unit_id() !=
					metadata.selected_catalog_compile_unit_id ||
				result.final_relation_compile_unit_id() != metadata.final_relation_compile_unit_id)
				return sdk::unexpected(ingress_error("task-result", "identity-mismatch"));
			std::vector<std::string> discovered_partition_ids;
			if (auto valid = validate_task_streams_v2(
					*claim_authority_,
					*binding_set_,
					request_task_index,
					std::span<const std::string>{expected_partition_ids_[request_task_index]},
					task_execution,
					result,
					task.receipt,
					task.partition_spools,
					true,
					&discovered_partition_ids);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			expected_partition_ids_[request_task_index] = std::move(discovered_partition_ids);
			claim_stream_tasks_.emplace_back(std::move(task.receipt),
											 std::move(task.partition_spools));
			++next_task_index_;
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
		catch (...)
		{
			return sdk::unexpected(ingress_error("ingress", "unexpected"));
		}
	}

	sdk::result<materialization_incremental_execution_journal_receipt>
	materialization_incremental_ingress::finalize() &&
	{
		auto finalized = std::move(*this).finalize_with_claim_stream();
		if (!finalized)
			return sdk::unexpected(std::move(finalized.error()));
		return std::move(finalized->journal);
	}

	sdk::result<materialization_incremental_ingress_result>
	materialization_incremental_ingress::finalize_with_claim_stream() &&
	{
		try
		{
			const auto expected_task_count =
				request_ != nullptr ? request_->tasks.size() : v2_task_count_;
			if ((request_ == nullptr && v2_request_ == nullptr) ||
				next_task_index_ != expected_task_count ||
				claim_stream_tasks_.size() != expected_task_count)
				return sdk::unexpected(ingress_error("tasks", "incomplete"));
			auto journal = seal_materialization_incremental_execution_journal(
				request_id_,
				expected_task_count,
				[&](const std::size_t index)
				{
					return index < claim_stream_tasks_.size() ? &claim_stream_tasks_[index].receipt
															  : nullptr;
				});
			if (!journal)
				return sdk::unexpected(std::move(journal.error()));
			return materialization_incremental_ingress_result{std::move(*journal),
															  std::move(claim_stream_tasks_)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(ingress_error("allocation", "unavailable"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization
