#include "sqlite_writer_gate_outcome_internal.hpp"

#include <algorithm>
#include <array>
#include <new>
#include <stdexcept>
#include <utility>

#include <cxxlens/sdk/common.hpp>

namespace cxxlens::sdk
{
	namespace
	{
		constexpr std::string_view gate_profile_id{"cxxlens.sqlite.current-v3-writer-gate.v1"};
		constexpr std::array stages{
			sqlite_shm_writer_gate_stage::writer_readwrite_mode,
			sqlite_shm_writer_gate_stage::runtime_version_and_locator,
			sqlite_shm_writer_gate_stage::runtime_vfs_file_family_and_open_epoch,
			sqlite_shm_writer_gate_stage::synchronous_full_and_wal_mode,
			sqlite_shm_writer_gate_stage::current_v3_format_schema_head_counter_authority,
			sqlite_shm_writer_gate_stage::store_writer_open_before_publication_effect,
		};
		constexpr std::array failures{
			sqlite_shm_writer_gate_failure::writer_mode_rejected,
			sqlite_shm_writer_gate_failure::runtime_or_locator_rejected,
			sqlite_shm_writer_gate_failure::runtime_vfs_or_file_family_rejected,
			sqlite_shm_writer_gate_failure::synchronous_or_wal_mode_rejected,
			sqlite_shm_writer_gate_failure::current_v3_authority_rejected,
			sqlite_shm_writer_gate_failure::store_writer_open_rejected,
		};

		[[nodiscard]] bool valid_identity(const sqlite_backend_opaque_identity& value) noexcept
		{
			return !value.profile.empty() && !value.bytes.empty();
		}

		[[nodiscard]] bool valid_binding(const sqlite_shm_writer_gate_attempt_binding& value)
		{
			return valid_identity(value.family.process_instance) &&
				valid_identity(value.family.shared_runtime_vfs_cohort) &&
				valid_identity(value.family.exact_file_family) &&
				valid_identity(value.alias_lifetime) && valid_identity(value.connection_token) &&
				valid_identity(value.main_native_file_receipt) &&
				valid_identity(value.main_xopen_receipt) && valid_identity(value.open_epoch) &&
				valid_identity(value.callback_cohort) &&
				valid_identity(value.expected_attachment_epoch) &&
				value.issuer_control_epoch != 0U && valid_identity(value.attempt_token) &&
				value.expected_reservation_phase ==
				sqlite_shm_writer_gate_reservation_phase::reserved &&
				value.observed_attachment_state ==
				sqlite_shm_writer_gate_observed_attachment_state::absent &&
				value.gate_profile_id == gate_profile_id && !value.policy_profile_bytes.empty() &&
				value.policy_profile_digest == content_digest(value.policy_profile_bytes);
		}

		[[nodiscard]] bool
		valid_terminal_reason(const sqlite_shm_writer_gate_terminal_reason reason) noexcept
		{
			switch (reason)
			{
				case sqlite_shm_writer_gate_terminal_reason::timeout:
				case sqlite_shm_writer_gate_terminal_reason::unknown:
				case sqlite_shm_writer_gate_terminal_reason::open_epoch_drift:
				case sqlite_shm_writer_gate_terminal_reason::validator_sealed_attempt_abandonment:
				case sqlite_shm_writer_gate_terminal_reason::authority_loss:
				case sqlite_shm_writer_gate_terminal_reason::incomplete_or_unclassified_effect:
					return true;
			}
			return false;
		}

		[[nodiscard]] sqlite_shm_lease_rejection
		reject(const sqlite_shm_lease_rejection_reason reason,
			   const sqlite_shm_lease_recovery_action action =
				   sqlite_shm_lease_recovery_action::deny_before_native_map) noexcept
		{
			return {reason, action};
		}

		[[nodiscard]] bool nonempty(const std::vector<std::byte>& value) noexcept
		{
			return !value.empty();
		}

		[[nodiscard]] bool
		full_stage_payload(const sqlite_shm_writer_gate_stage_bundle& bundle) noexcept
		{
			return nonempty(bundle.expected_policy_canonical_bytes) &&
				nonempty(bundle.allowed_effect_canonical_bytes) &&
				nonempty(bundle.observed_value_canonical_bytes) &&
				nonempty(bundle.authority_receipt_canonical_bytes) &&
				nonempty(bundle.observed_effect_canonical_bytes) &&
				bundle.observed_effect_slot ==
				sqlite_shm_writer_gate_observed_effect_slot::exact_present;
		}

