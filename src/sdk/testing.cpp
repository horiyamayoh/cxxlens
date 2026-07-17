#include <algorithm>
#include <limits>
#include <sstream>
#include <stop_token>

#include <cxxlens/sdk/testing.hpp>

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
			return conformance_report{false, outcome.error().code, {}, {}};
		}
		if (!outcome)
			return cxxlens::sdk::unexpected(std::move(outcome.error()));
		if (sink.frames.empty())
			return cxxlens::sdk::unexpected(error{"sdk.harness-empty-transcript", "frames", {}});
		if (fault == provider_fault::corrupt_checksum)
			sink.frames.back()[72U] ^= std::byte{0x01};
		else if (fault == provider_fault::truncate_last_frame)
			sink.frames.back().pop_back();

		conformance_report report;
		std::uint64_t expected_sequence{};
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
			if (decoded->sequence != expected_sequence++)
				return cxxlens::sdk::unexpected(
					error{"provider.noncontiguous-sequence", "sequence", {}});
			report.frames.push_back(std::move(*decoded));
		}
		report.accepted = true;
		report.reason_code = "accepted";
		report.semantic_transcript = transcript(report.frames);
		return report;
	}

	result<void> require_golden(const conformance_report& report, const std::string_view expected)
	{
		if (report.semantic_transcript != expected)
			return cxxlens::sdk::unexpected(
				error{"sdk.golden-mismatch", "semantic_transcript", {}});
		return {};
	}
} // namespace cxxlens::sdk::testing
