#include "frontend_worker_ipc.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "../../workspace/catalog_access.hpp"

namespace cxxlens::detail::frontend
{
	namespace
	{
		constexpr std::string_view request_magic = "CXXLFWQ1";
		constexpr std::string_view response_magic = "CXXLFWR1";
		constexpr std::size_t maximum_message_bytes = std::size_t{64U} * 1024U * 1024U;
		constexpr std::uint64_t maximum_collection_size = 1'000'000U;
		constexpr std::size_t maximum_error_depth = 32U;

		[[nodiscard]] error ipc_error(std::string reason)
		{
			error failure;
			failure.code.value = "parse.frontend-failed";
			failure.message = "Frontend worker IPC is invalid";
			failure.scope = failure_scope::compile_unit;
			failure.attributes.emplace("reason", std::move(reason));
			return failure;
		}

		class writer
		{
		  public:
			template <typename Integer>
			void integer(Integer value)
			{
				using unsigned_type = std::make_unsigned_t<Integer>;
				auto encoded = static_cast<unsigned_type>(value);
				for (std::size_t index{}; index < sizeof(Integer); ++index)
				{
					bytes_.push_back(static_cast<char>(encoded & 0xffU));
					encoded = static_cast<unsigned_type>(encoded / 256U);
				}
			}

			void boolean(const bool value)
			{
				integer<std::uint8_t>(value ? 1U : 0U);
			}

			void string(const std::string_view value)
			{
				integer<std::uint64_t>(value.size());
				bytes_.append(value);
			}

			void raw(const std::string_view value)
			{
				bytes_.append(value);
			}

			[[nodiscard]] std::string finish() &&
			{
				return std::move(bytes_);
			}

		  private:
			std::string bytes_;
		};

		class reader
		{
		  public:
			explicit reader(const std::string_view bytes) : bytes_{bytes} {}

			template <typename Integer>
			[[nodiscard]] std::optional<Integer> integer()
			{
				if (remaining() < sizeof(Integer))
					return std::nullopt;
				using unsigned_type = std::make_unsigned_t<Integer>;
				unsigned_type decoded{};
				for (std::size_t index{}; index < sizeof(Integer); ++index)
				{
					const auto byte = static_cast<unsigned char>(bytes_[offset_++]);
					decoded |= static_cast<unsigned_type>(byte) << (index * 8U);
				}
				return static_cast<Integer>(decoded);
			}

			[[nodiscard]] std::optional<bool> boolean()
			{
				const auto value = integer<std::uint8_t>();
				if (!value || *value > 1U)
					return std::nullopt;
				return *value == 1U;
			}

			[[nodiscard]] std::optional<std::string> string()
			{
				const auto size = integer<std::uint64_t>();
				if (!size || *size > remaining())
					return std::nullopt;
				const auto count = static_cast<std::size_t>(*size);
				std::string output{bytes_.substr(offset_, count)};
				offset_ += count;
				return output;
			}

			[[nodiscard]] bool consume(const std::string_view expected)
			{
				if (bytes_.substr(offset_, expected.size()) != expected)
					return false;
				offset_ += expected.size();
				return true;
			}

			[[nodiscard]] std::size_t remaining() const noexcept
			{
				return bytes_.size() - offset_;
			}

		  private:
			std::string_view bytes_;
			std::size_t offset_{};
		};

		[[nodiscard]] bool valid_count(const reader& input,
									   const std::optional<std::uint64_t> count)
		{
			const auto value = count.value_or(maximum_collection_size + 1U);
			return value <= maximum_collection_size && value <= input.remaining();
		}

		template <typename Value, typename Write>
		void write_optional(writer& output, const std::optional<Value>& value, Write write)
		{
			output.boolean(value.has_value());
			if (value)
				write(output, *value);
		}

