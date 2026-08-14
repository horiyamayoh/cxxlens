#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "materialization_partition_event_stream.hpp"
#include "materialization_seal.hpp"

namespace cxxlens::detail::clang22::materialization
{
	struct materialization_bounded_task_claims;
	struct materialization_guarantee_authority;
	struct materialization_producer_authority;
	class materialization_v2_1_claim_authority;
	struct materialization_v2_1_task_execution;
	struct materialization_v2_1_task_metadata_binding;

	/** One exact count and full-projection digest in the external D3 task receipt. */
	struct materialization_incremental_receipt_component
	{
		std::uint64_t count{};
		std::string full_projection_digest;

		[[nodiscard]] bool
		operator==(const materialization_incremental_receipt_component&) const = default;
	};

	/** One complete event projection retained by the independent pre-encoder oracle. */
	struct materialization_incremental_event_projection
	{
		std::string task_id;
		std::string partition_id;
		materialization_partition_event_kind kind{
			materialization_partition_event_kind::partition_begin};
		std::vector<std::byte> key;
		std::vector<std::byte> payload;

		[[nodiscard]] bool
		operator==(const materialization_incremental_event_projection&) const = default;
	};

	/** Immutable-before-ingress task receipt; its seal excludes the final journal digest. */
	struct materialization_incremental_task_receipt
	{
		std::string materialization_request_id;
		std::string selected_request_entry_binding_digest;
		std::string task_id;
		std::uint64_t canonical_task_ordinal{};
		bool successful_seal{};
		std::uint64_t provider_stdout_byte_count{};
		std::string provider_stdout_sha256;
		std::uint64_t decoded_provider_frame_count{};
		std::string provider_frame_transcript_digest;
		std::string provider_sealed_transcript_digest;
		materialization_incremental_receipt_component partition;
		materialization_incremental_receipt_component event;
		materialization_incremental_receipt_component claim;
		materialization_incremental_receipt_component row;
		materialization_incremental_receipt_component coverage;
		materialization_incremental_receipt_component unresolved;
		std::string pre_encoder_task_receipt_seal_digest;

		[[nodiscard]] bool
		operator==(const materialization_incremental_task_receipt&) const = default;
	};

	/** Immutable request-wide selected-entry journal generated before provider dispatch. */
	struct materialization_incremental_selected_request_binding_set
	{
		std::string materialization_request_id;
		std::uint64_t exact_task_count{};
		std::vector<std::string> canonical_task_ids;
		std::vector<std::string> ordered_entry_binding_digests;
		std::string selected_request_entry_binding_set_digest;

		[[nodiscard]] bool
		operator==(const materialization_incremental_selected_request_binding_set&) const = default;
	};

	/** Final cycle-free receipt-set seal created after all canonical task receipts are sealed. */
	struct materialization_incremental_execution_journal_receipt
	{
		std::string materialization_request_id;
		std::uint64_t exact_task_count{};
		std::vector<std::string> canonical_task_ids;
		std::vector<std::string> ordered_task_receipt_seal_digests;
		std::string execution_journal_receipt_set_digest;

		[[nodiscard]] bool
		operator==(const materialization_incremental_execution_journal_receipt&) const = default;
	};

	/** Derive the selected request identity without trusting any task receipt field. */
	[[nodiscard]] sdk::result<std::string>
	materialization_incremental_request_id(const validated_materialization_request& request);

	/** Derive the selected request identity from the bounded v2.1 claim authority. */
	[[nodiscard]] sdk::result<std::string>
	materialization_incremental_request_id(const materialization_v2_1_claim_authority& authority);

	/** Derive the exact selected-request entry binding for one canonical task ordinal. */
	[[nodiscard]] sdk::result<std::string>
	seal_materialization_incremental_selected_request_entry_binding(
		const validated_materialization_request& request, std::size_t task_index);

	/** Derive one selected-entry binding without constructing a legacy request task vector. */
	[[nodiscard]] sdk::result<std::string>
	seal_materialization_incremental_selected_request_entry_binding(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task);

	/** Derive one selected-entry binding from source-independent metadata before dispatch. */
	[[nodiscard]] sdk::result<std::string>
	seal_materialization_incremental_selected_request_entry_binding(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_metadata_binding& task);

	/** Seal the complete canonical selected-request entry set before provider dispatch. */
	[[nodiscard]] sdk::result<materialization_incremental_selected_request_binding_set>
	seal_materialization_incremental_selected_request_binding_set(
		const materialization_v2_1_claim_authority& authority);

