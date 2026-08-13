#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "materialization_incremental_receipt.hpp"
#include "materialization_partition_event_stream.hpp"

namespace cxxlens::detail::clang22::materialization
{
	/**
	 * Return the canonical encoded size of one length-delimited value with a payload of
	 * `payload_bytes`.
	 *
	 * Canonical length-delimited values consist of a one-byte tag, an eight-byte length, and the
	 * payload. This source-private seam is also used by the streaming digest builder so
	 * limit-adjacent arithmetic can be tested without allocating a near-`UINT64_MAX` buffer.
	 */
	[[nodiscard]] sdk::result<std::uint64_t>
	materialization_claim_stream_framed_length(std::uint64_t payload_bytes);

	/**
	 * Move-only external task handoff for the source-private claim/event boundary.
	 *
	 * The task receipt is the external completeness authority. The CXLPEV01 spools are retained
	 * sealed and are independently revalidated before the source is returned. No task result is
	 * accepted here because the pre-encoder result oracle belongs to
	 * materialization_incremental_ingress.
	 */
	struct materialization_claim_stream_task
	{
		materialization_incremental_task_receipt receipt;
		std::vector<std::unique_ptr<materialization_replayable_spool>> partition_spools;

		materialization_claim_stream_task(
			materialization_incremental_task_receipt receipt,
			std::vector<std::unique_ptr<materialization_replayable_spool>> partition_spools)
			: receipt{std::move(receipt)}, partition_spools{std::move(partition_spools)}
		{
		}

		materialization_claim_stream_task(const materialization_claim_stream_task&) = delete;
		materialization_claim_stream_task&
		operator=(const materialization_claim_stream_task&) = delete;
		materialization_claim_stream_task(materialization_claim_stream_task&&) noexcept = default;
		materialization_claim_stream_task&
		operator=(materialization_claim_stream_task&&) noexcept = default;
	};

	/**
	 * One value-owned view of one already validated event.
	 *
	 * `task_id` and `partition_id` are taken from the event key. In particular, partition_id is
	 * never synthesized from a task id, spool ordinal, vector position, or hash. The byte spans are
	 * valid only during the callback invocation.
	 */
	struct materialization_claim_stream_event
	{
		std::uint64_t stream_ordinal{};
		std::string_view task_id;
		std::string_view partition_id;
		materialization_partition_event_kind kind{
			materialization_partition_event_kind::partition_begin};
		std::span<const std::byte> key;
		std::span<const std::byte> payload;
	};

	using materialization_claim_stream_consumer =
		std::function<sdk::result<void>(const materialization_claim_stream_event&)>;

	/**
	 * Replayable source-private event ingress for a selected materialization request.
	 *
	 * This is deliberately an event source, not an sdk::partition_draft source. The current
	 * CXLPEV01 payloads carry canonical bytes for claim content, occurrence metadata, detached
	 * rows, annotations, coverage, unresolved references, and closure keys. They do not provide a
	 * safe source-private decoder that establishes the corresponding typed SDK values and closure
	 * certificates. Exposing a partition_draft here would therefore either lose fields or invent
	 * semantic identity. Consumers that require typed drafts must add an independently validated
	 * decoder and identity/closure boundary before using this source.
	 */
	class materialization_claim_stream_source
	{
	  public:
		materialization_claim_stream_source(const materialization_claim_stream_source&) = delete;
		materialization_claim_stream_source&
		operator=(const materialization_claim_stream_source&) = delete;
		materialization_claim_stream_source(materialization_claim_stream_source&&) noexcept =
			default;
		materialization_claim_stream_source&
		operator=(materialization_claim_stream_source&&) noexcept = default;
		~materialization_claim_stream_source() = default;

		/**
		 * Validate request/journal/task receipt bindings and every sealed event spool, then take
		 * ownership of the validated spools. No publication or Store operation is performed.
		 */
		[[nodiscard]] static sdk::result<materialization_claim_stream_source>
		begin(const validated_materialization_request& request,
			  const materialization_incremental_execution_journal_receipt& journal,
			  std::vector<materialization_claim_stream_task> tasks);

		/**
		 * Unit-testable validation entry point. It does not consume or mutate task spools. A caller
		 * may use it before transferring ownership to begin().
		 */
		[[nodiscard]] static sdk::result<void> validate_external_task_receipts(
			const validated_materialization_request& request,
			const materialization_incremental_execution_journal_receipt& journal,
			std::span<materialization_claim_stream_task> tasks);

		/** Replay all task/partition streams in their validated canonical source order. */
		[[nodiscard]] sdk::result<void>
		replay(const materialization_claim_stream_consumer& consumer);

		[[nodiscard]] std::string_view materialization_request_id() const noexcept
		{
			return materialization_request_id_;
		}

		[[nodiscard]] std::size_t task_count() const noexcept
		{
			return tasks_.size();
		}

		[[nodiscard]] std::size_t partition_count() const noexcept;

		/** Return the event-key-derived partition identities for one task. */
		[[nodiscard]] std::span<const std::string>
		partition_ids(std::size_t task_index) const noexcept;

		/** Return the externally sealed receipt retained for one task. */
		[[nodiscard]] const materialization_incremental_task_receipt*
		task_receipt(std::size_t task_index) const noexcept;

	  private:
		struct task_state
		{
			materialization_incremental_task_receipt receipt;
			std::vector<std::unique_ptr<materialization_replayable_spool>> partition_spools;
			std::vector<std::string> partition_ids;
		};

		[[nodiscard]] static sdk::result<void>
		validate_task_streams(std::string_view request_id,
							  const materialization_claim_stream_task& task,
							  task_state& output);

		[[nodiscard]] static sdk::result<std::vector<task_state>>
		build_states(std::string_view request_id,
					 std::vector<materialization_claim_stream_task>& tasks);

		materialization_claim_stream_source(std::string materialization_request_id,
											std::vector<task_state> tasks)
			: materialization_request_id_{std::move(materialization_request_id)},
			  tasks_{std::move(tasks)}
		{
		}

		std::string materialization_request_id_;
		std::vector<task_state> tasks_;
	};
} // namespace cxxlens::detail::clang22::materialization
