#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
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
			protocol_limits limits;
			limits.minimum_minor = 1U;
			limits.maximum_minor = 1U;
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
					std::make_unique<fake_process_port>(std::move(process))};
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
					1U,
					1U,
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
					1U,
					1U,
					0U};
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

		return {frame{message_type::task_accepted, 7U, 0U, *accepted_control, {}, 1U, 1U, 0U},
				std::move(heartbeat),
				frame{message_type::progress, 7U, 2U, *progress_control, {}, 1U, 1U, 0U},
				frame{message_type::progress, 7U, 3U, *terminal_progress_control, {}, 1U, 1U, 0U},
				frame{message_type::coverage_chunk, 7U, 4U, *coverage_control, {}, 1U, 1U, 0U},
				frame{message_type::unresolved_chunk, 7U, 5U, *unresolved_control, {}, 1U, 1U, 0U},
				frame{message_type::task_complete, 7U, 6U, *complete_control, {}, 1U, 1U, 0U}};
	}

	[[nodiscard]] result<ng1_output_validation_receipt>
	validated_clean_output(const fixture& values, const std::span<const frame> frames)
	{
		protocol_limits limits;
		limits.minimum_minor = 1U;
		limits.maximum_minor = 1U;
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
		process->incoming.push_back(frame{message_type::batch_begin, 7U, 2U, {}, {}, 1U, 1U, 0U});

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
		resume.protocol_major = 1U;
		resume.protocol_minor = 1U;
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

	void test_shared_validator_accepts_explicit_ng1_controls()
	{
		fixture values;
		auto frames = clean_transcript_frames(values);

		protocol_limits limits;
		limits.minimum_minor = 1U;
		limits.maximum_minor = 1U;
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
		after_terminal.insert(
			after_terminal.begin() + 4U,
			frame{message_type::progress, 7U, 0U, *terminal_progress, {}, 1U, 1U, 0U});
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
} // namespace

int main()
{
	test_live_driver_clean_finish_remains_sealable();
	test_live_driver_non_clean_finish_remains_fail_closed();
	test_live_driver_timeout_remains_unsealable();
	test_live_control_bridge_and_bounded_retention();
	test_live_driver_rejects_retention_overflow();
	test_live_driver_rejects_host_resume_without_receipt();
	test_live_driver_cleans_session_when_process_start_fails();
	test_live_driver_cleans_session_when_process_start_throws();
	test_live_driver_rebases_task_timers_at_acceptance();
	test_live_driver_does_not_sync_failed_process_effects();
	test_shared_validator_accepts_explicit_ng1_controls();
	return 0;
}
