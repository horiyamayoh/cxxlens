#include <algorithm>
#include <cctype>
#include <limits>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/provider.hpp>

#include "json_internal.hpp"

namespace cxxlens::sdk::provider
{
	namespace
	{
		[[nodiscard]] error
		runtime_error(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool canonical_digest(const std::string_view value)
		{
			return value.starts_with("sha256:") && value.size() == 71U &&
				std::ranges::all_of(value.substr(7U),
									[](const char byte)
									{
										return std::isdigit(static_cast<unsigned char>(byte)) !=
											0 ||
											(byte >= 'a' && byte <= 'f');
									});
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return cxxlens::sdk::detail::canonical_json_string(value);
		}

		template <std::unsigned_integral T>
		void append_big_endian(std::vector<std::byte>& output, const T value)
		{
			for (std::size_t index = sizeof(T); index > 0U; --index)
				output.push_back(static_cast<std::byte>(value >> ((index - 1U) * 8U)));
		}

		[[nodiscard]] std::vector<std::byte> cbor_text(const std::string_view text)
		{
			std::vector<std::byte> output;
			if (text.size() < 24U)
				output.push_back(static_cast<std::byte>(0x60U | text.size()));
			else if (text.size() <= std::numeric_limits<std::uint8_t>::max())
			{
				output.push_back(std::byte{0x78});
				output.push_back(static_cast<std::byte>(text.size()));
			}
			else if (text.size() <= std::numeric_limits<std::uint16_t>::max())
			{
				output.push_back(std::byte{0x79});
				append_big_endian(output, static_cast<std::uint16_t>(text.size()));
			}
			else
			{
				output.push_back(std::byte{0x7a});
				append_big_endian(output, static_cast<std::uint32_t>(text.size()));
			}
			for (const auto byte : text)
				output.push_back(static_cast<std::byte>(static_cast<unsigned char>(byte)));
			return output;
		}

		[[nodiscard]] result<std::vector<std::byte>>
		host_transcript(const process_task_request& request)
		{
			const auto& manifest = request.selection.candidate.description;
			const std::array values{
				frame{message_type::hello_ack,
					  1U,
					  0U,
					  cbor_text(manifest.provider_id + "|" + manifest.provider_version.string() +
								"|" + manifest.provider_binary_digest + "|" +
								manifest.provider_semantic_contract_digest),
					  {}},
				frame{message_type::schema_negotiate,
					  1U,
					  1U,
					  cbor_text("cxxlens.provider-protocol.v1|minor=0"),
					  {}},
				frame{message_type::open_task,
					  1U,
					  2U,
					  cbor_text(request.task_id + "|" + request.task_input_digest + "|" +
								request.normalized_invocation_digest + "|" +
								request.toolchain_digest + "|" + request.environment_digest),
					  request.payload},
				frame{message_type::credit,
					  1U,
					  3U,
					  cbor_text(std::to_string(request.output_credit.bytes) + "|" +
								std::to_string(request.output_credit.frames)),
					  {}},
				frame{message_type::close, 1U, 4U, cbor_text(request.task_id), {}},
			};
			std::vector<std::byte> output;
			for (const auto& value : values)
			{
				auto encoded = encode_frame(value, request.limits);
				if (!encoded)
					return cxxlens::sdk::unexpected(std::move(encoded.error()));
				output.insert(output.end(), encoded->begin(), encoded->end());
			}
			return output;
		}

		[[nodiscard]] std::string transcript_projection(const std::span<const frame> frames)
		{
			std::ostringstream output;
			for (const auto& value : frames)
				output << static_cast<std::uint16_t>(value.type) << '|' << value.stream_id << '|'
					   << value.sequence << '|' << content_digest(value.control) << '|'
					   << content_digest(value.payload) << '\n';
			return output.str();
		}

		[[nodiscard]] std::string terminal_for_status(const process_status status)
		{
			switch (status)
			{
				case process_status::timed_out:
					return "provider.timeout";
				case process_status::cancelled:
					return "provider.cancelled";
				case process_status::output_limit:
					return "provider.output-limit";
				case process_status::crashed:
					return "provider.crash";
				case process_status::unavailable:
				case process_status::launch_failed:
					return "provider.runtime-unavailable";
				case process_status::exited:
					return "provider.crash";
			}
			return "provider.runtime-unavailable";
		}

		[[nodiscard]] bool sandbox_satisfies(const sandbox_assurance achieved,
											 const sandbox_assurance required) noexcept
		{
			return static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
		}

		[[nodiscard]] process_execution_report transport_failure_report(
			const process_task_request& request, process_output output, std::string terminal)
		{
			process_execution_report report;
			report.terminal = std::move(terminal);
			report.provider = request.selection.candidate.description;
			report.task_input_digest = request.task_input_digest;
			report.normalized_invocation_digest = request.normalized_invocation_digest;
			report.toolchain_digest = request.toolchain_digest;
			report.environment_digest = request.environment_digest;
			report.sandbox = std::move(output.sandbox);
			report.exit_code = output.exit_code;
			report.termination_signal = output.termination_signal;
			if (!output.standard_error.empty())
				report.diagnostics.push_back(
					{"provider.worker-stderr", request.task_id, std::move(output.standard_error)});
			return report;
		}
	} // namespace

