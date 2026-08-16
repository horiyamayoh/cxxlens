#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <iostream>
#include <memory>
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

	class memory_spill_storage final : public ng1_spill_storage_port
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
			cleaned_ = true;
			return {};
		}

	  private:
		std::vector<std::byte> bytes_;
		std::uint64_t fsync_sequence_{};
		std::optional<ng1_spill_resume_frontier> frontier_;
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
			return process_output{process_status::exited, 0, 0, {}, {}, {}, {}, {}};
		}

		result<process_output> terminate(const process_status status) override
		{
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
					  const std::uint64_t maximum_retained_frames = 3U) const
		{
			process_invocation invocation;
			invocation.argv = {"fake-provider"};
			protocol_limits limits;
			limits.minimum_minor = 1U;
			limits.maximum_minor = 1U;
			return {ng1_session_configuration{heartbeat,
											  "dependency:test",
											  resume,
											  spill,
											  1'000U,
											  std::make_unique<memory_spill_storage>()},
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
		require(driver->session().confirm_worker_kill(),
				"live-driver did not expose the explicit worker-kill confirmation");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume &&
					rejected_resume.error().code == "provider.resume-replay-invalid",
				"live-driver session did not fail closed on an unreceipted resume");

		auto output = driver->finish({});
		require(output && output->status == process_status::exited,
				"live-driver did not preserve the exact finish outcome");
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
		require(driver->session().observe_worker_exit(),
				"live-driver overflow cleanup did not observe worker exit");
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
		require(driver->session().observe_worker_exit(),
				"live-driver resume cleanup did not observe worker exit");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume, "live-driver resume cleanup was not fail-closed");
		require(driver->cleanup(), "live-driver resume session cleanup failed");
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
		require(driver->session().observe_worker_exit(),
				"task-acceptance timer fixture did not record worker exit");
		auto rejected_resume = driver->session().accept_durable_resume(
			ng1_resume_control{}, ng1_spill_fsync_receipt{}, false, false, 0U);
		require(!rejected_resume, "task-acceptance timer fixture accepted an unreceipted resume");
		auto cleanup = driver->cleanup();
		if (!cleanup)
			require(false,
					"task-acceptance timer fixture cleanup failed: " + cleanup.error().code + ":" +
						cleanup.error().detail);
	}

	void test_shared_validator_accepts_explicit_ng1_controls()
	{
		fixture values;
		const auto accepted_control = encode_task_accepted_metadata(task_accepted_metadata{
			"provider:test", values.heartbeat.provider_version.string(), "task:test"});
		require(accepted_control, "NG1 shared-validator accepted encoding failed");
		const auto coverage_control = encode_coverage_metadata(
			std::vector<coverage_unit>{{"task", "task:test", "covered", {}}});
		require(coverage_control, "NG1 shared-validator coverage encoding failed");
		const auto unresolved_control = encode_unresolved_metadata(std::vector<unresolved_item>{});
		require(unresolved_control, "NG1 shared-validator unresolved encoding failed");
		const auto complete_control =
			encode_task_complete_metadata(task_complete_metadata{"task:test"});
		require(complete_control, "NG1 shared-validator completion encoding failed");
		const auto progress_control =
			encode_ng1_progress_control(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															 "task:test",
															 "dependency:test",
															 0U,
															 1'000U,
															 1U,
															 2U});
		require(progress_control, "NG1 shared-validator progress encoding failed");
		const auto next_progress_control =
			encode_ng1_progress_control(ng1_progress_control{"cxxlens.provider-control.progress.v2",
															 "task:test",
															 "dependency:test",
															 1U,
															 1'100U,
															 2U,
															 2U});
		require(next_progress_control, "NG1 shared-validator second progress encoding failed");

		std::vector<frame> frames;
		frames.push_back(
			frame{message_type::task_accepted, 7U, 0U, *accepted_control, {}, 1U, 1U, 0U});
		frames.push_back(values.heartbeat_frame(ng1_heartbeat_kind::ack, 1U, 1'000U));
		frames.push_back(frame{message_type::progress, 7U, 2U, *progress_control, {}, 1U, 1U, 0U});
		frames.push_back(
			frame{message_type::progress, 7U, 3U, *next_progress_control, {}, 1U, 1U, 0U});
		frames.push_back(
			frame{message_type::coverage_chunk, 7U, 4U, *coverage_control, {}, 1U, 1U, 0U});
		frames.push_back(
			frame{message_type::unresolved_chunk, 7U, 5U, *unresolved_control, {}, 1U, 1U, 0U});
		frames.push_back(
			frame{message_type::task_complete, 7U, 6U, *complete_control, {}, 1U, 1U, 0U});

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
			auto heartbeat = values.heartbeat_control(ng1_heartbeat_kind::ack, 1U, 1'000U);
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
	test_live_control_bridge_and_bounded_retention();
	test_live_driver_rejects_retention_overflow();
	test_live_driver_rejects_host_resume_without_receipt();
	test_live_driver_rebases_task_timers_at_acceptance();
	test_shared_validator_accepts_explicit_ng1_controls();
	return 0;
}
