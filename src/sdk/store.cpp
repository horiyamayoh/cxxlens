#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

#if defined(__unix__) || defined(__APPLE__)
#include <dlfcn.h>
#endif

#include <cxxlens/sdk/store.hpp>

#include "claim_internal.hpp"

namespace cxxlens::sdk
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

		[[nodiscard]] bool identity_digest(const std::string_view value)
		{
			const auto marker = value.rfind(":sha256:");
			if (marker == std::string_view::npos || marker == 0U)
				return false;
			const auto hex = value.substr(marker + 8U);
			return hex.size() == 64U &&
				std::ranges::all_of(hex,
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

		[[nodiscard]] canonical_value texts(const std::vector<std::string>& values)
		{
			std::vector<canonical_value> output;
			output.reserve(values.size());
			for (const auto& value : values)
				output.push_back(text(value));
			return canonical_value::from_tuple(std::move(output));
		}

		[[nodiscard]] std::string partition_identity(const partition_draft& draft)
		{
			const std::array fields{
				text(draft.relation_descriptor_id),
				text(draft.scope),
				text(draft.condition.canonical_form()),
				text(draft.interpretation),
				text(draft.producer_semantics),
				text(draft.producer_input_basis_digest),
				text(draft.precision_profile),
				text(draft.assumption_set_id),
			};
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

		[[nodiscard]] std::vector<std::string> claim_content_ids(const partition_draft& draft)
		{
			std::vector<std::string> output;
			output.reserve(draft.claims.size());
			for (const auto& value : draft.claims)
				output.push_back(value.content);
			std::ranges::sort(output);
			output.erase(std::ranges::unique(output).begin(), output.end());
			return output;
		}

		[[nodiscard]] std::vector<std::string> coverage_values(const partition_draft& draft)
		{
			std::vector<std::string> output;
			output.reserve(draft.coverage.size());
			for (const auto& value : draft.coverage)
				output.push_back(value.canonical_form());
			std::ranges::sort(output);
			return output;
		}

		[[nodiscard]] bool partition_complete(const partition_draft& draft)
		{
			return !draft.coverage.empty() && draft.unresolved.empty() &&
				std::ranges::all_of(draft.coverage,
									[](const snapshot_coverage_unit& unit)
									{
										return unit.state == "covered";
									});
		}

		[[nodiscard]] std::string closure_identity(const closure_candidate& value)
		{
			const std::array fields{
				text(value.relation_descriptor_id),
				text(value.subject_partition_id),
				text(value.partition_content_digest),
				text(value.coverage_digest),
				text(value.key_domain_digest),
				text(value.condition.canonical_form()),
				text(value.interpretation),
				text(value.assumption_set_id),
				text(value.closure_kind),
				text(value.producer_semantics),
				text(value.evidence_digest),
			};
			return *canonical_identity_digest("closure-certificate", fields);
		}

		[[nodiscard]] std::string snapshot_identity(const snapshot_manifest& value)
		{
			std::vector<canonical_value> partitions;
			partitions.reserve(value.partitions.size());
			for (const auto& partition : value.partitions)
				partitions.push_back(
					canonical_value::from_tuple({text(partition.partition_id),
												 text(partition.content_digest),
												 text(partition.coverage_digest)}));
			std::ranges::sort(partitions,
							  [](const canonical_value& left, const canonical_value& right)
							  {
								  return left.tuple.front().text < right.tuple.front().text;
							  });
			auto closures = value.closure_ids;
			std::ranges::sort(closures);
			const std::array fields{
				text(value.snapshot_semantics_version.string()),
				text(value.catalog_semantic_digest),
				text(value.condition_universe_id),
				text(value.relation_registry_digest),
				text(value.interpretation_policy_digest),
				canonical_value::from_tuple(std::move(partitions)),
				texts(closures),
			};
			return *canonical_identity_digest("snapshot", fields);
		}

		[[nodiscard]] std::string publication_identity(const publication_record& value)
		{
			const std::array fields{
				text(value.series_id),
				text(value.snapshot_id),
				canonical_value::from_integer(static_cast<std::int64_t>(value.sequence)),
				text(value.parent_publication.value_or("")),
			};
			return *canonical_identity_digest("publication", fields);
		}

		[[nodiscard]] std::string canonical_export_of(const snapshot_handle::data& value);

		class binary_writer
		{
		  public:
			void unsigned_value(std::uint64_t value)
			{
				for (int shift = 56; shift >= 0; shift -= 8)
					bytes_.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
			}
			void boolean(const bool value)
			{
				bytes_.push_back(value ? std::byte{1} : std::byte{0});
			}
			void string(const std::string_view value)
			{
				unsigned_value(value.size());
				for (const auto byte : value)
					bytes_.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			}
			void raw(const std::span<const std::byte> value)
			{
				unsigned_value(value.size());
				bytes_.insert(bytes_.end(), value.begin(), value.end());
			}
			[[nodiscard]] std::vector<std::byte> finish() &&
			{
				return std::move(bytes_);
			}

		  private:
			std::vector<std::byte> bytes_;
		};

		class binary_reader
		{
		  public:
			explicit binary_reader(const std::span<const std::byte> bytes) : bytes_{bytes} {}
			[[nodiscard]] result<std::uint64_t> unsigned_value()
			{
				if (bytes_.size() - offset_ < 8U)
					return unexpected(store_error("store.corrupt", "payload", "truncated-u64"));
				std::uint64_t output{};
				for (std::size_t index = 0U; index < 8U; ++index)
					output =
						(output << 8U) | std::to_integer<unsigned char>(bytes_[offset_ + index]);
				offset_ += 8U;
				return output;
			}
			[[nodiscard]] result<bool> boolean()
			{
				if (offset_ >= bytes_.size() ||
					std::to_integer<unsigned char>(bytes_[offset_]) > 1U)
					return unexpected(store_error("store.corrupt", "payload", "invalid-bool"));
				return std::to_integer<unsigned char>(bytes_[offset_++]) != 0U;
			}
			[[nodiscard]] result<std::string> string()
			{
				auto size = unsigned_value();
				if (!size || *size > bytes_.size() - offset_)
					return unexpected(store_error("store.corrupt", "payload", "truncated-string"));
				std::string output;
				output.reserve(static_cast<std::size_t>(*size));
				for (std::uint64_t index = 0U; index < *size; ++index)
					output.push_back(static_cast<char>(bytes_[offset_ + index]));
				offset_ += static_cast<std::size_t>(*size);
				return output;
			}
			[[nodiscard]] result<std::vector<std::byte>> raw()
			{
				auto size = unsigned_value();
				if (!size || *size > bytes_.size() - offset_)
					return unexpected(store_error("store.corrupt", "payload", "truncated-bytes"));
				std::vector<std::byte> output(bytes_.begin() + static_cast<std::ptrdiff_t>(offset_),
											  bytes_.begin() +
												  static_cast<std::ptrdiff_t>(offset_ + *size));
				offset_ += static_cast<std::size_t>(*size);
				return output;
			}
			[[nodiscard]] bool finished() const noexcept
			{
				return offset_ == bytes_.size();
			}

		  private:
			std::span<const std::byte> bytes_;
			std::size_t offset_{};
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
				*scalar > static_cast<std::uint8_t>(scalar_kind::set) ||
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
			output.reserve(static_cast<std::size_t>(*count));
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

		void encode_annotation(binary_writer& writer, const snapshot_claim_annotation& value)
		{
			encode_row(writer, value.row);
			encode_condition(writer, value.presence);
			writer.string(value.interpretation);
			writer.string(value.semantic_key);
			writer.string(value.assertion);
			writer.string(value.content);
			writer.string(value.producer.id);
			writer.string(value.producer.semantic_contract);
			writer.string(value.provenance_root);
			encode_guarantee(writer, value.guarantee);
		}

		[[nodiscard]] std::vector<std::byte>
		annotation_projection(const snapshot_claim_annotation& value)
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

		[[nodiscard]] result<std::vector<std::byte>> hex_bytes(const std::string_view value)
		{
			if (value.size() % 2U != 0U)
				return unexpected(store_error("store.corrupt", "sqlite", "odd-hex"));
			std::vector<std::byte> output;
			output.reserve(value.size() / 2U);
			auto nibble = [](const char byte) -> int
			{
				if (byte >= '0' && byte <= '9')
					return byte - '0';
				if (byte >= 'A' && byte <= 'F')
					return byte - 'A' + 10;
				if (byte >= 'a' && byte <= 'f')
					return byte - 'a' + 10;
				return -1;
			};
			for (std::size_t index = 0U; index < value.size(); index += 2U)
			{
				const auto upper = nibble(value[index]);
				const auto lower = nibble(value[index + 1U]);
				if (upper < 0 || lower < 0)
					return unexpected(store_error("store.corrupt", "sqlite", "invalid-hex"));
				output.push_back(static_cast<std::byte>((upper << 4) | lower));
			}
			return output;
		}

		[[nodiscard]] std::string sql_quote(const std::string_view value)
		{
			std::string output{"'"};
			for (const auto byte : value)
			{
				output.push_back(byte);
				if (byte == '\'')
					output.push_back('\'');
			}
			output.push_back('\'');
			return output;
		}
	} // namespace

	struct snapshot_handle::data
	{
		snapshot_manifest semantic_manifest;
		publication_record publication_record_value;
		std::map<std::string, relation_descriptor, std::less<>> descriptors;
		std::map<std::string, std::vector<detached_row>, std::less<>> rows;
		std::map<std::string, std::vector<snapshot_claim_annotation>, std::less<>> annotations;
		std::vector<snapshot_query_coverage> coverage;
		std::vector<snapshot_partition_binding> partition_bindings;
		std::map<std::string, partition_draft, std::less<>> partition_envelopes;
		std::vector<closure_certificate> closure_certificates;
		std::vector<std::string> claim_contents;
		std::vector<unresolved_reference> unresolved;
		std::string physical_backend;
		bool query_annotations_available{};
		std::shared_ptr<const std::uint64_t> generation_pin;
	};

	namespace
	{
		void encode_partition_envelopes(
			binary_writer& writer,
			const std::map<std::string, partition_draft, std::less<>>& envelopes)
		{
			writer.unsigned_value(envelopes.size());
			for (const auto& [partition_id, draft] : envelopes)
			{
				auto claims = draft.claims;
				std::ranges::sort(claims, {}, &claim::content);
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
		semantic_projection_bytes(const snapshot_handle::data& value);

		[[nodiscard]] std::string canonical_export_of(const snapshot_handle::data& value)
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
			output << "semantic-projection=" << bytes_hex(semantic_projection_bytes(value)) << '\n';
			binary_writer envelopes;
			encode_partition_envelopes(envelopes, value.partition_envelopes);
			output << "partition-envelopes=" << bytes_hex(std::move(envelopes).finish()) << '\n';
			return output.str();
		}

		[[nodiscard]] std::vector<std::byte> encode_snapshot(const snapshot_handle::data& value)
		{
			binary_writer writer;
			writer.string("cxxlens.ng-snapshot-payload.v5");
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
			writer.boolean(value.query_annotations_available);
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
			encode_partition_envelopes(writer, value.partition_envelopes);
			return std::move(writer).finish();
		}

		void sort_semantic_projections(snapshot_handle::data& value)
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
									  return annotation_projection(left) <
										  annotation_projection(right);
								  });
			}
		}

		[[nodiscard]] std::vector<std::byte>
		semantic_projection_bytes(const snapshot_handle::data& value)
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

		[[nodiscard]] result<void> validate_semantic_graph(snapshot_handle::data& value,
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
			sort_semantic_projections(expected);
			sort_semantic_projections(value);
			if (semantic_projection_bytes(expected) != semantic_projection_bytes(value))
				return unexpected(store_error("store.corrupt", "partition-envelope", "projection"));
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		decode_snapshot(const std::span<const std::byte> bytes, const relation_engine& engine)
		{
			binary_reader reader{bytes};
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
			auto value = std::make_shared<snapshot_handle::data>();
			auto& manifest = value->semantic_manifest;
			auto schema = reader.string();
			auto id = reader.string();
			auto major = reader.unsigned_value();
			auto minor = reader.unsigned_value();
			auto patch = reader.unsigned_value();
			auto catalog = reader.string();
			auto universe = reader.string();
			auto registry = reader.string();
			auto policy = reader.string();
			auto partition_count = reader.unsigned_value();
			if (!schema || !id || !major || !minor || !patch || !catalog || !universe ||
				!registry || !policy || !partition_count || *partition_count > 1'000'000U)
				return unexpected(store_error("store.corrupt", "manifest", "header"));
			manifest.schema = std::move(*schema);
			manifest.id = std::move(*id);
			manifest.snapshot_semantics_version = {static_cast<std::uint32_t>(*major),
												   static_cast<std::uint32_t>(*minor),
												   static_cast<std::uint32_t>(*patch)};
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
				rows.reserve(static_cast<std::size_t>(*row_count));
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
					annotations.reserve(static_cast<std::size_t>(*annotation_count));
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
					draft.claims.reserve(static_cast<std::size_t>(*claim_count_value));
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
				if (auto valid = validate_semantic_graph(*value, engine); !valid)
					return unexpected(std::move(valid.error()));
			}
			if (!reader.finished() || manifest.schema != "cxxlens.snapshot-manifest.v1" ||
				manifest.id != snapshot_identity(manifest) ||
				publication.snapshot_id != manifest.id)
				return unexpected(store_error("store.corrupt", "payload", "semantic-digest"));
			return value;
		}
	} // namespace

	result<void> snapshot_series_selector::validate() const
	{
		if (catalog_id.empty() || channel_id.empty() || engine_generation_id.empty() ||
			condition_universe_id.empty() || !digest(relation_registry_digest) ||
			!digest(interpretation_policy_digest) || !digest(trust_policy_digest))
			return unexpected(store_error("store.selection-authority-incomplete", "selector"));
		return {};
	}

	std::string snapshot_series_selector::id() const
	{
		if (!validate())
			return {};
		const std::array fields{text(catalog_id),
								text(channel_id),
								text(engine_generation_id),
								text(condition_universe_id),
								text(relation_registry_digest),
								text(interpretation_policy_digest),
								text(trust_policy_digest)};
		return *canonical_identity_digest("snapshot-series", fields);
	}

	result<void> snapshot_coverage_unit::validate() const
	{
		static const std::set<std::string, std::less<>> states{
			"covered", "not_covered", "unknown", "unresolved"};
		if (domain.empty() || key.empty() || !states.contains(state) ||
			(state == "covered" && !reason.empty()) || (state != "covered" && reason.empty()))
			return unexpected(store_error("store.coverage-invalid", "coverage", state));
		return {};
	}

	std::string snapshot_coverage_unit::canonical_form() const
	{
		const std::array fields{text(domain), text(key), text(state), text(reason)};
		return *canonical_identity_digest("coverage-unit", fields);
	}

	result<partition_manifest> make_partition_manifest(const relation_engine& engine,
													   const partition_draft& draft)
	{
		if (draft.relation_descriptor_id.empty() || draft.scope.empty() ||
			draft.interpretation.empty() || !digest(draft.producer_semantics) ||
			!digest(draft.producer_input_basis_digest) || draft.precision_profile.empty() ||
			draft.assumption_set_id.empty())
			return unexpected(store_error("store.partition-invalid", "identity"));
		if (auto valid = draft.condition.validate(); !valid)
			return unexpected(
				store_error("store.partition-invalid", "condition", valid.error().code));
		auto relation = engine.require_id(draft.relation_descriptor_id);
		if (!relation)
			return unexpected(
				store_error("store.partition-relation-unknown", draft.relation_descriptor_id));
		for (const auto& value : draft.claims)
		{
			if (auto valid = validate_claim(engine, value); !valid)
				return unexpected(
					store_error("store.claim-invalid", value.content, valid.error().code));
			if (value.descriptor != draft.relation_descriptor_id ||
				value.presence != draft.condition || value.interpretation != draft.interpretation ||
				value.producer.semantic_contract != draft.producer_semantics)
				return unexpected(store_error("store.partition-claim-mismatch", value.content));
			auto basis = claim_input_basis_digest(value.input_basis);
			if (!basis || *basis != draft.producer_input_basis_digest)
				return unexpected(store_error("store.partition-basis-mismatch", value.content));
		}
		std::set<std::string, std::less<>> coverage_ids;
		for (const auto& value : draft.coverage)
		{
			if (auto valid = value.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (!coverage_ids.insert(value.canonical_form()).second)
				return unexpected(store_error("store.coverage-duplicate", value.key));
		}
		const auto claims = claim_content_ids(draft);
		const auto coverage = coverage_values(draft);
		const auto partition_id = partition_identity(draft);
		const std::array claim_fields{texts(claims)};
		const auto claim_set = *canonical_identity_digest("claim-set", claim_fields.front().tuple);
		const std::array coverage_fields{texts(coverage)};
		const auto coverage_digest =
			*canonical_identity_digest("coverage", coverage_fields.front().tuple);
		const std::array content_fields{text(partition_id), text(claim_set), text(coverage_digest)};
		return partition_manifest{partition_id,
								  draft.relation_descriptor_id,
								  draft.producer_input_basis_digest,
								  claim_set,
								  coverage_digest,
								  *canonical_identity_digest("partition-content", content_fields),
								  static_cast<std::uint64_t>(claims.size()),
								  partition_complete(draft)};
	}

	result<partition_certificate_subject>
	make_partition_certificate_subject(partition_manifest partition,
									   snapshot_partition_binding binding)
	{
		const auto identity = identity_draft(binding);
		const std::array content_fields{text(partition.partition_id),
										text(partition.claim_set_digest),
										text(partition.coverage_digest)};
		if (partition.partition_id != binding.partition_id ||
			partition.relation_descriptor_id != binding.relation_descriptor_id ||
			partition.input_basis_digest != binding.producer_input_basis_digest ||
			partition_identity(identity) != partition.partition_id ||
			partition.content_digest !=
				*canonical_identity_digest("partition-content", content_fields) ||
			!digest(partition.input_basis_digest) || !identity_digest(partition.claim_set_digest) ||
			!identity_digest(partition.coverage_digest) ||
			!identity_digest(partition.content_digest) || binding.scope.empty() ||
			binding.interpretation.empty() || !digest(binding.producer_semantics) ||
			binding.precision_profile.empty() || binding.assumption_set_id.empty() ||
			!binding.condition.validate())
			return unexpected(store_error("store.closure-subject-invalid", partition.partition_id));
		return partition_certificate_subject{std::move(partition), std::move(binding)};
	}

	result<closure_certificate>
	make_closure_certificate(const partition_certificate_subject& subject,
							 closure_candidate candidate)
	{
		const auto& partition = subject.partition;
		const auto& binding = subject.binding;
		if (!partition.complete)
			return unexpected(
				store_error("store.partial-partition-closure", partition.partition_id));
		if (candidate.relation_descriptor_id != partition.relation_descriptor_id ||
			candidate.subject_partition_id != partition.partition_id ||
			candidate.partition_content_digest != partition.content_digest ||
			candidate.coverage_digest != partition.coverage_digest ||
			candidate.condition != binding.condition ||
			candidate.interpretation != binding.interpretation ||
			candidate.assumption_set_id != binding.assumption_set_id ||
			candidate.producer_semantics != binding.producer_semantics)
			return unexpected(
				store_error("store.closure-binding-mismatch", partition.partition_id));
		if (!digest(candidate.key_domain_digest) || !digest(candidate.producer_semantics) ||
			!digest(candidate.evidence_digest) || candidate.interpretation.empty() ||
			candidate.assumption_set_id.empty() ||
			candidate.closure_kind != "relation-key-enumeration" || !candidate.condition.validate())
			return unexpected(store_error("store.closure-invalid", partition.partition_id));
		return closure_certificate{closure_identity(candidate), std::move(candidate)};
	}

	snapshot_handle::snapshot_handle(std::shared_ptr<const data> data) : data_{std::move(data)} {}
	std::string_view snapshot_handle::id() const noexcept
	{
		return data_ ? std::string_view{data_->semantic_manifest.id} : std::string_view{};
	}
	const snapshot_manifest& snapshot_handle::manifest() const
	{
		return data_->semantic_manifest;
	}
	const publication_record& snapshot_handle::publication() const
	{
		return data_->publication_record_value;
	}
	result<row_cursor> snapshot_handle::open(const dynamic_relation& relation) const
	{
		if (!data_)
			return unexpected(store_error("sdk.snapshot-empty", "snapshot"));
		const auto descriptor = data_->descriptors.find(relation.descriptor().id);
		if (descriptor == data_->descriptors.end() || descriptor->second != relation.descriptor())
			return unexpected(
				store_error("sdk.snapshot-relation-mismatch", relation.descriptor().id));
		const auto rows = data_->rows.find(relation.descriptor().id);
		static const std::vector<detached_row> empty_rows;
		return row_cursor{data_, rows == data_->rows.end() ? &empty_rows : &rows->second};
	}
	result<claim_annotation_cursor>
	snapshot_handle::open_claims(const std::string_view relation_descriptor_id) const
	{
		if (!data_)
			return unexpected(store_error("sdk.snapshot-empty", "snapshot"));
		if (!data_->query_annotations_available)
			return unexpected(store_error("sdk.query-annotations-unavailable",
										  std::string{relation_descriptor_id}));
		if (!data_->descriptors.contains(relation_descriptor_id))
			return unexpected(
				store_error("sdk.snapshot-relation-mismatch", std::string{relation_descriptor_id}));
		const auto found = data_->annotations.find(relation_descriptor_id);
		static const std::vector<snapshot_claim_annotation> empty_annotations;
		return claim_annotation_cursor{
			data_, found == data_->annotations.end() ? &empty_annotations : &found->second};
	}
	result<relation_descriptor>
	snapshot_handle::descriptor(const std::string_view relation_descriptor_id) const
	{
		if (!data_)
			return unexpected(store_error("sdk.snapshot-empty", "snapshot"));
		const auto found = data_->descriptors.find(relation_descriptor_id);
		if (found == data_->descriptors.end())
			return unexpected(
				store_error("sdk.snapshot-relation-mismatch", std::string{relation_descriptor_id}));
		return found->second;
	}
	std::span<const snapshot_query_coverage> snapshot_handle::input_coverage() const noexcept
	{
		return data_ ? std::span<const snapshot_query_coverage>{data_->coverage}
					 : std::span<const snapshot_query_coverage>{};
	}
	std::span<const snapshot_partition_binding> snapshot_handle::partition_bindings() const noexcept
	{
		return data_ ? std::span<const snapshot_partition_binding>{data_->partition_bindings}
					 : std::span<const snapshot_partition_binding>{};
	}
	std::span<const closure_certificate> snapshot_handle::closure_certificates() const noexcept
	{
		return data_ ? std::span<const closure_certificate>{data_->closure_certificates}
					 : std::span<const closure_certificate>{};
	}
	std::span<const unresolved_reference> snapshot_handle::unresolved_items() const noexcept
	{
		return data_ ? std::span<const unresolved_reference>{data_->unresolved}
					 : std::span<const unresolved_reference>{};
	}
	std::string_view snapshot_handle::physical_backend() const noexcept
	{
		return data_ ? std::string_view{data_->physical_backend} : std::string_view{};
	}
	bool snapshot_handle::query_annotations_available() const noexcept
	{
		return data_ && data_->query_annotations_available;
	}
	bool snapshot_handle::empty() const noexcept
	{
		return !data_;
	}

	row_view::row_view(const detached_row* row,
					   std::weak_ptr<const std::uint64_t> generation,
					   const std::uint64_t expected)
		: row_{row}, generation_{std::move(generation)}, expected_{expected}
	{
	}
	result<detached_row> row_view::copy() const
	{
		const auto current = generation_.lock();
		if (!current || *current != expected_ || row_ == nullptr)
			return unexpected(store_error("sdk.row-view-expired", "row_view"));
		return *row_;
	}
	row_cursor::row_cursor(std::shared_ptr<const snapshot_handle::data> snapshot,
						   const std::vector<detached_row>* rows)
		: snapshot_{std::move(snapshot)}, rows_{rows}, owner_{std::this_thread::get_id()},
		  generation_{std::make_shared<std::uint64_t>(0U)}
	{
	}
	result<std::optional<row_view>> row_cursor::next()
	{
		if (owner_ != std::this_thread::get_id())
			return unexpected(store_error("sdk.cursor-thread-violation", "cursor"));
		++*generation_;
		if (rows_ == nullptr || index_ >= rows_->size())
			return std::optional<row_view>{};
		return std::optional<row_view>{row_view{&(*rows_)[index_++], generation_, *generation_}};
	}

	claim_annotation_view::claim_annotation_view(const snapshot_claim_annotation* value,
												 std::weak_ptr<const std::uint64_t> generation,
												 const std::uint64_t expected)
		: value_{value}, generation_{std::move(generation)}, expected_{expected}
	{
	}
	result<snapshot_claim_annotation> claim_annotation_view::copy() const
	{
		const auto current = generation_.lock();
		if (!current || *current != expected_ || value_ == nullptr)
			return unexpected(store_error("sdk.claim-annotation-view-expired", "claim_view"));
		return *value_;
	}
	claim_annotation_cursor::claim_annotation_cursor(
		std::shared_ptr<const snapshot_handle::data> snapshot,
		const std::vector<snapshot_claim_annotation>* values)
		: snapshot_{std::move(snapshot)}, values_{values}, owner_{std::this_thread::get_id()},
		  generation_{std::make_shared<std::uint64_t>(0U)}
	{
	}
	result<std::optional<claim_annotation_view>> claim_annotation_cursor::next()
	{
		if (owner_ != std::this_thread::get_id())
			return unexpected(
				store_error("sdk.claim-annotation-cursor-thread-violation", "cursor"));
		++*generation_;
		if (values_ == nullptr || index_ >= values_->size())
			return std::optional<claim_annotation_view>{};
		return std::optional<claim_annotation_view>{
			claim_annotation_view{&(*values_)[index_++], generation_, *generation_}};
	}
} // namespace cxxlens::sdk