		template <typename Value, typename Read>
		[[nodiscard]] bool read_optional(reader& input, std::optional<Value>& value, Read read)
		{
			const auto present = input.boolean();
			if (!present)
				return false;
			if (!*present)
			{
				value.reset();
				return true;
			}
			Value decoded;
			if (!read(input, decoded))
				return false;
			value = std::move(decoded);
			return true;
		}

		void write_string_vector(writer& output, const std::vector<std::string>& values)
		{
			output.integer<std::uint64_t>(values.size());
			for (const auto& value : values)
				output.string(value);
		}

		[[nodiscard]] bool read_string_vector(reader& input, std::vector<std::string>& values)
		{
			const auto count = input.integer<std::uint64_t>();
			if (!valid_count(input, count))
				return false;
			values.clear();
			values.reserve(static_cast<std::size_t>(*count));
			for (std::uint64_t index{}; index < *count; ++index)
			{
				auto value = input.string();
				if (!value)
					return false;
				values.push_back(std::move(*value));
			}
			return true;
		}

		void write_string_map(writer& output, const std::map<std::string, std::string>& values)
		{
			output.integer<std::uint64_t>(values.size());
			for (const auto& [key, value] : values)
			{
				output.string(key);
				output.string(value);
			}
		}

		[[nodiscard]] bool read_string_map(reader& input,
										   std::map<std::string, std::string>& values)
		{
			const auto count = input.integer<std::uint64_t>();
			if (!valid_count(input, count))
				return false;
			values.clear();
			for (std::uint64_t index{}; index < *count; ++index)
			{
				auto key = input.string();
				auto value = input.string();
				if (!key || !value || !values.emplace(std::move(*key), std::move(*value)).second)
					return false;
			}
			return true;
		}

		void write_point(writer& output, const source_point& value)
		{
			output.string(value.file.value());
			output.integer(value.byte_offset);
			output.integer(value.line);
			output.integer(value.column);
			output.integer(static_cast<std::uint8_t>(value.state));
		}

		[[nodiscard]] bool read_point(reader& input, source_point& value)
		{
			auto file = input.string();
			const auto offset = input.integer<std::uint64_t>();
			const auto line = input.integer<std::uint32_t>();
			const auto column = input.integer<std::uint32_t>();
			const auto state = input.integer<std::uint8_t>();
			if (!file || !offset || !line || !column || !state ||
				*state > static_cast<std::uint8_t>(source_location_state::unknown))
				return false;
			value = {file_id{std::move(*file)},
					 *offset,
					 *line,
					 *column,
					 static_cast<source_location_state>(*state)};
			return true;
		}

		void write_range(writer& output, const file_range& value)
		{
			write_point(output, value.begin);
			write_point(output, value.end);
			output.integer(static_cast<std::uint8_t>(value.kind));
		}

		[[nodiscard]] bool read_range(reader& input, file_range& value)
		{
			const auto before = input.remaining();
			if (!read_point(input, value.begin) || !read_point(input, value.end))
				return false;
			const auto kind = input.integer<std::uint8_t>();
			if (!kind || *kind > static_cast<std::uint8_t>(source_range_kind::token) ||
				input.remaining() >= before)
				return false;
			value.kind = static_cast<source_range_kind>(*kind);
			return true;
		}

		void write_span(writer& output, const source_span& value)
		{
			write_range(output, value.primary);
			write_optional(output, value.spelling, write_range);
			write_optional(output, value.expansion, write_range);
			output.integer<std::uint64_t>(value.macro_stack.size());
			for (const auto& frame : value.macro_stack)
			{
				output.string(frame.macro_name);
				write_range(output, frame.invocation);
				write_optional(output, frame.definition, write_range);
				write_optional(output,
							   frame.argument_index,
							   [](writer& nested, const std::uint32_t index)
							   {
								   nested.integer(index);
							   });
			}
			output.integer(static_cast<std::uint8_t>(value.origin));
			output.string(value.digest.algorithm);
			output.integer(value.digest.version);
			output.string(value.digest.value);
			output.boolean(value.read_only);
		}

