#include "compile_commands_capture_internal.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>
#include <utility>

#include "bounded_json_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {
				"application-analysis.capture-input-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		class metadata_budget
		{
		  public:
			explicit metadata_budget(const import_limits& limits) : limits_{limits} {}

			[[nodiscard]] result<void>
			add(const std::string_view value, std::string field, const bool allow_empty = false)
			{
				if (value.empty() && !allow_empty)
					return unexpected(invalid(std::move(field), "empty"));
				if (value.contains('\0'))
					return unexpected(invalid(std::move(field), "nul-byte"));
				if (value.size() > limits_.maximum_string_bytes)
					return unexpected(limit(std::move(field), "string-bytes"));
				if (value.size() > limits_.maximum_total_metadata_bytes - total_)
					return unexpected(limit("compile_commands", "metadata-bytes"));
				total_ += value.size();
				return {};
			}

		  private:
			const import_limits& limits_;
			std::size_t total_{};
		};

		[[nodiscard]] bool posix_absolute_path(const std::string_view value) noexcept
		{
			return value.starts_with('/') && !value.contains('\0');
		}

		[[nodiscard]] bool shell_space(const char value) noexcept
		{
			return value == ' ' || value == '\t' || value == '\n' || value == '\r';
		}

		[[nodiscard]] bool shell_operator(const char value) noexcept
		{
			return std::string_view{"|&;<>()"}.contains(value);
		}

		[[nodiscard]] result<std::vector<std::string>>
		decode_shell_words(const std::string_view command,
						   const import_limits& limits,
						   metadata_budget& budget,
						   const std::string_view field)
		{
			enum class quote_state : unsigned char
			{
				unquoted,
				single,
				double_quoted,
			};

			std::vector<std::string> output;
			std::string word;
			quote_state quote{quote_state::unquoted};
			bool word_started{};
			const auto finish_word = [&]() -> result<void>
			{
				if (!word_started)
					return {};
				if (output.size() >= limits.maximum_arguments_per_unit)
					return unexpected(limit(std::string{field}, "argument-count"));
				if (word.size() > limits.maximum_string_bytes)
					return unexpected(limit(std::string{field}, "argument-bytes"));
				if (auto bounded = budget.add(word, std::string{field}, true); !bounded)
					return bounded;
				output.push_back(std::move(word));
				word.clear();
				word_started = false;
				return {};
			};

			for (std::size_t index{}; index < command.size(); ++index)
			{
				const char value = command[index];
				if (quote == quote_state::single)
				{
					if (value == '\'')
						quote = quote_state::unquoted;
					else
						word.push_back(value);
					continue;
				}
				if (quote == quote_state::double_quoted)
				{
					if (value == '"')
					{
						quote = quote_state::unquoted;
						continue;
					}
					if (value == '$' || value == '`')
						return unexpected(invalid(std::string{field}, "shell-expansion"));
					if (value != '\\')
					{
						word.push_back(value);
						continue;
					}
					if (++index >= command.size())
						return unexpected(invalid(std::string{field}, "trailing-escape"));
					const char escaped = command[index];
					if (escaped == '\n' || escaped == '\r')
						return unexpected(invalid(std::string{field}, "line-continuation"));
					if (escaped != '"' && escaped != '\\' && escaped != '$' && escaped != '`')
						word.push_back('\\');
					word.push_back(escaped);
					continue;
				}

				if (shell_space(value))
				{
					if (auto finished = finish_word(); !finished)
						return unexpected(std::move(finished.error()));
					continue;
				}
				if (value == '\'')
				{
					quote = quote_state::single;
					word_started = true;
					continue;
				}
				if (value == '"')
				{
					quote = quote_state::double_quoted;
					word_started = true;
					continue;
				}
				if (value == '\\')
				{
					word_started = true;
					if (++index >= command.size())
						return unexpected(invalid(std::string{field}, "trailing-escape"));
					if (command[index] == '\n' || command[index] == '\r')
						return unexpected(invalid(std::string{field}, "line-continuation"));
					word.push_back(command[index]);
					continue;
				}
				if (value == '$' || value == '`')
					return unexpected(invalid(std::string{field}, "shell-expansion"));
				if (shell_operator(value) || value == '*' || value == '?' || value == '[' ||
					(value == '#' && !word_started))
					return unexpected(invalid(std::string{field}, "shell-semantics"));
				word_started = true;
				word.push_back(value);
			}
			if (quote != quote_state::unquoted)
				return unexpected(invalid(std::string{field}, "unterminated-quote"));
			if (auto finished = finish_word(); !finished)
				return unexpected(std::move(finished.error()));
			if (output.empty() || output.front().empty())
				return unexpected(invalid(std::string{field}, "compiler-argument-missing"));
			return output;
		}

		[[nodiscard]] result<std::string>
		required_string(const json_value::object_type& object,
						const std::string_view name, // NOLINT(bugprone-easily-swappable-parameters)
						const std::string_view field,
						metadata_budget& budget)
		{
			const auto found = object.find(name);
			if (found == object.end())
				return unexpected(invalid(std::string{field}, "missing"));
			const auto* value = found->second.as_string();
			if (value == nullptr)
				return unexpected(invalid(std::string{field}, "string-required"));
			if (auto bounded = budget.add(*value, std::string{field}); !bounded)
				return unexpected(std::move(bounded.error()));
			return *value;
		}

		[[nodiscard]] result<std::vector<std::string>> arguments(const json_value& value,
																 const import_limits& limits,
																 metadata_budget& budget,
																 const std::string_view field)
		{
			const auto* array = value.as_array();
			if (array == nullptr)
				return unexpected(invalid(std::string{field}, "array-required"));
			if (array->empty())
				return unexpected(invalid(std::string{field}, "compiler-argument-missing"));
			if (array->size() > limits.maximum_arguments_per_unit)
				return unexpected(limit(std::string{field}, "argument-count"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (std::size_t index{}; index < array->size(); ++index)
			{
				const auto* token = (*array)[index].as_string();
				const auto item_field = std::string{field} + "[" + std::to_string(index) + "]";
				if (token == nullptr)
					return unexpected(invalid(item_field, "string-required"));
				if (auto bounded = budget.add(*token, item_field, true); !bounded)
					return unexpected(std::move(bounded.error()));
				output.push_back(*token);
			}
			if (output.front().empty())
				return unexpected(invalid(std::string{field}, "compiler-argument-missing"));
			return output;
		}
	} // namespace

	compile_commands_capture::compile_commands_capture(std::vector<compile_command_entry> entries)
		: entries_{std::move(entries)}
	{
	}

	const std::vector<compile_command_entry>& compile_commands_capture::entries() const noexcept
	{
		return entries_;
	}

	result<compile_commands_capture> make_explicit_compile_capture(compile_command_entry entry,
																   const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		try
		{
			metadata_budget budget{limits};
			if (!posix_absolute_path(entry.directory))
				return unexpected(invalid("invocation.directory", "absolute-path-required"));
			if (auto bounded = budget.add(entry.directory, "invocation.directory"); !bounded)
				return unexpected(std::move(bounded.error()));
			if (auto bounded = budget.add(entry.file, "invocation.file"); !bounded)
				return unexpected(std::move(bounded.error()));
			if (entry.output)
				if (auto bounded = budget.add(*entry.output, "invocation.output"); !bounded)
					return unexpected(std::move(bounded.error()));
			if (entry.decoded_from_command)
				return unexpected(invalid("invocation", "tokenized-arguments-required"));
			if (entry.arguments.empty() || entry.arguments.front().empty())
				return unexpected(invalid("invocation.arguments", "compiler-argument-missing"));
			if (entry.arguments.size() > limits.maximum_arguments_per_unit)
				return unexpected(limit("invocation.arguments", "argument-count"));
			for (std::size_t index{}; index < entry.arguments.size(); ++index)
			{
				if (auto bounded = budget.add(entry.arguments[index],
											  "invocation.arguments[" + std::to_string(index) + "]",
											  true);
					!bounded)
					return unexpected(std::move(bounded.error()));
			}
			std::vector<compile_command_entry> entries;
			entries.push_back(std::move(entry));
			return compile_commands_capture{std::move(entries)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("invocation", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("invocation", "allocation-length"));
		}
	}

	result<compile_commands_capture> decode_compile_commands(const std::string_view input,
															 const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (input.size() > limits.maximum_bundle_bytes)
			return unexpected(limit("compile_commands", "input-bytes"));

		const json_limits json_bounds{
			.max_input_bytes = limits.maximum_bundle_bytes,
			.max_depth = std::min(limits.maximum_nesting_depth, std::size_t{64U}),
			.max_array_elements =
				std::max(limits.maximum_compile_units, limits.maximum_arguments_per_unit),
			.max_object_members = 5U,
			.max_string_bytes = limits.maximum_string_bytes,
			.max_total_string_bytes = limits.maximum_total_metadata_bytes,
			.max_total_values = limits.maximum_bundle_bytes,
		};
		const json_parse_contract contract{
			.error_code = "application-analysis.capture-input-invalid",
			.error_field = "compile_commands",
			.include_byte_offset = true,
			.require_top_level_object = false,
			.reject_utf8_bom = true,
			.numbers = json_number_syntax::integer_tokens,
			.depth = json_depth_semantics::all_values,
		};

		try
		{
			auto parsed = parse_json_value(input, json_bounds, contract);
			if (!parsed)
				return unexpected(std::move(parsed.error()));
			const auto* root = parsed->as_array();
			if (root == nullptr)
				return unexpected(invalid("compile_commands", "array-required"));
			if (root->empty())
				return unexpected(invalid("compile_commands", "empty"));
			if (root->size() > limits.maximum_compile_units)
				return unexpected(limit("compile_commands", "compile-unit-count"));

			metadata_budget budget{limits};
			std::vector<compile_command_entry> entries;
			entries.reserve(root->size());
			for (std::size_t index{}; index < root->size(); ++index)
			{
				const auto prefix = "compile_commands[" + std::to_string(index) + "]";
				const auto* object = (*root)[index].as_object();
				if (object == nullptr)
					return unexpected(invalid(prefix, "object-required"));
				for (const auto& [key, value] : *object)
				{
					(void)value;
					if (key != "directory" && key != "file" && key != "output" &&
						key != "arguments" && key != "command")
					{
						auto member_field = prefix;
						member_field.push_back('.');
						member_field += key;
						return unexpected(invalid(std::move(member_field), "unknown-member"));
					}
				}

				compile_command_entry entry;
				auto directory =
					required_string(*object, "directory", prefix + ".directory", budget);
				if (!directory)
					return unexpected(std::move(directory.error()));
				if (!posix_absolute_path(*directory))
					return unexpected(invalid(prefix + ".directory", "absolute-path-required"));
				entry.directory = std::move(*directory);

				auto file = required_string(*object, "file", prefix + ".file", budget);
				if (!file)
					return unexpected(std::move(file.error()));
				entry.file = std::move(*file);

				if (const auto found = object->find("output"); found != object->end())
				{
					const auto* output = found->second.as_string();
					if (output == nullptr)
						return unexpected(invalid(prefix + ".output", "string-required"));
					if (auto bounded = budget.add(*output, prefix + ".output"); !bounded)
						return unexpected(std::move(bounded.error()));
					entry.output = *output;
				}

				const auto arguments_member = object->find("arguments");
				const auto command_member = object->find("command");
				if ((arguments_member == object->end()) == (command_member == object->end()))
					return unexpected(invalid(prefix, "exactly-one-command-form-required"));
				if (arguments_member != object->end())
				{
					auto decoded =
						arguments(arguments_member->second, limits, budget, prefix + ".arguments");
					if (!decoded)
						return unexpected(std::move(decoded.error()));
					entry.arguments = std::move(*decoded);
				}
				else
				{
					const auto* command = command_member->second.as_string();
					if (command == nullptr)
						return unexpected(invalid(prefix + ".command", "string-required"));
					auto decoded =
						decode_shell_words(*command, limits, budget, prefix + ".command");
					if (!decoded)
						return unexpected(std::move(decoded.error()));
					entry.arguments = std::move(*decoded);
					entry.decoded_from_command = true;
				}
				entries.push_back(std::move(entry));
			}
			return compile_commands_capture{std::move(entries)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("compile_commands", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("compile_commands", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