namespace cxxlens::sdk
{
	namespace
	{
		struct sqlite_api
		{
			using open_fn = int (*)(const char*, void**, int, const char*);
			using close_fn = int (*)(void*);
			using errmsg_fn = const char* (*)(void*);
			using exec_fn =
				int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
			using free_fn = void (*)(void*);
			void* library{};
			open_fn open{};
			close_fn close{};
			errmsg_fn errmsg{};
			exec_fn exec{};
			free_fn free_memory{};
			~sqlite_api()
			{
#if defined(__unix__) || defined(__APPLE__)
				if (library != nullptr)
					dlclose(library);
#endif
			}
		};

		template <class Function>
		[[nodiscard]] bool resolve_sqlite(void* library, const char* name, Function& output)
		{
#if defined(__unix__) || defined(__APPLE__)
			void* symbol = dlsym(library, name);
			if (symbol == nullptr)
				return false;
			output = reinterpret_cast<Function>(symbol);
			return true;
#else
			(void)library;
			(void)name;
			(void)output;
			return false;
#endif
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_api>> load_sqlite()
		{
#if defined(__unix__) || defined(__APPLE__)
#if defined(__APPLE__)
			constexpr std::array candidates{"libsqlite3.dylib", "/usr/lib/libsqlite3.dylib"};
#else
			constexpr std::array candidates{"libsqlite3.so.0", "libsqlite3.so"};
#endif
			void* library{};
			for (const auto* candidate : candidates)
			{
				library = dlopen(candidate, RTLD_NOW | RTLD_LOCAL);
				if (library != nullptr)
					break;
			}
			if (library == nullptr)
				return unexpected(store_error("store.backend-unavailable", "sqlite", "library"));
			auto api = std::make_shared<sqlite_api>();
			api->library = library;
			if (!resolve_sqlite(library, "sqlite3_open_v2", api->open) ||
				!resolve_sqlite(library, "sqlite3_close_v2", api->close) ||
				!resolve_sqlite(library, "sqlite3_errmsg", api->errmsg) ||
				!resolve_sqlite(library, "sqlite3_exec", api->exec) ||
				!resolve_sqlite(library, "sqlite3_free", api->free_memory))
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			return api;
#else
			return unexpected(store_error("store.backend-unavailable", "sqlite", "platform"));
#endif
		}

		class sqlite_database
		{
		  public:
			sqlite_database(std::shared_ptr<sqlite_api> api, void* database)
				: api_{std::move(api)}, database_{database}
			{
			}
			~sqlite_database()
			{
				if (database_ != nullptr)
					api_->close(database_);
			}
			[[nodiscard]] result<void> execute(const std::string& sql) const
			{
				char* message{};
				const auto code = api_->exec(database_, sql.c_str(), nullptr, nullptr, &message);
				if (code == 0)
					return {};
				std::string detail = message != nullptr ? message : api_->errmsg(database_);
				if (message != nullptr)
					api_->free_memory(message);
				return unexpected(
					store_error("store.sqlite-failure", "database", std::move(detail)));
			}
			[[nodiscard]] result<std::vector<std::vector<std::string>>>
			query(const std::string& sql) const
			{
				std::vector<std::vector<std::string>> rows;
				auto callback = [](void* context, const int count, char** values, char**) -> int
				{
					auto& output = *static_cast<std::vector<std::vector<std::string>>*>(context);
					auto& row = output.emplace_back();
					for (int index = 0; index < count; ++index)
						row.emplace_back(values[index] == nullptr ? "" : values[index]);
					return 0;
				};
				char* message{};
				const auto code = api_->exec(database_, sql.c_str(), callback, &rows, &message);
				if (code == 0)
					return rows;
				std::string detail = message != nullptr ? message : api_->errmsg(database_);
				if (message != nullptr)
					api_->free_memory(message);
				return unexpected(
					store_error("store.sqlite-failure", "database", std::move(detail)));
			}

		  private:
			std::shared_ptr<sqlite_api> api_;
			void* database_{};
		};

		[[nodiscard]] result<std::unique_ptr<sqlite_database>>
		open_database(const std::string& path)
		{
			auto api = load_sqlite();
			if (!api)
				return unexpected(std::move(api.error()));
			void* database{};
			constexpr int read_write = 0x00000002;
			constexpr int create = 0x00000004;
			constexpr int full_mutex = 0x00010000;
			if ((*api)->open(path.c_str(), &database, read_write | create | full_mutex, nullptr) !=
				0)
			{
				std::string detail = database != nullptr ? (*api)->errmsg(database) : "open";
				if (database != nullptr)
					(*api)->close(database);
				return unexpected(store_error("store.sqlite-failure", "open", std::move(detail)));
			}
			auto output = std::make_unique<sqlite_database>(*api, database);
			if (auto configured = output->execute(
					"PRAGMA journal_mode=WAL;"
					"PRAGMA synchronous=FULL;"
					"CREATE TABLE IF NOT EXISTS cxxlens_ng_metadata("
					"key TEXT PRIMARY KEY,value TEXT NOT NULL);"
					"INSERT OR IGNORE INTO cxxlens_ng_metadata VALUES("
					"'physical_format','cxxlens.sqlite-semantic-store.v2');"
					"CREATE TABLE IF NOT EXISTS cxxlens_ng_publication("
					"publication_id TEXT PRIMARY KEY,series_id TEXT NOT NULL,"
					"snapshot_id TEXT NOT NULL,sequence INTEGER NOT NULL,"
					"generation INTEGER NOT NULL,parent TEXT,state INTEGER NOT NULL,"
					"checksum TEXT NOT NULL,payload BLOB NOT NULL);"
					"CREATE TABLE IF NOT EXISTS cxxlens_ng_series_head("
					"series_id TEXT PRIMARY KEY,current_publication TEXT NOT NULL,"
					"sequence INTEGER NOT NULL);"
					"CREATE INDEX IF NOT EXISTS cxxlens_ng_publication_series "
					"ON cxxlens_ng_publication(series_id,sequence);");
				!configured)
				return unexpected(std::move(configured.error()));
			auto format =
				output->query("SELECT value FROM cxxlens_ng_metadata WHERE key='physical_format'");
			if (!format || format->size() != 1U || format->front().size() != 1U ||
				format->front().front() != "cxxlens.sqlite-semantic-store.v2")
				return unexpected(store_error("store.format-incompatible", "sqlite"));
			return output;
		}

		[[nodiscard]] result<std::uint64_t> parse_unsigned(const std::string_view value)
		{
			std::uint64_t output{};
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
			if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size())
				return unexpected(store_error("store.corrupt", "sqlite", "integer"));
			return output;
		}
	} // namespace