		[[nodiscard]] bool read_span(reader& input, source_span& value)
		{
			if (!read_range(input, value.primary) ||
				!read_optional(input, value.spelling, read_range) ||
				!read_optional(input, value.expansion, read_range))
				return false;
			const auto count = input.integer<std::uint64_t>();
			if (!valid_count(input, count))
				return false;
			value.macro_stack.clear();
			value.macro_stack.reserve(static_cast<std::size_t>(*count));
			for (std::uint64_t index{}; index < *count; ++index)
			{
				macro_frame frame;
				auto name = input.string();
				if (!name || !read_range(input, frame.invocation) ||
					!read_optional(input, frame.definition, read_range) ||
					!read_optional(input,
								   frame.argument_index,
								   [](reader& nested, std::uint32_t& decoded)
								   {
									   const auto value = nested.integer<std::uint32_t>();
									   if (!value)
										   return false;
									   decoded = *value;
									   return true;
								   }))
					return false;
				frame.macro_name = std::move(*name);
				value.macro_stack.push_back(std::move(frame));
			}
			const auto origin = input.integer<std::uint8_t>();
			auto algorithm = input.string();
			const auto version = input.integer<std::uint32_t>();
			auto digest = input.string();
			const auto read_only = input.boolean();
			if (!origin || *origin > static_cast<std::uint8_t>(source_origin::unknown) ||
				!algorithm || !version || !digest || !read_only)
				return false;
			value.origin = static_cast<source_origin>(*origin);
			value.digest = {std::move(*algorithm), *version, std::move(*digest)};
			value.read_only = *read_only;
			return true;
		}

		void write_diagnostic(writer& output, const diagnostic& value)
		{
			output.string(value.id);
			output.string(value.message);
			output.integer(static_cast<std::uint8_t>(value.level));
			write_optional(output, value.primary, write_span);
			output.integer<std::uint64_t>(value.related.size());
			for (const auto& span : value.related)
				write_span(output, span);
			write_optional(output,
						   value.compiler_option,
						   [](writer& nested, const std::string& option)
						   {
							   nested.string(option);
						   });
		}

		[[nodiscard]] bool read_diagnostic(reader& input, diagnostic& value)
		{
			auto id = input.string();
			auto message = input.string();
			const auto level = input.integer<std::uint8_t>();
			if (!id || !message || !level || *level > static_cast<std::uint8_t>(severity::fatal) ||
				!read_optional(input, value.primary, read_span))
				return false;
			const auto count = input.integer<std::uint64_t>();
			if (!valid_count(input, count))
				return false;
			value.related.clear();
			value.related.reserve(static_cast<std::size_t>(*count));
			for (std::uint64_t index{}; index < *count; ++index)
			{
				source_span span;
				if (!read_span(input, span))
					return false;
				value.related.push_back(std::move(span));
			}
			if (!read_optional(input,
							   value.compiler_option,
							   [](reader& nested, std::string& option)
							   {
								   auto decoded = nested.string();
								   if (!decoded)
									   return false;
								   option = std::move(*decoded);
								   return true;
							   }))
				return false;
			value.id = std::move(*id);
			value.message = std::move(*message);
			value.level = static_cast<severity>(*level);
			return true;
		}

		void write_name(writer& output, const facts::name_identity& value)
		{
			output.string(value.display_qualified_name);
			for (const auto* optional : {&value.usr,
										 &value.semantic_owner,
										 &value.declaration_kind,
										 &value.signature_structure})
				write_optional(output,
							   *optional,
							   [](writer& nested, const std::string& text)
							   {
								   nested.string(text);
							   });
		}

		[[nodiscard]] bool read_optional_string(reader& input, std::optional<std::string>& value)
		{
			return read_optional(input,
								 value,
								 [](reader& nested, std::string& text)
								 {
									 auto decoded = nested.string();
									 if (!decoded)
										 return false;
									 text = std::move(*decoded);
									 return true;
								 });
		}