	bool process_execution_report::succeeded() const noexcept
	{
		return terminal == "provider.success";
	}

	std::string process_execution_report::canonical_form() const
	{
		std::ostringstream output;
		output << "{\"diagnostics\":[";
		for (std::size_t index = 0U; index < diagnostics.size(); ++index)
		{
			if (index != 0U)
				output << ',';
			const auto& diagnostic = diagnostics[index];
			output << "{\"code\":" << json_string(diagnostic.code)
				   << ",\"detail\":" << json_string(diagnostic.detail)
				   << ",\"subject\":" << json_string(diagnostic.subject) << '}';
		}
		const auto transcript = transcript_projection(frames);
		output << R"(],"frames":{"count":)" << frames.size() << R"(,"last_sequence":)";
		if (frames.empty())
			output << "null";
		else
			output << frames.back().sequence;
		output << R"(,"transcript_digest":)"
			   << json_string(
					  *cxxlens::sdk::semantic_digest("cxxlens.provider-transcript.v1", transcript))
			   << R"(},"input_binding":{"environment":)" << json_string(environment_digest)
			   << R"(,"invocation":)" << json_string(normalized_invocation_digest) << R"(,"task":)"
			   << json_string(task_input_digest) << R"(,"toolchain":)"
			   << json_string(toolchain_digest) << R"(},"provider":{"binary_digest":)"
			   << json_string(provider.provider_binary_digest)
			   << ",\"id\":" << json_string(provider.provider_id)
			   << ",\"semantic_contract_digest\":"
			   << json_string(provider.provider_semantic_contract_digest)
			   << ",\"version\":" << json_string(provider.provider_version.string())
			   << "},\"sandbox\":" << sandbox.canonical_form()
			   << R"(,"schema":"cxxlens.provider-execution-report.v1","semantic_digest":)"
			   << json_string(semantic_digest()) << ",\"terminal\":" << json_string(terminal)
			   << '}';
		return output.str();
	}

	std::string process_execution_report::semantic_digest() const
	{
		std::ostringstream projection;
		projection << terminal << '|' << provider.provider_id << '|'
				   << provider.provider_version.string() << '|' << provider.provider_binary_digest
				   << '|' << provider.provider_semantic_contract_digest << '|' << task_input_digest
				   << '|' << normalized_invocation_digest << '|' << toolchain_digest << '|'
				   << environment_digest << '|' << sandbox.canonical_form() << '|'
				   << transcript_projection(frames);
		for (const auto& diagnostic : diagnostics)
			projection << diagnostic.code << '|' << diagnostic.subject << '|' << diagnostic.detail
					   << '\n';
		return *cxxlens::sdk::semantic_digest("cxxlens.provider-execution-report.v1",
											  projection.str());
	}

	process_provider_runtime::process_provider_runtime(const provider_process_port& processes)
		: processes_{&processes}
	{
	}