		[[nodiscard]] bool
		terminal_payload_matches(const sqlite_shm_writer_gate_stage_bundle& bundle,
								 const sqlite_shm_writer_gate_terminal_phase phase) noexcept
		{
			const bool policy = nonempty(bundle.expected_policy_canonical_bytes);
			const bool allowed = nonempty(bundle.allowed_effect_canonical_bytes);
			const bool observed = nonempty(bundle.observed_value_canonical_bytes);
			const bool receipt = nonempty(bundle.authority_receipt_canonical_bytes);
			const bool effect = nonempty(bundle.observed_effect_canonical_bytes);
			switch (phase)
			{
				case sqlite_shm_writer_gate_terminal_phase::before_value_read:
					return !policy && !allowed && !observed && !receipt && !effect &&
						bundle.observed_effect_slot ==
						sqlite_shm_writer_gate_observed_effect_slot::not_executed;
				case sqlite_shm_writer_gate_terminal_phase::after_value_before_effect:
					return policy && allowed && observed && receipt && !effect &&
						bundle.observed_effect_slot ==
						sqlite_shm_writer_gate_observed_effect_slot::not_executed;
				case sqlite_shm_writer_gate_terminal_phase::after_effect_start_before_effect_result:
					return policy && allowed && observed && receipt && effect &&
						bundle.observed_effect_slot ==
						sqlite_shm_writer_gate_observed_effect_slot::started_outcome_unresolved;
				case sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result:
					return full_stage_payload(bundle);
			}
			return false;
		}

		[[nodiscard]] bool valid_stage_bundle(
			const sqlite_shm_writer_gate_stage_bundle& bundle,
			const bool terminal,
			const std::optional<sqlite_shm_writer_gate_terminal_phase>& phase) noexcept
		{
			if (bundle.result == sqlite_shm_writer_gate_stage_result::passed ||
				bundle.result == sqlite_shm_writer_gate_stage_result::typed_determinate_failure)
			{
				const bool failure_expected =
					bundle.result == sqlite_shm_writer_gate_stage_result::typed_determinate_failure;
				return bundle.failure.has_value() == failure_expected &&
					full_stage_payload(bundle) && !terminal && !phase.has_value();
			}
			if (bundle.result != sqlite_shm_writer_gate_stage_result::terminal_indeterminate ||
				bundle.failure.has_value() || !terminal || !phase.has_value())
				return false;
			return terminal_payload_matches(bundle, *phase);
		}

		[[nodiscard]] bool
		valid_stage_prefix(const std::vector<sqlite_shm_writer_gate_stage_bundle>& values) noexcept
		{
			if (values.size() > stages.size())
				return false;
			for (std::size_t index{}; index < values.size(); ++index)
				if (values[index].stage != stages[index])
					return false;
			for (std::size_t index{}; index + 1U < values.size(); ++index)
				if (values[index].result != sqlite_shm_writer_gate_stage_result::passed)
					return false;
			return true;
		}

		[[nodiscard]] bool failure_matches(const std::size_t index,
										   const sqlite_shm_writer_gate_failure value) noexcept
		{
			return index < failures.size() && failures[index] == value;
		}

	} // namespace