	struct snapshot_store::implementation
	{
		explicit implementation(relation_engine value, std::string backend_value)
			: engine{std::move(value)}, backend{std::move(backend_value)}
		{
		}

		relation_engine engine;
		std::string backend;
		mutable std::mutex mutex;
		std::map<std::string, publication_record, std::less<>> records;
		std::map<std::string, std::shared_ptr<snapshot_handle::data>, std::less<>> publications;
		std::map<std::string, std::string, std::less<>> heads;
		std::vector<std::weak_ptr<const std::uint64_t>> generation_tokens;
		std::uint64_t generation{};
		std::unique_ptr<sqlite_database> database;

		[[nodiscard]] result<void> persist(const snapshot_handle::data& value) const
		{
			if (database == nullptr)
				return {};
			const auto payload = encode_snapshot(value);
			const auto checksum = content_digest(payload);
			const auto& record = value.publication_record_value;
			std::ostringstream sql;
			sql << "BEGIN IMMEDIATE;INSERT OR REPLACE INTO cxxlens_ng_publication VALUES("
				<< sql_quote(record.publication_id) << ',' << sql_quote(record.series_id) << ','
				<< sql_quote(record.snapshot_id) << ',' << record.sequence << ','
				<< record.physical_generation << ','
				<< (record.parent_publication ? sql_quote(*record.parent_publication) : "NULL")
				<< ',' << static_cast<unsigned>(record.state) << ',' << sql_quote(checksum) << ",X'"
				<< bytes_hex(payload) << "');COMMIT;";
			auto persisted = database->execute(sql.str());
			if (!persisted)
				(void)database->execute("ROLLBACK;");
			return persisted;
		}

