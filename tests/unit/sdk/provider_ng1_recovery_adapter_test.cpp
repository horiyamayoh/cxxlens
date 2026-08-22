#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_recovery_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider::detail;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	template <typename T>
	void require(const result<T>& outcome, const std::string_view message)
	{
		require(outcome.has_value(), message);
	}

	void require(const result<void>& outcome, const std::string_view message)
	{
		require(outcome.has_value(), message);
	}

	[[nodiscard]] std::string digest(const std::string_view value)
	{
		auto output = semantic_digest("test.ng1.recovery-adapter", value);
		require(output, "test semantic digest construction failed");
		return *output;
	}

	[[nodiscard]] std::string manifest_digest(const char fill)
	{
		return std::string{"sha256:"} + std::string(64U, fill);
	}

	[[nodiscard]] ng1_resume_binding binding()
	{
		return {"provider:test",
				{1U, 2U, 3U},
				manifest_digest('a'),
				manifest_digest('b'),
				"session:test",
				"task:test",
				digest("input"),
				digest("invocation"),
				digest("toolchain"),
				digest("environment"),
				digest("sandbox"),
				"dependency:test",
				"atomic:test",
				"batch:test",
				7U};
	}

	[[nodiscard]] ng1_resume_token make_token(const ng1_resume_binding& value,
											  const std::uint64_t generation = 1U)
	{
		ng1_resume_token output;
		output.kind = ng1_resume_kind::accepted;
		output.binding = value;
		output.highest_contiguous_acked_sequence = 4U;
		output.staged_digest = digest("staged");
		output.token_generation = generation;
		auto token_digest = ng1_resume_token_digest(output);
		require(token_digest, "resume token digest construction failed");
		output.token_digest = *token_digest;
		return output;
	}

	[[nodiscard]] ng1_spill_fsync_receipt make_receipt(const ng1_resume_binding& value,
													   const std::uint64_t fsync_sequence = 1U)
	{
		return {"cxxlens.provider-spill-fsync-receipt.v1",
				value.provider_id,
				value.protocol_session_id,
				value.task_id,
				value.stream_id,
				4U,
				digest("staged"),
				digest("spill"),
				128U,
				2U,
				fsync_sequence};
	}

	[[nodiscard]] ng1_spill_binding spill_binding(const ng1_resume_binding& value)
	{
		return {value.provider_id,
				value.protocol_session_id,
				value.task_id,
				value.dependency_group_id,
				value.atomic_output_group_id,
				value.batch_id,
				value.stream_id};
	}

	[[nodiscard]] ng1_spill_record spill_record(const ng1_spill_binding& value,
												const std::uint64_t ordinal,
												const std::uint64_t sequence,
												const std::string_view payload_text)
	{
		ng1_spill_record output;
		output.record_ordinal = ordinal;
		output.task_id = value.task_id;
		output.dependency_group_id = value.dependency_group_id;
		output.atomic_output_group_id = value.atomic_output_group_id;
		output.batch_id = value.batch_id;
		output.stream_id = value.stream_id;
		output.sequence = sequence;
		for (const auto byte : payload_text)
			output.payload_bytes.push_back(
				static_cast<std::byte>(static_cast<unsigned char>(byte)));
		auto payload_digest = ng1_spill_payload_digest(output.payload_bytes);
		require(payload_digest, "spill payload digest construction failed");
		output.payload_digest = *payload_digest;
		auto record_digest = ng1_spill_record_digest(output);
		require(record_digest, "spill record digest construction failed");
		output.record_digest = *record_digest;
		return output;
	}

	[[nodiscard]] ng1_resume_token make_frontier_token(const ng1_resume_binding& value,
													   const std::uint64_t generation,
													   const std::uint64_t acknowledged_sequence,
													   std::string staged_digest)
	{
		ng1_resume_token output;
		output.kind = ng1_resume_kind::accepted;
		output.binding = value;
		output.highest_contiguous_acked_sequence = acknowledged_sequence;
		output.staged_digest = std::move(staged_digest);
		output.token_generation = generation;
		auto token_digest = ng1_resume_token_digest(output);
		require(token_digest, "frontier resume token digest construction failed");
		output.token_digest = *token_digest;
		return output;
	}

	void test_crash_restart_replays_after_durable_ack()
	{
		const auto resume_binding = binding();
		auto adapter = ng1_recovery_adapter::create(resume_binding);
		require(adapter, "recovery adapter creation failed");
		require(adapter->state() == ng1_recovery_state::running,
				"recovery adapter did not start in running state");
		require(adapter->observe_worker_exit(), "worker crash was not observed");
		require(adapter->state() == ng1_recovery_state::worker_killed,
				"worker crash did not require kill/restart boundary");
		require(adapter->accept_durable_resume(
					make_token(resume_binding), make_receipt(resume_binding), false, false, 4U),
				"durable resume token was not admitted after worker exit");
		require(adapter->state() == ng1_recovery_state::resume_replay,
				"durable token did not enter replay state");
		auto replay_start = adapter->replay_start_sequence();
		require(replay_start && *replay_start == 5U,
				"replay did not begin at durable acknowledgement plus one");
		require(adapter->accept_replay_validated(*replay_start),
				"valid restart replay was rejected");
		require(adapter->state() == ng1_recovery_state::resumed,
				"valid restart replay did not resume the task");
		require(adapter->seal_output(), "resumed output was not sealed");
		require(adapter->state() == ng1_recovery_state::completed,
				"sealed resumed output did not complete recovery");
	}

	void test_hang_path_requires_kill_before_resume()
	{
		const auto resume_binding = binding();
		auto adapter = ng1_recovery_adapter::create(resume_binding);
		require(adapter, "hang recovery adapter creation failed");
		require(adapter->observe_heartbeat_timeout(), "heartbeat timeout was not observed");
		require(adapter->state() == ng1_recovery_state::heartbeat_timeout,
				"heartbeat timeout did not enter its explicit state");
		require(adapter->confirm_worker_kill(), "worker kill was not confirmed");
		require(adapter->state() == ng1_recovery_state::worker_killed,
				"heartbeat recovery bypassed worker kill confirmation");
		require(adapter->accept_durable_resume(
					make_token(resume_binding), make_receipt(resume_binding), false, false, 4U),
				"durable token was rejected on the hang recovery path");
		require(adapter->accept_replay_validated(5U), "hang recovery replay was rejected");
		require(adapter->state() == ng1_recovery_state::resumed,
				"hang recovery did not reach resumed state");
	}

	void test_invalid_resume_fails_closed_and_wrong_order_is_rejected()
	{
		const auto resume_binding = binding();
		auto wrong_order = ng1_recovery_adapter::create(resume_binding);
		require(wrong_order, "wrong-order adapter creation failed");
		auto before_kill = wrong_order->accept_durable_resume(
			make_token(resume_binding), make_receipt(resume_binding), false, false, 4U);
		require(!before_kill && before_kill.error().code == "provider.recovery-failed",
				"resume token bypassed the worker-killed transition");
		require(wrong_order->state() == ng1_recovery_state::running,
				"wrong-order resume mutated the running state");

		auto invalid = ng1_recovery_adapter::create(resume_binding);
		require(invalid && invalid->observe_worker_exit(), "invalid-token setup failed");
		auto token = make_token(resume_binding);
		token.staged_digest = digest("mutated-without-reprojection");
		auto invalid_result =
			invalid->accept_durable_resume(token, make_receipt(resume_binding), false, false, 4U);
		require(!invalid_result && invalid_result.error().code == "provider.resume-replay-invalid",
				"mutated durable token was accepted");
		require(invalid->state() == ng1_recovery_state::failed,
				"invalid durable token did not fail closed");

		auto replay = ng1_recovery_adapter::create(resume_binding);
		require(replay && replay->observe_worker_exit(), "replay-boundary setup failed");
		require(replay->accept_durable_resume(
					make_token(resume_binding), make_receipt(resume_binding), false, false, 4U),
				"replay-boundary durable token setup failed");
		auto wrong_start = replay->accept_replay_validated(4U);
		require(!wrong_start && wrong_start.error().code == "provider.resume-replay-invalid",
				"replay before durable acknowledgement plus one was accepted");
		require(replay->state() == ng1_recovery_state::failed,
				"invalid replay start did not fail closed");
	}

	void test_cancellation_transition_is_terminal_or_explicitly_recoverable()
	{
		auto cancelled = ng1_recovery_transition(ng1_recovery_state::running,
												 ng1_recovery_event::cancel_requested);
		require(cancelled && *cancelled == ng1_recovery_state::cancel_requested,
				"cancel request did not enter the explicit cancellation state");

		auto acknowledged =
			ng1_recovery_transition(*cancelled, ng1_recovery_event::cancel_acknowledged);
		require(acknowledged && *acknowledged == ng1_recovery_state::failed,
				"acknowledged cancellation did not fail closed");
		auto post_cancel = ng1_recovery_transition(*acknowledged, ng1_recovery_event::worker_exit);
		require(!post_cancel && post_cancel.error().code == "provider.recovery-failed",
				"terminal cancellation state remained restartable");

		auto timed_out = ng1_recovery_transition(ng1_recovery_state::running,
												 ng1_recovery_event::cancel_requested);
		require(timed_out, "cancel-timeout setup did not enter cancellation state");
		timed_out = ng1_recovery_transition(*timed_out, ng1_recovery_event::cancel_timeout);
		require(timed_out && *timed_out == ng1_recovery_state::worker_killed,
				"cancel timeout did not require the worker-kill boundary");

		auto replay = ng1_recovery_transition(*timed_out, ng1_recovery_event::durable_token_valid);
		require(replay && *replay == ng1_recovery_state::resume_replay,
				"cancel-timeout restart did not require a durable resume token");
		auto resumed = ng1_recovery_transition(*replay, ng1_recovery_event::replay_valid);
		require(resumed && *resumed == ng1_recovery_state::resumed,
				"validated cancel-timeout replay did not resume");
		auto completed = ng1_recovery_transition(*resumed, ng1_recovery_event::output_sealed);
		require(completed && *completed == ng1_recovery_state::completed,
				"resumed cancel-timeout output did not complete");

		auto bypass = ng1_recovery_transition(ng1_recovery_state::cancel_requested,
											  ng1_recovery_event::durable_token_valid);
		require(!bypass && bypass.error().code == "provider.recovery-failed",
				"cancellation state accepted resume before worker termination");
		auto wrong_ack = ng1_recovery_transition(ng1_recovery_state::running,
												 ng1_recovery_event::cancel_acknowledged);
		require(!wrong_ack && wrong_ack.error().code == "provider.recovery-failed",
				"running state accepted an out-of-order cancel acknowledgement");
	}

	void test_cancellation_adapter_preserves_order_and_terminality()
	{
		const auto resume_binding = binding();
		auto acknowledged = ng1_recovery_adapter::create(resume_binding);
		require(acknowledged, "cancellation adapter creation failed");
		require(acknowledged->request_cancel(), "adapter did not admit cancellation request");
		require(acknowledged->state() == ng1_recovery_state::cancel_requested,
				"adapter cancellation request skipped explicit state");
		require(acknowledged->acknowledge_cancel(),
				"adapter did not consume cancellation acknowledgement");
		require(acknowledged->state() == ng1_recovery_state::failed,
				"acknowledged cancellation was not terminal failure");
		auto post_ack = acknowledged->timeout_cancel();
		require(!post_ack && post_ack.error().code == "provider.recovery-failed",
				"acknowledged cancellation remained restartable");

		auto timed_out = ng1_recovery_adapter::create(resume_binding);
		require(timed_out, "cancel-timeout adapter creation failed");
		require(timed_out->request_cancel(), "cancel-timeout request was rejected");
		require(timed_out->timeout_cancel(), "cancel-timeout escalation was rejected");
		require(timed_out->state() == ng1_recovery_state::worker_killed,
				"cancel-timeout did not require the worker-kill boundary");
		auto before_request = ng1_recovery_adapter::create(resume_binding);
		require(before_request, "out-of-order cancellation adapter creation failed");
		auto premature_ack = before_request->acknowledge_cancel();
		require(!premature_ack && premature_ack.error().code == "provider.recovery-failed",
				"adapter accepted cancellation acknowledgement before request");
		require(before_request->state() == ng1_recovery_state::running,
				"rejected cancellation acknowledgement mutated state");
	}

	void test_durable_receipts_advance_with_the_exact_spill_prefix()
	{
		const auto resume_binding = binding();
		const auto staged_zero = digest("staged-zero");
		const auto staged_one = digest("staged-one");
		const auto spill = spill_binding(resume_binding);
		auto prefix = ng1_spill_prefix_state::create(spill);
		require(prefix, "spill prefix creation failed");
		require(prefix->append(spill_record(spill, 0U, 0U, "first")),
				"first spill prefix record was rejected");

		auto beyond_prefix = prefix->observe_host_fsync(1U, staged_zero, 1U);
		require(!beyond_prefix && beyond_prefix.error().code == "provider.spill-corrupt",
				"fsync receipt acknowledged a sequence absent from the spill prefix");
		auto first_receipt = prefix->observe_host_fsync(0U, staged_zero, 1U);
		require(first_receipt && first_receipt->total_records == 1U &&
					first_receipt->total_bytes == prefix->total_bytes() &&
					first_receipt->staged_digest == staged_zero,
				"first durable receipt did not bind the complete staged prefix");

		auto resume = ng1_resume_state::create(resume_binding);
		require(resume, "durable resume state creation failed");
		auto first_token = make_frontier_token(resume_binding, 1U, 0U, staged_zero);
		require(resume->accept(first_token, *first_receipt, false, false, 0U),
				"first fsync-confirmed resume token was rejected");
		auto first_replay = resume->replay_start_sequence();
		require(first_replay && *first_replay == 1U,
				"first replay did not start after the durable acknowledgement");

		require(prefix->append(spill_record(spill, 1U, 1U, "second")),
				"second spill prefix record was rejected");
		auto second_receipt = prefix->observe_host_fsync(1U, staged_one, 2U);
		require(second_receipt && second_receipt->total_records == 2U &&
					second_receipt->total_bytes == prefix->total_bytes() &&
					second_receipt->spill_digest != first_receipt->spill_digest,
				"second durable receipt did not advance with the appended prefix");
		auto second_token = make_frontier_token(resume_binding, 2U, 1U, staged_one);
		require(resume->accept(second_token, *second_receipt, false, false, 1U),
				"strictly newer fsync-confirmed resume token was rejected");
		auto second_replay = resume->replay_start_sequence();
		require(second_replay && *second_replay == 2U,
				"second replay did not advance after the durable acknowledgement");

		auto volatile_receipt = *second_receipt;
		volatile_receipt.schema = "volatile-ack-without-fsync";
		auto volatile_token = make_frontier_token(resume_binding, 3U, 1U, staged_one);
		auto volatile_result = resume->accept(volatile_token, volatile_receipt, false, false, 1U);
		require(!volatile_result && volatile_result.error().code == "provider.resume-token-stale",
				"receipt without the durable schema remained resume authority");
	}
} // namespace

int main()
{
	test_crash_restart_replays_after_durable_ack();
	test_hang_path_requires_kill_before_resume();
	test_invalid_resume_fails_closed_and_wrong_order_is_rejected();
	test_cancellation_transition_is_terminal_or_explicitly_recoverable();
	test_cancellation_adapter_preserves_order_and_terminality();
	test_durable_receipts_advance_with_the_exact_spill_prefix();
	return 0;
}
