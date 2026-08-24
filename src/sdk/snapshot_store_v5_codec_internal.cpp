#include "snapshot_store_v5_codec_internal.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <ranges>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#include <cxxlens/sdk/store.hpp>

#include "claim_internal.hpp"
#include "store_claim_codec_internal.hpp"
#include "store_identity_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error
		store_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			if (hex.size() != 64U)
				return false;
			return std::ranges::all_of(hex,
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] canonical_value text(std::string value)
		{
			return canonical_value::from_string(std::move(value));
		}

		[[nodiscard]] std::string partition_identity(const partition_draft& draft)
		{
			const std::array fields{text(draft.relation_descriptor_id),
									text(draft.scope),
									text(draft.condition.canonical_form()),
									text(draft.interpretation),
									text(draft.producer_semantics),
									text(draft.producer_input_basis_digest),
									text(draft.precision_profile),
									text(draft.assumption_set_id)};
			return *canonical_identity_digest("partition", fields);
		}

		[[nodiscard]] snapshot_partition_binding partition_binding(const std::string& partition_id,
																   const partition_draft& draft)
		{
			return {partition_id,
					draft.relation_descriptor_id,
					draft.scope,
					draft.condition,
					draft.interpretation,
					draft.producer_semantics,
					draft.producer_input_basis_digest,
					draft.precision_profile,
					draft.assumption_set_id};
		}

		[[nodiscard]] partition_draft identity_draft(const snapshot_partition_binding& binding)
		{
			partition_draft draft;
			draft.relation_descriptor_id = binding.relation_descriptor_id;
			draft.scope = binding.scope;
			draft.condition = binding.condition;
			draft.interpretation = binding.interpretation;
			draft.producer_semantics = binding.producer_semantics;
			draft.producer_input_basis_digest = binding.producer_input_basis_digest;
			draft.precision_profile = binding.precision_profile;
			draft.assumption_set_id = binding.assumption_set_id;
			return draft;
		}

		[[nodiscard]] std::string snapshot_identity(const snapshot_manifest& value)
		{
			return *detail::snapshot_manifest_identity(value);
		}

		[[nodiscard]] std::string publication_identity(const publication_record& value)
		{
			return *detail::publication_record_identity(
				value.series_id, value.snapshot_id, value.sequence, value.parent_publication);
		}

		[[nodiscard]] result<void> validate_publication_identity(const publication_record& value)
		{
			if (value.publication_id != publication_identity(value))
				return unexpected(store_error("store.corrupt", "publication", "identity"));
			return {};
		}

		[[nodiscard]] std::string bytes_hex(const std::span<const std::byte> bytes)
		{
			static constexpr std::string_view digits{"0123456789abcdef"};
			std::string output;
			output.reserve(bytes.size() * 2U);
			for (const auto byte : bytes)
			{
				const auto value = std::to_integer<unsigned char>(byte);
				output.push_back(digits[value >> 4U]);
				output.push_back(digits[value & 0x0fU]);
			}
			return output;
		}

		class binary_writer
		{
		  public:
			struct measure_mode final
			{
			};
			struct sealed_canonical_mode final
			{
			};

			binary_writer() = default;
			explicit binary_writer(sqlite_bounded_byte_sink& sink) noexcept : sink_{&sink} {}
			binary_writer(sqlite_bounded_byte_sink& sink, sealed_canonical_mode) noexcept
				: sink_{&sink}, sealed_canonical_{true}
			{
			}
			binary_writer(measure_mode, const std::uint64_t maximum_bytes) noexcept
				: maximum_bytes_{maximum_bytes}, measuring_{true}
			{
			}

			void unsigned_value(std::uint64_t value)
			{
				std::array<std::byte, 8U> encoded{};
				std::size_t index{};
				for (int shift = 56; shift >= 0; shift -= 8)
					encoded.at(index++) = static_cast<std::byte>((value >> shift) & 0xffU);
				append(encoded);
			}
			void boolean(const bool value)
			{
				const std::array encoded{value ? std::byte{1} : std::byte{0}};
				append(encoded);
			}
			void string(const std::string_view value)
			{
				unsigned_value(value.size());
				if (!value.empty())
					append(std::as_bytes(std::span{value.data(), value.size()}));
			}
			void raw(const std::span<const std::byte> value)
			{
				unsigned_value(value.size());
				if (!value.empty())
					append(value);
			}
			[[nodiscard]] std::vector<std::byte> finish() &&
			{
				return std::move(bytes_);
			}
			[[nodiscard]] result<void> finish_stream() const
			{
				if (sink_ == nullptr || measuring_)
					return unexpected(store_error("store.corrupt", "payload", "writer-mode"));
				if (failure_)
					return unexpected(*failure_);
				return {};
			}
			[[nodiscard]] result<std::uint64_t> finish_measurement() const
			{
				if (!measuring_ || sink_ != nullptr)
					return unexpected(store_error("store.corrupt", "payload", "writer-mode"));
				if (failure_)
					return unexpected(*failure_);
				return measured_bytes_;
			}
			[[nodiscard]] bool measuring() const noexcept
			{
				return measuring_;
			}
			[[nodiscard]] bool sealed_canonical() const noexcept
			{
				return sealed_canonical_;
			}

		  private:
			void append(const std::span<const std::byte> value)
			{
				if (failure_)
					return;
				if (measuring_)
				{
					auto aggregate = measured_aggregate_;
					if (!aggregate.add(static_cast<std::uint64_t>(value.size())))
					{
						failure_ = store_error("store.counter-overflow",
											   "materialization-v5-collection-count");
						return;
					}
					if (aggregate.high != 0U || aggregate.low > maximum_bytes_)
					{
						failure_ = store_error(
							"store.resource-limit", "snapshot-payload", "maximum-bytes");
						return;
					}
					measured_aggregate_ = aggregate;
					measured_bytes_ = aggregate.low;
					return;
				}
				if (sink_ != nullptr)
				{
					auto appended = sink_->append(value);
					if (!appended)
						failure_ = std::move(appended.error());
					return;
				}
				bytes_.insert(bytes_.end(), value.begin(), value.end());
			}

			sqlite_bounded_byte_sink* sink_{};
			std::optional<error> failure_;
			std::vector<std::byte> bytes_;
			sqlite_u128_census measured_aggregate_;
			std::uint64_t measured_bytes_{};
			std::uint64_t maximum_bytes_{};
			bool measuring_{};
			bool sealed_canonical_{};
		};

		class binary_reader
		{
		  public:
			explicit binary_reader(const std::span<const std::byte> bytes)
				: bytes_{bytes}, expected_size_{bytes.size()}
			{
				static_assert(sizeof(std::size_t) <= sizeof(std::uint64_t));
			}
			binary_reader(sqlite_bounded_byte_source& source,
						  const std::uint64_t expected_size) noexcept
				: source_{&source}, expected_size_{expected_size}
			{
			}
			[[nodiscard]] result<std::uint64_t> unsigned_value()
			{
				std::array<std::byte, 8U> encoded{};
				auto read = read_exact(encoded);
				if (!read)
					return unexpected(std::move(read.error()));
				if (!*read)
					return unexpected(store_error("store.corrupt", "payload", "truncated-u64"));
				std::uint64_t output{};
				for (const auto byte : encoded)
					output = (output << 8U) | std::to_integer<unsigned char>(byte);
				return output;
			}
			[[nodiscard]] result<std::uint32_t> unsigned_32(const std::string_view field)
			{
				auto value = unsigned_value();
				if (!value)
					return unexpected(std::move(value.error()));
				if (*value > std::numeric_limits<std::uint32_t>::max())
					return unexpected(
						store_error("store.corrupt", std::string{field}, "u32-overflow"));
				return static_cast<std::uint32_t>(*value);
			}
			[[nodiscard]] result<bool> boolean()
			{
				std::array<std::byte, 1U> encoded{};
				auto read = read_exact(encoded);
				if (!read)
					return unexpected(std::move(read.error()));
				if (!*read || std::to_integer<unsigned char>(encoded.front()) > 1U)
					return unexpected(store_error("store.corrupt", "payload", "invalid-bool"));
				return std::to_integer<unsigned char>(encoded.front()) != 0U;
			}
			[[nodiscard]] result<std::string> string()
			{
				auto size = unsigned_value();
				if (!size)
					return unexpected(std::move(size.error()));
				if (*size > remaining() || *size > std::numeric_limits<std::size_t>::max())
					return unexpected(store_error("store.corrupt", "payload", "truncated-string"));
				std::string output;
				output.resize(static_cast<std::size_t>(*size));
				if (!output.empty())
				{
					auto read =
						read_exact(std::as_writable_bytes(std::span{output.data(), output.size()}));
					if (!read)
						return unexpected(std::move(read.error()));
					if (!*read)
						return unexpected(
							store_error("store.corrupt", "payload", "truncated-string"));
				}
				return output;
			}
			[[nodiscard]] result<std::vector<std::byte>> raw()
			{
				auto size = unsigned_value();
				if (!size)
					return unexpected(std::move(size.error()));
				if (*size > remaining() || *size > std::numeric_limits<std::size_t>::max())
					return unexpected(store_error("store.corrupt", "payload", "truncated-bytes"));
				std::vector<std::byte> output(static_cast<std::size_t>(*size));
				if (!output.empty())
				{
					auto read = read_exact(output);
					if (!read)
						return unexpected(std::move(read.error()));
					if (!*read)
						return unexpected(
							store_error("store.corrupt", "payload", "truncated-bytes"));
				}
				return output;
			}
			[[nodiscard]] result<bool> finished()
			{
				if (offset_ != expected_size_)
					return false;
				if (source_ == nullptr)
					return offset_ == bytes_.size();
				std::array<std::byte, 1U> extra{};
				auto read = source_->read(extra);
				if (!read)
					return unexpected(std::move(read.error()));
				return *read == 0U;
			}
			[[nodiscard]] std::size_t offset() const noexcept
			{
				return static_cast<std::size_t>(offset_);
			}

		  private:
			[[nodiscard]] std::uint64_t remaining() const noexcept
			{
				return offset_ <= expected_size_ ? expected_size_ - offset_ : 0U;
			}
			[[nodiscard]] result<bool> read_exact(const std::span<std::byte> output)
			{
				if (output.size() > remaining())
					return false;
				if (output.empty())
					return true;
				if (source_ == nullptr)
				{
					std::memcpy(output.data(),
								bytes_.data() + static_cast<std::size_t>(offset_),
								output.size());
					offset_ += output.size();
					return true;
				}
				std::size_t copied{};
				while (copied < output.size())
				{
					auto read = source_->read(output.subspan(copied));
					if (!read)
						return unexpected(std::move(read.error()));
					if (*read == 0U)
						return false;
					if (*read > output.size() - copied)
						return unexpected(store_error("store.corrupt", "payload", "source-window"));
					copied += *read;
					offset_ += *read;
				}
				return true;
			}

			std::span<const std::byte> bytes_;
			sqlite_bounded_byte_source* source_{};
			std::uint64_t expected_size_{};
			std::uint64_t offset_{};
		};

		void encode_cell(binary_writer& writer, const detached_cell& cell)
		{
			writer.unsigned_value(static_cast<std::uint8_t>(cell.type.scalar));
			writer.string(cell.type.parameter);
			writer.boolean(cell.type.optional);
			writer.unsigned_value(static_cast<std::uint8_t>(cell.state));
			writer.boolean(cell.value.has_value());
			if (cell.value)
				std::visit(
					[&](const auto& value)
					{
						using type = std::decay_t<decltype(value)>;
						if constexpr (std::is_same_v<type, bool>)
						{
							writer.unsigned_value(0U);
							writer.boolean(value);
						}
						else if constexpr (std::is_same_v<type, std::int64_t>)
						{
							writer.unsigned_value(1U);
							writer.unsigned_value(static_cast<std::uint64_t>(value));
						}
						else if constexpr (std::is_same_v<type, std::uint64_t>)
						{
							writer.unsigned_value(2U);
							writer.unsigned_value(value);
						}
						else if constexpr (std::is_same_v<type, std::string>)
						{
							writer.unsigned_value(3U);
							writer.string(value);
						}
						else
						{
							writer.unsigned_value(4U);
							writer.raw(value);
						}
					},
					*cell.value);
			writer.boolean(cell.unknown_reason.has_value());
			if (cell.unknown_reason)
				writer.string(*cell.unknown_reason);
		}

		[[nodiscard]] result<detached_cell> decode_cell(binary_reader& reader)
		{
			auto scalar = reader.unsigned_value();
			auto parameter = reader.string();
			auto optional = reader.boolean();
			auto state = reader.unsigned_value();
			auto has_value = reader.boolean();
			if (!scalar || !parameter || !optional || !state || !has_value ||
				*scalar > static_cast<std::uint8_t>(scalar_kind::interpretation_domain_id) ||
				*state > static_cast<std::uint8_t>(cell_state::unknown))
				return unexpected(store_error("store.corrupt", "cell", "invalid-header"));
			detached_cell output;
			output.type = {static_cast<scalar_kind>(*scalar), std::move(*parameter), *optional};
			output.state = static_cast<cell_state>(*state);
			if (*has_value)
			{
				auto tag = reader.unsigned_value();
				if (!tag)
					return unexpected(std::move(tag.error()));
				switch (*tag)
				{
					case 0U:
					{
						auto value = reader.boolean();
						if (!value)
							return unexpected(std::move(value.error()));
						output.value = scalar_value{*value};
						break;
					}
					case 1U:
					case 2U:
					{
						auto value = reader.unsigned_value();
						if (!value)
							return unexpected(std::move(value.error()));
						output.value = *tag == 1U ? scalar_value{static_cast<std::int64_t>(*value)}
												  : scalar_value{*value};
						break;
					}
					case 3U:
					{
						auto value = reader.string();
						if (!value)
							return unexpected(std::move(value.error()));
						output.value = scalar_value{std::move(*value)};
						break;
					}
					case 4U:
					{
						auto value = reader.raw();
						if (!value)
							return unexpected(std::move(value.error()));
						output.value = scalar_value{std::move(*value)};
						break;
					}
					default:
						return unexpected(
							store_error("store.corrupt", "cell", "invalid-value-tag"));
				}
			}
			auto has_reason = reader.boolean();
			if (!has_reason)
				return unexpected(std::move(has_reason.error()));
			if (*has_reason)
			{
				auto reason = reader.string();
				if (!reason)
					return unexpected(std::move(reason.error()));
				output.unknown_reason = std::move(*reason);
			}
			if (auto valid = output.validate(); !valid)
				return unexpected(store_error("store.corrupt", "cell", valid.error().code));
			return output;
		}

		void encode_row(binary_writer& writer, const detached_row& row)
		{
			writer.string(row.descriptor_id);
			writer.unsigned_value(row.cells.size());
			for (const auto& [column, cell] : row.cells)
			{
				writer.string(column);
				encode_cell(writer, cell);
			}
		}

		[[nodiscard]] result<detached_row> decode_row(binary_reader& reader)
		{
			auto descriptor = reader.string();
			auto count = reader.unsigned_value();
			if (!descriptor || !count || *count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "row", "invalid-header"));
			detached_row output;
			output.descriptor_id = std::move(*descriptor);
			for (std::uint64_t index = 0U; index < *count; ++index)
			{
				auto column = reader.string();
				auto cell = decode_cell(reader);
				if (!column || !cell ||
					!output.cells.emplace(std::move(*column), std::move(*cell)).second)
					return unexpected(store_error("store.corrupt", "row", "duplicate-cell"));
			}
			return output;
		}

		void encode_strings(binary_writer& writer, const std::span<const std::string> values)
		{
			writer.unsigned_value(values.size());
			for (const auto& value : values)
				writer.string(value);
		}

		[[nodiscard]] result<std::vector<std::string>> decode_strings(binary_reader& reader,
																	  const std::string_view field)
		{
			auto count = reader.unsigned_value();
			if (!count || *count > 1'000'000U)
				return unexpected(store_error("store.corrupt", std::string{field}, "count"));
			std::vector<std::string> output;
			for (std::uint64_t index = 0U; index < *count; ++index)
			{
				auto value = reader.string();
				if (!value)
					return unexpected(std::move(value.error()));
				output.push_back(std::move(*value));
			}
			return output;
		}

		void encode_condition(binary_writer& writer, const claim_condition& condition)
		{
			writer.string(condition.universe);
			encode_strings(writer, condition.fragments);
		}

		[[nodiscard]] result<claim_condition> decode_condition(binary_reader& reader)
		{
			auto universe = reader.string();
			auto fragments = decode_strings(reader, "condition-fragments");
			if (!universe || !fragments)
				return unexpected(store_error("store.corrupt", "condition", "payload"));
			claim_condition output{std::move(*universe), std::move(*fragments)};
			if (auto valid = output.validate(); !valid)
				return unexpected(store_error("store.corrupt", "condition", valid.error().code));
			return output;
		}

		void encode_guarantee(binary_writer& writer, const claim_guarantee& guarantee)
		{
			writer.string(guarantee.approximation);
			writer.string(guarantee.scope);
			writer.string(guarantee.assumptions);
			encode_strings(writer, guarantee.verification_modalities);
		}

		[[nodiscard]] result<claim_guarantee> decode_guarantee(binary_reader& reader)
		{
			auto approximation = reader.string();
			auto scope = reader.string();
			auto assumptions = reader.string();
			auto modalities = decode_strings(reader, "guarantee-modalities");
			if (!approximation || !scope || !assumptions || !modalities)
				return unexpected(store_error("store.corrupt", "guarantee", "payload"));
			claim_guarantee output{std::move(*approximation),
								   std::move(*scope),
								   std::move(*assumptions),
								   std::move(*modalities)};
			if (auto valid = output.validate(); !valid)
				return unexpected(store_error("store.corrupt", "guarantee", valid.error().code));
			return output;
		}

		void encode_annotation(binary_writer& writer,
							   const snapshot_claim_annotation& value,
							   const bool include_producer = true)
		{
			encode_row(writer, value.row);
			encode_condition(writer, value.presence);
			writer.string(value.interpretation);
			writer.string(value.semantic_key);
			writer.string(value.assertion);
			writer.string(value.content);
			if (include_producer)
			{
				writer.string(value.producer.id);
				writer.string(value.producer.semantic_contract);
			}
			writer.string(value.provenance_root);
			encode_guarantee(writer, value.guarantee);
		}

		[[nodiscard]] std::vector<std::byte>
		annotation_projection_private(const snapshot_claim_annotation& value)
		{
			binary_writer writer;
			encode_annotation(writer, value);
			return std::move(writer).finish();
		}

		[[nodiscard]] result<snapshot_claim_annotation> decode_annotation(
			binary_reader& reader, const relation_descriptor& descriptor, const bool has_producer)
		{
			auto row = decode_row(reader);
			auto condition = decode_condition(reader);
			auto interpretation = reader.string();
			auto semantic_key = reader.string();
			auto assertion = reader.string();
			auto content = reader.string();
			claim_producer producer{
				"cxxlens.snapshot-legacy-unknown",
				"sha256:0000000000000000000000000000000000000000000000000000000000000000"};
			if (has_producer)
			{
				auto id = reader.string();
				auto contract = reader.string();
				if (!id || !contract || id->empty() || contract->empty())
					return unexpected(store_error("store.corrupt", "claim-annotation", "producer"));
				producer = {std::move(*id), std::move(*contract)};
			}
			auto provenance = reader.string();
			auto guarantee = decode_guarantee(reader);
			if (!row || !condition || !interpretation || !semantic_key || !assertion || !content ||
				!provenance || !guarantee || !validate_row(descriptor, *row) ||
				interpretation->empty() || semantic_key->empty() || assertion->empty() ||
				content->empty() || provenance->empty())
				return unexpected(store_error("store.corrupt", "claim-annotation", "validation"));
			return snapshot_claim_annotation{std::move(*row),
											 std::move(*condition),
											 std::move(*interpretation),
											 std::move(*semantic_key),
											 std::move(*assertion),
											 std::move(*content),
											 std::move(producer),
											 std::move(*provenance),
											 std::move(*guarantee)};
		}

		void encode_claim(binary_writer& writer, const claim& value)
		{
			encode_row(writer, value.row);
			writer.string(value.descriptor);
			writer.string(value.semantic_key);
			writer.string(value.assertion);
			writer.string(value.content);
			encode_condition(writer, value.presence);
			writer.string(value.interpretation);
			writer.unsigned_value(static_cast<std::uint8_t>(value.stage));
			writer.string(value.producer.id);
			writer.string(value.producer.semantic_contract);
			if (const auto* direct = std::get_if<direct_claim_basis>(&value.input_basis))
			{
				writer.boolean(true);
				writer.string(direct->basis_digest);
			}
			else
			{
				const auto& derived = std::get<derived_claim_basis>(value.input_basis);
				writer.boolean(false);
				writer.string(derived.input_snapshot);
				encode_strings(writer, derived.consumed_partition_content_digests);
				writer.string(derived.transform_semantics);
			}
			writer.string(value.provenance_root);
			encode_guarantee(writer, value.guarantee);
		}

		[[nodiscard]] result<claim> decode_claim(binary_reader& reader,
												 const relation_engine& engine)
		{
			auto row = decode_row(reader);
			auto descriptor = reader.string();
			auto semantic_key = reader.string();
			auto assertion = reader.string();
			auto content = reader.string();
			auto condition = decode_condition(reader);
			auto interpretation = reader.string();
			auto stage = reader.unsigned_value();
			auto producer_id = reader.string();
			auto producer_semantics = reader.string();
			auto direct_basis = reader.boolean();
			if (!row || !descriptor || !semantic_key || !assertion || !content || !condition ||
				!interpretation || !stage ||
				*stage > static_cast<std::uint8_t>(claim_stage::derived_claim) || !producer_id ||
				!producer_semantics || !direct_basis)
				return unexpected(store_error("store.corrupt", "partition-envelope", "claim"));
			claim_input_basis basis;
			if (*direct_basis)
			{
				auto digest_value = reader.string();
				if (!digest_value)
					return unexpected(
						store_error("store.corrupt", "partition-envelope", "direct-basis"));
				basis = direct_claim_basis{std::move(*digest_value)};
			}
			else
			{
				auto input_snapshot = reader.string();
				auto consumed = decode_strings(reader, "partition-envelope-derived-basis");
				auto transform = reader.string();
				if (!input_snapshot || !consumed || !transform)
					return unexpected(
						store_error("store.corrupt", "partition-envelope", "derived-basis"));
				basis = derived_claim_basis{
					std::move(*input_snapshot), std::move(*consumed), std::move(*transform)};
			}
			auto provenance = reader.string();
			auto guarantee = decode_guarantee(reader);
			if (!provenance || !guarantee)
				return unexpected(store_error("store.corrupt", "partition-envelope", "claim-tail"));
			claim output{std::move(*row),
						 std::move(*descriptor),
						 std::move(*semantic_key),
						 std::move(*assertion),
						 std::move(*content),
						 std::move(*condition),
						 std::move(*interpretation),
						 static_cast<claim_stage>(*stage),
						 {std::move(*producer_id), std::move(*producer_semantics)},
						 std::move(basis),
						 std::move(*provenance),
						 std::move(*guarantee)};
			if (auto valid = validate_claim(engine, output); !valid)
				return unexpected(store_error(
					"store.corrupt", "partition-envelope", std::move(valid.error().code)));
			return output;
		}

		void canonicalize_partition_envelope(partition_draft& draft)
		{
			std::ranges::sort(draft.claims, detail::claim_occurrence_less);
			std::ranges::sort(
				draft.coverage,
				[](const snapshot_coverage_unit& left, const snapshot_coverage_unit& right)
				{
					return left.canonical_form() < right.canonical_form();
				});
			std::ranges::sort(
				draft.unresolved,
				[](const unresolved_reference& left, const unresolved_reference& right)
				{
					return std::tie(left.source_assertion,
									left.source_relation,
									left.target_relation,
									left.source_columns,
									left.reason) < std::tie(right.source_assertion,
															right.source_relation,
															right.target_relation,
															right.source_columns,
															right.reason);
				});
		}

		void encode_partition_envelopes(
			binary_writer& writer,
			const std::map<std::string, partition_draft, std::less<>>& envelopes)
		{
			writer.unsigned_value(envelopes.size());
			for (const auto& [partition_id, draft] : envelopes)
			{
				if (writer.measuring() || writer.sealed_canonical())
				{
					// Canonical ordering does not change encoded length. Avoid the three temporary
					// collections during the pre-I/O bound pass.
					writer.string(partition_id);
					writer.unsigned_value(draft.claims.size());
					for (const auto& value : draft.claims)
						encode_claim(writer, value);
					writer.unsigned_value(draft.coverage.size());
					for (const auto& coverage : draft.coverage)
					{
						writer.string(coverage.domain);
						writer.string(coverage.key);
						writer.string(coverage.state);
						writer.string(coverage.reason);
					}
					writer.unsigned_value(draft.unresolved.size());
					for (const auto& unresolved : draft.unresolved)
					{
						writer.string(unresolved.source_assertion);
						writer.string(unresolved.source_relation);
						writer.string(unresolved.target_relation);
						encode_strings(writer, unresolved.source_columns);
						writer.string(unresolved.reason);
					}
					continue;
				}
				auto claims = draft.claims;
				std::ranges::sort(claims, detail::claim_occurrence_less);
				auto coverage_values = draft.coverage;
				std::ranges::sort(
					coverage_values,
					[](const snapshot_coverage_unit& left, const snapshot_coverage_unit& right)
					{
						return left.canonical_form() < right.canonical_form();
					});
				auto unresolved_values = draft.unresolved;
				std::ranges::sort(
					unresolved_values,
					[](const unresolved_reference& left, const unresolved_reference& right)
					{
						return std::tie(left.source_assertion,
										left.source_relation,
										left.target_relation,
										left.source_columns,
										left.reason) < std::tie(right.source_assertion,
																right.source_relation,
																right.target_relation,
																right.source_columns,
																right.reason);
					});
				writer.string(partition_id);
				writer.unsigned_value(claims.size());
				for (const auto& value : claims)
					encode_claim(writer, value);
				writer.unsigned_value(coverage_values.size());
				for (const auto& coverage : coverage_values)
				{
					writer.string(coverage.domain);
					writer.string(coverage.key);
					writer.string(coverage.state);
					writer.string(coverage.reason);
				}
				writer.unsigned_value(unresolved_values.size());
				for (const auto& unresolved : unresolved_values)
				{
					writer.string(unresolved.source_assertion);
					writer.string(unresolved.source_relation);
					writer.string(unresolved.target_relation);
					encode_strings(writer, unresolved.source_columns);
					writer.string(unresolved.reason);
				}
			}
		}
		[[nodiscard]] std::vector<std::byte>
		semantic_projection_bytes_private(const snapshot_handle::data& value);

		[[nodiscard]] std::string canonical_export_of_private(const snapshot_handle::data& value)
		{
			std::ostringstream output;
			output << "schema=cxxlens.snapshot-export.v1\n";
			output << "snapshot=" << value.semantic_manifest.id << '\n';
			output << "semantics=" << value.semantic_manifest.snapshot_semantics_version.string()
				   << '\n';
			output << "catalog=" << value.semantic_manifest.catalog_semantic_digest << '\n';
			output << "universe=" << value.semantic_manifest.condition_universe_id << '\n';
			output << "registry=" << value.semantic_manifest.relation_registry_digest << '\n';
			output << "interpretation-policy="
				   << value.semantic_manifest.interpretation_policy_digest << '\n';
			for (const auto& partition : value.semantic_manifest.partitions)
				output << "partition=" << partition.partition_id << '|' << partition.content_digest
					   << '|' << partition.coverage_digest << '|' << partition.claim_count << '|'
					   << (partition.complete ? "complete" : "partial") << '\n';
			for (const auto& closure : value.semantic_manifest.closure_ids)
				output << "closure=" << closure << '\n';
			for (const auto& claim : value.claim_contents)
				output << "claim=" << claim << '\n';
			for (const auto& [descriptor, rows] : value.rows)
				for (const auto& row : rows)
					output << "row=" << descriptor << '|' << row.canonical_form() << '\n';
			for (const auto& unresolved : value.unresolved)
				output << "unresolved=" << unresolved.source_assertion << '|'
					   << unresolved.source_relation << '|' << unresolved.target_relation << '|'
					   << unresolved.reason << '\n';
			output << "semantic-projection=" << bytes_hex(semantic_projection_bytes_private(value))
				   << '\n';
			binary_writer envelopes;
			encode_partition_envelopes(envelopes, value.partition_envelopes);
			output << "partition-envelopes=" << bytes_hex(std::move(envelopes).finish()) << '\n';
			return output.str();
		}

		[[nodiscard]] constexpr std::string_view
		payload_schema_magic(const snapshot_payload_schema schema) noexcept
		{
			switch (schema)
			{
				case snapshot_payload_schema::v1:
					return "cxxlens.ng-snapshot-payload.v1";
				case snapshot_payload_schema::v2:
					return "cxxlens.ng-snapshot-payload.v2";
				case snapshot_payload_schema::v3:
					return "cxxlens.ng-snapshot-payload.v3";
				case snapshot_payload_schema::v4:
					return "cxxlens.ng-snapshot-payload.v4";
				case snapshot_payload_schema::v5:
					return "cxxlens.ng-snapshot-payload.v5";
			}
			return {};
		}

		void encode_snapshot_private(binary_writer& writer,
									 const snapshot_handle::data& value,
									 const snapshot_payload_schema payload_schema)
		{
			writer.string(payload_schema_magic(payload_schema));
			const auto& manifest = value.semantic_manifest;
			writer.string(manifest.schema);
			writer.string(manifest.id);
			writer.unsigned_value(manifest.snapshot_semantics_version.major);
			writer.unsigned_value(manifest.snapshot_semantics_version.minor);
			writer.unsigned_value(manifest.snapshot_semantics_version.patch);
			writer.string(manifest.catalog_semantic_digest);
			writer.string(manifest.condition_universe_id);
			writer.string(manifest.relation_registry_digest);
			writer.string(manifest.interpretation_policy_digest);
			writer.unsigned_value(manifest.partitions.size());
			for (const auto& partition : manifest.partitions)
			{
				writer.string(partition.partition_id);
				writer.string(partition.relation_descriptor_id);
				writer.string(partition.input_basis_digest);
				writer.string(partition.claim_set_digest);
				writer.string(partition.coverage_digest);
				writer.string(partition.content_digest);
				writer.unsigned_value(partition.claim_count);
				writer.boolean(partition.complete);
			}
			writer.unsigned_value(manifest.closure_ids.size());
			for (const auto& closure : manifest.closure_ids)
				writer.string(closure);
			const auto& publication = value.publication_record_value;
			writer.string(publication.publication_id);
			writer.string(publication.series_id);
			writer.string(publication.snapshot_id);
			writer.unsigned_value(publication.sequence);
			writer.unsigned_value(publication.physical_generation);
			writer.boolean(publication.parent_publication.has_value());
			if (publication.parent_publication)
				writer.string(*publication.parent_publication);
			writer.unsigned_value(static_cast<std::uint8_t>(publication.state));
			writer.boolean(publication.corrupt);
			writer.unsigned_value(value.rows.size());
			for (const auto& [descriptor, rows] : value.rows)
			{
				writer.string(descriptor);
				writer.unsigned_value(rows.size());
				for (const auto& row : rows)
					encode_row(writer, row);
			}
			writer.unsigned_value(value.claim_contents.size());
			for (const auto& content : value.claim_contents)
				writer.string(content);
			writer.unsigned_value(value.unresolved.size());
			for (const auto& unresolved : value.unresolved)
			{
				writer.string(unresolved.source_assertion);
				writer.string(unresolved.source_relation);
				writer.string(unresolved.target_relation);
				writer.unsigned_value(unresolved.source_columns.size());
				for (const auto& column : unresolved.source_columns)
					writer.string(column);
				writer.string(unresolved.reason);
			}
			if (payload_schema >= snapshot_payload_schema::v2)
			{
				writer.boolean(value.query_annotations_available);
				writer.unsigned_value(value.annotations.size());
				for (const auto& [descriptor, annotations] : value.annotations)
				{
					writer.string(descriptor);
					writer.unsigned_value(annotations.size());
					for (const auto& annotation : annotations)
						encode_annotation(
							writer, annotation, payload_schema >= snapshot_payload_schema::v3);
				}
				writer.unsigned_value(value.coverage.size());
				for (const auto& coverage : value.coverage)
				{
					writer.string(coverage.relation_descriptor_id);
					writer.string(coverage.unit.domain);
					writer.string(coverage.unit.key);
					writer.string(coverage.unit.state);
					writer.string(coverage.unit.reason);
				}
			}
			if (payload_schema >= snapshot_payload_schema::v4)
			{
				writer.unsigned_value(value.partition_bindings.size());
				for (const auto& binding : value.partition_bindings)
				{
					writer.string(binding.partition_id);
					writer.string(binding.relation_descriptor_id);
					writer.string(binding.scope);
					encode_condition(writer, binding.condition);
					writer.string(binding.interpretation);
					writer.string(binding.producer_semantics);
					writer.string(binding.producer_input_basis_digest);
					writer.string(binding.precision_profile);
					writer.string(binding.assumption_set_id);
				}
				writer.unsigned_value(value.closure_certificates.size());
				for (const auto& certificate : value.closure_certificates)
				{
					writer.string(certificate.id);
					const auto& subject = certificate.subject;
					writer.string(subject.relation_descriptor_id);
					writer.string(subject.subject_partition_id);
					writer.string(subject.partition_content_digest);
					writer.string(subject.coverage_digest);
					writer.string(subject.key_domain_digest);
					encode_condition(writer, subject.condition);
					writer.string(subject.interpretation);
					writer.string(subject.assumption_set_id);
					writer.string(subject.closure_kind);
					writer.string(subject.producer_semantics);
					writer.string(subject.evidence_digest);
				}
			}
			if (payload_schema == snapshot_payload_schema::v5)
				encode_partition_envelopes(writer, value.partition_envelopes);
		}

		[[nodiscard]] std::vector<std::byte>
		encode_snapshot_private(const snapshot_handle::data& value)
		{
			binary_writer measured{binary_writer::measure_mode{},
								   snapshot_store_v5_maximum_payload_bytes};
			encode_snapshot_private(measured, value, snapshot_payload_schema::v5);
			if (!measured.finish_measurement())
				throw std::length_error{"snapshot v5 payload exceeds its product bound"};
			binary_writer writer;
			encode_snapshot_private(writer, value, snapshot_payload_schema::v5);
			return std::move(writer).finish();
		}

