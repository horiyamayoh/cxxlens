#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <filesystem>
#include <map>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include <cxxlens/workspace.hpp>

#include "../core/canonical_encoding.hpp"
#include "../core/canonical_json.hpp"
#include "../runtime/filesystem_port.hpp"
#include "../runtime/hash_port.hpp"
#include "catalog_access.hpp"
#include "provisioning.hpp"
#include "semantic_path.hpp"

namespace cxxlens
{
	namespace
	{
		using detail::json::json_value;
		using detail::runtime::request_context;

		struct database_value
		{
			using array = std::vector<database_value>;
			using object = std::map<std::string, database_value>;
			std::variant<std::string, array, object> value;
		};

		[[nodiscard]] error
		workspace_error(std::string code, std::string reason, const path& source = {})
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "Workspace catalog input is invalid";
			failure.scope = failure_scope::workspace;
			failure.attributes.emplace("reason", std::move(reason));
			if (!source.empty())
				failure.attributes.emplace("source", source.generic_string());
			return failure;
		}

		class database_parser
		{
		  public:
			explicit database_parser(std::string_view input) : input_{input} {}

			[[nodiscard]] result<database_value> parse()
			{
				constexpr std::size_t maximum_database_bytes = std::size_t{16U} * 1024U * 1024U;
				if (input_.size() > maximum_database_bytes)
					return workspace_error("workspace.compile-database-invalid", "size-limit");
				auto value = parse_value(0U);
				skip_space();
				if (!value || position_ != input_.size())
					return value ? result<database_value>{workspace_error(
									   "workspace.compile-database-invalid", "trailing-data")}
								 : value;
				return value;
			}

		  private:
			[[nodiscard]] result<database_value> parse_value(std::size_t depth)
			{
				if (depth > 64U)
					return workspace_error("workspace.compile-database-invalid", "depth-limit");
				skip_space();
				if (position_ >= input_.size())
					return workspace_error("workspace.compile-database-invalid", "value-missing");
				if (input_[position_] == '"')
				{
					auto value = parse_string();
					return value ? database_value{std::move(value.value())}
								 : result<database_value>{std::move(value.error())};
				}
				if (input_[position_] == '[')
					return parse_array(depth + 1U);
				if (input_[position_] == '{')
					return parse_object(depth + 1U);
				return workspace_error("workspace.compile-database-invalid",
									   "unsupported-json-value");
			}

			[[nodiscard]] result<database_value> parse_array(std::size_t depth)
			{
				++position_;
				database_value::array output;
				skip_space();
				if (consume(']'))
					return database_value{std::move(output)};
				while (true)
				{
					auto value = parse_value(depth);
					if (!value)
						return std::move(value.error());
					output.push_back(std::move(value.value()));
					skip_space();
					if (consume(']'))
						return database_value{std::move(output)};
					if (!consume(','))
						return workspace_error("workspace.compile-database-invalid",
											   "array-comma-missing");
				}
			}

			[[nodiscard]] result<database_value> parse_object(std::size_t depth)
			{
				++position_;
				database_value::object output;
				skip_space();
				if (consume('}'))
					return database_value{std::move(output)};
				while (true)
				{
					auto key = parse_string();
					if (!key)
						return std::move(key.error());
					skip_space();
					if (!consume(':'))
						return workspace_error("workspace.compile-database-invalid",
											   "object-colon-missing");
					auto value = parse_value(depth);
					if (!value)
						return std::move(value.error());
					if (!output.emplace(std::move(key.value()), std::move(value.value())).second)
						return workspace_error("workspace.compile-database-invalid",
											   "duplicate-json-key");
					skip_space();
					if (consume('}'))
						return database_value{std::move(output)};
					if (!consume(','))
						return workspace_error("workspace.compile-database-invalid",
											   "object-comma-missing");
				}
			}

			[[nodiscard]] result<std::string> parse_string()
			{
				if (!consume('"'))
					return workspace_error("workspace.compile-database-invalid", "string-expected");
				std::string output;
				while (position_ < input_.size())
				{
					const char value = input_[position_++];
					if (value == '"')
						return output;
					if (static_cast<unsigned char>(value) < 0x20U)
						return workspace_error("workspace.compile-database-invalid",
											   "control-character");
					if (value != '\\')
					{
						output.push_back(value);
						continue;
					}
					if (position_ >= input_.size())
						return workspace_error("workspace.compile-database-invalid",
											   "short-escape");
					const char escaped = input_[position_++];
					switch (escaped)
					{
						case '"':
						case '\\':
						case '/':
							output.push_back(escaped);
							break;
						case 'b':
							output.push_back('\b');
							break;
						case 'f':
							output.push_back('\f');
							break;
						case 'n':
							output.push_back('\n');
							break;
						case 'r':
							output.push_back('\r');
							break;
						case 't':
							output.push_back('\t');
							break;
						default:
							return workspace_error("workspace.compile-database-invalid",
												   "unsupported-escape");
					}
				}
				return workspace_error("workspace.compile-database-invalid", "unterminated-string");
			}