		[[nodiscard]] result<void>
		persist_new(const snapshot_handle::data& value,
					const std::optional<std::string>& expected_parent) const
		{
			if (database == nullptr)
				return {};
			if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
				return begun;
			const auto rollback = [&]()
			{
				(void)database->execute("ROLLBACK;");
			};
			const auto& record = value.publication_record_value;
			auto head = database->query(
				"SELECT current_publication,sequence FROM cxxlens_ng_series_head WHERE series_id=" +
				sql_quote(record.series_id));
			if (!head)
			{
				rollback();
				return unexpected(std::move(head.error()));
			}
			if (head->size() > 1U || (!head->empty() && head->front().size() != 2U))
			{
				rollback();
				return unexpected(store_error("store.corrupt", record.series_id, "series-head"));
			}
			const std::optional<std::string> actual_parent = head->empty()
				? std::optional<std::string>{}
				: std::optional<std::string>{head->front().front()};
			auto prior_sequence = head->empty() ? result<std::uint64_t>{std::uint64_t{0U}}
												: parse_unsigned(head->front()[1U]);
			if (!prior_sequence)
			{
				rollback();
				return unexpected(std::move(prior_sequence.error()));
			}
			if (actual_parent != expected_parent || record.parent_publication != expected_parent ||
				record.sequence != *prior_sequence + 1U)
			{
				rollback();
				return unexpected(store_error("store.publication-conflict", record.series_id));
			}
			auto duplicate = database->query(
				"SELECT publication_id FROM cxxlens_ng_publication WHERE publication_id=" +
				sql_quote(record.publication_id));
			if (!duplicate)
			{
				rollback();
				return unexpected(std::move(duplicate.error()));
			}
			if (!duplicate->empty())
			{
				rollback();
				return unexpected(store_error("store.publication-conflict", record.publication_id));
			}
			const auto payload = encode_snapshot(value);
			const auto checksum = content_digest(payload);
			std::ostringstream insert;
			insert << "INSERT INTO cxxlens_ng_publication VALUES("
				   << sql_quote(record.publication_id) << ',' << sql_quote(record.series_id) << ','
				   << sql_quote(record.snapshot_id) << ',' << record.sequence << ','
				   << record.physical_generation << ','
				   << (record.parent_publication ? sql_quote(*record.parent_publication) : "NULL")
				   << ',' << static_cast<unsigned>(record.state) << ',' << sql_quote(checksum)
				   << ",X'" << bytes_hex(payload) << "');";
			if (auto inserted = database->execute(insert.str()); !inserted)
			{
				rollback();
				return inserted;
			}
			const auto head_sql = actual_parent
				? "UPDATE cxxlens_ng_series_head SET current_publication=" +
					sql_quote(record.publication_id) +
					",sequence=" + std::to_string(record.sequence) +
					" WHERE series_id=" + sql_quote(record.series_id) +
					" AND current_publication=" + sql_quote(*actual_parent) + ';'
				: "INSERT INTO cxxlens_ng_series_head VALUES(" + sql_quote(record.series_id) + ',' +
					sql_quote(record.publication_id) + ',' + std::to_string(record.sequence) + ");";
			if (auto updated = database->execute(head_sql); !updated)
			{
				rollback();
				return updated;
			}
			if (auto committed = database->execute("COMMIT;"); !committed)
			{
				rollback();
				return committed;
			}
			return {};
		}

