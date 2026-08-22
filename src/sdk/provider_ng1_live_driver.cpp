#include <exception>
#include <limits>
#include <new>
#include <string_view>
#include <utility>

#include "provider_ng1_live_driver_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
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

		[[nodiscard]] result<std::uint64_t> retained_frame_bytes(const frame& value)
		{
			constexpr auto fixed_bytes = static_cast<std::uint64_t>(sizeof(frame));
			const auto control_bytes = static_cast<std::uint64_t>(value.control.size());
			const auto payload_bytes = static_cast<std::uint64_t>(value.payload.size());
			if (control_bytes > std::numeric_limits<std::uint64_t>::max() - fixed_bytes)
				return cxxlens::sdk::unexpected(
					error{"provider.output-limit", "ng1-live", "retained-frame-bytes-overflow"});
			const auto with_control = fixed_bytes + control_bytes;
			if (payload_bytes > std::numeric_limits<std::uint64_t>::max() - with_control)
				return cxxlens::sdk::unexpected(
					error{"provider.output-limit", "ng1-live", "retained-frame-bytes-overflow"});
			return with_control + payload_bytes;
		}
	} // namespace

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
		if (configuration.maximum_retained_bytes < sizeof(frame))
			return cxxlens::sdk::unexpected(
				error{"provider.output-limit", "ng1-live", "frame-bytes"});
		if (configuration.limits.protocol_major != 1U || configuration.limits.minimum_minor != 1U ||
			configuration.limits.maximum_minor != 1U)
			return cxxlens::sdk::unexpected(
				error{"provider.protocol-minor-mismatch", "ng1-live", "minor-one-required"});
		if (!configuration.clock || !configuration.observation || !configuration.processes)
			return cxxlens::sdk::unexpected(
				error{"provider.process-request-invalid", "ng1-live", "missing-port"});
		if (auto valid = configuration.invocation.budget.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));

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
									   configuration.maximum_retained_frames,
									   configuration.maximum_retained_bytes};
	}

	ng1_live_session_driver::ng1_live_session_driver(
		ng1_session_coordinator session,
		std::unique_ptr<ng1_duplex_process> process,
		std::unique_ptr<ng1_monotonic_clock_port> clock,
		std::unique_ptr<ng1_host_observation_port> observation,
		const std::uint64_t maximum_retained_frames,
		const std::uint64_t maximum_retained_bytes) noexcept
		: session_{std::move(session)}, adapter_{session_}, process_{std::move(process)},
		  clock_{std::move(clock)}, observation_{std::move(observation)},
		  maximum_retained_frames_{maximum_retained_frames},
		  maximum_retained_bytes_{maximum_retained_bytes}
	{
	}

	ng1_live_session_driver::ng1_live_session_driver(ng1_live_session_driver&& other) noexcept
		: session_{std::move(other.session_)}, adapter_{session_},
		  process_{std::move(other.process_)}, clock_{std::move(other.clock_)},
		  observation_{std::move(other.observation_)},
		  provider_frames_{std::move(other.provider_frames_)},
		  last_provider_receipt_{std::move(other.last_provider_receipt_)},
		  maximum_retained_frames_{other.maximum_retained_frames_},
		  maximum_retained_bytes_{other.maximum_retained_bytes_},
		  retained_bytes_{other.retained_bytes_}, ended_{other.ended_}
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
	ng1_live_session_driver::receive_provider_frame(const std::stop_token cancellation)
	{
		if (auto open = ensure_open("receive_provider_frame"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		auto value = process_->receive_frame(cancellation);
		if (!value)
			return cxxlens::sdk::unexpected(std::move(value.error()));
		if (!*value)
			return std::optional<ng1_live_frame_receipt>{};
		if (provider_frames_.size() >= maximum_retained_frames_)
			return cxxlens::sdk::unexpected(
				error{"provider.output-limit", "ng1-live", "retained-frame-count"});
		auto frame_bytes = retained_frame_bytes(**value);
		if (!frame_bytes)
			return cxxlens::sdk::unexpected(std::move(frame_bytes.error()));
		if (retained_bytes_ > maximum_retained_bytes_ ||
			*frame_bytes > maximum_retained_bytes_ - retained_bytes_ ||
			*frame_bytes > (maximum_retained_bytes_ - retained_bytes_) / 2U)
			return cxxlens::sdk::unexpected(
				error{"provider.output-limit", "ng1-live", "retained-frame-bytes"});

		auto receipt = stamp_provider_frame(std::move(**value));
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		provider_frames_.push_back(receipt->value_);
		// The latest receipt retains a second copy of the decoded frame in addition to
		// the transcript vector. Count both copies so the bound covers all driver-owned
		// decoded frame storage, not only the eventual transcript snapshot.
		retained_bytes_ += *frame_bytes * 2U;
		last_provider_receipt_ = *receipt;

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

	result<void>
	ng1_live_session_driver::accept_provider_resume(const ng1_live_frame_receipt& receipt,
													const ng1_spill_fsync_receipt& fsync_receipt,
													const bool open_dependency_group,
													const bool terminal)
	{
		if (auto open = ensure_open("accept_provider_resume"); !open)
			return open;
		if (!last_provider_receipt_ || receipt.value_.type != message_type::resume ||
			!same_frame(receipt.value_, last_provider_receipt_->value_) ||
			receipt.host_receipt_time_ns_ != last_provider_receipt_->host_receipt_time_ns_)
			return cxxlens::sdk::unexpected(driver_error("resume", "receipt-is-not-latest"));
		return adapter_.accept_provider_resume_frame(receipt.value_,
													 receipt.host_receipt_time_ns_,
													 fsync_receipt,
													 open_dependency_group,
													 terminal,
													 receipt.highest_observed_sequence_);
	}

	result<process_output> ng1_live_session_driver::finish(const std::stop_token cancellation)
	{
		if (auto open = ensure_open("finish"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		auto output = process_->finish(cancellation);
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

	namespace
	{
		[[nodiscard]] error candidate_error(std::string field, std::string detail)
		{
			return {"provider.protocol-state-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool same_limits(const protocol_limits& left,
									   const protocol_limits& right) noexcept
		{
			return left.max_control_bytes == right.max_control_bytes &&
				left.max_payload_bytes == right.max_payload_bytes &&
				left.protocol_major == right.protocol_major &&
				left.minimum_minor == right.minimum_minor &&
				left.maximum_minor == right.maximum_minor &&
				left.supported_flags == right.supported_flags;
		}
	} // namespace

	result<ng1_live_session_candidate>
	ng1_live_session_candidate::start(ng1_live_session_candidate_configuration configuration,
									  const std::stop_token cancellation)
	{
		if (cancellation.stop_requested())
			return cxxlens::sdk::unexpected(
				error{"provider.cancelled", "ng1-candidate", "before-start"});
		if (!configuration.authority.explicit_ng1_request)
			return cxxlens::sdk::unexpected(error{
				"provider.ng1.implicit-downgrade-denied", "ng1-live", "explicit-request-required"});
		if (configuration.authority.source_closure != ng1_source_closure_authority_status::accepted)
			return cxxlens::sdk::unexpected(error{
				"provider.ng1.capability-unavailable", "source-closure", "authority-not-accepted"});
		if (configuration.authority.hardening != ng1_hardening_authority_status::accepted)
			return cxxlens::sdk::unexpected(error{
				"provider.ng1.capability-unavailable", "ng1-hardening", "authority-not-accepted"});

		const auto& expectation = configuration.host_transcript.expectation;
		if (expectation.provider_manifest.empty() || expectation.provider_manifest.contains('\0'))
			return cxxlens::sdk::unexpected(
				candidate_error("provider_manifest", "missing-or-invalid"));
		if (configuration.driver.limits.protocol_major != 1U ||
			configuration.driver.limits.minimum_minor != 1U ||
			configuration.driver.limits.maximum_minor != 1U)
			return cxxlens::sdk::unexpected(
				error{"provider.protocol-minor-mismatch", "ng1-candidate", "minor-one-required"});
		if (!same_limits(configuration.driver.limits, expectation.limits))
			return cxxlens::sdk::unexpected(
				candidate_error("protocol-limits", "driver-and-host-transcript-mismatch"));

		// Encode and validate the entire host transcript before starting a process.  The
		// duplex channel must not receive a prefix that the shared worker validator would
		// later reject, and the candidate must not invent a second input/credit grammar.
		auto encoded_host_transcript = encode_host_transcript(configuration.host_transcript);
		if (!encoded_host_transcript)
			return cxxlens::sdk::unexpected(std::move(encoded_host_transcript.error()));
		auto host_frames = decode_frame_stream(*encoded_host_transcript, expectation.limits);
		if (!host_frames)
			return cxxlens::sdk::unexpected(std::move(host_frames.error()));
		auto validated = validate_host_transcript(*host_frames, expectation);
		if (!validated)
			return cxxlens::sdk::unexpected(std::move(validated.error()));

		auto driver = ng1_live_session_driver::start(std::move(configuration.driver), cancellation);
		if (!driver)
			return cxxlens::sdk::unexpected(std::move(driver.error()));
		return ng1_live_session_candidate{std::move(*driver),
										  std::move(*host_frames),
										  expectation.provider_manifest,
										  expectation.limits};
	}

	ng1_live_session_candidate::ng1_live_session_candidate(
		ng1_live_session_candidate&& other) noexcept
		: driver_{std::move(other.driver_)}, host_frames_{std::move(other.host_frames_)},
		  provider_manifest_{std::move(other.provider_manifest_)}, limits_{other.limits_},
		  phase_{other.phase_}
	{
		other.phase_ = ng1_live_candidate_phase::failed;
	}

	result<void> ng1_live_session_candidate::ensure_phase(
		const std::string_view operation,
		const std::initializer_list<ng1_live_candidate_phase> allowed) const
	{
		for (const auto expected : allowed)
			if (phase_ == expected)
				return {};
		return cxxlens::sdk::unexpected(
			candidate_error(std::string{operation}, "candidate-phase-invalid"));
	}

	result<void> ng1_live_session_candidate::reject_candidate(std::string field, std::string detail)
	{
		phase_ = ng1_live_candidate_phase::failed;
		return cxxlens::sdk::unexpected(candidate_error(std::move(field), std::move(detail)));
	}

	result<void> ng1_live_session_candidate::validate_provider_hello(const frame& value) const
	{
		if (value.type != message_type::hello || value.stream_id != 1U || value.sequence != 0U ||
			value.protocol_major != limits_.protocol_major ||
			value.protocol_minor != limits_.maximum_minor || value.flags != 0U ||
			!value.payload.empty())
			return cxxlens::sdk::unexpected(candidate_error("hello", "header-or-direction"));
		auto manifest = decode_control_text(value.control);
		if (!manifest)
			return cxxlens::sdk::unexpected(std::move(manifest.error()));
		if (*manifest != provider_manifest_)
			return cxxlens::sdk::unexpected(
				error{"provider.task-binding-mismatch", "provider_manifest", "hello"});
		return {};
	}

	result<void> ng1_live_session_candidate::negotiate(const std::stop_token cancellation)
	{
		if (auto phase = ensure_phase("negotiate", {ng1_live_candidate_phase::awaiting_hello});
			!phase)
			return phase;
		auto hello = driver_.receive_provider_frame(cancellation);
		if (!hello)
			return reject_candidate("hello", "receive-failed");
		if (!hello->has_value())
			return reject_candidate("hello", "truncated-stream");
		if (auto valid = validate_provider_hello(hello->value().value()); !valid)
		{
			phase_ = ng1_live_candidate_phase::failed;
			return valid;
		}
		for (const auto& value : host_frames_)
		{
			if (auto sent = driver_.send_host_frame(value); !sent)
				return reject_candidate("host-handshake", "send-failed");
		}
		phase_ = ng1_live_candidate_phase::running;
		return {};
	}

	result<std::optional<ng1_live_frame_receipt>>
	ng1_live_session_candidate::receive_provider_frame(const std::stop_token cancellation)
	{
		if (auto phase = ensure_phase("receive", {ng1_live_candidate_phase::running}); !phase)
			return cxxlens::sdk::unexpected(std::move(phase.error()));
		auto received = driver_.receive_provider_frame(cancellation);
		if (!received)
			update_phase_after_process_effect();
		return received;
	}

	result<void> ng1_live_session_candidate::send_host_frame(const frame& value)
	{
		if (auto phase = ensure_phase("send", {ng1_live_candidate_phase::running}); !phase)
			return phase;
		auto sent = driver_.send_host_frame(value);
		if (!sent)
			update_phase_after_process_effect();
		return sent;
	}

	result<void> ng1_live_session_candidate::check_liveness()
	{
		if (auto phase = ensure_phase("liveness", {ng1_live_candidate_phase::running}); !phase)
			return phase;
		auto checked = driver_.check_liveness();
		if (!checked)
			update_phase_after_process_effect();
		return checked;
	}

	result<void> ng1_live_session_candidate::append_spill(const ng1_spill_record& record)
	{
		if (auto phase = ensure_phase("spill", {ng1_live_candidate_phase::running}); !phase)
			return phase;
		auto appended = driver_.session().append_spill(record);
		if (!appended)
			update_phase_after_process_effect();
		return appended;
	}

	result<ng1_spill_fsync_receipt>
	ng1_live_session_candidate::fsync_spill(const std::uint64_t highest_contiguous_acked_sequence,
											const std::uint64_t highest_observed_sequence,
											std::string staged_digest,
											const std::uint64_t resume_generation)
	{
		if (auto phase = ensure_phase("spill-fsync", {ng1_live_candidate_phase::running}); !phase)
			return cxxlens::sdk::unexpected(std::move(phase.error()));
		auto receipt = driver_.session().fsync_spill(highest_contiguous_acked_sequence,
													 highest_observed_sequence,
													 std::move(staged_digest),
													 resume_generation);
		if (!receipt)
			update_phase_after_process_effect();
		return receipt;
	}

	result<void>
	ng1_live_session_candidate::accept_provider_resume(const ng1_live_frame_receipt& receipt,
													   const ng1_spill_fsync_receipt& fsync_receipt,
													   const bool open_dependency_group,
													   const bool terminal)
	{
		if (auto phase = ensure_phase("resume", {ng1_live_candidate_phase::recovery}); !phase)
			return phase;
		auto accepted =
			driver_.accept_provider_resume(receipt, fsync_receipt, open_dependency_group, terminal);
		if (!accepted)
			update_phase_after_process_effect();
		return accepted;
	}

	result<std::uint64_t> ng1_live_session_candidate::replay_start_sequence() const
	{
		if (auto phase = ensure_phase("replay", {ng1_live_candidate_phase::recovery}); !phase)
			return cxxlens::sdk::unexpected(std::move(phase.error()));
		return driver_.session().replay_start_sequence();
	}

	result<void>
	ng1_live_session_candidate::accept_replay(const ng1_replay_validation_receipt& receipt)
	{
		if (auto phase = ensure_phase("replay", {ng1_live_candidate_phase::recovery}); !phase)
			return phase;
		auto accepted = driver_.session().accept_replay(receipt);
		if (!accepted)
			update_phase_after_process_effect();
		return accepted;
	}

	result<void>
	ng1_live_session_candidate::seal_output(const ng1_output_validation_receipt& receipt)
	{
		if (auto phase = ensure_phase(
				"output", {ng1_live_candidate_phase::running, ng1_live_candidate_phase::recovery});
			!phase)
			return phase;
		if (!driver_.ended())
			return cxxlens::sdk::unexpected(candidate_error("output", "process-not-ended"));
		auto sealed = driver_.session().seal_output(receipt);
		if (!sealed)
		{
			update_phase_after_process_effect();
			return sealed;
		}
		phase_ = ng1_live_candidate_phase::completed;
		return {};
	}

	result<void> ng1_live_session_candidate::reject_output()
	{
		if (auto phase = ensure_phase(
				"output", {ng1_live_candidate_phase::running, ng1_live_candidate_phase::recovery});
			!phase)
			return phase;
		auto rejected = driver_.session().reject_output();
		if (!rejected)
		{
			update_phase_after_process_effect();
			if (driver_.session().state() != ng1_recovery_state::failed)
				phase_ = ng1_live_candidate_phase::failed;
		}
		else
			phase_ = ng1_live_candidate_phase::failed;
		return rejected;
	}

	result<process_output> ng1_live_session_candidate::finish(const std::stop_token cancellation)
	{
		if (auto phase = ensure_phase("finish", {ng1_live_candidate_phase::running}); !phase)
			return cxxlens::sdk::unexpected(std::move(phase.error()));
		auto output = driver_.finish(cancellation);
		if (!output)
		{
			phase_ = ng1_live_candidate_phase::failed;
			return output;
		}
		update_phase_after_process_effect();
		return output;
	}

	result<process_output> ng1_live_session_candidate::terminate(const process_status status)
	{
		if (auto phase = ensure_phase("terminate",
									  {ng1_live_candidate_phase::awaiting_hello,
									   ng1_live_candidate_phase::running,
									   ng1_live_candidate_phase::recovery,
									   ng1_live_candidate_phase::failed});
			!phase)
			return cxxlens::sdk::unexpected(std::move(phase.error()));
		auto output = driver_.terminate(status);
		if (!output)
		{
			phase_ = ng1_live_candidate_phase::failed;
			return output;
		}
		update_phase_after_process_effect();
		return output;
	}

	result<void> ng1_live_session_candidate::cleanup()
	{
		if (auto phase = ensure_phase("cleanup",
									  {ng1_live_candidate_phase::recovery,
									   ng1_live_candidate_phase::failed,
									   ng1_live_candidate_phase::completed});
			!phase)
			return phase;
		if (driver_.session().cleaned())
			return cxxlens::sdk::unexpected(candidate_error("cleanup", "already-terminal"));
		auto cleaned = driver_.cleanup();
		if (!cleaned)
		{
			phase_ = ng1_live_candidate_phase::failed;
			return cleaned;
		}
		phase_ = driver_.session().state() == ng1_recovery_state::completed
			? ng1_live_candidate_phase::completed
			: ng1_live_candidate_phase::failed;
		return {};
	}

	void ng1_live_session_candidate::update_phase_after_process_effect() noexcept
	{
		switch (driver_.session().state())
		{
			case ng1_recovery_state::worker_killed:
			case ng1_recovery_state::resume_replay:
			case ng1_recovery_state::resumed:
				phase_ = ng1_live_candidate_phase::recovery;
				break;
			case ng1_recovery_state::completed:
				phase_ = ng1_live_candidate_phase::completed;
				break;
			case ng1_recovery_state::failed:
				phase_ = ng1_live_candidate_phase::failed;
				break;
			case ng1_recovery_state::running:
			case ng1_recovery_state::heartbeat_timeout:
			case ng1_recovery_state::progress_rate_failure:
			case ng1_recovery_state::cancel_requested:
				phase_ = ng1_live_candidate_phase::running;
				break;
		}
	}
} // namespace cxxlens::sdk::provider::detail
