#pragma once

#include <cstdint>
#include <string>
#include <string_view>

#include "frontend_port.hpp"

namespace cxxlens::detail::frontend
{
	inline constexpr std::string_view worker_ipc_schema = "cxxlens.frontend-worker-ipc.v1";
	inline constexpr std::uint32_t worker_ipc_version = 1U;

	struct worker_request
	{
		parse_task task;
		std::string profile;
		std::string snapshot_key;
		std::string input_fingerprint;
		std::string toolchain_version;
	};

	[[nodiscard]] result<std::string> encode_worker_request(const worker_request& request);
	[[nodiscard]] result<worker_request> decode_worker_request(std::string_view bytes);
	[[nodiscard]] result<std::string>
	encode_worker_response(const result<observation_batch>& response);
	[[nodiscard]] result<observation_batch> decode_worker_response(std::string_view bytes);
} // namespace cxxlens::detail::frontend