#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
		[[nodiscard]] std::vector<std::byte>
		encode_snapshot_private(const snapshot_handle::data& value,
								const snapshot_payload_schema payload_schema)
		{
			binary_writer measured{binary_writer::measure_mode{},
								   snapshot_store_v5_maximum_payload_bytes};
			encode_snapshot_private(measured, value, payload_schema);
			if (!measured.finish_measurement())
				throw std::length_error{"snapshot payload exceeds its product bound"};
			binary_writer writer;
			encode_snapshot_private(writer, value, payload_schema);
			return std::move(writer).finish();
		}
#endif

		[[nodiscard]] result<void> encode_snapshot_private(const snapshot_handle::data& value,
														   sqlite_bounded_byte_sink& sink)
		{
			binary_writer measured{binary_writer::measure_mode{},
								   snapshot_store_v5_maximum_payload_bytes};
			encode_snapshot_private(measured, value, snapshot_payload_schema::v5);
			if (auto bounded = measured.finish_measurement(); !bounded)
				return unexpected(std::move(bounded.error()));
			binary_writer writer{sink};
			encode_snapshot_private(writer, value, snapshot_payload_schema::v5);
			return writer.finish_stream();
		}

		[[nodiscard]] result<void>
		encode_sealed_snapshot_private(const snapshot_handle::data& value,
									   sqlite_bounded_byte_sink& sink)
		{
			binary_writer writer{sink, binary_writer::sealed_canonical_mode{}};
			encode_snapshot_private(writer, value, snapshot_payload_schema::v5);
			return writer.finish_stream();
		}

		[[nodiscard]] result<std::uint64_t>
		measure_snapshot_private(const snapshot_handle::data& value,
								 const std::uint64_t maximum_bytes)
		{
			binary_writer writer{binary_writer::measure_mode{}, maximum_bytes};
			encode_snapshot_private(writer, value, snapshot_payload_schema::v5);
			return writer.finish_measurement();
		}

		void sort_semantic_projections_private(snapshot_handle::data& value)
		{
			std::ranges::sort(value.claim_contents);
			value.claim_contents.erase(std::ranges::unique(value.claim_contents).begin(),
									   value.claim_contents.end());
			std::ranges::sort(
				value.coverage,
				[](const snapshot_query_coverage& left, const snapshot_query_coverage& right)
				{
					return std::tie(left.relation_descriptor_id,
									left.unit.domain,
									left.unit.key,
									left.unit.state,
									left.unit.reason) < std::tie(right.relation_descriptor_id,
																 right.unit.domain,
																 right.unit.key,
																 right.unit.state,
																 right.unit.reason);
				});
			std::ranges::sort(
				value.unresolved,
				[](const unresolved_reference& left, const unresolved_reference& right)
				{
					return std::tie(left.source_assertion,
									left.source_relation,
									left.target_relation,
									left.source_columns,
									left.reason) < std::tie(right.source_assertion,
															right.source_relation,
															right.target_relation,
															right.source_columns,
															right.reason);
				});
			std::ranges::sort(
				value.partition_bindings, {}, &snapshot_partition_binding::partition_id);
			for (auto& [descriptor, rows] : value.rows)
			{
				std::ranges::sort(rows,
								  [](const detached_row& left, const detached_row& right)
								  {
									  return left.canonical_form() < right.canonical_form();
								  });
				const auto relation = value.descriptors.find(descriptor);
				if (relation != value.descriptors.end() &&
					relation->second.merge != merge_mode::multiset)
					rows.erase(std::ranges::unique(
								   rows,
								   [](const detached_row& left, const detached_row& right)
								   {
									   return left.canonical_form() == right.canonical_form();
								   })
								   .begin(),
							   rows.end());
			}
			for (auto& [descriptor, annotations] : value.annotations)
			{
				(void)descriptor;
				std::ranges::sort(annotations,
								  [](const snapshot_claim_annotation& left,
									 const snapshot_claim_annotation& right)
								  {
									  return annotation_projection_private(left) <
										  annotation_projection_private(right);
								  });
			}
		}

		[[nodiscard]] std::vector<std::byte>
		semantic_projection_bytes_private(const snapshot_handle::data& value)
		{
			binary_writer writer;
			writer.unsigned_value(value.rows.size());
			for (const auto& [descriptor, rows] : value.rows)
			{
				writer.string(descriptor);
				writer.unsigned_value(rows.size());
				for (const auto& row : rows)
					encode_row(writer, row);
			}
			encode_strings(writer, value.claim_contents);
			writer.unsigned_value(value.unresolved.size());
			for (const auto& unresolved : value.unresolved)
			{
				writer.string(unresolved.source_assertion);
				writer.string(unresolved.source_relation);
				writer.string(unresolved.target_relation);
				encode_strings(writer, unresolved.source_columns);
				writer.string(unresolved.reason);
			}
			writer.unsigned_value(value.annotations.size());
			for (const auto& [descriptor, annotations] : value.annotations)
			{
				writer.string(descriptor);
				writer.unsigned_value(annotations.size());
				for (const auto& annotation : annotations)
					encode_annotation(writer, annotation);
			}
			writer.unsigned_value(value.coverage.size());
			for (const auto& coverage : value.coverage)
			{
				writer.string(coverage.relation_descriptor_id);
				writer.string(coverage.unit.domain);
				writer.string(coverage.unit.key);
				writer.string(coverage.unit.state);
				writer.string(coverage.unit.reason);
			}
			writer.unsigned_value(value.partition_bindings.size());
			for (const auto& binding : value.partition_bindings)
			{
				writer.string(binding.partition_id);
				writer.string(binding.relation_descriptor_id);
				writer.string(binding.scope);
				encode_condition(writer, binding.condition);
				writer.string(binding.interpretation);
				writer.string(binding.producer_semantics);
				writer.string(binding.producer_input_basis_digest);
				writer.string(binding.precision_profile);
				writer.string(binding.assumption_set_id);
			}
			return std::move(writer).finish();
		}

		[[nodiscard]] result<void> validate_semantic_graph_private(snapshot_handle::data& value,
																   const relation_engine& engine)
		{
			if (value.partition_envelopes.size() != value.semantic_manifest.partitions.size())
				return unexpected(
					store_error("store.corrupt", "partition-envelope", "manifest-count"));
			snapshot_handle::data expected;
			expected.query_annotations_available = true;
			for (const auto& partition : value.semantic_manifest.partitions)
			{
				const auto envelope = value.partition_envelopes.find(partition.partition_id);
				if (envelope == value.partition_envelopes.end())
					return unexpected(
						store_error("store.corrupt", "partition-envelope", "manifest-key"));
				auto rebuilt = make_partition_manifest(engine, envelope->second);
				if (!rebuilt || *rebuilt != partition)
					return unexpected(
						store_error("store.corrupt", "partition-envelope", "manifest"));
				expected.partition_bindings.push_back(
					partition_binding(partition.partition_id, envelope->second));
				auto relation = engine.require_id(partition.relation_descriptor_id);
				if (!relation)
					return unexpected(std::move(relation.error()));
				expected.descriptors.emplace(partition.relation_descriptor_id,
											 relation->descriptor());
				expected.rows.try_emplace(partition.relation_descriptor_id);
				expected.annotations.try_emplace(partition.relation_descriptor_id);
				for (const auto& coverage : envelope->second.coverage)
					expected.coverage.push_back({partition.relation_descriptor_id, coverage});
				for (const auto& claim_value : envelope->second.claims)
				{
					expected.rows[claim_value.descriptor].push_back(claim_value.row);
					expected.annotations[claim_value.descriptor].push_back(
						{claim_value.row,
						 claim_value.presence,
						 claim_value.interpretation,
						 claim_value.semantic_key,
						 claim_value.assertion,
						 claim_value.content,
						 claim_value.producer,
						 claim_value.provenance_root,
						 claim_value.guarantee});
					expected.claim_contents.push_back(claim_value.content);
				}
				expected.unresolved.insert(expected.unresolved.end(),
										   envelope->second.unresolved.begin(),
										   envelope->second.unresolved.end());
			}
			sort_semantic_projections_private(expected);
			sort_semantic_projections_private(value);
			if (semantic_projection_bytes_private(expected) !=
				semantic_projection_bytes_private(value))
				return unexpected(store_error("store.corrupt", "partition-envelope", "projection"));
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		decode_snapshot_private(binary_reader& reader,
								const relation_engine& engine,
								const std::optional<std::span<const std::byte>> canonical_input,
								bool* canonical_required = nullptr)
		{
			auto magic = reader.string();
			if (!magic ||
				(*magic != "cxxlens.ng-snapshot-payload.v1" &&
				 *magic != "cxxlens.ng-snapshot-payload.v2" &&
				 *magic != "cxxlens.ng-snapshot-payload.v3" &&
				 *magic != "cxxlens.ng-snapshot-payload.v4" &&
				 *magic != "cxxlens.ng-snapshot-payload.v5"))
				return unexpected(store_error("store.corrupt", "payload", "format"));
			const bool payload_has_annotations = *magic == "cxxlens.ng-snapshot-payload.v2" ||
				*magic == "cxxlens.ng-snapshot-payload.v3" ||
				*magic == "cxxlens.ng-snapshot-payload.v4" ||
				*magic == "cxxlens.ng-snapshot-payload.v5";
			const bool payload_has_producer = *magic == "cxxlens.ng-snapshot-payload.v3" ||
				*magic == "cxxlens.ng-snapshot-payload.v4" ||
				*magic == "cxxlens.ng-snapshot-payload.v5";
			const bool payload_has_closure_subjects = *magic == "cxxlens.ng-snapshot-payload.v4" ||
				*magic == "cxxlens.ng-snapshot-payload.v5";
			const bool payload_has_partition_envelopes = *magic == "cxxlens.ng-snapshot-payload.v5";
			if (canonical_required != nullptr)
				*canonical_required = payload_has_partition_envelopes;
			auto value = std::make_shared<snapshot_handle::data>();
			auto& manifest = value->semantic_manifest;
			auto schema = reader.string();
			if (!schema)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto id = reader.string();
			if (!id)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto major = reader.unsigned_32("snapshot-version-major");
			if (!major)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto minor = reader.unsigned_32("snapshot-version-minor");
			if (!minor)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto patch = reader.unsigned_32("snapshot-version-patch");
			if (!patch)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto catalog = reader.string();
			if (!catalog)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto universe = reader.string();
			if (!universe)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto registry = reader.string();
			if (!registry)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto policy = reader.string();
			if (!policy)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			auto partition_count = reader.unsigned_value();
			if (!partition_count || *partition_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			manifest.schema = std::move(*schema);
			manifest.id = std::move(*id);
			manifest.snapshot_semantics_version = {*major, *minor, *patch};
			manifest.catalog_semantic_digest = std::move(*catalog);
			manifest.condition_universe_id = std::move(*universe);
			manifest.relation_registry_digest = std::move(*registry);
			manifest.interpretation_policy_digest = std::move(*policy);
			for (std::uint64_t index = 0U; index < *partition_count; ++index)
			{
				partition_manifest partition;
				auto partition_id = reader.string();
				auto descriptor = reader.string();
				auto basis = reader.string();
				auto claims = reader.string();
				auto coverage = reader.string();
				auto content = reader.string();
				auto count = reader.unsigned_value();
				auto complete = reader.boolean();
				if (!partition_id || !descriptor || !basis || !claims || !coverage || !content ||
					!count || !complete)
					return unexpected(store_error("store.corrupt", "partition", "header"));
				partition = {std::move(*partition_id),
							 std::move(*descriptor),
							 std::move(*basis),
							 std::move(*claims),
							 std::move(*coverage),
							 std::move(*content),
							 *count,
							 *complete};
				manifest.partitions.push_back(std::move(partition));
			}
			auto closure_count = reader.unsigned_value();
			if (!closure_count || *closure_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "closures", "count"));
			for (std::uint64_t index = 0U; index < *closure_count; ++index)
			{
				auto closure = reader.string();
				if (!closure)
					return unexpected(std::move(closure.error()));
				manifest.closure_ids.push_back(std::move(*closure));
			}
			if (payload_has_partition_envelopes &&
				(!std::ranges::is_sorted(
					 manifest.partitions, {}, &partition_manifest::partition_id) ||
				 std::ranges::adjacent_find(
					 manifest.partitions, {}, &partition_manifest::partition_id) !=
					 manifest.partitions.end() ||
				 !std::ranges::is_sorted(manifest.closure_ids) ||
				 std::ranges::adjacent_find(manifest.closure_ids) != manifest.closure_ids.end()))
				return unexpected(store_error("store.corrupt", "manifest", "noncanonical-order"));
			auto& publication = value->publication_record_value;
			auto publication_id = reader.string();
			auto series = reader.string();
			auto snapshot = reader.string();
			auto sequence = reader.unsigned_value();
			auto generation = reader.unsigned_value();
			auto has_parent = reader.boolean();
			if (!publication_id || !series || !snapshot || !sequence || !generation || !has_parent)
				return unexpected(store_error("store.corrupt", "publication", "header"));
			publication.publication_id = std::move(*publication_id);
			publication.series_id = std::move(*series);
			publication.snapshot_id = std::move(*snapshot);
			publication.sequence = *sequence;
			publication.physical_generation = *generation;
			if (*has_parent)
			{
				auto parent = reader.string();
				if (!parent)
					return unexpected(std::move(parent.error()));
				publication.parent_publication = std::move(*parent);
			}
			auto state = reader.unsigned_value();
			auto corrupt = reader.boolean();
			if (!state || !corrupt ||
				*state > static_cast<std::uint8_t>(publication_state::rolled_back))
				return unexpected(store_error("store.corrupt", "publication", "state"));
			publication.state = static_cast<publication_state>(*state);
			publication.corrupt = *corrupt;
			if (auto valid = validate_publication_identity(publication); !valid)
				return unexpected(std::move(valid.error()));
			auto relation_count = reader.unsigned_value();
			if (!relation_count || *relation_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "rows", "relation-count"));
			for (std::uint64_t index = 0U; index < *relation_count; ++index)
			{
				auto descriptor_id = reader.string();
				auto row_count = reader.unsigned_value();
				if (!descriptor_id || !row_count || *row_count > 10'000'000U)
					return unexpected(store_error("store.corrupt", "rows", "header"));
				auto relation = engine.require_id(*descriptor_id);
				if (!relation)
					return unexpected(store_error("store.registry-mismatch", *descriptor_id));
				value->descriptors.emplace(*descriptor_id, relation->descriptor());
				auto& rows = value->rows[*descriptor_id];
				for (std::uint64_t row_index = 0U; row_index < *row_count; ++row_index)
				{
					auto row = decode_row(reader);
					if (!row || !validate_row(relation->descriptor(), *row))
						return unexpected(store_error("store.corrupt", "row", "validation"));
					rows.push_back(std::move(*row));
				}
			}
			auto claim_count = reader.unsigned_value();
			if (!claim_count || *claim_count > 10'000'000U)
				return unexpected(store_error("store.corrupt", "claims", "count"));
			for (std::uint64_t index = 0U; index < *claim_count; ++index)
			{
				auto claim = reader.string();
				if (!claim)
					return unexpected(std::move(claim.error()));
				value->claim_contents.push_back(std::move(*claim));
			}
			auto unresolved_count = reader.unsigned_value();
			if (!unresolved_count || *unresolved_count > 10'000'000U)
				return unexpected(store_error("store.corrupt", "unresolved", "count"));
			for (std::uint64_t index = 0U; index < *unresolved_count; ++index)
			{
				unresolved_reference unresolved;
				auto assertion = reader.string();
				auto source = reader.string();
				auto target = reader.string();
				auto columns = reader.unsigned_value();
				if (!assertion || !source || !target || !columns || *columns > 1'000'000U)
					return unexpected(store_error("store.corrupt", "unresolved", "header"));
				unresolved.source_assertion = std::move(*assertion);
				unresolved.source_relation = std::move(*source);
				unresolved.target_relation = std::move(*target);
				for (std::uint64_t column = 0U; column < *columns; ++column)
				{
					auto name = reader.string();
					if (!name)
						return unexpected(std::move(name.error()));
					unresolved.source_columns.push_back(std::move(*name));
				}
				auto reason = reader.string();
				if (!reason)
					return unexpected(std::move(reason.error()));
				unresolved.reason = std::move(*reason);
				value->unresolved.push_back(std::move(unresolved));
			}
			if (payload_has_annotations)
			{
				auto annotations_available = reader.boolean();
				auto annotation_relation_count = reader.unsigned_value();
				if (!annotations_available || !annotation_relation_count ||
					*annotation_relation_count > 1'000'000U)
					return unexpected(store_error("store.corrupt", "claim-annotations", "header"));
				value->query_annotations_available = *annotations_available;
				std::vector<std::string> annotation_contents;
				for (std::uint64_t index = 0U; index < *annotation_relation_count; ++index)
				{
					auto descriptor_id = reader.string();
					auto annotation_count = reader.unsigned_value();
					if (!descriptor_id || !annotation_count || *annotation_count > 10'000'000U)
						return unexpected(
							store_error("store.corrupt", "claim-annotations", "relation"));
					const auto descriptor = value->descriptors.find(*descriptor_id);
					if (descriptor == value->descriptors.end())
						return unexpected(store_error("store.registry-mismatch", *descriptor_id));
					auto& annotations = value->annotations[*descriptor_id];
					for (std::uint64_t annotation = 0U; annotation < *annotation_count;
						 ++annotation)
					{
						auto decoded =
							decode_annotation(reader, descriptor->second, payload_has_producer);
						if (!decoded || decoded->row.descriptor_id != *descriptor_id)
							return unexpected(
								store_error("store.corrupt", "claim-annotations", "value"));
						annotation_contents.push_back(decoded->content);
						annotations.push_back(std::move(*decoded));
					}
				}
				auto coverage_count = reader.unsigned_value();
				if (!coverage_count || *coverage_count > 10'000'000U)
					return unexpected(store_error("store.corrupt", "coverage", "count"));
				for (std::uint64_t index = 0U; index < *coverage_count; ++index)
				{
					auto descriptor = reader.string();
					auto domain = reader.string();
					auto key = reader.string();
					auto state_value = reader.string();
					auto reason = reader.string();
					if (!descriptor || !domain || !key || !state_value || !reason ||
						!value->descriptors.contains(*descriptor))
						return unexpected(store_error("store.corrupt", "coverage", "value"));
					snapshot_query_coverage coverage{std::move(*descriptor),
													 {std::move(*domain),
													  std::move(*key),
													  std::move(*state_value),
													  std::move(*reason)}};
					if (auto valid = coverage.unit.validate(); !valid)
						return unexpected(
							store_error("store.corrupt", "coverage", valid.error().code));
					value->coverage.push_back(std::move(coverage));
				}
				std::ranges::sort(annotation_contents);
				annotation_contents.erase(std::ranges::unique(annotation_contents).begin(),
										  annotation_contents.end());
				if ((!value->query_annotations_available && !annotation_contents.empty()) ||
					(value->query_annotations_available &&
					 annotation_contents != value->claim_contents))
					return unexpected(
						store_error("store.corrupt", "claim-annotations", "content-set"));
			}
			if (payload_has_closure_subjects)
			{
				auto binding_count = reader.unsigned_value();
				if (!binding_count || *binding_count != manifest.partitions.size())
					return unexpected(store_error("store.corrupt", "partition-bindings", "count"));
				for (std::uint64_t index = 0U; index < *binding_count; ++index)
				{
					auto partition_id = reader.string();
					auto descriptor = reader.string();
					auto scope = reader.string();
					auto condition = decode_condition(reader);
					auto interpretation = reader.string();
					auto producer = reader.string();
					auto basis = reader.string();
					auto precision = reader.string();
					auto assumptions = reader.string();
					if (!partition_id || !descriptor || !scope || !condition || !interpretation ||
						!producer || !basis || !precision || !assumptions)
						return unexpected(
							store_error("store.corrupt", "partition-bindings", "value"));
					snapshot_partition_binding binding{std::move(*partition_id),
													   std::move(*descriptor),
													   std::move(*scope),
													   std::move(*condition),
													   std::move(*interpretation),
													   std::move(*producer),
													   std::move(*basis),
													   std::move(*precision),
													   std::move(*assumptions)};
					const auto partition = std::ranges::find(manifest.partitions,
															 binding.partition_id,
															 &partition_manifest::partition_id);
					const auto identity = identity_draft(binding);
					if (partition == manifest.partitions.end() ||
						partition->relation_descriptor_id != binding.relation_descriptor_id ||
						partition_identity(identity) != binding.partition_id ||
						binding.scope.empty() || binding.interpretation.empty() ||
						!digest(binding.producer_semantics) ||
						!digest(binding.producer_input_basis_digest) ||
						binding.precision_profile.empty() || binding.assumption_set_id.empty())
						return unexpected(
							store_error("store.corrupt", "partition-bindings", "identity"));
					value->partition_bindings.push_back(std::move(binding));
				}
				std::ranges::sort(
					value->partition_bindings, {}, &snapshot_partition_binding::partition_id);
				if (std::ranges::adjacent_find(
						value->partition_bindings, {}, &snapshot_partition_binding::partition_id) !=
					value->partition_bindings.end())
					return unexpected(
						store_error("store.corrupt", "partition-bindings", "duplicate"));

				auto certificate_count = reader.unsigned_value();
				if (!certificate_count || *certificate_count != manifest.closure_ids.size())
					return unexpected(
						store_error("store.corrupt", "closure-certificates", "count"));
				for (std::uint64_t index = 0U; index < *certificate_count; ++index)
				{
					auto id_value = reader.string();
					auto descriptor = reader.string();
					auto partition_id = reader.string();
					auto content = reader.string();
					auto coverage = reader.string();
					auto key_domain = reader.string();
					auto condition = decode_condition(reader);
					auto interpretation = reader.string();
					auto assumptions = reader.string();
					auto kind = reader.string();
					auto producer = reader.string();
					auto evidence = reader.string();
					if (!id_value || !descriptor || !partition_id || !content || !coverage ||
						!key_domain || !condition || !interpretation || !assumptions || !kind ||
						!producer || !evidence)
						return unexpected(
							store_error("store.corrupt", "closure-certificates", "value"));
					closure_candidate subject{std::move(*descriptor),
											  std::move(*partition_id),
											  std::move(*content),
											  std::move(*coverage),
											  std::move(*key_domain),
											  std::move(*condition),
											  std::move(*interpretation),
											  std::move(*assumptions),
											  std::move(*kind),
											  std::move(*producer),
											  std::move(*evidence)};
					const auto partition = std::ranges::find(manifest.partitions,
															 subject.subject_partition_id,
															 &partition_manifest::partition_id);
					const auto binding =
						std::ranges::find(value->partition_bindings,
										  subject.subject_partition_id,
										  &snapshot_partition_binding::partition_id);
					if (partition == manifest.partitions.end() ||
						binding == value->partition_bindings.end() ||
						subject.condition != binding->condition ||
						subject.interpretation != binding->interpretation ||
						subject.assumption_set_id != binding->assumption_set_id ||
						subject.producer_semantics != binding->producer_semantics)
						return unexpected(
							store_error("store.corrupt", "closure-certificates", "binding"));
					auto validation_subject =
						make_partition_certificate_subject(*partition, *binding);
					if (!validation_subject)
						return unexpected(
							store_error("store.corrupt", "closure-certificates", "subject"));
					auto certificate =
						make_closure_certificate(*validation_subject, std::move(subject));
					if (!certificate || certificate->id != *id_value)
						return unexpected(
							store_error("store.corrupt", "closure-certificates", "identity"));
					value->closure_certificates.push_back(std::move(*certificate));
				}
				std::ranges::sort(value->closure_certificates, {}, &closure_certificate::id);
				if (std::ranges::adjacent_find(
						value->closure_certificates, {}, &closure_certificate::id) !=
						value->closure_certificates.end() ||
					!std::ranges::equal(value->closure_certificates,
										manifest.closure_ids,
										{},
										&closure_certificate::id,
										std::identity{}))
					return unexpected(
						store_error("store.corrupt", "closure-certificates", "manifest"));
			}
			if (payload_has_partition_envelopes)
			{
				auto envelope_count = reader.unsigned_value();
				if (!envelope_count || *envelope_count != manifest.partitions.size())
					return unexpected(store_error("store.corrupt", "partition-envelope", "count"));
				for (std::uint64_t index = 0U; index < *envelope_count; ++index)
				{
					auto partition_id = reader.string();
					auto claim_count_value = reader.unsigned_value();
					if (!partition_id || !claim_count_value || *claim_count_value > 10'000'000U)
						return unexpected(
							store_error("store.corrupt", "partition-envelope", "header"));
					const auto binding =
						std::ranges::find(value->partition_bindings,
										  *partition_id,
										  &snapshot_partition_binding::partition_id);
					if (binding == value->partition_bindings.end())
						return unexpected(
							store_error("store.corrupt", "partition-envelope", "binding"));
					auto draft = identity_draft(*binding);
					for (std::uint64_t claim_index = 0U; claim_index < *claim_count_value;
						 ++claim_index)
					{
						auto claim_value = decode_claim(reader, engine);
						if (!claim_value)
							return unexpected(std::move(claim_value.error()));
						draft.claims.push_back(std::move(*claim_value));
					}
					auto coverage_count_value = reader.unsigned_value();
					if (!coverage_count_value || *coverage_count_value > 10'000'000U)
						return unexpected(
							store_error("store.corrupt", "partition-envelope", "coverage-count"));
					for (std::uint64_t coverage_index = 0U; coverage_index < *coverage_count_value;
						 ++coverage_index)
					{
						auto domain = reader.string();
						auto key = reader.string();
						auto coverage_state = reader.string();
						auto reason = reader.string();
						if (!domain || !key || !coverage_state || !reason)
							return unexpected(
								store_error("store.corrupt", "partition-envelope", "coverage"));
						draft.coverage.push_back({std::move(*domain),
												  std::move(*key),
												  std::move(*coverage_state),
												  std::move(*reason)});
					}
					auto unresolved_count_value = reader.unsigned_value();
					if (!unresolved_count_value || *unresolved_count_value > 10'000'000U)
						return unexpected(
							store_error("store.corrupt", "partition-envelope", "unresolved-count"));
					for (std::uint64_t unresolved_index = 0U;
						 unresolved_index < *unresolved_count_value;
						 ++unresolved_index)
					{
						auto assertion = reader.string();
						auto source = reader.string();
						auto target = reader.string();
						auto columns = decode_strings(reader, "partition-envelope-unresolved");
						auto reason = reader.string();
						if (!assertion || !source || !target || !columns || !reason)
							return unexpected(
								store_error("store.corrupt", "partition-envelope", "unresolved"));
						draft.unresolved.push_back({std::move(*assertion),
													std::move(*source),
													std::move(*target),
													std::move(*columns),
													std::move(*reason)});
					}
					if (!value->partition_envelopes.emplace(*partition_id, std::move(draft)).second)
						return unexpected(
							store_error("store.corrupt", "partition-envelope", "duplicate"));
				}
				if (auto valid = validate_semantic_graph_private(*value, engine); !valid)
					return unexpected(std::move(valid.error()));
			}
			auto finished = reader.finished();
			if (!finished || !*finished || manifest.schema != "cxxlens.snapshot-manifest.v1" ||
				manifest.id != snapshot_identity(manifest) ||
				publication.snapshot_id != manifest.id)
				return unexpected(store_error("store.corrupt", "payload", "semantic-digest"));
			if (payload_has_partition_envelopes && canonical_input)
			{
				const auto canonical = encode_snapshot_private(*value);
				if (!std::ranges::equal(*canonical_input, canonical))
					return unexpected(store_error("store.corrupt", "payload", "noncanonical"));
			}
			return value;
		}

		class sqlite_byte_comparison_sink final : public sqlite_bounded_byte_sink
		{
		  public:
			sqlite_byte_comparison_sink(std::unique_ptr<sqlite_bounded_byte_source> source,
										const std::uint64_t expected_size) noexcept
				: source_{std::move(source)}, expected_size_{expected_size}
			{
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
			{
				if (!source_ || offset_ > expected_size_ || bytes.size() > expected_size_ - offset_)
					return unexpected(store_error("store.corrupt", "payload", "noncanonical-size"));
				std::size_t compared{};
				while (compared < bytes.size())
				{
					const auto count = std::min(scratch_.size(), bytes.size() - compared);
					std::size_t copied{};
					while (copied < count)
					{
						auto read = source_->read(std::span{scratch_}.first(count).subspan(copied));
						if (!read)
							return unexpected(std::move(read.error()));
						if (*read == 0U || *read > count - copied)
							return unexpected(
								store_error("store.corrupt", "payload", "noncanonical-size"));
						copied += *read;
					}
					if (!std::ranges::equal(std::span{scratch_}.first(count),
											bytes.subspan(compared, count)))
						return unexpected(store_error("store.corrupt", "payload", "noncanonical"));
					compared += count;
					offset_ += count;
				}
				return {};
			}

			[[nodiscard]] result<void> finish()
			{
				if (!source_ || offset_ != expected_size_)
					return unexpected(store_error("store.corrupt", "payload", "noncanonical-size"));
				std::array<std::byte, 1U> extra{};
				auto read = source_->read(extra);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read != 0U)
					return unexpected(store_error("store.corrupt", "payload", "noncanonical-size"));
				return {};
			}

		  private:
			std::unique_ptr<sqlite_bounded_byte_source> source_;
			std::array<std::byte, std::size_t{64U} * 1024U> scratch_{};
			std::uint64_t expected_size_{};
			std::uint64_t offset_{};
		};

		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		decode_snapshot_private(const sqlite_replayable_byte_source& source,
								const std::uint64_t expected_size,
								const relation_engine& engine)
		{
			auto first_pass = source.open_pass();
			if (!first_pass)
				return unexpected(std::move(first_pass.error()));
			if (!*first_pass)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-decode", "null-replay-pass"));
			binary_reader reader{**first_pass, expected_size};
			bool canonical_required{};
			auto value = decode_snapshot_private(reader, engine, std::nullopt, &canonical_required);
			if (!value)
				return unexpected(std::move(value.error()));
			if (!canonical_required)
				return value;

			auto comparison_pass = source.open_pass();
			if (!comparison_pass)
				return unexpected(std::move(comparison_pass.error()));
			if (!*comparison_pass)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-decode", "null-replay-pass"));
			sqlite_byte_comparison_sink comparison{std::move(*comparison_pass), expected_size};
			if (auto encoded = encode_snapshot_private(**value, comparison); !encoded)
				return unexpected(std::move(encoded.error()));
			if (auto compared = comparison.finish(); !compared)
				return unexpected(std::move(compared.error()));
			return value;
		}

		class snapshot_store_v5_hashing_sink final : public sqlite_bounded_byte_sink
		{
		  public:
			snapshot_store_v5_hashing_sink(sqlite_bounded_byte_sink& sink,
										   const std::uint64_t expected_bytes) noexcept
				: sink_{sink}, expected_bytes_{expected_bytes}
			{
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
			{
				if (offset_ > expected_bytes_ || bytes.size() > expected_bytes_ - offset_)
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-staging", "measured-size"));
				if (auto hashed = digest_.update(bytes); !hashed)
					return unexpected(std::move(hashed.error()));
				if (auto appended = sink_.append(bytes); !appended)
					return unexpected(std::move(appended.error()));
				offset_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

			[[nodiscard]] result<std::string> finish()
			{
				if (offset_ != expected_bytes_)
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-staging", "measured-size"));
				return digest_.finish();
			}

		  private:
			sqlite_bounded_byte_sink& sink_;
			sqlite_incremental_sha256 digest_;
			std::uint64_t expected_bytes_{};
			std::uint64_t offset_{};
		};

		[[nodiscard]] result<void>
		compare_snapshot_store_v5_source(const sqlite_replayable_byte_source& source,
										 const snapshot_handle::data& graph,
										 const std::uint64_t expected_bytes)
		{
			auto pass = source.open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			if (!*pass)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-staging", "null-replay-pass"));
			sqlite_byte_comparison_sink comparison{std::move(*pass), expected_bytes};
			if (auto encoded = encode_sealed_snapshot_private(graph, comparison); !encoded)
				return unexpected(std::move(encoded.error()));
			return comparison.finish();
		}

	} // namespace

	class snapshot_store_v5_reference_generation_binding final
	{
	  public:
		snapshot_store_v5_reference_generation_binding(
			const snapshot_store_v5_reference_generation_binding&) = delete;
		snapshot_store_v5_reference_generation_binding&
		operator=(const snapshot_store_v5_reference_generation_binding&) = delete;
		snapshot_store_v5_reference_generation_binding(
			snapshot_store_v5_reference_generation_binding&&) noexcept = default;
		snapshot_store_v5_reference_generation_binding&
		operator=(snapshot_store_v5_reference_generation_binding&&) noexcept = default;

		[[nodiscard]] static result<snapshot_store_v5_reference_generation_binding>
		seal(std::shared_ptr<const std::uint64_t> lifetime_pin,
			 const std::uint64_t expected_generation)
		{
			if (!lifetime_pin || expected_generation == 0U || *lifetime_pin != expected_generation)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "generation-custody"));
			return snapshot_store_v5_reference_generation_binding{std::move(lifetime_pin),
																  expected_generation};
		}

		[[nodiscard]] bool holds(const std::uint64_t generation) const noexcept
		{
			return lifetime_pin_ && generation_ == generation && *lifetime_pin_ == generation;
		}

	  private:
		snapshot_store_v5_reference_generation_binding(
			std::shared_ptr<const std::uint64_t> lifetime_pin,
			const std::uint64_t generation) noexcept
			: lifetime_pin_{std::move(lifetime_pin)}, generation_{generation}
		{
		}

		std::shared_ptr<const std::uint64_t> lifetime_pin_;
		std::uint64_t generation_{};
	};

	struct snapshot_store_v5_reference_authority final
	{
		snapshot_store_v5_reference_generation_binding generation;
		std::string snapshot_id;
		std::string publication_id;
		std::string series_id;
		std::uint64_t sequence{};
		std::uint64_t physical_generation{};
	};

	struct snapshot_store_v5_graph_binding::state
	{
		std::unique_ptr<snapshot_handle::data> graph;
		std::shared_ptr<const snapshot_store_v5_reference_authority> authority;
	};

	struct snapshot_store_v5_measurement::state
	{
		std::unique_ptr<snapshot_handle::data> graph;
		std::shared_ptr<const snapshot_store_v5_reference_authority> authority;
		std::uint64_t byte_count{};
		std::uint64_t maximum_bytes{};
	};

	struct snapshot_store_v5_staging_binding::state
	{
		std::shared_ptr<const sqlite_replayable_byte_source> source;
		std::shared_ptr<const snapshot_store_v5_reference_authority> authority;
		snapshot_store_v5_staging_observation observation;
	};

	struct snapshot_store_v5_authenticated_cursor::state
	{
		// Keep the sealed backing alive until the forward pass closes. The member order makes the
		// pass close before the backing source releases its physical staging custody.
		std::shared_ptr<const sqlite_replayable_byte_source> replay_source;
		std::unique_ptr<sqlite_bounded_byte_source> source;
		std::shared_ptr<const snapshot_store_v5_reference_authority> authority;
		snapshot_store_v5_staging_observation observation;
		sqlite_incremental_sha256 digest;
		std::uint64_t offset{};
		bool eof_observed{};
		bool finished{};
	};

	snapshot_store_v5_graph_binding::snapshot_store_v5_graph_binding(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	snapshot_store_v5_graph_binding::snapshot_store_v5_graph_binding(
		snapshot_store_v5_graph_binding&&) noexcept = default;
	snapshot_store_v5_graph_binding& snapshot_store_v5_graph_binding::operator=(
		snapshot_store_v5_graph_binding&&) noexcept = default;
	snapshot_store_v5_graph_binding::~snapshot_store_v5_graph_binding() = default;

	snapshot_store_v5_measurement::snapshot_store_v5_measurement(std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	snapshot_store_v5_measurement::snapshot_store_v5_measurement(
		snapshot_store_v5_measurement&&) noexcept = default;
	snapshot_store_v5_measurement&
	snapshot_store_v5_measurement::operator=(snapshot_store_v5_measurement&&) noexcept = default;
	snapshot_store_v5_measurement::~snapshot_store_v5_measurement() = default;
	result<void> snapshot_store_v5_staging_sink::discard()
	{
		if (discard_attempted_)
			return unexpected(
				store_error("store.invariant-breach", "snapshot-v5-staging", "discard-replayed"));
		discard_attempted_ = true;
		return discard_staging();
	}
	std::uint64_t snapshot_store_v5_measurement::byte_count() const noexcept
	{
		return state_ ? state_->byte_count : 0U;
	}
	std::uint64_t snapshot_store_v5_measurement::maximum_bytes() const noexcept
	{
		return state_ ? state_->maximum_bytes : 0U;
	}

	snapshot_store_v5_staging_binding::snapshot_store_v5_staging_binding(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	snapshot_store_v5_staging_binding::snapshot_store_v5_staging_binding(
		snapshot_store_v5_staging_binding&&) noexcept = default;
	snapshot_store_v5_staging_binding& snapshot_store_v5_staging_binding::operator=(
		snapshot_store_v5_staging_binding&&) noexcept = default;
	snapshot_store_v5_staging_binding::~snapshot_store_v5_staging_binding() = default;
	const snapshot_store_v5_staging_observation&
	snapshot_store_v5_staging_binding::observation() const noexcept
	{
		static const snapshot_store_v5_staging_observation empty;
		return state_ ? state_->observation : empty;
	}

	snapshot_store_v5_authenticated_cursor::snapshot_store_v5_authenticated_cursor(
		std::unique_ptr<state> state)
		: state_{std::move(state)}
	{
	}
	snapshot_store_v5_authenticated_cursor::snapshot_store_v5_authenticated_cursor(
		snapshot_store_v5_authenticated_cursor&&) noexcept = default;
	snapshot_store_v5_authenticated_cursor& snapshot_store_v5_authenticated_cursor::operator=(
		snapshot_store_v5_authenticated_cursor&&) noexcept = default;
	snapshot_store_v5_authenticated_cursor::~snapshot_store_v5_authenticated_cursor() = default;

	result<std::size_t>
	snapshot_store_v5_authenticated_cursor::read(const std::span<std::byte> output)
	{
		try
		{
			if (!state_ || !state_->source || !state_->authority || state_->finished)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-cursor", "moved-or-finished"));
			if (output.empty())
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(
					store_error("store.invariant-breach", "snapshot-v5-cursor", "empty-window"));
			}
			if (state_->eof_observed)
				return std::size_t{};

			if (state_->offset > state_->observation.byte_count)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-size"));
			}
			const auto remaining = state_->observation.byte_count - state_->offset;
			if (remaining == 0U)
			{
				auto read = state_->source->read(output.first(1U));
				if (!read)
				{
					state_->finished = true;
					state_->source.reset();
					state_->replay_source.reset();
					return unexpected(std::move(read.error()));
				}
				if (*read > 1U)
				{
					state_->finished = true;
					state_->source.reset();
					state_->replay_source.reset();
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-staging", "cursor-window"));
				}
				if (*read == 0U)
				{
					state_->eof_observed = true;
					return std::size_t{};
				}
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-size"));
			}
			const auto bounded_window = output.first(
				static_cast<std::size_t>(std::min<std::uint64_t>(remaining, output.size())));
			auto read = state_->source->read(bounded_window);
			if (!read)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(std::move(read.error()));
			}
			if (*read > bounded_window.size())
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-window"));
			}
			if (*read == 0U)
			{
				state_->eof_observed = true;
				return std::size_t{};
			}
			const auto count = static_cast<std::uint64_t>(*read);
			if (count > remaining)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-size"));
			}
			if (auto hashed = state_->digest.update(output.first(*read)); !hashed)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
				return unexpected(std::move(hashed.error()));
			}
			state_->offset += count;
			return *read;
		}
		catch (const std::bad_alloc&)
		{
			if (state_)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
			}
			return unexpected(error{"store.allocation-failure", "snapshot-v5-cursor", "read"});
		}
		catch (...)
		{
			if (state_)
			{
				state_->finished = true;
				state_->source.reset();
				state_->replay_source.reset();
			}
			return unexpected(
				error{"store.invariant-breach", "snapshot-v5-cursor", "read-exception"});
		}
	}

	result<snapshot_store_v5_staging_observation> snapshot_store_v5_authenticated_cursor::finish()
	{
		try
		{
			auto cursor_state = std::move(state_);
			if (!cursor_state || !cursor_state->source || !cursor_state->authority ||
				cursor_state->finished)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-cursor", "moved-or-finished"));
			cursor_state->finished = true;
			const auto& authority = *cursor_state->authority;
			const auto& observation = cursor_state->observation;
			if (!authority.generation.holds(observation.physical_generation) ||
				authority.snapshot_id != observation.snapshot_id ||
				authority.publication_id != observation.publication_id ||
				authority.series_id != observation.series_id ||
				authority.sequence != observation.sequence ||
				authority.physical_generation != observation.physical_generation)
				return unexpected(store_error("store.corrupt", "snapshot-v5-binding", "authority"));
			if (cursor_state->offset != cursor_state->observation.byte_count)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-size"));
			if (!cursor_state->eof_observed)
			{
				std::array<std::byte, 1U> extra{};
				auto read = cursor_state->source->read(extra);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read != 0U)
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-staging", "cursor-size"));
			}
			auto digest = cursor_state->digest.finish();
			if (!digest)
				return unexpected(std::move(digest.error()));
			if (*digest != cursor_state->observation.payload_sha256)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-staging", "cursor-digest"));
			return std::move(cursor_state->observation);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "snapshot-v5-cursor", "finish"});
		}
		catch (...)
		{
			return unexpected(
				error{"store.invariant-breach", "snapshot-v5-cursor", "finish-exception"});
		}
	}

	result<snapshot_store_v5_graph_binding>
	snapshot_store_v5_graph_binding::seal_reference(std::unique_ptr<snapshot_handle::data> graph,
													const relation_engine& engine)
	{
		try
		{
			if (!graph)
				return unexpected(
					store_error("store.invariant-breach", "snapshot-v5-binding", "null-graph"));
			if (graph->semantic_manifest.schema != "cxxlens.snapshot-manifest.v1")
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "manifest-schema"));
			auto snapshot = snapshot_manifest_identity(graph->semantic_manifest);
			if (!snapshot || *snapshot != graph->semantic_manifest.id)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "snapshot-identity"));
			const auto& publication = graph->publication_record_value;
			auto publication_id = publication_record_identity(publication.series_id,
															  publication.snapshot_id,
															  publication.sequence,
															  publication.parent_publication);
			if (!publication_id || publication.publication_id != *publication_id ||
				publication.snapshot_id != graph->semantic_manifest.id ||
				publication.state != publication_state::committed || publication.corrupt)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "publication-identity"));
			if (publication.sequence == 0U || publication.physical_generation == 0U)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "generation-custody"));
			auto generation = snapshot_store_v5_reference_generation_binding::seal(
				graph->generation_pin, publication.physical_generation);
			if (!generation)
				return unexpected(std::move(generation.error()));
			if (!graph->query_annotations_available)
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "query-annotations"));
			if (!std::ranges::is_sorted(
					graph->semantic_manifest.partitions, {}, &partition_manifest::partition_id) ||
				std::ranges::adjacent_find(
					graph->semantic_manifest.partitions, {}, &partition_manifest::partition_id) !=
					graph->semantic_manifest.partitions.end() ||
				!std::ranges::is_sorted(graph->semantic_manifest.closure_ids) ||
				std::ranges::adjacent_find(graph->semantic_manifest.closure_ids) !=
					graph->semantic_manifest.closure_ids.end() ||
				!std::ranges::is_sorted(
					graph->closure_certificates, {}, &closure_certificate::id) ||
				std::ranges::adjacent_find(
					graph->closure_certificates, {}, &closure_certificate::id) !=
					graph->closure_certificates.end())
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "noncanonical-order"));
			if (auto valid = validate_semantic_graph_private(*graph, engine); !valid)
				return unexpected(std::move(valid.error()));
			if (graph->closure_certificates.size() != graph->semantic_manifest.closure_ids.size() ||
				!std::ranges::equal(graph->closure_certificates,
									graph->semantic_manifest.closure_ids,
									{},
									&closure_certificate::id,
									std::identity{}))
				return unexpected(
					store_error("store.corrupt", "snapshot-v5-binding", "closure-manifest"));
			for (const auto& certificate : graph->closure_certificates)
			{
				const auto partition = std::ranges::find(graph->semantic_manifest.partitions,
														 certificate.subject.subject_partition_id,
														 &partition_manifest::partition_id);
				const auto binding = std::ranges::find(graph->partition_bindings,
													   certificate.subject.subject_partition_id,
													   &snapshot_partition_binding::partition_id);
				if (partition == graph->semantic_manifest.partitions.end() ||
					binding == graph->partition_bindings.end())
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-binding", "closure-subject"));
				auto subject = make_partition_certificate_subject(*partition, *binding);
				if (!subject)
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-binding", "closure-subject"));
				auto rebuilt = make_closure_certificate(*subject, certificate.subject);
				if (!rebuilt || *rebuilt != certificate)
					return unexpected(
						store_error("store.corrupt", "snapshot-v5-binding", "closure-identity"));
			}
			for (auto& [partition_id, envelope] : graph->partition_envelopes)
			{
				(void)partition_id;
				canonicalize_partition_envelope(envelope);
			}
			auto state = std::make_unique<snapshot_store_v5_graph_binding::state>();
			state->graph = std::move(graph);
			state->authority = std::make_shared<const snapshot_store_v5_reference_authority>(
				snapshot_store_v5_reference_authority{std::move(*generation),
													  state->graph->semantic_manifest.id,
													  publication.publication_id,
													  publication.series_id,
													  publication.sequence,
													  publication.physical_generation});
			return snapshot_store_v5_graph_binding{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "snapshot-v5-binding", "seal"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-binding", "exception"});
		}
	}

	result<snapshot_store_v5_measurement>
	snapshot_store_v5_graph_binding::measure(const std::uint64_t maximum_bytes) &&
	{
		try
		{
			auto binding_state = std::move(state_);
			if (!binding_state || !binding_state->graph || !binding_state->authority)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-measure", "moved-or-replayed"));
			if (maximum_bytes == 0U || maximum_bytes > snapshot_store_v5_maximum_payload_bytes)
				return unexpected(
					store_error("store.resource-limit", "snapshot-payload", "maximum-bytes"));
			auto measured = measure_snapshot_private(*binding_state->graph, maximum_bytes);
			if (!measured)
				return unexpected(std::move(measured.error()));
			auto state = std::make_unique<snapshot_store_v5_measurement::state>();
			state->graph = std::move(binding_state->graph);
			state->authority = std::move(binding_state->authority);
			state->byte_count = *measured;
			state->maximum_bytes = maximum_bytes;
			return snapshot_store_v5_measurement{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "snapshot-v5-measure", "measure"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-measure", "exception"});
		}
	}

	result<snapshot_store_v5_staging_binding>
	snapshot_store_v5_measurement::stream(std::unique_ptr<snapshot_store_v5_staging_sink> sink) &&
	{
		bool staging_started{};
		auto abort = [&](error failure) -> result<snapshot_store_v5_staging_binding>
		{
			if (!staging_started || !sink)
				return unexpected(std::move(failure));
			try
			{
				auto discarded = sink->discard();
				sink.reset();
				staging_started = false;
				if (!discarded)
					return unexpected(std::move(discarded.error()));
				return unexpected(std::move(failure));
			}
			catch (const std::bad_alloc&)
			{
				sink.reset();
				staging_started = false;
				return unexpected(
					error{"store.cleanup-failed", "snapshot-v5-staging", "discard-allocation"});
			}
			catch (...)
			{
				sink.reset();
				staging_started = false;
				return unexpected(
					error{"store.cleanup-failed", "snapshot-v5-staging", "discard-exception"});
			}
		};

		try
		{
			auto measurement_state = std::move(state_);
			if (!measurement_state || !measurement_state->graph || !measurement_state->authority)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-stream", "moved-or-replayed"));
			if (!sink)
				return unexpected(
					store_error("store.invariant-breach", "snapshot-v5-stream", "null-sink"));
			staging_started = true;
			snapshot_store_v5_hashing_sink hashing{*sink, measurement_state->byte_count};
			if (auto encoded = encode_sealed_snapshot_private(*measurement_state->graph, hashing);
				!encoded)
				return abort(std::move(encoded.error()));
			auto emitted_digest = hashing.finish();
			if (!emitted_digest)
				return abort(std::move(emitted_digest.error()));
			auto source = sink->seal();
			if (!source)
				return abort(std::move(source.error()));
			if (!*source)
				return abort(store_error(
					"store.invariant-breach", "snapshot-v5-stream", "null-sealed-source"));
			staging_started = false;
			sink.reset();
			if (auto compared = compare_snapshot_store_v5_source(
					**source, *measurement_state->graph, measurement_state->byte_count);
				!compared)
				return unexpected(std::move(compared.error()));

			const auto& authority = *measurement_state->authority;
			snapshot_store_v5_staging_observation observation{measurement_state->byte_count,
															  std::move(*emitted_digest),
															  authority.snapshot_id,
															  authority.publication_id,
															  authority.series_id,
															  authority.sequence,
															  authority.physical_generation};
			auto state = std::make_unique<snapshot_store_v5_staging_binding::state>();
			state->source = std::move(*source);
			state->authority = std::move(measurement_state->authority);
			state->observation = std::move(observation);
			return snapshot_store_v5_staging_binding{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return abort(error{"store.allocation-failure", "snapshot-v5-stream", "stream"});
		}
		catch (...)
		{
			return abort(error{"store.invariant-breach", "snapshot-v5-stream", "exception"});
		}
	}

	result<snapshot_store_v5_authenticated_cursor>
	snapshot_store_v5_staging_binding::take_cursor() &&
	{
		try
		{
			auto binding_state = std::move(state_);
			if (!binding_state || !binding_state->source || !binding_state->authority)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-cursor", "moved-or-replayed"));
			const auto& authority = *binding_state->authority;
			const auto& observation = binding_state->observation;
			if (!authority.generation.holds(observation.physical_generation) ||
				authority.snapshot_id != observation.snapshot_id ||
				authority.publication_id != observation.publication_id ||
				authority.series_id != observation.series_id ||
				authority.sequence != observation.sequence ||
				authority.physical_generation != observation.physical_generation)
				return unexpected(store_error("store.corrupt", "snapshot-v5-binding", "authority"));
			auto pass = binding_state->source->open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			if (!*pass)
				return unexpected(store_error(
					"store.invariant-breach", "snapshot-v5-cursor", "null-replay-pass"));
			auto state = std::make_unique<snapshot_store_v5_authenticated_cursor::state>();
			state->replay_source = std::move(binding_state->source);
			state->source = std::move(*pass);
			state->authority = std::move(binding_state->authority);
			state->observation = std::move(binding_state->observation);
			return snapshot_store_v5_authenticated_cursor{std::move(state)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "snapshot-v5-cursor", "open"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-cursor", "exception"});
		}
	}

	result<snapshot_store_v5_graph_binding>
	seal_snapshot_store_v5_reference_graph(std::unique_ptr<snapshot_handle::data> graph,
										   const relation_engine& engine)
	{
		return snapshot_store_v5_graph_binding::seal_reference(std::move(graph), engine);
	}

	result<snapshot_store_v5_measurement>
	measure_snapshot_store_v5(snapshot_store_v5_graph_binding&& graph,
							  const std::uint64_t maximum_bytes)
	{
		return std::move(graph).measure(maximum_bytes);
	}

	result<snapshot_store_v5_staging_binding>
	stream_snapshot_store_v5(snapshot_store_v5_measurement&& measurement,
							 std::unique_ptr<snapshot_store_v5_staging_sink> sink)
	{
		return std::move(measurement).stream(std::move(sink));
	}

	result<snapshot_store_v5_authenticated_cursor>
	take_snapshot_store_v5_cursor(snapshot_store_v5_staging_binding&& binding)
	{
		return std::move(binding).take_cursor();
	}

	result<std::vector<std::byte>> encode_store_claim(const claim& value)
	{
		try
		{
			binary_writer writer;
			encode_claim(writer, value);
			return std::move(writer).finish();
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "claim-codec", "encode"});
		}
	}

	result<claim> decode_store_claim(const std::span<const std::byte> bytes,
									 const relation_engine& engine)
	{
		try
		{
			binary_reader reader{bytes};
			auto value = decode_claim(reader, engine);
			if (!value)
				return unexpected(std::move(value.error()));
			auto finished = reader.finished();
			if (!finished)
				return unexpected(finished ? error{"store.corrupt", "claim-codec", "trailing"}
										   : std::move(finished.error()));
			if (!*finished)
				return unexpected(error{"store.corrupt", "claim-codec", "trailing"});
			return std::move(*value);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(error{"store.allocation-failure", "claim-codec", "decode"});
		}
	}

#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
	std::optional<snapshot_payload_schema>
	payload_schema_from_number(const std::uint8_t number) noexcept
	{
		switch (number)
		{
			case 1U:
				return snapshot_payload_schema::v1;
			case 2U:
				return snapshot_payload_schema::v2;
			case 3U:
				return snapshot_payload_schema::v3;
			case 4U:
				return snapshot_payload_schema::v4;
			case 5U:
				return snapshot_payload_schema::v5;
			default:
				return std::nullopt;
		}
	}

	result<std::vector<std::byte>> encode_snapshot(const snapshot_handle::data& value,
												   const snapshot_payload_schema payload_schema)
	{
		try
		{
			return encode_snapshot_private(value, payload_schema);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-encode", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-encode", "exception"});
		}
	}
