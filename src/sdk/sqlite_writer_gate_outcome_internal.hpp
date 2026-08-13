#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "sqlite_same_process_shm_mapping_lease_internal.hpp"

namespace cxxlens::sdk
{
	/** Closed six-stage profile used by the internal current-v3 writer gate evidence path. */
	enum class sqlite_shm_writer_gate_stage : std::uint8_t
	{
		writer_readwrite_mode = 1U,
		runtime_version_and_locator,
		runtime_vfs_file_family_and_open_epoch,
		synchronous_full_and_wal_mode,
		current_v3_format_schema_head_counter_authority,
		store_writer_open_before_publication_effect,
	};

	enum class sqlite_shm_writer_gate_stage_result : std::uint8_t
	{
		passed = 1U,
		typed_determinate_failure,
		terminal_indeterminate,
	};

	/** The only determinate failure paired with each ordered gate stage. */
	enum class sqlite_shm_writer_gate_failure : std::uint8_t
	{
		writer_mode_rejected = 1U,
		runtime_or_locator_rejected,
		runtime_vfs_or_file_family_rejected,
		synchronous_or_wal_mode_rejected,
		current_v3_authority_rejected,
		store_writer_open_rejected,
	};

	enum class sqlite_shm_writer_gate_terminal_reason : std::uint8_t
	{
		timeout = 1U,
		unknown,
		open_epoch_drift,
		validator_sealed_attempt_abandonment,
		authority_loss,
		incomplete_or_unclassified_effect,
	};

	enum class sqlite_shm_writer_gate_terminal_phase : std::uint8_t
	{
		before_value_read = 1U,
		after_value_before_effect,
		after_effect_start_before_effect_result,
		after_effect_before_stage_result,
	};

	enum class sqlite_shm_writer_gate_observed_effect_slot : std::uint8_t
	{
		not_executed = 1U,
		started_outcome_unresolved,
		exact_present,
	};

	enum class sqlite_shm_writer_gate_reservation_phase : std::uint8_t
	{
		reserved = 1U,
		claimed_inflight,
		consumed_to_present,
		revoked,
		quarantined,
	};

	enum class sqlite_shm_writer_gate_observed_attachment_state : std::uint8_t
	{
		absent = 1U,
		present,
	};

	enum class sqlite_shm_writer_gate_outcome_kind : std::uint8_t
	{
		positive_success = 1U,
		typed_determinate_failure,
		terminal_indeterminate,
	};

	enum class sqlite_shm_writer_gate_attempt_lifecycle : std::uint8_t
	{
		open = 1U,
		issuer_sealed_kind,
		transferred_to_registry,
	};

	/** Exact source-private identity bundle for one non-reusable gate attempt. */
	struct sqlite_shm_writer_gate_attempt_binding
	{
		sqlite_shm_lease_family_binding family;
		sqlite_backend_opaque_identity alias_lifetime;
		sqlite_backend_opaque_identity connection_token;
		sqlite_backend_opaque_identity main_native_file_receipt;
		sqlite_backend_opaque_identity main_xopen_receipt;
		sqlite_backend_opaque_identity open_epoch;
		sqlite_backend_opaque_identity callback_cohort;
		sqlite_backend_opaque_identity expected_attachment_epoch;
		std::uint64_t issuer_control_epoch{};
		sqlite_backend_opaque_identity attempt_token;
		sqlite_shm_writer_gate_reservation_phase expected_reservation_phase{
			sqlite_shm_writer_gate_reservation_phase::reserved};
		sqlite_shm_writer_gate_observed_attachment_state observed_attachment_state{
			sqlite_shm_writer_gate_observed_attachment_state::absent};
		std::string gate_profile_id;
		std::vector<std::byte> policy_profile_bytes;
		std::string policy_profile_digest;

		[[nodiscard]] bool
		operator==(const sqlite_shm_writer_gate_attempt_binding&) const = default;
	};

	/** One complete stage bundle, or the phase-tagged terminal bundle at the first non-pass stage.
	 */
	struct sqlite_shm_writer_gate_stage_bundle
	{
		sqlite_shm_writer_gate_stage stage{sqlite_shm_writer_gate_stage::writer_readwrite_mode};
		sqlite_shm_writer_gate_stage_result result{sqlite_shm_writer_gate_stage_result::passed};
		std::optional<sqlite_shm_writer_gate_failure> failure;
		std::vector<std::byte> expected_policy_canonical_bytes;
		std::vector<std::byte> allowed_effect_canonical_bytes;
		std::vector<std::byte> observed_value_canonical_bytes;
		std::vector<std::byte> authority_receipt_canonical_bytes;
		std::vector<std::byte> observed_effect_canonical_bytes;
		sqlite_shm_writer_gate_observed_effect_slot observed_effect_slot{
			sqlite_shm_writer_gate_observed_effect_slot::exact_present};