		[[nodiscard]] bool read_name(reader& input, facts::name_identity& value)
		{
			auto display = input.string();
			if (!display || !read_optional_string(input, value.usr) ||
				!read_optional_string(input, value.semantic_owner) ||
				!read_optional_string(input, value.declaration_kind) ||
				!read_optional_string(input, value.signature_structure))
				return false;
			value.display_qualified_name = std::move(*display);
			return true;
		}

		void write_type(writer& output, const facts::type_identity& value)
		{
			output.string(value.display_spelling);
			output.string(value.canonical_structure);
			write_optional(output,
						   value.declaration,
						   [](writer& nested, const symbol_id& id)
						   {
							   nested.string(id.value());
						   });
			output.integer<std::uint64_t>(value.components.size());
			for (const auto& component : value.components)
				output.string(component.value());
			output.boolean(value.builtin);
		}

		[[nodiscard]] bool read_type(reader& input, facts::type_identity& value)
		{
			auto display = input.string();
			auto canonical = input.string();
			if (!display || !canonical ||
				!read_optional(input,
							   value.declaration,
							   [](reader& nested, symbol_id& id)
							   {
								   auto encoded = nested.string();
								   if (!encoded)
									   return false;
								   id = symbol_id{std::move(*encoded)};
								   return id.valid();
							   }))
				return false;
			const auto count = input.integer<std::uint64_t>();
			if (!valid_count(input, count))
				return false;
			value.components.clear();
			value.components.reserve(static_cast<std::size_t>(*count));
			for (std::uint64_t index{}; index < *count; ++index)
			{
				auto encoded = input.string();
				if (!encoded)
					return false;
				type_id id{std::move(*encoded)};
				if (!id.valid())
					return false;
				value.components.push_back(std::move(id));
			}
			const auto builtin = input.boolean();
			if (!builtin)
				return false;
			value.display_spelling = std::move(*display);
			value.canonical_structure = std::move(*canonical);
			value.builtin = *builtin;
			return true;
		}

		void write_observation(writer& output, const facts::observation_record& value)
		{
			output.string(value.schema);
			output.string(value.adapter_id);
			output.string(value.adapter_version);
			output.integer(value.llvm_major);
			output.string(value.compile_unit.value());
			output.string(value.variant.value());
			output.integer(static_cast<std::uint16_t>(value.kind));
			write_optional(output, value.source, write_span);
			output.integer(value.payload_version);
			write_string_map(output, value.payload);
			output.integer<std::uint64_t>(value.diagnostics.size());
			for (const auto& diagnostic : value.diagnostics)
				write_diagnostic(output, diagnostic);
			output.integer<std::uint64_t>(value.coverage_contributions.size());
			for (const auto& contribution : value.coverage_contributions)
			{
				output.string(contribution.kind);
				output.string(contribution.id);
				output.integer(static_cast<std::uint8_t>(contribution.state));
				write_optional(output,
							   contribution.reason,
							   [](writer& nested, const std::string& reason)
							   {
								   nested.string(reason);
							   });
			}
			write_optional(output, value.name, write_name);
			write_optional(output, value.type, write_type);
		}

