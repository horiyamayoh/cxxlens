#include <algorithm>
#include <cctype>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "configuration_loader.hpp"
#include "configuration_spec.hpp"

namespace cxxlens::detail::config
{
	namespace
	{
		constexpr std::size_t maximum_document_bytes = std::size_t{1024U} * 1024U;
		constexpr std::size_t maximum_line_bytes = std::size_t{16U} * 1024U;
		constexpr std::size_t maximum_nodes = 4096U;
		constexpr std::size_t maximum_depth = 32U;

		struct yaml_node
		{
			using map = std::map<std::string, yaml_node, std::less<>>;
			using sequence = std::vector<yaml_node>;
			using scalar = std::variant<bool, std::int64_t, std::string>;
			std::variant<map, sequence, scalar> value;
		};

		struct yaml_line
		{
			std::size_t indent{};
			std::size_t number{};
			std::string content;
		};

		struct block_context
		{
			std::size_t indent{};
			std::size_t depth{};
		};

		[[nodiscard]] error config_error(std::string code, std::string reason, std::string key = {})
		{
			error failure;
			failure.code.value = std::move(code);
			failure.message = "configuration document is invalid";
			failure.attributes.emplace("reason", std::move(reason));
			if (!key.empty())
				failure.attributes.emplace("key", std::move(key));
			return failure;
		}