		[[nodiscard]] bool operator==(const sqlite_shm_writer_gate_stage_bundle&) const = default;
	};

	/** Phase/tag pair for terminal indeterminate evidence. */
	struct sqlite_shm_writer_gate_terminal_locus
	{
		std::optional<sqlite_shm_writer_gate_stage> stage;
		sqlite_shm_writer_gate_terminal_phase phase{
			sqlite_shm_writer_gate_terminal_phase::before_value_read};

		[[nodiscard]] bool operator==(const sqlite_shm_writer_gate_terminal_locus&) const = default;
	};

	class sqlite_shm_writer_gate_outcome
	{
	  public:
		sqlite_shm_writer_gate_outcome(const sqlite_shm_writer_gate_outcome&) = delete;
		sqlite_shm_writer_gate_outcome& operator=(const sqlite_shm_writer_gate_outcome&) = delete;
		sqlite_shm_writer_gate_outcome(sqlite_shm_writer_gate_outcome&&) noexcept = default;
		sqlite_shm_writer_gate_outcome& operator=(sqlite_shm_writer_gate_outcome&&) = delete;
		~sqlite_shm_writer_gate_outcome() noexcept = default;

		[[nodiscard]] sqlite_shm_writer_gate_outcome_kind kind() const noexcept;
		[[nodiscard]] const sqlite_shm_writer_gate_attempt_binding& binding() const noexcept;
		[[nodiscard]] std::span<const sqlite_shm_writer_gate_stage_bundle> stages() const noexcept;
		[[nodiscard]] const std::optional<sqlite_shm_writer_gate_failure>& failure() const noexcept;
		[[nodiscard]] const std::optional<sqlite_shm_writer_gate_terminal_reason>&
		terminal_reason() const noexcept;
		[[nodiscard]] const std::optional<sqlite_shm_writer_gate_terminal_locus>&
		terminal_locus() const noexcept;

	  private:
		sqlite_shm_writer_gate_outcome(
			sqlite_shm_writer_gate_outcome_kind kind,
			sqlite_shm_writer_gate_attempt_binding binding,
			std::vector<sqlite_shm_writer_gate_stage_bundle> stages,
			std::optional<sqlite_shm_writer_gate_failure> failure,
			std::optional<sqlite_shm_writer_gate_terminal_reason> terminal_reason,
			std::optional<sqlite_shm_writer_gate_terminal_locus> terminal_locus);

		sqlite_shm_writer_gate_outcome_kind kind_{
			sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate};
		sqlite_shm_writer_gate_attempt_binding binding_;
		std::vector<sqlite_shm_writer_gate_stage_bundle> stages_;
		std::optional<sqlite_shm_writer_gate_failure> failure_;
		std::optional<sqlite_shm_writer_gate_terminal_reason> terminal_reason_;
		std::optional<sqlite_shm_writer_gate_terminal_locus> terminal_locus_;

		friend class sqlite_same_process_shm_writer_gate_receipt_validator;
	};

	/** The caller-owned, exact one-shot owner minted before gate evaluation. */
	class sqlite_shm_writer_gate_attempt_owner
	{
	  public:
		sqlite_shm_writer_gate_attempt_owner(const sqlite_shm_writer_gate_attempt_owner&) = delete;
		sqlite_shm_writer_gate_attempt_owner&
		operator=(const sqlite_shm_writer_gate_attempt_owner&) = delete;
		sqlite_shm_writer_gate_attempt_owner(sqlite_shm_writer_gate_attempt_owner&&) noexcept;
		sqlite_shm_writer_gate_attempt_owner&
		operator=(sqlite_shm_writer_gate_attempt_owner&&) = delete;
		~sqlite_shm_writer_gate_attempt_owner() noexcept = default;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_shm_writer_gate_attempt_lifecycle lifecycle() const noexcept;
		[[nodiscard]] const sqlite_shm_writer_gate_attempt_binding& binding() const noexcept;

	  private:
		explicit sqlite_shm_writer_gate_attempt_owner(
			sqlite_shm_writer_gate_attempt_binding binding);

		sqlite_shm_writer_gate_attempt_binding binding_;
		sqlite_shm_writer_gate_attempt_lifecycle lifecycle_{
			sqlite_shm_writer_gate_attempt_lifecycle::open};
		bool valid_{true};

		friend class sqlite_same_process_shm_writer_gate_receipt_validator;
	};

