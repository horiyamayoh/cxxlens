#include "provider_protocol_v2_adapter.hpp"

#include <new>
#include <utility>

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		enum class codec_operation : std::uint8_t
		{
			encode,
			decode,
		};

		[[nodiscard]] error adapter_error(std::string field, std::string detail)
		{
			return {"provider.protocol-v2-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] result<protocol_v2::message_type> closure_type(const message_type type)
		{
			if (!is_source_closure_message(type))
				return cxxlens::sdk::unexpected(
					adapter_error("message_type", "source-closure-required"));
			return static_cast<protocol_v2::message_type>(static_cast<std::uint16_t>(type));
		}

		[[nodiscard]] protocol_v2::frame to_protocol_frame(const frame& value)
		{
			protocol_v2::frame output;
			output.type =
				static_cast<protocol_v2::message_type>(static_cast<std::uint16_t>(value.type));
			output.protocol_major = value.protocol_major;
			output.protocol_minor = value.protocol_minor;
			output.flags = value.flags;
			output.stream_id = value.stream_id;
			output.sequence = value.sequence;
			output.control = value.control;
			output.payload = value.payload;
			return output;
		}

		[[nodiscard]] frame from_protocol_frame(protocol_v2::frame value)
		{
			frame output;
			output.type = static_cast<message_type>(static_cast<std::uint16_t>(value.type));
			output.stream_id = value.stream_id;
			output.sequence = value.sequence;
			output.control = std::move(value.control);
			output.payload = std::move(value.payload);
			output.protocol_major = value.protocol_major;
			output.protocol_minor = value.protocol_minor;
			output.flags = value.flags;
			return output;
		}

		[[nodiscard]] result<protocol_v2::limits>
		protocol_limits_for_codec(const protocol_limits limits)
		{
			if (limits.protocol_major != protocol_v2_major)
				return cxxlens::sdk::unexpected(
					error{"provider.protocol-major-mismatch", "major", {}});
			if (limits.minimum_minor != protocol_v2_minor ||
				limits.maximum_minor != protocol_v2_minor)
				return cxxlens::sdk::unexpected(
					error{"provider.protocol-minor-mismatch", "minor", "protocol-2.0-only"});
			if (limits.max_control_bytes == 0U || limits.max_payload_bytes == 0U ||
				limits.max_control_bytes > protocol_v2::max_control_bytes ||
				limits.max_payload_bytes > protocol_v2::max_payload_bytes)
				return cxxlens::sdk::unexpected(error{
					"provider.protocol-state-invalid", "limits", "outside-protocol-2-bounds"});
			const auto end_of_stream = static_cast<std::uint16_t>(frame_flag::end_of_stream);
			if ((limits.supported_flags & static_cast<std::uint16_t>(~end_of_stream)) != 0U)
				return cxxlens::sdk::unexpected(error{
					"provider.invalid-frame-flags", "supported_flags", "unsupported-negotiation"});

			protocol_v2::limits output;
			output.minimum_minor = protocol_v2_minor;
			output.maximum_minor = protocol_v2_minor;
			output.supported_flags = limits.supported_flags;
			output.max_control_bytes = limits.max_control_bytes;
			output.max_payload_bytes = static_cast<std::size_t>(limits.max_payload_bytes);
			output.max_frame_bytes = protocol_v2::fixed_header_bytes + output.max_control_bytes +
				output.max_payload_bytes;
			return output;
		}

		[[nodiscard]] error map_codec_error(error failure, const codec_operation operation)
		{
			if (failure.code == "protocol-v2.downgrade-rejected")
				return {failure.field == "protocol_major" ? "provider.protocol-major-mismatch"
														  : "provider.protocol-minor-mismatch",
						failure.field == "protocol_major" ? "major" : "minor",
						std::move(failure.detail)};
			if (failure.code == "protocol-v2.digest-mismatch")
				return {"provider.checksum-mismatch", "digest", std::move(failure.detail)};
			if (failure.code == "protocol-v2.cbor-invalid")
				return {"provider.malformed-frame", "control", std::move(failure.detail)};
			if (failure.code == "protocol-v2.unknown-required-extension")
				return {"provider.unknown-required-extension", "flags", std::move(failure.detail)};
			if (failure.code == "protocol-v2.unsupported-compression")
				return {"provider.unsupported-compression", "flags", std::move(failure.detail)};
			if (failure.code == "protocol-v2.unknown-message")
				return {"provider.unknown-message-type", "type", std::move(failure.detail)};
			if (failure.code == "protocol-v2.unknown-extension")
				return {"provider.invalid-frame-flags", "flags", std::move(failure.detail)};
			if (failure.code == "protocol-v2.resource-limit")
			{
				if (failure.field == "limits")
					return {"provider.protocol-state-invalid", "limits", std::move(failure.detail)};
				if (failure.field == "maximum_frames")
					return {"provider.stream-invalid", "maximum_frames", std::move(failure.detail)};
				if (operation == codec_operation::encode && failure.field == "control")
					return {"provider.oversized-control", "control", std::move(failure.detail)};
				if (operation == codec_operation::encode && failure.field == "payload")
					return {"provider.oversized-payload", "payload", std::move(failure.detail)};
				return {"provider.oversized-frame", failure.field, std::move(failure.detail)};
			}
			if (failure.code == "protocol-v2.header-invalid")
			{
				if (failure.field == "header" || failure.field == "frame" ||
					failure.field == "transcript")
					return {"provider.truncated-stream", failure.field, std::move(failure.detail)};
				if (failure.field == "flags")
					return {"provider.invalid-frame-flags", "flags", std::move(failure.detail)};
				if (failure.field == "payload")
					return {
						"provider.protocol-state-invalid", "heartbeat", std::move(failure.detail)};
				return {"provider.malformed-frame", failure.field, std::move(failure.detail)};
			}
			return {"provider.protocol-v2-invalid",
					std::move(failure.field),
					std::move(failure.detail)};
		}
	} // namespace

	result<std::vector<std::byte>> encode_provider_protocol_v2_frame(const frame& value,
																	 const protocol_limits limits)
	{
		try
		{
			auto native_limits = protocol_limits_for_codec(limits);
			if (!native_limits)
				return cxxlens::sdk::unexpected(std::move(native_limits.error()));
			auto encoded = protocol_v2::encode_frame(to_protocol_frame(value), *native_limits);
			if (!encoded)
				return cxxlens::sdk::unexpected(
					map_codec_error(std::move(encoded.error()), codec_operation::encode));
			return std::move(*encoded);
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(
				error{"provider.oversized-frame", "frame", "allocation"});
		}
	}

	result<frame> decode_provider_protocol_v2_frame(const std::span<const std::byte> input,
													const protocol_limits limits)
	{
		try
		{
			auto native_limits = protocol_limits_for_codec(limits);
			if (!native_limits)
				return cxxlens::sdk::unexpected(std::move(native_limits.error()));
			auto decoded = protocol_v2::decode_frame(input, *native_limits);
			if (!decoded)
				return cxxlens::sdk::unexpected(
					map_codec_error(std::move(decoded.error()), codec_operation::decode));
			return from_protocol_frame(std::move(*decoded));
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(
				error{"provider.oversized-frame", "frame", "allocation"});
		}
	}

	result<std::vector<frame>>
	decode_provider_protocol_v2_frame_stream(const std::span<const std::byte> input,
											 const protocol_limits limits,
											 const std::uint64_t maximum_frames)
	{
		try
		{
			auto native_limits = protocol_limits_for_codec(limits);
			if (!native_limits)
				return cxxlens::sdk::unexpected(std::move(native_limits.error()));
			auto decoded = protocol_v2::decode_frame_stream(input, *native_limits, maximum_frames);
			if (!decoded)
				return cxxlens::sdk::unexpected(
					map_codec_error(std::move(decoded.error()), codec_operation::decode));
			std::vector<frame> output;
			output.reserve(decoded->size());
			for (auto& value : *decoded)
				output.push_back(from_protocol_frame(std::move(value)));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(
				error{"provider.oversized-frame", "frame", "allocation"});
		}
	}

	std::size_t prepared_provider_protocol_v2_frame::control_bytes() const noexcept
	{
		return prepared_.control_bytes();
	}

	std::size_t prepared_provider_protocol_v2_frame::payload_bytes() const noexcept
	{
		return prepared_.payload_bytes();
	}

	std::size_t prepared_provider_protocol_v2_frame::body_resident_bytes() const noexcept
	{
		return prepared_.body_resident_bytes();
	}

	message_type prepared_provider_protocol_v2_frame::type() const noexcept
	{
		return static_cast<message_type>(static_cast<std::uint16_t>(prepared_.type()));
	}

	std::uint16_t prepared_provider_protocol_v2_frame::flags() const noexcept
	{
		return prepared_.flags();
	}

	std::uint64_t prepared_provider_protocol_v2_frame::stream_id() const noexcept
	{
		return prepared_.stream_id();
	}

	std::uint64_t prepared_provider_protocol_v2_frame::sequence() const noexcept
	{
		return prepared_.sequence();
	}

	result<frame> prepared_provider_protocol_v2_frame::finalize(std::vector<std::byte>&& control,
																std::vector<std::byte>&& payload) &&
	{
		try
		{
			auto decoded = std::move(prepared_).finalize(std::move(control), std::move(payload));
			if (!decoded)
				return cxxlens::sdk::unexpected(
					map_codec_error(std::move(decoded.error()), codec_operation::decode));
			return from_protocol_frame(std::move(*decoded));
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(
				error{"provider.oversized-frame", "frame", "allocation"});
		}
	}

	result<prepared_provider_protocol_v2_frame> prepare_provider_protocol_v2_frame(
		const std::span<const std::byte, protocol_v2::fixed_header_bytes> header,
		const protocol_limits limits)
	{
		try
		{
			auto native_limits = protocol_limits_for_codec(limits);
			if (!native_limits)
				return cxxlens::sdk::unexpected(std::move(native_limits.error()));
			auto prepared = protocol_v2::prepare_frame_header(header, *native_limits);
			if (!prepared)
				return cxxlens::sdk::unexpected(
					map_codec_error(std::move(prepared.error()), codec_operation::decode));
			return prepared_provider_protocol_v2_frame{std::move(*prepared)};
		}
		catch (const std::bad_alloc&)
		{
			return cxxlens::sdk::unexpected(
				error{"provider.oversized-frame", "frame", "allocation"});
		}
	}

	result<void> provider_protocol_v2_session::validate() const
	{
		if (session_id.empty() || task_id.empty() || stream_id == 0U ||
			initial_credit.bytes == 0U || initial_credit.frames == 0U)
			return cxxlens::sdk::unexpected(adapter_error("session", "binding-or-credit-invalid"));
		if (!task_v4_digest.empty() && !task_v4_digest.starts_with("semantic-v2:sha256:"))
			return cxxlens::sdk::unexpected(adapter_error("task_v4_digest", "semantic-digest"));
		if (!closure_digest.empty() && !closure_digest.starts_with("semantic-v2:sha256:"))
			return cxxlens::sdk::unexpected(adapter_error("closure_digest", "semantic-digest"));
		if (!manifest_digest.empty() && !manifest_digest.starts_with("semantic-v2:sha256:"))
			return cxxlens::sdk::unexpected(adapter_error("manifest_digest", "semantic-digest"));
		return {};
	}

	result<std::vector<std::byte>>
	encode_provider_protocol_v2_closure_control(const message_type type,
												const provider_protocol_v2_control& control,
												const provider_protocol_v2_closure_limits limits)
	{
		auto mapped = closure_type(type);
		if (!mapped)
			return cxxlens::sdk::unexpected(std::move(mapped.error()));
		return protocol_v2::encode_closure_control(*mapped, control, limits);
	}

	result<provider_protocol_v2_control>
	decode_provider_protocol_v2_closure_control(const message_type type,
												const std::span<const std::byte> control,
												const provider_protocol_v2_closure_limits limits)
	{
		auto mapped = closure_type(type);
		if (!mapped)
			return cxxlens::sdk::unexpected(std::move(mapped.error()));
		return protocol_v2::decode_closure_control(*mapped, control, limits);
	}

	result<void>
	validate_provider_protocol_v2_closure_payload(const message_type type,
												  const provider_protocol_v2_control& control,
												  const std::span<const std::byte> payload,
												  const provider_protocol_v2_closure_limits limits)
	{
		auto mapped = closure_type(type);
		if (!mapped)
			return cxxlens::sdk::unexpected(std::move(mapped.error()));
		return protocol_v2::validate_closure_payload(*mapped, control, payload, limits);
	}

	result<provider_protocol_v2_closure_state>
	provider_protocol_v2_closure_state::create(provider_protocol_v2_session session)
	{
		if (auto valid = session.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		protocol_v2::closure_session native;
		native.session_id = std::move(session.session_id);
		native.task_id = std::move(session.task_id);
		native.task_v4_digest = std::move(session.task_v4_digest);
		native.closure_digest = std::move(session.closure_digest);
		native.manifest_digest = std::move(session.manifest_digest);
		native.stream_id = session.stream_id;
		native.initial_credit = {session.initial_credit.bytes, session.initial_credit.frames};
		native.limits = session.limits;
		auto transfer = protocol_v2::closure_transfer::create(std::move(native));
		if (!transfer)
			return cxxlens::sdk::unexpected(std::move(transfer.error()));
		return provider_protocol_v2_closure_state{std::move(*transfer)};
	}

	result<void> provider_protocol_v2_closure_state::accept(const frame& value)
	{
		if (!is_source_closure_message(value.type))
			return cxxlens::sdk::unexpected(
				adapter_error("message_type", "source-closure-required"));
		return transfer_.accept(to_protocol_frame(value));
	}

} // namespace cxxlens::sdk::provider::detail
