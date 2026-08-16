#include "materialization_claim_stream.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>
#include <optional>
#include <ranges>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
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
		constexpr std::string_view semantic_marker{"cxxlens-semantic-digest-v2"};

		[[nodiscard]] sdk::error stream_error(const std::string_view field,
											  const std::string_view detail)
		{
			return {
				"materialization.claim-stream-invalid", std::string{field}, std::string{detail}};
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

		[[nodiscard]] bool exact_sha256(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return (byte >= '0' && byte <= '9') ||
											(byte >= 'a' && byte <= 'f');
									});
		}

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
				return sdk::unexpected(stream_error("event.identity", "task-or-partition"));
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

		struct component_measurement
		{
			std::uint64_t count{};
			std::uint64_t projection_bytes{};
		};

		struct stream_audit
		{
			materialization_partition_event_stream_receipt receipt;
			std::string partition_id;
			component_measurement event;
			std::array<component_measurement, 4U> components{};
		};

		[[nodiscard]] sdk::result<void> add_measurement(component_measurement& output,
														const std::size_t projection_size)
		{
			if (projection_size > std::numeric_limits<std::uint64_t>::max() ||
				!checked_add(output.count, 1U, output.count) ||
				!checked_add(output.projection_bytes,
							 static_cast<std::uint64_t>(projection_size),
							 output.projection_bytes))
				return sdk::unexpected(stream_error("event", "census-overflow"));
			return {};
		}

		/**
		 * Streaming equivalent of sdk::semantic_digest(domain, tuple(task, count, projections)).
		 * Only the canonical framing prefix and one projection are live at a time.
		 */
		class projection_digest_builder
		{
		  public:
			projection_digest_builder() = default;
			projection_digest_builder(const projection_digest_builder&) = delete;
			projection_digest_builder& operator=(const projection_digest_builder&) = delete;
			projection_digest_builder(projection_digest_builder&&) noexcept = default;
			projection_digest_builder& operator=(projection_digest_builder&&) noexcept = default;

			[[nodiscard]] static sdk::result<projection_digest_builder>
			begin(const std::string_view domain,
				  const std::string_view task_id,
				  const std::uint64_t expected_count,
				  const std::uint64_t expected_projection_bytes)
			{
				try
				{
					projection_digest_builder output;
					output.expected_count_ = expected_count;
					output.expected_projection_bytes_ = expected_projection_bytes;
					output.accumulator_ = make_materialization_sha256_accumulator();
					if (!output.accumulator_)
						return sdk::unexpected(stream_error("digest", "unavailable"));

					const auto encoded_text_size =
						[](const std::string_view value) -> sdk::result<std::uint64_t>
					{
						if (value.size() > std::numeric_limits<std::uint64_t>::max() - 9U)
							return sdk::unexpected(stream_error("digest", "length-overflow"));
						return materialization_claim_stream_framed_length(
							static_cast<std::uint64_t>(value.size()));
					};
					auto marker_size = encoded_text_size(semantic_marker);
					auto domain_size = encoded_text_size(domain);
					auto task_size = encoded_text_size(task_id);
					if (!marker_size || !domain_size || !task_size)
						return sdk::unexpected(stream_error("digest", "length-overflow"));
					std::uint64_t projection_item_bytes{};
					if (!checked_mul(expected_count, 9U, projection_item_bytes) ||
						!checked_add(projection_item_bytes,
									 expected_projection_bytes,
									 projection_item_bytes))
						return sdk::unexpected(stream_error("digest", "length-overflow"));
					std::uint64_t projection_tuple_size{};
					if (!checked_mul(expected_count, 8U, projection_tuple_size) ||
						!checked_add(9U, projection_tuple_size, projection_tuple_size) ||
						!checked_add(
							projection_tuple_size, projection_item_bytes, projection_tuple_size))
						return sdk::unexpected(stream_error("digest", "length-overflow"));
					std::uint64_t inner_items{};
					if (!checked_add(*task_size, 17U, inner_items) ||
						!checked_add(inner_items, projection_tuple_size, inner_items))
						return sdk::unexpected(stream_error("digest", "length-overflow"));
					std::uint64_t inner_size{};
					if (!checked_mul(3U, 8U, inner_size) ||
						!checked_add(9U, inner_size, inner_size) ||
						!checked_add(inner_size, inner_items, inner_size))
						return sdk::unexpected(stream_error("digest", "length-overflow"));
					std::uint64_t payload_size{};
					if (!checked_add(9U, inner_size, payload_size))
						return sdk::unexpected(stream_error("digest", "length-overflow"));

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
					valid = output.update_u64(payload_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x03U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(inner_size);
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
					valid = output.update_u64(projection_tuple_size);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_byte(0x05U);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					valid = output.update_u64(expected_count);
					if (!valid)
						return sdk::unexpected(std::move(valid.error()));
					return output;
				}
				catch (const std::bad_alloc&)
				{
					return sdk::unexpected(stream_error("allocation", "unavailable"));
				}
			}

			[[nodiscard]] sdk::result<void> append(const std::span<const std::byte> projection)
			{
				if (!accumulator_ || actual_count_ >= expected_count_ ||
					projection.size() > std::numeric_limits<std::uint64_t>::max() ||
					!checked_add(actual_projection_bytes_,
								 static_cast<std::uint64_t>(projection.size()),
								 actual_projection_bytes_))
					return sdk::unexpected(stream_error("digest", "projection-census"));
				auto framed_projection_size = materialization_claim_stream_framed_length(
					static_cast<std::uint64_t>(projection.size()));
				if (!framed_projection_size)
					return sdk::unexpected(std::move(framed_projection_size.error()));
				if (auto updated = update_u64(*framed_projection_size); !updated)
					return updated;
				if (auto updated = update_byte(0x03U); !updated)
					return updated;
				if (auto updated = update_u64(static_cast<std::uint64_t>(projection.size()));
					!updated)
					return updated;
				if (auto updated = update(projection); !updated)
					return updated;
				++actual_count_;
				return {};
			}

			[[nodiscard]] sdk::result<std::string> finish() &&
			{
				if (!accumulator_ || actual_count_ != expected_count_ ||
					actual_projection_bytes_ != expected_projection_bytes_)
					return sdk::unexpected(stream_error("digest", "projection-census"));
				auto digest = accumulator_->finish();
				if (!digest || !exact_sha256(*digest))
					return sdk::unexpected(stream_error("digest", "finalize"));
				return std::string{"semantic-v2:"} + *digest;
			}

		  private:
			[[nodiscard]] sdk::result<void> update(const std::span<const std::byte> bytes)
			{
				if (!accumulator_)
					return sdk::unexpected(stream_error("digest", "unavailable"));
				if (auto updated = accumulator_->update(bytes); !updated)
					return sdk::unexpected(stream_error("digest", "update"));
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

			std::unique_ptr<materialization_digest_accumulator> accumulator_;
			std::uint64_t expected_count_{};
			std::uint64_t expected_projection_bytes_{};
			std::uint64_t actual_count_{};
			std::uint64_t actual_projection_bytes_{};
		};

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

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value u64_bytes(const std::uint64_t value)
		{
			std::vector<std::byte> bytes(sizeof(value));
			for (std::size_t index{}; index < bytes.size(); ++index)
				bytes[index] = static_cast<std::byte>(
					(value >> (56U - static_cast<unsigned>(index * 8U))) & 0xffU);
			return sdk::canonical_value::from_bytes(std::move(bytes));
		}

		[[nodiscard]] sdk::result<stream_audit>
		inspect_stream(materialization_replayable_spool& spool,
					   const std::string_view request_id,
					   const std::string_view task_id)
		{
			try
			{
				auto receipt = validate_materialization_partition_event_stream(spool, request_id);
				if (!receipt)
					return sdk::unexpected(std::move(receipt.error()));
				stream_audit output;
				output.receipt = *receipt;
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
							return sdk::unexpected(stream_error("event.identity", "task-binding"));
						if (!partition_id)
						{
							if (kind != materialization_partition_event_kind::partition_begin)
								return sdk::unexpected(
									stream_error("event.identity", "missing-partition-begin"));
							partition_id = identity->second;
						}
						else if (*partition_id != identity->second)
							return sdk::unexpected(
								stream_error("event.identity", "interleaved-partition"));
						auto projection =
							materialization_incremental_full_event_projection(kind, key, payload);
						if (!projection)
							return sdk::unexpected(std::move(projection.error()));
						if (auto valid = add_measurement(output.event, projection->size()); !valid)
							return valid;
						if (const auto component = component_index(kind))
							if (auto valid = add_measurement(output.components[*component],
															 projection->size());
								!valid)
								return valid;
						return {};
					});
				if (!replay || !partition_id)
					return sdk::unexpected(replay ? stream_error("event.identity", "partition-id")
												  : std::move(replay.error()));
				output.partition_id = std::move(*partition_id);
				if (output.event.count != output.receipt.actual_frame_count)
					return sdk::unexpected(stream_error("event", "stream-census"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(stream_error("allocation", "unavailable"));
			}
		}

		[[nodiscard]] sdk::result<void>
		validate_journal(const validated_materialization_request& request,
						 const std::string_view request_id,
						 const materialization_incremental_execution_journal_receipt& journal,
						 std::span<const materialization_claim_stream_task> tasks)
		{
			if (tasks.size() != request.tasks.size() ||
				journal.materialization_request_id != request_id ||
				journal.exact_task_count != tasks.size() ||
				journal.canonical_task_ids.size() != tasks.size() ||
				journal.ordered_task_receipt_seal_digests.size() != tasks.size())
				return sdk::unexpected(stream_error("execution-journal", "task-census"));
			try
			{
				for (std::size_t index{}; index < tasks.size(); ++index)
				{
					if (auto valid = validate_materialization_incremental_task_receipt(
							request, index, tasks[index].receipt);
						!valid)
						return valid;
					const auto& expected_task_id = request.tasks[index].provider_task_id;
					if (journal.canonical_task_ids[index] != expected_task_id ||
						tasks[index].receipt.task_id != expected_task_id ||
						journal.ordered_task_receipt_seal_digests[index] !=
							tasks[index].receipt.pre_encoder_task_receipt_seal_digest)
						return sdk::unexpected(stream_error("execution-journal", "task-binding"));
				}
				if (auto valid = validate_materialization_incremental_execution_journal(journal);
					!valid)
					return sdk::unexpected(stream_error("execution-journal", "seal-mismatch"));
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(stream_error("allocation", "unavailable"));
			}
		}

		[[nodiscard]] sdk::result<void>
		validate_v2_journal(const std::string_view request_id,
							const std::uint64_t task_count,
							const materialization_incremental_execution_journal_receipt& journal,
							const std::span<const materialization_claim_stream_task> tasks)
		{
			if (!sdk::validate_strong_id(request_id) || task_count == 0U ||
				task_count > std::numeric_limits<std::size_t>::max() ||
				tasks.size() != static_cast<std::size_t>(task_count) ||
				journal.materialization_request_id != request_id ||
				journal.exact_task_count != task_count ||
				journal.canonical_task_ids.size() != tasks.size() ||
				journal.ordered_task_receipt_seal_digests.size() != tasks.size())
				return sdk::unexpected(stream_error("execution-journal", "task-census"));
			try
			{
				for (std::size_t index{}; index < tasks.size(); ++index)
				{
					const auto& receipt = tasks[index].receipt;
					if (receipt.materialization_request_id != request_id ||
						receipt.canonical_task_ordinal != index || !receipt.successful_seal ||
						!sdk::validate_strong_id(receipt.task_id) ||
						journal.canonical_task_ids[index] != receipt.task_id ||
						journal.ordered_task_receipt_seal_digests[index] !=
							receipt.pre_encoder_task_receipt_seal_digest)
						return sdk::unexpected(stream_error("execution-journal", "task-binding"));
					if (auto valid =
							validate_materialization_incremental_task_receipt_seal(receipt);
						!valid)
						return sdk::unexpected(stream_error("execution-journal", "seal-mismatch"));
				}
				if (auto valid = validate_materialization_incremental_execution_journal(journal);
					!valid)
					return sdk::unexpected(stream_error("execution-journal", "seal-mismatch"));
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return sdk::unexpected(stream_error("allocation", "unavailable"));
			}
		}
	} // namespace

	sdk::result<std::uint64_t>
	materialization_claim_stream_framed_length(const std::uint64_t payload_bytes)
	{
		std::uint64_t output{};
		if (!checked_add(9U, payload_bytes, output))
			return sdk::unexpected(stream_error("digest", "length-overflow"));
		return output;
	}

	sdk::result<void> materialization_claim_stream_source::validate_task_streams(
		const std::string_view request_id,
		const materialization_claim_stream_task& task,
		materialization_claim_stream_source::task_state& output)
	{
		if (task.partition_spools.empty())
			return sdk::unexpected(stream_error("partitions", "empty"));
		try
		{
			std::vector<stream_audit> audits;
			audits.reserve(task.partition_spools.size());
			for (const auto& spool : task.partition_spools)
			{
				if (!spool)
					return sdk::unexpected(stream_error("partitions", "null-spool"));
				auto audit = inspect_stream(*spool, request_id, task.receipt.task_id);
				if (!audit)
					return sdk::unexpected(std::move(audit.error()));
				audits.push_back(std::move(*audit));
			}
			for (std::size_t index{}; index < audits.size(); ++index)
			{
				if (!sdk::validate_strong_id(audits[index].partition_id) ||
					(index != 0U &&
					 (audits[index - 1U].partition_id >= audits[index].partition_id ||
					  audits[index - 1U].receipt.spool_index >= audits[index].receipt.spool_index)))
					return sdk::unexpected(stream_error("partitions", "identity-or-order"));
			}

			std::uint64_t total_event_count{};
			std::uint64_t total_event_bytes{};
			std::array<component_measurement, 4U> total_components{};
			for (const auto& audit : audits)
			{
				if (!checked_add(total_event_count, audit.event.count, total_event_count) ||
					!checked_add(
						total_event_bytes, audit.event.projection_bytes, total_event_bytes))
					return sdk::unexpected(stream_error("event", "census-overflow"));
				for (std::size_t component{}; component < total_components.size(); ++component)
					if (!checked_add(total_components[component].count,
									 audit.components[component].count,
									 total_components[component].count) ||
						!checked_add(total_components[component].projection_bytes,
									 audit.components[component].projection_bytes,
									 total_components[component].projection_bytes))
						return sdk::unexpected(stream_error("event", "census-overflow"));
			}
			if (task.receipt.partition.count != audits.size() ||
				task.receipt.event.count != total_event_count ||
				task.receipt.claim.count != total_components[0U].count ||
				task.receipt.row.count != total_components[1U].count ||
				task.receipt.coverage.count != total_components[2U].count ||
				task.receipt.unresolved.count != total_components[3U].count)
				return sdk::unexpected(stream_error("receipt", "count-mismatch"));

			auto event_builder = projection_digest_builder::begin(
				task_event_domain, task.receipt.task_id, total_event_count, total_event_bytes);
			if (!event_builder)
				return sdk::unexpected(std::move(event_builder.error()));
			std::array<projection_digest_builder, 4U> component_builders;
			for (std::size_t component{}; component < component_builders.size(); ++component)
			{
				auto builder =
					projection_digest_builder::begin(component_domains[component],
													 task.receipt.task_id,
													 total_components[component].count,
													 total_components[component].projection_bytes);
				if (!builder)
					return sdk::unexpected(std::move(builder.error()));
				component_builders[component] = std::move(*builder);
			}

			std::vector<std::pair<std::string, std::string>> partition_digests;
			partition_digests.reserve(audits.size());
			for (std::size_t index{}; index < audits.size(); ++index)
			{
				auto partition_builder = projection_digest_builder::begin(
					"cxxlens.df-0200.partition-event-full-projection.v1",
					audits[index].partition_id,
					audits[index].event.count,
					audits[index].event.projection_bytes);
				if (!partition_builder)
					return sdk::unexpected(std::move(partition_builder.error()));
				auto replay = replay_materialization_partition_event_stream(
					*task.partition_spools[index],
					request_id,
					[&](const std::uint64_t,
						const materialization_partition_event_kind kind,
						const std::span<const std::byte> key,
						const std::span<const std::byte> payload) -> sdk::result<void>
					{
						auto identity = event_identity(key);
						if (!identity || identity->first != task.receipt.task_id ||
							identity->second != audits[index].partition_id)
							return sdk::unexpected(stream_error("event.identity", "binding"));
						auto projection =
							materialization_incremental_full_event_projection(kind, key, payload);
						if (!projection)
							return sdk::unexpected(std::move(projection.error()));
						if (auto valid = event_builder->append(*projection); !valid)
							return valid;
						if (const auto component = component_index(kind))
							if (auto valid = component_builders[*component].append(*projection);
								!valid)
								return valid;
						return partition_builder->append(*projection);
					});
				if (!replay)
					return sdk::unexpected(std::move(replay.error()));
				auto partition_digest = std::move(*partition_builder).finish();
				if (!partition_digest)
					return sdk::unexpected(std::move(partition_digest.error()));
				partition_digests.emplace_back(audits[index].partition_id,
											   std::move(*partition_digest));
			}

			auto event_digest = std::move(*event_builder).finish();
			if (!event_digest || *event_digest != task.receipt.event.full_projection_digest)
				return sdk::unexpected(stream_error("receipt.event", "digest-mismatch"));
			const std::array<const std::string*, 4U> expected_digests{
				&task.receipt.claim.full_projection_digest,
				&task.receipt.row.full_projection_digest,
				&task.receipt.coverage.full_projection_digest,
				&task.receipt.unresolved.full_projection_digest,
			};
			for (std::size_t component{}; component < component_builders.size(); ++component)
			{
				auto digest = std::move(component_builders[component]).finish();
				if (!digest || *digest != *expected_digests[component])
					return sdk::unexpected(stream_error("receipt.component", "digest-mismatch"));
			}

			std::vector<sdk::canonical_value> partition_rows;
			partition_rows.reserve(partition_digests.size());
			for (const auto& [partition_id, digest] : partition_digests)
				partition_rows.push_back(
					sdk::canonical_value::from_tuple({text(partition_id), text(digest)}));
			auto partition_digest = semantic_value_digest(
				task_partition_domain,
				sdk::canonical_value::from_tuple({
					text(task.receipt.task_id),
					u64_bytes(static_cast<std::uint64_t>(partition_digests.size())),
					sdk::canonical_value::from_tuple(std::move(partition_rows)),
				}));
			if (!partition_digest ||
				*partition_digest != task.receipt.partition.full_projection_digest)
				return sdk::unexpected(stream_error("receipt.partition", "digest-mismatch"));

			output.receipt = task.receipt;
			output.partition_ids.reserve(audits.size());
			for (const auto& audit : audits)
				output.partition_ids.push_back(audit.partition_id);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<std::vector<materialization_claim_stream_source::task_state>>
	materialization_claim_stream_source::build_states(
		const std::string_view request_id, std::vector<materialization_claim_stream_task>& tasks)
	{
		try
		{
			std::vector<materialization_claim_stream_source::task_state> states;
			states.reserve(tasks.size());
			for (std::size_t index{}; index < tasks.size(); ++index)
			{
				materialization_claim_stream_source::task_state state;
				auto valid = validate_task_streams(request_id, tasks[index], state);
				if (!valid)
					return sdk::unexpected(std::move(valid.error()));
				state.partition_spools = std::move(tasks[index].partition_spools);
				states.push_back(std::move(state));
			}
			return states;
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<void> materialization_claim_stream_source::validate_external_task_receipts(
		const validated_materialization_request& request,
		const materialization_incremental_execution_journal_receipt& journal,
		const std::span<materialization_claim_stream_task> tasks)
	{
		try
		{
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			if (auto valid = validate_journal(request, *request_id, journal, tasks); !valid)
				return valid;
			for (std::size_t index{}; index < tasks.size(); ++index)
			{
				materialization_claim_stream_source::task_state ignored;
				if (auto valid = validate_task_streams(*request_id, tasks[index], ignored); !valid)
					return valid;
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<void> materialization_claim_stream_source::validate_external_task_receipts(
		const std::string_view materialization_request_id,
		const std::uint64_t task_count,
		const materialization_incremental_execution_journal_receipt& journal,
		const std::span<materialization_claim_stream_task> tasks)
	{
		try
		{
			auto valid =
				validate_v2_journal(materialization_request_id,
									task_count,
									journal,
									std::span<const materialization_claim_stream_task>{tasks});
			if (!valid)
				return valid;
			for (const auto& task : tasks)
			{
				materialization_claim_stream_source::task_state ignored;
				if (auto stream_valid =
						validate_task_streams(materialization_request_id, task, ignored);
					!stream_valid)
					return stream_valid;
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_claim_stream_source> materialization_claim_stream_source::begin(
		const validated_materialization_request& request,
		const materialization_incremental_execution_journal_receipt& journal,
		std::vector<materialization_claim_stream_task> tasks)
	{
		try
		{
			auto request_id = materialization_incremental_request_id(request);
			if (!request_id)
				return sdk::unexpected(std::move(request_id.error()));
			if (auto valid =
					validate_journal(request,
									 *request_id,
									 journal,
									 std::span<const materialization_claim_stream_task>{tasks});
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			auto states = build_states(*request_id, tasks);
			if (!states)
				return sdk::unexpected(std::move(states.error()));
			return materialization_claim_stream_source{std::move(*request_id), std::move(*states)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<materialization_claim_stream_source> materialization_claim_stream_source::begin(
		std::string materialization_request_id,
		const std::uint64_t task_count,
		const materialization_incremental_execution_journal_receipt& journal,
		std::vector<materialization_claim_stream_task> tasks)
	{
		try
		{
			if (auto valid =
					validate_v2_journal(materialization_request_id,
										task_count,
										journal,
										std::span<const materialization_claim_stream_task>{tasks});
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			auto states = build_states(materialization_request_id, tasks);
			if (!states)
				return sdk::unexpected(std::move(states.error()));
			return materialization_claim_stream_source{std::move(materialization_request_id),
													   std::move(*states)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	sdk::result<void> materialization_claim_stream_source::replay(
		const materialization_claim_stream_consumer& consumer)
	{
		if (!consumer)
			return sdk::unexpected(stream_error("replay", "consumer"));
		try
		{
			for (auto& task : tasks_)
			{
				for (std::size_t index{}; index < task.partition_spools.size(); ++index)
				{
					if (!task.partition_spools[index])
						return sdk::unexpected(stream_error("replay", "null-spool"));
					auto replay = replay_materialization_partition_event_stream(
						*task.partition_spools[index],
						materialization_request_id_,
						[&](const std::uint64_t ordinal,
							const materialization_partition_event_kind kind,
							const std::span<const std::byte> key,
							const std::span<const std::byte> payload) -> sdk::result<void>
						{
							auto identity = event_identity(key);
							if (!identity || identity->first != task.receipt.task_id ||
								identity->second != task.partition_ids[index])
								return sdk::unexpected(stream_error("replay.identity", "binding"));
							return consumer(materialization_claim_stream_event{
								ordinal,
								task.receipt.task_id,
								task.partition_ids[index],
								kind,
								key,
								payload,
							});
						});
					if (!replay)
						return sdk::unexpected(std::move(replay.error()));
				}
			}
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(stream_error("allocation", "unavailable"));
		}
	}

	std::size_t materialization_claim_stream_source::partition_count() const noexcept
	{
		std::size_t output{};
		for (const auto& task : tasks_)
			if (task.partition_ids.size() <= std::numeric_limits<std::size_t>::max() - output)
				output += task.partition_ids.size();
			else
				return 0U;
		return output;
	}

	std::span<const std::string>
	materialization_claim_stream_source::partition_ids(const std::size_t task_index) const noexcept
	{
		if (task_index >= tasks_.size())
			return {};
		return tasks_[task_index].partition_ids;
	}

	const materialization_incremental_task_receipt*
	materialization_claim_stream_source::task_receipt(const std::size_t task_index) const noexcept
	{
		if (task_index >= tasks_.size())
			return nullptr;
		return &tasks_[task_index].receipt;
	}
} // namespace cxxlens::detail::clang22::materialization