	/** Move-only issuer-sealed owner. It carries evidence but no native cleanup authority. */
	class sqlite_shm_writer_gate_issuer_sealed_owner
	{
	  public:
		sqlite_shm_writer_gate_issuer_sealed_owner(
			const sqlite_shm_writer_gate_issuer_sealed_owner&) = delete;
		sqlite_shm_writer_gate_issuer_sealed_owner&
		operator=(const sqlite_shm_writer_gate_issuer_sealed_owner&) = delete;
		sqlite_shm_writer_gate_issuer_sealed_owner(
			sqlite_shm_writer_gate_issuer_sealed_owner&&) noexcept;
		sqlite_shm_writer_gate_issuer_sealed_owner&
		operator=(sqlite_shm_writer_gate_issuer_sealed_owner&&) = delete;
		~sqlite_shm_writer_gate_issuer_sealed_owner() noexcept = default;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_shm_writer_gate_attempt_lifecycle lifecycle() const noexcept;
		[[nodiscard]] const sqlite_shm_writer_gate_outcome& outcome() const noexcept;

	  private:
		explicit sqlite_shm_writer_gate_issuer_sealed_owner(sqlite_shm_writer_gate_outcome outcome);

		std::optional<sqlite_shm_writer_gate_outcome> outcome_;

		friend class sqlite_same_process_shm_writer_gate_receipt_validator;
	};

	/** Move-only registry continuation marker; cut execution is intentionally a later unit. */
	class sqlite_shm_writer_gate_registry_continuation_owner
	{
	  public:
		sqlite_shm_writer_gate_registry_continuation_owner(
			const sqlite_shm_writer_gate_registry_continuation_owner&) = delete;
		sqlite_shm_writer_gate_registry_continuation_owner&
		operator=(const sqlite_shm_writer_gate_registry_continuation_owner&) = delete;
		sqlite_shm_writer_gate_registry_continuation_owner(
			sqlite_shm_writer_gate_registry_continuation_owner&&) noexcept;
		sqlite_shm_writer_gate_registry_continuation_owner&
		operator=(sqlite_shm_writer_gate_registry_continuation_owner&&) = delete;
		~sqlite_shm_writer_gate_registry_continuation_owner() noexcept = default;

		[[nodiscard]] bool valid() const noexcept;
		[[nodiscard]] sqlite_shm_writer_gate_attempt_lifecycle lifecycle() const noexcept;
		[[nodiscard]] const sqlite_shm_writer_gate_outcome& outcome() const noexcept;

	  private:
		explicit sqlite_shm_writer_gate_registry_continuation_owner(
			sqlite_shm_writer_gate_outcome outcome);

		std::optional<sqlite_shm_writer_gate_outcome> outcome_;

		friend class sqlite_same_process_shm_writer_gate_receipt_validator;
	};

	/**
	 * Source-private issuer for the closed writer gate evidence unit.
	 *
	 * The value produced here is evidence only. It does not mint eligibility, native cleanup,
	 * reader authority, VFS binding, or a Store publication effect.
	 */
	class sqlite_same_process_shm_writer_gate_receipt_validator final
	{
	  public:
		sqlite_same_process_shm_writer_gate_receipt_validator() = delete;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_writer_gate_attempt_owner>
		begin(sqlite_shm_writer_gate_attempt_binding binding) noexcept;

		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_writer_gate_issuer_sealed_owner>
		seal(sqlite_shm_writer_gate_attempt_owner&& owner,
			 std::vector<sqlite_shm_writer_gate_stage_bundle> stages,
			 sqlite_shm_writer_gate_outcome_kind kind,
			 std::optional<sqlite_shm_writer_gate_terminal_reason> terminal_reason = std::nullopt,
			 std::optional<sqlite_shm_writer_gate_terminal_locus> terminal_locus =
				 std::nullopt) noexcept;

		[[nodiscard]] static sqlite_shm_lease_result<
			sqlite_shm_writer_gate_registry_continuation_owner>
		transfer_to_registry(sqlite_shm_writer_gate_issuer_sealed_owner&& owner) noexcept;

	  private:
		[[nodiscard]] static sqlite_shm_lease_result<sqlite_shm_writer_gate_issuer_sealed_owner>
		seal_impl(
			sqlite_shm_writer_gate_attempt_owner&& owner,
			std::vector<sqlite_shm_writer_gate_stage_bundle> stages,
			sqlite_shm_writer_gate_outcome_kind kind,
			const std::optional<sqlite_shm_writer_gate_terminal_reason>& terminal_reason,
			const std::optional<sqlite_shm_writer_gate_terminal_locus>& terminal_locus) noexcept;
	};
} // namespace cxxlens::sdk