		[[nodiscard]] result<void> load()
		{
			if (database == nullptr)
				return {};
			auto rows = database->query(
				"SELECT publication_id,series_id,snapshot_id,sequence,generation,"
				"COALESCE(parent,''),state,checksum,hex(payload) "
				"FROM cxxlens_ng_publication ORDER BY series_id,sequence,publication_id");
			if (!rows)
				return unexpected(std::move(rows.error()));
			for (const auto& row : *rows)
			{
				if (row.size() != 9U)
					return unexpected(store_error("store.corrupt", "sqlite", "column-count"));
				auto sequence = parse_unsigned(row[3]);
				auto physical = parse_unsigned(row[4]);
				auto state = parse_unsigned(row[6]);
				auto payload = hex_bytes(row[8]);
				if (!sequence || !physical || !state || !payload ||
					*state > static_cast<std::uint8_t>(publication_state::rolled_back))
					return unexpected(store_error("store.corrupt", "sqlite", "publication-row"));
				publication_record record{row[0],
										  row[1],
										  row[2],
										  *sequence,
										  *physical,
										  row[5].empty() ? std::optional<std::string>{}
														 : std::optional<std::string>{row[5]},
										  static_cast<publication_state>(*state),
										  false};
				generation = std::max(generation, *physical);
				if (content_digest(*payload) != row[7])
				{
					record.corrupt = true;
					records.emplace(record.publication_id, record);
					continue;
				}
				auto decoded = decode_snapshot(*payload, engine);
				if (!decoded || (*decoded)->publication_record_value != record)
				{
					record.corrupt = true;
					records.emplace(record.publication_id, record);
					continue;
				}
				auto token = std::make_shared<const std::uint64_t>(*physical);
				(*decoded)->generation_pin = token;
				(*decoded)->physical_backend = backend;
				const bool collision = std::ranges::any_of(
					publications,
					[&](const auto& existing)
					{
						return existing.second->semantic_manifest.id ==
							(*decoded)->semantic_manifest.id &&
							canonical_export_of(*existing.second) != canonical_export_of(**decoded);
					});
				if (collision)
				{
					record.corrupt = true;
					records.emplace(record.publication_id, record);
					continue;
				}
				generation_tokens.push_back(token);
				records.emplace(record.publication_id, record);
				publications.emplace(record.publication_id, std::move(*decoded));
			}
			for (const auto& [id, record] : records)
			{
				if (record.state != publication_state::committed)
					continue;
				const auto current = heads.find(record.series_id);
				if (current == heads.end() ||
					records.at(current->second).sequence < record.sequence)
					heads[record.series_id] = id;
				else if (records.at(current->second).sequence == record.sequence)
					return unexpected(store_error("store.current-ambiguous", record.series_id));
			}
			for (const auto& [series, publication] : heads)
			{
				const auto& record = records.at(publication);
				if (auto initialized =
						database->execute("INSERT OR IGNORE INTO cxxlens_ng_series_head VALUES(" +
										  sql_quote(series) + ',' + sql_quote(publication) + ',' +
										  std::to_string(record.sequence) + ");");
					!initialized)
					return initialized;
			}
			auto persisted_heads = database->query(
				"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head");
			if (!persisted_heads)
				return unexpected(std::move(persisted_heads.error()));
			if (persisted_heads->size() != heads.size())
				return unexpected(store_error("store.corrupt", "sqlite", "series-head-count"));
			for (const auto& row : *persisted_heads)
			{
				if (row.size() != 3U || !heads.contains(row[0U]) || heads.at(row[0U]) != row[1U])
					return unexpected(store_error("store.corrupt", "sqlite", "series-head"));
				auto sequence = parse_unsigned(row[2U]);
				if (!sequence || records.at(row[1U]).sequence != *sequence)
					return unexpected(
						store_error("store.corrupt", "sqlite", "series-head-sequence"));
			}
			return {};
		}
	};