	result<process_execution_report>
	process_provider_runtime::execute(const process_task_request& request) const
	{
		if (processes_ == nullptr || request.task_id.empty() || request.task_id.contains('|') ||
			request.selection.candidate.executable_argv.empty() ||
			request.selection.candidate.executable_argv.front().empty() ||
			!canonical_digest(request.task_input_digest) ||
			!canonical_digest(request.normalized_invocation_digest) ||
			!canonical_digest(request.toolchain_digest) ||
			!canonical_digest(request.environment_digest) ||
			content_digest(request.payload) != request.task_input_digest)
			return cxxlens::sdk::unexpected(
				runtime_error("provider.task-invalid", request.task_id));
		if (auto valid = request.selection.candidate.description.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (auto valid = request.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (request.budget.wall_ms == 0U || request.budget.cpu_ms == 0U ||
			request.budget.rss_bytes == 0U || request.budget.output_bytes == 0U ||
			request.budget.rows == 0U || request.budget.diagnostics == 0U ||
			request.budget.open_files == 0U || request.budget.created_files == 0U ||
			request.budget.subprocesses == 0U || request.output_credit.bytes == 0U ||
			request.output_credit.frames == 0U)
			return cxxlens::sdk::unexpected(runtime_error("provider.task-invalid", "budget"));

		auto transcript = host_transcript(request);
		if (!transcript)
			return cxxlens::sdk::unexpected(std::move(transcript.error()));
		process_invocation invocation;
		invocation.argv = request.selection.candidate.executable_argv;
		invocation.standard_input = std::move(*transcript);
		invocation.environment = {
			{"CXXLENS_PROVIDER_ID", request.selection.candidate.description.provider_id},
			{"CXXLENS_PROVIDER_BINARY_DIGEST",
			 request.selection.candidate.description.provider_binary_digest},
			{"CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST",
			 request.selection.candidate.description.provider_semantic_contract_digest},
			{"CXXLENS_PROVIDER_TASK_ID", request.task_id},
		};
		invocation.budget = request.budget;
		invocation.sandbox = request.sandbox;
		invocation.expected_binary_digest =
			request.selection.candidate.description.provider_binary_digest;
		auto launched = processes_->run(invocation, request.cancellation);
		if (!launched)
			return cxxlens::sdk::unexpected(std::move(launched.error()));
		auto output = std::move(*launched);
		if (auto valid = output.sandbox.validate(); !valid)
			return transport_failure_report(
				request, std::move(output), "provider.runtime-unavailable");
		if (output.sandbox.policy_digest != request.sandbox.policy_digest ||
			!sandbox_satisfies(output.sandbox.achieved, request.sandbox.minimum))
			return transport_failure_report(
				request, std::move(output), "security.sandbox-insufficient");
		if (output.status != process_status::exited)
		{
			const auto terminal = output.failure_code.empty() ? terminal_for_status(output.status)
															  : output.failure_code;
			return transport_failure_report(request, std::move(output), terminal);
		}
		if (output.exit_code != 0)
			return transport_failure_report(request, std::move(output), "provider.crash");

		auto frames = decode_frame_stream(output.standard_output, request.limits);
		if (!frames)
		{
			auto report = transport_failure_report(request, std::move(output), frames.error().code);
			report.diagnostics.push_back(
				{frames.error().code, request.task_id, frames.error().field});
			return report;
		}
		std::uint64_t expected_sequence{};
		bool terminal_seen{};
		std::string terminal;
		for (std::size_t index = 0U; index < frames->size(); ++index)
		{
			const auto& value = (*frames)[index];
			if (value.stream_id != 1U || value.sequence != expected_sequence++)
				return transport_failure_report(
					request, std::move(output), "provider.malformed-frame");
			if (terminal_seen)
				return transport_failure_report(
					request, std::move(output), "provider.malformed-frame");
			if (value.type == message_type::task_complete)
			{
				terminal = "provider.success";
				terminal_seen = true;
			}
			else if (value.type == message_type::task_failed)
			{
				auto decoded = decode_control_text(value.control);
				terminal = decoded && decoded->starts_with("provider.")
					? decoded->substr(0U, decoded->find('|'))
					: "provider.schema-invalid";
				terminal_seen = true;
			}
			if (terminal_seen && index + 1U != frames->size())
				return transport_failure_report(
					request, std::move(output), "provider.malformed-frame");
		}
		if (frames->front().type != message_type::hello || !terminal_seen)
			return transport_failure_report(
				request, std::move(output), "provider.truncated-stream");
		auto hello = decode_control_text(frames->front().control);
		const auto& manifest = request.selection.candidate.description;
		const auto expected_hello = manifest.provider_id + "|" +
			manifest.provider_version.string() + "|" + manifest.provider_binary_digest + "|" +
			manifest.provider_semantic_contract_digest;
		if (!hello || *hello != expected_hello)
			return transport_failure_report(
				request, std::move(output), "provider.binary-identity-mismatch");

		process_execution_report report;
		report.terminal = std::move(terminal);
		report.provider = manifest;
		report.task_input_digest = request.task_input_digest;
		report.normalized_invocation_digest = request.normalized_invocation_digest;
		report.toolchain_digest = request.toolchain_digest;
		report.environment_digest = request.environment_digest;
		report.sandbox = std::move(output.sandbox);
		report.frames = std::move(*frames);
		report.exit_code = output.exit_code;
		report.termination_signal = output.termination_signal;
		if (!output.standard_error.empty())
			report.diagnostics.push_back(
				{"provider.worker-stderr", request.task_id, std::move(output.standard_error)});
		return report;
	}
} // namespace cxxlens::sdk::provider