		[[nodiscard]] bool read_observation(reader& input, facts::observation_record& value)
		{
			auto schema = input.string();
			auto adapter = input.string();
			auto adapter_version = input.string();
			const auto llvm_major = input.integer<std::uint16_t>();
			auto unit = input.string();
			auto variant = input.string();
			const auto kind = input.integer<std::uint16_t>();
			if (!schema || !adapter || !adapter_version || !llvm_major || !unit || !variant ||
				!kind || *kind > static_cast<std::uint16_t>(fact_kind::custom) ||
				!read_optional(input, value.source, read_span))
				return false;
			const auto payload_version = input.integer<std::uint32_t>();
			if (!payload_version || !read_string_map(input, value.payload))
				return false;
			const auto diagnostic_count = input.integer<std::uint64_t>();
			if (!valid_count(input, diagnostic_count))
				return false;
			value.diagnostics.clear();
			value.diagnostics.reserve(static_cast<std::size_t>(*diagnostic_count));
			for (std::uint64_t index{}; index < *diagnostic_count; ++index)
			{
				diagnostic decoded;
				if (!read_diagnostic(input, decoded))
					return false;
				value.diagnostics.push_back(std::move(decoded));
			}
			const auto coverage_count = input.integer<std::uint64_t>();
			if (!valid_count(input, coverage_count))
				return false;
			value.coverage_contributions.clear();
			value.coverage_contributions.reserve(static_cast<std::size_t>(*coverage_count));
			for (std::uint64_t index{}; index < *coverage_count; ++index)
			{
				auto coverage_kind = input.string();
				auto id = input.string();
				const auto state = input.integer<std::uint8_t>();
				std::optional<std::string> reason;
				if (!coverage_kind || !id || !state ||
					*state > static_cast<std::uint8_t>(coverage_state::not_applicable) ||
					!read_optional_string(input, reason))
					return false;
				value.coverage_contributions.push_back({std::move(*coverage_kind),
														std::move(*id),
														static_cast<coverage_state>(*state),
														std::move(reason)});
			}
			if (!read_optional(input, value.name, read_name) ||
				!read_optional(input, value.type, read_type))
				return false;
			value.schema = std::move(*schema);
			value.adapter_id = std::move(*adapter);
			value.adapter_version = std::move(*adapter_version);
			value.llvm_major = *llvm_major;
			value.compile_unit = compile_unit_id{std::move(*unit)};
			value.variant = build_variant_id{std::move(*variant)};
			value.kind = static_cast<fact_kind>(*kind);
			value.payload_version = *payload_version;
			return true;
		}

		void write_normalized_diagnostic(writer& output, const normalized_diagnostic& value)
		{
			output.string(value.id);
			output.integer(static_cast<std::uint8_t>(value.severity));
			output.string(value.file);
			output.integer(value.line);
			output.integer(value.column);
			output.string(value.message);
		}

		[[nodiscard]] bool read_normalized_diagnostic(reader& input, normalized_diagnostic& value)
		{
			auto id = input.string();
			const auto severity_value = input.integer<std::uint8_t>();
			auto file = input.string();
			const auto line = input.integer<std::uint32_t>();
			const auto column = input.integer<std::uint32_t>();
			auto message = input.string();
			if (!id || !severity_value ||
				*severity_value > static_cast<std::uint8_t>(diagnostic_severity::fatal) || !file ||
				!line || !column || !message)
				return false;
			value = {std::move(*id),
					 static_cast<diagnostic_severity>(*severity_value),
					 std::move(*file),
					 *line,
					 *column,
					 std::move(*message)};
			return true;
		}

		void write_batch(writer& output, const observation_batch& value)
		{
			output.string(value.schema);
			output.string(value.adapter_id);
			output.string(value.adapter_version);
			output.string(value.unit.value());
			output.string(value.variant.value());
			output.integer(value.debug_context_identity);
			output.integer<std::uint64_t>(value.observations.size());
			for (const auto& observation : value.observations)
				write_observation(output, observation);
			output.integer<std::uint64_t>(value.diagnostics.size());
			for (const auto& diagnostic : value.diagnostics)
				write_normalized_diagnostic(output, diagnostic);
			output.integer(value.coverage.requested);
			output.integer(value.coverage.parsed);
			output.integer(value.coverage.failed);
			output.integer(value.coverage.cancelled);
		}

