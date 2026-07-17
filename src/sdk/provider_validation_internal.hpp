#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::sdk::provider::detail
{
	enum class transcript_terminal_kind : std::uint8_t
	{
		failed,
		complete,
	};

	struct transcript_terminal
	{
		transcript_terminal_kind kind{transcript_terminal_kind::failed};
		std::string reason;
	};

	struct transcript_validation_request
	{
		std::string task_id;
		std::string provider_id;
		semantic_version provider_version;
		const manifest* provider_manifest{};
		std::span<const relation_descriptor> output_descriptors;
		protocol_credit output_credit;
		bool require_handshake{};
	};

	[[nodiscard]] result<transcript_terminal>
	validate_provider_transcript(const transcript_validation_request& request,
								 std::span<const frame> frames,
								 protocol_limits session_limits);
} // namespace cxxlens::sdk::provider::detail