	struct snapshot_writer::data
	{
		std::shared_ptr<snapshot_store::implementation> store;
		snapshot_draft draft;
		publication_state current_state{publication_state::created};
		std::vector<partition_draft> partitions;
		std::vector<closure_candidate> closures;
		std::shared_ptr<snapshot_handle::data> candidate;
	};

	snapshot_store::snapshot_store(std::shared_ptr<implementation> implementation)
		: implementation_{std::move(implementation)}
	{
	}

	result<snapshot_handle> snapshot_store::current(const snapshot_series_selector& selector) const
	{
		if (auto valid = selector.validate(); !valid)
			return unexpected(std::move(valid.error()));
		std::scoped_lock lock{implementation_->mutex};
		const auto head = implementation_->heads.find(selector.id());
		if (head == implementation_->heads.end())
			return unexpected(store_error("store.current-not-found", selector.id()));
		const auto& record = implementation_->records.at(head->second);
		if (record.corrupt)
			return unexpected(store_error("store.current-corrupt", record.publication_id));
		const auto publication = implementation_->publications.find(record.publication_id);
		if (publication == implementation_->publications.end())
			return unexpected(store_error("store.current-corrupt", record.publication_id));
		return snapshot_handle{publication->second};
	}

	result<snapshot_handle> snapshot_store::open(const std::string_view snapshot_id) const
	{
		std::scoped_lock lock{implementation_->mutex};
		const publication_record* selected{};
		for (const auto& [publication_id, record] : implementation_->records)
		{
			(void)publication_id;
			if (record.state != publication_state::committed || record.snapshot_id != snapshot_id)
				continue;
			if (selected != nullptr && selected->sequence == record.sequence &&
				selected->physical_generation == record.physical_generation &&
				selected->publication_id != record.publication_id)
				return unexpected(
					store_error("store.snapshot-ambiguous", std::string{snapshot_id}));
			if (selected == nullptr ||
				std::tie(selected->sequence, selected->physical_generation) <
					std::tie(record.sequence, record.physical_generation))
				selected = &record;
		}
		if (selected == nullptr)
			return unexpected(store_error("store.snapshot-not-found", std::string{snapshot_id}));
		if (selected->corrupt)
			return unexpected(store_error("store.snapshot-corrupt", selected->publication_id));
		const auto publication = implementation_->publications.find(selected->publication_id);
		if (publication == implementation_->publications.end() ||
			publication->second->publication_record_value != *selected ||
			publication->second->semantic_manifest.id != snapshot_id)
			return unexpected(store_error("store.snapshot-corrupt", selected->publication_id));
		return snapshot_handle{publication->second};
	}

	result<snapshot_handle>
	snapshot_store::open_publication(const std::string_view publication_id) const
	{
		std::scoped_lock lock{implementation_->mutex};
		const auto record = implementation_->records.find(publication_id);
		if (record == implementation_->records.end() ||
			record->second.state != publication_state::committed)
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		if (record->second.corrupt)
			return unexpected(
				store_error("store.publication-corrupt", std::string{publication_id}));
		const auto publication = implementation_->publications.find(publication_id);
		if (publication == implementation_->publications.end() ||
			publication->second->publication_record_value != record->second)
			return unexpected(
				store_error("store.publication-corrupt", std::string{publication_id}));
		return snapshot_handle{publication->second};
	}

