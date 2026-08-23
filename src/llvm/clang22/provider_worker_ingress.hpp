#pragma once

/** @file provider_worker_ingress.hpp @brief Bounded task-v4 process-envelope decoding. */

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "provider_task_v4.hpp"
#include "source_closure_task_v4.hpp"
#include "source_closure_transport.hpp"

namespace cxxlens::detail::clang22
{
	/**
	 * Maximum bytes accepted by the source-private task-v4 process envelope.
	 *
	 * The envelope carries metadata and canonical source-free task payload bytes only.  Source
	 * closure bytes arrive on the separately inherited Protocol 2.0 channel and are never decoded
	 * from this value.
	 */
	inline constexpr std::size_t provider_worker_v4_maximum_envelope_bytes =
		source_closure_task_v4_maximum_payload_bytes + 64U * 1024U;

	/**
	 * Explicit task-v4 process authority after bounded JSON decoding.
	 *
	 * Every process input required by the worker is retained here: effective arguments, qualified
	 * read roots, closure/session binding, source-free base projection, and task-v4 payload.  No
	 * field is reconstructed from an ID, cwd, environment, or the source-closure channel.
	 */
	struct provider_worker_v4_ingress
	{
		std::vector<std::byte> task_payload;
		std::vector<std::byte> base_task_projection;
		std::string expected_base_task_digest;
		std::string expected_task_v4_input_digest;
		provider_task_v4_input_authority input_authority;
		source_closure_transfer_binding closure_binding;
		std::string expected_transfer_digest;
		std::uint64_t stream_id{};
	};

	/**
	 * Decode and independently bind one canonical task-v4 process envelope.
	 *
	 * This function only constructs a typed, source-free authority.  It neither opens an FD,
	 * receives source-closure frames, launches a worker, nor mutates Store state.
	 */
	[[nodiscard]] sdk::result<provider_worker_v4_ingress>
	decode_provider_worker_v4_ingress(std::string raw);
} // namespace cxxlens::detail::clang22
