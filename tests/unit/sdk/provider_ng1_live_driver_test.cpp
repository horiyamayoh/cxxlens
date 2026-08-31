#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <cxxlens/sdk/common.hpp>

#include "sdk/provider_ng1_live_driver_internal.hpp"
#include "sdk/provider_runtime_internal.hpp"
#include "sdk/provider_validation_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::provider;
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
		if (!outcome)
		{
			std::cerr << message << " (" << outcome.error().code << ": " << outcome.error().detail
					  << ")\n";
			std::exit(1);
		}
	}

	void require(const result<void>& outcome, const std::string_view message)
	{
		require(outcome.has_value(), message);
	}

	[[nodiscard]] std::string digest(const std::string_view value)
	{
		auto output = semantic_digest("test.ng1.live-driver", value);
		require(output, "live-driver digest construction failed");
		return *output;
	}

	[[nodiscard]] std::string manifest_digest(const char fill)
	{
		return std::string{"sha256:"} + std::string(64U, fill);
	}

	[[nodiscard]] std::string executable_digest(const std::string& executable)
	{
		std::ifstream input{executable, std::ios::binary};
		require(input.good(), "NG1 system executable could not be opened");
		const std::string bytes{std::istreambuf_iterator<char>{input},
								std::istreambuf_iterator<char>{}};
		require(!input.bad(), "NG1 system executable could not be read");
		return content_digest(std::as_bytes(std::span{bytes}));
	}

	[[nodiscard]] process_invocation system_invocation(const std::string& executable)
	{
		auto policies = builtin_sandbox_policies();
		require(policies.size() == 2U && policies.front().validate().has_value(),
				"NG1 system sandbox policy registry is invalid");
		process_invocation invocation;
		invocation.argv = {executable};
		invocation.budget.wall_ms = 3000U;
		invocation.budget.cpu_ms = 3000U;
		invocation.budget.address_space_bytes = 256U * 1024U * 1024U;
		invocation.budget.transport_bytes = 1024U * 1024U;
		invocation.budget.output_bytes = 1024U * 1024U;
		invocation.budget.open_files = 64U;
		invocation.budget.subprocesses = 1U;
		invocation.sandbox = {sandbox_assurance::enforced, policies.front().policy_digest()};
		invocation.expected_binary_digest = executable_digest(executable);
		return invocation;
	}

	[[nodiscard]] protocol_limits system_protocol_limits()
	{
		protocol_limits limits;
		limits.protocol_major = provider::protocol_v2_major;
		limits.minimum_minor = provider::protocol_v2_minor;
		limits.maximum_minor = provider::protocol_v2_minor;
		return limits;
	}

	[[nodiscard]] process_output clean_process_output()
	{
		return {process_status::exited,
				0,
				0,
				{},
				{},
				{"test-platform",
				 {"test-process-group"},
				 sandbox_assurance::enforced,
				 manifest_digest('c'),
				 manifest_digest('d')},
				{},
				manifest_digest('e')};
	}

	struct spill_state
	{
		bool cleaned{};
	};

	class memory_spill_storage final : public ng1_spill_storage_port
	{
	  public:
		explicit memory_spill_storage(std::shared_ptr<spill_state> state = {})
			: state_{state ? std::move(state) : std::make_shared<spill_state>()}
		{
		}

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

		result<std::unique_ptr<ng1_spill_storage_port>> reopen() const override
		{
			auto output = std::make_unique<memory_spill_storage>(state_);
			output->bytes_ = bytes_;
			output->fsync_sequence_ = fsync_sequence_;
			output->frontier_ = frontier_;
			return std::unique_ptr<ng1_spill_storage_port>{std::move(output)};
		}

		result<void> persist_resume_frontier(const ng1_spill_resume_frontier& value) override
		{
			if (auto valid = value.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (frontier_ &&
				(value.resume_generation <= frontier_->resume_generation ||
				 value.receipt.fsync_sequence <= frontier_->receipt.fsync_sequence))
				return unexpected(
					error{"provider.resume-token-stale", "resume_frontier", "not-increasing"});
			frontier_ = value;
			return {};
		}

		result<void> cleanup() override
		{
			state_->cleaned = true;
			cleaned_ = true;
			return {};
		}

	  private:
		std::vector<std::byte> bytes_;
		std::uint64_t fsync_sequence_{};
		std::optional<ng1_spill_resume_frontier> frontier_;
		std::shared_ptr<spill_state> state_;
		bool cleaned_{};
	};

	struct clock_state
	{
		std::uint64_t now_ns{2'000U};
	};

	class fake_clock final : public ng1_monotonic_clock_port
	{
	  public:
		explicit fake_clock(std::shared_ptr<clock_state> state) : state_{std::move(state)} {}

		result<std::uint64_t> now_ns() const override
		{
			return state_->now_ns;
		}

	  private:
		std::shared_ptr<clock_state> state_;
	};

	struct observation_state
	{
		std::uint64_t highest_observed_sequence{};
		std::string staged_digest{digest("staged")};
	};

	class fake_observation final : public ng1_host_observation_port
	{
	  public:
		explicit fake_observation(std::shared_ptr<observation_state> state)
			: state_{std::move(state)}
		{
		}

		result<ng1_host_observation> current() const override
		{
			return ng1_host_observation{state_->highest_observed_sequence, state_->staged_digest};
		}

	  private:
		std::shared_ptr<observation_state> state_;
	};

	struct process_state
	{
		std::deque<frame> incoming;
		std::vector<frame> sent;
		std::size_t start_count{};
		std::optional<std::size_t> throw_on_start;
		std::optional<error> finish_failure;
		std::optional<error> terminate_failure;
		std::optional<process_output> finish_output;
		std::optional<process_output> terminate_output;
	};

	class fake_process final : public ng1_duplex_process
	{
	  public:
		explicit fake_process(std::shared_ptr<process_state> state) : state_{std::move(state)} {}

		result<void> send_frame(const frame& value) override
		{
			state_->sent.push_back(value);
			return {};
		}

		result<std::optional<frame>> receive_frame(const std::stop_token cancellation) override
		{
			if (cancellation.stop_requested())
				return unexpected(error{"provider.cancelled", "ng1-live", "receive"});
			if (state_->incoming.empty())
				return std::optional<frame>{};
			frame value = std::move(state_->incoming.front());
			state_->incoming.pop_front();
			return std::optional<frame>{std::move(value)};
		}

		result<process_output> finish(const std::stop_token) override
		{
			if (state_->finish_failure)
				return unexpected(*state_->finish_failure);
			if (state_->finish_output)
				return *state_->finish_output;
			return clean_process_output();
		}

		result<process_output> terminate(const process_status status) override
		{
			if (state_->terminate_failure)
				return unexpected(*state_->terminate_failure);
			if (state_->terminate_output)
				return *state_->terminate_output;
			return process_output{status, 0, 0, {}, {}, {}, {}, {}};
		}

	  private:
		std::shared_ptr<process_state> state_;
	};

	class fake_process_port final : public ng1_duplex_process_port
	{
	  public:
		explicit fake_process_port(std::shared_ptr<process_state> state) : state_{std::move(state)}
		{
		}

		result<std::unique_ptr<ng1_duplex_process>>
		start(const process_invocation&, protocol_limits, const std::stop_token) const override
		{
			++state_->start_count;
			if (state_->throw_on_start && state_->start_count == *state_->throw_on_start)
				throw std::bad_alloc{};
			return std::unique_ptr<ng1_duplex_process>{new fake_process{state_}};
		}

	  private:
		std::shared_ptr<process_state> state_;
	};

	class rejecting_process_port final : public ng1_duplex_process_port
	{
	  public:
		result<std::unique_ptr<ng1_duplex_process>>
		start(const process_invocation&, protocol_limits, const std::stop_token) const override
		{
			return unexpected(
				error{"provider.process-request-invalid", "ng1-live", "injected-launch-failure"});
		}
	};

	class throwing_process_port final : public ng1_duplex_process_port
	{
	  public:
		result<std::unique_ptr<ng1_duplex_process>>
		start(const process_invocation&, protocol_limits, const std::stop_token) const override
		{
			throw std::bad_alloc{};
		}
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

		[[nodiscard]] ng1_live_driver_configuration
		configuration(std::shared_ptr<clock_state> clock,
					  std::shared_ptr<observation_state> observation,
					  std::shared_ptr<process_state> process,
					  const std::uint64_t maximum_retained_frames = 3U,
					  std::shared_ptr<spill_state> spill_lifecycle = {}) const
		{
			process_invocation invocation;
			invocation.argv = {"fake-provider"};
			invocation.expected_binary_digest = resume.provider_binary_digest;
			invocation.sandbox = {sandbox_assurance::enforced, resume.sandbox_policy_digest};
			protocol_limits limits;
			limits.minimum_minor = provider::protocol_v2_minor;
			limits.maximum_minor = provider::protocol_v2_minor;
			return {ng1_session_configuration{
						heartbeat,
						"dependency:test",
						resume,
						this->spill,
						1'000U,
						std::make_unique<memory_spill_storage>(std::move(spill_lifecycle))},
					invocation,
					limits,
					maximum_retained_frames,
					std::make_unique<fake_clock>(std::move(clock)),
					std::make_unique<fake_observation>(std::move(observation)),
					std::make_unique<fake_process_port>(std::move(process)),
					std::nullopt,
					std::nullopt};
		}

		[[nodiscard]] ng1_durable_resume_authority durable_authority() const
		{
			return {resume.task_input_digest, digest("source-closure")};
		}

		[[nodiscard]] ng1_heartbeat_control
		heartbeat_control(const ng1_heartbeat_kind kind,
						  const std::uint64_t sequence,
						  const std::uint64_t provider_time) const
		{
			return {"cxxlens.provider-control.heartbeat.v1",
					kind,
					heartbeat.provider_id,
					heartbeat.provider_version,
					heartbeat.protocol_session_id,
					heartbeat.task_id,
					heartbeat.stream_id,
					sequence,
					provider_time,
					0U,
					digest("staged")};
		}

		[[nodiscard]] frame heartbeat_frame(const ng1_heartbeat_kind kind,
											const std::uint64_t sequence,
											const std::uint64_t provider_time) const
		{
			auto encoded =
				encode_ng1_heartbeat_control(heartbeat_control(kind, sequence, provider_time));
			require(encoded, "live-driver heartbeat encoding failed");
			return {ng1_heartbeat_message_type,
					heartbeat.stream_id,
					sequence,
					std::move(*encoded),
					{},
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] frame task_accepted_frame(const std::uint64_t sequence) const
		{
			auto encoded = encode_task_accepted_metadata(task_accepted_metadata{
				heartbeat.provider_id, heartbeat.provider_version.string(), heartbeat.task_id});
			require(encoded, "live-driver task-accepted encoding failed");
			return {message_type::task_accepted,
					heartbeat.stream_id,
					sequence,
					std::move(*encoded),
					{},
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] frame batch_begin_frame(const std::uint64_t sequence) const
		{
			auto control =
				encode_batch_begin_metadata(batch_begin_metadata{spill.task_id,
																 "descriptor:test",
																 manifest_digest('f'),
																 spill.dependency_group_id,
																 spill.atomic_output_group_id,
																 spill.batch_id});
			require(control, "live-driver batch-begin encoding failed");
			return {message_type::batch_begin,
					spill.stream_id,
					sequence,
					std::move(*control),
					{},
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] frame batch_end_frame(const std::uint64_t sequence) const
		{
			columnar_batch_end terminal{spill.task_id,
										spill.dependency_group_id,
										spill.atomic_output_group_id,
										spill.batch_id,
										"descriptor:test",
										manifest_digest('f'),
										0U,
										{{"column:test", 0U, 0U}},
										{},
										{}};
			terminal.batch_digest = columnar_batch_digest(terminal);
			auto encoded = encode_columnar_batch_end(terminal);
			require(encoded, "live-driver batch-end encoding failed");
			return {message_type::batch_end,
					spill.stream_id,
					sequence,
					std::move(encoded->control),
					std::move(encoded->payload),
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] frame resume_frame(const std::uint64_t sequence,
										 const std::uint64_t generation = 1U) const
		{
			auto control = encode_ng1_resume_control(resume_control(generation));
			require(control, "live-driver resume encoding failed");
			return {message_type::resume,
					spill.stream_id,
					sequence,
					std::move(*control),
					{},
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] frame task_complete_frame(const std::uint64_t sequence) const
		{
			auto control = encode_task_complete_metadata(task_complete_metadata{spill.task_id});
			require(control, "live-driver task-complete encoding failed");
			return {message_type::task_complete,
					spill.stream_id,
					sequence,
					std::move(*control),
					{},
					provider::protocol_v2_major,
					provider::protocol_v2_minor,
					0U};
		}

		[[nodiscard]] ng1_spill_record spill_record(const std::string_view payload_text) const
		{
			ng1_spill_record output;
			output.task_id = spill.task_id;
			output.dependency_group_id = spill.dependency_group_id;
			output.atomic_output_group_id = spill.atomic_output_group_id;
			output.batch_id = spill.batch_id;
			output.stream_id = spill.stream_id;
			output.payload_bytes.reserve(payload_text.size());
			for (const auto byte : payload_text)
				output.payload_bytes.push_back(
					static_cast<std::byte>(static_cast<unsigned char>(byte)));
			auto payload_digest = ng1_spill_payload_digest(output.payload_bytes);
			require(payload_digest, "live-driver spill payload digest construction failed");
			output.payload_digest = *payload_digest;
			auto record_digest = ng1_spill_record_digest(output);
			require(record_digest, "live-driver spill record digest construction failed");
			output.record_digest = *record_digest;
			return output;
		}

		[[nodiscard]] ng1_resume_control resume_control(const std::uint64_t generation = 1U) const
		{
			ng1_resume_control output;
			output.kind = ng1_resume_kind::accepted;
			output.binding = resume;
			output.highest_contiguous_acked_sequence = 0U;
			output.staged_digest = digest("staged");
			output.token_generation = generation;
			ng1_resume_token token{output.schema,
								   output.kind,
								   output.binding,
								   output.highest_contiguous_acked_sequence,
								   output.staged_digest,
								   output.token_generation,
								   {}};
			auto token_digest = ng1_resume_token_digest(token);
			require(token_digest, "live-driver resume token digest construction failed");
			output.token_digest = *token_digest;
			return output;
		}
	};

	[[nodiscard]] std::vector<frame> clean_transcript_frames(const fixture& values)
	{
		const auto accepted_control = encode_task_accepted_metadata(task_accepted_metadata{
			"provider:test", values.heartbeat.provider_version.string(), "task:test"});
		require(accepted_control, "NG1 clean transcript accepted encoding failed");
		const auto coverage_control = encode_coverage_metadata(
			std::vector<coverage_unit>{{"task", "task:test", "covered", {}}});
		require(coverage_control, "NG1 clean transcript coverage encoding failed");
		const auto unresolved_control = encode_unresolved_metadata(std::vector<unresolved_item>{});
		require(unresolved_control, "NG1 clean transcript unresolved encoding failed");
		const auto complete_control =
			encode_task_complete_metadata(task_complete_metadata{"task:test"});
		require(complete_control, "NG1 clean transcript completion encoding failed");
		const auto progress_control =
			encode_ng1_progress_control(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															 "task:test",
															 "dependency:test",
															 0U,
															 1'000U,
															 1U,
															 2U});
		require(progress_control, "NG1 clean transcript progress encoding failed");
		const auto terminal_progress_control =
			encode_ng1_progress_control(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															 "task:test",
															 "dependency:test",
															 1U,
															 1'100U,
															 2U,
															 2U});
		require(terminal_progress_control,
				"NG1 clean transcript terminal progress encoding failed");
		auto heartbeat = values.heartbeat_frame(ng1_heartbeat_kind::ack, 0U, 1'000U);
		heartbeat.sequence = 1U;

		return {frame{message_type::task_accepted,
					  7U,
					  0U,
					  *accepted_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U},
				std::move(heartbeat),
				frame{message_type::progress,
					  7U,
					  2U,
					  *progress_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U},
				frame{message_type::progress,
					  7U,
					  3U,
					  *terminal_progress_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U},
				frame{message_type::coverage_chunk,
					  7U,
					  4U,
					  *coverage_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U},
				frame{message_type::unresolved_chunk,
					  7U,
					  5U,
					  *unresolved_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U},
				frame{message_type::task_complete,
					  7U,
					  6U,
					  *complete_control,
					  {},
					  provider::protocol_v2_major,
					  provider::protocol_v2_minor,
					  0U}};
	}

	[[nodiscard]] result<ng1_output_validation_receipt>
	validated_clean_output(const fixture& values, const std::span<const frame> frames)
	{
		protocol_limits limits;
		limits.minimum_minor = provider::protocol_v2_minor;
		limits.maximum_minor = provider::protocol_v2_minor;
		execution_budget budget;
		const transcript_validation_request request{"task:test",
													"provider:test",
													values.heartbeat.provider_version,
													nullptr,
													{},
													{1024U * 1024U, 8U},
													&budget,
													false,
													nullptr,
													7U,
													true,
													&values.heartbeat};
		auto validated = validate_provider_transcript(request, frames, limits);
		if (!validated)
			return unexpected(std::move(validated.error()));
		if (validated->kind != transcript_terminal_kind::complete || !validated->sealed())
			return unexpected(error{"provider.protocol-state-invalid", "output", "not-sealed"});
		return make_ng1_output_validation_receipt("task:test", *validated->sealed());
	}

	void receive_all(const std::shared_ptr<process_state>& process,
					 const std::shared_ptr<clock_state>& clock,
					 ng1_live_session_driver& driver,
					 const std::size_t expected_count)
	{
		for (std::size_t index{}; index < expected_count; ++index)
		{
			clock->now_ns += 100U;
			auto received = driver.receive_provider_frame({});
			require(received, "NG1 clean transcript receive failed");
			require(received->has_value(), "NG1 clean transcript ended before completion");
		}
		require(process->incoming.empty(), "NG1 clean transcript was not fully received");
	}

	void test_live_driver_clean_finish_remains_sealable()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		const auto frames = clean_transcript_frames(values);
		for (const auto& frame : frames)
			process->incoming.push_back(frame);

		auto driver = ng1_live_session_driver::start(
			values.configuration(clock, observation, process, frames.size()), {});
		require(driver, "NG1 clean-seal fixture start failed");
		receive_all(process, clock, *driver, frames.size());

		auto output = driver->finish({});
		require(output && output->status == process_status::exited && output->exit_code == 0,
				"clean process finish did not preserve the successful process outcome");
		require(driver->session().state() == ng1_recovery_state::running,
				"clean process finish fabricated a worker-exit transition");
		auto receipt = validated_clean_output(values, driver->provider_frames());
		require(receipt, "clean transcript did not produce a shared validation receipt");
		require(driver->session().seal_output(*receipt),
				"clean transcript could not consume the existing output-sealed transition");
		require(driver->session().state() == ng1_recovery_state::completed,
				"clean transcript did not reach the completed recovery state");
		require(driver->cleanup(), "clean-seal fixture cleanup failed");
	}

	void test_live_control_handoff_carries_controls_to_replay_frontier()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto configuration = values.configuration(clock, observation, process);
		auto handoff = ng1_live_control_handoff::create(std::move(configuration.session));
		require(handoff, "NG1 control handoff creation failed");

		require(handoff->observe_task_accepted(
					task_accepted_metadata{values.heartbeat.provider_id,
										   values.heartbeat.provider_version.string(),
										   values.heartbeat.task_id},
					2'000U),
				"NG1 control handoff task acceptance failed");
		require(handoff->observe_host_probe(
					values.heartbeat_control(ng1_heartbeat_kind::probe, 0U, 2'000U),
					2'000U,
					0U,
					digest("staged")),
				"NG1 control handoff heartbeat probe failed");
		require(handoff->observe_provider_ack(
					values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 2'001U),
					2'001U,
					0U,
					digest("staged")),
				"NG1 control handoff heartbeat ACK failed");

		require(
			handoff->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   0U,
														   2'001U,
														   0U,
														   10U},
									  2'001U),
			"NG1 control handoff initial progress failed");
		require(
			handoff->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   1U,
														   5'000'002'001U,
														   5U,
														   10U},
									  5'000'002'001U),
			"NG1 control handoff progress checkpoint failed");

		require(handoff->append_spill(values.spill_record("first")),
				"NG1 control handoff spill append failed");
		auto receipt = handoff->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "NG1 control handoff spill fsync failed");

		require(
			handoff->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   2U,
														   10'000'002'001U,
														   10U,
														   10U},
									  10'000'002'001U,
									  true),
			"NG1 control handoff terminal progress failed");

		require(handoff->observe_heartbeat_timeout(),
				"NG1 control handoff hang observation failed");
		require(handoff->state() == ng1_recovery_state::heartbeat_timeout,
				"NG1 control handoff did not retain the heartbeat-timeout state");
		require(handoff->confirm_worker_kill(),
				"NG1 control handoff worker-kill confirmation failed");
		require(handoff->state() == ng1_recovery_state::worker_killed,
				"NG1 control handoff did not reach worker-killed");
		require(handoff->accept_durable_resume(values.resume_control(), *receipt, false, false, 0U),
				"NG1 control handoff durable resume failed");
		auto replay_start = handoff->replay_start_sequence();
		require(replay_start && *replay_start == 1U,
				"NG1 control handoff replay frontier was not ack-plus-one");
		require(handoff->accept_replay_frontier(*replay_start),
				"NG1 control handoff replay frontier was rejected");
		require(handoff->state() == ng1_recovery_state::resumed,
				"NG1 control handoff did not enter resumed state");
		auto rejected = handoff->reject_output();
		require(!rejected && handoff->state() == ng1_recovery_state::failed,
				"NG1 control handoff allowed unvalidated replay output");
		require(handoff->cleanup(), "NG1 control handoff cleanup failed");
	}

	void test_live_control_handoff_cancellation_and_faults()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto configuration = values.configuration(clock, observation, process);
		auto handoff = ng1_live_control_handoff::create(std::move(configuration.session));
		require(handoff, "NG1 cancellation handoff creation failed");
		auto out_of_order_ack = handoff->acknowledge_cancel();
		require(!out_of_order_ack && out_of_order_ack.error().code == "provider.recovery-failed" &&
					handoff->state() == ng1_recovery_state::running,
				"NG1 cancellation accepted an acknowledgement before a request");
		require(handoff->request_cancel(), "NG1 cancellation request was rejected");
		require(handoff->acknowledge_cancel(), "NG1 cancellation acknowledgement was not terminal");
		require(handoff->state() == ng1_recovery_state::failed,
				"NG1 acknowledged cancellation did not fail closed");
		require(handoff->cleanup(), "NG1 acknowledged cancellation cleanup failed");

		auto timeout_configuration = values.configuration(std::make_shared<clock_state>(),
														  std::make_shared<observation_state>(),
														  std::make_shared<process_state>());
		auto timeout_handoff =
			ng1_live_control_handoff::create(std::move(timeout_configuration.session));
		require(timeout_handoff, "NG1 cancellation-timeout handoff creation failed");
		require(timeout_handoff->append_spill(values.spill_record("cancelled-prefix")),
				"NG1 cancellation-timeout spill append failed");
		auto receipt = timeout_handoff->fsync_spill(0U, 0U, digest("staged"), 1U);
		require(receipt, "NG1 cancellation-timeout spill fsync failed");
		require(timeout_handoff->request_cancel(), "NG1 cancellation-timeout request was rejected");
		require(timeout_handoff->timeout_cancel(),
				"NG1 cancellation timeout did not require worker kill");
		require(timeout_handoff->state() == ng1_recovery_state::worker_killed,
				"NG1 cancellation timeout did not reach worker-killed");

		auto corrupted_receipt = *receipt;
		corrupted_receipt.spill_digest = digest("corrupted-spill");
		auto rejected_resume = timeout_handoff->accept_durable_resume(
			values.resume_control(), corrupted_receipt, false, false, 0U);
		require(!rejected_resume &&
					rejected_resume.error().code == "provider.resume-replay-invalid" &&
					timeout_handoff->state() == ng1_recovery_state::failed,
				"NG1 cancellation recovery accepted a corrupted durable frontier");
		require(timeout_handoff->cleanup(), "NG1 cancellation-timeout cleanup failed");
	}

	void test_live_driver_non_clean_finish_remains_fail_closed()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		process->finish_output = process_output{process_status::exited,
												17,
												0,
												{},
												{},
												clean_process_output().sandbox,
												"provider.crash",
												manifest_digest('e')};

		auto driver = ng1_live_session_driver::start(
			values.configuration(clock, observation, process, 8U), {});
		require(driver, "NG1 non-clean fixture start failed");
		auto output = driver->finish({});
		require(output && output->status == process_status::exited && output->exit_code == 17,
				"non-clean process finish did not preserve the exact process outcome");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"non-clean process finish was treated as sealable clean completion");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"non-clean process finish did not remain fail-closed for cleanup");
		require(driver->cleanup(), "non-clean fixture cleanup failed");
	}

	void test_live_driver_timeout_remains_unsealable()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		const auto frames = clean_transcript_frames(values);
		for (const auto& frame : frames)
			process->incoming.push_back(frame);

		auto driver = ng1_live_session_driver::start(
			values.configuration(clock, observation, process, frames.size()), {});
		require(driver, "NG1 timeout fixture start failed");
		receive_all(process, clock, *driver, frames.size());
		clock->now_ns = 5'000'003'000ULL;
		auto timeout = driver->check_liveness();
		require(!timeout && timeout.error().code == "provider.heartbeat-timeout",
				"NG1 timeout fixture did not enter the heartbeat-timeout state");
		auto output = driver->finish({});
		require(output, "NG1 timeout fixture did not finish and reap the process");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"timeout cleanup did not remain a fail-closed kill transition");
		auto receipt = validated_clean_output(values, driver->provider_frames());
		require(receipt, "NG1 timeout transcript validation fixture failed");
		auto sealed = driver->session().seal_output(*receipt);
		require(!sealed && driver->session().state() == ng1_recovery_state::worker_killed,
				"timeout recovery state incorrectly accepted a clean seal");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"NG1 timeout fixture could not enter explicit failed cleanup");
		require(driver->cleanup(), "NG1 timeout fixture cleanup failed");
	}

	void test_live_control_bridge_and_bounded_retention()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		process->incoming.push_back(values.task_accepted_frame(0U));
		process->incoming.push_back(values.heartbeat_frame(ng1_heartbeat_kind::ack, 0U, 1'800U));
		process->incoming.push_back(frame{message_type::batch_begin,
										  7U,
										  2U,
										  {},
										  {},
										  provider::protocol_v2_major,
										  provider::protocol_v2_minor,
										  0U});

		auto driver =
			ng1_live_session_driver::start(values.configuration(clock, observation, process), {});
		require(driver, "live-driver start failed");

		auto probe = values.heartbeat_frame(ng1_heartbeat_kind::probe, 0U, 1'500U);
		require(driver->send_host_frame(probe), "live-driver host probe failed");
		require(process->sent.size() == 1U &&
					process->sent.front().type == ng1_heartbeat_message_type,
				"live-driver did not send the admitted host probe");

		auto accepted = driver->receive_provider_frame({});
		require(accepted && accepted->has_value() && accepted->value().ng1_control_admitted(),
				"live-driver did not admit task acceptance");
		auto receipt = driver->receive_provider_frame({});
		require(receipt, "live-driver provider ACK receive failed");
		require(receipt->has_value() && receipt->value().ng1_control_admitted(),
				"live-driver did not admit the provider ACK");
		require(receipt->value().host_receipt_time_ns() == clock->now_ns,
				"live-driver did not stamp the host receipt at ingress");

		auto ordinary = driver->receive_provider_frame({});
		require(ordinary, "live-driver ordinary frame receive failed");
		require(ordinary->has_value() && !ordinary->value().ng1_control_admitted(),
				"ordinary frame was incorrectly classified as NG1 control");
		require(driver->provider_frames().size() == 3U,
				"live-driver did not retain the bounded provider transcript prefix");

		clock->now_ns = 5'000'003'000ULL;
		auto liveness = driver->check_liveness();
		require(!liveness && liveness.error().code == "provider.heartbeat-timeout",
				"live-driver did not fail closed on the host liveness deadline");
		auto output = driver->finish({});
		require(output && output->status == process_status::exited,
				"live-driver did not preserve the exact finish outcome");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"live-driver finish did not synchronize timeout cleanup into worker-killed");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume &&
					rejected_resume.error().code == "provider.resume-replay-invalid",
				"live-driver session did not fail closed on an unreceipted resume");
		require(driver->cleanup(), "live-driver session cleanup failed");
	}

	void test_live_driver_rejects_retention_overflow()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		process->incoming.push_back(values.heartbeat_frame(ng1_heartbeat_kind::ack, 0U, 1'800U));
		process->incoming.push_back(values.task_accepted_frame(1U));

		auto driver = ng1_live_session_driver::start(
			values.configuration(clock, observation, process, 1U), {});
		require(driver, "live-driver overflow fixture start failed");
		auto first = driver->receive_provider_frame({});
		require(first, "live-driver overflow first frame failed");
		auto second = driver->receive_provider_frame({});
		require(!second && second.error().code == "provider.output-limit",
				"live-driver accepted a frame beyond the retention bound");
		require(driver->terminate(process_status::output_limit),
				"live-driver overflow cleanup did not terminate the process");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"live-driver terminate did not synchronize a running worker exit");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume, "live-driver overflow cleanup resume was not rejected");
		require(driver->cleanup(), "live-driver overflow session cleanup failed");
	}

	void test_live_driver_rejects_host_resume_without_receipt()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto driver =
			ng1_live_session_driver::start(values.configuration(clock, observation, process), {});
		require(driver, "live-driver resume fixture start failed");

		frame resume;
		resume.type = message_type::resume;
		resume.stream_id = 7U;
		resume.protocol_major = provider::protocol_v2_major;
		resume.protocol_minor = provider::protocol_v2_minor;
		auto rejected = driver->send_host_frame(resume);
		require(!rejected && rejected.error().code == "provider.protocol-state-invalid",
				"live-driver silently accepted a host resume without durable receipt");
		require(driver->terminate(process_status::cancelled),
				"live-driver resume cleanup did not terminate the process");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"live-driver terminate did not synchronize a cancelled worker exit");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume, "live-driver resume cleanup was not fail-closed");
		require(driver->cleanup(), "live-driver resume session cleanup failed");
	}

	void test_live_driver_cleans_session_when_process_start_fails()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto spill = std::make_shared<spill_state>();
		auto configuration = values.configuration(clock, observation, process, 3U, spill);
		configuration.processes = std::make_unique<rejecting_process_port>();

		auto rejected = ng1_live_session_driver::start(std::move(configuration), {});
		require(!rejected && rejected.error().code == "provider.process-request-invalid" &&
					rejected.error().detail == "injected-launch-failure",
				"live-driver did not preserve the process launch failure");
		require(
			spill->cleaned,
			"live-driver destroyed an unstarted session without cleaning its private spill port");
	}

	void test_live_driver_cleans_session_when_process_start_throws()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto spill = std::make_shared<spill_state>();
		auto configuration = values.configuration(clock, observation, process, 3U, spill);
		configuration.processes = std::make_unique<throwing_process_port>();

		auto rejected = ng1_live_session_driver::start(std::move(configuration), {});
		require(!rejected && rejected.error().code == "provider.process-launch-failed" &&
					rejected.error().field == "ng1-live" &&
					rejected.error().detail == "process-port-allocation-failed",
				"live-driver did not convert the process-port exception into a structured failure");
		require(spill->cleaned,
				"live-driver did not clean the spill port after a process-port exception");
	}

	void test_live_driver_closes_replacement_process_exceptions()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto spill = std::make_shared<spill_state>();
		process->throw_on_start = 2U;
		process->incoming.push_back(values.task_accepted_frame(0U));
		process->incoming.push_back(values.batch_begin_frame(1U));
		process->incoming.push_back(values.batch_end_frame(2U));
		process->incoming.push_back(values.resume_frame(1U));
		auto configuration = values.configuration(clock, observation, process, 8U, spill);
		configuration.durable_resume = values.durable_authority();
		auto driver = ng1_live_session_driver::start(std::move(configuration), {});
		require(driver && process->start_count == 1U,
				"replacement exception fixture did not launch its initial process");

		require(driver->receive_provider_frame({})->has_value(),
				"replacement exception fixture did not accept task metadata");
		require(driver->receive_provider_frame({})->has_value(),
				"replacement exception fixture did not open its output group");
		require(driver->append_durable_spill(values.spill_record("replacement-exception")),
				"replacement exception fixture did not stage its durable record");
		require(driver->receive_provider_frame({})->has_value(),
				"replacement exception fixture did not close its output group");
		auto checkpoint = driver->checkpoint_durable_spill(0U, 1U);
		require(checkpoint, "replacement exception fixture did not publish its durable frontier");
		require(driver->receive_provider_frame({})->has_value(),
				"replacement exception fixture did not receive its resume token");
		require(driver->terminate(process_status::crashed),
				"replacement exception fixture did not terminate the initial worker");

		auto replacement = driver->launch_replacement(*checkpoint, {});
		require(!replacement && replacement.error().code == "provider.process-launch-failed" &&
					replacement.error().field == "ng1-live" &&
					replacement.error().detail == "replacement-port-allocation-failed" &&
					process->start_count == 2U,
				"replacement process exception escaped its typed failure boundary");
		require(driver->session().state() == ng1_recovery_state::failed,
				"replacement process exception did not poison publication");
		require(driver->cleanup() && spill->cleaned,
				"replacement process exception leaked active spill cleanup custody");
	}

	void test_live_driver_rebases_task_timers_at_acceptance()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		process->incoming.push_back(values.task_accepted_frame(0U));

		auto driver =
			ng1_live_session_driver::start(values.configuration(clock, observation, process), {});
		require(driver, "task-acceptance timer fixture start failed");
		auto accepted = driver->receive_provider_frame({});
		require(accepted && accepted->has_value() && accepted->value().ng1_control_admitted(),
				"validated task acceptance was not admitted as NG1 control");

		// The configured session timestamp is 1,000 ns. At this point the old implementation
		// would be exactly at the inclusive 10-second startup boundary, while the validated
		// task-accepted receipt is 2,000 ns and therefore still has grace remaining.
		clock->now_ns = 10'000'001'000ULL;
		require(driver->check_liveness(),
				"NG1 lifecycle timer was anchored before validated task acceptance");
		auto output = driver->finish({});
		require(output && output->status == process_status::exited,
				"task-acceptance timer fixture did not preserve process outcome");
		require(driver->session().state() == ng1_recovery_state::running,
				"clean process finish fabricated a worker exit before output validation");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"incomplete clean output did not remain fail-closed");
		auto cleanup = driver->cleanup();
		if (!cleanup)
			require(false,
					"task-acceptance timer fixture cleanup failed: " + cleanup.error().code + ":" +
						cleanup.error().detail);
	}

	void test_live_driver_does_not_sync_failed_process_effects()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		auto driver =
			ng1_live_session_driver::start(values.configuration(clock, observation, process), {});
		require(driver, "process-effect failure fixture start failed");

		process->finish_failure =
			error{"provider.process-launch-failed", "ng1-live-finish", "injected"};
		auto finish = driver->finish({});
		require(!finish && driver->session().state() == ng1_recovery_state::running,
				"failed finish effect fabricated a worker lifecycle transition");

		process->finish_failure.reset();
		process->terminate_failure =
			error{"provider.process-launch-failed", "ng1-live-terminate", "injected"};
		auto terminate = driver->terminate(process_status::cancelled);
		require(!terminate && driver->session().state() == ng1_recovery_state::running,
				"failed terminate effect fabricated a worker lifecycle transition");

		process->terminate_failure.reset();
		auto completed = driver->terminate(process_status::cancelled);
		require(completed && driver->session().state() == ng1_recovery_state::worker_killed,
				"successful terminate did not synchronize after a failed process effect");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"process-effect failure fixture did not reach explicit failed cleanup");
		require(driver->cleanup(), "process-effect failure fixture cleanup failed");
	}

	void test_live_driver_publishes_only_fsynced_latest_resume_frontier()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		process->incoming.push_back(values.task_accepted_frame(0U));
		process->incoming.push_back(values.batch_begin_frame(1U));
		process->incoming.push_back(values.batch_end_frame(2U));
		process->incoming.push_back(values.resume_frame(1U));
		auto configuration = values.configuration(clock, observation, process, 8U);
		configuration.durable_resume = values.durable_authority();
		auto driver = ng1_live_session_driver::start(std::move(configuration), {});
		require(driver, "durable live-driver start failed");

		auto accepted = driver->receive_provider_frame({});
		require(accepted && accepted->has_value() && accepted->value().ng1_control_admitted(),
				"durable live-driver did not admit task acceptance");
		auto begin = driver->receive_provider_frame({});
		require(begin && begin->has_value(), "durable live-driver did not observe batch begin");
		require(driver->append_durable_spill(values.spill_record("sealed-row")),
				"durable live-driver did not append the bound spill record");
		auto premature = driver->checkpoint_durable_spill(0U, 1U);
		require(!premature && premature.error().code == "provider.protocol-state-invalid",
				"durable live-driver checkpointed an open output group");
		auto end = driver->receive_provider_frame({});
		require(end && end->has_value(), "durable live-driver did not observe batch end");
		auto checkpoint = driver->checkpoint_durable_spill(0U, 1U);
		require(checkpoint && checkpoint->receipt().fsync_sequence == 1U &&
					checkpoint->receipt().total_records == 1U &&
					checkpoint->source_closure_digest() ==
						values.durable_authority().source_closure_digest,
				"durable live-driver did not bind fsync, prefix, and source closure");

		auto resume = driver->receive_provider_frame({});
		require(resume && resume->has_value(), "durable live-driver did not capture resume token");
		require(driver->terminate(process_status::crashed),
				"durable live-driver did not reap the failed worker");
		auto resumed = driver->accept_provider_resume(resume->value(), *checkpoint);
		if (!resumed)
			require(false,
					"durable live-driver rejected its exact latest fsynced token: " +
						resumed.error().code + ":" + resumed.error().field + ":" +
						resumed.error().detail);
		auto replay = driver->session().replay_start_sequence();
		require(replay && *replay == 1U,
				"durable live-driver replay did not start at acknowledged sequence plus one");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"durable live-driver allowed an unvalidated replay to publish");
		require(driver->cleanup(), "durable live-driver cleanup failed");
	}

	void test_live_driver_rejects_stale_corrupt_gap_and_terminal_resume()
	{
		fixture values;
		{
			auto configuration = values.configuration(std::make_shared<clock_state>(),
													  std::make_shared<observation_state>(),
													  std::make_shared<process_state>(),
													  4U);
			configuration.durable_resume = values.durable_authority();
			configuration.invocation.expected_binary_digest = manifest_digest('9');
			auto rejected = ng1_live_session_driver::start(std::move(configuration), {});
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						rejected.error().field == "provider_binary_digest",
					"durable live-driver accepted a foreign process binary binding");
		}

		{
			auto configuration = values.configuration(std::make_shared<clock_state>(),
													  std::make_shared<observation_state>(),
													  std::make_shared<process_state>(),
													  4U);
			configuration.durable_resume = values.durable_authority();
			configuration.invocation.sandbox.policy_digest = digest("foreign-sandbox-policy");
			auto rejected = ng1_live_session_driver::start(std::move(configuration), {});
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						rejected.error().field == "sandbox_policy_digest",
					"durable live-driver accepted a foreign process sandbox binding");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto inherited = std::make_shared<process_inherited_channel_binding>();
			inherited->task_id = values.resume.task_id;
			inherited->session_id = values.resume.protocol_session_id;
			inherited->closure_digest = digest("foreign-source-closure");
			configuration.invocation.inherited_channel = std::move(inherited);
			auto rejected = ng1_live_session_driver::start(std::move(configuration), {});
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						rejected.error().field == "source_closure",
					"durable live-driver accepted a foreign inherited source closure");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.batch_begin_frame(0U));
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver, "unaccepted-task fixture start failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.task-binding-mismatch" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver opened output before task acceptance");
			require(driver->terminate(process_status::crashed),
					"unaccepted-task fixture worker cleanup failed");
			require(driver->cleanup(), "unaccepted-task fixture cleanup failed");
		}

		for (std::uint8_t variant{}; variant < 4U; ++variant)
		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			auto begin = values.batch_begin_frame(1U);
			if (variant == 0U)
				++begin.stream_id;
			else if (variant == 1U)
				++begin.protocol_major;
			else if (variant == 2U)
				begin.flags = 1U;
			else
				begin.payload.push_back(std::byte{0x01});
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(std::move(begin));
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"foreign output-frame fixture task acceptance failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.task-binding-mismatch" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted a foreign output-frame header");
			require(driver->terminate(process_status::crashed),
					"foreign output-frame fixture worker cleanup failed");
			require(driver->cleanup(), "foreign output-frame fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			auto foreign_end = values.batch_end_frame(2U);
			++foreign_end.stream_id;
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(std::move(foreign_end));
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"foreign batch-end fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "foreign batch-end fixture begin failed");
			require(driver->append_durable_spill(values.spill_record("foreign-end-row")),
					"foreign batch-end fixture spill append failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.task-binding-mismatch" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver sealed output with a foreign batch end");
			require(driver->terminate(process_status::crashed),
					"foreign batch-end fixture worker cleanup failed");
			require(driver->cleanup(), "foreign batch-end fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.resume_frame(3U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"pre-fsync fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "pre-fsync fixture batch begin failed");
			require(driver->append_durable_spill(values.spill_record("pre-fsync-row")),
					"pre-fsync fixture spill append failed");
			require(driver->receive_provider_frame({}), "pre-fsync fixture batch end failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.resume-replay-invalid" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted a token published before fsync");
			require(driver->terminate(process_status::crashed),
					"pre-fsync fixture worker cleanup failed");
			require(driver->cleanup(), "pre-fsync fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.resume_frame(1U, 2U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"future-token fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "future-token fixture batch begin failed");
			require(driver->append_durable_spill(values.spill_record("future-token-row")),
					"future-token fixture spill append failed");
			require(driver->receive_provider_frame({}), "future-token fixture batch end failed");
			require(driver->checkpoint_durable_spill(0U, 1U),
					"future-token fixture checkpoint failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted generation N before checkpoint N");
			require(driver->terminate(process_status::crashed),
					"future-token fixture worker cleanup failed");
			require(driver->cleanup(), "future-token fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.resume_frame(1U));
			process->incoming.push_back(values.resume_frame(1U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"duplicate-token fixture task acceptance failed");
			require(driver->receive_provider_frame({}),
					"duplicate-token fixture batch begin failed");
			require(driver->append_durable_spill(values.spill_record("duplicate-token-row")),
					"duplicate-token fixture spill append failed");
			require(driver->receive_provider_frame({}), "duplicate-token fixture batch end failed");
			require(driver->checkpoint_durable_spill(0U, 1U),
					"duplicate-token fixture checkpoint failed");
			auto first = driver->receive_provider_frame({});
			require(first && first->has_value(), "duplicate-token fixture first token failed");
			auto rejected = driver->receive_provider_frame({});
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted a duplicate token publication");
			require(driver->terminate(process_status::crashed),
					"duplicate-token fixture worker cleanup failed");
			require(driver->cleanup(), "duplicate-token fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.resume_frame(1U, 2U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver, "stale durable live-driver start failed");
			require(driver->receive_provider_frame({}), "stale fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "stale fixture batch begin failed");
			require(driver->append_durable_spill(values.spill_record("stale-row")),
					"stale fixture spill append failed");
			require(driver->receive_provider_frame({}), "stale fixture batch end failed");
			auto stale = driver->checkpoint_durable_spill(0U, 1U);
			auto latest = driver->checkpoint_durable_spill(0U, 2U);
			require(stale && latest && latest->resume_generation() == 2U,
					"stale fixture did not persist a newer frontier");
			auto resume = driver->receive_provider_frame({});
			require(resume && resume->has_value(), "stale fixture resume receive failed");
			require(driver->terminate(process_status::crashed),
					"stale fixture worker cleanup failed");
			auto rejected = driver->accept_provider_resume(resume->value(), *stale);
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver accepted a non-latest checkpoint");
			require(driver->cleanup(), "stale fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.resume_frame(1U, 1U));
			process->incoming.push_back(values.resume_frame(1U, 2U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"stale receipt fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "stale receipt fixture batch begin failed");
			require(driver->append_durable_spill(values.spill_record("stale-receipt-row")),
					"stale receipt fixture spill append failed");
			require(driver->receive_provider_frame({}), "stale receipt fixture batch end failed");
			require(driver->checkpoint_durable_spill(0U, 1U),
					"stale receipt fixture first checkpoint failed");
			auto stale = driver->receive_provider_frame({});
			auto latest = driver->checkpoint_durable_spill(0U, 2U);
			auto current = driver->receive_provider_frame({});
			require(latest && stale && stale->has_value() && current && current->has_value(),
					"stale receipt fixture did not capture both resume occurrences");
			require(driver->terminate(process_status::crashed),
					"stale receipt fixture worker cleanup failed");
			auto rejected = driver->accept_provider_resume(stale->value(), *latest);
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver left a stale receipt retryable");
			require(driver->cleanup(), "stale receipt fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			auto corrupt_resume = values.resume_frame(1U);
			require(!corrupt_resume.control.empty(), "corrupt resume fixture has no control bytes");
			corrupt_resume.control.back() ^= std::byte{0x01};
			process->incoming.push_back(std::move(corrupt_resume));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"corrupt fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "corrupt fixture begin failed");
			require(driver->append_durable_spill(values.spill_record("corrupt-row")),
					"corrupt fixture spill append failed");
			require(driver->receive_provider_frame({}), "corrupt fixture end failed");
			auto checkpoint = driver->checkpoint_durable_spill(0U, 1U);
			auto rejected = driver->receive_provider_frame({});
			require(checkpoint && !rejected &&
						rejected.error().code == "provider.resume-replay-invalid" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted a corrupt published resume token");
			require(driver->terminate(process_status::crashed),
					"corrupt fixture worker cleanup failed");
			require(driver->cleanup(), "corrupt fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			auto foreign_control = values.resume_control();
			foreign_control.binding.protocol_session_id = "session:foreign";
			ng1_resume_token foreign_token{foreign_control.schema,
										   foreign_control.kind,
										   foreign_control.binding,
										   foreign_control.highest_contiguous_acked_sequence,
										   foreign_control.staged_digest,
										   foreign_control.token_generation,
										   {}};
			auto foreign_digest = ng1_resume_token_digest(foreign_token);
			require(foreign_digest, "foreign resume digest construction failed");
			foreign_control.token_digest = *foreign_digest;
			auto foreign_bytes = encode_ng1_resume_control(foreign_control);
			require(foreign_bytes, "foreign resume encoding failed");
			process->incoming.push_back(frame{message_type::resume,
											  values.spill.stream_id,
											  1U,
											  std::move(*foreign_bytes),
											  {},
											  provider::protocol_v2_major,
											  provider::protocol_v2_minor,
											  0U});
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"foreign fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "foreign fixture begin failed");
			require(driver->append_durable_spill(values.spill_record("foreign-row")),
					"foreign fixture spill append failed");
			require(driver->receive_provider_frame({}), "foreign fixture end failed");
			auto checkpoint = driver->checkpoint_durable_spill(0U, 1U);
			auto rejected = driver->receive_provider_frame({});
			require(checkpoint && !rejected &&
						rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver admitted a foreign published resume token");
			require(driver->terminate(process_status::crashed),
					"foreign fixture worker cleanup failed");
			require(driver->cleanup(), "foreign fixture cleanup failed");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"gap fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "gap fixture start failed");
			auto gap = values.spill_record("gap-row");
			gap.sequence = 1U;
			auto rejected = driver->append_durable_spill(gap);
			require(!rejected && rejected.error().code == "provider.spill-corrupt" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver accepted a spill sequence gap");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			auto configuration = values.configuration(clock, observation, process, 4U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"reorder fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "reorder fixture start failed");
			auto reordered = values.spill_record("reordered-row");
			reordered.record_ordinal = 1U;
			auto rejected = driver->append_durable_spill(reordered);
			require(!rejected && rejected.error().code == "provider.spill-corrupt" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver accepted a reordered spill occurrence");
		}

		{
			auto clock = std::make_shared<clock_state>();
			auto observation = std::make_shared<observation_state>();
			auto process = std::make_shared<process_state>();
			process->incoming.push_back(values.task_accepted_frame(0U));
			process->incoming.push_back(values.batch_begin_frame(1U));
			process->incoming.push_back(values.batch_end_frame(2U));
			process->incoming.push_back(values.task_complete_frame(3U));
			process->incoming.push_back(values.resume_frame(1U));
			auto configuration = values.configuration(clock, observation, process, 8U);
			configuration.durable_resume = values.durable_authority();
			auto driver = ng1_live_session_driver::start(std::move(configuration), {});
			require(driver && driver->receive_provider_frame({}),
					"terminal fixture task acceptance failed");
			require(driver->receive_provider_frame({}), "terminal fixture begin failed");
			require(driver->append_durable_spill(values.spill_record("terminal-row")),
					"terminal fixture spill append failed");
			require(driver->receive_provider_frame({}), "terminal fixture end failed");
			auto checkpoint = driver->checkpoint_durable_spill(0U, 1U);
			require(checkpoint, "terminal fixture checkpoint failed");
			require(driver->receive_provider_frame({}), "terminal fixture terminal receive failed");
			auto resume = driver->receive_provider_frame({});
			require(resume && resume->has_value(), "terminal fixture resume receive failed");
			require(driver->terminate(process_status::crashed),
					"terminal fixture worker cleanup failed");
			auto rejected = driver->accept_provider_resume(resume->value(), *checkpoint);
			require(!rejected && rejected.error().code == "provider.resume-token-stale" &&
						driver->session().state() == ng1_recovery_state::failed,
					"durable live-driver accepted a terminal token");
			require(driver->cleanup(), "terminal fixture cleanup failed");
		}
	}

	void test_system_live_driver_connects_clock_and_duplex_process()
	{
		fixture values;
		auto observation = std::make_shared<observation_state>();
		auto configuration = values.configuration(
			std::make_shared<clock_state>(), observation, std::make_shared<process_state>());
		// Production construction owns a durable filesystem spill when no injected test port
		// exists.
		configuration.session.spill_storage.reset();
		const auto invocation = system_invocation("/bin/cat");
		configuration.session.resume_binding.provider_binary_digest =
			invocation.expected_binary_digest;
		configuration.session.resume_binding.sandbox_policy_digest =
			invocation.sandbox.policy_digest;
		frame input;
		input.type = message_type::input_chunk;
		input.stream_id = values.heartbeat.stream_id;
		input.sequence = 0U;
		auto input_control = encode_control_text("ng1-system-duplex");
		require(input_control, "NG1 system live driver control encoding failed");
		input.control = std::move(*input_control);
		input.payload = {std::byte{0x41}, std::byte{0x42}, std::byte{0x43}};
		input.protocol_major = provider::protocol_v2_major;
		input.protocol_minor = provider::protocol_v2_minor;

		auto driver =
			ng1_live_session_driver::start_system(std::move(configuration.session),
												  invocation,
												  system_protocol_limits(),
												  4U,
												  std::make_unique<fake_observation>(observation),
												  values.durable_authority(),
												  {});
		require(driver, "NG1 system live driver could not start");
		auto sent = driver->send_host_frame(input);
		if (!sent)
			require(false,
					"NG1 system live driver could not send a frame: " + sent.error().code + ":" +
						sent.error().field + ":" + sent.error().detail);
		auto echoed = driver->receive_provider_frame({});
		require(echoed && echoed->has_value(), "NG1 system live driver did not receive an echo");
		require(echoed->value().value().type == input.type &&
					echoed->value().value().stream_id == input.stream_id &&
					echoed->value().value().sequence == input.sequence &&
					echoed->value().value().payload == input.payload &&
					echoed->value().host_receipt_time_ns() != 0U,
				"NG1 system live driver lost the framed duplex or host clock receipt");

		auto finished = driver->finish({});
		require(finished && finished->status == process_status::exited &&
					finished->exit_code == 0 &&
					finished->measured_executable_digest == invocation.expected_binary_digest,
				"NG1 system live driver did not preserve process identity on finish");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"NG1 system live driver allowed unsealed output to remain publishable");
		require(driver->cleanup(), "NG1 system live driver cleanup failed");
	}

	void test_system_live_driver_propagates_cancellation_and_cleans_spill()
	{
		fixture values;
		auto observation = std::make_shared<observation_state>();
		auto process_state_value = std::make_shared<process_state>();
		auto spill = std::make_shared<spill_state>();
		auto configuration = values.configuration(
			std::make_shared<clock_state>(), observation, process_state_value, 4U, spill);
		auto invocation = system_invocation("/bin/cat");
		configuration.session.resume_binding.provider_binary_digest =
			invocation.expected_binary_digest;
		configuration.session.resume_binding.sandbox_policy_digest =
			invocation.sandbox.policy_digest;
		std::stop_source cancellation;
		auto driver =
			ng1_live_session_driver::start_system(std::move(configuration.session),
												  std::move(invocation),
												  system_protocol_limits(),
												  4U,
												  std::make_unique<fake_observation>(observation),
												  values.durable_authority(),
												  cancellation.get_token());
		require(driver, "NG1 system cancellation driver could not start");
		cancellation.request_stop();
		auto received = driver->receive_provider_frame({});
		require(!received && received.error().code == "provider.cancelled",
				"NG1 system live driver did not propagate its stop token");
		auto finished = driver->finish({});
		require(finished && finished->status == process_status::cancelled,
				"NG1 system cancellation did not preserve process cleanup outcome");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"NG1 system cancellation did not enter worker-killed state");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->cleanup(),
				"NG1 system cancellation could not finish fail-closed cleanup");
	}

	void test_system_live_driver_destructor_cleans_unfinished_session()
	{
		fixture values;
		auto observation = std::make_shared<observation_state>();
		auto spill = std::make_shared<spill_state>();
		{
			auto configuration = values.configuration(std::make_shared<clock_state>(),
													  observation,
													  std::make_shared<process_state>(),
													  4U,
													  spill);
			auto invocation = system_invocation("/bin/cat");
			configuration.session.resume_binding.provider_binary_digest =
				invocation.expected_binary_digest;
			configuration.session.resume_binding.sandbox_policy_digest =
				invocation.sandbox.policy_digest;
			auto driver = ng1_live_session_driver::start_system(
				std::move(configuration.session),
				std::move(invocation),
				system_protocol_limits(),
				4U,
				std::make_unique<fake_observation>(observation),
				values.durable_authority(),
				{});
			require(driver, "NG1 destructor cleanup driver could not start");
		}
		require(spill->cleaned,
				"NG1 live driver destructor did not clean the unfinished private spill session");
	}

	void test_shared_validator_accepts_explicit_ng1_controls()
	{
		fixture values;
		auto frames = clean_transcript_frames(values);

		protocol_limits limits;
		limits.minimum_minor = provider::protocol_v2_minor;
		limits.maximum_minor = provider::protocol_v2_minor;
		execution_budget budget;
		const transcript_validation_request request{"task:test",
													"provider:test",
													values.heartbeat.provider_version,
													nullptr,
													{},
													{1024U * 1024U, 8U},
													&budget,
													false,
													nullptr,
													7U,
													true,
													&values.heartbeat};
		auto validated = validate_provider_transcript(request, frames, limits);
		require(validated && validated->kind == transcript_terminal_kind::complete &&
					validated->sealed() && validated->sealed()->evidence().empty(),
				"explicit NG1 controls were not separated from NG0 evidence");

		auto validate_heartbeat_mutation = [&](const auto& mutate, const std::string_view detail)
		{
			auto candidate = frames;
			auto heartbeat = values.heartbeat_control(ng1_heartbeat_kind::ack, 0U, 1'000U);
			mutate(heartbeat);
			auto encoded = encode_ng1_heartbeat_control(heartbeat);
			require(encoded, "NG1 heartbeat negative-vector encoding failed");
			candidate.at(1U).control = std::move(*encoded);
			auto rejected = validate_provider_transcript(request, candidate, limits);
			require(!rejected && rejected.error().detail == detail,
					"NG1 heartbeat binding/direction negative vector was accepted");
		};
		validate_heartbeat_mutation(
			[](auto& heartbeat)
			{
				heartbeat.kind = ng1_heartbeat_kind::probe;
			},
			"ng1-heartbeat-direction");
		validate_heartbeat_mutation(
			[](auto& heartbeat)
			{
				heartbeat.provider_id = "provider:other";
			},
			"ng1-heartbeat-binding");
		validate_heartbeat_mutation(
			[](auto& heartbeat)
			{
				heartbeat.protocol_session_id = "session:other";
			},
			"ng1-heartbeat-binding");
		validate_heartbeat_mutation(
			[](auto& heartbeat)
			{
				heartbeat.task_id = "task:other";
			},
			"ng1-heartbeat-binding");
		validate_heartbeat_mutation(
			[](auto& heartbeat)
			{
				heartbeat.stream_id = 8U;
			},
			"ng1-heartbeat-binding");

		auto nonterminal = frames;
		nonterminal.erase(nonterminal.begin() + 3U);
		for (std::size_t index{}; index < nonterminal.size(); ++index)
			nonterminal[index].sequence = index;
		auto nonterminal_result = validate_provider_transcript(request, nonterminal, limits);
		require(!nonterminal_result && nonterminal_result.error().detail == "complete",
				"NG1 nonterminal progress was accepted as successful completion");

		auto after_terminal = frames;
		auto terminal_progress =
			encode_ng1_progress_control(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															 "task:test",
															 "dependency:test",
															 2U,
															 1'200U,
															 2U,
															 2U});
		require(terminal_progress, "NG1 post-terminal progress encoding failed");
		after_terminal.insert(after_terminal.begin() + 4U,
							  frame{message_type::progress,
									7U,
									0U,
									*terminal_progress,
									{},
									provider::protocol_v2_major,
									provider::protocol_v2_minor,
									0U});
		for (std::size_t index{}; index < after_terminal.size(); ++index)
			after_terminal[index].sequence = index;
		auto after_terminal_result = validate_provider_transcript(request, after_terminal, limits);
		require(!after_terminal_result &&
					after_terminal_result.error().detail == "ng1-progress-after-terminal",
				"NG1 progress after terminal sample was accepted");

		auto ng0_mode = request;
		ng0_mode.ng1_control_transcript = false;
		auto rejected = validate_provider_transcript(ng0_mode, frames, limits);
		require(!rejected && rejected.error().code == "provider.protocol-state-invalid",
				"reserved NG1 heartbeat was accepted without the NG1 transcript mode");
	}

	void test_live_control_handoff_enforces_progress_rate_algorithm()
	{
		fixture values;

		const auto make_handoff = [&values]()
		{
			auto configuration = values.configuration(std::make_shared<clock_state>(),
													  std::make_shared<observation_state>(),
													  std::make_shared<process_state>());
			return ng1_live_control_handoff::create(std::move(configuration.session));
		};

		// A one-unit advance over a five-second host-receipt window is below the one-unit/second
		// contract once the ten-second startup grace has elapsed. Provider timestamps do not affect
		// this decision: the test deliberately gives them the exact host receipt values.
		auto slow = make_handoff();
		require(slow, "NG1 progress-rate slow fixture creation failed");
		require(slow->observe_task_accepted(
					task_accepted_metadata{values.heartbeat.provider_id,
										   values.heartbeat.provider_version.string(),
										   values.heartbeat.task_id},
					2'000U),
				"NG1 progress-rate slow fixture task acceptance failed");
		require(slow->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															"task:test",
															"dependency:test",
															0U,
															2'001U,
															0U,
															10U},
									   2'001U),
				"NG1 progress-rate slow fixture initial sample failed");
		require(slow->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															"task:test",
															"dependency:test",
															1U,
															5'000'002'001ULL,
															5U,
															10U},
									   5'000'002'001ULL),
				"NG1 progress-rate slow fixture checkpoint sample failed");
		auto slow_sample =
			slow->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														"task:test",
														"dependency:test",
														2U,
														10'000'002'002ULL,
														6U,
														10U},
								   10'000'002'002ULL);
		require(!slow_sample && slow_sample.error().code == "provider.progress-rate" &&
					slow_sample.error().field == "rate" &&
					slow_sample.error().detail == "minimum-not-met" &&
					slow->state() == ng1_recovery_state::progress_rate_failure,
				"NG1 progress-rate accepted a host-receipted rate below the minimum");
		require(slow->confirm_worker_kill(),
				"NG1 progress-rate failure did not synchronize worker kill");
		auto slow_rejected = slow->reject_output();
		require(!slow_rejected && slow->state() == ng1_recovery_state::failed,
				"NG1 progress-rate failure remained restartable after explicit rejection");
		require(slow->cleanup(), "NG1 progress-rate slow fixture cleanup failed");

		// A sample gap strictly beyond the bounded observation window is rejected even when the
		// reported work would otherwise satisfy the rate. This prevents a silent long-run gap from
		// turning a stale progress claim into a live one.
		auto gapped = make_handoff();
		require(gapped, "NG1 progress-gap fixture creation failed");
		require(gapped->observe_task_accepted(
					task_accepted_metadata{values.heartbeat.provider_id,
										   values.heartbeat.provider_version.string(),
										   values.heartbeat.task_id},
					2'000U),
				"NG1 progress-gap fixture task acceptance failed");
		require(
			gapped->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														  "task:test",
														  "dependency:test",
														  0U,
														  2'001U,
														  0U,
														  2U},
									 2'001U),
			"NG1 progress-gap fixture initial sample failed");
		auto gapped_sample =
			gapped->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														  "task:test",
														  "dependency:test",
														  1U,
														  10'000'002'002ULL,
														  2U,
														  2U},
									 10'000'002'002ULL);
		require(!gapped_sample && gapped_sample.error().code == "provider.progress-rate" &&
					gapped_sample.error().field == "sample_gap" &&
					gapped_sample.error().detail == "maximum-exceeded" &&
					gapped->state() == ng1_recovery_state::progress_rate_failure,
				"NG1 progress-rate accepted a sample gap beyond its bounded window");
		require(gapped->confirm_worker_kill(),
				"NG1 progress-gap failure did not synchronize worker kill");
		auto gapped_rejected = gapped->reject_output();
		require(!gapped_rejected && gapped->state() == ng1_recovery_state::failed,
				"NG1 progress-gap failure remained restartable after explicit rejection");
		require(gapped->cleanup(), "NG1 progress-gap fixture cleanup failed");

		// A deterministic one-unit/second transcript remains admissible at both checkpoints and
		// reaches terminal progress without inventing a completion receipt.
		auto healthy = make_handoff();
		require(healthy, "NG1 progress-rate healthy fixture creation failed");
		require(healthy->observe_task_accepted(
					task_accepted_metadata{values.heartbeat.provider_id,
										   values.heartbeat.provider_version.string(),
										   values.heartbeat.task_id},
					2'000U),
				"NG1 progress-rate healthy fixture task acceptance failed");
		require(
			healthy->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   0U,
														   2'001U,
														   0U,
														   10U},
									  2'001U),
			"NG1 progress-rate healthy fixture initial sample failed");
		require(
			healthy->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   1U,
														   5'000'002'001ULL,
														   5U,
														   10U},
									  5'000'002'001ULL),
			"NG1 progress-rate healthy fixture checkpoint sample failed");
		require(
			healthy->observe_progress(ng1_progress_control{"cxxlens.provider-control.progress.v2",
														   "task:test",
														   "dependency:test",
														   2U,
														   10'000'002'001ULL,
														   10U,
														   10U},
									  10'000'002'001ULL,
									  true),
			"NG1 progress-rate healthy fixture terminal sample failed");
		require(healthy->state() == ng1_recovery_state::running,
				"NG1 progress-rate terminal observation fabricated completion authority");
		require(!healthy->reject_output() && healthy->state() == ng1_recovery_state::failed,
				"NG1 progress-rate healthy fixture could not remain unsealed");
		require(healthy->cleanup(), "NG1 progress-rate healthy fixture cleanup failed");
	}

	void test_live_driver_retains_a_long_run_only_within_the_frame_bound()
	{
		fixture values;
		auto clock = std::make_shared<clock_state>();
		auto observation = std::make_shared<observation_state>();
		auto process = std::make_shared<process_state>();
		constexpr std::size_t retained_frame_bound = 4'096U;
		for (std::size_t sequence{}; sequence <= retained_frame_bound; ++sequence)
		{
			frame value;
			value.type = message_type::input_chunk;
			value.stream_id = values.heartbeat.stream_id;
			value.sequence = sequence;
			value.payload = {std::byte{0x5a}};
			value.protocol_major = provider::protocol_v2_major;
			value.protocol_minor = provider::protocol_v2_minor;
			process->incoming.push_back(std::move(value));
		}

		auto driver = ng1_live_session_driver::start(
			values.configuration(clock, observation, process, retained_frame_bound), {});
		require(driver, "NG1 long-run retention fixture start failed");
		for (std::size_t index{}; index < retained_frame_bound; ++index)
		{
			auto received = driver->receive_provider_frame({});
			require(received && received->has_value(),
					"NG1 long-run retention fixture stopped before its explicit bound");
		}
		require(driver->provider_frames().size() == retained_frame_bound,
				"NG1 long-run retention exceeded the configured frame bound");
		auto overflow = driver->receive_provider_frame({});
		require(!overflow && overflow.error().code == "provider.output-limit" &&
					driver->provider_frames().size() == retained_frame_bound,
				"NG1 long-run retention admitted an occurrence beyond the bound");
		require(driver->terminate(process_status::output_limit),
				"NG1 long-run retention overflow cleanup did not terminate the worker");
		require(driver->session().state() == ng1_recovery_state::worker_killed,
				"NG1 long-run retention overflow did not synchronize worker kill");
		auto rejected = driver->session().reject_output();
		require(!rejected && driver->session().state() == ng1_recovery_state::failed,
				"NG1 long-run retention overflow did not enter explicit failed cleanup");
		require(driver->cleanup(), "NG1 long-run retention fixture cleanup failed");
	}
} // namespace

int main()
{
	test_live_control_handoff_carries_controls_to_replay_frontier();
	test_live_control_handoff_cancellation_and_faults();
	test_live_driver_clean_finish_remains_sealable();
	test_live_driver_non_clean_finish_remains_fail_closed();
	test_live_driver_timeout_remains_unsealable();
	test_live_control_bridge_and_bounded_retention();
	test_live_driver_rejects_retention_overflow();
	test_live_driver_rejects_host_resume_without_receipt();
	test_live_driver_cleans_session_when_process_start_fails();
	test_live_driver_cleans_session_when_process_start_throws();
	test_live_driver_closes_replacement_process_exceptions();
	test_live_driver_rebases_task_timers_at_acceptance();
	test_live_driver_does_not_sync_failed_process_effects();
	test_live_driver_publishes_only_fsynced_latest_resume_frontier();
	test_live_driver_rejects_stale_corrupt_gap_and_terminal_resume();
	test_system_live_driver_connects_clock_and_duplex_process();
	test_system_live_driver_propagates_cancellation_and_cleans_spill();
	test_system_live_driver_destructor_cleans_unfinished_session();
	test_shared_validator_accepts_explicit_ng1_controls();
	test_live_control_handoff_enforces_progress_rate_algorithm();
	test_live_driver_retains_a_long_run_only_within_the_frame_bound();
	return 0;
}
