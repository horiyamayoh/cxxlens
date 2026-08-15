#include <string_view>
#include <utility>

#include "provider_ng1_session_internal.hpp"

namespace cxxlens::sdk::provider::detail
{
	namespace
	{
		constexpr std::string_view semantic_digest_prefix{"semantic-v2:sha256:"};

		[[nodiscard]] bool valid_semantic_digest(const std::string_view value) noexcept
		{
			if (!value.starts_with(semantic_digest_prefix) ||
				value.size() != semantic_digest_prefix.size() + 64U)
				return false;
			for (const auto byte : value.substr(semantic_digest_prefix.size()))
				if (!((byte >= '0' && byte <= '9') || (byte >= 'a' && byte <= 'f')))
					return false;
			return true;
		}

		[[nodiscard]] error session_error(const std::string_view field,
										  const std::string_view detail)
		{
			return {"provider.protocol-state-invalid", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] error recovery_error(const std::string_view field,
										   const std::string_view detail)
		{
			return {"provider.recovery-failed", std::string{field}, std::string{detail}};
		}

		[[nodiscard]] bool bindings_match(const ng1_session_configuration& configuration) noexcept
		{
			const auto& heartbeat = configuration.heartbeat_binding;
			const auto& resume = configuration.resume_binding;
			const auto& spill = configuration.spill_binding;
			return heartbeat.provider_id == resume.provider_id &&
				heartbeat.provider_version == resume.provider_version &&
				heartbeat.provider_id == spill.provider_id &&
				heartbeat.protocol_session_id == resume.protocol_session_id &&
				heartbeat.protocol_session_id == spill.protocol_session_id &&
				heartbeat.task_id == resume.task_id && heartbeat.task_id == spill.task_id &&
				heartbeat.stream_id == resume.stream_id && heartbeat.stream_id == spill.stream_id &&
				configuration.dependency_group_id == resume.dependency_group_id &&
				configuration.dependency_group_id == spill.dependency_group_id &&
				resume.atomic_output_group_id == spill.atomic_output_group_id &&
				resume.batch_id == spill.batch_id;
		}

		[[nodiscard]] bool is_running(const ng1_recovery_state state) noexcept
		{
			return state == ng1_recovery_state::running;
		}
	} // namespace

	result<ng1_output_validation_receipt>
	make_ng1_output_validation_receipt(std::string task_id,
									   const sealed_provider_transcript& sealed)
	{
		if (task_id.empty() || task_id.contains('\0'))
			return unexpected(session_error("output.task_id", "invalid"));
		auto digest =
			provider_sealed_transcript_receipt_digest(task_id, "provider.success", sealed);
		if (!digest)
			return unexpected(std::move(digest.error()));
		return ng1_output_validation_receipt{std::move(task_id), std::move(*digest)};
	}

	result<ng1_replay_validation_receipt>
	make_ng1_replay_validation_receipt(const ng1_output_validation_receipt& output,
									   const std::uint64_t first_sequence,
									   const std::uint64_t replayed_records,
									   std::string replay_digest)
	{
		if (first_sequence == 0U)
			return unexpected(session_error("replay.first_sequence", "zero"));
		if (!valid_semantic_digest(replay_digest))
			return unexpected(session_error("replay.replay_digest", "semantic-v2"));
		return ng1_replay_validation_receipt{std::string{output.task_id()},
											 std::string{output.sealed_transcript_digest()},
											 first_sequence,
											 replayed_records,
											 std::move(replay_digest)};
	}

	result<ng1_session_coordinator>
	ng1_session_coordinator::create(ng1_session_configuration configuration)
	{
		if (!configuration.spill_storage)
			return unexpected(session_error("spill_storage", "missing-port"));
		if (!bindings_match(configuration))
			return unexpected(session_error("binding", "cross-surface-mismatch"));

		const auto task_id = configuration.heartbeat_binding.task_id;
		auto heartbeat = ng1_heartbeat_state::create(configuration.heartbeat_binding,
													 configuration.host_started_at_ns);
		if (!heartbeat)
			return unexpected(std::move(heartbeat.error()));
		auto progress = ng1_progress_state::create(configuration.resume_binding.task_id,
												   std::move(configuration.dependency_group_id),
												   configuration.host_started_at_ns);
		if (!progress)
			return unexpected(std::move(progress.error()));
		auto recovery = ng1_recovery_adapter::create(configuration.resume_binding);
		if (!recovery)
			return unexpected(std::move(recovery.error()));
		auto spill = ng1_spill_staging_session::create(std::move(configuration.spill_binding),
													   std::move(configuration.spill_storage));
		if (!spill)
			return unexpected(std::move(spill.error()));
		return ng1_session_coordinator{std::move(task_id),
									   std::move(*heartbeat),
									   std::move(*progress),
									   std::move(*recovery),
									   std::move(*spill)};
	}

