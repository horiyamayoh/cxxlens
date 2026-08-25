#include <utility>

#include "provider_protocol_v2_adapter.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		[[nodiscard]] error heartbeat_error(std::string field, std::string detail)
		{
			return {"provider.protocol-v2-invalid", std::move(field), std::move(detail)};
		}
	} // namespace

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
			return cxxlens::sdk::unexpected(heartbeat_error("heartbeat", "frame-shape-or-version"));
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
					heartbeat_error("heartbeat", "session-binding-mismatch"));
		}
		return {};
	}
} // namespace cxxlens::sdk::provider::detail