	/** Build the receipt from an independent pre-encoder event projection enumeration. */
	[[nodiscard]] sdk::result<std::vector<std::byte>>
	materialization_incremental_full_event_projection(materialization_partition_event_kind kind,
													  std::span<const std::byte> key,
													  std::span<const std::byte> payload);

	/**
	 * Enumerate the installed-tool-private D3 oracle projection from one immutable sealed result.
	 *
	 * This is intentionally separate from claim construction and event-stream replay.  It binds
	 * every emitted event to value-owned provider batches, rows, observations, coverage, unresolved
	 * items, and provider evidence while retaining no compiler or transport object.
	 */
	[[nodiscard]] sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const sealed_materialization_result& result, std::span<const std::string> partition_ids);

	/** Enumerate exact CXLPEV01 fields from the request-bound typed claim adoption. */
	[[nodiscard]] sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const validated_materialization_request& request,
		std::size_t task_index,
		const sealed_materialization_result& result,
		std::span<const std::string> partition_ids,
		const materialization_producer_authority& producer_authority,
		const materialization_guarantee_authority& guarantee_authority);

	/** Build one bounded D3 event projection from the v2.1 authority/task window. */
	[[nodiscard]] sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_result_event_projections(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const sealed_materialization_result& result,
		std::span<const std::string> partition_ids);

	/**
	 * Independently controlled receipt oracle. This must not call the event encoder's projection
	 * enumeration; ingress compares both this oracle and the encoded CXLPEV01 bytes.
	 */
	[[nodiscard]] sdk::result<std::vector<materialization_incremental_event_projection>>
	materialization_incremental_receipt_event_projections(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const sealed_materialization_result& result,
		std::span<const std::string> partition_ids);

	/** Build the receipt from an independent pre-encoder event projection enumeration. */
	[[nodiscard]] sdk::result<materialization_incremental_task_receipt>
	make_materialization_incremental_task_receipt(
		const validated_materialization_request& request,
		std::size_t task_index,
		std::uint64_t provider_stdout_byte_count,
		std::string provider_stdout_sha256,
		std::uint64_t decoded_provider_frame_count,
		std::string provider_frame_transcript_digest,
		std::string provider_sealed_transcript_digest,
		std::span<const materialization_incremental_event_projection> events);

	/** Build a v2.1 task receipt while retaining only the current metadata window. */
	[[nodiscard]] sdk::result<materialization_incremental_task_receipt>
	make_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		std::uint64_t provider_stdout_byte_count,
		std::string provider_stdout_sha256,
		std::uint64_t decoded_provider_frame_count,
		std::string provider_frame_transcript_digest,
		std::string provider_sealed_transcript_digest,
		std::span<const materialization_incremental_event_projection> events);

	/** Build a v2.1 receipt against the immutable pre-dispatch binding set. */
	[[nodiscard]] sdk::result<materialization_incremental_task_receipt>
	make_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		const materialization_incremental_selected_request_binding_set& binding_set,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		std::uint64_t provider_stdout_byte_count,
		std::string provider_stdout_sha256,
		std::uint64_t decoded_provider_frame_count,
		std::string provider_frame_transcript_digest,
		std::string provider_sealed_transcript_digest,
		std::span<const materialization_incremental_event_projection> events);

	/** Recompute the task seal and verify every task/request binding before ingress. */
	[[nodiscard]] sdk::result<void> validate_materialization_incremental_task_receipt(
		const validated_materialization_request& request,
		std::size_t task_index,
		const materialization_incremental_task_receipt& receipt);

	/** Validate one v2.1 task receipt against the current bounded task metadata. */
	[[nodiscard]] sdk::result<void> validate_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const materialization_incremental_task_receipt& receipt);

	/** Validate a v2.1 receipt against the immutable pre-dispatch binding set. */
	[[nodiscard]] sdk::result<void> validate_materialization_incremental_task_receipt(
		const materialization_v2_1_claim_authority& authority,
		const materialization_incremental_selected_request_binding_set& binding_set,
		std::size_t task_index,
		const materialization_v2_1_task_execution& task,
		const materialization_incremental_task_receipt& receipt);

	/** Seal the exact canonical-order task receipt set; task receipts cannot bind this value. */
	[[nodiscard]] sdk::result<materialization_incremental_execution_journal_receipt>
	seal_materialization_incremental_execution_journal(
		std::string materialization_request_id,
		std::span<const materialization_incremental_task_receipt> task_receipts);
} // namespace cxxlens::detail::clang22::materialization