	result<snapshot_writer> snapshot_store::begin(snapshot_draft draft)
	{
		if (auto valid = draft.series.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (!digest(draft.catalog_semantic_digest) ||
			draft.series.engine_generation_id != implementation_->engine.generation() ||
			draft.series.relation_registry_digest != implementation_->engine.registry_digest())
			return unexpected(store_error("store.draft-authority-mismatch", "snapshot"));
		auto value = std::make_unique<snapshot_writer::data>();
		value->store = implementation_;
		value->draft = std::move(draft);
		return snapshot_writer{std::move(value)};
	}

	store_compatibility snapshot_store::compatibility() const
	{
		return {implementation_->backend, {2U, 5U, 0U}, true, false};
	}

	result<void> snapshot_store::compact()
	{
		std::scoped_lock lock{implementation_->mutex};
		if (const auto corrupt = std::ranges::find_if(implementation_->records,
													  [](const auto& record)
													  {
														  return record.second.corrupt;
													  });
			corrupt != implementation_->records.end())
			return unexpected(store_error("store.compact-validation-failed", corrupt->first));
		std::vector<std::pair<std::string, std::shared_ptr<snapshot_handle::data>>> replacements;
		const auto next_generation = implementation_->generation + 1U;
		for (const auto& [id, current] : implementation_->publications)
		{
			if (current->semantic_manifest.id != snapshot_identity(current->semantic_manifest) ||
				current->publication_record_value.corrupt)
				return unexpected(store_error("store.compact-validation-failed", id));
			if (!current->partition_envelopes.empty())
				if (auto valid = validate_semantic_graph(*current, implementation_->engine); !valid)
					return unexpected(store_error("store.compact-validation-failed", id));
			auto replacement = std::make_shared<snapshot_handle::data>(*current);
			replacement->publication_record_value.physical_generation = next_generation;
			auto token = std::make_shared<const std::uint64_t>(next_generation);
			replacement->generation_pin = token;
			if (auto persisted = implementation_->persist(*replacement); !persisted)
				return unexpected(std::move(persisted.error()));
			implementation_->generation_tokens.push_back(token);
			replacements.emplace_back(id, std::move(replacement));
		}
		for (auto& [id, replacement] : replacements)
		{
			implementation_->records[id] = replacement->publication_record_value;
			implementation_->publications[id] = std::move(replacement);
		}
		implementation_->generation = next_generation;
		return {};
	}

	result<std::string> snapshot_store::canonical_export(const std::string_view snapshot_id) const
	{
		auto opened = open(snapshot_id);
		if (!opened)
			return unexpected(std::move(opened.error()));
		return canonical_export_of(*opened->data_);
	}

	std::size_t snapshot_store::retained_generation_count() const
	{
		std::scoped_lock lock{implementation_->mutex};
		std::set<std::uint64_t> live;
		for (const auto& weak : implementation_->generation_tokens)
			if (const auto token = weak.lock())
				live.insert(*token);
		return live.size();
	}

	snapshot_writer::snapshot_writer(std::unique_ptr<data> data) : data_{std::move(data)} {}
	snapshot_writer::~snapshot_writer()
	{
		if (data_ && data_->current_state != publication_state::committed &&
			data_->current_state != publication_state::rolled_back)
			cancel();
	}
	publication_state snapshot_writer::state() const noexcept
	{
		return data_ ? data_->current_state : publication_state::rolled_back;
	}
	result<void> snapshot_writer::stage(partition_draft partition)
	{
		if (!data_ ||
			(data_->current_state != publication_state::created &&
			 data_->current_state != publication_state::staged))
			return unexpected(store_error("store.transaction-state", "stage"));
		if (partition.condition.universe != data_->draft.series.condition_universe_id)
			return unexpected(store_error("store.condition-universe-mismatch", "partition"));
		std::ranges::sort(partition.claims, detail::claim_occurrence_less);
		partition.claims.erase(std::unique(partition.claims.begin(),
										   partition.claims.end(),
										   detail::same_claim_occurrence),
							   partition.claims.end());
		if (auto manifest = make_partition_manifest(data_->store->engine, partition); !manifest)
			return unexpected(std::move(manifest.error()));
		data_->partitions.push_back(std::move(partition));
		data_->current_state = publication_state::staged;
		return {};
	}
	result<void> snapshot_writer::add_closure(closure_candidate closure)
	{
		if (!data_ || data_->current_state != publication_state::staged)
			return unexpected(store_error("store.transaction-state", "closure"));
		data_->closures.push_back(std::move(closure));
		return {};
	}
	result<void> snapshot_writer::validate()
	{
		if (!data_ || data_->current_state != publication_state::staged ||
			data_->partitions.empty())
			return unexpected(store_error("store.transaction-state", "validate"));
		auto candidate = std::make_shared<snapshot_handle::data>();
		candidate->physical_backend = data_->store->backend;
		candidate->query_annotations_available = true;
		auto& manifest = candidate->semantic_manifest;
		manifest.snapshot_semantics_version = data_->draft.snapshot_semantics_version;
		manifest.catalog_semantic_digest = data_->draft.catalog_semantic_digest;
		manifest.condition_universe_id = data_->draft.series.condition_universe_id;
		manifest.relation_registry_digest = data_->draft.series.relation_registry_digest;
		manifest.interpretation_policy_digest = data_->draft.series.interpretation_policy_digest;
		for (const auto& partition : data_->partitions)
		{
			auto built = make_partition_manifest(data_->store->engine, partition);
			if (!built)
				return unexpected(std::move(built.error()));
			if (!candidate->partition_envelopes.emplace(built->partition_id, partition).second)
				return unexpected(store_error("store.partition-duplicate", built->partition_id));
			manifest.partitions.push_back(*built);
			candidate->partition_bindings.push_back(
				partition_binding(built->partition_id, partition));
			auto relation = data_->store->engine.require_id(partition.relation_descriptor_id);
			if (!relation)
				return unexpected(std::move(relation.error()));
			candidate->descriptors.emplace(partition.relation_descriptor_id,
										   relation->descriptor());
			candidate->rows.try_emplace(partition.relation_descriptor_id);
			candidate->annotations.try_emplace(partition.relation_descriptor_id);
			for (const auto& coverage : partition.coverage)
				candidate->coverage.push_back({partition.relation_descriptor_id, coverage});
			for (const auto& value : partition.claims)
			{
				if (const auto* derived = std::get_if<derived_claim_basis>(&value.input_basis))
				{
					std::scoped_lock lock{data_->store->mutex};
					const auto prior = std::ranges::find_if(
						data_->store->publications,
						[&](const auto& publication)
						{
							return publication.second->semantic_manifest.id ==
								derived->input_snapshot &&
								publication.second->publication_record_value.state ==
								publication_state::committed &&
								!publication.second->publication_record_value.corrupt &&
								publication.second->publication_record_value.physical_generation <=
								data_->store->generation;
						});
					if (prior == data_->store->publications.end())
						return unexpected(
							store_error("store.derived-basis-not-prior", value.content));
					const auto& partitions = prior->second->semantic_manifest.partitions;
					const bool complete_membership = std::ranges::all_of(
						derived->consumed_partition_content_digests,
						[&](const std::string& consumed)
						{
							return std::ranges::any_of(
								partitions,
								[&](const partition_manifest& partition_manifest_value)
								{
									return partition_manifest_value.content_digest == consumed;
								});
						});
					if (!complete_membership)
						return unexpected(
							store_error("store.derived-basis-partition-missing", value.content));
				}
				candidate->rows[value.descriptor].push_back(value.row);
				candidate->annotations[value.descriptor].push_back({value.row,
																	value.presence,
																	value.interpretation,
																	value.semantic_key,
																	value.assertion,
																	value.content,
																	value.producer,
																	value.provenance_root,
																	value.guarantee});
				candidate->claim_contents.push_back(value.content);
			}
			candidate->unresolved.insert(candidate->unresolved.end(),
										 partition.unresolved.begin(),
										 partition.unresolved.end());
		}
		std::ranges::sort(manifest.partitions, {}, &partition_manifest::partition_id);
		std::ranges::sort(
			candidate->partition_bindings, {}, &snapshot_partition_binding::partition_id);
		for (const auto& closure : data_->closures)
		{
			const auto subject = std::ranges::find(manifest.partitions,
												   closure.subject_partition_id,
												   &partition_manifest::partition_id);
			if (subject == manifest.partitions.end())
				return unexpected(
					store_error("store.closure-subject-missing", closure.subject_partition_id));
			const auto binding = std::ranges::find(candidate->partition_bindings,
												   subject->partition_id,
												   &snapshot_partition_binding::partition_id);
			if (binding == candidate->partition_bindings.end())
				return unexpected(
					store_error("store.closure-subject-missing", closure.subject_partition_id));
			auto validation_subject = make_partition_certificate_subject(*subject, *binding);
			if (!validation_subject)
				return unexpected(std::move(validation_subject.error()));
			auto certificate = make_closure_certificate(*validation_subject, closure);
			if (!certificate)
				return unexpected(std::move(certificate.error()));
			manifest.closure_ids.push_back(certificate->id);
			candidate->closure_certificates.push_back(std::move(*certificate));
		}
		std::ranges::sort(manifest.closure_ids);
		std::ranges::sort(candidate->closure_certificates, {}, &closure_certificate::id);
		if (std::ranges::adjacent_find(manifest.closure_ids) != manifest.closure_ids.end())
			return unexpected(store_error("store.closure-duplicate", "closures"));
		std::ranges::sort(candidate->claim_contents);
		std::ranges::sort(
			candidate->coverage,
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
			candidate->unresolved,
			[](const unresolved_reference& left, const unresolved_reference& right)
			{
				return std::tie(left.source_assertion, left.target_relation, left.reason) <
					std::tie(right.source_assertion, right.target_relation, right.reason);
			});
		for (auto& [descriptor, rows] : candidate->rows)
		{
			auto relation = data_->store->engine.require_id(descriptor);
			if (!relation)
				return unexpected(std::move(relation.error()));
			candidate->descriptors.emplace(descriptor, relation->descriptor());
			std::ranges::sort(rows,
							  [](const detached_row& left, const detached_row& right)
							  {
								  return left.canonical_form() < right.canonical_form();
							  });
			auto& annotations = candidate->annotations[descriptor];
			std::ranges::sort(
				annotations,
				[](const snapshot_claim_annotation& left, const snapshot_claim_annotation& right)
				{
					return annotation_projection(left) < annotation_projection(right);
				});
		}
		if (auto valid = validate_semantic_graph(*candidate, data_->store->engine); !valid)
			return unexpected(std::move(valid.error()));
		manifest.id = snapshot_identity(manifest);
		data_->candidate = std::move(candidate);
		data_->current_state = publication_state::validating;
		return {};
	}

	result<snapshot_handle> snapshot_writer::publish()
	{
		if (!data_ || data_->current_state != publication_state::validating || !data_->candidate)
			return unexpected(store_error("store.transaction-state", "publish"));
		auto& store = *data_->store;
		std::scoped_lock lock{store.mutex};
		const auto series_id = data_->draft.series.id();
		const auto head = store.heads.find(series_id);
		const std::optional<std::string> current_parent = head == store.heads.end()
			? std::optional<std::string>{}
			: std::optional<std::string>{head->second};
		if (current_parent != data_->draft.expected_parent_publication)
		{
			data_->current_state = publication_state::rejected;
			return unexpected(store_error("store.publish-stale-parent", series_id));
		}
		const auto sequence =
			head == store.heads.end() ? 1U : store.records.at(head->second).sequence + 1U;
		const auto generation = store.generation + 1U;
		publication_record record;
		record.series_id = series_id;
		record.snapshot_id = data_->candidate->semantic_manifest.id;
		record.sequence = sequence;
		record.physical_generation = generation;
		record.parent_publication = current_parent;
		record.state = publication_state::committed;
		record.publication_id = publication_identity(record);
		data_->candidate->publication_record_value = record;
		const bool collision = std::ranges::any_of(
			store.publications,
			[&](const auto& existing)
			{
				return existing.second->semantic_manifest.id ==
					data_->candidate->semantic_manifest.id &&
					canonical_export_of(*existing.second) != canonical_export_of(*data_->candidate);
			});
		if (collision)
		{
			data_->current_state = publication_state::rejected;
			return unexpected(store_error("store.hash-collision", record.snapshot_id));
		}
		auto token = std::make_shared<const std::uint64_t>(generation);
		data_->candidate->generation_pin = token;
		if (auto persisted = store.persist_new(*data_->candidate, current_parent); !persisted)
		{
			data_->current_state = publication_state::rolled_back;
			return unexpected(std::move(persisted.error()));
		}
		store.generation = generation;
		store.generation_tokens.push_back(token);
		store.records[record.publication_id] = record;
		store.publications[record.publication_id] = data_->candidate;
		store.heads[series_id] = record.publication_id;
		data_->current_state = publication_state::committed;
		return snapshot_handle{data_->candidate};
	}
	void snapshot_writer::cancel() noexcept
	{
		if (!data_)
			return;
		data_->candidate.reset();
		data_->partitions.clear();
		data_->closures.clear();
		data_->current_state = publication_state::rolled_back;
	}

	result<snapshot_store> make_in_memory_snapshot_store(relation_engine engine)
	{
		return snapshot_store{
			std::make_shared<snapshot_store::implementation>(std::move(engine), "memory")};
	}

	result<snapshot_store> open_sqlite_snapshot_store(const std::string& database_path,
													  relation_engine engine)
	{
		if (database_path.empty())
			return unexpected(store_error("store.sqlite-path-empty", "database_path"));
		auto database = open_database(database_path);
		if (!database)
			return unexpected(std::move(database.error()));
		auto implementation =
			std::make_shared<snapshot_store::implementation>(std::move(engine), "sqlite");
		implementation->database = std::move(*database);
		if (auto loaded = implementation->load(); !loaded)
			return unexpected(std::move(loaded.error()));
		return snapshot_store{std::move(implementation)};
	}

	result<void> mark_publication_corrupt_for_testing(snapshot_store& store,
													  const std::string_view publication_id)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		const auto found = store.implementation_->publications.find(publication_id);
		if (found == store.implementation_->publications.end())
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto corrupted = std::make_shared<snapshot_handle::data>(*found->second);
		corrupted->publication_record_value.corrupt = true;
		if (auto persisted = store.implementation_->persist(*corrupted); !persisted)
			return unexpected(std::move(persisted.error()));
		store.implementation_->records[std::string{publication_id}] =
			corrupted->publication_record_value;
		store.implementation_->publications[std::string{publication_id}] = std::move(corrupted);
		return {};
	}

