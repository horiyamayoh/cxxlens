#pragma once

/** @file provider_worker_protocol_v2_input.hpp
 *  @brief Bounded Protocol 2.0 host-transcript decoding for a worker launch.
 */

#include <cstddef>
#include <cstdint>
#include <istream>
#include <span>
#include <string>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

namespace cxxlens::detail::clang22
{
	/** Maximum logical task-input bytes accepted by the task-input-chunks-v2 profile. */
	inline constexpr std::size_t provider_worker_protocol_v2_maximum_input_bytes =
		64U * 1024U * 1024U;
	/** Maximum bytes carried by one input_chunk frame in the host profile. */
	inline constexpr std::size_t provider_worker_protocol_v2_maximum_chunk_bytes =
		1U * 1024U * 1024U;
	/** Maximum input_chunk frames in one bounded task transcript. */
	inline constexpr std::size_t provider_worker_protocol_v2_maximum_input_chunks = 64U;
	/**
	 * Maximum frames in the fixed host transcript state machine:
	 * hello_ack, schema, open_task, descriptor, chunks, credit, close.
	 */
	inline constexpr std::size_t provider_worker_protocol_v2_maximum_frames =
		6U + provider_worker_protocol_v2_maximum_input_chunks;
	/**
	 * Maximum bytes read from stdin before framing is attempted.  This includes the logical input,
	 * one maximum control section per bounded frame, and every fixed frame header.  A larger stream
	 * is rejected before it can cause an allocation or be mistaken for a second input format.
	 */
	inline constexpr std::size_t provider_worker_protocol_v2_maximum_wire_bytes =
		provider_worker_protocol_v2_maximum_input_bytes +
		provider_worker_protocol_v2_maximum_frames * (sizeof(std::byte) * 104U + 65'536U);

	/**
	 * Source-free, typed launch input after the complete Protocol 2.0 host transcript is validated.
	 *
	 * `protocol_content_digest` is the SHA-256 content digest of the reconstructed input bytes.  It
	 * is intentionally separate from any task-v4 semantic digest; this type contains no Store,
	 * publication, filesystem, or materializer authority and does not derive one from the wire.
	 */
	struct provider_worker_protocol_v2_launch_envelope
	{
		std::string provider_manifest;
		sdk::provider::open_task_metadata task;
		sdk::provider::protocol_credit output_credit;
		std::vector<std::byte> payload;
		std::string protocol_content_digest;
		std::uint64_t stream_id{1U};
		std::uint16_t protocol_major{sdk::provider::protocol_v2_major};
		std::uint16_t protocol_minor{sdk::provider::protocol_v2_minor};
	};

	/**
	 * Decode one complete, bounded Protocol 2.0 host transcript from already-read stdin bytes.
	 *
	 * The public provider frame codec and host-transcript state machine remain the sole wire
	 * authorities.  In particular, this path requires the six-frame/chunked task-input profile and
	 * cannot accept the legacy inline payload transcript, JSON, a second stream, or a task-v4
	 * semantic digest in the content-digest slot.
	 */
	[[nodiscard]] sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input(
		std::span<const std::byte> encoded,
		const sdk::provider::host_transcript_expectation& expected);

	/** Read stdin with the same wire bound and decode it as one host transcript. */
	[[nodiscard]] sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input(
		std::istream& input, const sdk::provider::host_transcript_expectation& expected);

	/**
	 * Read one host transcript from a live pipe and stop at the authenticated close frame.
	 *
	 * A process host keeps stdin open while it observes provider output, so waiting for EOF at
	 * this boundary is a deadlock.  This variant uses the same 104-byte codec and validation as the
	 * bounded-buffer overload, but treats the validated close frame as the logical input boundary.
	 */
	[[nodiscard]] sdk::result<provider_worker_protocol_v2_launch_envelope>
	decode_provider_worker_protocol_v2_input_until_close(
		std::istream& input, const sdk::provider::host_transcript_expectation& expected);
} // namespace cxxlens::detail::clang22
