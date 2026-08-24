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
				if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f'))
					return false;
			return true;
		}

		[[nodiscard]] bool valid_sha256_digest(const std::string_view value) noexcept
		{
			if (!value.starts_with("sha256:") || value.size() != 71U)
				return false;
			for (const auto byte : value.substr(7U))
				if ((byte < '0' || byte > '9') && (byte < 'a' || byte > 'f'))
					return false;
			return true;
		}

		[[nodiscard]] bool valid_runtime_id(const std::string_view value) noexcept
		{
			return !value.empty() && !value.contains('\0');
		}

		[[nodiscard]] bool
		complete_runtime_provenance(const provider_runtime_provenance& provenance,
									const std::string_view task_id) noexcept
		{
			return valid_runtime_id(provenance.provider_id) &&
				provenance.provider_version.major != 0U &&
				valid_sha256_digest(provenance.provider_binary_digest) &&
				valid_sha256_digest(provenance.provider_semantic_contract_digest) &&
				valid_runtime_id(provenance.protocol_session_id) && provenance.task_id == task_id &&
				valid_semantic_digest(provenance.task_input_digest) &&
				valid_semantic_digest(provenance.normalized_invocation_digest) &&
				valid_semantic_digest(provenance.toolchain_digest) &&
				valid_semantic_digest(provenance.environment_digest) &&
				valid_semantic_digest(provenance.sandbox_policy_digest) &&
				valid_runtime_id(provenance.dependency_group_id) &&
				valid_runtime_id(provenance.atomic_output_group_id) &&
				valid_runtime_id(provenance.batch_id) && provenance.stream_id != 0U;
		}

		[[nodiscard]] std::string_view
		provenance_mismatch_field(const provider_runtime_provenance& observed,
								  const ng1_resume_binding& expected) noexcept
		{
			if (observed.provider_id != expected.provider_id)
				return "provider_id";
			if (observed.provider_version != expected.provider_version)
				return "provider_version";
			if (observed.provider_binary_digest != expected.provider_binary_digest)
				return "provider_binary_digest";
			if (observed.provider_semantic_contract_digest !=
				expected.provider_semantic_contract_digest)
				return "provider_semantic_contract_digest";
			if (observed.protocol_session_id != expected.protocol_session_id)
				return "protocol_session_id";
			if (observed.task_id != expected.task_id)
				return "task_id";
			if (observed.task_input_digest != expected.task_input_digest)
				return "task_input_digest";
			if (observed.normalized_invocation_digest != expected.normalized_invocation_digest)
				return "normalized_invocation_digest";
			if (observed.toolchain_digest != expected.toolchain_digest)
				return "toolchain_digest";
			if (observed.environment_digest != expected.environment_digest)
				return "environment_digest";
			if (observed.sandbox_policy_digest != expected.sandbox_policy_digest)
				return "sandbox_policy_digest";
			if (observed.dependency_group_id != expected.dependency_group_id)
				return "dependency_group_id";
			if (observed.atomic_output_group_id != expected.atomic_output_group_id)
				return "atomic_output_group_id";
			if (observed.batch_id != expected.batch_id)
				return "batch_id";
			if (observed.stream_id != expected.stream_id)
				return "stream_id";
			return {};
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
									   const provider_runtime_receipt& replay_runtime)
	{
		if (replay_runtime.first_frame_sequence() == 0U)
			return unexpected(session_error("replay.first_sequence", "zero"));
		if (auto valid = replay_runtime.validate(); !valid)
			return unexpected(session_error("replay.runtime_receipt", "invalid"));
		if (replay_runtime.sealed_transcript_digest() != output.sealed_transcript_digest())
			return unexpected(session_error("replay.sealed_transcript_digest", "mismatch"));
		if (!valid_semantic_digest(replay_runtime.frame_transcript_digest()))
			return unexpected(session_error("replay.frame_transcript_digest", "semantic-v2"));
		if (!complete_runtime_provenance(replay_runtime.provenance(), output.task_id()))
			return unexpected(session_error("replay.provenance", "incomplete"));
		return ng1_replay_validation_receipt{std::string{output.task_id()},
											 std::string{output.sealed_transcript_digest()},
											 replay_runtime};
	}

	result<ng1_session_coordinator>
	ng1_session_coordinator::create(ng1_session_configuration configuration)
	{
		if (!configuration.spill_storage)
			return unexpected(session_error("spill_storage", "missing-port"));
		if (!bindings_match(configuration))
			return unexpected(session_error("binding", "cross-surface-mismatch"));

		auto task_id = configuration.heartbeat_binding.task_id;
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
									   configuration.resume_binding,
									   std::move(*heartbeat),
									   std::move(*progress),
									   std::move(*recovery),
									   std::move(*spill)};
	}

	ng1_session_coordinator::ng1_session_coordinator(ng1_session_coordinator&& other) noexcept
		: task_id_{std::move(other.task_id_)}, resume_binding_{std::move(other.resume_binding_)},
		  heartbeat_{std::move(other.heartbeat_)}, progress_{std::move(other.progress_)},
		  recovery_{std::move(other.recovery_)}, spill_{std::move(other.spill_)},
		  latest_fsync_receipt_{std::move(other.latest_fsync_receipt_)},
		  last_host_receipt_time_ns_{other.last_host_receipt_time_ns_},
		  replay_output_digest_{std::move(other.replay_output_digest_)},
		  replay_frame_transcript_digest_{std::move(other.replay_frame_transcript_digest_)},
		  task_accepted_{std::exchange(other.task_accepted_, true)},
		  progress_terminal_{std::exchange(other.progress_terminal_, true)},
		  poisoned_{std::exchange(other.poisoned_, true)},
		  cleaned_{std::exchange(other.cleaned_, true)},
		  moved_from_{std::exchange(other.moved_from_, true)}
	{
	}

	result<void> ng1_session_coordinator::ensure_open(const std::string_view operation) const
	{
		if (moved_from_)
			return unexpected(recovery_error(operation, "moved-from"));
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

	result<void>
	ng1_session_coordinator::observe_task_accepted(const task_accepted_metadata& metadata,
												   const std::uint64_t host_receipt_time_ns)
	{
		if (auto open = ensure_open("task-accepted"); !open)
			return open;
		if (task_accepted_)
			return poison(session_error("task_accepted", "duplicate"));
		if (metadata.provider_id != resume_binding_.provider_id ||
			metadata.provider_version != resume_binding_.provider_version.string() ||
			metadata.task_id != task_id_)
			return poison(error{"provider.task-binding-mismatch", "task_accepted", "identity"});
		if (auto monotonic = admit_host_receipt(host_receipt_time_ns); !monotonic)
			return poison(std::move(monotonic.error()));
		heartbeat_.rebase_start(host_receipt_time_ns);
		progress_.rebase_start(host_receipt_time_ns);
		last_host_receipt_time_ns_ = host_receipt_time_ns;
		task_accepted_ = true;
		return {};
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

	// The timestamp and sequence are an ordered protocol observation pair; their types and
	// order are intentionally identical to the provider heartbeat contract.
	// NOLINTBEGIN(bugprone-easily-swappable-parameters)
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
	// NOLINTEND(bugprone-easily-swappable-parameters)

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
										 std::string staged_digest,
										 const std::uint64_t resume_generation)
	{
		if (auto open = ensure_open("spill"); !open)
			return unexpected(std::move(open.error()));
		if (!is_running(recovery_.state()))
			return unexpected(recovery_error("state", "spill-not-open"));
		auto receipt = spill_.fsync(highest_contiguous_acked_sequence,
									highest_observed_sequence,
									std::move(staged_digest),
									resume_generation);
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

	result<void> ng1_session_coordinator::request_cancel()
	{
		if (auto open = ensure_open("cancel-request"); !open)
			return open;
		return recovery_.request_cancel();
	}

	result<void> ng1_session_coordinator::acknowledge_cancel()
	{
		if (auto open = ensure_open("cancel-acknowledged"); !open)
			return open;
		return recovery_.acknowledge_cancel();
	}

	result<void> ng1_session_coordinator::timeout_cancel()
	{
		if (auto open = ensure_open("cancel-timeout"); !open)
			return open;
		return recovery_.timeout_cancel();
	}

	result<void> ng1_session_coordinator::confirm_worker_kill()
	{
		if (auto open = ensure_open("worker-kill"); !open)
			return open;
		return recovery_.confirm_worker_kill();
	}

	void ng1_session_coordinator::fail_before_worker_start() noexcept
	{
		// No worker exists yet, so a worker-exit transition would manufacture a lifecycle event.
		// The coordinator is nevertheless terminal for cleanup purposes and must not let its spill
		// transaction reach the destructor while it is still open.
		poisoned_ = true;
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
		if (auto frontier = spill_.validate_persisted_frontier(receipt, token->token_generation);
			!frontier)
			return reject_resume(std::move(frontier.error()));
		return recovery_.accept_durable_resume(
			*token, receipt, open_dependency_group, terminal, highest_observed_sequence);
	}

	result<void>
	ng1_session_coordinator::restore_durable_resume(const ng1_resume_control& control,
													const ng1_spill_fsync_receipt& receipt,
													const bool open_dependency_group,
													const bool terminal,
													const std::uint64_t highest_observed_sequence)
	{
		if (auto open = ensure_open("restore-resume"); !open)
			return open;
		if (recovery_.state() != ng1_recovery_state::worker_killed)
			return unexpected(recovery_error("state", "resume-not-pending"));
		if (latest_fsync_receipt_)
			return reject_resume(error{"provider.resume-replay-invalid",
									   "fsync_receipt",
									   "coordinator-observation-present"});
		if (auto restored = spill_.restore_from_fsync_receipt(receipt, control.token_generation);
			!restored)
			return reject_resume(std::move(restored.error()));
		latest_fsync_receipt_ = receipt;
		return accept_durable_resume(
			control, receipt, open_dependency_group, terminal, highest_observed_sequence);
	}

	result<void>
	ng1_session_coordinator::replace_spill_for_resume(ng1_spill_staging_session&& replacement,
													  const ng1_spill_fsync_receipt& receipt,
													  const std::uint64_t resume_generation)
	{
		if (auto open = ensure_open("spill-replacement"); !open)
			return open;
		if (recovery_.state() != ng1_recovery_state::worker_killed)
			return unexpected(recovery_error("state", "spill-replacement-not-pending"));
		if (!latest_fsync_receipt_)
			return unexpected(error{"provider.resume-replay-invalid",
									"fsync_receipt",
									"coordinator-observation-missing"});
		if (*latest_fsync_receipt_ != receipt)
			return unexpected(
				error{"provider.resume-replay-invalid", "fsync_receipt", "not-latest"});
		if (auto valid = receipt.validate(); !valid)
			return unexpected(std::move(valid.error()));

		const ng1_spill_resume_frontier frontier{receipt, resume_generation};
		if (auto valid = frontier.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (spill_.total_bytes() != receipt.total_bytes ||
			spill_.total_records() != receipt.total_records ||
			replacement.total_bytes() != receipt.total_bytes ||
			replacement.total_records() != receipt.total_records)
			return unexpected(
				error{"provider.resume-replay-invalid", "spill_receipt", "prefix-size-mismatch"});

		if (auto persisted = spill_.validate_persisted_frontier(receipt, resume_generation);
			!persisted)
			return unexpected(std::move(persisted.error()));
		if (auto persisted = replacement.validate_persisted_frontier(receipt, resume_generation);
			!persisted)
			return unexpected(std::move(persisted.error()));

		auto recovered = replacement.recover();
		if (!recovered)
			return unexpected(std::move(recovered.error()));
		auto recovered_digest = recovered->spill_digest();
		if (!recovered_digest)
			return unexpected(std::move(recovered_digest.error()));
		if (*recovered_digest != receipt.spill_digest ||
			recovered->total_bytes() != receipt.total_bytes ||
			recovered->total_records() != receipt.total_records)
			return unexpected(
				error{"provider.resume-replay-invalid", "spill_receipt", "prefix-mismatch"});

		// The handoff performs its remaining binding/digest comparisons before its sole effect.
		// A failed handoff leaves replacement owned by the caller. On success it retires the old
		// descriptor without unlinking the durable names, making this move assignment noexcept.
		if (auto handed_off = spill_.handoff_cleanup_custody_to(replacement); !handed_off)
			return unexpected(std::move(handed_off.error()));
		spill_ = std::move(replacement);
		return {};
	}

	result<std::uint64_t> ng1_session_coordinator::replay_start_sequence() const
	{
		if (auto open = ensure_open("replay"); !open)
			return unexpected(std::move(open.error()));
		return recovery_.replay_start_sequence();
	}

	result<void> ng1_session_coordinator::accept_replay_frontier(const std::uint64_t first_sequence)
	{
		if (auto open = ensure_open("replay-frontier"); !open)
			return open;
		if (recovery_.state() != ng1_recovery_state::resume_replay)
			return unexpected(recovery_error("state", "replay-not-pending"));
		return recovery_.accept_replay_validated(first_sequence);
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
		const auto& runtime_receipt = replay_receipt.runtime_receipt();
		if (auto valid = runtime_receipt.validate(); !valid)
			return reject_replay(
				error{"provider.resume-replay-invalid", "runtime_receipt", "identity-incomplete"});
		if (runtime_receipt.sealed_transcript_digest() != replay_receipt.sealed_transcript_digest())
			return reject_replay(error{
				"provider.resume-replay-invalid", "sealed_transcript_digest", "identity-mismatch"});
		if (const auto field =
				provenance_mismatch_field(runtime_receipt.provenance(), resume_binding_);
			!field.empty())
			return reject_replay(
				error{"provider.resume-replay-invalid", std::string{field}, "binding-mismatch"});
		if (!valid_semantic_digest(runtime_receipt.frame_transcript_digest()))
			return reject_replay(error{
				"provider.resume-replay-invalid", "frame_transcript_digest", "missing-or-invalid"});
		auto accepted = recovery_.accept_replay_validated(replay_receipt.first_sequence());
		if (!accepted)
			return accepted;
		replay_output_digest_ = std::string{replay_receipt.sealed_transcript_digest()};
		replay_frame_transcript_digest_ = std::string{replay_receipt.frame_transcript_digest()};
		// A shared validated task-complete replay is the terminal-progress receipt for a
		// fresh coordinator. No provider progress sample can be replayed into the new
		// progress clock, so seal_output must use this sealed lifecycle observation.
		progress_terminal_ = true;
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
			 *replay_output_digest_ != output_receipt.sealed_transcript_digest() ||
			 !replay_frame_transcript_digest_ ||
			 !valid_semantic_digest(*replay_frame_transcript_digest_)))
			return reject_output(
				error{"provider.replay-invalid", "replay_receipt", "not-replayed-seal"});
		if (!progress_terminal_)
			return poison(error{"provider.progress-rate", "terminal", "missing"});
		if (current_state == ng1_recovery_state::running)
		{
			if (auto complete = progress_.finish(); !complete)
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
		if (moved_from_)
			return unexpected(recovery_error("cleanup", "moved-from"));
		if (cleaned_)
			return unexpected(recovery_error("cleanup", "already-terminal"));
		if (state() != ng1_recovery_state::completed && state() != ng1_recovery_state::failed)
			return unexpected(recovery_error("cleanup", "state-not-terminal"));
		cleaned_ = true;
		auto result = spill_.cleanup();
		if (!result)
			poisoned_ = true;
		return result;
	}

	result<void> ng1_live_session_adapter::validate_frame_header(const frame& value) const
	{
		if (session_ == nullptr)
			return unexpected(error{"provider.recovery-failed", "session", "missing"});
		if (value.protocol_major != protocol_v2_major || value.protocol_minor != protocol_v2_minor)
			return unexpected(error{"provider.protocol-minor-mismatch", "frame", "ng1"});
		if (value.stream_id != session_->resume_binding_.stream_id)
			return unexpected(error{"provider.task-binding-mismatch", "stream_id", "frame"});
		if (value.flags != 0U)
			return unexpected(error{"provider.protocol-state-invalid", "flags", "ng1-control"});
		if (!value.payload.empty())
			return unexpected(error{"provider.protocol-state-invalid", "payload", "ng1-control"});
		return {};
	}

	result<void> ng1_live_session_adapter::reject_heartbeat(error original_error)
	{
		if (session_ == nullptr)
			return unexpected(std::move(original_error));
		return session_->reject_heartbeat(std::move(original_error));
	}

	result<void> ng1_live_session_adapter::reject_progress(error original_error)
	{
		if (session_ == nullptr)
			return unexpected(std::move(original_error));
		return session_->reject_progress(std::move(original_error));
	}

	result<void> ng1_live_session_adapter::reject_resume(error original_error)
	{
		if (session_ == nullptr)
			return unexpected(std::move(original_error));
		return session_->reject_resume(std::move(original_error));
	}

	result<bool>
	ng1_live_session_adapter::observe_provider_frame(const frame& value,
													 const std::uint64_t host_receipt_time_ns,
													 const std::uint64_t highest_observed_sequence,
													 const std::string_view host_staged_digest,
													 const bool terminal_progress_sample)
	{
		if (!is_ng1_heartbeat_message(value.type) && value.type != message_type::progress &&
			value.type != message_type::task_accepted)
			return false;
		if (auto valid = validate_frame_header(value); !valid)
		{
			if (value.type == message_type::task_accepted)
				return unexpected(std::move(session_->poison(std::move(valid.error())).error()));
			if (value.type == message_type::progress)
				return unexpected(std::move(reject_progress(std::move(valid.error())).error()));
			return unexpected(std::move(reject_heartbeat(std::move(valid.error())).error()));
		}
		if (value.type == message_type::task_accepted)
		{
			auto metadata = decode_task_accepted_metadata(value.control);
			if (!metadata)
				return unexpected(std::move(session_->poison(std::move(metadata.error())).error()));
			if (auto observed = session_->observe_task_accepted(*metadata, host_receipt_time_ns);
				!observed)
				return unexpected(std::move(observed.error()));
			return true;
		}
		if (is_ng1_heartbeat_message(value.type))
		{
			auto control = decode_ng1_heartbeat_control(value.control);
			if (!control)
				return unexpected(std::move(reject_heartbeat(std::move(control.error())).error()));
			if (control->kind != ng1_heartbeat_kind::ack)
				return unexpected(std::move(
					reject_heartbeat(
						error{"provider.protocol-state-invalid", "heartbeat.kind", "provider-ack"})
						.error()));
			if (auto observed = session_->observe_provider_ack(
					*control, host_receipt_time_ns, highest_observed_sequence, host_staged_digest);
				!observed)
				return unexpected(std::move(observed.error()));
			return true;
		}

		auto control = decode_ng1_progress_control(value.control);
		if (!control)
			return unexpected(std::move(reject_progress(std::move(control.error())).error()));
		if (auto observed = session_->observe_progress(
				*control, host_receipt_time_ns, terminal_progress_sample);
			!observed)
			return unexpected(std::move(observed.error()));
		return true;
	}

	result<bool>
	ng1_live_session_adapter::observe_host_frame(const frame& value,
												 const std::uint64_t host_receipt_time_ns,
												 const std::uint64_t highest_observed_sequence,
												 const std::string_view host_staged_digest)
	{
		if (!is_ng1_heartbeat_message(value.type) && value.type != message_type::progress)
			return false;
		if (auto valid = validate_frame_header(value); !valid)
			return unexpected(std::move(reject_heartbeat(std::move(valid.error())).error()));
		if (value.type == message_type::progress)
			return unexpected(std::move(
				reject_progress(
					error{"provider.protocol-state-invalid", "progress", "host-to-provider"})
					.error()));
		auto control = decode_ng1_heartbeat_control(value.control);
		if (!control)
			return unexpected(std::move(reject_heartbeat(std::move(control.error())).error()));
		if (control->kind != ng1_heartbeat_kind::probe)
			return unexpected(std::move(
				reject_heartbeat(
					error{"provider.protocol-state-invalid", "heartbeat.kind", "host-probe"})
					.error()));
		if (auto observed = session_->observe_host_probe(
				*control, host_receipt_time_ns, highest_observed_sequence, host_staged_digest);
			!observed)
			return unexpected(std::move(observed.error()));
		return true;
	}

	result<void> ng1_live_session_adapter::accept_provider_resume_frame(
		const frame& value,
		const std::uint64_t host_receipt_time_ns,
		const ng1_spill_fsync_receipt& receipt,
		const bool open_dependency_group,
		const bool terminal,
		const std::uint64_t highest_observed_sequence)
	{
		if (value.type != message_type::resume)
			return reject_resume(
				error{"provider.protocol-state-invalid", "message_type", "resume"});
		if (auto valid = validate_frame_header(value); !valid)
			return reject_resume(std::move(valid.error()));
		auto control = decode_ng1_resume_control(value.control);
		if (!control)
			return reject_resume(std::move(control.error()));
		if (auto monotonic = session_->admit_host_receipt(host_receipt_time_ns); !monotonic)
			return reject_resume(std::move(monotonic.error()));
		if (auto accepted = session_->accept_durable_resume(
				*control, receipt, open_dependency_group, terminal, highest_observed_sequence);
			!accepted)
			return unexpected(std::move(accepted.error()));
		session_->last_host_receipt_time_ns_ = host_receipt_time_ns;
		return {};
	}
} // namespace cxxlens::sdk::provider::detail