	result<void> ng1_session_coordinator::ensure_open(const std::string_view operation) const
	{
		if (cleaned_ || spill_.cleaned())
			return unexpected(recovery_error(operation, "session-cleaned"));
		if (poisoned_ || spill_.poisoned())
			return unexpected(recovery_error(operation, "session-poisoned"));
		return {};
	}

	result<void>
	ng1_session_coordinator::admit_host_receipt(const std::uint64_t host_receipt_time_ns) const
	{
		if (last_host_receipt_time_ns_ && host_receipt_time_ns < *last_host_receipt_time_ns_)
			return unexpected(error{"provider.heartbeat-clock-invalid",
									"host_receipt_time_ns",
									"session-frontier-backwards"});
		return {};
	}

	result<void> ng1_session_coordinator::poison(error original_error)
	{
		poisoned_ = true;
		return unexpected(std::move(original_error));
	}

	result<void> ng1_session_coordinator::reject_heartbeat(error original_error)
	{
		if (is_running(recovery_.state()))
		{
			result<void> transition;
			if (original_error.code == "provider.heartbeat-timeout")
				transition = recovery_.observe_heartbeat_timeout();
			else
				transition = recovery_.reject_heartbeat_clock();
			if (!transition)
				return unexpected(std::move(transition.error()));
		}
		return unexpected(std::move(original_error));
	}

	result<void> ng1_session_coordinator::reject_progress(error original_error)
	{
		if (is_running(recovery_.state()))
		{
			result<void> transition;
			if (original_error.code == "provider.progress-rate")
				transition = recovery_.observe_progress_rate_failure();
			else
				transition = recovery_.reject_heartbeat_clock();
			if (!transition)
				return unexpected(std::move(transition.error()));
		}
		return unexpected(std::move(original_error));
	}

	result<void> ng1_session_coordinator::reject_resume(error original_error)
	{
		return recovery_.reject_durable_resume(std::move(original_error));
	}

	result<void> ng1_session_coordinator::reject_replay(error original_error)
	{
		return recovery_.reject_replay(std::move(original_error));
	}

	result<void> ng1_session_coordinator::reject_output(error original_error)
	{
		if (recovery_.state() == ng1_recovery_state::resumed)
		{
			auto rejected = recovery_.reject_output();
			if (!rejected)
				return rejected;
			return unexpected(std::move(original_error));
		}
		return poison(std::move(original_error));
	}

	result<void>
	ng1_session_coordinator::observe_heartbeat(const ng1_heartbeat_control& control,
											   const ng1_heartbeat_kind expected_kind,
											   const std::uint64_t host_receipt_time_ns,
											   const std::uint64_t highest_observed_sequence,
											   const std::string_view host_staged_digest)
	{
		if (auto open = ensure_open("heartbeat"); !open)
			return open;
		if (!is_running(recovery_.state()))
			return unexpected(recovery_error("state", "heartbeat-not-accepted"));
		if (control.kind != expected_kind)
			return reject_heartbeat(session_error("heartbeat.kind", "direction-mismatch"));
		auto sample = control.to_validation_sample(host_receipt_time_ns);
		if (!sample)
			return reject_heartbeat(std::move(sample.error()));
		if (auto monotonic = admit_host_receipt(host_receipt_time_ns); !monotonic)
			return reject_heartbeat(std::move(monotonic.error()));
		if (auto accepted =
				heartbeat_.accept(*sample, highest_observed_sequence, host_staged_digest);
			!accepted)
			return reject_heartbeat(std::move(accepted.error()));
		last_host_receipt_time_ns_ = host_receipt_time_ns;
		return {};
	}

	result<void>
	ng1_session_coordinator::observe_host_probe(const ng1_heartbeat_control& control,
												const std::uint64_t host_receipt_time_ns,
												const std::uint64_t highest_observed_sequence,
												const std::string_view host_staged_digest)
	{
		return observe_heartbeat(control,
								 ng1_heartbeat_kind::probe,
								 host_receipt_time_ns,
								 highest_observed_sequence,
								 host_staged_digest);
	}