	sqlite_shm_lease_result<sqlite_shm_writer_gate_issuer_sealed_owner>
	sqlite_same_process_shm_writer_gate_receipt_validator::seal_impl(
		sqlite_shm_writer_gate_attempt_owner&& owner,
		std::vector<sqlite_shm_writer_gate_stage_bundle> stages_value,
		const sqlite_shm_writer_gate_outcome_kind kind,
		const std::optional<sqlite_shm_writer_gate_terminal_reason>& terminal_reason,
		const std::optional<sqlite_shm_writer_gate_terminal_locus>& terminal_locus) noexcept
	{
		if (!owner.valid() || owner.lifecycle() != sqlite_shm_writer_gate_attempt_lifecycle::open)
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		owner.valid_ = false;
		if (!valid_stage_prefix(stages_value))
			return reject(sqlite_shm_lease_rejection_reason::invalid_request);

		std::optional<sqlite_shm_writer_gate_failure> failure;
		if (kind == sqlite_shm_writer_gate_outcome_kind::positive_success)
		{
			if (stages_value.size() != stages.size() ||
				std::ranges::any_of(stages_value,
									[](const auto& value)
									{
										return value.result !=
											sqlite_shm_writer_gate_stage_result::passed;
									}))
				return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			for (const auto& stage : stages_value)
				if (!valid_stage_bundle(stage, false, std::nullopt))
					return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			if (terminal_reason || terminal_locus)
				return reject(sqlite_shm_lease_rejection_reason::invalid_request);
		}
		else if (kind == sqlite_shm_writer_gate_outcome_kind::typed_determinate_failure)
		{
			if (stages_value.empty() || terminal_reason || terminal_locus ||
				stages_value.back().result !=
					sqlite_shm_writer_gate_stage_result::typed_determinate_failure ||
				!stages_value.back().failure ||
				!failure_matches(stages_value.size() - 1U, *stages_value.back().failure))
				return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			for (const auto& stage : stages_value)
				if (!valid_stage_bundle(stage, false, std::nullopt))
					return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			failure = stages_value.back().failure;
		}
		else
		{
			if (kind != sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate ||
				!terminal_reason || !valid_terminal_reason(*terminal_reason) || !terminal_locus ||
				stages_value.size() > stages.size())
				return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			if (stages_value.empty() ||
				stages_value.back().result !=
					sqlite_shm_writer_gate_stage_result::terminal_indeterminate)
			{
				if (!stages_value.empty() &&
					std::ranges::any_of(stages_value,
										[](const auto& value)
										{
											return value.result !=
												sqlite_shm_writer_gate_stage_result::passed;
										}))
					return reject(sqlite_shm_lease_rejection_reason::invalid_request);
				for (const auto& stage : stages_value)
					if (!valid_stage_bundle(stage, false, std::nullopt))
						return reject(sqlite_shm_lease_rejection_reason::invalid_request);
				if (terminal_locus->stage ||
					(stages_value.empty() &&
					 terminal_locus->phase !=
						 sqlite_shm_writer_gate_terminal_phase::before_value_read) ||
					(!stages_value.empty() &&
					 terminal_locus->phase !=
						 sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result))
					return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			}
			else
			{
				if (!terminal_locus->stage || *terminal_locus->stage != stages_value.back().stage)
					return reject(sqlite_shm_lease_rejection_reason::invalid_request);
				for (const auto& stage : stages_value)
					if (!valid_stage_bundle(
							stage, &stage == &stages_value.back(), terminal_locus->phase))
						return reject(sqlite_shm_lease_rejection_reason::invalid_request);
			}
		}

		try
		{
			return sqlite_shm_writer_gate_issuer_sealed_owner{
				sqlite_shm_writer_gate_outcome{kind,
											   std::move(owner.binding_),
											   std::move(stages_value),
											   std::move(failure),
											   terminal_reason,
											   terminal_locus}};
		}
		catch (const std::bad_alloc&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
		catch (const std::length_error&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
	}

	sqlite_shm_writer_gate_outcome::sqlite_shm_writer_gate_outcome(
		sqlite_shm_writer_gate_outcome_kind kind,
		sqlite_shm_writer_gate_attempt_binding binding,
		std::vector<sqlite_shm_writer_gate_stage_bundle> stages,
		std::optional<sqlite_shm_writer_gate_failure> failure,
		std::optional<sqlite_shm_writer_gate_terminal_reason> terminal_reason,
		std::optional<sqlite_shm_writer_gate_terminal_locus> terminal_locus)
		: kind_{kind}, binding_{std::move(binding)}, stages_{std::move(stages)},
		  failure_{std::move(failure)}, terminal_reason_{terminal_reason},
		  terminal_locus_{std::move(terminal_locus)}
	{
	}

	sqlite_shm_writer_gate_outcome_kind sqlite_shm_writer_gate_outcome::kind() const noexcept
	{
		return kind_;
	}
	const sqlite_shm_writer_gate_attempt_binding&
	sqlite_shm_writer_gate_outcome::binding() const noexcept
	{
		return binding_;
	}
	std::span<const sqlite_shm_writer_gate_stage_bundle>
	sqlite_shm_writer_gate_outcome::stages() const noexcept
	{
		return stages_;
	}
	const std::optional<sqlite_shm_writer_gate_failure>&
	sqlite_shm_writer_gate_outcome::failure() const noexcept
	{
		return failure_;
	}
	const std::optional<sqlite_shm_writer_gate_terminal_reason>&
	sqlite_shm_writer_gate_outcome::terminal_reason() const noexcept
	{
		return terminal_reason_;
	}
	const std::optional<sqlite_shm_writer_gate_terminal_locus>&
	sqlite_shm_writer_gate_outcome::terminal_locus() const noexcept
	{
		return terminal_locus_;
	}

	sqlite_shm_writer_gate_attempt_owner::sqlite_shm_writer_gate_attempt_owner(
		sqlite_shm_writer_gate_attempt_binding binding)
		: binding_{std::move(binding)}
	{
	}
	sqlite_shm_writer_gate_attempt_owner::sqlite_shm_writer_gate_attempt_owner(
		sqlite_shm_writer_gate_attempt_owner&& other) noexcept
		: binding_{std::move(other.binding_)}, lifecycle_{other.lifecycle_}, valid_{other.valid_}
	{
		other.valid_ = false;
	}
	bool sqlite_shm_writer_gate_attempt_owner::valid() const noexcept
	{
		return valid_;
	}
	sqlite_shm_writer_gate_attempt_lifecycle
	sqlite_shm_writer_gate_attempt_owner::lifecycle() const noexcept
	{
		return lifecycle_;
	}
	const sqlite_shm_writer_gate_attempt_binding&
	sqlite_shm_writer_gate_attempt_owner::binding() const noexcept
	{
		return binding_;
	}

	sqlite_shm_writer_gate_issuer_sealed_owner::sqlite_shm_writer_gate_issuer_sealed_owner(
		sqlite_shm_writer_gate_outcome outcome)
		: outcome_{std::move(outcome)}
	{
	}
	sqlite_shm_writer_gate_issuer_sealed_owner::sqlite_shm_writer_gate_issuer_sealed_owner(
		sqlite_shm_writer_gate_issuer_sealed_owner&& other) noexcept
		: outcome_{std::move(other.outcome_)}
	{
		other.outcome_.reset();
	}
	bool sqlite_shm_writer_gate_issuer_sealed_owner::valid() const noexcept
	{
		return outcome_.has_value();
	}
	sqlite_shm_writer_gate_attempt_lifecycle
	sqlite_shm_writer_gate_issuer_sealed_owner::lifecycle() const noexcept
	{
		return sqlite_shm_writer_gate_attempt_lifecycle::issuer_sealed_kind;
	}
	const sqlite_shm_writer_gate_outcome&
	sqlite_shm_writer_gate_issuer_sealed_owner::outcome() const noexcept
	{
		return *outcome_;
	}

	sqlite_shm_writer_gate_registry_continuation_owner::
		sqlite_shm_writer_gate_registry_continuation_owner(sqlite_shm_writer_gate_outcome outcome)
		: outcome_{std::move(outcome)}
	{
	}
	sqlite_shm_writer_gate_registry_continuation_owner::
		sqlite_shm_writer_gate_registry_continuation_owner(
			sqlite_shm_writer_gate_registry_continuation_owner&& other) noexcept
		: outcome_{std::move(other.outcome_)}
	{
		other.outcome_.reset();
	}
	bool sqlite_shm_writer_gate_registry_continuation_owner::valid() const noexcept
	{
		return outcome_.has_value();
	}
	sqlite_shm_writer_gate_attempt_lifecycle
	sqlite_shm_writer_gate_registry_continuation_owner::lifecycle() const noexcept
	{
		return sqlite_shm_writer_gate_attempt_lifecycle::transferred_to_registry;
	}
	const sqlite_shm_writer_gate_outcome&
	sqlite_shm_writer_gate_registry_continuation_owner::outcome() const noexcept
	{
		return *outcome_;
	}

	sqlite_shm_lease_result<sqlite_shm_writer_gate_attempt_owner>
	sqlite_same_process_shm_writer_gate_receipt_validator::begin(
		sqlite_shm_writer_gate_attempt_binding binding) noexcept
	{
		try
		{
			if (!valid_binding(binding))
				return reject(sqlite_shm_lease_rejection_reason::invalid_identity);
			return sqlite_shm_writer_gate_attempt_owner{std::move(binding)};
		}
		catch (const std::bad_alloc&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
		catch (const std::length_error&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
	}

	sqlite_shm_lease_result<sqlite_shm_writer_gate_issuer_sealed_owner>
	sqlite_same_process_shm_writer_gate_receipt_validator::seal(
		sqlite_shm_writer_gate_attempt_owner&& owner,
		std::vector<sqlite_shm_writer_gate_stage_bundle> stages_value,
		const sqlite_shm_writer_gate_outcome_kind kind,
		std::optional<sqlite_shm_writer_gate_terminal_reason> terminal_reason,
		std::optional<sqlite_shm_writer_gate_terminal_locus> terminal_locus) noexcept
	{
		return seal_impl(
			std::move(owner), std::move(stages_value), kind, terminal_reason, terminal_locus);
	}

	sqlite_shm_lease_result<sqlite_shm_writer_gate_registry_continuation_owner>
	sqlite_same_process_shm_writer_gate_receipt_validator::transfer_to_registry(
		sqlite_shm_writer_gate_issuer_sealed_owner&& owner) noexcept
	{
		if (!owner.valid())
			return reject(sqlite_shm_lease_rejection_reason::stale_token);
		try
		{
			auto outcome = std::move(*owner.outcome_);
			owner.outcome_.reset();
			return sqlite_shm_writer_gate_registry_continuation_owner{std::move(outcome)};
		}
		catch (const std::bad_alloc&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
		catch (const std::length_error&)
		{
			return reject(sqlite_shm_lease_rejection_reason::lifecycle_ambiguous,
						  sqlite_shm_lease_recovery_action::quarantine_no_retry);
		}
	}
} // namespace cxxlens::sdk
