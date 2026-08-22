#pragma once

#include <cstdint>
#include <initializer_list>
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
	 * no public capability or default clock factory while NG1 qualification remains proposed.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_driver_configuration
	{
		ng1_session_configuration session;
		process_invocation invocation;
		protocol_limits limits;
		std::uint64_t maximum_retained_frames{};
		/** Bound the resident decoded transcript bytes retained by the live driver. */
		std::uint64_t maximum_retained_bytes{64U * 1024U * 1024U};
		std::unique_ptr<ng1_monotonic_clock_port> clock;
		std::unique_ptr<ng1_host_observation_port> observation;
		std::unique_ptr<ng1_duplex_process_port> processes;
	};

	/**
	 * Source-private coordinator/transport bridge for one NG1 session.
	 *
	 * Each control occurrence is stamped by the injected host clock before entering the shared NG1
	 * adapter. Ordinary provider frames are retained under an explicit bound for a later shared
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
		[[nodiscard]] bool ended() const noexcept
		{
			return ended_;
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
								std::uint64_t maximum_retained_frames,
								std::uint64_t maximum_retained_bytes) noexcept;

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
		std::uint64_t maximum_retained_bytes_{};
		std::uint64_t retained_bytes_{};
		bool ended_{};
	};

	/**
	 * Authority status for the source-private NG1 candidate.
	 *
	 * The candidate is intentionally separate from the NG0 process runtime.  Both
	 * the source-closure registry and the NG1 hardening contract must be accepted
	 * before a caller can start it; an unaccepted status is never converted into
	 * an NG0 launch or another provider selection.
	 */
	enum class ng1_hardening_authority_status : std::uint8_t
	{
		not_accepted,
		accepted,
	};

	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_activation_authority
	{
		bool explicit_ng1_request{};
		ng1_source_closure_authority_status source_closure =
			ng1_source_closure_authority_status::not_accepted;
		ng1_hardening_authority_status hardening = ng1_hardening_authority_status::not_accepted;
	};

	/** Exact source-private phase of the negotiated NG1 candidate. */
	enum class ng1_live_candidate_phase : std::uint8_t
	{
		awaiting_hello,
		running,
		recovery,
		failed,
		completed,
	};

	/**
	 * Configuration for one explicitly requested NG1 candidate session.
	 *
	 * `host_transcript` is encoded and validated by the existing shared host
	 * transcript validator before the process is started.  This prevents the
	 * candidate from inventing a second handshake grammar or from sending a
	 * partially validated task input to a live worker.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_session_candidate_configuration
	{
		ng1_live_activation_authority authority;
		ng1_live_driver_configuration driver;
		host_transcript_request host_transcript;
	};

	/**
	 * Source-private negotiated NG1 candidate.
	 *
	 * This class wires the real duplex process port to the typed session driver:
	 * it validates the provider hello, sends the exact shared host handshake,
	 * and then exposes the existing host-receipted heartbeat, progress, spill,
	 * durable-resume, and recovery operations.  It is not part of the installed
	 * API and no production caller selects it while either authority status is
	 * proposed.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_session_candidate
	{
	  public:
		[[nodiscard]] static result<ng1_live_session_candidate>
		start(ng1_live_session_candidate_configuration configuration, std::stop_token cancellation);

		ng1_live_session_candidate(const ng1_live_session_candidate&) = delete;
		ng1_live_session_candidate& operator=(const ng1_live_session_candidate&) = delete;
		ng1_live_session_candidate(ng1_live_session_candidate&& other) noexcept;
		ng1_live_session_candidate& operator=(ng1_live_session_candidate&&) = delete;
		~ng1_live_session_candidate() noexcept = default;

		/** Receive/validate the provider hello, then send the shared host handshake. */
		[[nodiscard]] result<void> negotiate(std::stop_token cancellation);
		/** Receive one provider frame after successful negotiation. */
		[[nodiscard]] result<std::optional<ng1_live_frame_receipt>>
		receive_provider_frame(std::stop_token cancellation);
		/** Send one already validated ordinary/heartbeat host frame. */
		[[nodiscard]] result<void> send_host_frame(const frame& value);
		/** Apply the injected clock to the NG1 liveness authority. */
		[[nodiscard]] result<void> check_liveness();
		/** Append one validated output prefix record to the private spill port. */
		[[nodiscard]] result<void> append_spill(const ng1_spill_record& record);
		/** Fsync the private spill prefix before a resume token is admitted. */
		[[nodiscard]] result<ng1_spill_fsync_receipt>
		fsync_spill(std::uint64_t highest_contiguous_acked_sequence,
					std::uint64_t highest_observed_sequence,
					std::string staged_digest,
					std::uint64_t resume_generation);
		/** Admit one provider resume after the host-observed spill receipt. */
		[[nodiscard]] result<void>
		accept_provider_resume(const ng1_live_frame_receipt& receipt,
							   const ng1_spill_fsync_receipt& fsync_receipt,
							   bool open_dependency_group,
							   bool terminal);
		/** Return the exact replay start after a durable resume was admitted. */
		[[nodiscard]] result<std::uint64_t> replay_start_sequence() const;
		/** Admit the shared runtime replay receipt; raw frames are never self-authority. */
		[[nodiscard]] result<void> accept_replay(const ng1_replay_validation_receipt& receipt);
		/** Seal output only from the shared immutable transcript validation receipt. */
		[[nodiscard]] result<void> seal_output(const ng1_output_validation_receipt& receipt);
		/** Reject output and preserve a terminal fail-closed cleanup state. */
		[[nodiscard]] result<void> reject_output();
		/** Finish the current process and retain its exact lifecycle outcome. */
		[[nodiscard]] result<process_output> finish(std::stop_token cancellation);
		/** Kill/reap the current process and retain its exact lifecycle outcome. */
		[[nodiscard]] result<process_output> terminate(process_status status);
		/** Dispose the candidate spill port after a terminal lifecycle state. */
		[[nodiscard]] result<void> cleanup();

		[[nodiscard]] ng1_live_candidate_phase phase() const noexcept
		{
			return phase_;
		}
		[[nodiscard]] const ng1_session_coordinator& session() const noexcept
		{
			return driver_.session();
		}
		[[nodiscard]] std::span<const frame> provider_frames() const noexcept
		{
			return driver_.provider_frames();
		}

	  private:
		ng1_live_session_candidate(ng1_live_session_driver driver,
								   std::vector<frame> host_frames,
								   std::string provider_manifest,
								   protocol_limits limits) noexcept
			: driver_{std::move(driver)}, host_frames_{std::move(host_frames)},
			  provider_manifest_{std::move(provider_manifest)}, limits_{limits}
		{
		}

		[[nodiscard]] result<void>
		ensure_phase(std::string_view operation,
					 std::initializer_list<ng1_live_candidate_phase> allowed) const;
		[[nodiscard]] result<void> reject_candidate(std::string field, std::string detail);
		[[nodiscard]] result<void> validate_provider_hello(const frame& value) const;
		void update_phase_after_process_effect() noexcept;

		ng1_live_session_driver driver_;
		std::vector<frame> host_frames_;
		std::string provider_manifest_;
		protocol_limits limits_;
		ng1_live_candidate_phase phase_{ng1_live_candidate_phase::awaiting_hello};
	};
} // namespace cxxlens::sdk::provider::detail
