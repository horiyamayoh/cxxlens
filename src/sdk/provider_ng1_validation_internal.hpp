#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/provider.hpp>

#include "provider_visibility_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	/** Source-private identity shared by the NG1 heartbeat directions. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_session_binding
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string protocol_session_id;
		std::string task_id;
		std::uint64_t stream_id{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const ng1_session_binding&) const = default;
	};

	enum class ng1_heartbeat_kind : std::uint8_t
	{
		probe,
		ack,
	};

	/** One already-decoded NG1 heartbeat occurrence with its host receipt time. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_heartbeat_sample
	{
		std::string schema{"cxxlens.provider-control.heartbeat.v1"};
		ng1_session_binding binding;
		ng1_heartbeat_kind kind{ng1_heartbeat_kind::probe};
		std::uint64_t heartbeat_sequence{};
		std::uint64_t provider_monotonic_time_ns{};
		std::uint64_t host_receipt_time_ns{};
		std::uint64_t highest_contiguous_acked_sequence{};
		std::string staged_digest;
	};

	/**
	 * Private NG1 heartbeat validator. It is deliberately not wired to the NG0 process
	 * runtime: callers must provide the host monotonic receipt clock and the observed
	 * output sequence explicitly.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_heartbeat_state
	{
	  public:
		[[nodiscard]] static result<ng1_heartbeat_state> create(ng1_session_binding binding,
																std::uint64_t started_at_ns);

		[[nodiscard]] result<void> accept(const ng1_heartbeat_sample& sample,
										  std::uint64_t highest_observed_sequence,
										  std::string_view host_observed_staged_digest);
		/** Rebase the lifecycle clock at the validated task-accepted receipt. */
		void rebase_start(std::uint64_t started_at_ns) noexcept;
		[[nodiscard]] result<void> check_liveness(std::uint64_t now_ns) const;
		[[nodiscard]] result<void> mark_terminal() noexcept;

		[[nodiscard]] bool terminal() const noexcept
		{
			return terminal_;
		}

	  private:
		ng1_session_binding binding_;
		std::uint64_t started_at_ns_{};
		std::optional<std::uint64_t> last_probe_sequence_;
		std::optional<std::uint64_t> last_ack_sequence_;
		std::optional<std::uint64_t> last_provider_time_ns_;
		std::optional<std::uint64_t> last_probe_host_receipt_ns_;
		std::optional<std::uint64_t> last_host_receipt_time_ns_;
		std::optional<std::uint64_t> last_valid_ack_received_ns_;
		bool terminal_{};
	};

	/** One already-decoded NG1 progress occurrence plus the host receipt time. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_progress_sample
	{
		std::string schema{"cxxlens.provider-control.progress.v2"};
		std::string task_id;
		std::string dependency_group_id;
		std::uint64_t progress_sequence{};
		std::uint64_t provider_monotonic_time_ns{};
		std::uint64_t host_receipt_time_ns{};
		std::uint64_t completed_units{};
		std::uint64_t total_units{};
	};

	/**
	 * Private NG1 progress/rate validator. Host receipt time is the only rate
	 * authority; provider timestamps are retained only for ordering checks.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_progress_state
	{
	  public:
		[[nodiscard]] static result<ng1_progress_state>
		create(std::string task_id, std::string dependency_group_id, std::uint64_t started_at_ns);

		[[nodiscard]] result<void> observe(const ng1_progress_sample& sample,
										   bool terminal_sample = false);
		/** Rebase the lifecycle clock at the validated task-accepted receipt. */
		void rebase_start(std::uint64_t started_at_ns) noexcept;
		[[nodiscard]] result<void> finish() const;

	  private:
		std::string task_id_;
		std::string dependency_group_id_;
		std::uint64_t started_at_ns_{};
		std::optional<std::uint64_t> last_sequence_;
		std::optional<std::uint64_t> last_provider_time_ns_;
		std::optional<std::uint64_t> last_host_receipt_time_ns_;
		std::optional<std::uint64_t> last_completed_units_;
		std::optional<std::uint64_t> rate_checkpoint_receipt_ns_;
		std::optional<std::uint64_t> rate_checkpoint_completed_units_;
		std::optional<std::uint64_t> total_units_;
		bool terminal_observed_{};
	};

	enum class ng1_resume_kind : std::uint8_t
	{
		request,
		accepted,
		rejected,
	};

	/** Exact NG1 resume-token projection excluding token_digest. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_resume_binding
	{
		std::string provider_id;
		semantic_version provider_version;
		std::string provider_binary_digest;
		std::string provider_semantic_contract_digest;
		std::string protocol_session_id;
		std::string task_id;
		std::string task_input_digest;
		std::string normalized_invocation_digest;
		std::string toolchain_digest;
		std::string environment_digest;
		std::string sandbox_policy_digest;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::uint64_t stream_id{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const ng1_resume_binding&) const = default;
	};

	/** Source-private typed projection of the NG1 resume control. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_resume_token
	{
		std::string schema{"cxxlens.provider-control.resume.v2"};
		ng1_resume_kind kind{ng1_resume_kind::request};
		ng1_resume_binding binding;
		std::uint64_t highest_contiguous_acked_sequence{};
		std::string staged_digest;
		std::uint64_t token_generation{};
		std::string token_digest;
	};

	/** Derive the exact canonical projection digest for one resume token. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	ng1_resume_token_digest(const ng1_resume_token& token);

	/** Host-observed fsync receipt required before a resume token is authoritative. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_fsync_receipt
	{
		std::string schema{"cxxlens.provider-spill-fsync-receipt.v1"};
		std::string provider_id;
		std::string protocol_session_id;
		std::string task_id;
		std::uint64_t stream_id{};
		std::uint64_t highest_contiguous_acked_sequence{};
		std::string staged_digest;
		std::string spill_digest;
		std::uint64_t total_bytes{};
		std::uint64_t total_records{};
		std::uint64_t fsync_sequence{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const ng1_spill_fsync_receipt&) const = default;
	};

	/**
	 * Source-private durable frontier for the latest accepted resume checkpoint.
	 *
	 * The receipt proves the exact spill prefix; the generation proves which resume
	 * publication is latest. Keeping both values in the port-owned frontier prevents
	 * a fresh coordinator from treating a matching but older receipt as current.
	 */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_resume_frontier
	{
		ng1_spill_fsync_receipt receipt;
		std::uint64_t resume_generation{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const ng1_spill_resume_frontier&) const = default;
	};

	/** Exact identity binding for one source-private staged spill prefix. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_binding
	{
		std::string provider_id;
		std::string protocol_session_id;
		std::string task_id;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::uint64_t stream_id{};

		[[nodiscard]] result<void> validate() const;
		[[nodiscard]] bool operator==(const ng1_spill_binding&) const = default;
	};

	/** One decoded, source-private NG1 spill record before prefix admission. */
	struct CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_record
	{
		std::string schema{"cxxlens.provider-spill-record.v1"};
		std::uint64_t record_ordinal{};
		std::string task_id;
		std::string dependency_group_id;
		std::string atomic_output_group_id;
		std::string batch_id;
		std::uint64_t stream_id{};
		std::uint64_t sequence{};
		std::vector<std::byte> payload_bytes;
		std::string payload_digest;
		std::string record_digest;

		[[nodiscard]] bool operator==(const ng1_spill_record&) const = default;
	};

	inline constexpr std::uint64_t ng1_spill_maximum_record_bytes =
		std::uint64_t{16U} * 1024U * 1024U;
	inline constexpr std::uint64_t ng1_spill_maximum_total_bytes =
		std::uint64_t{64U} * 1024U * 1024U;
	inline constexpr std::uint64_t ng1_spill_maximum_records = 65'536U;

	/** Digest the exact payload bytes in the source-private spill payload domain. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	ng1_spill_payload_digest(std::span<const std::byte> payload);
	/** Digest the exact record projection excluding record_digest. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<std::string>
	ng1_spill_record_digest(const ng1_spill_record& record);

	/**
	 * Source-private bounded spill-prefix integrity core.
	 *
	 * This value-only component does not allocate payloads from an unvalidated wire header and
	 * does not perform fsync. A caller may convert an independently host-observed private-port
	 * fsync sequence into a receipt with observe_host_fsync(); that method is not a durability
	 * proof by itself and is intentionally not connected to production runtime or capability
	 * advertisement.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_spill_prefix_state
	{
	  public:
		[[nodiscard]] static result<ng1_spill_prefix_state> create(ng1_spill_binding binding);

		[[nodiscard]] result<void> append(const ng1_spill_record& record);
		/** Reject an ACK unless its sequence is represented by this exact spill prefix. */
		[[nodiscard]] result<void>
		validate_ack_frontier(std::uint64_t highest_contiguous_acked_sequence) const;
		[[nodiscard]] result<std::string> spill_digest() const;
		[[nodiscard]] result<ng1_spill_fsync_receipt>
		observe_host_fsync(std::uint64_t highest_contiguous_acked_sequence,
						   std::string staged_digest,
						   std::uint64_t fsync_sequence) const;

		[[nodiscard]] std::uint64_t total_bytes() const noexcept
		{
			return total_bytes_;
		}
		[[nodiscard]] std::uint64_t total_records() const noexcept
		{
			return static_cast<std::uint64_t>(record_digests_.size());
		}

	  private:
		ng1_spill_binding binding_;
		std::vector<std::string> record_digests_;
		std::uint64_t total_bytes_{};
		std::uint64_t next_record_ordinal_{};
		std::uint64_t next_sequence_{};
	};

	/**
	 * Validate durable, identity-bound, strictly increasing resume tokens. The
	 * durability and open-group flags are host observations, never token claims.
	 */
	class CXXLENS_PROVIDER_DETAIL_HIDDEN ng1_resume_state
	{
	  public:
		[[nodiscard]] static result<ng1_resume_state> create(ng1_resume_binding binding);

		[[nodiscard]] result<void> accept(const ng1_resume_token& token,
										  const ng1_spill_fsync_receipt& receipt,
										  bool open_dependency_group,
										  bool terminal,
										  std::uint64_t highest_observed_sequence);
		[[nodiscard]] result<std::uint64_t> replay_start_sequence() const;

	  private:
		ng1_resume_binding binding_;
		std::optional<std::uint64_t> last_generation_;
		std::uint64_t last_acked_sequence_{};
		std::optional<std::uint64_t> last_fsync_sequence_;
		std::optional<ng1_spill_fsync_receipt> last_receipt_;
		bool accepted_{};
	};

	enum class ng1_recovery_state : std::uint8_t
	{
		running,
		heartbeat_timeout,
		progress_rate_failure,
		cancel_requested,
		worker_killed,
		resume_replay,
		resumed,
		completed,
		failed,
	};

	enum class ng1_recovery_event : std::uint8_t
	{
		heartbeat_timeout,
		progress_rate_failure,
		cancel_requested,
		worker_exit,
		output_sealed,
		invalid_heartbeat_clock,
		worker_kill_confirmed,
		cancel_acknowledged,
		cancel_timeout,
		durable_token_valid,
		durable_token_invalid,
		replay_valid,
		replay_invalid,
		output_invalid,
	};

	/** Apply one exact NG1 recovery transition; absent matrix entries fail closed. */
	[[nodiscard]] CXXLENS_PROVIDER_DETAIL_HIDDEN result<ng1_recovery_state>
	ng1_recovery_transition(ng1_recovery_state state, ng1_recovery_event event);
} // namespace cxxlens::sdk::provider::detail
