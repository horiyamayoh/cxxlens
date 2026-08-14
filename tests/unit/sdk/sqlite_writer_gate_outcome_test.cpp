#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "sdk/sqlite_writer_gate_outcome_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
			throw std::runtime_error{std::string{message}};
	}

	[[nodiscard]] sqlite_backend_opaque_identity identity(const std::string_view profile,
														  const std::uint8_t marker)
	{
		return {std::string{profile}, {static_cast<std::byte>(marker)}};
	}

	[[nodiscard]] sqlite_shm_writer_gate_attempt_binding binding()
	{
		sqlite_shm_writer_gate_attempt_binding result;
		result.family = {identity("test.gate.process", 1U),
						 identity("test.gate.runtime", 2U),
						 identity("test.gate.file-family", 3U)};
		result.alias_lifetime = identity("test.gate.alias", 4U);
		result.connection_token = identity("test.gate.connection", 5U);
		result.main_native_file_receipt = identity("test.gate.main-file", 6U);
		result.main_xopen_receipt = identity("test.gate.main-xopen", 7U);
		result.open_epoch = identity("test.gate.open-epoch", 8U);
		result.callback_cohort = identity("test.gate.callback-cohort", 9U);
		result.expected_attachment_epoch = identity("test.gate.attachment-epoch", 10U);
		result.issuer_control_epoch = 11U;
		result.attempt_token = identity("test.gate.attempt", 12U);
		result.gate_profile_id = "cxxlens.sqlite.current-v3-writer-gate.v1";
		result.policy_profile_bytes = {std::byte{0x01}, std::byte{0x02}, std::byte{0x03}};
		result.policy_profile_digest = content_digest(result.policy_profile_bytes);
		return result;
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::uint8_t marker)
	{
		return {static_cast<std::byte>(marker), static_cast<std::byte>(marker + 1U)};
	}

	[[nodiscard]] sqlite_shm_writer_gate_stage_bundle
	passed_stage(const sqlite_shm_writer_gate_stage stage, const std::uint8_t marker)
	{
		return {stage,
				sqlite_shm_writer_gate_stage_result::passed,
				std::nullopt,
				bytes(marker),
				bytes(static_cast<std::uint8_t>(marker + 10U)),
				bytes(static_cast<std::uint8_t>(marker + 20U)),
				bytes(static_cast<std::uint8_t>(marker + 30U)),
				bytes(static_cast<std::uint8_t>(marker + 40U)),
				sqlite_shm_writer_gate_observed_effect_slot::exact_present};
	}

	[[nodiscard]] std::vector<sqlite_shm_writer_gate_stage_bundle> all_passed()
	{
		return {
			passed_stage(sqlite_shm_writer_gate_stage::writer_readwrite_mode, 1U),
			passed_stage(sqlite_shm_writer_gate_stage::runtime_version_and_locator, 2U),
			passed_stage(sqlite_shm_writer_gate_stage::runtime_vfs_file_family_and_open_epoch, 3U),
			passed_stage(sqlite_shm_writer_gate_stage::synchronous_full_and_wal_mode, 4U),
			passed_stage(
				sqlite_shm_writer_gate_stage::current_v3_format_schema_head_counter_authority, 5U),
			passed_stage(sqlite_shm_writer_gate_stage::store_writer_open_before_publication_effect,
						 6U)};
	}

	[[nodiscard]] sqlite_shm_writer_gate_stage_bundle
	terminal_stage(const sqlite_shm_writer_gate_stage stage,
				   const sqlite_shm_writer_gate_terminal_phase phase,
				   const std::uint8_t marker)
	{
		auto result = passed_stage(stage, marker);
		result.result = sqlite_shm_writer_gate_stage_result::terminal_indeterminate;
		result.failure.reset();
		switch (phase)
		{
			case sqlite_shm_writer_gate_terminal_phase::before_value_read:
				result.expected_policy_canonical_bytes.clear();
				result.allowed_effect_canonical_bytes.clear();
				result.observed_value_canonical_bytes.clear();
				result.authority_receipt_canonical_bytes.clear();
				result.observed_effect_canonical_bytes.clear();
				result.observed_effect_slot =
					sqlite_shm_writer_gate_observed_effect_slot::not_executed;
				break;
			case sqlite_shm_writer_gate_terminal_phase::after_value_before_effect:
				result.observed_effect_canonical_bytes.clear();
				result.observed_effect_slot =
					sqlite_shm_writer_gate_observed_effect_slot::not_executed;
				break;
			case sqlite_shm_writer_gate_terminal_phase::after_effect_start_before_effect_result:
				result.observed_effect_slot =
					sqlite_shm_writer_gate_observed_effect_slot::started_outcome_unresolved;
				break;
			case sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result:
				break;
		}
		return result;
	}

	void verify_move_only_surface()
	{
		static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_gate_attempt_owner>);
		static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_gate_outcome>);
		static_assert(!std::is_copy_constructible_v<sqlite_shm_writer_gate_issuer_sealed_owner>);
		static_assert(
			!std::is_copy_constructible_v<sqlite_shm_writer_gate_registry_continuation_owner>);
		static_assert(std::is_nothrow_move_constructible_v<sqlite_shm_writer_gate_attempt_owner>);
		static_assert(
			std::is_nothrow_move_constructible_v<sqlite_shm_writer_gate_issuer_sealed_owner>);
		static_assert(std::is_nothrow_move_constructible_v<
					  sqlite_shm_writer_gate_registry_continuation_owner>);
	}

	void verify_positive_transfer_is_single_use()
	{
		auto begun = sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		require(begun && begun->valid() &&
					begun->lifecycle() == sqlite_shm_writer_gate_attempt_lifecycle::open,
				"valid gate binding did not mint an open owner");
		auto sealed = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*begun), all_passed(), sqlite_shm_writer_gate_outcome_kind::positive_success);
		require(sealed && sealed->valid() &&
					sealed->lifecycle() ==
						sqlite_shm_writer_gate_attempt_lifecycle::issuer_sealed_kind &&
					sealed->outcome().kind() ==
						sqlite_shm_writer_gate_outcome_kind::positive_success &&
					sealed->outcome().stages().size() == 6U,
				"six passed stages did not produce the positive issuer-sealed outcome");
		auto transferred =
			sqlite_same_process_shm_writer_gate_receipt_validator::transfer_to_registry(
				std::move(*sealed));
		require(transferred && transferred->valid() &&
					transferred->lifecycle() ==
						sqlite_shm_writer_gate_attempt_lifecycle::transferred_to_registry &&
					transferred->outcome().binding() == binding(),
				"issuer outcome was not transferred byte-for-byte to the registry marker");
		auto replay = sqlite_same_process_shm_writer_gate_receipt_validator::transfer_to_registry(
			std::move(*sealed));
		require(!replay && replay.error().reason == sqlite_shm_lease_rejection_reason::stale_token,
				"transferred gate owner was reusable");
	}

	void verify_failure_bijection_and_terminal_slots()
	{
		auto begun = sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		auto stages = all_passed();
		stages.resize(4U);
		stages.back().result = sqlite_shm_writer_gate_stage_result::typed_determinate_failure;
		stages.back().failure = sqlite_shm_writer_gate_failure::synchronous_or_wal_mode_rejected;
		auto failed = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*begun),
			std::move(stages),
			sqlite_shm_writer_gate_outcome_kind::typed_determinate_failure);
		require(failed && failed->outcome().failure() &&
					*failed->outcome().failure() ==
						sqlite_shm_writer_gate_failure::synchronous_or_wal_mode_rejected,
				"stage-four failure did not retain its bijective typed failure");

		auto terminal_begin =
			sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		sqlite_shm_writer_gate_stage_bundle terminal{
			sqlite_shm_writer_gate_stage::writer_readwrite_mode,
			sqlite_shm_writer_gate_stage_result::terminal_indeterminate,
			std::nullopt,
			{},
			{},
			{},
			{},
			{},
			sqlite_shm_writer_gate_observed_effect_slot::not_executed};
		auto terminal_result = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*terminal_begin),
			{std::move(terminal)},
			sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate,
			sqlite_shm_writer_gate_terminal_reason::timeout,
			sqlite_shm_writer_gate_terminal_locus{
				sqlite_shm_writer_gate_stage::writer_readwrite_mode,
				sqlite_shm_writer_gate_terminal_phase::before_value_read});
		require(terminal_result && terminal_result->outcome().terminal_reason() &&
					*terminal_result->outcome().terminal_reason() ==
						sqlite_shm_writer_gate_terminal_reason::timeout,
				"before-value terminal outcome lost its typed locus");

		auto terminal_after_pass_begin =
			sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		auto passed_prefix = all_passed();
		passed_prefix.resize(1U);
		passed_prefix[0U].expected_policy_canonical_bytes.clear();
		auto malformed_terminal = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*terminal_after_pass_begin),
			std::move(passed_prefix),
			sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate,
			sqlite_shm_writer_gate_terminal_reason::timeout,
			sqlite_shm_writer_gate_terminal_locus{
				std::nullopt,
				sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result});
		require(!malformed_terminal &&
					malformed_terminal.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_request,
				"terminal outcome accepted a malformed passed prefix");
	}

	void verify_complete_gate_failure_and_terminal_matrix()
	{
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
		constexpr std::array terminal_phases{
			sqlite_shm_writer_gate_terminal_phase::before_value_read,
			sqlite_shm_writer_gate_terminal_phase::after_value_before_effect,
			sqlite_shm_writer_gate_terminal_phase::after_effect_start_before_effect_result,
			sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result,
		};
		constexpr std::array terminal_reasons{
			sqlite_shm_writer_gate_terminal_reason::timeout,
			sqlite_shm_writer_gate_terminal_reason::unknown,
			sqlite_shm_writer_gate_terminal_reason::open_epoch_drift,
			sqlite_shm_writer_gate_terminal_reason::validator_sealed_attempt_abandonment,
			sqlite_shm_writer_gate_terminal_reason::authority_loss,
			sqlite_shm_writer_gate_terminal_reason::incomplete_or_unclassified_effect,
		};

		for (std::size_t index{}; index < stages.size(); ++index)
		{
			auto passed_prefix = all_passed();
			passed_prefix.resize(index + 1U);
			passed_prefix.back().result =
				sqlite_shm_writer_gate_stage_result::typed_determinate_failure;
			passed_prefix.back().failure = failures[index];
			auto begun = sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
			auto sealed = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
				std::move(*begun),
				std::move(passed_prefix),
				sqlite_shm_writer_gate_outcome_kind::typed_determinate_failure);
			require(sealed && sealed->outcome().stages().size() == index + 1U &&
						sealed->outcome().failure() &&
						*sealed->outcome().failure() == failures[index],
					"one of the six stage/failure pairs was not accepted");

			for (const auto phase : terminal_phases)
			{
				for (const auto reason : terminal_reasons)
				{
					auto terminal_prefix = all_passed();
					terminal_prefix.resize(index);
					terminal_prefix.push_back(terminal_stage(
						stages[index], phase, static_cast<std::uint8_t>(index + 1U)));
					auto terminal_begin =
						sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
					auto terminal = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
						std::move(*terminal_begin),
						std::move(terminal_prefix),
						sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate,
						reason,
						sqlite_shm_writer_gate_terminal_locus{stages[index], phase});
					require(terminal && terminal->outcome().terminal_reason() &&
								*terminal->outcome().terminal_reason() == reason,
							"an accepted at-stage terminal phase/reason was rejected");
				}
			}
		}

		for (const auto reason : terminal_reasons)
		{
			auto terminal_begin =
				sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
			auto terminal = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
				std::move(*terminal_begin),
				all_passed(),
				sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate,
				reason,
				sqlite_shm_writer_gate_terminal_locus{
					std::nullopt,
					sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result});
			require(terminal && terminal->outcome().stages().size() == stages.size(),
					"post-sixth-stage terminal was not accepted");
		}

		for (std::size_t prefix_size{1U}; prefix_size < stages.size(); ++prefix_size)
		{
			auto incomplete_prefix = all_passed();
			incomplete_prefix.resize(prefix_size);
			auto terminal_begin =
				sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
			auto rejected = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
				std::move(*terminal_begin),
				std::move(incomplete_prefix),
				sqlite_shm_writer_gate_outcome_kind::terminal_indeterminate,
				sqlite_shm_writer_gate_terminal_reason::timeout,
				sqlite_shm_writer_gate_terminal_locus{
					std::nullopt,
					sqlite_shm_writer_gate_terminal_phase::after_effect_before_stage_result});
			require(!rejected &&
						rejected.error().reason ==
							sqlite_shm_lease_rejection_reason::invalid_request,
					"a stage-less terminal bypassed the six-stage completion boundary");
		}
	}

	void verify_negative_validation()
	{
		auto invalid = binding();
		invalid.policy_profile_digest = "sha256:" + std::string(64U, '0');
		auto rejected =
			sqlite_same_process_shm_writer_gate_receipt_validator::begin(std::move(invalid));
		require(!rejected &&
					rejected.error().reason == sqlite_shm_lease_rejection_reason::invalid_identity,
				"policy digest drift was accepted as gate authority");

		auto begun = sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		auto reordered = all_passed();
		std::swap(reordered[0U].stage, reordered[1U].stage);
		auto order_rejected = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*begun),
			std::move(reordered),
			sqlite_shm_writer_gate_outcome_kind::positive_success);
		require(!order_rejected &&
					order_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_request,
				"reordered gate stages were accepted");

		auto mismatch_begin =
			sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		auto mismatch = all_passed();
		mismatch.resize(2U);
		mismatch.back().result = sqlite_shm_writer_gate_stage_result::typed_determinate_failure;
		mismatch.back().failure = sqlite_shm_writer_gate_failure::writer_mode_rejected;
		auto mismatch_rejected = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*mismatch_begin),
			std::move(mismatch),
			sqlite_shm_writer_gate_outcome_kind::typed_determinate_failure);
		require(!mismatch_rejected &&
					mismatch_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_request,
				"non-bijective gate failure was accepted");

		auto unknown_kind_begin =
			sqlite_same_process_shm_writer_gate_receipt_validator::begin(binding());
		auto unknown_kind = sqlite_same_process_shm_writer_gate_receipt_validator::seal(
			std::move(*unknown_kind_begin),
			all_passed(),
			static_cast<sqlite_shm_writer_gate_outcome_kind>(0xffU));
		require(!unknown_kind &&
					unknown_kind.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_request,
				"unknown gate outcome kind was accepted");

		auto attached_binding = binding();
		attached_binding.observed_attachment_state =
			sqlite_shm_writer_gate_observed_attachment_state::present;
		auto attached_rejected = sqlite_same_process_shm_writer_gate_receipt_validator::begin(
			std::move(attached_binding));
		require(!attached_rejected &&
					attached_rejected.error().reason ==
						sqlite_shm_lease_rejection_reason::invalid_identity,
				"a pre-attached reservation was accepted by the initial gate owner");
	}
} // namespace

int main()
{
	try
	{
		verify_move_only_surface();
		verify_positive_transfer_is_single_use();
		verify_failure_bijection_and_terminal_slots();
		verify_complete_gate_failure_and_terminal_matrix();
		verify_negative_validation();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
	return 0;
}
