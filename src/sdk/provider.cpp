#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <iomanip>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::sdk::provider
{
	namespace
	{
		constexpr std::size_t header_size = 104U;

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

		[[nodiscard]] std::vector<std::byte> cbor_text(const std::string_view text)
		{
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

		[[nodiscard]] std::string escape_json(const std::string_view input)
		{
			std::ostringstream output;
			for (const auto byte : input)
			{
				switch (byte)
				{
					case '\\':
						output << "\\\\";
						break;
					case '"':
						output << "\\\"";
						break;
					case '\n':
						output << "\\n";
						break;
					case '\r':
						output << "\\r";
						break;
					case '\t':
						output << "\\t";
						break;
					default:
						if (static_cast<unsigned char>(byte) < 0x20U)
							output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
								   << static_cast<unsigned int>(static_cast<unsigned char>(byte));
						else
							output << byte;
				}
			}
			return output.str();
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return "\"" + escape_json(value) + "\"";
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
		if (type == 0U || type > static_cast<std::uint16_t>(message_type::close))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-message-type", "type"));

		std::vector<std::byte> output;
		output.reserve(header_size + value.control.size() + value.payload.size());
		for (const auto byte : std::string_view{"CXXP"})
			output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
		append_big_endian(output, static_cast<std::uint16_t>(1U));
		append_big_endian(output, static_cast<std::uint16_t>(0U));
		append_big_endian(output, type);
		append_big_endian(output, static_cast<std::uint16_t>(0U));
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
		if (read_big_endian<std::uint16_t>(input, 4U) != 1U)
			return cxxlens::sdk::unexpected(
				provider_error("provider.protocol-major-mismatch", "major"));
		const auto type = read_big_endian<std::uint16_t>(input, 8U);
		if (type == 0U || type > static_cast<std::uint16_t>(message_type::close))
			return cxxlens::sdk::unexpected(
				provider_error("provider.unknown-message-type", "type"));
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
		const auto control_digest = digest_bytes(content_digest(output.control));
		const auto payload_digest = digest_bytes(content_digest(output.payload));
		if (!std::ranges::equal(control_digest, input.subspan(40U, 32U)) ||
			!std::ranges::equal(payload_digest, input.subspan(72U, 32U)))
			return cxxlens::sdk::unexpected(provider_error("provider.checksum-mismatch", "digest"));
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
		{
			const auto byte = std::to_integer<unsigned char>(value);
			if (byte == 0U)
				return cxxlens::sdk::unexpected(
					provider_error("provider.malformed-frame", "control-utf8"));
			output.push_back(static_cast<char>(byte));
		}
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
									   const std::span<const std::byte> payload)
	{
		frame value{type,
					stream_id_,
					sequence_,
					{control.begin(), control.end()},
					{payload.begin(), payload.end()}};
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
		const auto control = cbor_text(descriptor_.id + "|" + dependency_group_ + "|" +
									   atomic_output_group_ + "|" + batch_id_);
		if (auto sent = writer_->send(message_type::batch_begin, control); !sent)
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
		const auto control = cbor_text(batch_id_ + "|row|" + std::to_string(row_count_));
		const auto payload = bytes(canonical);
		if (auto sent = writer_->send(message_type::column_chunk, control, payload); !sent)
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
		const auto control =
			cbor_text(batch_id_ + "|" + std::to_string(row_count_) + "|" + rolling_digest_);
		if (auto sent = writer_->send(message_type::batch_end, control); !sent)
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
		output << "],\"fallback_used\":" << (fallback_used ? "true" : "false")
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
		std::optional<std::uint8_t> exact_precedence;
		for (const auto index : order)
		{
			const auto& candidate = candidates[index];
			if (candidate.description.provider_id == request.provider_id &&
				candidate.description.provider_version == request.provider_version)
			{
				exact_precedence = exact_precedence
					? std::min(*exact_precedence, source_rank(candidate.source))
					: source_rank(candidate.source);
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
			const bool exact = candidate.description.provider_id == request.provider_id &&
				candidate.description.provider_version == request.provider_version &&
				candidate.description.provider_binary_digest == request.provider_binary_digest &&
				candidate.description.provider_semantic_contract_digest ==
					request.provider_semantic_contract_digest;
			const bool adjacent =
				candidate.description.provider_id == request.provider_id && !exact;
			if (!candidate.authoritative_path)
				decision.reason = "security.path-only-discovery";
			else if (auto manifest_valid = candidate.description.validate(); !manifest_valid)
				decision.reason = manifest_valid.error().code;
			else if (!candidate.validation_error.empty())
				decision.reason = candidate.validation_error;
			else if (!candidate.trust_valid)
				decision.reason = "security.signature-untrusted";
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
				else if (adjacent && request.allow_adjacent_fallback && !exact_precedence)
				{
					decision.selected = true;
					decision.reason = "selected-explicit-fallback";
					selected = index;
					selected_fallback = true;
				}
				else if (adjacent)
					decision.reason = "provider.adjacent-fallback-forbidden";
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
		return provider_selection{candidates[*selected], std::move(decisions), selected_fallback};
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
		const auto accepted = cbor_text(std::string{provider.id()} + "|" +
										provider.version().string() + "|" + task_value.task_id);
		if (auto sent = writer.send(message_type::task_accepted, accepted); !sent)
			return sent;
		context callback_context{writer, std::move(execution)};
		if (callback_context.stop_requested())
			return cxxlens::sdk::unexpected(provider_error("provider.cancelled", "task"));
		auto outcome = provider.run(task_value, callback_context);
		if (!outcome)
		{
			const auto failed = cbor_text(outcome.error().code + "|" + outcome.error().field);
			if (auto sent = writer.send(message_type::task_failed, failed); !sent)
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
			const auto control = cbor_text(text);
			if (auto sent = writer.send(type, control); !sent)
				return sent;
		}
		const auto complete = cbor_text(task_value.task_id + "|complete");
		return writer.send(message_type::task_complete, complete);
	}
} // namespace cxxlens::sdk::provider
