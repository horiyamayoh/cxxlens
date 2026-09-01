#include "materialization_request_v2_2.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <map>
#include <optional>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

namespace cxxlens::detail::clang22::materialization
{
	namespace
	{
		using ::cxxlens::detail::clang22::provider_task_v4_identity_projection;

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.request-v2_2-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.request-v2_2-binding-mismatch",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::error unsupported(std::string field, std::string detail = {})
		{
			return {"materialization.version-unsupported", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error feature_missing(std::string feature)
		{
			return {"provider.required-feature-missing", "required_features", std::move(feature)};
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return !value.empty() &&
				std::ranges::all_of(value,
									[](const unsigned char byte)
									{
										return (byte >= static_cast<unsigned char>('0') &&
												byte <= static_cast<unsigned char>('9')) ||
											(byte >= static_cast<unsigned char>('a') &&
											 byte <= static_cast<unsigned char>('f'));
									});
		}

		[[nodiscard]] bool semantic_digest_grammar(const std::string_view value) noexcept
		{
			return value.size() == 83U && value.starts_with("semantic-v2:sha256:") &&
				lower_hex(value.substr(19U));
		}

		[[nodiscard]] bool request_id_grammar(const std::string_view value) noexcept
		{
			return value.size() == 107U && value.starts_with("materialization-request:") &&
				semantic_digest_grammar(value.substr(24U));
		}

		// Ordered value/field pair keeps encoding failures attached to the contract field.
		// NOLINTNEXTLINE(bugprone-easily-swappable-parameters)
		[[nodiscard]] sdk::result<json_value> json_text(const std::string_view value,
														const std::string_view field)
		{
			auto encoded = json_value::string(std::string{value});
			if (!encoded)
				return sdk::unexpected(invalid(std::string{field}, "invalid-utf8"));
			return encoded;
		}

		[[nodiscard]] sdk::result<json_value>
		json_object(std::map<std::string, json_value, utf8_byte_less> fields)
		{
			auto encoded = json_value::object(std::move(fields));
			if (!encoded)
				return sdk::unexpected(invalid("projection", "object"));
			return encoded;
		}

		[[nodiscard]] sdk::result<json_value>
		summary_projection(const source_closure_summary& summary)
		{
			auto id = json_text(summary.source_closure_id, "source_closure.source_closure_id");
			auto digest =
				json_text(summary.source_closure_digest, "source_closure.source_closure_digest");
			auto manifest = json_text(summary.manifest_digest, "source_closure.manifest_digest");
			if (!id || !digest || !manifest)
				return sdk::unexpected(invalid("source_closures", "invalid-utf8"));
			return json_object({
				{"blob_count", json_value::unsigned_integer(summary.blob_count)},
				{"manifest_digest", std::move(*manifest)},
				{"member_count", json_value::unsigned_integer(summary.member_count)},
				{"source_closure_digest", std::move(*digest)},
				{"source_closure_id", std::move(*id)},
				{"unique_blob_bytes", json_value::unsigned_integer(summary.unique_blob_bytes)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		request_projection(const materialization_request_v2_2& request)
		{
			auto authority = decode_provider_task_v4_request_authority(request.inherited_authority);
			if (!authority)
				return sdk::unexpected(std::move(authority.error()));
			auto authority_digest = authority->authority_digest();
			if (!authority_digest)
				return sdk::unexpected(std::move(authority_digest.error()));
			auto schema = json_text(request.schema, "schema");
			auto version = json_text(request.request_version, "request_version");
			auto materialization_id =
				json_text(request.materialization_request_id, "materialization_request_id");
			auto semantic_request_digest =
				json_text(request.semantic_request_digest, "semantic_request_digest");
			if (!schema || !version || !materialization_id || !semantic_request_digest)
				return sdk::unexpected(invalid("request", "invalid-utf8"));
			auto authority_digest_value = json_text(*authority_digest, "authority_digest");
			if (!authority_digest_value)
				return sdk::unexpected(std::move(authority_digest_value.error()));
			std::vector<json_value> features;
			features.reserve(request.required_features.size());
			for (const auto& feature : request.required_features)
			{
				auto value = json_text(feature, "required_features");
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				features.push_back(std::move(*value));
			}
			std::vector<json_value> closures;
			closures.reserve(request.source_closures.size());
			for (const auto& closure : request.source_closures)
			{
				auto value = summary_projection(closure);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				closures.push_back(std::move(*value));
			}
			std::vector<json_value> extensions;
			extensions.reserve(request.task_extensions.size());
			for (const auto& task : request.task_extensions)
			{
				auto value = provider_task_v4_identity_projection(task);
				if (!value)
					return sdk::unexpected(std::move(value.error()));
				extensions.push_back(std::move(*value));
			}
			return json_object({
				{"authority_digest", std::move(*authority_digest_value)},
				{"materialization_request_id", std::move(*materialization_id)},
				{"request_version", std::move(*version)},
				{"schema", std::move(*schema)},
				{"required_features", json_value::array(std::move(features))},
				{"semantic_request_digest", std::move(*semantic_request_digest)},
				{"source_closures", json_value::array(std::move(closures))},
				{"task_extensions", json_value::array(std::move(extensions))},
			});
		}

		[[nodiscard]] bool contains_forbidden_source_bytes(const json_value& value)
		{
			if (const auto* object = value.as_object(); object != nullptr)
			{
				for (const auto& [name, child] : *object)
				{
					if (name == "content_base64" || name == "source_bytes" ||
						name == "source_bytes_base64")
						return true;
					if (contains_forbidden_source_bytes(child))
						return true;
				}
			}
			else if (const auto* array = value.as_array(); array != nullptr)
			{
				return std::ranges::any_of(*array, contains_forbidden_source_bytes);
			}
			return false;
		}

		[[nodiscard]] sdk::result<void> document_member_set(const json_value& value,
															std::span<const std::string_view> names,
															const std::string_view field)
		{
			if (value.as_object() == nullptr || !value.has_exact_members(names))
				return sdk::unexpected(invalid(std::string{field}, "member-set"));
			return {};
		}

		[[nodiscard]] sdk::result<void> document_member_kind(const json_value& object,
															 const std::string_view name,
															 const json_value::kind expected,
															 const std::string_view field)
		{
			const auto* member = object.member(name);
			if (member == nullptr || member->type() != expected)
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			return {};
		}

		[[nodiscard]] sdk::result<void> document_unsigned_member(const json_value& object,
																 const std::string_view name,
																 const std::string_view field)
		{
			const auto* member = object.member(name);
			if (member == nullptr ||
				(member->as_unsigned_integer() == nullptr &&
				 member->as_signed_integer() == nullptr))
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			if (member->as_signed_integer() != nullptr && *member->as_signed_integer() < 0)
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		document_string_array(const json_value& object,
							  const std::string_view name,
							  const std::span<const std::string_view> expected,
							  const std::string_view field)
		{
			const auto* value = object.member(name);
			if (value == nullptr || value->as_array() == nullptr ||
				value->as_array()->size() != expected.size())
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			for (std::size_t index{}; index < expected.size(); ++index)
			{
				const auto* item = (*value->as_array())[index].as_string();
				if (item == nullptr || *item != expected[index])
					return sdk::unexpected(invalid(std::string{field}, "required-feature-list"));
			}
			return {};
		}

		template <typename StringLike, std::size_t Size>
		[[nodiscard]] sdk::result<void>
		authority_exact_members(const json_value& value,
								const std::array<StringLike, Size>& names,
								const std::string_view field)
		{
			std::array<std::string_view, Size> views{};
			for (std::size_t index{}; index < Size; ++index)
				views.at(index) = names.at(index);
			if (value.as_object() == nullptr || !value.has_exact_members(views))
				return sdk::unexpected(invalid(std::string{field}, "member-set"));
			return {};
		}

		[[nodiscard]] sdk::result<const json_value*> authority_member(const json_value& object,
																	  const std::string_view name,
																	  const std::string_view field)
		{
			const auto* value = object.member(name);
			if (value == nullptr)
				return sdk::unexpected(invalid(std::string{field}, std::string{name}));
			return value;
		}

		[[nodiscard]] sdk::result<std::string> authority_text(const json_value& value,
															  const std::string_view field)
		{
			const auto* text = value.as_string();
			if (text == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "string"));
			return *text;
		}

		[[nodiscard]] sdk::result<std::string> authority_member_text(const json_value& object,
																	 const std::string_view name,
																	 const std::string_view field)
		{
			auto value = authority_member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			return authority_text(**value, field);
		}

		[[nodiscard]] sdk::result<bool> authority_member_bool(const json_value& object,
															  const std::string_view name,
															  const std::string_view field)
		{
			auto value = authority_member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			const auto* boolean = (*value)->as_boolean();
			if (boolean == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "boolean"));
			return *boolean;
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		authority_unsigned(const json_value& value,
						   const std::string_view field,
						   const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
		{
			std::uint64_t output{};
			if (const auto* unsigned_value = value.as_unsigned_integer(); unsigned_value != nullptr)
				output = *unsigned_value;
			else if (const auto* signed_value = value.as_signed_integer();
					 signed_value != nullptr && *signed_value >= 0)
				output = static_cast<std::uint64_t>(*signed_value);
			else
				return sdk::unexpected(invalid(std::string{field}, "unsigned-integer"));
			if (output > maximum)
				return sdk::unexpected(invalid(std::string{field}, "integer-range"));
			return output;
		}

		[[nodiscard]] sdk::result<std::uint64_t> authority_member_unsigned(
			const json_value& object,
			const std::string_view name,
			const std::string_view field,
			const std::uint64_t maximum = std::numeric_limits<std::uint64_t>::max())
		{
			auto value = authority_member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			return authority_unsigned(**value, field, maximum);
		}

		[[nodiscard]] sdk::result<std::optional<std::string>> authority_member_nullable_text(
			const json_value& object, const std::string_view name, const std::string_view field)
		{
			auto value = authority_member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if ((*value)->is_null())
				return std::optional<std::string>{};
			auto text = authority_text(**value, field);
			if (!text)
				return sdk::unexpected(std::move(text.error()));
			return std::optional<std::string>{std::move(*text)};
		}

		[[nodiscard]] sdk::result<std::vector<std::string>>
		authority_string_array(const json_value& value, const std::string_view field)
		{
			const auto* array = value.as_array();
			if (array == nullptr)
				return sdk::unexpected(invalid(std::string{field}, "array"));
			if (array->size() > 4096U)
				return sdk::unexpected(invalid(std::string{field}, "count"));
			std::vector<std::string> output;
			output.reserve(array->size());
			for (const auto& item : *array)
			{
				auto text = authority_text(item, field);
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				output.push_back(std::move(*text));
			}
			return output;
		}

		[[nodiscard]] sdk::result<std::vector<std::string>> authority_member_string_array(
			const json_value& object, const std::string_view name, const std::string_view field)
		{
			auto value = authority_member(object, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			return authority_string_array(**value, field);
		}

		[[nodiscard]] sdk::result<sdk::semantic_version>
		authority_version(const json_value& value, const std::string_view field)
		{
			auto text = authority_text(value, field);
			if (!text)
				return sdk::unexpected(std::move(text.error()));
			sdk::semantic_version version;
			std::size_t begin{};
			for (std::size_t component{}; component < 3U; ++component)
			{
				const auto end = component == 2U ? text->size() : text->find('.', begin);
				if (end == std::string::npos || end == begin ||
					(end - begin > 1U && (*text)[begin] == '0'))
					return sdk::unexpected(invalid(std::string{field}, "semantic-version"));
				std::uint32_t parsed{};
				const auto result =
					std::from_chars(text->data() + begin, text->data() + end, parsed);
				if (result.ec != std::errc{} || result.ptr != text->data() + end)
					return sdk::unexpected(invalid(std::string{field}, "semantic-version"));
				if (component == 0U)
					version.major = parsed;
				else if (component == 1U)
					version.minor = parsed;
				else
					version.patch = parsed;
				begin = end + 1U;
			}
			if (begin != text->size() + 1U)
				return sdk::unexpected(invalid(std::string{field}, "semantic-version"));
			return version;
		}

		[[nodiscard]] sdk::result<sdk::provider::sandbox_assurance>
		authority_sandbox_assurance(const json_value& value, const std::string_view field)
		{
			auto text = authority_text(value, field);
			if (!text)
				return sdk::unexpected(std::move(text.error()));
			if (*text == "enforced")
				return sdk::provider::sandbox_assurance::enforced;
			if (*text == "certified")
				return sdk::provider::sandbox_assurance::certified;
			return sdk::unexpected(invalid(std::string{field}, "sandbox-assurance"));
		}

		[[nodiscard]] sdk::result<provider_task_v4_source>
		decode_task_source(const json_value& value)
		{
			constexpr std::array fields{"source_snapshot_id",
										"file_id",
										"logical_path",
										"content_digest",
										"size_bytes",
										"encoding",
										"line_index_id",
										"read_only"};
			if (auto valid = authority_exact_members(value, fields, "task.source"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_source output;
			for (auto item :
				 {std::pair{&output.source_snapshot_id, std::string_view{"source_snapshot_id"}},
				  std::pair{&output.file_id, std::string_view{"file_id"}},
				  std::pair{&output.logical_path, std::string_view{"logical_path"}},
				  std::pair{&output.content_digest, std::string_view{"content_digest"}},
				  std::pair{&output.encoding, std::string_view{"encoding"}},
				  std::pair{&output.line_index_id, std::string_view{"line_index_id"}}})
			{
				auto text = authority_member_text(value, item.second, "task.source");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			auto size = authority_member_unsigned(value, "size_bytes", "task.source.size_bytes");
			auto read_only = authority_member_bool(value, "read_only", "task.source.read_only");
			if (!size || !read_only)
				return sdk::unexpected(!size ? std::move(size.error())
											 : std::move(read_only.error()));
			output.size_bytes = *size;
			output.read_only = *read_only;
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_toolchain_authority>
		decode_toolchain(const json_value& value)
		{
			constexpr std::array fields{"family",
										"exact_version",
										"target_triple",
										"builtin_headers_digest",
										"sysroot",
										"abi_digest",
										"plugin_spec_digest"};
			if (auto valid = authority_exact_members(value, fields, "task.toolchain"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_toolchain_authority output;
			for (auto item :
				 {std::pair{&output.family, std::string_view{"family"}},
				  std::pair{&output.exact_version, std::string_view{"exact_version"}},
				  std::pair{&output.target_triple, std::string_view{"target_triple"}},
				  std::pair{&output.builtin_headers_digest,
							std::string_view{"builtin_headers_digest"}},
				  std::pair{&output.abi_digest, std::string_view{"abi_digest"}},
				  std::pair{&output.plugin_spec_digest, std::string_view{"plugin_spec_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "task.toolchain");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			auto sysroot =
				authority_member_nullable_text(value, "sysroot", "task.toolchain.sysroot");
			if (!sysroot)
				return sdk::unexpected(std::move(sysroot.error()));
			output.sysroot = std::move(*sysroot);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_variant_authority>
		decode_variant(const json_value& value)
		{
			constexpr std::array fields{"language",
										"language_standard",
										"target_triple",
										"predefined_macros_digest",
										"include_search_digest",
										"semantic_flags_digest"};
			if (auto valid = authority_exact_members(value, fields, "task.variant"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_variant_authority output;
			for (auto item :
				 {std::pair{&output.language, std::string_view{"language"}},
				  std::pair{&output.language_standard, std::string_view{"language_standard"}},
				  std::pair{&output.target_triple, std::string_view{"target_triple"}},
				  std::pair{&output.predefined_macros_digest,
							std::string_view{"predefined_macros_digest"}},
				  std::pair{&output.include_search_digest,
							std::string_view{"include_search_digest"}},
				  std::pair{&output.semantic_flags_digest,
							std::string_view{"semantic_flags_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "task.variant");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			return output;
		}

		[[nodiscard]] sdk::result<sdk::provider::execution_budget>
		decode_budget(const json_value& value)
		{
			constexpr std::array fields{"output_bytes",
										"rows",
										"diagnostics",
										"wall_ms",
										"cpu_ms",
										"address_space_bytes",
										"transport_bytes",
										"open_files",
										"subprocesses"};
			if (auto valid = authority_exact_members(value, fields, "task.budget"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			constexpr auto maximum =
				static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
			sdk::provider::execution_budget output;
			for (auto item :
				 {std::pair{&output.output_bytes, std::string_view{"output_bytes"}},
				  std::pair{&output.rows, std::string_view{"rows"}},
				  std::pair{&output.diagnostics, std::string_view{"diagnostics"}},
				  std::pair{&output.wall_ms, std::string_view{"wall_ms"}},
				  std::pair{&output.cpu_ms, std::string_view{"cpu_ms"}},
				  std::pair{&output.address_space_bytes, std::string_view{"address_space_bytes"}},
				  std::pair{&output.transport_bytes, std::string_view{"transport_bytes"}},
				  std::pair{&output.open_files, std::string_view{"open_files"}},
				  std::pair{&output.subprocesses, std::string_view{"subprocesses"}}})
			{
				auto number = authority_member_unsigned(value, item.second, "task.budget", maximum);
				if (!number)
					return sdk::unexpected(std::move(number.error()));
				*item.first = *number;
			}
			return output;
		}

		[[nodiscard]] sdk::result<sdk::provider::sandbox_requirement>
		decode_sandbox(const json_value& value, const std::string_view field)
		{
			constexpr std::array fields{"minimum", "policy_digest"};
			if (auto valid = authority_exact_members(value, fields, field); !valid)
				return sdk::unexpected(std::move(valid.error()));
			auto minimum = authority_member(value, "minimum", field);
			auto digest = authority_member_text(value, "policy_digest", field);
			if (!minimum || !digest)
				return sdk::unexpected(!minimum ? std::move(minimum.error())
												: std::move(digest.error()));
			auto assurance =
				authority_sandbox_assurance(**minimum, std::string{field} + ".minimum");
			if (!assurance)
				return sdk::unexpected(std::move(assurance.error()));
			return sdk::provider::sandbox_requirement{*assurance, std::move(*digest)};
		}

		[[nodiscard]] sdk::result<provider_task_v4_task_authority>
		decode_task_authority(const json_value& value)
		{
			constexpr std::array fields{"provider_task_id",
										"provider_execution_id",
										"task_input_digest",
										"project_id",
										"catalog_id",
										"catalog_digest",
										"selected_catalog_compile_unit_id",
										"compile_unit_id",
										"build_variant_id",
										"toolchain_context_id",
										"toolchain_digest",
										"toolchain",
										"variant",
										"normalized_invocation_digest",
										"environment_digest",
										"language",
										"working_directory",
										"condition_universe_id",
										"condition_id",
										"interpretation_domain",
										"source",
										"effective_argv",
										"qualified_read_roots",
										"requested_descriptor_ids",
										"dependency_groups",
										"budget",
										"sandbox"};
			if (auto valid = authority_exact_members(value, fields, "task"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_task_authority output;
			for (auto item :
				 {std::pair{&output.provider_task_id, std::string_view{"provider_task_id"}},
				  std::pair{&output.provider_execution_id,
							std::string_view{"provider_execution_id"}},
				  std::pair{&output.project_id, std::string_view{"project_id"}},
				  std::pair{&output.catalog_id, std::string_view{"catalog_id"}},
				  std::pair{&output.catalog_digest, std::string_view{"catalog_digest"}},
				  std::pair{&output.selected_catalog_compile_unit_id,
							std::string_view{"selected_catalog_compile_unit_id"}},
				  std::pair{&output.compile_unit_id, std::string_view{"compile_unit_id"}},
				  std::pair{&output.build_variant_id, std::string_view{"build_variant_id"}},
				  std::pair{&output.toolchain_context_id, std::string_view{"toolchain_context_id"}},
				  std::pair{&output.toolchain_digest, std::string_view{"toolchain_digest"}},
				  std::pair{&output.task_input_digest, std::string_view{"task_input_digest"}},
				  std::pair{&output.normalized_invocation_digest,
							std::string_view{"normalized_invocation_digest"}},
				  std::pair{&output.environment_digest, std::string_view{"environment_digest"}},
				  std::pair{&output.language, std::string_view{"language"}},
				  std::pair{&output.working_directory, std::string_view{"working_directory"}},
				  std::pair{&output.condition_universe_id,
							std::string_view{"condition_universe_id"}},
				  std::pair{&output.condition_id, std::string_view{"condition_id"}},
				  std::pair{&output.interpretation_domain,
							std::string_view{"interpretation_domain"}}})
			{
				auto text = authority_member_text(value, item.second, "task");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			auto toolchain = authority_member(value, "toolchain", "task.toolchain");
			auto variant = authority_member(value, "variant", "task.variant");
			auto source = authority_member(value, "source", "task.source");
			auto budget = authority_member(value, "budget", "task.budget");
			auto sandbox = authority_member(value, "sandbox", "task.sandbox");
			if (!toolchain || !variant || !source || !budget || !sandbox)
				return sdk::unexpected(!toolchain	  ? std::move(toolchain.error())
										   : !variant ? std::move(variant.error())
										   : !source  ? std::move(source.error())
										   : !budget  ? std::move(budget.error())
													  : std::move(sandbox.error()));
			auto decoded_toolchain = decode_toolchain(**toolchain);
			auto decoded_variant = decode_variant(**variant);
			auto decoded_source = decode_task_source(**source);
			auto decoded_budget = decode_budget(**budget);
			auto decoded_sandbox = decode_sandbox(**sandbox, "task.sandbox");
			if (!decoded_toolchain || !decoded_variant || !decoded_source || !decoded_budget ||
				!decoded_sandbox)
				return sdk::unexpected(!decoded_toolchain	  ? std::move(decoded_toolchain.error())
										   : !decoded_variant ? std::move(decoded_variant.error())
										   : !decoded_source  ? std::move(decoded_source.error())
										   : !decoded_budget  ? std::move(decoded_budget.error())
															  : std::move(decoded_sandbox.error()));
			output.toolchain = std::move(*decoded_toolchain);
			output.variant = std::move(*decoded_variant);
			output.source = std::move(*decoded_source);
			output.budget = *decoded_budget;
			output.sandbox = std::move(*decoded_sandbox);
			auto arguments =
				authority_member_string_array(value, "effective_argv", "task.effective_argv");
			auto roots = authority_member_string_array(
				value, "qualified_read_roots", "task.qualified_read_roots");
			auto descriptors = authority_member_string_array(
				value, "requested_descriptor_ids", "task.requested_descriptor_ids");
			auto groups =
				authority_member_string_array(value, "dependency_groups", "task.dependency_groups");
			if (!arguments || !roots || !descriptors || !groups)
				return sdk::unexpected(!arguments		  ? std::move(arguments.error())
										   : !roots		  ? std::move(roots.error())
										   : !descriptors ? std::move(descriptors.error())
														  : std::move(groups.error()));
			output.input_authority = {output.normalized_invocation_digest,
									  output.working_directory,
									  std::move(*arguments),
									  std::move(*roots)};
			output.requested_descriptor_ids = std::move(*descriptors);
			output.dependency_groups = std::move(*groups);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_catalog_authority>
		decode_catalog(const json_value& value)
		{
			constexpr std::array fields{"project_id",
										"catalog_id",
										"catalog_digest",
										"logical_root",
										"catalog_environment_digest",
										"catalog_compile_unit_census_digest",
										"catalog_compile_units"};
			if (auto valid = authority_exact_members(value, fields, "project"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_catalog_authority output;
			auto project_id = authority_member_text(value, "project_id", "project.project_id");
			auto catalog_id = authority_member_text(value, "catalog_id", "project.catalog_id");
			auto catalog_digest =
				authority_member_text(value, "catalog_digest", "project.catalog_digest");
			auto logical_root =
				authority_member_text(value, "logical_root", "project.logical_root");
			auto environment = authority_member_text(
				value, "catalog_environment_digest", "project.catalog_environment_digest");
			auto census = authority_member_text(value,
												"catalog_compile_unit_census_digest",
												"project.catalog_compile_unit_census_digest");
			if (!project_id || !catalog_id || !catalog_digest || !logical_root || !environment ||
				!census)
			{
				return sdk::unexpected(!project_id			 ? std::move(project_id.error())
										   : !catalog_id	 ? std::move(catalog_id.error())
										   : !catalog_digest ? std::move(catalog_digest.error())
										   : !logical_root	 ? std::move(logical_root.error())
										   : !environment	 ? std::move(environment.error())
															 : std::move(census.error()));
			}
			auto units_value =
				authority_member(value, "catalog_compile_units", "project.catalog_compile_units");
			if (!units_value || (*units_value)->as_array() == nullptr)
			{
				return sdk::unexpected(!units_value
										   ? std::move(units_value.error())
										   : invalid("project.catalog_compile_units", "array"));
			}
			if ((*units_value)->as_array()->size() > 4096U)
				return sdk::unexpected(invalid("project.catalog_compile_units", "count"));
			std::vector<sdk::catalog_compile_unit> units;
			units.reserve((*units_value)->as_array()->size());
			for (const auto& item : *(*units_value)->as_array())
			{
				constexpr std::array unit_fields{std::string_view{"catalog_compile_unit_id"},
												 std::string_view{"effective_invocation_digest"},
												 std::string_view{"source_digest"},
												 std::string_view{"environment_digest"}};
				if (auto valid =
						authority_exact_members(item, unit_fields, "project.catalog_compile_unit");
					!valid)
					return sdk::unexpected(std::move(valid.error()));
				auto unit_id = authority_member_text(
					item, "catalog_compile_unit_id", "catalog_compile_unit_id");
				auto invocation = authority_member_text(
					item, "effective_invocation_digest", "effective_invocation_digest");
				auto source = authority_member_text(item, "source_digest", "source_digest");
				auto unit_environment =
					authority_member_text(item, "environment_digest", "environment_digest");
				if (!unit_id || !invocation || !source || !unit_environment)
					return sdk::unexpected(!unit_id			 ? std::move(unit_id.error())
											   : !invocation ? std::move(invocation.error())
											   : !source	 ? std::move(source.error())
															 : std::move(unit_environment.error()));
				units.push_back({std::move(*unit_id),
								 std::move(*invocation),
								 std::move(*source),
								 std::move(*unit_environment)});
			}
			auto catalog = sdk::project_catalog::make(
				std::move(*logical_root), std::move(*environment), std::move(units));
			if (!catalog)
				return sdk::unexpected(std::move(catalog.error()));
			if (catalog->catalog_id != *catalog_id || catalog->catalog_digest != *catalog_digest)
				return sdk::unexpected(mismatch("project.catalog", "bottom-up-digest"));
			output.project_id = std::move(*project_id);
			output.catalog = std::move(*catalog);
			output.catalog_compile_unit_census_digest = std::move(*census);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_base_descriptor_binding>
		decode_base_descriptor(const json_value& value)
		{
			constexpr std::array fields{"descriptor_id",
										"descriptor_version",
										"contract_digest",
										"runtime_descriptor_digest",
										"stage_order",
										"output_stage",
										"owner"};
			if (auto valid = authority_exact_members(value, fields, "registry.base_descriptor");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_base_descriptor_binding output;
			auto id = authority_member_text(value, "descriptor_id", "registry.base_descriptor");
			auto version =
				authority_member(value, "descriptor_version", "registry.base_descriptor");
			auto contract =
				authority_member_text(value, "contract_digest", "registry.base_descriptor");
			auto runtime = authority_member_text(
				value, "runtime_descriptor_digest", "registry.base_descriptor");
			auto stage =
				authority_member_unsigned(value, "stage_order", "registry.base_descriptor");
			auto output_stage =
				authority_member_text(value, "output_stage", "registry.base_descriptor");
			auto owner = authority_member_text(value, "owner", "registry.base_descriptor");
			if (!id || !version || !contract || !runtime || !stage || !output_stage || !owner)
			{
				return sdk::unexpected(!id ? std::move(id.error())
										   : !version	   ? std::move(version.error())
										   : !contract	   ? std::move(contract.error())
										   : !runtime	   ? std::move(runtime.error())
										   : !stage		   ? std::move(stage.error())
										   : !output_stage ? std::move(output_stage.error())
														   : std::move(owner.error()));
			}
			if (*stage > std::numeric_limits<std::uint32_t>::max())
				return sdk::unexpected(invalid("registry.base_descriptor.stage_order", "range"));
			auto parsed_version =
				authority_version(**version, "registry.base_descriptor.descriptor_version");
			if (!parsed_version)
				return sdk::unexpected(std::move(parsed_version.error()));
			output.descriptor_id = std::move(*id);
			output.descriptor_version = *parsed_version;
			output.contract_digest = std::move(*contract);
			output.runtime_descriptor_digest = std::move(*runtime);
			output.stage_order = static_cast<std::uint32_t>(*stage);
			output.output_stage = std::move(*output_stage);
			output.owner = std::move(*owner);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_output_descriptor_binding>
		decode_output_descriptor(const json_value& value)
		{
			constexpr std::array fields{"descriptor_id",
										"descriptor_version",
										"contract_digest",
										"runtime_descriptor_digest",
										"dependency_group_id",
										"atomic_output_group_id",
										"batch_id",
										"output_stage"};
			if (auto valid = authority_exact_members(value, fields, "registry.descriptor"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_output_descriptor_binding output;
			auto id = authority_member_text(value, "descriptor_id", "registry.descriptor");
			auto version = authority_member(value, "descriptor_version", "registry.descriptor");
			auto contract = authority_member_text(value, "contract_digest", "registry.descriptor");
			auto runtime =
				authority_member_text(value, "runtime_descriptor_digest", "registry.descriptor");
			auto group = authority_member_text(value, "dependency_group_id", "registry.descriptor");
			auto atomic =
				authority_member_text(value, "atomic_output_group_id", "registry.descriptor");
			auto batch = authority_member_text(value, "batch_id", "registry.descriptor");
			auto stage = authority_member_text(value, "output_stage", "registry.descriptor");
			if (!id || !version || !contract || !runtime || !group || !atomic || !batch || !stage)
				return sdk::unexpected(!id ? std::move(id.error())
										   : !version  ? std::move(version.error())
										   : !contract ? std::move(contract.error())
										   : !runtime  ? std::move(runtime.error())
										   : !group	   ? std::move(group.error())
										   : !atomic   ? std::move(atomic.error())
										   : !batch	   ? std::move(batch.error())
													   : std::move(stage.error()));
			auto parsed_version =
				authority_version(**version, "registry.descriptor.descriptor_version");
			if (!parsed_version)
				return sdk::unexpected(std::move(parsed_version.error()));
			output.descriptor_id = std::move(*id);
			output.descriptor_version = *parsed_version;
			output.contract_digest = std::move(*contract);
			output.runtime_descriptor_digest = std::move(*runtime);
			output.dependency_group_id = std::move(*group);
			output.atomic_output_group_id = std::move(*atomic);
			output.batch_id = std::move(*batch);
			output.output_stage = std::move(*stage);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_admitted_descriptor_binding>
		decode_admitted_descriptor(const json_value& value)
		{
			constexpr std::array fields{"descriptor_id", "runtime_descriptor_digest"};
			if (auto valid = authority_exact_members(value, fields, "engine.admitted_descriptor");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			auto id = authority_member_text(value, "descriptor_id", "engine.admitted_descriptor");
			auto digest = authority_member_text(
				value, "runtime_descriptor_digest", "engine.admitted_descriptor");
			if (!id || !digest)
				return sdk::unexpected(!id ? std::move(id.error()) : std::move(digest.error()));
			return provider_task_v4_admitted_descriptor_binding{std::move(*id), std::move(*digest)};
		}

		[[nodiscard]] sdk::result<provider_task_v4_tool_authority>
		decode_tool_authority(const json_value& value)
		{
			constexpr std::array fields{"executable",
										"interface_version",
										"distribution_version",
										"source_revision",
										"source_tree",
										"installed_executable_digest",
										"package_configuration",
										"occurrence_manifest_digest"};
			if (auto valid = authority_exact_members(value, fields, "tool"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_tool_authority output;
			for (auto item :
				 {std::pair{&output.executable, std::string_view{"executable"}},
				  std::pair{&output.interface_version, std::string_view{"interface_version"}},
				  std::pair{&output.distribution_version, std::string_view{"distribution_version"}},
				  std::pair{&output.source_revision, std::string_view{"source_revision"}},
				  std::pair{&output.source_tree, std::string_view{"source_tree"}},
				  std::pair{&output.installed_executable_digest,
							std::string_view{"installed_executable_digest"}},
				  std::pair{&output.package_configuration,
							std::string_view{"package_configuration"}},
				  std::pair{&output.occurrence_manifest_digest,
							std::string_view{"occurrence_manifest_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "tool");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_worker_authority>
		decode_worker_authority(const json_value& value)
		{
			constexpr std::array fields{"executable",
										"provider_id",
										"provider_version",
										"installed_binary_digest",
										"semantic_contract_digest",
										"protocol_major",
										"protocol_minor",
										"required_features",
										"sandbox_policy_digest"};
			if (auto valid = authority_exact_members(value, fields, "worker"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_worker_authority output;
			for (auto item : {std::pair{&output.executable, std::string_view{"executable"}},
							  std::pair{&output.provider_id, std::string_view{"provider_id"}},
							  std::pair{&output.installed_binary_digest,
										std::string_view{"installed_binary_digest"}},
							  std::pair{&output.semantic_contract_digest,
										std::string_view{"semantic_contract_digest"}},
							  std::pair{&output.sandbox_policy_digest,
										std::string_view{"sandbox_policy_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "worker");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			auto version = authority_member(value, "provider_version", "worker.provider_version");
			auto major =
				authority_member_unsigned(value, "protocol_major", "worker.protocol_major");
			auto minor =
				authority_member_unsigned(value, "protocol_minor", "worker.protocol_minor");
			auto features = authority_member_string_array(
				value, "required_features", "worker.required_features");
			if (!version || !major || !minor || !features)
			{
				return sdk::unexpected(!version		? std::move(version.error())
										   : !major ? std::move(major.error())
										   : !minor ? std::move(minor.error())
													: std::move(features.error()));
			}
			auto parsed_version = authority_version(**version, "worker.provider_version");
			if (!parsed_version)
				return sdk::unexpected(std::move(parsed_version.error()));
			if (*major > std::numeric_limits<std::uint16_t>::max() ||
				*minor > std::numeric_limits<std::uint16_t>::max())
				return sdk::unexpected(invalid("worker.protocol", "range"));
			output.provider_version = *parsed_version;
			output.protocol_major = static_cast<std::uint16_t>(*major);
			output.protocol_minor = static_cast<std::uint16_t>(*minor);
			output.required_features = std::move(*features);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_interpretation_authority>
		decode_interpretation(const json_value& value)
		{
			constexpr std::array fields{
				"policy_id", "selected_domain", "interpretation_policy_digest"};
			if (auto valid = authority_exact_members(value, fields, "interpretation_policy");
				!valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_interpretation_authority output;
			auto policy =
				authority_member_text(value, "policy_id", "interpretation_policy.policy_id");
			auto domain = authority_member_text(
				value, "selected_domain", "interpretation_policy.selected_domain");
			auto digest = authority_member_text(
				value, "interpretation_policy_digest", "interpretation_policy.digest");
			if (!policy || !domain || !digest)
				return sdk::unexpected(!policy		 ? std::move(policy.error())
										   : !domain ? std::move(domain.error())
													 : std::move(digest.error()));
			output.policy_id = std::move(*policy);
			output.selected_domain = std::move(*domain);
			output.interpretation_policy_digest = std::move(*digest);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_group_topology_authority>
		decode_group_topology(const json_value& value)
		{
			constexpr std::array fields{
				"dependency_groups", "atomic_output_group", "partial_policy"};
			if (auto valid = authority_exact_members(value, fields, "group_topology"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_group_topology_authority output;
			auto groups = authority_member_string_array(
				value, "dependency_groups", "group_topology.dependency_groups");
			auto atomic = authority_member_text(
				value, "atomic_output_group", "group_topology.atomic_output_group");
			auto partial =
				authority_member_text(value, "partial_policy", "group_topology.partial_policy");
			if (!groups || !atomic || !partial)
				return sdk::unexpected(!groups		 ? std::move(groups.error())
										   : !atomic ? std::move(atomic.error())
													 : std::move(partial.error()));
			output.dependency_groups = std::move(*groups);
			output.atomic_output_group = std::move(*atomic);
			output.partial_policy = std::move(*partial);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_trust_authority>
		decode_trust_policy(const json_value& value)
		{
			constexpr std::array fields{"policy_id",
										"execution_profile",
										"provider_id",
										"provider_version",
										"semantic_contract_digest",
										"protocol_major",
										"protocol_minor",
										"required_features",
										"required_qualification",
										"worker_sandbox_policy_digest",
										"task_sandbox_requirements",
										"trust_policy_digest"};
			if (auto valid = authority_exact_members(value, fields, "trust_policy"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_trust_authority output;
			for (auto item :
				 {std::pair{&output.policy_id, std::string_view{"policy_id"}},
				  std::pair{&output.execution_profile, std::string_view{"execution_profile"}},
				  std::pair{&output.provider_id, std::string_view{"provider_id"}},
				  std::pair{&output.semantic_contract_digest,
							std::string_view{"semantic_contract_digest"}},
				  std::pair{&output.required_qualification,
							std::string_view{"required_qualification"}},
				  std::pair{&output.worker_sandbox_policy_digest,
							std::string_view{"worker_sandbox_policy_digest"}},
				  std::pair{&output.trust_policy_digest, std::string_view{"trust_policy_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "trust_policy");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			auto version =
				authority_member(value, "provider_version", "trust_policy.provider_version");
			auto major =
				authority_member_unsigned(value, "protocol_major", "trust_policy.protocol_major");
			auto minor =
				authority_member_unsigned(value, "protocol_minor", "trust_policy.protocol_minor");
			auto features = authority_member_string_array(
				value, "required_features", "trust_policy.required_features");
			if (!version || !major || !minor || !features)
			{
				return sdk::unexpected(!version		? std::move(version.error())
										   : !major ? std::move(major.error())
										   : !minor ? std::move(minor.error())
													: std::move(features.error()));
			}
			auto parsed_version = authority_version(**version, "trust_policy.provider_version");
			if (!parsed_version)
				return sdk::unexpected(std::move(parsed_version.error()));
			if (*major > std::numeric_limits<std::uint16_t>::max() ||
				*minor > std::numeric_limits<std::uint16_t>::max())
				return sdk::unexpected(invalid("trust_policy.protocol", "range"));
			output.provider_version = *parsed_version;
			output.protocol_major = static_cast<std::uint16_t>(*major);
			output.protocol_minor = static_cast<std::uint16_t>(*minor);
			output.required_features = std::move(*features);
			auto requirements = authority_member(
				value, "task_sandbox_requirements", "trust_policy.task_sandbox_requirements");
			if (!requirements || (*requirements)->as_array() == nullptr)
				return sdk::unexpected(
					!requirements ? std::move(requirements.error())
								  : invalid("trust_policy.task_sandbox_requirements", "array"));
			if ((*requirements)->as_array()->size() > 4096U)
				return sdk::unexpected(invalid("trust_policy.task_sandbox_requirements", "count"));
			output.task_sandbox_requirements.reserve((*requirements)->as_array()->size());
			for (const auto& requirement : *(*requirements)->as_array())
			{
				auto decoded =
					decode_sandbox(requirement, "trust_policy.task_sandbox_requirements");
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.task_sandbox_requirements.push_back(std::move(*decoded));
			}
			return output;
		}

		[[nodiscard]] sdk::result<sdk::snapshot_series_selector>
		decode_selector(const json_value& value)
		{
			constexpr std::array fields{"catalog_id",
										"channel_id",
										"engine_generation_id",
										"condition_universe_id",
										"relation_registry_digest",
										"interpretation_policy_digest",
										"trust_policy_digest"};
			if (auto valid = authority_exact_members(value, fields, "publication.selector"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			sdk::snapshot_series_selector output;
			for (auto item :
				 {std::pair{&output.catalog_id, std::string_view{"catalog_id"}},
				  std::pair{&output.channel_id, std::string_view{"channel_id"}},
				  std::pair{&output.engine_generation_id, std::string_view{"engine_generation_id"}},
				  std::pair{&output.condition_universe_id,
							std::string_view{"condition_universe_id"}},
				  std::pair{&output.relation_registry_digest,
							std::string_view{"relation_registry_digest"}},
				  std::pair{&output.interpretation_policy_digest,
							std::string_view{"interpretation_policy_digest"}},
				  std::pair{&output.trust_policy_digest, std::string_view{"trust_policy_digest"}}})
			{
				auto text = authority_member_text(value, item.second, "publication.selector");
				if (!text)
					return sdk::unexpected(std::move(text.error()));
				*item.first = std::move(*text);
			}
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_publication_authority>
		decode_publication(const json_value& value)
		{
			constexpr std::array fields{"backend",
										"selector",
										"series_id",
										"genesis",
										"expected_parent_publication",
										"sqlite_path",
										"partial_policy",
										"transaction_count",
										"reopen_before_success",
										"recipe_id",
										"recipe_digest",
										"output_plan_digest",
										"publication_target"};
			if (auto valid = authority_exact_members(value, fields, "publication"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_publication_authority output;
			auto backend = authority_member_text(value, "backend", "publication.backend");
			auto selector = authority_member(value, "selector", "publication.selector");
			auto series = authority_member_text(value, "series_id", "publication.series_id");
			auto genesis = authority_member_bool(value, "genesis", "publication.genesis");
			auto parent = authority_member_nullable_text(
				value, "expected_parent_publication", "publication.expected_parent_publication");
			auto sqlite =
				authority_member_nullable_text(value, "sqlite_path", "publication.sqlite_path");
			auto partial =
				authority_member_text(value, "partial_policy", "publication.partial_policy");
			auto transactions = authority_member_unsigned(
				value, "transaction_count", "publication.transaction_count");
			auto reopen = authority_member_bool(
				value, "reopen_before_success", "publication.reopen_before_success");
			auto recipe_id = authority_member_text(value, "recipe_id", "publication.recipe_id");
			auto recipe_digest =
				authority_member_text(value, "recipe_digest", "publication.recipe_digest");
			auto output_plan = authority_member_text(
				value, "output_plan_digest", "publication.output_plan_digest");
			auto target = authority_member_text(
				value, "publication_target", "publication.publication_target");
			if (!backend || !selector || !series || !genesis || !parent || !sqlite || !partial ||
				!transactions || !reopen || !recipe_id || !recipe_digest || !output_plan || !target)
			{
				return sdk::unexpected(!backend				? std::move(backend.error())
										   : !selector		? std::move(selector.error())
										   : !series		? std::move(series.error())
										   : !genesis		? std::move(genesis.error())
										   : !parent		? std::move(parent.error())
										   : !sqlite		? std::move(sqlite.error())
										   : !partial		? std::move(partial.error())
										   : !transactions	? std::move(transactions.error())
										   : !reopen		? std::move(reopen.error())
										   : !recipe_id		? std::move(recipe_id.error())
										   : !recipe_digest ? std::move(recipe_digest.error())
										   : !output_plan	? std::move(output_plan.error())
															: std::move(target.error()));
			}
			auto decoded_selector = decode_selector(**selector);
			if (!decoded_selector)
				return sdk::unexpected(std::move(decoded_selector.error()));
			output.backend = std::move(*backend);
			output.selector = std::move(*decoded_selector);
			output.series_id = std::move(*series);
			output.genesis = *genesis;
			output.expected_parent_publication = std::move(*parent);
			output.sqlite_path = std::move(*sqlite);
			output.partial_policy = std::move(*partial);
			output.transaction_count = *transactions;
			output.reopen_before_success = *reopen;
			output.recipe_id = std::move(*recipe_id);
			output.recipe_digest = std::move(*recipe_digest);
			output.output_plan_digest = std::move(*output_plan);
			output.publication_target = std::move(*target);
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_registry_authority>
		decode_registry(const json_value& value)
		{
			constexpr std::array fields{
				"path", "authority_registry_digest", "base_descriptors", "descriptors"};
			if (auto valid = authority_exact_members(value, fields, "registry"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_registry_authority output;
			auto path = authority_member_text(value, "path", "registry.path");
			auto digest = authority_member_text(
				value, "authority_registry_digest", "registry.authority_registry_digest");
			auto base = authority_member(value, "base_descriptors", "registry.base_descriptors");
			auto descriptors = authority_member(value, "descriptors", "registry.descriptors");
			if (!path || !digest || !base || !descriptors)
				return sdk::unexpected(!path		 ? std::move(path.error())
										   : !digest ? std::move(digest.error())
										   : !base	 ? std::move(base.error())
													 : std::move(descriptors.error()));
			if ((*base)->as_array() == nullptr || (*descriptors)->as_array() == nullptr)
				return sdk::unexpected(invalid("registry", "descriptor-arrays"));
			if ((*base)->as_array()->size() > 4096U || (*descriptors)->as_array()->size() > 4096U)
				return sdk::unexpected(invalid("registry", "descriptor-count"));
			output.path = std::move(*path);
			output.authority_registry_digest = std::move(*digest);
			output.base_descriptors.reserve((*base)->as_array()->size());
			for (const auto& item : *(*base)->as_array())
			{
				auto decoded = decode_base_descriptor(item);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.base_descriptors.push_back(std::move(*decoded));
			}
			output.descriptors.reserve((*descriptors)->as_array()->size());
			for (const auto& item : *(*descriptors)->as_array())
			{
				auto decoded = decode_output_descriptor(item);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.descriptors.push_back(std::move(*decoded));
			}
			return output;
		}

		[[nodiscard]] sdk::result<provider_task_v4_engine_authority>
		decode_engine(const json_value& value)
		{
			constexpr std::array fields{"generation_contract",
										"admitted_descriptors",
										"engine_registry_digest",
										"engine_generation_id"};
			if (auto valid = authority_exact_members(value, fields, "engine"); !valid)
				return sdk::unexpected(std::move(valid.error()));
			provider_task_v4_engine_authority output;
			auto contract =
				authority_member_text(value, "generation_contract", "engine.generation_contract");
			auto digest = authority_member_text(
				value, "engine_registry_digest", "engine.engine_registry_digest");
			auto generation =
				authority_member_text(value, "engine_generation_id", "engine.engine_generation_id");
			auto admitted =
				authority_member(value, "admitted_descriptors", "engine.admitted_descriptors");
			if (!contract || !digest || !generation || !admitted)
				return sdk::unexpected(!contract		 ? std::move(contract.error())
										   : !digest	 ? std::move(digest.error())
										   : !generation ? std::move(generation.error())
														 : std::move(admitted.error()));
			if ((*admitted)->as_array() == nullptr)
				return sdk::unexpected(invalid("engine.admitted_descriptors", "array"));
			if ((*admitted)->as_array()->size() > 4096U)
				return sdk::unexpected(invalid("engine.admitted_descriptors", "count"));
			output.generation_contract = std::move(*contract);
			output.engine_registry_digest = std::move(*digest);
			output.engine_generation_id = std::move(*generation);
			output.admitted_descriptors.reserve((*admitted)->as_array()->size());
			for (const auto& item : *(*admitted)->as_array())
			{
				auto decoded = decode_admitted_descriptor(item);
				if (!decoded)
					return sdk::unexpected(std::move(decoded.error()));
				output.admitted_descriptors.push_back(std::move(*decoded));
			}
			return output;
		}

		[[nodiscard]] sdk::result<void> validate_document_closures(const json_value& root)
		{
			const auto* closures = root.member("source_closures");
			if (closures == nullptr || closures->as_array() == nullptr ||
				closures->as_array()->empty() || closures->as_array()->size() > 4096U)
				return sdk::unexpected(invalid("source_closures", "count"));
			constexpr std::array<std::string_view, 6U> fields{"blob_count",
															  "manifest_digest",
															  "member_count",
															  "source_closure_digest",
															  "source_closure_id",
															  "unique_blob_bytes"};
			for (const auto& closure : *closures->as_array())
			{
				if (auto shape = document_member_set(closure, fields, "source_closures"); !shape)
					return shape;
				for (const auto name :
					 {"manifest_digest", "source_closure_digest", "source_closure_id"})
					if (auto type = document_member_kind(
							closure, name, json_value::kind::string, "source_closures");
						!type)
						return type;
				for (const auto name : {"blob_count", "member_count", "unique_blob_bytes"})
					if (auto type = document_unsigned_member(closure, name, "source_closures");
						!type)
						return type;
			}
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_document_task_extensions(const json_value& root)
		{
			const auto* tasks = root.member("tasks");
			const auto* extensions = root.member("task_extensions");
			if (tasks == nullptr || extensions == nullptr || tasks->as_array() == nullptr ||
				extensions->as_array() == nullptr || tasks->as_array()->empty() ||
				tasks->as_array()->size() > 4096U ||
				tasks->as_array()->size() != extensions->as_array()->size())
				return sdk::unexpected(invalid("tasks", "count-or-census"));
			constexpr std::array<std::string_view, 8U> source_fields{"content_digest",
																	 "encoding",
																	 "file_id",
																	 "line_index_id",
																	 "logical_path",
																	 "read_only",
																	 "size_bytes",
																	 "source_snapshot_id"};
			for (const auto& task : *tasks->as_array())
			{
				if (task.as_object() == nullptr)
					return sdk::unexpected(invalid("tasks", "object"));
				const auto* source = task.member("source");
				if (source == nullptr)
					return sdk::unexpected(invalid("tasks.source", "member-set"));
				if (auto shape = document_member_set(*source, source_fields, "tasks.source");
					!shape)
					return shape;
			}
			constexpr std::array<std::string_view, 10U> extension_fields{
				"base_provider_task_id",
				"base_task_digest",
				"base_task_index",
				"logical_working_directory",
				"main_logical_path",
				"open_task",
				"schema",
				"source_closure",
				"task_id",
				"task_v4_digest"};
			constexpr std::array<std::string_view, 4U> open_fields{"environment_digest",
																   "normalized_invocation_digest",
																   "task_input_digest",
																   "toolchain_digest"};
			constexpr std::array<std::string_view, 3U> closure_fields{
				"digest", "id", "manifest_digest"};
			for (const auto& extension : *extensions->as_array())
			{
				if (auto shape =
						document_member_set(extension, extension_fields, "task_extensions");
					!shape)
					return shape;
				const auto* open = extension.member("open_task");
				const auto* closure = extension.member("source_closure");
				if (open == nullptr || closure == nullptr)
					return sdk::unexpected(invalid("task_extensions", "nested-member"));
				if (auto shape =
						document_member_set(*open, open_fields, "task_extensions.open_task");
					!shape)
					return shape;
				if (auto shape = document_member_set(
						*closure, closure_fields, "task_extensions.source_closure");
					!shape)
					return shape;
			}
			return {};
		}

		[[nodiscard]] sdk::result<provider_task_v4_request_authority>
		validate_inherited_authority(const materialization_request_v2_2& request)
		{
			auto decoded = decode_provider_task_v4_request_authority(request.inherited_authority);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			const auto* object = request.inherited_authority.as_object();
			if (object == nullptr)
				return sdk::unexpected(invalid("inherited_authority", "object-required"));
			const auto* request_id =
				request.inherited_authority.member("materialization_request_id");
			const auto* request_digest =
				request.inherited_authority.member("semantic_request_digest");
			if (request_id == nullptr || request_digest == nullptr ||
				request_id->as_string() == nullptr || request_digest->as_string() == nullptr ||
				*request_id->as_string() != request.materialization_request_id ||
				*request_digest->as_string() != request.semantic_request_digest)
				return sdk::unexpected(mismatch("inherited_authority.identity"));
			const auto* tasks = request.inherited_authority.member("tasks");
			if (tasks == nullptr || tasks->as_array() == nullptr ||
				tasks->as_array()->size() != request.base_tasks.size())
				return sdk::unexpected(mismatch("inherited_authority.tasks"));
			return std::move(*decoded);
		}

		[[nodiscard]] sdk::result<void>
		validate_feature_list(const std::span<const std::string> features)
		{
			std::set<std::string, std::less<>> unique;
			for (const auto& feature : features)
				if (feature.empty() || !unique.insert(feature).second)
					return sdk::unexpected(invalid("required_features", "duplicate-or-empty"));
			return {};
		}

		[[nodiscard]] sdk::result<std::uint64_t>
		aggregate_closure_bytes(const std::span<const source_closure_summary> closures,
								const std::uint64_t maximum)
		{
			std::uint64_t total{};
			for (const auto& closure : closures)
			{
				if (closure.unique_blob_bytes > maximum ||
					total > maximum - closure.unique_blob_bytes)
					return sdk::unexpected(invalid("source_closures", "aggregate-size"));
				total += closure.unique_blob_bytes;
			}
			return total;
		}

		[[nodiscard]] const source_closure_manifest*
		find_manifest(const std::span<const source_closure_manifest> manifests,
					  const std::string_view id)
		{
			const auto found = std::ranges::find_if(manifests,
													[&](const auto& manifest)
													{
														return manifest.closure_id == id;
													});
			return found == manifests.end() ? nullptr : &*found;
		}

		[[nodiscard]] sdk::result<void>
		validate_request_shape(const materialization_request_v2_2& request,
							   const materialization_request_v2_2_limits limits)
		{
			if (request.schema != materialization_request_v2_2_schema ||
				request.request_version != materialization_request_v2_2_version)
				return sdk::unexpected(unsupported("request_version", request.request_version));
			if (request.protocol_major != materialization_protocol_v2_major ||
				request.protocol_minor != materialization_protocol_v2_minor)
				return sdk::unexpected(unsupported("protocol", "downgrade-or-unknown"));
			if (!semantic_digest_grammar(request.request_digest) ||
				!request_id_grammar(request.request_id) ||
				request.request_id != "materialization-request:" + request.request_digest ||
				!semantic_digest_grammar(request.semantic_request_digest) ||
				request.materialization_request_id.empty() ||
				!sdk::validate_strong_id(request.materialization_request_id))
				return sdk::unexpected(invalid("request.identity", "grammar"));
			if (request.source_closures.empty() ||
				request.source_closures.size() > limits.maximum_closures ||
				request.base_tasks.empty() || request.base_tasks.size() > limits.maximum_tasks ||
				request.task_extensions.size() != request.base_tasks.size())
				return sdk::unexpected(invalid("request", "count-or-census"));
			if (!std::ranges::equal(request.required_features,
									materialization_request_v2_2_required_features()))
				return sdk::unexpected(invalid("required_features", "contract"));
			for (const auto& base : request.base_tasks)
				if (auto valid = base.validate(limits.task_limits); !valid)
					return valid;
			return {};
		}
	} // namespace

	sdk::result<provider_task_v4_request_authority>
	decode_provider_task_v4_request_authority(const json_value& root)
	{
		if (root.as_object() == nullptr)
			return sdk::unexpected(invalid("authority", "object-required"));
		if (contains_forbidden_source_bytes(root))
			return sdk::unexpected(invalid("authority", "source-bytes-forbidden"));
		constexpr std::array authority_fields{"tool",
											  "worker",
											  "project",
											  "registry",
											  "engine",
											  "interpretation_policy",
											  "trust_policy",
											  "group_topology",
											  "tasks",
											  "publication"};
		for (const auto name : authority_fields)
			if (root.member(name) == nullptr)
				return sdk::unexpected(invalid("authority", "missing:" + std::string{name}));

		auto tool = authority_member(root, "tool", "tool");
		auto worker = authority_member(root, "worker", "worker");
		auto project = authority_member(root, "project", "project");
		auto registry = authority_member(root, "registry", "registry");
		auto engine = authority_member(root, "engine", "engine");
		auto interpretation =
			authority_member(root, "interpretation_policy", "interpretation_policy");
		auto trust = authority_member(root, "trust_policy", "trust_policy");
		auto groups = authority_member(root, "group_topology", "group_topology");
		auto tasks = authority_member(root, "tasks", "tasks");
		auto publication = authority_member(root, "publication", "publication");
		if (!tool || !worker || !project || !registry || !engine || !interpretation || !trust ||
			!groups || !tasks || !publication)
		{
			return sdk::unexpected(!tool				 ? std::move(tool.error())
									   : !worker		 ? std::move(worker.error())
									   : !project		 ? std::move(project.error())
									   : !registry		 ? std::move(registry.error())
									   : !engine		 ? std::move(engine.error())
									   : !interpretation ? std::move(interpretation.error())
									   : !trust			 ? std::move(trust.error())
									   : !groups		 ? std::move(groups.error())
									   : !tasks			 ? std::move(tasks.error())
														 : std::move(publication.error()));
		}
		if ((*tasks)->as_array() == nullptr || (*tasks)->as_array()->empty() ||
			(*tasks)->as_array()->size() > 4096U)
			return sdk::unexpected(invalid("tasks", "array-or-empty"));

		auto decoded_tool = decode_tool_authority(**tool);
		auto decoded_worker = decode_worker_authority(**worker);
		auto decoded_project = decode_catalog(**project);
		auto decoded_registry = decode_registry(**registry);
		auto decoded_engine = decode_engine(**engine);
		auto decoded_interpretation = decode_interpretation(**interpretation);
		auto decoded_trust = decode_trust_policy(**trust);
		auto decoded_groups = decode_group_topology(**groups);
		auto decoded_publication = decode_publication(**publication);
		if (!decoded_tool || !decoded_worker || !decoded_project || !decoded_registry ||
			!decoded_engine || !decoded_interpretation || !decoded_trust || !decoded_groups ||
			!decoded_publication)
		{
			return sdk::unexpected(!decoded_tool		   ? std::move(decoded_tool.error())
									   : !decoded_worker   ? std::move(decoded_worker.error())
									   : !decoded_project  ? std::move(decoded_project.error())
									   : !decoded_registry ? std::move(decoded_registry.error())
									   : !decoded_engine   ? std::move(decoded_engine.error())
									   : !decoded_interpretation
									   ? std::move(decoded_interpretation.error())
									   : !decoded_trust	 ? std::move(decoded_trust.error())
									   : !decoded_groups ? std::move(decoded_groups.error())
														 : std::move(decoded_publication.error()));
		}

		provider_task_v4_request_authority output;
		output.tool = std::move(*decoded_tool);
		output.worker = std::move(*decoded_worker);
		output.project = std::move(*decoded_project);
		output.registry = std::move(*decoded_registry);
		output.engine = std::move(*decoded_engine);
		output.interpretation_policy = std::move(*decoded_interpretation);
		output.trust_policy = std::move(*decoded_trust);
		output.group_topology = std::move(*decoded_groups);
		output.publication = std::move(*decoded_publication);
		output.tasks.reserve((*tasks)->as_array()->size());
		for (const auto& task : *(*tasks)->as_array())
		{
			auto decoded = decode_task_authority(task);
			if (!decoded)
				return sdk::unexpected(std::move(decoded.error()));
			output.tasks.push_back(std::move(*decoded));
		}
		if (auto valid = output.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		return output;
	}

	sdk::result<std::vector<sdk::detail::validated_build_capture>>
	adapt_provider_task_v4_build_captures(const provider_task_v4_request_authority& authority,
										  const std::span<const provider_task_v4> task_extensions)
	{
		if (authority.tasks.size() != task_extensions.size())
			return sdk::unexpected(mismatch("build-captures", "task-census"));
		std::vector<sdk::detail::build_capture_draft> drafts;
		drafts.reserve(authority.tasks.size());
		for (std::size_t index{}; index < authority.tasks.size(); ++index)
		{
			const auto& task = authority.tasks[index];
			const auto& extension = task_extensions[index];
			sdk::detail::build_capture_draft draft;
			draft.project_id = task.project_id;
			draft.catalog = authority.project.catalog;
			draft.selected_catalog_compile_unit_id = task.selected_catalog_compile_unit_id;
			draft.compile_unit_id = task.compile_unit_id;
			draft.build_variant_id = task.build_variant_id;
			draft.toolchain_context_id = task.toolchain_context_id;
			draft.toolchain_digest = task.toolchain_digest;
			draft.toolchain = {
				task.toolchain.family,
				task.toolchain.exact_version,
				task.toolchain.target_triple,
				task.toolchain.builtin_headers_digest,
				task.toolchain.sysroot,
				task.toolchain.abi_digest,
				task.toolchain.plugin_spec_digest,
				sdk::detail::captured_value<std::string>::unavailable(
					"not-carried-by-clang22-request-v2_2", "capture-production-compiler-path"),
				sdk::detail::captured_value<std::string>::unavailable(
					"not-carried-by-clang22-request-v2_2",
					"capture-production-compiler-binary-digest")};
			draft.variant = {task.variant.language,
							 task.variant.language_standard,
							 task.variant.target_triple,
							 task.variant.predefined_macros_digest,
							 task.variant.include_search_digest,
							 task.variant.semantic_flags_digest};
			draft.invocation.original_arguments =
				sdk::detail::captured_value<std::vector<std::string>>::unavailable(
					"not-carried-by-clang22-request-v2_2", "recapture-original-argv");
			draft.invocation.normalized_semantic_options = sdk::detail::
				captured_value<std::vector<sdk::detail::normalized_build_option>>::unavailable(
					"not-carried-by-clang22-request-v2_2", "recapture-normalized-options");
			draft.invocation.effective_replay_arguments =
				sdk::detail::captured_value<std::vector<std::string>>::observed(
					task.input_authority.effective_arguments);
			draft.invocation.response_files = sdk::detail::
				captured_value<std::vector<sdk::detail::build_capture_auxiliary_file>>::unavailable(
					"not-carried-by-clang22-request-v2_2", "recapture-response-files");
			draft.invocation.config_files = sdk::detail::
				captured_value<std::vector<sdk::detail::build_capture_auxiliary_file>>::unavailable(
					"not-carried-by-clang22-request-v2_2", "recapture-config-files");
			draft.invocation.environment_effects = sdk::detail::captured_value<
				std::vector<sdk::detail::build_capture_environment_effect>>::
				unavailable("environment-values-not-carried-by-clang22-request-v2_2",
							"recapture-allowlisted-environment-effects");
			draft.invocation.effective_invocation_digest = task.normalized_invocation_digest;
			draft.invocation.environment_digest = task.environment_digest;
			draft.invocation.language = task.language;
			draft.invocation.logical_working_directory = task.working_directory;
			draft.invocation.qualified_read_roots = task.input_authority.qualified_read_roots;
			draft.source = {task.source.source_snapshot_id,
							task.source.file_id,
							task.source.logical_path,
							task.source.content_digest,
							task.source.size_bytes,
							task.source.encoding,
							task.source.line_index_id,
							task.source.read_only};
			draft.source_closure = {extension.source_closure.source_closure_id,
									extension.source_closure.source_closure_digest,
									extension.source_closure.manifest_digest,
									extension.source_closure.member_count,
									extension.source_closure.blob_count,
									extension.source_closure.unique_blob_bytes};
			drafts.push_back(std::move(draft));
		}
		return sdk::detail::validate_build_capture_set(std::move(drafts));
	}

	sdk::result<void> validate_provider_task_v4_build_capture_binding(
		const provider_task_v4_task_authority& task,
		const sdk::detail::validated_build_capture& capture)
	{
		const auto& value = capture.value();
		const provider_task_v4_toolchain_authority toolchain{value.toolchain.family,
															 value.toolchain.exact_version,
															 value.toolchain.target_triple,
															 value.toolchain.builtin_headers_digest,
															 value.toolchain.sysroot,
															 value.toolchain.abi_digest,
															 value.toolchain.plugin_spec_digest};
		const provider_task_v4_variant_authority variant{value.variant.language,
														 value.variant.language_standard,
														 value.variant.target_triple,
														 value.variant.predefined_macros_digest,
														 value.variant.include_search_digest,
														 value.variant.semantic_flags_digest};
		const provider_task_v4_source source{value.source.source_snapshot_id,
											 value.source.file_id,
											 value.source.logical_path,
											 value.source.content_digest,
											 value.source.size_bytes,
											 value.source.encoding,
											 value.source.line_index_id,
											 value.source.read_only};
		const provider_task_v4_input_authority input{
			value.invocation.effective_invocation_digest,
			value.invocation.logical_working_directory,
			*value.invocation.effective_replay_arguments.value,
			value.invocation.qualified_read_roots};
		if (task.project_id != value.project_id || task.catalog_id != value.catalog.catalog_id ||
			task.catalog_digest != value.catalog.catalog_digest ||
			task.selected_catalog_compile_unit_id != value.selected_catalog_compile_unit_id ||
			task.compile_unit_id != value.compile_unit_id ||
			task.build_variant_id != value.build_variant_id ||
			task.toolchain_context_id != value.toolchain_context_id ||
			task.toolchain_digest != value.toolchain_digest || task.toolchain != toolchain ||
			task.variant != variant ||
			task.normalized_invocation_digest != value.invocation.effective_invocation_digest ||
			task.environment_digest != value.invocation.environment_digest ||
			task.language != value.invocation.language ||
			task.working_directory != value.invocation.logical_working_directory ||
			task.source != source || task.input_authority != input)
			return sdk::unexpected(sdk::error{"materialization.build-capture-binding-mismatch",
											  "task",
											  "transport-generic-divergence"});
		return {};
	}

	std::vector<std::string> materialization_request_v2_2_required_features()
	{
		return {"task-input-chunks-v2", "task-source-closure-v2"};
	}

	sdk::result<void> validate_materialization_request_v2_2_document(const json_value& root)
	{
		if (root.as_object() == nullptr)
			return sdk::unexpected(invalid("request", "object-required"));
		constexpr std::array<std::string_view, 19U> fields{"engine",
														   "group_topology",
														   "interpretation_policy",
														   "materialization_request_id",
														   "publication",
														   "project",
														   "registry",
														   "request_digest",
														   "request_id",
														   "request_version",
														   "required_features",
														   "semantic_request_digest",
														   "schema",
														   "source_closures",
														   "task_extensions",
														   "tasks",
														   "tool",
														   "trust_policy",
														   "worker"};
		if (auto shape = document_member_set(root, fields, "request"); !shape)
			return shape;
		for (const auto name : {"request_id",
								"request_digest",
								"request_version",
								"schema",
								"materialization_request_id",
								"semantic_request_digest"})
			if (auto type = document_member_kind(root, name, json_value::kind::string, "request");
				!type)
				return type;
		for (const auto name : {"engine",
								"group_topology",
								"interpretation_policy",
								"publication",
								"project",
								"registry",
								"tool",
								"trust_policy",
								"worker"})
			if (auto type = document_member_kind(root, name, json_value::kind::object, "request");
				!type)
				return type;
		constexpr std::array<std::string_view, 2U> features{"task-input-chunks-v2",
															"task-source-closure-v2"};
		if (auto valid = document_string_array(root, "required_features", features, "request");
			!valid)
			return valid;
		const auto* worker = root.member("worker");
		if (worker == nullptr)
			return sdk::unexpected(invalid("worker", "missing"));
		if (auto valid = document_unsigned_member(*worker, "protocol_major", "worker"); !valid)
			return valid;
		if (auto valid = document_unsigned_member(*worker, "protocol_minor", "worker"); !valid)
			return valid;
		const auto* protocol_major = worker->member("protocol_major");
		const auto* protocol_minor = worker->member("protocol_minor");
		const auto major = protocol_major->as_unsigned_integer() != nullptr
			? *protocol_major->as_unsigned_integer()
			: static_cast<std::uint64_t>(*protocol_major->as_signed_integer());
		const auto minor = protocol_minor->as_unsigned_integer() != nullptr
			? *protocol_minor->as_unsigned_integer()
			: static_cast<std::uint64_t>(*protocol_minor->as_signed_integer());
		if (major != materialization_protocol_v2_major ||
			minor != materialization_protocol_v2_minor)
			return sdk::unexpected(unsupported("worker.protocol", "downgrade-or-unknown"));
		if (contains_forbidden_source_bytes(root))
			return sdk::unexpected(invalid("request", "source-bytes-forbidden"));
		if (auto authority = decode_provider_task_v4_request_authority(root); !authority)
			return sdk::unexpected(std::move(authority.error()));
		if (auto valid = validate_document_closures(root); !valid)
			return valid;
		return validate_document_task_extensions(root);
	}

	sdk::result<std::vector<std::string>>
	negotiate_materialization_request_v2_2(const std::uint16_t peer_protocol_major,
										   const std::uint16_t peer_protocol_minor,
										   const std::span<const std::string> advertised_features)
	{
		if (peer_protocol_major != materialization_protocol_v2_major ||
			peer_protocol_minor != materialization_protocol_v2_minor)
			return sdk::unexpected(unsupported("protocol", "downgrade-or-unknown"));
		if (auto valid = validate_feature_list(advertised_features); !valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto required = materialization_request_v2_2_required_features();
		for (const auto& feature : required)
			if (!std::ranges::contains(advertised_features, feature))
				return sdk::unexpected(feature_missing(feature));
		return required;
	}

	sdk::result<std::string>
	derive_materialization_request_v2_2_digest(const materialization_request_v2_2& request)
	{
		auto projection = request_projection(request);
		if (!projection)
			return sdk::unexpected(std::move(projection.error()));
		const auto canonical = canonical_json(*projection);
		return sdk::semantic_digest("cxxlens.clang22.materialization-request.v2_2", canonical);
	}

	sdk::result<validated_materialization_request_v2_2>
	validate_materialization_request_v2_2(materialization_request_v2_2 request,
										  const std::span<const std::string> advertised_features,
										  const materialization_request_v2_2_limits limits)
	{
		return validate_materialization_request_v2_2(std::move(request),
													 advertised_features,
													 std::span<const source_closure_manifest>{},
													 limits);
	}

	sdk::result<validated_materialization_request_v2_2>
	validate_materialization_request_v2_2(materialization_request_v2_2 request,
										  const std::span<const std::string> advertised_features,
										  const std::span<const source_closure_manifest> manifests,
										  const materialization_request_v2_2_limits limits)
	{
		if (limits.maximum_closures == 0U || limits.maximum_tasks == 0U ||
			limits.maximum_unique_blob_bytes == 0U)
			return sdk::unexpected(invalid("limits", "zero"));
		if (auto shape = validate_request_shape(request, limits); !shape)
			return sdk::unexpected(std::move(shape.error()));
		auto authority = validate_inherited_authority(request);
		if (!authority)
			return sdk::unexpected(std::move(authority.error()));
		auto negotiated = negotiate_materialization_request_v2_2(
			request.protocol_major, request.protocol_minor, advertised_features);
		if (!negotiated)
			return sdk::unexpected(std::move(negotiated.error()));
		std::set<std::string, std::less<>> closure_ids;
		for (const auto& closure : request.source_closures)
		{
			if (auto valid = closure.validate(limits.task_limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (!closure_ids.insert(closure.source_closure_id).second)
				return sdk::unexpected(invalid("source_closures", "duplicate-id"));
		}
		std::set<std::string, std::less<>> task_ids;
		std::set<std::uint64_t> task_indices;
		std::set<std::string, std::less<>> referenced_closures;
		for (std::size_t index{}; index < request.task_extensions.size(); ++index)
		{
			const auto& task = request.task_extensions[index];
			const auto& base = request.base_tasks[index];
			if (task.base_task_index != index || !task_ids.insert(task.task_id).second ||
				!task_indices.insert(task.base_task_index).second)
				return sdk::unexpected(invalid("task_extensions", "index-or-duplicate"));
			if (auto valid = task.validate(limits.task_limits); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (task.base_provider_task_id != base.provider_task_id ||
				task.base_task_digest != base.canonical_base_task_digest ||
				task.open_task.task_input_digest != base.task_input_digest ||
				task.open_task.normalized_invocation_digest != base.normalized_invocation_digest ||
				task.open_task.toolchain_digest != base.toolchain_digest ||
				task.open_task.environment_digest != base.environment_digest ||
				task.main_logical_path != base.source.logical_path ||
				task.logical_working_directory != base.working_directory)
				return sdk::unexpected(mismatch("task_extensions", task.task_id));
			if (!closure_ids.contains(task.source_closure.source_closure_id))
				return sdk::unexpected(mismatch("task.source_closure", task.task_id));
			referenced_closures.insert(task.source_closure.source_closure_id);
		}
		if (referenced_closures.size() != closure_ids.size())
			return sdk::unexpected(mismatch("source_closures", "unreferenced"));
		if (!manifests.empty())
		{
			if (manifests.size() != request.source_closures.size())
				return sdk::unexpected(mismatch("manifests", "census"));
			std::set<std::string, std::less<>> manifest_ids;
			for (const auto& manifest : manifests)
			{
				if (!manifest_ids.insert(manifest.closure_id).second)
					return sdk::unexpected(invalid("manifests", "duplicate-id"));
				const auto summary = std::ranges::find_if(request.source_closures,
														  [&](const auto& candidate)
														  {
															  return candidate.source_closure_id ==
																  manifest.closure_id;
														  });
				if (summary == request.source_closures.end())
					return sdk::unexpected(mismatch("manifests", manifest.closure_id));
				if (auto valid =
						bind_source_closure_summary(*summary, manifest, limits.task_limits);
					!valid)
					return sdk::unexpected(std::move(valid.error()));
			}
			for (std::size_t index{}; index < request.task_extensions.size(); ++index)
				if (const auto* manifest = find_manifest(
						manifests, request.task_extensions[index].source_closure.source_closure_id);
					manifest == nullptr)
					return sdk::unexpected(mismatch("manifests", "missing"));
				else if (auto valid =
							 bind_provider_task_v4_main_member(request.base_tasks[index],
															   request.task_extensions[index],
															   *manifest,
															   limits.task_limits);
						 !valid)
					return sdk::unexpected(std::move(valid.error()));
		}
		else
		{
			for (const auto& task : request.task_extensions)
				if (auto valid = validate_provider_task_v4_identity(task); !valid)
					return sdk::unexpected(std::move(valid.error()));
		}
		auto total =
			aggregate_closure_bytes(request.source_closures, limits.maximum_unique_blob_bytes);
		if (!total)
			return sdk::unexpected(std::move(total.error()));
		auto expected = derive_materialization_request_v2_2_digest(request);
		if (!expected || request.request_digest != *expected ||
			request.request_id != "materialization-request:" + *expected)
			return sdk::unexpected(sdk::error{
				"materialization.identity-mismatch", "request_id", "request-v2_2-digest"});
		auto build_captures =
			adapt_provider_task_v4_build_captures(*authority, request.task_extensions);
		if (!build_captures)
			return sdk::unexpected(std::move(build_captures.error()));
		return validated_materialization_request_v2_2{std::move(request),
													  std::move(*authority),
													  std::move(*build_captures),
													  std::move(*negotiated),
													  *total};
	}
} // namespace cxxlens::detail::clang22::materialization
