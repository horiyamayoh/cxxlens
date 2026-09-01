#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <utility>

#include <cxxlens/sdk/application_analysis.hpp>

namespace cxxlens::sdk
{
	struct capture_bundle::implementation
	{
		canonical_value root;
		std::string digest;
		std::string production_compiler;
		std::string capture_adapter;
		std::string target_abi;
		std::string project_id;
		std::size_t compile_unit_count{};
		std::vector<capture_gap> gaps;
	};

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

		[[nodiscard]] result<void> validate_auxiliary_files(const canonical_value& captured,
															const std::string& field,
															const import_limits& limits,
															std::vector<capture_gap>& gaps)
		{
			if (auto valid =
					validate_captured(captured, field, canonical_value::kind::ordered_tuple, gaps);
				!valid)
				return valid;
			const auto& tuple = captured.tuple[1];
			if (tuple.type == canonical_value::kind::null_value)
				return {};
			if (tuple.tuple.size() > limits.maximum_auxiliary_files_per_unit)
				return unexpected(limit(field, "count"));
			std::set<std::string, std::less<>> paths;
			std::vector<std::optional<std::size_t>> parents;
			parents.reserve(tuple.tuple.size());
			for (std::size_t index{}; index < tuple.tuple.size(); ++index)
			{
				const auto prefix = field + "[" + std::to_string(index) + "]";
				auto item = require_tuple(tuple.tuple[index], prefix, 4U);
				if (!item)
					return unexpected(std::move(item.error()));
				auto path = require_text((*item.value())[0], prefix + ".logical_path");
				if (!path || !logical_path(*path) || !paths.emplace(*path).second)
					return unexpected(
						path ? invalid(prefix + ".logical_path", "duplicate-or-machine-path")
							 : std::move(path.error()));
				if (auto valid = validate_captured((*item.value())[1],
												   prefix + ".content_digest",
												   canonical_value::kind::utf8_string,
												   gaps);
					!valid)
					return valid;
				if ((*item.value())[1].tuple[1].type == canonical_value::kind::utf8_string &&
					!digest_like((*item.value())[1].tuple[1].text))
					return unexpected(invalid(prefix + ".content_digest", "digest"));
				if (auto size = require_count((*item.value())[2], prefix + ".size_bytes"); !size)
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
			return {};
		}

		struct validated_bundle_projection
		{
			std::string production_compiler;
			std::string capture_adapter;
			std::string target_abi;
			std::string project_id;
			std::size_t compile_unit_count{};
			std::vector<capture_gap> gaps;
		};

		struct compile_source_binding
		{
			std::string logical_path;
			std::string content_digest;
			std::uint64_t size_bytes{};
		};

		[[nodiscard]] result<validated_bundle_projection>
		validate_bundle_shape(const canonical_value& root, const import_limits& limits)
		{
			validated_bundle_projection output;
			auto tuple = require_tuple(root, "root", 8U);
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
				!absolute_native_path((*toolchain.value())[2].tuple[1].text, *family == "msvc"))
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
			for (const auto index : {6U, 7U, 8U, 9U})
				if (auto valid =
						require_digest((*toolchain.value())[index],
									   "production_toolchain[" + std::to_string(index) + "]");
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

			const auto& units = (*tuple.value())[5];
			if (units.type != canonical_value::kind::ordered_tuple || units.tuple.empty() ||
				units.tuple.size() > limits.maximum_compile_units)
				return unexpected(limit("compile_units", "count"));
			std::set<std::string, std::less<>> unit_ids;
			std::set<std::string, std::less<>> source_paths;
			std::map<std::string, compile_source_binding, std::less<>> source_bindings;
			std::string previous_unit;
			for (std::size_t index{}; index < units.tuple.size(); ++index)
			{
				const auto prefix = "compile_units[" + std::to_string(index) + "]";
				auto unit = require_tuple(units.tuple[index], prefix, 12U);
				if (!unit)
					return unexpected(std::move(unit.error()));
				auto id = require_text((*unit.value())[0], prefix + ".compile_unit_id");
				auto path = require_text((*unit.value())[3], prefix + ".source_logical_path");
				if (!id || !path || !logical_path(*path) || !unit_ids.emplace(*id).second ||
					!source_paths.emplace(*path).second ||
					(!previous_unit.empty() && previous_unit >= *id))
					return unexpected(
						!id ? std::move(id.error())
							: (!path ? std::move(path.error())
									 : invalid(prefix, "duplicate-or-noncanonical-order")));
				previous_unit = *id;
				for (const auto field_index : {0U, 1U, 2U, 7U})
					if (auto valid =
							require_strong_id((*unit.value())[field_index],
											  prefix + "[" + std::to_string(field_index) + "]");
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
				if (!source_bindings
						 .emplace(std::string{*source_file_id},
								  compile_source_binding{std::string{*path},
														 std::string{*source_digest},
														 *source_size})
						 .second)
					return unexpected(invalid(prefix + ".source_file_id", "duplicate"));
				auto working =
					require_text((*unit.value())[6], prefix + ".logical_working_directory");
				if (!working || !logical_path(*working))
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
				if (auto valid = validate_auxiliary_files(
						(*unit.value())[9], prefix + ".response_files", limits, generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = validate_auxiliary_files(
						(*unit.value())[10], prefix + ".config_files", limits, generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
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
						if (!name || (!previous_name.empty() && previous_name >= *name))
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
			}
			output.compile_unit_count = units.tuple.size();

			auto closure = require_tuple((*tuple.value())[6], "source_closure", 7U);
			if (!closure)
				return unexpected(std::move(closure.error()));
			if (auto valid = require_strong_id((*closure.value())[0], "source_closure.closure_id");
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid = require_digest((*closure.value())[1], "source_closure.closure_digest");
				!valid)
				return unexpected(std::move(valid.error()));
			if (auto valid =
					require_digest((*closure.value())[2], "source_closure.manifest_digest");
				!valid)
				return unexpected(std::move(valid.error()));
			auto members = require_count((*closure.value())[3], "source_closure.member_count");
			auto blobs = require_count((*closure.value())[4], "source_closure.blob_count");
			auto bytes = require_count((*closure.value())[5], "source_closure.unique_blob_bytes");
			if (!members || !blobs || !bytes)
				return unexpected(
					!members ? std::move(members.error())
							 : (!blobs ? std::move(blobs.error()) : std::move(bytes.error())));
			if (*members > limits.maximum_source_closure_members ||
				*blobs > limits.maximum_source_closure_blobs ||
				*bytes > limits.maximum_source_closure_bytes || *blobs > *members)
				return unexpected(limit("source_closure", "census"));

			const auto& closure_members = (*closure.value())[6];
			if (closure_members.type != canonical_value::kind::ordered_tuple ||
				closure_members.tuple.size() != *members)
				return unexpected(invalid("source_closure.members", "census-mismatch"));
			std::set<std::string, std::less<>> member_ids;
			std::set<std::string, std::less<>> member_paths;
			std::map<std::string, std::uint64_t, std::less<>> unique_blobs;
			std::uint64_t recomputed_bytes{};
			std::string previous_member_id;
			for (std::size_t index{}; index < closure_members.tuple.size(); ++index)
			{
				const auto prefix = "source_closure.members[" + std::to_string(index) + "]";
				auto member = require_tuple(closure_members.tuple[index], prefix, 5U);
				if (!member)
					return unexpected(std::move(member.error()));
				auto file_id = require_text((*member.value())[0], prefix + ".file_id");
				auto path = require_text((*member.value())[1], prefix + ".logical_path");
				if (!file_id || !path || !logical_path(*path) ||
					!member_ids.emplace(*file_id).second || !member_paths.emplace(*path).second ||
					(!previous_member_id.empty() && previous_member_id >= *file_id))
					return unexpected(
						!file_id ? std::move(file_id.error())
								 : (!path ? std::move(path.error())
										  : invalid(prefix, "duplicate-or-noncanonical-order")));
				previous_member_id = *file_id;
				if (auto valid = require_strong_id((*member.value())[0], prefix + ".file_id");
					!valid)
					return unexpected(std::move(valid.error()));
				if (auto valid = validate_captured((*member.value())[2],
												   prefix + ".content_digest",
												   canonical_value::kind::utf8_string,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				if ((*member.value())[2].tuple[1].type == canonical_value::kind::utf8_string &&
					!digest_like((*member.value())[2].tuple[1].text))
					return unexpected(invalid(prefix + ".content_digest", "digest"));
				if (auto valid = validate_captured((*member.value())[3],
												   prefix + ".content",
												   canonical_value::kind::bytes,
												   generated_gaps);
					!valid)
					return unexpected(std::move(valid.error()));
				auto size = require_count((*member.value())[4], prefix + ".size_bytes");
				if (!size)
					return unexpected(std::move(size.error()));
				const auto& digest_value = (*member.value())[2].tuple[1];
				const auto& content_value = (*member.value())[3].tuple[1];
				if (content_value.type == canonical_value::kind::bytes)
				{
					if (digest_value.type != canonical_value::kind::utf8_string ||
						content_value.byte_string.size() != *size ||
						content_digest(content_value.byte_string) != digest_value.text)
						return unexpected(invalid(prefix + ".content", "digest-or-size-mismatch"));
					if (const auto [found, inserted] =
							unique_blobs.emplace(digest_value.text, *size);
						!inserted && found->second != *size)
						return unexpected(invalid(prefix + ".content", "duplicate-digest-size"));
					else if (inserted)
					{
						if (*size > limits.maximum_source_closure_bytes - recomputed_bytes)
							return unexpected(limit("source_closure", "byte-overflow"));
						recomputed_bytes += *size;
					}
				}
				const auto source = source_bindings.find(*file_id);
				if (source != source_bindings.end() &&
					(source->second.logical_path != *path || source->second.size_bytes != *size ||
					 digest_value.type != canonical_value::kind::utf8_string ||
					 source->second.content_digest != digest_value.text))
					return unexpected(invalid(prefix, "compile-unit-source-mismatch"));
			}
			if (unique_blobs.size() != *blobs || recomputed_bytes != *bytes)
				return unexpected(invalid("source_closure", "blob-census-mismatch"));
			auto encoded_members = canonical_binary(closure_members);
			if (!encoded_members)
				return unexpected(invalid("source_closure.members", "canonical-encoding"));
			const auto recomputed_manifest = content_digest(*encoded_members);
			if ((*closure.value())[2].text != recomputed_manifest)
				return unexpected(invalid("source_closure.manifest_digest", "binding-mismatch"));
			const std::array closure_fields{
				canonical_value::from_string(recomputed_manifest),
				canonical_value::from_integer(static_cast<std::int64_t>(*members)),
				canonical_value::from_integer(static_cast<std::int64_t>(*blobs)),
				canonical_value::from_integer(static_cast<std::int64_t>(*bytes)),
			};
			auto recomputed_closure =
				canonical_identity_digest("application-source-closure", closure_fields);
			if (!recomputed_closure || (*closure.value())[1].text != *recomputed_closure)
				return unexpected(invalid("source_closure.closure_digest", "binding-mismatch"));
			for (const auto& [file_id, binding] : source_bindings)
			{
				static_cast<void>(binding);
				if (!member_ids.contains(file_id))
					return unexpected(
						invalid("source_closure.members", "compile-unit-source-missing"));
			}

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
			maximum_environment_effects_per_unit == 0U || maximum_string_bytes == 0U ||
			maximum_total_metadata_bytes == 0U || maximum_source_closure_members == 0U ||
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
		value->compile_unit_count = projection->compile_unit_count;
		value->gaps = std::move(projection->gaps);
		value->digest = content_digest(input);
		return capture_bundle{std::move(value)};
	}

} // namespace cxxlens::sdk
