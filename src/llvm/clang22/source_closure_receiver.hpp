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
#include <memory>
#include <span>
#include <stop_token>
#include <string>

#include <cxxlens/sdk/provider.hpp>

#include "source_closure.hpp"
#include "source_closure_transport.hpp"

namespace cxxlens::detail::clang22
{
	/** Monotonic time environment port used by bounded source-closure operations. */
	class source_closure_monotonic_clock
	{
	  public:
		virtual ~source_closure_monotonic_clock() = default;
		[[nodiscard]] virtual sdk::result<std::uint64_t> now_ns() const = 0;
	};

	/** Production clock backed by the runtime monotonic clock port. */
	class source_closure_system_monotonic_clock final : public source_closure_monotonic_clock
	{
	  public:
		[[nodiscard]] sdk::result<std::uint64_t> now_ns() const override;
	};

	inline constexpr std::uint64_t source_closure_default_progress_timeout_ns = 5'000'000'000ULL;

	class source_closure_spool_relay;

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
		std::stop_token cancellation;
		/** Optional caller-owned clock; null selects an operation-owned system clock. */
		const source_closure_monotonic_clock* clock{};
		/** Absolute bound for one no-progress interval; a complete frame starts the next. */
		std::uint64_t progress_timeout_ns{source_closure_default_progress_timeout_ns};
	};

	/** Exact initial wire credit reserved for a bounded source-closure transcript. */
	struct source_closure_receiver_credit
	{
		std::uint64_t bytes{};
		std::uint64_t frames{};
	};

	/**
	 * Compute receiver credit using the same complete-frame byte unit as Protocol 2.
	 *
	 * This source-private product helper deliberately includes the fixed 104-byte
	 * header for every possible frame.  The caller remains responsible for first
	 * rejecting limits which exceed the fixed source-closure contract.
	 */
	[[nodiscard]] sdk::result<source_closure_receiver_credit>
	source_closure_receiver_initial_credit(const source_closure_transport_limits& limits);

	/** The only source-derived value exposed after a complete ACK. */
	struct source_closure_receiver_result
	{
		source_closure_snapshot snapshot;
		source_closure_ack_credentials credentials;
		/** The authenticated wire binding used for this transfer. */
		std::uint64_t stream_id{};
		std::uint64_t first_sequence{};
		/** Cleanup/terminal ownership retained after ACK for worker crash or connection loss. */
		std::shared_ptr<source_closure_spool_relay> relay;
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
								  const source_closure_receiver_options& options);
} // namespace cxxlens::detail::clang22
