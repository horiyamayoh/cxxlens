#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_session_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider::detail;
	static_assert(!std::is_move_assignable_v<ng1_session_coordinator>);

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
		auto output = semantic_digest("test.ng1.session", value);
		require(output, "session test digest construction failed");
		return *output;
	}

	[[nodiscard]] result<void> persist_frontier(std::optional<ng1_spill_resume_frontier>& current,
												const ng1_spill_resume_frontier& next)
	{
		if (auto valid = next.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (current &&
			(next.resume_generation <= current->resume_generation ||
			 next.receipt.fsync_sequence <= current->receipt.fsync_sequence))
			return unexpected(
				error{"provider.resume-token-stale", "resume_frontier", "not-increasing"});
		current = next;
		return {};
	}

	[[nodiscard]] std::string manifest_digest(const char fill)
	{
		return std::string{"sha256:"} + std::string(64U, fill);
	}

	class memory_spill_storage : public ng1_spill_storage_port
	{
	  public:
		result<void> append(const std::span<const std::byte> bytes) override
		{
			bytes_.insert(bytes_.end(), bytes.begin(), bytes.end());
			return {};
		}

		result<std::uint64_t> fsync() override
		{
			return ++fsync_sequence_;
		}

		result<std::vector<std::byte>> read_all() const override
		{
			return bytes_;
		}

		result<std::optional<ng1_spill_resume_frontier>> read_resume_frontier() const override
		{
			return frontier_;
		}

		result<void> persist_resume_frontier(const ng1_spill_resume_frontier& frontier) override
		{
			return persist_frontier(frontier_, frontier);
		}

		result<void> cleanup() override
		{
			cleaned_ = true;
			return {};
		}

	  private:
		std::vector<std::byte> bytes_;
		std::uint64_t fsync_sequence_{};
		std::optional<ng1_spill_resume_frontier> frontier_;
		bool cleaned_{};
	};

	struct durable_spill_state
	{
		std::vector<std::byte> bytes;
		std::uint64_t fsync_sequence{};
		std::optional<ng1_spill_resume_frontier> frontier;
	};

	class restartable_spill_storage final : public ng1_spill_storage_port
	{
	  public:
		explicit restartable_spill_storage(std::shared_ptr<durable_spill_state> state)
			: state_{std::move(state)}
		{
		}

		result<void> append(const std::span<const std::byte> bytes) override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "append", "cleaned"});
			state_->bytes.insert(state_->bytes.end(), bytes.begin(), bytes.end());
			return {};
		}

		result<std::uint64_t> fsync() override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "fsync", "cleaned"});
			return ++state_->fsync_sequence;
		}

		result<std::vector<std::byte>> read_all() const override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "read", "cleaned"});
			return state_->bytes;
		}

		result<std::optional<ng1_spill_resume_frontier>> read_resume_frontier() const override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "resume_frontier", "cleaned"});
			return state_->frontier;
		}

		result<void> persist_resume_frontier(const ng1_spill_resume_frontier& frontier) override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "resume_frontier", "cleaned"});
			return persist_frontier(state_->frontier, frontier);
		}

		result<void> cleanup() override
		{
			if (cleaned_)
				return unexpected(error{"provider.recovery-failed", "cleanup", "already"});
			cleaned_ = true;
			return {};
		}

	  private:
		std::shared_ptr<durable_spill_state> state_;
		bool cleaned_{};
	};

	class failing_spill_storage final : public ng1_spill_storage_port
	{
	  public:
		result<void> append(const std::span<const std::byte>) override
		{
			return unexpected(error{"provider.spill-port-failed", "append", "injected"});
		}

		result<std::uint64_t> fsync() override
		{
			return unexpected(error{"provider.spill-port-failed", "fsync", "injected"});
		}

		result<std::vector<std::byte>> read_all() const override
		{
			return unexpected(error{"provider.spill-port-failed", "read", "injected"});
		}

		result<std::optional<ng1_spill_resume_frontier>> read_resume_frontier() const override
		{
			return unexpected(error{"provider.spill-port-failed", "resume_frontier", "injected"});
		}

		result<void> persist_resume_frontier(const ng1_spill_resume_frontier&) override
		{
			return unexpected(error{"provider.spill-port-failed", "resume_frontier", "injected"});
		}

		result<void> cleanup() override
		{
			return {};
		}
	};

	class failing_cleanup_spill_storage final : public memory_spill_storage
	{
	  public:
		result<void> cleanup() override
		{
			return unexpected(error{"provider.spill-port-failed", "cleanup", "injected"});
		}
	};

	enum class throwing_spill_effect
	{
		append,
		fsync,
		read,
		cleanup,
	};

	class throwing_spill_storage final : public memory_spill_storage
	{
	  public:
		explicit throwing_spill_storage(const throwing_spill_effect effect) : effect_{effect} {}

		result<void> append(const std::span<const std::byte> bytes) override
		{
			if (effect_ == throwing_spill_effect::append)
				throw std::runtime_error{"append"};
			return memory_spill_storage::append(bytes);
		}

		result<std::uint64_t> fsync() override
		{
			if (effect_ == throwing_spill_effect::fsync)
				throw std::runtime_error{"fsync"};
			return memory_spill_storage::fsync();
		}

		result<std::vector<std::byte>> read_all() const override
		{
			if (effect_ == throwing_spill_effect::read)
				throw std::runtime_error{"read"};
			return memory_spill_storage::read_all();
		}

		result<void> cleanup() override
		{
			if (effect_ == throwing_spill_effect::cleanup)
				throw std::runtime_error{"cleanup"};
			return memory_spill_storage::cleanup();
		}

	  private:
		throwing_spill_effect effect_;
	};

	struct fixture
	{
		ng1_session_binding heartbeat{
			"provider:test", {1U, 2U, 3U}, "session:test", "task:test", 7U};
		ng1_resume_binding resume{"provider:test",
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
		ng1_spill_binding spill{"provider:test",
								"session:test",
								"task:test",
								"dependency:test",
								"atomic:test",
								"batch:test",
								7U};

		[[nodiscard]] ng1_session_configuration
		configuration(std::unique_ptr<ng1_spill_storage_port> storage = {}) const
		{
			if (!storage)
				storage = std::make_unique<memory_spill_storage>();
			return {heartbeat, "dependency:test", resume, spill, 1'000U, std::move(storage)};
		}

		[[nodiscard]] ng1_heartbeat_control heartbeat_control(const ng1_heartbeat_kind kind,
															  const std::uint64_t sequence,
															  const std::uint64_t timestamp) const
		{
			return {"cxxlens.provider-control.heartbeat.v1",
					kind,
					heartbeat.provider_id,
					heartbeat.provider_version,
					heartbeat.protocol_session_id,
					heartbeat.task_id,
					heartbeat.stream_id,
					sequence,
					timestamp,
					0U,
					digest("staged")};
		}

		[[nodiscard]] ng1_progress_control progress_control(const std::uint64_t sequence,
															const std::uint64_t timestamp,
															const std::uint64_t completed) const
		{
			return {"cxxlens.provider-control.progress.v2",
					"task:test",
					"dependency:test",
					sequence,
					timestamp,
					completed,
					10U};
		}

		[[nodiscard]] ng1_spill_record spill_record() const
		{
			ng1_spill_record record;
			record.task_id = spill.task_id;
			record.dependency_group_id = spill.dependency_group_id;
			record.atomic_output_group_id = spill.atomic_output_group_id;
			record.batch_id = spill.batch_id;
			record.stream_id = spill.stream_id;
			record.payload_bytes = {std::byte{'r'}, std::byte{'o'}, std::byte{'w'}};
			auto payload = ng1_spill_payload_digest(record.payload_bytes);
			require(payload, "spill payload digest construction failed");
			record.payload_digest = *payload;
			auto record_digest = ng1_spill_record_digest(record);
			require(record_digest, "spill record digest construction failed");
			record.record_digest = *record_digest;
			return record;
		}

		[[nodiscard]] ng1_resume_control resume_control(const std::uint64_t generation = 1U) const
		{
			ng1_resume_control control;
			control.kind = ng1_resume_kind::accepted;
			control.binding = resume;
			control.highest_contiguous_acked_sequence = 0U;
			control.staged_digest = digest("staged");
			control.token_generation = generation;
			ng1_resume_token token{control.schema,
								   control.kind,
								   control.binding,
								   control.highest_contiguous_acked_sequence,
								   control.staged_digest,
								   control.token_generation,
								   {}};
			auto token_digest = ng1_resume_token_digest(token);
			require(token_digest, "resume token digest construction failed");
			control.token_digest = *token_digest;
			return control;
		}

		[[nodiscard]] sealed_provider_transcript sealed_transcript() const
		{
			relation_descriptor descriptor;
			descriptor.id = "descriptor:test";
			descriptor.descriptor_digest = digest("descriptor");
			sealed_provider_batch_replay batch;
			batch.task_id = "task:test";
			batch.descriptor_id = descriptor.id;
			batch.descriptor_digest = descriptor.descriptor_digest;
			batch.dependency_group_id = "dependency:test";
			batch.atomic_output_group_id = "atomic:test";
			batch.batch_id = "batch:test";
			provider::columnar_batch_end batch_end{batch.task_id,
												   batch.dependency_group_id,
												   batch.atomic_output_group_id,
												   batch.batch_id,
												   batch.descriptor_id,
												   batch.descriptor_digest,
												   0U,
												   {},
												   {},
												   {}};
			batch.batch_digest = provider::columnar_batch_digest(batch_end);
			std::vector<relation_descriptor> descriptors{descriptor};
			auto transcript = rehydrate_provider_transcript(
				"task:test",
				descriptors,
				std::vector<sealed_provider_batch_replay>{std::move(batch)},
				std::vector<provider::coverage_unit>{{"task", "task:test", "covered", {}}},
				{},
				{});
			require(transcript, "shared transcript fixture construction failed");
			return std::move(*transcript);
		}

		[[nodiscard]] ng1_output_validation_receipt output_receipt() const
		{
			auto transcript = sealed_transcript();
			auto receipt = make_ng1_output_validation_receipt("task:test", transcript);
			require(receipt, "shared output validation receipt construction failed");
			return *receipt;
		}

		[[nodiscard]] provider_runtime_receipt
		replay_runtime_receipt(const std::string_view mutation = {}) const
		{
			auto transcript = sealed_transcript();
			provider::frame replay_frame;
			replay_frame.type = provider::message_type::task_complete;
			replay_frame.stream_id = 7U;
			replay_frame.sequence = 1U;
			replay_frame.flags = static_cast<std::uint16_t>(provider::frame_flag::end_of_stream);
			std::vector<provider::frame> frames{std::move(replay_frame)};
			provider_runtime_provenance provenance;
			provenance.provider_id = resume.provider_id;
			provenance.provider_version = resume.provider_version;
			provenance.provider_binary_digest = resume.provider_binary_digest;
			provenance.provider_semantic_contract_digest = resume.provider_semantic_contract_digest;
			provenance.protocol_session_id = resume.protocol_session_id;
			provenance.task_id = resume.task_id;
			provenance.task_input_digest = resume.task_input_digest;
			provenance.normalized_invocation_digest = resume.normalized_invocation_digest;
			provenance.toolchain_digest = resume.toolchain_digest;
			provenance.environment_digest = resume.environment_digest;
			provenance.sandbox_policy_digest = resume.sandbox_policy_digest;
			provenance.dependency_group_id = resume.dependency_group_id;
			provenance.atomic_output_group_id = resume.atomic_output_group_id;
			provenance.batch_id = resume.batch_id;
			provenance.stream_id = resume.stream_id;
			if (mutation == "task-input")
				provenance.task_input_digest = digest("different-input");
			else if (mutation == "malformed-task-input")
				provenance.task_input_digest = manifest_digest('c');
			else if (mutation == "malformed-invocation")
				provenance.normalized_invocation_digest = manifest_digest('c');
			else if (mutation == "malformed-toolchain")
				provenance.toolchain_digest = manifest_digest('c');
			else if (mutation == "malformed-environment")
				provenance.environment_digest = manifest_digest('c');
			else if (mutation == "malformed-sandbox")
				provenance.sandbox_policy_digest = manifest_digest('c');
			else if (mutation == "stream")
				provenance.stream_id = resume.stream_id + 1U;
			else if (mutation == "sequence")
				frames.front().sequence = 2U;
			else if (mutation == "zero-sequence")
				frames.front().sequence = 0U;
			auto runtime = make_provider_runtime_receipt(1U,
														 "sha256:" + std::string(64U, 'b'),
														 frames,
														 std::move(provenance),
														 "provider.success",
														 transcript);
			require(runtime, "shared replay runtime receipt construction failed");
			return *runtime;
		}
	};

	void admit_progress_and_heartbeat(ng1_session_coordinator& session, const fixture& values)
	{
		require(session.observe_host_probe(
					values.heartbeat_control(ng1_heartbeat_kind::probe, 0U, 1'000U),
					1'000U,
					0U,
					digest("staged")),
				"session probe was rejected");
		require(session.observe_provider_ack(
					values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'001U),
					1'001U,
					0U,
					digest("staged")),
				"session ACK was rejected");
		require(session.observe_progress(values.progress_control(0U, 1'001U, 0U), 1'001U),
				"initial progress was rejected");
		require(session.observe_progress(values.progress_control(1U, 5'000'001'000U, 5U),
										 5'000'001'000U),
				"progress checkpoint was rejected");
		require(session.observe_progress(
					values.progress_control(2U, 10'000'001'000U, 10U), 10'000'001'000U, true),
				"terminal progress was rejected");
	}

	void test_complete_session_requires_progress_and_cleans_spill()
	{
		const fixture values;
		auto session = ng1_session_coordinator::create(values.configuration());
		require(session, "NG1 session creation failed");
		require(session->append_spill(values.spill_record()), "spill append was rejected");
		auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "spill fsync receipt was not created");
		admit_progress_and_heartbeat(*session, values);
		auto output = values.output_receipt();
		require(session->seal_output(output), "complete session was not sealed");
		require(session->state() == ng1_recovery_state::completed,
				"complete session did not reach completed state");
		require(session->cleanup(), "complete session cleanup failed");
		require(session->cleaned(), "complete session did not record cleanup");

		auto missing_progress = ng1_session_coordinator::create(values.configuration());
		require(missing_progress, "missing-progress session creation failed");
		auto missing = missing_progress->seal_output(output);
		require(!missing && missing.error().code == "provider.progress-rate",
				"session sealed without terminal progress");
		require(missing_progress->cleanup(), "missing-progress cleanup failed");
	}

	void test_live_frame_adapter_binds_wire_controls_to_host_receipts()
	{
		const fixture values;
		auto session = ng1_session_coordinator::create(values.configuration());
		require(session, "live adapter session creation failed");
		ng1_live_session_adapter adapter{*session};

		auto make_frame = [](const provider::message_type type,
							 std::vector<std::byte> control,
							 const std::uint64_t sequence)
		{
			provider::frame value;
			value.type = type;
			value.stream_id = 7U;
			value.sequence = sequence;
			value.protocol_major = 1U;
			value.protocol_minor = 1U;
			value.control = std::move(control);
			return value;
		};

		auto probe_control = encode_ng1_heartbeat_control(
			values.heartbeat_control(ng1_heartbeat_kind::probe, 0U, 1'000U));
		require(probe_control, "live adapter probe encoding failed");
		auto probe = make_frame(ng1_heartbeat_message_type, std::move(*probe_control), 0U);
		auto host_observed = adapter.observe_host_frame(probe, 1'000U, 0U, digest("staged"));
		require(host_observed && *host_observed, "live adapter did not admit host probe");

		auto ack_control = encode_ng1_heartbeat_control(
			values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'001U));
		require(ack_control, "live adapter ACK encoding failed");
		auto ack = make_frame(ng1_heartbeat_message_type, std::move(*ack_control), 1U);
		auto provider_observed = adapter.observe_provider_frame(ack, 1'001U, 0U, digest("staged"));
		require(provider_observed && *provider_observed, "live adapter did not admit provider ACK");

		auto progress_control =
			encode_ng1_progress_control(values.progress_control(0U, 1'001U, 0U));
		require(progress_control, "live adapter progress encoding failed");
		auto progress =
			make_frame(provider::message_type::progress, std::move(*progress_control), 2U);
		provider_observed = adapter.observe_provider_frame(progress, 1'001U, 0U, digest("staged"));
		require(provider_observed && *provider_observed, "live adapter did not admit progress");

		provider::frame ordinary;
		ordinary.type = provider::message_type::batch_begin;
		auto ordinary_result =
			adapter.observe_provider_frame(ordinary, 1'002U, 0U, digest("staged"));
		require(ordinary_result && !*ordinary_result,
				"live adapter consumed an ordinary shared-validator frame");
		auto rejected = session->reject_output();
		require(!rejected, "live adapter incomplete output was not rejected");
		require(session->cleanup(), "live adapter session cleanup failed");

		auto resume_session = ng1_session_coordinator::create(values.configuration());
		require(resume_session, "live adapter resume session creation failed");
		require(resume_session->append_spill(values.spill_record()),
				"live adapter resume spill append failed");
		auto receipt = resume_session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "live adapter resume spill fsync failed");
		require(resume_session->observe_worker_exit(), "live adapter resume worker exit failed");
		ng1_live_session_adapter resume_adapter{*resume_session};
		auto resume_control = encode_ng1_resume_control(values.resume_control());
		require(resume_control, "live adapter resume encoding failed");
		auto resume = make_frame(provider::message_type::resume, std::move(*resume_control), 0U);
		require(
			resume_adapter.accept_provider_resume_frame(resume, 2'000U, *receipt, false, false, 0U),
			"live adapter did not admit durable resume frame");
		require(resume_session->state() == ng1_recovery_state::resume_replay,
				"live adapter resume frame skipped replay state");
		auto rejected_resume = resume_session->reject_output();
		require(!rejected_resume, "live adapter incomplete replay was not rejected");
		require(resume_session->cleanup(), "live adapter resume cleanup failed");
	}

	void test_timeout_kill_resume_checks_local_spill_prefix()
	{
		const fixture values;
		auto session = ng1_session_coordinator::create(values.configuration());
		require(session, "recovery session creation failed");
		require(session->append_spill(values.spill_record()), "recovery spill append failed");
		auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "recovery spill fsync failed");
		admit_progress_and_heartbeat(*session, values);
		auto timeout = session->check_liveness(10'000'001'000U + 5'000'000'000U);
		require(!timeout && timeout.error().code == "provider.heartbeat-timeout",
				"heartbeat timeout was not surfaced");
		require(session->state() == ng1_recovery_state::heartbeat_timeout,
				"heartbeat timeout did not enter recovery state");
		require(session->confirm_worker_kill(), "worker kill confirmation was rejected");
		require(session->state() == ng1_recovery_state::worker_killed,
				"worker kill was not required before resume");
		require(session->accept_durable_resume(values.resume_control(), *receipt, false, false, 0U),
				"durable resume was rejected after local spill recovery");
		auto replay_start = session->replay_start_sequence();
		require(replay_start && *replay_start == 1U, "replay start was not durable ACK plus one");
		auto output = values.output_receipt();
		auto replay = make_ng1_replay_validation_receipt(output, values.replay_runtime_receipt());
		require(replay, "shared replay validation receipt construction failed");
		require(session->accept_replay(*replay), "validated replay was rejected");
		require(session->state() == ng1_recovery_state::resumed,
				"validated replay did not enter resumed state");
		require(session->seal_output(output), "resumed output was not sealed");
		require(session->state() == ng1_recovery_state::completed,
				"resumed output did not complete");
		require(session->cleanup(), "recovery session cleanup failed");

		auto corrupted = ng1_session_coordinator::create(values.configuration());
		require(corrupted, "corrupted-receipt session creation failed");
		require(corrupted->append_spill(values.spill_record()), "corrupted spill append failed");
		auto corrupted_receipt = corrupted->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(corrupted_receipt, "corrupted receipt setup failed");
		require(corrupted->observe_worker_exit(), "worker exit setup failed");
		auto bad_receipt = *corrupted_receipt;
		++bad_receipt.total_records;
		auto rejected = corrupted->accept_durable_resume(
			values.resume_control(), bad_receipt, false, false, 0U);
		require(!rejected && rejected.error().code == "provider.resume-replay-invalid",
				"mismatched local spill receipt was accepted");
		require(corrupted->state() == ng1_recovery_state::failed,
				"mismatched local spill receipt did not fail closed");
		require(corrupted->cleanup(), "corrupted-receipt cleanup failed");
	}

	void test_fresh_coordinator_rehydrates_durable_prefix()
	{
		const fixture values;
		auto durable = std::make_shared<durable_spill_state>();
		auto predecessor = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(predecessor, "durable predecessor session creation failed");
		require(predecessor->append_spill(values.spill_record()),
				"durable predecessor spill append failed");
		auto first_receipt = predecessor->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(first_receipt, "durable predecessor first fsync failed");
		auto latest_receipt = predecessor->fsync_spill(0U, 0U, digest("staged"), 2U);
		require(latest_receipt, "durable predecessor latest fsync failed");
		require(predecessor->observe_worker_exit(), "durable predecessor exit failed");

		auto stale_restart = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(stale_restart, "stale restart session creation failed");
		require(stale_restart->observe_worker_exit(), "stale restart exit observation failed");
		auto stale_receipt = stale_restart->restore_durable_resume(
			values.resume_control(1U), *first_receipt, false, false, 0U);
		require(!stale_receipt && stale_receipt.error().code == "provider.resume-token-stale" &&
					stale_restart->state() == ng1_recovery_state::failed,
				"fresh coordinator accepted an older persisted receipt");
		require(stale_restart->cleanup(), "stale restart cleanup failed");

		auto stale_generation_restart = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(stale_generation_restart, "stale generation session creation failed");
		require(stale_generation_restart->observe_worker_exit(),
				"stale generation exit observation failed");
		auto stale_generation = stale_generation_restart->restore_durable_resume(
			values.resume_control(1U), *latest_receipt, false, false, 0U);
		require(!stale_generation &&
					stale_generation.error().code == "provider.resume-token-stale" &&
					stale_generation_restart->state() == ng1_recovery_state::failed,
				"fresh coordinator accepted an older resume generation for the latest receipt");
		require(stale_generation_restart->cleanup(), "stale generation cleanup failed");

		auto restarted = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(restarted, "fresh restart session creation failed");
		require(restarted->observe_worker_exit(), "fresh restart exit observation failed");
		require(restarted->restore_durable_resume(
					values.resume_control(2U), *latest_receipt, false, false, 0U),
				"fresh coordinator did not restore the durable prefix");
		require(restarted->spill_total_records() == predecessor->spill_total_records() &&
					restarted->spill_total_bytes() == predecessor->spill_total_bytes(),
				"fresh coordinator did not install the exact durable prefix");
		auto replay_start = restarted->replay_start_sequence();
		require(replay_start && *replay_start == 1U,
				"fresh coordinator replay did not start after the durable ACK");

		auto output = values.output_receipt();
		auto replay = make_ng1_replay_validation_receipt(output, values.replay_runtime_receipt());
		require(replay, "fresh coordinator replay receipt construction failed");
		require(restarted->accept_replay(*replay), "fresh coordinator replay was rejected");
		require(restarted->state() == ng1_recovery_state::resumed,
				"fresh coordinator did not enter resumed state");
		require(restarted->seal_output(output),
				"validated restart replay did not establish terminal progress");
		require(restarted->state() == ng1_recovery_state::completed,
				"validated restart replay did not complete output sealing");

		auto predecessor_rejected = predecessor->reject_output();
		require(!predecessor_rejected && predecessor->state() == ng1_recovery_state::failed,
				"predecessor was reusable after restart handoff");
		require(predecessor->cleanup(), "durable predecessor cleanup failed");
		require(restarted->cleanup(), "fresh restart cleanup failed");
	}

	void test_fresh_coordinator_resume_requires_exit_and_exact_receipt()
	{
		const fixture values;
		auto durable = std::make_shared<durable_spill_state>();
		auto predecessor = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(predecessor, "resume-negative predecessor creation failed");
		require(predecessor->append_spill(values.spill_record()),
				"resume-negative predecessor append failed");
		auto receipt = predecessor->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "resume-negative predecessor fsync failed");

		auto no_exit = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(no_exit, "resume-negative fresh session creation failed");
		auto before_exit =
			no_exit->restore_durable_resume(values.resume_control(), *receipt, false, false, 0U);
		require(!before_exit && before_exit.error().field == "state" &&
					no_exit->state() == ng1_recovery_state::running,
				"fresh coordinator accepted resume without worker termination");
		require(no_exit->observe_worker_exit(), "resume-negative exit observation failed");
		auto no_exit_cleanup = no_exit->reject_output();
		require(!no_exit_cleanup && no_exit->state() == ng1_recovery_state::failed,
				"resume-negative session did not fail closed for cleanup");
		require(no_exit->cleanup(), "resume-negative no-exit cleanup failed");

		durable->bytes.back() ^= std::byte{1U};
		auto corrupted = ng1_session_coordinator::create(
			values.configuration(std::make_unique<restartable_spill_storage>(durable)));
		require(corrupted, "resume-negative corrupted session creation failed");
		require(corrupted->observe_worker_exit(), "resume-negative corrupted exit failed");
		auto rejected = corrupted->restore_durable_resume(
			values.resume_control(1U), *receipt, false, false, 0U);
		require(!rejected && rejected.error().code == "provider.spill-corrupt" &&
					corrupted->state() == ng1_recovery_state::failed,
				"fresh coordinator accepted a receipt for a different spill prefix");
		require(corrupted->cleanup(), "resume-negative corrupted cleanup failed");

		require(predecessor->observe_worker_exit(), "resume-negative predecessor exit failed");
		auto predecessor_cleanup = predecessor->reject_output();
		require(!predecessor_cleanup && predecessor->state() == ng1_recovery_state::failed,
				"resume-negative predecessor did not fail closed");
		require(predecessor->cleanup(), "resume-negative predecessor cleanup failed");
	}

	void test_session_rejects_unbound_or_nonmonotonic_observations()
	{
		const fixture values;
		auto wrong_version = values.configuration();
		wrong_version.resume_binding.provider_version = {9U, 9U, 9U};
		auto rejected_version = ng1_session_coordinator::create(std::move(wrong_version));
		require(!rejected_version && rejected_version.error().field == "binding",
				"provider-version mismatch was admitted");

		auto wrong_direction = ng1_session_coordinator::create(values.configuration());
		require(wrong_direction, "wrong-direction session creation failed");
		auto direction = wrong_direction->observe_host_probe(
			values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'000U),
			1'000U,
			0U,
			digest("staged"));
		require(!direction && wrong_direction->state() == ng1_recovery_state::failed,
				"provider ACK crossed the host-probe direction boundary");
		require(wrong_direction->cleanup(), "wrong-direction cleanup failed");

		auto regressed = ng1_session_coordinator::create(values.configuration());
		require(regressed, "regression session creation failed");
		require(regressed->observe_host_probe(
					values.heartbeat_control(ng1_heartbeat_kind::probe, 0U, 1'000U),
					1'000U,
					0U,
					digest("staged")),
				"regression probe setup failed");
		auto regression = regressed->observe_progress(values.progress_control(0U, 999U, 0U), 999U);
		require(!regression && regressed->state() == ng1_recovery_state::failed,
				"session receipt frontier accepted a backwards observation");
		require(regressed->cleanup(), "regression cleanup failed");
	}

	void test_session_requires_local_receipt_and_poisoned_spill_is_terminal()
	{
		const fixture values;
		auto no_receipt = ng1_session_coordinator::create(values.configuration());
		require(no_receipt, "receiptless session creation failed");
		require(no_receipt->observe_worker_exit(), "receiptless worker-exit setup failed");
		auto forged_receipt = no_receipt->accept_durable_resume(
			values.resume_control(), ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!forged_receipt && forged_receipt.error().field == "fsync_receipt" &&
					no_receipt->state() == ng1_recovery_state::failed,
				"resume accepted without a coordinator-owned fsync receipt");
		require(no_receipt->cleanup(), "receiptless cleanup failed");

		auto poisoned = values.configuration();
		poisoned.spill_storage = std::make_unique<failing_spill_storage>();
		auto session = ng1_session_coordinator::create(std::move(poisoned));
		require(session, "poisoned session creation failed");
		auto append = session->append_spill(values.spill_record());
		require(!append && session->poisoned() && session->state() == ng1_recovery_state::failed,
				"spill effect failure did not poison the coordinator");
		auto after_failure = session->append_spill(values.spill_record());
		require(!after_failure, "poisoned spill session remained reusable");
		require(session->cleanup(), "poisoned spill cleanup failed");

		auto cleanup_failure = values.configuration();
		cleanup_failure.spill_storage = std::make_unique<failing_cleanup_spill_storage>();
		auto cleanup_session = ng1_session_coordinator::create(std::move(cleanup_failure));
		require(cleanup_session, "cleanup-failure session creation failed");
		auto early_cleanup = cleanup_session->cleanup();
		require(!early_cleanup && early_cleanup.error().detail == "state-not-terminal",
				"running session permitted cleanup before lifecycle classification");
		auto failed = cleanup_session->observe_host_probe(
			values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'000U),
			1'000U,
			0U,
			digest("staged"));
		require(!failed && cleanup_session->state() == ng1_recovery_state::failed,
				"cleanup-failure setup did not reach failed state");
		auto cleanup = cleanup_session->cleanup();
		require(!cleanup && cleanup_session->poisoned() && cleanup_session->cleaned() &&
					cleanup_session->state() == ng1_recovery_state::failed,
				"cleanup I/O failure did not poison the coordinator");
		auto after_cleanup_failure = cleanup_session->append_spill(values.spill_record());
		require(!after_cleanup_failure, "session remained usable after an unknown cleanup effect");
	}

	void test_session_rejects_bad_replay_and_post_cleanup_calls()
	{
		const fixture values;
		auto session = ng1_session_coordinator::create(values.configuration());
		require(session, "replay-negative session creation failed");
		require(session->append_spill(values.spill_record()),
				"replay-negative spill append failed");
		auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "replay-negative fsync failed");
		require(session->observe_worker_exit(), "replay-negative worker exit failed");
		require(session->accept_durable_resume(values.resume_control(), *receipt, false, false, 0U),
				"replay-negative resume failed");
		auto replay_start = session->replay_start_sequence();
		require(replay_start, "replay-negative start unavailable");
		auto output = values.output_receipt();
		auto bad_replay =
			make_ng1_replay_validation_receipt(output, values.replay_runtime_receipt("sequence"));
		require(bad_replay, "bad replay receipt construction failed");
		auto rejected = session->accept_replay(*bad_replay);
		require(!rejected && session->state() == ng1_recovery_state::failed,
				"reordered replay was accepted");
		require(session->cleanup(), "replay-negative cleanup failed");
		auto after_cleanup = session->append_spill(values.spill_record());
		require(!after_cleanup, "session accepted an append after cleanup");
	}

	void test_moved_from_session_is_terminal_and_cleanup_cannot_reuse_it()
	{
		const fixture values;
		auto source = ng1_session_coordinator::create(values.configuration());
		require(source, "moved-from source session creation failed");
		auto destination = ng1_session_coordinator{std::move(*source)};
		require(source->state() == ng1_recovery_state::failed && source->poisoned() &&
					source->cleaned(),
				"moved-from session did not become a terminal poisoned value");
		auto stale_operation = source->observe_worker_exit();
		require(!stale_operation && stale_operation.error().detail == "moved-from",
				"moved-from session admitted an operation");
		auto stale_cleanup = source->cleanup();
		require(!stale_cleanup && stale_cleanup.error().detail == "moved-from",
				"moved-from session cleanup was not fail-closed");

		auto destination_failure = destination.observe_host_probe(
			values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'000U),
			1'000U,
			0U,
			digest("staged"));
		require(!destination_failure && destination.state() == ng1_recovery_state::failed,
				"moved-to session did not retain its independent lifecycle");
		require(destination.cleanup(), "moved-to session cleanup failed");
	}

	void test_replay_requires_complete_resume_binding()
	{
		const fixture values;
		auto session = ng1_session_coordinator::create(values.configuration());
		require(session, "provenance session creation failed");
		require(session->append_spill(values.spill_record()), "provenance spill append failed");
		auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "provenance fsync failed");
		require(session->observe_worker_exit(), "provenance worker exit failed");
		require(session->accept_durable_resume(values.resume_control(), *receipt, false, false, 0U),
				"provenance durable resume failed");
		auto replay_start = session->replay_start_sequence();
		require(replay_start, "provenance replay start unavailable");
		auto replay = make_ng1_replay_validation_receipt(
			values.output_receipt(), values.replay_runtime_receipt("task-input"));
		require(replay, "provenance replay receipt construction failed");
		auto rejected = session->accept_replay(*replay);
		require(!rejected && rejected.error().field == "task_input_digest" &&
					session->state() == ng1_recovery_state::failed,
				"replay accepted a task-input provenance mutation");
		require(session->cleanup(), "provenance rejection cleanup failed");

		provider::frame generic_frame;
		generic_frame.type = provider::message_type::task_complete;
		generic_frame.stream_id = 7U;
		generic_frame.sequence = 1U;
		generic_frame.flags = static_cast<std::uint16_t>(provider::frame_flag::end_of_stream);
		std::vector<provider::frame> generic_frames{generic_frame};
		auto generic_runtime = make_provider_runtime_receipt(1U,
															 "sha256:" + std::string(64U, 'b'),
															 generic_frames,
															 "task:test",
															 "provider.success",
															 values.sealed_transcript());
		require(generic_runtime, "generic runtime receipt construction failed");
		auto incomplete_replay =
			make_ng1_replay_validation_receipt(values.output_receipt(), *generic_runtime);
		require(!incomplete_replay && incomplete_replay.error().field == "replay.provenance",
				"replay admitted an opaque runtime receipt without complete provenance");

		for (const auto mutation : {std::string_view{"malformed-task-input"},
									std::string_view{"malformed-invocation"},
									std::string_view{"malformed-toolchain"},
									std::string_view{"malformed-environment"},
									std::string_view{"malformed-sandbox"}})
		{
			auto malformed_replay = make_ng1_replay_validation_receipt(
				values.output_receipt(), values.replay_runtime_receipt(mutation));
			require(!malformed_replay && malformed_replay.error().field == "replay.provenance",
					"replay admitted a non-semantic provenance digest");
		}

		auto zero_sequence = make_ng1_replay_validation_receipt(
			values.output_receipt(), values.replay_runtime_receipt("zero-sequence"));
		require(!zero_sequence && zero_sequence.error().field == "replay.first_sequence",
				"replay admitted a sequence-zero runtime receipt");
	}

	void test_spill_port_throw_effects_are_terminal()
	{
		const fixture values;
		for (const auto effect : {throwing_spill_effect::append, throwing_spill_effect::fsync})
		{
			auto configuration = values.configuration();
			configuration.spill_storage = std::make_unique<throwing_spill_storage>(effect);
			auto session = ng1_session_coordinator::create(std::move(configuration));
			require(session, "throwing spill session creation failed");
			auto append = session->append_spill(values.spill_record());
			if (effect == throwing_spill_effect::fsync)
			{
				require(append, "fsync-throw setup append failed");
				auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
				require(!receipt, "fsync throw was swallowed as success");
			}
			else
				require(!append, "append throw was swallowed as success");
			require(session->poisoned() && session->state() == ng1_recovery_state::failed,
					"throwing spill effect did not poison the session");
			require(session->cleanup(), "throwing spill cleanup failed");
		}

		auto configuration = values.configuration();
		configuration.spill_storage =
			std::make_unique<throwing_spill_storage>(throwing_spill_effect::read);
		auto session = ng1_session_coordinator::create(std::move(configuration));
		require(session, "read-throw spill session creation failed");
		require(session->append_spill(values.spill_record()), "read-throw setup append failed");
		auto receipt = session->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "read-throw setup fsync failed");
		require(session->observe_worker_exit(), "read-throw worker exit failed");
		auto resumed =
			session->accept_durable_resume(values.resume_control(), *receipt, false, false, 0U);
		require(!resumed && session->poisoned() && session->state() == ng1_recovery_state::failed,
				"read throw did not poison the session");
		require(session->cleanup(), "read-throw spill cleanup failed");
	}
} // namespace

int main()
{
	test_complete_session_requires_progress_and_cleans_spill();
	test_live_frame_adapter_binds_wire_controls_to_host_receipts();
	test_timeout_kill_resume_checks_local_spill_prefix();
	test_fresh_coordinator_rehydrates_durable_prefix();
	test_fresh_coordinator_resume_requires_exit_and_exact_receipt();
	test_session_rejects_unbound_or_nonmonotonic_observations();
	test_session_requires_local_receipt_and_poisoned_spill_is_terminal();
	test_session_rejects_bad_replay_and_post_cleanup_calls();
	test_moved_from_session_is_terminal_and_cleanup_cannot_reuse_it();
	test_replay_requires_complete_resume_binding();
	test_spill_port_throw_effects_are_terminal();
	return 0;
}
