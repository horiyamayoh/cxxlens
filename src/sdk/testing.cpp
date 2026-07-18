#include <algorithm>
#include <limits>
#include <sstream>
#include <stop_token>

#include <cxxlens/sdk/testing.hpp>

#include "provider_validation_internal.hpp"

namespace cxxlens::sdk::testing
{
	namespace
	{
		class vector_sink final : public provider::frame_sink
		{
		  public:
			result<void> write(const std::span<const std::byte> frame_bytes) override
			{
				frames.emplace_back(frame_bytes.begin(), frame_bytes.end());
				return {};
			}
			std::vector<std::vector<std::byte>> frames;
		};

		[[nodiscard]] std::string transcript(const std::vector<provider::frame>& frames)
		{
			std::ostringstream output;
			for (const auto& frame : frames)
				output << frame.protocol_major << '|' << frame.protocol_minor << '|' << frame.flags
					   << '|' << static_cast<std::uint16_t>(frame.type) << '|' << frame.stream_id
					   << '|' << frame.sequence << '|' << content_digest(frame.control) << '|'
					   << content_digest(frame.payload) << '\n';
			return output.str();
		}
	} // namespace

	result<conformance_report>
	validate_logical_transcript(const provider::task& task,
								const std::string_view provider_id,
								const semantic_version provider_version,
								const std::span<const provider::frame> frames,
								const provider::protocol_credit credit,
								const provider::execution_budget budget)
	{
		conformance_report report;
		report.frames.assign(frames.begin(), frames.end());
		report.semantic_transcript = transcript(report.frames);
		const provider::detail::transcript_validation_request validation{
			task.task_id,
			std::string{provider_id},
			provider_version,
			nullptr,
			task.outputs,
			credit,
			&budget,
			false,
		};
		auto terminal = provider::detail::validate_provider_transcript(
			validation, report.frames, provider::protocol_limits{});
		if (!terminal)
		{
			report.reason_code = terminal.error().code;
			return report;
		}
		report.accepted = terminal->kind == provider::detail::transcript_terminal_kind::complete;
		report.reason_code = report.accepted ? "accepted" : terminal->reason;
		return report;
	}

	result<conformance_report>
	validate_process_transcript(const provider::task& task,
								const provider::manifest& manifest,
								const std::span<const provider::frame> frames,
								const provider::protocol_credit credit,
								const provider::protocol_limits limits,
								const provider::execution_budget budget)
	{
		conformance_report report;
		report.frames.assign(frames.begin(), frames.end());
		report.semantic_transcript = transcript(report.frames);
		const provider::detail::transcript_validation_request validation{
			task.task_id,
			manifest.provider_id,
			manifest.provider_version,
			&manifest,
			task.outputs,
			credit,
			&budget,
			true,
		};
		auto terminal =
			provider::detail::validate_provider_transcript(validation, report.frames, limits);
		if (!terminal)
		{
			report.reason_code = terminal.error().code;
			return report;
		}
		report.accepted = terminal->kind == provider::detail::transcript_terminal_kind::complete;
		report.reason_code = report.accepted ? "accepted" : terminal->reason;
		return report;
	}

	result<conformance_report> provider_harness::run(provider::portable_provider& implementation,
													 const provider::task& task,
													 const provider_fault fault) const
	{
		vector_sink sink;
		provider::protocol_writer writer{sink};
		if (fault != provider_fault::no_credit)
			writer.grant_credit({std::numeric_limits<std::uint64_t>::max(), 4096U});
		std::stop_source cancellation;
		if (fault == provider_fault::cancel_before_run)
			cancellation.request_stop();
		provider::execution_context execution;
		execution.cancellation = cancellation.get_token();
		auto outcome = provider::run_worker(implementation, task, writer, execution);
		if (fault == provider_fault::no_credit)
		{
			if (outcome || outcome.error().code != "provider.backpressure")
				return cxxlens::sdk::unexpected(
					error{"sdk.harness-fault-not-observed", "no_credit", {}});
			return conformance_report{false, outcome.error().code, {}, {}};
		}
		if (fault == provider_fault::cancel_before_run)
		{
			if (outcome || outcome.error().code != "provider.cancelled")
				return cxxlens::sdk::unexpected(
					error{"sdk.harness-fault-not-observed", "cancel_before_run", {}});
		}
		if (sink.frames.empty())
		{
			if (!outcome)
				return conformance_report{false, outcome.error().code, {}, {}};
			return cxxlens::sdk::unexpected(error{"sdk.harness-empty-transcript", "frames", {}});
		}
		if (fault == provider_fault::corrupt_checksum)
			sink.frames.back()[72U] ^= std::byte{0x01};
		else if (fault == provider_fault::truncate_last_frame)
			sink.frames.back().pop_back();
		else if (fault == provider_fault::wrong_direction)
		{
			sink.frames.back()[8U] = std::byte{};
			sink.frames.back()[9U] = static_cast<std::byte>(provider::message_type::credit);
		}
		else if (fault == provider_fault::drop_terminal)
			sink.frames.pop_back();
		else if (fault == provider_fault::wrong_terminal_task)
		{
			auto terminal = provider::decode_frame(sink.frames.back());
			auto control = provider::encode_task_complete_metadata({"foreign-task"});
			if (!terminal || !control)
				return cxxlens::sdk::unexpected(
					error{"sdk.harness-fault-not-observed", "wrong_terminal_task", {}});
			terminal->control = std::move(*control);
			auto encoded = provider::encode_frame(*terminal);
			if (!encoded)
				return cxxlens::sdk::unexpected(std::move(encoded.error()));
			sink.frames.back() = std::move(*encoded);
		}

		conformance_report report;
		for (const auto& encoded : sink.frames)
		{
			auto decoded = provider::decode_frame(encoded);
			if (!decoded)
			{
				report.accepted = false;
				report.reason_code = decoded.error().code;
				report.semantic_transcript = transcript(report.frames);
				return report;
			}
			report.frames.push_back(std::move(*decoded));
		}
		const auto frame_credit = fault == provider_fault::credit_exceeded
			? static_cast<std::uint64_t>(report.frames.size() - 1U)
			: std::uint64_t{4096U};
		auto validated =
			validate_logical_transcript(task,
										implementation.id(),
										implementation.version(),
										report.frames,
										{std::numeric_limits<std::uint64_t>::max(), frame_credit});
		if (!validated)
			return validated;
		if (!outcome && validated->accepted)
		{
			validated->accepted = false;
			validated->reason_code = outcome.error().code;
		}
		return validated;
	}

	result<void> require_golden(const conformance_report& report, const std::string_view expected)
	{
		if (report.semantic_transcript != expected)
			return cxxlens::sdk::unexpected(
				error{"sdk.golden-mismatch", "semantic_transcript", {}});
		return {};
	}
} // namespace cxxlens::sdk::testing
