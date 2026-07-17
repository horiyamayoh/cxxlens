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

		[[nodiscard]] std::vector<std::string_view> split_fields(const std::string_view value,
																 const char separator)
		{
			std::vector<std::string_view> output;
			std::size_t begin{};
			while (true)
			{
				const auto end = value.find(separator, begin);
				output.push_back(value.substr(begin, end - begin));
				if (end == std::string_view::npos)
					return output;
				begin = end + 1U;
			}
		}

		[[nodiscard]] std::optional<std::uint64_t> unsigned_value(const std::string_view value)
		{
			std::uint64_t output{};
			const auto parsed = std::from_chars(value.data(), value.data() + value.size(), output);
			if (value.empty() || parsed.ec != std::errc{} ||
				parsed.ptr != value.data() + value.size())
				return std::nullopt;
			return output;
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
		output_descriptor(const process_task_request& request, const std::string_view id)
		{
			const auto found =
				std::ranges::find(request.output_descriptors, id, &relation_descriptor::id);
			return found == request.output_descriptors.end() ? nullptr : &*found;
		}

		[[nodiscard]] result<protocol_limits> negotiated_limits(const process_task_request& request)
		{
			const auto& offered = request.selection.candidate.description.protocol;
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

		[[nodiscard]] bool canonical_row_shape(const relation_descriptor& descriptor,
											   const std::string_view payload)
		{
			constexpr std::string_view prefix{"{\"cells\":{"};
			const auto suffix =
				std::string{"},\"descriptor_id\":"} + json_string(descriptor.id) + "}";
			if (!cxxlens::sdk::detail::valid_utf8(payload) || !payload.starts_with(prefix) ||
				!payload.ends_with(suffix))
				return false;
			const auto cells =
				payload.substr(prefix.size(), payload.size() - prefix.size() - suffix.size());
			std::set<std::string, std::less<>> seen;
			std::size_t position{};
			while (position < cells.size())
			{
				if (cells[position] != '"')
					return false;
				const auto key_end = cells.find('"', position + 1U);
				if (key_end == std::string_view::npos || key_end + 2U >= cells.size() ||
					cells[key_end + 1U] != ':' || cells[key_end + 2U] != '{')
					return false;
				const std::string key{cells.substr(position + 1U, key_end - position - 1U)};
				if (key.contains('\\') || !seen.insert(key).second)
					return false;
				auto column = descriptor.column(key);
				if (!column)
					return false;

				bool quoted{};
				bool escaped{};
				std::size_t depth{};
				std::size_t object_end = std::string_view::npos;
				for (std::size_t index = key_end + 2U; index < cells.size(); ++index)
				{
					const auto byte = cells[index];
					if (quoted)
					{
						if (escaped)
							escaped = false;
						else if (byte == '\\')
							escaped = true;
						else if (byte == '"')
							quoted = false;
						continue;
					}
					if (byte == '"')
						quoted = true;
					else if (byte == '{')
						++depth;
					else if (byte == '}' && (--depth == 0U))
					{
						object_end = index;
						break;
					}
				}
				if (object_end == std::string_view::npos)
					return false;
				const auto cell = cells.substr(key_end + 2U, object_end - key_end - 1U);
				constexpr std::string_view state_prefix{R"({"state":")"};
				constexpr std::string_view type_separator{R"(","type":)"};
				const auto state_end = cell.find(type_separator, state_prefix.size());
				if (!cell.starts_with(state_prefix) || state_end == std::string_view::npos)
					return false;
				const auto state =
					cell.substr(state_prefix.size(), state_end - state_prefix.size());
				if (state != "present" && state != "absent" && state != "unknown")
					return false;
				const auto typed_prefix = std::string{state_prefix} + std::string{state} +
					std::string{type_separator} + json_string(column->type.canonical_name());
				if (!cell.starts_with(typed_prefix) ||
					(state == "present" && !cell.contains(R"(,"value":)")) ||
					(state == "absent" &&
					 (!column->type.optional || cell.size() != typed_prefix.size() + 1U)) ||
					(state == "unknown" && !cell.contains(R"(,"unknown_reason":)")))
					return false;
				position = object_end + 1U;
				if (position < cells.size())
				{
					if (cells[position] != ',')
						return false;
					++position;
				}
			}
			return std::ranges::all_of(descriptor.columns,
									   [&](const column_descriptor& column)
									   {
										   return !column.required || seen.contains(column.id);
									   });
		}

		[[nodiscard]] result<std::string>
		validate_provider_transcript(const process_task_request& request,
									 const std::span<const frame> frames,
									 const protocol_limits session_limits)
		{
			const auto fail = [](std::string code, std::string field, std::string detail = {})
			{
				return result<std::string>{cxxlens::sdk::unexpected(
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
			bool hello_seen{};
			bool schema_seen{};
			bool accepted{};
			bool coverage_seen{};
			bool unresolved_seen{};
			bool progress_seen{};
			bool terminal_seen{};
			std::string terminal;
			std::set<std::string, std::less<>> batches;
			struct open_batch
			{
				const relation_descriptor* descriptor{};
				std::string id;
				std::uint64_t rows{};
				std::string rolling_digest;
			};
			std::optional<open_batch> batch;
			const auto& provider = request.selection.candidate.description;
			const auto expected_hello = provider.canonical_json();
			const auto expected_schema = std::string{"cxxlens.provider-protocol.v1|minor="} +
				std::to_string(session_limits.maximum_minor);
			const auto expected_accepted = provider.provider_id + "|" +
				provider.provider_version.string() + "|" + request.task_id;

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
				auto control = decode_control_text(value.control);
				if (!control)
					return fail("provider.protocol-state-invalid", request.task_id, "control");
				switch (value.type)
				{
					case message_type::hello:
						if (index != 0U || hello_seen || *control != expected_hello ||
							!value.payload.empty())
							return fail(
								"provider.binary-identity-mismatch", request.task_id, "hello");
						hello_seen = true;
						break;
					case message_type::schema_negotiate:
						if (!hello_seen || schema_seen || accepted || *control != expected_schema ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "schema");
						schema_seen = true;
						break;
					case message_type::task_accepted:
						if (!schema_seen || accepted || *control != expected_accepted ||
							!value.payload.empty())
							return fail(
								"provider.task-binding-mismatch", request.task_id, "accepted");
						accepted = true;
						break;
					case message_type::batch_begin:
					{
						const auto fields = split_fields(*control, '|');
						if (!accepted || batch || fields.size() != 5U ||
							std::ranges::any_of(fields,
												[](const std::string_view field)
												{
													return field.empty();
												}) ||
							!protocol_digest(fields[1U]) || !value.payload.empty() ||
							!batches.insert(std::string{fields[4U]}).second)
							return fail("provider.batch-invalid", request.task_id, "begin");
						if (!offered_relation(provider, fields[0U]))
							return fail(
								"provider.relation-incompatible", std::string{fields[0U]}, "offer");
						const auto* descriptor = output_descriptor(request, fields[0U]);
						if (descriptor == nullptr || descriptor->descriptor_digest != fields[1U])
							return fail("provider.relation-incompatible",
										std::string{fields[0U]},
										"descriptor-digest");
						auto initial = semantic_digest("cxxlens.provider-batch.v1", fields[1U]);
						if (!initial)
							return fail("provider.batch-invalid", request.task_id, "digest");
						batch = open_batch{
							descriptor, std::string{fields[4U]}, 0U, std::move(*initial)};
						break;
					}
					case message_type::column_chunk:
					{
						const auto fields = split_fields(*control, '|');
						const auto ordinal =
							fields.size() == 3U ? unsigned_value(fields[2U]) : std::nullopt;
						const std::string payload{
							reinterpret_cast<const char*>(value.payload.data()),
							value.payload.size()};
						if (!batch || fields.size() != 3U || fields[0U] != batch->id ||
							fields[1U] != "row" || !ordinal || *ordinal != batch->rows ||
							value.payload.empty() ||
							!canonical_row_shape(*batch->descriptor, payload))
							return fail("provider.batch-invalid", request.task_id, "column");
						auto rolling = semantic_digest("cxxlens.provider-batch.v1",
													   batch->rolling_digest + "\n" + payload);
						if (!rolling)
							return fail("provider.batch-invalid", request.task_id, "digest");
						batch->rolling_digest = std::move(*rolling);
						++batch->rows;
						break;
					}
					case message_type::batch_end:
					{
						const auto fields = split_fields(*control, '|');
						const auto rows =
							fields.size() == 3U ? unsigned_value(fields[1U]) : std::nullopt;
						if (!batch || fields.size() != 3U || fields[0U] != batch->id || !rows ||
							*rows != batch->rows || fields[2U] != batch->rolling_digest ||
							!value.payload.empty())
							return fail("provider.batch-invalid", request.task_id, "end");
						batch.reset();
						break;
					}
					case message_type::coverage_chunk:
					{
						if (!accepted || batch || coverage_seen || !value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "coverage");
						coverage_seen = true;
						bool task_covered{};
						std::set<std::pair<std::string, std::string>> seen;
						for (const auto line : split_fields(*control, '\n'))
						{
							const auto fields = split_fields(line, '|');
							static const std::set<std::string_view> states{
								"covered", "excluded", "failed", "not_applicable", "unresolved"};
							if (fields.size() != 4U || fields[0U].empty() || fields[1U].empty() ||
								!states.contains(fields[2U]) ||
								!seen.emplace(fields[0U], fields[1U]).second)
								return fail(
									"provider.coverage-incomplete", request.task_id, "coverage");
							task_covered = task_covered ||
								(fields[0U] == "task" && fields[1U] == request.task_id &&
								 fields[2U] == "covered");
						}
						if (!task_covered)
							return fail("provider.coverage-incomplete", request.task_id, "task");
						break;
					}
					case message_type::unresolved_chunk:
					case message_type::progress:
					{
						bool& seen = value.type == message_type::unresolved_chunk ? unresolved_seen
																				  : progress_seen;
						if (!accepted || batch || seen || !value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "side-channel");
						seen = true;
						if (!control->empty())
							for (const auto line : split_fields(*control, '\n'))
							{
								const auto fields = split_fields(line, '|');
								if (fields.size() != 3U || !namespaced(fields[0U]) ||
									fields[1U].empty())
									return fail("provider.protocol-state-invalid",
												request.task_id,
												"side-channel-value");
							}
						break;
					}
					case message_type::task_complete:
						if (!accepted || batch || !coverage_seen || !unresolved_seen ||
							!progress_seen || *control != request.task_id + "|complete" ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "complete");
						terminal = "provider.success";
						terminal_seen = true;
						break;
					case message_type::task_failed:
					{
						const auto fields = split_fields(*control, '|');
						if (!schema_seen || fields.size() < 2U || !namespaced(fields[0U]) ||
							!value.payload.empty())
							return fail(
								"provider.protocol-state-invalid", request.task_id, "failed");
						terminal = std::string{fields[0U]};
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
		host_transcript(const process_task_request& request, const protocol_limits session_limits)
		{
			const auto& manifest = request.selection.candidate.description;
			std::array values{
				frame{message_type::hello_ack, 1U, 0U, cbor_text(manifest.canonical_json()), {}},
				frame{message_type::schema_negotiate,
					  1U,
					  1U,
					  cbor_text(std::string{"cxxlens.provider-protocol.v1|minor="} +
								std::to_string(session_limits.maximum_minor)),
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
			for (auto& value : values)
			{
				value.protocol_major = session_limits.protocol_major;
				value.protocol_minor = session_limits.maximum_minor;
			}
			std::vector<std::byte> output;
			for (const auto& value : values)
			{
				auto encoded = encode_frame(value, session_limits);
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
		static const std::set<std::string, std::less<>> supported_features{"credit-backpressure"};
		const auto& protocol = request.selection.candidate.description.protocol;
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
				!offered_relation(request.selection.candidate.description, descriptor.id))
				return cxxlens::sdk::unexpected(runtime_error(
					"provider.relation-incompatible", descriptor.id, "output-descriptor"));
		}
		if (auto valid = request.sandbox.validate(); !valid)
			return cxxlens::sdk::unexpected(std::move(valid.error()));
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

		auto frames = decode_frame_stream(output.standard_output, *session_limits);
		if (!frames)
		{
			auto report = transport_failure_report(request, std::move(output), frames.error().code);
			report.diagnostics.push_back(
				{frames.error().code, request.task_id, frames.error().field});
			return report;
		}
		auto terminal = validate_provider_transcript(request, *frames, *session_limits);
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
		const auto& manifest = request.selection.candidate.description;

		process_execution_report report;
		report.terminal = std::move(*terminal);
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
