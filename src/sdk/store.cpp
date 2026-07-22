#include <algorithm>
#include <array>
#include <bit>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <ranges>
#include <set>
#include <span>
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
#include "sqlite_connection_lifecycle_internal.hpp"
#include "sqlite_default_forwarding_vfs_internal.hpp"
#include "sqlite_limit_length_control_internal.hpp"
#include "sqlite_payload_streaming_internal.hpp"
#include "sqlite_store_fault_injection_internal.hpp"
#include "sqlite_store_terminal_internal.hpp"
#include "sqlite_terminal_reclassifier_internal.hpp"
#include "sqlite_wal_receipt_internal.hpp"
#include "sqlite_wal_source_capture_internal.hpp"
#include "store_backend_lifetime_internal.hpp"
#include "store_identity_internal.hpp"

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

		[[nodiscard]] bool sqlite_payload_digest(const std::string_view value)
		{
			if (!value.starts_with("sha256:") || value.size() != 71U)
				return false;
			return std::ranges::all_of(value.substr(7U),
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

		[[nodiscard]] result<std::uint64_t> checked_counter_increment(const std::uint64_t value,
																	  const std::string_view field)
		{
			if (value == std::numeric_limits<std::uint64_t>::max())
				return unexpected(store_error("store.counter-overflow", std::string{field}));
			return value + 1U;
		}

		[[nodiscard]] std::string canonical_export_of(const snapshot_handle::data& value);

		class binary_writer
		{
		  public:
			binary_writer() = default;
			explicit binary_writer(sqlite_bounded_byte_sink& sink) noexcept : sink_{&sink} {}

			void unsigned_value(std::uint64_t value)
			{
				std::array<std::byte, 8U> encoded{};
				std::size_t index{};
				for (int shift = 56; shift >= 0; shift -= 8)
					encoded[index++] = static_cast<std::byte>((value >> shift) & 0xffU);
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
				if (sink_ == nullptr)
					return unexpected(store_error("store.corrupt", "payload", "writer-mode"));
				if (failure_)
					return unexpected(*failure_);
				return {};
			}

		  private:
			void append(const std::span<const std::byte> value)
			{
				if (failure_)
					return;
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

		[[nodiscard]] std::string sqlite_unsigned(const std::uint64_t value)
		{
			if (value <= static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()))
				return std::to_string(value);
			const auto magnitude = std::numeric_limits<std::uint64_t>::max() - value + 1U;
			return "-" + std::to_string(magnitude);
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

		enum class snapshot_payload_schema : std::uint8_t
		{
			v1 = 1U,
			v2 = 2U,
			v3 = 3U,
			v4 = 4U,
			v5 = 5U,
		};

		[[nodiscard]] constexpr std::optional<snapshot_payload_schema>
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

		void encode_snapshot(binary_writer& writer,
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

		[[nodiscard]] std::vector<std::byte> encode_snapshot(const snapshot_handle::data& value)
		{
			binary_writer writer;
			encode_snapshot(writer, value, snapshot_payload_schema::v5);
			return std::move(writer).finish();
		}

		[[nodiscard]] std::vector<std::byte>
		encode_snapshot(const snapshot_handle::data& value,
						const snapshot_payload_schema payload_schema)
		{
			binary_writer writer;
			encode_snapshot(writer, value, payload_schema);
			return std::move(writer).finish();
		}

		[[nodiscard]] result<void> encode_snapshot(const snapshot_handle::data& value,
												   sqlite_bounded_byte_sink& sink)
		{
			binary_writer writer{sink};
			encode_snapshot(writer, value, snapshot_payload_schema::v5);
			return writer.finish_stream();
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
		decode_snapshot(binary_reader& reader,
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
			auto id = reader.string();
			auto major = reader.unsigned_32("snapshot-version-major");
			auto minor = reader.unsigned_32("snapshot-version-minor");
			auto patch = reader.unsigned_32("snapshot-version-patch");
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
			auto finished = reader.finished();
			if (!finished || !*finished || manifest.schema != "cxxlens.snapshot-manifest.v1" ||
				manifest.id != snapshot_identity(manifest) ||
				publication.snapshot_id != manifest.id)
				return unexpected(store_error("store.corrupt", "payload", "semantic-digest"));
			if (payload_has_partition_envelopes && canonical_input)
			{
				const auto canonical = encode_snapshot(*value);
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
			std::array<std::byte, 64U * 1024U> scratch_{};
			std::uint64_t expected_size_{};
			std::uint64_t offset_{};
		};

		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		decode_snapshot(const sqlite_replayable_byte_source& source,
						const std::uint64_t expected_size,
						const relation_engine& engine)
		{
			auto first_pass = source.open_pass();
			if (!first_pass)
				return unexpected(std::move(first_pass.error()));
			binary_reader reader{**first_pass, expected_size};
			bool canonical_required{};
			auto value = decode_snapshot(reader, engine, std::nullopt, &canonical_required);
			if (!value)
				return unexpected(std::move(value.error()));
			if (!canonical_required)
				return value;

			auto comparison_pass = source.open_pass();
			if (!comparison_pass)
				return unexpected(std::move(comparison_pass.error()));
			sqlite_byte_comparison_sink comparison{std::move(*comparison_pass), expected_size};
			if (auto encoded = encode_snapshot(**value, comparison); !encoded)
				return unexpected(std::move(encoded.error()));
			if (auto compared = comparison.finish(); !compared)
				return unexpected(std::move(compared.error()));
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
		constexpr int sqlite_ok = 0;
		constexpr int sqlite_row = 100;
		constexpr int sqlite_done = 101;
		constexpr int sqlite_integer = 1;
		constexpr int sqlite_text = 3;
		constexpr int sqlite_blob = 4;
		constexpr int sqlite_null = 5;
		constexpr int sqlite_readonly = 8;
		constexpr int sqlite_readonly_cantinit = sqlite_readonly | (5 << 8);
		constexpr int sqlite_open_readonly = 0x00000001;
		constexpr int sqlite_open_readwrite = 0x00000002;
		constexpr int sqlite_open_create = 0x00000004;
		constexpr int sqlite_open_uri = 0x00000040;
		constexpr int sqlite_open_main_database = 0x00000100;
		constexpr int sqlite_open_privatecache = 0x00040000;
		constexpr int sqlite_open_write_ahead_log = 0x00080000;
		constexpr int sqlite_open_fullmutex = 0x00010000;
		constexpr int sqlite_wal_read_lock_base = 3;
		constexpr int sqlite_wal_read_lock_count = 5;
		constexpr unsigned int sqlite_prepare_persistent = 0x01U;
		constexpr std::size_t sqlite_payload_chunk_maximum = 8U * 1024U * 1024U;
		thread_local bool sqlite_source_shm_symbols_available_for_testing = true;

		enum class sqlite_physical_format : std::uint8_t
		{
			predecessor_v2,
			current_v3,
		};

		[[nodiscard]] sqlite_last_validated_compatibility
		validated_store_compatibility(const std::string_view backend,
									  const sqlite_physical_format format) noexcept
		{
			if (backend != "sqlite")
				return {{2U, 6U, 0U}, true, false};
			if (format == sqlite_physical_format::predecessor_v2)
				return {{2U, 6U, 0U}, true, true};
			return {{3U, 0U, 0U}, true, false};
		}

		struct sqlite_api
		{
			using open_fn = int (*)(const char*, void**, int, const char*);
			using close_fn = int (*)(void*);
			using errmsg_fn = const char* (*)(void*);
			using exec_fn =
				int (*)(void*, const char*, int (*)(void*, int, char**, char**), void*, char**);
			using free_fn = void (*)(void*);
			using prepare_v2_fn = int (*)(void*, const char*, int, void**, const char**);
			using prepare_v3_fn =
				int (*)(void*, const char*, int, unsigned int, void**, const char**);
			using step_fn = int (*)(void*);
			using finalize_fn = int (*)(void*);
			using reset_fn = int (*)(void*);
			using bind_text_fn = int (*)(void*, int, const char*, int, void (*)(void*));
			using bind_int64_fn = int (*)(void*, int, std::int64_t);
			using bind_blob64_fn = int (*)(void*, int, const void*, std::uint64_t, void (*)(void*));
			using bind_null_fn = int (*)(void*, int);
			using column_type_fn = int (*)(void*, int);
			using column_text_fn = const unsigned char* (*)(void*, int);
			using column_blob_fn = const void* (*)(void*, int);
			using column_bytes_fn = int (*)(void*, int);
			using column_int64_fn = std::int64_t (*)(void*, int);
			using libversion_number_fn = int (*)();
			using sourceid_fn = const char* (*)();
			using uri_parameter_fn = const char* (*)(const char*, const char*);
			using uri_key_fn = const char* (*)(const char*, int);
			using limit_fn = int (*)(void*, int, int);
			using vfs_find_fn = void* (*)(const char*);
			using vfs_register_fn = int (*)(void*, int);
			using vfs_unregister_fn = int (*)(void*);
			using blob_open_fn =
				int (*)(void*, const char*, const char*, const char*, std::int64_t, int, void**);
			using blob_read_fn = int (*)(void*, void*, int, int);
			using blob_bytes_fn = int (*)(void*);
			using blob_close_fn = int (*)(void*);
			void* library{};
			const void* runtime_identity{};
			std::shared_ptr<void> retained_runtime_lifetime;
			open_fn open{};
			close_fn close{};
			errmsg_fn errmsg{};
			exec_fn exec{};
			free_fn free_memory{};
			prepare_v2_fn prepare_v2{};
			step_fn step{};
			finalize_fn finalize{};
			column_type_fn column_type{};
			column_text_fn column_text{};
			column_blob_fn column_blob{};
			column_bytes_fn column_bytes{};
			column_int64_fn column_int64{};
			prepare_v3_fn prepare_v3{};
			reset_fn reset{};
			bind_text_fn bind_text{};
			bind_int64_fn bind_int64{};
			bind_blob64_fn bind_blob64{};
			bind_null_fn bind_null{};
			libversion_number_fn libversion_number{};
			sourceid_fn sourceid{};
			uri_parameter_fn uri_parameter{};
			uri_key_fn uri_key{};
			limit_fn limit{};
			vfs_find_fn vfs_find{};
			vfs_register_fn vfs_register{};
			vfs_unregister_fn vfs_unregister{};
			blob_open_fn blob_open{};
			blob_read_fn blob_read{};
			blob_bytes_fn blob_bytes{};
			blob_close_fn blob_close{};
			bool owns_library{};
			bool v3_symbols_ready{};
			bool source_shm_readonly_symbols_ready{};
			const void* loader_image_identity{};
			[[nodiscard]] const void* runtime_lifetime_identity() const noexcept
			{
				return retained_runtime_lifetime ? retained_runtime_lifetime.get() : this;
			}
			~sqlite_api()
			{
#if defined(__unix__) || defined(__APPLE__)
				if (owns_library && library != nullptr)
					dlclose(library);
#endif
			}
		};

		template <class Function>
		[[nodiscard]] bool resolve_sqlite(void* library, const char* name, Function& output);

		template <class Function>
		[[nodiscard]] const void* sqlite_loader_image(Function function) noexcept
		{
#if defined(__unix__) || defined(__APPLE__)
			if (function == nullptr)
				return nullptr;
			Dl_info information{};
			if (::dladdr(reinterpret_cast<const void*>(function), &information) == 0 ||
				information.dli_fbase == nullptr)
				return nullptr;
			return information.dli_fbase;
#else
			(void)function;
			return nullptr;
#endif
		}

		[[nodiscard]] result<void> require_source_shm_readonly_symbols(sqlite_api& api)
		{
			if (api.source_shm_readonly_symbols_ready)
				return {};
			if (!sqlite_source_shm_symbols_available_for_testing)
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			sqlite_api::sourceid_fn sourceid{};
			sqlite_api::uri_parameter_fn uri_parameter{};
			sqlite_api::uri_key_fn uri_key{};
			if (!resolve_sqlite(api.library, "sqlite3_sourceid", sourceid) ||
				!resolve_sqlite(api.library, "sqlite3_uri_parameter", uri_parameter) ||
				!resolve_sqlite(api.library, "sqlite3_uri_key", uri_key))
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			const auto* loader_image = sqlite_loader_image(api.open);
			if (loader_image == nullptr || sqlite_loader_image(sourceid) != loader_image ||
				sqlite_loader_image(uri_parameter) != loader_image ||
				sqlite_loader_image(uri_key) != loader_image)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			const char* source_id = sourceid();
			if (source_id == nullptr || *source_id == '\0')
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			api.sourceid = sourceid;
			api.uri_parameter = uri_parameter;
			api.uri_key = uri_key;
			api.loader_image_identity = loader_image;
			api.source_shm_readonly_symbols_ready = true;
			return {};
		}

		[[nodiscard]] result<sqlite_source_shm_runtime_binding>
		bind_source_shm_readonly_runtime(const std::shared_ptr<sqlite_api>& api)
		{
			if (!api)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			if (auto resolved = require_source_shm_readonly_symbols(*api); !resolved)
				return unexpected(std::move(resolved.error()));
			std::shared_ptr<void> pinned_runtime_lifetime = api->retained_runtime_lifetime
				? api->retained_runtime_lifetime
				: std::static_pointer_cast<void>(api);
			return sqlite_source_shm_runtime_binding{
				api->runtime_identity,
				api->loader_image_identity,
				api->runtime_lifetime_identity(),
				std::move(pinned_runtime_lifetime),
				api->open,
				api->close,
				api->exec,
				api->errmsg,
				api->free_memory,
				api->sourceid,
				api->uri_parameter,
				api->uri_key,
				api->vfs_find,
				api->vfs_register,
				api->vfs_unregister,
			};
		}

		[[nodiscard]] result<void> require_v3_symbols(sqlite_api& api)
		{
			if (api.v3_symbols_ready)
				return {};
			if (!resolve_sqlite(api.library, "sqlite3_libversion_number", api.libversion_number) ||
				!resolve_sqlite(api.library, "sqlite3_limit", api.limit) ||
				!resolve_sqlite(api.library, "sqlite3_prepare_v3", api.prepare_v3) ||
				!resolve_sqlite(api.library, "sqlite3_reset", api.reset) ||
				!resolve_sqlite(api.library, "sqlite3_bind_int64", api.bind_int64) ||
				!resolve_sqlite(api.library, "sqlite3_bind_blob64", api.bind_blob64) ||
				!resolve_sqlite(api.library, "sqlite3_bind_null", api.bind_null))
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			if (api.libversion_number() < 3'037'000)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite-runtime-version", "3037000"));
			api.v3_symbols_ready = true;
			return {};
		}

		class sqlite_library_guard
		{
		  public:
			explicit sqlite_library_guard(void* library) noexcept : library_{library} {}
			sqlite_library_guard(const sqlite_library_guard&) = delete;
			sqlite_library_guard& operator=(const sqlite_library_guard&) = delete;
			~sqlite_library_guard()
			{
#if defined(__unix__) || defined(__APPLE__)
				if (library_ != nullptr)
					(void)dlclose(library_);
#endif
			}
			[[nodiscard]] void* release() noexcept
			{
				return std::exchange(library_, nullptr);
			}

		  private:
			void* library_{};
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

		[[nodiscard]] bool resolve_base_sqlite_symbols(sqlite_api& api)
		{
			return resolve_sqlite(api.library, "sqlite3_open_v2", api.open) &&
				resolve_sqlite(api.library, "sqlite3_close_v2", api.close) &&
				resolve_sqlite(api.library, "sqlite3_errmsg", api.errmsg) &&
				resolve_sqlite(api.library, "sqlite3_exec", api.exec) &&
				resolve_sqlite(api.library, "sqlite3_free", api.free_memory) &&
				resolve_sqlite(api.library, "sqlite3_prepare_v2", api.prepare_v2) &&
				resolve_sqlite(api.library, "sqlite3_step", api.step) &&
				resolve_sqlite(api.library, "sqlite3_finalize", api.finalize) &&
				resolve_sqlite(api.library, "sqlite3_column_type", api.column_type) &&
				resolve_sqlite(api.library, "sqlite3_column_text", api.column_text) &&
				resolve_sqlite(api.library, "sqlite3_column_blob", api.column_blob) &&
				resolve_sqlite(api.library, "sqlite3_column_bytes", api.column_bytes) &&
				resolve_sqlite(api.library, "sqlite3_column_int64", api.column_int64) &&
				resolve_sqlite(api.library, "sqlite3_bind_text", api.bind_text) &&
				resolve_sqlite(api.library, "sqlite3_bind_int64", api.bind_int64) &&
				resolve_sqlite(api.library, "sqlite3_bind_blob64", api.bind_blob64) &&
				resolve_sqlite(api.library, "sqlite3_bind_null", api.bind_null) &&
				resolve_sqlite(api.library, "sqlite3_vfs_find", api.vfs_find) &&
				resolve_sqlite(api.library, "sqlite3_vfs_register", api.vfs_register) &&
				resolve_sqlite(api.library, "sqlite3_vfs_unregister", api.vfs_unregister) &&
				resolve_sqlite(api.library, "sqlite3_blob_open", api.blob_open) &&
				resolve_sqlite(api.library, "sqlite3_blob_read", api.blob_read) &&
				resolve_sqlite(api.library, "sqlite3_blob_bytes", api.blob_bytes) &&
				resolve_sqlite(api.library, "sqlite3_blob_close", api.blob_close);
		}

		[[nodiscard]] result<std::shared_ptr<sqlite_api>>
		bind_sqlite_runtime(sqlite_backend_runtime_binding binding)
		{
#if defined(__unix__) || defined(__APPLE__)
			if (binding.native_library_handle == nullptr || binding.runtime_identity == nullptr ||
				!binding.runtime_lifetime)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			auto api = std::make_shared<sqlite_api>();
			api->library = binding.native_library_handle;
			api->runtime_identity = binding.runtime_identity;
			api->retained_runtime_lifetime = std::move(binding.runtime_lifetime);
			if (!resolve_base_sqlite_symbols(*api))
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			return api;
#else
			(void)binding;
			return unexpected(store_error("store.backend-unavailable", "sqlite", "platform"));
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
			sqlite_library_guard library_guard{library};
			auto api = std::make_shared<sqlite_api>();
			api->library = library_guard.release();
			api->runtime_identity = library;
			api->owns_library = true;
			if (!resolve_base_sqlite_symbols(*api))
				return unexpected(store_error("store.backend-unavailable", "sqlite", "symbols"));
			return api;
#else
			return unexpected(store_error("store.backend-unavailable", "sqlite", "platform"));
#endif
		}

		class sqlite_database
		{
		  public:
			sqlite_database(std::shared_ptr<sqlite_api> api, sqlite_connection_lifecycle connection)
				: api_{std::move(api)}, connection_{std::move(connection)}
			{
			}
			[[nodiscard]] result<void> execute(const std::string& sql) const
			{
				char* message{};
				const auto code =
					api_->exec(connection_.get(), sql.c_str(), nullptr, nullptr, &message);
				if (code == 0)
					return {};
				std::string detail = message != nullptr ? message : api_->errmsg(connection_.get());
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
				const auto code =
					api_->exec(connection_.get(), sql.c_str(), callback, &rows, &message);
				if (code == 0)
					return rows;
				std::string detail = message != nullptr ? message : api_->errmsg(connection_.get());
				if (message != nullptr)
					api_->free_memory(message);
				return unexpected(
					store_error("store.sqlite-failure", "database", std::move(detail)));
			}
			[[nodiscard]] void* handle() const noexcept
			{
				return connection_.get();
			}
			[[nodiscard]] const std::shared_ptr<sqlite_api>& api() const noexcept
			{
				return api_;
			}
			[[nodiscard]] std::string message() const
			{
				const auto* value = api_->errmsg(connection_.get());
				return value != nullptr ? value : "sqlite";
			}
			[[nodiscard]] sqlite_connection_close_outcome close_exactly_once() noexcept
			{
				return connection_.close_exactly_once();
			}

		  private:
			std::shared_ptr<sqlite_api> api_;
			sqlite_connection_lifecycle connection_;
		};

		[[nodiscard]] bool
		confirmed_connection_close(const sqlite_connection_close_outcome& outcome) noexcept
		{
			if (!std::holds_alternative<sqlite_confirmed_close_token>(outcome))
				return false;
			const auto& token = std::get<sqlite_confirmed_close_token>(outcome);
			return token.valid() && token.kind() == sqlite_confirmed_close_kind::sqlite_ok &&
				token.close_was_attempted();
		}

		[[nodiscard]] bool confirmed_failed_open_resolution(
			const sqlite_connection_close_outcome& outcome) noexcept
		{
			if (!std::holds_alternative<sqlite_confirmed_close_token>(outcome))
				return false;
			const auto& token = std::get<sqlite_confirmed_close_token>(outcome);
			return token.valid() &&
				((token.kind() == sqlite_confirmed_close_kind::sqlite_ok &&
				  token.close_was_attempted()) ||
				 (token.kind() == sqlite_confirmed_close_kind::no_connection &&
				  !token.close_was_attempted()));
		}

		class sqlite_statement
		{
		  public:
			sqlite_statement(sqlite_statement&& other) noexcept
				: api_{std::move(other.api_)}, database_{other.database_},
				  statement_{std::exchange(other.statement_, nullptr)}
			{
			}
			sqlite_statement& operator=(sqlite_statement&& other) noexcept
			{
				if (this == &other)
					return *this;
				if (statement_ != nullptr)
					(void)api_->finalize(statement_);
				api_ = std::move(other.api_);
				database_ = other.database_;
				statement_ = std::exchange(other.statement_, nullptr);
				return *this;
			}
			sqlite_statement(const sqlite_statement&) = delete;
			sqlite_statement& operator=(const sqlite_statement&) = delete;
			~sqlite_statement()
			{
				if (statement_ != nullptr)
					(void)api_->finalize(statement_);
			}

			[[nodiscard]] static result<sqlite_statement>
			prepare(sqlite_database& database, const std::string_view sql, const bool use_v3)
			{
				if (sql.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
					return unexpected(
						store_error("store.sqlite-failure", "database", "sql-length"));
				void* statement{};
				const auto code = use_v3 ? database.api()->prepare_v3(database.handle(),
																	  sql.data(),
																	  static_cast<int>(sql.size()),
																	  sqlite_prepare_persistent,
																	  &statement,
																	  nullptr)
										 : database.api()->prepare_v2(database.handle(),
																	  sql.data(),
																	  static_cast<int>(sql.size()),
																	  &statement,
																	  nullptr);
				if (code != sqlite_ok || statement == nullptr)
					return unexpected(
						store_error("store.sqlite-failure", "database", database.message()));
				return sqlite_statement{database.api(), database.handle(), statement};
			}

			[[nodiscard]] result<void> bind_text(const int index, const std::string_view value)
			{
				if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
					api_->bind_text(statement_,
									index,
									value.data(),
									static_cast<int>(value.size()),
									sqlite_transient()) != sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] result<void> bind_unsigned(const int index, const std::uint64_t value)
			{
				const auto encoded = std::bit_cast<std::int64_t>(value);
				if (api_->bind_int64(statement_, index, encoded) != sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] result<void> bind_blob(const int index,
												 const std::span<const std::byte> value)
			{
				const void* data = value.empty() ? nullptr : value.data();
				if (api_->bind_blob64(statement_, index, data, value.size(), sqlite_transient()) !=
					sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] result<void> bind_null_value(const int index)
			{
				if (api_->bind_null(statement_, index) != sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] int step() const
			{
				return api_->step(statement_);
			}
			[[nodiscard]] result<void> expect_done() const
			{
				if (step() != sqlite_done)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] result<void> reset() const
			{
				if (api_->reset(statement_) != sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] result<void> finalize_exactly_once()
			{
				if (statement_ == nullptr)
					return unexpected(
						store_error("store.sqlite-failure", "database", "statement-finalized"));
				auto* statement = std::exchange(statement_, nullptr);
				if (api_->finalize(statement) != sqlite_ok)
					return unexpected(
						store_error("store.sqlite-failure", "database", database_message()));
				return {};
			}
			[[nodiscard]] int column_type(const int index) const
			{
				return api_->column_type(statement_, index);
			}
			[[nodiscard]] result<std::string> column_text(const int index) const
			{
				if (column_type(index) != sqlite_text)
					return unexpected(store_error("store.corrupt", "sqlite", "storage-class"));
				const auto* value = api_->column_text(statement_, index);
				const auto size = api_->column_bytes(statement_, index);
				if (value == nullptr || size < 0)
					return unexpected(store_error("store.corrupt", "sqlite", "text"));
				return std::string{reinterpret_cast<const char*>(value),
								   static_cast<std::size_t>(size)};
			}
			[[nodiscard]] result<std::optional<std::string>>
			column_optional_text(const int index) const
			{
				if (column_type(index) == sqlite_null)
					return std::optional<std::string>{};
				auto value = column_text(index);
				if (!value)
					return unexpected(std::move(value.error()));
				return std::optional<std::string>{std::move(*value)};
			}
			[[nodiscard]] result<std::uint64_t> column_unsigned(const int index) const
			{
				if (column_type(index) != sqlite_integer)
					return unexpected(store_error("store.corrupt", "sqlite", "storage-class"));
				return std::bit_cast<std::uint64_t>(api_->column_int64(statement_, index));
			}
			[[nodiscard]] result<std::int64_t> column_signed(const int index) const
			{
				if (column_type(index) != sqlite_integer)
					return unexpected(store_error("store.corrupt", "sqlite", "storage-class"));
				return api_->column_int64(statement_, index);
			}
			[[nodiscard]] result<std::vector<std::byte>> column_blob(const int index) const
			{
				if (column_type(index) != sqlite_blob)
					return unexpected(store_error("store.corrupt", "sqlite", "storage-class"));
				const auto size = api_->column_bytes(statement_, index);
				const auto* value = api_->column_blob(statement_, index);
				if (size < 0 || (size != 0 && value == nullptr))
					return unexpected(store_error("store.corrupt", "sqlite", "blob"));
				const auto* begin = static_cast<const std::byte*>(value);
				return size == 0 ? std::vector<std::byte>{}
								 : std::vector<std::byte>{begin, begin + size};
			}
			[[nodiscard]] result<std::span<const std::byte>> column_blob_view(const int index) const
			{
				if (column_type(index) != sqlite_blob)
					return unexpected(store_error("store.corrupt", "sqlite", "storage-class"));
				const auto size = api_->column_bytes(statement_, index);
				const auto* value = api_->column_blob(statement_, index);
				if (size < 0 || (size != 0 && value == nullptr))
					return unexpected(store_error("store.corrupt", "sqlite", "blob"));
				return std::span{static_cast<const std::byte*>(value),
								 static_cast<std::size_t>(size)};
			}

		  private:
			sqlite_statement(std::shared_ptr<sqlite_api> api, void* database, void* statement)
				: api_{std::move(api)}, database_{database}, statement_{statement}
			{
			}
			[[nodiscard]] std::string database_message() const
			{
				const auto* value = api_->errmsg(database_);
				return value != nullptr ? value : "sqlite";
			}
			[[nodiscard]] static void (*sqlite_transient())(void*)
			{
				return reinterpret_cast<void (*)(void*)>(static_cast<std::intptr_t>(-1));
			}

			std::shared_ptr<sqlite_api> api_;
			void* database_{};
			void* statement_{};
		};

		class sqlite_blob_byte_source final : public sqlite_bounded_byte_source
		{
		  public:
			[[nodiscard]] static result<std::unique_ptr<sqlite_blob_byte_source>>
			open(sqlite_database& database,
				 const std::int64_t rowid,
				 const std::uint64_t expected_size)
			{
				if (expected_size > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
					return unexpected(store_error("store.format-incompatible",
												  "sqlite-physical-format",
												  "v2-profile-mismatch"));
				void* blob{};
				if (database.api()->blob_open(database.handle(),
											  "main",
											  "cxxlens_ng_publication",
											  "payload",
											  rowid,
											  0,
											  &blob) != sqlite_ok ||
					blob == nullptr)
					return unexpected(store_error("store.sqlite-failure", "database", "blob-open"));
				const auto close_on_failure = [&]()
				{
					(void)database.api()->blob_close(blob);
				};
				const auto byte_count = database.api()->blob_bytes(blob);
				if (byte_count < 0 || static_cast<std::uint64_t>(byte_count) != expected_size)
				{
					close_on_failure();
					return unexpected(store_error("store.format-incompatible",
												  "sqlite-physical-format",
												  "v2-profile-mismatch"));
				}
				return std::unique_ptr<sqlite_blob_byte_source>{
					new sqlite_blob_byte_source{database.api(), blob, expected_size}};
			}

			sqlite_blob_byte_source(const sqlite_blob_byte_source&) = delete;
			sqlite_blob_byte_source& operator=(const sqlite_blob_byte_source&) = delete;
			~sqlite_blob_byte_source() override
			{
				if (blob_ != nullptr)
					(void)api_->blob_close(blob_);
			}

			[[nodiscard]] result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (output.empty())
					return unexpected(store_error(
						"store.transaction-state", "sqlite-payload-stream", "empty-window"));
				if (finished_)
					return std::size_t{};
				if (offset_ == expected_size_)
				{
					finished_ = true;
					return std::size_t{};
				}
				const auto count = static_cast<int>(
					std::min<std::uint64_t>(output.size(), expected_size_ - offset_));
				if (offset_ > static_cast<std::uint64_t>(std::numeric_limits<int>::max()) ||
					api_->blob_read(blob_, output.data(), count, static_cast<int>(offset_)) !=
						sqlite_ok)
					return unexpected(store_error("store.sqlite-failure", "database", "blob-read"));
				offset_ += static_cast<std::uint64_t>(count);
				return static_cast<std::size_t>(count);
			}

		  private:
			sqlite_blob_byte_source(std::shared_ptr<sqlite_api> api,
									void* blob,
									const std::uint64_t expected_size)
				: api_{std::move(api)}, blob_{blob}, expected_size_{expected_size}
			{
			}

			std::shared_ptr<sqlite_api> api_;
			void* blob_{};
			std::uint64_t expected_size_{};
			std::uint64_t offset_{};
			bool finished_{};
		};

		class sqlite_v2_payload_source final : public sqlite_replayable_byte_source
		{
		  public:
			sqlite_v2_payload_source(sqlite_database& database,
									 const std::int64_t rowid,
									 const std::uint64_t byte_count)
				: database_{&database}, rowid_{rowid}, byte_count_{byte_count}
			{
			}

			[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>>
			open_pass() const override
			{
				if (database_ == nullptr)
					return unexpected(store_error("store.corrupt", "sqlite", "v2-payload-source"));
				auto opened = sqlite_blob_byte_source::open(*database_, rowid_, byte_count_);
				if (!opened)
					return unexpected(std::move(opened.error()));
				return std::unique_ptr<sqlite_bounded_byte_source>{std::move(*opened)};
			}

		  private:
			sqlite_database* database_{};
			std::int64_t rowid_{};
			std::uint64_t byte_count_{};
		};

		[[nodiscard]] result<std::unique_ptr<sqlite_database>>
		open_database(std::shared_ptr<sqlite_api> api,
					  const std::string& path,
					  const char* vfs_name,
					  const int flags,
					  sqlite_connection_lifetime_pins pins = {},
					  std::optional<sqlite_connection_close_outcome>* failed_open_close = nullptr)
		{
			if (!pins.runtime)
				pins.runtime = api;
			sqlite_connection_lifecycle connection{nullptr, api->close, std::move(pins)};
			auto** database_slot = connection.open_handle_out_parameter();
			if (database_slot == nullptr)
			{
				if (failed_open_close != nullptr)
					failed_open_close->emplace(connection.close_exactly_once());
				else
					(void)connection.close_exactly_once();
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "connection-lifecycle"));
			}
			const auto open_result = api->open(path.c_str(), database_slot, flags, vfs_name);
			if (open_result != 0 || connection.get() == nullptr)
			{
				std::string detail =
					connection.get() != nullptr ? api->errmsg(connection.get()) : "open";
				if (failed_open_close != nullptr)
					failed_open_close->emplace(connection.close_exactly_once());
				else
					(void)connection.close_exactly_once();
				return unexpected(store_error("store.sqlite-failure", "open", std::move(detail)));
			}
			auto output = std::make_unique<sqlite_database>(std::move(api), std::move(connection));
			return output;
		}

		template <class Operation>
		[[nodiscard]] result<void> with_immediate_transaction(sqlite_database& database,
															  Operation&& operation)
		{
			if (auto begun = database.execute("BEGIN IMMEDIATE;"); !begun)
				return begun;
			auto operated = std::forward<Operation>(operation)();
			if (!operated)
			{
				(void)database.execute("ROLLBACK;");
				return operated;
			}
			if (auto committed = database.execute("COMMIT;"); !committed)
			{
				(void)database.execute("ROLLBACK;");
				return committed;
			}
			return {};
		}

		constexpr std::array<std::string_view, 6U> sqlite_v3_ddl{
			"CREATE TABLE cxxlens_ng_metadata(key TEXT NOT NULL PRIMARY KEY,value TEXT NOT NULL) "
			"STRICT, WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_publication(publication_id TEXT NOT NULL PRIMARY "
			"KEY,series_id "
			"TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT NULL,generation INTEGER "
			"NOT NULL,parent TEXT,state INTEGER NOT NULL CHECK(state BETWEEN 0 AND 5),"
			"payload_checksum TEXT NOT NULL,payload_byte_count INTEGER NOT "
			"NULL,payload_chunk_count "
			"INTEGER NOT NULL CHECK(payload_chunk_count BETWEEN 0 AND 2199023255552)) STRICT, "
			"WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_payload_chunk(publication_id TEXT NOT NULL,generation INTEGER "
			"NOT NULL,chunk_ordinal INTEGER NOT NULL,byte_offset INTEGER NOT NULL,byte_count "
			"INTEGER "
			"NOT NULL,checksum TEXT NOT NULL,payload BLOB NOT NULL,PRIMARY KEY(publication_id,"
			"generation,chunk_ordinal),CHECK(chunk_ordinal BETWEEN 0 AND 2199023255551),"
			"CHECK(byte_count BETWEEN 1 AND 8388608),CHECK(length(payload)=byte_count)) STRICT, "
			"WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_series_head(series_id TEXT NOT NULL PRIMARY KEY,"
			"current_publication TEXT NOT NULL,sequence INTEGER NOT NULL) STRICT, WITHOUT ROWID",
			"CREATE INDEX cxxlens_ng_publication_series ON "
			"cxxlens_ng_publication(series_id,sequence)",
			"CREATE INDEX cxxlens_ng_payload_chunk_locator ON "
			"cxxlens_ng_payload_chunk(publication_id,generation,chunk_ordinal)",
		};

		constexpr std::array<std::string_view, 4U> sqlite_v2_ddl{
			"CREATE TABLE cxxlens_ng_metadata(key TEXT PRIMARY KEY,value TEXT NOT NULL)",
			"CREATE TABLE cxxlens_ng_publication(publication_id TEXT PRIMARY KEY,series_id TEXT "
			"NOT "
			"NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT NULL,generation INTEGER NOT NULL,"
			"parent TEXT,state INTEGER NOT NULL,checksum TEXT NOT NULL,payload BLOB NOT NULL)",
			"CREATE TABLE cxxlens_ng_series_head(series_id TEXT PRIMARY KEY,current_publication "
			"TEXT "
			"NOT NULL,sequence INTEGER NOT NULL)",
			"CREATE INDEX cxxlens_ng_publication_series ON "
			"cxxlens_ng_publication(series_id,sequence)",
		};

		constexpr std::array<std::string_view, 6U> sqlite_migration_shadow_ddl{
			"CREATE TABLE cxxlens_ng_migration_metadata(key TEXT NOT NULL PRIMARY KEY,value TEXT "
			"NOT NULL) STRICT, WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_migration_publication(publication_id TEXT NOT NULL PRIMARY "
			"KEY,series_id TEXT NOT NULL,snapshot_id TEXT NOT NULL,sequence INTEGER NOT NULL,"
			"generation INTEGER NOT NULL,parent TEXT,state INTEGER NOT NULL CHECK(state BETWEEN 0 "
			"AND 5),payload_checksum TEXT NOT NULL,payload_byte_count INTEGER NOT NULL,"
			"payload_chunk_count INTEGER NOT NULL CHECK(payload_chunk_count BETWEEN 0 AND "
			"2199023255552)) STRICT, WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_migration_payload_chunk(publication_id TEXT NOT NULL,"
			"generation INTEGER NOT NULL,chunk_ordinal INTEGER NOT NULL,byte_offset INTEGER NOT "
			"NULL,byte_count INTEGER NOT NULL,checksum TEXT NOT NULL,payload BLOB NOT NULL,PRIMARY "
			"KEY(publication_id,generation,chunk_ordinal),CHECK(chunk_ordinal BETWEEN 0 AND "
			"2199023255551),CHECK(byte_count BETWEEN 1 AND "
			"8388608),CHECK(length(payload)=byte_count)) "
			"STRICT, WITHOUT ROWID",
			"CREATE TABLE cxxlens_ng_migration_series_head(series_id TEXT NOT NULL PRIMARY KEY,"
			"current_publication TEXT NOT NULL,sequence INTEGER NOT NULL) STRICT, WITHOUT ROWID",
			"CREATE INDEX cxxlens_ng_migration_publication_series ON "
			"cxxlens_ng_migration_publication(series_id,sequence)",
			"CREATE INDEX cxxlens_ng_migration_payload_chunk_locator ON "
			"cxxlens_ng_migration_payload_chunk(publication_id,generation,chunk_ordinal)",
		};

		[[nodiscard]] result<std::map<std::string, std::string, std::less<>>>
		read_user_schema(sqlite_database& database)
		{
			auto selected = sqlite_statement::prepare(
				database,
				"SELECT name,sql FROM sqlite_schema WHERE sql IS NOT NULL AND "
				"name NOT LIKE 'sqlite_%' ORDER BY type,name",
				false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			std::map<std::string, std::string, std::less<>> output;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(store_error("store.sqlite-failure", "database", "schema"));
				auto name = selected->column_text(0);
				auto sql = selected->column_text(1);
				if (!name || !sql || !output.emplace(std::move(*name), std::move(*sql)).second)
					return unexpected(store_error("store.corrupt", "sqlite", "schema"));
			}
			return output;
		}

		template <std::size_t Size>
		[[nodiscard]] bool
		schema_matches(const std::map<std::string, std::string, std::less<>>& actual,
					   const std::array<std::string_view, Size>& expected)
		{
			if (actual.size() != expected.size())
				return false;
			for (const auto statement : expected)
			{
				const auto name_begin = statement.find("cxxlens_ng_");
				if (name_begin == std::string_view::npos)
					return false;
				const auto name_end = statement.find_first_of("( ", name_begin);
				const auto name = statement.substr(name_begin, name_end - name_begin);
				const auto found = actual.find(name);
				if (found == actual.end() || found->second != statement)
					return false;
			}
			return true;
		}

		template <std::size_t Size>
		[[nodiscard]] bool
		schema_includes(const std::map<std::string, std::string, std::less<>>& actual,
						const std::array<std::string_view, Size>& expected)
		{
			for (const auto statement : expected)
			{
				const auto name_begin = statement.find("cxxlens_ng_");
				if (name_begin == std::string_view::npos)
					return false;
				const auto name_end = statement.find_first_of("( ", name_begin);
				const auto name = statement.substr(name_begin, name_end - name_begin);
				const auto found = actual.find(name);
				if (found == actual.end() || found->second != statement)
					return false;
			}
			return true;
		}

		struct sqlite_expected_column
		{
			std::string_view name;
			std::string_view declared_type;
			bool not_null{};
			std::uint64_t primary_key_ordinal{};
		};

		struct sqlite_expected_index_column
		{
			std::int64_t cid{};
			std::string_view name;
			bool key{};
		};

		struct sqlite_expected_index
		{
			std::string_view name;
			bool unique{};
			std::string_view origin;
			bool partial{};
			std::span<const sqlite_expected_index_column> columns;
		};

		constexpr std::array v3_metadata_columns{
			sqlite_expected_column{"key", "TEXT", true, 1U},
			sqlite_expected_column{"value", "TEXT", true, 0U},
		};
		constexpr std::array v3_publication_columns{
			sqlite_expected_column{"publication_id", "TEXT", true, 1U},
			sqlite_expected_column{"series_id", "TEXT", true, 0U},
			sqlite_expected_column{"snapshot_id", "TEXT", true, 0U},
			sqlite_expected_column{"sequence", "INTEGER", true, 0U},
			sqlite_expected_column{"generation", "INTEGER", true, 0U},
			sqlite_expected_column{"parent", "TEXT", false, 0U},
			sqlite_expected_column{"state", "INTEGER", true, 0U},
			sqlite_expected_column{"payload_checksum", "TEXT", true, 0U},
			sqlite_expected_column{"payload_byte_count", "INTEGER", true, 0U},
			sqlite_expected_column{"payload_chunk_count", "INTEGER", true, 0U},
		};
		constexpr std::array v3_chunk_columns{
			sqlite_expected_column{"publication_id", "TEXT", true, 1U},
			sqlite_expected_column{"generation", "INTEGER", true, 2U},
			sqlite_expected_column{"chunk_ordinal", "INTEGER", true, 3U},
			sqlite_expected_column{"byte_offset", "INTEGER", true, 0U},
			sqlite_expected_column{"byte_count", "INTEGER", true, 0U},
			sqlite_expected_column{"checksum", "TEXT", true, 0U},
			sqlite_expected_column{"payload", "BLOB", true, 0U},
		};
		constexpr std::array v3_head_columns{
			sqlite_expected_column{"series_id", "TEXT", true, 1U},
			sqlite_expected_column{"current_publication", "TEXT", true, 0U},
			sqlite_expected_column{"sequence", "INTEGER", true, 0U},
		};
		constexpr std::array v2_metadata_columns{
			sqlite_expected_column{"key", "TEXT", false, 1U},
			sqlite_expected_column{"value", "TEXT", true, 0U},
		};
		constexpr std::array v2_publication_columns{
			sqlite_expected_column{"publication_id", "TEXT", false, 1U},
			sqlite_expected_column{"series_id", "TEXT", true, 0U},
			sqlite_expected_column{"snapshot_id", "TEXT", true, 0U},
			sqlite_expected_column{"sequence", "INTEGER", true, 0U},
			sqlite_expected_column{"generation", "INTEGER", true, 0U},
			sqlite_expected_column{"parent", "TEXT", false, 0U},
			sqlite_expected_column{"state", "INTEGER", true, 0U},
			sqlite_expected_column{"checksum", "TEXT", true, 0U},
			sqlite_expected_column{"payload", "BLOB", true, 0U},
		};
		constexpr std::array v2_head_columns{
			sqlite_expected_column{"series_id", "TEXT", false, 1U},
			sqlite_expected_column{"current_publication", "TEXT", true, 0U},
			sqlite_expected_column{"sequence", "INTEGER", true, 0U},
		};

		constexpr std::array v3_metadata_pk_columns{
			sqlite_expected_index_column{0, "key", true},
			sqlite_expected_index_column{1, "value", false},
		};
		constexpr std::array v3_publication_pk_columns{
			sqlite_expected_index_column{0, "publication_id", true},
			sqlite_expected_index_column{1, "series_id", false},
			sqlite_expected_index_column{2, "snapshot_id", false},
			sqlite_expected_index_column{3, "sequence", false},
			sqlite_expected_index_column{4, "generation", false},
			sqlite_expected_index_column{5, "parent", false},
			sqlite_expected_index_column{6, "state", false},
			sqlite_expected_index_column{7, "payload_checksum", false},
			sqlite_expected_index_column{8, "payload_byte_count", false},
			sqlite_expected_index_column{9, "payload_chunk_count", false},
		};
		constexpr std::array v3_publication_series_columns{
			sqlite_expected_index_column{1, "series_id", true},
			sqlite_expected_index_column{3, "sequence", true},
			sqlite_expected_index_column{0, "publication_id", false},
		};
		constexpr std::array v3_chunk_pk_columns{
			sqlite_expected_index_column{0, "publication_id", true},
			sqlite_expected_index_column{1, "generation", true},
			sqlite_expected_index_column{2, "chunk_ordinal", true},
			sqlite_expected_index_column{3, "byte_offset", false},
			sqlite_expected_index_column{4, "byte_count", false},
			sqlite_expected_index_column{5, "checksum", false},
			sqlite_expected_index_column{6, "payload", false},
		};
		constexpr std::array v3_chunk_locator_columns{
			sqlite_expected_index_column{0, "publication_id", true},
			sqlite_expected_index_column{1, "generation", true},
			sqlite_expected_index_column{2, "chunk_ordinal", true},
		};
		constexpr std::array v3_head_pk_columns{
			sqlite_expected_index_column{0, "series_id", true},
			sqlite_expected_index_column{1, "current_publication", false},
			sqlite_expected_index_column{2, "sequence", false},
		};
		constexpr std::array v2_metadata_pk_columns{
			sqlite_expected_index_column{0, "key", true},
			sqlite_expected_index_column{-1, "rowid", false},
		};
		constexpr std::array v2_publication_pk_columns{
			sqlite_expected_index_column{0, "publication_id", true},
			sqlite_expected_index_column{-1, "rowid", false},
		};
		constexpr std::array v2_publication_series_columns{
			sqlite_expected_index_column{1, "series_id", true},
			sqlite_expected_index_column{3, "sequence", true},
			sqlite_expected_index_column{-1, "rowid", false},
		};
		constexpr std::array v2_head_pk_columns{
			sqlite_expected_index_column{0, "series_id", true},
			sqlite_expected_index_column{-1, "rowid", false},
		};

		constexpr std::array v3_metadata_indexes{
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_metadata_1",
								  true,
								  "pk",
								  false,
								  v3_metadata_pk_columns},
		};
		constexpr std::array v3_publication_indexes{
			sqlite_expected_index{
				"cxxlens_ng_publication_series", false, "c", false, v3_publication_series_columns},
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_publication_1",
								  true,
								  "pk",
								  false,
								  v3_publication_pk_columns},
		};
		constexpr std::array v3_chunk_indexes{
			sqlite_expected_index{
				"cxxlens_ng_payload_chunk_locator", false, "c", false, v3_chunk_locator_columns},
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_payload_chunk_1",
								  true,
								  "pk",
								  false,
								  v3_chunk_pk_columns},
		};
		constexpr std::array v3_head_indexes{
			sqlite_expected_index{
				"sqlite_autoindex_cxxlens_ng_series_head_1", true, "pk", false, v3_head_pk_columns},
		};
		constexpr std::array v2_metadata_indexes{
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_metadata_1",
								  true,
								  "pk",
								  false,
								  v2_metadata_pk_columns},
		};
		constexpr std::array v2_publication_indexes{
			sqlite_expected_index{
				"cxxlens_ng_publication_series", false, "c", false, v2_publication_series_columns},
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_publication_1",
								  true,
								  "pk",
								  false,
								  v2_publication_pk_columns},
		};
		constexpr std::array v2_head_indexes{
			sqlite_expected_index{
				"sqlite_autoindex_cxxlens_ng_series_head_1", true, "pk", false, v2_head_pk_columns},
		};
		constexpr std::array migration_metadata_indexes{
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_migration_metadata_1",
								  true,
								  "pk",
								  false,
								  v3_metadata_pk_columns},
		};
		constexpr std::array migration_publication_indexes{
			sqlite_expected_index{"cxxlens_ng_migration_publication_series",
								  false,
								  "c",
								  false,
								  v3_publication_series_columns},
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_migration_publication_1",
								  true,
								  "pk",
								  false,
								  v3_publication_pk_columns},
		};
		constexpr std::array migration_chunk_indexes{
			sqlite_expected_index{"cxxlens_ng_migration_payload_chunk_locator",
								  false,
								  "c",
								  false,
								  v3_chunk_locator_columns},
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_migration_payload_chunk_1",
								  true,
								  "pk",
								  false,
								  v3_chunk_pk_columns},
		};
		constexpr std::array migration_head_indexes{
			sqlite_expected_index{"sqlite_autoindex_cxxlens_ng_migration_series_head_1",
								  true,
								  "pk",
								  false,
								  v3_head_pk_columns},
		};

		[[nodiscard]] result<bool>
		table_profile_matches(sqlite_database& database,
							  const std::string_view table_name,
							  const std::span<const sqlite_expected_column> expected)
		{
			auto selected = sqlite_statement::prepare(
				database,
				"SELECT cid,name,type,\"notnull\",dflt_value,pk,hidden FROM "
				"pragma_table_xinfo(?1) ORDER BY cid",
				false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (auto bound = selected->bind_text(1, table_name); !bound)
				return unexpected(std::move(bound.error()));
			std::size_t index{};
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "table-xinfo"));
				if (index >= expected.size())
					return false;
				auto cid = selected->column_unsigned(0);
				auto name = selected->column_text(1);
				auto declared_type = selected->column_text(2);
				auto not_null = selected->column_unsigned(3);
				auto default_expression = selected->column_optional_text(4);
				auto primary_key = selected->column_unsigned(5);
				auto hidden = selected->column_unsigned(6);
				const auto& profile = expected[index];
				if (!cid || !name || !declared_type || !not_null || !default_expression ||
					!primary_key || !hidden || *cid != index || *name != profile.name ||
					*declared_type != profile.declared_type || *not_null > 1U ||
					(*not_null != 0U) != profile.not_null || default_expression->has_value() ||
					*primary_key != profile.primary_key_ordinal || *hidden != 0U)
					return false;
				++index;
			}
			return index == expected.size();
		}

		[[nodiscard]] result<bool>
		index_xinfo_matches(sqlite_database& database,
							const std::string_view index_name,
							const std::span<const sqlite_expected_index_column> expected)
		{
			auto selected = sqlite_statement::prepare(
				database,
				"SELECT seqno,cid,name,desc,coll,key FROM pragma_index_xinfo(?1) ORDER BY seqno",
				false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (auto bound = selected->bind_text(1, index_name); !bound)
				return unexpected(std::move(bound.error()));
			std::size_t position{};
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "index-xinfo"));
				if (position >= expected.size())
					return false;
				auto sequence = selected->column_unsigned(0);
				auto cid = selected->column_signed(1);
				auto name = selected->column_optional_text(2);
				auto descending = selected->column_unsigned(3);
				auto collation = selected->column_text(4);
				auto key = selected->column_unsigned(5);
				const auto& profile = expected[position];
				const bool name_matches = profile.cid == -1
					? name && !name->has_value() && profile.name == "rowid"
					: name && name->has_value() && **name == profile.name;
				if (!sequence || !cid || !descending || !collation || !key ||
					*sequence != position || *cid != profile.cid || !name_matches ||
					*descending != 0U || *collation != "BINARY" || *key > 1U ||
					(*key != 0U) != profile.key)
					return false;
				++position;
			}
			return position == expected.size();
		}

		[[nodiscard]] result<bool>
		index_profiles_match(sqlite_database& database,
							 const std::string_view table_name,
							 const std::span<const sqlite_expected_index> expected)
		{
			struct index_list_entry
			{
				bool unique{};
				std::string origin;
				bool partial{};
			};
			auto selected = sqlite_statement::prepare(
				database,
				"SELECT name,\"unique\",origin,partial FROM pragma_index_list(?1) ORDER BY name",
				false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (auto bound = selected->bind_text(1, table_name); !bound)
				return unexpected(std::move(bound.error()));
			std::map<std::string, index_list_entry, std::less<>> actual;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "index-list"));
				auto name = selected->column_text(0);
				auto unique = selected->column_unsigned(1);
				auto origin = selected->column_text(2);
				auto partial = selected->column_unsigned(3);
				if (!name || !unique || !origin || !partial || *unique > 1U || *partial > 1U ||
					!actual
						 .emplace(
							 std::move(*name),
							 index_list_entry{*unique != 0U, std::move(*origin), *partial != 0U})
						 .second)
					return false;
			}
			if (actual.size() != expected.size())
				return false;
			for (const auto& profile : expected)
			{
				const auto found = actual.find(profile.name);
				if (found == actual.end() || found->second.unique != profile.unique ||
					found->second.origin != profile.origin ||
					found->second.partial != profile.partial)
					return false;
				auto xinfo = index_xinfo_matches(database, profile.name, profile.columns);
				if (!xinfo)
					return unexpected(std::move(xinfo.error()));
				if (!*xinfo)
					return false;
			}
			return true;
		}

		[[nodiscard]] result<bool> schema_profile_matches(sqlite_database& database,
														  const sqlite_physical_format format)
		{
			const auto validate_table =
				[&](const std::string_view name,
					const std::span<const sqlite_expected_column> columns,
					const std::span<const sqlite_expected_index> indexes) -> result<bool>
			{
				auto table = table_profile_matches(database, name, columns);
				if (!table || !*table)
					return table;
				return index_profiles_match(database, name, indexes);
			};
			if (format == sqlite_physical_format::predecessor_v2)
			{
				for (const auto& valid : std::array{
						 validate_table(
							 "cxxlens_ng_metadata", v2_metadata_columns, v2_metadata_indexes),
						 validate_table("cxxlens_ng_publication",
										v2_publication_columns,
										v2_publication_indexes),
						 validate_table("cxxlens_ng_series_head", v2_head_columns, v2_head_indexes),
					 })
				{
					if (!valid)
						return unexpected(std::move(valid.error()));
					if (!*valid)
						return false;
				}
				return true;
			}
			for (const auto& valid : std::array{
					 validate_table(
						 "cxxlens_ng_metadata", v3_metadata_columns, v3_metadata_indexes),
					 validate_table(
						 "cxxlens_ng_publication", v3_publication_columns, v3_publication_indexes),
					 validate_table("cxxlens_ng_payload_chunk", v3_chunk_columns, v3_chunk_indexes),
					 validate_table("cxxlens_ng_series_head", v3_head_columns, v3_head_indexes),
				 })
			{
				if (!valid)
					return unexpected(std::move(valid.error()));
				if (!*valid)
					return false;
			}
			return true;
		}

		[[nodiscard]] result<bool> migration_shadow_profile_matches(sqlite_database& database)
		{
			const auto validate_table =
				[&](const std::string_view name,
					const std::span<const sqlite_expected_column> columns,
					const std::span<const sqlite_expected_index> indexes) -> result<bool>
			{
				auto table = table_profile_matches(database, name, columns);
				if (!table || !*table)
					return table;
				return index_profiles_match(database, name, indexes);
			};
			for (const auto& valid : std::array{
					 validate_table("cxxlens_ng_migration_metadata",
									v3_metadata_columns,
									migration_metadata_indexes),
					 validate_table("cxxlens_ng_migration_publication",
									v3_publication_columns,
									migration_publication_indexes),
					 validate_table("cxxlens_ng_migration_payload_chunk",
									v3_chunk_columns,
									migration_chunk_indexes),
					 validate_table("cxxlens_ng_migration_series_head",
									v3_head_columns,
									migration_head_indexes),
				 })
			{
				if (!valid)
					return unexpected(std::move(valid.error()));
				if (!*valid)
					return false;
			}
			return true;
		}

		[[nodiscard]] result<std::map<std::string, std::string, std::less<>>>
		read_metadata(sqlite_database& database)
		{
			auto selected = sqlite_statement::prepare(
				database, "SELECT key,value FROM cxxlens_ng_metadata ORDER BY key", false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			std::map<std::string, std::string, std::less<>> output;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(store_error("store.sqlite-failure", "database", "metadata"));
				auto key = selected->column_text(0);
				auto value = selected->column_text(1);
				if (!key || !value || !output.emplace(std::move(*key), std::move(*value)).second)
					return unexpected(store_error("store.corrupt", "sqlite", "metadata"));
			}
			return output;
		}

		[[nodiscard]] result<std::int64_t> read_application_id(sqlite_database& database)
		{
			auto selected = sqlite_statement::prepare(database, "PRAGMA application_id", false);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (selected->step() != sqlite_row)
				return unexpected(
					store_error("store.sqlite-failure", "database", "application-id"));
			auto value = selected->column_signed(0);
			if (!value)
				return unexpected(store_error("store.format-incompatible",
											  "sqlite-physical-format",
											  "unknown-format-or-layout"));
			if (selected->step() != sqlite_done)
				return unexpected(
					store_error("store.sqlite-failure", "database", "application-id"));
			return *value;
		}

		[[nodiscard]] result<std::optional<sqlite_physical_format>>
		classify_sqlite_database(sqlite_database& database)
		{
			auto schema = read_user_schema(database);
			if (!schema)
				return unexpected(std::move(schema.error()));
			auto application_id = read_application_id(database);
			if (!application_id)
				return unexpected(std::move(application_id.error()));
			if (*application_id != 0)
				return unexpected(store_error("store.format-incompatible",
											  "sqlite-physical-format",
											  "unknown-format-or-layout"));
			if (schema->empty())
				return std::optional<sqlite_physical_format>{};
			if (!schema->contains("cxxlens_ng_metadata"))
				return unexpected(store_error("store.format-incompatible",
											  "sqlite-physical-format",
											  "unknown-format-or-layout"));
			auto metadata = read_metadata(database);
			if (!metadata)
				return unexpected(std::move(metadata.error()));
			const auto marker = metadata->find("physical_format");
			if (marker == metadata->end())
				return unexpected(store_error("store.format-incompatible",
											  "sqlite-physical-format",
											  "unknown-format-or-layout"));
			const auto publication = schema->find("cxxlens_ng_publication");
			const std::string_view publication_sql =
				publication == schema->end() ? std::string_view{} : publication->second;
			const bool v3_publication_signal =
				publication_sql.contains("payload_checksum TEXT NOT NULL") ||
				publication_sql.contains("payload_byte_count INTEGER NOT NULL") ||
				publication_sql.contains("payload_chunk_count INTEGER NOT NULL");
			const bool v2_publication_signal =
				publication_sql.contains(",checksum TEXT NOT NULL") &&
				publication_sql.contains("payload BLOB NOT NULL");
			const bool v3_signal = schema->contains("cxxlens_ng_payload_chunk") ||
				schema->contains("cxxlens_ng_payload_chunk_locator") || v3_publication_signal ||
				metadata->contains("physical_format_version") ||
				metadata->contains("payload_chunk_profile") ||
				metadata->contains("payload_chunk_maximum_bytes");
			if (marker->second == "cxxlens.sqlite-semantic-store.v2")
			{
				if (v3_signal)
					return unexpected(store_error(
						"store.corrupt", "sqlite-format-classification", "mixed-v2-v3"));
				if (!schema_matches(*schema, sqlite_v2_ddl) || metadata->size() != 1U)
					return unexpected(store_error("store.format-incompatible",
												  "sqlite-physical-format",
												  "v2-profile-mismatch"));
				auto profile =
					schema_profile_matches(database, sqlite_physical_format::predecessor_v2);
				if (!profile)
					return unexpected(std::move(profile.error()));
				if (!*profile)
					return unexpected(store_error("store.format-incompatible",
												  "sqlite-physical-format",
												  "v2-profile-mismatch"));
				return std::optional{sqlite_physical_format::predecessor_v2};
			}
			if (marker->second != "cxxlens.sqlite-semantic-store.v3")
				return unexpected(store_error("store.format-incompatible",
											  "sqlite-physical-format",
											  "unknown-format-or-layout"));
			if (v2_publication_signal)
				return unexpected(
					store_error("store.corrupt", "sqlite-format-classification", "mixed-v2-v3"));
			if (auto symbols = require_v3_symbols(*database.api()); !symbols)
				return unexpected(std::move(symbols.error()));
			const std::map<std::string, std::string, std::less<>> expected_metadata{
				{"payload_chunk_maximum_bytes", "8388608"},
				{"payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"},
				{"physical_format", "cxxlens.sqlite-semantic-store.v3"},
				{"physical_format_version", "3.0.0"},
			};
			if (!schema_matches(*schema, sqlite_v3_ddl) || *metadata != expected_metadata)
				return unexpected(store_error(
					"store.corrupt", "sqlite-current-v3", "schema-or-authority-damage"));
			auto profile = schema_profile_matches(database, sqlite_physical_format::current_v3);
			if (!profile)
				return unexpected(std::move(profile.error()));
			if (!*profile)
				return unexpected(store_error(
					"store.corrupt", "sqlite-current-v3", "schema-or-authority-damage"));
			return std::optional{sqlite_physical_format::current_v3};
		}

		[[nodiscard]] result<void> validate_v3_connection(sqlite_database& database)
		{
			if (auto symbols = require_v3_symbols(*database.api()); !symbols)
				return symbols;
			const auto actual =
				observe_actual_sqlite_limit_length(database.api()->limit, database.handle());
			if (actual < 16'777'216)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite-limit-length", "16777216"));
			return {};
		}

		[[nodiscard]] result<std::string>
		read_single_text(sqlite_database& database, const std::string_view sql, const bool use_v3)
		{
			auto selected = sqlite_statement::prepare(database, sql, use_v3);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (selected->step() != sqlite_row)
				return unexpected(store_error("store.sqlite-failure", "database", "pragma"));
			auto value = selected->column_text(0);
			if (!value || selected->step() != sqlite_done)
				return unexpected(store_error("store.sqlite-failure", "database", "pragma"));
			return value;
		}

		[[nodiscard]] result<std::int64_t> read_single_integer(sqlite_database& database,
															   const std::string_view sql,
															   const bool use_v3)
		{
			auto selected = sqlite_statement::prepare(database, sql, use_v3);
			if (!selected)
				return unexpected(std::move(selected.error()));
			if (selected->step() != sqlite_row)
				return unexpected(store_error("store.sqlite-failure", "database", "pragma"));
			auto value = selected->column_signed(0);
			if (!value || selected->step() != sqlite_done)
				return unexpected(store_error("store.sqlite-failure", "database", "pragma"));
			return value;
		}

		[[nodiscard]] result<void> require_wal_mode(sqlite_database& database, const bool use_v3)
		{
			auto mode = read_single_text(database, "PRAGMA journal_mode", use_v3);
			if (!mode || *mode != "wal")
				return unexpected(
					store_error("store.sqlite-failure", "sqlite-journal-mode", "expected-wal"));
			return {};
		}

		[[nodiscard]] result<void> set_and_require_full_synchronous(sqlite_database& database,
																	const bool use_v3)
		{
			if (auto configured = database.execute("PRAGMA synchronous=FULL;"); !configured)
				return configured;
			auto synchronous = read_single_integer(database, "PRAGMA synchronous", use_v3);
			if (!synchronous)
				return unexpected(std::move(synchronous.error()));
			if (*synchronous != 2)
				return unexpected(
					store_error("store.sqlite-failure", "database", "synchronous-not-full"));
			return {};
		}

		[[nodiscard]] result<void> set_and_require_memory_journal(sqlite_database& database,
																  const bool use_v3)
		{
			if (auto configured = database.execute("PRAGMA journal_mode=MEMORY;"); !configured)
				return configured;
			auto journal = read_single_text(database, "PRAGMA journal_mode", use_v3);
			if (!journal)
				return unexpected(std::move(journal.error()));
			if (*journal != "memory")
				return unexpected(
					store_error("store.sqlite-failure", "sqlite-journal-mode", "expected-memory"));
			return {};
		}

		[[nodiscard]] result<void> initialize_v3_authority(sqlite_database& database)
		{
			if (auto begun = database.execute("BEGIN IMMEDIATE;"); !begun)
				return begun;
			const auto rollback = [&database]()
			{
				(void)database.execute("ROLLBACK;");
			};
			for (const auto statement : sqlite_v3_ddl)
			{
				if (auto created = database.execute(std::string{statement} + ';'); !created)
				{
					rollback();
					return created;
				}
			}
			auto insert = sqlite_statement::prepare(
				database, "INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?1,?2)", true);
			if (!insert)
			{
				rollback();
				return unexpected(std::move(insert.error()));
			}
			constexpr std::array metadata{
				std::pair{"physical_format_version", "3.0.0"},
				std::pair{"payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"},
				std::pair{"payload_chunk_maximum_bytes", "8388608"},
				std::pair{"physical_format", "cxxlens.sqlite-semantic-store.v3"},
			};
			for (const auto& [key, value] : metadata)
			{
				if (auto bound = insert->bind_text(1, key); !bound)
				{
					rollback();
					return bound;
				}
				if (auto bound = insert->bind_text(2, value); !bound)
				{
					rollback();
					return bound;
				}
				if (auto inserted = insert->expect_done(); !inserted)
				{
					rollback();
					return inserted;
				}
				if (auto reset = insert->reset(); !reset)
				{
					rollback();
					return reset;
				}
			}
			if (auto committed = database.execute("COMMIT;"); !committed)
			{
				rollback();
				return committed;
			}
			auto classified = classify_sqlite_database(database);
			if (!classified || !*classified || **classified != sqlite_physical_format::current_v3)
				return unexpected(
					classified ? store_error("store.corrupt", "sqlite-current-v3", "initialization")
							   : std::move(classified.error()));
			return {};
		}

		struct sqlite_quiescent_source_anchor
		{
			sqlite_backend_namespace_census namespace_census;
			std::uint64_t byte_count{};
			std::string sha256;
		};

		struct sqlite_active_wal_read_anchor
		{
			sqlite_backend_opaque_identity main_object_identity;
			sqlite_backend_opaque_identity main_entry_identity;
			sqlite_backend_opaque_identity wal_object_identity;
			sqlite_backend_opaque_identity wal_entry_identity;
			sqlite_backend_opaque_identity shm_object_identity;
			sqlite_backend_opaque_identity shm_entry_identity;
			sqlite_backend_shm_lock_observation read_lock;
			std::uint32_t read_lock_index{};
			bool used_cantinit_heap_route{};
			sqlite_backend_opaque_identity target_namespace_epoch_identity;
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
			sqlite_source_shm_open_callback_receipt source_shm_open_receipt;
			sqlite_backend_copy_receipt main_before;
			std::uint64_t shared_memory_byte_count{};
			std::optional<sqlite_wal_header_receipt> wal_header;
		};

		enum class sqlite_wal_handoff_state : std::uint8_t
		{
			ready,
			wal_only_deferred,
			recovery_handoff_pending,
			poisoned,
		};

		struct sqlite_wal_only_lifetime_pin
		{
			std::shared_ptr<const sqlite_backend_held_object> main;
			std::shared_ptr<const sqlite_backend_held_object> wal;
			std::shared_ptr<sqlite_wal_recovery_workspace> workspace;
		};

		struct sqlite_active_wal_lifetime_pin
		{
			std::shared_ptr<const void> source_authority;
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
		};

		[[nodiscard]] std::shared_ptr<const void>
		sqlite_authority_anchor_pin(const sqlite_quiescent_source_anchor& anchor)
		{
			const std::shared_ptr<const sqlite_backend_held_object>* held{};
			for (const auto& entry : anchor.namespace_census.entries)
			{
				if (entry.role != sqlite_backend_file_role::main_database)
					continue;
				if (held != nullptr)
					return {};
				held = &entry.held_object;
			}
			return held != nullptr ? std::static_pointer_cast<const void>(*held) : nullptr;
		}

		[[nodiscard]] std::shared_ptr<const void>
		sqlite_wal_only_authority_pin(const sqlite_quiescent_source_anchor& anchor,
									  const sqlite_wal_source_capture& capture)
		{
			const sqlite_backend_entry_observation* main{};
			const sqlite_backend_entry_observation* wal{};
			for (const auto& entry : anchor.namespace_census.entries)
			{
				const sqlite_backend_entry_observation** selected =
					entry.role == sqlite_backend_file_role::main_database	  ? &main
					: entry.role == sqlite_backend_file_role::write_ahead_log ? &wal
																			  : nullptr;
				if (selected == nullptr)
					continue;
				if (*selected != nullptr)
					return {};
				*selected = &entry;
			}
			if (main == nullptr || wal == nullptr || !main->held_object || !wal->held_object ||
				!capture.workspace)
				return {};
			try
			{
				auto pin = std::make_shared<sqlite_wal_only_lifetime_pin>();
				pin->main = main->held_object;
				pin->wal = wal->held_object;
				pin->workspace = capture.workspace;
				return std::static_pointer_cast<const void>(std::move(pin));
			}
			catch (const std::bad_alloc&)
			{
				return {};
			}
		}

		struct observed_opened_sqlite_database
		{
			std::shared_ptr<sqlite_api> api;
			std::shared_ptr<sqlite_backend_private_snapshot> private_snapshot;
			std::shared_ptr<sqlite_backend_connection_observation_scope> active_observation;
			std::optional<sqlite_backend_effect_arm_receipt> active_denied_receipt;
			std::optional<sqlite_quiescent_source_anchor> source_anchor;
			std::optional<sqlite_active_wal_read_anchor> active_wal_anchor;
			std::optional<sqlite_wal_source_capture> wal_only_capture;
			sqlite_physical_format format{sqlite_physical_format::current_v3};
			std::unique_ptr<sqlite_database> database;
			bool private_read_transaction{};
		};

		[[nodiscard]] error sqlite_namespace_observation_failure()
		{
			return store_error(
				"store.sqlite-failure", "sqlite-sidecar-state", "observation-io-failure");
		}

		[[nodiscard]] error sqlite_quiescent_observation_failure()
		{
			return store_error(
				"store.sqlite-failure", "sqlite-open-snapshot", "quiescent-observation-io-failure");
		}

		[[nodiscard]] error sqlite_source_changed()
		{
			return store_error(
				"store.sqlite-failure", "sqlite-open-snapshot", "concurrent-source-change");
		}

		[[nodiscard]] const sqlite_backend_entry_observation*
		observed_entry(const sqlite_backend_namespace_census& census,
					   const sqlite_backend_file_role role)
		{
			const sqlite_backend_entry_observation* output{};
			for (const auto& entry : census.entries)
			{
				if (entry.role != role)
					continue;
				if (output != nullptr)
					return nullptr;
				output = &entry;
			}
			return output;
		}

		[[nodiscard]] bool
		observation_entry_is_well_formed(const sqlite_backend_entry_observation& entry)
		{
			switch (entry.state)
			{
				case sqlite_backend_entry_state::absent:
					return !entry.object_identity && !entry.directory_entry_identity &&
						entry.held_object == nullptr;
				case sqlite_backend_entry_state::held_regular:
					return entry.object_identity && entry.directory_entry_identity &&
						entry.held_object != nullptr && entry.held_object->role() == entry.role &&
						entry.held_object->object_identity() == *entry.object_identity &&
						entry.held_object->directory_entry_identity() ==
						*entry.directory_entry_identity;
				case sqlite_backend_entry_state::present_unreadable:
				case sqlite_backend_entry_state::unsupported_kind:
					return !entry.object_identity && entry.directory_entry_identity &&
						entry.held_object == nullptr;
			}
			return false;
		}

		[[nodiscard]] result<void>
		validate_namespace_census(const sqlite_backend_namespace_census& census,
								  const sqlite_backend_observation_capability& observation)
		{
			if (census.profile.empty() ||
				census.capability_token != observation.capability_token() ||
				census.parent_namespace_identity.profile.empty() ||
				census.parent_namespace_identity.bytes.empty())
				return unexpected(sqlite_namespace_observation_failure());
			constexpr std::array roles{
				sqlite_backend_file_role::main_database,
				sqlite_backend_file_role::write_ahead_log,
				sqlite_backend_file_role::shared_memory,
				sqlite_backend_file_role::rollback_journal,
			};
			for (const auto role : roles)
			{
				const auto* entry = observed_entry(census, role);
				if (entry == nullptr || !observation_entry_is_well_formed(*entry))
					return unexpected(sqlite_namespace_observation_failure());
			}
			return {};
		}

		[[nodiscard]] result<void> validate_observation_binding(
			const sqlite_api& api,
			const std::string_view vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation)
		{
			if (vfs_name.empty() || !backend_lifetime || !observation)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-lifetime"));
			const auto binding = observation->binding();
			if (binding.observation_profile.empty() || binding.registered_vfs_name != vfs_name ||
				binding.vfs_implementation_identity == nullptr ||
				binding.backend_lifetime_identity != backend_lifetime.get())
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-lifetime"));
			if (binding.observation_capability_identity != observation.get() ||
				binding.runtime_identity == nullptr ||
				binding.runtime_identity != api.runtime_identity ||
				binding.runtime_lifetime_identity == nullptr ||
				binding.runtime_lifetime_identity != api.runtime_lifetime_identity() ||
				observation->capability_token().profile.empty() ||
				observation->capability_token().bytes.empty())
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-observation"));
			return {};
		}

		[[nodiscard]] result<void> validate_read_write_open_observation(
			const sqlite_backend_connection_observation_scope& scope,
			const sqlite_backend_observation_capability& observation)
		{
			auto captured = scope.snapshot();
			if (!captured || !captured->complete || !captured->main_handle_open ||
				captured->capability_token != observation.capability_token())
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-observation"));
			const sqlite_backend_open_observation* main{};
			for (const auto& event : captured->open_events)
			{
				if (event.role != sqlite_backend_file_role::main_database)
					continue;
				if (main != nullptr)
					return unexpected(
						store_error("store.backend-unavailable", "sqlite", "vfs-observation"));
				main = &event;
			}
			if (main == nullptr || main->outcome != sqlite_backend_open_outcome::succeeded ||
				!main->returned_flags || (main->input_flags & sqlite_open_readwrite) == 0 ||
				(main->input_flags &
				 (sqlite_open_readonly | sqlite_open_create | sqlite_open_uri)) != 0)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-observation"));
			if ((*main->returned_flags & sqlite_open_readonly) != 0)
				return unexpected(
					store_error("store.sqlite-failure", "open", "read-write-required"));
			return {};
		}

		[[nodiscard]] result<void> validate_ephemeral_open_observation(
			const sqlite_backend_connection_observation_scope& scope,
			const sqlite_backend_observation_capability& observation)
		{
			auto captured = scope.snapshot();
			if (!captured || !captured->complete || captured->main_handle_open ||
				captured->capability_token != observation.capability_token() ||
				captured->connection_token != scope.token() || !captured->open_events.empty() ||
				captured->shared_memory_object_identity || captured->shared_memory_entry_identity ||
				!captured->held_shm_locks.empty())
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "vfs-observation"));
			return {};
		}

		[[nodiscard]] error sqlite_effect_gate_failure()
		{
			return store_error("store.backend-unavailable", "sqlite", "effect-gate");
		}

		struct sqlite_effect_connection_scope
		{
			std::shared_ptr<sqlite_backend_connection_observation_scope> observation;
			sqlite_backend_effect_gate* gate{};
			sqlite_backend_effect_arm_receipt denied_receipt;
		};

		[[nodiscard]] bool
		effect_receipt_matches(const sqlite_backend_effect_arm_receipt& receipt,
							   const sqlite_backend_opaque_identity& capability_token,
							   const sqlite_backend_opaque_identity& connection_token,
							   const std::string_view path,
							   const sqlite_backend_opaque_identity& prerequisite,
							   const sqlite_backend_effect_stage stage,
							   const bool after_exclusive_lock)
		{
			return !receipt.profile.empty() && receipt.capability_token == capability_token &&
				receipt.connection_token == connection_token &&
				receipt.canonical_vfs_locator == path &&
				receipt.prerequisite_receipt == prerequisite &&
				!receipt.validation_receipt.profile.empty() &&
				!receipt.validation_receipt.bytes.empty() && receipt.stage == stage &&
				receipt.sequence != 0U &&
				receipt.armed_after_underlying_exclusive_lock == after_exclusive_lock;
		}

		[[nodiscard]] bool effect_receipts_equal(const sqlite_backend_effect_arm_receipt& left,
												 const sqlite_backend_effect_arm_receipt& right)
		{
			return left.profile == right.profile &&
				left.capability_token == right.capability_token &&
				left.connection_token == right.connection_token &&
				left.canonical_vfs_locator == right.canonical_vfs_locator &&
				left.prerequisite_receipt == right.prerequisite_receipt &&
				left.validation_receipt == right.validation_receipt && left.stage == right.stage &&
				left.sequence == right.sequence &&
				left.armed_after_underlying_exclusive_lock ==
				right.armed_after_underlying_exclusive_lock;
		}

		[[nodiscard]] result<sqlite_effect_connection_scope> activate_effect_gated_connection(
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const std::string_view path,
			std::shared_ptr<sqlite_backend_connection_observation_scope> scope)
		{
			if (!observation || !scope)
				return unexpected(sqlite_effect_gate_failure());
			auto* gate = scope->effect_gate_port();
			if (gate == nullptr || !gate->enforcement_active() ||
				gate->stage() != sqlite_backend_effect_stage::denied)
				return unexpected(sqlite_effect_gate_failure());
			auto activated =
				gate->activate_denied(observation->capability_token(), scope->token(), path);
			const sqlite_backend_opaque_identity no_prerequisite;
			if (!activated ||
				!effect_receipt_matches(*activated,
										observation->capability_token(),
										scope->token(),
										path,
										no_prerequisite,
										sqlite_backend_effect_stage::denied,
										false) ||
				gate->stage() != sqlite_backend_effect_stage::denied || !gate->enforcement_active())
				return unexpected(activated ? sqlite_effect_gate_failure()
											: std::move(activated.error()));
			auto latest = gate->latest_receipt();
			if (!latest || !effect_receipts_equal(*latest, *activated))
				return unexpected(sqlite_effect_gate_failure());
			return sqlite_effect_connection_scope{std::move(scope), gate, std::move(*activated)};
		}

		[[nodiscard]] result<sqlite_effect_connection_scope> begin_effect_gated_connection(
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const std::string_view path)
		{
			if (!observation)
				return unexpected(sqlite_effect_gate_failure());
			auto scope = observation->begin_connection_observation(path);
			if (!scope || !*scope)
				return unexpected(scope ? sqlite_effect_gate_failure() : std::move(scope.error()));
			return activate_effect_gated_connection(observation, path, std::move(*scope));
		}

		[[nodiscard]] result<sqlite_effect_connection_scope>
		begin_ephemeral_effect_gated_connection(
			const std::shared_ptr<sqlite_backend_observation_capability>& observation)
		{
			if (!observation)
				return unexpected(sqlite_effect_gate_failure());
			auto scope = observation->begin_ephemeral_connection_observation();
			if (!scope || !*scope)
				return unexpected(scope ? sqlite_effect_gate_failure() : std::move(scope.error()));
			return activate_effect_gated_connection(observation, ":memory:", std::move(*scope));
		}

		void append_effect_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			for (int shift = 56; shift >= 0; shift -= 8)
				output.push_back(std::byte{static_cast<unsigned char>((value >> shift) & 0xffU)});
		}

		void append_effect_bytes(std::vector<std::byte>& output, const std::string_view value)
		{
			append_effect_u64(output, static_cast<std::uint64_t>(value.size()));
			const auto bytes = std::as_bytes(std::span{value.data(), value.size()});
			output.insert(output.end(), bytes.begin(), bytes.end());
		}

		void append_effect_identity(std::vector<std::byte>& output,
									const sqlite_backend_opaque_identity& identity)
		{
			append_effect_bytes(output, identity.profile);
			append_effect_u64(output, static_cast<std::uint64_t>(identity.bytes.size()));
			output.insert(output.end(), identity.bytes.begin(), identity.bytes.end());
		}

		void append_effect_optional_identity(
			std::vector<std::byte>& output,
			const std::optional<sqlite_backend_opaque_identity>& identity)
		{
			output.push_back(identity ? std::byte{1U} : std::byte{0U});
			if (identity)
				append_effect_identity(output, *identity);
		}

		void append_effect_receipt(std::vector<std::byte>& output,
								   const sqlite_backend_effect_arm_receipt& receipt)
		{
			append_effect_bytes(output, receipt.profile);
			append_effect_identity(output, receipt.capability_token);
			append_effect_identity(output, receipt.connection_token);
			append_effect_bytes(output, receipt.canonical_vfs_locator);
			append_effect_identity(output, receipt.prerequisite_receipt);
			append_effect_identity(output, receipt.validation_receipt);
			output.push_back(std::byte{static_cast<std::uint8_t>(receipt.stage)});
			append_effect_u64(output, receipt.sequence);
			output.push_back(receipt.armed_after_underlying_exclusive_lock ? std::byte{1U}
																		   : std::byte{0U});
		}

		void append_effect_wal_capture(std::vector<std::byte>& output,
									   const sqlite_wal_source_capture* capture)
		{
			output.push_back(capture != nullptr ? std::byte{1U} : std::byte{0U});
			if (capture == nullptr)
				return;
			for (const auto* receipt : {&capture->full_source_main,
										&capture->full_source_wal,
										&capture->workspace_receipt.authoritative_wal_prefix})
			{
				append_effect_u64(output, receipt->byte_count);
				append_effect_bytes(output, receipt->sha256);
			}
			const auto& scan = capture->wal_scan;
			output.push_back(std::byte{static_cast<std::uint8_t>(scan.classification)});
			output.push_back(std::byte{static_cast<std::uint8_t>(scan.stop)});
			append_effect_u64(output, scan.inspected_byte_count);
			append_effect_u64(output, scan.validated_prefix_byte_count);
			append_effect_u64(output, scan.authoritative_prefix_byte_count);
			append_effect_u64(output, scan.valid_frame_count);
			append_effect_u64(output, scan.valid_commit_count);
			append_effect_u64(output, scan.torn_remainder_byte_count);
		}

		[[nodiscard]] result<sqlite_backend_opaque_identity>
		make_effect_prerequisite(const std::string_view profile,
								 const std::string_view path,
								 const sqlite_backend_opaque_identity& capability_token,
								 const sqlite_backend_opaque_identity& connection_token,
								 const sqlite_quiescent_source_anchor* anchor,
								 const sqlite_backend_open_observation* main_open,
								 const bool preinit_absent,
								 const sqlite_wal_source_capture* wal_capture)
		{
			try
			{
				sqlite_backend_opaque_identity output{std::string{profile}, {}};
				auto& bytes = output.bytes;
				bytes.reserve(512U + path.size());
				append_effect_bytes(bytes, profile);
				append_effect_bytes(bytes, path);
				append_effect_identity(bytes, capability_token);
				append_effect_identity(bytes, connection_token);
				bytes.push_back(preinit_absent ? std::byte{1U} : std::byte{0U});
				if (anchor == nullptr)
				{
					append_effect_bytes(bytes, "not-applicable-filesystem-anchor");
				}
				else
				{
					append_effect_identity(bytes,
										   anchor->namespace_census.parent_namespace_identity);
					append_effect_u64(bytes, anchor->byte_count);
					append_effect_bytes(bytes, anchor->sha256);
					for (const auto& entry : anchor->namespace_census.entries)
					{
						bytes.push_back(std::byte{static_cast<std::uint8_t>(entry.role)});
						bytes.push_back(std::byte{static_cast<std::uint8_t>(entry.state)});
						bytes.push_back(entry.object_identity ? std::byte{1U} : std::byte{0U});
						if (entry.object_identity)
							append_effect_identity(bytes, *entry.object_identity);
						bytes.push_back(entry.directory_entry_identity ? std::byte{1U}
																	   : std::byte{0U});
						if (entry.directory_entry_identity)
							append_effect_identity(bytes, *entry.directory_entry_identity);
					}
					append_effect_wal_capture(bytes, wal_capture);
				}
				if (main_open == nullptr)
				{
					append_effect_bytes(bytes, "main-open-not-applicable");
				}
				else
				{
					append_effect_u64(bytes, static_cast<std::uint64_t>(main_open->input_flags));
					append_effect_u64(bytes,
									  static_cast<std::uint64_t>(*main_open->returned_flags));
					append_effect_identity(bytes, *main_open->object_identity);
					append_effect_identity(bytes, *main_open->directory_entry_identity);
				}
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(sqlite_effect_gate_failure());
			}
			catch (const std::length_error&)
			{
				return unexpected(sqlite_effect_gate_failure());
			}
		}

		[[nodiscard]] const sqlite_backend_open_observation*
		observed_unique_open(const sqlite_backend_connection_observation& observation,
							 const sqlite_backend_file_role role)
		{
			const sqlite_backend_open_observation* selected{};
			for (const auto& event : observation.open_events)
			{
				if (event.role != role)
					continue;
				if (selected != nullptr)
					return nullptr;
				selected = &event;
			}
			return selected;
		}

		[[nodiscard]] const sqlite_backend_open_observation*
		observed_main_open(const sqlite_backend_connection_observation& observation)
		{
			return observed_unique_open(observation, sqlite_backend_file_role::main_database);
		}

		[[nodiscard]] error sqlite_active_wal_observation_failure()
		{
			return store_error("store.sqlite-failure",
							   "sqlite-open-snapshot",
							   "active-wal-observation-io-failure");
		}

		[[nodiscard]] error sqlite_source_shm_qualification_failure()
		{
			return store_error(
				"store.backend-unavailable", "sqlite", "source-shm-readonly-qualification");
		}

		[[nodiscard]] error sqlite_active_wal_source_changed()
		{
			return store_error(
				"store.sqlite-failure", "sqlite-open-snapshot", "concurrent-source-change");
		}

		constexpr std::string_view sqlite_source_shm_readonly_profile{
			"sqlite-source-shm-readonly-unix-uri-v1"};

		[[nodiscard]] bool same_source_shm_runtime_binding(
			const sqlite_source_shm_runtime_binding& left,
			const sqlite_source_shm_runtime_binding& right) noexcept
		{
			return left.runtime_identity == right.runtime_identity &&
				left.runtime_image_identity == right.runtime_image_identity &&
				left.runtime_lifetime_identity == right.runtime_lifetime_identity &&
				left.runtime_lifetime.get() == right.runtime_lifetime.get() &&
				left.open_v2 == right.open_v2 && left.close_v2 == right.close_v2 &&
				left.exec == right.exec && left.errmsg == right.errmsg &&
				left.free_memory == right.free_memory && left.source_id == right.source_id &&
				left.uri_parameter == right.uri_parameter && left.uri_key == right.uri_key &&
				left.vfs_find == right.vfs_find && left.vfs_register == right.vfs_register &&
				left.vfs_unregister == right.vfs_unregister;
		}

		[[nodiscard]] bool valid_direct_active_wal_source_entry(
			const sqlite_backend_entry_observation* entry) noexcept
		{
			return entry != nullptr && entry->direct_regular_entry &&
				entry->state == sqlite_backend_entry_state::held_regular &&
				entry->object_identity && entry->directory_entry_identity &&
				entry->object_filesystem_profile && entry->held_object &&
				entry->held_object->object_identity() == *entry->object_identity &&
				entry->held_object->directory_entry_identity() ==
					*entry->directory_entry_identity &&
				entry->held_object->object_filesystem_profile() &&
				*entry->held_object->object_filesystem_profile() ==
					*entry->object_filesystem_profile &&
				entry->held_object->object_mount_identity() &&
				!entry->held_object->object_mount_identity()->profile.empty() &&
				!entry->held_object->object_mount_identity()->bytes.empty();
		}

		struct sqlite_active_wal_prequalification_header
		{
			std::array<std::byte, 20U> bytes;
			std::shared_ptr<sqlite_source_shm_namespace_guard> source_shm_guard;
		};

		[[nodiscard]] result<sqlite_active_wal_prequalification_header>
		observe_active_wal_prequalification_header(
			const sqlite_backend_namespace_census& source_census,
			const std::string_view logical_main_locator)
		{
			const auto* main =
				observed_entry(source_census, sqlite_backend_file_role::main_database);
			const auto* wal =
				observed_entry(source_census, sqlite_backend_file_role::write_ahead_log);
			const auto* shm =
				observed_entry(source_census, sqlite_backend_file_role::shared_memory);
			if (!valid_direct_active_wal_source_entry(main) ||
				!valid_direct_active_wal_source_entry(wal) ||
				!valid_direct_active_wal_source_entry(shm))
				return unexpected(sqlite_source_shm_qualification_failure());

			auto guard = source_census.source_shm_guard;
			if (!guard || guard->logical_main_locator() != logical_main_locator ||
				guard->anchored_main_locator().empty() || guard->identity().profile.empty() ||
				guard->identity().bytes.empty())
				return unexpected(sqlite_source_shm_qualification_failure());
			if (auto stable = guard->recheck(); !stable)
				return unexpected(sqlite_source_shm_qualification_failure());

			sqlite_active_wal_prequalification_header output;
			output.source_shm_guard = std::move(guard);
			if (auto read = main->held_object->read_exact(0U, output.bytes); !read)
				return unexpected(sqlite_quiescent_observation_failure());
			if (auto stable = output.source_shm_guard->recheck(); !stable)
				return unexpected(sqlite_source_shm_qualification_failure());
			return output;
		}

		[[nodiscard]] result<sqlite_source_shm_qualified_open_plan>
		qualify_active_wal_source_shm(
			const std::shared_ptr<sqlite_api>& api,
			const std::string_view path,
			const std::string_view vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const sqlite_backend_namespace_census& source_census)
		{
			if (!api || !backend_lifetime || !observation)
				return unexpected(sqlite_source_shm_qualification_failure());
			auto runtime = bind_source_shm_readonly_runtime(api);
			if (!runtime)
				return unexpected(sqlite_source_shm_qualification_failure());
			auto* port = observation->source_shm_readonly_port();
			if (port == nullptr)
				return unexpected(sqlite_source_shm_qualification_failure());
			const auto binding = observation->binding();
			if (binding.observation_profile.empty() || binding.registered_vfs_name != vfs_name ||
				binding.vfs_implementation_identity == nullptr ||
				binding.pinned_underlying_vfs_identity == nullptr ||
				binding.backend_lifetime_identity != backend_lifetime.get() ||
				binding.observation_capability_identity != observation.get() ||
				binding.runtime_identity != api->runtime_identity ||
				binding.runtime_lifetime_identity != api->runtime_lifetime_identity() ||
				source_census.profile != binding.observation_profile ||
				source_census.capability_token != observation->capability_token() ||
				source_census.parent_namespace_identity.profile.empty() ||
				source_census.parent_namespace_identity.bytes.empty())
				return unexpected(sqlite_source_shm_qualification_failure());
			sqlite_source_shm_qualification_request request{
				std::move(*runtime),
				std::string{path},
				source_census,
				source_census.parent_namespace_identity,
				std::string{vfs_name},
				binding.vfs_implementation_identity,
				binding.pinned_underlying_vfs_identity,
				binding.pinned_underlying_vfs_app_data_identity,
				binding.backend_lifetime_identity,
				observation->capability_token(),
			};
			auto qualified = port->qualify(request);
			if (!qualified)
				return unexpected(sqlite_source_shm_qualification_failure());
			const auto* expected_main = observed_entry(
				request.source_census, sqlite_backend_file_role::main_database);
			const auto* expected_wal = observed_entry(
				request.source_census, sqlite_backend_file_role::write_ahead_log);
			const auto* expected_shm = observed_entry(
				request.source_census, sqlite_backend_file_role::shared_memory);
			constexpr int required_open_flags = sqlite_open_readonly | sqlite_open_uri |
				sqlite_open_privatecache | sqlite_open_fullmutex;
			const auto& receipt = qualified->qualification;
			if (!valid_direct_active_wal_source_entry(expected_main) ||
				!valid_direct_active_wal_source_entry(expected_wal) ||
				!valid_direct_active_wal_source_entry(expected_shm) ||
				expected_main->object_filesystem_profile !=
					expected_wal->object_filesystem_profile ||
				expected_main->object_filesystem_profile !=
					expected_shm->object_filesystem_profile ||
				request.source_census.capability_token != request.observation_capability_token ||
				request.source_census.parent_namespace_identity !=
					request.parent_namespace_identity ||
				!same_source_shm_runtime_binding(qualified->runtime, request.runtime) ||
				qualified->canonical_vfs_locator != path ||
				qualified->delegated_vfs_locator.empty() ||
				qualified->registered_vfs_name != vfs_name ||
				qualified->application_generated_uri.empty() ||
				qualified->open_flags != required_open_flags ||
				receipt.profile != sqlite_source_shm_readonly_profile ||
				receipt.sqlite_source_id.empty() ||
				receipt.filesystem_profile.empty() ||
				receipt.runtime_identity != request.runtime.runtime_identity ||
				receipt.runtime_image_identity != request.runtime.runtime_image_identity ||
				receipt.runtime_lifetime_identity != request.runtime.runtime_lifetime_identity ||
				receipt.forwarding_vfs_identity != request.forwarding_vfs_identity ||
				receipt.pinned_underlying_vfs_identity !=
					request.pinned_underlying_vfs_identity ||
				receipt.pinned_underlying_vfs_app_data_identity !=
					request.pinned_underlying_vfs_app_data_identity ||
				receipt.backend_lifetime_identity != request.backend_lifetime_identity ||
				receipt.observation_capability_token != request.observation_capability_token ||
				receipt.parent_namespace_identity != request.parent_namespace_identity ||
				receipt.expected_shared_memory_object_identity !=
					*expected_shm->object_identity ||
				receipt.expected_shared_memory_entry_identity !=
					*expected_shm->directory_entry_identity ||
				receipt.target_namespace_epoch_identity.profile.empty() ||
				receipt.target_namespace_epoch_identity.bytes.empty() ||
				!receipt.target_namespace_epoch ||
				receipt.target_namespace_epoch->identity() !=
					receipt.target_namespace_epoch_identity ||
				receipt.target_namespace_epoch->logical_main_locator() != path ||
				receipt.target_namespace_epoch->anchored_main_locator() !=
					qualified->delegated_vfs_locator ||
				receipt.sealed_qualification_token.profile.empty() ||
				receipt.sealed_qualification_token.bytes.empty() ||
				!receipt.first_map_nonmutating || !receipt.later_map_nonmutating ||
				!receipt.cantinit_heap_wal_index_route_proven ||
				!receipt.readonly_mapped_wal_index_retry_route_proven)
				return unexpected(sqlite_source_shm_qualification_failure());
			if (auto stable = receipt.target_namespace_epoch->recheck(); !stable)
				return unexpected(sqlite_source_shm_qualification_failure());
			return qualified;
		}

		[[nodiscard]] std::optional<std::uint32_t>
		sqlite_wal_read_lock_index(const sqlite_backend_shm_lock_observation& lock) noexcept
		{
			if (lock.mode != sqlite_backend_shm_lock_mode::shared || lock.count != 1 ||
				lock.offset < sqlite_wal_read_lock_base ||
				lock.offset >= sqlite_wal_read_lock_base + sqlite_wal_read_lock_count)
				return std::nullopt;
			return static_cast<std::uint32_t>(lock.offset - sqlite_wal_read_lock_base);
		}

		[[nodiscard]] bool active_main_read_only_open(const sqlite_backend_open_observation& event,
												  const int required_role_flag) noexcept
		{
			return event.outcome == sqlite_backend_open_outcome::succeeded &&
				(event.input_flags & required_role_flag) != 0 &&
				(event.input_flags & sqlite_open_readonly) != 0 &&
				(event.input_flags & sqlite_open_uri) != 0 &&
				(event.input_flags & (sqlite_open_readwrite | sqlite_open_create)) == 0 &&
				event.returned_flags && (*event.returned_flags & sqlite_open_readonly) != 0 &&
				(*event.returned_flags & sqlite_open_readwrite) == 0 && event.object_identity &&
				event.directory_entry_identity;
		}

		[[nodiscard]] bool same_source_shm_open_callback_receipt(
			const sqlite_source_shm_open_callback_receipt& left,
			const sqlite_source_shm_open_callback_receipt& right) noexcept
		{
			return left.profile == right.profile && left.connection_token == right.connection_token &&
				left.qualification_token == right.qualification_token &&
				left.target_namespace_epoch_identity == right.target_namespace_epoch_identity &&
				left.canonical_vfs_locator == right.canonical_vfs_locator &&
				left.delegated_vfs_locator == right.delegated_vfs_locator &&
				left.application_generated_uri == right.application_generated_uri &&
				left.registered_vfs_name == right.registered_vfs_name && left.mode == right.mode &&
				left.cache == right.cache && left.readonly_shm == right.readonly_shm &&
				left.input_flags == right.input_flags &&
				left.runtime_identity == right.runtime_identity &&
				left.forwarding_vfs_identity == right.forwarding_vfs_identity &&
				left.pinned_underlying_vfs_identity == right.pinned_underlying_vfs_identity &&
				left.pinned_underlying_vfs_app_data_identity ==
					right.pinned_underlying_vfs_app_data_identity;
		}

		[[nodiscard]] bool source_shm_open_callback_matches_plan(
			const sqlite_source_shm_open_callback_receipt& receipt,
			const sqlite_source_shm_qualified_open_plan& plan,
			const sqlite_backend_connection_observation_scope& connection,
			const sqlite_backend_open_observation& main_open) noexcept
		{
			return receipt.profile == sqlite_source_shm_readonly_profile &&
				receipt.connection_token == connection.token() &&
				receipt.qualification_token == plan.qualification.sealed_qualification_token &&
				receipt.target_namespace_epoch_identity ==
					plan.qualification.target_namespace_epoch_identity &&
				receipt.canonical_vfs_locator == plan.canonical_vfs_locator &&
				receipt.delegated_vfs_locator == plan.delegated_vfs_locator &&
				receipt.application_generated_uri == plan.application_generated_uri &&
				receipt.registered_vfs_name == plan.registered_vfs_name &&
				receipt.mode == "ro" && receipt.cache == "private" && receipt.readonly_shm == "1" &&
				receipt.input_flags == main_open.input_flags &&
				receipt.runtime_identity == plan.runtime.runtime_identity &&
				receipt.forwarding_vfs_identity ==
					plan.qualification.forwarding_vfs_identity &&
				receipt.pinned_underlying_vfs_identity ==
					plan.qualification.pinned_underlying_vfs_identity &&
					receipt.pinned_underlying_vfs_app_data_identity ==
					plan.qualification.pinned_underlying_vfs_app_data_identity;
		}

		struct sqlite_source_shm_map_route_observation
		{
			bool used_cantinit_heap_route{};
		};

		[[nodiscard]] std::optional<sqlite_source_shm_map_route_observation>
		qualified_source_shm_map_route(
			const sqlite_backend_connection_observation& observation,
			const sqlite_source_shm_open_callback_receipt& receipt) noexcept
		{
			if (observation.shm_map_events.empty())
				return std::nullopt;
			sqlite_source_shm_map_route_observation route;
			for (std::size_t index{}; index < observation.shm_map_events.size(); ++index)
			{
				const auto& event = observation.shm_map_events[index];
				if ((event.caller_extend != 0 && event.caller_extend != 1) ||
					event.delegated_extend != 0 ||
					event.pinned_underlying_vfs_identity !=
						receipt.pinned_underlying_vfs_identity ||
					event.pinned_underlying_vfs_app_data_identity !=
						receipt.pinned_underlying_vfs_app_data_identity ||
					(index == 0U &&
					 (event.readonly_family_seen_before || !event.readonly_family_seen_after)) ||
					(index != 0U &&
					 (!event.readonly_family_seen_before || !event.readonly_family_seen_after)))
					return std::nullopt;
				if (event.native_status == sqlite_readonly_cantinit)
				{
					if (event.page != 0 || (index != 0U && !route.used_cantinit_heap_route) ||
						event.native_mapping_nonnull ||
						event.returned_status != sqlite_readonly_cantinit ||
						event.returned_mapping_nonnull)
						return std::nullopt;
					route.used_cantinit_heap_route = true;
					continue;
				}
				if (event.native_status != sqlite_readonly)
					return std::nullopt;
				if (event.native_mapping_nonnull)
				{
					if (event.returned_status != sqlite_readonly ||
						!event.returned_mapping_nonnull)
						return std::nullopt;
				}
				else if (event.returned_status != sqlite_readonly_cantinit ||
						 event.returned_mapping_nonnull)
				{
					return std::nullopt;
				}
				else
				{
					// READONLY/null normalization is part of the forwarding protocol, but it is
					// not the authentic page-zero CANTINIT trigger that authorizes SQLite's
					// same-connection heap WAL-index authority route.
					return std::nullopt;
				}
			}
			return route;
		}

		[[nodiscard]] bool valid_source_shm_read_lock_route(
			const bool used_cantinit_heap_route, const std::uint32_t read_lock_index) noexcept
		{
			return !used_cantinit_heap_route || read_lock_index == 0U;
		}

		[[nodiscard]] result<void> recheck_source_shm_epoch_during_live_read(
			const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
			const bool transaction_live,
			const bool used_cantinit_heap_route,
			const std::uint32_t read_lock_index)
		{
			if (!epoch || !transaction_live ||
				!valid_source_shm_read_lock_route(used_cantinit_heap_route, read_lock_index))
				return unexpected(sqlite_source_shm_qualification_failure());
			if (auto stable = epoch->recheck(); !stable)
				return unexpected(sqlite_source_shm_qualification_failure());
			return {};
		}

		[[nodiscard]] result<void> finish_source_shm_epoch_after_confirmed_close(
			const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
			const bool connection_close_confirmed)
		{
			if (!epoch || !connection_close_confirmed)
				return unexpected(sqlite_source_shm_qualification_failure());
			if (auto finished = epoch->finish(); !finished)
				return unexpected(sqlite_source_shm_qualification_failure());
			return {};
		}

		[[nodiscard]] result<void> retained_active_wal_epoch_entries_match_source(
			const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
			const sqlite_backend_namespace_census& source)
		{
			if (!epoch)
				return unexpected(sqlite_source_shm_qualification_failure());
			constexpr std::array roles{
				sqlite_backend_file_role::main_database,
				sqlite_backend_file_role::write_ahead_log,
				sqlite_backend_file_role::shared_memory,
			};
			for (const auto role : roles)
			{
				const auto* expected = observed_entry(source, role);
				auto retained = epoch->retained_entry(role);
				if (!valid_direct_active_wal_source_entry(expected) || !retained ||
					retained->role != role ||
					!retained->direct_regular_entry ||
					retained->state != sqlite_backend_entry_state::held_regular ||
					!retained->object_identity || !retained->directory_entry_identity ||
					!retained->held_object || !retained->object_filesystem_profile ||
					retained->object_identity != expected->object_identity ||
					retained->directory_entry_identity != expected->directory_entry_identity ||
					retained->held_object != expected->held_object ||
					retained->object_filesystem_profile != expected->object_filesystem_profile)
					return unexpected(sqlite_source_shm_qualification_failure());
			}
			return {};
		}

		[[nodiscard]] bool valid_active_wal_shm_recheck_transition(
			const bool same_object_identity,
			const bool same_entry_identity,
			const std::uint64_t before_byte_count,
			const std::uint64_t after_byte_count) noexcept
		{
			return same_object_identity && same_entry_identity &&
				after_byte_count >= before_byte_count;
		}

		[[nodiscard]] bool
		active_wal_read_only_open(const sqlite_backend_open_observation& event) noexcept
		{
			constexpr int requested_flags =
				sqlite_open_readwrite | sqlite_open_create | sqlite_open_write_ahead_log;
			return event.outcome == sqlite_backend_open_outcome::succeeded &&
				event.input_flags == requested_flags && event.returned_flags &&
				(*event.returned_flags & sqlite_open_write_ahead_log) != 0 &&
				(*event.returned_flags & sqlite_open_readonly) != 0 &&
				(*event.returned_flags & (sqlite_open_readwrite | sqlite_open_create)) == 0 &&
				event.object_identity && event.directory_entry_identity;
		}

		[[nodiscard]] result<sqlite_wal_header_receipt>
		read_active_wal_header(const sqlite_backend_held_object& held_wal)
		{
			std::array<std::byte, sqlite_wal_header_byte_count> header{};
			if (auto read = held_wal.read_exact(0U, header); !read)
				return unexpected(sqlite_active_wal_observation_failure());
			auto parsed = parse_sqlite_wal_header(header);
			if (!parsed)
				return unexpected(sqlite_active_wal_observation_failure());
			return parsed;
		}

		[[nodiscard]] bool active_wal_header_receipt_required(
			const bool initial_observation, const std::uint32_t read_lock_index) noexcept
		{
			return initial_observation || read_lock_index != 0U;
		}

		[[nodiscard]] result<sqlite_active_wal_read_anchor>
		observe_active_wal_read(const sqlite_effect_connection_scope& connection,
								const sqlite_backend_observation_capability& observation,
				const std::string_view path,
				const sqlite_quiescent_source_anchor& source,
				const sqlite_source_shm_qualified_open_plan* qualified_plan,
				const sqlite_active_wal_read_anchor* prior_anchor,
				const bool transaction_live,
				const bool recheck)
		{
			if (!connection.observation || connection.gate == nullptr ||
				connection.gate->stage() != sqlite_backend_effect_stage::denied ||
				!connection.gate->enforcement_active())
				return unexpected(sqlite_active_wal_observation_failure());
			auto latest = connection.gate->latest_receipt();
			if (!latest || !effect_receipts_equal(*latest, connection.denied_receipt))
				return unexpected(sqlite_active_wal_observation_failure());
			auto current = connection.observation->snapshot();
			if (!current || !current->complete || !current->main_handle_open ||
				current->capability_token != observation.capability_token() ||
				current->connection_token != connection.observation->token())
				return unexpected(sqlite_active_wal_observation_failure());
			const auto* main_open = observed_main_open(*current);
			const auto* wal_open =
				observed_unique_open(*current, sqlite_backend_file_role::write_ahead_log);
			const auto* source_main =
				observed_entry(source.namespace_census, sqlite_backend_file_role::main_database);
			const auto* source_wal =
				observed_entry(source.namespace_census, sqlite_backend_file_role::write_ahead_log);
			const auto* source_shm =
				observed_entry(source.namespace_census, sqlite_backend_file_role::shared_memory);
			const auto* source_journal =
				observed_entry(source.namespace_census, sqlite_backend_file_role::rollback_journal);
			if (main_open == nullptr || wal_open == nullptr || source_main == nullptr ||
				source_wal == nullptr || source_shm == nullptr || source_journal == nullptr ||
				!active_main_read_only_open(*main_open, sqlite_open_main_database) ||
				!active_wal_read_only_open(*wal_open) ||
				source_main->state != sqlite_backend_entry_state::held_regular ||
				source_wal->state != sqlite_backend_entry_state::held_regular ||
				source_shm->state != sqlite_backend_entry_state::held_regular ||
				source_journal->state != sqlite_backend_entry_state::absent ||
				!source_main->held_object || !source_wal->held_object || !source_shm->held_object ||
				main_open->object_identity != source_main->object_identity ||
				main_open->directory_entry_identity != source_main->directory_entry_identity ||
				wal_open->object_identity != source_wal->object_identity ||
				wal_open->directory_entry_identity != source_wal->directory_entry_identity ||
				current->shared_memory_object_identity != source_shm->object_identity ||
				current->shared_memory_entry_identity != source_shm->directory_entry_identity)
				return unexpected(recheck ? sqlite_active_wal_source_changed()
										  : sqlite_active_wal_observation_failure());
			if (!current->source_shm_open_callback_receipt ||
				(qualified_plan == nullptr) == (prior_anchor == nullptr) ||
				(qualified_plan != nullptr &&
				 !source_shm_open_callback_matches_plan(*current->source_shm_open_callback_receipt,
												 *qualified_plan,
												 *connection.observation,
												 *main_open)) ||
				(prior_anchor != nullptr &&
				 !same_source_shm_open_callback_receipt(*current->source_shm_open_callback_receipt,
											  prior_anchor->source_shm_open_receipt)))
				return unexpected(recheck ? sqlite_active_wal_source_changed()
										  : sqlite_active_wal_observation_failure());
			auto map_route = qualified_source_shm_map_route(
				*current, *current->source_shm_open_callback_receipt);
			if (!map_route)
				return unexpected(sqlite_active_wal_observation_failure());
			if (current->held_shm_locks.size() != 1U)
				return unexpected(recheck ? sqlite_active_wal_source_changed()
										  : sqlite_active_wal_observation_failure());
			const auto read_lock_index =
				sqlite_wal_read_lock_index(current->held_shm_locks.front());
			if (!read_lock_index)
				return unexpected(recheck ? sqlite_active_wal_source_changed()
									  : sqlite_active_wal_observation_failure());
			if (!valid_source_shm_read_lock_route(map_route->used_cantinit_heap_route,
										  *read_lock_index))
				return unexpected(recheck ? sqlite_active_wal_source_changed()
									  : sqlite_active_wal_observation_failure());
			const auto target_epoch = qualified_plan != nullptr
				? qualified_plan->qualification.target_namespace_epoch
				: prior_anchor->target_namespace_epoch;
			const auto& target_epoch_identity = qualified_plan != nullptr
				? qualified_plan->qualification.target_namespace_epoch_identity
				: prior_anchor->target_namespace_epoch_identity;
			if (!target_epoch || target_epoch_identity.profile.empty() ||
				target_epoch_identity.bytes.empty() ||
				target_epoch->identity() != target_epoch_identity ||
				target_epoch->logical_main_locator() != path ||
				target_epoch->anchored_main_locator() !=
					current->source_shm_open_callback_receipt->delegated_vfs_locator)
				return unexpected(sqlite_source_shm_qualification_failure());
			if (auto stable = recheck_source_shm_epoch_during_live_read(
					target_epoch,
					transaction_live,
					map_route->used_cantinit_heap_route,
					*read_lock_index);
				!stable)
				return unexpected(std::move(stable.error()));

			if (auto retained = retained_active_wal_epoch_entries_match_source(
					target_epoch, source.namespace_census);
				!retained)
				return unexpected(std::move(retained.error()));

			auto main_size = source_main->held_object->size();
			auto main_sha256 = source_main->held_object->sha256();
			auto shared_memory_size = source_shm->held_object->size();
			if (!main_size || !main_sha256 || !shared_memory_size)
				return unexpected(sqlite_active_wal_observation_failure());
			sqlite_active_wal_read_anchor anchor{
				*main_open->object_identity,
				*main_open->directory_entry_identity,
				*wal_open->object_identity,
				*wal_open->directory_entry_identity,
				*current->shared_memory_object_identity,
				*current->shared_memory_entry_identity,
				current->held_shm_locks.front(),
				*read_lock_index,
				map_route->used_cantinit_heap_route,
				target_epoch_identity,
				target_epoch,
				*current->source_shm_open_callback_receipt,
				{*main_size, std::move(*main_sha256)},
				*shared_memory_size,
				std::nullopt,
			};
			if (active_wal_header_receipt_required(!recheck, anchor.read_lock_index))
			{
				auto header = read_active_wal_header(*source_wal->held_object);
				if (!header)
					return unexpected(std::move(header.error()));
				anchor.wal_header = std::move(*header);
			}
			return anchor;
		}

		[[nodiscard]] result<void>
		recheck_active_wal_read(const sqlite_effect_connection_scope& connection,
								const sqlite_backend_observation_capability& observation,
								const std::string_view path,
								sqlite_quiescent_source_anchor& source,
								const sqlite_active_wal_read_anchor& before,
								const bool transaction_live)
		{
			auto after = observe_active_wal_read(
				connection, observation, path, source, nullptr, &before, transaction_live, true);
			if (!after)
				return unexpected(std::move(after.error()));
			if (after->main_object_identity != before.main_object_identity ||
				after->main_entry_identity != before.main_entry_identity ||
				after->wal_object_identity != before.wal_object_identity ||
				after->wal_entry_identity != before.wal_entry_identity ||
				!valid_active_wal_shm_recheck_transition(
					after->shm_object_identity == before.shm_object_identity,
					after->shm_entry_identity == before.shm_entry_identity,
					before.shared_memory_byte_count,
					after->shared_memory_byte_count) ||
				after->read_lock.offset != before.read_lock.offset ||
				after->read_lock.count != before.read_lock.count ||
				after->read_lock.mode != before.read_lock.mode ||
				after->read_lock_index != before.read_lock_index ||
				after->used_cantinit_heap_route != before.used_cantinit_heap_route)
				return unexpected(sqlite_active_wal_source_changed());
			if (after->target_namespace_epoch_identity !=
					before.target_namespace_epoch_identity ||
				after->target_namespace_epoch.get() != before.target_namespace_epoch.get())
				return unexpected(sqlite_source_shm_qualification_failure());
			if (!before.wal_header)
				return unexpected(sqlite_active_wal_source_changed());
			if (before.read_lock_index == 0U)
			{
				if (after->main_before != before.main_before)
					return unexpected(sqlite_active_wal_source_changed());
			}
			else if (after->wal_header != before.wal_header)
			{
				return unexpected(sqlite_active_wal_source_changed());
			}
			source.byte_count = after->main_before.byte_count;
			source.sha256 = after->main_before.sha256;
			return {};
		}

		[[nodiscard]] bool same_wal_scan_receipt(const sqlite_wal_scan_receipt& left,
												 const sqlite_wal_scan_receipt& right) noexcept
		{
			return left.classification == right.classification && left.stop == right.stop &&
				left.header == right.header && left.last_valid_frame == right.last_valid_frame &&
				left.last_valid_commit == right.last_valid_commit &&
				left.inspected_byte_count == right.inspected_byte_count &&
				left.validated_prefix_byte_count == right.validated_prefix_byte_count &&
				left.authoritative_prefix_byte_count == right.authoritative_prefix_byte_count &&
				left.valid_frame_count == right.valid_frame_count &&
				left.valid_commit_count == right.valid_commit_count &&
				left.torn_remainder_byte_count == right.torn_remainder_byte_count;
		}

		[[nodiscard]] error sqlite_wal_only_source_changed()
		{
			return store_error("store.sqlite-failure",
							   "sqlite-initialization-sidecar",
							   "concurrent-source-change");
		}

		[[nodiscard]] error sqlite_wal_only_unrecognized()
		{
			return store_error("store.sqlite-failure",
							   "sqlite-initialization-sidecar",
							   "unrecognized-preauthority-state");
		}

		[[nodiscard]] error sqlite_reopen_required()
		{
			return sqlite_store_reopen_required_error();
		}

		[[nodiscard]] error sqlite_recovery_handoff_opaque()
		{
			return store_error("store.sqlite-failure", "sqlite-recovery-handoff", "opaque");
		}

		[[nodiscard]] result<void>
		recheck_wal_only_source(const sqlite_quiescent_source_anchor& source,
								const sqlite_wal_source_capture& capture,
								const std::string_view path,
								const sqlite_backend_observation_capability& observation)
		{
			const auto* main =
				observed_entry(source.namespace_census, sqlite_backend_file_role::main_database);
			const auto* wal =
				observed_entry(source.namespace_census, sqlite_backend_file_role::write_ahead_log);
			const auto* shm =
				observed_entry(source.namespace_census, sqlite_backend_file_role::shared_memory);
			const auto* journal =
				observed_entry(source.namespace_census, sqlite_backend_file_role::rollback_journal);
			if (main == nullptr || wal == nullptr || shm == nullptr || journal == nullptr ||
				main->state != sqlite_backend_entry_state::held_regular ||
				wal->state != sqlite_backend_entry_state::held_regular ||
				shm->state != sqlite_backend_entry_state::absent ||
				journal->state != sqlite_backend_entry_state::absent || !main->held_object ||
				!wal->held_object || !capture.workspace ||
				capture.full_source_main.byte_count != source.byte_count ||
				capture.full_source_main.sha256 != source.sha256 ||
				capture.workspace_receipt.source_capability_token !=
					observation.capability_token() ||
				capture.workspace_receipt.main_database != capture.full_source_main ||
				!same_wal_scan_receipt(capture.workspace_receipt.source_wal_scan, capture.wal_scan))
				return unexpected(sqlite_wal_only_unrecognized());

			for (const auto* held : {main->held_object.get(), wal->held_object.get()})
			{
				auto replacement = held->recheck_current_entry();
				if (!replacement ||
					*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object)
					return unexpected(sqlite_wal_only_source_changed());
			}
			auto namespace_matches = observation.recheck_namespace(source.namespace_census, path);
			if (!namespace_matches || !*namespace_matches)
				return unexpected(sqlite_wal_only_source_changed());

			auto main_size = main->held_object->size();
			auto main_sha256 = main->held_object->sha256();
			auto wal_size = wal->held_object->size();
			auto wal_sha256 = wal->held_object->sha256();
			if (!main_size || !main_sha256 || !wal_size || !wal_sha256 ||
				*main_size != capture.full_source_main.byte_count ||
				*main_sha256 != capture.full_source_main.sha256 ||
				*wal_size != capture.full_source_wal.byte_count ||
				*wal_sha256 != capture.full_source_wal.sha256)
				return unexpected(sqlite_wal_only_source_changed());
			if (auto verified = capture.workspace->verify_sealed_objects(); !verified)
				return unexpected(std::move(verified.error()));
			return {};
		}

		[[nodiscard]] result<sqlite_quiescent_source_anchor>
		capture_recovered_source_anchor(const sqlite_quiescent_source_anchor& original,
										const std::string_view path,
										const sqlite_backend_observation_capability& observation)
		{
			auto current = observation.capture_namespace(path);
			if (!current)
				return unexpected(sqlite_quiescent_observation_failure());
			if (auto valid = validate_namespace_census(*current, observation); !valid)
				return unexpected(std::move(valid.error()));
			const auto* original_main =
				observed_entry(original.namespace_census, sqlite_backend_file_role::main_database);
			const auto* main = observed_entry(*current, sqlite_backend_file_role::main_database);
			const auto* wal = observed_entry(*current, sqlite_backend_file_role::write_ahead_log);
			const auto* shm = observed_entry(*current, sqlite_backend_file_role::shared_memory);
			const auto* journal =
				observed_entry(*current, sqlite_backend_file_role::rollback_journal);
			if (original_main == nullptr || main == nullptr || wal == nullptr || shm == nullptr ||
				journal == nullptr || main->state != sqlite_backend_entry_state::held_regular ||
				!main->held_object || main->object_identity != original_main->object_identity ||
				main->directory_entry_identity != original_main->directory_entry_identity ||
				journal->state != sqlite_backend_entry_state::absent ||
				wal->state == sqlite_backend_entry_state::present_unreadable ||
				wal->state == sqlite_backend_entry_state::unsupported_kind ||
				shm->state == sqlite_backend_entry_state::present_unreadable ||
				shm->state == sqlite_backend_entry_state::unsupported_kind ||
				(wal->state == sqlite_backend_entry_state::absent &&
				 shm->state != sqlite_backend_entry_state::absent))
				return unexpected(sqlite_wal_only_source_changed());
			if ((wal->state == sqlite_backend_entry_state::held_regular && !wal->held_object) ||
				(shm->state == sqlite_backend_entry_state::held_regular && !shm->held_object))
				return unexpected(sqlite_quiescent_observation_failure());
			if (wal->held_object)
			{
				auto wal_size = wal->held_object->size();
				if (!wal_size)
					return unexpected(sqlite_quiescent_observation_failure());
				if (*wal_size != 0U)
					return unexpected(sqlite_wal_only_source_changed());
			}
			auto byte_count = main->held_object->size();
			auto sha256 = main->held_object->sha256();
			if (!byte_count || !sha256)
				return unexpected(sqlite_quiescent_observation_failure());
			std::array<std::byte, 20U> sqlite_header{};
			if (*byte_count < sqlite_header.size())
				return unexpected(sqlite_wal_only_source_changed());
			if (auto read = main->held_object->read_exact(0U, sqlite_header); !read)
				return unexpected(sqlite_quiescent_observation_failure());
			if (sqlite_header[18U] != std::byte{2U} || sqlite_header[19U] != std::byte{2U})
				return unexpected(sqlite_wal_only_source_changed());
			return sqlite_quiescent_source_anchor{
				std::move(*current), *byte_count, std::move(*sha256)};
		}

		[[nodiscard]] result<void> checkpoint_wal_for_recovery(sqlite_database& database)
		{
			auto checkpoint =
				sqlite_statement::prepare(database, "PRAGMA wal_checkpoint(TRUNCATE)", true);
			if (!checkpoint)
				return unexpected(std::move(checkpoint.error()));
			if (checkpoint->step() != sqlite_row)
				return unexpected(
					store_error("store.sqlite-failure", "database", "wal-checkpoint"));
			auto busy = checkpoint->column_signed(0);
			auto log_frames = checkpoint->column_signed(1);
			auto checkpointed_frames = checkpoint->column_signed(2);
			if (!busy || !log_frames || !checkpointed_frames || *busy != 0 || *log_frames < 0 ||
				*checkpointed_frames < 0 || *checkpointed_frames > *log_frames ||
				checkpoint->step() != sqlite_done)
				return unexpected(
					store_error("store.sqlite-failure", "database", "wal-checkpoint"));
			return {};
		}

		[[nodiscard]] result<void> validate_closed_wal_recovery_workspace(
			sqlite_wal_source_capture& capture,
			const sqlite_backend_observation_capability& observation)
		{
			if (!capture.workspace)
				return unexpected(sqlite_wal_only_unrecognized());
			if (auto verified = capture.workspace->verify_sealed_objects(); !verified)
				return unexpected(std::move(verified.error()));
			auto receipt = capture.workspace->snapshot_receipt();
			if (!receipt)
				return unexpected(std::move(receipt.error()));
			constexpr int expected_main_input = sqlite_open_readwrite | sqlite_open_main_database;
			const bool private_wal = capture.wal_scan.authoritative_prefix_byte_count != 0U;
			const bool main_open_valid = private_wal ? receipt->opens.main_attempt_count == 1U &&
					receipt->opens.main_success_count == 1U &&
					receipt->opens.last_main_input_flags == expected_main_input &&
					(receipt->opens.last_main_output_flags & sqlite_open_main_database) != 0 &&
					(receipt->opens.last_main_output_flags & sqlite_open_readwrite) != 0 &&
					(receipt->opens.last_main_output_flags &
					 (sqlite_open_readonly | sqlite_open_create | sqlite_open_uri)) == 0 &&
					receipt->opens.main_readwrite_no_create
													 : receipt->opens.main_attempt_count == 0U &&
					receipt->opens.main_success_count == 0U &&
					!receipt->opens.main_readwrite_no_create;
			const bool wal_open_valid = private_wal ? receipt->opens.wal_attempt_count >= 1U &&
					receipt->opens.wal_success_count == receipt->opens.wal_attempt_count &&
					(receipt->opens.last_wal_input_flags & sqlite_open_write_ahead_log) != 0 &&
					(receipt->opens.last_wal_output_flags & sqlite_open_write_ahead_log) != 0 &&
					(receipt->opens.last_wal_output_flags & sqlite_open_readwrite) != 0 &&
					receipt->opens.wal_was_preexisting
													: receipt->opens.wal_attempt_count == 0U &&
					receipt->opens.wal_success_count == 0U && !receipt->opens.wal_was_preexisting;
			const bool shm_valid = private_wal
				? receipt->shm.map_request_count >= 1U && receipt->shm.created_region_count >= 1U &&
					receipt->shm.private_byte_count > 0U && receipt->shm.lock_request_count >= 1U &&
					receipt->shm.unlock_request_count >= 1U && receipt->shm.barrier_count >= 1U &&
					receipt->shm.unmap_request_count >= 1U && receipt->shm.held_lock_count == 0U
				: receipt->shm.map_request_count == 0U && receipt->shm.created_region_count == 0U &&
					receipt->shm.private_byte_count == 0U && receipt->shm.held_lock_count == 0U;
			if (receipt->profile.empty() ||
				receipt->source_capability_token != observation.capability_token() ||
				receipt->main_database != capture.full_source_main ||
				receipt->authoritative_wal_prefix !=
					capture.workspace_receipt.authoritative_wal_prefix ||
				!same_wal_scan_receipt(receipt->source_wal_scan, capture.wal_scan) ||
				receipt->private_wal_present != private_wal || !receipt->sealed ||
				!main_open_valid || !wal_open_valid || !shm_valid ||
				!receipt->effects.only_private_shm_mutation_permitted ||
				receipt->effects.denied_rollback_journal_open_count != 0U ||
				receipt->effects.denied_delete_count != 0U ||
				receipt->effects.denied_other_open_count != 0U)
				return unexpected(sqlite_wal_only_unrecognized());
			capture.workspace_receipt = std::move(*receipt);
			return {};
		}

		struct sqlite_wal_coordination_evidence
		{
			sqlite_backend_effect_arm_receipt prior_receipt;
			sqlite_backend_open_observation wal_open;
			sqlite_backend_opaque_identity shared_memory_object_identity;
			sqlite_backend_opaque_identity shared_memory_entry_identity;
			std::array<sqlite_backend_stat_only_entry_observation, 4U> entries;
		};

		[[nodiscard]] const sqlite_backend_stat_only_entry_observation* observed_stat_entry(
			const std::array<sqlite_backend_stat_only_entry_observation, 4U>& entries,
			const sqlite_backend_file_role role)
		{
			const sqlite_backend_stat_only_entry_observation* selected{};
			for (const auto& entry : entries)
			{
				if (entry.role != role)
					continue;
				if (selected != nullptr)
					return nullptr;
				selected = &entry;
			}
			return selected;
		}

		[[nodiscard]] bool open_observations_equal(const sqlite_backend_open_observation& left,
												   const sqlite_backend_open_observation& right)
		{
			return left.role == right.role && left.input_flags == right.input_flags &&
				left.outcome == right.outcome && left.returned_flags == right.returned_flags &&
				left.object_identity == right.object_identity &&
				left.directory_entry_identity == right.directory_entry_identity;
		}

		[[nodiscard]] bool
		stat_observations_equal(const sqlite_backend_stat_only_entry_observation& left,
								const sqlite_backend_stat_only_entry_observation& right)
		{
			return left.role == right.role && left.state == right.state &&
				left.parent_namespace_identity == right.parent_namespace_identity &&
				left.object_identity == right.object_identity &&
				left.directory_entry_identity == right.directory_entry_identity;
		}

		[[nodiscard]] result<sqlite_wal_coordination_evidence>
		capture_wal_coordination_evidence(const sqlite_effect_connection_scope& connection,
										  const sqlite_backend_observation_capability& observation,
										  const std::string_view path,
										  const sqlite_quiescent_source_anchor& anchor,
										  const sqlite_backend_effect_arm_receipt& prior_receipt)
		{
			if (!connection.observation || connection.gate == nullptr ||
				prior_receipt.stage != sqlite_backend_effect_stage::wal_shm_coordination_only ||
				prior_receipt.capability_token != observation.capability_token() ||
				prior_receipt.connection_token != connection.observation->token() ||
				prior_receipt.canonical_vfs_locator != path ||
				connection.gate->stage() != sqlite_backend_effect_stage::wal_shm_coordination_only)
				return unexpected(sqlite_effect_gate_failure());
			auto latest = connection.gate->latest_receipt();
			if (!latest || !effect_receipts_equal(*latest, prior_receipt))
				return unexpected(sqlite_effect_gate_failure());

			auto current = connection.observation->snapshot();
			if (!current || !current->complete || !current->main_handle_open ||
				current->capability_token != observation.capability_token() ||
				current->connection_token != connection.observation->token() ||
				!current->held_shm_locks.empty() || !current->shared_memory_object_identity ||
				!current->shared_memory_entry_identity)
				return unexpected(sqlite_effect_gate_failure());
			const auto* main_open = observed_main_open(*current);
			const auto* wal_open =
				observed_unique_open(*current, sqlite_backend_file_role::write_ahead_log);
			constexpr int created_wal_flags =
				sqlite_open_readwrite | sqlite_open_create | sqlite_open_write_ahead_log;
			constexpr int existing_wal_flags = sqlite_open_readwrite | sqlite_open_write_ahead_log;
			if (main_open == nullptr || wal_open == nullptr ||
				main_open->outcome != sqlite_backend_open_outcome::succeeded ||
				wal_open->outcome != sqlite_backend_open_outcome::succeeded ||
				(wal_open->input_flags != created_wal_flags &&
				 wal_open->input_flags != existing_wal_flags) ||
				!wal_open->returned_flags ||
				(*wal_open->returned_flags & sqlite_open_readwrite) == 0 ||
				(*wal_open->returned_flags & sqlite_open_readonly) != 0 ||
				!wal_open->object_identity || !wal_open->directory_entry_identity)
				return unexpected(sqlite_effect_gate_failure());

			sqlite_wal_coordination_evidence evidence;
			evidence.prior_receipt = prior_receipt;
			evidence.wal_open = *wal_open;
			evidence.shared_memory_object_identity = *current->shared_memory_object_identity;
			evidence.shared_memory_entry_identity = *current->shared_memory_entry_identity;
			constexpr std::array roles{
				sqlite_backend_file_role::main_database,
				sqlite_backend_file_role::write_ahead_log,
				sqlite_backend_file_role::shared_memory,
				sqlite_backend_file_role::rollback_journal,
			};
			for (std::size_t index{}; index < roles.size(); ++index)
			{
				auto observed = observation.observe_entry_state_without_open(path, roles.at(index));
				if (!observed)
					return unexpected(std::move(observed.error()));
				evidence.entries.at(index) = std::move(*observed);
			}

			const auto* source_main =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::main_database);
			const auto* source_wal =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::write_ahead_log);
			const auto* source_shm =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::shared_memory);
			const auto* source_journal =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::rollback_journal);
			const auto* current_main =
				observed_stat_entry(evidence.entries, sqlite_backend_file_role::main_database);
			const auto* current_wal =
				observed_stat_entry(evidence.entries, sqlite_backend_file_role::write_ahead_log);
			const auto* current_shm =
				observed_stat_entry(evidence.entries, sqlite_backend_file_role::shared_memory);
			const auto* current_journal =
				observed_stat_entry(evidence.entries, sqlite_backend_file_role::rollback_journal);
			const bool source_sidecars_absent = source_wal != nullptr && source_shm != nullptr &&
				source_wal->state == sqlite_backend_entry_state::absent &&
				source_shm->state == sqlite_backend_entry_state::absent;
			const bool source_sidecars_held = source_wal != nullptr && source_shm != nullptr &&
				source_wal->state == sqlite_backend_entry_state::held_regular &&
				source_shm->state == sqlite_backend_entry_state::held_regular;
			const bool source_wal_only = source_wal != nullptr && source_shm != nullptr &&
				source_wal->state == sqlite_backend_entry_state::held_regular &&
				source_shm->state == sqlite_backend_entry_state::absent;
			if (source_main == nullptr || source_wal == nullptr || source_shm == nullptr ||
				source_journal == nullptr || current_main == nullptr || current_wal == nullptr ||
				current_shm == nullptr || current_journal == nullptr ||
				source_main->state != sqlite_backend_entry_state::held_regular ||
				(!source_sidecars_absent && !source_sidecars_held && !source_wal_only) ||
				(source_sidecars_absent && wal_open->input_flags != created_wal_flags) ||
				source_journal->state != sqlite_backend_entry_state::absent ||
				current_main->state != sqlite_backend_entry_state::held_regular ||
				current_main->parent_namespace_identity !=
					anchor.namespace_census.parent_namespace_identity ||
				current_main->object_identity != source_main->object_identity ||
				current_main->directory_entry_identity != source_main->directory_entry_identity ||
				current_wal->state != sqlite_backend_entry_state::held_regular ||
				current_wal->parent_namespace_identity !=
					anchor.namespace_census.parent_namespace_identity ||
				current_wal->object_identity != wal_open->object_identity ||
				current_wal->directory_entry_identity != wal_open->directory_entry_identity ||
				((source_sidecars_held || source_wal_only) &&
				 (current_wal->object_identity != source_wal->object_identity ||
				  current_wal->directory_entry_identity != source_wal->directory_entry_identity)) ||
				current_shm->state != sqlite_backend_entry_state::held_regular ||
				current_shm->parent_namespace_identity !=
					anchor.namespace_census.parent_namespace_identity ||
				current_shm->object_identity != current->shared_memory_object_identity ||
				current_shm->directory_entry_identity != current->shared_memory_entry_identity ||
				(source_sidecars_held &&
				 (current_shm->object_identity != source_shm->object_identity ||
				  current_shm->directory_entry_identity != source_shm->directory_entry_identity)) ||
				current_journal->state != sqlite_backend_entry_state::absent ||
				current_journal->parent_namespace_identity !=
					anchor.namespace_census.parent_namespace_identity ||
				current_journal->object_identity || current_journal->directory_entry_identity)
				return unexpected(sqlite_effect_gate_failure());
			return evidence;
		}

		[[nodiscard]] result<sqlite_backend_opaque_identity>
		make_wal_coordination_prerequisite(const std::string_view profile,
										   const std::string_view path,
										   const sqlite_backend_opaque_identity& capability_token,
										   const sqlite_backend_opaque_identity& connection_token,
										   const sqlite_wal_coordination_evidence& evidence,
										   const sqlite_wal_source_capture* wal_capture)
		{
			try
			{
				sqlite_backend_opaque_identity output{std::string{profile}, {}};
				auto& bytes = output.bytes;
				bytes.reserve(1024U + path.size());
				append_effect_bytes(bytes, profile);
				append_effect_bytes(bytes, path);
				append_effect_identity(bytes, capability_token);
				append_effect_identity(bytes, connection_token);
				append_effect_receipt(bytes, evidence.prior_receipt);
				bytes.push_back(std::byte{static_cast<std::uint8_t>(evidence.wal_open.role)});
				append_effect_u64(bytes, static_cast<std::uint64_t>(evidence.wal_open.input_flags));
				bytes.push_back(std::byte{static_cast<std::uint8_t>(evidence.wal_open.outcome)});
				bytes.push_back(evidence.wal_open.returned_flags ? std::byte{1U} : std::byte{0U});
				if (evidence.wal_open.returned_flags)
					append_effect_u64(
						bytes, static_cast<std::uint64_t>(*evidence.wal_open.returned_flags));
				append_effect_optional_identity(bytes, evidence.wal_open.object_identity);
				append_effect_optional_identity(bytes, evidence.wal_open.directory_entry_identity);
				append_effect_identity(bytes, evidence.shared_memory_object_identity);
				append_effect_identity(bytes, evidence.shared_memory_entry_identity);
				for (const auto& entry : evidence.entries)
				{
					bytes.push_back(std::byte{static_cast<std::uint8_t>(entry.role)});
					bytes.push_back(std::byte{static_cast<std::uint8_t>(entry.state)});
					append_effect_identity(bytes, entry.parent_namespace_identity);
					append_effect_optional_identity(bytes, entry.object_identity);
					append_effect_optional_identity(bytes, entry.directory_entry_identity);
				}
				append_effect_wal_capture(bytes, wal_capture);
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(sqlite_effect_gate_failure());
			}
			catch (const std::length_error&)
			{
				return unexpected(sqlite_effect_gate_failure());
			}
		}

		class sqlite_store_effect_arm_authority final : public sqlite_backend_effect_arm_authority
		{
		  public:
			struct anchor_entry
			{
				sqlite_backend_file_role role{sqlite_backend_file_role::main_database};
				sqlite_backend_entry_state state{sqlite_backend_entry_state::absent};
				std::optional<sqlite_backend_opaque_identity> object_identity;
				std::optional<sqlite_backend_opaque_identity> directory_entry_identity;
			};

			sqlite_store_effect_arm_authority(
				std::shared_ptr<sqlite_backend_observation_capability> observation,
				std::string path,
				sqlite_backend_opaque_identity capability_token,
				sqlite_backend_opaque_identity connection_token,
				sqlite_backend_opaque_identity prerequisite,
				sqlite_backend_effect_stage target_stage,
				const sqlite_quiescent_source_anchor* anchor,
				bool ephemeral,
				const sqlite_wal_coordination_evidence* coordination = nullptr,
				const sqlite_wal_source_capture* wal_capture = nullptr)
				: observation_{std::move(observation)}, path_{std::move(path)},
				  capability_token_{std::move(capability_token)},
				  connection_token_{std::move(connection_token)},
				  prerequisite_{std::move(prerequisite)}, target_stage_{target_stage},
				  ephemeral_{ephemeral}
			{
				if (coordination != nullptr)
					coordination_ = *coordination;
				if (anchor == nullptr)
					return;
				requires_wal_anchor_ = wal_capture != nullptr;
				if (wal_capture != nullptr)
				{
					wal_anchor_byte_count_ = wal_capture->full_source_wal.byte_count;
					wal_anchor_sha256_ = wal_capture->full_source_wal.sha256;
				}
				has_anchor_ = true;
				parent_namespace_identity_ = anchor->namespace_census.parent_namespace_identity;
				anchor_byte_count_ = anchor->byte_count;
				anchor_sha256_ = anchor->sha256;
				for (std::size_t index{}; index < anchor_entries_.size(); ++index)
				{
					const auto& source = anchor->namespace_census.entries.at(index);
					auto& destination = anchor_entries_.at(index);
					destination = {source.role,
								   source.state,
								   source.object_identity,
								   source.directory_entry_identity};
					if (source.role == sqlite_backend_file_role::main_database)
						main_held_object_ = source.held_object;
					if (source.role == sqlite_backend_file_role::write_ahead_log)
						wal_held_object_ = source.held_object;
				}
			}

			[[nodiscard]] result<sqlite_backend_opaque_identity> recheck_and_seal(
				const sqlite_backend_effect_arm_request& request,
				const sqlite_backend_connection_observation_scope& connection) const override
			{
				if (!observation_ || request.target_stage != target_stage_ ||
					request.capability_token != capability_token_ ||
					request.connection_token != connection_token_ ||
					request.canonical_vfs_locator != path_ ||
					request.prerequisite_receipt != prerequisite_ ||
					connection.token() != connection_token_)
				{
					return unexpected(sqlite_effect_gate_failure());
				}
				auto current = connection.snapshot();
				if (!current || !current->complete ||
					current->capability_token != capability_token_ ||
					current->connection_token != connection_token_)
				{
					return unexpected(sqlite_effect_gate_failure());
				}
				if (ephemeral_)
				{
					if (has_anchor_ || current->main_handle_open || !current->open_events.empty() ||
						current->shared_memory_object_identity ||
						current->shared_memory_entry_identity || !current->held_shm_locks.empty())
					{
						return unexpected(sqlite_effect_gate_failure());
					}
				}
				else
				{
					if (!has_anchor_ || !current->main_handle_open)
					{
						return unexpected(sqlite_effect_gate_failure());
					}
					const auto* main_open = observed_main_open(*current);
					if (coordination_)
					{
						const auto* wal_open = observed_unique_open(
							*current, sqlite_backend_file_role::write_ahead_log);
						if (target_stage_ != sqlite_backend_effect_stage::fully_armed ||
							coordination_->prior_receipt.stage !=
								sqlite_backend_effect_stage::wal_shm_coordination_only ||
							coordination_->prior_receipt.capability_token != capability_token_ ||
							coordination_->prior_receipt.connection_token != connection_token_ ||
							coordination_->prior_receipt.canonical_vfs_locator != path_ ||
							wal_open == nullptr ||
							!open_observations_equal(*wal_open, coordination_->wal_open) ||
							current->shared_memory_object_identity !=
								coordination_->shared_memory_object_identity ||
							current->shared_memory_entry_identity !=
								coordination_->shared_memory_entry_identity ||
							!current->held_shm_locks.empty())
						{
							return unexpected(sqlite_effect_gate_failure());
						}
					}
					const auto main_anchor =
						std::ranges::find(anchor_entries_,
										  sqlite_backend_file_role::main_database,
										  &anchor_entry::role);
					auto held_main = main_held_object_.lock();
					if (main_open == nullptr || main_anchor == anchor_entries_.end() ||
						main_anchor->state != sqlite_backend_entry_state::held_regular ||
						!held_main || !main_open->object_identity ||
						!main_open->directory_entry_identity ||
						main_open->object_identity != main_anchor->object_identity ||
						main_open->directory_entry_identity !=
							main_anchor->directory_entry_identity)
					{
						return unexpected(sqlite_effect_gate_failure());
					}
					auto replacement = held_main->recheck_current_entry();
					auto byte_count = held_main->size();
					auto sha256 = held_main->sha256();
					if (!replacement ||
						*replacement !=
							sqlite_backend_replacement_state::exact_same_entry_and_object)
					{
						return unexpected(sqlite_effect_gate_failure());
					}
					if (!byte_count || *byte_count != anchor_byte_count_)
					{
						return unexpected(sqlite_effect_gate_failure());
					}
					if (!sha256 || *sha256 != anchor_sha256_)
					{
						return unexpected(sqlite_effect_gate_failure());
					}
					if (requires_wal_anchor_)
					{
						const auto wal_anchor =
							std::ranges::find(anchor_entries_,
											  sqlite_backend_file_role::write_ahead_log,
											  &anchor_entry::role);
						auto held_wal = wal_held_object_.lock();
						if (wal_anchor == anchor_entries_.end() ||
							wal_anchor->state != sqlite_backend_entry_state::held_regular ||
							!held_wal)
						{
							return unexpected(sqlite_effect_gate_failure());
						}
						auto wal_replacement = held_wal->recheck_current_entry();
						auto wal_byte_count = held_wal->size();
						auto wal_sha256 = held_wal->sha256();
						if (!wal_replacement ||
							*wal_replacement !=
								sqlite_backend_replacement_state::exact_same_entry_and_object ||
							!wal_byte_count || *wal_byte_count != wal_anchor_byte_count_ ||
							!wal_sha256 || *wal_sha256 != wal_anchor_sha256_)
						{
							return unexpected(sqlite_effect_gate_failure());
						}
					}
					if (coordination_)
					{
						for (const auto& expected : coordination_->entries)
						{
							auto stat_only = observation_->observe_entry_state_without_open(
								path_, expected.role);
							if (!stat_only || !stat_observations_equal(*stat_only, expected))
							{
								return unexpected(sqlite_effect_gate_failure());
							}
						}
					}
					else
					{
						for (const auto& expected : anchor_entries_)
						{
							auto stat_only = observation_->observe_entry_state_without_open(
								path_, expected.role);
							if (!stat_only || stat_only->role != expected.role ||
								stat_only->state != expected.state ||
								stat_only->parent_namespace_identity !=
									parent_namespace_identity_ ||
								stat_only->object_identity != expected.object_identity ||
								stat_only->directory_entry_identity !=
									expected.directory_entry_identity)
							{
								return unexpected(sqlite_effect_gate_failure());
							}
						}
					}
				}
				try
				{
					sqlite_backend_opaque_identity validation{
						"cxxlens.sqlite-effect-gate-validation.v1", {}};
					validation.bytes.reserve(prerequisite_.bytes.size() + 128U);
					append_effect_identity(validation.bytes, prerequisite_);
					append_effect_identity(validation.bytes, capability_token_);
					append_effect_identity(validation.bytes, connection_token_);
					append_effect_bytes(validation.bytes, path_);
					validation.bytes.push_back(std::byte{static_cast<std::uint8_t>(target_stage_)});
					return validation;
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(sqlite_effect_gate_failure());
				}
				catch (const std::length_error&)
				{
					return unexpected(sqlite_effect_gate_failure());
				}
			}

		  private:
			std::shared_ptr<sqlite_backend_observation_capability> observation_;
			std::string path_;
			sqlite_backend_opaque_identity capability_token_;
			sqlite_backend_opaque_identity connection_token_;
			sqlite_backend_opaque_identity prerequisite_;
			sqlite_backend_effect_stage target_stage_{sqlite_backend_effect_stage::denied};
			std::array<anchor_entry, 4U> anchor_entries_;
			sqlite_backend_opaque_identity parent_namespace_identity_;
			std::weak_ptr<const sqlite_backend_held_object> main_held_object_;
			std::weak_ptr<const sqlite_backend_held_object> wal_held_object_;
			std::uint64_t anchor_byte_count_{};
			std::string anchor_sha256_;
			std::uint64_t wal_anchor_byte_count_{};
			std::string wal_anchor_sha256_;
			std::optional<sqlite_wal_coordination_evidence> coordination_;
			bool has_anchor_{};
			bool requires_wal_anchor_{};
			bool ephemeral_{};
		};

		[[nodiscard]] result<sqlite_backend_effect_arm_request> make_effect_arm_request(
			const sqlite_effect_connection_scope& connection,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const std::string_view path,
			const std::string_view prerequisite_profile,
			const sqlite_backend_effect_stage target_stage,
			const sqlite_quiescent_source_anchor* anchor,
			const bool preinit_absent,
			const bool ephemeral,
			const sqlite_wal_source_capture* wal_capture = nullptr)
		{
			if (!connection.observation || connection.gate == nullptr || !observation)
				return unexpected(sqlite_effect_gate_failure());
			auto current = connection.observation->snapshot();
			if (!current || !current->complete ||
				current->capability_token != observation->capability_token() ||
				current->connection_token != connection.observation->token())
				return unexpected(sqlite_effect_gate_failure());
			const auto* main_open = ephemeral ? nullptr : observed_main_open(*current);
			if ((!ephemeral && main_open == nullptr) ||
				(main_open != nullptr &&
				 (!main_open->returned_flags || !main_open->object_identity ||
				  !main_open->directory_entry_identity)))
				return unexpected(sqlite_effect_gate_failure());
			auto prerequisite = make_effect_prerequisite(prerequisite_profile,
														 path,
														 observation->capability_token(),
														 connection.observation->token(),
														 anchor,
														 main_open,
														 preinit_absent,
														 wal_capture);
			if (!prerequisite)
				return unexpected(std::move(prerequisite.error()));
			auto authority =
				std::make_shared<sqlite_store_effect_arm_authority>(observation,
																	std::string{path},
																	observation->capability_token(),
																	connection.observation->token(),
																	*prerequisite,
																	target_stage,
																	anchor,
																	ephemeral,
																	nullptr,
																	wal_capture);
			return sqlite_backend_effect_arm_request{target_stage,
													 observation->capability_token(),
													 connection.observation->token(),
													 std::string{path},
													 std::move(*prerequisite),
													 std::move(authority)};
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_request>
		make_post_wal_coordination_arm_request(
			const sqlite_effect_connection_scope& connection,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const std::string_view path,
			const std::string_view prerequisite_profile,
			const sqlite_quiescent_source_anchor& anchor,
			const sqlite_backend_effect_arm_receipt& coordination_receipt,
			const sqlite_wal_source_capture* wal_capture = nullptr)
		{
			if (!observation)
				return unexpected(sqlite_effect_gate_failure());
			auto evidence = capture_wal_coordination_evidence(
				connection, *observation, path, anchor, coordination_receipt);
			if (!evidence)
				return unexpected(std::move(evidence.error()));
			auto prerequisite = make_wal_coordination_prerequisite(prerequisite_profile,
																   path,
																   observation->capability_token(),
																   connection.observation->token(),
																   *evidence,
																   wal_capture);
			if (!prerequisite)
				return unexpected(std::move(prerequisite.error()));
			auto authority = std::make_shared<sqlite_store_effect_arm_authority>(
				observation,
				std::string{path},
				observation->capability_token(),
				connection.observation->token(),
				*prerequisite,
				sqlite_backend_effect_stage::fully_armed,
				&anchor,
				false,
				&*evidence,
				wal_capture);
			return sqlite_backend_effect_arm_request{
				sqlite_backend_effect_stage::fully_armed,
				observation->capability_token(),
				connection.observation->token(),
				std::string{path},
				std::move(*prerequisite),
				std::move(authority),
			};
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_receipt>
		arm_effect_gate_now(const sqlite_effect_connection_scope& connection,
							sqlite_backend_effect_arm_request request)
		{
			const auto prerequisite = request.prerequisite_receipt;
			const auto target_stage = request.target_stage;
			auto prior = connection.gate->latest_receipt();
			if (!prior || prior->capability_token != connection.denied_receipt.capability_token ||
				prior->connection_token != connection.denied_receipt.connection_token ||
				prior->canonical_vfs_locator != connection.denied_receipt.canonical_vfs_locator ||
				prior->sequence == std::numeric_limits<std::uint64_t>::max() ||
				static_cast<std::uint8_t>(prior->stage) >=
					static_cast<std::uint8_t>(target_stage) ||
				connection.gate->stage() != prior->stage || !connection.gate->enforcement_active())
				return unexpected(sqlite_effect_gate_failure());
			auto armed = connection.gate->arm_now(std::move(request));
			if (!armed ||
				!effect_receipt_matches(*armed,
										connection.denied_receipt.capability_token,
										connection.denied_receipt.connection_token,
										connection.denied_receipt.canonical_vfs_locator,
										prerequisite,
										target_stage,
										false) ||
				armed->sequence != prior->sequence + 1U ||
				connection.gate->stage() != target_stage || !connection.gate->enforcement_active())
				return unexpected(armed ? sqlite_effect_gate_failure() : std::move(armed.error()));
			auto latest = connection.gate->latest_receipt();
			if (!latest || !effect_receipts_equal(*latest, *armed))
				return unexpected(sqlite_effect_gate_failure());
			return armed;
		}

		[[nodiscard]] result<void>
		install_effect_gate_on_exclusive_lock(const sqlite_effect_connection_scope& connection,
											  sqlite_backend_effect_arm_request request)
		{
			return connection.gate->install_arm_on_exclusive_lock(std::move(request));
		}

		[[nodiscard]] result<sqlite_backend_effect_arm_receipt>
		validate_exclusive_effect_arm(const sqlite_effect_connection_scope& connection,
									  const sqlite_backend_opaque_identity& prerequisite,
									  const sqlite_backend_effect_stage target_stage)
		{
			auto latest = connection.gate->latest_receipt();
			if (!latest ||
				!effect_receipt_matches(*latest,
										connection.denied_receipt.capability_token,
										connection.denied_receipt.connection_token,
										connection.denied_receipt.canonical_vfs_locator,
										prerequisite,
										target_stage,
										true) ||
				latest->sequence != connection.denied_receipt.sequence + 1U ||
				connection.gate->stage() != target_stage || !connection.gate->enforcement_active())
				return unexpected(sqlite_effect_gate_failure());
			return *latest;
		}

		[[nodiscard]] result<void>
		recheck_quiescent_source(const sqlite_quiescent_source_anchor& anchor,
								 const std::string_view path,
								 const sqlite_backend_observation_capability& observation)
		{
			const auto* main =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::main_database);
			if (main == nullptr || main->held_object == nullptr)
				return unexpected(sqlite_quiescent_observation_failure());
			auto replacement = main->held_object->recheck_current_entry();
			if (!replacement)
				return unexpected(sqlite_quiescent_observation_failure());
			if (*replacement != sqlite_backend_replacement_state::exact_same_entry_and_object)
				return unexpected(sqlite_source_changed());
			auto namespace_matches = observation.recheck_namespace(anchor.namespace_census, path);
			if (!namespace_matches)
				return unexpected(sqlite_quiescent_observation_failure());
			if (!*namespace_matches)
				return unexpected(sqlite_source_changed());
			auto byte_count = main->held_object->size();
			auto sha256 = main->held_object->sha256();
			if (!byte_count || !sha256)
				return unexpected(sqlite_quiescent_observation_failure());
			if (*byte_count != anchor.byte_count || *sha256 != anchor.sha256)
				return unexpected(sqlite_source_changed());
			return {};
		}

		[[nodiscard]] bool same_namespace_receipt(const sqlite_backend_namespace_census& left,
												  const sqlite_backend_namespace_census& right)
		{
			if (left.profile != right.profile || left.capability_token != right.capability_token ||
				left.parent_namespace_identity != right.parent_namespace_identity)
				return false;
			constexpr std::array roles{
				sqlite_backend_file_role::main_database,
				sqlite_backend_file_role::write_ahead_log,
				sqlite_backend_file_role::shared_memory,
				sqlite_backend_file_role::rollback_journal,
			};
			for (const auto role : roles)
			{
				const auto* before = observed_entry(left, role);
				const auto* after = observed_entry(right, role);
				if (before == nullptr || after == nullptr || before->state != after->state ||
					before->object_identity != after->object_identity ||
					before->directory_entry_identity != after->directory_entry_identity)
					return false;
			}
			return true;
		}

		[[nodiscard]] bool same_main_receipt(const sqlite_backend_namespace_census& left,
											 const sqlite_backend_namespace_census& right)
		{
			if (left.profile != right.profile || left.capability_token != right.capability_token ||
				left.parent_namespace_identity != right.parent_namespace_identity)
				return false;
			const auto* before = observed_entry(left, sqlite_backend_file_role::main_database);
			const auto* after = observed_entry(right, sqlite_backend_file_role::main_database);
			return before != nullptr && after != nullptr &&
				before->state == sqlite_backend_entry_state::held_regular &&
				after->state == sqlite_backend_entry_state::held_regular &&
				before->object_identity == after->object_identity &&
				before->directory_entry_identity == after->directory_entry_identity;
		}

		[[nodiscard]] result<void>
		revalidate_stored_quiescent_source(const sqlite_quiescent_source_anchor& anchor,
										   const std::string_view path,
										   const sqlite_backend_observation_capability& observation)
		{
			auto current = observation.capture_namespace(path);
			if (!current)
				return unexpected(sqlite_quiescent_observation_failure());
			if (auto valid = validate_namespace_census(*current, observation); !valid)
				return unexpected(sqlite_quiescent_observation_failure());
			if (!same_namespace_receipt(anchor.namespace_census, *current))
				return unexpected(sqlite_source_changed());
			const auto* main = observed_entry(*current, sqlite_backend_file_role::main_database);
			if (main == nullptr || main->state != sqlite_backend_entry_state::held_regular ||
				main->held_object == nullptr)
				return unexpected(sqlite_quiescent_observation_failure());
			auto byte_count = main->held_object->size();
			auto sha256 = main->held_object->sha256();
			if (!byte_count || !sha256)
				return unexpected(sqlite_quiescent_observation_failure());
			if (*byte_count != anchor.byte_count || *sha256 != anchor.sha256)
				return unexpected(sqlite_source_changed());
			std::array<std::byte, 20U> header{};
			if (*byte_count < header.size())
				return unexpected(sqlite_source_changed());
			if (auto read = main->held_object->read_exact(0U, header); !read)
				return unexpected(sqlite_quiescent_observation_failure());
			if (header[18U] != std::byte{2U} || header[19U] != std::byte{2U})
				return unexpected(
					store_error("store.sqlite-failure", "sqlite-journal-mode", "expected-wal"));
			return recheck_quiescent_source(sqlite_quiescent_source_anchor{std::move(*current),
																		   *byte_count,
																		   std::move(*sha256)},
											path,
											observation);
		}

		[[nodiscard]] result<void>
		finish_private_read(observed_opened_sqlite_database& opened,
							const std::string_view path,
							const sqlite_backend_observation_capability& observation,
							const bool commit)
		{
			if (!opened.private_read_transaction || opened.database == nullptr ||
				!opened.source_anchor)
				return unexpected(
					store_error("store.corrupt", "sqlite-open-snapshot", "private-read-state"));

			std::optional<error> pre_end_failure;
			const bool active_wal = opened.active_wal_anchor.has_value();
			const bool wal_only = opened.wal_only_capture.has_value();
			std::shared_ptr<sqlite_source_shm_target_namespace_epoch> target_namespace_epoch;
			if (active_wal && wal_only)
				return unexpected(
					store_error("store.corrupt", "sqlite-open-snapshot", "private-read-state"));
			if (active_wal)
			{
				target_namespace_epoch = opened.active_wal_anchor->target_namespace_epoch;
				if (!target_namespace_epoch ||
					target_namespace_epoch->identity() !=
						opened.active_wal_anchor->target_namespace_epoch_identity)
				{
					pre_end_failure = sqlite_source_shm_qualification_failure();
				}
				else if (!opened.active_observation || !opened.active_denied_receipt)
				{
					pre_end_failure = sqlite_active_wal_observation_failure();
				}
				else
				{
					auto* gate = opened.active_observation->effect_gate_port();
					if (gate == nullptr)
					{
						pre_end_failure = sqlite_active_wal_observation_failure();
					}
					else
					{
						sqlite_effect_connection_scope connection{
							opened.active_observation, gate, *opened.active_denied_receipt};
						auto stable = recheck_active_wal_read(connection,
															  observation,
														  path,
														  *opened.source_anchor,
														  *opened.active_wal_anchor,
														  opened.private_read_transaction);
						if (!stable)
							pre_end_failure = std::move(stable.error());
					}
				}
			}
			else if (opened.active_observation || opened.active_denied_receipt)
			{
				return unexpected(
					store_error("store.corrupt", "sqlite-open-snapshot", "private-read-state"));
			}
			if (wal_only)
			{
				auto stable = recheck_wal_only_source(
					*opened.source_anchor, *opened.wal_only_capture, path, observation);
				if (!stable)
					pre_end_failure = std::move(stable.error());
			}

			auto ended =
				opened.database->execute(commit && !pre_end_failure ? "COMMIT;" : "ROLLBACK;");
			opened.private_read_transaction = false;
			auto closed = opened.database->close_exactly_once();
			opened.database.reset();
			if (!confirmed_connection_close(closed))
				return unexpected(store_error(
					"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"));
			result<void> epoch_finished;
			if (active_wal)
			{
				if (!target_namespace_epoch)
					epoch_finished = unexpected(sqlite_source_shm_qualification_failure());
				else if (auto finished = finish_source_shm_epoch_after_confirmed_close(
							 target_namespace_epoch, true);
						 !finished)
					epoch_finished = unexpected(std::move(finished.error()));
			}
			opened.private_snapshot.reset();
			opened.active_observation.reset();
			opened.active_denied_receipt.reset();
			opened.active_wal_anchor.reset();
			target_namespace_epoch.reset();
			if (!epoch_finished)
				return unexpected(std::move(epoch_finished.error()));
			if (pre_end_failure)
				return unexpected(std::move(*pre_end_failure));
			if (!ended && (commit || active_wal || wal_only))
				return unexpected(std::move(ended.error()));
			if (active_wal)
				return {};
			if (wal_only)
			{
				if (auto verified = validate_closed_wal_recovery_workspace(*opened.wal_only_capture,
																		   observation);
					!verified)
					return unexpected(std::move(verified.error()));
				return recheck_wal_only_source(
					*opened.source_anchor, *opened.wal_only_capture, path, observation);
			}
			return recheck_quiescent_source(*opened.source_anchor, path, observation);
		}

		[[nodiscard]] result<void> qualify_v3_with_ephemeral_scratch(
			const std::shared_ptr<sqlite_api>& api,
			const std::string& vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation)
		{
			if (!api || !observation)
				return unexpected(sqlite_effect_gate_failure());
			if (auto symbols = require_v3_symbols(*api); !symbols)
				return symbols;
			auto effect = begin_ephemeral_effect_gated_connection(observation);
			if (!effect)
				return unexpected(std::move(effect.error()));
			auto scratch = open_database(api,
										 ":memory:",
										 vfs_name.c_str(),
										 sqlite_open_readwrite | sqlite_open_create |
											 sqlite_open_privatecache | sqlite_open_fullmutex,
										 {api, backend_lifetime, observation, {}});
			if (!scratch)
				return unexpected(std::move(scratch.error()));
			if (auto valid =
					validate_ephemeral_open_observation(*effect->observation, *observation);
				!valid)
				return valid;
			if (auto valid = validate_v3_connection(**scratch); !valid)
				return valid;
			const auto closed = (*scratch)->close_exactly_once();
			scratch->reset();
			if (!confirmed_connection_close(closed))
				return unexpected(store_error(
					"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"));
			auto latest = effect->gate->latest_receipt();
			if (!latest || !effect_receipts_equal(*latest, effect->denied_receipt) ||
				effect->gate->stage() != sqlite_backend_effect_stage::denied ||
				!effect->gate->enforcement_active())
				return unexpected(sqlite_effect_gate_failure());
			return {};
		}

		[[nodiscard]] result<observed_opened_sqlite_database> open_main_only_private_snapshot(
			const std::shared_ptr<sqlite_api>& api,
			const std::string_view path,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			sqlite_quiescent_source_anchor anchor)
		{
			const auto* main =
				observed_entry(anchor.namespace_census, sqlite_backend_file_role::main_database);
			if (!api || !observation || main == nullptr ||
				main->state != sqlite_backend_entry_state::held_regular || !main->held_object)
				return unexpected(sqlite_quiescent_observation_failure());
			auto builder = observation->create_private_snapshot();
			if (!builder)
				return unexpected(sqlite_quiescent_observation_failure());
			std::array<std::byte, 64U * 1024U> scratch{};
			auto private_snapshot = main->held_object->copy_exact(**builder, scratch);
			if (!private_snapshot)
				return unexpected(sqlite_quiescent_observation_failure());
			if ((*private_snapshot)->source_capability_token() != observation->capability_token() ||
				(*private_snapshot)->receipt().byte_count != anchor.byte_count ||
				(*private_snapshot)->receipt().sha256 != anchor.sha256 ||
				(*private_snapshot)->application_generated_uri().empty() ||
				(*private_snapshot)->registered_vfs_name().empty() ||
				(*private_snapshot)->vfs_implementation_identity() == nullptr)
				return unexpected(sqlite_quiescent_observation_failure());
			const std::string private_uri{(*private_snapshot)->application_generated_uri()};
			const std::string private_vfs_name{(*private_snapshot)->registered_vfs_name()};
			constexpr auto private_full = sqlite_open_privatecache | sqlite_open_fullmutex;
			auto database = open_database(api,
										  private_uri,
										  private_vfs_name.c_str(),
										  sqlite_open_readonly | sqlite_open_uri | private_full,
										  {api, *private_snapshot, observation, {}});
			if (!database)
				return unexpected(std::move(database.error()));
			if (auto begun = (*database)->execute("BEGIN;"); !begun)
				return unexpected(std::move(begun.error()));

			observed_opened_sqlite_database output;
			output.api = api;
			output.private_snapshot = std::move(*private_snapshot);
			output.database = std::move(*database);
			output.source_anchor = std::move(anchor);
			output.private_read_transaction = true;
			auto classification = classify_sqlite_database(*output.database);
			if (!classification)
			{
				auto failure = std::move(classification.error());
				if (auto stable = finish_private_read(output, path, *observation, false); !stable)
					return unexpected(std::move(stable.error()));
				return unexpected(std::move(failure));
			}
			if (!*classification)
			{
				if (auto stable = finish_private_read(output, path, *observation, true); !stable)
					return unexpected(std::move(stable.error()));
				return unexpected(sqlite_wal_only_unrecognized());
			}
			output.format = **classification;
			return output;
		}

		[[nodiscard]] result<std::unique_ptr<sqlite_database>> open_fresh_observed_database(
			const std::shared_ptr<sqlite_api>& api,
			const std::string& path,
			const std::string& vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const sqlite_quiescent_source_anchor& source_anchor,
			const bool preinit_absent)
		{
			if (!observation)
				return unexpected(sqlite_quiescent_observation_failure());
			auto anchor_pin = sqlite_authority_anchor_pin(source_anchor);
			if (!anchor_pin)
				return unexpected(sqlite_effect_gate_failure());
			auto effect = begin_effect_gated_connection(observation, path);
			if (!effect)
				return unexpected(std::move(effect.error()));
			auto database = open_database(
				api,
				path,
				vfs_name.c_str(),
				sqlite_open_readwrite | sqlite_open_privatecache | sqlite_open_fullmutex,
				{api, backend_lifetime, observation, std::move(anchor_pin)});
			if (!database)
				return unexpected(std::move(database.error()));
			if (auto valid =
					validate_read_write_open_observation(*effect->observation, *observation);
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = validate_v3_connection(**database); !valid)
				return unexpected(std::move(valid.error()));
			if (auto synchronous = set_and_require_full_synchronous(**database, true); !synchronous)
				return unexpected(std::move(synchronous.error()));
			auto classification = classify_sqlite_database(**database);
			if (!classification)
				return unexpected(std::move(classification.error()));
			if (*classification)
				return unexpected(sqlite_source_changed());
			auto request =
				make_effect_arm_request(*effect,
										observation,
										path,
										"cxxlens.sqlite-effect-prerequisite.fresh-init.v1",
										sqlite_backend_effect_stage::fully_armed,
										&source_anchor,
										preinit_absent,
										false);
			if (!request)
				return unexpected(std::move(request.error()));
			const auto prerequisite = request->prerequisite_receipt;
			if (auto installed =
					install_effect_gate_on_exclusive_lock(*effect, std::move(*request));
				!installed)
				return unexpected(std::move(installed.error()));
			if (auto journal = (*database)->execute("PRAGMA journal_mode=WAL;"); !journal)
				return unexpected(std::move(journal.error()));
			if (auto armed = validate_exclusive_effect_arm(
					*effect, prerequisite, sqlite_backend_effect_stage::fully_armed);
				!armed)
				return unexpected(std::move(armed.error()));
			if (auto wal = require_wal_mode(**database, true); !wal)
				return unexpected(std::move(wal.error()));
			if (auto initialized = initialize_v3_authority(**database); !initialized)
				return unexpected(std::move(initialized.error()));
			return database;
		}

		[[nodiscard]] result<std::unique_ptr<sqlite_database>> open_current_observed_database(
			const std::shared_ptr<sqlite_api>& api,
			const std::string& path,
			const std::string& vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation,
			const sqlite_quiescent_source_anchor& source_anchor)
		{
			if (!observation)
				return unexpected(sqlite_quiescent_observation_failure());
			auto anchor_pin = sqlite_authority_anchor_pin(source_anchor);
			if (!anchor_pin)
				return unexpected(sqlite_effect_gate_failure());
			auto effect = begin_effect_gated_connection(observation, path);
			if (!effect)
				return unexpected(std::move(effect.error()));
			auto database = open_database(
				api,
				path,
				vfs_name.c_str(),
				sqlite_open_readwrite | sqlite_open_privatecache | sqlite_open_fullmutex,
				{api, backend_lifetime, observation, std::move(anchor_pin)});
			if (!database)
				return unexpected(std::move(database.error()));
			if (auto valid =
					validate_read_write_open_observation(*effect->observation, *observation);
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = validate_v3_connection(**database); !valid)
				return unexpected(std::move(valid.error()));
			auto coordination_request = make_effect_arm_request(
				*effect,
				observation,
				path,
				"cxxlens.sqlite-effect-prerequisite.current-v3.wal-coordination.v1",
				sqlite_backend_effect_stage::wal_shm_coordination_only,
				&source_anchor,
				false,
				false);
			if (!coordination_request)
				return unexpected(std::move(coordination_request.error()));
			auto coordination = arm_effect_gate_now(*effect, std::move(*coordination_request));
			if (!coordination)
				return unexpected(std::move(coordination.error()));
			if (auto synchronous = set_and_require_full_synchronous(**database, true); !synchronous)
				return unexpected(std::move(synchronous.error()));
			auto request = make_post_wal_coordination_arm_request(
				*effect,
				observation,
				path,
				"cxxlens.sqlite-effect-prerequisite.current-v3.post-wal-coordination.v1",
				source_anchor,
				*coordination);
			if (!request)
				return unexpected(std::move(request.error()));
			if (auto armed = arm_effect_gate_now(*effect, std::move(*request)); !armed)
				return unexpected(std::move(armed.error()));
			if (auto wal = require_wal_mode(**database, true); !wal)
				return unexpected(std::move(wal.error()));
			return database;
		}

		[[nodiscard]] result<observed_opened_sqlite_database> open_observed_store_database(
			std::shared_ptr<sqlite_api> api,
			const std::string& path,
			const std::string& vfs_name,
			const std::shared_ptr<void>& backend_lifetime,
			const std::shared_ptr<sqlite_backend_observation_capability>& observation)
		{
			if (!api)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			if (auto bound =
					validate_observation_binding(*api, vfs_name, backend_lifetime, observation);
				!bound)
				return unexpected(std::move(bound.error()));
			const auto private_full = sqlite_open_privatecache | sqlite_open_fullmutex;
			if (path == ":memory:")
			{
				auto effect = begin_effect_gated_connection(observation, path);
				if (!effect)
					return unexpected(std::move(effect.error()));
				auto database =
					open_database(api,
								  path,
								  vfs_name.c_str(),
								  sqlite_open_readwrite | sqlite_open_create | private_full,
								  {api, backend_lifetime, observation, {}});
				if (!database)
					return unexpected(std::move(database.error()));
				if (auto valid =
						validate_ephemeral_open_observation(*effect->observation, *observation);
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = validate_v3_connection(**database); !valid)
					return unexpected(std::move(valid.error()));
				if (auto synchronous = set_and_require_full_synchronous(**database, true);
					!synchronous)
					return unexpected(std::move(synchronous.error()));
				if (auto journal = set_and_require_memory_journal(**database, true); !journal)
					return unexpected(std::move(journal.error()));
				auto request = make_effect_arm_request(
					*effect,
					observation,
					path,
					"cxxlens.sqlite-effect-prerequisite.ephemeral-not-applicable.v1",
					sqlite_backend_effect_stage::fully_armed,
					nullptr,
					false,
					true);
				if (!request)
					return unexpected(std::move(request.error()));
				if (auto armed = arm_effect_gate_now(*effect, std::move(*request)); !armed)
					return unexpected(std::move(armed.error()));
				if (auto initialized = initialize_v3_authority(**database); !initialized)
					return unexpected(std::move(initialized.error()));
				observed_opened_sqlite_database output;
				output.api = api;
				output.database = std::move(*database);
				return output;
			}

			auto captured = observation->capture_namespace(path);
			if (!captured)
				return unexpected(sqlite_namespace_observation_failure());
			if (auto valid = validate_namespace_census(*captured, *observation); !valid)
				return unexpected(std::move(valid.error()));
			const auto* main = observed_entry(*captured, sqlite_backend_file_role::main_database);
			const auto* wal = observed_entry(*captured, sqlite_backend_file_role::write_ahead_log);
			const auto* shm = observed_entry(*captured, sqlite_backend_file_role::shared_memory);
			const auto* journal =
				observed_entry(*captured, sqlite_backend_file_role::rollback_journal);
			if (main == nullptr || wal == nullptr || shm == nullptr || journal == nullptr)
				return unexpected(sqlite_namespace_observation_failure());
			const auto present = [](const sqlite_backend_entry_observation& entry)
			{
				return entry.state != sqlite_backend_entry_state::absent;
			};
			const auto main_present = present(*main);
			const auto wal_present = present(*wal);
			const auto shm_present = present(*shm);
			const auto journal_present = present(*journal);
			if (!main_present)
			{
				if (wal_present || shm_present || journal_present)
					return unexpected(store_error("store.sqlite-failure",
												  "sqlite-initialization-sidecar",
												  "orphan-without-main"));
				if (auto symbols = require_v3_symbols(*api); !symbols)
					return unexpected(std::move(symbols.error()));
				if (auto scratch_qualification = qualify_v3_with_ephemeral_scratch(
						api, vfs_name, backend_lifetime, observation);
					!scratch_qualification)
					return unexpected(std::move(scratch_qualification.error()));
				auto receipt = observation->exclusive_create_sync_zero_main(path);
				if (!receipt)
					return unexpected(std::move(receipt.error()));
				const auto empty_sha256 = content_digest(std::span<const std::byte>{});
				if (receipt->capability_token != observation->capability_token() ||
					receipt->parent_namespace_identity != captured->parent_namespace_identity ||
					receipt->held_main == nullptr ||
					receipt->held_main->role() != sqlite_backend_file_role::main_database ||
					receipt->held_main->object_identity() != receipt->object_identity ||
					receipt->held_main->directory_entry_identity() !=
						receipt->directory_entry_identity ||
					receipt->byte_count != 0U || receipt->sha256 != empty_sha256 ||
					!receipt->object_full_sync || !receipt->parent_namespace_sync)
					return unexpected(sqlite_quiescent_observation_failure());
				auto fresh_census = std::move(*captured);
				bool installed_main{};
				for (auto& entry : fresh_census.entries)
				{
					if (entry.role != sqlite_backend_file_role::main_database)
						continue;
					if (installed_main)
						return unexpected(sqlite_quiescent_observation_failure());
					entry = {sqlite_backend_file_role::main_database,
							 sqlite_backend_entry_state::held_regular,
							 receipt->object_identity,
							 receipt->directory_entry_identity,
							 receipt->held_main,
							 receipt->held_main->object_filesystem_profile()};
					installed_main = true;
				}
				if (!installed_main)
					return unexpected(sqlite_quiescent_observation_failure());
				sqlite_quiescent_source_anchor fresh_anchor{
					std::move(fresh_census), receipt->byte_count, receipt->sha256};
				auto database = open_fresh_observed_database(
					api, path, vfs_name, backend_lifetime, observation, fresh_anchor, true);
				if (!database)
					return unexpected(std::move(database.error()));
				observed_opened_sqlite_database output;
				output.api = api;
				output.database = std::move(*database);
				output.source_anchor = std::move(fresh_anchor);
				return output;
			}

			if (journal_present)
				return unexpected(
					store_error("store.sqlite-failure", "sqlite-sidecar-state", "journal-present"));
			if (!wal_present && shm_present)
				return unexpected(store_error(
					"store.sqlite-failure", "sqlite-sidecar-state", "incomplete-wal-shm-pair"));
			for (const auto* entry : {main, wal, shm, journal})
				if (entry->state == sqlite_backend_entry_state::unsupported_kind)
					return unexpected(store_error(
						"store.sqlite-failure", "sqlite-object-kind", "not-regular-or-equivalent"));
			if (wal_present && wal->state != sqlite_backend_entry_state::held_regular)
				return unexpected(
					store_error("store.sqlite-failure",
								"sqlite-initialization-sidecar",
								shm_present ? "unreadable-wal-shm-pair" : "unreadable-wal-only"));
			if (shm_present && shm->state != sqlite_backend_entry_state::held_regular)
				return unexpected(store_error(
					"store.sqlite-failure", "sqlite-sidecar-state", "unreadable-wal-shm-pair"));
			if (main->state != sqlite_backend_entry_state::held_regular ||
				main->held_object == nullptr)
				return unexpected(sqlite_namespace_observation_failure());

			std::optional<sqlite_source_shm_qualified_open_plan> qualified_active_wal;
			if (wal_present && shm_present)
			{
				auto prequalification =
					observe_active_wal_prequalification_header(*captured, path);
				if (!prequalification)
					return unexpected(std::move(prequalification.error()));
				if (prequalification->bytes[18U] != std::byte{2U} ||
					prequalification->bytes[19U] != std::byte{2U})
					return unexpected(store_error(
						"store.sqlite-failure", "sqlite-journal-mode", "expected-wal"));
				auto qualified = qualify_active_wal_source_shm(
					api, path, vfs_name, backend_lifetime, observation, *captured);
				if (!qualified)
					return unexpected(std::move(qualified.error()));
				qualified_active_wal.emplace(std::move(*qualified));
			}

			auto byte_count = main->held_object->size();
			if (!byte_count)
				return unexpected(sqlite_quiescent_observation_failure());
			auto sha256 = main->held_object->sha256();
			if (!sha256)
				return unexpected(sqlite_quiescent_observation_failure());
			sqlite_quiescent_source_anchor anchor{
				std::move(*captured), *byte_count, std::move(*sha256)};
			main = observed_entry(anchor.namespace_census, sqlite_backend_file_role::main_database);
			if (main == nullptr || main->state != sqlite_backend_entry_state::held_regular ||
				main->held_object == nullptr)
				return unexpected(sqlite_namespace_observation_failure());
			if (wal_present && !shm_present)
			{
				wal = observed_entry(anchor.namespace_census,
									 sqlite_backend_file_role::write_ahead_log);
				if (wal == nullptr || wal->state != sqlite_backend_entry_state::held_regular ||
					!wal->held_object)
					return unexpected(store_error("store.sqlite-failure",
												  "sqlite-initialization-sidecar",
												  "unreadable-wal-only"));
				auto builder = observation->create_wal_recovery_workspace();
				if (!builder || !*builder)
					return unexpected(builder ? sqlite_wal_only_unrecognized()
											  : std::move(builder.error()));
				auto capture =
					capture_sqlite_wal_source(main->held_object, wal->held_object, **builder);
				if (!capture)
					return unexpected(std::move(capture.error()));
				if (capture->full_source_main.byte_count != anchor.byte_count ||
					capture->full_source_main.sha256 != anchor.sha256 || !capture->workspace ||
					capture->workspace->database_path().empty() ||
					capture->workspace->registered_vfs_name().empty() ||
					capture->workspace->vfs_implementation_identity() == nullptr)
					return unexpected(sqlite_wal_only_unrecognized());
				const std::string recovery_path{capture->workspace->database_path()};
				const std::string recovery_vfs{capture->workspace->registered_vfs_name()};
				if (api->vfs_find(recovery_vfs.c_str()) !=
					capture->workspace->vfs_implementation_identity())
					return unexpected(sqlite_wal_only_unrecognized());
				if (auto stable = recheck_wal_only_source(anchor, *capture, path, *observation);
					!stable)
					return unexpected(std::move(stable.error()));
				if (capture->wal_scan.authoritative_prefix_byte_count == 0U)
				{
					auto main_only =
						open_main_only_private_snapshot(api, path, observation, std::move(anchor));
					if (!main_only)
						return unexpected(std::move(main_only.error()));
					main_only->wal_only_capture = std::move(*capture);
					return main_only;
				}
				auto database = open_database(api,
											  recovery_path,
											  recovery_vfs.c_str(),
											  sqlite_wal_recovery_required_open_flags,
											  {api, capture->workspace, observation, {}});
				if (!database)
					return unexpected(std::move(database.error()));
				if (auto begun = (*database)->execute("BEGIN;"); !begun)
				{
					auto failure = std::move(begun.error());
					auto closed = (*database)->close_exactly_once();
					database->reset();
					if (!confirmed_connection_close(closed))
						return unexpected(store_error(
							"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"));
					return unexpected(std::move(failure));
				}

				observed_opened_sqlite_database output;
				output.api = api;
				output.source_anchor = std::move(anchor);
				output.wal_only_capture = std::move(*capture);
				output.database = std::move(*database);
				output.private_read_transaction = true;
				auto classification = classify_sqlite_database(*output.database);
				if (!classification)
				{
					auto failure = std::move(classification.error());
					if (auto stable = finish_private_read(output, path, *observation, false);
						!stable)
						return unexpected(std::move(stable.error()));
					return unexpected(std::move(failure));
				}
				if (!*classification)
				{
					if (auto stable = finish_private_read(output, path, *observation, true);
						!stable)
						return unexpected(std::move(stable.error()));
					return unexpected(sqlite_wal_only_unrecognized());
				}
				output.format = **classification;
				if (output.format == sqlite_physical_format::current_v3)
					if (auto valid = validate_v3_connection(*output.database); !valid)
					{
						auto failure = std::move(valid.error());
						if (auto stable = finish_private_read(output, path, *observation, false);
							!stable)
							return unexpected(std::move(stable.error()));
						return unexpected(std::move(failure));
					}
				if (auto wal_mode = require_wal_mode(
						*output.database, output.format == sqlite_physical_format::current_v3);
					!wal_mode)
				{
					auto failure = std::move(wal_mode.error());
					if (auto stable = finish_private_read(output, path, *observation, false);
						!stable)
						return unexpected(std::move(stable.error()));
					return unexpected(std::move(failure));
				}
				return output;
			}
			if (*byte_count == 0U)
			{
				if (auto stable = recheck_quiescent_source(anchor, path, *observation); !stable)
					return unexpected(std::move(stable.error()));
				if (auto symbols = require_v3_symbols(*api); !symbols)
					return unexpected(std::move(symbols.error()));
				if (auto scratch_qualification = qualify_v3_with_ephemeral_scratch(
						api, vfs_name, backend_lifetime, observation);
					!scratch_qualification)
					return unexpected(std::move(scratch_qualification.error()));
				auto database = open_fresh_observed_database(
					api, path, vfs_name, backend_lifetime, observation, anchor, false);
				if (!database)
					return unexpected(std::move(database.error()));
				observed_opened_sqlite_database output;
				output.api = api;
				output.database = std::move(*database);
				output.source_anchor = std::move(anchor);
				return output;
			}

			std::array<std::byte, 20U> sqlite_header{};
			if (auto read = main->held_object->read_exact(0U, sqlite_header); !read)
				return unexpected(sqlite_quiescent_observation_failure());
			if (sqlite_header[18U] != std::byte{2U} || sqlite_header[19U] != std::byte{2U})
				return unexpected(
					store_error("store.sqlite-failure", "sqlite-journal-mode", "expected-wal"));
			if (wal_present)
			{
					if (!qualified_active_wal)
						return unexpected(sqlite_source_shm_qualification_failure());
				auto target_namespace_epoch =
					qualified_active_wal->qualification.target_namespace_epoch;
				if (!target_namespace_epoch)
					return unexpected(sqlite_source_shm_qualification_failure());
				auto source_anchor_pin = sqlite_authority_anchor_pin(anchor);
					if (!source_anchor_pin)
						return unexpected(sqlite_source_shm_qualification_failure());
				auto lifetime_pin = std::make_shared<sqlite_active_wal_lifetime_pin>(
					sqlite_active_wal_lifetime_pin{std::move(source_anchor_pin),
											 target_namespace_epoch});
				std::shared_ptr<const void> anchor_pin =
					std::static_pointer_cast<const void>(std::move(lifetime_pin));
				auto effect = begin_effect_gated_connection(observation, path);
				if (!effect)
				{
					if (auto finished = finish_source_shm_epoch_after_confirmed_close(
							target_namespace_epoch, true);
						!finished)
						return unexpected(sqlite_source_shm_qualification_failure());
					return unexpected(sqlite_source_shm_qualification_failure());
				}
				if (auto armed = effect->observation->arm_source_shm_readonly_profile(
						*qualified_active_wal);
					!armed)
				{
					if (auto finished = finish_source_shm_epoch_after_confirmed_close(
							target_namespace_epoch, true);
						!finished)
						return unexpected(sqlite_source_shm_qualification_failure());
					return unexpected(sqlite_source_shm_qualification_failure());
				}
				std::optional<sqlite_connection_close_outcome> failed_open_close;
				auto database =
					open_database(api,
								  qualified_active_wal->application_generated_uri,
								  qualified_active_wal->registered_vfs_name.c_str(),
								  qualified_active_wal->open_flags,
								  {api, backend_lifetime, observation, std::move(anchor_pin)},
								  &failed_open_close);
					if (!database)
					{
						if (!failed_open_close ||
							!confirmed_failed_open_resolution(*failed_open_close))
							return unexpected(store_error(
								"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"));
						if (auto finished = finish_source_shm_epoch_after_confirmed_close(
								target_namespace_epoch, true);
							!finished)
							return unexpected(sqlite_source_shm_qualification_failure());
						return unexpected(sqlite_source_shm_qualification_failure());
					}

				bool transaction_active{};
				const auto fail = [&](error failure) -> result<observed_opened_sqlite_database>
				{
					if (transaction_active)
						(void)(*database)->execute("ROLLBACK;");
					transaction_active = false;
					auto closed = (*database)->close_exactly_once();
					database->reset();
					if (!confirmed_connection_close(closed))
						return unexpected(store_error(
							"store.sqlite-failure", "sqlite-initialization-recovery", "opaque"));
					if (auto finished = finish_source_shm_epoch_after_confirmed_close(
							target_namespace_epoch, true);
						!finished)
						return unexpected(sqlite_source_shm_qualification_failure());
					return unexpected(std::move(failure));
				};

				if (auto begun = (*database)->execute("BEGIN;"); !begun)
					return fail(std::move(begun.error()));
				transaction_active = true;
				if (auto schema_version = (*database)->execute("PRAGMA schema_version;");
					!schema_version)
					return fail(std::move(schema_version.error()));
				auto active = observe_active_wal_read(
					*effect,
					*observation,
					path,
					anchor,
					&*qualified_active_wal,
					nullptr,
					transaction_active,
					false);
				if (!active)
					return fail(std::move(active.error()));

				observed_opened_sqlite_database output;
				output.api = api;
				output.active_observation = effect->observation;
				output.active_denied_receipt = effect->denied_receipt;
				output.source_anchor = std::move(anchor);
				output.active_wal_anchor = std::move(*active);
				output.database = std::move(*database);
				output.private_read_transaction = true;
				transaction_active = false;
				auto classification = classify_sqlite_database(*output.database);
				if (!classification)
				{
					auto failure = std::move(classification.error());
					if (auto stable = finish_private_read(output, path, *observation, false);
						!stable)
						return unexpected(std::move(stable.error()));
					return unexpected(std::move(failure));
				}
				if (!*classification)
				{
					if (auto stable = finish_private_read(output, path, *observation, true);
						!stable)
						return unexpected(std::move(stable.error()));
					return unexpected(store_error("store.sqlite-failure",
												  "sqlite-open-snapshot",
												  "active-wal-unrecognized-format"));
				}
				output.format = **classification;
				if (output.format == sqlite_physical_format::current_v3)
					if (auto valid = validate_v3_connection(*output.database); !valid)
					{
						auto failure = std::move(valid.error());
						if (auto stable = finish_private_read(
								output, path, *observation, false);
							!stable)
							return unexpected(std::move(stable.error()));
						return unexpected(sqlite_source_shm_qualification_failure());
					}
				return output;
			}

			auto builder = observation->create_private_snapshot();
			if (!builder)
				return unexpected(sqlite_quiescent_observation_failure());
			std::array<std::byte, 64U * 1024U> scratch{};
			auto private_snapshot = main->held_object->copy_exact(**builder, scratch);
			if (!private_snapshot)
				return unexpected(sqlite_quiescent_observation_failure());
			if ((*private_snapshot)->source_capability_token() != observation->capability_token() ||
				(*private_snapshot)->receipt().byte_count != anchor.byte_count ||
				(*private_snapshot)->receipt().sha256 != anchor.sha256 ||
				(*private_snapshot)->application_generated_uri().empty() ||
				(*private_snapshot)->registered_vfs_name().empty() ||
				(*private_snapshot)->vfs_implementation_identity() == nullptr)
				return unexpected(sqlite_quiescent_observation_failure());
			const std::string private_uri{(*private_snapshot)->application_generated_uri()};
			const std::string private_vfs_name{(*private_snapshot)->registered_vfs_name()};
			auto database = open_database(api,
										  private_uri,
										  private_vfs_name.c_str(),
										  sqlite_open_readonly | sqlite_open_uri | private_full,
										  {api, *private_snapshot, observation, {}});
			if (!database)
				return unexpected(std::move(database.error()));
			if (auto begun = (*database)->execute("BEGIN;"); !begun)
				return unexpected(std::move(begun.error()));

			observed_opened_sqlite_database output;
			output.api = api;
			output.private_snapshot = std::move(*private_snapshot);
			output.database = std::move(*database);
			output.source_anchor = std::move(anchor);
			output.private_read_transaction = true;
			auto classification = classify_sqlite_database(*output.database);
			if (!classification)
			{
				auto failure = std::move(classification.error());
				if (auto stable = finish_private_read(output, path, *observation, false); !stable)
					return unexpected(std::move(stable.error()));
				return unexpected(std::move(failure));
			}
			if (!*classification)
			{
				if (auto stable = finish_private_read(output, path, *observation, true); !stable)
					return unexpected(std::move(stable.error()));
				if (auto symbols = require_v3_symbols(*api); !symbols)
					return unexpected(std::move(symbols.error()));
				if (!output.source_anchor)
					return unexpected(sqlite_quiescent_observation_failure());
				if (auto scratch_qualification = qualify_v3_with_ephemeral_scratch(
						api, vfs_name, backend_lifetime, observation);
					!scratch_qualification)
					return unexpected(std::move(scratch_qualification.error()));
				auto fresh = open_fresh_observed_database(api,
														  path,
														  vfs_name,
														  backend_lifetime,
														  observation,
														  *output.source_anchor,
														  false);
				if (!fresh)
					return unexpected(std::move(fresh.error()));
				output.api = api;
				output.database = std::move(*fresh);
				return output;
			}
			output.format = **classification;
			return output;
		}

		[[nodiscard]] std::uint64_t payload_chunk_count(const std::size_t byte_count)
		{
			return byte_count == 0U
				? 0U
				: 1U + static_cast<std::uint64_t>((byte_count - 1U) / sqlite_payload_chunk_maximum);
		}

		[[nodiscard]] result<void> insert_chunk_rows(sqlite_database& database,
													 const std::string_view insert_sql,
													 const std::string_view publication_id,
													 const std::uint64_t generation,
													 const std::span<const std::byte> payload)
		{
			auto insert = sqlite_statement::prepare(database, insert_sql, true);
			if (!insert)
				return unexpected(std::move(insert.error()));
			std::uint64_t ordinal{};
			for (std::size_t offset = 0U; offset < payload.size();)
			{
				const auto count = std::min(sqlite_payload_chunk_maximum, payload.size() - offset);
				const auto chunk = payload.subspan(offset, count);
				const auto checksum = content_digest(chunk);
				if (auto value = insert->bind_text(1, publication_id); !value)
					return value;
				if (auto value = insert->bind_unsigned(2, generation); !value)
					return value;
				if (auto value = insert->bind_unsigned(3, ordinal); !value)
					return value;
				if (auto value = insert->bind_unsigned(4, offset); !value)
					return value;
				if (auto value = insert->bind_unsigned(5, count); !value)
					return value;
				if (auto value = insert->bind_text(6, checksum); !value)
					return value;
				if (auto value = insert->bind_blob(7, chunk); !value)
					return value;
				if (auto value = insert->expect_done(); !value)
					return value;
				if (auto value = insert->reset(); !value)
					return value;
				offset += count;
				++ordinal;
			}
			return {};
		}

		constexpr std::string_view v3_chunk_insert_sql =
			"INSERT INTO cxxlens_ng_payload_chunk(publication_id,generation,chunk_ordinal,"
			"byte_offset,byte_count,checksum,payload) VALUES(?1,?2,?3,?4,?5,?6,?7)";
		constexpr std::string_view migration_chunk_insert_sql =
			"INSERT INTO cxxlens_ng_migration_payload_chunk(publication_id,generation,"
			"chunk_ordinal,byte_offset,byte_count,checksum,payload) "
			"VALUES(?1,?2,?3,?4,?5,?6,?7)";

		class sqlite_chunk_insert_port final : public sqlite_payload_chunk_port
		{
		  public:
			[[nodiscard]] static result<std::unique_ptr<sqlite_chunk_insert_port>>
			create(sqlite_database& database,
				   const std::string_view insert_sql,
				   const std::string_view publication_id,
				   const std::uint64_t generation)
			{
				auto insert = sqlite_statement::prepare(database, insert_sql, true);
				if (!insert)
					return unexpected(std::move(insert.error()));
				return std::unique_ptr<sqlite_chunk_insert_port>{new sqlite_chunk_insert_port{
					std::move(*insert), std::string{publication_id}, generation}};
			}

			[[nodiscard]] result<void> emit(const sqlite_payload_chunk_frame& frame) override
			{
				if (auto value = insert_.bind_text(1, publication_id_); !value)
					return value;
				if (auto value = insert_.bind_unsigned(2, generation_); !value)
					return value;
				if (auto value = insert_.bind_unsigned(3, frame.ordinal); !value)
					return value;
				if (auto value = insert_.bind_unsigned(4, frame.byte_offset); !value)
					return value;
				if (auto value = insert_.bind_unsigned(5, frame.byte_count); !value)
					return value;
				if (auto value = insert_.bind_text(6, frame.checksum); !value)
					return value;
				if (auto value = insert_.bind_blob(7, frame.payload); !value)
					return value;
				if (auto value = insert_.expect_done(); !value)
					return value;
				return insert_.reset();
			}

		  private:
			sqlite_chunk_insert_port(sqlite_statement insert,
									 std::string publication_id,
									 const std::uint64_t generation)
				: insert_{std::move(insert)}, publication_id_{std::move(publication_id)},
				  generation_{generation}
			{
			}

			sqlite_statement insert_;
			std::string publication_id_;
			std::uint64_t generation_{};
		};

		[[nodiscard]] result<sqlite_payload_stream_receipt>
		insert_streamed_snapshot_chunks(sqlite_database& database,
										const std::string_view insert_sql,
										const std::string_view publication_id,
										const std::uint64_t generation,
										const snapshot_handle::data& value)
		{
			sqlite_payload_chunk_framer measured;
			if (auto encoded = encode_snapshot(value, measured); !encoded)
				return unexpected(std::move(encoded.error()));
			auto measurement = measured.finish();
			if (!measurement)
				return unexpected(std::move(measurement.error()));

			auto port =
				sqlite_chunk_insert_port::create(database, insert_sql, publication_id, generation);
			if (!port)
				return unexpected(std::move(port.error()));
			sqlite_payload_chunk_framer emitted{port->get()};
			if (auto encoded = encode_snapshot(value, emitted); !encoded)
				return unexpected(std::move(encoded.error()));
			auto receipt = emitted.finish();
			if (!receipt)
				return unexpected(std::move(receipt.error()));
			if (receipt->byte_count != measurement->byte_count ||
				receipt->chunk_count != measurement->chunk_count ||
				receipt->aggregate_byte_count != measurement->aggregate_byte_count ||
				receipt->full_checksum != measurement->full_checksum)
				return unexpected(
					store_error("store.corrupt", std::string{publication_id}, "payload-replay"));
			return receipt;
		}

		[[nodiscard]] result<void> insert_v3_chunks(sqlite_database& database,
													const std::string_view publication_id,
													const std::uint64_t generation,
													const std::span<const std::byte> payload)
		{
			return insert_chunk_rows(
				database, v3_chunk_insert_sql, publication_id, generation, payload);
		}

		constexpr std::string_view v3_chunk_select_sql =
			"SELECT chunk_ordinal,byte_offset,byte_count,checksum,payload FROM "
			"cxxlens_ng_payload_chunk WHERE publication_id=?1 AND generation=?2 "
			"ORDER BY chunk_ordinal";
		constexpr std::string_view migration_chunk_select_sql =
			"SELECT chunk_ordinal,byte_offset,byte_count,checksum,payload FROM "
			"cxxlens_ng_migration_payload_chunk WHERE publication_id=?1 AND generation=?2 "
			"ORDER BY chunk_ordinal";

		class sqlite_statement_chunk_record_source final : public sqlite_payload_chunk_record_source
		{
		  public:
			[[nodiscard]] static result<std::unique_ptr<sqlite_statement_chunk_record_source>>
			create(sqlite_database& database,
				   const std::string_view select_sql,
				   const std::string_view publication_id,
				   const std::uint64_t generation)
			{
				auto selected = sqlite_statement::prepare(database, select_sql, true);
				if (!selected)
					return unexpected(std::move(selected.error()));
				if (auto bound = selected->bind_text(1, publication_id); !bound)
					return unexpected(std::move(bound.error()));
				if (auto bound = selected->bind_unsigned(2, generation); !bound)
					return unexpected(std::move(bound.error()));
				return std::unique_ptr<sqlite_statement_chunk_record_source>{
					new sqlite_statement_chunk_record_source{std::move(*selected)}};
			}

			[[nodiscard]] result<std::optional<sqlite_payload_chunk_record>> next() override
			{
				if (done_)
					return std::optional<sqlite_payload_chunk_record>{};
				const auto code = selected_.step();
				if (code == sqlite_done)
				{
					done_ = true;
					return std::optional<sqlite_payload_chunk_record>{};
				}
				if (code != sqlite_row)
					return unexpected(store_error("store.sqlite-failure", "database", "chunks"));
				auto ordinal = selected_.column_unsigned(0);
				auto offset = selected_.column_unsigned(1);
				auto byte_count = selected_.column_unsigned(2);
				auto checksum = selected_.column_text(3);
				auto payload = selected_.column_blob_view(4);
				if (!ordinal || !offset || !byte_count || !checksum || !payload)
					return unexpected(store_error("store.corrupt", "sqlite", "chunk-row"));
				checksum_ = std::move(*checksum);
				return std::optional<sqlite_payload_chunk_record>{sqlite_payload_chunk_record{
					*ordinal, *offset, *byte_count, checksum_, *payload}};
			}

		  private:
			explicit sqlite_statement_chunk_record_source(sqlite_statement selected) noexcept
				: selected_{std::move(selected)}
			{
			}

			sqlite_statement selected_;
			std::string checksum_;
			bool done_{};
		};

		class sqlite_database_payload_chunk_source final
			: public sqlite_replayable_payload_chunk_source
		{
		  public:
			sqlite_database_payload_chunk_source(sqlite_database& database,
												 const std::string_view select_sql,
												 std::string publication_id,
												 const std::uint64_t generation)
				: database_{&database}, select_sql_{select_sql},
				  publication_id_{std::move(publication_id)}, generation_{generation}
			{
			}

			[[nodiscard]] result<std::unique_ptr<sqlite_payload_chunk_record_source>>
			open_chunk_pass() const override
			{
				if (database_ == nullptr)
					return unexpected(store_error("store.corrupt", "sqlite", "chunk-source"));
				auto opened = sqlite_statement_chunk_record_source::create(
					*database_, select_sql_, publication_id_, generation_);
				if (!opened)
					return unexpected(std::move(opened.error()));
				return std::unique_ptr<sqlite_payload_chunk_record_source>{std::move(*opened)};
			}

		  private:
			sqlite_database* database_{};
			std::string_view select_sql_;
			std::string publication_id_;
			std::uint64_t generation_{};
		};

		[[nodiscard]] result<std::shared_ptr<const sqlite_replayable_byte_source>>
		make_validated_payload_source(sqlite_database& database,
									  const std::string_view select_sql,
									  const std::string_view publication_id,
									  const std::uint64_t generation,
									  sqlite_payload_stream_expectation expectation)
		{
			auto rows = std::make_shared<sqlite_database_payload_chunk_source>(
				database, select_sql, std::string{publication_id}, generation);
			return std::shared_ptr<const sqlite_replayable_byte_source>{
				std::make_shared<sqlite_validated_replayable_payload_source>(
					std::move(rows), std::move(expectation))};
		}

		[[nodiscard]] result<void> insert_publication_row(sqlite_database& database,
														  const std::string_view insert_sql,
														  const publication_record& record,
														  const std::string_view checksum,
														  const std::uint64_t byte_count,
														  const std::uint64_t chunk_count)
		{
			auto insert = sqlite_statement::prepare(database, insert_sql, true);
			if (!insert)
				return unexpected(std::move(insert.error()));
			const std::array text_values{
				std::pair{1, std::string_view{record.publication_id}},
				std::pair{2, std::string_view{record.series_id}},
				std::pair{3, std::string_view{record.snapshot_id}},
			};
			for (const auto& [index, value] : text_values)
				if (auto bound = insert->bind_text(index, value); !bound)
					return bound;
			if (auto value = insert->bind_unsigned(4, record.sequence); !value)
				return value;
			if (auto value = insert->bind_unsigned(5, record.physical_generation); !value)
				return value;
			if (auto value =
					insert->bind_unsigned(6, record.parent_publication.has_value() ? 1U : 0U);
				!value)
				return value;
			if (auto value =
					insert->bind_text(7, record.parent_publication.value_or(std::string{}));
				!value)
				return value;
			if (auto value = insert->bind_unsigned(8, static_cast<std::uint8_t>(record.state));
				!value)
				return value;
			if (auto value = insert->bind_text(9, checksum); !value)
				return value;
			if (auto value = insert->bind_unsigned(10, byte_count); !value)
				return value;
			if (auto value = insert->bind_unsigned(11, chunk_count); !value)
				return value;
			return insert->expect_done();
		}

		constexpr std::string_view v3_publication_insert_sql =
			"INSERT INTO cxxlens_ng_publication(publication_id,series_id,snapshot_id,sequence,"
			"generation,parent,state,payload_checksum,payload_byte_count,payload_chunk_count) "
			"VALUES(?1,?2,?3,?4,?5,CASE WHEN ?6=0 THEN NULL ELSE ?7 END,?8,?9,?10,?11)";
		constexpr std::string_view migration_publication_insert_sql =
			"INSERT INTO cxxlens_ng_migration_publication(publication_id,series_id,snapshot_id,"
			"sequence,generation,parent,state,payload_checksum,payload_byte_count,"
			"payload_chunk_count) VALUES(?1,?2,?3,?4,?5,CASE WHEN ?6=0 THEN NULL ELSE ?7 "
			"END,?8,?9,?10,?11)";

		[[nodiscard]] result<void> insert_v3_publication_row(sqlite_database& database,
															 const publication_record& record,
															 const std::string_view checksum,
															 const std::uint64_t byte_count,
															 const std::uint64_t chunk_count)
		{
			return insert_publication_row(
				database, v3_publication_insert_sql, record, checksum, byte_count, chunk_count);
		}

		[[nodiscard]] result<void>
		bind_v2_publication_values(sqlite_statement& statement,
								   const publication_record& record,
								   const std::string_view checksum,
								   const std::span<const std::byte> payload)
		{
			const std::array text_values{
				std::pair{1, std::string_view{record.publication_id}},
				std::pair{2, std::string_view{record.series_id}},
				std::pair{3, std::string_view{record.snapshot_id}},
			};
			for (const auto& [index, value] : text_values)
				if (auto bound = statement.bind_text(index, value); !bound)
					return bound;
			if (auto bound = statement.bind_unsigned(4, record.sequence); !bound)
				return bound;
			if (auto bound = statement.bind_unsigned(5, record.physical_generation); !bound)
				return bound;
			if (record.parent_publication)
			{
				if (auto bound = statement.bind_text(6, *record.parent_publication); !bound)
					return bound;
			}
			else if (auto bound = statement.bind_null_value(6); !bound)
				return bound;
			if (auto bound = statement.bind_unsigned(7, static_cast<std::uint8_t>(record.state));
				!bound)
				return bound;
			if (auto bound = statement.bind_text(8, checksum); !bound)
				return bound;
			return statement.bind_blob(9, payload);
		}

		[[nodiscard]] result<void>
		insert_v2_publication_row(sqlite_database& database,
								  const publication_record& record,
								  const std::string_view checksum,
								  const std::span<const std::byte> payload,
								  const bool replace)
		{
			auto insert = sqlite_statement::prepare(
				database,
				replace ? "INSERT OR REPLACE INTO cxxlens_ng_publication(publication_id,series_id,"
						  "snapshot_id,sequence,generation,parent,state,checksum,payload) "
						  "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)"
						: "INSERT INTO cxxlens_ng_publication(publication_id,series_id,snapshot_id,"
						  "sequence,generation,parent,state,checksum,payload) "
						  "VALUES(?1,?2,?3,?4,?5,?6,?7,?8,?9)",
				false);
			if (!insert)
				return unexpected(std::move(insert.error()));
			if (auto bound = bind_v2_publication_values(*insert, record, checksum, payload); !bound)
				return bound;
			return insert->expect_done();
		}

		[[nodiscard]] result<void>
		update_v2_publication_row(sqlite_database& database,
								  const std::string_view prior_publication_id,
								  const publication_record& record,
								  const std::string_view checksum,
								  const std::span<const std::byte> payload)
		{
			auto update = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_publication SET publication_id=?1,series_id=?2,snapshot_id=?3,"
				"sequence=?4,generation=?5,parent=?6,state=?7,checksum=?8,payload=?9 "
				"WHERE publication_id=?10",
				false);
			if (!update)
				return unexpected(std::move(update.error()));
			if (auto bound = bind_v2_publication_values(*update, record, checksum, payload); !bound)
				return bound;
			if (auto bound = update->bind_text(10, prior_publication_id); !bound)
				return bound;
			return update->expect_done();
		}

		[[nodiscard]] result<void>
		update_v2_series_head_for_replacement(sqlite_database& database,
											  const std::string_view prior_publication_id,
											  const publication_record& record)
		{
			auto update = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_series_head SET series_id=?1,current_publication=?2,sequence=?3 "
				"WHERE current_publication=?4",
				false);
			if (!update)
				return unexpected(std::move(update.error()));
			if (auto bound = update->bind_text(1, record.series_id); !bound)
				return bound;
			if (auto bound = update->bind_text(2, record.publication_id); !bound)
				return bound;
			if (auto bound = update->bind_unsigned(3, record.sequence); !bound)
				return bound;
			if (auto bound = update->bind_text(4, prior_publication_id); !bound)
				return bound;
			return update->expect_done();
		}

		[[nodiscard]] result<void> delete_v3_chunks(sqlite_database& database,
													const std::string_view publication_id,
													const std::uint64_t generation)
		{
			auto deleted = sqlite_statement::prepare(
				database,
				"DELETE FROM cxxlens_ng_payload_chunk WHERE publication_id=?1 AND generation=?2",
				true);
			if (!deleted)
				return unexpected(std::move(deleted.error()));
			if (auto value = deleted->bind_text(1, publication_id); !value)
				return value;
			if (auto value = deleted->bind_unsigned(2, generation); !value)
				return value;
			return deleted->expect_done();
		}

		[[nodiscard]] result<void>
		replace_v3_publication_payload(sqlite_database& database,
									   const publication_record& old_record,
									   const publication_record& record,
									   const std::span<const std::byte> payload)
		{
			const auto checksum = content_digest(payload);
			const bool same_owner = old_record.publication_id == record.publication_id &&
				old_record.physical_generation == record.physical_generation;
			if (same_owner)
				if (auto deleted = delete_v3_chunks(
						database, old_record.publication_id, old_record.physical_generation);
					!deleted)
					return deleted;
			if (auto chunks = insert_v3_chunks(
					database, record.publication_id, record.physical_generation, payload);
				!chunks)
				return chunks;

			auto delete_row = sqlite_statement::prepare(
				database, "DELETE FROM cxxlens_ng_publication WHERE publication_id=?1", true);
			if (!delete_row)
				return unexpected(std::move(delete_row.error()));
			if (auto bound = delete_row->bind_text(1, old_record.publication_id); !bound)
				return bound;
			if (auto deleted = delete_row->expect_done(); !deleted)
				return deleted;
			if (auto inserted = insert_v3_publication_row(database,
														  record,
														  checksum,
														  payload.size(),
														  payload_chunk_count(payload.size()));
				!inserted)
				return inserted;

			auto update_head = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_series_head SET series_id=?1,current_publication=?2,sequence=?3 "
				"WHERE current_publication=?4",
				true);
			if (!update_head)
				return unexpected(std::move(update_head.error()));
			if (auto bound = update_head->bind_text(1, record.series_id); !bound)
				return bound;
			if (auto bound = update_head->bind_text(2, record.publication_id); !bound)
				return bound;
			if (auto bound = update_head->bind_unsigned(3, record.sequence); !bound)
				return bound;
			if (auto bound = update_head->bind_text(4, old_record.publication_id); !bound)
				return bound;
			if (auto updated = update_head->expect_done(); !updated)
				return updated;
			if (!same_owner)
				if (auto deleted = delete_v3_chunks(
						database, old_record.publication_id, old_record.physical_generation);
					!deleted)
					return deleted;
			return {};
		}

		[[nodiscard]] result<void> replace_v3_publication(sqlite_database& database,
														  const publication_record& old_record,
														  const snapshot_handle::data& replacement)
		{
			const auto& record = replacement.publication_record_value;
			const bool same_owner = old_record.publication_id == record.publication_id &&
				old_record.physical_generation == record.physical_generation;
			if (same_owner)
				if (auto deleted = delete_v3_chunks(
						database, old_record.publication_id, old_record.physical_generation);
					!deleted)
					return deleted;
			auto receipt = insert_streamed_snapshot_chunks(database,
														   v3_chunk_insert_sql,
														   record.publication_id,
														   record.physical_generation,
														   replacement);
			if (!receipt)
				return unexpected(std::move(receipt.error()));

			auto delete_row = sqlite_statement::prepare(
				database, "DELETE FROM cxxlens_ng_publication WHERE publication_id=?1", true);
			if (!delete_row)
				return unexpected(std::move(delete_row.error()));
			if (auto bound = delete_row->bind_text(1, old_record.publication_id); !bound)
				return bound;
			if (auto deleted = delete_row->expect_done(); !deleted)
				return deleted;
			if (auto inserted = insert_v3_publication_row(database,
														  record,
														  receipt->full_checksum,
														  receipt->byte_count,
														  receipt->chunk_count);
				!inserted)
				return inserted;

			auto update_head = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_series_head SET series_id=?1,current_publication=?2,sequence=?3 "
				"WHERE current_publication=?4",
				true);
			if (!update_head)
				return unexpected(std::move(update_head.error()));
			if (auto bound = update_head->bind_text(1, record.series_id); !bound)
				return bound;
			if (auto bound = update_head->bind_text(2, record.publication_id); !bound)
				return bound;
			if (auto bound = update_head->bind_unsigned(3, record.sequence); !bound)
				return bound;
			if (auto bound = update_head->bind_text(4, old_record.publication_id); !bound)
				return bound;
			if (auto updated = update_head->expect_done(); !updated)
				return updated;
			if (!same_owner)
				if (auto deleted = delete_v3_chunks(
						database, old_record.publication_id, old_record.physical_generation);
					!deleted)
					return deleted;
			return {};
		}

		struct sqlite_persisted_publication
		{
			publication_record record;
			std::string checksum;
			std::shared_ptr<const sqlite_replayable_byte_source> payload_source;
			std::uint64_t payload_byte_count{};
			std::uint64_t payload_chunk_count{};
		};

		[[nodiscard]] result<std::size_t>
		payload_generation_offset(binary_reader& reader, const std::uint64_t expected_generation)
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

		class sqlite_generation_rewrite_byte_source final : public sqlite_bounded_byte_source
		{
		  public:
			sqlite_generation_rewrite_byte_source(
				std::unique_ptr<sqlite_bounded_byte_source> source,
				const std::uint64_t expected_size,
				const std::uint64_t generation_offset,
				const std::uint64_t generation) noexcept
				: source_{std::move(source)}, expected_size_{expected_size},
				  generation_offset_{generation_offset}
			{
				for (std::size_t index{}; index < generation_bytes_.size(); ++index)
				{
					const auto shift =
						static_cast<unsigned>((generation_bytes_.size() - 1U - index) * 8U);
					generation_bytes_[index] =
						static_cast<std::byte>((generation >> shift) & 0xffU);
				}
			}

			[[nodiscard]] result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (!source_)
					return unexpected(store_error("store.corrupt", "payload", "generation-source"));
				auto read = source_->read(output);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read == 0U)
				{
					if (offset_ != expected_size_)
						return unexpected(
							store_error("store.corrupt", "payload", "generation-source-size"));
					return std::size_t{};
				}
				if (offset_ > expected_size_ || *read > output.size() ||
					*read > expected_size_ - offset_)
					return unexpected(
						store_error("store.corrupt", "payload", "generation-source-size"));
				const auto begin = offset_;
				const auto end = offset_ + *read;
				for (std::size_t index{}; index < generation_bytes_.size(); ++index)
				{
					const auto position = generation_offset_ + index;
					if (position >= begin && position < end)
						output[static_cast<std::size_t>(position - begin)] =
							generation_bytes_[index];
				}
				offset_ = end;
				return *read;
			}

		  private:
			std::unique_ptr<sqlite_bounded_byte_source> source_;
			std::uint64_t expected_size_{};
			std::uint64_t generation_offset_{};
			std::uint64_t offset_{};
			std::array<std::byte, 8U> generation_bytes_{};
		};

		class sqlite_generation_rewrite_payload_source final : public sqlite_replayable_byte_source
		{
		  public:
			[[nodiscard]] static result<std::shared_ptr<const sqlite_replayable_byte_source>>
			create(std::shared_ptr<const sqlite_replayable_byte_source> source,
				   const std::uint64_t expected_size,
				   const std::uint64_t expected_generation,
				   const std::uint64_t replacement_generation)
			{
				if (!source || expected_size > std::numeric_limits<std::size_t>::max())
					return unexpected(store_error("store.corrupt", "payload", "generation-source"));
				auto pass = source->open_pass();
				if (!pass)
					return unexpected(std::move(pass.error()));
				binary_reader reader{**pass, expected_size};
				auto offset = payload_generation_offset(reader, expected_generation);
				if (!offset || *offset > expected_size || expected_size - *offset < 8U)
					return unexpected(
						offset ? store_error("store.corrupt", "payload", "truncated-generation")
							   : std::move(offset.error()));
				return std::shared_ptr<const sqlite_replayable_byte_source>{
					new sqlite_generation_rewrite_payload_source{
						std::move(source), expected_size, *offset, replacement_generation}};
			}

			[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>>
			open_pass() const override
			{
				auto pass = source_->open_pass();
				if (!pass)
					return unexpected(std::move(pass.error()));
				return std::unique_ptr<sqlite_bounded_byte_source>{
					new sqlite_generation_rewrite_byte_source{std::move(*pass),
															  expected_size_,
															  generation_offset_,
															  replacement_generation_}};
			}

		  private:
			sqlite_generation_rewrite_payload_source(
				std::shared_ptr<const sqlite_replayable_byte_source> source,
				const std::uint64_t expected_size,
				const std::uint64_t generation_offset,
				const std::uint64_t replacement_generation) noexcept
				: source_{std::move(source)}, expected_size_{expected_size},
				  generation_offset_{generation_offset},
				  replacement_generation_{replacement_generation}
			{
			}

			std::shared_ptr<const sqlite_replayable_byte_source> source_;
			std::uint64_t expected_size_{};
			std::uint64_t generation_offset_{};
			std::uint64_t replacement_generation_{};
		};

		class sqlite_window_rewrite_byte_source final : public sqlite_bounded_byte_source
		{
		  public:
			sqlite_window_rewrite_byte_source(
				std::unique_ptr<sqlite_bounded_byte_source> source,
				const std::uint64_t expected_size,
				const std::uint64_t replacement_offset,
				std::shared_ptr<const std::vector<std::byte>> replacement)
				: source_{std::move(source)}, expected_size_{expected_size},
				  replacement_offset_{replacement_offset}, replacement_{std::move(replacement)}
			{
			}

			[[nodiscard]] result<std::size_t> read(const std::span<std::byte> output) override
			{
				if (!source_ || !replacement_)
					return unexpected(store_error("store.corrupt", "payload", "rewrite-source"));
				auto read = source_->read(output);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read == 0U)
				{
					if (offset_ != expected_size_)
						return unexpected(store_error("store.corrupt", "payload", "source-size"));
					return std::size_t{};
				}
				if (offset_ > expected_size_ || *read > output.size() ||
					*read > expected_size_ - offset_)
					return unexpected(store_error("store.corrupt", "payload", "source-size"));
				const auto begin = offset_;
				const auto end = offset_ + *read;
				for (std::size_t index{}; index < replacement_->size(); ++index)
				{
					const auto position = replacement_offset_ + index;
					if (position >= begin && position < end)
						output[static_cast<std::size_t>(position - begin)] =
							replacement_->at(index);
				}
				offset_ = end;
				return *read;
			}

		  private:
			std::unique_ptr<sqlite_bounded_byte_source> source_;
			std::uint64_t expected_size_{};
			std::uint64_t replacement_offset_{};
			std::uint64_t offset_{};
			std::shared_ptr<const std::vector<std::byte>> replacement_;
		};

		class sqlite_window_rewrite_payload_source final : public sqlite_replayable_byte_source
		{
		  public:
			[[nodiscard]] static result<std::shared_ptr<const sqlite_window_rewrite_payload_source>>
			create(std::shared_ptr<const sqlite_replayable_byte_source> source,
				   const std::uint64_t expected_size,
				   const std::string_view before,
				   const std::string_view after,
				   const std::size_t occurrence)
			{
				if (!source || before.empty() || before.size() != after.size())
					return unexpected(store_error("store.corrupt", "test-rewrite", "invalid"));
				std::vector<std::byte> needle;
				std::vector<std::byte> replacement;
				needle.reserve(before.size());
				replacement.reserve(after.size());
				for (const auto value : before)
					needle.push_back(static_cast<std::byte>(static_cast<unsigned char>(value)));
				for (const auto value : after)
					replacement.push_back(
						static_cast<std::byte>(static_cast<unsigned char>(value)));
				std::vector<std::size_t> prefix(needle.size());
				for (std::size_t index = 1U, matched{}; index < needle.size(); ++index)
				{
					while (matched != 0U && needle[index] != needle[matched])
						matched = prefix[matched - 1U];
					if (needle[index] == needle[matched])
						++matched;
					prefix[index] = matched;
				}

				auto pass = source->open_pass();
				if (!pass)
					return unexpected(std::move(pass.error()));
				std::array<std::byte, 64U * 1024U> scratch{};
				std::uint64_t consumed{};
				std::size_t matched{};
				std::size_t found_count{};
				std::optional<std::uint64_t> found_offset;
				for (;;)
				{
					auto read = (*pass)->read(scratch);
					if (!read)
						return unexpected(std::move(read.error()));
					if (*read == 0U)
						break;
					std::uint64_t next_consumed{};
					if (*read > scratch.size() ||
						!sqlite_checked_add_u64(consumed, *read, next_consumed) ||
						next_consumed > expected_size)
						return unexpected(store_error("store.corrupt", "payload", "source-size"));
					for (std::size_t index{}; index < *read && !found_offset; ++index)
					{
						const auto value = scratch[index];
						while (matched != 0U && value != needle[matched])
							matched = prefix[matched - 1U];
						if (value == needle[matched])
							++matched;
						if (matched == needle.size())
						{
							if (found_count == occurrence)
								found_offset = consumed + index + 1U - needle.size();
							else
								++found_count;
							matched = 0U;
						}
					}
					consumed = next_consumed;
				}
				if (consumed != expected_size)
					return unexpected(store_error("store.corrupt", "payload", "source-size"));
				if (!found_offset)
					return unexpected(store_error("store.corrupt", "test-rewrite", "not-found"));
				return std::shared_ptr<const sqlite_window_rewrite_payload_source>{
					new sqlite_window_rewrite_payload_source{
						std::move(source),
						expected_size,
						*found_offset,
						std::make_shared<const std::vector<std::byte>>(std::move(replacement))}};
			}

			[[nodiscard]] result<std::unique_ptr<sqlite_bounded_byte_source>>
			open_pass() const override
			{
				auto pass = source_->open_pass();
				if (!pass)
					return unexpected(std::move(pass.error()));
				return std::unique_ptr<sqlite_bounded_byte_source>{
					new sqlite_window_rewrite_byte_source{
						std::move(*pass), expected_size_, replacement_offset_, replacement_}};
			}

			[[nodiscard]] std::uint64_t replacement_offset() const noexcept
			{
				return replacement_offset_;
			}

		  private:
			sqlite_window_rewrite_payload_source(
				std::shared_ptr<const sqlite_replayable_byte_source> source,
				const std::uint64_t expected_size,
				const std::uint64_t replacement_offset,
				std::shared_ptr<const std::vector<std::byte>> replacement)
				: source_{std::move(source)}, expected_size_{expected_size},
				  replacement_offset_{replacement_offset}, replacement_{std::move(replacement)}
			{
			}

			std::shared_ptr<const sqlite_replayable_byte_source> source_;
			std::uint64_t expected_size_{};
			std::uint64_t replacement_offset_{};
			std::shared_ptr<const std::vector<std::byte>> replacement_;
		};

		class sqlite_migration_fault_chunk_port final : public sqlite_payload_chunk_port
		{
		  public:
			sqlite_migration_fault_chunk_port(std::unique_ptr<sqlite_payload_chunk_port> delegate,
											  std::uint64_t& ordinal,
											  const std::uint64_t total) noexcept
				: delegate_{std::move(delegate)}, ordinal_{ordinal}, total_{total}
			{
			}

			[[nodiscard]] result<void> emit(const sqlite_payload_chunk_frame& frame) override
			{
				if (!delegate_ || ordinal_ == std::numeric_limits<std::uint64_t>::max() ||
					ordinal_ >= total_)
					return unexpected(
						store_error("store.corrupt", "migration-fault", "payload-ordinal"));
				++ordinal_;
				const auto dispatch = [&](const sqlite_store_fault_timing timing) -> result<void>
				{
					const auto directive =
						dispatch_sqlite_store_fault({sqlite_store_operation::migrate_predecessor,
													 sqlite_store_fault_boundary::payload_chunk,
													 timing,
													 ordinal_,
													 total_});
					if (!directive.issued)
						return {};
					if (directive.action == sqlite_store_fault_action::request_process_crash)
						std::_Exit(86);
					return unexpected(
						store_error("store.sqlite-failure", "migration-fault", "injected"));
				};
				if (auto fault = dispatch(sqlite_store_fault_timing::before); !fault)
					return fault;
				if (auto emitted = delegate_->emit(frame); !emitted)
					return emitted;
				return dispatch(sqlite_store_fault_timing::after);
			}

		  private:
			std::unique_ptr<sqlite_payload_chunk_port> delegate_;
			std::uint64_t& ordinal_;
			std::uint64_t total_{};
		};

		[[nodiscard]] result<sqlite_payload_stream_receipt>
		insert_replayable_payload_chunks(sqlite_database& database,
										 const std::string_view insert_sql,
										 const std::string_view publication_id,
										 const std::uint64_t generation,
										 const sqlite_replayable_byte_source& source,
										 const std::uint64_t expected_size,
										 std::uint64_t* const migration_fault_ordinal = nullptr,
										 const std::uint64_t migration_fault_total = 0U)
		{
			auto pass = source.open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			auto port =
				sqlite_chunk_insert_port::create(database, insert_sql, publication_id, generation);
			if (!port)
				return unexpected(std::move(port.error()));
			std::unique_ptr<sqlite_payload_chunk_port> emitting_port = std::move(*port);
			if (migration_fault_ordinal != nullptr)
				emitting_port = std::make_unique<sqlite_migration_fault_chunk_port>(
					std::move(emitting_port), *migration_fault_ordinal, migration_fault_total);
			sqlite_payload_chunk_framer framer{emitting_port.get()};
			std::array<std::byte, 64U * 1024U> scratch{};
			for (;;)
			{
				auto read = (*pass)->read(scratch);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read == 0U)
					break;
				if (*read > scratch.size())
					return unexpected(store_error("store.corrupt", "payload", "source-window"));
				if (auto appended = framer.append(std::span{scratch}.first(*read)); !appended)
					return unexpected(std::move(appended.error()));
			}
			auto receipt = framer.finish();
			if (!receipt)
				return unexpected(std::move(receipt.error()));
			if (receipt->byte_count != expected_size)
				return unexpected(store_error("store.corrupt", "payload", "source-size"));
			return receipt;
		}

		[[nodiscard]] result<bool>
		replayable_payloads_equal(const sqlite_replayable_byte_source& left,
								  const sqlite_replayable_byte_source& right,
								  const std::uint64_t expected_size)
		{
			auto left_pass = left.open_pass();
			auto right_pass = right.open_pass();
			if (!left_pass)
				return unexpected(std::move(left_pass.error()));
			if (!right_pass)
				return unexpected(std::move(right_pass.error()));
			std::array<std::byte, 64U * 1024U> left_bytes{};
			std::array<std::byte, 64U * 1024U> right_bytes{};
			std::uint64_t compared{};
			for (;;)
			{
				auto left_read = (*left_pass)->read(left_bytes);
				auto right_read = (*right_pass)->read(right_bytes);
				if (!left_read)
					return unexpected(std::move(left_read.error()));
				if (!right_read)
					return unexpected(std::move(right_read.error()));
				if (*left_read != *right_read || *left_read > left_bytes.size())
					return false;
				if (*left_read == 0U)
					break;
				if (!std::ranges::equal(std::span{left_bytes}.first(*left_read),
										std::span{right_bytes}.first(*right_read)))
					return false;
				std::uint64_t next_compared{};
				if (!sqlite_checked_add_u64(compared, *left_read, next_compared) ||
					next_compared > expected_size)
					return unexpected(store_error("store.counter-overflow", "payload_byte_count"));
				compared = next_compared;
			}
			return compared == expected_size;
		}

		[[nodiscard]] result<std::string>
		replayable_payload_checksum(const sqlite_replayable_byte_source& source,
									const std::uint64_t expected_size)
		{
			auto pass = source.open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			std::array<std::byte, 64U * 1024U> scratch{};
			sqlite_incremental_sha256 digest;
			std::uint64_t consumed{};
			for (;;)
			{
				auto read = (*pass)->read(scratch);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read == 0U)
					break;
				std::uint64_t next_consumed{};
				if (*read > scratch.size() ||
					!sqlite_checked_add_u64(consumed, *read, next_consumed) ||
					next_consumed > expected_size)
					return unexpected(store_error("store.corrupt", "payload", "source-size"));
				if (auto updated = digest.update(std::span{scratch}.first(*read)); !updated)
					return unexpected(std::move(updated.error()));
				consumed = next_consumed;
			}
			if (consumed != expected_size)
				return unexpected(store_error("store.corrupt", "payload", "source-size"));
			auto checksum = digest.finish();
			if (!checksum)
				return unexpected(std::move(checksum.error()));
			return checksum;
		}

		[[nodiscard]] result<bool>
		replayable_payload_checksum_matches(const sqlite_replayable_byte_source& source,
											const std::uint64_t expected_size,
											const std::string_view expected_checksum)
		{
			auto checksum = replayable_payload_checksum(source, expected_size);
			if (!checksum)
				return unexpected(std::move(checksum.error()));
			return *checksum == expected_checksum;
		}

		[[nodiscard]] result<void>
		rewrite_v3_payload_window_in_place(sqlite_database& database,
										   const sqlite_persisted_publication& row,
										   const std::string_view before,
										   const std::string_view after,
										   const std::size_t occurrence)
		{
			if (!row.payload_source)
				return unexpected(
					store_error("store.publication-not-found", row.record.publication_id));
			auto intact = replayable_payload_checksum_matches(
				*row.payload_source, row.payload_byte_count, row.checksum);
			if (!intact || !*intact)
				return unexpected(intact ? store_error("store.corrupt",
													   row.record.publication_id,
													   "payload-checksum")
										 : std::move(intact.error()));
			auto rewritten = sqlite_window_rewrite_payload_source::create(
				row.payload_source, row.payload_byte_count, before, after, occurrence);
			if (!rewritten)
				return unexpected(std::move(rewritten.error()));
			auto expected_checksum =
				replayable_payload_checksum(**rewritten, row.payload_byte_count);
			if (!expected_checksum)
				return unexpected(std::move(expected_checksum.error()));
			const auto replacement_offset = (*rewritten)->replacement_offset();
			if (replacement_offset > row.payload_byte_count ||
				after.size() > row.payload_byte_count - replacement_offset)
				return unexpected(store_error("store.corrupt", "test-rewrite", "range"));
			const auto replacement_end = replacement_offset + after.size();
			const auto first_ordinal = replacement_offset / sqlite_payload_chunk_maximum;
			const auto final_ordinal = (replacement_end - 1U) / sqlite_payload_chunk_maximum;
			auto update = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_payload_chunk SET checksum=?1,payload=?2 WHERE "
				"publication_id=?3 AND generation=?4 AND chunk_ordinal=?5",
				true);
			if (!update)
				return unexpected(std::move(update.error()));
			for (auto ordinal = first_ordinal; ordinal <= final_ordinal; ++ordinal)
			{
				auto selected = sqlite_statement::prepare(
					database,
					"SELECT byte_offset,byte_count,payload FROM cxxlens_ng_payload_chunk WHERE "
					"publication_id=?1 AND generation=?2 AND chunk_ordinal=?3",
					true);
				if (!selected)
					return unexpected(std::move(selected.error()));
				if (auto bound = selected->bind_text(1, row.record.publication_id); !bound)
					return bound;
				if (auto bound = selected->bind_unsigned(2, row.record.physical_generation); !bound)
					return bound;
				if (auto bound = selected->bind_unsigned(3, ordinal); !bound)
					return bound;
				if (selected->step() != sqlite_row)
					return unexpected(
						store_error("store.corrupt", row.record.publication_id, "chunk"));
				auto chunk_offset = selected->column_unsigned(0);
				auto byte_count = selected->column_unsigned(1);
				auto payload = selected->column_blob(2);
				if (!chunk_offset || !byte_count || !payload || *byte_count != payload->size() ||
					*chunk_offset > row.payload_byte_count ||
					*byte_count > row.payload_byte_count - *chunk_offset)
					return unexpected(
						store_error("store.corrupt", row.record.publication_id, "chunk"));
				const auto chunk_end = *chunk_offset + *byte_count;
				const auto overlap_begin = std::max(*chunk_offset, replacement_offset);
				const auto overlap_end = std::min(chunk_end, replacement_end);
				if (overlap_begin >= overlap_end)
					return unexpected(
						store_error("store.corrupt", row.record.publication_id, "chunk-range"));
				for (auto position = overlap_begin; position < overlap_end; ++position)
					payload->at(static_cast<std::size_t>(position - *chunk_offset)) =
						static_cast<std::byte>(static_cast<unsigned char>(
							after[static_cast<std::size_t>(position - replacement_offset)]));
				const auto checksum = content_digest(*payload);
				if (auto bound = update->bind_text(1, checksum); !bound)
					return bound;
				if (auto bound = update->bind_blob(2, *payload); !bound)
					return bound;
				if (auto bound = update->bind_text(3, row.record.publication_id); !bound)
					return bound;
				if (auto bound = update->bind_unsigned(4, row.record.physical_generation); !bound)
					return bound;
				if (auto bound = update->bind_unsigned(5, ordinal); !bound)
					return bound;
				if (auto updated = update->expect_done(); !updated)
					return updated;
				if (auto reset = update->reset(); !reset)
					return reset;
			}

			auto updated_source = make_validated_payload_source(
				database,
				v3_chunk_select_sql,
				row.record.publication_id,
				row.record.physical_generation,
				sqlite_payload_stream_expectation{
					row.payload_byte_count, row.payload_chunk_count, {}, false});
			if (!updated_source)
				return unexpected(std::move(updated_source.error()));
			auto actual_checksum =
				replayable_payload_checksum(**updated_source, row.payload_byte_count);
			if (!actual_checksum)
				return unexpected(std::move(actual_checksum.error()));
			if (*actual_checksum != *expected_checksum)
				return unexpected(
					store_error("store.corrupt", row.record.publication_id, "payload-rewrite"));
			auto publication = sqlite_statement::prepare(
				database,
				"UPDATE cxxlens_ng_publication SET payload_checksum=?1 WHERE publication_id=?2",
				true);
			if (!publication)
				return unexpected(std::move(publication.error()));
			if (auto bound = publication->bind_text(1, *actual_checksum); !bound)
				return bound;
			if (auto bound = publication->bind_text(2, row.record.publication_id); !bound)
				return bound;
			return publication->expect_done();
		}

		[[nodiscard]] result<std::vector<sqlite_persisted_publication>>
		read_sqlite_publications(sqlite_database& database, const sqlite_physical_format format)
		{
			const auto sql = format == sqlite_physical_format::predecessor_v2
				? "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
				  "checksum,length(payload),typeof(payload),rowid FROM cxxlens_ng_publication "
				  "ORDER BY series_id,sequence,publication_id"
				: "SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
				  "payload_checksum,payload_byte_count,payload_chunk_count FROM "
				  "cxxlens_ng_publication ORDER BY series_id,sequence,publication_id";
			auto selected = sqlite_statement::prepare(
				database, sql, format == sqlite_physical_format::current_v3);
			if (!selected)
				return unexpected(std::move(selected.error()));
			std::vector<sqlite_persisted_publication> output;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "publication"));
				auto publication_id = selected->column_text(0);
				auto series_id = selected->column_text(1);
				auto snapshot_id = selected->column_text(2);
				auto sequence = selected->column_unsigned(3);
				auto generation = selected->column_unsigned(4);
				auto parent = selected->column_optional_text(5);
				auto state = selected->column_unsigned(6);
				auto checksum = selected->column_text(7);
				if (!publication_id || !series_id || !snapshot_id || !sequence || !generation ||
					!parent || !state || !checksum ||
					*state > static_cast<std::uint8_t>(publication_state::rolled_back) ||
					(format == sqlite_physical_format::predecessor_v2 &&
					 !sqlite_payload_digest(*checksum)))
				{
					if (format == sqlite_physical_format::predecessor_v2)
						return unexpected(store_error("store.format-incompatible",
													  "sqlite-physical-format",
													  "v2-profile-mismatch"));
					return unexpected(store_error("store.corrupt", "sqlite", "publication-row"));
				}
				sqlite_persisted_publication value{{std::move(*publication_id),
													std::move(*series_id),
													std::move(*snapshot_id),
													*sequence,
													*generation,
													std::move(*parent),
													static_cast<publication_state>(*state),
													false},
												   std::move(*checksum),
												   nullptr,
												   0U,
												   0U};
				if (format == sqlite_physical_format::predecessor_v2)
				{
					auto byte_count = selected->column_unsigned(8);
					auto storage_class = selected->column_text(9);
					auto rowid = selected->column_signed(10);
					if (!byte_count || !storage_class || *storage_class != "blob" || !rowid ||
						*rowid <= 0 ||
						*byte_count > static_cast<std::uint64_t>(std::numeric_limits<int>::max()))
						return unexpected(store_error("store.format-incompatible",
													  "sqlite-physical-format",
													  "v2-profile-mismatch"));
					value.payload_source =
						std::make_shared<sqlite_v2_payload_source>(database, *rowid, *byte_count);
					value.payload_byte_count = *byte_count;
				}
				else
				{
					auto byte_count = selected->column_unsigned(8);
					auto chunk_count = selected->column_unsigned(9);
					if (!byte_count || !chunk_count)
						return unexpected(
							store_error("store.corrupt", "sqlite", "publication-row"));
					auto payload = make_validated_payload_source(
						database,
						v3_chunk_select_sql,
						value.record.publication_id,
						value.record.physical_generation,
						sqlite_payload_stream_expectation{
							*byte_count, *chunk_count, value.checksum, false});
					if (!payload)
						return unexpected(std::move(payload.error()));
					value.payload_source = std::move(*payload);
					value.payload_byte_count = *byte_count;
					value.payload_chunk_count = *chunk_count;
				}
				output.push_back(std::move(value));
			}
			return output;
		}

		[[nodiscard]] result<std::vector<sqlite_persisted_publication>>
		read_chunked_publications(sqlite_database& database,
								  const std::string_view publication_select_sql,
								  const std::string_view chunk_select_sql)
		{
			auto selected = sqlite_statement::prepare(database, publication_select_sql, true);
			if (!selected)
				return unexpected(std::move(selected.error()));
			std::vector<sqlite_persisted_publication> output;
			for (;;)
			{
				const auto code = selected->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "migration-shadow-row"));
				auto publication_id = selected->column_text(0);
				auto series_id = selected->column_text(1);
				auto snapshot_id = selected->column_text(2);
				auto sequence = selected->column_unsigned(3);
				auto generation = selected->column_unsigned(4);
				auto parent = selected->column_optional_text(5);
				auto state = selected->column_unsigned(6);
				auto checksum = selected->column_text(7);
				auto byte_count = selected->column_unsigned(8);
				auto chunk_count = selected->column_unsigned(9);
				if (!publication_id || !series_id || !snapshot_id || !sequence || !generation ||
					!parent || !state || !checksum || !byte_count || !chunk_count ||
					*state > static_cast<std::uint8_t>(publication_state::rolled_back))
					return unexpected(
						store_error("store.corrupt", "migration-shadow", "publication-row"));
				sqlite_persisted_publication value{{std::move(*publication_id),
													std::move(*series_id),
													std::move(*snapshot_id),
													*sequence,
													*generation,
													std::move(*parent),
													static_cast<publication_state>(*state),
													false},
												   std::move(*checksum),
												   nullptr,
												   0U,
												   0U};
				auto payload = make_validated_payload_source(
					database,
					chunk_select_sql,
					value.record.publication_id,
					value.record.physical_generation,
					sqlite_payload_stream_expectation{
						*byte_count, *chunk_count, value.checksum, false});
				if (!payload)
					return unexpected(std::move(payload.error()));
				value.payload_source = std::move(*payload);
				value.payload_byte_count = *byte_count;
				value.payload_chunk_count = *chunk_count;
				output.push_back(std::move(value));
			}
			return output;
		}

		constexpr std::string_view migration_publication_select_sql =
			"SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
			"payload_checksum,payload_byte_count,payload_chunk_count FROM "
			"cxxlens_ng_migration_publication ORDER BY series_id,sequence,publication_id";
		constexpr std::string_view v3_publication_select_sql =
			"SELECT publication_id,series_id,snapshot_id,sequence,generation,parent,state,"
			"payload_checksum,payload_byte_count,payload_chunk_count FROM "
			"cxxlens_ng_publication ORDER BY series_id,sequence,publication_id";

		[[nodiscard]] result<std::vector<sqlite_persisted_publication>>
		read_migration_shadow_publications(sqlite_database& database)
		{
			return read_chunked_publications(
				database, migration_publication_select_sql, migration_chunk_select_sql);
		}

		[[nodiscard]] result<std::vector<sqlite_persisted_publication>>
		read_v3_migration_projection(sqlite_database& database)
		{
			return read_chunked_publications(
				database, v3_publication_select_sql, v3_chunk_select_sql);
		}

		using publication_record_map = std::map<std::string, publication_record, std::less<>>;
		using series_head_map = std::map<std::string, std::string, std::less<>>;

		[[nodiscard]] result<series_head_map>
		derive_authority_heads(const publication_record_map& records)
		{
			std::map<std::string, std::map<std::uint64_t, const publication_record*>, std::less<>>
				series_records;
			std::map<std::string,
					 std::map<std::pair<std::uint64_t, std::uint64_t>, std::string>,
					 std::less<>>
				snapshot_resolver_keys;
			for (const auto& [id, record] : records)
			{
				if (record.state != publication_state::committed || record.corrupt ||
					record.publication_id != id || !validate_publication_identity(record))
					return unexpected(store_error("store.corrupt", id, "authority-record"));
				auto& by_sequence = series_records[record.series_id];
				if (!by_sequence.emplace(record.sequence, &record).second)
					return unexpected(
						store_error("store.corrupt", record.series_id, "duplicate-sequence"));
				auto& resolver = snapshot_resolver_keys[record.snapshot_id];
				if (const auto [position, inserted] =
						resolver.emplace(std::pair{record.sequence, record.physical_generation},
										 record.publication_id);
					!inserted && position->second != record.publication_id)
					return unexpected(store_error("store.snapshot-ambiguous", record.snapshot_id));
			}

			series_head_map heads;
			for (const auto& [series, by_sequence] : series_records)
			{
				// Generic Store topology requires one parentless root and checked +1 edges.
				// Fresh writers produce sequence 1; the materialization contract is what
				// additionally restricts its accepted genesis evidence to sequence 1.
				std::size_t roots{};
				for (const auto& [sequence, record] : by_sequence)
				{
					(void)sequence;
					if (!record->parent_publication)
					{
						++roots;
						continue;
					}
					const auto parent = records.find(*record->parent_publication);
					if (parent == records.end() || parent->second.series_id != series)
						return unexpected(
							store_error("store.corrupt", record->publication_id, "parent"));
					auto expected =
						checked_counter_increment(parent->second.sequence, "publication_sequence");
					if (!expected || *expected != record->sequence)
						return unexpected(store_error(
							"store.corrupt", record->publication_id, "parent-sequence"));
				}
				if (roots != 1U)
					return unexpected(store_error("store.corrupt", series, "series-roots"));
				heads.emplace(series, by_sequence.rbegin()->second->publication_id);
			}
			return heads;
		}

		// Reopen retains corrupt committed records so read APIs can return their typed
		// no-fallback verdict. Head topology still comes from the complete durable
		// committed record graph rather than a map-order first/max shortcut.
		[[nodiscard]] result<series_head_map>
		derive_diagnostic_heads(const publication_record_map& records)
		{
			std::map<std::string, std::map<std::uint64_t, const publication_record*>, std::less<>>
				series_records;
			for (const auto& [id, record] : records)
			{
				(void)id;
				if (record.state != publication_state::committed)
					continue;
				auto& by_sequence = series_records[record.series_id];
				if (!by_sequence.emplace(record.sequence, &record).second)
					return unexpected(store_error("store.current-ambiguous", record.series_id));
			}

			series_head_map heads;
			for (const auto& [series, by_sequence] : series_records)
			{
				if (std::ranges::any_of(by_sequence,
										[](const auto& entry)
										{
											return entry.second->corrupt;
										}))
				{
					heads.emplace(series, by_sequence.rbegin()->second->publication_id);
					continue;
				}
				std::size_t roots{};
				for (const auto& [sequence, record] : by_sequence)
				{
					(void)sequence;
					if (!record->parent_publication)
					{
						++roots;
						continue;
					}
					const auto parent = records.find(*record->parent_publication);
					if (parent == records.end() ||
						parent->second.state != publication_state::committed ||
						parent->second.series_id != series)
						return unexpected(
							store_error("store.corrupt", record->publication_id, "parent"));
					auto expected =
						checked_counter_increment(parent->second.sequence, "publication_sequence");
					if (!expected || *expected != record->sequence)
						return unexpected(store_error(
							"store.corrupt", record->publication_id, "parent-sequence"));
				}
				if (roots != 1U)
					return unexpected(store_error("store.corrupt", series, "series-roots"));
				heads.emplace(series, by_sequence.rbegin()->second->publication_id);
			}
			return heads;
		}
	} // namespace

	// Source-private test seam for proving the active-WAL-only symbol gate and its precedence.
	void set_sqlite_source_shm_symbols_available_for_testing(const bool available) noexcept
	{
		sqlite_source_shm_symbols_available_for_testing = available;
	}

	bool sqlite_source_shm_map_event_read_lock_valid_for_testing(
		const bool native_cantinit_heap_route,
		const bool native_mapping_nonnull,
		const std::uint32_t read_lock_index)
	{
		static const int underlying_vfs_identity{};
		static const int underlying_vfs_app_data_identity{};
		sqlite_source_shm_open_callback_receipt receipt;
		receipt.pinned_underlying_vfs_identity = &underlying_vfs_identity;
		receipt.pinned_underlying_vfs_app_data_identity =
			&underlying_vfs_app_data_identity;
		sqlite_backend_connection_observation observation;
		sqlite_backend_shm_map_observation event;
		event.caller_extend = 1;
		event.delegated_extend = 0;
		event.native_status = native_cantinit_heap_route ? sqlite_readonly_cantinit
											  : sqlite_readonly;
		event.returned_status = !native_cantinit_heap_route && !native_mapping_nonnull
			? sqlite_readonly_cantinit
			: event.native_status;
		event.native_mapping_nonnull = native_mapping_nonnull;
		event.returned_mapping_nonnull = native_mapping_nonnull;
		event.readonly_family_seen_after = true;
		event.pinned_underlying_vfs_identity = &underlying_vfs_identity;
		event.pinned_underlying_vfs_app_data_identity = &underlying_vfs_app_data_identity;
		observation.shm_map_events.push_back(event);
		auto route = qualified_source_shm_map_route(observation, receipt);
		return route && valid_source_shm_read_lock_route(
							route->used_cantinit_heap_route, read_lock_index);
	}

	bool sqlite_source_shm_authentic_heap_trigger_valid_for_testing(
		const bool mapped_event_precedes_cantinit,
		const int cantinit_page,
		const bool repeat_cantinit)
	{
		static const int underlying_vfs_identity{};
		static const int underlying_vfs_app_data_identity{};
		sqlite_source_shm_open_callback_receipt receipt;
		receipt.pinned_underlying_vfs_identity = &underlying_vfs_identity;
		receipt.pinned_underlying_vfs_app_data_identity =
			&underlying_vfs_app_data_identity;
		sqlite_backend_connection_observation observation;
		auto event = [&](const int page,
						 const int native_status,
						 const bool native_mapping_nonnull,
						 const bool seen_before) {
			sqlite_backend_shm_map_observation value;
			value.page = page;
			value.page_size = 32'768;
			value.caller_extend = 1;
			value.delegated_extend = 0;
			value.native_status = native_status;
			value.returned_status = native_status;
			value.native_mapping_nonnull = native_mapping_nonnull;
			value.returned_mapping_nonnull = native_mapping_nonnull;
			value.readonly_family_seen_before = seen_before;
			value.readonly_family_seen_after = true;
			value.pinned_underlying_vfs_identity = &underlying_vfs_identity;
			value.pinned_underlying_vfs_app_data_identity =
				&underlying_vfs_app_data_identity;
			return value;
		};
		if (mapped_event_precedes_cantinit)
			observation.shm_map_events.push_back(event(0, sqlite_readonly, true, false));
		observation.shm_map_events.push_back(event(
			cantinit_page,
			sqlite_readonly_cantinit,
			false,
			mapped_event_precedes_cantinit));
		if (repeat_cantinit)
			observation.shm_map_events.push_back(
				event(cantinit_page, sqlite_readonly_cantinit, false, true));
		return qualified_source_shm_map_route(observation, receipt).has_value();
	}

	bool sqlite_source_shm_callback_epoch_binding_valid_for_testing(
		const bool same_epoch_identity)
	{
		sqlite_source_shm_open_callback_receipt expected;
		expected.target_namespace_epoch_identity.profile = "test.target-epoch.v1";
		expected.target_namespace_epoch_identity.bytes.push_back(std::byte{1U});
		auto actual = expected;
		if (!same_epoch_identity)
			actual.target_namespace_epoch_identity.bytes.front() = std::byte{2U};
		return same_source_shm_open_callback_receipt(actual, expected);
	}

	bool sqlite_active_wal_header_receipt_required_for_testing(
		const bool initial_observation, const std::uint32_t read_lock_index) noexcept
	{
		return active_wal_header_receipt_required(initial_observation, read_lock_index);
	}

	bool sqlite_active_wal_shm_recheck_transition_valid_for_testing(
		const bool same_object_identity,
		const bool same_entry_identity,
		const std::uint64_t before_byte_count,
		const std::uint64_t after_byte_count) noexcept
	{
		return valid_active_wal_shm_recheck_transition(
			same_object_identity, same_entry_identity, before_byte_count, after_byte_count);
	}

	result<void> observe_sqlite_active_wal_prequalification_for_testing(
		const sqlite_backend_namespace_census& source_census,
		const std::string_view logical_main_locator,
		std::size_t& qualification_call_count)
	{
		auto observed =
			observe_active_wal_prequalification_header(source_census, logical_main_locator);
		if (!observed)
			return unexpected(std::move(observed.error()));
		++qualification_call_count;
		return {};
	}

	result<void> recheck_sqlite_source_shm_epoch_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		const bool transaction_live,
		const bool used_cantinit_heap_route,
		const std::uint32_t read_lock_index)
	{
		return recheck_source_shm_epoch_during_live_read(
			epoch, transaction_live, used_cantinit_heap_route, read_lock_index);
	}

	result<void> finish_sqlite_source_shm_epoch_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		const bool connection_close_confirmed)
	{
		return finish_source_shm_epoch_after_confirmed_close(
			epoch, connection_close_confirmed);
	}

	result<void> validate_sqlite_source_shm_epoch_census_for_testing(
		const std::shared_ptr<sqlite_source_shm_target_namespace_epoch>& epoch,
		const sqlite_backend_namespace_census& source)
	{
		return retained_active_wal_epoch_entries_match_source(epoch, source);
	}

	struct snapshot_store::implementation
	{
		explicit implementation(relation_engine value, std::string backend_value)
			: engine{std::move(value)}, backend{std::move(backend_value)},
			  availability{
				  validated_store_compatibility(backend, sqlite_physical_format::current_v3)}
		{
		}

		relation_engine engine;
		std::string backend;
		sqlite_store_availability_policy availability;
		mutable std::mutex mutex;
		std::map<std::string, publication_record, std::less<>> records;
		std::map<std::string, std::shared_ptr<snapshot_handle::data>, std::less<>> publications;
		std::map<std::string, std::string, std::less<>> heads;
		std::vector<std::weak_ptr<const std::uint64_t>> generation_tokens;
		std::uint64_t generation{};
		std::shared_ptr<sqlite_api> sqlite_runtime;
		std::string sqlite_path;
		std::string sqlite_vfs_name;
		sqlite_physical_format sqlite_format{sqlite_physical_format::current_v3};
		std::optional<sqlite_quiescent_source_anchor> sqlite_source_anchor;
		std::optional<sqlite_wal_source_capture> sqlite_wal_only_capture;
		sqlite_wal_handoff_state sqlite_wal_state{sqlite_wal_handoff_state::ready};
		std::shared_ptr<sqlite_backend_observation_capability> sqlite_observation;
		std::shared_ptr<void> backend_lifetime;
		std::unique_ptr<sqlite_database> database;

		[[nodiscard]] bool result_operations_available() const noexcept
		{
			return availability.result_operations_available();
		}

		void poison_result_operations() noexcept
		{
			availability.poison();
			if (backend == "sqlite")
				sqlite_wal_state = sqlite_wal_handoff_state::poisoned;
		}

		[[nodiscard]] bool begin_recovery_handoff() noexcept
		{
			if (!availability.begin_recovery_handoff())
				return false;
			sqlite_wal_state = sqlite_wal_handoff_state::recovery_handoff_pending;
			return true;
		}

		[[nodiscard]] bool install_independently_validated_state() noexcept
		{
			return availability.install_independently_validated(
				validated_store_compatibility(backend, sqlite_format));
		}

		[[nodiscard]] std::size_t live_generation_count_unlocked() const
		{
			std::set<std::uint64_t> live;
			for (const auto& weak : generation_tokens)
				if (const auto token = weak.lock())
					live.insert(*token);
			return live.size();
		}

		[[nodiscard]] result<void> persist(const snapshot_handle::data& value) const
		{
			if (auto valid = validate_publication_identity(value.publication_record_value); !valid)
				return unexpected(std::move(valid.error()));
			if (database == nullptr)
				return {};
			const auto& record = value.publication_record_value;
			if (sqlite_format == sqlite_physical_format::current_v3)
			{
				if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
					return begun;
				const auto rollback = [&]()
				{
					(void)database->execute("ROLLBACK;");
				};
				if (auto deleted = delete_v3_chunks(
						*database, record.publication_id, record.physical_generation);
					!deleted)
				{
					rollback();
					return deleted;
				}
				auto receipt = insert_streamed_snapshot_chunks(*database,
															   v3_chunk_insert_sql,
															   record.publication_id,
															   record.physical_generation,
															   value);
				if (!receipt)
				{
					rollback();
					return unexpected(std::move(receipt.error()));
				}
				auto update = sqlite_statement::prepare(
					*database,
					"UPDATE cxxlens_ng_publication SET payload_checksum=?1,"
					"payload_byte_count=?2,payload_chunk_count=?3 WHERE publication_id=?4",
					true);
				if (!update)
				{
					rollback();
					return unexpected(std::move(update.error()));
				}
				if (auto bound = update->bind_text(1, receipt->full_checksum); !bound)
				{
					rollback();
					return bound;
				}
				if (auto bound = update->bind_unsigned(2, receipt->byte_count); !bound)
				{
					rollback();
					return bound;
				}
				if (auto bound = update->bind_unsigned(3, receipt->chunk_count); !bound)
				{
					rollback();
					return bound;
				}
				if (auto bound = update->bind_text(4, record.publication_id); !bound)
				{
					rollback();
					return bound;
				}
				if (auto updated = update->expect_done(); !updated)
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
			const auto payload = encode_snapshot(value);
			const auto checksum = content_digest(payload);
			if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
				return begun;
			if (auto inserted =
					insert_v2_publication_row(*database, record, checksum, payload, true);
				!inserted)
			{
				(void)database->execute("ROLLBACK;");
				return inserted;
			}
			if (auto committed = database->execute("COMMIT;"); !committed)
			{
				(void)database->execute("ROLLBACK;");
				return committed;
			}
			return {};
		}

		struct authority_census
		{
			publication_record_map records;
			std::map<std::string, std::shared_ptr<snapshot_handle::data>, std::less<>> publications;
			publication_record_map authority_records;
			std::map<std::string, std::shared_ptr<snapshot_handle::data>, std::less<>>
				authority_publications;
			series_head_map heads;
			std::map<std::string, std::string, std::less<>> snapshot_exports;
			std::vector<std::string> invalid_committed;
			std::uint64_t maximum_generation{};
		};

		[[nodiscard]] static std::vector<std::string>
		ordered_invalid_committed(const authority_census& census)
		{
			auto output = census.invalid_committed;
			std::ranges::sort(output,
							  [&census](const std::string& left, const std::string& right)
							  {
								  const auto& left_record = census.records.at(left);
								  const auto& right_record = census.records.at(right);
								  return std::tie(left_record.sequence,
												  left_record.physical_generation,
												  left_record.publication_id) <
									  std::tie(right_record.sequence,
											   right_record.physical_generation,
											   right_record.publication_id);
							  });
			output.erase(std::unique(output.begin(), output.end()), output.end());
			return output;
		}

		[[nodiscard]] result<authority_census> database_authority_census() const
		{
			if (database == nullptr)
				return unexpected(store_error("store.corrupt", "sqlite", "backend"));
			auto rows = read_sqlite_publications(*database, sqlite_format);
			if (!rows)
				return unexpected(std::move(rows.error()));

			authority_census census;
			std::vector<std::string> validated_committed;
			std::map<std::string, std::vector<std::string>, std::less<>> snapshot_members;
			std::set<std::string, std::less<>> colliding_snapshots;
			for (auto& row : *rows)
			{
				auto record = row.record;
				const auto id = record.publication_id;
				const auto invalid = [&]()
				{
					record.corrupt = true;
					if (!census.records.emplace(id, record).second)
						return false;
					if (record.state == publication_state::committed)
						census.invalid_committed.push_back(id);
					return true;
				};
				bool checksum_valid{};
				if (row.payload_source)
				{
					auto matches = replayable_payload_checksum_matches(
						*row.payload_source, row.payload_byte_count, row.checksum);
					if (!matches && matches.error().code != "store.corrupt")
						return unexpected(std::move(matches.error()));
					checksum_valid = matches && *matches;
				}
				if (!validate_publication_identity(record) || !checksum_valid)
				{
					if (!invalid())
						return unexpected(
							store_error("store.corrupt", id, "duplicate-publication-id"));
					continue;
				}
				auto decoded = decode_snapshot(*row.payload_source, row.payload_byte_count, engine);
				if (!decoded && decoded.error().code != "store.corrupt")
					return unexpected(std::move(decoded.error()));
				if (!decoded || (*decoded)->publication_record_value != record)
				{
					if (!invalid())
						return unexpected(
							store_error("store.corrupt", id, "duplicate-publication-id"));
					continue;
				}
				(*decoded)->physical_backend = backend;
				if (!census.records.emplace(record.publication_id, record).second ||
					!census.publications.emplace(record.publication_id, *decoded).second)
					return unexpected(store_error(
						"store.corrupt", record.publication_id, "duplicate-publication-id"));
				if (record.state != publication_state::committed)
					continue;

				validated_committed.push_back(record.publication_id);
				snapshot_members[record.snapshot_id].push_back(record.publication_id);
				const auto canonical_export = canonical_export_of(**decoded);
				const auto [position, inserted] =
					census.snapshot_exports.emplace(record.snapshot_id, canonical_export);
				if (!inserted && position->second != canonical_export)
					colliding_snapshots.insert(record.snapshot_id);
			}

			if (sqlite_format == sqlite_physical_format::current_v3)
			{
				auto chunks = sqlite_statement::prepare(
					*database,
					"SELECT publication_id,generation FROM cxxlens_ng_payload_chunk "
					"ORDER BY publication_id,generation,chunk_ordinal",
					true);
				if (!chunks)
					return unexpected(std::move(chunks.error()));
				for (;;)
				{
					const auto code = chunks->step();
					if (code == sqlite_done)
						break;
					if (code != sqlite_row)
						return unexpected(
							store_error("store.sqlite-failure", "database", "chunks"));
					auto owner = chunks->column_text(0);
					auto chunk_generation = chunks->column_unsigned(1);
					const auto record = owner ? census.records.find(*owner) : census.records.end();
					if (!owner || !chunk_generation || record == census.records.end() ||
						record->second.physical_generation != *chunk_generation)
						return unexpected(
							store_error("store.corrupt",
										"sqlite-chunk-authority",
										"global-orphan-retired-or-duplicate-committed-generation"));
				}
			}

			for (const auto& snapshot : colliding_snapshots)
			{
				census.snapshot_exports.erase(snapshot);
				for (const auto& id : snapshot_members.at(snapshot))
				{
					census.records.at(id).corrupt = true;
					census.invalid_committed.push_back(id);
				}
			}
			for (const auto& id : validated_committed)
			{
				const auto& record = census.records.at(id);
				if (record.corrupt)
					continue;
				census.authority_records.emplace(id, record);
				census.authority_publications.emplace(id, census.publications.at(id));
				census.maximum_generation =
					std::max(census.maximum_generation, record.physical_generation);
			}
			{
				std::set<std::uint64_t> committed_generations;
				for (const auto& [id, record] : census.authority_records)
				{
					(void)id;
					if (!committed_generations.insert(record.physical_generation).second)
						return unexpected(
							store_error("store.corrupt",
										"sqlite-chunk-authority",
										"global-orphan-retired-or-duplicate-committed-generation"));
				}
			}

			auto derived_heads = derive_diagnostic_heads(census.records);
			if (!derived_heads)
				return unexpected(std::move(derived_heads.error()));
			census.heads = std::move(*derived_heads);
			auto durable_heads = sqlite_statement::prepare(
				*database,
				"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head "
				"ORDER BY series_id",
				sqlite_format == sqlite_physical_format::current_v3);
			if (!durable_heads)
				return unexpected(std::move(durable_heads.error()));
			std::size_t durable_head_count{};
			for (;;)
			{
				const auto code = durable_heads->step();
				if (code == sqlite_done)
					break;
				if (code != sqlite_row)
					return unexpected(
						store_error("store.sqlite-failure", "database", "series-head"));
				++durable_head_count;
				auto series = durable_heads->column_text(0);
				auto publication = durable_heads->column_text(1);
				auto sequence = durable_heads->column_unsigned(2);
				if (!series || !publication || !sequence || !census.heads.contains(*series) ||
					census.heads.at(*series) != *publication)
					return unexpected(store_error("store.corrupt", "sqlite", "series-head"));
				const auto record = census.records.find(*publication);
				if (record == census.records.end() || record->second.sequence != *sequence)
					return unexpected(
						store_error("store.corrupt", "sqlite", "series-head-sequence"));
			}
			if (durable_head_count != census.heads.size())
				return unexpected(store_error("store.corrupt", "sqlite", "series-head-count"));
			return census;
		}

		struct owned_sqlite_authority_state
		{
			sqlite_authority_format format{sqlite_authority_format::current_v3};
			std::uint64_t committed_row_count{};
			sqlite_committed_generation_maximum committed_generation_maximum{};
			std::vector<std::byte> canonical_bytes;

			[[nodiscard]] sqlite_authority_state_view view() const noexcept
			{
				return {
					format, committed_row_count, committed_generation_maximum, canonical_bytes, {}};
			}
		};

		static void append_authority_byte(std::vector<std::byte>& output, const std::uint8_t value)
		{
			output.push_back(static_cast<std::byte>(value));
		}

		static void append_authority_blob(std::vector<std::byte>& output,
										  const std::span<const std::byte> value)
		{
			append_effect_u64(output, static_cast<std::uint64_t>(value.size()));
			output.insert(output.end(), value.begin(), value.end());
		}

		static void append_authority_optional_text(std::vector<std::byte>& output,
												   const std::optional<std::string>& value)
		{
			append_authority_byte(output, value ? 1U : 0U);
			if (value)
				append_effect_bytes(output, *value);
		}

		static void append_authority_record(std::vector<std::byte>& output,
											const publication_record& record)
		{
			append_effect_bytes(output, record.publication_id);
			append_effect_bytes(output, record.series_id);
			append_effect_bytes(output, record.snapshot_id);
			append_effect_u64(output, record.sequence);
			append_effect_u64(output, record.physical_generation);
			append_authority_optional_text(output, record.parent_publication);
			append_authority_byte(output, static_cast<std::uint8_t>(record.state));
			append_authority_byte(output, record.corrupt ? 1U : 0U);
		}

		[[nodiscard]] static result<void>
		append_authority_payload(std::vector<std::byte>& output,
								 const sqlite_replayable_byte_source& source,
								 const std::uint64_t expected_size)
		{
			append_effect_u64(output, expected_size);
			auto pass = source.open_pass();
			if (!pass)
				return unexpected(std::move(pass.error()));
			std::array<std::byte, 64U * 1024U> scratch{};
			std::uint64_t consumed{};
			for (;;)
			{
				auto read = (*pass)->read(scratch);
				if (!read)
					return unexpected(std::move(read.error()));
				if (*read == 0U)
					break;
				std::uint64_t next{};
				if (*read > scratch.size() || !sqlite_checked_add_u64(consumed, *read, next) ||
					next > expected_size)
					return unexpected(
						store_error("store.corrupt", "migration-receipt", "payload-size"));
				output.insert(output.end(), scratch.begin(), scratch.begin() + *read);
				consumed = next;
			}
			if (consumed != expected_size)
				return unexpected(
					store_error("store.corrupt", "migration-receipt", "payload-size"));
			return {};
		}

		[[nodiscard]] result<owned_sqlite_authority_state>
		migration_authority_projection(const sqlite_physical_format format,
									   const authority_census& census,
									   const std::vector<sqlite_persisted_publication>& rows) const
		{
			if (database == nullptr || format != sqlite_format)
				return unexpected(
					store_error("store.corrupt", "migration-receipt", "database-state"));
			try
			{
				owned_sqlite_authority_state output;
				output.format = format == sqlite_physical_format::predecessor_v2
					? sqlite_authority_format::legacy_v2
					: sqlite_authority_format::current_v3;
				output.committed_row_count = census.authority_records.size();
				output.committed_generation_maximum = census.authority_records.empty()
					? sqlite_committed_generation_maximum{sqlite_committed_maximum_tag::none, 0U}
					: sqlite_committed_generation_maximum{sqlite_committed_maximum_tag::some,
														  census.maximum_generation};

				auto& bytes = output.canonical_bytes;
				append_effect_bytes(bytes, "cxxlens.sqlite-authority-state.v1");
				append_authority_byte(bytes, static_cast<std::uint8_t>(output.format));

				auto schema = read_user_schema(*database);
				if (!schema)
					return unexpected(std::move(schema.error()));
				append_effect_u64(bytes, schema->size());
				for (const auto& [name, sql] : *schema)
				{
					append_effect_bytes(bytes, name);
					append_effect_bytes(bytes, sql);
				}

				auto metadata = read_metadata(*database);
				if (!metadata)
					return unexpected(std::move(metadata.error()));
				append_effect_u64(bytes, metadata->size());
				for (const auto& [key, value] : *metadata)
				{
					append_effect_bytes(bytes, key);
					append_effect_bytes(bytes, value);
				}

				append_effect_u64(bytes, rows.size());
				for (const auto& row : rows)
				{
					append_authority_record(bytes, row.record);
					append_effect_bytes(bytes, row.checksum);
					append_effect_u64(bytes, row.payload_byte_count);
					append_effect_u64(bytes, row.payload_chunk_count);
					if (!row.payload_source)
						return unexpected(
							store_error("store.corrupt", "migration-receipt", "payload-source"));
					if (auto appended = append_authority_payload(
							bytes, *row.payload_source, row.payload_byte_count);
						!appended)
						return unexpected(std::move(appended.error()));
				}

				append_effect_u64(bytes, census.records.size());
				for (const auto& [id, record] : census.records)
				{
					append_effect_bytes(bytes, id);
					append_authority_record(bytes, record);
					append_authority_byte(bytes, census.publications.contains(id) ? 1U : 0U);
					append_authority_byte(bytes, census.authority_records.contains(id) ? 1U : 0U);
				}
				auto invalid = ordered_invalid_committed(census);
				append_effect_u64(bytes, invalid.size());
				for (const auto& id : invalid)
					append_effect_bytes(bytes, id);

				auto durable_heads = sqlite_statement::prepare(
					*database,
					"SELECT series_id,current_publication,sequence FROM cxxlens_ng_series_head "
					"ORDER BY series_id",
					format == sqlite_physical_format::current_v3);
				if (!durable_heads)
					return unexpected(std::move(durable_heads.error()));
				append_effect_u64(bytes, census.heads.size());
				std::size_t observed_heads{};
				for (;;)
				{
					const auto code = durable_heads->step();
					if (code == sqlite_done)
						break;
					if (code != sqlite_row)
						return unexpected(store_error(
							"store.sqlite-failure", "database", "migration-receipt-head"));
					auto series = durable_heads->column_text(0);
					auto publication = durable_heads->column_text(1);
					auto sequence = durable_heads->column_unsigned(2);
					if (!series || !publication || !sequence)
						return unexpected(
							store_error("store.corrupt", "migration-receipt", "head"));
					append_effect_bytes(bytes, *series);
					append_effect_bytes(bytes, *publication);
					append_effect_u64(bytes, *sequence);
					++observed_heads;
				}
				if (observed_heads != census.heads.size())
					return unexpected(
						store_error("store.corrupt", "migration-receipt", "head-count"));

				if (format == sqlite_physical_format::current_v3)
				{
					auto chunk_count = sqlite_statement::prepare(
						*database, "SELECT count(*) FROM cxxlens_ng_payload_chunk", true);
					if (!chunk_count || chunk_count->step() != sqlite_row)
						return unexpected(store_error(
							"store.sqlite-failure", "database", "migration-receipt-chunks"));
					auto count = chunk_count->column_unsigned(0);
					if (!count || chunk_count->step() != sqlite_done)
						return unexpected(
							store_error("store.corrupt", "migration-receipt", "chunk-count"));
					append_effect_u64(bytes, *count);

					auto chunks = sqlite_statement::prepare(
						*database,
						"SELECT publication_id,generation,chunk_ordinal,byte_offset,byte_count,"
						"checksum,payload FROM cxxlens_ng_payload_chunk ORDER BY "
						"publication_id,generation,chunk_ordinal",
						true);
					if (!chunks)
						return unexpected(std::move(chunks.error()));
					std::uint64_t observed{};
					for (;;)
					{
						const auto code = chunks->step();
						if (code == sqlite_done)
							break;
						if (code != sqlite_row)
							return unexpected(store_error(
								"store.sqlite-failure", "database", "migration-receipt-chunk"));
						auto owner = chunks->column_text(0);
						auto chunk_generation = chunks->column_unsigned(1);
						auto ordinal = chunks->column_unsigned(2);
						auto offset = chunks->column_unsigned(3);
						auto byte_count = chunks->column_unsigned(4);
						auto checksum = chunks->column_text(5);
						auto payload = chunks->column_blob_view(6);
						if (!owner || !chunk_generation || !ordinal || !offset || !byte_count ||
							!checksum || !payload || payload->size() != *byte_count)
							return unexpected(
								store_error("store.corrupt", "migration-receipt", "chunk"));
						append_effect_bytes(bytes, *owner);
						append_effect_u64(bytes, *chunk_generation);
						append_effect_u64(bytes, *ordinal);
						append_effect_u64(bytes, *offset);
						append_effect_u64(bytes, *byte_count);
						append_effect_bytes(bytes, *checksum);
						append_authority_blob(bytes, *payload);
						++observed;
					}
					if (observed != *count)
						return unexpected(
							store_error("store.corrupt", "migration-receipt", "chunk-count"));
				}
				else
				{
					append_effect_u64(bytes, 0U);
				}

				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(
					store_error("store.sqlite-failure", "migration-receipt", "allocation"));
			}
			catch (const std::length_error&)
			{
				return unexpected(store_error("store.sqlite-failure", "migration-receipt", "size"));
			}
		}

		[[nodiscard]] result<void> install_migration_census(authority_census census)
		{
			try
			{
				for (auto& [id, publication] : census.publications)
				{
					const auto record = census.records.find(id);
					if (record == census.records.end() ||
						record->second.state != publication_state::committed ||
						record->second.corrupt)
						continue;
					auto token =
						std::make_shared<const std::uint64_t>(record->second.physical_generation);
					publication->generation_pin = token;
					generation_tokens.push_back(token);
				}
				records = std::move(census.records);
				publications = std::move(census.publications);
				heads = std::move(census.heads);
				generation = census.maximum_generation;
				if (!install_independently_validated_state())
					return unexpected(sqlite_reopen_required());
				return {};
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(
					store_error("store.sqlite-failure", "migration-recovery", "allocation"));
			}
		}

		[[nodiscard]] result<void> recover_wal_only_for_mutation()
		{
			if (!result_operations_available())
				return unexpected(sqlite_reopen_required());
			if (backend != "sqlite" || sqlite_wal_state == sqlite_wal_handoff_state::ready)
				return {};
			if (sqlite_wal_state != sqlite_wal_handoff_state::wal_only_deferred ||
				!sqlite_runtime || !sqlite_observation || !sqlite_source_anchor ||
				!sqlite_wal_only_capture || database != nullptr || sqlite_vfs_name.empty())
			{
				poison_result_operations();
				return unexpected(sqlite_reopen_required());
			}

			auto& anchor = *sqlite_source_anchor;
			auto& capture = *sqlite_wal_only_capture;
			if (auto stable =
					recheck_wal_only_source(anchor, capture, sqlite_path, *sqlite_observation);
				!stable)
				return unexpected(std::move(stable.error()));
			if (auto symbols = require_v3_symbols(*sqlite_runtime); !symbols)
				return symbols;

			auto effect = begin_effect_gated_connection(sqlite_observation, sqlite_path);
			if (!effect)
				return unexpected(std::move(effect.error()));
			auto anchor_pin = sqlite_wal_only_authority_pin(anchor, capture);
			if (!anchor_pin)
				return unexpected(sqlite_effect_gate_failure());
			auto opened = open_database(
				sqlite_runtime,
				sqlite_path,
				sqlite_vfs_name.c_str(),
				sqlite_open_readwrite | sqlite_open_privatecache | sqlite_open_fullmutex,
				{sqlite_runtime, backend_lifetime, sqlite_observation, std::move(anchor_pin)});
			if (!opened)
				return unexpected(std::move(opened.error()));
			database = std::move(*opened);
			bool coordination_effect_authorized{};
			bool transaction_active{};
			const auto fail = [&](error failure) -> result<void>
			{
				if (transaction_active)
					(void)database->execute("ROLLBACK;");
				transaction_active = false;
				auto closed = database->close_exactly_once();
				database.reset();
				if (coordination_effect_authorized || !confirmed_connection_close(closed))
				{
					poison_result_operations();
					return unexpected(sqlite_recovery_handoff_opaque());
				}
				return unexpected(std::move(failure));
			};

			if (auto valid =
					validate_read_write_open_observation(*effect->observation, *sqlite_observation);
				!valid)
				return fail(std::move(valid.error()));
			if (auto valid = validate_v3_connection(*database); !valid)
				return fail(std::move(valid.error()));
			if (auto stable =
					recheck_wal_only_source(anchor, capture, sqlite_path, *sqlite_observation);
				!stable)
				return fail(std::move(stable.error()));

			auto coordination_request = make_effect_arm_request(
				*effect,
				sqlite_observation,
				sqlite_path,
				"cxxlens.sqlite-effect-prerequisite.wal-only-recovery.coordination.v1",
				sqlite_backend_effect_stage::wal_shm_coordination_only,
				&anchor,
				false,
				false,
				&capture);
			if (!coordination_request)
				return fail(std::move(coordination_request.error()));
			if (!begin_recovery_handoff())
			{
				poison_result_operations();
				return unexpected(sqlite_reopen_required());
			}
			coordination_effect_authorized = true;
			auto coordination = arm_effect_gate_now(*effect, std::move(*coordination_request));
			if (!coordination)
				return fail(std::move(coordination.error()));
			if (auto synchronous = set_and_require_full_synchronous(*database, true); !synchronous)
				return fail(std::move(synchronous.error()));
			auto fully_armed_request = make_post_wal_coordination_arm_request(
				*effect,
				sqlite_observation,
				sqlite_path,
				"cxxlens.sqlite-effect-prerequisite.wal-only-recovery.post-coordination.v1",
				anchor,
				*coordination,
				&capture);
			if (!fully_armed_request)
				return fail(std::move(fully_armed_request.error()));
			if (auto armed = arm_effect_gate_now(*effect, std::move(*fully_armed_request)); !armed)
				return fail(std::move(armed.error()));
			if (auto wal = require_wal_mode(*database, true); !wal)
				return fail(std::move(wal.error()));
			if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
				return fail(std::move(begun.error()));
			transaction_active = true;
			auto classification = classify_sqlite_database(*database);
			if (!classification)
				return fail(std::move(classification.error()));
			if (!*classification || **classification != sqlite_format)
				return fail(sqlite_wal_only_source_changed());
			auto recovered = database_authority_census();
			if (!recovered)
				return fail(std::move(recovered.error()));
			const auto exact_eager_state = [&]()
			{
				if (recovered->records != records || recovered->heads != heads ||
					recovered->maximum_generation != generation ||
					recovered->publications.size() != publications.size())
					return false;
				for (const auto& [id, eager] : publications)
				{
					const auto current = recovered->publications.find(id);
					if (current == recovered->publications.end() ||
						current->second->publication_record_value !=
							eager->publication_record_value ||
						canonical_export_of(*current->second) != canonical_export_of(*eager))
						return false;
				}
				return true;
			}();
			if (!exact_eager_state)
				return fail(sqlite_wal_only_source_changed());
			if (auto rolled_back = database->execute("ROLLBACK;"); !rolled_back)
				return fail(std::move(rolled_back.error()));
			transaction_active = false;
			if (auto checkpointed = checkpoint_wal_for_recovery(*database); !checkpointed)
				return fail(std::move(checkpointed.error()));

			auto closed = database->close_exactly_once();
			const auto close_fault =
				dispatch_sqlite_store_fault({sqlite_store_operation::wal_recovery_handoff,
											 sqlite_store_fault_boundary::connection_close,
											 sqlite_store_fault_timing::after,
											 1U,
											 1U});
			database.reset();
			if (close_fault.issued || !confirmed_connection_close(closed))
			{
				poison_result_operations();
				return unexpected(sqlite_recovery_handoff_opaque());
			}
			auto recovered_anchor =
				capture_recovered_source_anchor(anchor, sqlite_path, *sqlite_observation);
			if (!recovered_anchor)
			{
				poison_result_operations();
				return unexpected(sqlite_recovery_handoff_opaque());
			}
			sqlite_source_anchor = std::move(*recovered_anchor);
			if (sqlite_format == sqlite_physical_format::current_v3)
			{
				auto writable = open_current_observed_database(sqlite_runtime,
															   sqlite_path,
															   sqlite_vfs_name,
															   backend_lifetime,
															   sqlite_observation,
															   *sqlite_source_anchor);
				if (!writable)
				{
					poison_result_operations();
					return unexpected(sqlite_recovery_handoff_opaque());
				}
				database = std::move(*writable);
			}
			sqlite_wal_only_capture.reset();
			if (!install_independently_validated_state())
			{
				poison_result_operations();
				return unexpected(sqlite_recovery_handoff_opaque());
			}
			sqlite_wal_state = sqlite_wal_handoff_state::ready;
			return {};
		}

		[[nodiscard]] result<void> migrate_predecessor_v2()
		{
			if (sqlite_runtime == nullptr || !sqlite_observation || !sqlite_source_anchor ||
				sqlite_format != sqlite_physical_format::predecessor_v2 ||
				(database == nullptr && sqlite_source_anchor->namespace_census.profile.empty()))
				return unexpected(
					store_error("store.corrupt", "sqlite-physical-format", "migration-source"));
			std::vector<const publication_record*> preflight_invalid;
			for (const auto& [id, record] : records)
			{
				(void)id;
				if (record.state == publication_state::committed &&
					(record.corrupt || !validate_publication_identity(record)))
					preflight_invalid.push_back(&record);
			}
			std::ranges::sort(preflight_invalid,
							  [](const publication_record* left, const publication_record* right)
							  {
								  return std::tie(left->sequence,
												  left->physical_generation,
												  left->publication_id) <
									  std::tie(right->sequence,
											   right->physical_generation,
											   right->publication_id);
							  });
			if (!preflight_invalid.empty())
				return unexpected(store_error("store.compact-validation-failed",
											  preflight_invalid.front()->publication_id));
			if (auto symbols = require_v3_symbols(*sqlite_runtime); !symbols)
				return symbols;
			if (auto stable = revalidate_stored_quiescent_source(
					*sqlite_source_anchor, sqlite_path, *sqlite_observation);
				!stable)
				return stable;

			auto effect = begin_effect_gated_connection(sqlite_observation, sqlite_path);
			if (!effect)
				return unexpected(std::move(effect.error()));
			auto anchor_pin = sqlite_authority_anchor_pin(*sqlite_source_anchor);
			if (!anchor_pin)
				return unexpected(sqlite_effect_gate_failure());
			const auto* vfs_name = sqlite_vfs_name.empty() ? nullptr : sqlite_vfs_name.c_str();
			auto writable = open_database(
				sqlite_runtime,
				sqlite_path,
				vfs_name,
				sqlite_open_readwrite | sqlite_open_privatecache | sqlite_open_fullmutex,
				{sqlite_runtime, backend_lifetime, sqlite_observation, std::move(anchor_pin)});
			if (!writable)
				return unexpected(std::move(writable.error()));
			if (auto valid =
					validate_read_write_open_observation(*effect->observation, *sqlite_observation);
				!valid)
				return valid;
			if (auto valid = validate_v3_connection(**writable); !valid)
				return valid;
			auto coordination_request = make_effect_arm_request(
				*effect,
				sqlite_observation,
				sqlite_path,
				"cxxlens.sqlite-effect-prerequisite.migration-v2-to-v3.wal-coordination.v1",
				sqlite_backend_effect_stage::wal_shm_coordination_only,
				&*sqlite_source_anchor,
				false,
				false);
			if (!coordination_request)
				return unexpected(std::move(coordination_request.error()));
			auto coordination = arm_effect_gate_now(*effect, std::move(*coordination_request));
			if (!coordination)
				return unexpected(std::move(coordination.error()));
			if (auto synchronous = set_and_require_full_synchronous(**writable, true); !synchronous)
				return synchronous;
			auto request = make_post_wal_coordination_arm_request(
				*effect,
				sqlite_observation,
				sqlite_path,
				"cxxlens.sqlite-effect-prerequisite.migration-v2-to-v3.post-wal-coordination.v1",
				*sqlite_source_anchor,
				*coordination);
			if (!request)
				return unexpected(std::move(request.error()));
			if (auto armed = arm_effect_gate_now(*effect, std::move(*request)); !armed)
				return unexpected(std::move(armed.error()));
			if (auto wal = require_wal_mode(**writable, true); !wal)
				return wal;

			auto predecessor_connection = std::move(database);
			database = std::move(*writable);
			const auto source_anchor = *sqlite_source_anchor;
			bool transaction_active{};
			bool rollback_attempted{};
			std::optional<owned_sqlite_authority_state> source_projection;
			std::optional<owned_sqlite_authority_state> candidate_projection;
			const auto injected_fault = []
			{
				return store_error("store.sqlite-failure", "migration-fault", "injected");
			};
			const auto interpret_fault =
				[&](const sqlite_store_fault_directive& directive) -> std::optional<error>
			{
				if (!directive.issued)
					return std::nullopt;
				if (directive.action == sqlite_store_fault_action::request_process_crash)
					std::_Exit(86);
				return injected_fault();
			};
			const auto dispatch_fault = [&](const sqlite_store_fault_boundary boundary,
											const sqlite_store_fault_timing timing,
											const std::uint64_t ordinal = 1U,
											const std::uint64_t total = 1U) -> std::optional<error>
			{
				return interpret_fault(
					dispatch_sqlite_store_fault({sqlite_store_operation::migrate_predecessor,
												 boundary,
												 timing,
												 ordinal,
												 total}));
			};
			const auto conclude = [&](error failure,
									  const sqlite_terminal_phase requested_phase,
									  sqlite_terminal_cause cause) -> result<void>
			{
				auto phase = requested_phase;
				if (transaction_active && !rollback_attempted)
				{
					rollback_attempted = true;
					auto before = dispatch_fault(sqlite_store_fault_boundary::transaction_rollback,
												 sqlite_store_fault_timing::before);
					auto rolled_back = database != nullptr
						? database->execute("ROLLBACK;")
						: result<void>{unexpected(store_error(
							  "store.sqlite-failure", "database", "rollback-connection"))};
					auto after = dispatch_fault(sqlite_store_fault_boundary::transaction_rollback,
												sqlite_store_fault_timing::after);
					if (!rolled_back || before || after)
						cause = sqlite_terminal_cause::rollback_uncertain;
					transaction_active = false;
				}

				const auto close_before =
					dispatch_fault(sqlite_store_fault_boundary::connection_close,
								   sqlite_store_fault_timing::before);
				bool writer_close_ok{};
				if (database != nullptr)
					writer_close_ok = confirmed_connection_close(database->close_exactly_once());
				database.reset();
				const auto close_after =
					dispatch_fault(sqlite_store_fault_boundary::connection_close,
								   sqlite_store_fault_timing::after);
				bool predecessor_close_ok = true;
				if (predecessor_connection)
				{
					predecessor_close_ok =
						confirmed_connection_close(predecessor_connection->close_exactly_once());
					predecessor_connection.reset();
				}
				if (close_before || close_after || !writer_close_ok || !predecessor_close_ok)
				{
					poison_result_operations();
					const auto resolution =
						resolve_sqlite_terminal({sqlite_store_operation::migrate_predecessor,
												 phase,
												 sqlite_terminal_cause::close_non_ok_or_unknown,
												 sqlite_terminal_class::not_classified});
					auto mapped =
						map_sqlite_terminal_public_error(resolution.public_result, failure);
					return unexpected(mapped ? std::move(*mapped) : std::move(failure));
				}
				if (!source_projection || !sqlite_runtime || !sqlite_observation)
				{
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}

				if (auto fault =
						dispatch_fault(sqlite_store_fault_boundary::terminal_namespace_census,
									   sqlite_store_fault_timing::before);
					fault)
				{
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}
				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::terminal_reopen,
												sqlite_store_fault_timing::before);
					fault)
				{
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}
				auto reopened = open_observed_store_database(sqlite_runtime,
															 sqlite_path,
															 sqlite_vfs_name,
															 backend_lifetime,
															 sqlite_observation);
				const auto reopen_after = dispatch_fault(
					sqlite_store_fault_boundary::terminal_reopen, sqlite_store_fault_timing::after);
				const auto census_after =
					dispatch_fault(sqlite_store_fault_boundary::terminal_namespace_census,
								   sqlite_store_fault_timing::after);
				if (!reopened || reopen_after || census_after || !reopened->source_anchor ||
					!reopened->database || !reopened->private_read_transaction ||
					!same_main_receipt(source_anchor.namespace_census,
									   reopened->source_anchor->namespace_census))
				{
					if (reopened && reopened->database)
					{
						(void)reopened->database->close_exactly_once();
						reopened->database.reset();
					}
					poison_result_operations();
					return unexpected(store_error("store.sqlite-failure",
												  "migration-recovery",
												  reopened && reopened->source_anchor
													  ? "concurrent-authority-change"
													  : "opaque"));
				}

				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::terminal_validation,
												sqlite_store_fault_timing::before);
					fault)
				{
					(void)finish_private_read(*reopened, sqlite_path, *sqlite_observation, false);
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}

				sqlite_format = reopened->format;
				database = std::move(reopened->database);
				auto terminal_rows = read_sqlite_publications(*database, sqlite_format);
				auto terminal_census = terminal_rows
					? database_authority_census()
					: result<authority_census>{unexpected(terminal_rows.error())};
				auto terminal_projection = terminal_census
					? migration_authority_projection(
						  sqlite_format, *terminal_census, *terminal_rows)
					: result<owned_sqlite_authority_state>{unexpected(terminal_census.error())};
				reopened->database = std::move(database);
				const auto validation_after =
					dispatch_fault(sqlite_store_fault_boundary::terminal_validation,
								   sqlite_store_fault_timing::after);
				if (!terminal_projection || validation_after)
				{
					(void)finish_private_read(*reopened, sqlite_path, *sqlite_observation, false);
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}

				const auto accept_candidate =
					[](const sqlite_descendant_candidate_witness&, const void*) noexcept
				{
					return sqlite_candidate_gate_result::accepted;
				};
				const auto accepted = candidate_projection.has_value()
					? sqlite_projection_gate::accepted
					: sqlite_projection_gate::rejected;
				const sqlite_descendant_validation descendant{
					accepted, accepted, accepted, accepted, accepted, accept_candidate, nullptr};
				const auto source_view = source_projection->view();
				const auto candidate_view = candidate_projection
					? std::optional<sqlite_authority_state_view>{candidate_projection->view()}
					: std::nullopt;
				const auto terminal_view = terminal_projection->view();
				const auto classified = reclassify_sqlite_terminal(
					{sqlite_store_operation::migrate_predecessor,
					 sqlite_main_identity_class::same,
					 source_view,
					 candidate_view,
					 {sqlite_terminal_observation_kind::valid_authority_state, terminal_view},
					 descendant,
					 0U});

				if (auto finished =
						finish_private_read(*reopened, sqlite_path, *sqlite_observation, true);
					!finished)
				{
					poison_result_operations();
					return unexpected(
						store_error("store.sqlite-failure", "migration-recovery", "opaque"));
				}
				if (sqlite_format == sqlite_physical_format::predecessor_v2)
				{
					auto closed_anchor = capture_recovered_source_anchor(
						source_anchor, sqlite_path, *sqlite_observation);
					if (!closed_anchor)
					{
						poison_result_operations();
						return unexpected(
							store_error("store.sqlite-failure", "migration-recovery", "opaque"));
					}
					sqlite_source_anchor = std::move(*closed_anchor);
				}
				else
				{
					sqlite_source_anchor = *reopened->source_anchor;
				}
				sqlite_wal_only_capture.reset();
				sqlite_wal_state = sqlite_wal_handoff_state::ready;
				if (reopened->wal_only_capture)
				{
					sqlite_wal_only_capture = std::move(reopened->wal_only_capture);
					sqlite_wal_state = sqlite_wal_handoff_state::wal_only_deferred;
				}
				else if (sqlite_format == sqlite_physical_format::current_v3)
				{
					auto writable_reopen = open_current_observed_database(sqlite_runtime,
																		  sqlite_path,
																		  sqlite_vfs_name,
																		  backend_lifetime,
																		  sqlite_observation,
																		  *sqlite_source_anchor);
					if (!writable_reopen)
						cause = sqlite_terminal_cause::reopen_failure;
					else
						database = std::move(*writable_reopen);
				}

				const auto resolution =
					resolve_sqlite_terminal({sqlite_store_operation::migrate_predecessor,
											 phase,
											 cause,
											 classified.terminal_class});
				if (resolution.state_effect ==
					sqlite_terminal_state_effect::poison_result_operations)
				{
					if (database)
					{
						(void)database->close_exactly_once();
						database.reset();
					}
					poison_result_operations();
				}
				else if (resolution.state_effect ==
						 sqlite_terminal_state_effect::install_authorized_state)
				{
					if (auto installed = install_migration_census(std::move(*terminal_census));
						!installed)
					{
						poison_result_operations();
						return unexpected(
							store_error("store.sqlite-failure", "migration-recovery", "opaque"));
					}
				}
				auto mapped = map_sqlite_terminal_public_error(resolution.public_result, failure);
				if (mapped)
					return unexpected(std::move(*mapped));
				return {};
			};
			const auto fail = [&](error failure,
								  const sqlite_terminal_phase phase =
									  sqlite_terminal_phase::precommit,
								  const sqlite_terminal_cause cause =
									  sqlite_terminal_cause::triggering_failure) -> result<void>
			{
				return conclude(std::move(failure), phase, cause);
			};

			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::transaction_begin,
											sqlite_store_fault_timing::before);
				fault)
				return fail(std::move(*fault));
			if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
				return fail(std::move(begun.error()));
			transaction_active = true;
			auto classified = classify_sqlite_database(*database);
			if (!classified)
				return fail(std::move(classified.error()));
			if (!*classified || **classified != sqlite_physical_format::predecessor_v2)
				return fail(store_error(
					"store.sqlite-failure", "migration-source", "concurrent-authority-change"));

			auto source_rows =
				read_sqlite_publications(*database, sqlite_physical_format::predecessor_v2);
			if (!source_rows)
				return fail(std::move(source_rows.error()));
			auto source_census = database_authority_census();
			if (!source_census)
			{
				auto failure = std::move(source_census.error());
				if (failure.code == "store.corrupt" || failure.code == "store.current-ambiguous")
					return fail(store_error(
						"store.compact-validation-failed", failure.field, failure.detail));
				return fail(std::move(failure));
			}
			const auto source_invalid = ordered_invalid_committed(*source_census);
			if (!source_invalid.empty())
				return fail(store_error("store.compact-validation-failed", source_invalid.front()));
			auto sealed_source = migration_authority_projection(
				sqlite_physical_format::predecessor_v2, *source_census, *source_rows);
			if (!sealed_source)
				return fail(std::move(sealed_source.error()));
			source_projection.emplace(std::move(*sealed_source));
			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::transaction_begin,
											sqlite_store_fault_timing::after);
				fault)
				return fail(std::move(*fault));

			using replacement_entry =
				std::pair<std::string, std::shared_ptr<snapshot_handle::data>>;
			std::vector<replacement_entry> replacements;
			for (const auto& [id, publication] : source_census->authority_publications)
				replacements.emplace_back(id,
										  std::make_shared<snapshot_handle::data>(*publication));
			std::ranges::sort(replacements,
							  [](const replacement_entry& left, const replacement_entry& right)
							  {
								  const auto& left_record = left.second->publication_record_value;
								  const auto& right_record = right.second->publication_record_value;
								  return std::tie(left_record.sequence,
												  left_record.physical_generation,
												  left_record.publication_id) <
									  std::tie(right_record.sequence,
											   right_record.physical_generation,
											   right_record.publication_id);
							  });

			auto next_generation = source_census->maximum_generation;
			for (auto& [id, replacement] : replacements)
			{
				auto source = std::ranges::find(*source_rows,
												id,
												[](const auto& row)
												{
													return row.record.publication_id;
												});
				if (source == source_rows->end() || !source->payload_source)
					return fail(store_error("store.compact-validation-failed", id));
				const auto source_generation = source->record.physical_generation;
				auto allocated = checked_counter_increment(next_generation, "physical_generation");
				if (!allocated)
					return fail(std::move(allocated.error()));
				next_generation = *allocated;
				replacement->publication_record_value.physical_generation = next_generation;
				auto rewritten =
					sqlite_generation_rewrite_payload_source::create(source->payload_source,
																	 source->payload_byte_count,
																	 source_generation,
																	 next_generation);
				if (!rewritten)
					return fail(store_error("store.compact-validation-failed", id));
				source->payload_source = std::move(*rewritten);
				source->record.physical_generation = next_generation;
			}

			constexpr std::array metadata{
				std::pair{"physical_format_version", "3.0.0"},
				std::pair{"payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"},
				std::pair{"payload_chunk_maximum_bytes", "8388608"},
			};
			const std::map<std::string, std::string, std::less<>> expected_non_marker_metadata{
				{"payload_chunk_maximum_bytes", "8388608"},
				{"payload_chunk_profile", "cxxlens.sqlite-payload-chunks.v1"},
				{"physical_format_version", "3.0.0"},
			};
			struct migration_head
			{
				std::string series_id;
				std::string publication_id;
				std::uint64_t sequence{};
				[[nodiscard]] bool operator==(const migration_head&) const = default;
			};
			std::vector<migration_head> source_heads;
			for (const auto& [series, publication_id] : source_census->heads)
			{
				const auto record = source_census->records.find(publication_id);
				if (record == source_census->records.end())
					return fail(store_error("store.compact-validation-failed", publication_id));
				source_heads.push_back({series, publication_id, record->second.sequence});
			}
			std::uint64_t metadata_fault_ordinal{};
			constexpr auto metadata_fault_total = static_cast<std::uint64_t>(metadata.size() * 2U);
			std::uint64_t payload_fault_ordinal{};
			std::uint64_t payload_fault_total{};
			for (const auto& row : *source_rows)
			{
				const auto chunks = row.payload_byte_count == 0U
					? 0U
					: 1U + ((row.payload_byte_count - 1U) / sqlite_payload_chunk_maximum);
				if (chunks > (std::numeric_limits<std::uint64_t>::max() - payload_fault_total) / 2U)
					return fail(
						store_error("store.counter-overflow", "migration-payload-chunk-count"));
				payload_fault_total += chunks * 2U;
			}

			const auto insert_metadata_rows = [&](const std::string_view sql) -> result<void>
			{
				auto insert = sqlite_statement::prepare(*database, sql, true);
				if (!insert)
					return unexpected(std::move(insert.error()));
				for (const auto& [key, value] : metadata)
				{
					++metadata_fault_ordinal;
					if (auto fault = dispatch_fault(sqlite_store_fault_boundary::metadata_row,
													sqlite_store_fault_timing::before,
													metadata_fault_ordinal,
													metadata_fault_total);
						fault)
						return unexpected(std::move(*fault));
					if (auto bound = insert->bind_text(1, key); !bound)
						return bound;
					if (auto bound = insert->bind_text(2, value); !bound)
						return bound;
					if (auto inserted = insert->expect_done(); !inserted)
						return inserted;
					if (auto fault = dispatch_fault(sqlite_store_fault_boundary::metadata_row,
													sqlite_store_fault_timing::after,
													metadata_fault_ordinal,
													metadata_fault_total);
						fault)
						return unexpected(std::move(*fault));
					if (auto reset = insert->reset(); !reset)
						return reset;
				}
				return {};
			};
			const auto insert_rows = [&](std::vector<sqlite_persisted_publication>& rows,
										 const bool shadow) -> result<void>
			{
				for (auto& row : rows)
				{
					if (!row.payload_source)
						return unexpected(store_error("store.compact-validation-failed",
													  row.record.publication_id));
					auto receipt = insert_replayable_payload_chunks(
						*database,
						shadow ? migration_chunk_insert_sql : v3_chunk_insert_sql,
						row.record.publication_id,
						row.record.physical_generation,
						*row.payload_source,
						row.payload_byte_count,
						&payload_fault_ordinal,
						payload_fault_total);
					if (!receipt)
						return unexpected(std::move(receipt.error()));
					if (row.record.state == publication_state::committed)
						row.checksum = receipt->full_checksum;
					row.payload_byte_count = receipt->byte_count;
					row.payload_chunk_count = receipt->chunk_count;
					if (auto inserted = insert_publication_row(
							*database,
							shadow ? migration_publication_insert_sql : v3_publication_insert_sql,
							row.record,
							row.checksum,
							row.payload_byte_count,
							row.payload_chunk_count);
						!inserted)
						return inserted;
				}
				return {};
			};
			const auto insert_heads =
				[&](const std::string_view sql,
					const std::vector<migration_head>& head_rows) -> result<void>
			{
				auto insert = sqlite_statement::prepare(*database, sql, true);
				if (!insert)
					return unexpected(std::move(insert.error()));
				for (const auto& head : head_rows)
				{
					if (auto bound = insert->bind_text(1, head.series_id); !bound)
						return bound;
					if (auto bound = insert->bind_text(2, head.publication_id); !bound)
						return bound;
					if (auto bound = insert->bind_unsigned(3, head.sequence); !bound)
						return bound;
					if (auto inserted = insert->expect_done(); !inserted)
						return inserted;
					if (auto reset = insert->reset(); !reset)
						return reset;
				}
				return {};
			};
			const auto read_heads =
				[&](const std::string_view sql) -> result<std::vector<migration_head>>
			{
				auto selected = sqlite_statement::prepare(*database, sql, true);
				if (!selected)
					return unexpected(std::move(selected.error()));
				std::vector<migration_head> output;
				for (;;)
				{
					const auto code = selected->step();
					if (code == sqlite_done)
						break;
					if (code != sqlite_row)
						return unexpected(
							store_error("store.sqlite-failure", "database", "migration-head"));
					auto series = selected->column_text(0);
					auto publication = selected->column_text(1);
					auto sequence = selected->column_unsigned(2);
					if (!series || !publication || !sequence)
						return unexpected(
							store_error("store.corrupt", "migration-head", "storage-class"));
					output.push_back({std::move(*series), std::move(*publication), *sequence});
				}
				return output;
			};
			const auto read_key_values = [&](const std::string_view sql)
				-> result<std::map<std::string, std::string, std::less<>>>
			{
				auto selected = sqlite_statement::prepare(*database, sql, true);
				if (!selected)
					return unexpected(std::move(selected.error()));
				std::map<std::string, std::string, std::less<>> output;
				for (;;)
				{
					const auto code = selected->step();
					if (code == sqlite_done)
						break;
					if (code != sqlite_row)
						return unexpected(
							store_error("store.sqlite-failure", "database", "migration-metadata"));
					auto key = selected->column_text(0);
					auto value = selected->column_text(1);
					if (!key || !value ||
						!output.emplace(std::move(*key), std::move(*value)).second)
						return unexpected(
							store_error("store.corrupt", "migration-metadata", "row"));
				}
				return output;
			};
			const std::vector<sqlite_persisted_publication>* expected_rows = &*source_rows;
			const auto validate_row_projection =
				[&](const std::vector<sqlite_persisted_publication>& actual) -> result<void>
			{
				if (actual.size() != expected_rows->size())
					return unexpected(
						store_error("store.corrupt", "migration-target", "row-count"));
				for (std::size_t index = 0U; index < actual.size(); ++index)
				{
					const auto& expected = (*expected_rows)[index];
					const auto& observed = actual[index];
					if (observed.record != expected.record ||
						observed.checksum != expected.checksum ||
						observed.payload_byte_count != expected.payload_byte_count ||
						observed.payload_chunk_count != expected.payload_chunk_count ||
						!observed.payload_source || !expected.payload_source)
						return unexpected(store_error("store.corrupt",
													  observed.record.publication_id,
													  "migration-projection"));
					auto equal = replayable_payloads_equal(*expected.payload_source,
														   *observed.payload_source,
														   expected.payload_byte_count);
					if (!equal || !*equal)
						return unexpected(equal ? store_error("store.corrupt",
															  observed.record.publication_id,
															  "migration-projection")
												: std::move(equal.error()));
					if (observed.record.state != publication_state::committed)
						continue;
					auto checksum_matches = replayable_payload_checksum_matches(
						*observed.payload_source, observed.payload_byte_count, observed.checksum);
					if (!checksum_matches || !*checksum_matches)
						return unexpected(checksum_matches
											  ? store_error("store.corrupt",
															observed.record.publication_id,
															"migration-checksum")
											  : std::move(checksum_matches.error()));
					auto decoded = decode_snapshot(
						*observed.payload_source, observed.payload_byte_count, engine);
					const auto source =
						source_census->authority_publications.find(observed.record.publication_id);
					if (!decoded || (*decoded)->publication_record_value != observed.record ||
						source == source_census->authority_publications.end() ||
						canonical_export_of(**decoded) != canonical_export_of(*source->second))
						return unexpected(store_error(
							"store.corrupt", observed.record.publication_id, "migration-semantic"));
				}
				return {};
			};
			std::uint64_t ddl_fault_ordinal{};
			constexpr auto ddl_fault_total = static_cast<std::uint64_t>(
				sqlite_migration_shadow_ddl.size() + sqlite_v3_ddl.size() + 4U + 6U);
			const auto execute_migration_ddl = [&](const std::string_view sql) -> result<void>
			{
				++ddl_fault_ordinal;
				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::ddl_object,
												sqlite_store_fault_timing::before,
												ddl_fault_ordinal,
												ddl_fault_total);
					fault)
					return unexpected(std::move(*fault));
				if (auto executed = database->execute(std::string{sql}); !executed)
					return executed;
				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::ddl_object,
												sqlite_store_fault_timing::after,
												ddl_fault_ordinal,
												ddl_fault_total);
					fault)
					return unexpected(std::move(*fault));
				return {};
			};

			for (const auto statement : sqlite_migration_shadow_ddl)
				if (auto created = execute_migration_ddl(std::string{statement} + ';'); !created)
					return fail(std::move(created.error()));
			if (auto inserted = insert_metadata_rows(
					"INSERT INTO cxxlens_ng_migration_metadata(key,value) VALUES(?1,?2)");
				!inserted)
				return fail(std::move(inserted.error()));
			if (auto inserted = insert_rows(*source_rows, true); !inserted)
				return fail(std::move(inserted.error()));
			if (auto inserted = insert_heads(
					"INSERT INTO cxxlens_ng_migration_series_head(series_id,current_publication,"
					"sequence) VALUES(?1,?2,?3)",
					source_heads);
				!inserted)
				return fail(std::move(inserted.error()));

			auto shadow_schema = read_user_schema(*database);
			if (!shadow_schema || shadow_schema->size() != 10U ||
				!schema_includes(*shadow_schema, sqlite_v2_ddl) ||
				!schema_includes(*shadow_schema, sqlite_migration_shadow_ddl))
				return fail(shadow_schema
								? store_error("store.corrupt", "migration-shadow", "schema")
								: std::move(shadow_schema.error()));
			auto shadow_profile = migration_shadow_profile_matches(*database);
			if (!shadow_profile || !*shadow_profile)
				return fail(shadow_profile
								? store_error("store.corrupt", "migration-shadow", "profile")
								: std::move(shadow_profile.error()));
			auto shadow_metadata =
				read_key_values("SELECT key,value FROM cxxlens_ng_migration_metadata ORDER BY key");
			if (!shadow_metadata || *shadow_metadata != expected_non_marker_metadata)
				return fail(shadow_metadata
								? store_error("store.corrupt", "migration-shadow", "metadata")
								: std::move(shadow_metadata.error()));
			auto shadow_rows = read_migration_shadow_publications(*database);
			if (!shadow_rows)
				return fail(std::move(shadow_rows.error()));
			if (auto valid = validate_row_projection(*shadow_rows); !valid)
				return fail(std::move(valid.error()));
			expected_rows = &*shadow_rows;
			auto shadow_heads = read_heads("SELECT series_id,current_publication,sequence FROM "
										   "cxxlens_ng_migration_series_head ORDER BY series_id");
			if (!shadow_heads || *shadow_heads != source_heads)
				return fail(shadow_heads ? store_error("store.corrupt", "migration-shadow", "heads")
										 : std::move(shadow_heads.error()));

			constexpr std::array drop_v2{
				"DROP INDEX cxxlens_ng_publication_series;",
				"DROP TABLE cxxlens_ng_series_head;",
				"DROP TABLE cxxlens_ng_publication;",
				"DROP TABLE cxxlens_ng_metadata;",
			};
			for (const auto statement : drop_v2)
				if (auto dropped = execute_migration_ddl(statement); !dropped)
					return fail(std::move(dropped.error()));
			for (const auto statement : sqlite_v3_ddl)
				if (auto created = execute_migration_ddl(std::string{statement} + ';'); !created)
					return fail(std::move(created.error()));
			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::final_object_copy,
											sqlite_store_fault_timing::before);
				fault)
				return fail(std::move(*fault));
			if (auto inserted = insert_metadata_rows(
					"INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?1,?2)");
				!inserted)
				return fail(std::move(inserted.error()));
			if (auto inserted = insert_rows(*shadow_rows, false); !inserted)
				return fail(std::move(inserted.error()));
			if (auto inserted = insert_heads(
					"INSERT INTO cxxlens_ng_series_head(series_id,current_publication,sequence) "
					"VALUES(?1,?2,?3)",
					*shadow_heads);
				!inserted)
				return fail(std::move(inserted.error()));
			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::final_object_copy,
											sqlite_store_fault_timing::after);
				fault)
				return fail(std::move(*fault));

			auto final_with_shadow_schema = read_user_schema(*database);
			if (!final_with_shadow_schema || final_with_shadow_schema->size() != 12U ||
				!schema_includes(*final_with_shadow_schema, sqlite_v3_ddl) ||
				!schema_includes(*final_with_shadow_schema, sqlite_migration_shadow_ddl))
				return fail(final_with_shadow_schema
								? store_error("store.corrupt", "migration-target", "schema")
								: std::move(final_with_shadow_schema.error()));
			auto final_profile =
				schema_profile_matches(*database, sqlite_physical_format::current_v3);
			if (!final_profile || !*final_profile)
				return fail(final_profile
								? store_error("store.corrupt", "migration-target", "profile")
								: std::move(final_profile.error()));
			auto final_metadata = read_metadata(*database);
			if (!final_metadata || *final_metadata != expected_non_marker_metadata)
				return fail(final_metadata
								? store_error("store.corrupt", "migration-target", "metadata")
								: std::move(final_metadata.error()));
			auto final_rows = read_v3_migration_projection(*database);
			if (!final_rows)
				return fail(std::move(final_rows.error()));
			if (auto valid = validate_row_projection(*final_rows); !valid)
				return fail(std::move(valid.error()));
			auto final_heads = read_heads("SELECT series_id,current_publication,sequence FROM "
										  "cxxlens_ng_series_head ORDER BY "
										  "series_id");
			if (!final_heads || *final_heads != *shadow_heads)
				return fail(final_heads ? store_error("store.corrupt", "migration-target", "heads")
										: std::move(final_heads.error()));

			constexpr std::array drop_shadow{
				"DROP INDEX cxxlens_ng_migration_payload_chunk_locator;",
				"DROP INDEX cxxlens_ng_migration_publication_series;",
				"DROP TABLE cxxlens_ng_migration_payload_chunk;",
				"DROP TABLE cxxlens_ng_migration_series_head;",
				"DROP TABLE cxxlens_ng_migration_publication;",
				"DROP TABLE cxxlens_ng_migration_metadata;",
			};
			for (const auto statement : drop_shadow)
				if (auto dropped = execute_migration_ddl(statement); !dropped)
					return fail(std::move(dropped.error()));
			const auto write_format_marker = [&]() -> result<void>
			{
				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::format_marker,
												sqlite_store_fault_timing::before);
					fault)
					return unexpected(std::move(*fault));
				auto marker = sqlite_statement::prepare(
					*database, "INSERT INTO cxxlens_ng_metadata(key,value) VALUES(?1,?2)", true);
				if (!marker)
					return unexpected(std::move(marker.error()));
				if (auto bound = marker->bind_text(1, "physical_format"); !bound)
					return unexpected(std::move(bound.error()));
				if (auto bound = marker->bind_text(2, "cxxlens.sqlite-semantic-store.v3"); !bound)
					return unexpected(std::move(bound.error()));
				if (auto inserted = marker->expect_done(); !inserted)
					return unexpected(std::move(inserted.error()));
				const auto finalize_before =
					dispatch_fault(sqlite_store_fault_boundary::statement_finalize,
								   sqlite_store_fault_timing::before);
				auto finalized = marker->finalize_exactly_once();
				const auto finalize_after =
					dispatch_fault(sqlite_store_fault_boundary::statement_finalize,
								   sqlite_store_fault_timing::after);
				if (finalize_before)
					return unexpected(std::move(*finalize_before));
				if (!finalized)
					return unexpected(std::move(finalized.error()));
				if (finalize_after)
					return unexpected(std::move(*finalize_after));
				if (auto fault = dispatch_fault(sqlite_store_fault_boundary::format_marker,
												sqlite_store_fault_timing::after);
					fault)
					return unexpected(std::move(*fault));
				return {};
			};
			if (auto marker = write_format_marker(); !marker)
				return fail(std::move(marker.error()),
							sqlite_terminal_phase::precommit,
							sqlite_terminal_cause::finalization_uncertain);

			auto final_classification = classify_sqlite_database(*database);
			if (!final_classification || !*final_classification ||
				**final_classification != sqlite_physical_format::current_v3)
				return fail(final_classification
								? store_error("store.corrupt", "migration-target", "classification")
								: std::move(final_classification.error()));
			sqlite_format = sqlite_physical_format::current_v3;
			auto target_census = database_authority_census();
			if (!target_census)
				return fail(std::move(target_census.error()));
			const auto target_invalid = ordered_invalid_committed(*target_census);
			if (!target_invalid.empty())
				return fail(store_error("store.compact-validation-failed", target_invalid.front()));
			auto candidate_rows =
				read_sqlite_publications(*database, sqlite_physical_format::current_v3);
			if (!candidate_rows)
				return fail(std::move(candidate_rows.error()));
			auto sealed_candidate = migration_authority_projection(
				sqlite_physical_format::current_v3, *target_census, *candidate_rows);
			if (!sealed_candidate)
				return fail(std::move(sealed_candidate.error()));
			candidate_projection.emplace(std::move(*sealed_candidate));

			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::transaction_commit,
											sqlite_store_fault_timing::before);
				fault)
				return fail(std::move(*fault));
			if (auto committed = database->execute("COMMIT;"); !committed)
				return fail(std::move(committed.error()),
							sqlite_terminal_phase::commit_outcome_unknown);
			transaction_active = false;
			if (auto fault = dispatch_fault(sqlite_store_fault_boundary::transaction_commit,
											sqlite_store_fault_timing::after);
				fault)
				return conclude(std::move(*fault),
								sqlite_terminal_phase::commit_outcome_unknown,
								sqlite_terminal_cause::triggering_failure);
			return conclude(store_error("store.sqlite-failure", "migration", "successful-handoff"),
							sqlite_terminal_phase::successful_handoff,
							sqlite_terminal_cause::triggering_failure);
		}

		[[nodiscard]] result<authority_census>
		persist_new(snapshot_handle::data& value,
					const std::optional<std::string>& expected_parent) const
		{
			if (database == nullptr)
				return unexpected(store_error("store.corrupt", "sqlite", "backend"));
			if (auto begun = database->execute("BEGIN IMMEDIATE;"); !begun)
				return unexpected(std::move(begun.error()));
			const auto rollback = [&]()
			{
				(void)database->execute("ROLLBACK;");
			};
			auto census = database_authority_census();
			if (!census)
			{
				rollback();
				return unexpected(std::move(census.error()));
			}
			const auto invalid_committed = ordered_invalid_committed(*census);
			if (!invalid_committed.empty())
			{
				const auto& id = invalid_committed.front();
				rollback();
				return unexpected(store_error("store.corrupt", id, "mutation-authority"));
			}
			auto next_generation =
				checked_counter_increment(census->maximum_generation, "physical_generation");
			if (!next_generation)
			{
				rollback();
				return unexpected(std::move(next_generation.error()));
			}
			auto& record = value.publication_record_value;
			const auto head = census->heads.find(record.series_id);
			const std::optional<std::string> actual_parent = head == census->heads.end()
				? std::optional<std::string>{}
				: std::optional<std::string>{head->second};
			if (actual_parent != expected_parent)
			{
				rollback();
				return unexpected(store_error("store.publication-conflict", record.series_id));
			}
			const auto prior_sequence = actual_parent
				? census->authority_records.at(*actual_parent).sequence
				: std::uint64_t{};
			auto next_sequence = checked_counter_increment(prior_sequence, "publication_sequence");
			if (!next_sequence)
			{
				rollback();
				return unexpected(std::move(next_sequence.error()));
			}
			record.sequence = *next_sequence;
			record.physical_generation = *next_generation;
			record.parent_publication = expected_parent;
			record.state = publication_state::committed;
			record.corrupt = false;
			record.publication_id = publication_identity(record);
			if (auto valid = validate_publication_identity(record); !valid)
			{
				rollback();
				return unexpected(std::move(valid.error()));
			}
			if (const auto existing = census->snapshot_exports.find(record.snapshot_id);
				existing != census->snapshot_exports.end() &&
				existing->second != canonical_export_of(value))
			{
				rollback();
				return unexpected(store_error("store.hash-collision", record.snapshot_id));
			}
			if (census->records.contains(record.publication_id))
			{
				rollback();
				return unexpected(store_error(
					"store.corrupt", record.publication_id, "duplicate-publication-id"));
			}
			if (sqlite_format == sqlite_physical_format::current_v3)
			{
				auto receipt = insert_streamed_snapshot_chunks(*database,
															   v3_chunk_insert_sql,
															   record.publication_id,
															   record.physical_generation,
															   value);
				if (!receipt)
				{
					rollback();
					return unexpected(std::move(receipt.error()));
				}
				if (auto row = insert_v3_publication_row(*database,
														 record,
														 receipt->full_checksum,
														 receipt->byte_count,
														 receipt->chunk_count);
					!row)
				{
					rollback();
					return unexpected(std::move(row.error()));
				}
			}
			else
			{
				const auto payload = encode_snapshot(value);
				const auto checksum = content_digest(payload);
				if (auto inserted =
						insert_v2_publication_row(*database, record, checksum, payload, false);
					!inserted)
				{
					rollback();
					return unexpected(std::move(inserted.error()));
				}
			}

			if (sqlite_format == sqlite_physical_format::current_v3)
			{
				const auto sql = actual_parent
					? "UPDATE cxxlens_ng_series_head SET current_publication=?1,sequence=?2 "
					  "WHERE series_id=?3 AND current_publication=?4"
					: "INSERT INTO cxxlens_ng_series_head(series_id,current_publication,"
					  "sequence) VALUES(?1,?2,?3)";
				auto head_statement = sqlite_statement::prepare(*database, sql, true);
				if (!head_statement)
				{
					rollback();
					return unexpected(std::move(head_statement.error()));
				}
				if (actual_parent)
				{
					if (auto bound = head_statement->bind_text(1, record.publication_id); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
					if (auto bound = head_statement->bind_unsigned(2, record.sequence); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
					if (auto bound = head_statement->bind_text(3, record.series_id); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
					if (auto bound = head_statement->bind_text(4, *actual_parent); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
				}
				else
				{
					if (auto bound = head_statement->bind_text(1, record.series_id); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
					if (auto bound = head_statement->bind_text(2, record.publication_id); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
					if (auto bound = head_statement->bind_unsigned(3, record.sequence); !bound)
					{
						rollback();
						return unexpected(std::move(bound.error()));
					}
				}
				if (auto updated = head_statement->expect_done(); !updated)
				{
					rollback();
					return unexpected(std::move(updated.error()));
				}
				auto changed = sqlite_statement::prepare(*database, "SELECT changes()", true);
				if (!changed || changed->step() != sqlite_row)
				{
					rollback();
					return unexpected(
						store_error("store.corrupt", record.series_id, "series-head-cas"));
				}
				auto count = changed->column_unsigned(0);
				if (!count || *count != 1U)
				{
					rollback();
					return unexpected(
						store_error("store.corrupt", record.series_id, "series-head-cas"));
				}
			}
			else
			{
				const auto head_sql = actual_parent
					? "UPDATE cxxlens_ng_series_head SET current_publication=" +
						sql_quote(record.publication_id) +
						",sequence=" + sqlite_unsigned(record.sequence) +
						" WHERE series_id=" + sql_quote(record.series_id) +
						" AND current_publication=" + sql_quote(*actual_parent) + ';'
					: "INSERT INTO cxxlens_ng_series_head VALUES(" + sql_quote(record.series_id) +
						',' + sql_quote(record.publication_id) + ',' +
						sqlite_unsigned(record.sequence) + ");";
				if (auto updated = database->execute(head_sql); !updated)
				{
					rollback();
					return unexpected(std::move(updated.error()));
				}
			}
			auto committed_value = std::make_shared<snapshot_handle::data>(value);
			census->records.emplace(record.publication_id, record);
			census->publications.emplace(record.publication_id, committed_value);
			census->authority_records.emplace(record.publication_id, record);
			census->authority_publications.emplace(record.publication_id, committed_value);
			census->heads[record.series_id] = record.publication_id;
			census->snapshot_exports[record.snapshot_id] = canonical_export_of(value);
			census->maximum_generation = record.physical_generation;
			if (auto committed = database->execute("COMMIT;"); !committed)
			{
				rollback();
				return unexpected(std::move(committed.error()));
			}
			return std::move(*census);
		}

		[[nodiscard]] result<std::shared_ptr<snapshot_handle::data>>
		resolve_snapshot_publication(const std::string_view snapshot_id) const
		{
			const publication_record* selected{};
			std::map<std::pair<std::uint64_t, std::uint64_t>, std::string> resolver_keys;
			for (const auto& [publication_id, record] : records)
			{
				(void)publication_id;
				if (record.state != publication_state::committed ||
					record.snapshot_id != snapshot_id)
					continue;
				const auto [position, inserted] = resolver_keys.emplace(
					std::pair{record.sequence, record.physical_generation}, record.publication_id);
				if (!inserted && position->second != record.publication_id)
					return unexpected(
						store_error("store.snapshot-ambiguous", std::string{snapshot_id}));
				if (selected == nullptr ||
					std::tie(selected->sequence, selected->physical_generation) <
						std::tie(record.sequence, record.physical_generation))
					selected = &record;
			}
			if (selected == nullptr)
				return unexpected(
					store_error("store.snapshot-not-found", std::string{snapshot_id}));
			if (selected->corrupt || !validate_publication_identity(*selected))
				return unexpected(store_error("store.snapshot-corrupt", selected->publication_id));
			const auto publication = publications.find(selected->publication_id);
			if (publication == publications.end() ||
				publication->second->publication_record_value != *selected ||
				publication->second->semantic_manifest.id != snapshot_id)
				return unexpected(store_error("store.snapshot-corrupt", selected->publication_id));
			return publication->second;
		}

		[[nodiscard]] result<void> load()
		{
			if (database == nullptr)
				return {};
			auto census = database_authority_census();
			if (!census)
				return unexpected(std::move(census.error()));
			for (auto& [id, publication] : census->publications)
			{
				const auto record = census->records.find(id);
				if (record == census->records.end() ||
					record->second.state != publication_state::committed || record->second.corrupt)
					continue;
				auto token =
					std::make_shared<const std::uint64_t>(record->second.physical_generation);
				publication->generation_pin = token;
				generation_tokens.push_back(token);
			}
			records = std::move(census->records);
			publications = std::move(census->publications);
			heads = std::move(census->heads);
			generation = census->maximum_generation;
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

	snapshot_writer::snapshot_writer(snapshot_writer&&) noexcept = default;
	snapshot_writer& snapshot_writer::operator=(snapshot_writer&&) noexcept = default;

	snapshot_store::snapshot_store(std::shared_ptr<implementation> implementation)
		: implementation_{std::move(implementation)}
	{
	}

	result<snapshot_handle> snapshot_store::current(const snapshot_series_selector& selector) const
	{
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		if (auto valid = selector.validate(); !valid)
			return unexpected(std::move(valid.error()));
		const auto head = implementation_->heads.find(selector.id());
		if (head == implementation_->heads.end())
			return unexpected(store_error("store.current-not-found", selector.id()));
		const auto& record = implementation_->records.at(head->second);
		if (record.corrupt || !validate_publication_identity(record))
			return unexpected(store_error("store.current-corrupt", record.publication_id));
		const auto publication = implementation_->publications.find(record.publication_id);
		if (publication == implementation_->publications.end())
			return unexpected(store_error("store.current-corrupt", record.publication_id));
		return snapshot_handle{publication->second};
	}

	result<snapshot_handle> snapshot_store::open(const std::string_view snapshot_id) const
	{
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		auto resolved = implementation_->resolve_snapshot_publication(snapshot_id);
		if (!resolved)
			return unexpected(std::move(resolved.error()));
		return snapshot_handle{std::move(*resolved)};
	}

	result<snapshot_handle>
	snapshot_store::open_publication(const std::string_view publication_id) const
	{
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		const auto record = implementation_->records.find(publication_id);
		if (record == implementation_->records.end() ||
			record->second.state != publication_state::committed)
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		if (record->second.corrupt || !validate_publication_identity(record->second))
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
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		if (auto valid = draft.series.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (!digest(draft.catalog_semantic_digest) ||
			draft.series.engine_generation_id != implementation_->engine.generation() ||
			draft.series.relation_registry_digest != implementation_->engine.registry_digest())
			return unexpected(store_error("store.draft-authority-mismatch", "snapshot"));
		if (implementation_->backend == "sqlite" &&
			implementation_->sqlite_format == sqlite_physical_format::predecessor_v2)
			return unexpected(store_error("store.migration-required",
										  "sqlite-physical-format",
										  "cxxlens.sqlite-semantic-store.v2-to-v3"));
		auto value = std::make_unique<snapshot_writer::data>();
		value->store = implementation_;
		value->draft = std::move(draft);
		return snapshot_writer{std::move(value)};
	}

	store_compatibility snapshot_store::compatibility() const
	{
		std::scoped_lock lock{implementation_->mutex};
		const auto observed =
			implementation_->availability.observe(implementation_->live_generation_count_unlocked())
				.compatibility;
		return {implementation_->backend,
				observed.readable_format,
				observed.direct_open,
				observed.migration_required};
	}

	result<void> snapshot_store::compact()
	{
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		if (implementation_->backend == "sqlite")
		{
			if (auto recovered = implementation_->recover_wal_only_for_mutation(); !recovered)
				return recovered;
		}
		if (implementation_->backend == "sqlite" &&
			implementation_->sqlite_format == sqlite_physical_format::predecessor_v2)
			return implementation_->migrate_predecessor_v2();
		if (implementation_->database == nullptr && implementation_->backend != "memory")
			return unexpected(store_error("store.corrupt", "sqlite", "backend"));
		using replacement_entry = std::pair<std::string, std::shared_ptr<snapshot_handle::data>>;
		const auto replacement_order =
			[](const replacement_entry& left, const replacement_entry& right)
		{
			const auto& left_record = left.second->publication_record_value;
			const auto& right_record = right.second->publication_record_value;
			return std::tie(left_record.sequence,
							left_record.physical_generation,
							left_record.publication_id) < std::tie(right_record.sequence,
																   right_record.physical_generation,
																   right_record.publication_id);
		};
		const auto validate_replacement = [&](snapshot_handle::data& value) -> result<void>
		{
			const auto& record = value.publication_record_value;
			if (record.state != publication_state::committed || record.corrupt ||
				value.semantic_manifest.id != snapshot_identity(value.semantic_manifest) ||
				!validate_publication_identity(record))
				return unexpected(
					store_error("store.compact-validation-failed", record.publication_id));
			if (!value.partition_envelopes.empty())
				if (auto valid = validate_semantic_graph(value, implementation_->engine); !valid)
					return unexpected(
						store_error("store.compact-validation-failed", record.publication_id));
			return {};
		};

		if (implementation_->database == nullptr)
		{
			publication_record_map authority_records;
			std::vector<replacement_entry> replacements;
			std::map<std::string, std::string, std::less<>> snapshot_exports;
			std::uint64_t maximum_generation{};
			for (const auto& [id, record] : implementation_->records)
			{
				if (record.state != publication_state::committed)
					continue;
				const auto current = implementation_->publications.find(id);
				if (record.corrupt || !validate_publication_identity(record) ||
					current == implementation_->publications.end() ||
					current->second->publication_record_value != record)
					return unexpected(store_error("store.compact-validation-failed", id));
				auto replacement = std::make_shared<snapshot_handle::data>(*current->second);
				if (auto valid = validate_replacement(*replacement); !valid)
					return valid;
				const auto export_value = canonical_export_of(*replacement);
				const auto [position, inserted] =
					snapshot_exports.emplace(record.snapshot_id, export_value);
				if (!inserted && position->second != export_value)
					return unexpected(store_error("store.compact-validation-failed", id));
				authority_records.emplace(id, record);
				maximum_generation = std::max(maximum_generation, record.physical_generation);
				replacements.emplace_back(id, std::move(replacement));
			}
			auto authority_heads = derive_authority_heads(authority_records);
			if (!authority_heads)
				return unexpected(std::move(authority_heads.error()));
			if (*authority_heads != implementation_->heads)
				return unexpected(store_error("store.compact-validation-failed", "series-head"));
			std::ranges::sort(replacements, replacement_order);
			auto next_generation = maximum_generation;
			for (auto& [id, replacement] : replacements)
			{
				(void)id;
				auto allocated = checked_counter_increment(next_generation, "physical_generation");
				if (!allocated)
					return unexpected(std::move(allocated.error()));
				next_generation = *allocated;
				replacement->publication_record_value.physical_generation = next_generation;
				if (auto valid = validate_replacement(*replacement); !valid)
					return valid;
			}
			for (auto& [id, replacement] : replacements)
			{
				auto token = std::make_shared<const std::uint64_t>(
					replacement->publication_record_value.physical_generation);
				replacement->generation_pin = token;
				implementation_->generation_tokens.push_back(token);
				implementation_->records[id] = replacement->publication_record_value;
				implementation_->publications[id] = std::move(replacement);
			}
			implementation_->generation = next_generation;
			return {};
		}

		if (auto begun = implementation_->database->execute("BEGIN IMMEDIATE;"); !begun)
			return begun;
		const auto rollback = [&]()
		{
			(void)implementation_->database->execute("ROLLBACK;");
		};
		auto census = implementation_->database_authority_census();
		if (!census)
		{
			auto failure = std::move(census.error());
			rollback();
			if (failure.code == "store.corrupt" || failure.code == "store.current-ambiguous")
				return unexpected(
					store_error("store.compact-validation-failed", failure.field, failure.detail));
			return unexpected(std::move(failure));
		}
		const auto invalid_committed =
			snapshot_store::implementation::ordered_invalid_committed(*census);
		if (!invalid_committed.empty())
		{
			const auto& id = invalid_committed.front();
			rollback();
			return unexpected(store_error("store.compact-validation-failed", id));
		}

		std::vector<replacement_entry> replacements;
		for (const auto& [id, current] : census->authority_publications)
		{
			auto replacement = std::make_shared<snapshot_handle::data>(*current);
			if (auto valid = validate_replacement(*replacement); !valid)
			{
				rollback();
				return valid;
			}
			replacements.emplace_back(id, std::move(replacement));
		}
		std::ranges::sort(replacements, replacement_order);
		auto next_generation = census->maximum_generation;
		if (replacements.empty())
		{
			rollback();
			return {};
		}
		std::vector<std::pair<std::string, std::uint64_t>> retired_generations;
		auto update = sqlite_statement::prepare(
			*implementation_->database,
			"UPDATE cxxlens_ng_publication SET generation=?1,payload_checksum=?2,"
			"payload_byte_count=?3,payload_chunk_count=?4 WHERE publication_id=?5 "
			"AND generation=?6",
			implementation_->sqlite_format == sqlite_physical_format::current_v3);
		if (!update)
		{
			rollback();
			return unexpected(std::move(update.error()));
		}
		for (auto& [id, replacement] : replacements)
		{
			const auto old_generation = replacement->publication_record_value.physical_generation;
			auto allocated = checked_counter_increment(next_generation, "physical_generation");
			if (!allocated)
			{
				rollback();
				return unexpected(std::move(allocated.error()));
			}
			next_generation = *allocated;
			replacement->publication_record_value.physical_generation = next_generation;
			if (auto valid = validate_replacement(*replacement); !valid)
			{
				rollback();
				return valid;
			}
			if (implementation_->sqlite_format == sqlite_physical_format::current_v3)
			{
				auto receipt = insert_streamed_snapshot_chunks(*implementation_->database,
															   v3_chunk_insert_sql,
															   id,
															   next_generation,
															   *replacement);
				if (!receipt)
				{
					rollback();
					return unexpected(std::move(receipt.error()));
				}
				const std::array counters{
					std::pair{1, next_generation},
					std::pair{3, receipt->byte_count},
					std::pair{4, receipt->chunk_count},
					std::pair{6, old_generation},
				};
				for (const auto& [index, counter] : counters)
					if (auto bound = update->bind_unsigned(index, counter); !bound)
					{
						rollback();
						return bound;
					}
				if (auto bound = update->bind_text(2, receipt->full_checksum); !bound)
				{
					rollback();
					return bound;
				}
				if (auto bound = update->bind_text(5, id); !bound)
				{
					rollback();
					return bound;
				}
				if (auto updated = update->expect_done(); !updated)
				{
					rollback();
					return updated;
				}
				if (auto reset = update->reset(); !reset)
				{
					rollback();
					return reset;
				}
				auto changed =
					sqlite_statement::prepare(*implementation_->database, "SELECT changes()", true);
				if (!changed || changed->step() != sqlite_row)
				{
					rollback();
					return unexpected(store_error("store.corrupt", id, "compaction-replace"));
				}
				auto count = changed->column_unsigned(0);
				if (!count || *count != 1U)
				{
					rollback();
					return unexpected(store_error("store.corrupt", id, "compaction-replace"));
				}
				retired_generations.emplace_back(id, old_generation);
			}
			else
			{
				const auto payload = encode_snapshot(*replacement);
				const auto checksum = content_digest(payload);
				const auto updated =
					update_v2_publication_row(*implementation_->database,
											  id,
											  replacement->publication_record_value,
											  checksum,
											  payload);
				if (!updated)
				{
					rollback();
					return updated;
				}
			}
			census->records[id] = replacement->publication_record_value;
			census->publications[id] = replacement;
			census->authority_records[id] = replacement->publication_record_value;
			census->authority_publications[id] = replacement;
		}

		if (implementation_->sqlite_format == sqlite_physical_format::current_v3)
			for (const auto& [id, old_generation] : retired_generations)
			{
				if (auto deleted = delete_v3_chunks(*implementation_->database, id, old_generation);
					!deleted)
				{
					rollback();
					return deleted;
				}
			}
		if (auto committed = implementation_->database->execute("COMMIT;"); !committed)
		{
			rollback();
			return committed;
		}

		for (auto& [id, replacement] : replacements)
		{
			(void)id;
			auto token = std::make_shared<const std::uint64_t>(
				replacement->publication_record_value.physical_generation);
			replacement->generation_pin = token;
			implementation_->generation_tokens.push_back(token);
		}
		implementation_->records = std::move(census->records);
		implementation_->publications = std::move(census->publications);
		implementation_->heads = std::move(census->heads);
		implementation_->generation = next_generation;
		return {};
	}

	result<std::string> snapshot_store::canonical_export(const std::string_view snapshot_id) const
	{
		std::scoped_lock lock{implementation_->mutex};
		if (!implementation_->result_operations_available())
			return unexpected(sqlite_reopen_required());
		auto resolved = implementation_->resolve_snapshot_publication(snapshot_id);
		if (!resolved)
			return unexpected(std::move(resolved.error()));
		return canonical_export_of(**resolved);
	}

	std::size_t snapshot_store::retained_generation_count() const
	{
		std::scoped_lock lock{implementation_->mutex};
		return implementation_->availability
			.observe(implementation_->live_generation_count_unlocked())
			.retained_generation_count;
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
					auto prior =
						data_->store->resolve_snapshot_publication(derived->input_snapshot);
					if (!prior)
					{
						if (prior.error().code != "store.snapshot-not-found")
							return unexpected(std::move(prior.error()));
						return unexpected(
							store_error("store.derived-basis-not-prior", value.content));
					}
					if ((*prior)->publication_record_value.physical_generation >
						data_->store->generation)
						return unexpected(
							store_error("store.derived-basis-not-prior", value.content));
					const auto& partitions = (*prior)->semantic_manifest.partitions;
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
		if (!data_)
			return unexpected(store_error("store.transaction-state", "publish"));
		auto& store = *data_->store;
		std::scoped_lock lock{store.mutex};
		if (!store.result_operations_available())
		{
			data_->current_state = publication_state::rolled_back;
			return unexpected(sqlite_reopen_required());
		}
		if (data_->current_state != publication_state::validating || !data_->candidate)
			return unexpected(store_error("store.transaction-state", "publish"));
		if (store.backend == "sqlite")
		{
			if (auto recovered = store.recover_wal_only_for_mutation(); !recovered)
			{
				data_->current_state = publication_state::rolled_back;
				return unexpected(std::move(recovered.error()));
			}
		}
		const auto series_id = data_->draft.series.id();
		publication_record record;
		record.series_id = series_id;
		record.snapshot_id = data_->candidate->semantic_manifest.id;
		record.parent_publication = data_->draft.expected_parent_publication;
		record.state = publication_state::committed;
		data_->candidate->publication_record_value = record;
		std::optional<snapshot_store::implementation::authority_census> committed_census;

		if (store.database == nullptr)
		{
			publication_record_map authority_records;
			std::map<std::string, std::string, std::less<>> snapshot_exports;
			std::uint64_t maximum_generation{};
			for (const auto& [id, existing_record] : store.records)
			{
				if (existing_record.state != publication_state::committed)
					continue;
				const auto publication = store.publications.find(id);
				if (existing_record.corrupt || !validate_publication_identity(existing_record) ||
					publication == store.publications.end() ||
					publication->second->publication_record_value != existing_record)
					continue;
				authority_records.emplace(id, existing_record);
				maximum_generation =
					std::max(maximum_generation, existing_record.physical_generation);
				const auto export_value = canonical_export_of(*publication->second);
				const auto [position, inserted] =
					snapshot_exports.emplace(existing_record.snapshot_id, export_value);
				if (!inserted && position->second != export_value)
				{
					data_->current_state = publication_state::rejected;
					return unexpected(
						store_error("store.hash-collision", existing_record.snapshot_id));
				}
			}
			auto authority_heads = derive_authority_heads(authority_records);
			if (!authority_heads || *authority_heads != store.heads)
			{
				data_->current_state = publication_state::rejected;
				return unexpected(store_error("store.corrupt", series_id, "series-head"));
			}
			const auto head = authority_heads->find(series_id);
			const std::optional<std::string> current_parent = head == authority_heads->end()
				? std::optional<std::string>{}
				: std::optional<std::string>{head->second};
			if (current_parent != data_->draft.expected_parent_publication)
			{
				data_->current_state = publication_state::rejected;
				return unexpected(store_error("store.publish-stale-parent", series_id));
			}
			const auto prior_sequence =
				current_parent ? authority_records.at(*current_parent).sequence : std::uint64_t{};
			auto sequence = checked_counter_increment(prior_sequence, "publication_sequence");
			auto next_generation =
				checked_counter_increment(maximum_generation, "physical_generation");
			if (!sequence || !next_generation)
			{
				data_->current_state = publication_state::rejected;
				return unexpected(!sequence ? std::move(sequence.error())
											: std::move(next_generation.error()));
			}
			auto& candidate_record = data_->candidate->publication_record_value;
			candidate_record.sequence = *sequence;
			candidate_record.physical_generation = *next_generation;
			candidate_record.publication_id = publication_identity(candidate_record);
			if (auto valid = validate_publication_identity(candidate_record); !valid)
			{
				data_->current_state = publication_state::rejected;
				return unexpected(std::move(valid.error()));
			}
			if (const auto existing = snapshot_exports.find(candidate_record.snapshot_id);
				existing != snapshot_exports.end() &&
				existing->second != canonical_export_of(*data_->candidate))
			{
				data_->current_state = publication_state::rejected;
				return unexpected(
					store_error("store.hash-collision", candidate_record.snapshot_id));
			}
		}
		if (store.database != nullptr)
		{
			auto persisted =
				store.persist_new(*data_->candidate, data_->draft.expected_parent_publication);
			if (!persisted)
			{
				data_->current_state = persisted.error().code == "store.publication-conflict"
					? publication_state::rejected
					: publication_state::rolled_back;
				return unexpected(std::move(persisted.error()));
			}
			committed_census.emplace(std::move(*persisted));
		}
		const auto& committed_record = data_->candidate->publication_record_value;
		if (committed_census)
		{
			committed_census->publications[committed_record.publication_id] = data_->candidate;
			committed_census->authority_publications[committed_record.publication_id] =
				data_->candidate;
			for (auto& [id, publication] : committed_census->authority_publications)
			{
				const auto record_value = committed_census->authority_records.find(id);
				if (record_value == committed_census->authority_records.end())
					continue;
				auto token =
					std::make_shared<const std::uint64_t>(record_value->second.physical_generation);
				publication->generation_pin = token;
				store.generation_tokens.push_back(token);
			}
			store.records = std::move(committed_census->records);
			store.publications = std::move(committed_census->publications);
			store.heads = std::move(committed_census->heads);
			store.generation = committed_census->maximum_generation;
		}
		else
		{
			auto token =
				std::make_shared<const std::uint64_t>(committed_record.physical_generation);
			data_->candidate->generation_pin = token;
			store.generation = committed_record.physical_generation;
			store.generation_tokens.push_back(token);
			store.records[committed_record.publication_id] = committed_record;
			store.publications[committed_record.publication_id] = data_->candidate;
			store.heads[series_id] = committed_record.publication_id;
		}
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
		if (database_path != ":memory:" &&
			(database_path.find('\0') != std::string::npos || database_path.starts_with("file:") ||
			 database_path.contains('?') || database_path.contains('#')))
			return unexpected(
				store_error("store.sqlite-failure", "sqlite-locator", "invalid-filesystem-path"));
		if (database_path != ":memory:")
		{
			auto api = load_sqlite();
			if (!api)
				return unexpected(std::move(api.error()));
			void* default_vfs = (*api)->vfs_find(nullptr);
			if (default_vfs == nullptr)
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "default-vfs"));
			auto bundle = make_sqlite_default_forwarding_store_bundle(
				database_path,
				sqlite_private_snapshot_registry_binding{(*api)->runtime_identity,
														 default_vfs,
														 (*api)->vfs_find,
														 (*api)->vfs_register,
														 (*api)->vfs_unregister,
														 *api});
			if (!bundle)
				return unexpected(std::move(bundle.error()));
			if (bundle->runtime_identity != (*api)->runtime_identity ||
				bundle->runtime_lifetime.get() != api->get() || !bundle->forwarding_vfs ||
				!bundle->observation || bundle->canonical_vfs_locator.empty())
				return unexpected(
					store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
			const std::string vfs_name{bundle->forwarding_vfs->registered_vfs_name()};
			std::shared_ptr<void> backend_lifetime = bundle->forwarding_vfs;
			void* native_library_handle = (*api)->library;
			const void* runtime_identity = (*api)->runtime_identity;
			auto runtime_lifetime = std::move(*api);
			return snapshot_store_backend_lifetime_access::open_sqlite(
				bundle->canonical_vfs_locator,
				std::move(engine),
				vfs_name,
				sqlite_backend_runtime_binding{
					native_library_handle, runtime_identity, std::move(runtime_lifetime)},
				std::move(backend_lifetime),
				std::move(bundle->observation));
		}
		auto api = load_sqlite();
		if (!api)
			return unexpected(std::move(api.error()));
		void* default_vfs = (*api)->vfs_find(nullptr);
		if (default_vfs == nullptr)
			return unexpected(store_error("store.backend-unavailable", "sqlite", "default-vfs"));
		auto bundle = make_sqlite_default_ephemeral_store_bundle(
			sqlite_private_snapshot_registry_binding{(*api)->runtime_identity,
													 default_vfs,
													 (*api)->vfs_find,
													 (*api)->vfs_register,
													 (*api)->vfs_unregister,
													 *api});
		if (!bundle)
			return unexpected(std::move(bundle.error()));
		if (bundle->runtime_identity != (*api)->runtime_identity ||
			bundle->runtime_lifetime.get() != api->get() || !bundle->forwarding_vfs ||
			!bundle->observation || bundle->sqlite_locator != ":memory:")
			return unexpected(
				store_error("store.backend-unavailable", "sqlite", "runtime-binding"));
		const std::string vfs_name{bundle->forwarding_vfs->registered_vfs_name()};
		std::shared_ptr<void> backend_lifetime = bundle->forwarding_vfs;
		void* native_library_handle = (*api)->library;
		const void* runtime_identity = (*api)->runtime_identity;
		auto runtime_lifetime = std::move(*api);
		return snapshot_store_backend_lifetime_access::open_sqlite(
			bundle->sqlite_locator,
			std::move(engine),
			vfs_name,
			sqlite_backend_runtime_binding{
				native_library_handle, runtime_identity, std::move(runtime_lifetime)},
			std::move(backend_lifetime),
			std::move(bundle->observation));
	}

	result<snapshot_store> snapshot_store_backend_lifetime_access::open_sqlite(
		const std::string& database_path,
		relation_engine engine,
		const std::string& vfs_name,
		sqlite_backend_runtime_binding runtime,
		std::shared_ptr<void> backend_lifetime,
		std::shared_ptr<sqlite_backend_observation_capability> observation)
	{
		if (database_path.empty())
			return unexpected(store_error("store.sqlite-path-empty", "database_path"));
		if (vfs_name.empty() || !backend_lifetime || !observation)
			return unexpected(store_error("store.backend-unavailable", "sqlite", "vfs-lifetime"));
		auto api = bind_sqlite_runtime(std::move(runtime));
		if (!api)
			return unexpected(std::move(api.error()));
		auto opened = open_observed_store_database(
			std::move(*api), database_path, vfs_name, backend_lifetime, observation);
		if (!opened)
			return unexpected(std::move(opened.error()));
		auto implementation =
			std::make_shared<snapshot_store::implementation>(std::move(engine), "sqlite");
		implementation->sqlite_runtime = opened->api;
		implementation->sqlite_path = database_path;
		implementation->sqlite_vfs_name = vfs_name;
		implementation->sqlite_format = opened->format;
		implementation->sqlite_observation = observation;
		implementation->backend_lifetime = backend_lifetime;
		if (opened->source_anchor)
			implementation->sqlite_source_anchor = *opened->source_anchor;
		implementation->database = std::move(opened->database);
		if (opened->private_read_transaction)
		{
			auto loaded = implementation->load();
			opened->database = std::move(implementation->database);
			if (!loaded)
			{
				auto failure = std::move(loaded.error());
				if (auto stable = finish_private_read(*opened, database_path, *observation, false);
					!stable)
					return unexpected(std::move(stable.error()));
				return unexpected(std::move(failure));
			}
			if (auto stable = finish_private_read(*opened, database_path, *observation, true);
				!stable)
				return unexpected(std::move(stable.error()));
			if (!opened->source_anchor)
				return unexpected(
					store_error("store.corrupt", "sqlite-open-snapshot", "source-anchor"));
			implementation->sqlite_source_anchor = *opened->source_anchor;
			if (opened->wal_only_capture)
			{
				implementation->sqlite_wal_only_capture = std::move(opened->wal_only_capture);
				implementation->sqlite_wal_state = sqlite_wal_handoff_state::wal_only_deferred;
			}
			if (opened->format == sqlite_physical_format::current_v3 &&
				!implementation->sqlite_wal_only_capture)
			{
				auto writable =
					open_current_observed_database(opened->api,
												   database_path,
												   vfs_name,
												   backend_lifetime,
												   observation,
												   *implementation->sqlite_source_anchor);
				if (!writable)
					return unexpected(std::move(writable.error()));
				implementation->database = std::move(*writable);
			}
		}
		else
		{
			if (auto loaded = implementation->load(); !loaded)
				return unexpected(std::move(loaded.error()));
		}
		if (!implementation->install_independently_validated_state())
			return unexpected(sqlite_reopen_required());
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

	// Existing test-only fault-injection hook. Ordinary calls perform a symmetric raw
	// before/after payload rewrite; closed `test.*` selectors exercise persisted metadata,
	// topology, and canonical-order failures. Source-private qualification wrappers below
	// reuse those selectors without adding public header declarations.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
	result<void> rewrite_publication_payload_for_testing(snapshot_store& store,
														 const std::string_view publication_id,
														 const std::string_view before,
														 const std::string_view after,
														 const std::size_t occurrence)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		if (store.implementation_->database == nullptr)
			return unexpected(store_error("store.corrupt", "test-rewrite", "backend"));
		const auto publication = store.implementation_->publications.find(publication_id);
		if (before == "test.payload-schema")
		{
			if (publication == store.implementation_->publications.end())
				return unexpected(
					store_error("store.publication-not-found", std::string{publication_id}));
			if (after.size() != 1U || after.front() < '1' || after.front() > '5')
				return unexpected(store_error("store.corrupt", "test-payload-schema", "version"));
			const auto schema =
				payload_schema_from_number(static_cast<std::uint8_t>(after.front() - '0'));
			if (!schema)
				return unexpected(store_error("store.corrupt", "test-payload-schema", "version"));
			const auto& value = *publication->second;
			const auto& record = value.publication_record_value;
			const auto payload = encode_snapshot(value, *schema);
			const auto checksum = content_digest(payload);
			if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
				return with_immediate_transaction(
					*store.implementation_->database,
					[&]()
					{
						return replace_v3_publication_payload(
							*store.implementation_->database, record, record, payload);
					});
			return with_immediate_transaction(*store.implementation_->database,
											  [&]()
											  {
												  return update_v2_publication_row(
													  *store.implementation_->database,
													  publication_id,
													  record,
													  checksum,
													  payload);
											  });
		}
		if (before == "test.series-head")
		{
			if (publication == store.implementation_->publications.end())
				return unexpected(
					store_error("store.publication-not-found", std::string{publication_id}));
			const auto& series_id = publication->second->publication_record_value.series_id;
			std::string sql;
			if (after == "missing")
				sql = "DELETE FROM cxxlens_ng_series_head WHERE series_id=" + sql_quote(series_id);
			else if (after == "publication")
				sql = "UPDATE cxxlens_ng_series_head SET current_publication='publication:missing' "
					  "WHERE series_id=" +
					sql_quote(series_id);
			else if (after == "sequence")
				sql = "UPDATE cxxlens_ng_series_head SET sequence=0 WHERE series_id=" +
					sql_quote(series_id);
			else
				return unexpected(store_error("store.corrupt", "test-series-head", "mutation"));
			return store.implementation_->database->execute(std::move(sql));
		}
		if (before == "test.duplicate-sequence")
		{
			if (publication == store.implementation_->publications.end())
				return unexpected(
					store_error("store.publication-not-found", std::string{publication_id}));
			auto duplicate = snapshot_handle::data{*publication->second};
			auto& record = duplicate.publication_record_value;
			record.parent_publication = "publication:duplicate-parent";
			auto generation =
				checked_counter_increment(record.physical_generation, "physical_generation");
			if (!generation)
				return unexpected(std::move(generation.error()));
			record.physical_generation = *generation;
			record.publication_id = publication_identity(record);
			const auto payload = encode_snapshot(duplicate);
			const auto checksum = content_digest(payload);
			if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
				return with_immediate_transaction(
					*store.implementation_->database,
					[&]() -> result<void>
					{
						if (auto chunks = insert_v3_chunks(*store.implementation_->database,
														   record.publication_id,
														   record.physical_generation,
														   payload);
							!chunks)
							return chunks;
						return insert_v3_publication_row(*store.implementation_->database,
														 record,
														 checksum,
														 payload.size(),
														 payload_chunk_count(payload.size()));
					});
			return insert_v2_publication_row(
				*store.implementation_->database, record, checksum, payload, false);
		}
		if (before == "test.reverse-manifest-partitions")
		{
			if (publication == store.implementation_->publications.end())
				return unexpected(
					store_error("store.publication-not-found", std::string{publication_id}));
			auto reordered = snapshot_handle::data{*publication->second};
			if (reordered.semantic_manifest.partitions.size() < 2U)
				return unexpected(store_error("store.corrupt", "test-manifest-order", "count"));
			std::ranges::reverse(reordered.semantic_manifest.partitions);
			if (reordered.semantic_manifest.id != snapshot_identity(reordered.semantic_manifest))
				return unexpected(store_error("store.corrupt", "test-manifest-order", "identity"));
			return store.implementation_->persist(reordered);
		}
		if (before == "test.orphan-parent")
		{
			if (publication == store.implementation_->publications.end())
				return unexpected(
					store_error("store.publication-not-found", std::string{publication_id}));
			auto orphaned = snapshot_handle::data{*publication->second};
			auto& record = orphaned.publication_record_value;
			record.parent_publication =
				"publication:sha256:"
				"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";
			record.publication_id = publication_identity(record);
			const auto payload = encode_snapshot(orphaned);
			const auto checksum = content_digest(payload);
			if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
				return with_immediate_transaction(
					*store.implementation_->database,
					[&]()
					{
						return replace_v3_publication(*store.implementation_->database,
													  publication->second->publication_record_value,
													  orphaned);
					});
			return with_immediate_transaction(
				*store.implementation_->database,
				[&]() -> result<void>
				{
					if (auto updated = update_v2_publication_row(*store.implementation_->database,
																 publication_id,
																 record,
																 checksum,
																 payload);
						!updated)
						return updated;
					auto head = sqlite_statement::prepare(
						*store.implementation_->database,
						"UPDATE cxxlens_ng_series_head SET current_publication=?1 WHERE "
						"current_publication=?2",
						false);
					if (!head)
						return unexpected(std::move(head.error()));
					if (auto bound = head->bind_text(1, record.publication_id); !bound)
						return bound;
					if (auto bound = head->bind_text(2, publication_id); !bound)
						return bound;
					return head->expect_done();
				});
		}
		if (before.empty() || before.size() != after.size())
			return unexpected(store_error("store.corrupt", "test-rewrite", "invalid"));
		const auto rewrite_bytes = [&](std::vector<std::byte>& payload) -> result<void>
		{
			const auto needle = std::as_bytes(std::span{before.data(), before.size()});
			auto search_begin = payload.begin();
			decltype(search_begin) found;
			for (std::size_t index = 0U; index <= occurrence; ++index)
			{
				found = std::search(search_begin, payload.end(), needle.begin(), needle.end());
				if (found == payload.end())
					return unexpected(store_error("store.corrupt", "test-rewrite", "not-found"));
				search_begin = found + static_cast<std::ptrdiff_t>(needle.size());
			}
			for (std::size_t index = 0U; index < after.size(); ++index)
				found[static_cast<std::ptrdiff_t>(index)] =
					static_cast<std::byte>(static_cast<unsigned char>(after[index]));
			return {};
		};
		if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
			return with_immediate_transaction(
				*store.implementation_->database,
				[&]() -> result<void>
				{
					auto rows = read_sqlite_publications(*store.implementation_->database,
														 sqlite_physical_format::current_v3);
					if (!rows)
						return unexpected(std::move(rows.error()));
					auto row =
						std::ranges::find(*rows,
										  publication_id,
										  [](const auto& value)
										  {
											  return std::string_view{value.record.publication_id};
										  });
					if (row == rows->end() || !row->payload_source)
						return unexpected(store_error("store.publication-not-found",
													  std::string{publication_id}));
					return rewrite_v3_payload_window_in_place(
						*store.implementation_->database, *row, before, after, occurrence);
				});
		auto selected = sqlite_statement::prepare(
			*store.implementation_->database,
			"SELECT payload FROM cxxlens_ng_publication WHERE publication_id=?1",
			false);
		if (!selected)
			return unexpected(std::move(selected.error()));
		if (auto bound = selected->bind_text(1, publication_id); !bound)
			return bound;
		if (selected->step() != sqlite_row)
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto payload = selected->column_blob(0);
		if (!payload)
			return unexpected(std::move(payload.error()));
		if (auto rewritten = rewrite_bytes(*payload); !rewritten)
			return rewritten;
		const auto checksum = content_digest(*payload);
		auto update = sqlite_statement::prepare(
			*store.implementation_->database,
			"UPDATE cxxlens_ng_publication SET checksum=?1,payload=?2 WHERE publication_id=?3",
			false);
		if (!update)
			return unexpected(std::move(update.error()));
		if (auto bound = update->bind_text(1, checksum); !bound)
			return bound;
		if (auto bound = update->bind_blob(2, *payload); !bound)
			return bound;
		if (auto bound = update->bind_text(3, publication_id); !bound)
			return bound;
		return update->expect_done();
	}
	// NOLINTEND(bugprone-easily-swappable-parameters)

	result<void>
	rewrite_publication_payload_schema_for_testing(snapshot_store& store,
												   const std::string_view publication_id,
												   const std::uint8_t payload_version)
	{
		if (!payload_schema_from_number(payload_version))
			return unexpected(store_error("store.corrupt", "test-payload-schema", "version"));
		const std::string version_text(1U, static_cast<char>('0' + payload_version));
		return rewrite_publication_payload_for_testing(
			store, publication_id, "test.payload-schema", version_text, 0U);
	}

	result<void> rewrite_publication_identity_field_for_testing(
		snapshot_store& store, const std::string_view publication_id, const std::string_view field)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		if (store.implementation_->database == nullptr)
			return unexpected(store_error("store.corrupt", "test-identity-rewrite", "backend"));
		const auto found = store.implementation_->publications.find(publication_id);
		if (found == store.implementation_->publications.end())
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto rewritten = snapshot_handle::data{*found->second};
		auto& record = rewritten.publication_record_value;
		const auto mutate_text = [](std::string& value)
		{
			if (value.empty())
				value = "tampered";
			else
				value.back() = value.back() == '0' ? '1' : '0';
		};
		if (field == "publication_id")
			mutate_text(record.publication_id);
		else if (field == "series_id")
			mutate_text(record.series_id);
		else if (field == "snapshot_id")
			mutate_text(record.snapshot_id);
		else if (field == "sequence")
			++record.sequence;
		else if (field == "parent" && record.parent_publication)
			mutate_text(*record.parent_publication);
		else
			return unexpected(store_error("store.corrupt", "test-identity-rewrite", "field"));

		const auto payload = encode_snapshot(rewritten);
		const auto checksum = content_digest(payload);
		if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
			return with_immediate_transaction(*store.implementation_->database,
											  [&]()
											  {
												  return replace_v3_publication(
													  *store.implementation_->database,
													  found->second->publication_record_value,
													  rewritten);
											  });
		return with_immediate_transaction(
			*store.implementation_->database,
			[&]() -> result<void>
			{
				if (auto updated = update_v2_publication_row(*store.implementation_->database,
															 publication_id,
															 record,
															 checksum,
															 payload);
					!updated)
					return updated;
				return update_v2_series_head_for_replacement(
					*store.implementation_->database, publication_id, record);
			});
	}

	result<std::string> rewrite_snapshot_version_for_testing(snapshot_store& store,
															 const std::string_view publication_id,
															 const std::string_view component,
															 const std::uint64_t wire_value,
															 const std::uint32_t semantic_value)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		if (store.implementation_->database == nullptr)
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "backend"));
		const auto found = store.implementation_->publications.find(publication_id);
		if (found == store.implementation_->publications.end())
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		if (component == "test.insert-noncommitted-payload")
		{
			if (semantic_value > std::numeric_limits<std::uint8_t>::max())
				return unexpected(
					store_error("store.corrupt", "test-noncommitted-payload", "version"));
			const auto schema =
				payload_schema_from_number(static_cast<std::uint8_t>(semantic_value));
			if (!schema)
				return unexpected(
					store_error("store.corrupt", "test-noncommitted-payload", "version"));

			auto rejected = snapshot_handle::data{*found->second};
			auto& record = rejected.publication_record_value;
			record.series_id += ".qualification-rejected";
			record.state = publication_state::rejected;
			record.corrupt = false;
			record.publication_id = publication_identity(record);
			if (auto valid = validate_publication_identity(record); !valid)
				return unexpected(std::move(valid.error()));
			const auto payload = encode_snapshot(rejected, *schema);
			const auto checksum = content_digest(payload);
			if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
			{
				auto inserted = with_immediate_transaction(
					*store.implementation_->database,
					[&]() -> result<void>
					{
						if (auto chunks = insert_v3_chunks(*store.implementation_->database,
														   record.publication_id,
														   record.physical_generation,
														   payload);
							!chunks)
							return chunks;
						return insert_v3_publication_row(*store.implementation_->database,
														 record,
														 checksum,
														 payload.size(),
														 payload_chunk_count(payload.size()));
					});
				if (!inserted)
					return unexpected(std::move(inserted.error()));
				return record.publication_id;
			}
			auto inserted = with_immediate_transaction(
				*store.implementation_->database,
				[&]()
				{
					return insert_v2_publication_row(
						*store.implementation_->database, record, checksum, payload, false);
				});
			if (!inserted)
				return unexpected(std::move(inserted.error()));
			return record.publication_id;
		}
		auto rewritten = snapshot_handle::data{*found->second};
		auto& version = rewritten.semantic_manifest.snapshot_semantics_version;
		std::size_t component_index{};
		if (component == "major")
			version.major = semantic_value;
		else if (component == "minor")
		{
			version.minor = semantic_value;
			component_index = 1U;
		}
		else if (component == "patch")
		{
			version.patch = semantic_value;
			component_index = 2U;
		}
		else
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "component"));
		rewritten.semantic_manifest.id = snapshot_identity(rewritten.semantic_manifest);
		auto& record = rewritten.publication_record_value;
		record.snapshot_id = rewritten.semantic_manifest.id;
		record.publication_id = publication_identity(record);

		auto payload = encode_snapshot(rewritten);
		binary_reader reader{payload};
		auto magic = reader.string();
		auto schema = reader.string();
		auto snapshot_id = reader.string();
		if (!magic || !schema || !snapshot_id || *magic != "cxxlens.ng-snapshot-payload.v5")
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "payload"));
		const auto component_offset = reader.offset() + component_index * 8U;
		if (payload.size() - component_offset < 8U)
			return unexpected(store_error("store.corrupt", "test-version-rewrite", "truncated"));
		for (std::size_t index = 0U; index < 8U; ++index)
		{
			const auto shift = static_cast<unsigned>((7U - index) * 8U);
			payload[component_offset + index] =
				static_cast<std::byte>((wire_value >> shift) & 0xffU);
		}
		const auto checksum = content_digest(payload);
		if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
		{
			auto replaced = with_immediate_transaction(
				*store.implementation_->database,
				[&]()
				{
					return replace_v3_publication_payload(*store.implementation_->database,
														  found->second->publication_record_value,
														  record,
														  payload);
				});
			if (!replaced)
				return unexpected(std::move(replaced.error()));
			return record.publication_id;
		}
		auto updated = with_immediate_transaction(
			*store.implementation_->database,
			[&]() -> result<void>
			{
				if (auto publication_updated =
						update_v2_publication_row(*store.implementation_->database,
												  publication_id,
												  record,
												  checksum,
												  payload);
					!publication_updated)
					return publication_updated;
				return update_v2_series_head_for_replacement(
					*store.implementation_->database, publication_id, record);
			});
		if (!updated)
			return unexpected(std::move(updated.error()));
		return record.publication_id;
	}

	result<std::string>
	insert_noncommitted_publication_for_testing(snapshot_store& store,
												const std::string_view source_publication_id,
												const std::uint8_t payload_version)
	{
		if (!payload_schema_from_number(payload_version))
			return unexpected(store_error("store.corrupt", "test-noncommitted-payload", "version"));
		return rewrite_snapshot_version_for_testing(
			store, source_publication_id, "test.insert-noncommitted-payload", 0U, payload_version);
	}

	result<std::string>
	rewrite_publication_counters_for_testing(snapshot_store& store,
											 const std::string_view publication_id,
											 const std::uint64_t sequence,
											 const std::uint64_t generation)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		const auto found = store.implementation_->publications.find(publication_id);
		if (found == store.implementation_->publications.end())
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto replacement = std::make_shared<snapshot_handle::data>(*found->second);
		const auto old_record = replacement->publication_record_value;
		auto& record = replacement->publication_record_value;
		record.sequence = sequence;
		record.physical_generation = generation;
		record.publication_id = publication_identity(record);
		if (auto valid = validate_publication_identity(record); !valid)
			return unexpected(std::move(valid.error()));
		auto token = std::make_shared<const std::uint64_t>(generation);
		replacement->generation_pin = token;

		if (store.implementation_->database != nullptr)
		{
			if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
			{
				if (auto begun = store.implementation_->database->execute("BEGIN IMMEDIATE;");
					!begun)
					return unexpected(std::move(begun.error()));
				if (auto replaced = replace_v3_publication(
						*store.implementation_->database, old_record, *replacement);
					!replaced)
				{
					(void)store.implementation_->database->execute("ROLLBACK;");
					return unexpected(std::move(replaced.error()));
				}
				if (auto committed = store.implementation_->database->execute("COMMIT;");
					!committed)
				{
					(void)store.implementation_->database->execute("ROLLBACK;");
					return unexpected(std::move(committed.error()));
				}
			}
			else
			{
				const auto payload = encode_snapshot(*replacement);
				const auto checksum = content_digest(payload);
				auto updated = with_immediate_transaction(
					*store.implementation_->database,
					[&]() -> result<void>
					{
						if (auto publication_updated =
								update_v2_publication_row(*store.implementation_->database,
														  publication_id,
														  record,
														  checksum,
														  payload);
							!publication_updated)
							return publication_updated;
						return update_v2_series_head_for_replacement(
							*store.implementation_->database, publication_id, record);
					});
				if (!updated)
					return unexpected(std::move(updated.error()));
			}
		}

		store.implementation_->records.erase(std::string{publication_id});
		store.implementation_->publications.erase(std::string{publication_id});
		store.implementation_->records[record.publication_id] = record;
		store.implementation_->publications[record.publication_id] = replacement;
		store.implementation_->heads[record.series_id] = record.publication_id;
		store.implementation_->generation = generation;
		store.implementation_->generation_tokens.push_back(token);
		return record.publication_id;
	}

	result<void> poison_rejected_generation_for_testing(snapshot_store& store,
														const std::string_view publication_id,
														const std::uint64_t generation)
	{
		std::scoped_lock lock{store.implementation_->mutex};
		if (store.implementation_->database == nullptr)
			return unexpected(store_error("store.corrupt", "test-counter-poison", "backend"));
		const auto found = store.implementation_->publications.find(publication_id);
		if (found == store.implementation_->publications.end())
			return unexpected(
				store_error("store.publication-not-found", std::string{publication_id}));
		auto rejected = snapshot_handle::data{*found->second};
		const auto old_record = rejected.publication_record_value;
		auto& record = rejected.publication_record_value;
		record.physical_generation = generation;
		record.state = publication_state::rejected;
		if (store.implementation_->sqlite_format == sqlite_physical_format::current_v3)
			return with_immediate_transaction(
				*store.implementation_->database,
				[&]() -> result<void>
				{
					if (auto replaced = replace_v3_publication(
							*store.implementation_->database, old_record, rejected);
						!replaced)
						return replaced;
					auto delete_head = sqlite_statement::prepare(
						*store.implementation_->database,
						"DELETE FROM cxxlens_ng_series_head WHERE current_publication=?1",
						true);
					if (!delete_head)
						return unexpected(std::move(delete_head.error()));
					if (auto bound = delete_head->bind_text(1, record.publication_id); !bound)
						return bound;
					return delete_head->expect_done();
				});
		const auto payload = encode_snapshot(rejected);
		const auto checksum = content_digest(payload);
		return with_immediate_transaction(
			*store.implementation_->database,
			[&]() -> result<void>
			{
				if (auto updated = update_v2_publication_row(*store.implementation_->database,
															 publication_id,
															 record,
															 checksum,
															 payload);
					!updated)
					return updated;
				auto delete_head = sqlite_statement::prepare(
					*store.implementation_->database,
					"DELETE FROM cxxlens_ng_series_head WHERE current_publication=?1",
					false);
				if (!delete_head)
					return unexpected(std::move(delete_head.error()));
				if (auto bound = delete_head->bind_text(1, publication_id); !bound)
					return bound;
				return delete_head->expect_done();
			});
	}

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
