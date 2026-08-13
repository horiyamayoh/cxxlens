#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include "provider_ng1_transport_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		constexpr std::string_view heartbeat_schema{"cxxlens.provider-control.heartbeat.v1"};
		constexpr std::string_view progress_schema{"cxxlens.provider-control.progress.v2"};
		constexpr std::string_view resume_schema{"cxxlens.provider-control.resume.v2"};
		constexpr std::string_view semantic_digest_prefix{"semantic-v2:sha256:"};

		[[nodiscard]] error transport_error(std::string field, std::string detail)
		{
			return {"provider.protocol-state-invalid", std::move(field), std::move(detail)};
		}

		enum class cbor_major : std::uint8_t
		{
			unsigned_integer = 0U,
			text = 3U,
			map = 5U,
		};

		using cbor_scalar = std::variant<std::uint64_t, std::string>;

		struct cbor_field
		{
			std::string key;
			cbor_scalar value;
		};

		struct encoded_cbor_field
		{
			std::vector<std::byte> key;
			std::vector<std::byte> value;
		};

		using decoded_cbor_map = std::map<std::string, cbor_scalar, std::less<>>;

		void append_big_endian(std::vector<std::byte>& output,
							   const std::uint64_t value,
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

		[[nodiscard]] std::vector<std::byte> cbor_text(const std::string_view value)
		{
			std::vector<std::byte> output;
			output.reserve(1U + value.size());
			append_cbor_head(output, cbor_major::text, value.size());
			for (const auto byte : value)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		}

		[[nodiscard]] cbor_field text_value(std::string key, const std::string_view value)
		{
			return {std::move(key), std::string{value}};
		}

		[[nodiscard]] cbor_field uint_value(std::string key, const std::uint64_t value)
		{
			return {std::move(key), value};
		}

		[[nodiscard]] result<std::vector<std::byte>>
		encode_cbor_map(const std::vector<cbor_field>& fields)
		{
			std::set<std::string, std::less<>> keys;
			std::vector<encoded_cbor_field> encoded;
			encoded.reserve(fields.size());
			for (const auto& field : fields)
			{
				if (!keys.insert(field.key).second)
					return unexpected(transport_error("cbor", "duplicate-key"));
				if (auto valid = validate_utf8_text(field.key); !valid)
					return unexpected(transport_error("cbor-key", "invalid-utf8"));

				encoded_cbor_field item{cbor_text(field.key), {}};
				std::visit(
					[&item](const auto& value)
					{
						using value_type = std::remove_cvref_t<decltype(value)>;
						if constexpr (std::is_same_v<value_type, std::string>)
						{
							append_cbor_head(item.value, cbor_major::text, value.size());
							for (const auto byte : value)
								item.value.push_back(
									static_cast<std::byte>(static_cast<unsigned char>(byte)));
						}
						else
							append_cbor_head(item.value, cbor_major::unsigned_integer, value);
					},
					field.value);
				if (const auto* text = std::get_if<std::string>(&field.value); text != nullptr)
					if (auto valid = validate_utf8_text(*text); !valid)
						return unexpected(transport_error(field.key, "invalid-utf8"));
				encoded.push_back(std::move(item));
			}

			std::ranges::sort(encoded,
							  [](const encoded_cbor_field& left, const encoded_cbor_field& right)
							  {
								  if (left.key.size() != right.key.size())
									  return left.key.size() < right.key.size();
								  return std::lexicographical_compare(left.key.begin(),
																	  left.key.end(),
																	  right.key.begin(),
																	  right.key.end());
							  });

			std::vector<std::byte> output;
			output.reserve(1U + fields.size() * 8U);
			append_cbor_head(output, cbor_major::map, encoded.size());
			for (const auto& item : encoded)
			{
				output.insert(output.end(), item.key.begin(), item.key.end());
				output.insert(output.end(), item.value.begin(), item.value.end());
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
				return unexpected(transport_error("cbor", "truncated-or-indefinite"));

			std::uint64_t value{};
			for (std::size_t index{}; index < width; ++index)
				value = (value << 8U) | std::to_integer<std::uint64_t>(input[offset + index]);
			const auto minimum = width == 1U ? 24U : (std::uint64_t{1U} << (width * 4U));
			if (value < minimum)
				return unexpected(transport_error("cbor", "non-shortest"));
			return std::pair{value, offset + width};
		}

		[[nodiscard]] result<std::pair<cbor_scalar, std::size_t>>
		decode_cbor_scalar(const std::span<const std::byte> input, const std::size_t offset)
		{
			if (offset >= input.size())
				return unexpected(transport_error("cbor", "truncated"));
			const auto initial = std::to_integer<std::uint8_t>(input[offset]);
			const auto major = initial >> 5U;
			if (major != static_cast<std::uint8_t>(cbor_major::unsigned_integer) &&
				major != static_cast<std::uint8_t>(cbor_major::text))
				return unexpected(transport_error("cbor", "scalar-type"));
			auto argument = decode_cbor_argument(input, offset + 1U, initial & 0x1fU);
			if (!argument)
				return unexpected(std::move(argument.error()));
			if (major == static_cast<std::uint8_t>(cbor_major::unsigned_integer))
				return std::pair{cbor_scalar{argument->first}, argument->second};
			if (argument->first > input.size() - argument->second)
				return unexpected(transport_error("cbor", "text-length"));
			std::string text{reinterpret_cast<const char*>(input.data() + argument->second),
							 static_cast<std::size_t>(argument->first)};
			if (auto valid = validate_utf8_text(text); !valid)
				return unexpected(transport_error("cbor", "invalid-utf8"));
			return std::pair{cbor_scalar{std::move(text)},
							 argument->second + static_cast<std::size_t>(argument->first)};
		}

		[[nodiscard]] result<decoded_cbor_map>
		decode_cbor_map(const std::span<const std::byte> input,
						const std::size_t expected_field_count)
		{
			if (input.empty())
				return unexpected(transport_error("cbor", "empty"));
			const auto initial = std::to_integer<std::uint8_t>(input.front());
			if ((initial >> 5U) != static_cast<std::uint8_t>(cbor_major::map))
				return unexpected(transport_error("cbor", "map-type"));
			auto count = decode_cbor_argument(input, 1U, initial & 0x1fU);
			if (!count)
				return unexpected(std::move(count.error()));
			if (count->first != expected_field_count)
				return unexpected(transport_error("cbor", "field-count"));
			if (count->second > input.size() || count->first > input.size() - count->second)
				return unexpected(transport_error("cbor", "field-count-overflow"));

			decoded_cbor_map output;
			std::vector<std::byte> previous_key;
			std::size_t offset = count->second;
			for (std::uint64_t index{}; index < count->first; ++index)
			{
				const auto key_begin = offset;
				auto key = decode_cbor_scalar(input, offset);
				if (!key || !std::holds_alternative<std::string>(key->first))
					return unexpected(transport_error("cbor", "map-key-type"));
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
						return unexpected(transport_error("cbor", "map-order-or-duplicate"));
				}
				previous_key = std::move(encoded_key);

				auto value = decode_cbor_scalar(input, offset);
				if (!value)
					return unexpected(std::move(value.error()));
				offset = value->second;
				const auto& key_text = std::get<std::string>(key->first);
				if (!output.emplace(key_text, std::move(value->first)).second)
					return unexpected(transport_error(key_text, "duplicate-key"));
			}
			if (offset != input.size())
				return unexpected(transport_error("cbor", "trailing"));
			return output;
		}

		template <typename T>
		[[nodiscard]] result<T> map_field(const decoded_cbor_map& fields,
										  const std::string_view name)
		{
			const auto found = fields.find(name);
			if (found == fields.end())
				return unexpected(transport_error(std::string{name}, "missing-field"));
			const auto* value = std::get_if<T>(&found->second);
			if (value == nullptr)
				return unexpected(transport_error(std::string{name}, "field-type"));
			return *value;
		}

		template <typename T>
		[[nodiscard]] result<void> require_value(const result<T>& value)
		{
			if (!value)
				return unexpected(value.error());
			return {};
		}

		[[nodiscard]] result<std::uint32_t>
		parse_semantic_version_component(const std::string_view text, const std::string_view field)
		{
			if (text.empty() || (text.size() > 1U && text.front() == '0'))
				return unexpected(transport_error(std::string{field}, "semantic-version"));
			std::uint64_t value{};
			for (const auto byte : text)
			{
				if (byte < '0' || byte > '9')
					return unexpected(transport_error(std::string{field}, "semantic-version"));
				const auto digit = static_cast<std::uint64_t>(byte - '0');
				if (value > (std::numeric_limits<std::uint32_t>::max() - digit) / 10U)
					return unexpected(
						transport_error(std::string{field}, "semantic-version-overflow"));
				value = value * 10U + digit;
			}
			return static_cast<std::uint32_t>(value);
		}

		[[nodiscard]] result<semantic_version> parse_semantic_version(const std::string_view text)
		{
			std::array<std::uint32_t, 3U> components{};
			std::size_t begin{};
			for (std::size_t index{}; index < components.size(); ++index)
			{
				const auto end =
					index + 1U == components.size() ? text.size() : text.find('.', begin);
				if (end == std::string_view::npos || end < begin)
					return unexpected(transport_error("provider_version", "semantic-version"));
				auto component = parse_semantic_version_component(text.substr(begin, end - begin),
																  "provider_version");
				if (!component)
					return unexpected(std::move(component.error()));
				components[index] = *component;
				begin = end + 1U;
			}
			if (text.find('.', begin) != std::string_view::npos)
				return unexpected(transport_error("provider_version", "semantic-version"));
			return semantic_version{components[0U], components[1U], components[2U]};
		}

		[[nodiscard]] result<ng1_heartbeat_kind> parse_heartbeat_kind(const std::string_view value)
		{
			if (value == "probe")
				return ng1_heartbeat_kind::probe;
			if (value == "ack")
				return ng1_heartbeat_kind::ack;
			return unexpected(transport_error("kind", "heartbeat-enum"));
		}

		[[nodiscard]] std::string_view heartbeat_kind_text(const ng1_heartbeat_kind value)
		{
			switch (value)
			{
				case ng1_heartbeat_kind::probe:
					return "probe";
				case ng1_heartbeat_kind::ack:
					return "ack";
			}
			return {};
		}

		[[nodiscard]] result<ng1_resume_kind> parse_resume_kind(const std::string_view value)
		{
			if (value == "request")
				return ng1_resume_kind::request;
			if (value == "accepted")
				return ng1_resume_kind::accepted;
			if (value == "rejected")
				return ng1_resume_kind::rejected;
			return unexpected(transport_error("kind", "resume-enum"));
		}

		[[nodiscard]] std::string_view resume_kind_text(const ng1_resume_kind value)
		{
			switch (value)
			{
				case ng1_resume_kind::request:
					return "request";
				case ng1_resume_kind::accepted:
					return "accepted";
				case ng1_resume_kind::rejected:
					return "rejected";
			}
			return {};
		}

		[[nodiscard]] bool valid_semantic_digest(const std::string_view value) noexcept
		{
			if (!value.starts_with(semantic_digest_prefix) ||
				value.size() != semantic_digest_prefix.size() + 64U)
				return false;
			for (const auto byte : value.substr(semantic_digest_prefix.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] result<void> validate_digest(const std::string_view value,
												   const std::string_view field)
		{
			if (!valid_semantic_digest(value))
				return unexpected(transport_error(std::string{field}, "semantic-digest"));
			return {};
		}

		[[nodiscard]] result<void> validate_id(const std::string_view value,
											   const std::string_view field)
		{
			if (auto valid = validate_strong_id(value); !valid)
				return unexpected(transport_error(std::string{field}, "typed-id"));
			return {};
		}

		[[nodiscard]] result<void> validate_heartbeat_control(const ng1_heartbeat_control& value)
		{
			if (value.schema != heartbeat_schema)
				return unexpected(transport_error("schema", "heartbeat-schema"));
			if (heartbeat_kind_text(value.kind).empty())
				return unexpected(transport_error("kind", "heartbeat-enum"));
			const ng1_session_binding binding{value.provider_id,
											  value.provider_version,
											  value.protocol_session_id,
											  value.task_id,
											  value.stream_id};
			if (auto valid = binding.validate(); !valid)
				return unexpected(std::move(valid.error()));
			return validate_digest(value.staged_digest, "staged_digest");
		}

		[[nodiscard]] result<void> validate_progress_control(const ng1_progress_control& value)
		{
			if (value.schema != progress_schema)
				return unexpected(transport_error("schema", "progress-schema"));
			if (auto valid = validate_id(value.task_id, "task_id"); !valid)
				return valid;
			if (auto valid = validate_id(value.dependency_group_id, "dependency_group_id"); !valid)
				return valid;
			if (value.total_units == 0U || value.completed_units > value.total_units)
				return unexpected(transport_error("completed_units", "progress-range"));
			return {};
		}
	} // namespace

	result<ng1_heartbeat_sample>
	ng1_heartbeat_control::to_validation_sample(const std::uint64_t host_receipt_time_ns) const
	{
		if (auto valid = validate_heartbeat_control(*this); !valid)
			return unexpected(std::move(valid.error()));
		return ng1_heartbeat_sample{
			schema,
			ng1_session_binding{
				provider_id, provider_version, protocol_session_id, task_id, stream_id},
			kind,
			heartbeat_sequence,
			monotonic_time_ns,
			host_receipt_time_ns,
			highest_contiguous_acked_sequence,
			staged_digest};
	}

	result<ng1_progress_sample>
	ng1_progress_control::to_validation_sample(const std::uint64_t host_receipt_time_ns) const
	{
		if (auto valid = validate_progress_control(*this); !valid)
			return unexpected(std::move(valid.error()));
		return ng1_progress_sample{schema,
								   task_id,
								   dependency_group_id,
								   progress_sequence,
								   monotonic_time_ns,
								   host_receipt_time_ns,
								   completed_units,
								   total_units};
	}

	result<ng1_resume_token> ng1_resume_control::to_validation_token() const
	{
		ng1_resume_token token{schema,
							   kind,
							   binding,
							   highest_contiguous_acked_sequence,
							   staged_digest,
							   token_generation,
							   token_digest};
		auto expected = ng1_resume_token_digest(token);
		if (!expected)
			return unexpected(std::move(expected.error()));
		if (token.token_digest != *expected)
			return unexpected(
				{"provider.resume-replay-invalid", "token_digest", "projection-mismatch"});
		return token;
	}

	result<std::vector<std::byte>> encode_ng1_heartbeat_control(const ng1_heartbeat_control& value)
	{
		if (auto valid = validate_heartbeat_control(value); !valid)
			return unexpected(std::move(valid.error()));
		return encode_cbor_map({
			text_value("schema", value.schema),
			text_value("kind", heartbeat_kind_text(value.kind)),
			text_value("provider_id", value.provider_id),
			text_value("provider_version", value.provider_version.string()),
			text_value("protocol_session_id", value.protocol_session_id),
			text_value("task_id", value.task_id),
			uint_value("stream_id", value.stream_id),
			uint_value("heartbeat_sequence", value.heartbeat_sequence),
			uint_value("monotonic_time_ns", value.monotonic_time_ns),
			uint_value("highest_contiguous_acked_sequence",
					   value.highest_contiguous_acked_sequence),
			text_value("staged_digest", value.staged_digest),
		});
	}

	result<ng1_heartbeat_control>
	decode_ng1_heartbeat_control(const std::span<const std::byte> control)
	{
		auto fields = decode_cbor_map(control, 11U);
		if (!fields)
			return unexpected(std::move(fields.error()));
		auto schema = map_field<std::string>(*fields, "schema");
		auto kind_text = map_field<std::string>(*fields, "kind");
		auto provider_id = map_field<std::string>(*fields, "provider_id");
		auto provider_version_text = map_field<std::string>(*fields, "provider_version");
		auto session_id = map_field<std::string>(*fields, "protocol_session_id");
		auto task_id = map_field<std::string>(*fields, "task_id");
		auto stream_id = map_field<std::uint64_t>(*fields, "stream_id");
		auto heartbeat_sequence = map_field<std::uint64_t>(*fields, "heartbeat_sequence");
		auto monotonic_time = map_field<std::uint64_t>(*fields, "monotonic_time_ns");
		auto highest_acked = map_field<std::uint64_t>(*fields, "highest_contiguous_acked_sequence");
		auto staged_digest = map_field<std::string>(*fields, "staged_digest");
		for (const auto& value : {require_value(schema),
								  require_value(kind_text),
								  require_value(provider_id),
								  require_value(provider_version_text),
								  require_value(session_id),
								  require_value(task_id),
								  require_value(stream_id),
								  require_value(heartbeat_sequence),
								  require_value(monotonic_time),
								  require_value(highest_acked),
								  require_value(staged_digest)})
			if (!value)
				return unexpected(value.error());

		auto kind = parse_heartbeat_kind(*kind_text);
		if (!kind)
			return unexpected(std::move(kind.error()));
		auto provider_version = parse_semantic_version(*provider_version_text);
		if (!provider_version)
			return unexpected(std::move(provider_version.error()));
		ng1_heartbeat_control output;
		output.schema = std::move(*schema);
		output.kind = *kind;
		output.provider_id = std::move(*provider_id);
		output.provider_version = *provider_version;
		output.protocol_session_id = std::move(*session_id);
		output.task_id = std::move(*task_id);
		output.stream_id = *stream_id;
		output.heartbeat_sequence = *heartbeat_sequence;
		output.monotonic_time_ns = *monotonic_time;
		output.highest_contiguous_acked_sequence = *highest_acked;
		output.staged_digest = std::move(*staged_digest);
		if (auto valid = validate_heartbeat_control(output); !valid)
			return unexpected(std::move(valid.error()));
		return output;
	}

	result<std::vector<std::byte>> encode_ng1_progress_control(const ng1_progress_control& value)
	{
		if (auto valid = validate_progress_control(value); !valid)
			return unexpected(std::move(valid.error()));
		return encode_cbor_map({
			text_value("schema", value.schema),
			text_value("task_id", value.task_id),
			text_value("dependency_group_id", value.dependency_group_id),
			uint_value("progress_sequence", value.progress_sequence),
			uint_value("monotonic_time_ns", value.monotonic_time_ns),
			uint_value("completed_units", value.completed_units),
			uint_value("total_units", value.total_units),
		});
	}

	result<ng1_progress_control>
	decode_ng1_progress_control(const std::span<const std::byte> control)
	{
		auto fields = decode_cbor_map(control, 7U);
		if (!fields)
			return unexpected(std::move(fields.error()));
		auto schema = map_field<std::string>(*fields, "schema");
		auto task_id = map_field<std::string>(*fields, "task_id");
		auto dependency_group_id = map_field<std::string>(*fields, "dependency_group_id");
		auto progress_sequence = map_field<std::uint64_t>(*fields, "progress_sequence");
		auto monotonic_time = map_field<std::uint64_t>(*fields, "monotonic_time_ns");
		auto completed_units = map_field<std::uint64_t>(*fields, "completed_units");
		auto total_units = map_field<std::uint64_t>(*fields, "total_units");
		for (const auto& value : {require_value(schema),
								  require_value(task_id),
								  require_value(dependency_group_id),
								  require_value(progress_sequence),
								  require_value(monotonic_time),
								  require_value(completed_units),
								  require_value(total_units)})
			if (!value)
				return unexpected(value.error());

		ng1_progress_control output;
		output.schema = std::move(*schema);
		output.task_id = std::move(*task_id);
		output.dependency_group_id = std::move(*dependency_group_id);
		output.progress_sequence = *progress_sequence;
		output.monotonic_time_ns = *monotonic_time;
		output.completed_units = *completed_units;
		output.total_units = *total_units;
		if (auto valid = validate_progress_control(output); !valid)
			return unexpected(std::move(valid.error()));
		return output;
	}

	result<std::vector<std::byte>> encode_ng1_resume_control(const ng1_resume_control& value)
	{
		auto token = value.to_validation_token();
		if (!token)
			return unexpected(std::move(token.error()));
		return encode_cbor_map({
			text_value("schema", value.schema),
			text_value("kind", resume_kind_text(value.kind)),
			text_value("provider_id", value.binding.provider_id),
			text_value("provider_version", value.binding.provider_version.string()),
			text_value("provider_binary_digest", value.binding.provider_binary_digest),
			text_value("provider_semantic_contract_digest",
					   value.binding.provider_semantic_contract_digest),
			text_value("protocol_session_id", value.binding.protocol_session_id),
			text_value("task_id", value.binding.task_id),
			text_value("task_input_digest", value.binding.task_input_digest),
			text_value("normalized_invocation_digest", value.binding.normalized_invocation_digest),
			text_value("toolchain_digest", value.binding.toolchain_digest),
			text_value("environment_digest", value.binding.environment_digest),
			text_value("sandbox_policy_digest", value.binding.sandbox_policy_digest),
			text_value("dependency_group_id", value.binding.dependency_group_id),
			text_value("atomic_output_group_id", value.binding.atomic_output_group_id),
			text_value("batch_id", value.binding.batch_id),
			uint_value("stream_id", value.binding.stream_id),
			uint_value("highest_contiguous_acked_sequence",
					   value.highest_contiguous_acked_sequence),
			text_value("staged_digest", value.staged_digest),
			uint_value("token_generation", value.token_generation),
			text_value("token_digest", value.token_digest),
		});
	}

	result<ng1_resume_control> decode_ng1_resume_control(const std::span<const std::byte> control)
	{
		auto fields = decode_cbor_map(control, 21U);
		if (!fields)
			return unexpected(std::move(fields.error()));
		auto schema = map_field<std::string>(*fields, "schema");
		auto kind_text = map_field<std::string>(*fields, "kind");
		auto provider_id = map_field<std::string>(*fields, "provider_id");
		auto provider_version_text = map_field<std::string>(*fields, "provider_version");
		auto provider_binary_digest = map_field<std::string>(*fields, "provider_binary_digest");
		auto provider_contract_digest =
			map_field<std::string>(*fields, "provider_semantic_contract_digest");
		auto session_id = map_field<std::string>(*fields, "protocol_session_id");
		auto task_id = map_field<std::string>(*fields, "task_id");
		auto input_digest = map_field<std::string>(*fields, "task_input_digest");
		auto invocation_digest = map_field<std::string>(*fields, "normalized_invocation_digest");
		auto toolchain_digest = map_field<std::string>(*fields, "toolchain_digest");
		auto environment_digest = map_field<std::string>(*fields, "environment_digest");
		auto sandbox_digest = map_field<std::string>(*fields, "sandbox_policy_digest");
		auto dependency_group_id = map_field<std::string>(*fields, "dependency_group_id");
		auto atomic_group_id = map_field<std::string>(*fields, "atomic_output_group_id");
		auto batch_id = map_field<std::string>(*fields, "batch_id");
		auto stream_id = map_field<std::uint64_t>(*fields, "stream_id");
		auto highest_acked = map_field<std::uint64_t>(*fields, "highest_contiguous_acked_sequence");
		auto staged_digest = map_field<std::string>(*fields, "staged_digest");
		auto token_generation = map_field<std::uint64_t>(*fields, "token_generation");
		auto token_digest = map_field<std::string>(*fields, "token_digest");
		for (const auto& value : {require_value(schema),
								  require_value(kind_text),
								  require_value(provider_id),
								  require_value(provider_version_text),
								  require_value(provider_binary_digest),
								  require_value(provider_contract_digest),
								  require_value(session_id),
								  require_value(task_id),
								  require_value(input_digest),
								  require_value(invocation_digest),
								  require_value(toolchain_digest),
								  require_value(environment_digest),
								  require_value(sandbox_digest),
								  require_value(dependency_group_id),
								  require_value(atomic_group_id),
								  require_value(batch_id),
								  require_value(stream_id),
								  require_value(highest_acked),
								  require_value(staged_digest),
								  require_value(token_generation),
								  require_value(token_digest)})
			if (!value)
				return unexpected(value.error());

		auto kind = parse_resume_kind(*kind_text);
		if (!kind)
			return unexpected(std::move(kind.error()));
		auto provider_version = parse_semantic_version(*provider_version_text);
		if (!provider_version)
			return unexpected(std::move(provider_version.error()));
		ng1_resume_control output;
		output.schema = std::move(*schema);
		output.kind = *kind;
		output.binding.provider_id = std::move(*provider_id);
		output.binding.provider_version = *provider_version;
		output.binding.provider_binary_digest = std::move(*provider_binary_digest);
		output.binding.provider_semantic_contract_digest = std::move(*provider_contract_digest);
		output.binding.protocol_session_id = std::move(*session_id);
		output.binding.task_id = std::move(*task_id);
		output.binding.task_input_digest = std::move(*input_digest);
		output.binding.normalized_invocation_digest = std::move(*invocation_digest);
		output.binding.toolchain_digest = std::move(*toolchain_digest);
		output.binding.environment_digest = std::move(*environment_digest);
		output.binding.sandbox_policy_digest = std::move(*sandbox_digest);
		output.binding.dependency_group_id = std::move(*dependency_group_id);
		output.binding.atomic_output_group_id = std::move(*atomic_group_id);
		output.binding.batch_id = std::move(*batch_id);
		output.binding.stream_id = *stream_id;
		output.highest_contiguous_acked_sequence = *highest_acked;
		output.staged_digest = std::move(*staged_digest);
		output.token_generation = *token_generation;
		output.token_digest = std::move(*token_digest);
		if (auto token = output.to_validation_token(); !token)
			return unexpected(std::move(token.error()));
		return output;
	}
} // namespace cxxlens::sdk::provider::detail
