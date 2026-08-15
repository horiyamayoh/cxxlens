#include <utility>

#include "provider_ng1_recovery_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	result<ng1_recovery_adapter> ng1_recovery_adapter::create(ng1_resume_binding binding)
	{
		auto resume_state = ng1_resume_state::create(std::move(binding));
		if (!resume_state)
			return unexpected(std::move(resume_state.error()));

		ng1_recovery_adapter output;
		output.resume_state_.emplace(std::move(*resume_state));
		return output;
	}

	result<void> ng1_recovery_adapter::transition(const ng1_recovery_event event)
	{
		auto next = ng1_recovery_transition(state_, event);
		if (!next)
			return unexpected(std::move(next.error()));
		state_ = *next;
		return {};
	}

	result<void> ng1_recovery_adapter::fail_from(const ng1_recovery_event event,
												 error original_error)
	{
		auto failed = ng1_recovery_transition(state_, event);
		if (!failed)
			return unexpected(std::move(failed.error()));
		state_ = *failed;
		return unexpected(std::move(original_error));
	}

	result<void> ng1_recovery_adapter::observe_worker_exit()
	{
		return transition(ng1_recovery_event::worker_exit);
	}

	result<void> ng1_recovery_adapter::observe_heartbeat_timeout()
	{
		return transition(ng1_recovery_event::heartbeat_timeout);
	}

	result<void> ng1_recovery_adapter::observe_progress_rate_failure()
	{
		return transition(ng1_recovery_event::progress_rate_failure);
	}

	result<void> ng1_recovery_adapter::confirm_worker_kill()
	{
		return transition(ng1_recovery_event::worker_kill_confirmed);
	}

	result<void> ng1_recovery_adapter::reject_heartbeat_clock()
	{
		return transition(ng1_recovery_event::invalid_heartbeat_clock);
	}

	result<void>
	ng1_recovery_adapter::accept_durable_resume(const ng1_resume_token& token,
												const ng1_spill_fsync_receipt& receipt,
												const bool open_dependency_group,
												const bool terminal,
												const std::uint64_t highest_observed_sequence)
	{
		if (!resume_state_)
			return unexpected(error{"provider.recovery-failed", "state", "uninitialized"});

		// The recovery matrix is the first authority: a valid token must never
		// bypass worker termination or be accepted from a terminal state.
		if (state_ != ng1_recovery_state::worker_killed)
			return transition(ng1_recovery_event::durable_token_valid);

		auto accepted = resume_state_->accept(
			token, receipt, open_dependency_group, terminal, highest_observed_sequence);
		if (!accepted)
			return fail_from(ng1_recovery_event::durable_token_invalid,
							 std::move(accepted.error()));

		return transition(ng1_recovery_event::durable_token_valid);
	}

	result<std::uint64_t> ng1_recovery_adapter::replay_start_sequence() const
	{
		if (!resume_state_ || state_ != ng1_recovery_state::resume_replay)
			return unexpected(error{"provider.recovery-failed", "state", "replay-not-pending"});
		return resume_state_->replay_start_sequence();
	}

	result<void> ng1_recovery_adapter::accept_replay_validated(const std::uint64_t first_sequence)
	{
		if (!resume_state_ || state_ != ng1_recovery_state::resume_replay)
			return transition(ng1_recovery_event::replay_valid);

		auto expected = resume_state_->replay_start_sequence();
		if (!expected)
			return fail_from(ng1_recovery_event::replay_invalid, std::move(expected.error()));
		if (first_sequence != *expected)
			return fail_from(
				ng1_recovery_event::replay_invalid,
				error{"provider.resume-replay-invalid", "first_sequence", "not-ack-plus-one"});

		return transition(ng1_recovery_event::replay_valid);
	}

	result<void> ng1_recovery_adapter::seal_output()
	{
		return transition(ng1_recovery_event::output_sealed);
	}

	result<void> ng1_recovery_adapter::reject_output()
	{
		return transition(ng1_recovery_event::output_invalid);
	}
} // namespace cxxlens::sdk::provider::detail
