#include <cstdlib>
#include <iostream>
#include <string>
#include <string_view>

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
} // namespace

int main()
{
	test_crash_restart_replays_after_durable_ack();
	test_hang_path_requires_kill_before_resume();
	test_invalid_resume_fails_closed_and_wrong_order_is_rejected();
	return 0;
}
