#include "gcc_auxiliary_capture_internal.hpp"

#include <algorithm>
#include <cstdint>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <utility>

#include "gcc_capture_file_port_internal.hpp"

namespace cxxlens::sdk::detail
{
	namespace
	{
		constexpr std::size_t gcc_16_2_maximum_response_expansions{1999U};
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

		[[nodiscard]] bool ascii_space(const unsigned char value) noexcept
		{
			return value == ' ' || value == '\t' || value == '\n' || value == '\r' ||
				value == '\f' || value == '\v';
		}

		[[nodiscard]] bool at_or_below(const std::string_view path,
									   const std::string_view root) noexcept
		{
			return path == root ||
				(path.size() > root.size() && path.starts_with(root) &&
				 (root == "/" || path[root.size()] == '/'));
		}

		struct path_resolution_context
		{
			std::string_view directory;
			std::string_view field;
		};

		[[nodiscard]] result<std::string> resolve_path(const std::string_view value,
													   const path_resolution_context context)
		{
			const auto [directory, field] = context;
			if (value.empty() || value.contains('\0') || value.contains('\\') ||
				directory.empty() || !directory.starts_with('/'))
				return unexpected(invalid(std::string{field}, "posix-path-required"));
			std::vector<std::string_view> segments;
			const auto append = [&](const std::string_view path,
									const std::size_t begin) -> result<void>
			{
				std::size_t offset{begin};
				while (offset < path.size())
				{
					const auto next = path.find('/', offset);
					const auto segment = path.substr(
						offset,
						next == std::string_view::npos ? path.size() - offset : next - offset);
					if (segment.empty())
						return unexpected(invalid(std::string{field}, "empty-path-segment"));
					if (segment == "..")
					{
						if (segments.empty())
							return unexpected(invalid(std::string{field}, "path-escapes-root"));
						segments.pop_back();
					}
					else if (segment != ".")
						segments.push_back(segment);
					if (next == std::string_view::npos)
						break;
					offset = next + 1U;
				}
				return {};
			};
			if (!value.starts_with('/'))
				if (auto added = append(directory, 1U); !added)
					return unexpected(std::move(added.error()));
			if (auto added = append(value, value.starts_with('/') ? 1U : 0U); !added)
				return unexpected(std::move(added.error()));
			std::string output{"/"};
			for (std::size_t index{}; index < segments.size(); ++index)
			{
				if (index != 0U)
					output.push_back('/');
				output += segments[index];
			}
			return output;
		}

		[[nodiscard]] std::string logical_path_for(const std::string_view physical,
												   const std::string_view root)
		{
			if (physical == root)
				return "project://";
			const auto offset = root == "/" ? 1U : root.size() + 1U;
			return "project://" + std::string{physical.substr(offset)};
		}

		[[nodiscard]] std::string source_encoding(const std::vector<std::byte>& content)
		{
			const auto text =
				std::string_view{reinterpret_cast<const char*>(content.data()), content.size()};
			return validate_utf8_text(text) ? "utf8" : "binary_or_unknown";
		}

		[[nodiscard]] bool has_spec_include(const std::vector<std::byte>& content) noexcept
		{
			const auto text =
				std::string_view{reinterpret_cast<const char*>(content.data()), content.size()};
			std::size_t offset{};
			while (offset < text.size())
			{
				const auto end = text.find('\n', offset);
				auto line = text.substr(
					offset, end == std::string_view::npos ? text.size() - offset : end - offset);
				while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
					line.remove_prefix(1U);
				if (line.starts_with("%include ") || line.starts_with("%include\t") ||
					line.starts_with("%include_noerr ") || line.starts_with("%include_noerr\t"))
					return true;
				if (end == std::string_view::npos)
					break;
				offset = end + 1U;
			}
			return false;
		}

		class auxiliary_collector
		{
		  public:
			struct context
			{
				std::string_view working_directory;
				std::string_view project_root;
			};

			auxiliary_collector(gcc_capture_file_port& files,
								const context path_context,
								const std::uint64_t maximum_capture_bytes,
								const import_limits limits)
				: files_{files}, working_directory_{path_context.working_directory},
				  project_root_{path_context.project_root}, remaining_bytes_{maximum_capture_bytes},
				  limits_{limits}
			{
			}

