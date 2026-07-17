#include <algorithm>
#include <array>
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

		[[nodiscard]] std::string join_lines(const auto& values)
		{
			std::ostringstream output;
			for (const auto& value : values)
				output << value << '\n';
			return output.str();
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
			return "none";
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
			return "system_registry";
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
			return "upgrade";
		}

		[[nodiscard]] std::uint8_t source_rank(const discovery_source value) noexcept
		{
			return static_cast<std::uint8_t>(value);
		}

		[[nodiscard]] bool sandbox_satisfies(const sandbox_assurance achieved,
											 const sandbox_assurance required) noexcept
		{
			return static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
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
								 std::uint64_t& total_rows,
								 const std::uint64_t maximum_rows)
		: writer_{&writer}, descriptor_{std::move(descriptor)}, total_rows_{&total_rows},
		  maximum_rows_{maximum_rows}
	{
	}

	result<void> relation_sink::begin(std::string dependency_group,
									  std::string atomic_output_group,
									  std::string batch_id)
	{
		if (open_ || dependency_group.empty() || atomic_output_group.empty() || batch_id.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.batch-state-invalid", "begin"));
		if (auto valid = descriptor_.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		dependency_group_ = std::move(dependency_group);
		atomic_output_group_ = std::move(atomic_output_group);
		batch_id_ = std::move(batch_id);
		auto control =
			encode_control_text(descriptor_.id + "|" + descriptor_.descriptor_digest + "|" +
								dependency_group_ + "|" + atomic_output_group_ + "|" + batch_id_);
		if (!control)
			return cxxlens::sdk::unexpected(std::move(control.error()));
		if (auto sent = writer_->send(message_type::batch_begin, *control); !sent)
			return sent;
		rolling_digest_ =
			*semantic_digest("cxxlens.provider-batch.v1", descriptor_.descriptor_digest);
		open_ = true;
		return {};
	}

	result<void> relation_sink::push(const detached_row& row)
	{
		if (!open_)
			return cxxlens::sdk::unexpected(provider_error("provider.batch-state-invalid", "push"));
		if (total_rows_ == nullptr || *total_rows_ >= maximum_rows_)
			return cxxlens::sdk::unexpected(provider_error("provider.output-limit", "rows"));
		if (auto valid = validate_row(descriptor_, row); !valid)
			return valid;
		const auto canonical = row.canonical_form();
		auto control = encode_control_text(batch_id_ + "|row|" + std::to_string(row_count_));
		if (!control)
			return cxxlens::sdk::unexpected(std::move(control.error()));
		const auto payload = bytes(canonical);
		if (auto sent = writer_->send(message_type::column_chunk, *control, payload); !sent)
			return sent;
		rolling_digest_ =
			*semantic_digest("cxxlens.provider-batch.v1", rolling_digest_ + "\n" + canonical);
		++row_count_;
		++*total_rows_;
		return {};
	}

	result<void> relation_sink::end()
	{
		if (!open_)
			return cxxlens::sdk::unexpected(provider_error("provider.batch-state-invalid", "end"));
		auto control = encode_control_text(batch_id_ + "|" + std::to_string(row_count_) + "|" +
										   rolling_digest_);
		if (!control)
			return cxxlens::sdk::unexpected(std::move(control.error()));
		if (auto sent = writer_->send(message_type::batch_end, *control); !sent)
			return sent;
		open_ = false;
		return {};
	}

	std::uint64_t relation_sink::row_count() const noexcept
	{
		return row_count_;
	}

	context::context(protocol_writer& writer, execution_context execution)
		: writer_{&writer}, execution_{std::move(execution)}
	{
	}

	relation_sink context::relation(relation_descriptor descriptor)
	{
		return relation_sink{*writer_, std::move(descriptor), output_rows_, execution_.budget.rows};
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
		if (!canonical_digest(policy_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-requirement-invalid", "policy_digest"));
		return {};
	}

	result<void> sandbox_report::validate() const
	{
		if (platform.empty() || !unique_nonempty(mechanisms) || !canonical_digest(policy_digest) ||
			!canonical_digest(evidence_digest))
			return cxxlens::sdk::unexpected(
				provider_error("provider.sandbox-report-invalid", "report"));
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

	std::string provider_selection::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"decisions\":[";
		for (std::size_t index = 0U; index < decisions.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& decision = decisions[index];
			output << "{\"binary_digest\":" << json_string(decision.binary_digest)
				   << ",\"provider_id\":" << json_string(decision.provider_id)
				   << ",\"provider_version\":" << json_string(decision.provider_version.string())
				   << ",\"reason\":" << json_string(decision.reason)
				   << ",\"selected\":" << (decision.selected ? "true" : "false")
				   << ",\"source\":" << json_string(source_name(decision.source)) << '}';
		}
		output << "],\"fallback_policy_digest\":";
		if (fallback_policy_digest)
			output << json_string(*fallback_policy_digest);
		else
			output << "null";
		output << ",\"fallback_used\":" << (fallback_used ? "true" : "false")
			   << ",\"selected_manifest\":" << candidate.description.canonical_json()
			   << ",\"selected_source\":" << json_string(source_name(candidate.source)) << '}';
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
		if (request.fallback_policy)
			if (auto valid = request.fallback_policy->validate(request.provider_version); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (candidates.empty())
			return cxxlens::sdk::unexpected(
				provider_error("provider.not-found", request.provider_id));

		std::map<std::pair<std::string, std::string>, std::set<std::pair<std::string, std::string>>>
			binaries;
		for (const auto& candidate : candidates)
			binaries[{candidate.description.provider_id,
					  candidate.description.provider_version.string()}]
				.emplace(candidate.description.package_identity,
						 candidate.description.provider_binary_digest);
		for (const auto& [identity, values] : binaries)
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
												lhs.description.provider_binary_digest} <
								  std::tuple{source_rank(rhs.source),
											 rhs.description.provider_id,
											 rhs.description.provider_version,
											 rhs.description.provider_binary_digest};
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
						static_cast<std::uint8_t>(*manifest_minimum) >
							static_cast<std::uint8_t>(request.sandbox.minimum)
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
		std::vector<scaffold_file> output;
		output.push_back(
			{"CMakeLists.txt",
			 "cmake_minimum_required(VERSION 3.25)\nproject(" + class_name +
				 " LANGUAGES CXX)\nfind_package(" + package +
				 " CONFIG REQUIRED)\n"
				 "add_executable(provider src/main.cpp)\ntarget_link_libraries(provider PRIVATE " +
				 target + ")\ntarget_compile_features(provider PRIVATE cxx_std_23)\n"});
		output.push_back(
			{"provider-manifest.json",
			 R"({"schema":"cxxlens.provider-manifest.v1","provider_id":")" + options.provider_id +
				 R"(","provider_version":"0.1.0","package_identity":")" + options.provider_id +
				 R"(.package","provider_binary_digest":")" + zero_digest +
				 R"(","provider_semantic_contract_digest":")" + zero_digest + R"(","publisher":")" +
				 options.provider_id +
				 ".publisher\",\"license\":\"UNLICENSED\",\"signature\":null,"
				 "\"protocol_range\":{\"major\":1,\"minimum_minor\":0,\"maximum_minor\":0,"
				 "\"required_features\":[\"credit-backpressure\"],\"optional_features\":[]},"
				 "\"platform_tuples\":[\"linux-development\"],\"offered_relations\":[\"" +
				 options.relation_name +
				 R"("],"required_relations":[],"interpretation_domains":[")" + options.provider_id +
				 R"(.interpretation"],"invalidation_contract":")" + zero_digest +
				 R"(","determinism_contract":")" + zero_digest +
				 "\",\"resource_class\":\"provider.standard\",\"sandbox_minimum\":\"enforced\","
				 "\"requested_qualifications\":[\"experimental\"],\"trust_flags\":[],"
				 "\"task_stage\":{\"input\":\"observation\",\"output\":\"assertion\"}}\n"});
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
		if (task_value.task_id.empty() || !task_value.project.validate())
			return cxxlens::sdk::unexpected(provider_error("provider.task-invalid", "task"));
		if (budget.wall_ms == 0U || budget.cpu_ms == 0U || budget.rss_bytes == 0U ||
			budget.output_bytes == 0U || budget.rows == 0U || budget.diagnostics == 0U ||
			budget.open_files == 0U || budget.created_files == 0U || budget.subprocesses == 0U)
			return cxxlens::sdk::unexpected(provider_error("provider.task-invalid", "budget"));
		auto accepted = encode_control_text(std::string{provider.id()} + "|" +
											provider.version().string() + "|" + task_value.task_id);
		if (!accepted)
			return cxxlens::sdk::unexpected(std::move(accepted.error()));
		if (auto sent = writer.send(message_type::task_accepted, *accepted); !sent)
			return sent;
		context callback_context{writer, std::move(execution)};
		if (callback_context.stop_requested())
			return cxxlens::sdk::unexpected(provider_error("provider.cancelled", "task"));
		auto outcome = provider.run(task_value, callback_context);
		if (!outcome)
		{
			auto failed = encode_control_text(outcome.error().code + "|" + outcome.error().field);
			if (!failed)
				return cxxlens::sdk::unexpected(std::move(failed.error()));
			if (auto sent = writer.send(message_type::task_failed, *failed); !sent)
				return sent;
			return cxxlens::sdk::unexpected(std::move(outcome.error()));
		}
		auto coverage = std::move(callback_context.coverage()).finish();
		auto unresolved = std::move(callback_context.unresolved()).finish();
		auto evidence = std::move(callback_context.evidence()).finish();
		if (!coverage)
			return cxxlens::sdk::unexpected(std::move(coverage.error()));
		if (!unresolved)
			return cxxlens::sdk::unexpected(std::move(unresolved.error()));
		if (!evidence)
			return cxxlens::sdk::unexpected(std::move(evidence.error()));
		std::vector<std::string> coverage_lines;
		for (const auto& item : *coverage)
			coverage_lines.push_back(item.kind + "|" + item.id + "|" + item.state + "|" +
									 item.reason);
		std::vector<std::string> unresolved_lines;
		for (const auto& item : *unresolved)
			unresolved_lines.push_back(item.code + "|" + item.subject + "|" + item.detail);
		std::vector<std::string> evidence_lines;
		for (const auto& item : *evidence)
			evidence_lines.push_back(item.kind + "|" + item.subject + "|" + item.producer);
		for (const auto& [type, text] : {
				 std::pair{message_type::coverage_chunk, join_lines(coverage_lines)},
				 std::pair{message_type::unresolved_chunk, join_lines(unresolved_lines)},
				 std::pair{message_type::progress, join_lines(evidence_lines)},
			 })
		{
			auto control = encode_control_text(text);
			if (!control)
				return cxxlens::sdk::unexpected(std::move(control.error()));
			if (auto sent = writer.send(type, *control); !sent)
				return sent;
		}
		auto complete = encode_control_text(task_value.task_id + "|complete");
		if (!complete)
			return cxxlens::sdk::unexpected(std::move(complete.error()));
		return writer.send(message_type::task_complete, *complete);
	}
} // namespace cxxlens::sdk::provider
