#include <chrono>
#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "provider_ng1_live_driver_internal.hpp"
#include "provider_runtime_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		class system_ng1_monotonic_clock final : public ng1_monotonic_clock_port
		{
		  public:
			result<std::uint64_t> now_ns() const override
			{
				const auto count = std::chrono::duration_cast<std::chrono::nanoseconds>(
									   std::chrono::steady_clock::now().time_since_epoch())
									   .count();
				if (count < 0)
					return cxxlens::sdk::unexpected(
						error{"provider.heartbeat-clock-invalid", "clock", "negative"});
				if (static_cast<unsigned long long>(count) >
					std::numeric_limits<std::uint64_t>::max())
					return cxxlens::sdk::unexpected(
						error{"provider.heartbeat-clock-invalid", "clock", "overflow"});
				return static_cast<std::uint64_t>(count);
			}
		};

		[[nodiscard]] error driver_error(std::string field, std::string detail)
		{
			return {"provider.protocol-state-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error process_start_exception_error(const std::string_view detail)
		{
			return {"provider.process-launch-failed", "ng1-live", std::string{detail}};
		}

		[[nodiscard]] bool same_frame(const frame& left, const frame& right) noexcept
		{
			return left.type == right.type && left.stream_id == right.stream_id &&
				left.sequence == right.sequence && left.control == right.control &&
				left.payload == right.payload && left.protocol_major == right.protocol_major &&
				left.protocol_minor == right.protocol_minor && left.flags == right.flags;
		}

		[[nodiscard]] bool is_clean_process_completion(const process_output& output)
		{
			return output.status == process_status::exited && output.exit_code == 0 &&
				output.termination_signal == 0 && output.failure_code.empty() &&
				output.sandbox.validate().has_value();
		}

		[[nodiscard]] bool valid_semantic_digest_spelling(const std::string_view value) noexcept
		{
			constexpr std::string_view prefix{"semantic-v2:sha256:"};
			if (!value.starts_with(prefix) || value.size() != prefix.size() + 64U)
				return false;
			for (const auto byte : value.substr(prefix.size()))
				if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f'))
					return false;
			return true;
		}
	} // namespace

	std::unique_ptr<ng1_monotonic_clock_port> make_system_ng1_monotonic_clock_port()
	{
		return std::make_unique<system_ng1_monotonic_clock>();
	}

	result<ng1_live_control_handoff>
	ng1_live_control_handoff::create(ng1_session_configuration configuration)
	{
		auto session = ng1_session_coordinator::create(std::move(configuration));
		if (!session)
			return cxxlens::sdk::unexpected(std::move(session.error()));
		return ng1_live_control_handoff{std::move(*session)};
	}

	ng1_live_control_handoff::ng1_live_control_handoff(ng1_live_control_handoff&& other) noexcept
		: session_{std::move(other.session_)}
	{
	}

	result<void>
	ng1_live_control_handoff::observe_task_accepted(const task_accepted_metadata& metadata,
													const std::uint64_t host_receipt_time_ns)
	{
		return session_.observe_task_accepted(metadata, host_receipt_time_ns);
	}

	result<void>
	ng1_live_control_handoff::observe_host_probe(const ng1_heartbeat_control& control,
												 const std::uint64_t host_receipt_time_ns,
												 const std::uint64_t highest_observed_sequence,
												 const std::string_view host_staged_digest)
	{
		return session_.observe_host_probe(
			control, host_receipt_time_ns, highest_observed_sequence, host_staged_digest);
	}

	result<void>
	ng1_live_control_handoff::observe_provider_ack(const ng1_heartbeat_control& control,
												   const std::uint64_t host_receipt_time_ns,
												   const std::uint64_t highest_observed_sequence,
												   const std::string_view host_staged_digest)
	{
		return session_.observe_provider_ack(
			control, host_receipt_time_ns, highest_observed_sequence, host_staged_digest);
	}

	result<void>
	ng1_live_control_handoff::observe_progress(const ng1_progress_control& control,
											   const std::uint64_t host_receipt_time_ns,
											   const bool terminal_sample)
	{
		return session_.observe_progress(control, host_receipt_time_ns, terminal_sample);
	}

	result<void> ng1_live_control_handoff::check_liveness(const std::uint64_t now_ns)
	{
		return session_.check_liveness(now_ns);
	}

	result<void> ng1_live_control_handoff::append_spill(const ng1_spill_record& record)
	{
		return session_.append_spill(record);
	}

	result<ng1_spill_fsync_receipt>
	ng1_live_control_handoff::fsync_spill(const std::uint64_t highest_contiguous_acked_sequence,
										  const std::uint64_t highest_observed_sequence,
										  std::string staged_digest,
										  const std::uint64_t resume_generation)
	{
		return session_.fsync_spill(highest_contiguous_acked_sequence,
									highest_observed_sequence,
									std::move(staged_digest),
									resume_generation);
	}

	result<void> ng1_live_control_handoff::observe_worker_exit()
	{
		return session_.observe_worker_exit();
	}

	result<void> ng1_live_control_handoff::observe_heartbeat_timeout()
	{
		return session_.observe_heartbeat_timeout();
	}

	result<void> ng1_live_control_handoff::observe_progress_rate_failure()
	{
		return session_.observe_progress_rate_failure();
	}

	result<void> ng1_live_control_handoff::request_cancel()
	{
		return session_.request_cancel();
	}

	result<void> ng1_live_control_handoff::acknowledge_cancel()
	{
		return session_.acknowledge_cancel();
	}

	result<void> ng1_live_control_handoff::timeout_cancel()
	{
		return session_.timeout_cancel();
	}

	result<void> ng1_live_control_handoff::confirm_worker_kill()
	{
		return session_.confirm_worker_kill();
	}

	result<void>
	ng1_live_control_handoff::accept_durable_resume(const ng1_resume_control& control,
													const ng1_spill_fsync_receipt& receipt,
													const bool open_dependency_group,
													const bool terminal,
													const std::uint64_t highest_observed_sequence)
	{
		return session_.accept_durable_resume(
			control, receipt, open_dependency_group, terminal, highest_observed_sequence);
	}

	result<void>
	ng1_live_control_handoff::restore_durable_resume(const ng1_resume_control& control,
													 const ng1_spill_fsync_receipt& receipt,
													 const bool open_dependency_group,
													 const bool terminal,
													 const std::uint64_t highest_observed_sequence)
	{
		return session_.restore_durable_resume(
			control, receipt, open_dependency_group, terminal, highest_observed_sequence);
	}

	result<std::uint64_t> ng1_live_control_handoff::replay_start_sequence() const
	{
		return session_.replay_start_sequence();
	}

	result<void>
	ng1_live_control_handoff::accept_replay_frontier(const std::uint64_t first_sequence)
	{
		return session_.accept_replay_frontier(first_sequence);
	}

	result<void> ng1_live_control_handoff::reject_output()
	{
		return session_.reject_output();
	}

	result<void> ng1_live_control_handoff::cleanup()
	{
		return session_.cleanup();
	}

	result<ng1_live_session_driver>
	ng1_live_session_driver::start_system(ng1_session_configuration session,
										  process_invocation invocation,
										  protocol_limits limits,
										  const std::uint64_t maximum_retained_frames,
										  std::unique_ptr<ng1_host_observation_port> observation,
										  ng1_durable_resume_authority durable_resume,
										  const std::stop_token cancellation)
	{
		auto clock = make_system_ng1_monotonic_clock_port();
		auto processes = make_system_ng1_duplex_process_port();
		if (!clock || !processes)
			return cxxlens::sdk::unexpected(
				error{"provider.runtime-unavailable", "ng1-live", "system-port"});
		return start(ng1_live_driver_configuration{std::move(session),
												   std::move(invocation),
												   limits,
												   maximum_retained_frames,
												   std::move(clock),
												   std::move(observation),
												   std::move(processes),
												   std::move(durable_resume)},
					 cancellation);
	}

	result<ng1_live_session_driver>
	ng1_live_session_driver::start(ng1_live_driver_configuration configuration,
								   const std::stop_token cancellation)
	{
		if (cancellation.stop_requested())
			return cxxlens::sdk::unexpected(
				error{"provider.cancelled", "ng1-live", "before-start"});
		if (configuration.maximum_retained_frames == 0U ||
			configuration.maximum_retained_frames > std::numeric_limits<std::size_t>::max())
			return cxxlens::sdk::unexpected(
				error{"provider.output-limit", "ng1-live", "frame-count"});
		if (configuration.limits.protocol_major != protocol_v2_major ||
			configuration.limits.minimum_minor != protocol_v2_minor ||
			configuration.limits.maximum_minor != protocol_v2_minor)
			return cxxlens::sdk::unexpected(
				error{"provider.protocol-minor-mismatch", "ng1-live", "protocol-2.0-required"});
		if (!configuration.clock || !configuration.observation || !configuration.processes)
			return cxxlens::sdk::unexpected(
				error{"provider.process-request-invalid", "ng1-live", "missing-port"});
		if (auto valid = configuration.invocation.budget.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (auto valid = configuration.invocation.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (configuration.durable_resume &&
			(!valid_semantic_digest_spelling(configuration.durable_resume->task_input_digest) ||
			 !valid_semantic_digest_spelling(configuration.durable_resume->source_closure_digest)))
			return cxxlens::sdk::unexpected(
				error{"provider.resume-token-stale", "durable_resume", "digest-spelling"});
		if (configuration.durable_resume &&
			configuration.durable_resume->task_input_digest !=
				configuration.session.resume_binding.task_input_digest)
			return cxxlens::sdk::unexpected(
				error{"provider.resume-token-stale", "task_input_digest", "binding-mismatch"});
		if (configuration.durable_resume &&
			configuration.invocation.expected_binary_digest !=
				configuration.session.resume_binding.provider_binary_digest)
			return cxxlens::sdk::unexpected(error{"provider.resume-token-stale",
												  "provider_binary_digest",
												  "process-binding-mismatch"});
		if (configuration.durable_resume &&
			configuration.invocation.sandbox.policy_digest !=
				configuration.session.resume_binding.sandbox_policy_digest)
			return cxxlens::sdk::unexpected(error{"provider.resume-token-stale",
												  "sandbox_policy_digest",
												  "process-binding-mismatch"});
		if (configuration.durable_resume && configuration.invocation.inherited_channel)
		{
			const auto& channel = *configuration.invocation.inherited_channel;
			if (channel.task_id != configuration.session.resume_binding.task_id ||
				channel.session_id != configuration.session.resume_binding.protocol_session_id ||
				channel.closure_digest != configuration.durable_resume->source_closure_digest)
				return cxxlens::sdk::unexpected(error{"provider.resume-token-stale",
													  "source_closure",
													  "process-channel-binding-mismatch"});
			if (auto valid = channel.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
		}
		auto resume_binding = configuration.session.resume_binding;

		auto session = ng1_session_coordinator::create(std::move(configuration.session));
		if (!session)
			return cxxlens::sdk::unexpected(std::move(session.error()));
		bool process_start_cleanup_attempted{};
		auto cleanup_failed_start = [&]() -> result<void>
		{
			if (process_start_cleanup_attempted)
				return {};
			process_start_cleanup_attempted = true;
			session->fail_before_worker_start();
			return session->cleanup();
		};
		result<std::unique_ptr<ng1_duplex_process>> process =
			[&]() -> result<std::unique_ptr<ng1_duplex_process>>
		{
			try
			{
				return configuration.processes->start(
					configuration.invocation, configuration.limits, cancellation);
			}
			catch (const std::bad_alloc&)
			{
				auto cleanup = cleanup_failed_start();
				if (!cleanup)
					return cxxlens::sdk::unexpected(std::move(cleanup.error()));
				return cxxlens::sdk::unexpected(
					process_start_exception_error("process-port-allocation-failed"));
			}
			catch (const std::exception&)
			{
				auto cleanup = cleanup_failed_start();
				if (!cleanup)
					return cxxlens::sdk::unexpected(std::move(cleanup.error()));
				return cxxlens::sdk::unexpected(
					process_start_exception_error("process-port-exception"));
			}
			catch (...)
			{
				auto cleanup = cleanup_failed_start();
				if (!cleanup)
					return cxxlens::sdk::unexpected(std::move(cleanup.error()));
				return cxxlens::sdk::unexpected(
					process_start_exception_error("process-port-unknown-exception"));
			}
		}();
		if (!process)
		{
			auto launch_error = std::move(process.error());
			if (auto cleanup = cleanup_failed_start(); !cleanup)
				return cxxlens::sdk::unexpected(std::move(cleanup.error()));
			return cxxlens::sdk::unexpected(std::move(launch_error));
		}
		return ng1_live_session_driver{std::move(*session),
									   std::move(*process),
									   std::move(configuration.clock),
									   std::move(configuration.observation),
									   std::move(resume_binding),
									   std::move(configuration.durable_resume),
									   configuration.maximum_retained_frames,
									   cancellation};
	}

	ng1_live_session_driver::ng1_live_session_driver(
		ng1_session_coordinator session,
		std::unique_ptr<ng1_duplex_process> process,
		std::unique_ptr<ng1_monotonic_clock_port> clock,
		std::unique_ptr<ng1_host_observation_port> observation,
		ng1_resume_binding resume_binding,
		std::optional<ng1_durable_resume_authority> durable_resume,
		const std::uint64_t maximum_retained_frames,
		const std::stop_token cancellation) noexcept
		: session_{std::move(session)}, adapter_{session_}, process_{std::move(process)},
		  clock_{std::move(clock)}, observation_{std::move(observation)},
		  resume_binding_{std::move(resume_binding)}, durable_resume_{std::move(durable_resume)},
		  maximum_retained_frames_{maximum_retained_frames}, cancellation_{cancellation}
	{
	}

	ng1_live_session_driver::~ng1_live_session_driver() noexcept
	{
		try
		{
			if (process_)
			{
				if (!ended_)
				{
					auto terminated = process_->terminate(process_status::cancelled);
					if (terminated)
					{
						ended_ = true;
						(void)synchronize_process_outcome(*terminated);
					}
				}
				// Reset even when the process port returned an effect error.  The concrete system
				// process destructor owns the final process-group kill/reap fallback.
				process_.reset();
				ended_ = true;
			}

			if (!session_.cleaned())
			{
				if (session_.state() != ng1_recovery_state::completed &&
					session_.state() != ng1_recovery_state::failed)
					(void)session_.reject_output();
				if (session_.state() == ng1_recovery_state::completed ||
					session_.state() == ng1_recovery_state::failed)
					(void)session_.cleanup();
			}
		}
		catch (...)
		{
			// Destruction is a last-resort fail-closed boundary.  The process object has already
			// been given an opportunity to kill/reap, and no exception may escape this path.
		}
	}

	ng1_live_session_driver::ng1_live_session_driver(ng1_live_session_driver&& other) noexcept
		: session_{std::move(other.session_)}, adapter_{session_},
		  process_{std::move(other.process_)}, clock_{std::move(other.clock_)},
		  observation_{std::move(other.observation_)},
		  resume_binding_{std::move(other.resume_binding_)},
		  durable_resume_{std::move(other.durable_resume_)},
		  provider_frames_{std::move(other.provider_frames_)},
		  last_provider_receipt_{std::move(other.last_provider_receipt_)},
		  latest_checkpoint_{std::move(other.latest_checkpoint_)},
		  maximum_retained_frames_{other.maximum_retained_frames_},
		  cancellation_{other.cancellation_},
		  task_accepted_observed_{other.task_accepted_observed_},
		  bound_output_group_open_{other.bound_output_group_open_},
		  bound_output_group_sealed_{other.bound_output_group_sealed_},
		  resume_token_published_{other.resume_token_published_},
		  provider_terminal_observed_{other.provider_terminal_observed_}, ended_{other.ended_}
	{
		other.ended_ = true;
	}

	result<void> ng1_live_session_driver::ensure_open(const std::string_view operation) const
	{
		if (ended_ || !process_)
			return cxxlens::sdk::unexpected(
				driver_error(std::string{operation}, "channel-not-open"));
		return {};
	}

	result<void> ng1_live_session_driver::synchronize_process_outcome(const process_output& output)
	{
		if (session_.state() == ng1_recovery_state::running && is_clean_process_completion(output))
			return {};
		switch (session_.state())
		{
			case ng1_recovery_state::running:
				return session_.observe_worker_exit();
			case ng1_recovery_state::heartbeat_timeout:
			case ng1_recovery_state::progress_rate_failure:
				return session_.confirm_worker_kill();
			case ng1_recovery_state::cancel_requested:
			case ng1_recovery_state::worker_killed:
			case ng1_recovery_state::resume_replay:
			case ng1_recovery_state::resumed:
			case ng1_recovery_state::completed:
			case ng1_recovery_state::failed:
				return {};
		}
		return cxxlens::sdk::unexpected(driver_error("process-outcome", "unknown-recovery-state"));
	}

	result<std::uint64_t> ng1_live_session_driver::now_ns() const
	{
		if (!clock_)
			return cxxlens::sdk::unexpected(
				error{"provider.heartbeat-clock-invalid", "clock", "missing"});
		return clock_->now_ns();
	}

	result<ng1_host_observation> ng1_live_session_driver::current_observation() const
	{
		if (!observation_)
			return cxxlens::sdk::unexpected(
				error{"provider.heartbeat-clock-invalid", "observation", "missing"});
		return observation_->current();
	}

	result<void> ng1_live_session_driver::observe_output_group_state(const frame& value)
	{
		if (!durable_resume_)
			return {};
		if (value.type == message_type::resume)
		{
			if (!latest_checkpoint_)
				return cxxlens::sdk::unexpected(error{"provider.resume-replay-invalid",
													  "resume",
													  "published-before-durable-frontier"});
			if (value.protocol_major != protocol_v2_major ||
				value.protocol_minor != protocol_v2_minor ||
				value.stream_id != resume_binding_.stream_id || value.flags != 0U ||
				!value.payload.empty())
				return cxxlens::sdk::unexpected(
					error{"provider.resume-token-stale", "resume", "frame-binding"});
			if (resume_token_published_)
				return cxxlens::sdk::unexpected(
					error{"provider.resume-token-stale", "resume", "duplicate-publication"});
			auto control = decode_ng1_resume_control(value.control);
			if (!control)
				return cxxlens::sdk::unexpected(std::move(control.error()));
			auto token = control->to_validation_token();
			if (!token)
				return cxxlens::sdk::unexpected(std::move(token.error()));
			if (token->binding != latest_checkpoint_->binding_ ||
				token->token_generation != latest_checkpoint_->resume_generation_)
				return cxxlens::sdk::unexpected(
					error{"provider.resume-token-stale", "resume", "not-latest-durable-frontier"});
			if (token->highest_contiguous_acked_sequence !=
					latest_checkpoint_->receipt_.highest_contiguous_acked_sequence ||
				token->staged_digest != latest_checkpoint_->receipt_.staged_digest)
				return cxxlens::sdk::unexpected(
					error{"provider.resume-replay-invalid", "resume", "durable-frontier-mismatch"});
			resume_token_published_ = true;
			return {};
		}
		if (value.type == message_type::task_complete || value.type == message_type::task_failed)
		{
			provider_terminal_observed_ = true;
			return {};
		}
		if (value.type == message_type::batch_begin)
		{
			if (!task_accepted_observed_)
				return cxxlens::sdk::unexpected(
					error{"provider.task-binding-mismatch", "output-group", "task-not-accepted"});
			if (value.protocol_major != protocol_v2_major ||
				value.protocol_minor != protocol_v2_minor ||
				value.stream_id != resume_binding_.stream_id || value.flags != 0U ||
				!value.payload.empty())
				return cxxlens::sdk::unexpected(
					error{"provider.task-binding-mismatch", "output-group", "frame-binding"});
			auto metadata = decode_batch_begin_metadata(value.control);
			if (!metadata)
				return cxxlens::sdk::unexpected(std::move(metadata.error()));
			if (metadata->task_id != resume_binding_.task_id ||
				metadata->dependency_group_id != resume_binding_.dependency_group_id ||
				metadata->atomic_output_group_id != resume_binding_.atomic_output_group_id ||
				metadata->batch_id != resume_binding_.batch_id)
				return {};
			if (bound_output_group_open_ || bound_output_group_sealed_)
				return cxxlens::sdk::unexpected(
					driver_error("output-group", "duplicate-or-reopened-batch"));
			bound_output_group_open_ = true;
			return {};
		}
		if (value.type == message_type::batch_end)
		{
			if (value.protocol_major != protocol_v2_major ||
				value.protocol_minor != protocol_v2_minor ||
				value.stream_id != resume_binding_.stream_id || value.flags != 0U)
				return cxxlens::sdk::unexpected(
					error{"provider.task-binding-mismatch", "output-group", "frame-binding"});
			auto metadata = decode_columnar_batch_end(value.control, value.payload);
			if (!metadata)
				return cxxlens::sdk::unexpected(std::move(metadata.error()));
			if (metadata->task_id != resume_binding_.task_id ||
				metadata->dependency_group_id != resume_binding_.dependency_group_id ||
				metadata->atomic_output_group_id != resume_binding_.atomic_output_group_id ||
				metadata->batch_id != resume_binding_.batch_id)
				return {};
			if (!bound_output_group_open_ || bound_output_group_sealed_)
				return cxxlens::sdk::unexpected(
					driver_error("output-group", "batch-end-without-open"));
			bound_output_group_open_ = false;
			bound_output_group_sealed_ = true;
		}
		return {};
	}

	result<void> ng1_live_session_driver::reject_resume_checkpoint(error original_error)
	{
		(void)session_.reject_output();
		return cxxlens::sdk::unexpected(std::move(original_error));
	}

	result<void> ng1_live_session_driver::send_host_frame(const frame& value)
	{
		if (auto open = ensure_open("send_host_frame"); !open)
			return open;
		if (value.type == message_type::resume)
			return cxxlens::sdk::unexpected(
				driver_error("resume", "host-resume-requires-durable-receipt"));
		if (!is_ng1_heartbeat_message(value.type) && value.type != message_type::progress)
			return process_->send_frame(value);

		auto receipt = now_ns();
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		auto observation = current_observation();
		if (!observation)
			return cxxlens::sdk::unexpected(std::move(observation.error()));
		auto admitted = adapter_.observe_host_frame(
			value, *receipt, observation->highest_observed_sequence, observation->staged_digest);
		if (!admitted)
			return cxxlens::sdk::unexpected(std::move(admitted.error()));
		if (!*admitted)
			return cxxlens::sdk::unexpected(driver_error("host-frame", "ng1-control-not-admitted"));
		return process_->send_frame(value);
	}

	result<ng1_live_frame_receipt> ng1_live_session_driver::stamp_provider_frame(frame value)
	{
		auto receipt = now_ns();
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		auto observation = current_observation();
		if (!observation)
			return cxxlens::sdk::unexpected(std::move(observation.error()));
		return ng1_live_frame_receipt{std::move(value),
									  *receipt,
									  observation->highest_observed_sequence,
									  std::move(observation->staged_digest),
									  false};
	}

	result<std::optional<ng1_live_frame_receipt>>
	ng1_live_session_driver::receive_provider_frame(std::stop_token cancellation)
	{
		if (auto open = ensure_open("receive_provider_frame"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		if (!cancellation.stop_possible())
			cancellation = cancellation_;
		auto value = process_->receive_frame(std::move(cancellation));
		if (!value)
			return cxxlens::sdk::unexpected(std::move(value.error()));
		if (!*value)
			return std::optional<ng1_live_frame_receipt>{};
		if (provider_frames_.size() >= maximum_retained_frames_)
			return cxxlens::sdk::unexpected(
				error{"provider.output-limit", "ng1-live", "retained-frame-count"});

		auto receipt = stamp_provider_frame(std::move(**value));
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		provider_frames_.push_back(receipt->value_);
		last_provider_receipt_ = *receipt;
		if (auto group = observe_output_group_state(receipt->value_); !group)
		{
			(void)session_.reject_output();
			return cxxlens::sdk::unexpected(std::move(group.error()));
		}

		if (is_ng1_heartbeat_message(receipt->value_.type) ||
			receipt->value_.type == message_type::progress ||
			receipt->value_.type == message_type::task_accepted)
		{
			bool terminal_progress_sample = false;
			if (receipt->value_.type == message_type::progress)
			{
				auto progress = decode_ng1_progress_control(receipt->value_.control);
				if (progress)
					terminal_progress_sample = progress->completed_units == progress->total_units;
			}
			auto admitted = adapter_.observe_provider_frame(receipt->value_,
															receipt->host_receipt_time_ns_,
															receipt->highest_observed_sequence_,
															receipt->host_staged_digest_,
															terminal_progress_sample);
			if (!admitted)
				return cxxlens::sdk::unexpected(std::move(admitted.error()));
			receipt->ng1_control_admitted_ = *admitted;
			if (receipt->value_.type == message_type::task_accepted && *admitted)
				task_accepted_observed_ = true;
			last_provider_receipt_ = *receipt;
		}
		return std::optional<ng1_live_frame_receipt>{std::move(*receipt)};
	}

	result<void> ng1_live_session_driver::check_liveness()
	{
		if (auto open = ensure_open("check_liveness"); !open)
			return open;
		auto now = now_ns();
		if (!now)
			return cxxlens::sdk::unexpected(std::move(now.error()));
		return session_.check_liveness(*now);
	}

	result<void> ng1_live_session_driver::append_durable_spill(const ng1_spill_record& record)
	{
		if (auto open = ensure_open("append_durable_spill"); !open)
			return open;
		if (!durable_resume_)
			return cxxlens::sdk::unexpected(
				error{"provider.recovery-failed", "durable_resume", "authority-missing"});
		if (!bound_output_group_open_ || bound_output_group_sealed_ || provider_terminal_observed_)
			return cxxlens::sdk::unexpected(driver_error("durable-spill", "output-group-not-open"));
		return session_.append_spill(record);
	}

	result<ng1_durable_spill_checkpoint> ng1_live_session_driver::checkpoint_durable_spill(
		const std::uint64_t highest_contiguous_acked_sequence,
		const std::uint64_t resume_generation)
	{
		if (auto open = ensure_open("checkpoint_durable_spill"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		if (!durable_resume_)
			return cxxlens::sdk::unexpected(
				error{"provider.recovery-failed", "durable_resume", "authority-missing"});
		if (bound_output_group_open_ || !bound_output_group_sealed_ || provider_terminal_observed_)
			return cxxlens::sdk::unexpected(
				driver_error("durable-spill", "output-group-not-sealed"));
		auto observation = current_observation();
		if (!observation)
			return cxxlens::sdk::unexpected(std::move(observation.error()));
		auto receipt = session_.fsync_spill(highest_contiguous_acked_sequence,
											observation->highest_observed_sequence,
											observation->staged_digest,
											resume_generation);
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		ng1_durable_spill_checkpoint checkpoint{std::move(*receipt),
												resume_binding_,
												*durable_resume_,
												observation->highest_observed_sequence,
												resume_generation};
		latest_checkpoint_ = checkpoint;
		resume_token_published_ = false;
		return checkpoint;
	}

	result<void>
	ng1_live_session_driver::accept_provider_resume(const ng1_live_frame_receipt& receipt,
													const ng1_durable_spill_checkpoint& checkpoint)
	{
		if (!ended_ || session_.state() != ng1_recovery_state::worker_killed)
			return cxxlens::sdk::unexpected(
				error{"provider.recovery-failed", "resume", "worker-not-terminated"});
		if (!last_provider_receipt_ || receipt.value_.type != message_type::resume ||
			!same_frame(receipt.value_, last_provider_receipt_->value_) ||
			receipt.host_receipt_time_ns_ != last_provider_receipt_->host_receipt_time_ns_)
			return reject_resume_checkpoint(
				error{"provider.resume-token-stale", "resume", "receipt-is-not-latest"});
		if (!durable_resume_ || !latest_checkpoint_ || checkpoint.binding_ != resume_binding_ ||
			checkpoint.authority_ != *durable_resume_ || checkpoint != *latest_checkpoint_)
			return reject_resume_checkpoint(
				error{"provider.resume-token-stale", "checkpoint", "not-latest-or-foreign"});
		if (bound_output_group_open_ || !bound_output_group_sealed_ || provider_terminal_observed_)
			return reject_resume_checkpoint(
				error{"provider.resume-token-stale", "checkpoint", "open-or-terminal"});
		return adapter_.accept_provider_resume_frame(receipt.value_,
													 receipt.host_receipt_time_ns_,
													 checkpoint.receipt_,
													 false,
													 false,
													 checkpoint.highest_observed_sequence_);
	}

	result<process_output> ng1_live_session_driver::finish(std::stop_token cancellation)
	{
		if (auto open = ensure_open("finish"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		if (!cancellation.stop_possible())
			cancellation = cancellation_;
		auto output = process_->finish(std::move(cancellation));
		if (!output)
			return cxxlens::sdk::unexpected(std::move(output.error()));
		ended_ = true;
		if (auto synchronized = synchronize_process_outcome(*output); !synchronized)
			return cxxlens::sdk::unexpected(std::move(synchronized.error()));
		return output;
	}

	result<process_output> ng1_live_session_driver::terminate(const process_status status)
	{
		if (auto open = ensure_open("terminate"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		auto output = process_->terminate(status);
		if (!output)
			return cxxlens::sdk::unexpected(std::move(output.error()));
		ended_ = true;
		if (auto synchronized = synchronize_process_outcome(*output); !synchronized)
			return cxxlens::sdk::unexpected(std::move(synchronized.error()));
		return output;
	}

	result<void> ng1_live_session_driver::cleanup()
	{
		if (!ended_)
			return cxxlens::sdk::unexpected(driver_error("cleanup", "process-not-ended"));
		return session_.cleanup();
	}
} // namespace cxxlens::sdk::provider::detail