	// The raw before/after pair is intentionally symmetric in this test-only fault injector.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	result<void> rewrite_publication_payload_for_testing(snapshot_store& store,
														 const std::string_view publication_id,
														 const std::string_view before,
														 const std::string_view after,
														 const std::size_t occurrence)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		if (store.implementation_->database == nullptr || before.empty() ||
			before.size() != after.size())
			return unexpected(store_error("store.corrupt", "test-rewrite", "invalid"));
		auto selected = store.implementation_->database->query(
			"SELECT hex(payload) FROM cxxlens_ng_publication WHERE publication_id=" +
			sql_quote(publication_id));
		if (!selected || selected->size() != 1U || selected->front().size() != 1U)
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto payload = hex_bytes(selected->front().front());
		if (!payload)
			return unexpected(std::move(payload.error()));
		const auto needle = std::as_bytes(std::span{before.data(), before.size()});
		auto search_begin = payload->begin();
		decltype(search_begin) found;
		for (std::size_t index = 0U; index <= occurrence; ++index)
		{
			found = std::search(search_begin, payload->end(), needle.begin(), needle.end());
			if (found == payload->end())
				return unexpected(store_error("store.corrupt", "test-rewrite", "not-found"));
			search_begin = found + static_cast<std::ptrdiff_t>(needle.size());
		}
		for (std::size_t index = 0U; index < after.size(); ++index)
			found[static_cast<std::ptrdiff_t>(index)] =
				static_cast<std::byte>(static_cast<unsigned char>(after[index]));
		const auto checksum = content_digest(*payload);
		return store.implementation_->database->execute(
			"UPDATE cxxlens_ng_publication SET checksum=" + sql_quote(checksum) + ",payload=X'" +
			bytes_hex(*payload) + "' WHERE publication_id=" + sql_quote(publication_id));
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	snapshot_builder::snapshot_builder(relation_registry registry) : registry_{std::move(registry)}
	{
	}
	result<void> snapshot_builder::add(detached_row row)
	{
		const auto descriptors = registry_.descriptors();
		const auto descriptor =
			std::ranges::find(descriptors, row.descriptor_id, &relation_descriptor::id);
		if (descriptor == descriptors.end())
			return unexpected(store_error("sdk.snapshot-unknown-relation", row.descriptor_id));
		if (auto valid = validate_row(*descriptor, row); !valid)
			return valid;
		rows_[row.descriptor_id].push_back(std::move(row));
		return {};
	}
	result<snapshot_handle> snapshot_builder::publish() &&
	{
		auto value = std::make_shared<snapshot_handle::data>();
		std::vector<canonical_value> identity_rows;
		for (const auto& descriptor : registry_.descriptors())
			value->descriptors.emplace(descriptor.id, descriptor);
		for (auto& [descriptor, rows] : rows_)
		{
			std::ranges::sort(rows,
							  [](const detached_row& left, const detached_row& right)
							  {
								  return left.canonical_form() < right.canonical_form();
							  });
			for (const auto& row : rows)
				identity_rows.push_back(text(row.canonical_form()));
			value->rows.emplace(descriptor, std::move(rows));
		}
		const std::array fields{canonical_value::from_tuple(std::move(identity_rows))};
		value->semantic_manifest.id = *canonical_identity_digest("snapshot-compat", fields);
		value->semantic_manifest.catalog_semantic_digest =
			*semantic_digest("compat.catalog", "legacy");
		value->semantic_manifest.condition_universe_id = "compat-universe";
		value->semantic_manifest.relation_registry_digest =
			*semantic_digest("compat.registry", "legacy");
		value->semantic_manifest.interpretation_policy_digest =
			*semantic_digest("compat.policy", "legacy");
		value->publication_record_value = {*canonical_identity_digest("publication-compat", fields),
										   "compat-series",
										   value->semantic_manifest.id,
										   1U,
										   1U,
										   std::nullopt,
										   publication_state::committed,
										   false};
		value->generation_pin = std::make_shared<const std::uint64_t>(1U);
		return snapshot_handle{std::move(value)};
	}
} // namespace cxxlens::sdk
