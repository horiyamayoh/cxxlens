#pragma once

/**
 * @file worker_observation_codec.hpp
 * @brief Bounded canonical transport for detached Clang 23 replay observations.
 */

#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sdk/gcc_replay_input_internal.hpp"
#include "worker_parser.hpp"

namespace cxxlens::detail::clang23_gcc_replay
{
	struct worker_observation_codec_limits
	{
		std::size_t maximum_bytes{std::size_t{64U} * 1024U * 1024U};
		std::size_t maximum_text_bytes{std::size_t{1024U} * 1024U};
		std::size_t maximum_logical_bytes{observer_product_maximum_logical_bytes};
		std::size_t maximum_observations{observer_product_maximum_observations};

		[[nodiscard]] sdk::result<void> validate() const;
	};

	struct worker_observation_output
	{
		std::string replay_input_digest;
		std::size_t declaration_count{};
		std::size_t warning_count{};
		std::size_t error_count{};
		observation_batch observations;

		[[nodiscard]] bool operator==(const worker_observation_output&) const = default;
	};

	/** Encode one successfully parsed, AST-detached observation batch. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	encode_worker_observations(std::string_view replay_input_digest,
							   const parse_result& parsed,
							   worker_observation_codec_limits limits = {});

	/** Allocation-free preflight followed by strict canonical decode and domain validation. */
	[[nodiscard]] sdk::result<worker_observation_output>
	decode_worker_observations(std::span<const std::byte> bytes,
							   worker_observation_codec_limits limits = {});
} // namespace cxxlens::detail::clang23_gcc_replay
