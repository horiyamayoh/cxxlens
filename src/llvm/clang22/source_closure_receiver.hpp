#pragma once

/**
 * @file source_closure_receiver.hpp
 * @brief Bounded Protocol 2.0 source-closure frame receiver.
 *
 * The materialization request document and the source-closure frame channel
 * are deliberately separate.  This receiver consumes only the latter.  It
 * never reads stdin, opens a path, or derives task authority from untrusted
 * request metadata.
 */

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <cxxlens/sdk/provider.hpp>

#include "source_closure.hpp"
#include "source_closure_transport.hpp"

namespace cxxlens::detail::clang22
{
	/** Pull bytes from the authenticated Protocol 2.0 source-closure channel. */
	class source_closure_frame_source
	{
	  public:
		virtual ~source_closure_frame_source() = default;
		[[nodiscard]] virtual sdk::result<std::size_t> read(std::span<std::byte> destination) = 0;
	};

	/** Emit ACK frames on the authenticated Protocol 2.0 source-closure channel. */
	class source_closure_frame_sink
	{
	  public:
		virtual ~source_closure_frame_sink() = default;
		[[nodiscard]] virtual sdk::result<void> write(std::span<const std::byte> frame_bytes) = 0;
	};

	/** Caller-owned authority and wire binding for one closure transfer. */
	struct source_closure_receiver_options
	{
		source_closure_transfer_binding binding;
		source_closure_task_v4_authority* authority{};
		std::uint64_t stream_id{1U};
		std::uint64_t maximum_frames{16'384U};
		source_closure_transport_limits limits{};
	};

	/** The only source-derived value exposed after a complete ACK. */
	struct source_closure_receiver_result
	{
		source_closure_snapshot snapshot;
		source_closure_ack_credentials credentials;
		std::string transfer_digest;
	};

	/**
	 * Receive one complete Protocol 2.0 source-closure transcript.
	 *
	 * The caller must provide an authority whose `revalidate()` checks the
	 * inherited v2.2/task-v4 contract.  A frame is never admitted to the spool
	 * before that authority and the canonical provider codec have accepted it.
	 * Once the inbound seal is valid, the receiver issues message 28 through
	 * `sink` and returns the sealed snapshot.  Any malformed, truncated,
	 * rejected, or failed-ACK transfer fails closed and cleans the task-local
	 * spool.  It never manufactures worker/output/publication authority.
	 */
	[[nodiscard]] sdk::result<source_closure_receiver_result>
	receive_source_closure_frames(source_closure_frame_source& source,
								  source_closure_frame_sink& sink,
								  source_closure_receiver_options options);
} // namespace cxxlens::detail::clang22