			void skip_space()
			{
				while (position_ < input_.size() &&
					   std::isspace(static_cast<unsigned char>(input_[position_])) != 0)
					++position_;
			}
			[[nodiscard]] bool consume(char expected)
			{
				if (position_ < input_.size() && input_[position_] == expected)
				{
					++position_;
					return true;
				}
				return false;
			}

			std::string_view input_;
			std::size_t position_{};
		};

		[[nodiscard]] result<std::vector<std::string>> parse_command(std::string_view input)
		{
			std::vector<std::string> output;
			std::string token;
			char quote{};
			bool escaped{};
			bool active{};
			for (char value : input)
			{
				if (escaped)
				{
					token.push_back(value);
					escaped = false;
					active = true;
				}
				else if (value == '\\' && quote != '\'')
					escaped = true;
				else if (quote != 0)
				{
					if (value == quote)
						quote = 0;
					else
						token.push_back(value);
					active = true;
				}
				else if (value == '\'' || value == '"')
				{
					quote = value;
					active = true;
				}
				else if (std::isspace(static_cast<unsigned char>(value)) != 0)
				{
					if (active)
					{
						output.push_back(std::move(token));
						token.clear();
						active = false;
					}
				}
				else
				{
					token.push_back(value);
					active = true;
				}
			}
			if (escaped || quote != 0)
				return workspace_error("workspace.compile-database-invalid",
									   "malformed-command-string");
			if (active)
				output.push_back(std::move(token));
			if (output.empty())
				return workspace_error("workspace.compile-database-invalid", "empty-command");
			return output;
		}

		[[nodiscard]] const database_value* field(const database_value::object& object,
												  const std::string& name)
		{
			const auto found = object.find(name);
			return found == object.end() ? nullptr : &found->second;
		}

		[[nodiscard]] result<std::string> required_string(const database_value::object& object,
														  const std::string& name)
		{
			const auto* value = field(object, name);
			if (value == nullptr || !std::holds_alternative<std::string>(value->value))
				return workspace_error("workspace.compile-database-invalid",
									   "missing-or-invalid-" + name);
			return std::get<std::string>(value->value);
		}

		struct resolved_entry
		{
			const database_value::object* object{};
			path directory;
			path source;
		};

		[[nodiscard]] path common_ancestor(path left, const path& right)
		{
			left = left.lexically_normal();
			const auto normalized_right = right.lexically_normal();
			path output;
			auto left_part = left.begin();
			auto right_part = normalized_right.begin();
			while (left_part != left.end() && right_part != normalized_right.end() &&
				   *left_part == *right_part)
			{
				output /= *left_part;
				++left_part;
				++right_part;
			}
			return output.lexically_normal();
		}

		[[nodiscard]] result<std::vector<resolved_entry>>
		resolve_entries(const database_value::array& entries,
						const path& database,
						const detail::runtime::filesystem_port& filesystem,
						const request_context& request)
		{
			std::vector<resolved_entry> output;
			output.reserve(entries.size());
			for (const auto& entry : entries)
			{
				if (!std::holds_alternative<database_value::object>(entry.value))
					return workspace_error("workspace.compile-database-invalid",
										   "entry-not-object");
				const auto& object = std::get<database_value::object>(entry.value);
				auto directory_text = required_string(object, "directory");
				auto file_text = required_string(object, "file");
				if (!directory_text || !file_text)
					return workspace_error("workspace.compile-database-invalid",
										   "entry-required-field");
				path directory = directory_text.value();
				if (directory.is_relative())
					directory = database.parent_path() / directory;
				auto canonical_directory = filesystem.canonicalize(directory, request);
				if (!canonical_directory)
					return workspace_error("workspace.compile-database-invalid",
										   "working-directory-invalid",
										   directory);
				directory = canonical_directory.value();
				path source = file_text.value();
				if (source.is_relative())
					source = directory / source;
				auto canonical_source = filesystem.canonicalize(source, request);
				if (!canonical_source)
					return workspace_error(
						"workspace.compile-database-invalid", "source-path-invalid", source);
				output.push_back({&object, directory, canonical_source.value()});
			}
			return output;
		}

		[[nodiscard]] result<path> infer_project_root(const path& database,
													  const std::vector<resolved_entry>& entries)
		{
			path root = database.parent_path().lexically_normal();
			for (const auto& entry : entries)
			{
				root = common_ancestor(std::move(root), entry.directory);
				root = common_ancestor(std::move(root), entry.source.parent_path());
			}
			if (root.empty() || root == root.root_path())
				return workspace_error(
					"workspace.compile-database-invalid", "project-root-ambiguous", database);
			return root;
		}

		void replace_path_prefix(std::string& value,
								 const path& anchor,
								 const std::string_view replacement)
		{
			const auto prefix = anchor.lexically_normal().generic_string();
			const auto position = value.find(prefix);
			if (!prefix.empty() && position != std::string::npos)
				value.replace(position, prefix.size(), replacement);
		}

