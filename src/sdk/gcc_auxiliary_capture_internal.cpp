#include "gcc_auxiliary_capture_internal.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstdint>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include "gcc_capture_file_port_internal.hpp"

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

		struct path_list_projection
		{
			std::string value;
			bool machine_local{};
			bool unavailable{};
		};

		[[nodiscard]] result<path_list_projection>
		path_list_value(gcc_capture_file_port& files,
						const std::string_view value,
						const gcc_environment_capture_request& request,
						const import_limits& limits,
						const bool split_paths)
		{
			path_list_projection output;
			std::vector<std::string> logical_paths;
			std::size_t offset{};
			for (;;)
			{
				const auto separator =
					split_paths ? value.find(':', offset) : std::string_view::npos;
				const auto component =
					value.substr(offset,
								 separator == std::string_view::npos ? value.size() - offset
																	 : separator - offset);
				auto resolved = component.empty()
					? result<std::string>{std::string{request.canonical_working_directory}}
					: resolve_path(
						  component,
						  {request.canonical_working_directory, "capture.environment.path"});
				if (!resolved)
				{
					output.unavailable = true;
					return output;
				}
				auto canonical =
					files.canonical_directory(*resolved, request.maximum_canonical_path_bytes);
				if (!canonical)
				{
					output.unavailable = true;
					return output;
				}
				if (!at_or_below(*canonical, request.canonical_project_root))
				{
					output.machine_local = true;
					return output;
				}
				logical_paths.push_back(
					logical_path_for(*canonical, request.canonical_project_root));
				if (logical_paths.size() > limits.maximum_path_mappings)
					return unexpected(limit("capture.environment.path-list", "count"));
				if (separator == std::string_view::npos)
					break;
				offset = separator + 1U;
			}

			output.value = "path-list-v1";
			if (output.value.size() > limits.maximum_string_bytes)
				return unexpected(limit("capture.environment.path-list", "string-bytes"));
			const auto append = [&](const std::string_view part) -> bool
			{
				if (part.size() > limits.maximum_string_bytes - output.value.size())
					return false;
				output.value.append(part);
				return true;
			};
			for (const auto& path : logical_paths)
			{
				const auto length = std::to_string(path.size());
				if (!append("|") || !append(length) || !append(":") || !append(path))
					return unexpected(limit("capture.environment.path-list", "string-bytes"));
			}
			return output;
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
				std::string_view main_source;
			};

			auxiliary_collector(gcc_capture_file_port& files,
								const context path_context,
								const std::uint64_t maximum_capture_bytes,
								const import_limits limits,
								const bool dependency_output_bound_to_invocation,
								const bool strict_response_files = false)
				: files_{files}, working_directory_{path_context.working_directory},
				  project_root_{path_context.project_root}, main_source_{path_context.main_source},
				  remaining_bytes_{maximum_capture_bytes}, limits_{limits},
				  dependency_output_bound_to_invocation_{dependency_output_bound_to_invocation},
				  strict_response_files_{strict_response_files}
			{
			}

			[[nodiscard]] result<gcc_prepared_auxiliary_capture>
			prepare_responses(const std::span<const std::string> arguments)
			{
				auto expanded = expand(arguments, std::nullopt, 0U);
				if (!expanded)
					return unexpected(std::move(expanded.error()));
				return gcc_prepared_auxiliary_capture{std::move(*expanded),
													  std::move(response_files_),
													  {},
													  std::move(closure_members_),
													  captured_bytes_};
			}

			[[nodiscard]] result<gcc_prepared_auxiliary_capture>
			prepare_specs(gcc_capture_workspace& workspace, gcc_prepared_auxiliary_capture prepared)
			{
				auto execution_arguments = prepared.expanded_arguments;
				if (auto seeded = seed(execution_arguments, std::move(prepared)); !seeded)
					return unexpected(std::move(seeded.error()));
				for (std::size_t index{}; index < execution_arguments.size(); ++index)
				{
					std::string_view path;
					enum class spelling : std::uint8_t
					{
						single_dash,
						double_dash,
						separate
					};
					std::optional<spelling> form;
					if (execution_arguments[index].starts_with("-specs=") &&
						execution_arguments[index].size() > 7U)
					{
						path = std::string_view{execution_arguments[index]}.substr(7U);
						form = spelling::single_dash;
					}
					else if (execution_arguments[index].starts_with("--specs=") &&
							 execution_arguments[index].size() > 8U)
					{
						path = std::string_view{execution_arguments[index]}.substr(8U);
						form = spelling::double_dash;
					}
					else if (execution_arguments[index] == "--specs")
					{
						if (index + 1U >= execution_arguments.size())
							return unexpected(
								invalid("config_file.path", "missing-option-argument"));
						path = execution_arguments[index + 1U];
						form = spelling::separate;
					}
					else if (execution_arguments[index] == "-specs" ||
							 execution_arguments[index] == "-specs=" ||
							 execution_arguments[index] == "--specs=")
						return unexpected(invalid("config_file.path", "missing-option-argument"));
					if (!form)
						continue;
					auto staged = capture_spec(path, &workspace, config_files_.size());
					if (!staged)
						return unexpected(std::move(staged.error()));
					if (!*staged)
						return unexpected(invalid("config_file.path", "staging-failed"));
					if ((**staged).empty() || !(**staged).starts_with('/') ||
						(**staged).contains('\0'))
						return unexpected(
							invalid("config_file.staged_path", "absolute-path-required"));
					if ((**staged).size() > limits_.maximum_string_bytes)
						return unexpected(limit("config_file.staged_path", "string-bytes"));
					if (*form == spelling::single_dash)
						execution_arguments[index] = "-specs=" + **staged;
					else if (*form == spelling::double_dash)
						execution_arguments[index] = "--specs=" + **staged;
					else
						execution_arguments[++index] = std::move(**staged);
				}
				return gcc_prepared_auxiliary_capture{std::move(execution_arguments),
													  std::move(response_files_),
													  std::move(config_files_),
													  std::move(closure_members_),
													  captured_bytes_};
			}

			[[nodiscard]] result<gcc_auxiliary_capture>
			capture(const std::span<const std::string> arguments,
					gcc_prepared_auxiliary_capture prepared_auxiliary = {})
			{
				if (auto seeded = seed(arguments, std::move(prepared_auxiliary)); !seeded)
					return unexpected(std::move(seeded.error()));
				auto expanded = expand(arguments, std::nullopt, 0U);
				if (!expanded)
					return unexpected(std::move(expanded.error()));
				if (!specs_prepared_)
					if (auto captured = capture_specs(*expanded); !captured)
						return unexpected(std::move(captured.error()));
				if (auto captured = capture_dependency_output(*expanded); !captured)
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
				if (saw_dependency_output_)
					output.invocation.source_closure_membership = std::move(dependency_membership_);
				output.closure_members = std::move(closure_members_);
				output.captured_bytes = captured_bytes_;
				return output;
			}

		  private:
			[[nodiscard]] result<void> seed(const std::span<const std::string> arguments,
											gcc_prepared_auxiliary_capture prepared)
			{
				const bool has_prepared_capture = !prepared.expanded_arguments.empty() ||
					!prepared.response_files.empty() || !prepared.config_files.empty() ||
					!prepared.closure_members.empty() || prepared.captured_bytes != 0U;
				if (has_prepared_capture &&
					!std::ranges::equal(arguments, prepared.expanded_arguments))
					return unexpected(invalid("response_files", "execution-binding-mismatch"));
				if (prepared.captured_bytes > remaining_bytes_)
					return unexpected(limit("response_file.content", "byte-count"));
				remaining_bytes_ -= prepared.captured_bytes;
				captured_bytes_ = prepared.captured_bytes;
				response_files_ = std::move(prepared.response_files);
				saw_response_ = !response_files_.empty();
				config_files_ = std::move(prepared.config_files);
				saw_specs_ = !config_files_.empty();
				specs_prepared_ = saw_specs_;
				closure_members_ = std::move(prepared.closure_members);
				for (const auto& member : closure_members_)
					closure_member_paths_.emplace(
						logical_path_for(member.canonical_path, project_root_));
				return {};
			}

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
						if (strict_response_files_)
							return unexpected(
								invalid("response_file.path", "unreadable-before-execution"));
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
						return unexpected(std::move(captured.error()));
				}
				return {};
			}

			[[nodiscard]] result<std::optional<std::string>>
			capture_spec(const std::string_view value,
						 gcc_capture_workspace* workspace = nullptr,
						 const std::size_t staging_index = 0U)
			{
				auto path = resolve_path(
					value, {.directory = working_directory_, .field = "config_file.path"});
				if (!path)
					return unexpected(std::move(path.error()));
				if (!at_or_below(*path, project_root_))
					return unexpected(invalid("config_file.path", "path-outside-project-root"));
				const auto requested_path = *path;
				if (captured_spec_paths_.contains(*path))
				{
					if (workspace != nullptr)
					{
						const auto existing = staged_spec_paths_.find(*path);
						if (existing == staged_spec_paths_.end())
							return unexpected(
								invalid("config_file.path", "staging-binding-missing"));
						return std::optional<std::string>{existing->second};
					}
					return std::optional<std::string>{};
				}
				if (config_files_.size() >= limits_.maximum_auxiliary_files_per_unit)
					return unexpected(limit("config_files", "count"));
				auto snapshot = files_.read_regular_file(
					*path, {remaining_bytes_, limits_.maximum_string_bytes, project_root_});
				if (!snapshot)
				{
					if (snapshot.error().code != capture_file_unavailable_code)
						return unexpected(std::move(snapshot.error()));
					if (workspace != nullptr)
						return unexpected(
							invalid("config_file.path", "unreadable-before-execution"));
					config_files_.push_back(
						{logical_path_for(*path, project_root_),
						 captured_value<std::string>::unavailable(
							 "config-file-unreadable", "restore-config-file-and-recapture"),
						 0U,
						 std::nullopt});
					captured_spec_paths_.emplace(*path);
					return std::optional<std::string>{};
				}
				if (!at_or_below(snapshot->canonical_path, project_root_))
					return unexpected(invalid("config_file.path", "path-outside-project-root"));
				if (captured_spec_paths_.contains(snapshot->canonical_path))
				{
					captured_spec_paths_.emplace(requested_path);
					if (workspace != nullptr)
					{
						const auto existing = staged_spec_paths_.find(snapshot->canonical_path);
						if (existing == staged_spec_paths_.end())
							return unexpected(
								invalid("config_file.path", "staging-binding-missing"));
						staged_spec_paths_.emplace(requested_path, existing->second);
						return std::optional<std::string>{existing->second};
					}
					return std::optional<std::string>{};
				}
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
				{
					if (workspace != nullptr)
						return unexpected(
							invalid("config_file.content", "include-staging-required"));
					spec_include_unresolved_ = true;
				}
				std::optional<std::string> staged;
				if (workspace != nullptr)
				{
					auto staged_path =
						workspace->stage_specification(snapshot->content, staging_index);
					if (!staged_path)
						return unexpected(std::move(staged_path.error()));
					staged = std::move(*staged_path);
					staged_spec_paths_.emplace(snapshot->canonical_path, *staged);
					staged_spec_paths_.emplace(requested_path, *staged);
				}
				const auto encoding = source_encoding(snapshot->content);
				if (closure_member_paths_.emplace(logical).second)
					closure_members_.push_back({snapshot->canonical_path,
												std::move(snapshot->content),
												encoding,
												"generated"});
				captured_spec_paths_.emplace(snapshot->canonical_path);
				captured_spec_paths_.emplace(requested_path);
				return staged;
			}

			[[nodiscard]] result<void>
			capture_dependency_output(const std::span<const std::string> arguments)
			{
				std::optional<std::string_view> dependency_path;
				for (std::size_t index{}; index < arguments.size(); ++index)
				{
					const auto& token = arguments[index];
					if (token == "-MF")
					{
						if (index + 1U >= arguments.size())
							return unexpected(
								invalid("dependency_output.path", "missing-option-argument"));
						dependency_path = arguments[++index];
					}
					else if (token.starts_with("-MF") && token.size() > 3U)
						dependency_path = std::string_view{token}.substr(3U);
				}
				if (!dependency_path)
					return {};

				saw_dependency_output_ = true;
				dependency_membership_ = captured_value<std::string>::unavailable(
					"dependency-output-unreadable", "restore-dependency-output-and-recapture");
				auto path = resolve_path(
					*dependency_path,
					{.directory = working_directory_, .field = "dependency_output.path"});
				if (!path)
					return unexpected(std::move(path.error()));
				if (!at_or_below(*path, project_root_))
				{
					dependency_membership_ = captured_value<std::string>::unavailable(
						"dependency-output-outside-project-root",
						"write-the-dependency-output-below-the-project-root-and-recapture");
					return {};
				}
				auto snapshot = files_.read_regular_file(
					*path, {remaining_bytes_, limits_.maximum_string_bytes, project_root_});
				if (!snapshot)
				{
					if (snapshot.error().code == capture_file_unavailable_code)
						return {};
					return unexpected(std::move(snapshot.error()));
				}
				if (snapshot->content.size() > remaining_bytes_)
					return unexpected(limit("dependency_output.content", "byte-count"));
				remaining_bytes_ -= static_cast<std::uint64_t>(snapshot->content.size());
				captured_bytes_ += static_cast<std::uint64_t>(snapshot->content.size());
				auto prerequisites = parse_gcc_16_2_dependency_output(snapshot->content, limits_);
				if (!prerequisites)
				{
					dependency_membership_ = captured_value<std::string>::unavailable(
						"dependency-output-invalid",
						"regenerate-the-GCC-dependency-output-and-recapture");
					return {};
				}

				std::optional<std::pair<std::string, std::string>> incomplete_member;
				for (const auto& prerequisite : *prerequisites)
				{
					auto member_path = resolve_path(
						prerequisite,
						{.directory = working_directory_, .field = "dependency_output.member"});
					if (!member_path)
					{
						if (!incomplete_member)
							incomplete_member = {"dependency-member-invalid",
												 "regenerate-the-dependency-output-and-recapture"};
						continue;
					}
					if (!at_or_below(*member_path, project_root_))
					{
						if (!incomplete_member)
							incomplete_member = {"dependency-member-outside-project-root",
												 "recapture-with-a-qualified-logical-read-root"};
						continue;
					}
					auto member = files_.read_regular_file(
						*member_path,
						{remaining_bytes_, limits_.maximum_string_bytes, project_root_});
					if (!member)
					{
						if (member.error().code == capture_file_unavailable_code)
						{
							if (!incomplete_member)
								incomplete_member = {"dependency-member-unreadable",
													 "restore-the-dependency-member-and-recapture"};
							continue;
						}
						return unexpected(std::move(member.error()));
					}
					if (member->canonical_path == main_source_)
						continue;
					const auto logical = logical_path_for(member->canonical_path, project_root_);
					if (!closure_member_paths_.emplace(logical).second)
						continue;
					if (member->content.size() > remaining_bytes_)
						return unexpected(limit("dependency_output.member", "byte-count"));
					remaining_bytes_ -= static_cast<std::uint64_t>(member->content.size());
					captured_bytes_ += static_cast<std::uint64_t>(member->content.size());
					const auto encoding = source_encoding(member->content);
					closure_members_.push_back({std::move(member->canonical_path),
												std::move(member->content),
												encoding,
												"header"});
				}
				if (!dependency_output_bound_to_invocation_)
					dependency_membership_ = captured_value<std::string>::unavailable(
						"dependency-output-not-bound-to-invocation",
						"recapture-with-shell-free-wrapper");
				else if (incomplete_member)
					dependency_membership_ = captured_value<std::string>::unavailable(
						std::move(incomplete_member->first), std::move(incomplete_member->second));
				else
					dependency_membership_ = captured_value<std::string>::observed("complete");
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
			std::string main_source_;
			std::uint64_t remaining_bytes_{};
			import_limits limits_;
			bool dependency_output_bound_to_invocation_{};
			bool strict_response_files_{};
			bool specs_prepared_{};
			std::size_t response_expansions_{};
			std::uint64_t captured_bytes_{};
			bool saw_response_{};
			bool saw_specs_{};
			bool spec_include_unresolved_{};
			bool saw_dependency_output_{};
			captured_value<std::string> dependency_membership_;
			std::vector<build_capture_auxiliary_file> response_files_;
			std::vector<build_capture_auxiliary_file> config_files_;
			std::map<std::string, std::string, std::less<>> staged_spec_paths_;
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

	result<std::vector<std::string>>
	parse_gcc_16_2_dependency_output(const std::span<const std::byte> content,
									 const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		try
		{
			if (std::ranges::find(content, std::byte{}) != content.end())
				return unexpected(invalid("dependency_output", "nul-byte"));
			const auto input =
				std::string_view{reinterpret_cast<const char*>(content.data()), content.size()};
			std::size_t colon{std::string_view::npos};
			bool escaped{};
			for (std::size_t index{}; index < input.size(); ++index)
			{
				const auto byte = input[index];
				if (escaped)
				{
					escaped = false;
					continue;
				}
				if (byte == '\\')
				{
					escaped = true;
					continue;
				}
				if (byte == ':')
				{
					colon = index;
					break;
				}
				if (byte == '\n' || byte == '\r')
					return unexpected(invalid("dependency_output", "target-rule"));
			}
			if (colon == std::string_view::npos)
				return unexpected(invalid("dependency_output", "target-rule"));

			std::vector<std::string> output;
			std::string token;
			std::size_t metadata_bytes{};
			const auto append = [&]() -> result<void>
			{
				if (token.empty())
					return {};
				if (output.size() >= limits.maximum_source_closure_members)
					return unexpected(limit("dependency_output.members", "count"));
				if (token.size() > limits.maximum_string_bytes)
					return unexpected(limit("dependency_output.member", "string-bytes"));
				if (token.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
					return unexpected(limit("dependency_output.members", "metadata-bytes"));
				metadata_bytes += token.size();
				output.push_back(std::move(token));
				token.clear();
				return {};
			};
			for (std::size_t index = colon + 1U; index < input.size(); ++index)
			{
				const auto byte = input[index];
				if (byte == '\\')
				{
					if (index + 1U >= input.size())
						return unexpected(invalid("dependency_output", "dangling-escape"));
					if (input[index + 1U] == '\n')
					{
						++index;
						continue;
					}
					if (input[index + 1U] == '\r' && index + 2U < input.size() &&
						input[index + 2U] == '\n')
					{
						index += 2U;
						continue;
					}
					token.push_back(input[++index]);
				}
				else if (byte == '$' && index + 1U < input.size() && input[index + 1U] == '$')
				{
					token.push_back('$');
					++index;
				}
				else if (byte == '#' || byte == '\n' || byte == '\r')
					break;
				else if (ascii_space(static_cast<unsigned char>(byte)))
				{
					if (auto added = append(); !added)
						return unexpected(std::move(added.error()));
				}
				else
					token.push_back(byte);
				if (token.size() > limits.maximum_string_bytes)
					return unexpected(limit("dependency_output.member", "string-bytes"));
			}
			if (auto added = append(); !added)
				return unexpected(std::move(added.error()));
			if (output.empty())
				return unexpected(invalid("dependency_output", "empty-prerequisites"));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("dependency_output", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("dependency_output", "allocation-length"));
		}
	}

	result<std::vector<build_capture_environment_effect>>
	capture_gcc_16_2_environment_effects(gcc_capture_file_port& files,
										 const gcc_environment_capture_request& request,
										 const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (request.maximum_environment_count == 0U || request.maximum_environment_bytes == 0U ||
			request.maximum_canonical_path_bytes == 0U)
			return unexpected(invalid("capture.environment.limits", "zero"));
		if (request.environment.size() > request.maximum_environment_count)
			return unexpected(limit("capture.environment", "count"));
		if ((request.language != "c" && request.language != "c++") ||
			request.canonical_working_directory.empty() ||
			!request.canonical_working_directory.starts_with('/') ||
			request.canonical_project_root.empty() ||
			!request.canonical_project_root.starts_with('/') ||
			!at_or_below(request.canonical_working_directory, request.canonical_project_root))
			return unexpected(invalid("capture.environment.context", "canonical-context"));
		try
		{
			std::map<std::string, std::string_view, std::less<>> environment;
			std::size_t environment_bytes{};
			for (const auto& entry : request.environment)
			{
				if (entry.contains('\0') ||
					entry.size() == std::numeric_limits<std::size_t>::max() ||
					entry.size() + 1U > request.maximum_environment_bytes - environment_bytes)
					return unexpected(limit("capture.environment", "bytes"));
				environment_bytes += entry.size() + 1U;
				const auto separator = entry.find('=');
				if (separator == 0U || separator == std::string::npos)
					return unexpected(invalid("capture.environment", "name-value"));
				auto [position, inserted] = environment.emplace(
					entry.substr(0U, separator), std::string_view{entry}.substr(separator + 1U));
				if (!inserted)
					return unexpected(invalid("capture.environment", "duplicate-name"));
			}

			struct path_effect_spec
			{
				std::string_view variable;
				std::string_view effect_name;
				bool split_paths{true};
			};
			std::vector<build_capture_environment_effect> output;
			const auto add_path_effect = [&](const path_effect_spec spec) -> result<void>
			{
				const auto found = environment.find(spec.variable);
				if (found == environment.end())
					return {};
				auto normalized =
					path_list_value(files, found->second, request, limits, spec.split_paths);
				if (!normalized)
					return unexpected(std::move(normalized.error()));
				build_capture_environment_effect effect;
				effect.name = spec.effect_name;
				if (normalized->machine_local)
					effect.semantic_value = captured_value<std::string>::redacted(
						"machine-local-environment-path",
						"provide-a-logical-toolchain-path-mapping");
				else if (normalized->unavailable)
					effect.semantic_value = captured_value<std::string>::unavailable(
						"environment-path-unavailable",
						"recapture-when-the-environment-path-exists");
				else
					effect.semantic_value =
						captured_value<std::string>::observed(std::move(normalized->value));
				output.push_back(std::move(effect));
				return {};
			};

			for (const auto spec : {path_effect_spec{"COMPILER_PATH", "gcc.compiler-path"},
									path_effect_spec{"CPATH", "gcc.cpath"},
									path_effect_spec{"GCC_EXEC_PREFIX", "gcc.exec-prefix", false},
									path_effect_spec{"LIBRARY_PATH", "gcc.library-path"}})
				if (auto added = add_path_effect(spec); !added)
					return unexpected(std::move(added.error()));
			if (request.language == "c")
			{
				if (auto added = add_path_effect({"C_INCLUDE_PATH", "gcc.c-include-path"}); !added)
					return unexpected(std::move(added.error()));
			}
			else if (auto added = add_path_effect({"CPLUS_INCLUDE_PATH", "gcc.cplus-include-path"});
					 !added)
				return unexpected(std::move(added.error()));

			const auto locale_value = [&]() -> std::optional<std::string_view>
			{
				for (const auto name : {std::string_view{"LC_ALL"},
										std::string_view{"LC_CTYPE"},
										std::string_view{"LANG"}})
				{
					const auto found = environment.find(name);
					if (found != environment.end() && !found->second.empty())
						return found->second;
				}
				return std::nullopt;
			}();
			if (locale_value)
			{
				if (locale_value->size() > limits.maximum_string_bytes)
					return unexpected(limit("capture.environment.locale", "string-bytes"));
				output.push_back(
					{"gcc.locale-ctype",
					 captured_value<std::string>::observed(std::string{*locale_value})});
			}

			if (const auto source_epoch = environment.find("SOURCE_DATE_EPOCH");
				source_epoch != environment.end())
			{
				std::uint64_t seconds{};
				const auto [end, parse_error] =
					std::from_chars(source_epoch->second.data(),
									source_epoch->second.data() + source_epoch->second.size(),
									seconds);
				if (parse_error != std::errc{} ||
					end != source_epoch->second.data() + source_epoch->second.size())
					output.push_back(
						{"gcc.source-date-epoch",
						 captured_value<std::string>::unavailable(
							 "source-date-epoch-invalid", "supply-a-bounded-decimal-epoch")});
				else
					output.push_back(
						{"gcc.source-date-epoch",
						 captured_value<std::string>::observed(std::to_string(seconds))});
			}

			std::ranges::sort(output, {}, &build_capture_environment_effect::name);
			if (output.size() > limits.maximum_environment_effects_per_unit)
				return unexpected(limit("capture.environment.effects", "count"));
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("capture.environment", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("capture.environment", "allocation-length"));
		}
	}

	result<gcc_invocation_plan> plan_gcc_16_2_invocation(const gcc_invocation_plan_request& request,
														 const import_limits limits)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		if (request.compiler_path.empty() || !request.compiler_path.starts_with('/') ||
			request.compiler_path.contains('\0') || request.expanded_arguments.empty() ||
			request.expanded_arguments.front() != request.compiler_path)
			return unexpected(invalid("invocation.compiler", "absolute-identity-mismatch"));
		if (request.expanded_arguments.size() > limits.maximum_arguments_per_unit)
			return unexpected(limit("invocation.arguments", "count"));
		try
		{
			std::size_t metadata_bytes{};
			for (const auto& argument : request.expanded_arguments)
			{
				if (argument.contains('\0'))
					return unexpected(invalid("invocation.argument", "nul-byte"));
				if (argument.size() > limits.maximum_string_bytes)
					return unexpected(limit("invocation.argument", "string-bytes"));
				if (argument.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
					return unexpected(limit("invocation.arguments", "metadata-bytes"));
				metadata_bytes += argument.size();
			}

			std::optional<std::string> source;
			std::optional<std::string> current_language;
			std::optional<std::string> source_language;
			std::optional<std::string> dependency_output;
			bool compile_only{};
			bool dependency_mode{};
			bool after_options{};
			const auto language_for = [](const std::string_view path) -> std::string_view
			{
				if (path.ends_with(".c") || path.ends_with(".i"))
					return "c";
				for (const auto extension : {std::string_view{".C"},
											 std::string_view{".cc"},
											 std::string_view{".cp"},
											 std::string_view{".cpp"},
											 std::string_view{".CPP"},
											 std::string_view{".c++"},
											 std::string_view{".cxx"},
											 std::string_view{".ii"}})
					if (path.ends_with(extension))
						return "c++";
				return {};
			};
			const auto option_has_separate_value = [](const std::string_view option) noexcept
			{
				return option == "-o" || option == "-include" || option == "-imacros" ||
					option == "-MT" || option == "-MQ" || option == "-I" || option == "-L" ||
					option == "-B" || option == "-D" || option == "-U" || option == "-A" ||
					option == "-isystem" || option == "-iquote" || option == "-idirafter" ||
					option == "-iprefix" || option == "-iwithprefix" ||
					option == "-iwithprefixbefore" || option == "-isysroot" ||
					option == "--sysroot" || option == "-specs" || option == "--specs" ||
					option == "-Xpreprocessor" || option == "-Xassembler" || option == "-Xlinker" ||
					option == "-aux-info" || option == "-dumpbase" || option == "-dumpdir" ||
					option == "-fplugin";
			};
			for (std::size_t index{1U}; index < request.expanded_arguments.size(); ++index)
			{
				const auto& token = request.expanded_arguments[index];
				if (token.starts_with('@'))
					return unexpected(invalid("invocation.arguments", "response-not-expanded"));
				if (!after_options && token == "--")
				{
					after_options = true;
					continue;
				}
				if (!after_options && token == "-c")
				{
					compile_only = true;
					continue;
				}
				if (!after_options &&
					(token == "-E" || token == "-S" || token == "-M" || token == "-MM" ||
					 token == "-fsyntax-only"))
					return unexpected(invalid("invocation.phase", "object-compile-required"));
				if (!after_options && (token == "-MD" || token == "-MMD"))
				{
					dependency_mode = true;
					continue;
				}
				if (!after_options && token.starts_with("-Wp,-M"))
					return unexpected(
						invalid("invocation.dependency_output", "preprocessor-option-unsupported"));
				if (!after_options && token == "-MF")
				{
					if (index + 1U >= request.expanded_arguments.size())
						return unexpected(invalid("invocation.dependency_output", "missing-value"));
					if (dependency_output)
						return unexpected(invalid("invocation.dependency_output", "duplicate"));
					dependency_output = request.expanded_arguments[++index];
					continue;
				}
				if (!after_options && token.starts_with("-MF") && token.size() > 3U)
				{
					if (dependency_output)
						return unexpected(invalid("invocation.dependency_output", "duplicate"));
					dependency_output = token.substr(3U);
					continue;
				}
				if (!after_options && token == "-x")
				{
					if (index + 1U >= request.expanded_arguments.size())
						return unexpected(invalid("invocation.language", "missing-value"));
					const auto& value = request.expanded_arguments[++index];
					if (value == "none")
						current_language.reset();
					else if (value == "c" || value == "c++")
						current_language = value;
					else
						return unexpected(invalid("invocation.language", "unsupported"));
					continue;
				}
				if (!after_options && token.starts_with("-x") && token.size() > 2U)
				{
					const auto value = std::string_view{token}.substr(2U);
					if (value == "none")
						current_language.reset();
					else if (value == "c" || value == "c++")
						current_language = value;
					else
						return unexpected(invalid("invocation.language", "unsupported"));
					continue;
				}
				if (!after_options && option_has_separate_value(token))
				{
					if (index + 1U >= request.expanded_arguments.size())
						return unexpected(invalid("invocation.option", "missing-value"));
					++index;
					continue;
				}
				if (!after_options && token.starts_with('-'))
				{
					continue;
				}
				const auto language = language_for(token);
				if (language.empty() && !current_language)
					continue;
				if (source)
					return unexpected(invalid("invocation.source", "multiple"));
				source = token;
				source_language = current_language ? *current_language : std::string{language};
			}
			if (!compile_only)
				return unexpected(invalid("invocation.phase", "compile-only-required"));
			if (!source || !source_language)
				return unexpected(invalid("invocation.source", "unique-c-or-cxx-source-required"));

			gcc_invocation_plan output;
			output.source_path = std::move(*source);
			output.language = std::move(*source_language);
			output.capture_arguments.assign(request.expanded_arguments.begin(),
											request.expanded_arguments.end());
			if (dependency_output)
			{
				if (dependency_output->empty())
					return unexpected(invalid("invocation.dependency_output", "empty-value"));
				output.dependency_output_path = std::move(*dependency_output);
				if (!dependency_mode)
				{
					output.capture_arguments.emplace_back("-MMD");
					output.dependency_arguments_injected = true;
				}
			}
			else
			{
				if (request.injected_dependency_output_path.empty() ||
					!request.injected_dependency_output_path.starts_with('/') ||
					request.injected_dependency_output_path.contains('\0'))
					return unexpected(
						invalid("invocation.dependency_output", "injection-path-required"));
				if (!dependency_mode)
					output.capture_arguments.emplace_back("-MMD");
				output.capture_arguments.emplace_back("-MF");
				output.capture_arguments.emplace_back(request.injected_dependency_output_path);
				output.dependency_output_path = request.injected_dependency_output_path;
				output.dependency_arguments_injected = true;
			}
			if (output.capture_arguments.size() > limits.maximum_arguments_per_unit)
				return unexpected(limit("invocation.capture_arguments", "count"));
			metadata_bytes = 0U;
			for (const auto& argument : output.capture_arguments)
			{
				if (argument.size() > limits.maximum_string_bytes)
					return unexpected(limit("invocation.capture_argument", "string-bytes"));
				if (argument.size() > limits.maximum_total_metadata_bytes - metadata_bytes)
					return unexpected(limit("invocation.capture_arguments", "metadata-bytes"));
				metadata_bytes += argument.size();
			}
			return output;
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("invocation.plan", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("invocation.plan", "allocation-length"));
		}
	}

	result<gcc_prepared_auxiliary_capture>
	prepare_gcc_16_2_response_files(gcc_capture_file_port& files,
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
										.project_root = canonical_project_root,
										.main_source = {}},
									   maximum_capture_bytes,
									   limits,
									   false,
									   true}
				.prepare_responses(arguments);
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

	result<gcc_prepared_auxiliary_capture>
	prepare_gcc_16_2_spec_files(gcc_capture_file_port& files,
								gcc_capture_workspace& workspace,
								gcc_prepared_auxiliary_capture prepared,
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
										.project_root = canonical_project_root,
										.main_source = {}},
									   maximum_capture_bytes,
									   limits,
									   false,
									   true}
				.prepare_specs(workspace, std::move(prepared));
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("config_file", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("config_file", "allocation-length"));
		}
	}

	result<gcc_auxiliary_capture>
	capture_gcc_auxiliary_files(gcc_capture_file_port& files,
								const std::span<const std::string> arguments,
								const std::string_view canonical_working_directory,
								const std::string_view canonical_project_root,
								const std::string_view canonical_main_source,
								const std::uint64_t maximum_capture_bytes,
								const import_limits limits,
								const bool dependency_output_bound_to_invocation,
								gcc_prepared_auxiliary_capture prepared_auxiliary)
	{
		if (auto valid = limits.validate(); !valid)
			return unexpected(std::move(valid.error()));
		try
		{
			if (!at_or_below(canonical_working_directory, canonical_project_root))
				return unexpected(
					invalid("capture.working_directory", "path-outside-project-root"));
			if (!at_or_below(canonical_main_source, canonical_project_root))
				return unexpected(invalid("capture.main_source", "path-outside-project-root"));
			return auxiliary_collector{files,
									   {.working_directory = canonical_working_directory,
										.project_root = canonical_project_root,
										.main_source = canonical_main_source},
									   maximum_capture_bytes,
									   limits,
									   dependency_output_bound_to_invocation}
				.capture(arguments, std::move(prepared_auxiliary));
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