		[[nodiscard]] std::string_view trim(std::string_view value)
		{
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front())) != 0)
				value.remove_prefix(1U);
			while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())) != 0)
				value.remove_suffix(1U);
			return value;
		}

		[[nodiscard]] result<std::vector<yaml_line>> tokenize(const std::string_view input)
		{
			if (input.size() > maximum_document_bytes)
				return config_error("config.yaml-invalid", "document-size-limit");
			std::vector<yaml_line> output;
			std::size_t offset = 0U;
			std::size_t line_number = 1U;
			while (offset <= input.size())
			{
				const auto end = input.find('\n', offset);
				const auto extent =
					end == std::string_view::npos ? input.size() - offset : end - offset;
				if (extent > maximum_line_bytes)
					return config_error("config.yaml-invalid", "line-size-limit");
				auto line = input.substr(offset, extent);
				if (!line.empty() && line.back() == '\r')
					line.remove_suffix(1U);
				bool single = false;
				bool double_quote = false;
				std::size_t comment = line.size();
				for (std::size_t index = 0U; index < line.size(); ++index)
				{
					const char character = line.at(index);
					if (character == '\t')
						return config_error("config.yaml-invalid", "tab-not-allowed");
					if (character == '\'' && !double_quote)
						single = !single;
					else if (character == '"' && !single &&
							 (index == 0U || line.at(index - 1U) != '\\'))
						double_quote = !double_quote;
					else if (character == '#' && !single && !double_quote &&
							 (index == 0U ||
							  std::isspace(static_cast<unsigned char>(line.at(index - 1U))) != 0))
					{
						comment = index;
						break;
					}
				}
				if (single || double_quote)
					return config_error("config.yaml-invalid", "unterminated-quote");
				line = line.substr(0U, comment);
				std::size_t indent = 0U;
				while (indent < line.size() && line.at(indent) == ' ')
					++indent;
				const auto content = trim(line.substr(indent));
				if (!content.empty())
					output.push_back({indent, line_number, std::string{content}});
				if (end == std::string_view::npos)
					break;
				offset = end + 1U;
				++line_number;
			}
			return output;
		}

		class inline_parser
		{
		  public:
			explicit inline_parser(const std::string_view input, std::size_t& nodes)
				: input_{input}, nodes_{nodes}
			{
			}

			[[nodiscard]] result<yaml_node> parse()
			{
				auto node = parse_value(0U);
				if (!node)
					return std::move(node.error());
				skip_space();
				if (position_ != input_.size())
					return config_error("config.yaml-invalid", "unexpected-inline-token");
				return std::move(node.value());
			}

		  private:
			[[nodiscard]] result<yaml_node> parse_value(const std::size_t depth)
			{
				if (depth > maximum_depth || ++nodes_ > maximum_nodes)
					return config_error("config.yaml-invalid", "structure-limit");
				skip_space();
				if (position_ >= input_.size())
					return config_error("config.yaml-invalid", "missing-value");
				if (input_.at(position_) == '[')
					return parse_sequence(depth);
				if (input_.at(position_) == '{')
					return parse_map(depth);
				return parse_scalar();
			}

			[[nodiscard]] result<yaml_node> parse_sequence(const std::size_t depth)
			{
				++position_;
				yaml_node::sequence sequence;
				skip_space();
				while (position_ < input_.size() && input_.at(position_) != ']')
				{
					auto child = parse_value(depth + 1U);
					if (!child)
						return std::move(child.error());
					sequence.push_back(std::move(child.value()));
					skip_space();
					if (position_ < input_.size() && input_.at(position_) == ',')
					{
						++position_;
						skip_space();
					}
					else
						break;
				}
				if (position_ >= input_.size() || input_.at(position_) != ']')
					return config_error("config.yaml-invalid", "unterminated-sequence");
				++position_;
				return yaml_node{std::move(sequence)};
			}

			[[nodiscard]] result<yaml_node> parse_map(const std::size_t depth)
			{
				++position_;
				yaml_node::map map;
				skip_space();
				while (position_ < input_.size() && input_.at(position_) != '}')
				{
					auto key = parse_token(":");
					if (!key || position_ >= input_.size() || input_.at(position_) != ':')
						return config_error("config.yaml-invalid", "inline-map-key");
					++position_;
					auto child = parse_value(depth + 1U);
					if (!child)
						return std::move(child.error());
					if (!map.emplace(std::move(*key), std::move(child.value())).second)
						return config_error("config.yaml-invalid", "duplicate-key");
					skip_space();
					if (position_ < input_.size() && input_.at(position_) == ',')
					{
						++position_;
						skip_space();
					}
					else
						break;
				}
				if (position_ >= input_.size() || input_.at(position_) != '}')
					return config_error("config.yaml-invalid", "unterminated-map");
				++position_;
				return yaml_node{std::move(map)};
			}

			[[nodiscard]] result<yaml_node> parse_scalar()
			{
				auto token = parse_token(",]}");
				if (!token)
					return config_error("config.yaml-invalid", "empty-scalar");
				if (token->starts_with('&') || token->starts_with('*') || token->starts_with('!') ||
					*token == "|" || *token == ">")
					return config_error("config.yaml-invalid", "unsupported-yaml-feature");
				if (*token == "true")
					return yaml_node{yaml_node::scalar{true}};
				if (*token == "false")
					return yaml_node{yaml_node::scalar{false}};
				std::int64_t integer{};
				const auto converted =
					std::from_chars(token->data(), token->data() + token->size(), integer);
				if (converted.ec == std::errc{} && converted.ptr == token->data() + token->size())
					return yaml_node{yaml_node::scalar{integer}};
				return yaml_node{yaml_node::scalar{std::move(*token)}};
			}

			[[nodiscard]] std::optional<std::string> parse_token(const std::string_view delimiters)
			{
				skip_space();
				if (position_ >= input_.size())
					return std::nullopt;
				const char quote = input_.at(position_);
				if (quote == '\'' || quote == '"')
				{
					++position_;
					std::string output;
					while (position_ < input_.size() && input_.at(position_) != quote)
					{
						const char character = input_.at(position_++);
						if (character == '\\' && quote == '"' && position_ < input_.size())
						{
							const char escaped = input_.at(position_++);
							if (escaped == 'n')
								output.push_back('\n');
							else if (escaped == 't')
								output.push_back('\t');
							else if (escaped == '"' || escaped == '\\')
								output.push_back(escaped);
							else
								return std::nullopt;
						}
						else
							output.push_back(character);
					}
					if (position_ >= input_.size())
						return std::nullopt;
					++position_;
					return output;
				}
				const auto begin = position_;
				while (position_ < input_.size() &&
					   delimiters.find(input_.at(position_)) == std::string_view::npos)
					++position_;
				const auto token = trim(input_.substr(begin, position_ - begin));
				return token.empty() ? std::nullopt
									 : std::optional<std::string>{std::string{token}};
			}

			void skip_space()
			{
				while (position_ < input_.size() &&
					   std::isspace(static_cast<unsigned char>(input_.at(position_))) != 0)
					++position_;
			}

			std::string_view input_;
			std::size_t& nodes_;
			std::size_t position_{};
		};

		class block_parser
		{
		  public:
			explicit block_parser(std::vector<yaml_line> lines) : lines_{std::move(lines)} {}

			[[nodiscard]] result<yaml_node> parse()
			{
				if (lines_.empty())
					return config_error("config.yaml-invalid", "empty-document");
				if (lines_.front().indent != 0U)
					return config_error("config.yaml-invalid", "root-indented");
				auto result = parse_block(0U, 0U);
				if (!result)
					return std::move(result.error());
				if (index_ != lines_.size())
					return config_error("config.yaml-invalid", "invalid-indentation");
				return std::move(result.value());
			}

		  private:
			[[nodiscard]] static std::optional<std::size_t>
			mapping_colon(const std::string_view text)
			{
				bool single = false;
				bool double_quote = false;
				int brackets = 0;
				for (std::size_t index = 0U; index < text.size(); ++index)
				{
					const char character = text.at(index);
					if (character == '\'' && !double_quote)
						single = !single;
					else if (character == '"' && !single)
						double_quote = !double_quote;
					else if (!single && !double_quote && (character == '[' || character == '{'))
						++brackets;
					else if (!single && !double_quote && (character == ']' || character == '}'))
						--brackets;
					else if (!single && !double_quote && brackets == 0 && character == ':')
						return index;
				}
				return std::nullopt;
			}

			[[nodiscard]] result<yaml_node> parse_block(const std::size_t indent,
														const std::size_t depth)
			{
				if (depth > maximum_depth || ++nodes_ > maximum_nodes)
					return config_error("config.yaml-invalid", "structure-limit");
				if (index_ >= lines_.size() || lines_.at(index_).indent != indent)
					return config_error("config.yaml-invalid", "invalid-indentation");
				if (lines_.at(index_).content.starts_with("-"))
					return parse_sequence({indent, depth});
				return parse_map({indent, depth});
			}

			[[nodiscard]] result<yaml_node> parse_map(const block_context context)
			{
				yaml_node::map map;
				while (index_ < lines_.size() && lines_.at(index_).indent == context.indent &&
					   !lines_.at(index_).content.starts_with("-"))
				{
					const auto& line = lines_.at(index_);
					const auto colon = mapping_colon(line.content);
					if (!colon)
						return config_error("config.yaml-invalid", "mapping-colon-missing");
					const auto key_view = trim(std::string_view{line.content}.substr(0U, *colon));
					if (key_view.empty() ||
						key_view.find_first_not_of(
							"abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") !=
							std::string_view::npos)
						return config_error("config.yaml-invalid", "invalid-key");
					std::string key{key_view};
					if (map.contains(key))
						return config_error("config.yaml-invalid", "duplicate-key", key);
					const auto remainder = trim(std::string_view{line.content}.substr(*colon + 1U));
					++index_;
					result<yaml_node> child = config_error("config.yaml-invalid", "missing-value");
					if (!remainder.empty())
						child = inline_parser{remainder, nodes_}.parse();
					else if (index_ < lines_.size() && lines_.at(index_).indent > context.indent)
						child = parse_block(lines_.at(index_).indent, context.depth + 1U);
					if (!child)
						return std::move(child.error());
					map.emplace(std::move(key), std::move(child.value()));
				}
				return yaml_node{std::move(map)};
			}

			[[nodiscard]] result<yaml_node> parse_sequence(const block_context context)
			{
				yaml_node::sequence sequence;
				while (index_ < lines_.size() && lines_.at(index_).indent == context.indent &&
					   lines_.at(index_).content.starts_with("-"))
				{
					const auto item = trim(std::string_view{lines_.at(index_).content}.substr(1U));
					++index_;
					result<yaml_node> child =
						config_error("config.yaml-invalid", "empty-sequence-item");
					if (!item.empty())
						child = inline_parser{item, nodes_}.parse();
					else if (index_ < lines_.size() && lines_.at(index_).indent > context.indent)
						child = parse_block(lines_.at(index_).indent, context.depth + 1U);
					if (!child)
						return std::move(child.error());
					sequence.push_back(std::move(child.value()));
				}
				return yaml_node{std::move(sequence)};
			}

			std::vector<yaml_line> lines_;
			std::size_t index_{};
			std::size_t nodes_{};
		};

		[[nodiscard]] result<configuration_value> to_value(const std::string_view key,
														   const yaml_node& node,
														   const environment_port& environment)
		{
			const auto found = configuration_specs().find(key);
			if (found == configuration_specs().end())
				return config_error("config.unknown-key", "unknown-key", std::string{key});
			configuration_value value;
			if (const auto* scalar = std::get_if<yaml_node::scalar>(&node.value))
				value = std::visit(
					[](const auto& item) -> configuration_value
					{
						return item;
					},
					*scalar);
			else if (const auto* sequence = std::get_if<yaml_node::sequence>(&node.value))
			{
				std::vector<std::string> strings;
				for (const auto& item : *sequence)
				{
					const auto* item_scalar = std::get_if<yaml_node::scalar>(&item.value);
					if (item_scalar == nullptr ||
						!std::holds_alternative<std::string>(*item_scalar))
						return config_error(
							"config.invalid-value", "list-item-wrong-type", std::string{key});
					strings.push_back(std::get<std::string>(*item_scalar));
				}
				value = std::move(strings);
			}
			else
				return config_error("config.invalid-value", "map-used-as-value", std::string{key});

			const auto interpolate = [&](std::string& text) -> result<void>
			{
				std::size_t position = 0U;
				while ((position = text.find("${", position)) != std::string::npos)
				{
					const auto end = text.find('}', position + 2U);
					if (end == std::string::npos)
						return config_error(
							"config.invalid-value", "malformed-placeholder", std::string{key});
					if (!found->second.path_placeholder && !found->second.secret)
						return config_error("config.invalid-value",
											"semantic-environment-override",
											std::string{key});
					const auto name =
						std::string_view{text}.substr(position + 2U, end - position - 2U);
					if (name.empty() ||
						name.find_first_not_of("ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_") !=
							std::string_view::npos)
						return config_error(
							"config.invalid-value", "invalid-placeholder-name", std::string{key});
					const auto replacement = environment.get(name);
					if (!replacement)
						return config_error(
							"config.invalid-value", "placeholder-not-found", std::string{key});
					text.replace(position, end - position + 1U, *replacement);
					position += replacement->size();
				}
				return {};
			};
			if (auto* text = std::get_if<std::string>(&value))
			{
				if (auto status = interpolate(*text); !status)
					return std::move(status.error());
			}
			else if (auto* strings = std::get_if<std::vector<std::string>>(&value))
			{
				for (auto& item_text : *strings)
					if (auto status = interpolate(item_text); !status)
						return std::move(status.error());
			}
			if (auto status = validate_configuration_value(key, value); !status)
				return std::move(status.error());
			return value;
		}

		[[nodiscard]] result<void>
		flatten(const yaml_node::map& map,
				const std::string& prefix,
				std::map<std::string, configuration_value, std::less<>>& output,
				const environment_port& environment)
		{
			for (const auto& [name, node] : map)
			{
				std::string key = prefix;
				if (!key.empty())
					key.push_back('.');
				key.append(name);
				if (const auto* child = std::get_if<yaml_node::map>(&node.value))
				{
					if (auto status = flatten(*child, key, output, environment); !status)
						return status;
				}
				else
				{
					auto value = to_value(key, node, environment);
					if (!value)
						return std::move(value.error());
					output.emplace(key, std::move(value.value()));
				}
			}
			return {};
		}

		[[nodiscard]] result<std::shared_ptr<const configuration_data>> parse_document(
			const std::string& bytes, const path& yaml_file, const environment_port& environment)
		{
			auto lines = tokenize(bytes);
			if (!lines)
				return std::move(lines.error());
			auto parsed = block_parser{std::move(lines.value())}.parse();
			if (!parsed)
				return std::move(parsed.error());
			const auto* root = std::get_if<yaml_node::map>(&parsed.value().value);
			if (root == nullptr)
				return config_error("config.yaml-invalid", "root-not-map");
			const auto schema = root->find("schema");
			if (schema == root->end())
				return config_error("config.yaml-invalid", "schema-missing");
			const auto* schema_scalar = std::get_if<yaml_node::scalar>(&schema->second.value);
			if (schema_scalar == nullptr || !std::holds_alternative<std::string>(*schema_scalar) ||
				std::get<std::string>(*schema_scalar) != "cxxlens.config.v1")
				return config_error("config.yaml-invalid", "schema-unsupported");

			yaml_node::map defaults = *root;
			defaults.erase("schema");
			const auto profiles_node = defaults.extract("profiles");
			auto data = std::make_shared<configuration_data>();
			std::map<std::string, configuration_value, std::less<>> flattened;
			if (auto status = flatten(defaults, {}, flattened, environment); !status)
				return std::move(status.error());
			for (auto& [key, value] : flattened)
				data->values[key].push_back({configuration_layer::config_default,
											 std::move(value),
											 yaml_file.filename().generic_string()});

			if (!profiles_node.empty())
			{
				const auto* profiles = std::get_if<yaml_node::map>(&profiles_node.mapped().value);
				if (profiles == nullptr)
					return config_error("config.invalid-value", "profiles-not-map", "profiles");
				for (const auto& [name, profile_node] : *profiles)
				{
					const auto* profile = std::get_if<yaml_node::map>(&profile_node.value);
					if (profile == nullptr)
						return config_error("config.invalid-value", "profile-not-map", name);
					if (auto status = flatten(*profile, {}, data->profiles[name], environment);
						!status)
						return std::move(status.error());
				}
			}
			return std::shared_ptr<const configuration_data>{std::move(data)};
		}

		[[nodiscard]] error runtime_file_error(const runtime::runtime_failure& failure,
											   const std::string& code,
											   const path& file)
		{
			auto result = config_error(code, "filesystem-operation-failed");
			result.attributes.emplace("path", file.generic_string());
			result.attributes.emplace("runtime_operation", failure.operation);
			result.attributes.emplace("runtime_status",
									  std::to_string(static_cast<unsigned>(failure.status)));
			return result;
		}
	} // namespace

	result<std::shared_ptr<const configuration_data>>
	load_document(const path& yaml_file,
				  const runtime::filesystem_port& filesystem,
				  const environment_port& environment)
	{
		runtime::request_context context;
		context.operation = "configuration.load";
		auto canonical = filesystem.canonicalize(yaml_file, context);
		if (!canonical)
			return runtime_file_error(canonical.error(), "config.file-not-found", yaml_file);
		auto bytes = filesystem.read(canonical.value(), context);
		if (!bytes)
		{
			const auto code = bytes.error().status == runtime::runtime_status::missing
				? "config.file-not-found"
				: "config.yaml-invalid";
			return runtime_file_error(bytes.error(), code, canonical.value());
		}
		return parse_document(bytes.value(), canonical.value(), environment);
	}

	result<std::shared_ptr<const configuration_data>>
	load_nearest_document(const path& start,
						  const runtime::filesystem_port& filesystem,
						  const environment_port& environment)
	{
		runtime::request_context context;
		context.operation = "configuration.load-nearest";
		auto canonical_start = filesystem.canonicalize(start, context);
		if (!canonical_start)
			return runtime_file_error(canonical_start.error(), "config.file-not-found", start);
		auto current = canonical_start.value();
		auto status = filesystem.stat(current, context);
		if (status && status.value().regular)
			current = current.parent_path();
		const auto filesystem_root = current.root_path();
		path project_boundary = filesystem_root;
		for (auto probe = current;; probe = probe.parent_path())
		{
			const auto root_marker = filesystem.stat(probe / ".cxxlens-root", context);
			const auto git_marker = filesystem.stat(probe / ".git", context);
			if ((root_marker && root_marker.value().exists) ||
				(git_marker && git_marker.value().exists))
			{
				project_boundary = probe;
				break;
			}
			if (probe == filesystem_root || probe.parent_path() == probe)
				break;
		}

		for (auto probe = current;; probe = probe.parent_path())
		{
			const auto candidate = probe / ".cxxlens.yaml";
			auto candidate_status = filesystem.stat(candidate, context);
			if (candidate_status && candidate_status.value().exists)
			{
				auto canonical_candidate = filesystem.canonicalize(candidate, context);
				auto canonical_boundary = filesystem.canonicalize(project_boundary, context);
				if (!canonical_candidate || !canonical_boundary ||
					!runtime::is_within_root(canonical_boundary.value(),
											 canonical_candidate.value()))
					return config_error("config.yaml-invalid", "symlink-escape");
				return load_document(canonical_candidate.value(), filesystem, environment);
			}
			if (probe == project_boundary || probe.parent_path() == probe)
				break;
		}
		return config_error("config.file-not-found", "nearest-config-not-found");
	}
} // namespace cxxlens::detail::config