			[[nodiscard]] result<gcc_auxiliary_capture>
			capture(const std::span<const std::string> arguments)
			{
				auto expanded = expand(arguments, std::nullopt, 0U);
				if (!expanded)
					return unexpected(std::move(expanded.error()));
				if (auto captured = capture_specs(*expanded); !captured)
					return unexpected(std::move(captured.error()));

				gcc_auxiliary_capture output;
				if (saw_response_)
					output.invocation.response_files =
						captured_value<std::vector<build_capture_auxiliary_file>>::observed(
							std::move(response_files_));
				if (saw_specs_ && !spec_include_unresolved_)
					output.invocation.config_files =
						captured_value<std::vector<build_capture_auxiliary_file>>::observed(
							std::move(config_files_));
				else if (spec_include_unresolved_)
					output.invocation.config_files =
						captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
							"spec-include-search-unobserved",
							"recapture-with-shell-free-wrapper-and-toolchain-search-paths");
				output.closure_members = std::move(closure_members_);
				output.captured_bytes = captured_bytes_;
				return output;
			}

		  private:
			[[nodiscard]] result<std::vector<std::string>>
			expand(const std::span<const std::string> arguments,
				   const std::optional<std::size_t> parent,
				   const std::size_t depth)
			{
				std::vector<std::string> output;
				for (const auto& argument : arguments)
				{
					if (!argument.starts_with('@'))
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}
					saw_response_ = true;
					if (++response_expansions_ > gcc_16_2_maximum_response_expansions)
						return unexpected(limit("response_files", "gcc-expansion-count"));
					if (argument.size() == 1U)
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}
					if (depth >= limits_.maximum_nesting_depth)
						return unexpected(limit("response_files", "depth"));
					auto path = resolve_path(
						std::string_view{argument}.substr(1U),
						{.directory = working_directory_, .field = "response_file.path"});
					if (!path)
						return unexpected(std::move(path.error()));
					if (!at_or_below(*path, project_root_))
						return unexpected(
							invalid("response_file.path", "path-outside-project-root"));
					if (active_response_paths_.contains(*path))
						return unexpected(invalid("response_files", "recursive-reference"));
					if (const auto found = response_cache_.find(*path);
						found != response_cache_.end())
					{
						auto nested =
							expand(found->second.arguments, found->second.index, depth + 1U);
						if (!nested)
							return unexpected(std::move(nested.error()));
						if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
							return unexpected(limit("response_file.arguments", "count"));
						output.insert(output.end(), nested->begin(), nested->end());
						continue;
					}
					if (unavailable_response_paths_.contains(*path))
					{
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}