	result<void>
	ng1_session_coordinator::observe_provider_ack(const ng1_heartbeat_control& control,
												  const std::uint64_t host_receipt_time_ns,
												  const std::uint64_t highest_observed_sequence,
												  const std::string_view host_staged_digest)
	{
		return observe_heartbeat(control,
								 ng1_heartbeat_kind::ack,
								 host_receipt_time_ns,
								 highest_observed_sequence,
								 host_staged_digest);
	}

	result<void> ng1_session_coordinator::check_liveness(const std::uint64_t now_ns)
	{
		if (auto open = ensure_open("liveness"); !open)
			return open;
		if (!is_running(recovery_.state()))
			return unexpected(recovery_error("state", "liveness-not-checked"));
		if (auto monotonic = admit_host_receipt(now_ns); !monotonic)
			return reject_heartbeat(std::move(monotonic.error()));
		auto checked = heartbeat_.check_liveness(now_ns);
		if (!checked)
		{
			if (checked.error().code == "provider.heartbeat-timeout")
				last_host_receipt_time_ns_ = now_ns;
			return reject_heartbeat(std::move(checked.error()));
		}
		last_host_receipt_time_ns_ = now_ns;
		return {};
	}

	result<void> ng1_session_coordinator::observe_progress(const ng1_progress_control& control,
														   const std::uint64_t host_receipt_time_ns,
														   const bool terminal_sample)
	{
		if (auto open = ensure_open("progress"); !open)
			return open;
		if (!is_running(recovery_.state()))
			return unexpected(recovery_error("state", "progress-not-accepted"));
		auto sample = control.to_validation_sample(host_receipt_time_ns);
		if (!sample)
			return reject_progress(std::move(sample.error()));
		if (auto monotonic = admit_host_receipt(host_receipt_time_ns); !monotonic)
			return reject_progress(std::move(monotonic.error()));
		if (auto observed = progress_.observe(*sample, terminal_sample); !observed)
			return reject_progress(std::move(observed.error()));
		last_host_receipt_time_ns_ = host_receipt_time_ns;
		if (terminal_sample)
			progress_terminal_ = true;
		return {};
	}

	result<void> ng1_session_coordinator::append_spill(const ng1_spill_record& record)
	{
		if (auto open = ensure_open("spill"); !open)
			return open;
		if (!is_running(recovery_.state()) || progress_terminal_)
			return unexpected(recovery_error("state", "spill-not-open"));
		auto appended = spill_.append(record);
		if (!appended)
			return poison(std::move(appended.error()));
		return {};
	}

	result<ng1_spill_fsync_receipt>
	ng1_session_coordinator::fsync_spill(const std::uint64_t highest_contiguous_acked_sequence,
										 const std::uint64_t highest_observed_sequence,
										 std::string staged_digest)
	{
		if (auto open = ensure_open("spill"); !open)
			return unexpected(std::move(open.error()));
		if (!is_running(recovery_.state()))
			return unexpected(recovery_error("state", "spill-not-open"));
		auto receipt = spill_.fsync(
			highest_contiguous_acked_sequence, highest_observed_sequence, std::move(staged_digest));
		if (!receipt)
		{
			poisoned_ = true;
			return unexpected(std::move(receipt.error()));
		}
		latest_fsync_receipt_ = *receipt;
		return receipt;
	}

	result<void> ng1_session_coordinator::observe_worker_exit()
	{
		if (auto open = ensure_open("worker-exit"); !open)
			return open;
		return recovery_.observe_worker_exit();
	}

	result<void> ng1_session_coordinator::observe_heartbeat_timeout()
	{
		if (auto open = ensure_open("heartbeat-timeout"); !open)
			return open;
		return recovery_.observe_heartbeat_timeout();
	}

	result<void> ng1_session_coordinator::observe_progress_rate_failure()
	{
		if (auto open = ensure_open("progress-rate"); !open)
			return open;
		return recovery_.observe_progress_rate_failure();
	}

	result<void> ng1_session_coordinator::confirm_worker_kill()
	{
		if (auto open = ensure_open("worker-kill"); !open)
			return open;
		return recovery_.confirm_worker_kill();
	}

