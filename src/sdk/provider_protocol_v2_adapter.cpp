#include "provider_protocol_v2_adapter.hpp"

#include <utility>

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
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
	} // namespace

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
		native.limits = std::move(session.limits);
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

	result<std::vector<std::byte>> encode_provider_protocol_v2_heartbeat_control(
		const provider_protocol_v2_heartbeat_control& value)
	{
		return encode_ng1_heartbeat_control(value);
	}

	result<provider_protocol_v2_heartbeat_control>
	decode_provider_protocol_v2_heartbeat_control(const std::span<const std::byte> control)
	{
		return decode_ng1_heartbeat_control(control);
	}

	result<void> validate_provider_protocol_v2_heartbeat(
		const frame& value, const provider_protocol_v2_session_binding* expected_binding)
	{
		if (value.type != message_type::heartbeat || value.protocol_major != protocol_v2_major ||
			value.protocol_minor != protocol_v2_minor || value.flags != 0U ||
			!value.payload.empty())
			return cxxlens::sdk::unexpected(adapter_error("heartbeat", "frame-shape-or-version"));
		auto decoded = decode_provider_protocol_v2_heartbeat_control(value.control);
		if (!decoded)
			return cxxlens::sdk::unexpected(std::move(decoded.error()));
		if (expected_binding != nullptr)
		{
			const provider_protocol_v2_session_binding observed{decoded->provider_id,
																decoded->provider_version,
																decoded->protocol_session_id,
																decoded->task_id,
																decoded->stream_id};
			if (observed != *expected_binding || value.stream_id != expected_binding->stream_id)
				return cxxlens::sdk::unexpected(
					adapter_error("heartbeat", "session-binding-mismatch"));
		}
		return {};
	}
} // namespace cxxlens::sdk::provider::detail