#endif

	result<std::vector<std::byte>> encode_snapshot(const snapshot_handle::data& value)
	{
		try
		{
			return encode_snapshot_private(value);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-encode", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-encode", "exception"});
		}
	}

	result<void> encode_snapshot(const snapshot_handle::data& value, sqlite_bounded_byte_sink& sink)
	{
		try
		{
			return encode_snapshot_private(value, sink);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-encode", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-encode", "exception"});
		}
	}

	std::string canonical_export_of(const snapshot_handle::data& value)
	{
		return canonical_export_of_private(value);
	}

	std::vector<std::byte> semantic_projection_bytes(const snapshot_handle::data& value)
	{
		return semantic_projection_bytes_private(value);
	}

	std::vector<std::byte> annotation_projection(const snapshot_claim_annotation& value)
	{
		return annotation_projection_private(value);
	}

	void sort_semantic_projections(snapshot_handle::data& value)
	{
		sort_semantic_projections_private(value);
	}

	result<void> validate_semantic_graph(snapshot_handle::data& value,
										 const relation_engine& engine)
	{
		try
		{
			return validate_semantic_graph_private(value, engine);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-validation", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(
				error{"store.invariant-breach", "snapshot-v5-validation", "exception"});
		}
	}

	result<std::shared_ptr<snapshot_handle::data>>
	decode_snapshot(const sqlite_replayable_byte_source& source,
					const std::uint64_t expected_size,
					const relation_engine& engine)
	{
		if (expected_size > snapshot_store_v5_maximum_payload_bytes)
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		try
		{
			return decode_snapshot_private(source, expected_size, engine);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-decode", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(error{"store.invariant-breach", "snapshot-v5-decode", "exception"});
		}
	}

	namespace
	{
		[[nodiscard]] result<std::size_t>
		payload_generation_offset_private(binary_reader& reader,
										  const std::uint64_t expected_generation)
		{
			auto magic = reader.string();
			if (!magic ||
				(*magic != "cxxlens.ng-snapshot-payload.v1" &&
				 *magic != "cxxlens.ng-snapshot-payload.v2" &&
				 *magic != "cxxlens.ng-snapshot-payload.v3" &&
				 *magic != "cxxlens.ng-snapshot-payload.v4" &&
				 *magic != "cxxlens.ng-snapshot-payload.v5"))
				return unexpected(store_error("store.corrupt", "payload", "format"));
			auto schema = reader.string();
			auto snapshot_id = reader.string();
			auto major = reader.unsigned_value();
			auto minor = reader.unsigned_value();
			auto patch = reader.unsigned_value();
			auto catalog = reader.string();
			auto universe = reader.string();
			auto registry = reader.string();
			auto policy = reader.string();
			auto partition_count = reader.unsigned_value();
			if (!schema || !snapshot_id || !major || !minor || !patch || !catalog || !universe ||
				!registry || !policy || !partition_count || *partition_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "payload", "manifest-header"));
			for (std::uint64_t index = 0U; index < *partition_count; ++index)
			{
				auto partition_id = reader.string();
				auto descriptor = reader.string();
				auto basis = reader.string();
				auto claims = reader.string();
				auto coverage = reader.string();
				auto content = reader.string();
				auto claim_count = reader.unsigned_value();
				auto complete = reader.boolean();
				if (!partition_id || !descriptor || !basis || !claims || !coverage || !content ||
					!claim_count || !complete)
					return unexpected(store_error("store.corrupt", "payload", "partition-header"));
			}
			auto closure_count = reader.unsigned_value();
			if (!closure_count || *closure_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "payload", "closure-count"));
			for (std::uint64_t index = 0U; index < *closure_count; ++index)
				if (auto closure = reader.string(); !closure)
					return unexpected(std::move(closure.error()));
			auto publication_id = reader.string();
			auto series_id = reader.string();
			auto publication_snapshot_id = reader.string();
			auto sequence = reader.unsigned_value();
			if (!publication_id || !series_id || !publication_snapshot_id || !sequence)
				return unexpected(store_error("store.corrupt", "payload", "publication-header"));
			const auto generation_offset = reader.offset();
			auto stored_generation = reader.unsigned_value();
			if (!stored_generation || *stored_generation != expected_generation)
				return unexpected(store_error("store.corrupt", "payload", "generation-mismatch"));
			return generation_offset;
		}

	} // namespace

	result<std::size_t> payload_generation_offset(const sqlite_replayable_byte_source& source,
												  const std::uint64_t expected_size,
												  const std::uint64_t expected_generation)
	{
		if (expected_size > snapshot_store_v5_maximum_payload_bytes)
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		try
		{
			auto pass = source.open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			if (!*pass)
				return unexpected(error{
					"store.invariant-breach", "snapshot-v5-generation-offset", "null-replay-pass"});
			binary_reader reader{**pass, expected_size};
			return payload_generation_offset_private(reader, expected_generation);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(
				error{"store.allocation-failure", "snapshot-v5-generation-offset", "allocation"});
		}
		catch (const std::length_error&)
		{
			return unexpected(error{"store.resource-limit", "snapshot-payload", "maximum-bytes"});
		}
		catch (...)
		{
			return unexpected(
				error{"store.invariant-breach", "snapshot-v5-generation-offset", "exception"});
		}
	}

#if defined(CXXLENS_STORE_FAULT_TEST_SUPPORT)
	result<std::size_t> snapshot_version_component_offset(const std::span<const std::byte> payload,
														  const std::size_t component_index)
	{
		if (component_index > 2U)
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "component"));
		binary_reader reader{payload};
		auto magic = reader.string();
		auto schema = reader.string();
		auto snapshot_id = reader.string();
		if (!magic || !schema || !snapshot_id || *magic != "cxxlens.ng-snapshot-payload.v5")
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "payload"));
		const auto component_offset = reader.offset() + component_index * 8U;
		if (payload.size() - component_offset < 8U)
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "truncated"));
		return component_offset;
	}
#endif

} // namespace cxxlens::sdk::detail
