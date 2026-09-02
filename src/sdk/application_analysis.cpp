#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <new>
#include <set>
#include <stdexcept>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/application_analysis.hpp>

#include "application_analysis_internal.hpp"

namespace cxxlens::sdk
{
	namespace
	{
		[[nodiscard]] error invalid(std::string field, std::string detail)
		{
			return {"application-analysis.capture-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] error limit(std::string field, std::string detail)
		{
			return {
				"application-analysis.import-limit-exceeded", std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool digest_like(const std::string_view value)
		{
			const auto marker = value.rfind("sha256:");
			if (marker == std::string_view::npos || marker + 7U + 64U != value.size())
				return false;
			return std::ranges::all_of(value.substr(marker + 7U),
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] result<std::uint64_t> read_length(const std::span<const std::byte> input,
														std::size_t& offset)
		{
			if (offset > input.size() || input.size() - offset < 8U)
				return unexpected(invalid("binary", "truncated-length"));
			std::uint64_t value{};
			for (std::size_t index{}; index < 8U; ++index)
				value = (value << 8U) | std::to_integer<unsigned char>(input[offset + index]);
			offset += 8U;
			return value;
		}

		[[nodiscard]] result<void> preflight_value(const std::span<const std::byte> input,
												   const std::size_t depth,
												   const import_limits& limits)
		{
			if (depth > limits.maximum_nesting_depth)
				return unexpected(limit("binary", "nesting-depth"));
			if (input.empty())
				return unexpected(invalid("binary", "missing-tag"));
			std::size_t offset{1U};
			switch (std::to_integer<unsigned char>(input.front()))
			{
				case 0x00U:
					break;
				case 0x01U:
					if (input.size() - offset != 1U)
						return unexpected(invalid("binary", "boolean-size"));
					++offset;
					break;
				case 0x02U:
				{
					if (offset == input.size())
						return unexpected(invalid("binary", "integer-sign"));
					++offset;
					auto width = read_length(input, offset);
					if (!width)
						return unexpected(std::move(width.error()));
					if (*width > input.size() - offset)
						return unexpected(invalid("binary", "integer-width"));
					offset += static_cast<std::size_t>(*width);
					break;
				}
				case 0x03U:
				case 0x04U:
				{
					auto size = read_length(input, offset);
					if (!size)
						return unexpected(std::move(size.error()));
					if (*size > input.size() - offset)
						return unexpected(invalid("binary", "payload-size"));
					offset += static_cast<std::size_t>(*size);
					break;
				}
				case 0x05U:
				{
					auto count = read_length(input, offset);
					if (!count)
						return unexpected(std::move(count.error()));
					if (*count > (input.size() - offset) / 9U)
						return unexpected(invalid("binary", "tuple-count"));
					for (std::uint64_t index{}; index < *count; ++index)
					{
						auto size = read_length(input, offset);
						if (!size)
							return unexpected(std::move(size.error()));
						if (*size == 0U || *size > input.size() - offset)
							return unexpected(invalid("binary", "tuple-item-size"));
						if (auto valid = preflight_value(
								input.subspan(offset, static_cast<std::size_t>(*size)),
								depth + 1U,
								limits);
							!valid)
							return valid;
						offset += static_cast<std::size_t>(*size);
					}
					break;
				}
				default:
					return unexpected(invalid("binary", "unknown-tag"));
			}
			if (offset != input.size())
				return unexpected(invalid("binary", "trailing-bytes"));
			return {};
		}

		class metadata_budget
		{
		  public:
			explicit metadata_budget(const import_limits& limits) : limits_{limits} {}

			[[nodiscard]] result<void> add(const std::string_view value, const std::string& field)
			{
				if (value.size() > limits_.maximum_string_bytes)
					return unexpected(limit(field, "string-bytes"));
				if (value.size() > limits_.maximum_total_metadata_bytes - total_)
					return unexpected(limit("metadata", "total-bytes"));
				total_ += value.size();
				return {};
			}

		  private:
			const import_limits& limits_;
			std::size_t total_{};
		};

		[[nodiscard]] result<void> account_strings(const canonical_value& value,
												   metadata_budget& budget,
												   const std::string& field)
		{
			if (value.type == canonical_value::kind::utf8_string)
				return budget.add(value.text, field);
			if (value.type == canonical_value::kind::ordered_tuple)
				for (std::size_t index{}; index < value.tuple.size(); ++index)
					if (auto valid = account_strings(
							value.tuple[index], budget, field + "[" + std::to_string(index) + "]");
						!valid)
						return valid;
			return {};
		}

		[[nodiscard]] result<const std::vector<canonical_value>*> require_tuple(
			const canonical_value& value, const std::string& field, const std::size_t size)
		{
			if (value.type != canonical_value::kind::ordered_tuple || value.tuple.size() != size)
				return unexpected(invalid(field, "tuple-shape"));
			return &value.tuple;
		}

		[[nodiscard]] result<std::string_view> require_text(const canonical_value& value,
															const std::string& field,
															const bool nonempty = true)
		{
			if (value.type != canonical_value::kind::utf8_string ||
				(nonempty && value.text.empty()))
				return unexpected(invalid(field, "string"));
			return std::string_view{value.text};
		}

		[[nodiscard]] result<std::uint64_t> require_count(const canonical_value& value,
														  const std::string& field)
		{
			if (value.type != canonical_value::kind::signed_integer || value.integer < 0)
				return unexpected(invalid(field, "nonnegative-integer"));
			return static_cast<std::uint64_t>(value.integer);
		}

		[[nodiscard]] result<void> require_digest(const canonical_value& value,
												  const std::string& field)
		{
			auto text = require_text(value, field);
			if (!text)
				return unexpected(std::move(text.error()));
			if (!digest_like(*text))
				return unexpected(invalid(field, "digest"));
			return {};
		}

		[[nodiscard]] bool logical_path(const std::string_view value)
		{
			return !value.empty() && value.front() != '/' && !value.contains('\\') &&
				(value.size() < 2U || value[1U] != ':');
		}

		[[nodiscard]] bool absolute_native_path(const std::string_view value,
												const bool windows_path)
		{
			if (!windows_path)
				return value.starts_with('/');
			return value.size() >= 3U &&
				((value.front() >= 'A' && value.front() <= 'Z') ||
				 (value.front() >= 'a' && value.front() <= 'z')) &&
				value[1U] == ':' && (value[2U] == '\\' || value[2U] == '/');
		}

		[[nodiscard]] bool canonical_native_path(const std::string_view value,
												 const bool windows_path)
		{
			if (!absolute_native_path(value, windows_path))
				return false;
			const char separator = windows_path ? '\\' : '/';
			if (windows_path)
			{
				if (value.front() < 'A' || value.front() > 'Z' || value.contains('/'))
					return false;
			}
			else if (value.contains('\\'))
				return false;
			for (std::size_t index{}; index < value.size(); ++index)
			{
				const auto byte = static_cast<unsigned char>(value[index]);
				if (byte <= 0x1fU || byte == 0x7fU ||
					(windows_path && index >= 2U &&
					 std::string_view{"<>:\"|?*"}.contains(value[index])))
					return false;
			}
			const std::size_t root_width = windows_path ? 3U : 1U;
			if (value.size() > root_width && value.back() == separator)
				return false;
			std::size_t offset = root_width;
			while (offset < value.size())
			{
				const auto next = value.find(separator, offset);
				const auto segment = value.substr(
					offset, next == std::string_view::npos ? value.size() - offset : next - offset);
				if (segment.empty() || segment == "." || segment == "..")
					return false;
				if (next == std::string_view::npos)
					break;
				offset = next + 1U;
			}
			return true;
		}

		[[nodiscard]] char ascii_path_fold(const char value) noexcept
		{
			return value >= 'A' && value <= 'Z' ? static_cast<char>(value - 'A' + 'a') : value;
		}

		[[nodiscard]] bool native_path_at_or_below(const std::string_view path,
												   const std::string_view root,
												   const bool windows_path)
		{
			if (path.size() < root.size())
				return false;
			for (std::size_t index{}; index < root.size(); ++index)
				if ((windows_path ? ascii_path_fold(path[index]) : path[index]) !=
					(windows_path ? ascii_path_fold(root[index]) : root[index]))
					return false;
			if (path.size() == root.size())
				return true;
			const char separator = windows_path ? '\\' : '/';
			return root.back() == separator || path[root.size()] == separator;
		}

		[[nodiscard]] bool path_at_or_below(const std::string_view path,
											const std::string_view root,
											const char separator)
		{
			if (path == root)
				return true;
			if (!path.starts_with(root))
				return false;
			return root.back() == separator || path[root.size()] == separator;
		}

		[[nodiscard]] bool logical_at_or_below(const std::string_view path,
											   const std::string_view root)
		{
			return path_at_or_below(path, root, '/');
		}

		[[nodiscard]] result<void> require_strong_id(const canonical_value& value,
													 const std::string& field)
		{
			auto text = require_text(value, field);
			if (!text)
				return unexpected(std::move(text.error()));
			if (auto valid = validate_strong_id(*text); !valid)
				return unexpected(invalid(field, "strong-id"));
			return {};
		}

		[[nodiscard]] result<void> validate_captured(const canonical_value& value,
													 const std::string& field,
													 const canonical_value::kind present_kind,
													 std::vector<capture_gap>& gaps)
		{
			auto tuple = require_tuple(value, field, 4U);
			if (!tuple)
				return unexpected(std::move(tuple.error()));
			auto state = require_text((*tuple.value())[0], field + ".state");
			auto reason = require_text((*tuple.value())[2], field + ".reason", false);
			auto action = require_text((*tuple.value())[3], field + ".completion_action", false);
			if (!state || !reason || !action)
				return unexpected(
					!state ? std::move(state.error())
						   : (!reason ? std::move(reason.error()) : std::move(action.error())));
			const bool present = *state == "observed" || *state == "derived";
			const bool absent = *state == "redacted" || *state == "unavailable";
			if ((!present && !absent) ||
				(present &&
				 ((*tuple.value())[1].type != present_kind || !reason->empty() ||
				  !action->empty())) ||
				(absent &&
				 ((*tuple.value())[1].type != canonical_value::kind::null_value ||
				  reason->empty() || action->empty())))
				return unexpected(invalid(field, "captured-value-shape"));
			if (absent)
				gaps.push_back(
					{field, std::string{*state}, std::string{*reason}, std::string{*action}});
			return {};
		}

		[[nodiscard]] result<void> validate_string_tuple(const canonical_value& value,
														 const std::string& field,
														 const std::size_t maximum)
		{
			if (value.type != canonical_value::kind::ordered_tuple || value.tuple.size() > maximum)
				return unexpected(limit(field, "count"));
			for (std::size_t index{}; index < value.tuple.size(); ++index)
				if (auto text = require_text(
						value.tuple[index], field + "[" + std::to_string(index) + "]", false);
					!text)
					return unexpected(std::move(text.error()));
			return {};
		}

		[[nodiscard]] result<std::vector<detail::decoded_capture_auxiliary_file>>
		decode_auxiliary_files(const canonical_value& captured,
							   const std::string& field,
							   const import_limits& limits,
							   const std::string_view logical_root,
							   std::vector<capture_gap>& gaps)
		{
			if (auto valid =
					validate_captured(captured, field, canonical_value::kind::ordered_tuple, gaps);
				!valid)
				return unexpected(std::move(valid.error()));
			const auto& tuple = captured.tuple[1];
			if (tuple.type == canonical_value::kind::null_value)
				return std::vector<detail::decoded_capture_auxiliary_file>{};
			if (tuple.tuple.size() > limits.maximum_auxiliary_files_per_unit)
				return unexpected(limit(field, "count"));
			std::set<std::string, std::less<>> paths;
			std::vector<std::optional<std::size_t>> parents;
			std::vector<detail::decoded_capture_auxiliary_file> output;
			parents.reserve(tuple.tuple.size());
			output.reserve(tuple.tuple.size());
			for (std::size_t index{}; index < tuple.tuple.size(); ++index)
			{
				const auto prefix = field + "[" + std::to_string(index) + "]";
				auto item = require_tuple(tuple.tuple[index], prefix, 4U);
				if (!item)
					return unexpected(std::move(item.error()));
				auto path = require_text((*item.value())[0], prefix + ".logical_path");
				if (!path || !logical_path(*path) || !logical_at_or_below(*path, logical_root) ||
					!paths.emplace(*path).second)
					return unexpected(
						path ? invalid(prefix + ".logical_path", "duplicate-or-machine-path")
							 : std::move(path.error()));
				if (auto valid = validate_captured((*item.value())[1],
												   prefix + ".content_digest",
												   canonical_value::kind::utf8_string,
												   gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*item.value())[1].tuple[1].type == canonical_value::kind::utf8_string &&
					!digest_like((*item.value())[1].tuple[1].text))
					return unexpected(invalid(prefix + ".content_digest", "digest"));
				auto size = require_count((*item.value())[2], prefix + ".size_bytes");
				if (!size)
					return unexpected(std::move(size.error()));
				const auto& parent = (*item.value())[3];
				std::optional<std::size_t> parent_value;
				if (parent.type != canonical_value::kind::null_value)
				{
					auto parent_index = require_count(parent, prefix + ".parent_index");
					if (!parent_index || *parent_index >= tuple.tuple.size())
						return unexpected(parent_index ? invalid(prefix + ".parent_index", "range")
													   : std::move(parent_index.error()));
					parent_value = static_cast<std::size_t>(*parent_index);
				}
				parents.push_back(parent_value);
				std::optional<std::string> content_digest;
				if ((*item.value())[1].tuple[1].type == canonical_value::kind::utf8_string)
					content_digest = (*item.value())[1].tuple[1].text;
				output.push_back(
					{std::string{*path}, std::move(content_digest), *size, parent_value});
			}
			for (std::size_t index{}; index < parents.size(); ++index)
			{
				std::set<std::size_t> visited;
				auto cursor = std::optional<std::size_t>{index};
				std::size_t depth{};
				while (cursor)
				{
					if (!visited.emplace(*cursor).second)
						return unexpected(invalid(field, "recursive-reference"));
					if (++depth > limits.maximum_nesting_depth)
						return unexpected(limit(field, "auxiliary-depth"));
					cursor = parents[*cursor];
				}
			}
			return output;
		}

		struct validated_bundle_projection
		{
			std::string production_compiler;
			std::string capture_adapter;
			std::string target_abi;
			std::string project_id;
			std::string logical_project_root;
			std::size_t compile_unit_count{};
			std::vector<capture_gap> gaps;
			detail::decoded_capture_projection decoded;
		};

		struct compile_source_binding
		{
			std::optional<std::string> source_snapshot_id;
			std::string file_id;
			std::string logical_path;
			std::string content_digest;
			std::uint64_t size_bytes{};
			std::string source_closure_id;
		};

		[[nodiscard]] result<std::string>
		application_source_file_id(const std::string_view logical_path)
		{
			constexpr std::string_view root{"project://"};
			if (!logical_path.starts_with(root) || logical_path.size() == root.size())
				return unexpected(invalid("source.logical_path", "project-relative"));
			const std::array fields{
				canonical_value::from_string("project"),
				canonical_value::from_string(std::string{logical_path.substr(root.size())}),
				canonical_value::from_string("cxxlens.logical-path.v1"),
			};
			return canonical_identity_digest("file", fields);
		}

		[[nodiscard]] result<std::string>
		application_source_snapshot_id(const std::string_view file_id,
									   const std::string_view content,
									   const std::string_view encoding)
		{
			const std::array fields{
				canonical_value::from_string(std::string{file_id}),
				canonical_value::from_string(std::string{content}),
				canonical_value::from_string(std::string{encoding}),
			};
			return canonical_identity_digest("source-snapshot", fields);
		}

		[[nodiscard]] bool source_role(const std::string_view value) noexcept
		{
			return value == "main" || value == "header" || value == "generated" ||
				value == "forced-include" || value == "macro-file";
		}

		[[nodiscard]] bool source_encoding(const std::string_view value) noexcept
		{
			return value == "utf8" || value == "utf16le" || value == "utf16be" ||
				value == "locale_dependent" || value == "binary_or_unknown";
		}

		[[nodiscard]] result<validated_bundle_projection>
		validate_bundle_shape(const canonical_value& root, const import_limits& limits)
		{
			validated_bundle_projection output;
			auto tuple = require_tuple(root, "root", 10U);
			if (!tuple)
				return unexpected(std::move(tuple.error()));
			auto schema = require_text((*tuple.value())[0], "schema");
			if (!schema || *schema != "cxxlens.build-capture-bundle.v1")
				return unexpected(schema ? invalid("schema", "unsupported")
										 : std::move(schema.error()));

			auto toolchain = require_tuple((*tuple.value())[1], "production_toolchain", 10U);
			if (!toolchain)
				return unexpected(std::move(toolchain.error()));
			auto family = require_text((*toolchain.value())[0], "production_toolchain.family");
			auto version =
				require_text((*toolchain.value())[1], "production_toolchain.exact_version");
			if (!family || !version || (*family != "gcc" && *family != "msvc"))
				return unexpected(
					!family ? std::move(family.error())
							: (!version ? std::move(version.error())
										: invalid("production_toolchain.family", "unsupported")));
			output.production_compiler = std::string{*family} + "-" + std::string{*version};
			output.decoded.toolchain_family = *family;
			output.decoded.toolchain_version = *version;
			if ((*family == "gcc" && *version != "16.2.0") ||
				(*family == "msvc" && *version != "19.51.36247"))
				return unexpected(invalid("production_toolchain.exact_version", "not-pinned"));
			std::vector<capture_gap> generated_gaps;
			if (auto valid = validate_captured((*toolchain.value())[2],
											   "production_toolchain.canonical_binary_path",
											   canonical_value::kind::utf8_string,
											   generated_gaps);
				!valid)
				return unexpected(std::move(valid.error()));
			if ((*toolchain.value())[2].tuple[1].type == canonical_value::kind::utf8_string &&
				!canonical_native_path((*toolchain.value())[2].tuple[1].text, *family == "msvc"))
				return unexpected(invalid("production_toolchain.canonical_binary_path",
										  "not-canonical-absolute"));
			if (auto valid = validate_captured((*toolchain.value())[3],
											   "production_toolchain.binary_digest",
											   canonical_value::kind::utf8_string,
											   generated_gaps);
				!valid)
				return unexpected(std::move(valid.error()));
			if ((*toolchain.value())[3].tuple[1].type == canonical_value::kind::utf8_string &&
				!digest_like((*toolchain.value())[3].tuple[1].text))
				return unexpected(invalid("production_toolchain.binary_digest", "digest"));
			if (auto valid = require_strong_id((*toolchain.value())[4],
											   "production_toolchain.target_triple");
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = validate_captured((*toolchain.value())[5],
											   "production_toolchain.sysroot",
											   canonical_value::kind::utf8_string,
											   generated_gaps);
				!valid)
				return unexpected(std::move(valid.error()));
			if ((*toolchain.value())[5].tuple[1].type == canonical_value::kind::utf8_string &&
				!canonical_native_path((*toolchain.value())[5].tuple[1].text, *family == "msvc"))
				return unexpected(
					invalid("production_toolchain.sysroot", "not-canonical-absolute"));
			auto capture_digest = [&](const std::size_t index,
									  const std::string_view name,
									  std::optional<std::string>& destination) -> result<void>
			{
				const auto field = "production_toolchain." + std::string{name};
				if (auto valid = validate_captured((*toolchain.value())[index],
												   field,
												   canonical_value::kind::utf8_string,
												   generated_gaps);
					!valid)
					return valid;
				const auto& captured = (*toolchain.value())[index].tuple[1];
				if (captured.type == canonical_value::kind::utf8_string)
				{
					if (auto valid = require_digest(captured, field); !valid)
						return valid;
					destination = captured.text;
				}
				return {};
			};
			if (auto valid = capture_digest(6U, "abi_digest", output.decoded.abi_digest); !valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = capture_digest(
					7U, "builtin_headers_digest", output.decoded.builtin_headers_digest);
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = capture_digest(
					8U, "builtin_macros_digest", output.decoded.builtin_macros_digest);
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = capture_digest(
					9U, "include_search_digest", output.decoded.include_search_digest);
				!valid)
				return unexpected(std::move(valid.error()));

			auto adapter = require_text((*tuple.value())[2], "capture_adapter");
			auto abi = require_text((*tuple.value())[3], "target_abi");
			auto project = require_text((*tuple.value())[4], "project_id");
			if (!adapter || !abi || !project)
				return unexpected(
					!adapter ? std::move(adapter.error())
							 : (!abi ? std::move(abi.error()) : std::move(project.error())));
			const bool gcc_adapter = *family == "gcc" &&
				(*adapter == "compile-commands" || *adapter == "shell-free-wrapper");
			const bool msvc_adapter = *family == "msvc" && *adapter == "msbuild-cltool-proxy";
			if (!gcc_adapter && !msvc_adapter)
				return unexpected(invalid("capture_adapter", "toolchain-mismatch"));
			const auto expected_abi =
				*family == "gcc" ? "x86_64-linux-gnu" : "x86_64-pc-windows-msvc";
			if (*abi != expected_abi)
				return unexpected(invalid("target_abi", "not-pinned"));
			output.capture_adapter = *adapter;
			output.target_abi = *abi;
			output.project_id = *project;
			output.decoded.target_triple = (*toolchain.value())[4].text;
			output.decoded.target_abi = *abi;
			output.decoded.project_id = *project;
			auto logical_root = require_text((*tuple.value())[8], "logical_project_root");
			if (!logical_root || !logical_path(*logical_root) || *logical_root != "project://")
				return unexpected(logical_root ? invalid("logical_project_root", "machine-path")
											   : std::move(logical_root.error()));
			output.logical_project_root = *logical_root;
			output.decoded.logical_project_root = *logical_root;

			if (auto valid = validate_captured((*tuple.value())[9],
											   "path_mappings",
											   canonical_value::kind::ordered_tuple,
											   generated_gaps);
				!valid)
				return unexpected(std::move(valid.error()));
			const auto& mappings = (*tuple.value())[9].tuple[1];
			if (mappings.type == canonical_value::kind::ordered_tuple &&
				mappings.tuple.size() > limits.maximum_path_mappings)
				return unexpected(limit("path_mappings", "count"));
			std::vector<std::pair<std::string, std::string>> path_mappings;
			if (mappings.type == canonical_value::kind::ordered_tuple)
			{
				std::string previous_physical;
				std::set<std::string, std::less<>> logical_prefixes;
				for (std::size_t index{}; index < mappings.tuple.size(); ++index)
				{
					const auto prefix = "path_mappings[" + std::to_string(index) + "]";
					auto mapping = require_tuple(mappings.tuple[index], prefix, 2U);
					if (!mapping)
						return unexpected(std::move(mapping.error()));
					auto physical =
						require_text((*mapping.value())[0], prefix + ".captured_physical_prefix");
					auto logical = require_text((*mapping.value())[1], prefix + ".logical_prefix");
					if (!physical || !logical ||
						!canonical_native_path(*physical, *family == "msvc") ||
						!logical_path(*logical) || !logical_at_or_below(*logical, *logical_root) ||
						(!previous_physical.empty() && previous_physical >= *physical) ||
						!logical_prefixes.emplace(*logical).second)
						return unexpected(invalid(prefix, "invalid-or-noncanonical"));
					for (const auto& [existing_physical, existing_logical] : path_mappings)
						if (native_path_at_or_below(
								*physical, existing_physical, *family == "msvc") ||
							native_path_at_or_below(
								existing_physical, *physical, *family == "msvc") ||
							logical_at_or_below(*logical, existing_logical) ||
							logical_at_or_below(existing_logical, *logical))
							return unexpected(invalid(prefix, "overlapping-authority"));
					previous_physical = *physical;
					path_mappings.emplace_back(*physical, *logical);
					output.decoded.path_mappings.push_back(
						{std::string{*physical}, std::string{*logical}});
				}
			}

			const auto& units = (*tuple.value())[5];
			if (units.type != canonical_value::kind::ordered_tuple || units.tuple.empty() ||
				units.tuple.size() > limits.maximum_compile_units)
				return unexpected(limit("compile_units", "count"));
			std::set<std::string, std::less<>> unit_ids;
			std::vector<compile_source_binding> source_bindings;
			source_bindings.reserve(units.tuple.size());
			std::string previous_unit;
			for (std::size_t index{}; index < units.tuple.size(); ++index)
			{
				const auto prefix = "compile_units[" + std::to_string(index) + "]";
				auto unit = require_tuple(units.tuple[index], prefix, 16U);
				if (!unit)
					return unexpected(std::move(unit.error()));
				auto id = require_text((*unit.value())[0], prefix + ".compile_unit_id");
				auto path = require_text((*unit.value())[3], prefix + ".source_logical_path");
				if (!id || !path || !logical_path(*path) ||
					!logical_at_or_below(*path, *logical_root) || !unit_ids.emplace(*id).second ||
					(!previous_unit.empty() && previous_unit >= *id))
					return unexpected(
						!id ? std::move(id.error())
							: (!path ? std::move(path.error())
									 : invalid(prefix, "duplicate-or-noncanonical-order")));
				previous_unit = *id;
				for (const auto field_index : {0U, 2U, 7U})
					if (auto valid =
							require_strong_id((*unit.value())[field_index],
											  prefix + "[" + std::to_string(field_index) + "]");
						!valid)
						return unexpected(std::move(valid.error()));
				if (auto valid = validate_captured((*unit.value())[1],
												   prefix + ".source_snapshot_id",
												   canonical_value::kind::utf8_string,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*unit.value())[1].tuple[1].type == canonical_value::kind::utf8_string)
					if (auto valid = require_strong_id((*unit.value())[1].tuple[1],
													   prefix + ".source_snapshot_id");
						!valid)
						return unexpected(std::move(valid.error()));
				if (auto valid =
						require_digest((*unit.value())[4], prefix + ".source_content_digest");
					!valid)
					return unexpected(std::move(valid.error()));
				auto source_file_id = require_text((*unit.value())[2], prefix + ".source_file_id");
				auto source_digest =
					require_text((*unit.value())[4], prefix + ".source_content_digest");
				auto source_size = require_count((*unit.value())[5], prefix + ".source_size_bytes");
				if (!source_file_id || !source_digest || !source_size)
					return unexpected(!source_file_id
										  ? std::move(source_file_id.error())
										  : (!source_digest ? std::move(source_digest.error())
															: std::move(source_size.error())));
				auto source_closure =
					require_text((*unit.value())[15], prefix + ".source_closure_id");
				if (!source_closure)
					return unexpected(std::move(source_closure.error()));
				if (auto valid =
						require_strong_id((*unit.value())[15], prefix + ".source_closure_id");
					!valid)
					return unexpected(std::move(valid.error()));
				std::optional<std::string> source_snapshot;
				if ((*unit.value())[1].tuple[1].type == canonical_value::kind::utf8_string)
					source_snapshot = (*unit.value())[1].tuple[1].text;
				source_bindings.push_back({source_snapshot,
										   std::string{*source_file_id},
										   std::string{*path},
										   std::string{*source_digest},
										   *source_size,
										   std::string{*source_closure}});
				const auto& added_binding = source_bindings.back();
				for (const auto& prior :
					 std::span{source_bindings}.first(source_bindings.size() - 1U))
					if (prior.source_closure_id == added_binding.source_closure_id &&
						prior.file_id == added_binding.file_id &&
						(prior.source_snapshot_id != added_binding.source_snapshot_id ||
						 prior.logical_path != added_binding.logical_path ||
						 prior.content_digest != added_binding.content_digest ||
						 prior.size_bytes != added_binding.size_bytes))
						return unexpected(invalid(prefix, "conflicting-shared-source-closure"));
				auto working =
					require_text((*unit.value())[6], prefix + ".logical_working_directory");
				if (!working || !logical_path(*working) ||
					!logical_at_or_below(*working, *logical_root))
					return unexpected(
						working ? invalid(prefix + ".logical_working_directory", "machine-path")
								: std::move(working.error()));
				if (auto valid = validate_captured((*unit.value())[8],
												   prefix + ".original_argv",
												   canonical_value::kind::ordered_tuple,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*unit.value())[8].tuple[1].type == canonical_value::kind::ordered_tuple)
					if (auto valid = validate_string_tuple((*unit.value())[8].tuple[1],
														   prefix + ".original_argv",
														   limits.maximum_arguments_per_unit);
						!valid)
						return unexpected(std::move(valid.error()));
				auto response_files = decode_auxiliary_files((*unit.value())[9],
															 prefix + ".response_files",
															 limits,
															 *logical_root,
															 generated_gaps);
				if (!response_files)
					return unexpected(std::move(response_files.error()));
				auto config_files = decode_auxiliary_files((*unit.value())[10],
														   prefix + ".config_files",
														   limits,
														   *logical_root,
														   generated_gaps);
				if (!config_files)
					return unexpected(std::move(config_files.error()));
				if (auto valid = validate_captured((*unit.value())[11],
												   prefix + ".environment_effects",
												   canonical_value::kind::ordered_tuple,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*unit.value())[11].tuple[1].type == canonical_value::kind::ordered_tuple &&
					(*unit.value())[11].tuple[1].tuple.size() >
						limits.maximum_environment_effects_per_unit)
					return unexpected(limit(prefix + ".environment_effects", "count"));
				if ((*unit.value())[11].tuple[1].type == canonical_value::kind::ordered_tuple)
				{
					std::string previous_name;
					for (std::size_t effect_index{};
						 effect_index < (*unit.value())[11].tuple[1].tuple.size();
						 ++effect_index)
					{
						const auto effect_prefix =
							prefix + ".environment_effects[" + std::to_string(effect_index) + "]";
						auto effect = require_tuple(
							(*unit.value())[11].tuple[1].tuple[effect_index], effect_prefix, 2U);
						if (!effect)
							return unexpected(std::move(effect.error()));
						auto name = require_text((*effect.value())[0], effect_prefix + ".name");
						if (!name || !validate_registered_symbol(*name) ||
							(!previous_name.empty() && previous_name >= *name))
							return unexpected(name ? invalid(effect_prefix + ".name", "order")
												   : std::move(name.error()));
						previous_name = *name;
						if (auto valid = validate_captured((*effect.value())[1],
														   effect_prefix + ".semantic_value",
														   canonical_value::kind::utf8_string,
														   generated_gaps);
							!valid)
							return unexpected(std::move(valid.error()));
					}
				}
				if (auto valid = validate_captured((*unit.value())[12],
												   prefix + ".captured_working_directory",
												   canonical_value::kind::utf8_string,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*unit.value())[12].tuple[1].type == canonical_value::kind::utf8_string)
				{
					const auto physical = std::string_view{(*unit.value())[12].tuple[1].text};
					if (!canonical_native_path(physical, *family == "msvc"))
						return unexpected(invalid(prefix + ".captured_working_directory",
												  "not-canonical-absolute"));
					const auto mapping =
						std::ranges::find_if(path_mappings,
											 [&](const auto& candidate)
											 {
												 return native_path_at_or_below(
													 physical, candidate.first, *family == "msvc");
											 });
					if (mapping == path_mappings.end())
						return unexpected(invalid(prefix + ".captured_working_directory",
												  "unmapped-physical-path"));
				}
				for (const auto& [field_index, field_name] : {
						 std::pair{13U, std::string_view{"language_standard"}},
						 std::pair{14U, std::string_view{"extension_mode"}},
					 })
				{
					if (auto valid = validate_captured((*unit.value())[field_index],
													   prefix + "." + std::string{field_name},
													   canonical_value::kind::utf8_string,
													   generated_gaps);
						!valid)
						return unexpected(std::move(valid.error()));
				}
				for (const auto field_index : {13U, 14U})
					if ((*unit.value())[field_index].tuple[1].type ==
						canonical_value::kind::utf8_string)
						if (auto valid =
								require_strong_id((*unit.value())[field_index].tuple[1],
												  prefix +
													  (field_index == 13U ? ".language_standard"
																		  : ".extension_mode"));
							!valid)
							return unexpected(std::move(valid.error()));
				const auto& extension = (*unit.value())[14].tuple[1];
				if (extension.type == canonical_value::kind::utf8_string &&
					((*family == "gcc" && extension.text != "strict" && extension.text != "gnu") ||
					 (*family == "msvc" && extension.text != "strict" && extension.text != "msvc")))
					return unexpected(invalid(prefix + ".extension_mode", "toolchain-mismatch"));

				detail::decoded_capture_unit decoded_unit;
				decoded_unit.compile_unit_id = *id;
				decoded_unit.source_snapshot_id = source_snapshot;
				decoded_unit.source_file_id = *source_file_id;
				decoded_unit.source_logical_path = *path;
				decoded_unit.source_content_digest = *source_digest;
				decoded_unit.source_size_bytes = *source_size;
				decoded_unit.logical_working_directory = *working;
				decoded_unit.language = (*unit.value())[7].text;
				if ((*unit.value())[8].tuple[1].type == canonical_value::kind::ordered_tuple)
				{
					std::vector<std::string> arguments;
					arguments.reserve((*unit.value())[8].tuple[1].tuple.size());
					for (const auto& argument : (*unit.value())[8].tuple[1].tuple)
						arguments.push_back(argument.text);
					decoded_unit.original_arguments = std::move(arguments);
				}
				decoded_unit.response_files = std::move(*response_files);
				decoded_unit.config_files = std::move(*config_files);
				if ((*unit.value())[11].tuple[1].type == canonical_value::kind::ordered_tuple)
				{
					decoded_unit.environment_effects.reserve(
						(*unit.value())[11].tuple[1].tuple.size());
					for (const auto& effect_value : (*unit.value())[11].tuple[1].tuple)
					{
						const auto& captured = effect_value.tuple[1];
						detail::decoded_capture_environment_effect effect;
						effect.name = effect_value.tuple[0].text;
						effect.state = captured.tuple[0].text;
						if (captured.tuple[1].type == canonical_value::kind::utf8_string)
							effect.semantic_value = captured.tuple[1].text;
						effect.reason = captured.tuple[2].text;
						effect.completion_action = captured.tuple[3].text;
						decoded_unit.environment_effects.push_back(std::move(effect));
					}
				}
				if ((*unit.value())[13].tuple[1].type == canonical_value::kind::utf8_string)
					decoded_unit.language_standard = (*unit.value())[13].tuple[1].text;
				if (extension.type == canonical_value::kind::utf8_string)
					decoded_unit.extension_mode = extension.text;
				decoded_unit.source_closure_id = *source_closure;
				output.decoded.compile_units.push_back(std::move(decoded_unit));
			}
			output.compile_unit_count = units.tuple.size();

			const auto& closures = (*tuple.value())[6];
			if (closures.type != canonical_value::kind::ordered_tuple || closures.tuple.empty() ||
				closures.tuple.size() > limits.maximum_source_closures)
				return unexpected(limit("source_closures", "count"));
			std::set<std::string, std::less<>> referenced_closures;
			for (const auto& binding : source_bindings)
				referenced_closures.emplace(binding.source_closure_id);
			std::set<std::string, std::less<>> admitted_closures;
			std::uint64_t total_members{};
			std::uint64_t total_blobs{};
			std::uint64_t total_bytes{};
			std::string previous_closure_id;
			for (std::size_t closure_index{}; closure_index < closures.tuple.size();
				 ++closure_index)
			{
				const auto closure_prefix =
					"source_closures[" + std::to_string(closure_index) + "]";
				auto closure = require_tuple(closures.tuple[closure_index], closure_prefix, 8U);
				if (!closure)
					return unexpected(std::move(closure.error()));
				auto closure_id =
					require_text((*closure.value())[0], closure_prefix + ".closure_id");
				if (!closure_id ||
					(!previous_closure_id.empty() && previous_closure_id >= *closure_id) ||
					!admitted_closures.emplace(*closure_id).second ||
					!referenced_closures.contains(*closure_id))
					return unexpected(invalid(closure_prefix, "unreferenced-or-noncanonical"));
				previous_closure_id = *closure_id;
				detail::decoded_capture_source_closure decoded_closure;
				decoded_closure.id = *closure_id;
				if (auto valid =
						require_strong_id((*closure.value())[0], closure_prefix + ".closure_id");
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid =
						require_digest((*closure.value())[1], closure_prefix + ".closure_digest");
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid =
						require_digest((*closure.value())[2], closure_prefix + ".manifest_digest");
					!valid)
					return unexpected(std::move(valid.error()));
				auto members =
					require_count((*closure.value())[3], closure_prefix + ".member_count");
				auto blobs = require_count((*closure.value())[4], closure_prefix + ".blob_count");
				auto bytes =
					require_count((*closure.value())[5], closure_prefix + ".unique_blob_bytes");
				if (!members || !blobs || !bytes || *members == 0U || *blobs > *members ||
					*members > limits.maximum_source_closure_members - total_members ||
					*blobs > limits.maximum_source_closure_blobs - total_blobs ||
					*bytes > limits.maximum_source_closure_bytes - total_bytes)
					return unexpected(limit(closure_prefix, "census"));
				total_members += *members;
				total_blobs += *blobs;
				total_bytes += *bytes;

				const auto& closure_members = (*closure.value())[6];
				if (closure_members.type != canonical_value::kind::ordered_tuple ||
					closure_members.tuple.size() != *members)
					return unexpected(invalid(closure_prefix + ".members", "census-mismatch"));
				std::set<std::string, std::less<>> member_ids;
				std::set<std::string, std::less<>> member_paths;
				std::map<std::string, std::uint64_t, std::less<>> unique_blobs;
				std::uint64_t recomputed_bytes{};
				std::string previous_member_id;
				for (std::size_t index{}; index < closure_members.tuple.size(); ++index)
				{
					const auto prefix = closure_prefix + ".members[" + std::to_string(index) + "]";
					auto member = require_tuple(closure_members.tuple[index], prefix, 8U);
					if (!member)
						return unexpected(std::move(member.error()));
					auto file_id = require_text((*member.value())[0], prefix + ".file_id");
					auto path = require_text((*member.value())[1], prefix + ".logical_path");
					if (!file_id || !path || !logical_path(*path) ||
						!logical_at_or_below(*path, *logical_root) ||
						!member_ids.emplace(*file_id).second ||
						!member_paths.emplace(*path).second ||
						(!previous_member_id.empty() && previous_member_id >= *file_id))
						return unexpected(invalid(prefix, "duplicate-or-noncanonical-order"));
					previous_member_id = *file_id;
					auto expected_file_id = application_source_file_id(*path);
					if (!expected_file_id || *expected_file_id != *file_id)
						return unexpected(invalid(prefix + ".file_id", "binding-mismatch"));
					for (const auto& [field_index, field_name] : {
							 std::pair{2U, std::string_view{"content_digest"}},
							 std::pair{5U, std::string_view{"role"}},
							 std::pair{6U, std::string_view{"encoding"}},
						 })
						if (auto valid = validate_captured((*member.value())[field_index],
														   prefix + "." + std::string{field_name},
														   canonical_value::kind::utf8_string,
														   generated_gaps);
							!valid)
							return unexpected(std::move(valid.error()));
					const auto& digest_value = (*member.value())[2].tuple[1];
					const auto& role_value = (*member.value())[5].tuple[1];
					const auto& encoding_value = (*member.value())[6].tuple[1];
					if (digest_value.type == canonical_value::kind::utf8_string &&
						!digest_like(digest_value.text))
						return unexpected(invalid(prefix + ".content_digest", "digest"));
					if (role_value.type == canonical_value::kind::utf8_string &&
						!source_role(role_value.text))
						return unexpected(invalid(prefix + ".role", "enum"));
					if (encoding_value.type == canonical_value::kind::utf8_string &&
						!source_encoding(encoding_value.text))
						return unexpected(invalid(prefix + ".encoding", "enum"));
					if ((*member.value())[7].type != canonical_value::kind::boolean ||
						!(*member.value())[7].boolean)
						return unexpected(invalid(prefix + ".read_only", "required-true"));
					if (auto valid = validate_captured((*member.value())[3],
													   prefix + ".content",
													   canonical_value::kind::bytes,
													   generated_gaps);
						!valid)
						return unexpected(std::move(valid.error()));
					auto size = require_count((*member.value())[4], prefix + ".size_bytes");
					if (!size)
						return unexpected(std::move(size.error()));
					const auto& content_value = (*member.value())[3].tuple[1];
					if (content_value.type == canonical_value::kind::bytes)
					{
						if (digest_value.type != canonical_value::kind::utf8_string ||
							content_value.byte_string.size() != *size ||
							content_digest(content_value.byte_string) != digest_value.text)
							return unexpected(
								invalid(prefix + ".content", "digest-or-size-mismatch"));
						if (const auto [found, inserted] =
								unique_blobs.emplace(digest_value.text, *size);
							!inserted && found->second != *size)
							return unexpected(
								invalid(prefix + ".content", "duplicate-digest-size"));
						else if (inserted)
						{
							if (*size > limits.maximum_source_closure_bytes - recomputed_bytes)
								return unexpected(limit(closure_prefix, "byte-overflow"));
							recomputed_bytes += *size;
						}
						if (role_value.type == canonical_value::kind::utf8_string)
							decoded_closure.members.push_back({std::string{*path},
															   digest_value.text,
															   content_value.byte_string,
															   role_value.text});
					}

					const auto binding = std::ranges::find_if(
						source_bindings,
						[&](const auto& candidate)
						{
							return candidate.source_closure_id == *closure_id &&
								candidate.file_id == *file_id;
						});
					if (binding != source_bindings.end())
					{
						if (binding->logical_path != *path || binding->size_bytes != *size ||
							digest_value.type != canonical_value::kind::utf8_string ||
							binding->content_digest != digest_value.text ||
							role_value.type != canonical_value::kind::utf8_string ||
							role_value.text != "main")
							return unexpected(invalid(prefix, "compile-unit-source-mismatch"));
						if (binding->source_snapshot_id)
						{
							if (encoding_value.type != canonical_value::kind::utf8_string)
								return unexpected(
									invalid(prefix + ".encoding", "snapshot-binding-missing"));
							auto snapshot = application_source_snapshot_id(
								*file_id, digest_value.text, encoding_value.text);
							if (!snapshot || *snapshot != *binding->source_snapshot_id)
								return unexpected(invalid(prefix, "source-snapshot-mismatch"));
						}
					}
					else if (role_value.type == canonical_value::kind::utf8_string &&
							 role_value.text == "main")
						return unexpected(invalid(prefix + ".role", "unbound-main"));
				}

				if (unique_blobs.size() != *blobs || recomputed_bytes != *bytes)
					return unexpected(invalid(closure_prefix, "blob-census-mismatch"));
				if (auto valid = validate_captured((*closure.value())[7],
												   closure_prefix + ".membership_coverage",
												   canonical_value::kind::utf8_string,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				const auto& membership_coverage = (*closure.value())[7].tuple[1];
				if (membership_coverage.type == canonical_value::kind::utf8_string &&
					membership_coverage.text != "complete")
					return unexpected(invalid(closure_prefix + ".membership_coverage", "enum"));
				auto encoded_members = canonical_binary(closure_members);
				if (!encoded_members)
					return unexpected(invalid(closure_prefix + ".members", "canonical-encoding"));
				const auto recomputed_manifest = content_digest(*encoded_members);
				if ((*closure.value())[2].text != recomputed_manifest)
					return unexpected(
						invalid(closure_prefix + ".manifest_digest", "binding-mismatch"));
				const std::array closure_fields{
					canonical_value::from_string(recomputed_manifest),
					canonical_value::from_integer(static_cast<std::int64_t>(*members)),
					canonical_value::from_integer(static_cast<std::int64_t>(*blobs)),
					canonical_value::from_integer(static_cast<std::int64_t>(*bytes)),
					(*closure.value())[7],
				};
				auto recomputed_closure =
					canonical_identity_digest("application-source-closure", closure_fields);
				if (!recomputed_closure || (*closure.value())[1].text != *recomputed_closure ||
					*closure_id != "source-closure:" + *recomputed_closure)
					return unexpected(
						invalid(closure_prefix + ".closure_digest", "binding-mismatch"));
				for (auto& unit : output.decoded.compile_units)
					if (unit.source_closure_id == *closure_id)
						unit.source_closure_digest = *recomputed_closure;
				for (const auto& binding : source_bindings)
					if (binding.source_closure_id == *closure_id &&
						!member_ids.contains(binding.file_id))
						return unexpected(
							invalid(closure_prefix + ".members", "compile-unit-source-missing"));
				output.decoded.source_closures.push_back(std::move(decoded_closure));
			}
			for (std::size_t unit_index{}; unit_index < output.decoded.compile_units.size();
				 ++unit_index)
			{
				const auto& unit = output.decoded.compile_units[unit_index];
				const auto closure = std::ranges::find(output.decoded.source_closures,
													   unit.source_closure_id,
													   &detail::decoded_capture_source_closure::id);
				if (closure == output.decoded.source_closures.end())
					return unexpected(invalid("source_closures", "reference-mismatch"));
				const auto validate_auxiliary_binding =
					[&](const std::span<const detail::decoded_capture_auxiliary_file> files,
						const std::string_view name) -> result<void>
				{
					for (std::size_t index{}; index < files.size(); ++index)
					{
						const auto& file = files[index];
						if (!file.content_digest)
							continue;
						const auto member =
							std::ranges::find(closure->members,
											  file.logical_path,
											  &detail::decoded_capture_source_member::logical_path);
						if (member == closure->members.end() ||
							member->content_digest != *file.content_digest ||
							member->content.size() != file.size_bytes ||
							member->role != "generated")
							return unexpected(
								invalid("compile_units[" + std::to_string(unit_index) + "]." +
											std::string{name} + "[" + std::to_string(index) + "]",
										"source-closure-binding-mismatch"));
					}
					return {};
				};
				if (auto valid = validate_auxiliary_binding(unit.response_files, "response_files");
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = validate_auxiliary_binding(unit.config_files, "config_files");
					!valid)
					return unexpected(std::move(valid.error()));
			}
			if (admitted_closures != referenced_closures)
				return unexpected(invalid("source_closures", "reference-mismatch"));

			const auto& declared = (*tuple.value())[7];
			if (declared.type != canonical_value::kind::ordered_tuple ||
				declared.tuple.size() != generated_gaps.size())
				return unexpected(invalid("gaps", "census-mismatch"));
			std::vector<capture_gap> declared_gaps;
			for (std::size_t index{}; index < declared.tuple.size(); ++index)
			{
				auto gap =
					require_tuple(declared.tuple[index], "gaps[" + std::to_string(index) + "]", 4U);
				if (!gap)
					return unexpected(std::move(gap.error()));
				capture_gap value;
				auto field = require_text((*gap.value())[0], "gaps.field");
				auto state = require_text((*gap.value())[1], "gaps.state");
				auto reason = require_text((*gap.value())[2], "gaps.reason");
				auto action = require_text((*gap.value())[3], "gaps.completion_action");
				if (!field || !state || !reason || !action ||
					(*state != "redacted" && *state != "unavailable"))
					return unexpected(invalid("gaps", "entry"));
				value = {std::string{*field},
						 std::string{*state},
						 std::string{*reason},
						 std::string{*action}};
				declared_gaps.push_back(std::move(value));
			}
			auto gap_order = [](const capture_gap& left, const capture_gap& right)
			{
				return std::tie(left.field, left.state, left.reason, left.completion_action) <
					std::tie(right.field, right.state, right.reason, right.completion_action);
			};
			std::ranges::sort(generated_gaps, gap_order);
			if (!std::ranges::is_sorted(declared_gaps, gap_order) ||
				declared_gaps != generated_gaps)
				return unexpected(invalid("gaps", "projection-mismatch"));
			output.gaps = std::move(declared_gaps);
			return output;
		}
	} // namespace

	result<void> import_limits::validate() const
	{
		if (maximum_bundle_bytes == 0U || maximum_nesting_depth == 0U ||
			maximum_nesting_depth > 64U || maximum_compile_units == 0U ||
			maximum_arguments_per_unit == 0U || maximum_auxiliary_files_per_unit == 0U ||
			maximum_environment_effects_per_unit == 0U || maximum_path_mappings == 0U ||
			maximum_string_bytes == 0U || maximum_total_metadata_bytes == 0U ||
			maximum_source_closure_members == 0U || maximum_source_closures == 0U ||
			maximum_source_closure_blobs == 0U || maximum_source_closure_bytes == 0U)
			return unexpected(error{"application-analysis.import-limits-invalid", "limits", {}});
		return {};
	}

	capture_bundle::capture_bundle(std::shared_ptr<const implementation> value)
		: value_{std::move(value)}
	{
	}
	std::string_view capture_bundle::digest() const noexcept
	{
		return value_->digest;
	}
	std::string_view capture_bundle::production_compiler() const noexcept
	{
		return value_->production_compiler;
	}
	std::string_view capture_bundle::capture_adapter() const noexcept
	{
		return value_->capture_adapter;
	}
	std::string_view capture_bundle::target_abi() const noexcept
	{
		return value_->target_abi;
	}
	std::string_view capture_bundle::project_id() const noexcept
	{
		return value_->project_id;
	}
	std::string_view capture_bundle::logical_project_root() const noexcept
	{
		return value_->logical_project_root;
	}
	std::size_t capture_bundle::compile_unit_count() const noexcept
	{
		return value_->compile_unit_count;
	}
	std::span<const capture_gap> capture_bundle::gaps() const noexcept
	{
		return value_->gaps;
	}

	result<capture_bundle> decode_capture_bundle(const std::span<const std::byte> input,
												 const import_limits limits)
	{
		try
		{
			if (auto valid = limits.validate(); !valid)
				return unexpected(std::move(valid.error()));
			if (input.empty() || input.size() > limits.maximum_bundle_bytes)
				return unexpected(limit("bundle", "bytes"));
			if (auto valid = preflight_value(input, 1U, limits); !valid)
				return unexpected(std::move(valid.error()));
			auto decoded = canonical_binary_decode(input);
			if (!decoded)
				return unexpected(invalid("binary", decoded.error().detail));
			metadata_budget budget{limits};
			if (auto valid = account_strings(*decoded, budget, "root"); !valid)
				return unexpected(std::move(valid.error()));
			auto value = std::make_shared<capture_bundle::implementation>();
			value->root = std::move(*decoded);
			auto projection = validate_bundle_shape(value->root, limits);
			if (!projection)
				return unexpected(std::move(projection.error()));
			value->production_compiler = std::move(projection->production_compiler);
			value->capture_adapter = std::move(projection->capture_adapter);
			value->target_abi = std::move(projection->target_abi);
			value->project_id = std::move(projection->project_id);
			value->logical_project_root = std::move(projection->logical_project_root);
			value->compile_unit_count = projection->compile_unit_count;
			value->gaps = std::move(projection->gaps);
			value->projection = std::move(projection->decoded);
			value->digest = content_digest(input);
			return capture_bundle{std::move(value)};
		}
		catch (const std::bad_alloc&)
		{
			return unexpected(limit("bundle", "allocation"));
		}
		catch (const std::length_error&)
		{
			return unexpected(limit("bundle", "allocation-length"));
		}
	}

} // namespace cxxlens::sdk
