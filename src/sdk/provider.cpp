#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cstring>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/provider.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk::provider
{
	namespace detail
	{
		struct relation_sink_registry
		{
			struct active_batch_state
			{
				std::uint64_t sink_id;
				std::string dependency_group_id;
				std::string atomic_output_group_id;
				std::string batch_id;
			};
			std::set<std::string, std::less<>> seen_batch_ids;
			std::optional<active_batch_state> active_batch;
			std::optional<error> violation;
			std::uint64_t next_sink_id{1U};
		};
	} // namespace detail

	namespace
	{
		constexpr std::size_t header_size = 104U;
		constexpr std::uint16_t required_extension_flag =
			static_cast<std::uint16_t>(frame_flag::required_extension);
		constexpr std::uint16_t optional_extension_flag =
			static_cast<std::uint16_t>(frame_flag::optional_extension);
		constexpr std::uint16_t compressed_payload_flag =
			static_cast<std::uint16_t>(frame_flag::compressed_payload);
		constexpr std::uint16_t end_of_stream_flag =
			static_cast<std::uint16_t>(frame_flag::end_of_stream);
		constexpr std::uint16_t known_flag_mask = required_extension_flag |
			optional_extension_flag | compressed_payload_flag | end_of_stream_flag;

		[[nodiscard]] bool has_flag(const std::uint16_t flags, const frame_flag flag) noexcept
		{
			return (flags & static_cast<std::uint16_t>(flag)) != 0U;
		}

		[[nodiscard]] error
		provider_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		template <std::unsigned_integral T>
		void append_big_endian(std::vector<std::byte>& output, const T value)
		{
			for (std::size_t index = sizeof(T); index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		template <std::unsigned_integral T>
		[[nodiscard]] T read_big_endian(const std::span<const std::byte> input,
										const std::size_t offset)
		{
			T output{};
			for (std::size_t index = 0U; index < sizeof(T); ++index)
				output = static_cast<T>((static_cast<std::uint64_t>(output) << 8U) |
										std::to_integer<std::uint64_t>(input[offset + index]));
			return output;
		}

		[[nodiscard]] std::array<std::byte, 32U> digest_bytes(std::string digest)
		{
			std::array<std::byte, 32U> output{};
			if (digest.starts_with("sha256:"))
				digest.erase(0U, 7U);
			for (std::size_t index = 0U; index < output.size(); ++index)
			{
				const auto value = digest.substr(index * 2U, 2U);
				output.at(index) = static_cast<std::byte>(std::stoul(value, nullptr, 16));
			}
			return output;
		}

		[[nodiscard]] std::vector<std::byte> bytes(const std::string_view text)
		{
			std::vector<std::byte> output;
			output.reserve(text.size());
			for (const auto byte : text)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		}

		using cbor_scalar = std::variant<std::uint64_t, std::string>;
		using cbor_fields = std::vector<std::pair<std::string, cbor_scalar>>;
		enum class cbor_major : std::uint8_t
		{
			unsigned_integer = 0U,
			text = 3U,
			map = 5U,
		};

		void append_cbor_head(std::vector<std::byte>& output,
							  const cbor_major major,
							  const std::uint64_t value)
		{
			const auto prefix = static_cast<std::uint8_t>(static_cast<std::uint8_t>(major) << 5U);
			if (value < 24U)
				output.push_back(static_cast<std::byte>(
					static_cast<std::uint8_t>(prefix | static_cast<std::uint8_t>(value))));
			else if (value <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(prefix | 24U)));
				output.push_back(static_cast<std::byte>(value));
			}
			else if (value <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(prefix | 25U)));
				append_big_endian(output, static_cast<std::uint16_t>(value));
			}
			else if (value <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(prefix | 26U)));
				append_big_endian(output, static_cast<std::uint32_t>(value));
			}
			else
			{
				output.push_back(static_cast<std::byte>(static_cast<std::uint8_t>(prefix | 27U)));
				append_big_endian(output, value);
			}
		}

		[[nodiscard]] std::vector<std::byte> cbor_text_value(const std::string_view value)
		{
			std::vector<std::byte> output;
			append_cbor_head(output, cbor_major::text, value.size());
			const auto encoded = bytes(value);
			output.insert(output.end(), encoded.begin(), encoded.end());
			return output;
		}

		[[nodiscard]] result<std::vector<std::byte>> encode_cbor_map(const cbor_fields& fields)
		{
			std::vector<std::tuple<std::vector<std::byte>, std::vector<std::byte>>> encoded;
			encoded.reserve(fields.size());
			std::set<std::string, std::less<>> keys;
			for (const auto& [key, value] : fields)
			{
				if (!keys.insert(key).second || !cxxlens::sdk::detail::valid_utf8(key))
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", key, "cbor-key"));
				auto encoded_value = std::visit(
					[](const auto& item)
					{
						std::vector<std::byte> output;
						using item_type = std::remove_cvref_t<decltype(item)>;
						if constexpr (std::same_as<item_type, std::string>)
						{
							append_cbor_head(output, cbor_major::text, item.size());
							const auto value_bytes = bytes(item);
							output.insert(output.end(), value_bytes.begin(), value_bytes.end());
						}
						else
							append_cbor_head(output, cbor_major::unsigned_integer, item);
						return output;
					},
					value);
				if (const auto* text = std::get_if<std::string>(&value);
					text != nullptr && !cxxlens::sdk::detail::valid_utf8(*text))
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", key, "cbor-utf8"));
				encoded.emplace_back(cbor_text_value(key), std::move(encoded_value));
			}
			std::ranges::sort(encoded,
							  [](const auto& left, const auto& right)
							  {
								  const auto& lhs = std::get<0>(left);
								  const auto& rhs = std::get<0>(right);
								  return std::pair{lhs.size(), lhs} < std::pair{rhs.size(), rhs};
							  });
			std::vector<std::byte> output;
			append_cbor_head(output, cbor_major::map, encoded.size());
			for (const auto& [key, value] : encoded)
			{
				output.insert(output.end(), key.begin(), key.end());
				output.insert(output.end(), value.begin(), value.end());
			}
			return output;
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::size_t>>
		decode_cbor_argument(const std::span<const std::byte> input,
							 const std::size_t offset,
							 const std::uint8_t additional)
		{
			if (additional < 24U)
				return std::pair<std::uint64_t, std::size_t>{additional, offset};
			const auto width = additional == 24U ? 1U
				: additional == 25U				 ? 2U
				: additional == 26U				 ? 4U
				: additional == 27U				 ? 8U
												 : 0U;
			if (width == 0U || offset > input.size() || width > input.size() - offset)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "argument"));
			std::uint64_t value{};
			for (std::size_t index = 0U; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			const auto minimum = width == 1U ? 24U : (std::uint64_t{1U} << (width * 4U));
			if (value < minimum)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		[[nodiscard]] result<std::pair<cbor_scalar, std::size_t>>
		decode_cbor_scalar(const std::span<const std::byte> input, const std::size_t offset)
		{
			if (offset >= input.size())
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto major = initial >> 5U;
			if (major != 0U && major != 3U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "scalar-type"));
			auto argument = decode_cbor_argument(input, offset + 1U, initial & 0x1fU);
			if (!argument)
				return cxxlens::sdk::unexpected(std::move(argument.error()));
			if (major == 0U)
				return std::pair{cbor_scalar{argument->first}, argument->second};
			if (argument->first > input.size() - argument->second)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "text-length"));
			const std::string text{reinterpret_cast<const char*>(input.data() + argument->second),
								   static_cast<std::size_t>(argument->first)};
			if (!cxxlens::sdk::detail::valid_utf8(text))
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "utf8"));
			return std::pair{cbor_scalar{text}, argument->second + text.size()};
		}

		[[nodiscard]] result<std::map<std::string, cbor_scalar, std::less<>>>
		decode_cbor_map(const std::span<const std::byte> input)
		{
			if (input.empty() || (std::to_integer<std::uint8_t>(input.front()) >> 5U) != 5U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "map"));
			const auto initial = std::to_integer<std::uint8_t>(input.front());
			auto count = decode_cbor_argument(input, 1U, initial & 0x1fU);
			if (!count)
				return cxxlens::sdk::unexpected(std::move(count.error()));
			std::map<std::string, cbor_scalar, std::less<>> output;
			std::vector<std::byte> previous_key;
			auto offset = count->second;
			for (std::uint64_t index = 0U; index < count->first; ++index)
			{
				const auto key_begin = offset;
				auto key = decode_cbor_scalar(input, offset);
				if (!key || !std::holds_alternative<std::string>(key->first))
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", "cbor", "map-key"));
				offset = key->second;
				std::vector encoded_key(input.begin() + static_cast<std::ptrdiff_t>(key_begin),
										input.begin() + static_cast<std::ptrdiff_t>(offset));
				if (!previous_key.empty() &&
					std::pair{encoded_key.size(), encoded_key} <=
						std::pair{previous_key.size(), previous_key})
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", "cbor", "map-order"));
				previous_key = std::move(encoded_key);
				auto value = decode_cbor_scalar(input, offset);
				if (!value)
					return cxxlens::sdk::unexpected(std::move(value.error()));
				offset = value->second;
				if (!output
						 .emplace(std::get<std::string>(std::move(key->first)),
								  std::move(value->first))
						 .second)
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", "cbor", "duplicate-key"));
			}
			if (offset != input.size())
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", "cbor", "trailing"));
			return output;
		}

		template <typename T>
		[[nodiscard]] const T*
		cbor_field(const std::map<std::string, cbor_scalar, std::less<>>& fields,
				   const std::string_view key)
		{
			const auto found = fields.find(key);
			return found == fields.end() ? nullptr : std::get_if<T>(&found->second);
		}

		template <std::unsigned_integral T>
		void append_little_endian(std::vector<std::byte>& output, const T value)
		{
			for (std::size_t index = 0U; index < sizeof(T); ++index)
				output.push_back(static_cast<std::byte>(value >> (index * 8U)));
		}

		template <std::unsigned_integral T>
		[[nodiscard]] T read_little_endian(const std::span<const std::byte> input,
										   const std::size_t offset)
		{
			T output{};
			for (std::size_t index = 0U; index < sizeof(T); ++index)
				output |= static_cast<T>(std::to_integer<std::uint64_t>(input[offset + index])
										 << (index * 8U));
			return output;
		}

		[[nodiscard]] std::string_view column_encoding(const scalar_kind kind)
		{
			switch (kind)
			{
				case scalar_kind::boolean:
					return "fixed-width-bool-u8";
				case scalar_kind::signed_integer:
					return "fixed-width-i64-le";
				case scalar_kind::unsigned_integer:
					return "fixed-width-u64-le";
				case scalar_kind::bytes:
				case scalar_kind::set:
					return "bytes-offsets-u32-le";
				case scalar_kind::open_symbol:
				case scalar_kind::closed_symbol:
					return "dictionary-index-u32-le";
				default:
					return "utf8-offsets-u32-le";
			}
		}

		[[nodiscard]] canonical_value digest_field(std::string name, canonical_value value)
		{
			return canonical_value::from_tuple(
				{canonical_value::from_string(std::move(name)), std::move(value)});
		}

		[[nodiscard]] canonical_value digest_text_field(std::string name,
														const std::string_view value)
		{
			return digest_field(std::move(name), canonical_value::from_string(std::string{value}));
		}

		[[nodiscard]] canonical_value digest_u64_field(std::string name, const std::uint64_t value)
		{
			std::vector<std::byte> encoded;
			encoded.reserve(sizeof(value));
			append_big_endian(encoded, value);
			return digest_field(std::move(name), canonical_value::from_bytes(std::move(encoded)));
		}

		[[nodiscard]] result<std::string> semantic_tuple_digest(const std::string_view domain,
																const canonical_value& value,
																const std::string_view field)
		{
			auto encoded = canonical_binary(value);
			if (!encoded)
				return cxxlens::sdk::unexpected(provider_error(
					"provider.columnar-invalid", std::string{field}, "semantic-tuple"));
			const std::string bytes{reinterpret_cast<const char*>(encoded->data()),
									encoded->size()};
			auto digest = semantic_digest(domain, bytes);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			return digest;
		}

		[[nodiscard]] canonical_value
		digest_cbor_field(const std::pair<std::string, cbor_scalar>& field)
		{
			return std::visit(
				[&](const auto& value)
				{
					using value_type = std::remove_cvref_t<decltype(value)>;
					if constexpr (std::same_as<value_type, std::string>)
						return digest_text_field(field.first, value);
					else
						return digest_u64_field(field.first, value);
				},
				field.second);
		}

		[[nodiscard]] result<std::string>
		semantic_fields_digest(const std::string_view domain,
							   const std::string_view schema,
							   const cbor_fields& fields,
							   std::vector<canonical_value> additional_fields,
							   const std::string_view field)
		{
			std::vector<canonical_value> projection;
			projection.reserve(1U + fields.size() + additional_fields.size());
			projection.push_back(digest_text_field("schema", schema));
			for (const auto& semantic_field : fields)
				projection.push_back(digest_cbor_field(semantic_field));
			projection.insert(projection.end(),
							  std::make_move_iterator(additional_fields.begin()),
							  std::make_move_iterator(additional_fields.end()));
			return semantic_tuple_digest(
				domain, canonical_value::from_tuple(std::move(projection)), field);
		}

		struct semantic_control
		{
			std::string digest;
			std::vector<std::byte> control;
		};

		[[nodiscard]] result<semantic_control>
		encode_semantic_control(const std::string_view domain,
								const std::string_view schema,
								cbor_fields fields,
								std::vector<canonical_value> additional_fields,
								std::string digest_key,
								const std::string_view field)
		{
			auto digest =
				semantic_fields_digest(domain, schema, fields, std::move(additional_fields), field);
			if (!digest)
				return cxxlens::sdk::unexpected(std::move(digest.error()));
			fields.emplace_back(std::move(digest_key), *digest);
			auto control = encode_cbor_map(fields);
			if (!control)
				return cxxlens::sdk::unexpected(std::move(control.error()));
			return semantic_control{std::move(*digest), std::move(*control)};
		}

		[[nodiscard]] cbor_fields chunk_semantic_fields(const column_chunk_record& value,
														const std::string_view payload_digest)
		{
			return {
				{"task_id", value.task_id},
				{"dependency_group_id", value.dependency_group_id},
				{"atomic_output_group_id", value.atomic_output_group_id},
				{"batch_id", value.batch_id},
				{"descriptor_id", value.descriptor_id},
				{"descriptor_digest", value.descriptor_digest},
				{"column_id", value.column_id},
				{"row_offset", value.row_offset},
				{"row_count", static_cast<std::uint64_t>(value.row_count)},
				{"chunk_index", value.chunk_index},
				{"encoding", value.encoding},
				{"payload_digest", std::string{payload_digest}},
			};
		}

		[[nodiscard]] result<std::string> column_chunk_digest(const column_chunk_record& value,
															  const std::string_view payload_digest)
		{
			return semantic_fields_digest("cxxlens.provider-column-chunk.v2",
										  "cxxlens.provider-column-chunk-digest.v2",
										  chunk_semantic_fields(value, payload_digest),
										  {},
										  value.column_id);
		}

		[[nodiscard]] cbor_fields batch_semantic_fields(const columnar_batch_end& value)
		{
			return {
				{"task_id", value.task_id},
				{"dependency_group_id", value.dependency_group_id},
				{"atomic_output_group_id", value.atomic_output_group_id},
				{"batch_id", value.batch_id},
				{"descriptor_id", value.descriptor_id},
				{"descriptor_digest", value.descriptor_digest},
				{"row_count", value.row_count},
				{"column_count", static_cast<std::uint64_t>(value.columns.size())},
				{"chunk_count", static_cast<std::uint64_t>(value.ordered_chunk_digests.size())},
			};
		}

		[[nodiscard]] std::vector<canonical_value>
		batch_additional_fields(const columnar_batch_end& value)
		{
			std::vector<canonical_value> columns;
			columns.reserve(value.columns.size());
			for (const auto& column : value.columns)
				columns.push_back(canonical_value::from_tuple({
					digest_text_field("column_id", column.column_id),
					digest_u64_field("payload_bytes", column.payload_bytes),
					digest_u64_field("chunk_count", column.chunk_count),
				}));
			std::vector<canonical_value> digests;
			digests.reserve(value.ordered_chunk_digests.size());
			for (const auto& digest : value.ordered_chunk_digests)
				digests.push_back(digest_text_field("chunk_digest", digest));
			return {
				digest_field("columns", canonical_value::from_tuple(std::move(columns))),
				digest_field("ordered_chunk_digests",
							 canonical_value::from_tuple(std::move(digests))),
			};
		}

		[[nodiscard]] result<std::string> batch_digest(const columnar_batch_end& value)
		{
			return semantic_fields_digest("cxxlens.provider-columnar-batch.v2",
										  "cxxlens.provider-columnar-batch-digest.v2",
										  batch_semantic_fields(value),
										  batch_additional_fields(value),
										  value.batch_id);
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value)
		{
			const auto hex = value.starts_with("sha256:")  ? value.substr(7U)
				: value.starts_with("semantic-v2:sha256:") ? value.substr(19U)
														   : std::string_view{};
			if (hex.size() != 64U)
				return false;
			return std::ranges::all_of(hex,
									   [](const char byte)
									   {
										   return std::isdigit(static_cast<unsigned char>(byte)) !=
											   0 ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] bool namespaced(const std::string_view value)
		{
			return !value.empty() && value.contains('.') && value.front() != '.' &&
				value.back() != '.' &&
				std::ranges::all_of(value,
									[](const char byte)
									{
										return std::islower(static_cast<unsigned char>(byte)) !=
											0 ||
											std::isdigit(static_cast<unsigned char>(byte)) != 0 ||
											byte == '.' || byte == '_' || byte == '-';
									});
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return cxxlens::sdk::detail::canonical_json_string(value);
		}

		[[nodiscard]] bool unique_nonempty(const std::vector<std::string>& values,
										   const bool require_value = false)
		{
			if (require_value && values.empty())
				return false;
			std::set<std::string, std::less<>> unique;
			for (const auto& value : values)
				if (value.empty() || !unique.insert(value).second)
					return false;
			return true;
		}

		[[nodiscard]] bool semantic_text(const std::string_view value)
		{
			return !value.empty() && cxxlens::sdk::detail::valid_utf8(value) &&
				std::ranges::none_of(value,
									 [](const char byte)
									 {
										 const auto code = static_cast<unsigned char>(byte);
										 return code < 0x20U || code == 0x7fU;
									 });
		}

		[[nodiscard]] bool stable_token(const std::string_view value)
		{
			return !value.empty() && value.front() >= 'a' && value.front() <= 'z' &&
				std::ranges::all_of(value.substr(1U),
									[](const char byte)
									{
										return (byte >= 'a' && byte <= 'z') ||
											(byte >= '0' && byte <= '9') || byte == '_' ||
											byte == '.' || byte == '-';
									});
		}

		[[nodiscard]] result<void>
		validate_descriptor_set(const std::span<const relation_descriptor> descriptors,
								const bool require_value,
								const std::string_view field)
		{
			if (require_value && descriptors.empty())
				return cxxlens::sdk::unexpected(
					provider_error("provider.task-invalid", std::string{field}, "empty"));
			const relation_descriptor* previous{};
			for (const auto& descriptor : descriptors)
			{
				if (auto valid = descriptor.validate(); !valid)
					return cxxlens::sdk::unexpected(std::move(valid.error()));
				if (previous != nullptr &&
					(previous->id == descriptor.id ||
					 std::tie(previous->id, previous->descriptor_digest) >=
						 std::tie(descriptor.id, descriptor.descriptor_digest)))
					return cxxlens::sdk::unexpected(provider_error("provider.task-invalid",
																   std::string{field},
																   "duplicate-conflict-or-order"));
				previous = &descriptor;
			}
			return {};
		}

		[[nodiscard]] std::string canonical_array(std::vector<std::string> values)
		{
			std::ranges::sort(values);
			std::ostringstream output;
			output << '[';
			for (std::size_t index = 0U; index < values.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << json_string(values[index]);
			}
			output << ']';
			return output.str();
		}

		[[nodiscard]] error control_metadata_error(std::string detail)
		{
			return provider_error(
				"provider.protocol-state-invalid", "control-metadata", std::move(detail));
		}

		[[nodiscard]] result<std::vector<std::byte>> encode_control_metadata_record(
			const std::string_view schema,
			const std::span<const std::pair<std::string_view, std::string_view>> fields)
		{
			std::vector<std::pair<std::string, cbor_scalar>> values;
			values.reserve(fields.size() + 1U);
			values.emplace_back("schema", std::string{schema});
			for (const auto& [name, value] : fields)
				values.emplace_back(std::string{name}, std::string{value});
			auto encoded = encode_cbor_map(values);
			if (!encoded)
				return cxxlens::sdk::unexpected(control_metadata_error(encoded.error().detail));
			return encoded;
		}

		[[nodiscard]] result<std::vector<std::string>>
		decode_control_metadata_record(const std::span<const std::byte> control,
									   const std::string_view schema,
									   const std::span<const std::string_view> field_names)
		{
			auto fields = decode_cbor_map(control);
			if (!fields || fields->size() != field_names.size() + 1U)
				return cxxlens::sdk::unexpected(control_metadata_error("record-shape"));
			const auto* actual_schema = cbor_field<std::string>(*fields, "schema");
			if (actual_schema == nullptr || *actual_schema != schema)
				return cxxlens::sdk::unexpected(control_metadata_error("record-schema"));
			std::vector<std::string> output;
			output.reserve(field_names.size());
			for (const auto name : field_names)
			{
				const auto* value = cbor_field<std::string>(*fields, name);
				if (value == nullptr)
					return cxxlens::sdk::unexpected(control_metadata_error("record-field"));
				output.push_back(*value);
			}
			return output;
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_control_metadata_records(const std::string_view schema,
										const std::span<const std::string_view> field_names,
										const std::span<const std::vector<std::string>> records)
		{
			if (field_names.empty() ||
				records.size() >
					(std::numeric_limits<std::size_t>::max() - 2U) / field_names.size())
				return cxxlens::sdk::unexpected(control_metadata_error("records-size"));
			std::vector<std::vector<std::string>> canonical_records{records.begin(), records.end()};
			std::ranges::sort(canonical_records);
			if (std::ranges::adjacent_find(canonical_records) != canonical_records.end())
				return cxxlens::sdk::unexpected(control_metadata_error("records-duplicate"));
			std::vector<std::pair<std::string, cbor_scalar>> fields;
			fields.reserve(2U + canonical_records.size() * field_names.size());
			fields.emplace_back("schema", std::string{schema});
			fields.emplace_back("record_count",
								static_cast<std::uint64_t>(canonical_records.size()));
			for (std::size_t record = 0U; record < canonical_records.size(); ++record)
			{
				if (canonical_records[record].size() != field_names.size())
					return cxxlens::sdk::unexpected(control_metadata_error("record-width"));
				for (std::size_t field = 0U; field < field_names.size(); ++field)
					fields.emplace_back(std::to_string(record) + "." +
											std::string{field_names[field]},
										canonical_records[record][field]);
			}
			auto encoded = encode_cbor_map(fields);
			if (!encoded)
				return cxxlens::sdk::unexpected(control_metadata_error(encoded.error().detail));
			return encoded;
		}

		[[nodiscard]] result<std::vector<std::vector<std::string>>>
		decode_control_metadata_records(const std::span<const std::byte> control,
										const std::string_view schema,
										const std::span<const std::string_view> field_names)
		{
			auto fields = decode_cbor_map(control);
			if (!fields || fields->size() < 2U || field_names.empty())
				return cxxlens::sdk::unexpected(control_metadata_error("records-shape"));
			const auto* actual_schema = cbor_field<std::string>(*fields, "schema");
			const auto* count = cbor_field<std::uint64_t>(*fields, "record_count");
			const auto available = fields->size() - 2U;
			if (actual_schema == nullptr || *actual_schema != schema || count == nullptr ||
				available % field_names.size() != 0U || *count != available / field_names.size())
				return cxxlens::sdk::unexpected(control_metadata_error("records-schema-or-count"));
			std::vector<std::vector<std::string>> output;
			output.reserve(static_cast<std::size_t>(*count));
			for (std::uint64_t record = 0U; record < *count; ++record)
			{
				std::vector<std::string> row;
				row.reserve(field_names.size());
				for (const auto name : field_names)
				{
					const auto key = std::to_string(record) + "." + std::string{name};
					const auto* value = cbor_field<std::string>(*fields, key);
					if (value == nullptr)
						return cxxlens::sdk::unexpected(control_metadata_error("records-field"));
					row.push_back(*value);
				}
				output.push_back(std::move(row));
			}
			if (!std::ranges::is_sorted(output) ||
				std::ranges::adjacent_find(output) != output.end())
				return cxxlens::sdk::unexpected(
					control_metadata_error("records-order-or-duplicate"));
			return output;
		}

		[[nodiscard]] std::string_view sandbox_name(const sandbox_assurance value) noexcept
		{
			switch (value)
			{
				case sandbox_assurance::none:
					return "none";
				case sandbox_assurance::best_effort:
					return "best_effort";
				case sandbox_assurance::enforced:
					return "enforced";
				case sandbox_assurance::certified:
					return "certified";
			}
			return "invalid";
		}

		[[nodiscard]] std::string_view source_name(const discovery_source value) noexcept
		{
			switch (value)
			{
				case discovery_source::explicit_path:
					return "explicit_path";
				case discovery_source::installation_manifest:
					return "installation_manifest";
				case discovery_source::project_config:
					return "project_config";
				case discovery_source::system_registry:
					return "system_registry";
			}
			return "invalid";
		}

		[[nodiscard]] std::string_view direction_name(const fallback_direction value) noexcept
		{
			switch (value)
			{
				case fallback_direction::upgrade:
					return "upgrade";
				case fallback_direction::downgrade:
					return "downgrade";
				case fallback_direction::same_version_rebuild:
					return "same_version_rebuild";
			}
			return "invalid";
		}

		[[nodiscard]] std::uint8_t source_rank(const discovery_source value) noexcept
		{
			return static_cast<std::uint8_t>(value);
		}

		[[nodiscard]] bool sandbox_satisfies(const sandbox_assurance achieved,
											 const sandbox_assurance required) noexcept
		{
			return is_valid(achieved) && is_valid(required) &&
				static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
		}

		[[nodiscard]] std::optional<sandbox_assurance>
		parse_sandbox(const std::string_view value) noexcept
		{
			if (value == "none")
				return sandbox_assurance::none;
			if (value == "best_effort")
				return sandbox_assurance::best_effort;
			if (value == "enforced")
				return sandbox_assurance::enforced;
			if (value == "certified")
				return sandbox_assurance::certified;
			return std::nullopt;
		}
	} // namespace

	result<std::vector<std::byte>> encode_frame(const frame& value, const protocol_limits limits)
	{
		if (value.control.size() > limits.max_control_bytes)
			return cxxlens::sdk::unexpected(
				provider_error("provider.oversized-control", "control"));
		if (value.payload.size() > limits.max_payload_bytes)
			return cxxlens::sdk::unexpected(
				provider_error("provider.oversized-payload", "payload"));
		const auto type = static_cast<std::uint16_t>(value.type);
		const bool optional_extension = has_flag(value.flags, frame_flag::optional_extension);
		if (value.protocol_major != limits.protocol_major)
			return cxxlens::sdk::unexpected(
				provider_error("provider.protocol-major-mismatch", "major"));
		if (value.protocol_minor < limits.minimum_minor ||
			value.protocol_minor > limits.maximum_minor)
			return cxxlens::sdk::unexpected(
				provider_error("provider.protocol-minor-mismatch", "minor"));
		if ((value.flags & ~known_flag_mask) != 0U ||
			(has_flag(value.flags, frame_flag::required_extension) && optional_extension) ||
			(optional_extension && has_flag(value.flags, frame_flag::end_of_stream)) ||
			(optional_extension && type <= static_cast<std::uint16_t>(message_type::close)))
			return cxxlens::sdk::unexpected(
				provider_error("provider.invalid-frame-flags", "flags"));
		if (has_flag(value.flags, frame_flag::required_extension))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-required-extension", "flags"));
		if (has_flag(value.flags, frame_flag::compressed_payload))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unsupported-compression", "flags"));
		if (has_flag(value.flags, frame_flag::end_of_stream) &&
			(limits.supported_flags & end_of_stream_flag) == 0U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.invalid-frame-flags", "end-of-stream"));
		if (type == 0U ||
			(type > static_cast<std::uint16_t>(message_type::close) && !optional_extension))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-message-type", "type"));

		std::vector<std::byte> output;
		output.reserve(header_size + value.control.size() + value.payload.size());
		for (const auto byte : std::string_view{"CXXP"})
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		append_big_endian(output, value.protocol_major);
		append_big_endian(output, value.protocol_minor);
		append_big_endian(output, type);
		append_big_endian(output, value.flags);
		append_big_endian(output, value.stream_id);
		append_big_endian(output, value.sequence);
		append_big_endian(output, static_cast<std::uint32_t>(value.control.size()));
		append_big_endian(output, static_cast<std::uint64_t>(value.payload.size()));
		const auto control_digest = digest_bytes(content_digest(value.control));
		const auto payload_digest = digest_bytes(content_digest(value.payload));
		output.insert(output.end(), control_digest.begin(), control_digest.end());
		output.insert(output.end(), payload_digest.begin(), payload_digest.end());
		output.insert(output.end(), value.control.begin(), value.control.end());
		output.insert(output.end(), value.payload.begin(), value.payload.end());
		return output;
	}

	result<frame> decode_frame(const std::span<const std::byte> input, const protocol_limits limits)
	{
		if (input.size() < header_size)
			return cxxlens::sdk::unexpected(provider_error("provider.truncated-stream", "header"));
		if (std::to_integer<char>(input[0]) != 'C' || std::to_integer<char>(input[1]) != 'X' ||
			std::to_integer<char>(input[2]) != 'X' || std::to_integer<char>(input[3]) != 'P')
			return cxxlens::sdk::unexpected(provider_error("provider.malformed-frame", "magic"));
		const auto protocol_major = read_big_endian<std::uint16_t>(input, 4U);
		const auto protocol_minor = read_big_endian<std::uint16_t>(input, 6U);
		const auto type = read_big_endian<std::uint16_t>(input, 8U);
		const auto flags = read_big_endian<std::uint16_t>(input, 10U);
		const auto control_length = read_big_endian<std::uint32_t>(input, 28U);
		const auto payload_length = read_big_endian<std::uint64_t>(input, 32U);
		if (control_length > limits.max_control_bytes || payload_length > limits.max_payload_bytes)
			return cxxlens::sdk::unexpected(provider_error("provider.oversized-frame", "length"));
		if (payload_length >
				std::numeric_limits<std::size_t>::max() - header_size - control_length ||
			input.size() != header_size + control_length + static_cast<std::size_t>(payload_length))
			return cxxlens::sdk::unexpected(provider_error("provider.truncated-stream", "length"));

		frame output;
		output.type = static_cast<message_type>(type);
		output.stream_id = read_big_endian<std::uint64_t>(input, 12U);
		output.sequence = read_big_endian<std::uint64_t>(input, 20U);
		output.control.assign(input.begin() + static_cast<std::ptrdiff_t>(header_size),
							  input.begin() +
								  static_cast<std::ptrdiff_t>(header_size + control_length));
		output.payload.assign(
			input.begin() + static_cast<std::ptrdiff_t>(header_size + control_length), input.end());
		output.protocol_major = protocol_major;
		output.protocol_minor = protocol_minor;
		output.flags = flags;
		const auto control_digest = digest_bytes(content_digest(output.control));
		const auto payload_digest = digest_bytes(content_digest(output.payload));
		if (!std::ranges::equal(control_digest, input.subspan(40U, 32U)) ||
			!std::ranges::equal(payload_digest, input.subspan(72U, 32U)))
			return cxxlens::sdk::unexpected(provider_error("provider.checksum-mismatch", "digest"));
		if (protocol_major != limits.protocol_major)
			return cxxlens::sdk::unexpected(
				provider_error("provider.protocol-major-mismatch", "major"));
		if (protocol_minor < limits.minimum_minor || protocol_minor > limits.maximum_minor)
			return cxxlens::sdk::unexpected(
				provider_error("provider.protocol-minor-mismatch", "minor"));
		if ((flags & ~known_flag_mask) != 0U)
			return cxxlens::sdk::unexpected(
				provider_error(has_flag(flags, frame_flag::required_extension)
								   ? "provider.unknown-required-extension"
								   : "provider.invalid-frame-flags",
							   "flags"));
		const bool required_extension = has_flag(flags, frame_flag::required_extension);
		const bool optional_extension = has_flag(flags, frame_flag::optional_extension);
		if ((required_extension && optional_extension) ||
			(optional_extension && has_flag(flags, frame_flag::end_of_stream)))
			return cxxlens::sdk::unexpected(
				provider_error("provider.invalid-frame-flags", "flags"));
		if (has_flag(flags, frame_flag::compressed_payload))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unsupported-compression", "flags"));
		if (has_flag(flags, frame_flag::end_of_stream) &&
			(limits.supported_flags & end_of_stream_flag) == 0U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.invalid-frame-flags", "end-of-stream"));
		if (type == 0U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-message-type", "type"));
		const bool known_type = type <= static_cast<std::uint16_t>(message_type::close);
		if (optional_extension && known_type)
			return cxxlens::sdk::unexpected(
				provider_error("provider.invalid-frame-flags", "optional-known-type"));
		if (required_extension)
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-required-extension", "type"));
		if (!known_type && !optional_extension)
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-message-type", "type"));
		return output;
	}

	result<std::vector<frame>> decode_frame_stream(const std::span<const std::byte> input,
												   const protocol_limits limits,
												   const std::uint64_t maximum_frames)
	{
		if (maximum_frames == 0U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.stream-invalid", "maximum_frames"));
		std::vector<frame> output;
		std::size_t offset{};
		while (offset < input.size())
		{
			if (output.size() >= maximum_frames)
				return cxxlens::sdk::unexpected(
					provider_error("provider.oversized-frame", "frame-count"));
			if (input.size() - offset < header_size)
				return cxxlens::sdk::unexpected(
					provider_error("provider.truncated-stream", "header"));
			const auto header = input.subspan(offset, header_size);
			const auto control_length = read_big_endian<std::uint32_t>(header, 28U);
			const auto payload_length = read_big_endian<std::uint64_t>(header, 32U);
			if (control_length > limits.max_control_bytes ||
				payload_length > limits.max_payload_bytes ||
				payload_length >
					std::numeric_limits<std::size_t>::max() - header_size - control_length)
				return cxxlens::sdk::unexpected(
					provider_error("provider.oversized-frame", "length"));
			const auto frame_size =
				header_size + control_length + static_cast<std::size_t>(payload_length);
			if (frame_size > input.size() - offset)
				return cxxlens::sdk::unexpected(
					provider_error("provider.truncated-stream", "frame"));
			auto decoded = decode_frame(input.subspan(offset, frame_size), limits);
			if (!decoded)
				return cxxlens::sdk::unexpected(std::move(decoded.error()));
			output.push_back(std::move(*decoded));
			offset += frame_size;
		}
		if (output.empty())
			return cxxlens::sdk::unexpected(provider_error("provider.truncated-stream", "empty"));
		return output;
	}

	result<std::string> decode_control_text(const std::span<const std::byte> control)
	{
		if (control.empty())
			return cxxlens::sdk::unexpected(provider_error("provider.malformed-frame", "control"));
		const auto initial = std::to_integer<std::uint8_t>(control.front());
		if ((initial & 0xe0U) != 0x60U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.malformed-frame", "control-type"));
		std::size_t offset{1U};
		std::uint64_t length = initial & 0x1fU;
		if (length == 24U)
		{
			if (control.size() < 2U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.truncated-stream", "control-length"));
			length = std::to_integer<std::uint8_t>(control[1U]);
			offset = 2U;
			if (length < 24U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.malformed-frame", "non-shortest-control"));
		}
		else if (length == 25U)
		{
			if (control.size() < 3U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.truncated-stream", "control-length"));
			length = read_big_endian<std::uint16_t>(control, 1U);
			offset = 3U;
			if (length <= std::numeric_limits<std::uint8_t>::max())
				return cxxlens::sdk::unexpected(
					provider_error("provider.malformed-frame", "non-shortest-control"));
		}
		else if (length == 26U)
		{
			if (control.size() < 5U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.truncated-stream", "control-length"));
			length = read_big_endian<std::uint32_t>(control, 1U);
			offset = 5U;
			if (length <= std::numeric_limits<std::uint16_t>::max())
				return cxxlens::sdk::unexpected(
					provider_error("provider.malformed-frame", "non-shortest-control"));
		}
		else if (length >= 27U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.malformed-frame", "control-length"));
		if (length != control.size() - offset)
			return cxxlens::sdk::unexpected(provider_error("provider.truncated-stream", "control"));
		std::string output;
		output.reserve(static_cast<std::size_t>(length));
		for (const auto value : control.subspan(offset))
			output.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
		if (!cxxlens::sdk::detail::valid_utf8(output))
			return cxxlens::sdk::unexpected(
				provider_error("provider.malformed-frame", "control-utf8"));
		return output;
	}

	result<std::vector<std::byte>> encode_control_text(const std::string_view text)
	{
		if (!cxxlens::sdk::detail::valid_utf8(text))
			return cxxlens::sdk::unexpected(
				provider_error("provider.malformed-frame", "control-utf8"));
		std::vector<std::byte> output;
		if (text.size() < 24U)
			output.push_back(static_cast<std::byte>(0x60U | text.size()));
		else if (text.size() <= std::numeric_limits<std::uint8_t>::max())
		{
			output.push_back(std::byte{0x78});
			output.push_back(static_cast<std::byte>(text.size()));
		}
		else if (text.size() <= std::numeric_limits<std::uint16_t>::max())
		{
			output.push_back(std::byte{0x79});
			append_big_endian(output, static_cast<std::uint16_t>(text.size()));
		}
		else
		{
			output.push_back(std::byte{0x7a});
			append_big_endian(output, static_cast<std::uint32_t>(text.size()));
		}
		const auto encoded = bytes(text);
		output.insert(output.end(), encoded.begin(), encoded.end());
		return output;
	}

	result<std::vector<std::byte>>
	encode_task_accepted_metadata(const task_accepted_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"provider_id", value.provider_id},
			std::pair<std::string_view, std::string_view>{"provider_version",
														  value.provider_version},
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id},
		};
		return encode_control_metadata_record("cxxlens.provider-control.task-accepted.v1", fields);
	}

	result<task_accepted_metadata>
	decode_task_accepted_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"provider_id"},
									std::string_view{"provider_version"},
									std::string_view{"task_id"}};
		auto values = decode_control_metadata_record(
			control, "cxxlens.provider-control.task-accepted.v1", fields);
		if (!values)
			return cxxlens::sdk::unexpected(std::move(values.error()));
		return task_accepted_metadata{
			std::move(values->at(0U)), std::move(values->at(1U)), std::move(values->at(2U))};
	}

	result<std::vector<std::byte>> encode_batch_begin_metadata(const batch_begin_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id},
			std::pair<std::string_view, std::string_view>{"descriptor_id", value.descriptor_id},
			std::pair<std::string_view, std::string_view>{"descriptor_digest",
														  value.descriptor_digest},
			std::pair<std::string_view, std::string_view>{"dependency_group_id",
														  value.dependency_group_id},
			std::pair<std::string_view, std::string_view>{"atomic_output_group_id",
														  value.atomic_output_group_id},
			std::pair<std::string_view, std::string_view>{"batch_id", value.batch_id},
		};
		return encode_control_metadata_record("cxxlens.provider-control.batch-begin.v1", fields);
	}

	result<batch_begin_metadata>
	decode_batch_begin_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"task_id"},
									std::string_view{"descriptor_id"},
									std::string_view{"descriptor_digest"},
									std::string_view{"dependency_group_id"},
									std::string_view{"atomic_output_group_id"},
									std::string_view{"batch_id"}};
		auto values = decode_control_metadata_record(
			control, "cxxlens.provider-control.batch-begin.v1", fields);
		if (!values)
			return cxxlens::sdk::unexpected(std::move(values.error()));
		return batch_begin_metadata{std::move(values->at(0U)),
									std::move(values->at(1U)),
									std::move(values->at(2U)),
									std::move(values->at(3U)),
									std::move(values->at(4U)),
									std::move(values->at(5U))};
	}

	result<std::vector<std::byte>>
	encode_coverage_metadata(const std::span<const coverage_unit> values)
	{
		constexpr std::array fields{std::string_view{"kind"},
									std::string_view{"id"},
									std::string_view{"state"},
									std::string_view{"reason"}};
		std::vector<std::vector<std::string>> records;
		records.reserve(values.size());
		for (const auto& value : values)
			records.push_back({value.kind, value.id, value.state, value.reason});
		return encode_control_metadata_records(
			"cxxlens.provider-control.coverage.v1", fields, records);
	}

	result<std::vector<coverage_unit>>
	decode_coverage_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"kind"},
									std::string_view{"id"},
									std::string_view{"state"},
									std::string_view{"reason"}};
		auto records = decode_control_metadata_records(
			control, "cxxlens.provider-control.coverage.v1", fields);
		if (!records)
			return cxxlens::sdk::unexpected(std::move(records.error()));
		std::vector<coverage_unit> output;
		output.reserve(records->size());
		for (auto& record : *records)
			output.push_back({std::move(record[0U]),
							  std::move(record[1U]),
							  std::move(record[2U]),
							  std::move(record[3U])});
		return output;
	}

	result<std::vector<std::byte>>
	encode_unresolved_metadata(const std::span<const unresolved_item> values)
	{
		constexpr std::array fields{
			std::string_view{"code"}, std::string_view{"subject"}, std::string_view{"detail"}};
		std::vector<std::vector<std::string>> records;
		records.reserve(values.size());
		for (const auto& value : values)
			records.push_back({value.code, value.subject, value.detail});
		return encode_control_metadata_records(
			"cxxlens.provider-control.unresolved.v1", fields, records);
	}

	result<std::vector<unresolved_item>>
	decode_unresolved_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{
			std::string_view{"code"}, std::string_view{"subject"}, std::string_view{"detail"}};
		auto records = decode_control_metadata_records(
			control, "cxxlens.provider-control.unresolved.v1", fields);
		if (!records)
			return cxxlens::sdk::unexpected(std::move(records.error()));
		std::vector<unresolved_item> output;
		output.reserve(records->size());
		for (auto& record : *records)
			output.push_back({std::move(record[0U]), std::move(record[1U]), std::move(record[2U])});
		return output;
	}

	result<std::vector<std::byte>>
	encode_evidence_metadata(const std::span<const evidence_item> values)
	{
		constexpr std::array fields{std::string_view{"kind"},
									std::string_view{"subject"},
									std::string_view{"producer"},
									std::string_view{"summary"}};
		std::vector<std::vector<std::string>> records;
		records.reserve(values.size());
		for (const auto& value : values)
			records.push_back({value.kind, value.subject, value.producer, value.summary});
		return encode_control_metadata_records(
			"cxxlens.provider-control.evidence.v1", fields, records);
	}

	result<std::vector<evidence_item>>
	decode_evidence_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"kind"},
									std::string_view{"subject"},
									std::string_view{"producer"},
									std::string_view{"summary"}};
		auto records = decode_control_metadata_records(
			control, "cxxlens.provider-control.evidence.v1", fields);
		if (!records)
			return cxxlens::sdk::unexpected(std::move(records.error()));
		std::vector<evidence_item> output;
		output.reserve(records->size());
		for (auto& record : *records)
			output.push_back({std::move(record[0U]),
							  std::move(record[1U]),
							  std::move(record[2U]),
							  std::move(record[3U])});
		return output;
	}

	result<std::vector<std::byte>>
	encode_task_complete_metadata(const task_complete_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id},
			std::pair<std::string_view, std::string_view>{"status", "complete"},
		};
		return encode_control_metadata_record("cxxlens.provider-control.task-complete.v1", fields);
	}

	result<task_complete_metadata>
	decode_task_complete_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"task_id"}, std::string_view{"status"}};
		auto values = decode_control_metadata_record(
			control, "cxxlens.provider-control.task-complete.v1", fields);
		if (!values || values->at(1U) != "complete")
			return cxxlens::sdk::unexpected(values ? control_metadata_error("complete-status")
												   : std::move(values.error()));
		return task_complete_metadata{std::move(values->at(0U))};
	}

	result<std::vector<std::byte>> encode_task_failed_metadata(const task_failed_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"error_code", value.error_code},
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id},
			std::pair<std::string_view, std::string_view>{"error_field", value.error_field},
		};
		return encode_control_metadata_record("cxxlens.provider-control.task-failed.v1", fields);
	}

	result<task_failed_metadata>
	decode_task_failed_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"error_code"},
									std::string_view{"task_id"},
									std::string_view{"error_field"}};
		auto values = decode_control_metadata_record(
			control, "cxxlens.provider-control.task-failed.v1", fields);
		if (!values)
			return cxxlens::sdk::unexpected(std::move(values.error()));
		return task_failed_metadata{
			std::move(values->at(0U)), std::move(values->at(1U)), std::move(values->at(2U))};
	}

	result<std::vector<std::byte>>
	encode_schema_negotiate_metadata(const schema_negotiate_metadata& value)
	{
		auto encoded = encode_cbor_map({
			{"schema", std::string{"cxxlens.provider-control.schema-negotiate.v1"}},
			{"protocol_schema", value.protocol_schema},
			{"protocol_minor", value.protocol_minor},
		});
		if (!encoded)
			return cxxlens::sdk::unexpected(control_metadata_error(encoded.error().detail));
		return encoded;
	}

	result<schema_negotiate_metadata>
	decode_schema_negotiate_metadata(const std::span<const std::byte> control)
	{
		auto fields = decode_cbor_map(control);
		if (!fields || fields->size() != 3U)
			return cxxlens::sdk::unexpected(control_metadata_error("schema-negotiate-shape"));
		const auto* schema = cbor_field<std::string>(*fields, "schema");
		const auto* protocol_schema = cbor_field<std::string>(*fields, "protocol_schema");
		const auto* protocol_minor = cbor_field<std::uint64_t>(*fields, "protocol_minor");
		if (schema == nullptr || *schema != "cxxlens.provider-control.schema-negotiate.v1" ||
			protocol_schema == nullptr || protocol_minor == nullptr)
			return cxxlens::sdk::unexpected(control_metadata_error("schema-negotiate-fields"));
		return schema_negotiate_metadata{*protocol_schema, *protocol_minor};
	}

	result<std::vector<std::byte>> encode_open_task_metadata(const open_task_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id},
			std::pair<std::string_view, std::string_view>{"task_input_digest",
														  value.task_input_digest},
			std::pair<std::string_view, std::string_view>{"normalized_invocation_digest",
														  value.normalized_invocation_digest},
			std::pair<std::string_view, std::string_view>{"toolchain_digest",
														  value.toolchain_digest},
			std::pair<std::string_view, std::string_view>{"environment_digest",
														  value.environment_digest},
		};
		return encode_control_metadata_record("cxxlens.provider-control.open-task.v1", fields);
	}

	result<open_task_metadata> decode_open_task_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"task_id"},
									std::string_view{"task_input_digest"},
									std::string_view{"normalized_invocation_digest"},
									std::string_view{"toolchain_digest"},
									std::string_view{"environment_digest"}};
		auto values = decode_control_metadata_record(
			control, "cxxlens.provider-control.open-task.v1", fields);
		if (!values)
			return cxxlens::sdk::unexpected(std::move(values.error()));
		return open_task_metadata{std::move(values->at(0U)),
								  std::move(values->at(1U)),
								  std::move(values->at(2U)),
								  std::move(values->at(3U)),
								  std::move(values->at(4U))};
	}

	result<std::vector<std::byte>> encode_credit_metadata(const credit_metadata& value)
	{
		auto encoded = encode_cbor_map({
			{"schema", std::string{"cxxlens.provider-control.credit.v1"}},
			{"bytes", value.bytes},
			{"frames", value.frames},
		});
		if (!encoded)
			return cxxlens::sdk::unexpected(control_metadata_error(encoded.error().detail));
		return encoded;
	}

	result<credit_metadata> decode_credit_metadata(const std::span<const std::byte> control)
	{
		auto fields = decode_cbor_map(control);
		if (!fields || fields->size() != 3U)
			return cxxlens::sdk::unexpected(control_metadata_error("credit-shape"));
		const auto* schema = cbor_field<std::string>(*fields, "schema");
		const auto* bytes_value = cbor_field<std::uint64_t>(*fields, "bytes");
		const auto* frames_value = cbor_field<std::uint64_t>(*fields, "frames");
		if (schema == nullptr || *schema != "cxxlens.provider-control.credit.v1" ||
			bytes_value == nullptr || frames_value == nullptr)
			return cxxlens::sdk::unexpected(control_metadata_error("credit-fields"));
		return credit_metadata{*bytes_value, *frames_value};
	}

	result<std::vector<std::byte>> encode_close_metadata(const close_metadata& value)
	{
		const std::array fields{
			std::pair<std::string_view, std::string_view>{"task_id", value.task_id}};
		return encode_control_metadata_record("cxxlens.provider-control.close.v1", fields);
	}

	result<close_metadata> decode_close_metadata(const std::span<const std::byte> control)
	{
		constexpr std::array fields{std::string_view{"task_id"}};
		auto values =
			decode_control_metadata_record(control, "cxxlens.provider-control.close.v1", fields);
		if (!values)
			return cxxlens::sdk::unexpected(std::move(values.error()));
		return close_metadata{std::move(values->front())};
	}

	result<std::vector<std::byte>> encode_host_transcript(const host_transcript_request& request)
	{
		const auto& expected = request.expectation;
		if (expected.provider_manifest.empty() || expected.task.task_id.empty() ||
			expected.task.task_id.contains('\0') ||
			!canonical_digest(expected.task.task_input_digest) ||
			!canonical_digest(expected.task.normalized_invocation_digest) ||
			!canonical_digest(expected.task.toolchain_digest) ||
			!canonical_digest(expected.task.environment_digest) ||
			content_digest(request.payload) != expected.task.task_input_digest ||
			request.credit.bytes == 0U || request.credit.frames == 0U)
			return cxxlens::sdk::unexpected(provider_error(
				"provider.host-transcript-invalid", expected.task.task_id, "request-binding"));
		auto hello = encode_control_text(expected.provider_manifest);
		auto schema = encode_schema_negotiate_metadata(
			{"cxxlens.provider-protocol.v1", expected.limits.maximum_minor});
		auto open = encode_open_task_metadata(expected.task);
		auto credit = encode_credit_metadata({request.credit.bytes, request.credit.frames});
		auto close = encode_close_metadata({expected.task.task_id});
		if (!hello || !schema || !open || !credit || !close)
			return cxxlens::sdk::unexpected(provider_error(
				"provider.host-transcript-invalid", expected.task.task_id, "control-encoding"));
		std::array frames{
			frame{message_type::hello_ack, 1U, 0U, std::move(*hello), {}},
			frame{message_type::schema_negotiate, 1U, 1U, std::move(*schema), {}},
			frame{message_type::open_task, 1U, 2U, std::move(*open), request.payload},
			frame{message_type::credit, 1U, 3U, std::move(*credit), {}},
			frame{message_type::close, 1U, 4U, std::move(*close), {}},
		};
		for (auto& value : frames)
		{
			value.protocol_major = expected.limits.protocol_major;
			value.protocol_minor = expected.limits.maximum_minor;
		}
		if (auto valid = validate_host_transcript(frames, expected); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		std::vector<std::byte> output;
		for (const auto& value : frames)
		{
			auto encoded = encode_frame(value, expected.limits);
			if (!encoded)
				return cxxlens::sdk::unexpected(std::move(encoded.error()));
			output.insert(output.end(), encoded->begin(), encoded->end());
		}
		return output;
	}

	result<validated_host_transcript>
	validate_host_transcript(const std::span<const frame> frames,
							 const host_transcript_expectation& expectation)
	{
		const auto fail = [&](const std::string_view detail)
		{
			return cxxlens::sdk::unexpected(provider_error(
				"provider.host-transcript-invalid", expectation.task.task_id, std::string{detail}));
		};
		if (frames.size() != 5U || expectation.provider_manifest.empty() ||
			expectation.task.task_id.empty() || expectation.task.task_id.contains('\0') ||
			!canonical_digest(expectation.task.task_input_digest) ||
			!canonical_digest(expectation.task.normalized_invocation_digest) ||
			!canonical_digest(expectation.task.toolchain_digest) ||
			!canonical_digest(expectation.task.environment_digest))
			return fail("expectation-or-count");
		constexpr std::array types{message_type::hello_ack,
								   message_type::schema_negotiate,
								   message_type::open_task,
								   message_type::credit,
								   message_type::close};
		for (std::size_t index = 0U; index < frames.size(); ++index)
		{
			const auto& value = frames[index];
			if (value.type != types[index] || value.stream_id != 1U || value.sequence != index ||
				value.flags != 0U || value.protocol_major != expectation.limits.protocol_major ||
				value.protocol_minor != expectation.limits.maximum_minor || value.control.empty() ||
				value.control.size() > expectation.limits.max_control_bytes ||
				value.payload.size() > expectation.limits.max_payload_bytes ||
				(index != 2U && !value.payload.empty()))
				return fail("frame-state");
		}
		auto hello = decode_control_text(frames[0U].control);
		auto schema = decode_schema_negotiate_metadata(frames[1U].control);
		auto task = decode_open_task_metadata(frames[2U].control);
		auto credit = decode_credit_metadata(frames[3U].control);
		auto close = decode_close_metadata(frames[4U].control);
		if (!hello || !schema || !task || !credit || !close ||
			*hello != expectation.provider_manifest ||
			schema->protocol_schema != "cxxlens.provider-protocol.v1" ||
			schema->protocol_minor != expectation.limits.maximum_minor ||
			*task != expectation.task ||
			content_digest(frames[2U].payload) != task->task_input_digest || credit->bytes == 0U ||
			credit->frames == 0U || close->task_id != task->task_id)
			return fail("control-or-binding");
		return validated_host_transcript{
			std::move(*task), {credit->bytes, credit->frames}, frames[2U].payload};
	}

	result<encoded_column_chunk> encode_column_chunk(const column_chunk_record& value,
													 const column_descriptor& column)
	{
		if (value.task_id.empty() || value.dependency_group_id.empty() ||
			value.atomic_output_group_id.empty() || value.batch_id.empty() ||
			value.descriptor_id.empty() || !canonical_digest(value.descriptor_digest) ||
			value.column_id != column.id || value.row_count == 0U ||
			value.cells.size() != value.row_count ||
			value.encoding != column_encoding(column.type.scalar))
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", value.column_id, "metadata"));
		const auto bitmap_size = (value.cells.size() + 7U) / 8U;
		std::vector<std::byte> validity(bitmap_size);
		std::vector<std::byte> unknown(bitmap_size);
		std::vector<std::byte> value_auxiliary;
		std::vector<std::byte> values;
		std::vector<std::byte> reason_offsets;
		std::vector<std::byte> reasons;
		const bool dictionary = value.encoding == "dictionary-index-u32-le";
		const bool variable = !dictionary && column.type.scalar != scalar_kind::boolean &&
			column.type.scalar != scalar_kind::signed_integer &&
			column.type.scalar != scalar_kind::unsigned_integer;
		std::map<std::string, std::uint32_t, std::less<>> dictionary_indexes;
		if (dictionary)
		{
			std::set<std::string, std::less<>> entries;
			for (const auto& cell : value.cells)
				if (cell.state == cell_state::present)
				{
					const auto* text =
						cell.value ? std::get_if<std::string>(&*cell.value) : nullptr;
					if (text == nullptr || !cxxlens::sdk::detail::valid_utf8(*text))
						return cxxlens::sdk::unexpected(provider_error(
							"provider.columnar-invalid", column.id, "dictionary-value"));
					entries.insert(*text);
				}
			append_little_endian(value_auxiliary, static_cast<std::uint32_t>(entries.size()));
			append_little_endian(value_auxiliary, std::uint32_t{});
			std::vector<std::byte> dictionary_bytes;
			for (const auto& entry : entries)
			{
				if (dictionary_indexes.size() > std::numeric_limits<std::uint32_t>::max())
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", column.id, "dictionary-count"));
				dictionary_indexes.emplace(entry,
										   static_cast<std::uint32_t>(dictionary_indexes.size()));
				const auto encoded = bytes(entry);
				dictionary_bytes.insert(dictionary_bytes.end(), encoded.begin(), encoded.end());
				if (dictionary_bytes.size() > std::numeric_limits<std::uint32_t>::max())
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", column.id, "dictionary-size"));
				append_little_endian(value_auxiliary,
									 static_cast<std::uint32_t>(dictionary_bytes.size()));
			}
			value_auxiliary.insert(
				value_auxiliary.end(), dictionary_bytes.begin(), dictionary_bytes.end());
		}
		if (variable)
			append_little_endian(value_auxiliary, std::uint32_t{});
		append_little_endian(reason_offsets, std::uint32_t{});
		for (std::size_t index = 0U; index < value.cells.size(); ++index)
		{
			const auto& cell = value.cells[index];
			if (cell.type != column.type)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "cell-type"));
			if (auto valid = cell.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			const auto mask = static_cast<std::byte>(1U << (index % 8U));
			if (cell.state == cell_state::present)
				validity[index / 8U] |= mask;
			else if (cell.state == cell_state::unknown)
				unknown[index / 8U] |= mask;

			if (column.type.scalar == scalar_kind::boolean)
				values.push_back(cell.state == cell_state::present && std::get<bool>(*cell.value)
									 ? std::byte{1U}
									 : std::byte{});
			else if (column.type.scalar == scalar_kind::signed_integer)
				append_little_endian(
					values,
					cell.state == cell_state::present
						? std::bit_cast<std::uint64_t>(std::get<std::int64_t>(*cell.value))
						: std::uint64_t{});
			else if (column.type.scalar == scalar_kind::unsigned_integer)
				append_little_endian(values,
									 cell.state == cell_state::present
										 ? std::get<std::uint64_t>(*cell.value)
										 : std::uint64_t{});
			else if (dictionary)
			{
				const auto index_value = cell.state == cell_state::present
					? dictionary_indexes.at(std::get<std::string>(*cell.value))
					: std::uint32_t{};
				append_little_endian(values, index_value);
			}
			else
			{
				if (cell.state == cell_state::present)
				{
					if (const auto* text = std::get_if<std::string>(&*cell.value))
					{
						const auto encoded = bytes(*text);
						values.insert(values.end(), encoded.begin(), encoded.end());
					}
					else
					{
						const auto& encoded = std::get<std::vector<std::byte>>(*cell.value);
						values.insert(values.end(), encoded.begin(), encoded.end());
					}
				}
				if (values.size() > std::numeric_limits<std::uint32_t>::max())
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", column.id, "value-size"));
				append_little_endian(value_auxiliary, static_cast<std::uint32_t>(values.size()));
			}
			if (cell.state == cell_state::unknown)
			{
				const auto encoded = bytes(*cell.unknown_reason);
				reasons.insert(reasons.end(), encoded.begin(), encoded.end());
			}
			if (reasons.size() > std::numeric_limits<std::uint32_t>::max())
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "reason-size"));
			append_little_endian(reason_offsets, static_cast<std::uint32_t>(reasons.size()));
		}
		const std::array sections{validity.size(),
								  unknown.size(),
								  value_auxiliary.size(),
								  values.size(),
								  reason_offsets.size(),
								  reasons.size()};
		if (std::ranges::any_of(sections,
								[](const std::size_t size)
								{
									return size > std::numeric_limits<std::uint32_t>::max();
								}))
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "section-size"));
		encoded_column_chunk output;
		output.payload = {std::byte{'C'},
						  std::byte{'X'},
						  std::byte{'C'},
						  std::byte{'C'},
						  std::byte{1U},
						  static_cast<std::byte>(column.type.scalar),
						  std::byte{},
						  std::byte{}};
		for (const auto size : sections)
			append_little_endian(output.payload, static_cast<std::uint32_t>(size));
		for (const auto* section :
			 {&validity, &unknown, &value_auxiliary, &values, &reason_offsets, &reasons})
			output.payload.insert(output.payload.end(), section->begin(), section->end());
		const auto payload_digest = content_digest(output.payload);
		auto semantic = encode_semantic_control("cxxlens.provider-column-chunk.v2",
												"cxxlens.provider-column-chunk-digest.v2",
												chunk_semantic_fields(value, payload_digest),
												{},
												"chunk_digest",
												column.id);
		if (!semantic)
			return cxxlens::sdk::unexpected(std::move(semantic.error()));
		output.chunk_digest = std::move(semantic->digest);
		if (!value.chunk_digest.empty() && value.chunk_digest != output.chunk_digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "chunk-digest"));
		output.control = std::move(semantic->control);
		return output;
	}

	// The public wire API deliberately takes two labeled byte spans: control then payload.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	result<column_chunk_record> decode_column_chunk(const std::span<const std::byte> control,
													const std::span<const std::byte> payload,
													const column_descriptor& column)
	{
		auto fields = decode_cbor_map(control);
		if (!fields || fields->size() != 13U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "control-fields"));
		const auto text = [&](const std::string_view key) -> const std::string*
		{
			return cbor_field<std::string>(*fields, key);
		};
		const auto number = [&](const std::string_view key) -> const std::uint64_t*
		{
			return cbor_field<std::uint64_t>(*fields, key);
		};
		const auto* task_id = text("task_id");
		const auto* dependency = text("dependency_group_id");
		const auto* atomic = text("atomic_output_group_id");
		const auto* batch_id = text("batch_id");
		const auto* descriptor_id = text("descriptor_id");
		const auto* descriptor_digest = text("descriptor_digest");
		const auto* column_id = text("column_id");
		const auto* encoding = text("encoding");
		const auto* payload_digest = text("payload_digest");
		const auto* chunk_digest = text("chunk_digest");
		const auto* row_offset = number("row_offset");
		const auto* row_count = number("row_count");
		const auto* chunk_index = number("chunk_index");
		if (task_id == nullptr || dependency == nullptr || atomic == nullptr ||
			batch_id == nullptr || descriptor_id == nullptr || descriptor_digest == nullptr ||
			column_id == nullptr || encoding == nullptr || payload_digest == nullptr ||
			chunk_digest == nullptr || row_offset == nullptr || row_count == nullptr ||
			chunk_index == nullptr || task_id->empty() || dependency->empty() || atomic->empty() ||
			batch_id->empty() || descriptor_id->empty() || !canonical_digest(*descriptor_digest) ||
			!canonical_digest(*payload_digest) || !canonical_digest(*chunk_digest) ||
			*column_id != column.id || *encoding != column_encoding(column.type.scalar) ||
			*row_count == 0U || *row_count > std::numeric_limits<std::uint32_t>::max())
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "control"));
		if (payload.size() < 32U || payload[0] != std::byte{'C'} || payload[1] != std::byte{'X'} ||
			payload[2] != std::byte{'C'} || payload[3] != std::byte{'C'} ||
			payload[4] != std::byte{1U} ||
			std::to_integer<std::uint8_t>(payload[5]) !=
				static_cast<std::uint8_t>(column.type.scalar) ||
			payload[6] != std::byte{} || payload[7] != std::byte{})
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "payload-header"));
		std::array<std::uint32_t, 6U> sizes{};
		std::size_t total{32U};
		for (std::size_t index = 0U; index < sizes.size(); ++index)
		{
			sizes.at(index) = read_little_endian<std::uint32_t>(payload, 8U + index * 4U);
			if (sizes.at(index) > payload.size() - std::min(total, payload.size()))
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "section-length"));
			total += sizes.at(index);
		}
		if (total != payload.size())
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "payload-length"));
		std::array<std::span<const std::byte>, 6U> sections;
		std::size_t section_offset{32U};
		for (std::size_t index = 0U; index < sections.size(); ++index)
		{
			sections.at(index) = payload.subspan(section_offset, sizes.at(index));
			section_offset += sizes.at(index);
		}
		const auto rows = static_cast<std::uint32_t>(*row_count);
		const auto bitmap_size = (static_cast<std::size_t>(rows) + 7U) / 8U;
		const bool dictionary = *encoding == "dictionary-index-u32-le";
		const bool variable = !dictionary && column.type.scalar != scalar_kind::boolean &&
			column.type.scalar != scalar_kind::signed_integer &&
			column.type.scalar != scalar_kind::unsigned_integer;
		const bool fixed = !variable && !dictionary;
		const auto fixed_width = column.type.scalar == scalar_kind::boolean ? 1U : 8U;
		if (sections[0].size() != bitmap_size || sections[1].size() != bitmap_size ||
			(variable && sections[2].size() != (static_cast<std::size_t>(rows) + 1U) * 4U) ||
			(fixed && !sections[2].empty()) ||
			(fixed && sections[3].size() != static_cast<std::size_t>(rows) * fixed_width) ||
			(dictionary && sections[2].size() < 8U) ||
			(dictionary && sections[3].size() != static_cast<std::size_t>(rows) * 4U) ||
			sections[4].size() != (static_cast<std::size_t>(rows) + 1U) * 4U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "shape"));
		const auto unused_bits = static_cast<std::uint8_t>(rows % 8U);
		if (unused_bits != 0U)
		{
			const auto used_mask = static_cast<std::uint8_t>((1U << unused_bits) - 1U);
			if ((std::to_integer<std::uint8_t>(sections[0].back()) & ~used_mask) != 0U ||
				(std::to_integer<std::uint8_t>(sections[1].back()) & ~used_mask) != 0U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "unused-validity-bits"));
		}
		struct offset_shape
		{
			std::size_t data_size;
			std::size_t entry_count;
		};
		auto offsets = [&](const std::span<const std::byte> encoded,
						   const offset_shape shape) -> result<std::vector<std::uint32_t>>
		{
			std::vector<std::uint32_t> output;
			output.reserve(shape.entry_count + 1U);
			for (std::size_t index = 0U; index <= shape.entry_count; ++index)
			{
				const auto value = read_little_endian<std::uint32_t>(encoded, index * 4U);
				if ((!output.empty() && value < output.back()) || value > shape.data_size)
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", column.id, "offset"));
				output.push_back(value);
			}
			if (output.front() != 0U || output.back() != shape.data_size)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "offset-end"));
			return output;
		};
		result<std::vector<std::uint32_t>> value_offsets{std::vector<std::uint32_t>{}};
		if (variable)
			value_offsets = offsets(sections[2], {sections[3].size(), rows});
		std::vector<std::string> dictionary_entries;
		if (dictionary)
		{
			const auto count = read_little_endian<std::uint32_t>(sections[2], 0U);
			const auto offset_bytes = (static_cast<std::size_t>(count) + 1U) * 4U;
			if (offset_bytes > sections[2].size() - 4U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "dictionary-shape"));
			const auto dictionary_bytes = sections[2].subspan(4U + offset_bytes);
			auto dictionary_offsets =
				offsets(sections[2].subspan(4U, offset_bytes), {dictionary_bytes.size(), count});
			if (!dictionary_offsets || dictionary_offsets->size() != count + 1U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "dictionary-offsets"));
			dictionary_entries.reserve(count);
			for (std::size_t index = 0U; index < count; ++index)
			{
				const auto begin = dictionary_offsets->at(index);
				const auto end = dictionary_offsets->at(index + 1U);
				std::string entry{reinterpret_cast<const char*>(dictionary_bytes.data() + begin),
								  end - begin};
				if (!cxxlens::sdk::detail::valid_utf8(entry) ||
					(!dictionary_entries.empty() && entry <= dictionary_entries.back()))
					return cxxlens::sdk::unexpected(
						provider_error("provider.columnar-invalid", column.id, "dictionary-order"));
				dictionary_entries.push_back(std::move(entry));
			}
		}
		auto unknown_offsets = offsets(sections[4], {sections[5].size(), rows});
		if (!value_offsets || !unknown_offsets)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "offsets"));
		column_chunk_record output{*task_id,
								   *dependency,
								   *atomic,
								   *batch_id,
								   *descriptor_id,
								   *descriptor_digest,
								   *column_id,
								   *row_offset,
								   rows,
								   *chunk_index,
								   *encoding,
								   {},
								   *chunk_digest};
		output.cells.reserve(rows);
		for (std::size_t index = 0U; index < rows; ++index)
		{
			const auto mask = static_cast<std::uint8_t>(1U << (index % 8U));
			const bool present =
				(std::to_integer<std::uint8_t>(sections[0][index / 8U]) & mask) != 0U;
			const bool unknown_state =
				(std::to_integer<std::uint8_t>(sections[1][index / 8U]) & mask) != 0U;
			if (present && unknown_state)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "state-bitmap"));
			if (!present && !unknown_state && !column.type.optional)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "required-absent"));
			const auto value_begin = variable ? value_offsets->at(index) : 0U;
			const auto value_end = variable ? value_offsets->at(index + 1U) : 0U;
			const auto reason_begin = unknown_offsets->at(index);
			const auto reason_end = unknown_offsets->at(index + 1U);
			if ((variable && !present && value_begin != value_end) ||
				(!unknown_state && reason_begin != reason_end))
				return cxxlens::sdk::unexpected(provider_error(
					"provider.columnar-invalid", column.id, "noncanonical-null-storage"));
			if (fixed && !present)
			{
				const auto width = column.type.scalar == scalar_kind::boolean ? 1U : 8U;
				const auto encoded = sections[3].subspan(index * width, width);
				if (std::ranges::any_of(encoded,
										[](const std::byte byte)
										{
											return byte != std::byte{};
										}))
					return cxxlens::sdk::unexpected(provider_error(
						"provider.columnar-invalid", column.id, "noncanonical-null-storage"));
			}
			if (present)
			{
				scalar_value decoded;
				if (column.type.scalar == scalar_kind::boolean)
				{
					const auto byte = std::to_integer<std::uint8_t>(sections[3][index]);
					if (byte > 1U)
						return cxxlens::sdk::unexpected(
							provider_error("provider.columnar-invalid", column.id, "boolean"));
					decoded = byte != 0U;
				}
				else if (column.type.scalar == scalar_kind::signed_integer)
					decoded = std::bit_cast<std::int64_t>(
						read_little_endian<std::uint64_t>(sections[3], index * 8U));
				else if (column.type.scalar == scalar_kind::unsigned_integer)
					decoded = read_little_endian<std::uint64_t>(sections[3], index * 8U);
				else if (dictionary)
				{
					const auto dictionary_index =
						read_little_endian<std::uint32_t>(sections[3], index * 4U);
					if (dictionary_index >= dictionary_entries.size())
						return cxxlens::sdk::unexpected(provider_error(
							"provider.columnar-invalid", column.id, "dictionary-index"));
					decoded = dictionary_entries[dictionary_index];
				}
				else
				{
					const auto encoded = sections[3].subspan(value_begin, value_end - value_begin);
					if (column.type.scalar == scalar_kind::bytes ||
						column.type.scalar == scalar_kind::set)
						decoded = std::vector<std::byte>{encoded.begin(), encoded.end()};
					else
					{
						std::string text_value{reinterpret_cast<const char*>(encoded.data()),
											   encoded.size()};
						if (!cxxlens::sdk::detail::valid_utf8(text_value))
							return cxxlens::sdk::unexpected(provider_error(
								"provider.columnar-invalid", column.id, "value-utf8"));
						decoded = std::move(text_value);
					}
				}
				output.cells.push_back({column.type, cell_state::present, std::move(decoded), {}});
			}
			else if (unknown_state)
			{
				std::string reason{reinterpret_cast<const char*>(sections[5].data() + reason_begin),
								   reason_end - reason_begin};
				output.cells.push_back(detached_cell::unknown(column.type, std::move(reason)));
			}
			else
				output.cells.push_back(detached_cell::absent(column.type));
			if (dictionary && !present &&
				read_little_endian<std::uint32_t>(sections[3], index * 4U) != 0U)
				return cxxlens::sdk::unexpected(provider_error(
					"provider.columnar-invalid", column.id, "noncanonical-null-storage"));
			if (auto valid = output.cells.back().validate(); !valid)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", column.id, "decoded-cell"));
		}
		const auto actual_payload_digest = content_digest(payload);
		if (*payload_digest != actual_payload_digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "payload-digest"));
		auto expected_chunk = column_chunk_digest(output, actual_payload_digest);
		if (!expected_chunk)
			return cxxlens::sdk::unexpected(std::move(expected_chunk.error()));
		if (*chunk_digest != *expected_chunk)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", column.id, "chunk-digest"));
		return output;
	}

	// The public wire API deliberately takes two labeled byte spans: control then payload.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	result<column_chunk_record> decode_column_chunk(const std::span<const std::byte> control,
													const std::span<const std::byte> payload,
													const relation_descriptor& descriptor)
	{
		auto fields = decode_cbor_map(control);
		if (!fields)
			return cxxlens::sdk::unexpected(std::move(fields.error()));
		const auto* column_id = cbor_field<std::string>(*fields, "column_id");
		if (column_id == nullptr)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", descriptor.id, "column-id"));
		const auto column =
			std::ranges::find(descriptor.columns, *column_id, &column_descriptor::id);
		if (column == descriptor.columns.end())
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", *column_id, "unknown-column"));
		return decode_column_chunk(control, payload, *column);
	}

	std::string columnar_batch_digest(const columnar_batch_end& value)
	{
		auto digest = batch_digest(value);
		return digest ? std::move(*digest) : std::string{};
	}

	result<encoded_columnar_batch_end> encode_columnar_batch_end(const columnar_batch_end& value)
	{
		const bool empty_batch = value.row_count == 0U;
		auto semantic = encode_semantic_control("cxxlens.provider-columnar-batch.v2",
												"cxxlens.provider-columnar-batch-digest.v2",
												batch_semantic_fields(value),
												batch_additional_fields(value),
												"batch_digest",
												value.batch_id);
		if (value.task_id.empty() || value.dependency_group_id.empty() ||
			value.atomic_output_group_id.empty() || value.batch_id.empty() ||
			value.descriptor_id.empty() || !canonical_digest(value.descriptor_digest) ||
			value.columns.empty() || empty_batch != value.ordered_chunk_digests.empty() ||
			!semantic || value.batch_digest != semantic->digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", value.batch_id, "batch-end"));
		encoded_columnar_batch_end output;
		output.payload = {std::byte{'C'},
						  std::byte{'X'},
						  std::byte{'B'},
						  std::byte{'E'},
						  std::byte{1U},
						  std::byte{},
						  std::byte{},
						  std::byte{}};
		std::set<std::string, std::less<>> column_ids;
		std::uint64_t summarized_chunks{};
		for (const auto& column : value.columns)
		{
			if (column.column_id.empty() ||
				column.column_id.size() > std::numeric_limits<std::uint16_t>::max() ||
				!cxxlens::sdk::detail::valid_utf8(column.column_id) ||
				empty_batch != (column.chunk_count == 0U) ||
				(empty_batch && column.payload_bytes != 0U) ||
				!column_ids.insert(column.column_id).second ||
				column.chunk_count > std::numeric_limits<std::uint64_t>::max() - summarized_chunks)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", value.batch_id, "column-summary"));
			summarized_chunks += column.chunk_count;
			append_little_endian(output.payload,
								 static_cast<std::uint16_t>(column.column_id.size()));
			const auto id = bytes(column.column_id);
			output.payload.insert(output.payload.end(), id.begin(), id.end());
			append_little_endian(output.payload, column.payload_bytes);
			append_little_endian(output.payload, column.chunk_count);
		}
		if (summarized_chunks != value.ordered_chunk_digests.size())
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", value.batch_id, "chunk-count"));
		for (const auto& digest : value.ordered_chunk_digests)
		{
			if (!canonical_digest(digest) ||
				digest.size() > std::numeric_limits<std::uint16_t>::max())
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", value.batch_id, "chunk-summary"));
			append_little_endian(output.payload, static_cast<std::uint16_t>(digest.size()));
			const auto encoded = bytes(digest);
			output.payload.insert(output.payload.end(), encoded.begin(), encoded.end());
		}
		output.control = std::move(semantic->control);
		return output;
	}

	// The public wire API deliberately takes two labeled byte spans: control then payload.
	// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
	result<columnar_batch_end> decode_columnar_batch_end(const std::span<const std::byte> control,
														 const std::span<const std::byte> payload)
	{
		auto fields = decode_cbor_map(control);
		if (!fields || fields->size() != 10U || payload.size() < 8U ||
			payload[0] != std::byte{'C'} || payload[1] != std::byte{'X'} ||
			payload[2] != std::byte{'B'} || payload[3] != std::byte{'E'} ||
			payload[4] != std::byte{1U} || payload[5] != std::byte{} || payload[6] != std::byte{} ||
			payload[7] != std::byte{})
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", "batch-end", "header"));
		const auto text = [&](const std::string_view key) -> const std::string*
		{
			return cbor_field<std::string>(*fields, key);
		};
		const auto number = [&](const std::string_view key) -> const std::uint64_t*
		{
			return cbor_field<std::uint64_t>(*fields, key);
		};
		const auto* column_count = number("column_count");
		const auto* chunk_count = number("chunk_count");
		const auto* row_count = number("row_count");
		if (column_count == nullptr || chunk_count == nullptr || row_count == nullptr ||
			*column_count == 0U || (*row_count == 0U) != (*chunk_count == 0U))
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", "batch-end", "counts"));
		columnar_batch_end output;
		const auto assign = [&](std::string& target, const std::string_view key) -> bool
		{
			const auto* value = text(key);
			if (value == nullptr || value->empty())
				return false;
			target = *value;
			return true;
		};
		if (!assign(output.task_id, "task_id") ||
			!assign(output.dependency_group_id, "dependency_group_id") ||
			!assign(output.atomic_output_group_id, "atomic_output_group_id") ||
			!assign(output.batch_id, "batch_id") ||
			!assign(output.descriptor_id, "descriptor_id") ||
			!assign(output.descriptor_digest, "descriptor_digest") ||
			!assign(output.batch_digest, "batch_digest"))
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", "batch-end", "fields"));
		if (!canonical_digest(output.descriptor_digest) || !canonical_digest(output.batch_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", output.batch_id, "digest"));
		output.row_count = *row_count;
		std::size_t offset{8U};
		std::set<std::string, std::less<>> column_ids;
		std::uint64_t summarized_chunks{};
		for (std::uint64_t index = 0U; index < *column_count; ++index)
		{
			if (offset > payload.size() || payload.size() - offset < 2U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "column-length"));
			const auto length = read_little_endian<std::uint16_t>(payload, offset);
			offset += 2U;
			if (length == 0U || payload.size() - offset < static_cast<std::size_t>(length) + 16U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "column-entry"));
			std::string id{reinterpret_cast<const char*>(payload.data() + offset), length};
			offset += length;
			const auto payload_bytes = read_little_endian<std::uint64_t>(payload, offset);
			offset += 8U;
			const auto chunks = read_little_endian<std::uint64_t>(payload, offset);
			offset += 8U;
			if (!cxxlens::sdk::detail::valid_utf8(id) || (*row_count == 0U) != (chunks == 0U) ||
				(*row_count == 0U && payload_bytes != 0U) || !column_ids.insert(id).second ||
				chunks > std::numeric_limits<std::uint64_t>::max() - summarized_chunks)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "column-summary"));
			summarized_chunks += chunks;
			output.columns.push_back({std::move(id), payload_bytes, chunks});
		}
		if (summarized_chunks != *chunk_count)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", output.batch_id, "chunk-count"));
		for (std::uint64_t index = 0U; index < *chunk_count; ++index)
		{
			if (offset > payload.size() || payload.size() - offset < 2U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "digest-length"));
			const auto length = read_little_endian<std::uint16_t>(payload, offset);
			offset += 2U;
			if (length == 0U || payload.size() - offset < length)
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "digest-entry"));
			output.ordered_chunk_digests.emplace_back(
				reinterpret_cast<const char*>(payload.data() + offset), length);
			offset += length;
			if (!canonical_digest(output.ordered_chunk_digests.back()))
				return cxxlens::sdk::unexpected(
					provider_error("provider.columnar-invalid", output.batch_id, "chunk-digest"));
		}
		auto expected_batch_digest = batch_digest(output);
		if (offset != payload.size() || !expected_batch_digest ||
			output.batch_digest != *expected_batch_digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", output.batch_id, "batch-digest"));
		return output;
	}

	protocol_writer::protocol_writer(frame_sink& sink, const protocol_limits limits)
		: sink_{&sink}, limits_{limits}
	{
	}

	void protocol_writer::grant_credit(const protocol_credit amount) noexcept
	{
		credit_bytes_ = amount.bytes > std::numeric_limits<std::uint64_t>::max() - credit_bytes_
			? std::numeric_limits<std::uint64_t>::max()
			: credit_bytes_ + amount.bytes;
		credit_frames_ = amount.frames > std::numeric_limits<std::uint64_t>::max() - credit_frames_
			? std::numeric_limits<std::uint64_t>::max()
			: credit_frames_ + amount.frames;
	}

	result<void> protocol_writer::send(const message_type type,
									   const std::span<const std::byte> control,
									   const std::span<const std::byte> payload,
									   const std::uint16_t flags)
	{
		frame value{type,
					stream_id_,
					sequence_,
					{control.begin(), control.end()},
					{payload.begin(), payload.end()},
					limits_.protocol_major,
					limits_.maximum_minor,
					flags};
		auto encoded = encode_frame(value, limits_);
		if (!encoded)
			return cxxlens::sdk::unexpected(std::move(encoded.error()));
		if (credit_frames_ == 0U || credit_bytes_ < encoded->size())
			return cxxlens::sdk::unexpected(provider_error("provider.backpressure", "credit"));
		if (auto written = sink_->write(*encoded); !written)
			return written;
		credit_bytes_ -= encoded->size();
		--credit_frames_;
		++sequence_;
		return {};
	}

	std::uint64_t protocol_writer::remaining_bytes() const noexcept
	{
		return credit_bytes_;
	}
	std::uint64_t protocol_writer::remaining_frames() const noexcept
	{
		return credit_frames_;
	}

	result<void> provider_session::validate() const
	{
		if (!namespaced(provider_id) || provider_version.major == 0U ||
			!canonical_digest(provider_semantic_contract_digest) || !stable_token(input_stage) ||
			!stable_token(output_stage) || !unique_nonempty(interpretation_domains, true) ||
			!std::ranges::is_sorted(interpretation_domains) ||
			!std::ranges::all_of(interpretation_domains, namespaced))
			return cxxlens::sdk::unexpected(
				provider_error("provider.task-invalid", "provider-session", "identity"));
		if (auto valid = validate_descriptor_set(offered_outputs, true, "offered_outputs"); !valid)
			return valid;
		return validate_descriptor_set(required_relations, false, "required_relations");
	}

	result<std::vector<std::byte>> task::canonical_projection() const
	{
		if (auto valid = session.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (auto valid = project.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (auto valid = validate_descriptor_set(outputs, true, "outputs"); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (!semantic_text(condition) || !namespaced(interpretation) ||
			!std::ranges::binary_search(session.interpretation_domains, interpretation) ||
			!unique_nonempty(dependency_groups, true) ||
			!std::ranges::is_sorted(dependency_groups) ||
			!std::ranges::all_of(dependency_groups, semantic_text))
			return cxxlens::sdk::unexpected(
				provider_error("provider.task-invalid", "task-authority", "grammar-or-order"));

		const auto descriptor_values = [](const std::span<const relation_descriptor> descriptors)
		{
			std::vector<canonical_value> values;
			values.reserve(descriptors.size());
			for (const auto& descriptor : descriptors)
				values.push_back(canonical_value::from_tuple({
					canonical_value::from_string(descriptor.id),
					canonical_value::from_string(descriptor.descriptor_digest),
				}));
			return canonical_value::from_tuple(std::move(values));
		};
		const auto string_values = [](const std::span<const std::string> strings)
		{
			std::vector<canonical_value> values;
			values.reserve(strings.size());
			for (const auto& value : strings)
				values.push_back(canonical_value::from_string(value));
			return canonical_value::from_tuple(std::move(values));
		};
		return canonical_binary(canonical_value::from_tuple({
			canonical_value::from_string("cxxlens.provider-task.v1"),
			canonical_value::from_string(session.provider_id),
			canonical_value::from_string(session.provider_version.string()),
			canonical_value::from_string(session.provider_semantic_contract_digest),
			canonical_value::from_string(project.catalog_id),
			canonical_value::from_string(project.catalog_digest),
			descriptor_values(outputs),
			canonical_value::from_string(condition),
			canonical_value::from_string(interpretation),
			descriptor_values(session.offered_outputs),
			descriptor_values(session.required_relations),
			string_values(session.interpretation_domains),
			canonical_value::from_string(session.input_stage),
			canonical_value::from_string(session.output_stage),
			string_values(dependency_groups),
		}));
	}

	result<task> task::make(provider_session session_value,
							project_catalog project_value,
							std::vector<relation_descriptor> output_values,
							std::string condition_value,
							std::string interpretation_value,
							std::vector<std::string> dependency_group_values)
	{
		const auto descriptor_order =
			[](const relation_descriptor& left, const relation_descriptor& right)
		{
			return std::tie(left.id, left.descriptor_digest) <
				std::tie(right.id, right.descriptor_digest);
		};
		std::ranges::sort(session_value.offered_outputs, descriptor_order);
		std::ranges::sort(session_value.required_relations, descriptor_order);
		std::ranges::sort(session_value.interpretation_domains);
		std::ranges::sort(output_values, descriptor_order);
		std::ranges::sort(dependency_group_values);
		task output{{},
					std::move(session_value),
					std::move(project_value),
					std::move(output_values),
					std::move(condition_value),
					std::move(interpretation_value),
					std::move(dependency_group_values)};
		auto projection = output.canonical_projection();
		if (!projection)
			return cxxlens::sdk::unexpected(std::move(projection.error()));
		std::string projection_bytes;
		projection_bytes.reserve(projection->size());
		for (const auto byte : *projection)
			projection_bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		auto digest = semantic_digest("cxxlens.provider-task.v1", projection_bytes);
		if (!digest)
			return cxxlens::sdk::unexpected(std::move(digest.error()));
		output.task_id = "task:" + *digest;
		if (auto valid = output.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		return output;
	}

	result<void> task::validate() const
	{
		auto projection = canonical_projection();
		if (!projection)
			return cxxlens::sdk::unexpected(std::move(projection.error()));
		std::string projection_bytes;
		projection_bytes.reserve(projection->size());
		for (const auto byte : *projection)
			projection_bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		auto digest = semantic_digest("cxxlens.provider-task.v1", projection_bytes);
		if (!digest || task_id != "task:" + *digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.task-invalid", "task_id", "identity-mismatch"));
		for (const auto& descriptor : outputs)
		{
			const auto offered =
				std::ranges::find(session.offered_outputs, descriptor.id, &relation_descriptor::id);
			if (offered == session.offered_outputs.end() ||
				offered->descriptor_digest != descriptor.descriptor_digest)
				return cxxlens::sdk::unexpected(
					provider_error("provider.relation-incompatible", descriptor.id, "not-offered"));
		}
		return {};
	}

	coverage_builder& coverage_builder::request(std::string kind, std::string id)
	{
		requested_.emplace_back(std::move(kind), std::move(id));
		return *this;
	}

	result<void> coverage_builder::classify(coverage_unit unit)
	{
		static const std::set<std::string, std::less<>> states{
			"covered", "excluded", "failed", "not_applicable", "unresolved"};
		if (!states.contains(unit.state))
			return cxxlens::sdk::unexpected(
				provider_error("provider.coverage-invalid", "state", unit.state));
		if (unit.kind.empty() || unit.id.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.coverage-invalid", "identity"));
		units_.push_back(std::move(unit));
		return {};
	}

	result<std::vector<coverage_unit>> coverage_builder::finish() &&
	{
		std::ranges::sort(requested_);
		if (std::ranges::adjacent_find(requested_) != requested_.end())
			return cxxlens::sdk::unexpected(
				provider_error("provider.coverage-duplicate-request", "requested"));
		std::ranges::sort(units_,
						  [](const coverage_unit& left, const coverage_unit& right)
						  {
							  return std::tie(left.kind, left.id) < std::tie(right.kind, right.id);
						  });
		for (std::size_t index = 0U; index < units_.size(); ++index)
		{
			const auto key = std::pair{units_[index].kind, units_[index].id};
			if (!std::ranges::binary_search(requested_, key))
				return cxxlens::sdk::unexpected(
					provider_error("provider.coverage-unrequested", units_[index].id));
			if (index != 0U && units_[index - 1U].kind == units_[index].kind &&
				units_[index - 1U].id == units_[index].id)
				return cxxlens::sdk::unexpected(
					provider_error("provider.coverage-duplicate-state", units_[index].id));
		}
		if (units_.size() != requested_.size())
			return cxxlens::sdk::unexpected(
				provider_error("provider.coverage-incomplete", "coverage"));
		return std::move(units_);
	}

	unresolved_builder& unresolved_builder::add(unresolved_item item)
	{
		items_.push_back(std::move(item));
		return *this;
	}

	result<std::vector<unresolved_item>> unresolved_builder::finish() &&
	{
		for (const auto& item : items_)
			if (!namespaced(item.code) || item.subject.empty())
				return cxxlens::sdk::unexpected(
					provider_error("provider.unresolved-invalid", item.subject));
		std::ranges::sort(items_,
						  [](const unresolved_item& left, const unresolved_item& right)
						  {
							  return std::tie(left.code, left.subject, left.detail) <
								  std::tie(right.code, right.subject, right.detail);
						  });
		items_.erase(std::ranges::unique(items_).begin(), items_.end());
		return std::move(items_);
	}

	evidence_builder& evidence_builder::add(evidence_item item)
	{
		items_.push_back(std::move(item));
		return *this;
	}

	result<std::vector<evidence_item>> evidence_builder::finish() &&
	{
		for (const auto& item : items_)
			if (!namespaced(item.kind) || item.subject.empty() || item.producer.empty())
				return cxxlens::sdk::unexpected(
					provider_error("provider.evidence-invalid", item.subject));
		std::ranges::sort(
			items_,
			[](const evidence_item& left, const evidence_item& right)
			{
				return std::tie(left.kind, left.subject, left.producer, left.summary) <
					std::tie(right.kind, right.subject, right.producer, right.summary);
			});
		items_.erase(std::ranges::unique(items_).begin(), items_.end());
		return std::move(items_);
	}

	relation_sink::relation_sink(protocol_writer& writer,
								 relation_descriptor descriptor,
								 std::string task_id,
								 std::uint64_t& total_rows,
								 const std::uint64_t maximum_rows,
								 const bool authorized,
								 const std::span<const std::string> dependency_groups,
								 std::shared_ptr<detail::relation_sink_registry> registry,
								 const std::uint64_t sink_id)
		: writer_{&writer}, descriptor_{std::move(descriptor)}, task_id_{std::move(task_id)},
		  total_rows_{&total_rows}, maximum_rows_{maximum_rows},
		  dependency_groups_{dependency_groups.begin(), dependency_groups.end()},
		  registry_{std::move(registry)}, sink_id_{sink_id}, authorized_{authorized}
	{
	}

	relation_sink::~relation_sink()
	{
		if (registry_ && (open_ || poisoned_) && registry_->active_batch &&
			registry_->active_batch->sink_id == sink_id_ && !registry_->violation)
			registry_->violation = provider_error(
				"provider.batch-state-invalid", registry_->active_batch->batch_id, "abandoned");
	}

	result<void> relation_sink::begin(std::string dependency_group,
									  std::string atomic_output_group,
									  std::string batch_id)
	{
		if (!registry_ || poisoned_)
			return cxxlens::sdk::unexpected(
				provider_error("provider.batch-state-invalid", "poisoned"));
		if (!authorized_ || !std::ranges::binary_search(dependency_groups_, dependency_group))
		{
			auto failure = provider_error(
				"provider.relation-incompatible", descriptor_.id, "task-output-or-dependency");
			if (!registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (open_ || task_id_.empty() || dependency_group.empty() || atomic_output_group.empty() ||
			batch_id.empty())
		{
			auto failure = provider_error("provider.batch-state-invalid", "begin");
			if (!registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (auto valid = descriptor_.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		std::vector<batch_column_summary> fresh_summaries;
		fresh_summaries.reserve(descriptor_.columns.size());
		for (const auto& column : descriptor_.columns)
			fresh_summaries.push_back({column.id, 0U, 0U});
		auto control = encode_batch_begin_metadata({task_id_,
													descriptor_.id,
													descriptor_.descriptor_digest,
													dependency_group,
													atomic_output_group,
													batch_id});
		if (!control)
			return cxxlens::sdk::unexpected(std::move(control.error()));
		if (registry_->active_batch || registry_->seen_batch_ids.contains(batch_id))
		{
			auto failure = provider_error(
				"provider.batch-state-invalid", batch_id, "duplicate-or-interleaved");
			if (!registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		registry_->seen_batch_ids.insert(batch_id);
		registry_->active_batch.emplace(sink_id_, dependency_group, atomic_output_group, batch_id);
		if (auto sent = writer_->send(message_type::batch_begin, *control); !sent)
		{
			poisoned_ = true;
			return sent;
		}
		dependency_group_ = std::move(dependency_group);
		atomic_output_group_ = std::move(atomic_output_group);
		batch_id_ = std::move(batch_id);
		pending_rows_.clear();
		column_summaries_ = std::move(fresh_summaries);
		ordered_chunk_digests_.clear();
		row_count_ = 0U;
		emitted_rows_ = 0U;
		open_ = true;
		return {};
	}

	result<void> relation_sink::flush_chunk()
	{
		if (pending_rows_.empty())
			return {};
		if (!registry_ || poisoned_ || !registry_->active_batch ||
			registry_->active_batch->sink_id != sink_id_)
			return cxxlens::sdk::unexpected(
				provider_error("provider.batch-state-invalid", "poisoned"));
		if (!open_ || writer_ == nullptr || pending_rows_.size() > 256U ||
			column_summaries_.size() != descriptor_.columns.size())
			return cxxlens::sdk::unexpected(
				provider_error("provider.columnar-invalid", batch_id_, "flush-state"));
		std::vector<encoded_column_chunk> encoded_chunks;
		encoded_chunks.reserve(descriptor_.columns.size());
		for (std::size_t column_index = 0U; column_index < descriptor_.columns.size();
			 ++column_index)
		{
			const auto& column = descriptor_.columns[column_index];
			const auto& summary = column_summaries_[column_index];
			column_chunk_record chunk{task_id_,
									  dependency_group_,
									  atomic_output_group_,
									  batch_id_,
									  descriptor_.id,
									  descriptor_.descriptor_digest,
									  column.id,
									  emitted_rows_,
									  static_cast<std::uint32_t>(pending_rows_.size()),
									  summary.chunk_count,
									  std::string{column_encoding(column.type.scalar)},
									  {},
									  {}};
			chunk.cells.reserve(pending_rows_.size());
			for (const auto& row : pending_rows_)
			{
				const auto found = row.cells.find(column.id);
				if (found == row.cells.end())
				{
					if (!column.type.optional)
						return cxxlens::sdk::unexpected(
							provider_error("provider.columnar-invalid", column.id, "missing-cell"));
					chunk.cells.push_back(detached_cell::absent(column.type));
				}
				else
					chunk.cells.push_back(found->second);
			}
			auto encoded = encode_column_chunk(chunk, column);
			if (!encoded)
				return cxxlens::sdk::unexpected(std::move(encoded.error()));
			auto validated = decode_column_chunk(encoded->control, encoded->payload, column);
			if (!validated)
				return cxxlens::sdk::unexpected(std::move(validated.error()));
			encoded_chunks.push_back(std::move(*encoded));
		}
		for (std::size_t column_index = 0U; column_index < encoded_chunks.size(); ++column_index)
		{
			auto& encoded = encoded_chunks[column_index];
			if (auto sent =
					writer_->send(message_type::column_chunk, encoded.control, encoded.payload);
				!sent)
			{
				poisoned_ = true;
				return sent;
			}
			auto& summary = column_summaries_[column_index];
			summary.payload_bytes += encoded.payload.size();
			++summary.chunk_count;
			ordered_chunk_digests_.push_back(std::move(encoded.chunk_digest));
		}
		emitted_rows_ += pending_rows_.size();
		pending_rows_.clear();
		return {};
	}

	result<void> relation_sink::push(const detached_row& row)
	{
		if (!registry_ || poisoned_ || !registry_->active_batch ||
			registry_->active_batch->sink_id != sink_id_)
		{
			auto failure = provider_error("provider.batch-state-invalid", "poisoned");
			if (registry_ && !registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (!open_)
		{
			auto failure = provider_error("provider.batch-state-invalid", "push");
			if (!registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (total_rows_ == nullptr || *total_rows_ >= maximum_rows_)
			return cxxlens::sdk::unexpected(provider_error("provider.output-limit", "rows"));
		if (auto valid = validate_row(descriptor_, row); !valid)
			return valid;
		pending_rows_.push_back(row);
		++row_count_;
		++*total_rows_;
		if (pending_rows_.size() == 256U)
			return flush_chunk();
		return {};
	}

	result<void> relation_sink::end()
	{
		if (!registry_ || poisoned_ || !registry_->active_batch ||
			registry_->active_batch->sink_id != sink_id_)
		{
			auto failure = provider_error("provider.batch-state-invalid", "poisoned");
			if (registry_ && !registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (!open_)
		{
			auto failure = provider_error("provider.batch-state-invalid", "end");
			if (!registry_->violation)
				registry_->violation = failure;
			return cxxlens::sdk::unexpected(std::move(failure));
		}
		if (auto flushed = flush_chunk(); !flushed)
			return flushed;
		columnar_batch_end terminal{task_id_,
									dependency_group_,
									atomic_output_group_,
									batch_id_,
									descriptor_.id,
									descriptor_.descriptor_digest,
									row_count_,
									column_summaries_,
									ordered_chunk_digests_,
									{}};
		terminal.batch_digest = columnar_batch_digest(terminal);
		auto encoded = encode_columnar_batch_end(terminal);
		if (!encoded)
			return cxxlens::sdk::unexpected(std::move(encoded.error()));
		auto validated = decode_columnar_batch_end(encoded->control, encoded->payload);
		if (!validated)
			return cxxlens::sdk::unexpected(std::move(validated.error()));
		if (auto sent = writer_->send(message_type::batch_end, encoded->control, encoded->payload);
			!sent)
		{
			poisoned_ = true;
			return sent;
		}
		open_ = false;
		registry_->active_batch.reset();
		return {};
	}

	std::uint64_t relation_sink::row_count() const noexcept
	{
		return row_count_;
	}

	context::context(protocol_writer& writer,
					 execution_context execution,
					 std::string task_id,
					 const std::span<const relation_descriptor> outputs,
					 const std::span<const std::string> dependency_groups)
		: writer_{&writer}, execution_{std::move(execution)}, task_id_{std::move(task_id)},
		  outputs_{outputs.begin(), outputs.end()},
		  dependency_groups_{dependency_groups.begin(), dependency_groups.end()},
		  sink_registry_{std::make_shared<detail::relation_sink_registry>()}
	{
	}

	relation_sink context::relation(relation_descriptor descriptor)
	{
		const auto requested = std::ranges::find(outputs_, descriptor.id, &relation_descriptor::id);
		const bool authorized = requested != outputs_.end() &&
			requested->descriptor_digest == descriptor.descriptor_digest &&
			requested->canonical_form() == descriptor.canonical_form();
		if (!authorized && !sink_registry_->violation)
			sink_registry_->violation = provider_error(
				"provider.relation-incompatible", descriptor.id, "task-output-whitelist");
		const auto sink_id = sink_registry_->next_sink_id++;
		return relation_sink{*writer_,
							 std::move(descriptor),
							 task_id_,
							 output_rows_,
							 execution_.budget.rows,
							 authorized,
							 dependency_groups_,
							 sink_registry_,
							 sink_id};
	}
	coverage_builder& context::coverage() noexcept
	{
		return coverage_;
	}
	unresolved_builder& context::unresolved() noexcept
	{
		return unresolved_;
	}
	evidence_builder& context::evidence() noexcept
	{
		return evidence_;
	}
	bool context::stop_requested() const noexcept
	{
		return execution_.cancellation.stop_requested();
	}

	result<void> context::validate() const
	{
		if (sink_registry_->violation)
			return cxxlens::sdk::unexpected(*sink_registry_->violation);
		if (sink_registry_->active_batch)
			return cxxlens::sdk::unexpected(
				provider_error("provider.batch-state-invalid", "open-batch"));
		return {};
	}

	result<void> manifest::validate() const
	{
		if (!namespaced(provider_id) || provider_version.major == 0U || package_identity.empty() ||
			publisher.empty() || license.empty() || (signature && signature->empty()) ||
			protocol.major != 1U || protocol.minimum_minor > protocol.maximum_minor ||
			resource_class.empty() || sandbox_minimum.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.manifest-invalid", "identity"));
		if (!canonical_digest(provider_binary_digest) ||
			!canonical_digest(provider_semantic_contract_digest) ||
			!canonical_digest(invalidation_contract) || !canonical_digest(determinism_contract))
			return cxxlens::sdk::unexpected(provider_error("provider.manifest-invalid", "digest"));
		if (!unique_nonempty(platform_tuples, true) || !unique_nonempty(offered_relations) ||
			!unique_nonempty(required_relations) ||
			!unique_nonempty(interpretation_domains, true) ||
			!unique_nonempty(protocol.required_features) ||
			!unique_nonempty(protocol.optional_features) ||
			!unique_nonempty(requested_qualifications) || !unique_nonempty(trust_flags))
			return cxxlens::sdk::unexpected(provider_error("provider.manifest-invalid", "set"));
		static const std::set<std::string, std::less<>> qualifications{
			"canonical-semantic-qualified",
			"cross-version-qualified",
			"deterministic",
			"experimental",
			"production-supported",
			"sandbox-qualified",
			"schema-conformant",
		};
		static const std::set<std::string, std::less<>> stages{
			"assertion", "canonical_claim", "derived_claim", "observation"};
		if (std::ranges::any_of(requested_qualifications,
								[&](const std::string& value)
								{
									return !qualifications.contains(value);
								}) ||
			!stages.contains(task_input_stage) || !stages.contains(task_output_stage))
			return cxxlens::sdk::unexpected(provider_error("provider.manifest-invalid", "enum"));
		return {};
	}

	std::string manifest::canonical_json() const
	{
		std::ostringstream output;
		output << "{\"determinism_contract\":" << json_string(determinism_contract)
			   << ",\"interpretation_domains\":" << canonical_array(interpretation_domains)
			   << ",\"invalidation_contract\":" << json_string(invalidation_contract)
			   << ",\"license\":" << json_string(license)
			   << ",\"offered_relations\":" << canonical_array(offered_relations)
			   << ",\"package_identity\":" << json_string(package_identity)
			   << ",\"platform_tuples\":" << canonical_array(platform_tuples)
			   << R"(,"protocol_range":{"major":)" << protocol.major
			   << ",\"maximum_minor\":" << protocol.maximum_minor
			   << ",\"minimum_minor\":" << protocol.minimum_minor
			   << ",\"optional_features\":" << canonical_array(protocol.optional_features)
			   << ",\"required_features\":" << canonical_array(protocol.required_features) << '}'
			   << ",\"provider_binary_digest\":" << json_string(provider_binary_digest)
			   << ",\"provider_id\":" << json_string(provider_id)
			   << ",\"provider_semantic_contract_digest\":"
			   << json_string(provider_semantic_contract_digest)
			   << ",\"provider_version\":" << json_string(provider_version.string())
			   << ",\"publisher\":" << json_string(publisher)
			   << ",\"requested_qualifications\":" << canonical_array(requested_qualifications)
			   << ",\"required_relations\":" << canonical_array(required_relations)
			   << ",\"resource_class\":" << json_string(resource_class)
			   << ",\"sandbox_minimum\":" << json_string(sandbox_minimum)
			   << R"(,"schema":"cxxlens.provider-manifest.v1","signature":)"
			   << (signature ? json_string(*signature) : "null") << R"(,"task_stage":{"input":)"
			   << json_string(task_input_stage) << ",\"output\":" << json_string(task_output_stage)
			   << "},\"trust_flags\":" << canonical_array(trust_flags) << '}';
		return output.str();
	}

	result<void> sandbox_requirement::validate() const
	{
		if (!is_valid(minimum) || !canonical_digest(policy_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-requirement-invalid",
							   is_valid(minimum) ? "policy_digest" : "minimum",
							   is_valid(minimum) ? "" : "closed-enum"));
		return {};
	}

	result<void> sandbox_policy::validate() const
	{
		const auto has_mechanism = [&](const std::string_view mechanism)
		{
			return std::ranges::find(mechanisms, mechanism) != mechanisms.end();
		};
		if (!namespaced(id) || !unique_nonempty(mechanisms, true) ||
			deny_network != has_mechanism("network-syscall-deny") ||
			zero_core_dump != has_mechanism("core-dump-limit") ||
			zero_locked_memory != has_mechanism("locked-memory-limit"))
			return cxxlens::sdk::unexpected(
				provider_error("security.sandbox-policy-mismatch", "policy"));
		return {};
	}

	std::string sandbox_policy::canonical_form() const
	{
		return std::string{R"({"deny_network":)"} + (deny_network ? "true" : "false") +
			R"(,"id":)" + json_string(id) + R"(,"mechanisms":)" + canonical_array(mechanisms) +
			R"(,"schema":"cxxlens.provider-sandbox-policy.v1","zero_core_dump":)" +
			(zero_core_dump ? "true" : "false") + R"(,"zero_locked_memory":)" +
			(zero_locked_memory ? "true" : "false") + "}";
	}

	std::string sandbox_policy::policy_digest() const
	{
		return *cxxlens::sdk::semantic_digest("cxxlens.provider-sandbox-policy.v1",
											  canonical_form());
	}

	std::vector<sandbox_policy> builtin_sandbox_policies()
	{
		const std::vector<std::string> baseline{
			"address-space-limit",
			"anonymous-readonly-input",
			"cpu-limit",
			"explicit-environment",
			"inherited-fd-close-range",
			"network-syscall-deny",
			"no-new-privileges",
			"no-shell-argv-exec",
			"open-file-limit",
			"output-file-size-limit",
			"process-group-cleanup",
			"seccomp-audit-arch",
			"subprocess-limit",
		};
		auto strict = baseline;
		strict.emplace_back("core-dump-limit");
		strict.emplace_back("locked-memory-limit");
		std::ranges::sort(strict);
		return {
			{"cxxlens.sandbox.linux-provider-baseline", baseline, true, false, false},
			{"cxxlens.sandbox.linux-provider-strict", std::move(strict), true, true, true},
		};
	}

	result<sandbox_policy> resolve_sandbox_policy(const std::string_view policy_digest)
	{
		if (!canonical_digest(policy_digest))
			return cxxlens::sdk::unexpected(
				provider_error("security.sandbox-policy-mismatch", "policy-digest"));
		for (auto policy : builtin_sandbox_policies())
			if (policy.policy_digest() == policy_digest)
				return policy;
		return cxxlens::sdk::unexpected(
			provider_error("security.sandbox-policy-mismatch", "unknown-policy"));
	}

	result<std::string>
	sandbox_evidence_digest(const sandbox_policy& policy,
							const execution_budget& budget,
							const sandbox_assurance achieved,
							const std::span<const std::string> applied_mechanisms,
							const std::string_view measured_executable_digest)
	{
		if (!is_valid(achieved))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-report-invalid", "achieved", "closed-enum"));
		if (auto valid = policy.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (!canonical_digest(measured_executable_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-report-invalid", "measured_executable_digest"));
		std::vector<std::string> mechanisms{applied_mechanisms.begin(), applied_mechanisms.end()};
		std::ostringstream projection;
		projection << policy.canonical_form() << "\npolicy-digest=" << policy.policy_digest()
				   << "\nmeasured-executable-digest=" << measured_executable_digest
				   << "\nachieved=" << sandbox_name(achieved) << "\nwall-ms=" << budget.wall_ms
				   << "\ncpu-ms=" << budget.cpu_ms << "\nrss-bytes=" << budget.rss_bytes
				   << "\noutput-bytes=" << budget.output_bytes
				   << "\nopen-files=" << budget.open_files
				   << "\nsubprocesses=" << budget.subprocesses
				   << "\nmechanisms=" << canonical_array(std::move(mechanisms));
		return cxxlens::sdk::semantic_digest("cxxlens.provider-sandbox-evidence.v3",
											 projection.str());
	}

	result<void> sandbox_report::validate() const
	{
		if (!is_valid(achieved) || platform.empty() || !unique_nonempty(mechanisms) ||
			!canonical_digest(policy_digest) || !canonical_digest(evidence_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-report-invalid",
							   is_valid(achieved) ? "report" : "achieved",
							   is_valid(achieved) ? "" : "closed-enum"));
		return {};
	}

	std::string sandbox_report::canonical_form() const
	{
		return "{\"achieved\":" + json_string(sandbox_name(achieved)) +
			",\"evidence_digest\":" + json_string(evidence_digest) +
			",\"mechanisms\":" + canonical_array(mechanisms) +
			",\"platform\":" + json_string(platform) +
			",\"policy_digest\":" + json_string(policy_digest) + "}";
	}

	result<void> provider_fallback_tuple::validate(const semantic_version& requested_version) const
	{
		if (!is_valid(direction))
			return cxxlens::sdk::unexpected(
				provider_error("provider.fallback-policy-invalid", "direction", "closed-enum"));
		const auto actual_direction = provider_version > requested_version
			? fallback_direction::upgrade
			: (provider_version < requested_version ? fallback_direction::downgrade
													: fallback_direction::same_version_rebuild);
		if (priority == 0U || !namespaced(provider_id) || provider_version.major == 0U ||
			!canonical_digest(provider_binary_digest) ||
			!canonical_digest(provider_semantic_contract_digest) || actual_direction != direction ||
			!unique_nonempty(required_qualifications))
			return cxxlens::sdk::unexpected(
				provider_error("provider.fallback-policy-invalid", provider_id));
		return {};
	}

	std::string provider_fallback_tuple::canonical_form() const
	{
		return std::string{R"({"direction":)"} + json_string(direction_name(direction)) +
			R"(,"priority":)" + std::to_string(priority) + R"(,"provider_binary_digest":)" +
			json_string(provider_binary_digest) + R"(,"provider_id":)" + json_string(provider_id) +
			R"(,"provider_semantic_contract_digest":)" +
			json_string(provider_semantic_contract_digest) + R"(,"provider_version":)" +
			json_string(provider_version.string()) + R"(,"require_certification":)" +
			(require_certification ? "true" : "false") + R"(,"required_qualifications":)" +
			canonical_array(required_qualifications) + "}";
	}

	result<void> provider_fallback_policy::validate(const semantic_version& requested_version) const
	{
		if (!namespaced(policy_id) || allowed.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.fallback-policy-invalid", policy_id));
		std::set<std::uint32_t> priorities;
		std::set<std::tuple<std::string, semantic_version, std::string, std::string>> identities;
		for (const auto& value : allowed)
		{
			if (auto valid = value.validate(requested_version); !valid)
				return valid;
			if (!priorities.insert(value.priority).second ||
				!identities
					 .emplace(value.provider_id,
							  value.provider_version,
							  value.provider_binary_digest,
							  value.provider_semantic_contract_digest)
					 .second)
				return cxxlens::sdk::unexpected(
					provider_error("provider.fallback-policy-invalid", policy_id, "duplicate"));
		}
		return {};
	}

	std::string provider_fallback_policy::canonical_form() const
	{
		auto ordered = allowed;
		std::ranges::sort(
			ordered,
			[](const provider_fallback_tuple& left, const provider_fallback_tuple& right)
			{
				return std::tie(left.priority,
								left.provider_id,
								left.provider_version,
								left.provider_binary_digest,
								left.provider_semantic_contract_digest) <
					std::tie(right.priority,
							 right.provider_id,
							 right.provider_version,
							 right.provider_binary_digest,
							 right.provider_semantic_contract_digest);
			});
		std::ostringstream output;
		output << R"({"allowed":[)";
		for (std::size_t index = 0U; index < ordered.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			output << ordered[index].canonical_form();
		}
		output << R"(],"policy_id":)" << json_string(policy_id) << '}';
		return output.str();
	}

	std::string provider_fallback_policy::semantic_digest() const
	{
		return *cxxlens::sdk::semantic_digest("cxxlens.provider-fallback-policy.v1",
											  canonical_form());
	}

	namespace
	{
		[[nodiscard]] std::string ordered_json_array(const std::vector<std::string>& values)
		{
			std::ostringstream output;
			output << '[';
			for (std::size_t index = 0U; index < values.size(); ++index)
			{
				if (index != 0U)
					output << ',';
				output << json_string(values[index]);
			}
			output << ']';
			return output.str();
		}

		[[nodiscard]] std::string candidate_canonical_form(const provider_candidate& candidate)
		{
			std::ostringstream output;
			output << R"({"authoritative_path":)"
				   << (candidate.authoritative_path ? "true" : "false")
				   << R"(,"certification_valid":)"
				   << (candidate.certification_valid ? "true" : "false")
				   << R"(,"certified_qualifications":)"
				   << canonical_array(candidate.certified_qualifications)
				   << R"(,"executable_argv":)" << ordered_json_array(candidate.executable_argv)
				   << R"(,"manifest":)" << candidate.description.canonical_json()
				   << R"(,"sandbox":)" << candidate.sandbox.canonical_form()
				   << R"(,"schema":"cxxlens.provider-candidate.v1","trust_valid":)"
				   << (candidate.trust_valid ? "true" : "false") << R"(,"validation_error":)"
				   << json_string(candidate.validation_error) << '}';
			return output.str();
		}

		[[nodiscard]] std::string candidate_identity_digest(const provider_candidate& candidate)
		{
			return *cxxlens::sdk::semantic_digest("cxxlens.provider-candidate.v1",
												  candidate_canonical_form(candidate));
		}
	} // namespace

	provider_selection::provider_selection(provider_candidate candidate,
										   provider_selection_request request,
										   std::vector<provider_candidate_decision> decisions,
										   const bool fallback_used,
										   std::optional<std::string> fallback_policy_digest)
		: candidate_{std::move(candidate)}, request_{std::move(request)},
		  decisions_{std::move(decisions)}, fallback_used_{fallback_used},
		  fallback_policy_digest_{std::move(fallback_policy_digest)}, validated_{true}
	{
	}

	const provider_candidate& provider_selection::selected_candidate() const noexcept
	{
		return candidate_;
	}

	const provider_selection_request& provider_selection::authority_request() const noexcept
	{
		return request_;
	}

	std::span<const provider_candidate_decision> provider_selection::decisions() const noexcept
	{
		return decisions_;
	}

	bool provider_selection::fallback_used() const noexcept
	{
		return fallback_used_;
	}

	const std::optional<std::string>& provider_selection::fallback_policy_digest() const noexcept
	{
		return fallback_policy_digest_;
	}

	result<void> provider_selection::validate() const
	{
		if (!validated_ || decisions_.empty() || !is_valid(candidate_.source) ||
			std::ranges::any_of(decisions_,
								[](const provider_candidate_decision& decision)
								{
									return !is_valid(decision.source);
								}))
			return cxxlens::sdk::unexpected(
				provider_error("provider.selection-invalid", "selection-token"));
		if (auto valid = candidate_.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		const auto selected_count =
			std::ranges::count(decisions_, true, &provider_candidate_decision::selected);
		const auto selected =
			std::ranges::find(decisions_, true, &provider_candidate_decision::selected);
		const auto expected_reason = fallback_used_ ? std::string_view{"selected-explicit-fallback"}
													: std::string_view{"selected-exact"};
		const auto expected_policy_digest = request_.fallback_policy
			? std::optional<std::string>{request_.fallback_policy->semantic_digest()}
			: std::nullopt;
		if (selected_count != 1 || selected == decisions_.end() ||
			selected->source != candidate_.source ||
			selected->provider_id != candidate_.description.provider_id ||
			selected->provider_version != candidate_.description.provider_version ||
			selected->binary_digest != candidate_.description.provider_binary_digest ||
			selected->candidate_digest != candidate_identity_digest(candidate_) ||
			selected->reason != expected_reason ||
			fallback_policy_digest_ != expected_policy_digest ||
			(fallback_used_ && !request_.fallback_policy))
			return cxxlens::sdk::unexpected(
				provider_error("provider.selection-invalid", "decision-binding"));
		auto replay = select_provider(request_, std::span{&candidate_, 1U});
		if (!replay)
		{
			if (replay.error().code == "provider.sandbox-requirement-invalid" ||
				replay.error().code == "provider.sandbox-report-invalid")
				return cxxlens::sdk::unexpected(std::move(replay.error()));
			return cxxlens::sdk::unexpected(provider_error(
				"provider.selection-invalid", "authority-revalidation", replay.error().code));
		}
		if (replay->fallback_used() != fallback_used_ ||
			replay->fallback_policy_digest() != fallback_policy_digest_)
			return cxxlens::sdk::unexpected(
				provider_error("provider.selection-invalid", "authority-revalidation", "mismatch"));
		return {};
	}

	std::string provider_selection::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"decisions\":[";
		for (std::size_t index = 0U; index < decisions_.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& decision = decisions_[index];
			output << "{\"binary_digest\":" << json_string(decision.binary_digest)
				   << ",\"candidate_digest\":" << json_string(decision.candidate_digest)
				   << ",\"provider_id\":" << json_string(decision.provider_id)
				   << ",\"provider_version\":" << json_string(decision.provider_version.string())
				   << ",\"reason\":" << json_string(decision.reason)
				   << ",\"selected\":" << (decision.selected ? "true" : "false")
				   << ",\"source\":" << json_string(source_name(decision.source)) << '}';
		}
		output << "],\"fallback_policy_digest\":";
		if (fallback_policy_digest_)
			output << json_string(*fallback_policy_digest_);
		else
			output << "null";
		output << ",\"fallback_used\":" << (fallback_used_ ? "true" : "false")
			   << ",\"selected_manifest\":" << candidate_.description.canonical_json()
			   << ",\"selected_source\":" << json_string(source_name(candidate_.source)) << '}';
		return output.str();
	}

	result<provider_selection> select_provider(const provider_selection_request& request,
											   const std::span<const provider_candidate> candidates)
	{
		if (!namespaced(request.provider_id) || request.provider_version.major == 0U ||
			!canonical_digest(request.provider_binary_digest) ||
			!canonical_digest(request.provider_semantic_contract_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.selection-invalid", "identity"));
		if (auto valid = request.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (auto policy = resolve_sandbox_policy(request.sandbox.policy_digest); !policy)
			return cxxlens::sdk::unexpected(std::move(policy.error()));
		if (request.fallback_policy)
			if (auto valid = request.fallback_policy->validate(request.provider_version); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (candidates.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.not-found", request.provider_id));
		for (const auto& candidate : candidates)
			if (!is_valid(candidate.source))
				return cxxlens::sdk::unexpected(provider_error(
					"provider.selection-invalid", "discovery_source", "closed-enum"));
		for (const auto& candidate : candidates)
			if (auto valid = candidate.sandbox.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));

		std::vector<std::string> candidate_digests;
		candidate_digests.reserve(candidates.size());
		using candidate_sources = std::map<std::string, std::set<discovery_source>, std::less<>>;
		std::map<std::pair<std::string, std::string>, candidate_sources> identities;
		for (const auto& candidate : candidates)
		{
			auto digest = candidate_identity_digest(candidate);
			auto& sources = identities[{candidate.description.provider_id,
										candidate.description.provider_version.string()}][digest];
			if (!sources.insert(candidate.source).second)
				return cxxlens::sdk::unexpected(provider_error("security.provider-shadowing",
															   candidate.description.provider_id,
															   "duplicate-canonical-candidate"));
			candidate_digests.push_back(std::move(digest));
		}
		for (const auto& [identity, values] : identities)
			if (values.size() > 1U)
				return cxxlens::sdk::unexpected(
					provider_error("security.provider-shadowing", identity.first, identity.second));

		std::vector<std::size_t> order(candidates.size());
		for (std::size_t index = 0U; index < order.size(); ++index)
			order[index] = index;
		std::ranges::sort(order,
						  [&](const std::size_t left, const std::size_t right)
						  {
							  const auto& lhs = candidates[left];
							  const auto& rhs = candidates[right];
							  return std::tuple{source_rank(lhs.source),
												lhs.description.provider_id,
												lhs.description.provider_version,
												lhs.description.provider_binary_digest,
												candidate_digests[left]} <
								  std::tuple{source_rank(rhs.source),
											 rhs.description.provider_id,
											 rhs.description.provider_version,
											 rhs.description.provider_binary_digest,
											 candidate_digests[right]};
						  });

		std::vector<provider_candidate_decision> decisions;
		decisions.reserve(candidates.size());
		const auto exact_identity = [&](const provider_candidate& candidate)
		{
			return candidate.description.provider_id == request.provider_id &&
				candidate.description.provider_version == request.provider_version &&
				candidate.description.provider_binary_digest == request.provider_binary_digest &&
				candidate.description.provider_semantic_contract_digest ==
				request.provider_semantic_contract_digest;
		};
		const auto fallback_tuple =
			[&](const provider_candidate& candidate) -> const provider_fallback_tuple*
		{
			if (!request.fallback_policy)
				return nullptr;
			const auto found = std::ranges::find_if(
				request.fallback_policy->allowed,
				[&](const provider_fallback_tuple& allowed)
				{
					return candidate.description.provider_id == allowed.provider_id &&
						candidate.description.provider_version == allowed.provider_version &&
						candidate.description.provider_binary_digest ==
						allowed.provider_binary_digest &&
						candidate.description.provider_semantic_contract_digest ==
						allowed.provider_semantic_contract_digest;
				});
			return found == request.fallback_policy->allowed.end() ? nullptr : &*found;
		};

		std::optional<std::uint8_t> exact_precedence;
		std::optional<std::pair<std::uint32_t, std::uint8_t>> fallback_precedence;
		for (const auto index : order)
		{
			const auto& candidate = candidates[index];
			if (exact_identity(candidate))
			{
				exact_precedence = exact_precedence
					? std::min(*exact_precedence, source_rank(candidate.source))
					: source_rank(candidate.source);
			}
			else if (const auto* allowed = fallback_tuple(candidate); allowed != nullptr)
			{
				const auto precedence = std::pair{allowed->priority, source_rank(candidate.source)};
				fallback_precedence =
					fallback_precedence ? std::min(*fallback_precedence, precedence) : precedence;
			}
		}

		std::optional<std::size_t> selected;
		bool selected_fallback{};
		for (const auto index : order)
		{
			const auto& candidate = candidates[index];
			provider_candidate_decision decision{candidate.source,
												 candidate.description.provider_id,
												 candidate.description.provider_version,
												 candidate.description.provider_binary_digest,
												 candidate_digests[index],
												 false,
												 {}};
			const bool exact = exact_identity(candidate);
			const auto* allowed_fallback = exact ? nullptr : fallback_tuple(candidate);
			if (!candidate.authoritative_path)
				decision.reason = "security.path-only-discovery";
			else if (auto manifest_valid = candidate.description.validate(); !manifest_valid)
				decision.reason = manifest_valid.error().code;
			else if (!candidate.validation_error.empty())
				decision.reason = candidate.validation_error;
			else if (!candidate.trust_valid)
				decision.reason = "security.signature-untrusted";
			else if (!unique_nonempty(candidate.certified_qualifications))
				decision.reason = "provider.certification-invalid";
			else if (request.require_certification && !candidate.certification_valid)
				decision.reason = "security.certification-missing";
			else if (auto sandbox_valid = candidate.sandbox.validate(); !sandbox_valid)
				decision.reason = sandbox_valid.error().code;
			else if (candidate.sandbox.policy_digest != request.sandbox.policy_digest)
				decision.reason = "security.sandbox-policy-mismatch";
			else
			{
				const auto manifest_minimum = parse_sandbox(candidate.description.sandbox_minimum);
				const auto effective_minimum = manifest_minimum &&
						sandbox_satisfies(*manifest_minimum, request.sandbox.minimum)
					? *manifest_minimum
					: request.sandbox.minimum;
				if (!manifest_minimum)
					decision.reason = "provider.manifest-invalid";
				else if (!sandbox_satisfies(candidate.sandbox.achieved, effective_minimum))
					decision.reason = "security.sandbox-insufficient";
				else if (exact)
				{
					if (exact_precedence && source_rank(candidate.source) != *exact_precedence)
						decision.reason = "security.downgrade-forbidden";
					else
					{
						decision.selected = true;
						decision.reason = "selected-exact";
						selected = index;
					}
				}
				else if (allowed_fallback != nullptr && !exact_precedence)
				{
					const auto precedence =
						std::pair{allowed_fallback->priority, source_rank(candidate.source)};
					const auto qualified = std::ranges::all_of(
						allowed_fallback->required_qualifications,
						[&](const std::string& qualification)
						{
							return std::ranges::find(candidate.certified_qualifications,
													 qualification) !=
								candidate.certified_qualifications.end();
						});
					if (fallback_precedence && precedence != *fallback_precedence)
						decision.reason = "provider.fallback-lower-policy-precedence";
					else if (allowed_fallback->require_certification &&
							 !candidate.certification_valid)
						decision.reason = "security.certification-missing";
					else if (!qualified)
						decision.reason = "provider.fallback-qualification-missing";
					else
					{
						decision.selected = true;
						decision.reason = "selected-explicit-fallback";
						selected = index;
						selected_fallback = true;
					}
				}
				else if (candidate.description.provider_id == request.provider_id)
					decision.reason = request.fallback_policy
						? "provider.fallback-policy-mismatch"
						: "provider.adjacent-fallback-forbidden";
				else if (request.fallback_policy)
					decision.reason = "provider.fallback-policy-mismatch";
				else
					decision.reason = "provider.identity-mismatch";
			}
			decisions.push_back(std::move(decision));
			if (selected)
				break;
		}
		for (std::size_t decision_index = decisions.size(); decision_index < order.size();
			 ++decision_index)
		{
			const auto& candidate = candidates[order[decision_index]];
			decisions.push_back({candidate.source,
								 candidate.description.provider_id,
								 candidate.description.provider_version,
								 candidate.description.provider_binary_digest,
								 candidate_digests[order[decision_index]],
								 false,
								 "provider.lower-precedence-not-considered"});
		}
		if (!selected)
		{
			const bool downgrade = exact_precedence &&
				std::ranges::any_of(decisions,
									[](const provider_candidate_decision& decision)
									{
										return decision.reason != "provider.identity-mismatch" &&
											decision.reason !=
											"provider.adjacent-fallback-forbidden";
									});
			return cxxlens::sdk::unexpected(
				provider_error(downgrade ? "security.downgrade-forbidden" : "provider.not-found",
							   request.provider_id));
		}
		return provider_selection{
			candidates[*selected],
			request,
			std::move(decisions),
			selected_fallback,
			request.fallback_policy
				? std::optional<std::string>{request.fallback_policy->semantic_digest()}
				: std::nullopt};
	}

	result<std::vector<scaffold_file>> make_scaffold(const scaffold_options& options)
	{
		if (!namespaced(options.provider_id) || !namespaced(options.relation_name) ||
			(options.provider_class != "portable" && options.provider_class != "clang22-native"))
			return cxxlens::sdk::unexpected(provider_error("provider.scaffold-invalid", "options"));
		auto class_name = options.provider_id;
		std::ranges::replace(class_name, '.', '_');
		const bool native = options.provider_class == "clang22-native";
		const std::string package = native ? "cxxlensClang22ProviderSDK" : "cxxlensProviderSDK";
		const std::string target =
			native ? "cxxlens::clang22_provider_sdk" : "cxxlens::provider_sdk";
		const std::string header = native ? "<cxxlens/provider/clang22.hpp>" : "<cxxlens/sdk.hpp>";
		const std::string zero_digest = "sha256:" + std::string(64U, '0');
		manifest generated_manifest;
		generated_manifest.provider_id = options.provider_id;
		generated_manifest.provider_version = {1U, 0U, 0U};
		generated_manifest.package_identity = options.provider_id + ".package";
		generated_manifest.publisher = options.provider_id + ".publisher";
		generated_manifest.license = "UNLICENSED";
		generated_manifest.protocol.required_features = {"credit-backpressure"};
		generated_manifest.platform_tuples = {"linux-development"};
		generated_manifest.provider_binary_digest = zero_digest;
		generated_manifest.provider_semantic_contract_digest = zero_digest;
		generated_manifest.offered_relations = {options.relation_name};
		generated_manifest.interpretation_domains = {options.provider_id + ".interpretation"};
		generated_manifest.invalidation_contract = zero_digest;
		generated_manifest.determinism_contract = zero_digest;
		generated_manifest.resource_class = std::string{"provider"} + ".standard";
		generated_manifest.requested_qualifications = {"experimental"};
		generated_manifest.task_output_stage = "assertion";
		if (auto valid = generated_manifest.validate(); !valid)
			return cxxlens::sdk::unexpected(
				provider_error("provider.scaffold-invalid", "generated_manifest"));
		std::vector<scaffold_file> output;
		output.push_back(
			{"CMakeLists.txt",
			 "cmake_minimum_required(VERSION 3.25)\nproject(" + class_name +
				 " LANGUAGES CXX)\nfind_package(" + package +
				 " CONFIG REQUIRED)\n"
				 "add_executable(provider src/main.cpp)\ntarget_link_libraries(provider PRIVATE " +
				 target + ")\ntarget_compile_features(provider PRIVATE cxx_std_23)\n"});
		output.push_back({"provider-manifest.json", generated_manifest.canonical_json() + '\n'});
		output.push_back({"src/main.cpp",
						  "#include " + header +
							  "\n// Implement cxxlens::sdk::provider::portable_provider and "
							  "call run_worker; framing, credit, and checksums are SDK-owned.\nint "
							  "main(){return 0;}\n"});
		output.push_back({"tests/provider_test.cpp",
						  "#include <cxxlens/sdk/testing.hpp>\nint main(){return 0;}\n"});
		output.push_back(
			{"README.md",
			 "# " + options.provider_id + "\n\nProvides `" + options.relation_name + "`.\n"});
		return output;
	}

	result<void> run_worker(portable_provider& provider,
							const task& task_value,
							protocol_writer& writer,
							execution_context execution)
	{
		const auto& budget = execution.budget;
		if (auto valid = task_value.validate(); !valid)
			return cxxlens::sdk::unexpected(
				provider_error("provider.task-invalid", valid.error().field, valid.error().code));
		const auto provider_id = provider.id();
		const auto provider_version = provider.version();
		const auto provider_contract = provider.semantic_contract_digest();
		if (!namespaced(provider_id) || provider_version.major == 0U ||
			!canonical_digest(provider_contract) || provider_id != task_value.session.provider_id ||
			provider_version != task_value.session.provider_version ||
			provider_contract != task_value.session.provider_semantic_contract_digest)
			return cxxlens::sdk::unexpected(
				provider_error("provider.task-invalid", "provider-session", "identity-mismatch"));
		if (budget.wall_ms == 0U || budget.cpu_ms == 0U || budget.rss_bytes == 0U ||
			budget.output_bytes == 0U || budget.rows == 0U || budget.diagnostics == 0U ||
			budget.open_files == 0U || budget.created_files == 0U || budget.subprocesses == 0U)
			return cxxlens::sdk::unexpected(provider_error("provider.task-invalid", "budget"));
		auto accepted = encode_task_accepted_metadata(
			{std::string{provider_id}, provider_version.string(), task_value.task_id});
		if (!accepted)
			return cxxlens::sdk::unexpected(std::move(accepted.error()));
		if (auto sent = writer.send(message_type::task_accepted, *accepted); !sent)
			return sent;
		context callback_context{writer,
								 std::move(execution),
								 task_value.task_id,
								 task_value.outputs,
								 task_value.dependency_groups};
		const auto send_failed = [&](error failure) -> result<void>
		{
			auto failed =
				encode_task_failed_metadata({failure.code, task_value.task_id, failure.field});
			if (!failed)
				return cxxlens::sdk::unexpected(std::move(failed.error()));
			if (auto sent = writer.send(message_type::task_failed, *failed); !sent)
				return sent;
			return cxxlens::sdk::unexpected(std::move(failure));
		};
		if (callback_context.stop_requested())
			return send_failed(provider_error("provider.cancelled", "task"));
		auto outcome = provider.run(task_value, callback_context);
		if (!outcome)
			return send_failed(std::move(outcome.error()));
		if (auto valid = callback_context.validate(); !valid)
			return send_failed(std::move(valid.error()));
		auto coverage = std::move(callback_context.coverage()).finish();
		auto unresolved = std::move(callback_context.unresolved()).finish();
		auto evidence = std::move(callback_context.evidence()).finish();
		if (!coverage)
			return send_failed(std::move(coverage.error()));
		if (!unresolved)
			return send_failed(std::move(unresolved.error()));
		if (!evidence)
			return send_failed(std::move(evidence.error()));
		auto coverage_control = encode_coverage_metadata(*coverage);
		auto unresolved_control = encode_unresolved_metadata(*unresolved);
		auto evidence_control = encode_evidence_metadata(*evidence);
		if (!coverage_control)
			return cxxlens::sdk::unexpected(std::move(coverage_control.error()));
		if (!unresolved_control)
			return cxxlens::sdk::unexpected(std::move(unresolved_control.error()));
		if (!evidence_control)
			return cxxlens::sdk::unexpected(std::move(evidence_control.error()));
		for (const auto& [type, control] : {
				 std::pair{message_type::coverage_chunk, &*coverage_control},
				 std::pair{message_type::unresolved_chunk, &*unresolved_control},
				 std::pair{message_type::progress, &*evidence_control},
			 })
			if (auto sent = writer.send(type, *control); !sent)
				return sent;
		auto complete = encode_task_complete_metadata({task_value.task_id});
		if (!complete)
			return cxxlens::sdk::unexpected(std::move(complete.error()));
		return writer.send(message_type::task_complete, *complete);
	}
} // namespace cxxlens::sdk::provider