					if (response_files_.size() >= limits_.maximum_auxiliary_files_per_unit)
						return unexpected(limit("response_files", "count"));
					auto snapshot = files_.read_regular_file(
						*path, {remaining_bytes_, limits_.maximum_string_bytes, project_root_});
					if (!snapshot)
					{
						if (snapshot.error().code != capture_file_unavailable_code)
							return unexpected(std::move(snapshot.error()));
						const auto logical = logical_path_for(*path, project_root_);
						response_files_.push_back(
							{logical,
							 captured_value<std::string>::unavailable(
								 "response-file-unreadable", "restore-response-file-and-recapture"),
							 0U,
							 parent});
						unavailable_response_paths_.emplace(*path);
						if (output.size() >= limits_.maximum_arguments_per_unit)
							return unexpected(limit("response_file.arguments", "count"));
						output.push_back(argument);
						continue;
					}
					if (!at_or_below(snapshot->canonical_path, project_root_))
						return unexpected(
							invalid("response_file.path", "path-outside-project-root"));
					if (active_response_paths_.contains(snapshot->canonical_path))
						return unexpected(invalid("response_files", "recursive-reference"));
					if (const auto found = response_cache_.find(snapshot->canonical_path);
						found != response_cache_.end())
					{
						response_cache_.emplace(*path, found->second);
						auto nested =
							expand(found->second.arguments, found->second.index, depth + 1U);
						if (!nested)
							return unexpected(std::move(nested.error()));
						if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
							return unexpected(limit("response_file.arguments", "count"));
						output.insert(output.end(), nested->begin(), nested->end());
						continue;
					}
					if (snapshot->content.size() > remaining_bytes_)
						return unexpected(limit("response_file.content", "byte-count"));
					remaining_bytes_ -= static_cast<std::uint64_t>(snapshot->content.size());
					captured_bytes_ += static_cast<std::uint64_t>(snapshot->content.size());
					auto parsed = parse_gcc_16_2_response_arguments(snapshot->content, limits_);
					if (!parsed)
						return unexpected(std::move(parsed.error()));
					const auto digest = content_digest(snapshot->content);
					const auto logical = logical_path_for(snapshot->canonical_path, project_root_);
					const auto response_index = response_files_.size();
					response_files_.push_back({logical,
											   captured_value<std::string>::observed(digest),
											   static_cast<std::uint64_t>(snapshot->content.size()),
											   parent});
					const auto encoding = source_encoding(snapshot->content);
					closure_member_paths_.emplace(logical);
					closure_members_.push_back({snapshot->canonical_path,
												std::move(snapshot->content),
												encoding,
												"generated"});
					const auto cached = cached_response{response_index, *parsed};
					response_cache_.emplace(snapshot->canonical_path, cached);
					response_cache_.emplace(*path, cached);
					active_response_paths_.emplace(snapshot->canonical_path);
					auto nested = expand(*parsed, response_index, depth + 1U);
					active_response_paths_.erase(snapshot->canonical_path);
					if (!nested)
						return unexpected(std::move(nested.error()));
					if (nested->size() > limits_.maximum_arguments_per_unit - output.size())
						return unexpected(limit("response_file.arguments", "count"));
					output.insert(output.end(), nested->begin(), nested->end());
				}
				return output;
			}

			[[nodiscard]] result<void> capture_specs(const std::span<const std::string> arguments)
			{
				for (std::size_t index{}; index < arguments.size(); ++index)
				{
					std::string_view path;
					if (arguments[index].starts_with("-specs=") && arguments[index].size() > 7U)
						path = std::string_view{arguments[index]}.substr(7U);
					else if (arguments[index].starts_with("--specs=") &&
							 arguments[index].size() > 8U)
						path = std::string_view{arguments[index]}.substr(8U);
					else if (arguments[index] == "--specs" && index + 1U < arguments.size())
						path = arguments[++index];
					else
						continue;
					saw_specs_ = true;
					auto captured = capture_spec(path);
					if (!captured)
						return captured;
				}
				return {};
			}

			[[nodiscard]] result<void> capture_spec(const std::string_view value)
			{
				auto path = resolve_path(
					value, {.directory = working_directory_, .field = "config_file.path"});
				if (!path)
					return unexpected(std::move(path.error()));
				if (!at_or_below(*path, project_root_))
					return unexpected(invalid("config_file.path", "path-outside-project-root"));
				if (captured_spec_paths_.contains(*path))
					return {};
				if (config_files_.size() >= limits_.maximum_auxiliary_files_per_unit)
					return unexpected(limit("config_files", "count"));
				auto snapshot = files_.read_regular_file(
					*path, {remaining_bytes_, limits_.maximum_string_bytes, project_root_});
				if (!snapshot)
				{
					if (snapshot.error().code != capture_file_unavailable_code)
						return unexpected(std::move(snapshot.error()));
					config_files_.push_back(
						{logical_path_for(*path, project_root_),
						 captured_value<std::string>::unavailable(
							 "config-file-unreadable", "restore-config-file-and-recapture"),
						 0U,
						 std::nullopt});
					captured_spec_paths_.emplace(*path);
					return {};
				}
				if (!at_or_below(snapshot->canonical_path, project_root_))
					return unexpected(invalid("config_file.path", "path-outside-project-root"));
				if (snapshot->content.size() > remaining_bytes_)
					return unexpected(limit("config_file.content", "byte-count"));
				remaining_bytes_ -= static_cast<std::uint64_t>(snapshot->content.size());
				captured_bytes_ += static_cast<std::uint64_t>(snapshot->content.size());
				const auto digest = content_digest(snapshot->content);
				const auto logical = logical_path_for(snapshot->canonical_path, project_root_);
				config_files_.push_back({logical,
										 captured_value<std::string>::observed(digest),
										 static_cast<std::uint64_t>(snapshot->content.size()),
										 std::nullopt});
				if (has_spec_include(snapshot->content))
					spec_include_unresolved_ = true;
				const auto encoding = source_encoding(snapshot->content);
				if (closure_member_paths_.emplace(logical).second)
					closure_members_.push_back({snapshot->canonical_path,
												std::move(snapshot->content),
												encoding,
												"generated"});
				captured_spec_paths_.emplace(snapshot->canonical_path);
				return {};
			}

			struct cached_response
			{
				std::size_t index{};
				std::vector<std::string> arguments;
			};

			gcc_capture_file_port& files_;
			std::string working_directory_;
			std::string project_root_;
			std::uint64_t remaining_bytes_{};
			import_limits limits_;
			std::size_t response_expansions_{};
			std::uint64_t captured_bytes_{};
			bool saw_response_{};
			bool saw_specs_{};
			bool spec_include_unresolved_{};
			std::vector<build_capture_auxiliary_file> response_files_;
			std::vector<build_capture_auxiliary_file> config_files_;
			std::vector<gcc_source_closure_member_observation> closure_members_;
			std::map<std::string, cached_response, std::less<>> response_cache_;
			std::set<std::string, std::less<>> unavailable_response_paths_;
			std::set<std::string, std::less<>> active_response_paths_;
			std::set<std::string, std::less<>> captured_spec_paths_;
			std::set<std::string, std::less<>> closure_member_paths_;
		};
	} // namespace

	result<std::vector<std::string>>
	parse_gcc_16_2_response_arguments(const std::span<const std::byte> content,
									  const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		try
		{
			const auto nul = std::ranges::find(content, std::byte{});
			const auto length =
				static_cast<std::size_t>(std::ranges::distance(content.begin(), nul));
			const auto input =
				std::string_view{reinterpret_cast<const char*>(content.data()), length};
			std::vector<std::string> output;
			std::size_t offset{};
			std::size_t metadata_bytes{};
			while (offset < input.size())
			{
				while (offset < input.size() &&
					   ascii_space(static_cast<unsigned char>(input[offset])))
					++offset;
				if (offset == input.size())
					break;
				if (output.size() >= limits.maximum_arguments_per_unit)
					return unexpected(limit("response_file.arguments", "count"));
				std::string argument;
				bool single_quote{};
				bool double_quote{};
				bool backslash_quote{};
				while (offset < input.size())
				{
					const auto byte = input[offset];
					if (ascii_space(static_cast<unsigned char>(byte)) && !single_quote &&
						!double_quote && !backslash_quote)
						break;
					if (backslash_quote)
					{
						backslash_quote = false;
						if (byte != '\n')
							argument.push_back(byte);
					}
					else if (byte == '\\' && !single_quote &&
							 (!double_quote ||
							  (offset + 1U < input.size() &&
							   std::string_view{"$`\"\\\n"}.contains(input[offset + 1U]))))
						backslash_quote = true;
					else if (single_quote)
					{
						if (byte == '\'')
							single_quote = false;
						else
							argument.push_back(byte);
					}
					else if (double_quote)
					{
						if (byte == '"')
							double_quote = false;
						else
							argument.push_back(byte);
					}
					else if (byte == '\'')
						single_quote = true;
					else if (byte == '"')
						double_quote = true;
					else
						argument.push_back(byte);
					if (argument.size() > limits.maximum_string_bytes)
						return unexpected(limit("response_file.argument", "string-bytes"));
					++offset;
				}
				if (argument.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
					return unexpected(limit("response_file.arguments", "metadata-bytes"));
				metadata_bytes += argument.size();
				output.push_back(std::move(argument));
			}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("response_file", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("response_file", "allocation-length"));
		}
	}

	result<gcc_auxiliary_capture>
	capture_gcc_auxiliary_files(gcc_capture_file_port& files,
								const std::span<const std::string> arguments,
								const std::string_view canonical_working_directory,
								const std::string_view canonical_project_root,
								const std::uint64_t maximum_capture_bytes,
								const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		try
		{
			if (!at_or_below(canonical_working_directory, canonical_project_root))
				return unexpected(
					invalid("capture.working_directory", "path-outside-project-root"));
			return auxiliary_collector{files,
									   {.working_directory = canonical_working_directory,
										.project_root = canonical_project_root},
									   maximum_capture_bytes,
									   limits}
				.capture(arguments);
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("auxiliary_capture", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("auxiliary_capture", "allocation-length"));
		}
	}
} // namespace cxxlens::sdk::detail
