#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "provider_ng1_process_internal.hpp"
#include "provider_ng1_session_internal.hpp"
#include "provider_ng1_transport_internal.hpp"
#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/**
	 * Source-private typed NG1 control handoff for an integrator that owns process effects.
	 *
	 * This seam validates host-receipted heartbeat/progress, durable spill/resume frontiers, replay
	 * boundaries, and crash/hang/cancellation transitions without launching a worker or publishing
	 * provider output. The integrating runtime remains responsible for wire/process effects and for
	 * shared transcript/output certification.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_control_handoff
	{
	  public:
		[[nodiscard]] static result<ng1_live_control_handoff>
		create(ng1_session_configuration configuration);

		ng1_live_control_handoff(const ng1_live_control_handoff&) = delete;
		ng1_live_control_handoff& operator=(const ng1_live_control_handoff&) = delete;
		ng1_live_control_handoff(ng1_live_control_handoff&& other) noexcept;
		ng1_live_control_handoff& operator=(ng1_live_control_handoff&&) = delete;
		~ng1_live_control_handoff() noexcept = default;

		[[nodiscard]] ng1_recovery_state state() const noexcept
		{
			return session_.state();
		}
		[[nodiscard]] bool poisoned() const noexcept
		{
			return session_.poisoned();
		}
		[[nodiscard]] bool cleaned() const noexcept
		{
			return session_.cleaned();
		}

		/** Admit typed controls after the host has captured the ingress receipt. */
		[[nodiscard]] result<void> observe_task_accepted(const task_accepted_metadata& metadata,
														 std::uint64_t host_receipt_time_ns);
		[[nodiscard]] result<void> observe_host_probe(const ng1_heartbeat_control& control,
													  std::uint64_t host_receipt_time_ns,
													  std::uint64_t highest_observed_sequence,
													  std::string_view host_staged_digest);
		[[nodiscard]] result<void> observe_provider_ack(const ng1_heartbeat_control& control,
														std::uint64_t host_receipt_time_ns,
														std::uint64_t highest_observed_sequence,
														std::string_view host_staged_digest);
		[[nodiscard]] result<void> observe_progress(const ng1_progress_control& control,
													std::uint64_t host_receipt_time_ns,
													bool terminal_sample = false);
		[[nodiscard]] result<void> check_liveness(std::uint64_t now_ns);

		/** Stage and durably checkpoint the exact spill prefix. */
		[[nodiscard]] result<void> append_spill(const ng1_spill_record& record);
		[[nodiscard]] result<ng1_spill_fsync_receipt>
		fsync_spill(std::uint64_t highest_contiguous_acked_sequence,
					std::uint64_t highest_observed_sequence,
					std::string staged_digest,
					std::uint64_t resume_generation);

		/** Record worker/cancellation outcomes in the explicit recovery matrix. */
		[[nodiscard]] result<void> observe_worker_exit();
		[[nodiscard]] result<void> observe_heartbeat_timeout();
		[[nodiscard]] result<void> observe_progress_rate_failure();
		[[nodiscard]] result<void> request_cancel();
		[[nodiscard]] result<void> acknowledge_cancel();
		[[nodiscard]] result<void> timeout_cancel();
		[[nodiscard]] result<void> confirm_worker_kill();

		/** Rehydrate and accept a durable frontier after worker termination. */
		[[nodiscard]] result<void> accept_durable_resume(const ng1_resume_control& control,
														 const ng1_spill_fsync_receipt& receipt,
														 bool open_dependency_group,
														 bool terminal,
														 std::uint64_t highest_observed_sequence);
		[[nodiscard]] result<void> restore_durable_resume(const ng1_resume_control& control,
														  const ng1_spill_fsync_receipt& receipt,
														  bool open_dependency_group,
														  bool terminal,
														  std::uint64_t highest_observed_sequence);
		[[nodiscard]] result<std::uint64_t> replay_start_sequence() const;
		/** Admit only the exact first sequence; output validation remains external. */
		[[nodiscard]] result<void> accept_replay_frontier(std::uint64_t first_sequence);

		/** Reject output when external validation fails, then permit spill cleanup. */
		[[nodiscard]] result<void> reject_output();
		[[nodiscard]] result<void> cleanup();

	  private:
		explicit ng1_live_control_handoff(ng1_session_coordinator session) noexcept
			: session_{std::move(session)}
		{
		}

		ng1_session_coordinator session_;
	};

	/** Host-owned monotonic receipt source required by the NG1 lifecycle authority. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_monotonic_clock_port
	{
	  public:
		virtual ~ng1_monotonic_clock_port() = default;
		[[nodiscard]] virtual result<std::uint64_t> now_ns() const = 0;
	};

	/** Host observation supplied alongside each NG1 control receipt. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_host_observation
	{
		std::uint64_t highest_observed_sequence{};
		std::string staged_digest;
	};

	/**
	 * Port for the current durable host observation. The driver never infers this state from a
	 * provider frame or reconstructs it from an opaque digest.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_host_observation_port
	{
	  public:
		virtual ~ng1_host_observation_port() = default;
		[[nodiscard]] virtual result<ng1_host_observation> current() const = 0;
	};

	/** One provider frame plus the host receipt and observation captured at ingress. */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_frame_receipt
	{
	  public:
		[[nodiscard]] const frame& value() const noexcept
		{
			return value_;
		}
		[[nodiscard]] std::uint64_t host_receipt_time_ns() const noexcept
		{
			return host_receipt_time_ns_;
		}
		[[nodiscard]] std::uint64_t highest_observed_sequence() const noexcept
		{
			return highest_observed_sequence_;
		}
		[[nodiscard]] std::string_view host_staged_digest() const noexcept
		{
			return host_staged_digest_;
		}
		[[nodiscard]] bool ng1_control_admitted() const noexcept
		{
			return ng1_control_admitted_;
		}

	  private:
		ng1_live_frame_receipt(frame value,
							   std::uint64_t host_receipt_time_ns,
							   std::uint64_t highest_observed_sequence,
							   std::string host_staged_digest,
							   bool ng1_control_admitted) noexcept
			: value_{std::move(value)}, host_receipt_time_ns_{host_receipt_time_ns},
			  highest_observed_sequence_{highest_observed_sequence},
			  host_staged_digest_{std::move(host_staged_digest)},
			  ng1_control_admitted_{ng1_control_admitted}
		{
		}

		frame value_;
		std::uint64_t host_receipt_time_ns_{};
		std::uint64_t highest_observed_sequence_{};
		std::string host_staged_digest_;
		bool ng1_control_admitted_{};

		friend class ng1_live_session_driver;
	};

	/**
	 * Construction inputs for the step-driven NG1 live session seam.
	 *
	 * The clock and observation ports are mandatory. This keeps wall-clock time, provider-reported
	 * progress, and opaque digest strings outside lifecycle authority. The driver deliberately has
	 * no public capability or default clock factory until the NG1 capability has direct
	 * positive/negative/fault coverage.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_driver_configuration
	{
		ng1_session_configuration session;
		process_invocation invocation;
		protocol_limits limits;
		std::uint64_t maximum_retained_frames{};
		std::unique_ptr<ng1_monotonic_clock_port> clock;
		std::unique_ptr<ng1_host_observation_port> observation;
		std::unique_ptr<ng1_duplex_process_port> processes;
	};

	/**
	 * Source-private coordinator/transport bridge for one NG1 session.
	 *
	 * Each control occurrence is stamped by the injected host clock before entering the shared NG1
	 * adapter. Ordinary provider frames are retained under an explicit bound for a later direct
	 * transcript validation pass; this class never treats a control, process exit, or provider
	 * claim as sealed output authority.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_session_driver
	{
	  public:
		[[nodiscard]] static result<ng1_live_session_driver>
		start(ng1_live_driver_configuration configuration, std::stop_token cancellation);

		ng1_live_session_driver(const ng1_live_session_driver&) = delete;
		ng1_live_session_driver& operator=(const ng1_live_session_driver&) = delete;
		ng1_live_session_driver(ng1_live_session_driver&& other) noexcept;
		ng1_live_session_driver& operator=(ng1_live_session_driver&&) = delete;
		~ng1_live_session_driver() noexcept = default;

		/** Stamp and validate an NG1 host control before sending it to the provider. */
		[[nodiscard]] result<void> send_host_frame(const frame& value);
		/** Receive and stamp one provider frame; nullopt denotes orderly EOF. */
		[[nodiscard]] result<std::optional<ng1_live_frame_receipt>>
		receive_provider_frame(std::stop_token cancellation);
		/** Apply the injected host clock to the current liveness deadline. */
		[[nodiscard]] result<void> check_liveness();
		/** Admit a provider resume only with the captured host receipt and durable fsync receipt.
		 */
		[[nodiscard]] result<void>
		accept_provider_resume(const ng1_live_frame_receipt& receipt,
							   const ng1_spill_fsync_receipt& fsync_receipt,
							   bool open_dependency_group,
							   bool terminal);
		/** Close the live channel and return the exact process outcome. */
		[[nodiscard]] result<process_output> finish(std::stop_token cancellation);
		/** Kill the process group and return the exact bounded cleanup outcome. */
		[[nodiscard]] result<process_output> terminate(process_status status);
		/** Cleanup the durable spill port after the process and recovery state are terminal. */
		[[nodiscard]] result<void> cleanup();

		[[nodiscard]] ng1_session_coordinator& session() noexcept
		{
			return session_;
		}
		[[nodiscard]] const ng1_session_coordinator& session() const noexcept
		{
			return session_;
		}
		[[nodiscard]] std::span<const frame> provider_frames() const noexcept
		{
			return provider_frames_;
		}

	  private:
		ng1_live_session_driver(ng1_session_coordinator session,
								std::unique_ptr<ng1_duplex_process> process,
								std::unique_ptr<ng1_monotonic_clock_port> clock,
								std::unique_ptr<ng1_host_observation_port> observation,
								std::uint64_t maximum_retained_frames) noexcept;

		[[nodiscard]] result<void> ensure_open(std::string_view operation) const;
		/**
		 * Synchronize a successfully completed process-port effect with the recovery matrix.
		 *
		 * A clean process completion leaves a running session available for the shared
		 * `output-sealed` transition; it is not itself a worker lifecycle event.
		 * A running worker is recorded as `worker_exit`; a completed kill/reap after a heartbeat or
		 * progress failure is recorded as `worker-kill-confirmed`. Other states already own their
		 * transition and must not receive a synthetic lifecycle event.
		 */
		[[nodiscard]] result<void> synchronize_process_outcome(const process_output& output);
		[[nodiscard]] result<ng1_live_frame_receipt> stamp_provider_frame(frame value);
		[[nodiscard]] result<ng1_host_observation> current_observation() const;
		[[nodiscard]] result<std::uint64_t> now_ns() const;

		ng1_session_coordinator session_;
		ng1_live_session_adapter adapter_;
		std::unique_ptr<ng1_duplex_process> process_;
		std::unique_ptr<ng1_monotonic_clock_port> clock_;
		std::unique_ptr<ng1_host_observation_port> observation_;
		std::vector<frame> provider_frames_;
		std::optional<ng1_live_frame_receipt> last_provider_receipt_;
		std::uint64_t maximum_retained_frames_{};
		bool ended_{};
	};
} // namespace cxxlens::sdk::provider::detail
