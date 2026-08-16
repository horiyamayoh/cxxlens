#include <limits>
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

		[[nodiscard]] bool same_frame(const frame& left, const frame& right) noexcept
		{
			return left.type == right.type && left.stream_id == right.stream_id &&
				left.sequence == right.sequence && left.control == right.control &&
				left.payload == right.payload && left.protocol_major == right.protocol_major &&
				left.protocol_minor == right.protocol_minor && left.flags == right.flags;
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
		auto process = configuration.processes->start(
			configuration.invocation, configuration.limits, cancellation);
		if (!process)
			return cxxlens::sdk::unexpected(std::move(process.error()));
		return ng1_live_session_driver{std::move(*session),
									   std::move(*process),
									   std::move(configuration.clock),
									   std::move(configuration.observation),
									   configuration.maximum_retained_frames};
	}

	ng1_live_session_driver::ng1_live_session_driver(
		ng1_session_coordinator session,
		std::unique_ptr<ng1_duplex_process> process,
		std::unique_ptr<ng1_monotonic_clock_port> clock,
		std::unique_ptr<ng1_host_observation_port> observation,
		const std::uint64_t maximum_retained_frames) noexcept
		: session_{std::move(session)}, adapter_{session_}, process_{std::move(process)},
		  clock_{std::move(clock)}, observation_{std::move(observation)},
		  maximum_retained_frames_{maximum_retained_frames}
	{
	}

	ng1_live_session_driver::ng1_live_session_driver(ng1_live_session_driver&& other) noexcept
		: session_{std::move(other.session_)}, adapter_{session_},
		  process_{std::move(other.process_)}, clock_{std::move(other.clock_)},
		  observation_{std::move(other.observation_)},
		  provider_frames_{std::move(other.provider_frames_)},
		  last_provider_receipt_{std::move(other.last_provider_receipt_)},
		  maximum_retained_frames_{other.maximum_retained_frames_}, ended_{other.ended_}
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

		auto receipt = stamp_provider_frame(std::move(**value));
		if (!receipt)
			return cxxlens::sdk::unexpected(std::move(receipt.error()));
		provider_frames_.push_back(receipt->value_);
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
		ended_ = true;
		return output;
	}

	result<process_output> ng1_live_session_driver::terminate(const process_status status)
	{
		if (auto open = ensure_open("terminate"); !open)
			return cxxlens::sdk::unexpected(std::move(open.error()));
		auto output = process_->terminate(status);
		ended_ = true;
		return output;
	}

	result<void> ng1_live_session_driver::cleanup()
	{
		if (!ended_)
			return cxxlens::sdk::unexpected(driver_error("cleanup", "process-not-ended"));
		return session_.cleanup();
	}
} // namespace cxxlens::sdk::provider::detail