	result<void>
	ng1_session_coordinator::accept_durable_resume(const ng1_resume_control& control,
												   const ng1_spill_fsync_receipt& receipt,
												   const bool open_dependency_group,
												   const bool terminal,
												   const std::uint64_t highest_observed_sequence)
	{
		if (auto open = ensure_open("resume"); !open)
			return open;
		if (recovery_.state() != ng1_recovery_state::worker_killed)
			return unexpected(recovery_error("state", "resume-not-pending"));
		if (!latest_fsync_receipt_)
			return reject_resume(error{"provider.resume-replay-invalid",
									   "fsync_receipt",
									   "coordinator-observation-missing"});
		if (*latest_fsync_receipt_ != receipt)
			return reject_resume(
				error{"provider.resume-replay-invalid", "fsync_receipt", "not-latest"});
		auto recovered = spill_.recover();
		if (!recovered)
			return reject_resume(std::move(recovered.error()));
		auto recovered_digest = recovered->spill_digest();
		if (!recovered_digest)
			return reject_resume(std::move(recovered_digest.error()));
		if (receipt.spill_digest != *recovered_digest ||
			receipt.total_bytes != recovered->total_bytes() ||
			receipt.total_records != recovered->total_records())
			return reject_resume(
				error{"provider.resume-replay-invalid", "spill_receipt", "prefix-mismatch"});
		auto token = control.to_validation_token();
		if (!token)
			return reject_resume(std::move(token.error()));
		return recovery_.accept_durable_resume(
			*token, receipt, open_dependency_group, terminal, highest_observed_sequence);
	}

	result<std::uint64_t> ng1_session_coordinator::replay_start_sequence() const
	{
		if (auto open = ensure_open("replay"); !open)
			return unexpected(std::move(open.error()));
		return recovery_.replay_start_sequence();
	}

	result<void>
	ng1_session_coordinator::accept_replay(const ng1_replay_validation_receipt& replay_receipt)
	{
		if (auto open = ensure_open("replay"); !open)
			return open;
		if (recovery_.state() != ng1_recovery_state::resume_replay)
			return unexpected(recovery_error("state", "replay-not-pending"));
		if (replay_receipt.task_id() != task_id_)
			return reject_replay(
				error{"provider.resume-replay-invalid", "task_id", "binding-mismatch"});
		auto accepted = recovery_.accept_replay_validated(replay_receipt.first_sequence());
		if (!accepted)
			return accepted;
		replay_output_digest_ = std::string{replay_receipt.sealed_transcript_digest()};
		return {};
	}

	result<void>
	ng1_session_coordinator::seal_output(const ng1_output_validation_receipt& output_receipt)
	{
		if (auto open = ensure_open("output"); !open)
			return open;
		const auto current_state = recovery_.state();
		if (current_state != ng1_recovery_state::running &&
			current_state != ng1_recovery_state::resumed)
			return unexpected(recovery_error("state", "output-not-sealable"));
		if (output_receipt.task_id() != task_id_)
		{
			const auto failure =
				error{"provider.protocol-state-invalid", "output.task_id", "binding-mismatch"};
			return current_state == ng1_recovery_state::resumed ? reject_output(failure)
																: poison(failure);
		}
		if (current_state == ng1_recovery_state::resumed &&
			(!replay_output_digest_ ||
			 *replay_output_digest_ != output_receipt.sealed_transcript_digest()))
			return reject_output(
				error{"provider.replay-invalid", "sealed_transcript_digest", "not-replayed-seal"});
		if (!progress_terminal_)
			return unexpected(error{"provider.progress-rate", "terminal", "missing"});
		if (auto complete = progress_.finish(); !complete)
		{
			if (current_state == ng1_recovery_state::resumed)
				return recovery_.reject_output();
			return poison(std::move(complete.error()));
		}
		if (auto sealed = recovery_.seal_output(); !sealed)
			return sealed;
		(void)heartbeat_.mark_terminal();
		return {};
	}

	result<void> ng1_session_coordinator::reject_output()
	{
		if (auto open = ensure_open("output"); !open)
			return open;
		const auto failure = error{"provider.replay-invalid", "output", "rejected"};
		return reject_output(failure);
	}

	result<void> ng1_session_coordinator::cleanup()
	{
		if (cleaned_)
			return unexpected(recovery_error("cleanup", "already-terminal"));
		cleaned_ = true;
		auto result = spill_.cleanup();
		if (!result)
			poisoned_ = true;
		return result;
	}
} // namespace cxxlens::sdk::provider::detail
