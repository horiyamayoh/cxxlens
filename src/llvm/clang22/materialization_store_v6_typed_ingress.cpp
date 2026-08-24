#include "materialization_store_v6_typed_ingress.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include "materialization_io.hpp"
#include "sdk/claim_internal.hpp"
#include "sdk/store_claim_codec_internal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		using record_kind = sdk::detail::bounded_store_v6_record_kind;
		using semantic_record = sdk::detail::bounded_store_v6_semantic_record;
		using typed_event_consumer =
			std::function<sdk::result<void>(const materialization_store_v6_typed_event&)>;

		constexpr std::uint64_t record_length_prefix_bytes = 8U;
		[[nodiscard]] sdk::result<const std::vector<sdk::canonical_value>*>
		require_tuple(const sdk::canonical_value&, std::size_t, std::string_view);
		[[nodiscard]] sdk::result<std::string>
		require_text(const sdk::canonical_value&, std::string_view, bool allow_empty = false);
		[[nodiscard]] sdk::result<std::uint64_t> require_count(const sdk::canonical_value&,
															   std::string_view);
		[[nodiscard]] sdk::result<sdk::snapshot_partition_binding>
		decode_binding(const sdk::canonical_value&);
		[[nodiscard]] sdk::result<sdk::partition_manifest>
		decode_manifest(const sdk::canonical_value&);
		[[nodiscard]] sdk::result<sdk::detached_row> decode_row(const sdk::canonical_value&,
																const sdk::relation_engine&);
		[[nodiscard]] sdk::result<materialization_origin_association>
		decode_association(const sdk::canonical_value&);
		[[nodiscard]] sdk::result<sdk::snapshot_coverage_unit>
		decode_coverage(const sdk::canonical_value&);
		[[nodiscard]] sdk::result<sdk::unresolved_reference>
		decode_unresolved(const sdk::canonical_value&);
		[[nodiscard]] sdk::result<semantic_record>
		semantic_projection(const materialization_store_v6_typed_event&);
		[[nodiscard]] sdk::result<std::vector<std::byte>>
		encode_expected_record(const semantic_record&);
		[[nodiscard]] sdk::result<std::vector<std::byte>>
		semantic_order_key(const semantic_record&);
		[[nodiscard]] sdk::error cancelled();

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.store-v6-typed-ingress-invalid",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.store-v6-typed-ingress-mismatch",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error resource(std::string field, std::string detail = {})
		{
			return {"materialization.store-v6-typed-ingress-resource-exhausted",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error spool_failure(std::string field, std::string detail = {})
		{
			return {"materialization.store-v6-typed-ingress-spool-failure",
					std::move(field),
					std::move(detail)};
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

		[[nodiscard]] bool checked_multiply(const std::uint64_t left,
											const std::uint64_t right,
											std::uint64_t& output) noexcept
		{
			if (left != 0U && right > std::numeric_limits<std::uint64_t>::max() / left)
				return false;
			output = left * right;
			return true;
		}

		// Allocation-free upper bound for the canonical shape of one source-owned value.  The
		// per-node allowance covers the canonical tag/length representation as well as bounded
		// container bookkeeping; source strings and byte strings are counted exactly.
		class source_byte_meter final
		{
		  public:
			void node(const std::uint64_t count = 1U) noexcept
			{
				std::uint64_t charge{};
				if (!checked_multiply(count, 32U, charge))
				{
					overflow_ = true;
					return;
				}
				add(charge);
			}

			void text_value(const std::string_view value) noexcept
			{
				node();
				add(static_cast<std::uint64_t>(value.size()));
			}

			void byte_value(const std::span<const std::byte> value) noexcept
			{
				node();
				add(static_cast<std::uint64_t>(value.size()));
			}

			void add(const std::uint64_t value) noexcept
			{
				std::uint64_t next{};
				if (overflow_ || !checked_add(bytes_, value, next))
				{
					overflow_ = true;
					return;
				}
				bytes_ = next;
			}

			[[nodiscard]] sdk::result<std::uint64_t> finish() const
			{
				if (overflow_)
					return sdk::unexpected(resource("record-source", "checked-overflow"));
				if (auto valid = validate_materialization_store_v6_record_source_bytes(bytes_);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				return bytes_;
			}

		  private:
			std::uint64_t bytes_{};
			bool overflow_{};
		};

		void measure_condition(source_byte_meter& meter, const sdk::claim_condition& value) noexcept
		{
			meter.node(2U);
			meter.text_value(value.universe);
			for (const auto& fragment : value.fragments)
				meter.text_value(fragment);
		}

		void measure_cell(source_byte_meter& meter, const sdk::detached_cell& value) noexcept
		{
			meter.node(4U);
			meter.text_value(value.type.parameter);
			if (value.value)
				std::visit(
					[&](const auto& item)
					{
						using item_type = std::decay_t<decltype(item)>;
						meter.node(2U);
						if constexpr (std::is_same_v<item_type, std::string>)
							meter.text_value(item);
						else if constexpr (std::is_same_v<item_type, std::vector<std::byte>>)
							meter.byte_value(item);
						else
							meter.node();
					},
					*value.value);
			if (value.unknown_reason)
				meter.text_value(*value.unknown_reason);
		}

		void measure_row(source_byte_meter& meter, const sdk::detached_row& value) noexcept
		{
			meter.node(2U);
			meter.text_value(value.descriptor_id);
			for (const auto& [column, cell] : value.cells)
			{
				meter.node();
				meter.text_value(column);
				measure_cell(meter, cell);
			}
		}

		void measure_strings(source_byte_meter& meter,
							 const std::span<const std::string> values) noexcept
		{
			meter.node();
			for (const auto& value : values)
				meter.text_value(value);
		}

		void measure_claim(source_byte_meter& meter, const sdk::claim& value) noexcept
		{
			meter.node(5U);
			measure_row(meter, value.row);
			meter.text_value(value.descriptor);
			meter.text_value(value.semantic_key);
			meter.text_value(value.assertion);
			meter.text_value(value.content);
			measure_condition(meter, value.presence);
			meter.text_value(value.interpretation);
			meter.text_value(value.producer.id);
			meter.text_value(value.producer.semantic_contract);
			std::visit(
				[&](const auto& basis)
				{
					using basis_type = std::decay_t<decltype(basis)>;
					meter.node();
					if constexpr (std::is_same_v<basis_type, sdk::direct_claim_basis>)
						meter.text_value(basis.basis_digest);
					else
					{
						meter.text_value(basis.input_snapshot);
						measure_strings(meter, basis.consumed_partition_content_digests);
						meter.text_value(basis.transform_semantics);
					}
				},
				value.input_basis);
			meter.text_value(value.provenance_root);
			meter.text_value(value.guarantee.approximation);
			meter.text_value(value.guarantee.scope);
			meter.text_value(value.guarantee.assumptions);
			measure_strings(meter, value.guarantee.verification_modalities);
		}

		void measure_context(source_byte_meter& meter,
							 const materialization_semantic_task_context& value) noexcept
		{
			meter.node();
			meter.text_value(value.provider_task_id);
			meter.text_value(value.task_input_digest);
			meter.text_value(value.selected_catalog_compile_unit_id);
			meter.text_value(value.compile_unit_id);
			meter.text_value(value.condition_universe_id);
			meter.text_value(value.condition_id);
			meter.text_value(value.interpretation_domain);
		}

		void measure_association(source_byte_meter& meter,
								 const materialization_origin_association& value) noexcept
		{
			meter.node();
			meter.text_value(value.association_id);
			meter.text_value(value.stored_claim_ref);
			measure_context(meter, value.originating_task);
			meter.text_value(value.sealed_row_digest);
			if (value.source_evidence_digest)
				meter.text_value(*value.source_evidence_digest);
			else
				meter.node();
		}

		void measure_coverage(source_byte_meter& meter,
							  const sdk::snapshot_coverage_unit& value) noexcept
		{
			meter.node();
			meter.text_value(value.domain);
			meter.text_value(value.key);
			meter.text_value(value.state);
			meter.text_value(value.reason);
		}

		void measure_unresolved(source_byte_meter& meter,
								const sdk::unresolved_reference& value) noexcept
		{
			meter.node();
			meter.text_value(value.source_assertion);
			meter.text_value(value.source_relation);
			meter.text_value(value.target_relation);
			measure_strings(meter, value.source_columns);
			meter.text_value(value.reason);
		}

		void measure_closure(source_byte_meter& meter, const sdk::closure_candidate& value) noexcept
		{
			meter.node();
			meter.text_value(value.relation_descriptor_id);
			meter.text_value(value.subject_partition_id);
			meter.text_value(value.partition_content_digest);
			meter.text_value(value.coverage_digest);
			meter.text_value(value.key_domain_digest);
			measure_condition(meter, value.condition);
			meter.text_value(value.interpretation);
			meter.text_value(value.assumption_set_id);
			meter.text_value(value.closure_kind);
			meter.text_value(value.producer_semantics);
			meter.text_value(value.evidence_digest);
		}

		void measure_binding(source_byte_meter& meter,
							 const sdk::snapshot_partition_binding& value) noexcept
		{
			meter.node();
			meter.text_value(value.partition_id);
			meter.text_value(value.relation_descriptor_id);
			meter.text_value(value.scope);
			measure_condition(meter, value.condition);
			meter.text_value(value.interpretation);
			meter.text_value(value.producer_semantics);
			meter.text_value(value.producer_input_basis_digest);
			meter.text_value(value.precision_profile);
			meter.text_value(value.assumption_set_id);
		}

		void measure_manifest(source_byte_meter& meter,
							  const sdk::partition_manifest& value) noexcept
		{
			meter.node(3U);
			meter.text_value(value.partition_id);
			meter.text_value(value.relation_descriptor_id);
			meter.text_value(value.input_basis_digest);
			meter.text_value(value.claim_set_digest);
			meter.text_value(value.coverage_digest);
			meter.text_value(value.content_digest);
		}

		void measure_canonical_value(source_byte_meter& meter,
									 const sdk::canonical_value& value) noexcept
		{
			meter.node();
			switch (value.type)
			{
				case sdk::canonical_value::kind::utf8_string:
					meter.text_value(value.text);
					break;
				case sdk::canonical_value::kind::bytes:
					meter.byte_value(value.byte_string);
					break;
				case sdk::canonical_value::kind::ordered_tuple:
					meter.node();
					for (const auto& child : value.tuple)
						measure_canonical_value(meter, child);
					break;
				case sdk::canonical_value::kind::null_value:
				case sdk::canonical_value::kind::boolean:
				case sdk::canonical_value::kind::signed_integer:
					meter.node();
					break;
			}
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		measure_semantic_record_source(const semantic_record& record)
		{
			source_byte_meter meter;
			meter.node(3U);
			measure_canonical_value(meter, record.key);
			measure_canonical_value(meter, record.payload);
			return meter.finish();
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		measure_typed_event_source(const materialization_store_v6_typed_event& event)
		{
			source_byte_meter meter;
			meter.node(3U);
			std::visit(
				[&](const auto& value)
				{
					using value_type = std::decay_t<decltype(value)>;
					meter.text_value(value.task_id);
					if constexpr (std::is_same_v<value_type,
												 materialization_store_v6_partition_begin>)
					{
						meter.text_value(value.task_authority_digest);
						measure_binding(meter, value.binding);
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_occurrence>)
					{
						meter.text_value(value.partition_id);
						meter.text_value(value.claim_ref);
						measure_claim(meter, value.value);
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_detached_row>)
					{
						meter.text_value(value.partition_id);
						meter.text_value(value.row_digest);
						measure_row(meter, value.value);
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_annotation>)
					{
						meter.text_value(value.partition_id);
						measure_association(meter, value.association);
						measure_claim(meter, value.occurrence);
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_coverage>)
					{
						meter.text_value(value.partition_id);
						measure_coverage(meter, value.value);
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_unresolved>)
					{
						meter.text_value(value.partition_id);
						measure_unresolved(meter, value.value);
					}
					else
					{
						measure_manifest(meter, value.manifest);
						meter.text_value(value.semantic_partition_digest);
						meter.node(6U);
					}
				},
				event);
			return meter.finish();
		}

		[[nodiscard]] sdk::result<std::uint64_t> measure_canonicalization_event_source(
			const materialization_store_v6_canonicalization_edge_event& event)
		{
			source_byte_meter meter;
			meter.node(5U);
			meter.text_value(event.task_id);
			meter.text_value(event.edge.precursor_claim_ref);
			meter.text_value(event.edge.final_claim_ref);
			meter.text_value(event.edge.transform_semantics);
			measure_claim(meter, event.hidden_precursor);
			return meter.finish();
		}

		template <class Range, class Compare>
		[[nodiscard]] sdk::result<void>
		cancellable_sort(Range& range, Compare compare, const std::stop_token cancellation)
		{
			struct cancellation_signal final
			{
			};
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			std::uint64_t comparisons{};
			try
			{
				std::ranges::sort(range,
								  [&](const auto& left, const auto& right)
								  {
									  ++comparisons;
									  if ((comparisons & 0xffU) == 0U &&
										  cancellation.stop_requested())
										  throw cancellation_signal{};
									  return compare(left, right);
								  });
			}
			catch (const cancellation_signal&)
			{
				return sdk::unexpected(cancelled());
			}
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return {};
		}

		void append_u64(std::span<std::byte, 8U> output, const std::uint64_t value) noexcept
		{
			for (std::size_t index{}; index < output.size(); ++index)
				output[index] = static_cast<std::byte>(value >> (56U - index * 8U));
		}

		[[nodiscard]] std::uint64_t read_u64(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t output{};
			for (const auto value : input.first<8U>())
				output = (output << 8U) | std::to_integer<std::uint64_t>(value);
			return output;
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value bytes(std::vector<std::byte> value)
		{
			return sdk::canonical_value::from_bytes(std::move(value));
		}

		[[nodiscard]] sdk::canonical_value tuple(std::vector<sdk::canonical_value> value)
		{
			return sdk::canonical_value::from_tuple(std::move(value));
		}

		[[nodiscard]] sdk::canonical_value count_value(const std::uint64_t value)
		{
			return sdk::canonical_value::from_integer(static_cast<std::int64_t>(value));
		}

		[[nodiscard]] sdk::canonical_value strings(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(text(value));
			return tuple(std::move(output));
		}

		[[nodiscard]] sdk::canonical_value condition_value(const sdk::claim_condition& value)
		{
			return tuple({text(value.universe), strings(value.fragments)});
		}

		[[nodiscard]] sdk::canonical_value
		binding_value(const sdk::snapshot_partition_binding& value)
		{
			return tuple({text(value.partition_id),
						  text(value.relation_descriptor_id),
						  text(value.scope),
						  condition_value(value.condition),
						  text(value.interpretation),
						  text(value.producer_semantics),
						  text(value.producer_input_basis_digest),
						  text(value.precision_profile),
						  text(value.assumption_set_id)});
		}

		[[nodiscard]] sdk::canonical_value manifest_value(const sdk::partition_manifest& value)
		{
			return tuple({text(value.partition_id),
						  text(value.relation_descriptor_id),
						  text(value.input_basis_digest),
						  text(value.claim_set_digest),
						  text(value.coverage_digest),
						  text(value.content_digest),
						  count_value(value.claim_count),
						  sdk::canonical_value::from_boolean(value.complete)});
		}

		[[nodiscard]] sdk::canonical_value
		task_context_value(const materialization_semantic_task_context& value)
		{
			return tuple({text(value.provider_task_id),
						  text(value.task_input_digest),
						  text(value.selected_catalog_compile_unit_id),
						  text(value.compile_unit_id),
						  text(value.condition_universe_id),
						  text(value.condition_id),
						  text(value.interpretation_domain)});
		}

		[[nodiscard]] sdk::canonical_value
		association_value(const materialization_origin_association& value)
		{
			return tuple({text(value.association_id),
						  text(value.stored_claim_ref),
						  task_context_value(value.originating_task),
						  text(value.sealed_row_digest),
						  value.source_evidence_digest ? text(*value.source_evidence_digest)
													   : sdk::canonical_value::null()});
		}

		[[nodiscard]] sdk::canonical_value
		canonicalization_edge_value(const materialization_canonicalization_edge& value)
		{
			return tuple({text(value.precursor_claim_ref),
						  text(value.final_claim_ref),
						  text(value.transform_semantics)});
		}

		[[nodiscard]] sdk::canonical_value coverage_value(const sdk::snapshot_coverage_unit& value)
		{
			return tuple(
				{text(value.domain), text(value.key), text(value.state), text(value.reason)});
		}

		[[nodiscard]] sdk::canonical_value unresolved_value(const sdk::unresolved_reference& value)
		{
			return tuple({text(value.source_assertion),
						  text(value.source_relation),
						  text(value.target_relation),
						  strings(value.source_columns),
						  text(value.reason)});
		}

		[[nodiscard]] sdk::canonical_value cell_value(const sdk::detached_cell& value)
		{
			sdk::canonical_value payload = sdk::canonical_value::null();
			if (value.value)
				payload = std::visit(
					[](const auto& item) -> sdk::canonical_value
					{
						using item_type = std::decay_t<decltype(item)>;
						if constexpr (std::is_same_v<item_type, bool>)
							return tuple({text("bool"), sdk::canonical_value::from_boolean(item)});
						else if constexpr (std::is_same_v<item_type, std::int64_t>)
							return tuple(
								{text("signed"), sdk::canonical_value::from_integer(item)});
						else if constexpr (std::is_same_v<item_type, std::uint64_t>)
							return tuple({text("unsigned"), text(std::to_string(item))});
						else if constexpr (std::is_same_v<item_type, std::string>)
							return tuple({text("string"), text(item)});
						else
							return tuple({text("bytes"), bytes(item)});
					},
					*value.value);
			return tuple({count_value(static_cast<std::uint64_t>(value.type.scalar)),
						  text(value.type.parameter),
						  sdk::canonical_value::from_boolean(value.type.optional),
						  count_value(static_cast<std::uint64_t>(value.state)),
						  std::move(payload),
						  value.unknown_reason ? text(*value.unknown_reason)
											   : sdk::canonical_value::null()});
		}

		[[nodiscard]] sdk::canonical_value row_value(const sdk::detached_row& value)
		{
			std::vector<sdk::canonical_value> cells;
			cells.reserve(value.cells.size());
			for (const auto& [column, cell] : value.cells)
				cells.push_back(tuple({text(column), cell_value(cell)}));
			return tuple({text(value.descriptor_id), tuple(std::move(cells))});
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>> claim_bytes(const sdk::claim& value)
		{
			return sdk::detail::encode_store_claim(value);
		}

		[[nodiscard]] sdk::result<std::string>
		identity_digest(std::string_view domain, std::vector<sdk::canonical_value> fields);

		[[nodiscard]] sdk::result<std::string> claim_envelope_ref(const std::string_view role,
																  const sdk::claim& value)
		{
			auto singleton = sdk::claim_batch_content_digest(
				std::span<const sdk::claim>{&value, 1U}, {}, {}, {});
			if (!singleton)
				return sdk::unexpected(std::move(singleton.error()));
			return identity_digest("materialization-claim-envelope",
								   {text(role), text(*singleton)});
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		canonical(const sdk::canonical_value& value)
		{
			auto output = sdk::canonical_binary(value);
			if (!output)
				return sdk::unexpected(invalid("canonical-value", output.error().code));
			return output;
		}

		[[nodiscard]] sdk::result<std::string>
		identity_digest(const std::string_view domain, std::vector<sdk::canonical_value> fields)
		{
			auto output = sdk::canonical_identity_digest(domain, fields);
			if (!output)
				return sdk::unexpected(invalid("identity", output.error().code));
			return output;
		}

		[[nodiscard]] sdk::result<std::string> row_digest(const sdk::detached_row& value)
		{
			auto encoded = canonical(row_value(value));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				"cxxlens.clang22.materialization-store-v6-row.v1",
				std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] int hexadecimal_nibble(const char value) noexcept
		{
			if (value >= '0' && value <= '9')
				return value - '0';
			if (value >= 'a' && value <= 'f')
				return value - 'a' + 10;
			return -1;
		}

		[[nodiscard]] sdk::result<std::array<std::byte, 32U>>
		binary_sha256(const std::string_view value)
		{
			if (value.size() != 71U || !value.starts_with("sha256:"))
				return sdk::unexpected(invalid("sha256", "shape"));
			std::array<std::byte, 32U> output{};
			for (std::size_t index{}; index < output.size(); ++index)
			{
				const auto high = hexadecimal_nibble(value[7U + index * 2U]);
				const auto low = hexadecimal_nibble(value[8U + index * 2U]);
				if (high < 0 || low < 0)
					return sdk::unexpected(invalid("sha256", "hex"));
				output[index] = static_cast<std::byte>((high << 4) | low);
			}
			return output;
		}

		class stream_digest final
		{
		  public:
			stream_digest() : digest_{make_materialization_sha256_accumulator()} {}

			[[nodiscard]] sdk::result<void> update(const std::span<const std::byte> value)
			{
				if (!digest_)
					return sdk::unexpected(spool_failure("digest", "unavailable"));
				std::array<std::byte, 8U> length{};
				append_u64(length, static_cast<std::uint64_t>(value.size()));
				if (auto updated = digest_->update(length); !updated)
					return sdk::unexpected(spool_failure("digest", "length"));
				if (auto updated = digest_->update(value); !updated)
					return sdk::unexpected(spool_failure("digest", "value"));
				return {};
			}

			[[nodiscard]] sdk::result<std::string> finish()
			{
				if (!digest_)
					return sdk::unexpected(spool_failure("digest", "consumed"));
				auto output = digest_->finish();
				digest_.reset();
				if (!output)
					return sdk::unexpected(spool_failure("digest", "finish"));
				return std::move(*output);
			}

		  private:
			std::unique_ptr<materialization_digest_accumulator> digest_;
		};

		class raw_stream_digest final
		{
		  public:
			raw_stream_digest() : digest_{make_materialization_sha256_accumulator()} {}

			[[nodiscard]] sdk::result<void> update(const std::span<const std::byte> value)
			{
				if (!digest_)
					return sdk::unexpected(spool_failure("raw-digest", "unavailable"));
				if (auto updated = digest_->update(value); !updated)
					return sdk::unexpected(spool_failure("raw-digest", "update"));
				return {};
			}

			[[nodiscard]] sdk::result<std::string> finish()
			{
				if (!digest_)
					return sdk::unexpected(spool_failure("raw-digest", "consumed"));
				auto output = digest_->finish();
				digest_.reset();
				if (!output)
					return sdk::unexpected(spool_failure("raw-digest", "finish"));
				return std::move(*output);
			}

		  private:
			std::unique_ptr<materialization_digest_accumulator> digest_;
		};

		[[nodiscard]] sdk::result<void> append_spool(materialization_replayable_spool& spool,
													 const std::span<const std::byte> bytes_value)
		{
			if (auto written = spool.append(bytes_value); !written)
				return sdk::unexpected(spool_failure("append", "io"));
			return {};
		}

		[[nodiscard]] sdk::result<void> read_exact(materialization_replayable_spool& spool,
												   std::uint64_t offset,
												   const std::span<std::byte> destination)
		{
			std::size_t consumed{};
			while (consumed < destination.size())
			{
				auto read = spool.read_at(offset + consumed, destination.subspan(consumed));
				if (!read || *read == 0U || *read > destination.size() - consumed)
					return sdk::unexpected(spool_failure("read", "short-or-failed"));
				consumed += *read;
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::shared_ptr<materialization_replayable_spool>> make_spool()
		{
			auto output = make_materialization_private_spool();
			if (!output)
				return sdk::unexpected(spool_failure("create", "unavailable"));
			return std::shared_ptr<materialization_replayable_spool>{std::move(*output)};
		}

		[[nodiscard]] sdk::result<void> seal_spool(materialization_replayable_spool& spool)
		{
			if (auto sealed = spool.seal(); !sealed || !spool.sealed())
				return sdk::unexpected(spool_failure("seal", "failed"));
			return {};
		}

		[[nodiscard]] sdk::result<std::string>
		digest_spool_with_cancellation(materialization_replayable_spool& spool,
									   const std::stop_token cancellation)
		{
			if (!spool.sealed())
				return sdk::unexpected(spool_failure("digest", "unsealed"));
			std::array<std::byte, default_stream_chunk_bytes> buffer{};
			raw_stream_digest digest;
			std::uint64_t offset{};
			while (offset < spool.size_bytes())
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto remaining = spool.size_bytes() - offset;
				auto destination = std::span{buffer}.first(
					static_cast<std::size_t>(std::min<std::uint64_t>(remaining, buffer.size())));
				auto received = spool.read_at(offset, destination);
				if (!received || *received == 0U || *received > destination.size())
					return sdk::unexpected(spool_failure("digest", "read"));
				if (auto updated = digest.update(std::span{buffer}.first(*received)); !updated)
					return sdk::unexpected(std::move(updated.error()));
				offset += static_cast<std::uint64_t>(*received);
			}
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return digest.finish();
		}

		[[nodiscard]] sdk::result<void>
		preflight_task_input_bounds(const materialization_store_v6_task_input& task,
									const std::stop_token cancellation)
		{
			std::uint64_t arena_bytes{};
			const auto charge = [&](const std::uint64_t value) -> sdk::result<void>
			{
				auto next = checked_materialization_store_v6_sort_arena_charge(arena_bytes, value);
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				arena_bytes = *next;
				return {};
			};
			const auto measured_charge = [&](source_byte_meter meter,
											 const std::uint64_t multiplier,
											 const std::uint64_t fixed) -> sdk::result<void>
			{
				auto measured = meter.finish();
				std::uint64_t scaled{};
				std::uint64_t total{};
				if (!measured || !checked_multiply(*measured, multiplier, scaled) ||
					!checked_add(scaled, fixed, total))
					return sdk::unexpected(!measured
											   ? std::move(measured.error())
											   : resource("task-sort-arena", "checked-overflow"));
				return charge(total);
			};
			const auto scan_claims =
				[&](const std::span<const sdk::claim> values) -> sdk::result<void>
			{
				for (const auto& value : values)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					source_byte_meter meter;
					measure_claim(meter, value);
					// The validator and the two independent order traversals retain separate keys.
					if (auto charged = measured_charge(std::move(meter), 3U, 512U); !charged)
						return charged;
				}
				return {};
			};
			if (auto valid = scan_claims(task.claims.translation.partition.claims); !valid)
				return valid;
			if (auto valid = scan_claims(task.claims.translation.batch.claims); !valid)
				return valid;
			for (const auto& edge : task.canonicalization_edges)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				source_byte_meter meter;
				meter.node(4U);
				meter.text_value(edge.edge.precursor_claim_ref);
				meter.text_value(edge.edge.final_claim_ref);
				meter.text_value(edge.edge.transform_semantics);
				measure_claim(meter, edge.hidden_precursor);
				if (auto charged = measured_charge(std::move(meter), 2U, 384U); !charged)
					return charged;
			}
			for (const auto& origin : task.origins)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				source_byte_meter meter;
				measure_association(meter, origin.association);
				if (auto charged = measured_charge(std::move(meter), 3U, 384U); !charged)
					return charged;
			}
			for (const auto& batch : task.normalized.batches)
				for (const auto& row : batch.rows)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					source_byte_meter meter;
					measure_row(meter, row);
					if (auto charged = measured_charge(std::move(meter), 2U, 256U); !charged)
						return charged;
				}
			for (const auto& coverage : task.claims.translation.partition.coverage)
			{
				source_byte_meter meter;
				measure_coverage(meter, coverage);
				if (auto charged = measured_charge(std::move(meter), 2U, 128U); !charged)
					return charged;
			}
			for (const auto& unresolved : task.claims.translation.partition.unresolved)
			{
				source_byte_meter meter;
				measure_unresolved(meter, unresolved);
				if (auto charged = measured_charge(std::move(meter), 2U, 128U); !charged)
					return charged;
			}
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return {};
		}
	} // namespace

	sdk::result<void> validate_materialization_store_v6_task_count(const std::uint64_t task_count)
	{
		if (task_count == 0U || task_count > materialization_store_v6_max_tasks)
			return sdk::unexpected(resource("task-count", "hard-bound"));
		return {};
	}

	sdk::result<std::uint64_t>
	checked_materialization_store_v6_spool_charge(const std::uint64_t aggregate_bytes,
												  const std::uint64_t task_aggregate_begin,
												  const std::uint64_t next_bytes)
	{
		std::uint64_t output{};
		if (task_aggregate_begin > aggregate_bytes ||
			!checked_add(aggregate_bytes, next_bytes, output) ||
			output > sdk::detail::bounded_store_v6_max_aggregate_bytes ||
			output - task_aggregate_begin > sdk::detail::bounded_store_v6_source_window_bytes)
			return sdk::unexpected(resource("spool-charge", "task-or-aggregate-bound"));
		return output;
	}

	sdk::result<void>
	validate_materialization_store_v6_record_source_bytes(const std::uint64_t source_bytes)
	{
		if (source_bytes > sdk::detail::bounded_store_v6_record_buffer_bytes)
			return sdk::unexpected(resource("record-source", "hard-bound"));
		return {};
	}

	sdk::result<std::uint64_t>
	checked_materialization_store_v6_sort_arena_charge(const std::uint64_t used_bytes,
													   const std::uint64_t next_bytes)
	{
		std::uint64_t output{};
		if (!checked_add(used_bytes, next_bytes, output) ||
			output > sdk::detail::bounded_store_v6_sort_arena_bytes)
			return sdk::unexpected(resource("task-sort-arena", "hard-bound"));
		return output;
	}

	sdk::result<std::string> derive_materialization_store_v6_claim_ref(const sdk::claim& value)
	{
		return claim_envelope_ref("stored_final", value);
	}
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		struct task_range
		{
			materialization_store_v6_typed_task_receipt receipt;
			std::uint64_t begin{};
			std::uint64_t end{};
			std::uint64_t edge_begin{};
			std::uint64_t edge_end{};
		};

		struct task_completion_ledger
		{
			std::vector<std::uint8_t> complete;
		};

		[[nodiscard]] sdk::error cancelled();
		[[nodiscard]] sdk::error allocation_failure();
		[[nodiscard]] sdk::result<std::vector<source_closure_manifest>>
		request_manifests(const materialization_request_v2_2&,
						  std::span<const materialization_store_v6_task_input>,
						  std::stop_token);
		[[nodiscard]] sdk::result<std::vector<std::string>>
		validate_output_authority(const sdk::relation_engine&,
								  const materialization_v4_incremental_receipt&,
								  const materialization_v4_provider_output_authority&,
								  std::span<const materialization_store_v6_task_input>,
								  std::stop_token);
		[[nodiscard]] sdk::result<void>
		validate_execution_journal(const materialization_v4_incremental_receipt&,
								   const materialization_v4_execution_receipt&,
								   std::span<const materialization_store_v6_task_input>,
								   std::stop_token);
		[[nodiscard]] sdk::result<void>
		validate_normalized_rows(const sdk::relation_engine&,
								 const provider_worker_v4_normalized_output&,
								 std::stop_token);
		[[nodiscard]] sdk::result<void>
		validate_task_authority(const materialization_request_v2_2&,
								std::uint64_t,
								const provider_task_v4_authority_identity&,
								const materialization_v4_claim_sealed&);
		[[nodiscard]] sdk::result<void>
		validate_worker_output(const provider_task_v4_authority_identity&,
							   const provider_worker_v4_receipt&,
							   const provider_worker_v4_normalized_output&,
							   const sdk::provider::detail::sealed_provider_transcript&,
							   const sdk::provider::detail::provider_runtime_receipt&,
							   const materialization_v4_claim_sealed&,
							   std::stop_token);
		[[nodiscard]] sdk::result<void> validate_canonicalization_edges(
			const sdk::relation_engine&,
			const provider_task_v4_authority_identity&,
			const provider_worker_v4_normalized_output&,
			const materialization_v4_claim_sealed&,
			std::span<const materialization_store_v6_canonicalization_input>,
			std::stop_token);
		[[nodiscard]] sdk::result<void>
		validate_origins(const provider_task_v4_authority_identity&,
						 const provider_worker_v4_normalized_output&,
						 const materialization_v4_claim_sealed&,
						 std::span<const materialization_store_v6_canonicalization_input>,
						 std::span<const materialization_store_v6_origin_input>,
						 std::stop_token);
		[[nodiscard]] sdk::result<std::string>
		normalized_output_digest(const provider_worker_v4_normalized_output&,
								 const sdk::provider::detail::provider_runtime_receipt&,
								 const provider_worker_v4_receipt&,
								 std::stop_token);
		[[nodiscard]] sdk::result<materialization_store_v6_partition_end>
		build_task_events(std::uint64_t,
						  std::string_view,
						  std::string_view,
						  const sdk::partition_draft&,
						  const sdk::partition_manifest&,
						  const sdk::snapshot_partition_binding&,
						  std::span<const sdk::claim>,
						  std::span<const sdk::unresolved_reference>,
						  std::span<const materialization_store_v6_origin_input>,
						  std::uint64_t,
						  std::stop_token,
						  const typed_event_consumer&);
		[[nodiscard]] sdk::result<sdk::canonical_value>
		typed_event_value(const materialization_store_v6_typed_event&);
		[[nodiscard]] sdk::result<sdk::canonical_value>
		canonicalization_event_value(const materialization_store_v6_canonicalization_edge_event&);
		[[nodiscard]] sdk::result<materialization_store_v6_canonicalization_edge_event>
		decode_canonicalization_event(std::span<const std::byte>, const sdk::relation_engine&);
		[[nodiscard]] record_kind event_kind(const materialization_store_v6_typed_event&);
		[[nodiscard]] sdk::result<void> append_length_prefixed(materialization_replayable_spool&,
															   std::span<const std::byte>,
															   std::uint64_t&,
															   std::uint64_t,
															   std::uint64_t&);
		[[nodiscard]] sdk::result<void> append_expected(materialization_replayable_spool&,
														const semantic_record&,
														std::uint64_t&,
														std::uint64_t&,
														std::uint64_t,
														std::uint64_t&);
		[[nodiscard]] sdk::result<std::string>
		snapshot_authority_digest(const materialization_v4_provider_output_authority&,
								  std::span<const std::string>,
								  std::stop_token);
	} // namespace

	struct materialization_store_v6_typed_task::state
	{
		std::shared_ptr<const sdk::relation_engine> engine;
		std::shared_ptr<materialization_replayable_spool> spool;
		std::shared_ptr<materialization_replayable_spool> edge_spool;
		std::shared_ptr<task_completion_ledger> ledger;
		materialization_store_v6_typed_task_receipt receipt;
		std::uint64_t offset{};
		std::uint64_t end{};
		std::uint64_t observed{};
		std::uint64_t edge_offset{};
		std::uint64_t edge_end{};
		std::uint64_t edge_observed{};
		std::optional<
			std::tuple<std::uint64_t, std::uint64_t, std::string, std::string, std::string>>
			previous_edge;
		bool eof{};
		bool edge_eof{};
		bool failed{};
	};

	struct materialization_store_v6_expected_authority::state
	{
		std::shared_ptr<materialization_replayable_spool> spool;
		std::uint64_t offset{};
		std::uint64_t end{};
		std::uint64_t expected_count{};
		std::uint64_t observed{};
		bool eof{};
		bool failed{};
	};

	struct materialization_store_v6_typed_ingress::state
	{
		std::shared_ptr<const sdk::relation_engine> engine;
		std::shared_ptr<materialization_replayable_spool> typed_spool;
		std::shared_ptr<materialization_replayable_spool> expected_spool;
		std::shared_ptr<materialization_replayable_spool> edge_spool;
		std::shared_ptr<task_completion_ledger> ledger;
		std::vector<task_range> tasks;
		materialization_store_v6_structural_census structural;
		materialization_store_v6_semantic_census semantic;
		std::string binding;
		std::uint64_t next_task{};
		bool expected_taken{};
	};

	sdk::result<materialization_store_v6_typed_ingress>
	materialization_store_v6_typed_ingress_factory_impl(
		const sdk::relation_engine& engine, materialization_store_v6_ingress_input input)
	{
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (auto valid = validate_materialization_store_v6_task_count(input.tasks.size()); !valid)
			return sdk::unexpected(std::move(valid.error()));
		if (input.tasks.size() != input.request.base_tasks.size() ||
			input.tasks.size() != input.request.task_extensions.size())
			return sdk::unexpected(mismatch("tasks", "request-census"));

		std::vector<const materialization_v4_claim_sealed*> sealed_tasks;
		sealed_tasks.reserve(input.tasks.size());
		for (const auto& task : input.tasks)
		{
			if (input.cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			if (auto bounded = preflight_task_input_bounds(task, input.cancellation); !bounded)
				return sdk::unexpected(std::move(bounded.error()));
			if (auto valid = validate_materialization_v4_claim_receipt(engine, task.claims); !valid)
				return sdk::unexpected(std::move(valid.error()));
			sealed_tasks.push_back(&task.claims);
		}
		if (auto valid = validate_materialization_v4_incremental_receipt(
				engine, input.incremental, sealed_tasks);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (input.incremental.materialization_request_id !=
			input.request.materialization_request_id)
			return sdk::unexpected(mismatch("incremental", "request"));

		auto manifests = request_manifests(input.request, input.tasks, input.cancellation);
		if (!manifests)
			return sdk::unexpected(std::move(manifests.error()));
		auto request = validate_materialization_request_v2_2(
			input.request, input.advertised_features, *manifests);
		if (!request)
			return sdk::unexpected(std::move(request.error()));
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		auto closure_ids = validate_output_authority(
			engine, input.incremental, input.output, input.tasks, input.cancellation);
		if (!closure_ids)
			return sdk::unexpected(std::move(closure_ids.error()));
		if (auto valid = validate_execution_journal(
				input.incremental, input.journal, input.tasks, input.cancellation);
			!valid)
			return sdk::unexpected(std::move(valid.error()));

		auto typed_spool = make_spool();
		auto expected_spool = make_spool();
		auto edge_spool = make_spool();
		if (!typed_spool || !expected_spool || !edge_spool)
			return sdk::unexpected(!typed_spool			 ? std::move(typed_spool.error())
									   : !expected_spool ? std::move(expected_spool.error())
														 : std::move(edge_spool.error()));
		auto state = std::make_unique<materialization_store_v6_typed_ingress::state>();
		state->engine = std::make_shared<const sdk::relation_engine>(engine);
		state->typed_spool = std::move(*typed_spool);
		state->expected_spool = std::move(*expected_spool);
		state->edge_spool = std::move(*edge_spool);
		state->ledger = std::make_shared<task_completion_ledger>();
		state->ledger->complete.resize(input.tasks.size());
		state->tasks.reserve(input.tasks.size());

		stream_digest task_authority_digest;
		stream_digest provider_output_digest;
		stream_digest claim_occurrence_digest;
		stream_digest unique_row_digest_stream;
		stream_digest origin_digest;
		stream_digest canonicalization_edge_digest_stream;
		stream_digest coverage_digest_stream;
		stream_digest unresolved_digest_stream;
		stream_digest semantic_projection_digest;
		std::set<std::string, std::less<>> unique_claim_contents;
		std::uint64_t unique_content_sort_bytes{};
		std::uint64_t typed_bytes{};
		std::uint64_t expected_bytes{};
		std::uint64_t expected_framed_bytes{};
		std::uint64_t edge_bytes{};
		std::uint64_t aggregate_spool_bytes{};
		std::uint64_t normalized_rows{};

		const auto update_text = [](stream_digest& digest,
									const std::string_view value) -> sdk::result<void>
		{
			auto encoded = canonical(text(value));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return digest.update(*encoded);
		};

		for (std::size_t index{}; index < input.tasks.size(); ++index)
		{
			if (input.cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			auto& task = input.tasks[index];
			const auto task_aggregate_begin = aggregate_spool_bytes;
			const std::string authority_digest{task.authority.authority_digest()};
			auto identity = std::move(task.authority).consume();
			if (!identity)
				return sdk::unexpected(std::move(identity.error()));
			if (auto valid =
					validate_task_authority(request->request, index, *identity, task.claims);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_worker_output(*identity,
													task.worker,
													task.normalized,
													task.transcript,
													task.runtime,
													task.claims,
													input.cancellation);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_normalized_rows(engine, task.normalized, input.cancellation);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_canonicalization_edges(engine,
															 *identity,
															 task.normalized,
															 task.claims,
															 task.canonicalization_edges,
															 input.cancellation);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = validate_origins(*identity,
											  task.normalized,
											  task.claims,
											  task.canonicalization_edges,
											  task.origins,
											  input.cancellation);
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			if (authority_digest.empty())
				return sdk::unexpected(mismatch("task-authority", "empty-digest"));
			if (auto updated = update_text(task_authority_digest, authority_digest); !updated)
				return sdk::unexpected(std::move(updated.error()));
			auto output_digest = normalized_output_digest(
				task.normalized, task.runtime, task.worker, input.cancellation);
			if (!output_digest)
				return sdk::unexpected(std::move(output_digest.error()));
			if (auto updated = update_text(provider_output_digest, *output_digest); !updated)
				return sdk::unexpected(std::move(updated.error()));
			for (const auto& batch : task.normalized.batches)
			{
				std::uint64_t next{};
				if (!checked_add(normalized_rows, batch.rows.size(), next))
					return sdk::unexpected(resource("normalized-rows", "checked-overflow"));
				normalized_rows = next;
			}

			const auto task_begin = typed_bytes;
			raw_stream_digest task_source_digest;
			stream_digest stage_semantic_digest;
			std::uint64_t stage_event_count{};
			const typed_event_consumer stage_consumer =
				[&](const materialization_store_v6_typed_event& event) -> sdk::result<void>
			{
				if (input.cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (auto measured = measure_typed_event_source(event); !measured)
					return sdk::unexpected(std::move(measured.error()));
				auto value = typed_event_value(event);
				auto encoded = value
					? canonical(*value)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(value.error())};
				auto projection = semantic_projection(event);
				auto semantic = projection
					? encode_expected_record(*projection)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(projection.error())};
				if (!encoded || !semantic)
					return sdk::unexpected(!encoded ? std::move(encoded.error())
													: std::move(semantic.error()));
				std::array<std::byte, 8U> task_prefix{};
				append_u64(task_prefix, encoded->size());
				if (auto updated = task_source_digest.update(task_prefix); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = task_source_digest.update(*encoded); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = stage_semantic_digest.update(*semantic); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto appended = append_length_prefixed(*state->typed_spool,
														   *encoded,
														   typed_bytes,
														   task_aggregate_begin,
														   aggregate_spool_bytes);
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				std::uint64_t next{};
				if (!checked_add(stage_event_count, 1U, next))
					return sdk::unexpected(resource("stage-event-count", "checked-overflow"));
				stage_event_count = next;
				return {};
			};
			auto stage_end = build_task_events(index,
											   identity->task_id,
											   authority_digest,
											   task.claims.translation.partition,
											   task.claims.partition_manifest,
											   task.claims.partition_binding,
											   task.claims.translation.partition.claims,
											   task.claims.translation.partition.unresolved,
											   task.origins,
											   unique_content_sort_bytes,
											   input.cancellation,
											   stage_consumer);
			if (!stage_end)
				return sdk::unexpected(std::move(stage_end.error()));
			auto stage_digest = stage_semantic_digest.finish();
			auto task_source_sha = task_source_digest.finish();
			if (!stage_digest || !task_source_sha)
				return sdk::unexpected(!stage_digest ? std::move(stage_digest.error())
													 : std::move(task_source_sha.error()));

			// Independently traverse the sealed claim-batch projection.  Never decode the
			// stage spool or reuse its semantic records as the expected oracle.
			stream_digest expected_task_digest;
			std::uint64_t expected_event_count{};
			const typed_event_consumer expected_consumer =
				[&](const materialization_store_v6_typed_event& event) -> sdk::result<void>
			{
				if (input.cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (auto measured = measure_typed_event_source(event); !measured)
					return sdk::unexpected(std::move(measured.error()));
				auto projection = semantic_projection(event);
				auto encoded = projection
					? encode_expected_record(*projection)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(projection.error())};
				if (!projection || !encoded)
					return sdk::unexpected(!projection ? std::move(projection.error())
													   : std::move(encoded.error()));
				if (auto updated = expected_task_digest.update(*encoded); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = semantic_projection_digest.update(*encoded); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto appended = append_expected(*state->expected_spool,
													*projection,
													expected_bytes,
													expected_framed_bytes,
													task_aggregate_begin,
													aggregate_spool_bytes);
					!appended)
					return sdk::unexpected(std::move(appended.error()));
				const auto kind = event_kind(event);
				stream_digest* category{};
				switch (kind)
				{
					case record_kind::claim_occurrence:
						category = &claim_occurrence_digest;
						break;
					case record_kind::detached_row:
						category = &unique_row_digest_stream;
						break;
					case record_kind::claim_annotation:
						category = &origin_digest;
						break;
					case record_kind::coverage:
						category = &coverage_digest_stream;
						break;
					case record_kind::unresolved:
						category = &unresolved_digest_stream;
						break;
					case record_kind::partition_begin:
					case record_kind::partition_end:
						break;
				}
				if (category != nullptr)
					if (auto updated = category->update(*encoded); !updated)
						return sdk::unexpected(std::move(updated.error()));
				std::uint64_t next{};
				if (!checked_add(expected_event_count, 1U, next))
					return sdk::unexpected(resource("expected-event-count", "checked-overflow"));
				expected_event_count = next;
				return {};
			};
			auto expected_end = build_task_events(index,
												  identity->task_id,
												  authority_digest,
												  task.claims.translation.partition,
												  task.claims.partition_manifest,
												  task.claims.partition_binding,
												  task.claims.translation.batch.claims,
												  task.claims.translation.batch.unresolved,
												  task.origins,
												  unique_content_sort_bytes,
												  input.cancellation,
												  expected_consumer);
			if (!expected_end || expected_event_count != stage_event_count ||
				*expected_end != *stage_end)
				return sdk::unexpected(expected_end ? mismatch("expected-events", "stage-census")
													: std::move(expected_end.error()));
			auto expected_digest = expected_task_digest.finish();
			if (!expected_digest || *expected_digest != *stage_digest)
				return sdk::unexpected(mismatch("expected-projection", "independent-stage"));
			for (const auto& claim : task.claims.translation.batch.claims)
			{
				if (input.cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (unique_claim_contents.contains(claim.content))
					continue;
				std::uint64_t charge{};
				if (!checked_add(static_cast<std::uint64_t>(claim.content.size()), 128U, charge))
					return sdk::unexpected(resource("unique-content-sort", "checked-overflow"));
				auto next = checked_materialization_store_v6_sort_arena_charge(
					unique_content_sort_bytes, charge);
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				unique_content_sort_bytes = *next;
				unique_claim_contents.insert(claim.content);
			}

			const auto task_edge_begin = edge_bytes;
			raw_stream_digest task_edge_source_digest;
			std::vector<const materialization_store_v6_canonicalization_input*> ordered_edges;
			std::uint64_t edge_pointer_bytes{};
			if (!checked_multiply(
					task.canonicalization_edges.size(), sizeof(void*), edge_pointer_bytes) ||
				!checked_materialization_store_v6_sort_arena_charge(0U, edge_pointer_bytes))
				return sdk::unexpected(resource("canonicalization-edge-sort", "hard-bound"));
			ordered_edges.reserve(task.canonicalization_edges.size());
			for (const auto& edge : task.canonicalization_edges)
			{
				if (input.cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				ordered_edges.push_back(&edge);
			}
			if (auto sorted = cancellable_sort(
					ordered_edges,
					[](const auto* left, const auto* right)
					{
						return std::tie(left->batch_index,
										left->row_index,
										left->edge.precursor_claim_ref,
										left->edge.final_claim_ref,
										left->edge.transform_semantics) <
							std::tie(right->batch_index,
									 right->row_index,
									 right->edge.precursor_claim_ref,
									 right->edge.final_claim_ref,
									 right->edge.transform_semantics);
					},
					input.cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			for (const auto* edge : ordered_edges)
			{
				if (input.cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				materialization_store_v6_canonicalization_edge_event event{index,
																		   identity->task_id,
																		   edge->batch_index,
																		   edge->row_index,
																		   edge->edge,
																		   edge->hidden_precursor};
				if (auto measured = measure_canonicalization_event_source(event); !measured)
					return sdk::unexpected(std::move(measured.error()));
				auto value = canonicalization_event_value(event);
				auto encoded = value
					? canonical(*value)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(value.error())};
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				std::array<std::byte, 8U> prefix{};
				append_u64(prefix, encoded->size());
				if (auto updated = task_edge_source_digest.update(prefix); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = task_edge_source_digest.update(*encoded); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto updated = canonicalization_edge_digest_stream.update(*encoded); !updated)
					return sdk::unexpected(std::move(updated.error()));
				if (auto appended = append_length_prefixed(*state->edge_spool,
														   *encoded,
														   edge_bytes,
														   task_aggregate_begin,
														   aggregate_spool_bytes);
					!appended)
					return sdk::unexpected(std::move(appended.error()));
			}
			auto task_edge_sha = task_edge_source_digest.finish();
			if (!task_edge_sha)
				return sdk::unexpected(std::move(task_edge_sha.error()));
			auto task_edge_binary = binary_sha256(*task_edge_sha);
			if (!task_edge_binary)
				return sdk::unexpected(std::move(task_edge_binary.error()));

			const auto& end = *stage_end;
			auto task_binary = binary_sha256(*task_source_sha);
			if (!task_binary)
				return sdk::unexpected(std::move(task_binary.error()));
			auto immutable =
				identity_digest("cxxlens.clang22.materialization-store-v6-task-authority.v1",
								{text(identity->task_id),
								 text(authority_digest),
								 text(task.claims.receipt.receipt_digest),
								 text(task.runtime.sealed_transcript_digest()),
								 text(input.journal.execution_digest),
								 text(*stage_digest),
								 text(*task_edge_sha),
								 count_value(stage_event_count),
								 count_value(typed_bytes - task_begin),
								 count_value(task.canonicalization_edges.size()),
								 count_value(edge_bytes - task_edge_begin)});
			if (!immutable)
				return sdk::unexpected(std::move(immutable.error()));
			materialization_store_v6_typed_task_receipt task_receipt{
				identity->task_id,
				index,
				1U,
				stage_event_count,
				end.claim_occurrence_count,
				end.unique_claim_content_count,
				end.unique_row_count,
				end.annotation_count,
				end.coverage_count,
				end.unresolved_count,
				typed_bytes - task_begin,
				*task_binary,
				static_cast<std::uint64_t>(task.canonicalization_edges.size()),
				edge_bytes - task_edge_begin,
				*task_edge_binary,
				std::move(*immutable)};
			state->tasks.push_back(task_range{
				std::move(task_receipt), task_begin, typed_bytes, task_edge_begin, edge_bytes});

			std::uint64_t next{};
			if (!checked_add(state->structural.event_count, stage_event_count, next))
				return sdk::unexpected(resource("event-count", "checked-overflow"));
			state->structural.event_count = next;
			const auto add_count = [&](std::uint64_t& target,
									   const std::uint64_t value,
									   const std::string_view field) -> sdk::result<void>
			{
				std::uint64_t output{};
				if (!checked_add(target, value, output))
					return sdk::unexpected(resource(std::string{field}, "checked-overflow"));
				target = output;
				return {};
			};
			if (auto added = add_count(state->structural.claim_occurrence_count,
									   end.claim_occurrence_count,
									   "claim-occurrence-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = add_count(
					state->structural.unique_row_count, end.unique_row_count, "unique-row-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = add_count(
					state->structural.annotation_count, end.annotation_count, "annotation-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = add_count(
					state->structural.coverage_count, end.coverage_count, "coverage-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = add_count(
					state->structural.unresolved_count, end.unresolved_count, "unresolved-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
			if (auto added = add_count(state->structural.canonicalization_edge_count,
									   task.canonicalization_edges.size(),
									   "canonicalization-edge-count");
				!added)
				return sdk::unexpected(std::move(added.error()));
		}

		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (auto sealed = seal_spool(*state->typed_spool); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (auto sealed = seal_spool(*state->expected_spool); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (auto sealed = seal_spool(*state->edge_spool); !sealed)
			return sdk::unexpected(std::move(sealed.error()));
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		std::uint64_t combined_spool_bytes{};
		std::uint64_t typed_and_expected{};
		if (!checked_add(typed_bytes, expected_bytes, typed_and_expected) ||
			!checked_add(typed_and_expected, edge_bytes, combined_spool_bytes))
			return sdk::unexpected(resource("sealed-spool", "checked-overflow"));
		if (state->typed_spool->size_bytes() != typed_bytes ||
			state->expected_spool->size_bytes() != expected_bytes ||
			state->edge_spool->size_bytes() != edge_bytes ||
			aggregate_spool_bytes != combined_spool_bytes)
			return sdk::unexpected(mismatch("sealed-spool", "size"));

		state->structural.task_count = input.tasks.size();
		state->structural.partition_count = input.tasks.size();
		state->structural.canonical_source_bytes = typed_bytes;
		auto exact_spool_sha =
			digest_spool_with_cancellation(*state->typed_spool, input.cancellation);
		if (!exact_spool_sha)
			return sdk::unexpected(std::move(exact_spool_sha.error()));
		auto typed_sha = sdk::result<std::string>{std::move(*exact_spool_sha)};
		auto typed_binary = binary_sha256(*typed_sha);
		if (!typed_binary)
			return sdk::unexpected(std::move(typed_binary.error()));
		state->structural.canonical_source_sha256 = *typed_binary;
		state->structural.canonicalization_edge_bytes = edge_bytes;
		auto exact_edge_sha =
			digest_spool_with_cancellation(*state->edge_spool, input.cancellation);
		if (!exact_edge_sha)
			return sdk::unexpected(std::move(exact_edge_sha.error()));
		auto edge_binary = binary_sha256(*exact_edge_sha);
		if (!edge_binary)
			return sdk::unexpected(std::move(edge_binary.error()));
		state->structural.canonicalization_edge_sha256 = *edge_binary;
		const __uint128_t event_formula =
			static_cast<__uint128_t>(state->structural.partition_count) * 2U +
			state->structural.claim_occurrence_count + state->structural.unique_row_count +
			state->structural.annotation_count + state->structural.coverage_count +
			state->structural.unresolved_count;
		if (event_formula > std::numeric_limits<std::uint64_t>::max() ||
			static_cast<std::uint64_t>(event_formula) != state->structural.event_count)
			return sdk::unexpected(mismatch("structural-census", "event-formula"));

		stream_digest unique_content_stream;
		for (const auto& content : unique_claim_contents)
		{
			if (input.cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			if (auto updated = update_text(unique_content_stream, content); !updated)
				return sdk::unexpected(std::move(updated.error()));
		}
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		auto task_set = task_authority_digest.finish();
		auto provider_set = provider_output_digest.finish();
		auto occurrences = claim_occurrence_digest.finish();
		auto contents = unique_content_stream.finish();
		const auto unique_claim_content_count =
			static_cast<std::uint64_t>(unique_claim_contents.size());
		std::set<std::string, std::less<>>{}.swap(unique_claim_contents);
		unique_content_sort_bytes = 0U;
		auto rows = unique_row_digest_stream.finish();
		auto origins = origin_digest.finish();
		auto canonicalization_edges = canonicalization_edge_digest_stream.finish();
		auto coverage = coverage_digest_stream.finish();
		auto unresolved = unresolved_digest_stream.finish();
		auto projection = semantic_projection_digest.finish();
		auto snapshot = snapshot_authority_digest(input.output, *closure_ids, input.cancellation);
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		if (!task_set || !provider_set || !occurrences || !contents || !rows || !origins ||
			!canonicalization_edges || !coverage || !unresolved || !projection || !snapshot)
			return sdk::unexpected(invalid("semantic-census", "digest-finalize"));
		stream_digest closure_stream;
		for (const auto& id : *closure_ids)
		{
			if (input.cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			if (auto updated = update_text(closure_stream, id); !updated)
				return sdk::unexpected(std::move(updated.error()));
		}
		auto closures = closure_stream.finish();
		if (!closures)
			return sdk::unexpected(std::move(closures.error()));
		std::uint64_t descriptor_batches{};
		if (!checked_add(0U,
						 static_cast<std::uint64_t>(input.tasks.size()) *
							 task_v4_output_descriptor_ids.size(),
						 descriptor_batches))
			return sdk::unexpected(resource("descriptor-batches", "checked-overflow"));
		state->semantic = materialization_store_v6_semantic_census{
			unique_claim_content_count,
			descriptor_batches,
			normalized_rows,
			static_cast<std::uint64_t>(input.output.closures.size()),
			state->structural.canonicalization_edge_count,
			request->request.request_digest,
			std::move(*task_set),
			std::move(*provider_set),
			std::move(*occurrences),
			std::move(*contents),
			std::move(*rows),
			std::move(*origins),
			std::move(*canonicalization_edges),
			std::move(*coverage),
			std::move(*unresolved),
			std::move(*closures),
			input.journal.execution_digest,
			std::move(*snapshot),
			{}};
		auto semantic_input =
			identity_digest("cxxlens.clang22.materialization-store-v6-semantic-input.v1",
							{text(state->semantic.request_digest),
							 text(state->semantic.task_authority_set_digest),
							 text(state->semantic.provider_output_digest),
							 text(state->semantic.claim_occurrence_digest),
							 text(state->semantic.unique_claim_content_digest),
							 text(state->semantic.unique_row_digest),
							 text(state->semantic.origin_association_digest),
							 text(state->semantic.canonicalization_edge_digest),
							 text(state->semantic.coverage_digest),
							 text(state->semantic.unresolved_digest),
							 text(state->semantic.closure_digest),
							 text(state->semantic.journal_digest),
							 text(state->semantic.snapshot_authority_digest),
							 text(*projection),
							 count_value(state->structural.task_count),
							 count_value(state->structural.event_count),
							 count_value(state->semantic.unique_claim_content_count),
							 count_value(state->semantic.normalized_descriptor_batch_count),
							 count_value(state->semantic.normalized_row_count),
							 count_value(state->semantic.canonicalization_edge_count)});
		if (!semantic_input)
			return sdk::unexpected(std::move(semantic_input.error()));
		state->semantic.semantic_input_digest = std::move(*semantic_input);
		auto binding =
			identity_digest("cxxlens.clang22.materialization-store-v6-ingress-authority.v1",
							{text(materialization_store_v6_typed_ingress_schema),
							 text(state->semantic.semantic_input_digest),
							 text(*typed_sha),
							 text(*exact_edge_sha),
							 count_value(state->structural.canonical_source_bytes),
							 count_value(state->structural.canonicalization_edge_bytes),
							 count_value(expected_framed_bytes)});
		if (!binding)
			return sdk::unexpected(std::move(binding.error()));
		state->binding = std::move(*binding);
		if (input.cancellation.stop_requested())
			return sdk::unexpected(cancelled());
		return materialization_store_v6_typed_ingress{std::move(state)};
	}

	sdk::result<materialization_store_v6_typed_ingress>
	make_materialization_store_v6_typed_ingress(const sdk::relation_engine& engine,
												materialization_store_v6_ingress_input input)
	{
		try
		{
			return materialization_store_v6_typed_ingress_factory_impl(engine, std::move(input));
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			return sdk::unexpected(invalid("factory.exception", exception.what()));
		}
		catch (...)
		{
			return sdk::unexpected(invalid("factory.exception", "non-standard"));
		}
	}
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::error cancelled()
		{
			return {"materialization.store-v6-typed-ingress-cancelled", "request", {}};
		}

		[[nodiscard]] sdk::error allocation_failure()
		{
			return {"materialization.store-v6-typed-ingress-resource-exhausted",
					"allocation",
					"bad-alloc"};
		}

		[[nodiscard]] sdk::result<void> require_strong(const std::string_view value,
													   const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::canonical_value series_value(const sdk::snapshot_series_selector& value)
		{
			return tuple({text(value.catalog_id),
						  text(value.channel_id),
						  text(value.engine_generation_id),
						  text(value.condition_universe_id),
						  text(value.relation_registry_digest),
						  text(value.interpretation_policy_digest),
						  text(value.trust_policy_digest)});
		}

		[[nodiscard]] sdk::canonical_value closure_value(const sdk::closure_candidate& value)
		{
			return tuple({text(value.relation_descriptor_id),
						  text(value.subject_partition_id),
						  text(value.partition_content_digest),
						  text(value.coverage_digest),
						  text(value.key_domain_digest),
						  condition_value(value.condition),
						  text(value.interpretation),
						  text(value.assumption_set_id),
						  text(value.closure_kind),
						  text(value.producer_semantics),
						  text(value.evidence_digest)});
		}

		[[nodiscard]] sdk::result<std::string> digest_value(const std::string_view domain,
															const sdk::canonical_value& value)
		{
			auto encoded = canonical(value);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			return sdk::semantic_digest(
				domain,
				std::string_view{reinterpret_cast<const char*>(encoded->data()), encoded->size()});
		}

		[[nodiscard]] sdk::result<std::string>
		normalized_output_digest(const provider_worker_v4_normalized_output& normalized,
								 const sdk::provider::detail::provider_runtime_receipt& runtime,
								 const provider_worker_v4_receipt& worker,
								 const std::stop_token cancellation)
		{
			std::vector<sdk::canonical_value> batches;
			batches.reserve(normalized.batches.size());
			for (const auto& batch : normalized.batches)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				std::vector<sdk::canonical_value> rows;
				rows.reserve(batch.rows.size());
				for (const auto& row : batch.rows)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					rows.push_back(row_value(row));
				}
				batches.push_back(tuple({text(batch.descriptor_id),
										 text(batch.dependency_group_id),
										 text(batch.atomic_output_group_id),
										 text(batch.batch_id),
										 tuple(std::move(rows))}));
			}
			std::vector<sdk::canonical_value> unresolved;
			unresolved.reserve(normalized.unresolved.size());
			for (const auto& value : normalized.unresolved)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				unresolved.push_back(
					tuple({text(value.code), text(value.subject), text(value.detail)}));
			}
			std::vector<sdk::canonical_value> missing;
			missing.reserve(worker.missing_output.size());
			for (const auto& value : worker.missing_output)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				missing.push_back(
					tuple({text(value.field), text(value.required_for), text(value.reason)}));
			}
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return digest_value(
				"cxxlens.clang22.materialization-store-v6-provider-output.v1",
				tuple({text(normalized.task_id),
					   text(normalized.task_v4_digest),
					   text(normalized.compile_unit),
					   tuple(std::move(batches)),
					   tuple(std::move(unresolved)),
					   strings(normalized.limitations),
					   sdk::canonical_value::from_boolean(normalized.exact_equivalence),
					   text(runtime.raw_stdout_sha256()),
					   text(runtime.frame_transcript_digest()),
					   text(runtime.sealed_transcript_digest()),
					   text(worker.output_state),
					   sdk::canonical_value::from_boolean(worker.translation_unit_executed),
					   tuple(std::move(missing))}));
		}

		[[nodiscard]] sdk::result<std::string>
		snapshot_authority_digest(const materialization_v4_provider_output_authority& output,
								  const std::span<const std::string> closure_ids,
								  const std::stop_token cancellation)
		{
			std::vector<std::pair<std::vector<std::byte>, sdk::canonical_value>> ordered_closures;
			std::uint64_t sort_bytes{};
			std::uint64_t vector_bytes{};
			if (!checked_multiply(output.closures.size(),
								  sizeof(typename decltype(ordered_closures)::value_type),
								  vector_bytes))
				return sdk::unexpected(resource("snapshot-closure-sort", "checked-overflow"));
			auto initial_sort =
				checked_materialization_store_v6_sort_arena_charge(sort_bytes, vector_bytes);
			if (!initial_sort)
				return sdk::unexpected(std::move(initial_sort.error()));
			sort_bytes = *initial_sort;
			ordered_closures.reserve(output.closures.size());
			for (const auto& value : output.closures)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				source_byte_meter meter;
				measure_closure(meter, value);
				auto measured = meter.finish();
				std::uint64_t reservation{};
				if (!measured || !checked_multiply(*measured, 2U, reservation))
					return sdk::unexpected(
						!measured ? std::move(measured.error())
								  : resource("snapshot-closure-sort", "checked-overflow"));
				auto next_sort =
					checked_materialization_store_v6_sort_arena_charge(sort_bytes, reservation);
				if (!next_sort)
					return sdk::unexpected(std::move(next_sort.error()));
				sort_bytes = *next_sort;
				auto projection = closure_value(value);
				auto encoded = canonical(projection);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				ordered_closures.emplace_back(std::move(*encoded), std::move(projection));
			}
			if (auto sorted = cancellable_sort(
					ordered_closures,
					[](const auto& left, const auto& right)
					{
						return left.first < right.first;
					},
					cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			std::vector<sdk::canonical_value> closures;
			std::uint64_t closure_vector_bytes{};
			if (!checked_multiply(
					ordered_closures.size(), sizeof(sdk::canonical_value), closure_vector_bytes) ||
				!checked_materialization_store_v6_sort_arena_charge(sort_bytes,
																	closure_vector_bytes))
				return sdk::unexpected(resource("snapshot-closure-sort", "hard-bound"));
			closures.reserve(ordered_closures.size());
			for (auto& [encoded, projection] : ordered_closures)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				(void)encoded;
				closures.push_back(std::move(projection));
			}
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return digest_value("cxxlens.clang22.materialization-store-v6-snapshot-authority.v1",
								tuple({text(output.schema),
									   text(output.materialization_request_id),
									   text(output.publication.analysis_recipe_digest),
									   text(output.publication.output_plan_digest),
									   text(output.publication.publication_target),
									   series_value(output.snapshot.series),
									   text(output.snapshot.snapshot_semantics_version.string()),
									   text(output.snapshot.catalog_semantic_digest),
									   output.snapshot.expected_parent_publication
										   ? text(*output.snapshot.expected_parent_publication)
										   : sdk::canonical_value::null(),
									   tuple(std::move(closures)),
									   strings(closure_ids)}));
		}

		[[nodiscard]] sdk::result<void>
		append_length_prefixed(materialization_replayable_spool& spool,
							   const std::span<const std::byte> record,
							   std::uint64_t& logical_bytes,
							   const std::uint64_t task_aggregate_begin,
							   std::uint64_t& aggregate_bytes)
		{
			std::uint64_t next{};
			std::uint64_t record_charge{};
			sdk::result<std::uint64_t> next_aggregate = sdk::unexpected(resource("spool-charge"));
			if (record.size() > sdk::detail::bounded_store_v6_record_buffer_bytes ||
				!checked_add(record_length_prefix_bytes,
							 static_cast<std::uint64_t>(record.size()),
							 record_charge) ||
				!checked_add(logical_bytes, record_charge, next) ||
				!(next_aggregate = checked_materialization_store_v6_spool_charge(
					  aggregate_bytes, task_aggregate_begin, record_charge)))
				return sdk::unexpected(resource("typed-spool", "record-or-aggregate-bound"));
			std::array<std::byte, 8U> prefix{};
			append_u64(prefix, record.size());
			if (auto written = append_spool(spool, prefix); !written)
				return written;
			if (auto written = append_spool(spool, record); !written)
				return written;
			logical_bytes = next;
			aggregate_bytes = *next_aggregate;
			return {};
		}

		[[nodiscard]] sdk::result<void> append_expected(materialization_replayable_spool& spool,
														const semantic_record& record,
														std::uint64_t& spool_bytes,
														std::uint64_t& framed_bytes,
														const std::uint64_t task_aggregate_begin,
														std::uint64_t& aggregate_bytes)
		{
			if (auto measured = measure_semantic_record_source(record); !measured)
				return sdk::unexpected(std::move(measured.error()));
			auto key = canonical(record.key);
			auto payload = canonical(record.payload);
			if (!key || !payload)
				return sdk::unexpected(!key ? std::move(key.error()) : std::move(payload.error()));
			auto physical = sdk::detail::checked_bounded_store_v6_record_frame_bytes(
				key->size(), payload->size());
			auto encoded = encode_expected_record(record);
			std::uint64_t next_spool{};
			std::uint64_t next_physical{};
			sdk::result<std::uint64_t> next_aggregate = sdk::unexpected(resource("spool-charge"));
			if (!physical)
				return sdk::unexpected(std::move(physical.error()));
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			if (!checked_add(spool_bytes, encoded->size(), next_spool) ||
				!checked_add(framed_bytes, *physical, next_physical) ||
				!(next_aggregate = checked_materialization_store_v6_spool_charge(
					  aggregate_bytes, task_aggregate_begin, encoded->size())) ||
				next_spool > sdk::detail::bounded_store_v6_max_aggregate_bytes ||
				next_physical > sdk::detail::bounded_store_v6_max_aggregate_bytes)
				return sdk::unexpected(resource("expected-spool", "record-or-aggregate-bound"));
			if (auto written = append_spool(spool, *encoded); !written)
				return written;
			spool_bytes = next_spool;
			framed_bytes = next_physical;
			aggregate_bytes = *next_aggregate;
			return {};
		}

		[[nodiscard]] std::pair<std::uint64_t, std::string_view>
		event_task_identity(const materialization_store_v6_typed_event& event)
		{
			return std::visit(
				[](const auto& value)
				{
					return std::pair<std::uint64_t, std::string_view>{value.task_index,
																	  value.task_id};
				},
				event);
		}
	} // namespace

} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] std::string legacy_row_digest(const sdk::detached_row& value)
		{
			const auto form = value.canonical_form();
			return sdk::content_digest(std::as_bytes(std::span{form.data(), form.size()}));
		}

		[[nodiscard]] sdk::result<std::string>
		expected_association_id(const materialization_origin_association& value)
		{
			return identity_digest("materialization-claim-association",
								   {text(value.stored_claim_ref),
									task_context_value(value.originating_task),
									text(value.sealed_row_digest),
									text(value.source_evidence_digest.value_or(""))});
		}

		[[nodiscard]] bool same_row(const sdk::detached_row& left, const sdk::detached_row& right)
		{
			return left.descriptor_id == right.descriptor_id &&
				left.canonical_form() == right.canonical_form();
		}

		[[nodiscard]] sdk::result<void>
		validate_worker_output(const provider_task_v4_authority_identity& identity,
							   const provider_worker_v4_receipt& worker,
							   const provider_worker_v4_normalized_output& normalized,
							   const sdk::provider::detail::sealed_provider_transcript& transcript,
							   const sdk::provider::detail::provider_runtime_receipt& runtime,
							   const materialization_v4_claim_sealed& claims,
							   const std::stop_token cancellation)
		{
			if (auto valid = worker.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = normalized.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (auto valid = runtime.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (worker.task_id != identity.task_id ||
				worker.task_v4_digest != identity.task_v4_digest ||
				worker.task_v4_input_digest != identity.task_input_digest ||
				worker.source_closure_id != identity.closure_id ||
				worker.main_file_id != claims.translation.binding.base_task.source.file_id)
				return sdk::unexpected(mismatch("worker-receipt", "task-or-closure"));
			if (normalized.task_id != identity.task_id ||
				normalized.task_v4_digest != identity.task_v4_digest)
				return sdk::unexpected(mismatch("normalized-output", "task"));
			if (identity.output_group_count != task_v4_output_descriptor_ids.size())
				return sdk::unexpected(mismatch("task-authority.output-group-count", "six"));

			const auto sealed_batches = transcript.batches();
			if (sealed_batches.size() != normalized.batches.size())
				return sdk::unexpected(mismatch("provider-transcript", "six-batch-census"));
			for (std::size_t index{}; index < normalized.batches.size(); ++index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto& expected = normalized.batches[index];
				const auto& actual = sealed_batches[index];
				if (expected.descriptor_id != task_v4_output_descriptor_ids[index] ||
					actual.task_id() != identity.task_id ||
					actual.descriptor_id() != expected.descriptor_id ||
					actual.dependency_group_id() != expected.dependency_group_id ||
					actual.atomic_output_group_id() != expected.atomic_output_group_id ||
					actual.batch_id() != expected.batch_id ||
					actual.rows().size() != expected.rows.size())
					return sdk::unexpected(mismatch("provider-transcript.batch", "binding"));
				for (std::size_t row_index{}; row_index < expected.rows.size(); ++row_index)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					if (!same_row(actual.rows()[row_index], expected.rows[row_index]))
						return sdk::unexpected(
							mismatch("provider-transcript.batch-row", "typed-output"));
				}
			}
			if (transcript.unresolved().size() != normalized.unresolved.size() ||
				!std::ranges::equal(transcript.unresolved(), normalized.unresolved))
				return sdk::unexpected(mismatch("provider-transcript.unresolved", "normalized"));

			const auto& provenance = runtime.provenance();
			if (provenance.provider_id != claims.translation.binding.provider_id ||
				provenance.provider_semantic_contract_digest !=
					claims.translation.binding.provider_semantic_contract_digest ||
				provenance.protocol_session_id != identity.session_id ||
				provenance.task_id != identity.task_id ||
				provenance.task_input_digest != identity.task_input_digest ||
				provenance.normalized_invocation_digest != identity.normalized_invocation_digest ||
				provenance.toolchain_digest != identity.toolchain_digest ||
				provenance.environment_digest != identity.environment_digest ||
				provenance.stream_id != identity.stream_id ||
				runtime.first_frame_sequence() != identity.first_sequence ||
				!sdk::validate_strong_id(provenance.provider_binary_digest) ||
				!sdk::validate_strong_id(provenance.sandbox_policy_digest))
				return sdk::unexpected(mismatch("runtime-receipt", "task-process-authority"));
			auto sealed_digest = sdk::provider::detail::provider_sealed_transcript_receipt_digest(
				identity.task_id, "provider.success", transcript);
			if (!sealed_digest || *sealed_digest != runtime.sealed_transcript_digest())
				return sdk::unexpected(mismatch("runtime-receipt", "sealed-transcript"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_task_authority(const materialization_request_v2_2& request,
								const std::uint64_t index,
								const provider_task_v4_authority_identity& identity,
								const materialization_v4_claim_sealed& claims)
		{
			if (index >= request.base_tasks.size() || index >= request.task_extensions.size())
				return sdk::unexpected(mismatch("task-authority", "request-index"));
			const auto& base = request.base_tasks[static_cast<std::size_t>(index)];
			const auto& task = request.task_extensions[static_cast<std::size_t>(index)];
			const auto& binding = claims.translation.binding;
			if (identity.request_schema != request.schema ||
				identity.request_version != request.request_version ||
				identity.request_id != request.request_id ||
				identity.request_digest != request.request_digest ||
				identity.materialization_request_id != request.materialization_request_id ||
				identity.semantic_request_digest != request.semantic_request_digest ||
				identity.protocol_major != request.protocol_major ||
				identity.protocol_minor != request.protocol_minor ||
				identity.required_features != request.required_features ||
				identity.task_count != request.task_extensions.size() ||
				identity.task_index != index || identity.provider_task_id != task.task_id ||
				identity.provider_execution_id != base.provider_execution_id ||
				identity.task_id != task.task_id ||
				identity.task_v4_digest != task.task_v4_digest ||
				identity.base_task_digest != task.base_task_digest ||
				identity.main_logical_path != task.main_logical_path ||
				identity.logical_working_directory != task.logical_working_directory ||
				identity.task_input_digest != task.open_task.task_input_digest ||
				identity.normalized_invocation_digest !=
					task.open_task.normalized_invocation_digest ||
				identity.environment_digest != task.open_task.environment_digest ||
				identity.closure_id != task.source_closure.source_closure_id ||
				identity.closure_digest != task.source_closure.source_closure_digest ||
				identity.manifest_digest != task.source_closure.manifest_digest)
				return sdk::unexpected(mismatch("task-authority", "request-task-closure"));
			if (binding.materialization_request_id != request.materialization_request_id ||
				binding.task_index != index ||
				binding.base_task.provider_task_id != base.provider_task_id ||
				binding.base_task.canonical_base_task_digest != base.canonical_base_task_digest ||
				binding.task.task_id != task.task_id ||
				binding.task.task_v4_digest != task.task_v4_digest ||
				binding.manifest.closure_id != identity.closure_id ||
				binding.manifest.closure_digest != identity.closure_digest ||
				binding.manifest.manifest_digest != identity.manifest_digest)
				return sdk::unexpected(mismatch("task-authority", "sealed-claim-binding"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_canonicalization_edges(
			const sdk::relation_engine& engine,
			const provider_task_v4_authority_identity& identity,
			const provider_worker_v4_normalized_output& normalized,
			const materialization_v4_claim_sealed& sealed,
			const std::span<const materialization_store_v6_canonicalization_input> edges,
			const std::stop_token cancellation)
		{
			std::map<std::string, const sdk::claim*, std::less<>> final_claims;
			for (const auto& claim : sealed.translation.partition.claims)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				auto ref = derive_materialization_store_v6_claim_ref(claim);
				if (!ref || !final_claims.emplace(*ref, &claim).second)
					return sdk::unexpected(invalid("canonicalization-edge", "final-claim-ref"));
			}

			std::uint64_t canonical_row_count{};
			for (std::size_t batch_index{}; batch_index < 3U; ++batch_index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				std::uint64_t next{};
				if (!checked_add(
						canonical_row_count,
						static_cast<std::uint64_t>(normalized.batches[batch_index].rows.size()),
						next))
					return sdk::unexpected(resource("canonicalization-edge", "row-count"));
				canonical_row_count = next;
			}
			std::set<std::pair<std::uint64_t, std::uint64_t>> mapped_rows;
			std::set<std::string, std::less<>> precursor_refs;
			std::set<std::string, std::less<>> final_refs;
			for (const auto& input : edges)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (input.batch_index >= 3U || input.batch_index >= normalized.batches.size())
					return sdk::unexpected(mismatch("canonicalization-edge", "batch-index"));
				const auto& batch = normalized.batches[static_cast<std::size_t>(input.batch_index)];
				if (input.row_index >= batch.rows.size() ||
					batch.descriptor_id !=
						task_v4_output_descriptor_ids[static_cast<std::size_t>(
							input.batch_index)] ||
					!precursor_refs.insert(input.edge.precursor_claim_ref).second ||
					!final_refs.insert(input.edge.final_claim_ref).second ||
					input.edge.transform_semantics !=
						sealed.translation.binding.canonical_adoption_transform_digest ||
					!sdk::validate_strong_id(input.edge.transform_semantics))
					return sdk::unexpected(
						mismatch("canonicalization-edge", "mapping-or-transform"));
				const auto& worker_row = batch.rows[static_cast<std::size_t>(input.row_index)];
				if (!same_row(input.hidden_precursor.row, worker_row))
					return sdk::unexpected(mismatch("canonicalization-edge", "precursor-row"));
				auto precursor_ref = claim_envelope_ref("hidden_precursor", input.hidden_precursor);
				const auto found = final_claims.find(input.edge.final_claim_ref);
				if (!precursor_ref || *precursor_ref != input.edge.precursor_claim_ref ||
					found == final_claims.end())
					return sdk::unexpected(mismatch("canonicalization-edge", "claim-ref"));
				const sdk::claim_producer materializer{
					sealed.translation.binding.materializer_id,
					sealed.translation.binding.materializer_semantic_contract_digest};
				auto recomputed = sdk::make_canonical_claim(engine,
															input.hidden_precursor,
															materializer,
															worker_row,
															input.edge.transform_semantics);
				auto expected_bytes = recomputed
					? claim_bytes(*recomputed)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(recomputed.error())};
				auto actual_bytes = claim_bytes(*found->second);
				if (!same_row(found->second->row, worker_row) || !expected_bytes || !actual_bytes ||
					*expected_bytes != *actual_bytes ||
					input.hidden_precursor.producer.id != sealed.translation.binding.provider_id ||
					input.hidden_precursor.producer.semantic_contract !=
						sealed.translation.binding.provider_semantic_contract_digest ||
					input.hidden_precursor.row.descriptor_id != batch.descriptor_id)
					return sdk::unexpected(mismatch("canonicalization-edge", "canonical-claim"));
				mapped_rows.emplace(input.batch_index, input.row_index);
			}
			if (mapped_rows.size() != canonical_row_count)
				return sdk::unexpected(mismatch("canonicalization-edge", "canonical-row-census"));
			(void)identity;
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_origins(
			const provider_task_v4_authority_identity& identity,
			const provider_worker_v4_normalized_output& normalized,
			const materialization_v4_claim_sealed& sealed,
			const std::span<const materialization_store_v6_canonicalization_input> edges,
			const std::span<const materialization_store_v6_origin_input> origins,
			const std::stop_token cancellation)
		{
			std::map<std::string, const sdk::claim*, std::less<>> claims_by_ref;
			for (const auto& claim : sealed.translation.partition.claims)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				auto ref = derive_materialization_store_v6_claim_ref(claim);
				if (!ref || !claims_by_ref.emplace(*ref, &claim).second)
					return sdk::unexpected(invalid("claim-occurrence", "duplicate-ref"));
			}
			std::map<std::pair<std::uint64_t, std::uint64_t>, std::set<std::string, std::less<>>>
				canonical_rows;
			for (const auto& edge : edges)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				canonical_rows[{edge.batch_index, edge.row_index}].insert(
					edge.edge.final_claim_ref);
			}
			std::set<std::string, std::less<>> association_ids;
			std::map<std::string, std::uint64_t, std::less<>> association_count;
			std::set<std::pair<std::uint64_t, std::uint64_t>> mapped_rows;
			std::uint64_t worker_row_count{};
			for (const auto& batch : normalized.batches)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				std::uint64_t next{};
				if (!checked_add(worker_row_count, batch.rows.size(), next))
					return sdk::unexpected(resource("origin-association", "row-count"));
				worker_row_count = next;
			}
			for (const auto& input : origins)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto& value = input.association;
				if (input.batch_index >= normalized.batches.size())
					return sdk::unexpected(mismatch("origin-association", "batch-index"));
				const auto& batch = normalized.batches[static_cast<std::size_t>(input.batch_index)];
				if (input.row_index >= batch.rows.size() ||
					batch.descriptor_id !=
						task_v4_output_descriptor_ids[static_cast<std::size_t>(input.batch_index)])
					return sdk::unexpected(mismatch("origin-association", "row-index-or-replay"));
				mapped_rows.emplace(input.batch_index, input.row_index);
				const auto& worker_row = batch.rows[static_cast<std::size_t>(input.row_index)];
				auto expected = expected_association_id(value);
				const auto found = claims_by_ref.find(value.stored_claim_ref);
				if (!expected || *expected != value.association_id ||
					!association_ids.insert(value.association_id).second ||
					found == claims_by_ref.end() ||
					legacy_row_digest(worker_row) != value.sealed_row_digest ||
					value.originating_task.provider_task_id != identity.provider_task_id ||
					value.originating_task.task_input_digest != identity.task_input_digest ||
					value.originating_task.selected_catalog_compile_unit_id !=
						normalized.compile_unit ||
					value.originating_task.compile_unit_id.empty() ||
					value.originating_task.condition_universe_id !=
						sealed.translation.partition.condition.universe ||
					value.originating_task.condition_id !=
						sealed.translation.partition.condition.id() ||
					value.originating_task.interpretation_domain !=
						sealed.translation.partition.interpretation ||
					(value.source_evidence_digest &&
					 !sdk::validate_strong_id(*value.source_evidence_digest)))
					return sdk::unexpected(mismatch("origin-association", "authority-or-identity"));
				const auto canonical = canonical_rows.find({input.batch_index, input.row_index});
				if ((input.batch_index < 3U &&
					 (canonical == canonical_rows.end() ||
					  !canonical->second.contains(value.stored_claim_ref))) ||
					(input.batch_index >= 3U && !same_row(found->second->row, worker_row)))
					return sdk::unexpected(mismatch("origin-association", "claim-row-mapping"));
				++association_count[value.stored_claim_ref];
			}
			if (mapped_rows.size() != worker_row_count)
				return sdk::unexpected(mismatch("origin-association", "worker-row-census"));
			for (const auto& [claim_ref, claim] : claims_by_ref)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				(void)claim;
				if (association_count[claim_ref] == 0U)
					return sdk::unexpected(mismatch("origin-association", "claim-without-origin"));
			}
			return {};
		}

		[[nodiscard]] sdk::result<materialization_store_v6_partition_end>
		build_task_events(const std::uint64_t task_index,
						  const std::string_view task_id,
						  const std::string_view task_authority_digest,
						  const sdk::partition_draft& partition,
						  const sdk::partition_manifest& manifest,
						  const sdk::snapshot_partition_binding& binding,
						  const std::span<const sdk::claim> claims,
						  const std::span<const sdk::unresolved_reference> unresolved,
						  const std::span<const materialization_store_v6_origin_input> origins,
						  const std::uint64_t retained_sort_bytes,
						  const std::stop_token cancellation,
						  const typed_event_consumer& consume)
		{
			if (!consume || cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			std::uint64_t sort_bytes{retained_sort_bytes};
			if (sort_bytes > sdk::detail::bounded_store_v6_sort_arena_bytes)
				return sdk::unexpected(resource("task-sort-arena", "retained-bound"));
			const auto charge_sort = [&](const std::uint64_t bytes_value) -> sdk::result<void>
			{
				auto next =
					checked_materialization_store_v6_sort_arena_charge(sort_bytes, bytes_value);
				if (!next)
					return sdk::unexpected(std::move(next.error()));
				sort_bytes = *next;
				return {};
			};
			const auto charge_vector = [&](const std::uint64_t count,
										   const std::uint64_t element_bytes) -> sdk::result<void>
			{
				std::uint64_t allocation{};
				if (!checked_multiply(count, element_bytes, allocation))
					return sdk::unexpected(resource("task-sort-arena", "checked-overflow"));
				return charge_sort(allocation);
			};
			stream_digest partition_digest;
			const auto emit = [&](const materialization_store_v6_typed_event& event,
								  const bool include_in_partition_digest =
									  true) -> sdk::result<void>
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (include_in_partition_digest)
				{
					if (auto measured = measure_typed_event_source(event); !measured)
						return sdk::unexpected(std::move(measured.error()));
					auto projection = semantic_projection(event);
					auto encoded = projection
						? encode_expected_record(*projection)
						: sdk::result<std::vector<std::byte>>{sdk::unexpected(projection.error())};
					if (!encoded)
						return sdk::unexpected(std::move(encoded.error()));
					if (auto updated = partition_digest.update(*encoded); !updated)
						return sdk::unexpected(std::move(updated.error()));
				}
				return consume(event);
			};

			materialization_store_v6_typed_event begin = materialization_store_v6_partition_begin{
				task_index, std::string{task_id}, std::string{task_authority_digest}, binding};
			if (auto consumed = emit(begin); !consumed)
				return sdk::unexpected(std::move(consumed.error()));

			struct claim_order_entry
			{
				std::vector<std::byte> order;
				std::string ref;
				const sdk::claim* claim{};
			};
			std::vector<claim_order_entry> claim_order;
			if (auto charged = charge_vector(claims.size(), sizeof(claim_order_entry)); !charged)
				return sdk::unexpected(std::move(charged.error()));
			claim_order.reserve(claims.size());
			std::map<std::string, const sdk::claim*, std::less<>> claims_by_ref;
			std::map<std::vector<std::byte>, const sdk::detached_row*, std::less<>> unique_rows;
			std::set<std::string, std::less<>> unique_contents;
			for (const auto& claim : claims)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				source_byte_meter claim_meter;
				claim_meter.node(5U);
				claim_meter.text_value(task_id);
				claim_meter.text_value(manifest.partition_id);
				// A derived claim reference is one strong-id string. Reserve its complete shape
				// before the claim/ref/order-key/map allocations below.
				claim_meter.add(96U);
				measure_claim(claim_meter, claim);
				auto measured = claim_meter.finish();
				std::uint64_t key_reservation{};
				std::uint64_t key_with_nodes{};
				if (!measured || !checked_multiply(*measured, 4U, key_reservation) ||
					!checked_add(key_reservation, 512U, key_with_nodes))
					return sdk::unexpected(!measured
											   ? std::move(measured.error())
											   : resource("task-sort-arena", "checked-overflow"));
				if (auto charged = charge_sort(key_with_nodes); !charged)
					return sdk::unexpected(std::move(charged.error()));
				auto ref = derive_materialization_store_v6_claim_ref(claim);
				auto row_projection = canonical(row_value(claim.row));
				if (!ref || !row_projection || !claims_by_ref.emplace(*ref, &claim).second)
					return sdk::unexpected(invalid("task-events.claim", "identity"));
				materialization_store_v6_typed_event event =
					materialization_store_v6_claim_occurrence{
						task_index, std::string{task_id}, manifest.partition_id, *ref, claim};
				auto projected = semantic_projection(event);
				auto order = projected
					? semantic_order_key(*projected)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(projected.error())};
				if (!order)
					return sdk::unexpected(std::move(order.error()));
				claim_order.push_back({std::move(*order), std::move(*ref), &claim});
				if (auto [found, inserted] = unique_rows.try_emplace(*row_projection, &claim.row);
					inserted)
					(void)found;
				unique_contents.insert(claim.content);
			}
			if (auto sorted = cancellable_sort(
					claim_order,
					[](const auto& left, const auto& right)
					{
						return left.order < right.order;
					},
					cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			for (std::size_t index{1U}; index < claim_order.size(); ++index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (claim_order[index - 1U].order == claim_order[index].order)
					return sdk::unexpected(invalid("task-events.claim", "duplicate-occurrence"));
			}
			for (const auto& ordered : claim_order)
			{
				materialization_store_v6_typed_event event =
					materialization_store_v6_claim_occurrence{task_index,
															  std::string{task_id},
															  manifest.partition_id,
															  ordered.ref,
															  *ordered.claim};
				if (auto consumed = emit(event); !consumed)
					return sdk::unexpected(std::move(consumed.error()));
			}
			std::vector<claim_order_entry>{}.swap(claim_order);

			struct ordered_row
			{
				std::vector<std::byte> order;
				std::string digest;
				const sdk::detached_row* row{};
			};
			std::vector<ordered_row> rows;
			if (auto charged = charge_vector(unique_rows.size(), sizeof(ordered_row)); !charged)
				return sdk::unexpected(std::move(charged.error()));
			rows.reserve(unique_rows.size());
			for (const auto& [projection, row] : unique_rows)
			{
				(void)projection;
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				source_byte_meter row_meter;
				row_meter.node(5U);
				row_meter.text_value(task_id);
				row_meter.text_value(manifest.partition_id);
				row_meter.add(96U);
				measure_row(row_meter, *row);
				auto measured = row_meter.finish();
				std::uint64_t reservation{};
				std::uint64_t with_nodes{};
				if (!measured || !checked_multiply(*measured, 4U, reservation) ||
					!checked_add(reservation, 384U, with_nodes))
					return sdk::unexpected(!measured
											   ? std::move(measured.error())
											   : resource("task-sort-arena", "checked-overflow"));
				if (auto charged = charge_sort(with_nodes); !charged)
					return sdk::unexpected(std::move(charged.error()));
				auto digest = row_digest(*row);
				if (!digest)
					return sdk::unexpected(std::move(digest.error()));
				materialization_store_v6_typed_event event = materialization_store_v6_detached_row{
					task_index, std::string{task_id}, manifest.partition_id, *digest, *row};
				auto projected = semantic_projection(event);
				auto order = projected
					? semantic_order_key(*projected)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(projected.error())};
				if (!order)
					return sdk::unexpected(std::move(order.error()));
				rows.push_back({std::move(*order), std::move(*digest), row});
			}
			if (auto sorted = cancellable_sort(
					rows,
					[](const auto& left, const auto& right)
					{
						return left.order < right.order;
					},
					cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			for (const auto& ordered : rows)
			{
				materialization_store_v6_typed_event event =
					materialization_store_v6_detached_row{task_index,
														  std::string{task_id},
														  manifest.partition_id,
														  ordered.digest,
														  *ordered.row};
				if (auto consumed = emit(event); !consumed)
					return sdk::unexpected(std::move(consumed.error()));
			}
			std::vector<ordered_row>{}.swap(rows);

			struct ordered_origin
			{
				std::vector<std::byte> order;
				const materialization_store_v6_origin_input* origin{};
				const sdk::claim* claim{};
			};
			std::vector<ordered_origin> origin_order;
			if (auto charged = charge_vector(origins.size(), sizeof(ordered_origin)); !charged)
				return sdk::unexpected(std::move(charged.error()));
			origin_order.reserve(origins.size());
			for (const auto& origin : origins)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto found = claims_by_ref.find(origin.association.stored_claim_ref);
				if (found == claims_by_ref.end())
					return sdk::unexpected(mismatch("task-events.origin", "claim-ref"));
				source_byte_meter origin_meter;
				origin_meter.node(5U);
				origin_meter.text_value(task_id);
				origin_meter.text_value(manifest.partition_id);
				measure_association(origin_meter, origin.association);
				measure_claim(origin_meter, *found->second);
				auto measured = origin_meter.finish();
				std::uint64_t reservation{};
				std::uint64_t with_nodes{};
				if (!measured || !checked_multiply(*measured, 4U, reservation) ||
					!checked_add(reservation, 384U, with_nodes))
					return sdk::unexpected(!measured
											   ? std::move(measured.error())
											   : resource("task-sort-arena", "checked-overflow"));
				if (auto charged = charge_sort(with_nodes); !charged)
					return sdk::unexpected(std::move(charged.error()));
				materialization_store_v6_typed_event event =
					materialization_store_v6_claim_annotation{task_index,
															  std::string{task_id},
															  manifest.partition_id,
															  origin.association,
															  *found->second};
				auto projected = semantic_projection(event);
				auto order = projected
					? semantic_order_key(*projected)
					: sdk::result<std::vector<std::byte>>{sdk::unexpected(projected.error())};
				if (!order)
					return sdk::unexpected(std::move(order.error()));
				origin_order.push_back({std::move(*order), &origin, found->second});
			}
			if (auto sorted = cancellable_sort(
					origin_order,
					[](const auto& left, const auto& right)
					{
						return left.order < right.order;
					},
					cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			for (std::size_t index{1U}; index < origin_order.size(); ++index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (origin_order[index - 1U].order == origin_order[index].order)
					return sdk::unexpected(invalid("task-events.origin", "duplicate"));
			}
			for (const auto& ordered : origin_order)
			{
				materialization_store_v6_typed_event event =
					materialization_store_v6_claim_annotation{task_index,
															  std::string{task_id},
															  manifest.partition_id,
															  ordered.origin->association,
															  *ordered.claim};
				if (auto consumed = emit(event); !consumed)
					return sdk::unexpected(std::move(consumed.error()));
			}
			std::vector<ordered_origin>{}.swap(origin_order);

			const auto emit_simple = [&](const auto& values,
										 const auto& value_projection,
										 const auto& make_event,
										 const std::string_view field) -> sdk::result<void>
			{
				using value_type = typename std::decay_t<decltype(values)>::value_type;
				std::vector<std::pair<std::vector<std::byte>, const value_type*>> ordered;
				if (auto charged = charge_vector(values.size(),
												 sizeof(typename decltype(ordered)::value_type));
					!charged)
					return sdk::unexpected(std::move(charged.error()));
				ordered.reserve(values.size());
				for (const auto& value : values)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					source_byte_meter value_meter;
					value_meter.node(4U);
					value_meter.text_value(task_id);
					value_meter.text_value(manifest.partition_id);
					if constexpr (std::is_same_v<value_type, sdk::snapshot_coverage_unit>)
						measure_coverage(value_meter, value);
					else
						measure_unresolved(value_meter, value);
					auto measured = value_meter.finish();
					std::uint64_t reservation{};
					std::uint64_t with_nodes{};
					if (!measured || !checked_multiply(*measured, 3U, reservation) ||
						!checked_add(reservation, 256U, with_nodes))
						return sdk::unexpected(
							!measured ? std::move(measured.error())
									  : resource("task-sort-arena", "checked-overflow"));
					if (auto charged = charge_sort(with_nodes); !charged)
						return sdk::unexpected(std::move(charged.error()));
					auto projection = canonical(value_projection(value));
					if (!projection)
						return sdk::unexpected(std::move(projection.error()));
					ordered.emplace_back(std::move(*projection), &value);
				}
				if (auto sorted = cancellable_sort(
						ordered,
						[](const auto& left, const auto& right)
						{
							return left.first < right.first;
						},
						cancellation);
					!sorted)
					return sdk::unexpected(std::move(sorted.error()));
				for (std::size_t index{1U}; index < ordered.size(); ++index)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					if (ordered[index - 1U].first == ordered[index].first)
						return sdk::unexpected(invalid(std::string{field}, "duplicate"));
				}
				for (const auto& [order, value] : ordered)
				{
					(void)order;
					materialization_store_v6_typed_event event = make_event(*value);
					if (auto consumed = emit(event); !consumed)
						return sdk::unexpected(std::move(consumed.error()));
				}
				return {};
			};
			if (auto consumed = emit_simple(
					partition.coverage,
					[](const sdk::snapshot_coverage_unit& value)
					{
						return coverage_value(value);
					},
					[&](const sdk::snapshot_coverage_unit& value)
					{
						return materialization_store_v6_coverage{
							task_index, std::string{task_id}, manifest.partition_id, value};
					},
					"task-events.coverage");
				!consumed)
				return sdk::unexpected(std::move(consumed.error()));
			if (auto consumed = emit_simple(
					unresolved,
					[](const sdk::unresolved_reference& value)
					{
						return unresolved_value(value);
					},
					[&](const sdk::unresolved_reference& value)
					{
						return materialization_store_v6_unresolved{
							task_index, std::string{task_id}, manifest.partition_id, value};
					},
					"task-events.unresolved");
				!consumed)
				return sdk::unexpected(std::move(consumed.error()));

			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			auto digest = partition_digest.finish();
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			materialization_store_v6_partition_end end{
				task_index,
				std::string{task_id},
				manifest,
				static_cast<std::uint64_t>(claims.size()),
				static_cast<std::uint64_t>(unique_contents.size()),
				static_cast<std::uint64_t>(unique_rows.size()),
				static_cast<std::uint64_t>(origins.size()),
				static_cast<std::uint64_t>(partition.coverage.size()),
				static_cast<std::uint64_t>(unresolved.size()),
				std::move(*digest)};
			materialization_store_v6_typed_event end_event = end;
			if (auto consumed = emit(end_event, false); !consumed)
				return sdk::unexpected(std::move(consumed.error()));
			if (cancellation.stop_requested())
				return sdk::unexpected(cancelled());
			return end;
		}
	} // namespace
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::result<materialization_store_v6_typed_event>
		decode_typed_event(const std::span<const std::byte> encoded,
						   const sdk::relation_engine& engine)
		{
			auto value = sdk::canonical_binary_decode(encoded);
			if (!value || value->type != sdk::canonical_value::kind::ordered_tuple ||
				value->tuple.empty())
				return sdk::unexpected(invalid("typed-event", "canonical-record"));
			auto tag = require_text(value->tuple[0U], "typed-event.tag");
			if (!tag)
				return sdk::unexpected(std::move(tag.error()));
			if (*tag == "partition_begin")
			{
				auto fields = require_tuple(*value, 5U, "typed-event.partition-begin");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto authority = require_text((**fields)[3U], "typed-event.task-authority");
				auto binding = decode_binding((**fields)[4U]);
				if (!index || !task || !authority || !binding)
					return sdk::unexpected(invalid("typed-event.partition-begin", "field"));
				return materialization_store_v6_typed_event{
					materialization_store_v6_partition_begin{
						*index, std::move(*task), std::move(*authority), std::move(*binding)}};
			}
			if (*tag == "claim_occurrence")
			{
				auto fields = require_tuple(*value, 6U, "typed-event.claim");
				if (!fields || (**fields)[5U].type != sdk::canonical_value::kind::bytes)
					return sdk::unexpected(invalid("typed-event.claim", "shape"));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto partition = require_text((**fields)[3U], "typed-event.partition-id");
				auto claim_ref = require_text((**fields)[4U], "typed-event.claim-ref");
				auto claim = sdk::detail::decode_store_claim((**fields)[5U].byte_string, engine);
				if (!index || !task || !partition || !claim_ref || !claim)
					return sdk::unexpected(invalid("typed-event.claim", "field"));
				return materialization_store_v6_typed_event{
					materialization_store_v6_claim_occurrence{*index,
															  std::move(*task),
															  std::move(*partition),
															  std::move(*claim_ref),
															  std::move(*claim)}};
			}
			if (*tag == "detached_row")
			{
				auto fields = require_tuple(*value, 6U, "typed-event.row");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto partition = require_text((**fields)[3U], "typed-event.partition-id");
				auto digest = require_text((**fields)[4U], "typed-event.row-digest");
				auto row = decode_row((**fields)[5U], engine);
				if (!index || !task || !partition || !digest || !row)
					return sdk::unexpected(invalid("typed-event.row", "field"));
				auto expected = row_digest(*row);
				if (!expected || *expected != *digest)
					return sdk::unexpected(mismatch("typed-event.row-digest", "recomputed"));
				return materialization_store_v6_typed_event{
					materialization_store_v6_detached_row{*index,
														  std::move(*task),
														  std::move(*partition),
														  std::move(*digest),
														  std::move(*row)}};
			}
			if (*tag == "claim_annotation")
			{
				auto fields = require_tuple(*value, 6U, "typed-event.annotation");
				if (!fields || (**fields)[5U].type != sdk::canonical_value::kind::bytes)
					return sdk::unexpected(invalid("typed-event.annotation", "shape"));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto partition = require_text((**fields)[3U], "typed-event.partition-id");
				auto association = decode_association((**fields)[4U]);
				auto claim = sdk::detail::decode_store_claim((**fields)[5U].byte_string, engine);
				if (!index || !task || !partition || !association || !claim)
					return sdk::unexpected(invalid("typed-event.annotation", "field"));
				return materialization_store_v6_typed_event{
					materialization_store_v6_claim_annotation{*index,
															  std::move(*task),
															  std::move(*partition),
															  std::move(*association),
															  std::move(*claim)}};
			}
			if (*tag == "coverage")
			{
				auto fields = require_tuple(*value, 5U, "typed-event.coverage");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto partition = require_text((**fields)[3U], "typed-event.partition-id");
				auto coverage = decode_coverage((**fields)[4U]);
				if (!index || !task || !partition || !coverage)
					return sdk::unexpected(invalid("typed-event.coverage", "field"));
				return materialization_store_v6_typed_event{materialization_store_v6_coverage{
					*index, std::move(*task), std::move(*partition), std::move(*coverage)}};
			}
			if (*tag == "unresolved")
			{
				auto fields = require_tuple(*value, 5U, "typed-event.unresolved");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto partition = require_text((**fields)[3U], "typed-event.partition-id");
				auto unresolved = decode_unresolved((**fields)[4U]);
				if (!index || !task || !partition || !unresolved)
					return sdk::unexpected(invalid("typed-event.unresolved", "field"));
				return materialization_store_v6_typed_event{materialization_store_v6_unresolved{
					*index, std::move(*task), std::move(*partition), std::move(*unresolved)}};
			}
			if (*tag == "partition_end")
			{
				auto fields = require_tuple(*value, 11U, "typed-event.partition-end");
				if (!fields)
					return sdk::unexpected(std::move(fields.error()));
				auto index = require_count((**fields)[1U], "typed-event.task-index");
				auto task = require_text((**fields)[2U], "typed-event.task-id");
				auto manifest = decode_manifest((**fields)[3U]);
				std::array<std::uint64_t, 6U> counts{};
				for (std::size_t position{}; position < counts.size(); ++position)
				{
					auto decoded = require_count((**fields)[4U + position], "typed-event.count");
					if (!decoded)
						return sdk::unexpected(std::move(decoded.error()));
					counts[position] = *decoded;
				}
				auto digest = require_text((**fields)[10U], "typed-event.semantic-digest");
				if (!index || !task || !manifest || !digest)
					return sdk::unexpected(invalid("typed-event.partition-end", "field"));
				return materialization_store_v6_typed_event{
					materialization_store_v6_partition_end{*index,
														   std::move(*task),
														   std::move(*manifest),
														   counts[0U],
														   counts[1U],
														   counts[2U],
														   counts[3U],
														   counts[4U],
														   counts[5U],
														   std::move(*digest)}};
			}
			return sdk::unexpected(invalid("typed-event.tag", "unknown"));
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		encode_expected_record(const semantic_record& record)
		{
			if (!sdk::detail::is_valid(record.kind) ||
				record.key.type != sdk::canonical_value::kind::ordered_tuple ||
				record.payload.type != sdk::canonical_value::kind::ordered_tuple)
				return sdk::unexpected(invalid("expected-record", "shape"));
			if (auto measured = measure_semantic_record_source(record); !measured)
				return sdk::unexpected(std::move(measured.error()));
			auto key = canonical(record.key);
			auto payload = canonical(record.payload);
			if (!key || !payload)
				return sdk::unexpected(!key ? std::move(key.error()) : std::move(payload.error()));
			std::uint64_t body{};
			if (!checked_add(key->size(), payload->size(), body) ||
				body > sdk::detail::bounded_store_v6_record_buffer_bytes - 17U)
				return sdk::unexpected(resource("expected-record", "record-bound"));
			std::vector<std::byte> output;
			output.reserve(static_cast<std::size_t>(17U + body));
			output.push_back(static_cast<std::byte>(record.kind));
			std::array<std::byte, 8U> length{};
			append_u64(length, key->size());
			output.insert(output.end(), length.begin(), length.end());
			append_u64(length, payload->size());
			output.insert(output.end(), length.begin(), length.end());
			output.insert(output.end(), key->begin(), key->end());
			output.insert(output.end(), payload->begin(), payload->end());
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		semantic_order_key(const semantic_record& record)
		{
			if (auto measured = measure_semantic_record_source(record); !measured)
				return sdk::unexpected(std::move(measured.error()));
			auto key = canonical(record.key);
			auto payload = canonical(record.payload);
			if (!key || !payload)
				return sdk::unexpected(!key ? std::move(key.error()) : std::move(payload.error()));
			return canonical(tuple({count_value(static_cast<std::uint64_t>(record.kind)),
									bytes(std::move(*key)),
									bytes(std::move(*payload))}));
		}
	} // namespace
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] sdk::result<const std::vector<sdk::canonical_value>*> require_tuple(
			const sdk::canonical_value& value, const std::size_t size, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple ||
				value.tuple.size() != size)
				return sdk::unexpected(invalid(std::string{field}, "tuple-shape"));
			return &value.tuple;
		}

		[[nodiscard]] sdk::result<std::string> require_text(const sdk::canonical_value& value,
															const std::string_view field,
															const bool allow_empty)
		{
			if (value.type != sdk::canonical_value::kind::utf8_string ||
				(!allow_empty && value.text.empty()))
				return sdk::unexpected(invalid(std::string{field}, "text"));
			return value.text;
		}

		[[nodiscard]] sdk::result<std::uint64_t> require_count(const sdk::canonical_value& value,
															   const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::signed_integer || value.integer < 0)
				return sdk::unexpected(invalid(std::string{field}, "nonnegative-integer"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] sdk::result<bool> require_boolean(const sdk::canonical_value& value,
														const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::boolean)
				return sdk::unexpected(invalid(std::string{field}, "boolean"));
			return value.boolean;
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		decode_strings(const sdk::canonical_value& value, const std::string_view field)
		{
			if (value.type != sdk::canonical_value::kind::ordered_tuple)
				return sdk::unexpected(invalid(std::string{field}, "string-tuple"));
			std::vector<std::string> output;
			output.reserve(value.tuple.size());
			for (const auto& entry : value.tuple)
			{
				auto decoded = require_text(entry, field, true);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.push_back(std::move(*decoded));
			}
			return output;
		}

		[[nodiscard]] sdk::result<sdk::claim_condition>
		decode_condition(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 2U, "condition");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto universe = require_text((**fields)[0U], "condition.universe");
			auto fragments = decode_strings((**fields)[1U], "condition.fragments");
			if (!universe || !fragments)
				return sdk::unexpected(!universe ? std::move(universe.error())
												 : std::move(fragments.error()));
			sdk::claim_condition output{std::move(*universe), std::move(*fragments)};
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(invalid("condition", valid.error().code));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::snapshot_partition_binding>
		decode_binding(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 9U, "partition-binding");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto partition = require_text((**fields)[0U], "partition-binding.partition-id");
			auto descriptor = require_text((**fields)[1U], "partition-binding.descriptor");
			auto scope = require_text((**fields)[2U], "partition-binding.scope");
			auto condition = decode_condition((**fields)[3U]);
			auto interpretation = require_text((**fields)[4U], "partition-binding.interpretation");
			auto producer = require_text((**fields)[5U], "partition-binding.producer");
			auto basis = require_text((**fields)[6U], "partition-binding.basis");
			auto precision = require_text((**fields)[7U], "partition-binding.precision");
			auto assumptions = require_text((**fields)[8U], "partition-binding.assumptions");
			if (!partition || !descriptor || !scope || !condition || !interpretation || !producer ||
				!basis || !precision || !assumptions)
				return sdk::unexpected(invalid("partition-binding", "field"));
			return sdk::snapshot_partition_binding{std::move(*partition),
												   std::move(*descriptor),
												   std::move(*scope),
												   std::move(*condition),
												   std::move(*interpretation),
												   std::move(*producer),
												   std::move(*basis),
												   std::move(*precision),
												   std::move(*assumptions)};
		}

		[[nodiscard]] sdk::result<sdk::partition_manifest>
		decode_manifest(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 8U, "partition-manifest");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto partition = require_text((**fields)[0U], "partition-manifest.partition-id");
			auto descriptor = require_text((**fields)[1U], "partition-manifest.descriptor");
			auto basis = require_text((**fields)[2U], "partition-manifest.basis");
			auto claims = require_text((**fields)[3U], "partition-manifest.claims");
			auto coverage = require_text((**fields)[4U], "partition-manifest.coverage");
			auto content_digest = require_text((**fields)[5U], "partition-manifest.content");
			auto count = require_count((**fields)[6U], "partition-manifest.claim-count");
			auto complete = require_boolean((**fields)[7U], "partition-manifest.complete");
			if (!partition || !descriptor || !basis || !claims || !coverage || !content_digest ||
				!count || !complete)
				return sdk::unexpected(invalid("partition-manifest", "field"));
			return sdk::partition_manifest{std::move(*partition),
										   std::move(*descriptor),
										   std::move(*basis),
										   std::move(*claims),
										   std::move(*coverage),
										   std::move(*content_digest),
										   *count,
										   *complete};
		}

		[[nodiscard]] sdk::result<sdk::scalar_value>
		decode_scalar_value(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 2U, "row.scalar");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto tag = require_text((**fields)[0U], "row.scalar.tag");
			if (!tag)
				return sdk::unexpected(std::move(tag.error()));
			const auto& payload = (**fields)[1U];
			if (*tag == "bool")
			{
				auto decoded = require_boolean(payload, "row.scalar.bool");
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				return sdk::scalar_value{*decoded};
			}
			if (*tag == "signed")
			{
				if (payload.type != sdk::canonical_value::kind::signed_integer)
					return sdk::unexpected(invalid("row.scalar.signed", "integer"));
				return sdk::scalar_value{payload.integer};
			}
			if (*tag == "unsigned")
			{
				auto decoded = require_text(payload, "row.scalar.unsigned");
				if (!decoded || decoded->empty() ||
					!std::ranges::all_of(*decoded,
										 [](const char value)
										 {
											 return value >= '0' && value <= '9';
										 }))
					return sdk::unexpected(invalid("row.scalar.unsigned", "decimal"));
				std::uint64_t output{};
				for (const char digit : *decoded)
				{
					const auto number = static_cast<std::uint64_t>(digit - '0');
					if (output > (std::numeric_limits<std::uint64_t>::max() - number) / 10U)
						return sdk::unexpected(invalid("row.scalar.unsigned", "overflow"));
					output = output * 10U + number;
				}
				return sdk::scalar_value{output};
			}
			if (*tag == "string")
			{
				auto decoded = require_text(payload, "row.scalar.string", true);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				return sdk::scalar_value{std::move(*decoded)};
			}
			if (*tag == "bytes" && payload.type == sdk::canonical_value::kind::bytes)
				return sdk::scalar_value{payload.byte_string};
			return sdk::unexpected(invalid("row.scalar", "tag"));
		}

		[[nodiscard]] sdk::result<sdk::detached_cell> decode_cell(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 6U, "row.cell");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto kind = require_count((**fields)[0U], "row.cell.kind");
			auto parameter = require_text((**fields)[1U], "row.cell.parameter", true);
			auto optional = require_boolean((**fields)[2U], "row.cell.optional");
			auto state = require_count((**fields)[3U], "row.cell.state");
			if (!kind || !parameter || !optional || !state ||
				*kind > static_cast<std::uint64_t>(sdk::scalar_kind::interpretation_domain_id) ||
				*state > static_cast<std::uint64_t>(sdk::cell_state::unknown))
				return sdk::unexpected(invalid("row.cell", "enum"));
			sdk::detached_cell output;
			output.type = {static_cast<sdk::scalar_kind>(*kind), std::move(*parameter), *optional};
			output.state = static_cast<sdk::cell_state>(*state);
			if ((**fields)[4U].type != sdk::canonical_value::kind::null_value)
			{
				auto scalar = decode_scalar_value((**fields)[4U]);
				if (!scalar)
					return sdk::unexpected(std::move(scalar.error()));
				output.value = std::move(*scalar);
			}
			if ((**fields)[5U].type != sdk::canonical_value::kind::null_value)
			{
				auto reason = require_text((**fields)[5U], "row.cell.unknown-reason");
				if (!reason)
					return sdk::unexpected(std::move(reason.error()));
				output.unknown_reason = std::move(*reason);
			}
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(invalid("row.cell", valid.error().code));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::detached_row> decode_row(const sdk::canonical_value& value,
																const sdk::relation_engine& engine)
		{
			auto fields = require_tuple(value, 2U, "row");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto descriptor = require_text((**fields)[0U], "row.descriptor");
			if (!descriptor || (**fields)[1U].type != sdk::canonical_value::kind::ordered_tuple)
				return sdk::unexpected(invalid("row", "shape"));
			sdk::detached_row output;
			output.descriptor_id = std::move(*descriptor);
			for (const auto& entry : (**fields)[1U].tuple)
			{
				auto pair = require_tuple(entry, 2U, "row.cell-entry");
				if (!pair)
					return sdk::unexpected(std::move(pair.error()));
				auto column = require_text((**pair)[0U], "row.column");
				auto cell = decode_cell((**pair)[1U]);
				if (!column || !cell || !output.cells.emplace(*column, std::move(*cell)).second)
					return sdk::unexpected(invalid("row", "cell"));
			}
			auto relation = engine.require_id(output.descriptor_id);
			if (!relation)
				return sdk::unexpected(std::move(relation.error()));
			if (auto valid = sdk::validate_row(relation->descriptor(), output); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] sdk::result<materialization_semantic_task_context>
		decode_task_context(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 7U, "origin.context");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			std::array<std::string, 7U> decoded;
			for (std::size_t index{}; index < decoded.size(); ++index)
			{
				auto item = require_text((**fields)[index], "origin.context.field");
				if (!item)
					return sdk::unexpected(std::move(item.error()));
				decoded[index] = std::move(*item);
			}
			return materialization_semantic_task_context{std::move(decoded[0U]),
														 std::move(decoded[1U]),
														 std::move(decoded[2U]),
														 std::move(decoded[3U]),
														 std::move(decoded[4U]),
														 std::move(decoded[5U]),
														 std::move(decoded[6U])};
		}

		[[nodiscard]] sdk::result<materialization_origin_association>
		decode_association(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 5U, "origin");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto id = require_text((**fields)[0U], "origin.id");
			auto claim = require_text((**fields)[1U], "origin.claim");
			auto context = decode_task_context((**fields)[2U]);
			auto row = require_text((**fields)[3U], "origin.row");
			std::optional<std::string> evidence;
			if ((**fields)[4U].type != sdk::canonical_value::kind::null_value)
			{
				auto decoded = require_text((**fields)[4U], "origin.evidence");
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				evidence = std::move(*decoded);
			}
			if (!id || !claim || !context || !row)
				return sdk::unexpected(invalid("origin", "field"));
			return materialization_origin_association{std::move(*id),
													  std::move(*claim),
													  std::move(*context),
													  std::move(*row),
													  std::move(evidence)};
		}

		[[nodiscard]] sdk::result<sdk::snapshot_coverage_unit>
		decode_coverage(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 4U, "coverage");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			std::array<std::string, 4U> decoded;
			for (std::size_t index{}; index < decoded.size(); ++index)
			{
				auto item = require_text((**fields)[index], "coverage.field", index == 3U);
				if (!item)
					return sdk::unexpected(std::move(item.error()));
				decoded[index] = std::move(*item);
			}
			sdk::snapshot_coverage_unit output{std::move(decoded[0U]),
											   std::move(decoded[1U]),
											   std::move(decoded[2U]),
											   std::move(decoded[3U])};
			if (auto valid = output.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			return output;
		}

		[[nodiscard]] sdk::result<sdk::unresolved_reference>
		decode_unresolved(const sdk::canonical_value& value)
		{
			auto fields = require_tuple(value, 5U, "unresolved");
			if (!fields)
				return sdk::unexpected(std::move(fields.error()));
			auto assertion = require_text((**fields)[0U], "unresolved.assertion");
			auto source = require_text((**fields)[1U], "unresolved.source");
			auto target = require_text((**fields)[2U], "unresolved.target");
			auto columns = decode_strings((**fields)[3U], "unresolved.columns");
			auto reason = require_text((**fields)[4U], "unresolved.reason");
			if (!assertion || !source || !target || !columns || !reason)
				return sdk::unexpected(invalid("unresolved", "field"));
			return sdk::unresolved_reference{std::move(*assertion),
											 std::move(*source),
											 std::move(*target),
											 std::move(*columns),
											 std::move(*reason)};
		}

		[[nodiscard]] record_kind event_kind(const materialization_store_v6_typed_event& event)
		{
			return std::visit(
				[](const auto& value) -> record_kind
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type,
												 materialization_store_v6_partition_begin>)
						return record_kind::partition_begin;
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_occurrence>)
						return record_kind::claim_occurrence;
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_detached_row>)
						return record_kind::detached_row;
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_annotation>)
						return record_kind::claim_annotation;
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_coverage>)
						return record_kind::coverage;
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_unresolved>)
						return record_kind::unresolved;
					else
						return record_kind::partition_end;
				},
				event);
		}

		[[nodiscard]] sdk::result<sdk::canonical_value>
		typed_event_value(const materialization_store_v6_typed_event& event)
		{
			return std::visit(
				[](const auto& value) -> sdk::result<sdk::canonical_value>
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type,
												 materialization_store_v6_partition_begin>)
						return tuple({text("partition_begin"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.task_authority_digest),
									  binding_value(value.binding)});
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_occurrence>)
					{
						auto encoded = claim_bytes(value.value);
						if (!encoded)
							return sdk::unexpected(std::move(encoded.error()));
						return tuple({text("claim_occurrence"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.partition_id),
									  text(value.claim_ref),
									  bytes(std::move(*encoded))});
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_detached_row>)
						return tuple({text("detached_row"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.partition_id),
									  text(value.row_digest),
									  row_value(value.value)});
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_annotation>)
					{
						auto encoded = claim_bytes(value.occurrence);
						if (!encoded)
							return sdk::unexpected(std::move(encoded.error()));
						return tuple({text("claim_annotation"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.partition_id),
									  association_value(value.association),
									  bytes(std::move(*encoded))});
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_coverage>)
						return tuple({text("coverage"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.partition_id),
									  coverage_value(value.value)});
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_unresolved>)
						return tuple({text("unresolved"),
									  count_value(value.task_index),
									  text(value.task_id),
									  text(value.partition_id),
									  unresolved_value(value.value)});
					else
						return tuple({text("partition_end"),
									  count_value(value.task_index),
									  text(value.task_id),
									  manifest_value(value.manifest),
									  count_value(value.claim_occurrence_count),
									  count_value(value.unique_claim_content_count),
									  count_value(value.unique_row_count),
									  count_value(value.annotation_count),
									  count_value(value.coverage_count),
									  count_value(value.unresolved_count),
									  text(value.semantic_partition_digest)});
				},
				event);
		}

		[[nodiscard]] sdk::result<sdk::canonical_value> canonicalization_event_value(
			const materialization_store_v6_canonicalization_edge_event& value)
		{
			auto precursor = claim_bytes(value.hidden_precursor);
			if (!precursor)
				return sdk::unexpected(std::move(precursor.error()));
			return tuple({text("canonicalization_edge"),
						  count_value(value.task_index),
						  text(value.task_id),
						  count_value(value.batch_index),
						  count_value(value.row_index),
						  canonicalization_edge_value(value.edge),
						  bytes(std::move(*precursor))});
		}

		[[nodiscard]] sdk::result<materialization_store_v6_canonicalization_edge_event>
		decode_canonicalization_event(const std::span<const std::byte> encoded,
									  const sdk::relation_engine& engine)
		{
			auto value = sdk::canonical_binary_decode(encoded);
			if (!value)
				return sdk::unexpected(invalid("canonicalization-event", "canonical-record"));
			auto fields = require_tuple(*value, 7U, "canonicalization-event");
			if (!fields || (**fields)[6U].type != sdk::canonical_value::kind::bytes)
				return sdk::unexpected(invalid("canonicalization-event", "shape"));
			auto tag = require_text((**fields)[0U], "canonicalization-event.tag");
			auto task_index = require_count((**fields)[1U], "canonicalization-event.task-index");
			auto task_id = require_text((**fields)[2U], "canonicalization-event.task-id");
			auto batch_index = require_count((**fields)[3U], "canonicalization-event.batch-index");
			auto row_index = require_count((**fields)[4U], "canonicalization-event.row-index");
			auto edge_fields = require_tuple((**fields)[5U], 3U, "canonicalization-event.edge");
			if (!tag || *tag != "canonicalization_edge" || !task_index || !task_id ||
				!batch_index || !row_index || !edge_fields)
				return sdk::unexpected(invalid("canonicalization-event", "field"));
			auto precursor_ref =
				require_text((**edge_fields)[0U], "canonicalization-event.precursor");
			auto final_ref = require_text((**edge_fields)[1U], "canonicalization-event.final");
			auto transform = require_text((**edge_fields)[2U], "canonicalization-event.transform");
			auto precursor = sdk::detail::decode_store_claim((**fields)[6U].byte_string, engine);
			if (!precursor_ref || !final_ref || !transform || !precursor ||
				!sdk::validate_strong_id(*precursor_ref) || !sdk::validate_strong_id(*final_ref) ||
				!sdk::validate_strong_id(*transform))
				return sdk::unexpected(invalid("canonicalization-event", "edge"));
			auto expected = claim_envelope_ref("hidden_precursor", *precursor);
			if (!expected || *expected != *precursor_ref)
				return sdk::unexpected(mismatch("canonicalization-event", "precursor-ref"));
			return materialization_store_v6_canonicalization_edge_event{
				*task_index,
				std::move(*task_id),
				*batch_index,
				*row_index,
				{std::move(*precursor_ref), std::move(*final_ref), std::move(*transform)},
				std::move(*precursor)};
		}

		[[nodiscard]] sdk::result<semantic_record>
		semantic_projection(const materialization_store_v6_typed_event& event)
		{
			return std::visit(
				[](const auto& value) -> sdk::result<semantic_record>
				{
					using value_type = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<value_type,
												 materialization_store_v6_partition_begin>)
						return semantic_record{
							record_kind::partition_begin,
							tuple({text(value.task_id), text(value.binding.partition_id)}),
							tuple({count_value(value.task_index),
								   text(value.task_authority_digest),
								   binding_value(value.binding)})};
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_occurrence>)
					{
						auto encoded = claim_bytes(value.value);
						auto occurrence = sdk::detail::claim_occurrence_projection(value.value);
						if (!encoded || !occurrence)
							return sdk::unexpected(!encoded ? std::move(encoded.error())
															: std::move(occurrence.error()));
						return semantic_record{
							record_kind::claim_occurrence,
							tuple({text(value.task_id),
								   text(value.partition_id),
								   bytes(std::move(*occurrence)),
								   text(value.claim_ref)}),
							tuple({bytes(std::move(*encoded)), text(value.value.content)})};
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_detached_row>)
						return semantic_record{record_kind::detached_row,
											   tuple({text(value.task_id),
													  text(value.partition_id),
													  text(value.value.descriptor_id),
													  text(value.row_digest)}),
											   tuple({row_value(value.value)})};
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_claim_annotation>)
					{
						auto encoded = claim_bytes(value.occurrence);
						if (!encoded)
							return sdk::unexpected(std::move(encoded.error()));
						return semantic_record{record_kind::claim_annotation,
											   tuple({text(value.task_id),
													  text(value.partition_id),
													  text(value.occurrence.content),
													  text(value.association.association_id)}),
											   tuple({association_value(value.association),
													  bytes(std::move(*encoded))})};
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_coverage>)
					{
						auto projection = canonical(coverage_value(value.value));
						if (!projection)
							return sdk::unexpected(std::move(projection.error()));
						return semantic_record{record_kind::coverage,
											   tuple({text(value.task_id),
													  text(value.partition_id),
													  bytes(*projection)}),
											   tuple({coverage_value(value.value)})};
					}
					else if constexpr (std::is_same_v<value_type,
													  materialization_store_v6_unresolved>)
					{
						auto projection = canonical(unresolved_value(value.value));
						if (!projection)
							return sdk::unexpected(std::move(projection.error()));
						return semantic_record{record_kind::unresolved,
											   tuple({text(value.task_id),
													  text(value.partition_id),
													  bytes(*projection)}),
											   tuple({unresolved_value(value.value)})};
					}
					else
						return semantic_record{
							record_kind::partition_end,
							tuple({text(value.task_id), text(value.manifest.partition_id)}),
							tuple({manifest_value(value.manifest),
								   count_value(value.claim_occurrence_count),
								   count_value(value.unique_claim_content_count),
								   count_value(value.unique_row_count),
								   count_value(value.annotation_count),
								   count_value(value.coverage_count),
								   count_value(value.unresolved_count),
								   text(value.semantic_partition_digest)})};
				},
				event);
		}
	} // namespace
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	materialization_store_v6_typed_task::materialization_store_v6_typed_task(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}
	materialization_store_v6_typed_task::materialization_store_v6_typed_task(
		materialization_store_v6_typed_task&&) noexcept = default;
	materialization_store_v6_typed_task& materialization_store_v6_typed_task::operator=(
		materialization_store_v6_typed_task&&) noexcept = default;
	materialization_store_v6_typed_task::~materialization_store_v6_typed_task() = default;

	const materialization_store_v6_typed_task_receipt&
	materialization_store_v6_typed_task::receipt() const noexcept
	{
		static const materialization_store_v6_typed_task_receipt empty{};
		return state_ ? state_->receipt : empty;
	}

	sdk::result<std::optional<materialization_store_v6_typed_event>>
	materialization_store_v6_typed_task::next()
	{
		try
		{
			if (!state_ || state_->failed || state_->eof)
				return sdk::unexpected(invalid("task-cursor", "closed"));
			if (state_->offset == state_->end)
			{
				if (state_->observed != state_->receipt.event_count || state_->observed == 0U)
				{
					state_->failed = true;
					return sdk::unexpected(mismatch("task-cursor", "event-census"));
				}
				state_->eof = true;
				return std::optional<materialization_store_v6_typed_event>{};
			}
			if (state_->end - state_->offset < record_length_prefix_bytes)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("task-cursor", "truncated-prefix"));
			}
			std::array<std::byte, 8U> prefix{};
			if (auto read = read_exact(*state_->spool, state_->offset, prefix); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			const auto size = read_u64(prefix);
			std::uint64_t record_begin{};
			std::uint64_t record_end{};
			if (size == 0U || size > sdk::detail::bounded_store_v6_record_buffer_bytes ||
				!checked_add(state_->offset, record_length_prefix_bytes, record_begin) ||
				!checked_add(record_begin, size, record_end) || record_end > state_->end)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("task-cursor", "record-bound"));
			}
			std::vector<std::byte> encoded(static_cast<std::size_t>(size));
			if (auto read = read_exact(*state_->spool, record_begin, encoded); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			auto event = decode_typed_event(encoded, *state_->engine);
			if (!event)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(event.error()));
			}
			const auto [task_index, task_id] = event_task_identity(*event);
			const auto kind = event_kind(*event);
			const bool first = state_->observed == 0U;
			const bool last = record_end == state_->end;
			if (task_index != state_->receipt.ordinal || task_id != state_->receipt.task_id ||
				(first != (kind == record_kind::partition_begin)) ||
				(last != (kind == record_kind::partition_end)) ||
				(!first && !last &&
				 (kind == record_kind::partition_begin || kind == record_kind::partition_end)))
			{
				state_->failed = true;
				return sdk::unexpected(mismatch("task-cursor", "phase-or-task-binding"));
			}
			state_->offset = record_end;
			++state_->observed;
			return std::optional<materialization_store_v6_typed_event>{std::move(*event)};
		}
		catch (const std::bad_alloc&)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(invalid("task-cursor.exception", exception.what()));
		}
		catch (...)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(invalid("task-cursor.exception", "non-standard"));
		}
	}

	sdk::result<std::optional<materialization_store_v6_canonicalization_edge_event>>
	materialization_store_v6_typed_task::next_canonicalization_edge()
	{
		try
		{
			if (!state_ || state_->failed || !state_->eof || state_->edge_eof)
				return sdk::unexpected(invalid("canonicalization-edge-cursor", "phase-or-closed"));
			if (state_->edge_offset == state_->edge_end)
			{
				if (state_->edge_observed != state_->receipt.canonicalization_edge_count)
				{
					state_->failed = true;
					return sdk::unexpected(mismatch("canonicalization-edge-cursor", "census"));
				}
				state_->edge_eof = true;
				state_->ledger->complete[static_cast<std::size_t>(state_->receipt.ordinal)] = 1U;
				return std::optional<materialization_store_v6_canonicalization_edge_event>{};
			}
			if (state_->edge_end - state_->edge_offset < record_length_prefix_bytes)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("canonicalization-edge-cursor", "prefix"));
			}
			std::array<std::byte, 8U> prefix{};
			if (auto read = read_exact(*state_->edge_spool, state_->edge_offset, prefix); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			const auto size = read_u64(prefix);
			std::uint64_t record_begin{};
			std::uint64_t record_end{};
			if (size == 0U || size > sdk::detail::bounded_store_v6_record_buffer_bytes ||
				!checked_add(state_->edge_offset, record_length_prefix_bytes, record_begin) ||
				!checked_add(record_begin, size, record_end) || record_end > state_->edge_end)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("canonicalization-edge-cursor", "record"));
			}
			std::vector<std::byte> encoded(static_cast<std::size_t>(size));
			if (auto read = read_exact(*state_->edge_spool, record_begin, encoded); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			auto event = decode_canonicalization_event(encoded, *state_->engine);
			const auto current = event ? std::optional{std::tuple{event->batch_index,
																  event->row_index,
																  event->edge.precursor_claim_ref,
																  event->edge.final_claim_ref,
																  event->edge.transform_semantics}}
									   : std::nullopt;
			if (!event || event->task_index != state_->receipt.ordinal ||
				event->task_id != state_->receipt.task_id || event->batch_index >= 3U ||
				(state_->previous_edge && current && !(*state_->previous_edge < *current)))
			{
				state_->failed = true;
				return sdk::unexpected(
					event ? mismatch("canonicalization-edge-cursor", "binding-order")
						  : std::move(event.error()));
			}
			state_->edge_offset = record_end;
			state_->previous_edge = current;
			++state_->edge_observed;
			return std::optional<materialization_store_v6_canonicalization_edge_event>{
				std::move(*event)};
		}
		catch (const std::bad_alloc&)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(
				invalid("canonicalization-edge-cursor.exception", exception.what()));
		}
		catch (...)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(
				invalid("canonicalization-edge-cursor.exception", "non-standard"));
		}
	}

	sdk::result<bool> materialization_store_v6_typed_task::authority_complete() const
	{
		if (!state_)
			return sdk::unexpected(invalid("task-cursor", "moved"));
		return state_->eof && state_->edge_eof && !state_->failed;
	}

	materialization_store_v6_expected_authority::materialization_store_v6_expected_authority(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}
	materialization_store_v6_expected_authority::materialization_store_v6_expected_authority(
		materialization_store_v6_expected_authority&&) noexcept = default;
	materialization_store_v6_expected_authority&
	materialization_store_v6_expected_authority::operator=(
		materialization_store_v6_expected_authority&&) noexcept = default;
	materialization_store_v6_expected_authority::~materialization_store_v6_expected_authority() =
		default;

	sdk::result<std::optional<sdk::detail::bounded_store_v6_semantic_record>>
	materialization_store_v6_expected_authority::next_semantic_record()
	{
		try
		{
			if (!state_ || state_->failed || state_->eof)
				return sdk::unexpected(invalid("expected-cursor", "closed"));
			if (state_->offset == state_->end)
			{
				if (state_->observed != state_->expected_count || state_->observed == 0U)
				{
					state_->failed = true;
					return sdk::unexpected(mismatch("expected-cursor", "event-census"));
				}
				state_->eof = true;
				return std::optional<semantic_record>{};
			}
			if (state_->end - state_->offset < 17U)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("expected-cursor", "truncated-prefix"));
			}
			std::array<std::byte, 17U> prefix{};
			if (auto read = read_exact(*state_->spool, state_->offset, prefix); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			const auto kind = static_cast<record_kind>(std::to_integer<std::uint8_t>(prefix[0U]));
			const auto key_size = read_u64(std::span<const std::byte>{prefix}.subspan(1U, 8U));
			const auto payload_size = read_u64(std::span<const std::byte>{prefix}.subspan(9U, 8U));
			std::uint64_t body{};
			std::uint64_t record_end{};
			if (!sdk::detail::is_valid(kind) || !checked_add(key_size, payload_size, body) ||
				body > sdk::detail::bounded_store_v6_record_buffer_bytes - 17U ||
				!checked_add(state_->offset, 17U, record_end) ||
				!checked_add(record_end, body, record_end) || record_end > state_->end)
			{
				state_->failed = true;
				return sdk::unexpected(spool_failure("expected-cursor", "record-bound"));
			}
			std::vector<std::byte> encoded(static_cast<std::size_t>(body));
			if (auto read = read_exact(*state_->spool, state_->offset + 17U, encoded); !read)
			{
				state_->failed = true;
				return sdk::unexpected(std::move(read.error()));
			}
			auto key = sdk::canonical_binary_decode(
				std::span<const std::byte>{encoded}.first(static_cast<std::size_t>(key_size)));
			auto payload = sdk::canonical_binary_decode(
				std::span<const std::byte>{encoded}.subspan(static_cast<std::size_t>(key_size)));
			if (!key || !payload || key->type != sdk::canonical_value::kind::ordered_tuple ||
				payload->type != sdk::canonical_value::kind::ordered_tuple)
			{
				state_->failed = true;
				return sdk::unexpected(invalid("expected-cursor", "canonical-value"));
			}
			state_->offset = record_end;
			++state_->observed;
			return std::optional<semantic_record>{
				semantic_record{kind, std::move(*key), std::move(*payload)}};
		}
		catch (const std::bad_alloc&)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(allocation_failure());
		}
		catch (const std::exception& exception)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(invalid("expected-cursor.exception", exception.what()));
		}
		catch (...)
		{
			if (state_)
				state_->failed = true;
			return sdk::unexpected(invalid("expected-cursor.exception", "non-standard"));
		}
	}

	sdk::result<bool> materialization_store_v6_expected_authority::authority_complete() const
	{
		if (!state_)
			return sdk::unexpected(invalid("expected-cursor", "moved"));
		return state_->eof && !state_->failed;
	}

	materialization_store_v6_typed_ingress::materialization_store_v6_typed_ingress(
		std::unique_ptr<state> state) noexcept
		: state_{std::move(state)}
	{
	}
	materialization_store_v6_typed_ingress::materialization_store_v6_typed_ingress(
		materialization_store_v6_typed_ingress&&) noexcept = default;
	materialization_store_v6_typed_ingress& materialization_store_v6_typed_ingress::operator=(
		materialization_store_v6_typed_ingress&&) noexcept = default;
	materialization_store_v6_typed_ingress::~materialization_store_v6_typed_ingress() = default;

	const materialization_store_v6_structural_census&
	materialization_store_v6_typed_ingress::structural_census() const noexcept
	{
		static const materialization_store_v6_structural_census empty{};
		return state_ ? state_->structural : empty;
	}

	const materialization_store_v6_semantic_census&
	materialization_store_v6_typed_ingress::semantic_census() const noexcept
	{
		static const materialization_store_v6_semantic_census empty{};
		return state_ ? state_->semantic : empty;
	}

	std::string_view
	materialization_store_v6_typed_ingress::immutable_authority_binding() const noexcept
	{
		return state_ ? std::string_view{state_->binding} : std::string_view{};
	}

	std::uint64_t materialization_store_v6_typed_ingress::task_count() const noexcept
	{
		return state_ ? static_cast<std::uint64_t>(state_->tasks.size()) : 0U;
	}

	sdk::result<materialization_store_v6_typed_task>
	materialization_store_v6_typed_ingress::take_task(const std::uint64_t canonical_ordinal)
	{
		try
		{
			if (!state_ || state_->expected_taken || canonical_ordinal != state_->next_task ||
				canonical_ordinal >= state_->tasks.size())
				return sdk::unexpected(invalid("take-task", "order-or-replay"));
			if (canonical_ordinal != 0U &&
				state_->ledger->complete[static_cast<std::size_t>(canonical_ordinal - 1U)] == 0U)
				return sdk::unexpected(invalid("take-task", "previous-task-incomplete"));
			const auto& range = state_->tasks[static_cast<std::size_t>(canonical_ordinal)];
			auto task = std::make_unique<materialization_store_v6_typed_task::state>();
			task->engine = state_->engine;
			task->spool = state_->typed_spool;
			task->edge_spool = state_->edge_spool;
			task->ledger = state_->ledger;
			task->receipt = range.receipt;
			task->offset = range.begin;
			task->end = range.end;
			task->edge_offset = range.edge_begin;
			task->edge_end = range.edge_end;
			++state_->next_task;
			return materialization_store_v6_typed_task{std::move(task)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(allocation_failure());
		}
	}

	sdk::result<materialization_store_v6_expected_authority>
	materialization_store_v6_typed_ingress::take_expected_authority()
	{
		try
		{
			if (!state_ || state_->expected_taken || state_->next_task != state_->tasks.size() ||
				std::ranges::any_of(state_->ledger->complete,
									[](const std::uint8_t value)
									{
										return value == 0U;
									}))
				return sdk::unexpected(invalid("take-expected", "tasks-incomplete-or-replay"));
			auto expected = std::make_unique<materialization_store_v6_expected_authority::state>();
			expected->spool = state_->expected_spool;
			expected->end = state_->expected_spool->size_bytes();
			expected->expected_count = state_->structural.event_count;
			state_->expected_taken = true;
			return materialization_store_v6_expected_authority{std::move(expected)};
		}
		catch (const std::bad_alloc&)
		{
			return sdk::unexpected(allocation_failure());
		}
	}
} // namespace cxxlens::detail::clang22::materialization

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		[[nodiscard]] bool same_manifest(const source_closure_manifest& left,
										 const source_closure_manifest& right)
		{
			return left.schema == right.schema && left.closure_id == right.closure_id &&
				left.closure_digest == right.closure_digest &&
				left.manifest_digest == right.manifest_digest && left.members == right.members &&
				left.blobs == right.blobs;
		}

		[[nodiscard]] sdk::result<std::vector<source_closure_manifest>>
		request_manifests(const materialization_request_v2_2& request,
						  const std::span<const materialization_store_v6_task_input> tasks,
						  const std::stop_token cancellation)
		{
			std::vector<source_closure_manifest> output;
			output.reserve(request.source_closures.size());
			for (const auto& summary : request.source_closures)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const source_closure_manifest* selected{};
				for (const auto& task : tasks)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					const auto& manifest = task.claims.translation.binding.manifest;
					if (manifest.closure_id != summary.source_closure_id)
						continue;
					if (selected != nullptr && !same_manifest(*selected, manifest))
						return sdk::unexpected(
							mismatch("request.source-closure", "inconsistent-manifest"));
					selected = &manifest;
				}
				if (selected == nullptr)
					return sdk::unexpected(mismatch("request.source-closure", "unreferenced"));
				output.push_back(*selected);
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		validate_output_authority(const sdk::relation_engine& engine,
								  const materialization_v4_incremental_receipt& receipt,
								  const materialization_v4_provider_output_authority& output,
								  const std::span<const materialization_store_v6_task_input> tasks,
								  const std::stop_token cancellation)
		{
			if (output.schema != materialization_v4_provider_output_authority_schema ||
				output.materialization_request_id != receipt.materialization_request_id)
				return sdk::unexpected(mismatch("output-authority", "schema-or-request"));
			if (auto valid = require_strong(output.materialization_request_id,
											"output.materialization-request-id");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			const std::array<std::pair<std::string_view, std::string_view>, 4U> ids{{
				{"output.analysis-recipe", output.publication.analysis_recipe_digest},
				{"output.output-plan", output.publication.output_plan_digest},
				{"output.publication-target", output.publication.publication_target},
				{"output.catalog", output.snapshot.catalog_semantic_digest},
			}};
			for (const auto& [field, value] : ids)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (auto valid = require_strong(value, field); !valid)
					return sdk::unexpected(std::move(valid.error()));
			}
			if (auto valid = output.snapshot.series.validate(); !valid)
				return sdk::unexpected(invalid("output.snapshot.series", valid.error().code));
			if (output.snapshot.series.engine_generation_id != engine.generation() ||
				output.snapshot.series.relation_registry_digest != engine.registry_digest() ||
				output.snapshot.snapshot_semantics_version != sdk::semantic_version{1U, 0U, 0U})
				return sdk::unexpected(mismatch("output.snapshot", "engine-or-version"));
			if (output.snapshot.expected_parent_publication)
				if (auto valid = require_strong(*output.snapshot.expected_parent_publication,
												"output.expected-parent");
					!valid)
					return sdk::unexpected(std::move(valid.error()));
			if (output.closures.size() > materialization_v4_store_max_closures)
				return sdk::unexpected(resource("output.closures", "hard-bound"));

			std::map<std::string, const materialization_v4_claim_sealed*, std::less<>> partitions;
			for (const auto& task : tasks)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto& sealed = task.claims;
				if (!partitions.emplace(sealed.partition_manifest.partition_id, &sealed).second)
					return sdk::unexpected(mismatch("output.partition", "duplicate-id"));
				if (sealed.translation.partition.condition.universe !=
					output.snapshot.series.condition_universe_id)
					return sdk::unexpected(mismatch("output.partition", "condition-universe"));
			}
			std::vector<std::string> closure_ids;
			std::uint64_t closure_id_bytes{};
			if (!checked_multiply(
					output.closures.size(), sizeof(std::string) + 640U, closure_id_bytes) ||
				!checked_materialization_store_v6_sort_arena_charge(0U, closure_id_bytes))
				return sdk::unexpected(resource("output.closure-sort", "hard-bound"));
			closure_ids.reserve(output.closures.size());
			for (const auto& candidate : output.closures)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto found = partitions.find(candidate.subject_partition_id);
				if (found == partitions.end())
					return sdk::unexpected(mismatch("output.closure", "subject-partition"));
				auto subject = sdk::make_partition_certificate_subject(
					found->second->partition_manifest, found->second->partition_binding);
				auto certificate = subject
					? sdk::make_closure_certificate(*subject, candidate)
					: sdk::result<sdk::closure_certificate>{sdk::unexpected(subject.error())};
				if (!certificate)
					return sdk::unexpected(std::move(certificate.error()));
				closure_ids.push_back(std::move(certificate->id));
			}
			if (auto sorted = cancellable_sort(
					closure_ids,
					[](const auto& left, const auto& right)
					{
						return left < right;
					},
					cancellation);
				!sorted)
				return sdk::unexpected(std::move(sorted.error()));
			for (std::size_t index{1U}; index < closure_ids.size(); ++index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				if (closure_ids[index - 1U] == closure_ids[index])
					return sdk::unexpected(mismatch("output.closure", "duplicate-certificate"));
			}
			return closure_ids;
		}

		[[nodiscard]] sdk::result<void>
		validate_execution_journal(const materialization_v4_incremental_receipt& incremental,
								   const materialization_v4_execution_receipt& expected,
								   const std::span<const materialization_store_v6_task_input> tasks,
								   const std::stop_token cancellation)
		{
			if (expected.materialization_request_id != incremental.materialization_request_id ||
				expected.task_count != tasks.size() || expected.tasks.size() != tasks.size() ||
				expected.provider_call_count != tasks.size() || expected.reused_task_count != 0U)
				return sdk::unexpected(mismatch("execution-journal", "request-or-live-census"));
			auto journal = materialization_v4_execution_journal::begin(
				expected.materialization_request_id, expected.task_count);
			if (!journal)
				return sdk::unexpected(std::move(journal.error()));
			for (std::size_t index{}; index < tasks.size(); ++index)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				const auto& execution = expected.tasks[index];
				if (execution.receipt != tasks[index].claims.receipt || execution.reused ||
					execution.provider_call_count != 1U)
					return sdk::unexpected(mismatch("execution-journal.task", "live-task"));
				if (auto recorded = journal->record(
						execution.receipt, execution.reused, execution.provider_call_count);
					!recorded)
					return sdk::unexpected(std::move(recorded.error()));
			}
			auto rebuilt = std::move(*journal).finish(incremental);
			if (!rebuilt)
				return sdk::unexpected(std::move(rebuilt.error()));
			if (*rebuilt != expected)
				return sdk::unexpected(mismatch("execution-journal", "recomputed"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		validate_normalized_rows(const sdk::relation_engine& engine,
								 const provider_worker_v4_normalized_output& normalized,
								 const std::stop_token cancellation)
		{
			for (const auto& batch : normalized.batches)
			{
				if (cancellation.stop_requested())
					return sdk::unexpected(cancelled());
				auto relation = engine.require_id(batch.descriptor_id);
				if (!relation)
					return sdk::unexpected(std::move(relation.error()));
				for (const auto& row : batch.rows)
				{
					if (cancellation.stop_requested())
						return sdk::unexpected(cancelled());
					if (auto valid = sdk::validate_row(relation->descriptor(), row); !valid)
						return sdk::unexpected(std::move(valid.error()));
				}
			}
			return {};
		}
	} // namespace
} // namespace cxxlens::detail::clang22::materialization