		[[nodiscard]] bool read_batch(reader& input, observation_batch& value)
		{
			auto schema = input.string();
			auto adapter = input.string();
			auto adapter_version = input.string();
			auto unit = input.string();
			auto variant = input.string();
			const auto identity = input.integer<std::uint64_t>();
			const auto observation_count = input.integer<std::uint64_t>();
			if (!schema || !adapter || !adapter_version || !unit || !variant || !identity ||
				!valid_count(input, observation_count))
				return false;
			value.observations.clear();
			value.observations.reserve(static_cast<std::size_t>(*observation_count));
			for (std::uint64_t index{}; index < *observation_count; ++index)
			{
				facts::observation_record observation;
				if (!read_observation(input, observation))
					return false;
				value.observations.push_back(std::move(observation));
			}
			const auto diagnostic_count = input.integer<std::uint64_t>();
			if (!valid_count(input, diagnostic_count))
				return false;
			value.diagnostics.clear();
			value.diagnostics.reserve(static_cast<std::size_t>(*diagnostic_count));
			for (std::uint64_t index{}; index < *diagnostic_count; ++index)
			{
				normalized_diagnostic diagnostic;
				if (!read_normalized_diagnostic(input, diagnostic))
					return false;
				value.diagnostics.push_back(std::move(diagnostic));
			}
			const auto requested = input.integer<std::uint32_t>();
			const auto parsed = input.integer<std::uint32_t>();
			const auto failed = input.integer<std::uint32_t>();
			const auto cancelled = input.integer<std::uint32_t>();
			if (!requested || !parsed || !failed || !cancelled)
				return false;
			value.schema = std::move(*schema);
			value.adapter_id = std::move(*adapter);
			value.adapter_version = std::move(*adapter_version);
			value.unit = compile_unit_id{std::move(*unit)};
			value.variant = build_variant_id{std::move(*variant)};
			value.debug_context_identity = *identity;
			value.coverage = {*requested, *parsed, *failed, *cancelled};
			return true;
		}

		void write_error(writer& output, const error& value, const std::size_t depth)
		{
			output.string(value.code.value);
			output.string(value.message);
			output.integer(static_cast<std::uint8_t>(value.scope));
			output.integer<std::uint64_t>(value.locations.size());
			for (const auto& location : value.locations)
				write_span(output, location);
			const auto causes = value.causes.size();
			output.integer<std::uint64_t>(causes);
			for (std::size_t index{}; index < causes; ++index)
				write_error(output, value.causes[index], depth + 1U);
			write_string_vector(output, value.suggested_actions);
			write_string_map(output, value.attributes);
			output.boolean(value.retryable);
		}

		[[nodiscard]] bool error_depth_within_limit(const error& value, const std::size_t depth)
		{
			if (depth > maximum_error_depth)
				return false;
			return std::ranges::all_of(value.causes,
									   [depth](const error& cause)
									   {
										   return error_depth_within_limit(cause, depth + 1U);
									   });
		}

		[[nodiscard]] bool read_error(reader& input, error& value, const std::size_t depth)
		{
			if (depth > maximum_error_depth)
				return false;
			auto code = input.string();
			auto message = input.string();
			const auto scope = input.integer<std::uint8_t>();
			const auto location_count = input.integer<std::uint64_t>();
			if (!code || !message || !scope ||
				*scope > static_cast<std::uint8_t>(failure_scope::workspace) ||
				!valid_count(input, location_count))
				return false;
			value.locations.clear();
			value.locations.reserve(static_cast<std::size_t>(*location_count));
			for (std::uint64_t index{}; index < *location_count; ++index)
			{
				source_span location;
				if (!read_span(input, location))
					return false;
				value.locations.push_back(std::move(location));
			}
			const auto cause_count = input.integer<std::uint64_t>();
			if (!valid_count(input, cause_count))
				return false;
			value.causes.clear();
			value.causes.reserve(static_cast<std::size_t>(*cause_count));
			for (std::uint64_t index{}; index < *cause_count; ++index)
			{
				error cause;
				if (!read_error(input, cause, depth + 1U))
					return false;
				value.causes.push_back(std::move(cause));
			}
			const auto retryable = [&]() -> std::optional<bool>
			{
				if (!read_string_vector(input, value.suggested_actions) ||
					!read_string_map(input, value.attributes))
					return std::nullopt;
				return input.boolean();
			}();
			if (!retryable)
				return false;
			value.code.value = std::move(*code);
			value.message = std::move(*message);
			value.scope = static_cast<failure_scope>(*scope);
			value.retryable = *retryable;
			return true;
		}