		[[nodiscard]] std::string
		semantic_argument(const path& root, const path& build_root, std::string value)
		{
			replace_path_prefix(value, root, "$root");
			if (!detail::workspace_paths::semantic_path(root, build_root))
				replace_path_prefix(value, build_root, "$build");
			return value;
		}

		struct variant_argument_paths
		{
			const path& directory;
			const path& source;
		};

		[[nodiscard]] std::vector<std::string>
		variant_arguments(const std::vector<std::string>& arguments,
						  const variant_argument_paths paths)
		{
			std::vector<std::string> output;
			for (std::size_t index = 0U; index < arguments.size(); ++index)
			{
				const auto& argument = arguments[index];
				if (argument == "-c")
					continue;
				if (argument == "-o" || argument == "--output" || argument == "-MF" ||
					argument == "-MT" || argument == "-MQ" || argument == "-MJ")
				{
					if (index + 1U < arguments.size())
						++index;
					continue;
				}
				if ((argument.starts_with("-o") && argument.size() > 2U) ||
					argument.starts_with("--output=") || argument.starts_with("-MF=") ||
					argument.starts_with("-MT=") || argument.starts_with("-MQ=") ||
					argument.starts_with("-MJ="))
					continue;
				if (!argument.empty() && argument.front() != '-')
				{
					path candidate = argument;
					if (candidate.is_relative())
						candidate = paths.directory / candidate;
					if (candidate.lexically_normal() == paths.source.lexically_normal())
						continue;
				}
				output.push_back(argument);
			}
			return output;
		}

		[[nodiscard]] result<std::string>
		stable_id(const std::string& prefix, // NOLINT(bugprone-easily-swappable-parameters)
				  const std::string& domain,
				  const std::vector<std::pair<std::string, std::string>>& fields)
		{
			detail::identity::canonical_encoder encoder{domain, {1U, 1U}};
			for (const auto& [name, value] : fields)
				encoder.bytes_field(name, std::as_bytes(std::span{value.data(), value.size()}));
			auto payload = encoder.finish();
			if (!payload)
				return workspace_error("core.internal-invariant-violation", "identity-encoding");
			detail::runtime::fnv1a_hash_adapter hashes;
			detail::identity::identity_service identities{hashes};
			detail::identity::collision_registry collisions;
			auto id = identities.make_id(prefix, payload.value(), collisions);
			if (!id)
				return workspace_error("facts.identity-collision", "workspace-identity");
			return id.value();
		}

		[[nodiscard]] std::string digest_part(const std::string& stable_id)
		{
			const auto separator = stable_id.rfind('_');
			return separator == std::string::npos ? std::string{}
												  : stable_id.substr(separator + 1U);
		}

		[[nodiscard]] result<std::vector<std::string>>
		expand_response_files(const std::vector<std::string>& arguments,
							  const path& directory,
							  const detail::runtime::filesystem_port& files,
							  std::size_t depth = 0U)
		{
			if (depth > 8U)
				return workspace_error("workspace.compile-database-invalid",
									   "response-depth-limit");
			std::vector<std::string> output;
			for (const auto& argument : arguments)
			{
				if (argument.size() <= 1U || argument.front() != '@')
				{
					output.push_back(argument);
					continue;
				}
				path response = argument.substr(1U);
				if (response.is_relative())
					response = directory / response;
				request_context context;
				context.operation = "workspace.response-file.read";
				auto content = files.read(response, context);
				if (!content)
					return workspace_error(
						"workspace.compile-database-invalid", "response-file-unreadable", response);
				auto parsed = parse_command(content.value());
				if (!parsed)
					return std::move(parsed.error());
				auto expanded = expand_response_files(parsed.value(), directory, files, depth + 1U);
				if (!expanded)
					return std::move(expanded.error());
				output.insert(output.end(), expanded.value().begin(), expanded.value().end());
			}
			return output;
		}

		[[nodiscard]] bool unsafe_driver_flags(const std::vector<std::string>& arguments)
		{
			for (std::size_t index = 0U; index < arguments.size(); ++index)
			{
				const auto& value = arguments[index];
				if (value == "-fplugin" || value.starts_with("-fplugin=") || value == "-load" ||
					(value == "-Xclang" && index + 1U < arguments.size() &&
					 (arguments[index + 1U] == "-load" ||
					  arguments[index + 1U] == "-load-pass-plugin")))
					return true;
			}
			return false;
		}

		[[nodiscard]] target_context project_target(const std::vector<std::string>& arguments)
		{
			target_context output;
			for (std::size_t index = 0U; index < arguments.size(); ++index)
			{
				const auto& value = arguments[index];
				auto next = [&]() -> std::string
				{
					return index + 1U < arguments.size() ? arguments[index + 1U] : std::string{};
				};
				if (value.starts_with("--target="))
					output.triple = value.substr(9U);
				else if (value == "-target")
					output.triple = next();
				else if (value.starts_with("-std="))
					output.language_standard = value.substr(5U);
				else if (value.starts_with("-resource-dir="))
					output.resource_directory = value.substr(14U);
				else if (value == "-resource-dir")
					output.resource_directory = next();
				else if (value.starts_with("-mabi="))
					output.abi = value.substr(6U);
			}
			return output;
		}

