#pragma once

#include <cstdint>
#include <optional>

#include "provider_ng1_validation_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/**
	 * Source-private NG1 crash/restart value adapter.
	 *
	 * This adapter composes the accepted recovery transition matrix with the
	 * durable resume validator. It does not launch, kill, restart, or inspect a
	 * worker and it does not turn a spill receipt into a durability proof. Those
	 * observations remain inputs supplied by a future host process/spill port.
	 * Keeping this seam private allows the state ordering to be tested without
	 * advertising or activating NG1 on the NG0 runtime path.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_recovery_adapter
	{
	  public:
		[[nodiscard]] static result<ng1_recovery_adapter> create(ng1_resume_binding binding);

		[[nodiscard]] ng1_recovery_state state() const noexcept
		{
			return state_;
		}

		/** Observe a provider exit, including a crash before a kill confirmation. */
		[[nodiscard]] result<void> observe_worker_exit();
		/** Observe a liveness failure before the host confirms worker termination. */
		[[nodiscard]] result<void> observe_heartbeat_timeout();
		/** Observe a progress-rate failure before the host confirms worker termination. */
		[[nodiscard]] result<void> observe_progress_rate_failure();
		/** Observe an explicit host cancellation request while the worker is running. */
		[[nodiscard]] result<void> request_cancel();
		/** Observe a provider cancellation acknowledgement; this is terminal and non-restartable.
		 */
		[[nodiscard]] result<void> acknowledge_cancel();
		/** Observe cancellation timeout; the worker-kill boundary is still required. */
		[[nodiscard]] result<void> timeout_cancel();
		/** Confirm that the failed worker/process group is no longer running. */
		[[nodiscard]] result<void> confirm_worker_kill();
		/** Fail closed when the negotiated heartbeat clock is invalid. */
		[[nodiscard]] result<void> reject_heartbeat_clock();

		/**
		 * Admit one host-observed durable token after worker termination.
		 * Invalid token/receipt input transitions the adapter to `failed` and
		 * returns the validator's stable failure unchanged.
		 */
		[[nodiscard]] result<void> accept_durable_resume(const ng1_resume_token& token,
														 const ng1_spill_fsync_receipt& receipt,
														 bool open_dependency_group,
														 bool terminal,
														 std::uint64_t highest_observed_sequence);
		/** Transition a malformed or mismatched durable resume to the terminal failure state. */
		[[nodiscard]] result<void> reject_durable_resume(error original_error);

		/** Return the exact replay start derived from the accepted durable ack. */
		[[nodiscard]] result<std::uint64_t> replay_start_sequence() const;
		/**
		 * Accept an externally shared-validated replay observation whose first
		 * occurrence is exactly durable_ack + 1. This value-only adapter does not
		 * claim to validate the replay stream or adopt output.
		 */
		[[nodiscard]] result<void> accept_replay_validated(std::uint64_t first_sequence);
		/** Transition a malformed or mismatched replay observation to terminal failure. */
		[[nodiscard]] result<void> reject_replay(error original_error);
		/** Seal a validated output after running or a valid restart replay. */
		[[nodiscard]] result<void> seal_output();
		/** Fail a resumed output whose shared validation did not seal. */
		[[nodiscard]] result<void> reject_output();

	  private:
		[[nodiscard]] result<void> transition(ng1_recovery_event event);
		[[nodiscard]] result<void> fail_from(ng1_recovery_event event, error original_error);

		std::optional<ng1_resume_state> resume_state_;
		ng1_recovery_state state_{ng1_recovery_state::running};
	};
} // namespace cxxlens::sdk::provider::detail