		void write_compile_unit(writer& output, const compile_unit& unit)
		{
			output.string(unit.id().value());
			output.string(unit.variant_id().value());
			output.string(unit.main_file().value());
			const auto& command = unit.command();
			output.string(command.directory.generic_string());
			output.string(command.file.generic_string());
			write_string_vector(output, command.arguments);
			write_optional(output,
						   command.output,
						   [](writer& nested, const path& value)
						   {
							   nested.string(value.generic_string());
						   });
			const auto& target = unit.target();
			output.string(target.triple);
			output.string(target.abi);
			output.string(target.language_standard);
			write_optional(output,
						   target.resource_directory,
						   [](writer& nested, const std::string& value)
						   {
							   nested.string(value);
						   });
			output.string(unit.command_digest());
		}

		[[nodiscard]] bool read_compile_unit(reader& input, compile_unit& unit)
		{
			auto unit_id = input.string();
			auto variant_id = input.string();
			auto main_file = input.string();
			auto directory = input.string();
			auto file = input.string();
			compile_command command;
			if (!unit_id || !variant_id || !main_file || !directory || !file ||
				!read_string_vector(input, command.arguments) ||
				!read_optional(input,
							   command.output,
							   [](reader& nested, path& value)
							   {
								   auto decoded = nested.string();
								   if (!decoded)
									   return false;
								   value = path{std::move(*decoded)};
								   return true;
							   }))
				return false;
			auto triple = input.string();
			auto abi = input.string();
			auto language = input.string();
			target_context target;
			if (!triple || !abi || !language ||
				!read_optional_string(input, target.resource_directory))
				return false;
			auto digest = input.string();
			compile_unit_id decoded_unit{std::move(*unit_id)};
			build_variant_id decoded_variant{std::move(*variant_id)};
			file_id decoded_file{std::move(*main_file)};
			if (!digest || !decoded_unit.valid() || !decoded_variant.valid() ||
				!decoded_file.valid() || command.arguments.empty())
				return false;
			command.directory = path{std::move(*directory)};
			command.file = path{std::move(*file)};
			target.triple = std::move(*triple);
			target.abi = std::move(*abi);
			target.language_standard = std::move(*language);
			unit = workspace_catalog_access::reconstitute_compile_unit(std::move(decoded_unit),
																	   std::move(decoded_variant),
																	   std::move(decoded_file),
																	   std::move(command),
																	   std::move(target),
																	   std::move(*digest));
			return true;
		}

		[[nodiscard]] bool valid_envelope_size(const std::string_view bytes) noexcept
		{
			return bytes.size() <= maximum_message_bytes;
		}
	} // namespace

	result<std::string> encode_worker_request(const worker_request& request)
	{
		if (!request.task.unit.id().valid() || !request.task.unit.variant_id().valid() ||
			request.task.unit.command().arguments.empty() || request.profile.empty() ||
			request.snapshot_key.empty() || !request.input_fingerprint.starts_with("input_") ||
			request.input_fingerprint.size() != 70U || request.toolchain_version.empty())
			return ipc_error("request-invalid");
		writer output;
		output.raw(request_magic);
		output.integer(worker_ipc_version);
		output.string(worker_ipc_schema);
		write_compile_unit(output, request.task.unit);
		output.integer<std::uint64_t>(request.task.files.size());
		for (const auto& file : request.task.files)
		{
			output.string(file.file.generic_string());
			output.string(file.content);
		}
		output.integer(static_cast<std::uint8_t>(request.task.injected_fault));
		output.string(request.profile);
		output.string(request.snapshot_key);
		output.string(request.input_fingerprint);
		output.string(request.toolchain_version);
		auto encoded = std::move(output).finish();
		if (!valid_envelope_size(encoded))
			return ipc_error("request-size-limit");
		return encoded;
	}

