#include <algorithm>
#include <cctype>
#include <charconv>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <sstream>
#include <tuple>

#include <cxxlens/sdk/provider.hpp>

#include "json_internal.hpp"
#include "provider_validation_internal.hpp"

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

		[[nodiscard]] bool protocol_digest(const std::string_view value)
		{
			constexpr std::string_view semantic_prefix{"semantic-v2:"};
			return canonical_digest(value) ||
				(value.starts_with(semantic_prefix) &&
				 canonical_digest(value.substr(semantic_prefix.size())));
		}

		[[nodiscard]] std::string json_string(const std::string_view value)
		{
			return cxxlens::sdk::detail::canonical_json_string(value);
		}

		[[nodiscard]] bool namespaced(const std::string_view value)
		{
			const auto separator = value.find('.');
			return separator != std::string_view::npos && separator != 0U &&
				separator + 1U < value.size();
		}

		[[nodiscard]] bool offered_relation(const manifest& value,
											const std::string_view descriptor)
		{
			return std::ranges::any_of(value.offered_relations,
									   [&](const std::string& offered)
									   {
										   if (offered == descriptor)
											   return true;
										   const auto version = offered.rfind('@');
										   if (version == std::string::npos)
											   return false;
										   return descriptor ==
											   offered.substr(0U, version) + ".v" +
											   offered.substr(version + 1U);
									   });
		}

		[[nodiscard]] const relation_descriptor*
		output_descriptor(const std::span<const relation_descriptor> descriptors,
						  const std::string_view id)
		{
			const auto found = std::ranges::find(descriptors, id, &relation_descriptor::id);
			return found == descriptors.end() ? nullptr : &*found;
		}

		constexpr std::array stable_terminal_reasons{
			std::string_view{"provider.success"},
			std::string_view{"provider.timeout"},
			std::string_view{"provider.cancelled"},
			std::string_view{"provider.output-limit"},
			std::string_view{"provider.crash"},
			std::string_view{"provider.malformed-frame"},
			std::string_view{"provider.checksum-mismatch"},
			std::string_view{"provider.truncated-stream"},
			std::string_view{"provider.schema-invalid"},
			std::string_view{"provider.coverage-incomplete"},
			std::string_view{"provider.runtime-unavailable"},
			std::string_view{"provider.binary-identity-mismatch"},
			std::string_view{"provider.protocol-state-invalid"},
			std::string_view{"provider.credit-exceeded"},
			std::string_view{"provider.backpressure"},
			std::string_view{"provider.task-binding-mismatch"},
			std::string_view{"provider.batch-invalid"},
			std::string_view{"provider.batch-state-invalid"},
			std::string_view{"provider.relation-incompatible"},
			std::string_view{"provider.required-feature-missing"},
			std::string_view{"provider.protocol-minor-mismatch"},
			std::string_view{"provider.unknown-required-extension"},
			std::string_view{"provider.invalid-frame-flags"},
			std::string_view{"provider.unsupported-compression"},
			std::string_view{"provider.frontend-request-invalid"},
			std::string_view{"security.sandbox-insufficient"},
			std::string_view{"security.sandbox-policy-mismatch"},
		};

		[[nodiscard]] bool stable_terminal_reason(const std::string_view reason)
		{
			return std::ranges::find(stable_terminal_reasons, reason) !=
				stable_terminal_reasons.end();
		}

		[[nodiscard]] bool allowed_failure_terminal(const std::string_view reason)
		{
			return reason != "provider.success" && reason.starts_with("provider.") &&
				stable_terminal_reason(reason);
		}

		[[nodiscard]] result<protocol_limits> negotiated_limits(const process_task_request& request)
		{
			const auto& offered = request.selection.selected_candidate().description.protocol;
			const auto minimum =
				std::max<std::uint32_t>(request.limits.minimum_minor, offered.minimum_minor);
			const auto maximum =
				std::min<std::uint32_t>(request.limits.maximum_minor, offered.maximum_minor);
			if (request.limits.protocol_major != offered.major || minimum > maximum ||
				maximum > std::numeric_limits<std::uint16_t>::max())
				return cxxlens::sdk::unexpected(
					runtime_error("provider.protocol-minor-mismatch", "negotiation"));
			auto output = request.limits;
			output.minimum_minor = static_cast<std::uint16_t>(maximum);
			output.maximum_minor = static_cast<std::uint16_t>(maximum);
			return output;
		}
	} // namespace

	namespace detail
	{
		result<transcript_terminal>
		validate_provider_transcript(const transcript_validation_request& request,
									 const std::span<const frame> frames,
									 const protocol_limits session_limits)
		{
			const auto fail = [](std::string code, std::string field, std::string detail = {})
			{
				return result<transcript_terminal>{cxxlens::sdk::unexpected(
					runtime_error(std::move(code), std::move(field), std::move(detail)))};
			};
			std::uint64_t consumed_bytes{};
			for (const auto& value : frames)
			{
				auto encoded = encode_frame(value, session_limits);
				if (!encoded || consumed_bytes > request.output_credit.bytes ||
					encoded->size() > request.output_credit.bytes - consumed_bytes)
					return fail("provider.credit-exceeded", request.task_id, "bytes");
				consumed_bytes += encoded->size();
			}
			if (frames.size() > request.output_credit.frames)
				return fail("provider.credit-exceeded", request.task_id, "frames");

			std::uint64_t expected_sequence{};
			bool hello_seen{!request.require_handshake};
			bool schema_seen{!request.require_handshake};
			bool accepted{};
			bool coverage_seen{};
			bool unresolved_seen{};
			bool progress_seen{};
			bool terminal_seen{};
			transcript_terminal terminal;
			std::set<std::string, std::less<>> batches;
			struct open_batch
			{
				const relation_descriptor* descriptor{};
				std::string dependency_group_id;
				std::string atomic_output_group_id;
				std::string id;
				std::vector<batch_column_summary> columns;
				std::vector<std::string> ordered_chunk_digests;
				std::map<std::string, std::uint64_t, std::less<>> next_row_offsets;
				std::map<std::string, std::uint64_t, std::less<>> next_chunk_indexes;
			};
			std::optional<open_batch> batch;
			const auto expected_hello = request.provider_manifest == nullptr
				? std::string{}
				: request.provider_manifest->canonical_json();
			for (std::size_t index = 0U; index < frames.size(); ++index)
			{
				const auto& value = frames[index];
				if (value.stream_id != 1U || value.sequence != expected_sequence++)
					return fail("provider.protocol-state-invalid", request.task_id, "sequence");
				const auto optional_extension =
					(value.flags & static_cast<std::uint16_t>(frame_flag::optional_extension)) !=
					0U;
				const auto end_of_stream =
					(value.flags & static_cast<std::uint16_t>(frame_flag::end_of_stream)) != 0U;
				if (end_of_stream &&
					(index + 1U != frames.size() ||
					 (value.type != message_type::task_complete &&
					  value.type != message_type::task_failed)))
					return fail(
						"provider.protocol-state-invalid", request.task_id, "end-of-stream");
				if (terminal_seen ||
					(index + 1U == frames.size() && value.type != message_type::task_complete &&
					 value.type != message_type::task_failed))
					return fail(
						"provider.protocol-state-invalid", request.task_id, "terminal-order");
				if (optional_extension)
					continue;
				std::optional<std::string> control;
				if (value.type == message_type::hello)
				{
					auto decoded = decode_control_text(value.control);
					if (!decoded)
						return fail("provider.protocol-state-invalid", request.task_id, "control");
					if (decoded->contains('\0'))
						return fail(
							"provider.protocol-state-invalid", request.task_id, "control-nul");
					control = std::move(*decoded);
				}
				switch (value.type)
				{
					case message_type::hello:
						if (!request.require_handshake || index != 0U || hello_seen ||
							*control != expected_hello || !value.payload.empty())
							return fail(
								"provider.binary-identity-mismatch", request.task_id, "hello");
						hello_seen = true;
						break;
					case message_type::schema_negotiate:
					{
						auto metadata = decode_schema_negotiate_metadata(value.control);
						if (!hello_seen || schema_seen || accepted || !metadata ||
							metadata->protocol_schema != "cxxlens.provider-protocol.v1" ||
							metadata->protocol_minor != session_limits.maximum_minor ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "schema");
						schema_seen = true;
						break;
					}
					case message_type::task_accepted:
					{
						auto metadata = decode_task_accepted_metadata(value.control);
						if (!metadata)
							return fail("provider.protocol-state-invalid",
										request.task_id,
										"control-metadata");
						if (!schema_seen || accepted ||
							metadata->provider_id != request.provider_id ||
							metadata->provider_version != request.provider_version.string() ||
							metadata->task_id != request.task_id || !value.payload.empty())
							return fail(
								"provider.task-binding-mismatch", request.task_id, "accepted");
						accepted = true;
						break;
					}
					case message_type::batch_begin:
					{
						auto metadata = decode_batch_begin_metadata(value.control);
						if (!accepted || batch || !metadata ||
							metadata->task_id != request.task_id ||
							metadata->descriptor_id.empty() ||
							metadata->dependency_group_id.empty() ||
							metadata->atomic_output_group_id.empty() ||
							metadata->batch_id.empty() ||
							!protocol_digest(metadata->descriptor_digest) ||
							!value.payload.empty() || !batches.insert(metadata->batch_id).second)
							return fail("provider.batch-invalid", request.task_id, "begin");
						if (request.provider_manifest != nullptr &&
							!offered_relation(*request.provider_manifest, metadata->descriptor_id))
							return fail(
								"provider.relation-incompatible", metadata->descriptor_id, "offer");
						const auto* descriptor =
							output_descriptor(request.output_descriptors, metadata->descriptor_id);
						if (descriptor == nullptr ||
							descriptor->descriptor_digest != metadata->descriptor_digest)
							return fail("provider.relation-incompatible",
										metadata->descriptor_id,
										"descriptor-digest");
						open_batch opened{descriptor,
										  std::move(metadata->dependency_group_id),
										  std::move(metadata->atomic_output_group_id),
										  std::move(metadata->batch_id),
										  {},
										  {},
										  {},
										  {}};
						for (const auto& column : descriptor->columns)
						{
							opened.columns.push_back({column.id, 0U, 0U});
							opened.next_row_offsets.emplace(column.id, 0U);
							opened.next_chunk_indexes.emplace(column.id, 0U);
						}
						batch = std::move(opened);
						break;
					}
					case message_type::column_chunk:
					{
						if (!batch || value.payload.empty() || batch->descriptor->columns.empty())
							return fail("provider.batch-invalid", request.task_id, "column");
						auto chunk =
							decode_column_chunk(value.control, value.payload, *batch->descriptor);
						if (!chunk)
							return fail(
								"provider.batch-invalid", request.task_id, chunk.error().detail);
						const auto expected_column_index =
							batch->ordered_chunk_digests.size() % batch->descriptor->columns.size();
						const auto& expected_column =
							batch->descriptor->columns[expected_column_index];
						const auto summary = std::ranges::find(
							batch->columns, chunk->column_id, &batch_column_summary::column_id);
						if (chunk->task_id != request.task_id ||
							chunk->dependency_group_id != batch->dependency_group_id ||
							chunk->atomic_output_group_id != batch->atomic_output_group_id ||
							chunk->batch_id != batch->id ||
							chunk->descriptor_id != batch->descriptor->id ||
							chunk->descriptor_digest != batch->descriptor->descriptor_digest ||
							chunk->column_id != expected_column.id ||
							summary == batch->columns.end() ||
							chunk->row_offset != batch->next_row_offsets.at(chunk->column_id) ||
							chunk->chunk_index != batch->next_chunk_indexes.at(chunk->column_id))
							return fail("provider.batch-invalid", request.task_id, "chunk-binding");
						summary->payload_bytes += value.payload.size();
						++summary->chunk_count;
						batch->next_row_offsets.at(chunk->column_id) += chunk->row_count;
						++batch->next_chunk_indexes.at(chunk->column_id);
						batch->ordered_chunk_digests.push_back(std::move(chunk->chunk_digest));
						break;
					}
					case message_type::batch_end:
					{
						if (!batch)
							return fail(
								"provider.batch-invalid", request.task_id, "end-without-batch");
						auto batch_terminal =
							decode_columnar_batch_end(value.control, value.payload);
						if (!batch_terminal)
							return fail("provider.batch-invalid",
										request.task_id,
										batch_terminal.error().detail);
						const bool all_rows_match =
							std::ranges::all_of(batch->next_row_offsets,
												[&](const auto& item)
												{
													return item.second == batch_terminal->row_count;
												});
						if (batch_terminal->task_id != request.task_id ||
							batch_terminal->dependency_group_id != batch->dependency_group_id ||
							batch_terminal->atomic_output_group_id !=
								batch->atomic_output_group_id ||
							batch_terminal->batch_id != batch->id ||
							batch_terminal->descriptor_id != batch->descriptor->id ||
							batch_terminal->descriptor_digest !=
								batch->descriptor->descriptor_digest ||
							batch_terminal->columns != batch->columns ||
							batch_terminal->ordered_chunk_digests != batch->ordered_chunk_digests ||
							!all_rows_match)
							return fail("provider.batch-invalid", request.task_id, "end");
						batch.reset();
						break;
					}
					case message_type::coverage_chunk:
					{
						auto records = decode_coverage_metadata(value.control);
						if (!accepted || batch || coverage_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "coverage");
						coverage_seen = true;
						bool task_covered{};
						std::set<std::pair<std::string, std::string>> seen;
						for (const auto& record : *records)
						{
							static const std::set<std::string_view> states{
								"covered", "excluded", "failed", "not_applicable", "unresolved"};
							if (record.kind.empty() || record.id.empty() ||
								!states.contains(record.state) ||
								!seen.emplace(record.kind, record.id).second)
								return fail(
									"provider.coverage-incomplete", request.task_id, "coverage");
							task_covered = task_covered ||
								(record.kind == "task" && record.id == request.task_id &&
								 record.state == "covered");
						}
						if (!task_covered)
							return fail("provider.coverage-incomplete", request.task_id, "task");
						break;
					}
					case message_type::unresolved_chunk:
					{
						auto records = decode_unresolved_metadata(value.control);
						if (!accepted || batch || unresolved_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "side-channel");
						unresolved_seen = true;
						for (const auto& record : *records)
							if (!namespaced(record.code) || record.code.contains('\0') ||
								record.subject.empty())
								return fail("provider.protocol-state-invalid",
											request.task_id,
											"side-channel-value");
						break;
					}
					case message_type::progress:
					{
						auto records = decode_evidence_metadata(value.control);
						if (!accepted || batch || progress_seen || !records ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "side-channel");
						progress_seen = true;
						for (const auto& record : *records)
							if (!namespaced(record.kind) || record.kind.contains('\0') ||
								record.subject.empty() || record.producer.empty())
								return fail("provider.protocol-state-invalid",
											request.task_id,
											"side-channel-value");
						break;
					}
					case message_type::task_complete:
					{
						auto metadata = decode_task_complete_metadata(value.control);
						if (!accepted || batch || !coverage_seen || !unresolved_seen ||
							!progress_seen || !metadata || metadata->task_id != request.task_id ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "complete");
						terminal = {transcript_terminal_kind::complete, "provider.success"};
						terminal_seen = true;
						break;
					}
					case message_type::task_failed:
					{
						auto metadata = decode_task_failed_metadata(value.control);
						if (!metadata || metadata->error_code.contains('\0'))
							return fail("provider.protocol-state-invalid",
										request.task_id,
										"control-metadata");
						if (!schema_seen || !namespaced(metadata->error_code) ||
							metadata->task_id != request.task_id || !value.payload.empty())
							return fail(
								"provider.task-binding-mismatch", request.task_id, "failed");
						if (!allowed_failure_terminal(metadata->error_code))
							return fail(
								"provider.schema-invalid", request.task_id, "failure-reason");
						terminal = {transcript_terminal_kind::failed,
									std::move(metadata->error_code)};
						terminal_seen = true;
						break;
					}
					case message_type::hello_ack:
					case message_type::open_task:
					case message_type::input_descriptor:
					case message_type::input_chunk:
					case message_type::credit:
					case message_type::batch_ack:
					case message_type::batch_reject:
					case message_type::cancel:
						return fail(
							"provider.protocol-state-invalid", request.task_id, "direction");
					case message_type::closure_candidate:
					case message_type::resume:
					case message_type::close:
						return fail(
							"provider.protocol-state-invalid", request.task_id, "unsupported");
				}
			}
			if (!hello_seen || !schema_seen || !terminal_seen)
				return fail("provider.truncated-stream", request.task_id, "state");
			return terminal;
		}
	} // namespace detail

	namespace
	{

		[[nodiscard]] result<std::vector<std::byte>>
		host_transcript(const process_task_request& request, const protocol_limits session_limits)
		{
			const auto& manifest = request.selection.selected_candidate().description;
			return encode_host_transcript({{manifest.canonical_json(),
											{request.task_id,
											 request.task_input_digest,
											 request.normalized_invocation_digest,
											 request.toolchain_digest,
											 request.environment_digest},
											session_limits},
										   request.output_credit,
										   request.payload});
		}

		[[nodiscard]] std::string transcript_projection(const std::span<const frame> frames)
		{
			std::ostringstream output;
			for (const auto& value : frames)
				output << value.protocol_major << '|' << value.protocol_minor << '|' << value.flags
					   << '|' << static_cast<std::uint16_t>(value.type) << '|' << value.stream_id
					   << '|' << value.sequence << '|' << content_digest(value.control) << '|'
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
			return is_valid(achieved) && is_valid(required) &&
				static_cast<std::uint8_t>(achieved) >= static_cast<std::uint8_t>(required);
		}

		[[nodiscard]] std::optional<sandbox_assurance>
		parse_sandbox_assurance(const std::string_view value) noexcept
		{
			if (value == "none")
				return sandbox_assurance::none;
			if (value == "best_effort")
				return sandbox_assurance::best_effort;
			if (value == "enforced")
				return sandbox_assurance::enforced;
			if (value == "certified")
				return sandbox_assurance::certified;
			return std::nullopt;
		}

		[[nodiscard]] result<sandbox_requirement>
		effective_sandbox(const process_task_request& request)
		{
			const auto& authority = request.selection.authority_request().sandbox;
			if (auto valid = authority.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (auto valid = request.sandbox.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (request.sandbox.policy_digest != authority.policy_digest)
				return cxxlens::sdk::unexpected(
					runtime_error("security.sandbox-policy-mismatch", "selection"));
			const auto manifest_minimum = parse_sandbox_assurance(
				request.selection.selected_candidate().description.sandbox_minimum);
			if (!manifest_minimum)
				return cxxlens::sdk::unexpected(
					runtime_error("provider.manifest-invalid", "sandbox_minimum"));
			auto minimum = authority.minimum;
			if (sandbox_satisfies(request.sandbox.minimum, minimum))
				minimum = request.sandbox.minimum;
			if (sandbox_satisfies(*manifest_minimum, minimum))
				minimum = *manifest_minimum;
			return sandbox_requirement{minimum, authority.policy_digest};
		}

		[[nodiscard]] process_execution_report transport_failure_report(
			const process_task_request& request, process_output output, std::string terminal)
		{
			process_execution_report report;
			report.terminal = stable_terminal_reason(terminal)
				? std::move(terminal)
				: std::string{"provider.runtime-unavailable"};
			report.provider = request.selection.selected_candidate().description;
			report.task_input_digest = request.task_input_digest;
			report.normalized_invocation_digest = request.normalized_invocation_digest;
			report.toolchain_digest = request.toolchain_digest;
			report.environment_digest = request.environment_digest;
			report.measured_executable_digest = std::move(output.measured_executable_digest);
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
		return validated_success_ && terminal == "provider.success" && !frames.empty() &&
			frames.back().type == message_type::task_complete;
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
			   << json_string(toolchain_digest) << R"(},"measured_executable_digest":)";
		if (measured_executable_digest.empty())
			output << "null";
		else
			output << json_string(measured_executable_digest);
		output << R"(,"provider":{"binary_digest":)" << json_string(provider.provider_binary_digest)
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
				   << environment_digest << '|' << measured_executable_digest << '|'
				   << sandbox.canonical_form() << '|' << transcript_projection(frames);
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
		if (processes_ == nullptr)
			return cxxlens::sdk::unexpected(
				runtime_error("provider.runtime-unavailable", "process-port"));
		if (auto valid = request.selection.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (request.task_id.empty() || request.task_id.contains('\0') ||
			request.selection.selected_candidate().executable_argv.empty() ||
			request.selection.selected_candidate().executable_argv.front().empty() ||
			!canonical_digest(request.task_input_digest) ||
			!canonical_digest(request.normalized_invocation_digest) ||
			!canonical_digest(request.toolchain_digest) ||
			!canonical_digest(request.environment_digest) ||
			content_digest(request.payload) != request.task_input_digest)
			return cxxlens::sdk::unexpected(
				runtime_error("provider.task-invalid", request.task_id));
		if (auto valid = request.selection.selected_candidate().description.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		static const std::set<std::string, std::less<>> supported_features{"credit-backpressure"};
		const auto& protocol = request.selection.selected_candidate().description.protocol;
		if (std::ranges::any_of(protocol.required_features,
								[&](const std::string& feature)
								{
									return !supported_features.contains(feature);
								}))
			return cxxlens::sdk::unexpected(
				runtime_error("provider.required-feature-missing", "protocol"));
		if (request.output_descriptors.empty())
			return cxxlens::sdk::unexpected(
				runtime_error("provider.task-invalid", "output_descriptors"));
		std::set<std::string, std::less<>> output_descriptor_ids;
		for (const auto& descriptor : request.output_descriptors)
		{
			if (auto valid = descriptor.validate(); !valid)
				return cxxlens::sdk::unexpected(std::move(valid.error()));
			if (!output_descriptor_ids.insert(descriptor.id).second ||
				!offered_relation(request.selection.selected_candidate().description,
								  descriptor.id))
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.relation-incompatible", descriptor.id, "output-descriptor"));
		}
		if (auto valid = request.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		auto sandbox = effective_sandbox(request);
		if (!sandbox)
			return cxxlens::sdk::unexpected(std::move(sandbox.error()));
		if (request.budget.wall_ms == 0U || request.budget.cpu_ms == 0U ||
			request.budget.rss_bytes == 0U || request.budget.output_bytes == 0U ||
			request.budget.rows == 0U || request.budget.diagnostics == 0U ||
			request.budget.open_files == 0U || request.budget.created_files == 0U ||
			request.budget.subprocesses == 0U || request.output_credit.bytes == 0U ||
			request.output_credit.frames == 0U)
			return cxxlens::sdk::unexpected(runtime_error("provider.task-invalid", "budget"));

		auto session_limits = negotiated_limits(request);
		if (!session_limits)
			return cxxlens::sdk::unexpected(std::move(session_limits.error()));
		auto transcript = host_transcript(request, *session_limits);
		if (!transcript)
			return cxxlens::sdk::unexpected(std::move(transcript.error()));
		process_invocation invocation;
		invocation.argv = request.selection.selected_candidate().executable_argv;
		invocation.standard_input = std::move(*transcript);
		invocation.environment = {
			{"CXXLENS_PROVIDER_ID", request.selection.selected_candidate().description.provider_id},
			{"CXXLENS_PROVIDER_MANIFEST",
			 request.selection.selected_candidate().description.canonical_json()},
			{"CXXLENS_PROVIDER_BINARY_DIGEST",
			 request.selection.selected_candidate().description.provider_binary_digest},
			{"CXXLENS_PROVIDER_SEMANTIC_CONTRACT_DIGEST",
			 request.selection.selected_candidate().description.provider_semantic_contract_digest},
			{"CXXLENS_PROVIDER_TASK_ID", request.task_id},
			{"CXXLENS_PROVIDER_TASK_INPUT_DIGEST", request.task_input_digest},
			{"CXXLENS_PROVIDER_NORMALIZED_INVOCATION_DIGEST", request.normalized_invocation_digest},
			{"CXXLENS_PROVIDER_TOOLCHAIN_DIGEST", request.toolchain_digest},
			{"CXXLENS_PROVIDER_ENVIRONMENT_DIGEST", request.environment_digest},
			{"CXXLENS_PROVIDER_PROTOCOL_MAJOR", std::to_string(session_limits->protocol_major)},
			{"CXXLENS_PROVIDER_PROTOCOL_MINOR", std::to_string(session_limits->maximum_minor)},
		};
		invocation.budget = request.budget;
		invocation.sandbox = *sandbox;
		invocation.expected_binary_digest =
			request.selection.selected_candidate().description.provider_binary_digest;
		auto launched = processes_->run(invocation, request.cancellation);
		if (!launched)
			return cxxlens::sdk::unexpected(std::move(launched.error()));
		auto output = std::move(*launched);
		if (auto valid = output.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
		if (output.sandbox.policy_digest != sandbox->policy_digest)
			return transport_failure_report(
				request, std::move(output), "security.sandbox-policy-mismatch");
		auto applied_policy = resolve_sandbox_policy(output.sandbox.policy_digest);
		const auto& selected_binary_digest =
			request.selection.selected_candidate().description.provider_binary_digest;
		const auto evidence_binary_digest = canonical_digest(output.measured_executable_digest)
			? std::string_view{output.measured_executable_digest}
			: std::string_view{selected_binary_digest};
		auto evidence = applied_policy
			? sandbox_evidence_digest(*applied_policy,
									  request.budget,
									  output.sandbox.achieved,
									  output.sandbox.mechanisms,
									  evidence_binary_digest)
			: result<std::string>{
				  unexpected(runtime_error("security.sandbox-policy-mismatch", "unknown-policy"))};
		if (!evidence || output.sandbox.evidence_digest != *evidence)
			return transport_failure_report(
				request, std::move(output), "security.sandbox-policy-mismatch");
		if (sandbox_satisfies(output.sandbox.achieved, sandbox_assurance::enforced) &&
			output.measured_executable_digest != selected_binary_digest)
			return transport_failure_report(
				request, std::move(output), "provider.binary-identity-mismatch");
		if (sandbox_satisfies(output.sandbox.achieved, sandbox_assurance::enforced))
		{
			auto actual_mechanisms = output.sandbox.mechanisms;
			std::ranges::sort(actual_mechanisms);
			if (actual_mechanisms != applied_policy->mechanisms)
				return transport_failure_report(
					request, std::move(output), "security.sandbox-policy-mismatch");
		}
		if (output.status != process_status::exited)
		{
			const auto terminal = output.failure_code.empty() ? terminal_for_status(output.status)
															  : output.failure_code;
			return transport_failure_report(request, std::move(output), terminal);
		}
		if (!sandbox_satisfies(output.sandbox.achieved, sandbox->minimum))
			return transport_failure_report(
				request, std::move(output), "security.sandbox-insufficient");
		if (output.exit_code != 0)
			return transport_failure_report(request, std::move(output), "provider.crash");

		auto frames = decode_frame_stream(output.standard_output, *session_limits);
		if (!frames)
		{
			auto report = transport_failure_report(request, std::move(output), frames.error().code);
			report.diagnostics.push_back(
				{frames.error().code, request.task_id, frames.error().field});
			return report;
		}
		const auto& selected_manifest = request.selection.selected_candidate().description;
		const detail::transcript_validation_request validation{
			request.task_id,
			selected_manifest.provider_id,
			selected_manifest.provider_version,
			&selected_manifest,
			request.output_descriptors,
			request.output_credit,
			true,
		};
		auto terminal = detail::validate_provider_transcript(validation, *frames, *session_limits);
		if (!terminal)
		{
			auto validation_error = std::move(terminal.error());
			auto report =
				transport_failure_report(request, std::move(output), validation_error.code);
			report.frames = std::move(*frames);
			report.diagnostics.push_back(
				{validation_error.code, validation_error.field, validation_error.detail});
			return report;
		}
		const auto& manifest = selected_manifest;

		process_execution_report report;
		report.terminal = terminal->reason;
		report.validated_success_ = terminal->kind == detail::transcript_terminal_kind::complete;
		report.provider = manifest;
		report.task_input_digest = request.task_input_digest;
		report.normalized_invocation_digest = request.normalized_invocation_digest;
		report.toolchain_digest = request.toolchain_digest;
		report.environment_digest = request.environment_digest;
		report.measured_executable_digest = std::move(output.measured_executable_digest);
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