		[[nodiscard]] std::string driver_mode(const std::vector<std::string>& arguments)
		{
			if (arguments.empty())
				return {};
			const auto driver = path{arguments.front()}.filename().generic_string();
			return driver.find("++") == std::string::npos ? "c" : "c++";
		}

		[[nodiscard]] std::vector<std::string>
		prefixed_arguments(const std::vector<std::string>& arguments, std::string_view prefix)
		{
			std::vector<std::string> output;
			for (std::size_t index = 0U; index < arguments.size(); ++index)
			{
				if (arguments[index].starts_with(prefix) && arguments[index].size() > prefix.size())
					output.push_back(arguments[index].substr(prefix.size()));
				else if (arguments[index] == prefix && index + 1U < arguments.size())
					output.push_back(arguments[++index]);
			}
			std::ranges::sort(output);
			output.erase(std::unique(output.begin(), output.end()), output.end());
			return output;
		}

		[[nodiscard]] json_value::array strings(const std::vector<std::string>& values)
		{
			json_value::array output;
			for (const auto& value : values)
				output.emplace_back(value);
			return output;
		}
	} // namespace

	struct compile_unit::data
	{
		compile_unit_id unit_id;
		build_variant_id build_id;
		file_id file;
		compile_command invocation;
		target_context target;
		std::string digest;
		std::string semantic_file;
	};

	struct workspace::data
	{
		path project_root;
		path database;
		path build_root;
		std::string root_origin;
		workspace_options options;
		std::vector<compile_unit> units;
		std::string database_digest;
		std::string configuration_digest;
		std::map<std::string, std::string> source_digests;
		std::string snapshot_key;
		std::shared_ptr<detail::runtime::filesystem_port> files;
		std::vector<detail::frontend::virtual_source_file> virtual_files;
		std::shared_ptr<detail::provisioning::provisioning_service> provisioning;
	};

	workspace_options workspace_options::from_compilation_database(path build_or_json)
	{
		workspace_options output;
		output.compilation_database = std::move(build_or_json);
		return output;
	}

	bool doctor_report::healthy() const noexcept
	{
		return std::ranges::none_of(diagnostics_,
									[](const diagnostic& value)
									{
										return value.level == severity::error ||
											value.level == severity::fatal;
									});
	}
	std::span<const diagnostic> doctor_report::diagnostics() const noexcept
	{
		return diagnostics_;
	}
	std::string doctor_report::to_json() const
	{
		constexpr std::array level_names{"note", "info", "warning", "error", "fatal"};
		json_value::array rows;
		for (const auto& value : diagnostics_)
			rows.emplace_back(json_value::object{
				{"id", value.id},
				{"message", value.message},
				{"severity", level_names.at(static_cast<std::size_t>(value.level))}});
		json_value document{detail::json::envelope(
			{"cxxlens.workspace-doctor.v1"},
			{{"diagnostics", json_value{std::move(rows)}}, {"healthy", healthy()}})};
		return detail::json::write(document).value();
	}
	std::string doctor_report::to_markdown() const
	{
		std::string output = healthy() ? "# Workspace: healthy\n" : "# Workspace: unhealthy\n";
		for (const auto& value : diagnostics_)
			output += "- `" + value.id + "`: " + value.message + "\n";
		return output;
	}

	compile_unit::compile_unit(std::shared_ptr<const data> value) : data_{std::move(value)} {}
	compile_unit_id compile_unit::id() const
	{
		return data_ ? data_->unit_id : compile_unit_id{};
	}
	build_variant_id compile_unit::variant_id() const
	{
		return data_ ? data_->build_id : build_variant_id{};
	}
	file_id compile_unit::main_file() const
	{
		return data_ ? data_->file : file_id{};
	}
	const compile_command& compile_unit::command() const
	{
		static const compile_command empty;
		return data_ ? data_->invocation : empty;
	}
	const target_context& compile_unit::target() const
	{
		static const target_context empty;
		return data_ ? data_->target : empty;
	}
	std::string compile_unit::command_digest() const
	{
		return data_ ? data_->digest : std::string{};
	}

	analysis_scope analysis_scope::all()
	{
		return {};
	}
	analysis_scope analysis_scope::files(std::vector<path> files)
	{
		analysis_scope output;
		output.kind_ = kind::files;
		std::ranges::sort(files,
						  {},
						  [](const path& value)
						  {
							  return value.generic_string();
						  });
		files.erase(std::unique(files.begin(), files.end()), files.end());
		output.files_ = std::move(files);
		return output;
	}
	analysis_scope analysis_scope::compile_units(std::vector<compile_unit_id> units)
	{
		analysis_scope output;
		output.kind_ = kind::compile_units;
		std::ranges::sort(units,
						  {},
						  [](const auto& value)
						  {
							  return value.value();
						  });
		units.erase(std::unique(units.begin(), units.end()), units.end());
		output.units_ = std::move(units);
		return output;
	}
	analysis_scope analysis_scope::changed_files(std::vector<path> files)
	{
		auto output = analysis_scope::files(std::move(files));
		output.kind_ = kind::changed_files;
		return output;
	}
	analysis_scope analysis_scope::include_headers(bool enabled) const
	{
		auto output = *this;
		output.include_headers_ = enabled;
		return output;
	}
	analysis_scope analysis_scope::variants(std::vector<build_variant_id> values) const
	{
		auto output = *this;
		std::ranges::sort(values,
						  {},
						  [](const auto& value)
						  {
							  return value.value();
						  });
		values.erase(std::unique(values.begin(), values.end()), values.end());
		output.variants_ = std::move(values);
		return output;
	}
	std::string analysis_scope::to_json() const
	{
		const std::array names{"all", "files", "compile_units", "changed_files"};
		json_value::array files;
		for (const auto& value : files_)
			files.emplace_back(value.generic_string());
		json_value::array units;
		for (const auto& value : units_)
			units.emplace_back(std::string{value.value()});
		json_value variants = json_value{};
		if (variants_)
		{
			json_value::array values;
			for (const auto& value : *variants_)
				values.emplace_back(std::string{value.value()});
			variants = json_value{std::move(values)};
		}
		json_value::object fields{{"compile_units", json_value{std::move(units)}},
								  {"files", json_value{std::move(files)}},
								  {"include_headers", include_headers_},
								  {"kind", names.at(static_cast<std::size_t>(kind_))},
								  {"variants", std::move(variants)}};
		json_value document{
			detail::json::envelope({"cxxlens.analysis-scope.v1"}, std::move(fields))};
		return detail::json::write(document).value();
	}

	workspace::workspace(std::shared_ptr<const data> value) : data_{std::move(value)} {}

	result<workspace>
	workspace::open(workspace_options options,
					execution_context context) // NOLINT(performance-unnecessary-value-param)
	{
		return detail::workspace_catalog_access::open(
			std::move(options),
			context,
			std::make_shared<detail::runtime::standard_filesystem_adapter>());
	}

	result<workspace>
	detail::workspace_catalog_access::open(workspace_options options,
										   const execution_context& context,
										   const std::shared_ptr<runtime::filesystem_port>& files,
										   std::vector<frontend::virtual_source_file> virtual_files)
	{
		if (context.cancellation.stop_requested())
			return workspace_error("core.cancelled", "cancelled");
		if (!files)
			return workspace_error("core.invalid-argument", "filesystem-port-missing");
		const auto& filesystem = *files;
		path database = options.compilation_database;
		if (database.extension() != ".json")
			database /= "compile_commands.json";
		request_context request;
		request.operation = "workspace.compile-database.read";
		request.cancellation = context.cancellation;
		request.deadline = context.deadline;
		auto database_path = filesystem.canonicalize(database, request);
		if (!database_path)
			return workspace_error(
				"workspace.compile-database-not-found", "database-not-found", database);
		auto content = filesystem.read(database_path.value(), request);
		if (!content)
			return workspace_error(
				"workspace.compile-database-not-found", "database-not-found", database);
		auto parsed = database_parser{content.value()}.parse();
		if (!parsed || !std::holds_alternative<database_value::array>(parsed.value().value))
			return parsed ? result<workspace>{workspace_error("workspace.compile-database-invalid",
															  "root-not-array")}
						  : result<workspace>{std::move(parsed.error())};

		const auto& database_entries = std::get<database_value::array>(parsed.value().value);
		auto resolved_entries =
			resolve_entries(database_entries, database_path.value(), filesystem, request);
		if (!resolved_entries)
			return std::move(resolved_entries.error());
		const bool inferred_root = options.project_root.empty();
		path root;
		if (inferred_root)
		{
			auto inferred = infer_project_root(database_path.value(), resolved_entries.value());
			if (!inferred)
				return std::move(inferred.error());
			root = std::move(inferred.value());
		}
		else
			root = options.project_root;
		auto canonical_root = filesystem.canonicalize(root, request);
		if (!canonical_root)
			return workspace_error(
				"workspace.compile-database-invalid", "project-root-invalid", root);
		root = canonical_root.value();
		auto state = std::make_shared<workspace::data>();
		state->project_root = root;
		state->database = database_path.value();
		state->build_root = database_path.value().parent_path();
		state->root_origin = inferred_root ? "inferred-common-ancestor" : "explicit";
		state->options = options;
		state->files = files;
		state->virtual_files = std::move(virtual_files);
		auto database_id = stable_id(
			"snapshot", "cxxlens.compile-database-digest.v1", {{"content", content.value()}});
		if (!database_id)
			return std::move(database_id.error());
		state->database_digest = digest_part(database_id.value());

		std::set<std::string> unit_keys;
		for (const auto& resolved : resolved_entries.value())
		{
			const auto& object = *resolved.object;
			const auto& directory = resolved.directory;
			const auto& source = resolved.source;
			auto source_key = detail::workspace_paths::semantic_path(root, source);
			if (!source_key)
				return workspace_error(
					"workspace.compile-database-invalid", "source-outside-project-root", source);
			auto directory_key = detail::workspace_paths::build_path(
				{root, database_path.value().parent_path()}, directory);
			if (!directory_key)
				return workspace_error("workspace.compile-database-invalid",
									   "working-directory-outside-mapped-roots",
									   directory);

			std::vector<std::string> arguments;
			if (const auto* argument_value = field(object, "arguments"))
			{
				if (!std::holds_alternative<database_value::array>(argument_value->value))
					return workspace_error("workspace.compile-database-invalid",
										   "arguments-not-array");
				for (const auto& argument : std::get<database_value::array>(argument_value->value))
				{
					if (!std::holds_alternative<std::string>(argument.value))
						return workspace_error("workspace.compile-database-invalid",
											   "argument-not-string");
					arguments.push_back(std::get<std::string>(argument.value));
				}
			}
			else
			{
				auto command = required_string(object, "command");
				if (!command)
					return std::move(command.error());
				auto command_arguments = parse_command(command.value());
				if (!command_arguments)
					return std::move(command_arguments.error());
				arguments = std::move(command_arguments.value());
			}
			auto expanded = expand_response_files(arguments, directory, filesystem);
			if (!expanded)
				return std::move(expanded.error());
			arguments = std::move(expanded.value());
			if (arguments.empty())
				return workspace_error("workspace.compile-database-invalid", "arguments-empty");
			if (unsafe_driver_flags(arguments))
				return workspace_error("workspace.driver-not-allowed", "plugin-or-load-flag");

			const auto variant_inputs = variant_arguments(arguments, {directory, source});
			std::vector<std::string> semantic_arguments;
			semantic_arguments.reserve(variant_inputs.size());
			for (const auto& argument : variant_inputs)
				semantic_arguments.push_back(
					semantic_argument(root, database_path.value().parent_path(), argument));
			std::string joined;
			for (const auto& argument : semantic_arguments)
				joined += std::to_string(argument.size()) + ":" + argument;
			auto variant = stable_id("variant",
									 "cxxlens.build-variant-id.v1",
									 {{"arguments", joined}, {"directory", directory_key.value()}});
			if (!variant)
				return std::move(variant.error());
			auto file_identity =
				stable_id("file", "cxxlens.file-id.v1", {{"source_key", source_key.value()}});
			if (!file_identity)
				return std::move(file_identity.error());
			auto unit =
				stable_id("cu",
						  "cxxlens.compile-unit-id.v1",
						  {{"source_key", source_key.value()}, {"variant", variant.value()}});
			if (!unit)
				return std::move(unit.error());
			const auto duplicate_key = source_key.value() + '\0' + variant.value();
			if (!unit_keys.insert(duplicate_key).second)
				return workspace_error(
					"workspace.compile-database-invalid", "duplicate-command", source);

			auto value = std::make_shared<compile_unit::data>();
			value->unit_id = compile_unit_id{unit.value()};
			value->build_id = build_variant_id{variant.value()};
			value->file = file_id{file_identity.value()};
			value->invocation = {directory, source, std::move(arguments), std::nullopt};
			if (const auto* output = field(object, "output");
				output != nullptr && std::holds_alternative<std::string>(output->value))
				value->invocation.output = std::get<std::string>(output->value);
			value->target = project_target(value->invocation.arguments);
			value->digest = std::string{build_variant_id{variant.value()}.full_digest()};
			value->semantic_file = source_key.value();
			state->units.emplace_back(compile_unit{std::move(value)});

			request.operation = "workspace.source.read";
			auto source_content = filesystem.read(source, request);
			const auto source_payload =
				source_content ? source_content.value() : std::string{"<missing>"};
			auto source_digest =
				stable_id("snapshot", "cxxlens.source-digest.v1", {{"content", source_payload}});
			if (!source_digest)
				return std::move(source_digest.error());
			state->source_digests[source_key.value()] = digest_part(source_digest.value());
		}
		std::ranges::sort(state->units,
						  [](const compile_unit& left, const compile_unit& right)
						  {
							  return std::pair{left.command().file.generic_string(),
											   std::string{left.variant_id().value()}} <
								  std::pair{right.command().file.generic_string(),
											std::string{right.variant_id().value()}};
						  });
		std::string snapshot_payload = state->database_digest + "|schema=2|semantics=2|llvm=22";
		for (const auto& [file, digest] : state->source_digests)
		{
			snapshot_payload.append("|");
			snapshot_payload.append(file);
			snapshot_payload.append("=");
			snapshot_payload.append(digest);
		}
		if (options.configuration_file)
		{
			request.operation = "workspace.configuration.read";
			auto configuration = filesystem.read(*options.configuration_file, request);
			if (!configuration)
				return workspace_error("config.file-not-found",
									   "workspace-configuration",
									   *options.configuration_file);
			auto digest = stable_id("snapshot",
									"cxxlens.configuration-digest.v1",
									{{"content", configuration.value()}});
			if (!digest)
				return std::move(digest.error());
			state->configuration_digest = digest_part(digest.value());
			snapshot_payload += "|config=" + state->configuration_digest;
		}
		auto snapshot =
			stable_id("snapshot", "cxxlens.workspace-snapshot.v2", {{"inputs", snapshot_payload}});
		if (!snapshot)
			return std::move(snapshot.error());
		state->snapshot_key = snapshot.value();
		auto database_key = detail::workspace_paths::build_path(
			{root, database_path.value().parent_path()}, database_path.value());
		if (!database_key)
			return workspace_error("workspace.compile-database-invalid",
								   "database-outside-mapped-roots",
								   database_path.value());
		auto cache_key = stable_id(
			"snapshot",
			"cxxlens.workspace-cache.v2",
			{{"database", database_key.value()}, {"path_mapping", "project-relative-v2"}});
		if (!cache_key)
			return std::move(cache_key.error());
		auto provisioning =
			detail::provisioning::provisioning_service::create({state->project_root,
																state->units,
																state->options.configuration_file,
																state->options.cache_directory,
																cache_key.value(),
																state->files,
																state->virtual_files});
		if (!provisioning)
			return std::move(provisioning.error());
		state->provisioning = std::move(provisioning.value());
		return workspace{std::move(state)};
	}

	compile_unit
	detail::workspace_catalog_access::reconstitute_compile_unit(compile_unit_id unit,
																build_variant_id variant,
																file_id main_file,
																compile_command command,
																target_context target,
																std::string command_digest)
	{
		auto value = std::make_shared<compile_unit::data>();
		value->unit_id = std::move(unit);
		value->build_id = std::move(variant);
		value->file = std::move(main_file);
		value->invocation = std::move(command);
		value->target = std::move(target);
		value->digest = std::move(command_digest);
		return compile_unit{std::move(value)};
	}

	path workspace::root() const
	{
		return data_ ? data_->project_root : path{};
	}
	std::vector<compile_unit> workspace::compile_units() const
	{
		return data_ ? data_->units : std::vector<compile_unit>{};
	}
	std::vector<compile_unit> workspace::command_for(path file) const
	{
		if (!data_)
			return {};
		if (file.is_relative())
			file = data_->project_root / file;
		file = file.lexically_normal();
		std::vector<compile_unit> output;
		for (const auto& unit : data_->units)
			if (unit.command().file.lexically_normal() == file)
				output.push_back(unit);
		if (!output.empty() ||
			data_->options.commands != compile_command_policy::allow_header_inference)
			return output;
		const auto extension = file.extension().generic_string();
		if (extension != ".h" && extension != ".hh" && extension != ".hpp" && extension != ".hxx")
			return {};
		for (const auto& unit : data_->units)
			if (unit.command().file.stem() == file.stem())
				output.push_back(unit);
		return output;
	}

	// The frozen public API takes immutable request values by value.
	// NOLINTBEGIN(performance-unnecessary-value-param)
	result<void>
	workspace::ensure(fact_profile profile, analysis_scope scope, execution_context context) const
	{
		if (!data_ || !data_->provisioning)
			return workspace_error("core.invalid-argument", "unbound-workspace");
		return data_->provisioning->ensure(profile, scope, std::move(context));
	}
	// NOLINTEND(performance-unnecessary-value-param)

	fact_store workspace::facts() const
	{
		return data_ && data_->provisioning ? data_->provisioning->facts() : fact_store{};
	}

	capability_set workspace::capabilities() const
	{
		return data_ && data_->provisioning ? data_->provisioning->capabilities()
											: cxxlens::capabilities();
	}

	result<doctor_report> workspace::doctor(execution_context context) const
	{
		if (!data_ || !data_->provisioning)
			return workspace_error("core.invalid-argument", "unbound-workspace");
		auto diagnostics = data_->provisioning->diagnose(std::move(context));
		if (!diagnostics)
			return std::move(diagnostics.error());
		doctor_report output;
		output.diagnostics_ = std::move(diagnostics.value());
		return output;
	}

	std::string workspace::explain_build_context(path file) const
	{
		if (!data_)
			return "{}";
		if (!data_->files)
			return "{}";
		const auto& files = *data_->files;
		request_context request;
		request.operation = "workspace.snapshot.recheck";
		auto database = files.read(data_->database, request);
		bool compatible = database.has_value();
		bool database_compatible = compatible;
		if (database)
		{
			auto digest = stable_id(
				"snapshot", "cxxlens.compile-database-digest.v1", {{"content", database.value()}});
			database_compatible = digest && digest_part(digest.value()) == data_->database_digest;
			compatible = database_compatible;
		}
		json_value::array stale;
		if (!database_compatible)
			stale.emplace_back("compilation_database");
		if (data_->options.configuration_file)
		{
			const auto configuration_file = data_->options.configuration_file.value_or(path{});
			auto content = files.read(configuration_file, request);
			const auto payload = content ? content.value() : std::string{"<missing>"};
			auto digest =
				stable_id("snapshot", "cxxlens.configuration-digest.v1", {{"content", payload}});
			if (!digest || digest_part(digest.value()) != data_->configuration_digest)
			{
				compatible = false;
				stale.emplace_back("configuration");
			}
		}
		for (const auto& unit : data_->units)
		{
			const auto key =
				detail::workspace_paths::semantic_path(data_->project_root, unit.command().file);
			if (!key || !data_->source_digests.contains(key.value()))
			{
				compatible = false;
				stale.emplace_back("path-mapping-invalid");
				continue;
			}
			auto content = files.read(unit.command().file, request);
			const auto payload = content ? content.value() : std::string{"<missing>"};
			auto digest = stable_id("snapshot", "cxxlens.source-digest.v1", {{"content", payload}});
			if (!digest || digest_part(digest.value()) != data_->source_digests.at(key.value()))
			{
				compatible = false;
				const auto already_reported = std::ranges::any_of(
					stale,
					[&](const json_value& value)
					{
						return std::get<std::string>(value.value) == key.value();
					});
				if (!already_reported)
					stale.emplace_back(key.value());
			}
		}
		auto matches = file.empty() ? data_->units : command_for(file);
		json_value::array units;
		for (const auto& unit : matches)
		{
			const auto directory = detail::workspace_paths::build_path(
				{data_->project_root, data_->build_root}, unit.command().directory);
			const auto source =
				detail::workspace_paths::semantic_path(data_->project_root, unit.command().file);
			auto macros = prefixed_arguments(unit.command().arguments, "-D");
			auto undefines = prefixed_arguments(unit.command().arguments, "-U");
			auto includes = prefixed_arguments(unit.command().arguments, "-I");
			auto system_includes = prefixed_arguments(unit.command().arguments, "-isystem");
			units.emplace_back(json_value::object{
				{"arguments", json_value{strings(unit.command().arguments)}},
				{"command_digest", unit.command_digest()},
				{"compile_unit_id", std::string{unit.id().value()}},
				{"directory", directory ? directory.value() : std::string{"$invalid/path-mapping"}},
				{"driver_mode", driver_mode(unit.command().arguments)},
				{"file", source ? source.value() : std::string{"$invalid/path-mapping"}},
				{"includes", json_value{strings(includes)}},
				{"language_standard", unit.target().language_standard},
				{"macros", json_value{strings(macros)}},
				{"resource_directory",
				 unit.target().resource_directory
					 ? json_value{unit.target().resource_directory.value_or(std::string{})}
					 : json_value{}},
				{"response_files_expanded",
				 std::ranges::none_of(unit.command().arguments,
									  [](const std::string& value)
									  {
										  return value.starts_with('@');
									  })},
				{"system_includes", json_value{strings(system_includes)}},
				{"target", unit.target().triple},
				{"undefines", json_value{strings(undefines)}},
				{"variant_id", std::string{unit.variant_id().value()}},
			});
		}
		const bool inferred = !file.empty() && !matches.empty() &&
			std::ranges::none_of(
				matches,
				[&](const compile_unit& unit)
				{
					path query = file.is_relative() ? data_->project_root / file : file;
					return unit.command().file.lexically_normal() == query.lexically_normal();
				});
		const auto query_path = file.empty()
			? detail::workspace_paths::semantic_path(data_->project_root, data_->project_root)
			: detail::workspace_paths::semantic_path(
				  data_->project_root, file.is_relative() ? data_->project_root / file : file);
		const auto build_root = detail::workspace_paths::build_path(
			{data_->project_root, data_->build_root}, data_->build_root);
		json_value::object fields{
			{"compatible", compatible},
			{"header_inference", inferred ? "stem-match" : "none"},
			{"path_mapping",
			 json_value::object{
				 {"build_root",
				  build_root ? build_root.value() : std::string{"$invalid/path-mapping"}},
				 {"external_build_namespace", "$build"},
				 {"external_source_policy", "reject"},
				 {"policy", "project-relative-v2"},
				 {"project_root", data_->project_root.generic_string()},
				 {"project_root_origin", data_->root_origin}}},
			{"query",
			 file.empty() ? std::string{}
						  : (query_path ? query_path.value()
										: std::string{"$invalid/outside-project-root"})},
			{"snapshot_key", data_->snapshot_key},
			{"stale_inputs", json_value{std::move(stale)}},
			{"units", json_value{std::move(units)}}};
		json_value document{
			detail::json::envelope({"cxxlens.workspace-context.v1"}, std::move(fields))};
		return detail::json::write(document).value();
	}

	void detail::workspace_provisioning_access::set_worker(
		workspace& value,
		std::shared_ptr<detail::scheduling::worker_port> worker,
		const bool requires_clang_capability)
	{
		if (value.data_ && value.data_->provisioning)
			value.data_->provisioning->set_worker(std::move(worker), requires_clang_capability);
	}

	detail::provisioning::provisioning_trace
	detail::workspace_provisioning_access::last_trace(const workspace& value)
	{
		return value.data_ && value.data_->provisioning
			? value.data_->provisioning->last_trace()
			: detail::provisioning::provisioning_trace{};
	}
} // namespace cxxlens