	result<worker_request> decode_worker_request(const std::string_view bytes)
	{
		if (!valid_envelope_size(bytes))
			return ipc_error("request-size-limit");
		reader input{bytes};
		const auto version = [&]() -> std::optional<std::uint32_t>
		{
			if (!input.consume(request_magic))
				return std::nullopt;
			return input.integer<std::uint32_t>();
		}();
		auto schema = input.string();
		worker_request request;
		if (!version || *version != worker_ipc_version || !schema || *schema != worker_ipc_schema ||
			!read_compile_unit(input, request.task.unit))
			return ipc_error("request-header-invalid");
		const auto file_count = input.integer<std::uint64_t>();
		if (!valid_count(input, file_count))
			return ipc_error("request-files-invalid");
		request.task.files.reserve(static_cast<std::size_t>(*file_count));
		for (std::uint64_t index{}; index < *file_count; ++index)
		{
			auto file = input.string();
			auto content = input.string();
			if (!file || !content)
				return ipc_error("request-file-truncated");
			request.task.files.push_back({path{std::move(*file)}, std::move(*content)});
		}
		const auto fault = input.integer<std::uint8_t>();
		auto profile = input.string();
		auto snapshot = input.string();
		auto fingerprint = input.string();
		auto toolchain = input.string();
		if (!fault || *fault > static_cast<std::uint8_t>(frontend_fault::crash) || !profile ||
			profile->empty() || !snapshot || snapshot->empty() || !fingerprint ||
			!fingerprint->starts_with("input_") || fingerprint->size() != 70U || !toolchain ||
			toolchain->empty() || input.remaining() != 0U)
			return ipc_error("request-payload-invalid");
		request.task.injected_fault = static_cast<frontend_fault>(*fault);
		request.profile = std::move(*profile);
		request.snapshot_key = std::move(*snapshot);
		request.input_fingerprint = std::move(*fingerprint);
		request.toolchain_version = std::move(*toolchain);
		return request;
	}

	result<std::string> encode_worker_response(const result<observation_batch>& response)
	{
		if (response && !response.value().validate())
			return ipc_error("response-batch-invalid");
		if (!response && !error_depth_within_limit(response.error(), 0U))
			return ipc_error("response-error-depth-limit");
		writer output;
		output.raw(response_magic);
		output.integer(worker_ipc_version);
		output.string(worker_ipc_schema);
		output.boolean(response.has_value());
		if (response)
			write_batch(output, response.value());
		else
			write_error(output, response.error(), 0U);
		auto encoded = std::move(output).finish();
		if (!valid_envelope_size(encoded))
			return ipc_error("response-size-limit");
		return encoded;
	}

	result<observation_batch> decode_worker_response(const std::string_view bytes)
	{
		if (!valid_envelope_size(bytes))
			return ipc_error("response-size-limit");
		reader input{bytes};
		const auto version = [&]() -> std::optional<std::uint32_t>
		{
			if (!input.consume(response_magic))
				return std::nullopt;
			return input.integer<std::uint32_t>();
		}();
		auto schema = input.string();
		const auto success = input.boolean();
		if (!version || *version != worker_ipc_version || !schema || *schema != worker_ipc_schema ||
			!success)
			return ipc_error("response-header-invalid");
		if (!*success)
		{
			error failure;
			if (!read_error(input, failure, 0U) || input.remaining() != 0U)
				return ipc_error("response-error-invalid");
			return failure;
		}
		observation_batch batch;
		if (!read_batch(input, batch) || input.remaining() != 0U || !batch.validate())
			return ipc_error("response-batch-invalid");
		return batch;
	}
} // namespace cxxlens::detail::frontend
