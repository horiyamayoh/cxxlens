#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "provider_ng1_recovery_internal.hpp"
#include "provider_ng1_spill_port_internal.hpp"
#include "provider_ng1_transport_internal.hpp"
#include "provider_runtime_internal.hpp"
#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	class ng1_live_session_adapter;

	/**
	 * Opaque output-validation authority supplied by the shared provider validator.
	 *
	 * A task/session coordinator may consume this value, but cannot construct it from a digest or
	 * terminal flag.  Construction requires the immutable provider transcript seal produced by the
	 * shared validation pass.  This source-private bridge therefore cannot turn progress alone into
	 * a production completion claim.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_output_validation_receipt
	{
	  public:
		[[nodiscard]] std::string_view task_id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view sealed_transcript_digest() const noexcept
		{
			return sealed_transcript_digest_;
		}

	  private:
		explicit ng1_output_validation_receipt(std::string task_id,
											   std::string sealed_transcript_digest) noexcept
			: task_id_{std::move(task_id)},
			  sealed_transcript_digest_{std::move(sealed_transcript_digest)}
		{
		}

		std::string task_id_;
		std::string sealed_transcript_digest_;

		friend result<ng1_output_validation_receipt>
		make_ng1_output_validation_receipt(std::string task_id,
										   const sealed_provider_transcript& sealed);
	};

	/** Construct output authority only from the shared immutable provider transcript seal. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_output_validation_receipt>
	make_ng1_output_validation_receipt(std::string task_id,
									   const sealed_provider_transcript& sealed);

	/**
	 * Opaque replay-validation authority supplied by the shared replay validator.
	 *
	 * The coordinator still checks the first occurrence against the durable ACK.  The remaining
	 * replay authority is retained from the shared runtime receipt so this seam cannot turn a raw
	 * caller-supplied digest or record count into a validation claim.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_replay_validation_receipt
	{
	  public:
		[[nodiscard]] std::string_view task_id() const noexcept
		{
			return task_id_;
		}
		[[nodiscard]] std::string_view sealed_transcript_digest() const noexcept
		{
			return sealed_transcript_digest_;
		}
		[[nodiscard]] std::uint64_t first_sequence() const noexcept
		{
			return runtime_receipt_.first_frame_sequence();
		}
		[[nodiscard]] std::string_view frame_transcript_digest() const noexcept
		{
			return runtime_receipt_.frame_transcript_digest();
		}
		[[nodiscard]] const provider_runtime_provenance& provenance() const noexcept
		{
			return runtime_receipt_.provenance();
		}
		[[nodiscard]] const provider_runtime_receipt& runtime_receipt() const noexcept
		{
			return runtime_receipt_;
		}

	  private:
		ng1_replay_validation_receipt(std::string task_id,
									  std::string sealed_transcript_digest,
									  provider_runtime_receipt runtime_receipt) noexcept
			: task_id_{std::move(task_id)},
			  sealed_transcript_digest_{std::move(sealed_transcript_digest)},
			  runtime_receipt_{std::move(runtime_receipt)}
		{
		}

		std::string task_id_;
		std::string sealed_transcript_digest_;
		provider_runtime_receipt runtime_receipt_;

		friend result<ng1_replay_validation_receipt>
		make_ng1_replay_validation_receipt(const ng1_output_validation_receipt& output,
										   const provider_runtime_receipt& replay_runtime);
	};

	/** Construct replay authority only from a shared runtime validation receipt and output seal. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_replay_validation_receipt>
	make_ng1_replay_validation_receipt(const ng1_output_validation_receipt& output,
									   const provider_runtime_receipt& replay_runtime);

	/**
	 * Source-private construction inputs for one NG1 task session.
	 *
	 * The three identity projections are deliberately supplied independently and checked for
	 * equality at creation. This keeps the future duplex process port from using a convenient but
	 * incomplete identity as its lifecycle authority.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_session_configuration
	{
		ng1_session_binding heartbeat_binding;
		std::string dependency_group_id;
		ng1_resume_binding resume_binding;
		ng1_spill_binding spill_binding;
		std::uint64_t host_started_at_ns{};
		std::unique_ptr<ng1_spill_storage_port> spill_storage;
	};

	/**
	 * Source-private NG1 lifecycle seam.
	 *
	 * This coordinator owns ordering between host-receipted controls, bounded spill staging, and
	 * the accepted recovery matrix. It has no process-launch, kill, restart, or capability
	 * advertisement behavior; those observations remain explicit inputs for the future live port.
	 * It is therefore safe to qualify independently without reclassifying the NG0 completed-process
	 * runtime as NG1.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_session_coordinator
	{
	  public:
		[[nodiscard]] static result<ng1_session_coordinator>
		create(ng1_session_configuration configuration);

		ng1_session_coordinator(const ng1_session_coordinator&) = delete;
		ng1_session_coordinator& operator=(const ng1_session_coordinator&) = delete;
		ng1_session_coordinator(ng1_session_coordinator&& other) noexcept;
		ng1_session_coordinator& operator=(ng1_session_coordinator&&) = delete;
		~ng1_session_coordinator() noexcept = default;

		[[nodiscard]] ng1_recovery_state state() const noexcept
		{
			return (moved_from_ || poisoned_ || spill_.poisoned()) ? ng1_recovery_state::failed
																   : recovery_.state();
		}
		[[nodiscard]] bool poisoned() const noexcept
		{
			return moved_from_ || poisoned_ || spill_.poisoned();
		}
		[[nodiscard]] bool cleaned() const noexcept
		{
			return moved_from_ || cleaned_ || spill_.cleaned();
		}
		[[nodiscard]] std::uint64_t spill_total_bytes() const noexcept
		{
			return spill_.total_bytes();
		}
		[[nodiscard]] std::uint64_t spill_total_records() const noexcept
		{
			return spill_.total_records();
		}
		/** Admit the exact provider task-accepted identity and start task timers at its receipt. */
		[[nodiscard]] result<void> observe_task_accepted(const task_accepted_metadata& metadata,
														 std::uint64_t host_receipt_time_ns);

		/** Validate and admit a host-to-provider heartbeat probe. */
		[[nodiscard]] result<void> observe_host_probe(const ng1_heartbeat_control& control,
													  std::uint64_t host_receipt_time_ns,
													  std::uint64_t highest_observed_sequence,
													  std::string_view host_staged_digest);
		/** Validate and admit a provider-to-host heartbeat acknowledgement. */
		[[nodiscard]] result<void> observe_provider_ack(const ng1_heartbeat_control& control,
														std::uint64_t host_receipt_time_ns,
														std::uint64_t highest_observed_sequence,
														std::string_view host_staged_digest);
		/** Apply the host clock to the current liveness deadline. */
		[[nodiscard]] result<void> check_liveness(std::uint64_t now_ns);
		/** Validate and admit one provider progress occurrence. */
		[[nodiscard]] result<void> observe_progress(const ng1_progress_control& control,
													std::uint64_t host_receipt_time_ns,
													bool terminal_sample = false);

		/** Append one validated output prefix record before the next durability checkpoint. */
		[[nodiscard]] result<void> append_spill(const ng1_spill_record& record);
		/** Fsync the exact prefix and retain the latest host-observed receipt. */
		[[nodiscard]] result<ng1_spill_fsync_receipt>
		fsync_spill(std::uint64_t highest_contiguous_acked_sequence,
					std::uint64_t highest_observed_sequence,
					std::string staged_digest,
					std::uint64_t resume_generation);

		/** Explicit process observations supplied by the future live process port. */
		[[nodiscard]] result<void> observe_worker_exit();
		[[nodiscard]] result<void> observe_heartbeat_timeout();
		[[nodiscard]] result<void> observe_progress_rate_failure();
		[[nodiscard]] result<void> confirm_worker_kill();
		/**
		 * Close a session whose worker was never created because the process port rejected launch.
		 * This marks the private spill transaction failed without inventing a worker-exit event;
		 * the caller must still invoke cleanup() before the coordinator is destroyed.
		 */
		void fail_before_worker_start() noexcept;

		/** Admit a resume only after local spill bytes and the host receipt agree. */
		[[nodiscard]] result<void> accept_durable_resume(const ng1_resume_control& control,
														 const ng1_spill_fsync_receipt& receipt,
														 bool open_dependency_group,
														 bool terminal,
														 std::uint64_t highest_observed_sequence);
		/**
		 * Rehydrate a fresh coordinator's spill prefix before accepting a durable resume.
		 *
		 * The host must separately observe worker termination first.  This source-private seam
		 * does not launch, kill, restart, or advertise an NG1 provider capability.
		 */
		[[nodiscard]] result<void> restore_durable_resume(const ng1_resume_control& control,
														  const ng1_spill_fsync_receipt& receipt,
														  bool open_dependency_group,
														  bool terminal,
														  std::uint64_t highest_observed_sequence);
		[[nodiscard]] result<std::uint64_t> replay_start_sequence() const;
		[[nodiscard]] result<void>
		accept_replay(const ng1_replay_validation_receipt& replay_receipt);

		/** Seal output only after complete progress, accepted state, and shared output validation.
		 */
		[[nodiscard]] result<void> seal_output(const ng1_output_validation_receipt& output_receipt);
		[[nodiscard]] result<void> reject_output();
		/** Cleanup is explicit; an unknown cleanup effect remains terminal. */
		[[nodiscard]] result<void> cleanup();

	  private:
		explicit ng1_session_coordinator(std::string task_id,
										 ng1_resume_binding resume_binding,
										 ng1_heartbeat_state heartbeat,
										 ng1_progress_state progress,
										 ng1_recovery_adapter recovery,
										 ng1_spill_staging_session spill) noexcept
			: task_id_{std::move(task_id)}, resume_binding_{std::move(resume_binding)},
			  heartbeat_{std::move(heartbeat)}, progress_{std::move(progress)},
			  recovery_{std::move(recovery)}, spill_{std::move(spill)}
		{
		}

		[[nodiscard]] result<void> reject_heartbeat(error original_error);
		[[nodiscard]] result<void> reject_progress(error original_error);
		[[nodiscard]] result<void> reject_resume(error original_error);
		[[nodiscard]] result<void> reject_replay(error original_error);
		[[nodiscard]] result<void> reject_output(error original_error);
		[[nodiscard]] result<void> observe_heartbeat(const ng1_heartbeat_control& control,
													 ng1_heartbeat_kind expected_kind,
													 std::uint64_t host_receipt_time_ns,
													 std::uint64_t highest_observed_sequence,
													 std::string_view host_staged_digest);
		[[nodiscard]] result<void> ensure_open(std::string_view operation) const;
		[[nodiscard]] result<void> admit_host_receipt(std::uint64_t host_receipt_time_ns) const;
		[[nodiscard]] result<void> poison(error original_error);

		std::string task_id_;
		ng1_resume_binding resume_binding_;
		ng1_heartbeat_state heartbeat_;
		ng1_progress_state progress_;
		ng1_recovery_adapter recovery_;
		ng1_spill_staging_session spill_;
		std::optional<ng1_spill_fsync_receipt> latest_fsync_receipt_;
		std::optional<std::uint64_t> last_host_receipt_time_ns_;
		std::optional<std::string> replay_output_digest_;
		std::optional<std::string> replay_frame_transcript_digest_;
		bool task_accepted_{};
		bool progress_terminal_{};
		bool poisoned_{};
		bool cleaned_{};
		bool moved_from_{};

		friend class ng1_live_session_adapter;
	};

	/**
	 * Source-private bridge from decoded live wire frames to the NG1 session coordinator.
	 *
	 * The adapter deliberately does not launch a process, own a pipe, or advertise NG1. It only
	 * binds the typed control codecs to host-receipted coordinator observations. Ordinary output
	 * frames return `false` so the caller can still pass them through the shared transcript
	 * validator. A resume acceptance requires the independently host-observed spill receipt and is
	 * therefore exposed as a separate operation rather than being inferred from a frame alone.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_live_session_adapter
	{
	  public:
		explicit ng1_live_session_adapter(ng1_session_coordinator& session) noexcept
			: session_{&session}
		{
		}

		/** Observe one provider-to-host frame; return true for an admitted NG1 control frame. */
		[[nodiscard]] result<bool> observe_provider_frame(const frame& value,
														  std::uint64_t host_receipt_time_ns,
														  std::uint64_t highest_observed_sequence,
														  std::string_view host_staged_digest,
														  bool terminal_progress_sample = false);

		/** Observe one host-to-provider frame; return true for an admitted NG1 control frame. */
		[[nodiscard]] result<bool> observe_host_frame(const frame& value,
													  std::uint64_t host_receipt_time_ns,
													  std::uint64_t highest_observed_sequence,
													  std::string_view host_staged_digest);

		/**
		 * Admit a provider resume response only with the latest host-observed durable receipt. The
		 * frame's control is decoded and identity-checked before the coordinator is mutated.
		 */
		[[nodiscard]] result<void>
		accept_provider_resume_frame(const frame& value,
									 std::uint64_t host_receipt_time_ns,
									 const ng1_spill_fsync_receipt& receipt,
									 bool open_dependency_group,
									 bool terminal,
									 std::uint64_t highest_observed_sequence);

	  private:
		[[nodiscard]] result<void> validate_frame_header(const frame& value) const;
		[[nodiscard]] result<void> reject_heartbeat(error original_error);
		[[nodiscard]] result<void> reject_progress(error original_error);
		[[nodiscard]] result<void> reject_resume(error original_error);

		ng1_session_coordinator* session_{};
	};
} // namespace cxxlens::sdk::provider::detail
