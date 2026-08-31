#include "provider_ng1_spill_port_internal.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#if defined(__linux__) && defined(__GLIBC__)
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		constexpr std::size_t spill_length_prefix_bytes = sizeof(std::uint64_t);
		constexpr std::string_view spill_schema{"cxxlens.provider-spill-record.v1"};

		[[nodiscard]] error port_error(const std::string_view field, const std::string_view detail)
		{
			return {"provider.recovery-failed", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] error corrupt_error(const std::string_view field,
										  const std::string_view detail)
		{
			return {"provider.spill-corrupt", std::string{field}, std::string{detail}};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): ordered protocol inputs.
		[[nodiscard]] result<void> valid_semantic_digest(const std::string_view value,
														 const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U)
				return unexpected(corrupt_error(field, "semantic-v2"));
			for (const auto byte : value.substr(prefix.size()))
				if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f'))
					return unexpected(corrupt_error(field, "semantic-v2"));
			return {};
		}

		enum class cbor_major : std::uint8_t
		{
			unsigned_integer = 0U,
			bytes = 2U,
			text = 3U,
			map = 5U,
		};

		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters): encoded value and byte width are
		// ordered wire-format inputs
		void append_big_endian(
			std::vector<std::byte>& output,
			const std::uint64_t value, // NOLINT(bugprone-easily-swappable-parameters): encoded
									   // value and byte width are ordered wire-format inputs
			const std::size_t width)
		{
			for (std::size_t index = width; index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		void append_cbor_head(std::vector<std::byte>& output,
							  const cbor_major major,
							  const std::uint64_t value)
		{
			const auto prefix = static_cast<std::uint8_t>(static_cast<std::uint8_t>(major) << 5U);
			if (value < 24U)
				output.push_back(static_cast<std::byte>(prefix | static_cast<std::uint8_t>(value)));
			else if (value <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 24U));
				append_big_endian(output, value, 1U);
			}
			else if (value <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 25U));
				append_big_endian(output, value, 2U);
			}
			else if (value <= std::numeric_limits<std::uint32_t>::max())
			{
				output.push_back(static_cast<std::byte>(prefix | 26U));
				append_big_endian(output, value, 4U);
			}
			else
			{
				output.push_back(static_cast<std::byte>(prefix | 27U));
				append_big_endian(output, value, 8U);
			}
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): ordered protocol inputs.
		[[nodiscard]] result<std::vector<std::byte>> cbor_text(const std::string_view value,
															   const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			if (auto valid = validate_utf8_text(value); !valid)
				return unexpected(corrupt_error(field, "invalid-utf8"));
			try
			{
				std::vector<std::byte> output;
				output.reserve(9U + value.size());
				append_cbor_head(output, cbor_major::text, value.size());
				for (const auto byte : value)
					output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		[[nodiscard]] result<std::vector<std::byte>>
		cbor_bytes(const std::span<const std::byte> value, const std::string_view field)
		{
			try
			{
				std::vector<std::byte> output;
				output.reserve(9U + value.size());
				append_cbor_head(output, cbor_major::bytes, value.size());
				output.insert(output.end(), value.begin(), value.end());
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		struct encoded_field
		{
			std::vector<std::byte> key;
			std::vector<std::byte> value;
		};

		[[nodiscard]] constexpr std::uint64_t
		encoded_cbor_head_bytes(const std::uint64_t value) noexcept
		{
			if (value < 24U)
				return 1U;
			if (value <= std::numeric_limits<std::uint8_t>::max())
				return 2U;
			if (value <= std::numeric_limits<std::uint16_t>::max())
				return 3U;
			if (value <= std::numeric_limits<std::uint32_t>::max())
				return 5U;
			return 9U;
		}

		[[nodiscard]] result<std::uint64_t> checked_wire_add(const std::uint64_t left,
															 const std::uint64_t right,
															 const std::string_view field)
		{
			if (right > std::numeric_limits<std::uint64_t>::max() - left)
				return unexpected(corrupt_error(field, "size-overflow"));
			return left + right;
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): ordered protocol inputs.
		[[nodiscard]] result<std::uint64_t> encoded_string_wire_bytes(const std::string_view value,
																	  const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			const auto length = static_cast<std::uint64_t>(value.size());
			return checked_wire_add(encoded_cbor_head_bytes(length), length, field);
		}

		[[nodiscard]] result<void> validate_spill_record_wire_quota(const ng1_spill_record& record)
		{
			// This is the exact deterministic-CBOR size, computed without hashing or allocating.
			// Rejecting here keeps attacker-controlled payload length outside digest and codec
			// allocation paths.
			std::uint64_t total = spill_length_prefix_bytes + encoded_cbor_head_bytes(11U);
			auto add = [&total](const std::uint64_t bytes,
								const std::string_view field) -> result<void>
			{
				auto next = checked_wire_add(total, bytes, field);
				if (!next)
					return unexpected(std::move(next.error()));
				total = *next;
				return {};
			};
			auto add_text_pair = [&add](const std::string_view key,
										const std::string_view value) -> result<void>
			{
				auto key_bytes = encoded_string_wire_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				auto value_bytes = encoded_string_wire_bytes(value, key);
				if (!value_bytes)
					return unexpected(std::move(value_bytes.error()));
				if (auto outcome = add(*key_bytes, key); !outcome)
					return outcome;
				return add(*value_bytes, key);
			};
			auto add_uint_pair = [&add](const std::string_view key,
										const std::uint64_t value) -> result<void>
			{
				auto key_bytes = encoded_string_wire_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				if (auto outcome = add(*key_bytes, key); !outcome)
					return outcome;
				return add(encoded_cbor_head_bytes(value), key);
			};
			auto add_bytes_pair = [&add](const std::string_view key,
										 const std::span<const std::byte> value) -> result<void>
			{
				auto key_bytes = encoded_string_wire_bytes(key, "record-key");
				if (!key_bytes)
					return unexpected(std::move(key_bytes.error()));
				const auto length = static_cast<std::uint64_t>(value.size());
				auto value_bytes = checked_wire_add(encoded_cbor_head_bytes(length), length, key);
				if (!value_bytes)
					return unexpected(std::move(value_bytes.error()));
				if (auto outcome = add(*key_bytes, key); !outcome)
					return outcome;
				return add(*value_bytes, key);
			};

			for (const auto& outcome :
				 {add_text_pair("schema", record.schema),
				  add_uint_pair("record_ordinal", record.record_ordinal),
				  add_text_pair("task_id", record.task_id),
				  add_text_pair("dependency_group_id", record.dependency_group_id),
				  add_text_pair("atomic_output_group_id", record.atomic_output_group_id),
				  add_text_pair("batch_id", record.batch_id),
				  add_uint_pair("stream_id", record.stream_id),
				  add_uint_pair("sequence", record.sequence),
				  add_bytes_pair("payload_bytes", record.payload_bytes),
				  add_text_pair("payload_digest", record.payload_digest),
				  add_text_pair("record_digest", record.record_digest)})
				if (!outcome)
					return unexpected(outcome.error());
			if (total > ng1_spill_maximum_record_bytes)
				return unexpected(corrupt_error("record_bytes", "record-quota"));
			return {};
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_spill_record(const ng1_spill_record& record)
		{
			// The prefix stores the deterministic-CBOR body length.  The complete wire
			// record, including this eight-byte framing prefix, is the quota/accounting
			// unit shared with ng1_spill_prefix_state::spill_record_wire_bytes().
			if (record.schema != spill_schema)
				return unexpected(corrupt_error("schema", "unexpected"));
			if (auto bounded = validate_spill_record_wire_quota(record); !bounded)
				return unexpected(std::move(bounded.error()));
			if (auto digest = ng1_spill_record_digest(record); !digest)
				return unexpected(std::move(digest.error()));

			try
			{
				std::vector<encoded_field> fields;
				fields.reserve(11U);
				auto add_text = [&fields](const std::string_view key,
										  const std::string_view value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					auto encoded_value = cbor_text(value, key);
					if (!encoded_value)
						return unexpected(std::move(encoded_value.error()));
					fields.push_back({std::move(*encoded_key), std::move(*encoded_value)});
					return {};
				};
				auto add_uint = [&fields](const std::string_view key,
										  const std::uint64_t value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					std::vector<std::byte> encoded_value;
					encoded_value.reserve(9U);
					append_cbor_head(encoded_value, cbor_major::unsigned_integer, value);
					fields.push_back({std::move(*encoded_key), std::move(encoded_value)});
					return {};
				};
				auto add_bytes = [&fields](const std::string_view key,
										   const std::span<const std::byte> value) -> result<void>
				{
					auto encoded_key = cbor_text(key, "record-key");
					if (!encoded_key)
						return unexpected(std::move(encoded_key.error()));
					auto encoded_value = cbor_bytes(value, key);
					if (!encoded_value)
						return unexpected(std::move(encoded_value.error()));
					fields.push_back({std::move(*encoded_key), std::move(*encoded_value)});
					return {};
				};

				for (const auto& outcome :
					 {add_text("schema", record.schema),
					  add_uint("record_ordinal", record.record_ordinal),
					  add_text("task_id", record.task_id),
					  add_text("dependency_group_id", record.dependency_group_id),
					  add_text("atomic_output_group_id", record.atomic_output_group_id),
					  add_text("batch_id", record.batch_id),
					  add_uint("stream_id", record.stream_id),
					  add_uint("sequence", record.sequence),
					  add_bytes("payload_bytes", record.payload_bytes),
					  add_text("payload_digest", record.payload_digest),
					  add_text("record_digest", record.record_digest)})
					if (!outcome)
						return unexpected(outcome.error());

				std::ranges::sort(fields,
								  [](const encoded_field& left, const encoded_field& right)
								  {
									  if (left.key.size() != right.key.size())
										  return left.key.size() < right.key.size();
									  return std::lexicographical_compare(left.key.begin(),
																		  left.key.end(),
																		  right.key.begin(),
																		  right.key.end());
								  });

				std::vector<std::byte> body;
				body.reserve(1U + fields.size() * 8U + record.payload_bytes.size());
				append_cbor_head(body, cbor_major::map, fields.size());
				for (const auto& field : fields)
				{
					body.insert(body.end(), field.key.begin(), field.key.end());
					body.insert(body.end(), field.value.begin(), field.value.end());
				}
				if (body.size() > ng1_spill_maximum_record_bytes - spill_length_prefix_bytes)
					return unexpected(corrupt_error("record_bytes", "record-quota"));

				std::vector<std::byte> output;
				output.reserve(spill_length_prefix_bytes + body.size());
				append_big_endian(
					output, static_cast<std::uint64_t>(body.size()), spill_length_prefix_bytes);
				output.insert(output.end(), body.begin(), body.end());
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("record", "allocation"));
			}
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::size_t>>
		decode_argument(const std::span<const std::byte> input,
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
				return unexpected(corrupt_error("cbor", "truncated-or-indefinite"));
			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			// CBOR additional-info widths are 1, 2, 4, and 8 bytes.  The
			// shortest value for the latter three is 2^8, 2^16, and 2^32;
			// using width * 8 - 8 incorrectly made valid 4/8-byte arguments
			// appear non-shortest during spill recovery.
			const auto minimum = width == 1U ? 24U : (std::uint64_t{1U} << (width * 4U));
			if (value < minimum)
				return unexpected(corrupt_error("cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		using decoded_scalar = std::variant<std::uint64_t, std::string, std::span<const std::byte>>;
		using decoded_map = std::map<std::string, decoded_scalar, std::less<>>;

		template <typename T>
		[[nodiscard]] result<void> require_value(const result<T>& value)
		{
			if (!value)
				return unexpected(value.error());
			return {};
		}

		[[nodiscard]] result<std::pair<decoded_scalar, std::size_t>>
		decode_scalar(const std::span<const std::byte> input, const std::size_t offset)
		{
			if (offset >= input.size())
				return unexpected(corrupt_error("cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto major = static_cast<cbor_major>(initial >> 5U);
			auto argument = decode_argument(input, offset + 1U, initial & 0x1fU);
			if (!argument)
				return unexpected(std::move(argument.error()));
			if (major == cbor_major::unsigned_integer)
				return std::pair{decoded_scalar{argument->first}, argument->second};
			if (argument->first > input.size() - argument->second)
				return unexpected(corrupt_error("cbor", "value-length"));
			const auto begin = argument->second;
			const auto end = begin + static_cast<std::size_t>(argument->first);
			if (major == cbor_major::text)
			{
				std::string value{reinterpret_cast<const char*>(input.data() + begin),
								  static_cast<std::size_t>(argument->first)};
				if (auto valid = validate_utf8_text(value); !valid)
					return unexpected(corrupt_error("cbor", "invalid-utf8"));
				return std::pair{decoded_scalar{std::move(value)}, end};
			}
			if (major == cbor_major::bytes)
				return std::pair{
					decoded_scalar{input.subspan(begin, static_cast<std::size_t>(argument->first))},
					end};
			return unexpected(corrupt_error("cbor", "scalar-type"));
		}

		[[nodiscard]] result<decoded_map> decode_map(const std::span<const std::byte> input)
		{
			if (input.empty())
				return unexpected(corrupt_error("cbor", "empty"));
			const auto initial = std::to_integer<std::uint8_t>(input.front());
			if ((initial >> 5U) != static_cast<std::uint8_t>(cbor_major::map))
				return unexpected(corrupt_error("cbor", "map-type"));
			auto count = decode_argument(input, 1U, initial & 0x1fU);
			if (!count)
				return unexpected(std::move(count.error()));
			if (count->first != 11U)
				return unexpected(corrupt_error("cbor", "field-count"));

			try
			{
				decoded_map output;
				std::vector<std::byte> previous_key;
				std::size_t offset = count->second;
				for (std::uint64_t index{}; index < count->first; ++index)
				{
					const auto key_begin = offset;
					auto key = decode_scalar(input, offset);
					if (!key || !std::holds_alternative<std::string>(key->first))
						return unexpected(corrupt_error("cbor", "map-key-type"));
					offset = key->second;
					std::vector<std::byte> encoded_key(
						input.begin() + static_cast<std::ptrdiff_t>(key_begin),
						input.begin() + static_cast<std::ptrdiff_t>(offset));
					if (!previous_key.empty())
					{
						const auto ordered = previous_key.size() < encoded_key.size() ||
							(previous_key.size() == encoded_key.size() &&
							 std::lexicographical_compare(previous_key.begin(),
														  previous_key.end(),
														  encoded_key.begin(),
														  encoded_key.end()));
						if (!ordered)
							return unexpected(corrupt_error("cbor", "map-order-or-duplicate"));
					}
					previous_key = std::move(encoded_key);

					auto value = decode_scalar(input, offset);
					if (!value)
						return unexpected(std::move(value.error()));
					offset = value->second;
					const auto& key_text = std::get<std::string>(key->first);
					if (!output.emplace(key_text, std::move(value->first)).second)
						return unexpected(corrupt_error(key_text, "duplicate-key"));
				}
				if (offset != input.size())
					return unexpected(corrupt_error("cbor", "trailing"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("cbor", "allocation"));
			}
		}

		template <typename T>
		[[nodiscard]] result<T> map_field(const decoded_map& fields, const std::string_view name)
		{
			const auto found = fields.find(name);
			if (found == fields.end())
				return unexpected(corrupt_error(name, "missing-field"));
			const auto* value = std::get_if<T>(&found->second);
			if (value == nullptr)
				return unexpected(corrupt_error(name, "field-type"));
			return *value;
		}

		[[nodiscard]] result<ng1_spill_record>
		decode_spill_record(const std::span<const std::byte> body)
		{
			auto fields = decode_map(body);
			if (!fields)
				return unexpected(std::move(fields.error()));
			auto schema = map_field<std::string>(*fields, "schema");
			auto ordinal = map_field<std::uint64_t>(*fields, "record_ordinal");
			auto task = map_field<std::string>(*fields, "task_id");
			auto dependency = map_field<std::string>(*fields, "dependency_group_id");
			auto atomic = map_field<std::string>(*fields, "atomic_output_group_id");
			auto batch = map_field<std::string>(*fields, "batch_id");
			auto stream = map_field<std::uint64_t>(*fields, "stream_id");
			auto sequence = map_field<std::uint64_t>(*fields, "sequence");
			auto payload = map_field<std::span<const std::byte>>(*fields, "payload_bytes");
			auto payload_digest = map_field<std::string>(*fields, "payload_digest");
			auto record_digest = map_field<std::string>(*fields, "record_digest");
			for (const auto& value : {require_value(schema),
									  require_value(ordinal),
									  require_value(task),
									  require_value(dependency),
									  require_value(atomic),
									  require_value(batch),
									  require_value(stream),
									  require_value(sequence),
									  require_value(payload),
									  require_value(payload_digest),
									  require_value(record_digest)})
				if (!value)
					return unexpected(value.error());
			if (*schema != spill_schema)
				return unexpected(corrupt_error("schema", "unexpected"));

			auto expected_payload_digest = ng1_spill_payload_digest(*payload);
			if (!expected_payload_digest || *payload_digest != *expected_payload_digest)
				return unexpected(corrupt_error("payload_digest", "mismatch"));
			try
			{
				ng1_spill_record output{*schema,
										*ordinal,
										*task,
										*dependency,
										*atomic,
										*batch,
										*stream,
										*sequence,
										std::vector<std::byte>{payload->begin(), payload->end()},
										*payload_digest,
										*record_digest};
				auto expected_record_digest = ng1_spill_record_digest(output);
				if (!expected_record_digest || *record_digest != *expected_record_digest)
					return unexpected(corrupt_error("record_digest", "mismatch"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("record", "allocation"));
			}
		}

		[[nodiscard]] std::uint64_t read_big_endian(const std::span<const std::byte> input) noexcept
		{
			std::uint64_t value{};
			for (const auto byte : input)
				value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
			return value;
		}

		[[nodiscard]] result<ng1_spill_record>
		decode_framed_record(const std::span<const std::byte> bytes, std::size_t& offset)
		{
			// Validate the bounded body length before decoding the CBOR map.  The
			// eight-byte prefix remains part of the total wire-byte quota.
			if (offset > bytes.size() || bytes.size() - offset < spill_length_prefix_bytes)
				return unexpected(corrupt_error("framing", "truncated-length-prefix"));
			const auto length = read_big_endian(bytes.subspan(offset, spill_length_prefix_bytes));
			if (length == 0U || length > ng1_spill_maximum_record_bytes - spill_length_prefix_bytes)
				return unexpected(corrupt_error("record_bytes", "record-quota"));
			offset += spill_length_prefix_bytes;
			if (length > bytes.size() - offset)
				return unexpected(corrupt_error("framing", "torn-last-record"));
			const auto body = bytes.subspan(offset, static_cast<std::size_t>(length));
			offset += static_cast<std::size_t>(length);
			return decode_spill_record(body);
		}

#if defined(__linux__) && defined(__GLIBC__)
		constexpr auto spill_data_file_name_storage = std::to_array("spill.data");
		constexpr auto spill_commit_file_name_storage = std::to_array("spill.commit");
		constexpr auto spill_frontier_file_name_storage = std::to_array("spill.frontier");
		constexpr std::string_view spill_data_file_name{spill_data_file_name_storage.data(),
														spill_data_file_name_storage.size() - 1U};
		constexpr std::string_view spill_commit_file_name{
			spill_commit_file_name_storage.data(), spill_commit_file_name_storage.size() - 1U};
		constexpr std::string_view spill_frontier_file_name{
			spill_frontier_file_name_storage.data(), spill_frontier_file_name_storage.size() - 1U};
		constexpr std::string_view spill_metadata_magic{"CXXLNG1S"};
		constexpr std::string_view spill_frontier_magic{"CXXLNG1F"};
		constexpr std::uint64_t spill_maximum_metadata_bytes = 64ULL * 1024ULL;
		constexpr std::uint64_t spill_maximum_metadata_string_bytes = 4ULL * 1024ULL;

		void append_u64_be(std::vector<std::byte>& output, const std::uint64_t value)
		{
			append_big_endian(output, value, sizeof(value));
		}

		void append_magic(std::vector<std::byte>& output, const std::string_view magic)
		{
			for (const auto byte : magic)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		}

		[[nodiscard]] bool has_magic(const std::span<const std::byte> input,
									 const std::string_view magic) noexcept
		{
			if (input.size() < magic.size())
				return false;
			for (std::size_t index{}; index < magic.size(); ++index)
				if (input[index] !=
					static_cast<std::byte>(static_cast<unsigned char>(magic[index])))
					return false;
			return true;
		}

		[[nodiscard]] result<void> write_all(const int descriptor,
											 const std::span<const std::byte> bytes,
											 const std::string_view field)
		{
			std::size_t offset{};
			while (offset < bytes.size())
			{
				const auto count =
					::write(descriptor, bytes.data() + offset, bytes.size() - offset);
				if (count > 0)
				{
					offset += static_cast<std::size_t>(count);
					continue;
				}
				if (count < 0 && errno == EINTR)
					continue;
				return unexpected(port_error(field, count == 0 ? "zero-write" : "write"));
			}
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): descriptor quota contract order.
		[[nodiscard]] result<std::vector<std::byte>> read_descriptor(
			const int descriptor, const std::uint64_t maximum_bytes, const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			struct stat metadata{};
			if (::fstat(descriptor, &metadata) != 0 || metadata.st_size < 0)
				return unexpected(port_error(field, "stat"));
			const auto size = static_cast<std::uint64_t>(metadata.st_size);
			if (size > maximum_bytes ||
				size > static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
				return unexpected(corrupt_error(field, "quota"));
			try
			{
				std::vector<std::byte> output(static_cast<std::size_t>(size));
				std::size_t offset{};
				while (offset < output.size())
				{
					const auto count = ::pread(descriptor,
											   output.data() + offset,
											   output.size() - offset,
											   static_cast<off_t>(offset));
					if (count > 0)
					{
						offset += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					return unexpected(corrupt_error(field, "truncated"));
				}
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		[[nodiscard]] result<std::optional<int>> open_existing_regular_named_file(
			const int directory_descriptor,
			const std::string_view name,
			const int access_flags,
			const std::string_view field,
			const bool optional,
			const std::optional<std::pair<dev_t, ino_t>> expected_identity = std::nullopt)
		{
			const std::string native_name{name};
			struct stat named_metadata{};
			if (::fstatat(directory_descriptor,
						  native_name.c_str(),
						  &named_metadata,
						  AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (optional && errno == ENOENT)
					return std::optional<int>{};
				return unexpected(port_error(field, "stat"));
			}
			if (!S_ISREG(named_metadata.st_mode) || named_metadata.st_nlink != 1)
				return unexpected(corrupt_error(field, "not-regular"));
			if (expected_identity &&
				(named_metadata.st_dev != expected_identity->first ||
				 named_metadata.st_ino != expected_identity->second))
				return unexpected(port_error(field, "identity"));

			const auto descriptor = ::openat(directory_descriptor,
											 native_name.c_str(),
											 access_flags | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
			if (descriptor < 0)
				return unexpected(port_error(field, "open"));
			struct stat descriptor_metadata{};
			struct stat post_open_named_metadata{};
			if (::fstat(descriptor, &descriptor_metadata) != 0 ||
				::fstatat(directory_descriptor,
						  native_name.c_str(),
						  &post_open_named_metadata,
						  AT_SYMLINK_NOFOLLOW) != 0 ||
				!S_ISREG(descriptor_metadata.st_mode) || descriptor_metadata.st_nlink != 1 ||
				!S_ISREG(post_open_named_metadata.st_mode) ||
				post_open_named_metadata.st_nlink != 1 ||
				descriptor_metadata.st_dev != named_metadata.st_dev ||
				descriptor_metadata.st_ino != named_metadata.st_ino ||
				post_open_named_metadata.st_dev != named_metadata.st_dev ||
				post_open_named_metadata.st_ino != named_metadata.st_ino ||
				(expected_identity &&
				 (descriptor_metadata.st_dev != expected_identity->first ||
				  descriptor_metadata.st_ino != expected_identity->second)))
			{
				(void)::close(descriptor);
				return unexpected(port_error(field, "identity"));
			}
			return std::optional<int>{descriptor};
		}

		[[nodiscard]] result<std::optional<std::vector<std::byte>>>
		read_named_file(const int directory_descriptor,
						const std::string_view name,
						const std::uint64_t maximum_bytes,
						const std::string_view field,
						const bool optional)
		{
			auto opened = open_existing_regular_named_file(
				directory_descriptor, name, O_RDONLY, field, optional);
			if (!opened)
				return unexpected(std::move(opened.error()));
			if (!opened->has_value())
				return std::optional<std::vector<std::byte>>{};
			const auto descriptor = **opened;
			auto output = read_descriptor(descriptor, maximum_bytes, field);
			if (::close(descriptor) != 0 && output)
				return unexpected(port_error(field, "close"));
			if (!output)
				return unexpected(std::move(output.error()));
			return std::optional<std::vector<std::byte>>{std::move(*output)};
		}

		[[nodiscard]] result<void> fsync_directory(const int descriptor)
		{
			if (::fsync(descriptor) != 0)
				return unexpected(port_error("parent_directory", "fsync"));
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): path and diagnostic field order.
		[[nodiscard]] result<std::pair<dev_t, ino_t>> capture_directory_identity(
			const int descriptor, const std::string_view path, const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			const std::string native_path{path};
			struct stat descriptor_metadata{};
			struct stat path_metadata{};
			if (::fstat(descriptor, &descriptor_metadata) != 0 ||
				!S_ISDIR(descriptor_metadata.st_mode) ||
				::lstat(native_path.c_str(), &path_metadata) != 0 ||
				!S_ISDIR(path_metadata.st_mode) ||
				descriptor_metadata.st_dev != path_metadata.st_dev ||
				descriptor_metadata.st_ino != path_metadata.st_ino)
				return unexpected(port_error(field, "identity"));
			return std::pair{descriptor_metadata.st_dev, descriptor_metadata.st_ino};
		}

		[[nodiscard]] result<void> verify_directory_identity(const int descriptor,
															 const std::string_view path,
															 const dev_t expected_device,
															 const ino_t expected_inode,
															 const std::string_view field)
		{
			const std::string native_path{path};
			struct stat descriptor_metadata{};
			struct stat path_metadata{};
			if (::fstat(descriptor, &descriptor_metadata) != 0 ||
				!S_ISDIR(descriptor_metadata.st_mode) ||
				::lstat(native_path.c_str(), &path_metadata) != 0 ||
				!S_ISDIR(path_metadata.st_mode) || descriptor_metadata.st_dev != expected_device ||
				descriptor_metadata.st_ino != expected_inode ||
				path_metadata.st_dev != expected_device || path_metadata.st_ino != expected_inode)
				return unexpected(port_error(field, "identity"));
			return {};
		}

		[[nodiscard]] result<void> verify_data_identity(const int directory_descriptor,
														const int descriptor,
														const dev_t expected_device,
														const ino_t expected_inode,
														const std::string_view field)
		{
			struct stat descriptor_metadata{};
			struct stat path_metadata{};
			if (::fstat(descriptor, &descriptor_metadata) != 0 ||
				::fstatat(directory_descriptor,
						  spill_data_file_name_storage.data(),
						  &path_metadata,
						  AT_SYMLINK_NOFOLLOW) != 0 ||
				!S_ISREG(descriptor_metadata.st_mode) || !S_ISREG(path_metadata.st_mode) ||
				descriptor_metadata.st_nlink != 1 || path_metadata.st_nlink != 1 ||
				descriptor_metadata.st_dev != expected_device ||
				descriptor_metadata.st_ino != expected_inode ||
				path_metadata.st_dev != expected_device || path_metadata.st_ino != expected_inode)
				return unexpected(port_error(field, "identity"));
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): name and diagnostic field order.
		[[nodiscard]] result<void> remove_regular_named_file(const int directory_descriptor,
															 const std::string_view name,
															 const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			const std::string native_name{name};
			struct stat metadata{};
			if (::fstatat(
					directory_descriptor, native_name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT)
					return {};
				return unexpected(port_error(field, "stale-file"));
			}
			if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)
				return unexpected(port_error(field, "stale-file"));
			if (::unlinkat(directory_descriptor, native_name.c_str(), 0) != 0 && errno != ENOENT)
				return unexpected(port_error(field, "unlink"));
			return {};
		}

		// NOLINTBEGIN(bugprone-easily-swappable-parameters): name and diagnostic field order.
		[[nodiscard]] result<void>
		validate_regular_destination_or_missing(const int directory_descriptor,
												const std::string_view name,
												const std::string_view field)
		{
			// NOLINTEND(bugprone-easily-swappable-parameters)
			const std::string native_name{name};
			struct stat metadata{};
			if (::fstatat(
					directory_descriptor, native_name.c_str(), &metadata, AT_SYMLINK_NOFOLLOW) != 0)
			{
				if (errno == ENOENT)
					return {};
				return unexpected(port_error(field, "destination-stat"));
			}
			if (!S_ISREG(metadata.st_mode) || metadata.st_nlink != 1)
				return unexpected(corrupt_error(field, "destination-not-regular"));
			return {};
		}

		[[nodiscard]] result<void> write_atomic_named_file(const int directory_descriptor,
														   const std::string_view name,
														   const std::span<const std::byte> bytes,
														   const std::string_view field)
		{
			const std::string native_name{name};
			std::string temporary_name;
			try
			{
				temporary_name = name;
				temporary_name.append(".tmp");
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
			if (auto removed =
					remove_regular_named_file(directory_descriptor, temporary_name, "stale-temp");
				!removed)
				return unexpected(std::move(removed.error()));
			const auto descriptor =
				::openat(directory_descriptor,
						 temporary_name.c_str(),
						 O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
						 static_cast<mode_t>(0600));
			if (descriptor < 0)
				return unexpected(port_error(field, "temp-open"));
			auto written = write_all(descriptor, bytes, field);
			if (written && ::fsync(descriptor) != 0)
				written = unexpected(port_error(field, "temp-fsync"));
			if (::close(descriptor) != 0 && written)
				written = unexpected(port_error(field, "temp-close"));
			if (!written)
			{
				(void)remove_regular_named_file(directory_descriptor, temporary_name, "stale-temp");
				return written;
			}
			if (auto destination =
					validate_regular_destination_or_missing(directory_descriptor, name, field);
				!destination)
			{
				(void)remove_regular_named_file(directory_descriptor, temporary_name, "stale-temp");
				return unexpected(std::move(destination.error()));
			}
			if (::renameat(directory_descriptor,
						   temporary_name.c_str(),
						   directory_descriptor,
						   native_name.c_str()) != 0)
			{
				(void)remove_regular_named_file(directory_descriptor, temporary_name, "stale-temp");
				return unexpected(port_error(field, "rename"));
			}
			return fsync_directory(directory_descriptor);
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_commit_metadata(const std::uint64_t byte_count, const std::uint64_t fsync_sequence)
		{
			try
			{
				std::vector<std::byte> output;
				output.reserve(spill_metadata_magic.size() + 16U);
				append_magic(output, spill_metadata_magic);
				append_u64_be(output, byte_count);
				append_u64_be(output, fsync_sequence);
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("commit", "allocation"));
			}
		}

		[[nodiscard]] result<std::pair<std::uint64_t, std::uint64_t>>
		decode_commit_metadata(const std::span<const std::byte> bytes)
		{
			if (bytes.size() != spill_metadata_magic.size() + 16U ||
				!has_magic(bytes, spill_metadata_magic))
				return unexpected(corrupt_error("commit", "header"));
			const auto byte_count = read_big_endian(bytes.subspan(spill_metadata_magic.size(), 8U));
			const auto fsync_sequence =
				read_big_endian(bytes.subspan(spill_metadata_magic.size() + 8U, 8U));
			if (byte_count > ng1_spill_maximum_total_bytes)
				return unexpected(corrupt_error("commit", "total-quota"));
			return std::pair{byte_count, fsync_sequence};
		}

		void append_metadata_string(std::vector<std::byte>& output, const std::string_view value)
		{
			append_u64_be(output, static_cast<std::uint64_t>(value.size()));
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		}

		void append_metadata_u64(std::vector<std::byte>& output, const std::uint64_t value)
		{
			append_u64_be(output, value);
		}

		[[nodiscard]] result<std::string>
		read_metadata_string(const std::span<const std::byte> bytes,
							 std::size_t& offset,
							 const std::string_view field)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t))
				return unexpected(corrupt_error(field, "length"));
			const auto size = read_big_endian(bytes.subspan(offset, sizeof(std::uint64_t)));
			offset += sizeof(std::uint64_t);
			if (size > spill_maximum_metadata_string_bytes || size > bytes.size() - offset)
				return unexpected(corrupt_error(field, "quota"));
			try
			{
				std::string output;
				output.reserve(static_cast<std::size_t>(size));
				for (const auto byte : bytes.subspan(offset, static_cast<std::size_t>(size)))
					output.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
				offset += static_cast<std::size_t>(size);
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error(field, "allocation"));
			}
		}

		[[nodiscard]] result<std::uint64_t>
		read_metadata_u64(const std::span<const std::byte> bytes,
						  std::size_t& offset,
						  const std::string_view field)
		{
			if (offset > bytes.size() || bytes.size() - offset < sizeof(std::uint64_t))
				return unexpected(corrupt_error(field, "length"));
			const auto value = read_big_endian(bytes.subspan(offset, sizeof(std::uint64_t)));
			offset += sizeof(std::uint64_t);
			return value;
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_frontier_metadata(const ng1_spill_resume_frontier& frontier)
		{
			try
			{
				const auto& receipt = frontier.receipt;
				std::vector<std::byte> output;
				output.reserve(512U);
				append_magic(output, spill_frontier_magic);
				append_metadata_string(output, receipt.schema);
				append_metadata_string(output, receipt.provider_id);
				append_metadata_string(output, receipt.protocol_session_id);
				append_metadata_string(output, receipt.task_id);
				append_metadata_u64(output, receipt.stream_id);
				append_metadata_u64(output, receipt.highest_contiguous_acked_sequence);
				append_metadata_string(output, receipt.staged_digest);
				append_metadata_string(output, receipt.spill_digest);
				append_metadata_u64(output, receipt.total_bytes);
				append_metadata_u64(output, receipt.total_records);
				append_metadata_u64(output, receipt.fsync_sequence);
				append_metadata_u64(output, frontier.resume_generation);
				if (output.size() > spill_maximum_metadata_bytes)
					return unexpected(corrupt_error("resume_frontier", "quota"));
				return output;
			}
			catch (const std::bad_alloc&)
			{
				return unexpected(port_error("resume_frontier", "allocation"));
			}
		}

		[[nodiscard]] result<ng1_spill_resume_frontier>
		decode_frontier_metadata(const std::span<const std::byte> bytes)
		{
			if (bytes.size() > spill_maximum_metadata_bytes ||
				bytes.size() < spill_frontier_magic.size() ||
				!has_magic(bytes, spill_frontier_magic))
				return unexpected(corrupt_error("resume_frontier", "header"));
			std::size_t offset = spill_frontier_magic.size();
			auto schema = read_metadata_string(bytes, offset, "schema");
			auto provider_id = read_metadata_string(bytes, offset, "provider_id");
			auto protocol_session_id = read_metadata_string(bytes, offset, "protocol_session_id");
			auto task_id = read_metadata_string(bytes, offset, "task_id");
			auto stream_id = read_metadata_u64(bytes, offset, "stream_id");
			auto highest_ack = read_metadata_u64(bytes, offset, "highest_ack");
			auto staged_digest = read_metadata_string(bytes, offset, "staged_digest");
			auto spill_digest = read_metadata_string(bytes, offset, "spill_digest");
			auto total_bytes = read_metadata_u64(bytes, offset, "total_bytes");
			auto total_records = read_metadata_u64(bytes, offset, "total_records");
			auto fsync_sequence = read_metadata_u64(bytes, offset, "fsync_sequence");
			auto resume_generation = read_metadata_u64(bytes, offset, "resume_generation");
			if (!schema || !provider_id || !protocol_session_id || !task_id || !stream_id ||
				!highest_ack || !staged_digest || !spill_digest || !total_bytes || !total_records ||
				!fsync_sequence || !resume_generation || offset != bytes.size())
				return unexpected(corrupt_error("resume_frontier", "fields"));
			ng1_spill_resume_frontier output{{*schema,
											  *provider_id,
											  *protocol_session_id,
											  *task_id,
											  *stream_id,
											  *highest_ack,
											  *staged_digest,
											  *spill_digest,
											  *total_bytes,
											  *total_records,
											  *fsync_sequence},
											 *resume_generation};
			if (auto valid = output.validate(); !valid)
				return unexpected(std::move(valid.error()));
			return output;
		}

		class temporary_spill_directory_guard
		{
		  public:
			explicit temporary_spill_directory_guard(const char* path) noexcept : path_{path}
			{
				struct stat metadata{};
				if (::lstat(path_, &metadata) == 0 && S_ISDIR(metadata.st_mode))
				{
					device_ = metadata.st_dev;
					inode_ = metadata.st_ino;
					identity_captured_ = true;
				}
			}

			temporary_spill_directory_guard(const temporary_spill_directory_guard&) = delete;
			temporary_spill_directory_guard&
			operator=(const temporary_spill_directory_guard&) = delete;

			~temporary_spill_directory_guard() noexcept
			{
				if (!active_ || !identity_captured_)
					return;
				const auto descriptor =
					::open(path_, O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
				if (descriptor < 0)
					return;
				struct stat descriptor_metadata{};
				struct stat path_metadata{};
				if (::fstat(descriptor, &descriptor_metadata) != 0 ||
					::lstat(path_, &path_metadata) != 0 || descriptor_metadata.st_dev != device_ ||
					descriptor_metadata.st_ino != inode_ || path_metadata.st_dev != device_ ||
					path_metadata.st_ino != inode_)
				{
					(void)::close(descriptor);
					return;
				}
				for (const auto name : {"spill.data",
										"spill.commit",
										"spill.frontier",
										"spill.commit.tmp",
										"spill.frontier.tmp"})
				{
					struct stat metadata{};
					if (::fstatat(descriptor, name, &metadata, AT_SYMLINK_NOFOLLOW) == 0 &&
						S_ISREG(metadata.st_mode) && metadata.st_nlink == 1)
						(void)::unlinkat(descriptor, name, 0);
				}
				(void)::close(descriptor);
				if (::lstat(path_, &path_metadata) == 0 && path_metadata.st_dev == device_ &&
					path_metadata.st_ino == inode_)
					(void)::rmdir(path_);
			}

			void release() noexcept
			{
				active_ = false;
			}

		  private:
			const char* path_{};
			dev_t device_{};
			ino_t inode_{};
			bool identity_captured_{};
			bool active_{true};
		};

		class linux_ng1_spill_storage_port final : public ng1_spill_storage_port
		{
		  public:
			~linux_ng1_spill_storage_port() override
			{
				if (!cleaned_)
					(void)cleanup();
				if (descriptor_ >= 0)
					(void)::close(descriptor_);
				if (directory_descriptor_ >= 0)
					(void)::close(directory_descriptor_);
			}

			[[nodiscard]] static result<std::unique_ptr<linux_ng1_spill_storage_port>> create_new()
			{
				auto directory_template = std::to_array("/tmp/cxxlens-ng1-spill-XXXXXX");
				const auto directory = ::mkdtemp(directory_template.data());
				if (directory == nullptr)
					return unexpected(port_error("platform", "temp-directory"));
				temporary_spill_directory_guard directory_guard{directory_template.data()};
				std::string directory_path;
				std::string data_path;
				std::string commit_path;
				std::string frontier_path;
				try
				{
					directory_path = directory;
					data_path = directory_path;
					data_path.append("/");
					data_path.append(spill_data_file_name);
					commit_path = directory_path;
					commit_path.append("/");
					commit_path.append(spill_commit_file_name);
					frontier_path = directory_path;
					frontier_path.append("/");
					frontier_path.append(spill_frontier_file_name);
				}
				catch (const std::bad_alloc&)
				{
					return unexpected(port_error("platform", "allocation"));
				}
				const auto directory_descriptor =
					::open(directory_path.c_str(),
						   O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
				if (directory_descriptor < 0)
				{
					(void)::rmdir(directory_path.c_str());
					return unexpected(port_error("platform", "temp-directory-open"));
				}
				auto directory_identity = capture_directory_identity(
					directory_descriptor, directory_path, "parent_directory");
				if (!directory_identity)
				{
					(void)::close(directory_descriptor);
					(void)::rmdir(directory_path.c_str());
					return unexpected(std::move(directory_identity.error()));
				}
				const auto descriptor =
					::openat(directory_descriptor,
							 spill_data_file_name_storage.data(),
							 O_RDWR | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW,
							 static_cast<mode_t>(0600));
				if (descriptor < 0)
				{
					(void)::close(directory_descriptor);
					(void)::rmdir(directory_path.c_str());
					return unexpected(port_error("platform", "spill-file-open"));
				}
				bool failed{};
				if (::fsync(descriptor) != 0 || ::fsync(directory_descriptor) != 0)
					failed = true;
				struct stat data_metadata{};
				if (!failed &&
					(::fstat(descriptor, &data_metadata) != 0 || !S_ISREG(data_metadata.st_mode) ||
					 data_metadata.st_nlink != 1))
					failed = true;
				if (!failed)
				{
					auto initial_commit = encode_commit_metadata(0U, 0U);
					if (!initial_commit ||
						!write_atomic_named_file(directory_descriptor,
												 spill_commit_file_name,
												 *initial_commit,
												 "commit"))
						failed = true;
				}
				if (failed)
				{
					(void)::close(descriptor);
					(void)::unlinkat(directory_descriptor, spill_data_file_name_storage.data(), 0);
					(void)::unlinkat(
						directory_descriptor, spill_commit_file_name_storage.data(), 0);
					(void)::unlinkat(directory_descriptor, "spill.commit.tmp", 0);
					(void)::close(directory_descriptor);
					(void)::rmdir(directory_path.c_str());
					return unexpected(port_error("platform", "spill-initialize"));
				}
				try
				{
					auto output = std::unique_ptr<linux_ng1_spill_storage_port>{
						new linux_ng1_spill_storage_port(descriptor,
														 directory_descriptor,
														 directory_path,
														 data_path,
														 commit_path,
														 frontier_path,
														 directory_identity->first,
														 directory_identity->second,
														 data_metadata.st_dev,
														 data_metadata.st_ino,
														 0U,
														 0U,
														 0U,
														 std::nullopt,
														 true)};
					directory_guard.release();
					return output;
				}
				catch (const std::bad_alloc&)
				{
					(void)::close(descriptor);
					(void)::unlinkat(directory_descriptor, spill_data_file_name_storage.data(), 0);
					(void)::unlinkat(
						directory_descriptor, spill_commit_file_name_storage.data(), 0);
					(void)::close(directory_descriptor);
					(void)::rmdir(directory_path.c_str());
					return unexpected(port_error("platform", "allocation"));
				}
			}

			// NOLINTBEGIN(bugprone-easily-swappable-parameters): persisted identity tuple order.
			[[nodiscard]] static result<std::unique_ptr<linux_ng1_spill_storage_port>>
			open_existing(const std::string& directory_path,
						  const std::string& data_path,
						  const std::string& commit_path,
						  const std::string& frontier_path,
						  const dev_t expected_directory_device,
						  const ino_t expected_directory_inode,
						  const dev_t expected_data_device,
						  const ino_t expected_data_inode,
						  const bool cleanup_custody)
			{
				// NOLINTEND(bugprone-easily-swappable-parameters)
				const auto directory_descriptor =
					::open(directory_path.c_str(),
						   O_RDONLY | O_DIRECTORY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
				if (directory_descriptor < 0)
					return unexpected(port_error("reopen", "parent-directory-open"));
				auto directory_identity = capture_directory_identity(
					directory_descriptor, directory_path, "parent_directory");
				if (!directory_identity)
				{
					(void)::close(directory_descriptor);
					return unexpected(std::move(directory_identity.error()));
				}
				if (directory_identity->first != expected_directory_device ||
					directory_identity->second != expected_directory_inode)
				{
					(void)::close(directory_descriptor);
					return unexpected(port_error("reopen", "parent-directory-identity"));
				}
				auto opened = open_existing_regular_named_file(
					directory_descriptor,
					spill_data_file_name,
					O_RDWR,
					"spill_file",
					false,
					std::pair{expected_data_device, expected_data_inode});
				if (!opened)
				{
					(void)::close(directory_descriptor);
					return unexpected(std::move(opened.error()));
				}
				const auto descriptor = **opened;
				struct stat data_metadata{};
				if (::fstat(descriptor, &data_metadata) != 0 || !S_ISREG(data_metadata.st_mode) ||
					data_metadata.st_nlink != 1 || data_metadata.st_dev != expected_data_device ||
					data_metadata.st_ino != expected_data_inode)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(port_error("reopen", "spill-file-identity"));
				}
				auto commit_bytes = read_named_file(directory_descriptor,
													spill_commit_file_name,
													spill_metadata_magic.size() + 16U,
													"commit",
													false);
				if (!commit_bytes)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(std::move(commit_bytes.error()));
				}
				auto commit = decode_commit_metadata(**commit_bytes);
				if (!commit)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(std::move(commit.error()));
				}
				struct stat metadata{};
				if (::fstat(descriptor, &metadata) != 0 || metadata.st_size < 0)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(port_error("reopen", "spill-file-stat"));
				}
				const auto stored_size = static_cast<std::uint64_t>(metadata.st_size);
				const auto committed_size = commit->first;
				if (stored_size < committed_size || committed_size > ng1_spill_maximum_total_bytes)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(corrupt_error("reopen", "commit-size"));
				}
				std::optional<ng1_spill_resume_frontier> frontier;
				auto frontier_bytes = read_named_file(directory_descriptor,
													  spill_frontier_file_name,
													  spill_maximum_metadata_bytes,
													  "resume_frontier",
													  true);
				if (!frontier_bytes)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(std::move(frontier_bytes.error()));
				}
				if (frontier_bytes->has_value())
				{
					auto decoded = decode_frontier_metadata(**frontier_bytes);
					if (!decoded)
					{
						(void)::close(descriptor);
						(void)::close(directory_descriptor);
						return unexpected(std::move(decoded.error()));
					}
					if (decoded->receipt.fsync_sequence > commit->second ||
						decoded->receipt.total_bytes > committed_size ||
						decoded->receipt.total_bytes > stored_size)
					{
						(void)::close(descriptor);
						(void)::close(directory_descriptor);
						return unexpected(corrupt_error("resume_frontier", "commit-mismatch"));
					}
					frontier = std::move(*decoded);
				}
				const auto published_size = frontier ? frontier->receipt.total_bytes : 0U;
				const auto published_sequence = frontier ? frontier->receipt.fsync_sequence : 0U;
				if (stored_size != published_size)
				{
					if (::ftruncate(descriptor, static_cast<off_t>(published_size)) != 0 ||
						::fsync(descriptor) != 0)
					{
						(void)::close(descriptor);
						(void)::close(directory_descriptor);
						return unexpected(port_error("reopen", "discard-unpublished-tail"));
					}
					if (auto synced = fsync_directory(directory_descriptor); !synced)
					{
						(void)::close(descriptor);
						(void)::close(directory_descriptor);
						return unexpected(std::move(synced.error()));
					}
				}
				if (commit->first != published_size || commit->second != published_sequence)
				{
					auto rewritten = encode_commit_metadata(published_size, published_sequence);
					if (!rewritten ||
						!write_atomic_named_file(
							directory_descriptor, spill_commit_file_name, *rewritten, "commit"))
					{
						(void)::close(descriptor);
						(void)::close(directory_descriptor);
						return unexpected(port_error("reopen", "rewrite-commit"));
					}
				}
				try
				{
					return std::unique_ptr<linux_ng1_spill_storage_port>{
						new linux_ng1_spill_storage_port(descriptor,
														 directory_descriptor,
														 std::string{directory_path},
														 std::string{data_path},
														 std::string{commit_path},
														 std::string{frontier_path},
														 directory_identity->first,
														 directory_identity->second,
														 data_metadata.st_dev,
														 data_metadata.st_ino,
														 published_size,
														 published_size,
														 published_sequence,
														 std::move(frontier),
														 cleanup_custody)};
				}
				catch (const std::bad_alloc&)
				{
					(void)::close(descriptor);
					(void)::close(directory_descriptor);
					return unexpected(port_error("reopen", "allocation"));
				}
			}

			[[nodiscard]] result<void> append(const std::span<const std::byte> bytes) override
			{
				if (descriptor_ < 0 || poisoned_ || cleaned_)
					return unexpected(port_error("append", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
				{
					poisoned_ = true;
					return unexpected(std::move(identity.error()));
				}
				if (auto identity = verify_data_identity(directory_descriptor_,
														 descriptor_,
														 data_device_,
														 data_inode_,
														 "spill_file");
					!identity)
				{
					poisoned_ = true;
					return unexpected(std::move(identity.error()));
				}
				if (bytes.size() > ng1_spill_maximum_total_bytes - byte_count_)
					return unexpected(corrupt_error("total_bytes", "total-quota"));
				const auto maximum_offset =
					static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
				if (byte_count_ > maximum_offset || bytes.size() > maximum_offset - byte_count_)
					return unexpected(port_error("append", "offset-overflow"));
				std::size_t consumed{};
				while (consumed < bytes.size())
				{
					const auto count = ::pwrite(descriptor_,
												bytes.data() + consumed,
												bytes.size() - consumed,
												static_cast<off_t>(byte_count_ + consumed));
					if (count > 0)
					{
						consumed += static_cast<std::size_t>(count);
						continue;
					}
					if (count < 0 && errno == EINTR)
						continue;
					poisoned_ = true;
					return unexpected(port_error("append", count == 0 ? "zero-write" : "write"));
				}
				byte_count_ += static_cast<std::uint64_t>(bytes.size());
				return {};
			}

			[[nodiscard]] result<std::uint64_t> fsync() override
			{
				if (descriptor_ < 0 || directory_descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("fsync", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
				{
					poisoned_ = true;
					return unexpected(std::move(identity.error()));
				}
				if (auto identity = verify_data_identity(directory_descriptor_,
														 descriptor_,
														 data_device_,
														 data_inode_,
														 "spill_file");
					!identity)
				{
					poisoned_ = true;
					return unexpected(std::move(identity.error()));
				}
				if (fsync_sequence_ == std::numeric_limits<std::uint64_t>::max())
				{
					poisoned_ = true;
					return unexpected(port_error("fsync_sequence", "overflow"));
				}
				if (::fsync(descriptor_) != 0)
				{
					poisoned_ = true;
					return unexpected(port_error("fsync", "durability-unknown"));
				}
				const auto next_sequence = fsync_sequence_ + 1U;
				auto encoded = encode_commit_metadata(byte_count_, next_sequence);
				if (!encoded)
				{
					poisoned_ = true;
					return unexpected(std::move(encoded.error()));
				}
				if (auto committed = write_atomic_named_file(
						directory_descriptor_, spill_commit_file_name, *encoded, "commit");
					!committed)
				{
					poisoned_ = true;
					return unexpected(std::move(committed.error()));
				}
				committed_byte_count_ = byte_count_;
				fsync_sequence_ = next_sequence;
				return fsync_sequence_;
			}

			[[nodiscard]] result<std::vector<std::byte>> read_all() const override
			{
				if (descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("read", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
					return unexpected(std::move(identity.error()));
				if (auto identity = verify_data_identity(directory_descriptor_,
														 descriptor_,
														 data_device_,
														 data_inode_,
														 "spill_file");
					!identity)
					return unexpected(std::move(identity.error()));
				struct stat metadata{};
				if (::fstat(descriptor_, &metadata) != 0 || metadata.st_size < 0)
					return unexpected(port_error("read", "stat"));
				const auto size = static_cast<std::uint64_t>(metadata.st_size);
				if (size > ng1_spill_maximum_total_bytes || size != byte_count_)
					return unexpected(corrupt_error("total_bytes", "storage-drift"));
				return read_descriptor(descriptor_, ng1_spill_maximum_total_bytes, "read");
			}

			[[nodiscard]] result<std::optional<ng1_spill_resume_frontier>>
			read_resume_frontier() const override
			{
				if (descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("resume_frontier", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
					return unexpected(std::move(identity.error()));
				if (auto identity = verify_data_identity(directory_descriptor_,
														 descriptor_,
														 data_device_,
														 data_inode_,
														 "spill_file");
					!identity)
					return unexpected(std::move(identity.error()));
				auto commit_bytes = read_named_file(directory_descriptor_,
													spill_commit_file_name,
													spill_metadata_magic.size() + 16U,
													"commit",
													false);
				if (!commit_bytes)
					return unexpected(std::move(commit_bytes.error()));
				auto commit = decode_commit_metadata(**commit_bytes);
				if (!commit)
					return unexpected(std::move(commit.error()));
				struct stat metadata{};
				if (::fstat(descriptor_, &metadata) != 0 || metadata.st_size < 0)
					return unexpected(port_error("resume_frontier", "spill-file-stat"));
				const auto stored_size = static_cast<std::uint64_t>(metadata.st_size);
				if (commit->first > stored_size || commit->first > ng1_spill_maximum_total_bytes)
					return unexpected(corrupt_error("resume_frontier", "commit-size"));
				auto frontier_bytes = read_named_file(directory_descriptor_,
													  spill_frontier_file_name,
													  spill_maximum_metadata_bytes,
													  "resume_frontier",
													  true);
				if (!frontier_bytes)
					return unexpected(std::move(frontier_bytes.error()));
				if (!frontier_bytes->has_value())
					return std::optional<ng1_spill_resume_frontier>{};
				auto decoded = decode_frontier_metadata(**frontier_bytes);
				if (!decoded)
					return unexpected(std::move(decoded.error()));
				if (decoded->receipt.fsync_sequence > commit->second ||
					decoded->receipt.total_bytes > commit->first ||
					decoded->receipt.total_bytes > stored_size)
					return unexpected(corrupt_error("resume_frontier", "commit-mismatch"));
				return std::optional<ng1_spill_resume_frontier>{std::move(*decoded)};
			}

			[[nodiscard]] result<std::unique_ptr<ng1_spill_storage_port>> reopen() const override
			{
				if (descriptor_ < 0 || directory_descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("reopen", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
					return unexpected(std::move(identity.error()));
				auto reopened = open_existing(directory_path_,
											  data_path_,
											  commit_path_,
											  frontier_path_,
											  directory_device_,
											  directory_inode_,
											  data_device_,
											  data_inode_,
											  false);
				if (!reopened)
					return unexpected(std::move(reopened.error()));
				return std::unique_ptr<ng1_spill_storage_port>{std::move(*reopened)};
			}

			[[nodiscard]] result<void>
			transfer_cleanup_custody_to(ng1_spill_storage_port& replacement) override
			{
				auto* target = dynamic_cast<linux_ng1_spill_storage_port*>(&replacement);
				if (target == nullptr || target == this || !cleanup_custody_ ||
					target->cleanup_custody_ || target->cleaned_ || target->descriptor_ < 0 ||
					target->directory_descriptor_ < 0)
					return unexpected(port_error("cleanup", "custody-transfer"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
					return unexpected(std::move(identity.error()));
				if (auto identity = verify_data_identity(directory_descriptor_,
														 descriptor_,
														 data_device_,
														 data_inode_,
														 "spill_file");
					!identity)
					return unexpected(std::move(identity.error()));
				if (target->directory_device_ != directory_device_ ||
					target->directory_inode_ != directory_inode_ ||
					target->data_device_ != data_device_ || target->data_inode_ != data_inode_)
					return unexpected(port_error("cleanup", "custody-identity"));
				if (auto identity = verify_directory_identity(target->directory_descriptor_,
															  target->directory_path_,
															  target->directory_device_,
															  target->directory_inode_,
															  "parent_directory");
					!identity)
					return unexpected(std::move(identity.error()));
				if (auto identity = verify_data_identity(target->directory_descriptor_,
														 target->descriptor_,
														 target->data_device_,
														 target->data_inode_,
														 "spill_file");
					!identity)
					return unexpected(std::move(identity.error()));
				cleanup_custody_ = false;
				target->cleanup_custody_ = true;
				return {};
			}

			[[nodiscard]] result<void>
			persist_resume_frontier(const ng1_spill_resume_frontier& frontier) override
			{
				if (descriptor_ < 0 || directory_descriptor_ < 0 || cleaned_ || poisoned_)
					return unexpected(port_error("resume_frontier", "terminal-port"));
				if (auto identity = verify_directory_identity(directory_descriptor_,
															  directory_path_,
															  directory_device_,
															  directory_inode_,
															  "parent_directory");
					!identity)
				{
					poisoned_ = true;
					return unexpected(std::move(identity.error()));
				}
				if (auto data_identity = verify_data_identity(directory_descriptor_,
															  descriptor_,
															  data_device_,
															  data_inode_,
															  "spill_file");
					!data_identity)
				{
					poisoned_ = true;
					return unexpected(std::move(data_identity.error()));
				}
				if (auto valid = frontier.validate(); !valid)
					return unexpected(std::move(valid.error()));
				if (frontier.receipt.fsync_sequence != fsync_sequence_ ||
					frontier.receipt.total_bytes != committed_byte_count_)
					return unexpected(corrupt_error("resume_frontier", "commit-mismatch"));
				if (frontier_ &&
					(frontier.resume_generation <= frontier_->resume_generation ||
					 frontier.receipt.fsync_sequence <= frontier_->receipt.fsync_sequence))
					return unexpected(
						error{"provider.resume-token-stale", "resume_frontier", "not-increasing"});
				auto encoded = encode_frontier_metadata(frontier);
				if (!encoded)
				{
					poisoned_ = true;
					return unexpected(std::move(encoded.error()));
				}
				if (auto persisted = write_atomic_named_file(directory_descriptor_,
															 spill_frontier_file_name,
															 *encoded,
															 "resume_frontier");
					!persisted)
				{
					poisoned_ = true;
					return unexpected(std::move(persisted.error()));
				}
				frontier_ = frontier;
				return {};
			}

			[[nodiscard]] result<void> cleanup() override
			{
				if (cleaned_)
					return unexpected(port_error("cleanup", "already-terminal"));
				cleaned_ = true;
				bool failed{};
				const auto descriptor = std::exchange(descriptor_, -1);
				if (descriptor < 0 || ::close(descriptor) != 0)
					failed = true;
				const auto directory_descriptor = std::exchange(directory_descriptor_, -1);
				if (directory_descriptor >= 0 && cleanup_custody_)
					for (const auto name : {spill_data_file_name,
											spill_commit_file_name,
											spill_frontier_file_name,
											std::string_view{"spill.commit.tmp"},
											std::string_view{"spill.frontier.tmp"}})
					{
						auto removed =
							remove_regular_named_file(directory_descriptor, name, "cleanup");
						if (!removed)
							failed = true;
					}
				if (directory_descriptor >= 0 && ::close(directory_descriptor) != 0)
					failed = true;
				struct stat path_metadata{};
				if (cleanup_custody_ && ::lstat(directory_path_.c_str(), &path_metadata) == 0)
				{
					if (path_metadata.st_dev != directory_device_ ||
						path_metadata.st_ino != directory_inode_ ||
						::rmdir(directory_path_.c_str()) != 0)
						failed = true;
				}
				else if (cleanup_custody_ && errno != ENOENT)
					failed = true;
				if (failed)
				{
					poisoned_ = true;
					return unexpected(port_error("cleanup", "effect-unknown"));
				}
				return {};
			}

		  private:
			// NOLINTBEGIN(bugprone-easily-swappable-parameters): persisted identity tuple order.
			linux_ng1_spill_storage_port(const int descriptor,
										 const int directory_descriptor,
										 std::string directory_path,
										 std::string data_path,
										 std::string commit_path,
										 std::string frontier_path,
										 const dev_t directory_device,
										 const ino_t directory_inode,
										 const dev_t data_device,
										 const ino_t data_inode,
										 const std::uint64_t byte_count,
										 const std::uint64_t committed_byte_count,
										 const std::uint64_t fsync_sequence,
										 std::optional<ng1_spill_resume_frontier> frontier,
										 const bool cleanup_custody) noexcept
				: descriptor_{descriptor}, directory_descriptor_{directory_descriptor},
				  directory_path_{std::move(directory_path)}, data_path_{std::move(data_path)},
				  commit_path_{std::move(commit_path)}, frontier_path_{std::move(frontier_path)},
				  directory_device_{directory_device}, directory_inode_{directory_inode},
				  data_device_{data_device}, data_inode_{data_inode}, byte_count_{byte_count},
				  committed_byte_count_{committed_byte_count}, fsync_sequence_{fsync_sequence},
				  frontier_{std::move(frontier)}, cleanup_custody_{cleanup_custody}
			{
				// NOLINTEND(bugprone-easily-swappable-parameters)
			}

			int descriptor_{-1};
			int directory_descriptor_{-1};
			std::string directory_path_;
			std::string data_path_;
			std::string commit_path_;
			std::string frontier_path_;
			dev_t directory_device_{};
			ino_t directory_inode_{};
			dev_t data_device_{};
			ino_t data_inode_{};
			std::uint64_t byte_count_{};
			std::uint64_t committed_byte_count_{};
			std::uint64_t fsync_sequence_{};
			std::optional<ng1_spill_resume_frontier> frontier_;
			bool poisoned_{};
			bool cleaned_{};
			bool cleanup_custody_{};
		};
#endif
	} // namespace

	result<std::unique_ptr<ng1_spill_storage_port>> ng1_spill_storage_port::reopen() const
	{
		return unexpected(error{"provider.recovery-failed", "reopen", "unsupported"});
	}

	result<void> ng1_spill_storage_port::transfer_cleanup_custody_to(ng1_spill_storage_port&)
	{
		return unexpected(
			error{"provider.recovery-failed", "cleanup", "custody-transfer-unsupported"});
	}

	result<std::unique_ptr<ng1_spill_storage_port>> make_system_ng1_spill_storage_port()
	{
#if defined(__linux__) && defined(__GLIBC__)
		auto storage = linux_ng1_spill_storage_port::create_new();
		if (!storage)
			return unexpected(std::move(storage.error()));
		return std::unique_ptr<ng1_spill_storage_port>{std::move(*storage)};
#else
		return unexpected(port_error("platform", "linux-glibc-filesystem-required"));
#endif
	}

	ng1_spill_staging_session::ng1_spill_staging_session(
		ng1_spill_prefix_state prefix,
		ng1_spill_binding binding,
		std::unique_ptr<ng1_spill_storage_port> storage) noexcept
		: prefix_{std::move(prefix)}, binding_{std::move(binding)}, storage_{std::move(storage)}
	{
	}

	ng1_spill_staging_session::~ng1_spill_staging_session() noexcept
	{
		if (storage_ && !cleaned_)
			std::terminate();
	}

	ng1_spill_staging_session::ng1_spill_staging_session(ng1_spill_staging_session&& other) noexcept
		: prefix_{std::move(other.prefix_)}, binding_{std::move(other.binding_)},
		  storage_{std::move(other.storage_)}, last_fsync_sequence_{other.last_fsync_sequence_},
		  last_resume_generation_{other.last_resume_generation_},
		  has_fsync_sequence_{other.has_fsync_sequence_},
		  has_resume_generation_{other.has_resume_generation_}, poisoned_{other.poisoned_},
		  cleaned_{other.cleaned_}
	{
		other.cleaned_ = true;
		other.poisoned_ = true;
	}

	ng1_spill_staging_session&
	ng1_spill_staging_session::operator=(ng1_spill_staging_session&& other) noexcept
	{
		if (this == &other)
			return *this;
		if (storage_ && !cleaned_)
			std::terminate();
		prefix_ = std::move(other.prefix_);
		binding_ = std::move(other.binding_);
		storage_ = std::move(other.storage_);
		last_fsync_sequence_ = other.last_fsync_sequence_;
		last_resume_generation_ = other.last_resume_generation_;
		has_fsync_sequence_ = other.has_fsync_sequence_;
		has_resume_generation_ = other.has_resume_generation_;
		poisoned_ = other.poisoned_;
		cleaned_ = other.cleaned_;
		other.cleaned_ = true;
		other.poisoned_ = true;
		return *this;
	}

	result<ng1_spill_staging_session>
	ng1_spill_staging_session::create(ng1_spill_binding binding,
									  std::unique_ptr<ng1_spill_storage_port> storage)
	{
		if (!storage)
			return unexpected(port_error("storage", "missing-port"));
		auto prefix = ng1_spill_prefix_state::create(binding);
		if (!prefix)
			return unexpected(std::move(prefix.error()));
		return ng1_spill_staging_session{
			std::move(*prefix), std::move(binding), std::move(storage)};
	}

	result<void> ng1_spill_staging_session::append(const ng1_spill_record& record)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("append", "terminal-session"));
		if (auto bounded = validate_spill_record_wire_quota(record); !bounded)
			return unexpected(std::move(bounded.error()));
		try
		{
			auto candidate = prefix_;
			if (auto valid = candidate.append(record); !valid)
				return unexpected(std::move(valid.error()));
			auto wire = encode_spill_record(record);
			if (!wire)
				return unexpected(std::move(wire.error()));
			result<void> stored;
			try
			{
				stored = storage_->append(*wire);
			}
			catch (...)
			{
				poisoned_ = true;
				return unexpected(port_error("append", "effect-unknown"));
			}
			if (!stored)
			{
				poisoned_ = true;
				return unexpected(std::move(stored.error()));
			}
			prefix_ = std::move(candidate);
			return {};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(port_error("append", "allocation"));
		}
	}

	result<ng1_spill_fsync_receipt>
	ng1_spill_staging_session::fsync(const std::uint64_t highest_contiguous_acked_sequence,
									 const std::uint64_t highest_observed_sequence,
									 std::string staged_digest,
									 const std::uint64_t resume_generation)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("fsync", "terminal-session"));
		if (resume_generation == 0U ||
			(has_resume_generation_ && resume_generation <= last_resume_generation_))
			return unexpected(port_error("resume_generation", "not-increasing"));
		if (highest_contiguous_acked_sequence > highest_observed_sequence)
			return unexpected(
				corrupt_error("highest_contiguous_acked_sequence", "ahead-of-observed"));
		if (auto valid = prefix_.validate_ack_frontier(highest_contiguous_acked_sequence); !valid)
			return unexpected(std::move(valid.error()));
		if (auto valid = valid_semantic_digest(staged_digest, "staged_digest"); !valid)
			return unexpected(std::move(valid.error()));
		result<std::uint64_t> sequence{port_error("fsync", "not-called")};
		try
		{
			sequence = storage_->fsync();
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("fsync", "effect-unknown"));
		}
		if (!sequence)
		{
			poisoned_ = true;
			return unexpected(std::move(sequence.error()));
		}
		if (*sequence == 0U || (has_fsync_sequence_ && *sequence <= last_fsync_sequence_))
		{
			poisoned_ = true;
			return unexpected(port_error("fsync_sequence", "not-increasing"));
		}
		result<ng1_spill_fsync_receipt> receipt{port_error("fsync", "not-observed")};
		try
		{
			receipt = prefix_.observe_host_fsync(
				highest_contiguous_acked_sequence, std::move(staged_digest), *sequence);
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("fsync", "effect-unknown"));
		}
		if (!receipt)
		{
			poisoned_ = true;
			return unexpected(std::move(receipt.error()));
		}
		ng1_spill_resume_frontier frontier{*receipt, resume_generation};
		if (auto valid = frontier.validate(); !valid)
		{
			poisoned_ = true;
			return unexpected(std::move(valid.error()));
		}
		result<void> persisted{port_error("resume_frontier", "not-persisted")};
		try
		{
			persisted = storage_->persist_resume_frontier(frontier);
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("resume_frontier", "effect-unknown"));
		}
		if (!persisted)
		{
			poisoned_ = true;
			return unexpected(std::move(persisted.error()));
		}
		last_fsync_sequence_ = *sequence;
		has_fsync_sequence_ = true;
		last_resume_generation_ = resume_generation;
		has_resume_generation_ = true;
		return receipt;
	}

	result<void>
	ng1_spill_staging_session::validate_persisted_frontier(const ng1_spill_fsync_receipt& receipt,
														   const std::uint64_t resume_generation)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("resume_frontier", "terminal-session"));
		ng1_spill_resume_frontier expected{receipt, resume_generation};
		if (auto valid = expected.validate(); !valid)
			return unexpected(std::move(valid.error()));
		result<std::optional<ng1_spill_resume_frontier>> persisted{
			port_error("resume_frontier", "not-read")};
		try
		{
			persisted = storage_->read_resume_frontier();
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("resume_frontier", "effect-unknown"));
		}
		if (!persisted)
		{
			poisoned_ = true;
			return unexpected(std::move(persisted.error()));
		}
		if (!*persisted)
		{
			poisoned_ = true;
			return unexpected(error{"provider.resume-token-stale", "resume_frontier", "missing"});
		}
		if (auto valid = (*persisted)->validate(); !valid)
		{
			poisoned_ = true;
			return unexpected(std::move(valid.error()));
		}
		if (**persisted != expected)
		{
			poisoned_ = true;
			return unexpected(
				error{"provider.resume-token-stale", "resume_frontier", "not-latest"});
		}
		return {};
	}

	result<ng1_spill_prefix_state> ng1_spill_staging_session::recover()
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("recovery", "terminal-session"));
		result<std::vector<std::byte>> raw{port_error("recovery", "not-read")};
		try
		{
			raw = storage_->read_all();
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("recovery", "effect-unknown"));
		}
		if (!raw)
		{
			poisoned_ = true;
			return unexpected(std::move(raw.error()));
		}
		try
		{
			auto recovered = ng1_spill_prefix_state::create(binding_);
			if (!recovered)
			{
				poisoned_ = true;
				return unexpected(std::move(recovered.error()));
			}
			std::size_t offset{};
			while (offset < raw->size())
			{
				auto record = decode_framed_record(*raw, offset);
				if (!record)
				{
					poisoned_ = true;
					return unexpected(std::move(record.error()));
				}
				if (auto admitted = recovered->append(*record); !admitted)
				{
					poisoned_ = true;
					return unexpected(std::move(admitted.error()));
				}
			}
			if (recovered->total_bytes() != raw->size())
			{
				poisoned_ = true;
				return unexpected(corrupt_error("total_bytes", "framing-mismatch"));
			}
			return recovered;
		}
		catch (...)
		{
			poisoned_ = true;
			return unexpected(port_error("recovery", "effect-unknown"));
		}
	}

	result<void>
	ng1_spill_staging_session::restore_from_fsync_receipt(const ng1_spill_fsync_receipt& receipt,
														  const std::uint64_t resume_generation)
	{
		if (!storage_ || cleaned_ || poisoned_)
			return unexpected(port_error("restore", "terminal-session"));
		if (prefix_.total_bytes() != 0U || prefix_.total_records() != 0U || has_fsync_sequence_)
		{
			poisoned_ = true;
			return unexpected(port_error("restore", "non-fresh-session"));
		}
		if (auto valid = receipt.validate(); !valid)
		{
			poisoned_ = true;
			return unexpected(std::move(valid.error()));
		}
		if (receipt.provider_id != binding_.provider_id ||
			receipt.protocol_session_id != binding_.protocol_session_id ||
			receipt.task_id != binding_.task_id || receipt.stream_id != binding_.stream_id)
		{
			poisoned_ = true;
			return unexpected(corrupt_error("receipt", "binding-mismatch"));
		}

		auto recovered = recover();
		if (!recovered)
			return unexpected(std::move(recovered.error()));
		auto expected = recovered->observe_host_fsync(receipt.highest_contiguous_acked_sequence,
													  receipt.staged_digest,
													  receipt.fsync_sequence);
		if (!expected)
		{
			poisoned_ = true;
			return unexpected(std::move(expected.error()));
		}
		if (*expected != receipt)
		{
			poisoned_ = true;
			return unexpected(corrupt_error("receipt", "prefix-mismatch"));
		}
		if (auto valid = validate_persisted_frontier(receipt, resume_generation); !valid)
			return valid;

		prefix_ = std::move(*recovered);
		last_fsync_sequence_ = receipt.fsync_sequence;
		has_fsync_sequence_ = true;
		last_resume_generation_ = resume_generation;
		has_resume_generation_ = true;
		return {};
	}

	result<void>
	ng1_spill_staging_session::handoff_cleanup_custody_to(ng1_spill_staging_session& replacement)
	{
		if (this == &replacement)
			return unexpected(port_error("cleanup", "custody-self-transfer"));
		if (!storage_ || !replacement.storage_ || cleaned_ || replacement.cleaned_ || poisoned_ ||
			replacement.poisoned_)
			return unexpected(port_error("cleanup", "custody-session-terminal"));
		if (binding_ != replacement.binding_)
			return unexpected(corrupt_error("cleanup", "binding-mismatch"));
		if (prefix_.total_bytes() != replacement.prefix_.total_bytes() ||
			prefix_.total_records() != replacement.prefix_.total_records() ||
			has_fsync_sequence_ != replacement.has_fsync_sequence_ ||
			(has_fsync_sequence_ && last_fsync_sequence_ != replacement.last_fsync_sequence_) ||
			has_resume_generation_ != replacement.has_resume_generation_ ||
			(has_resume_generation_ &&
			 last_resume_generation_ != replacement.last_resume_generation_))
			return unexpected(corrupt_error("cleanup", "prefix-frontier-mismatch"));
		result<std::string> source_digest{port_error("cleanup", "prefix-digest")};
		result<std::string> replacement_digest{port_error("cleanup", "prefix-digest")};
		try
		{
			source_digest = prefix_.spill_digest();
			replacement_digest = replacement.prefix_.spill_digest();
		}
		catch (...)
		{
			return unexpected(port_error("cleanup", "prefix-digest"));
		}
		if (!source_digest || !replacement_digest)
			return unexpected(corrupt_error("cleanup", "prefix-digest"));
		if (*source_digest != *replacement_digest)
			return unexpected(corrupt_error("cleanup", "prefix-mismatch"));

		result<void> transferred{port_error("cleanup", "custody-transfer")};
		try
		{
			transferred = storage_->transfer_cleanup_custody_to(*replacement.storage_);
		}
		catch (...)
		{
			return unexpected(port_error("cleanup", "custody-transfer-effect-unknown"));
		}
		if (!transferred)
			return unexpected(std::move(transferred.error()));

		// The replacement is now the sole unlink/rmdir custodian. Retire the old
		// descriptor-only session before a coordinator move-installs the replacement.
		result<void> retired{port_error("cleanup", "custody-retire")};
		try
		{
			retired = storage_->cleanup();
		}
		catch (...)
		{
			storage_.reset();
			cleaned_ = true;
			poisoned_ = true;
			return unexpected(port_error("cleanup", "custody-retire-effect-unknown"));
		}
		storage_.reset();
		cleaned_ = true;
		if (!retired)
		{
			poisoned_ = true;
			return unexpected(std::move(retired.error()));
		}
		return {};
	}

	result<void> ng1_spill_staging_session::cleanup()
	{
		if (!storage_ || cleaned_)
			return unexpected(port_error("cleanup", "already-terminal"));
		cleaned_ = true;
		result<void> result;
		try
		{
			result = storage_->cleanup();
		}
		catch (...)
		{
			poisoned_ = true;
			storage_.reset();
			return unexpected(port_error("cleanup", "effect-unknown"));
		}
		storage_.reset();
		if (!result)
		{
			poisoned_ = true;
			return unexpected(std::move(result.error()));
		}
		return {};
	}
} // namespace cxxlens::sdk::provider::detail
