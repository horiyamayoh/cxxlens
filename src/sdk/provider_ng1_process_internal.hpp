#pragma once

#include <memory>
#include <optional>
#include <span>
#include <stop_token>

#include <cxxlens/sdk/provider.hpp>

#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/**
	 * Own a forked NG1 child until the live process channel takes ownership.
	 *
	 * The group-id probe is deliberately conservative: a child PID allocated by fork cannot be
	 * an unrelated process-group leader while it remains waitable.  Cleanup therefore attempts the
	 * child process group even when the parent has not observed the setup ACK, then signals the
	 * leader as a fallback and waits for that exact child.  This keeps a partial launch fail-closed
	 * without depending on an ACK arriving before the bounded handshake deadline.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_post_fork_process_guard
	{
	  public:
		explicit ng1_post_fork_process_guard(int child) noexcept;
		ng1_post_fork_process_guard(const ng1_post_fork_process_guard&) = delete;
		ng1_post_fork_process_guard& operator=(const ng1_post_fork_process_guard&) = delete;
		~ng1_post_fork_process_guard() noexcept;

		/** Transfer child and process-group ownership to the live process object. */
		void release() noexcept;

	  private:
		void cleanup() noexcept;

		int child_{};
	};

	/**
	 * Source-private bidirectional process channel for a future NG1 session.
	 *
	 * The channel owns framed transport only. It does not validate the NG1 lifecycle, publish
	 * output, or advertise capability; callers must pass every received frame through the shared
	 * validator and the host-receipted NG1 session coordinator.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_duplex_process
	{
	  public:
		virtual ~ng1_duplex_process() = default;

		/** Encode and write one host-to-provider frame under the negotiated limits. */
		[[nodiscard]] virtual result<void> send_frame(const frame& value) = 0;
		/** Read one complete provider-to-host frame, or nullopt after an orderly EOF. */
		[[nodiscard]] virtual result<std::optional<frame>>
		receive_frame(std::stop_token cancellation) = 0;
		/** Close stdin, drain output, and return the exact process/sandbox outcome. */
		[[nodiscard]] virtual result<process_output> finish(std::stop_token cancellation) = 0;
		/** Kill the process group and return the bounded cleanup outcome. */
		[[nodiscard]] virtual result<process_output> terminate(process_status status) = 0;
	};

	/** Source-private system process port that keeps stdin/stdout live until explicit completion.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_duplex_process_port
	{
	  public:
		virtual ~ng1_duplex_process_port() = default;
		[[nodiscard]] virtual result<std::unique_ptr<ng1_duplex_process>>
		start(const process_invocation& invocation,
			  protocol_limits limits,
			  std::stop_token cancellation) const = 0;
	};

	/** Create the source-private Linux live process port, or a fail-closed unavailable port. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN std::unique_ptr<ng1_duplex_process_port>
	make_system_ng1_duplex_process_port();
} // namespace cxxlens::sdk::provider::detail
