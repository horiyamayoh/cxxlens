#include "provider_task_v4.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <string_view>
#include <utility>

#include "materialization_json.hpp"
#include "source_closure.hpp"
#include "source_closure_invocation.hpp"
#include "unicode_nfc.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;

		[[nodiscard]] sdk::error invalid(std::string field, std::string detail = {})
		{
			return {"materialization.task-v4-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error mismatch(std::string field, std::string detail = {})
		{
			return {
				"materialization.task-v4-binding-mismatch", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error digest_mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.identity-mismatch", std::move(field), std::move(detail)};
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

		[[nodiscard]] bool content_digest_grammar(const std::string_view value) noexcept
		{
			return value.size() == 71U && value.starts_with("sha256:") &&
				lower_hex(value.substr(7U));
		}

		[[nodiscard]] bool semantic_digest_grammar(const std::string_view value) noexcept
		{
			return value.size() == 83U && value.starts_with("semantic-v2:sha256:") &&
				lower_hex(value.substr(19U));
		}

		[[nodiscard]] bool content_or_semantic_digest_grammar(const std::string_view value) noexcept
		{
			return content_digest_grammar(value) || semantic_digest_grammar(value);
		}

		[[nodiscard]] bool task_id_grammar(const std::string_view value) noexcept
		{
			return value.size() == 88U && value.starts_with("task:semantic-v2:sha256:") &&
				lower_hex(value.substr(24U));
		}

		[[nodiscard]] bool source_closure_id_grammar(const std::string_view value) noexcept
		{
			return value.size() == 98U && value.starts_with("source-closure:semantic-v2:sha256:") &&
				lower_hex(value.substr(34U));
		}

		[[nodiscard]] bool file_id_grammar(const std::string_view value) noexcept
		{
			return value.size() == 76U && value.starts_with("file:sha256:") &&
				lower_hex(value.substr(12U));
		}

		[[nodiscard]] bool line_index_id_grammar(const std::string_view value) noexcept
		{
			return value.size() == 82U && value.starts_with("line-index:sha256:") &&
				lower_hex(value.substr(18U));
		}

		[[nodiscard]] bool byte_less(const std::string_view left,
									 const std::string_view right) noexcept
		{
			const auto common = std::min(left.size(), right.size());
			for (std::size_t index{}; index < common; ++index)
			{
				const auto lhs = static_cast<unsigned char>(left[index]);
				const auto rhs = static_cast<unsigned char>(right[index]);
				if (lhs != rhs)
					return lhs < rhs;
			}
			return left.size() < right.size();
		}

		[[nodiscard]] sdk::result<void> validate_utf8(const std::string_view value,
													  const std::string_view field)
		{
			if (auto valid = sdk::validate_utf8_text(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "invalid-utf8"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_project_path(const std::string_view value,
															  const std::string_view field,
															  const provider_task_v4_limits limits)
		{
			if (value.size() > limits.maximum_logical_path_bytes)
				return sdk::unexpected(invalid(std::string{field}, "path-length"));
			if (auto valid = validate_utf8(value, field); !valid)
				return valid;
			if (auto relative = source_closure_relative_path(value); !relative)
				return sdk::unexpected(invalid(std::string{field}, "project-path"));
			return {};
		}

		[[nodiscard]] sdk::result<void> validate_strong(const std::string_view value,
														const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(invalid(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<json_value> json_string(const std::string_view value,
														  const std::string_view field)
		{
			auto result = json_value::string(std::string{value});
			if (!result)
				return sdk::unexpected(invalid(std::string{field}, "invalid-utf8"));
			return result;
		}

		[[nodiscard]] sdk::result<json_value>
		json_object(std::map<std::string, json_value, materialization::utf8_byte_less> fields)
		{
			auto result = json_value::object(std::move(fields));
			if (!result)
				return sdk::unexpected(invalid("projection", "object"));
			return result;
		}

		[[nodiscard]] sdk::result<json_value>
		json_member(const source_closure_manifest_member& member)
		{
			std::map<std::string, json_value, materialization::utf8_byte_less> fields;
			for (const auto& [name, value] :
				 std::array<std::pair<std::string_view, std::string_view>, 5U>{
					 std::pair<std::string_view, std::string_view>{"content_digest",
																   member.content_digest},
					 {"encoding", member.encoding},
					 {"file_id", member.file_id},
					 {"logical_path", member.logical_path},
					 {"role", member.role}})
			{
				auto encoded = json_string(value, name);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				fields.emplace(std::string{name}, std::move(*encoded));
			}
			fields.emplace("read_only", json_value::boolean(member.read_only));
			fields.emplace("size_bytes", json_value::unsigned_integer(member.size_bytes));
			return json_object(std::move(fields));
		}

		[[nodiscard]] sdk::result<json_value> json_blob(const source_closure_manifest_blob& blob)
		{
			auto digest = json_string(blob.content_digest, "blob.content_digest");
			if (!digest)
				return sdk::unexpected(std::move(digest.error()));
			return json_object({
				{"content_digest", std::move(*digest)},
				{"size_bytes", json_value::unsigned_integer(blob.size_bytes)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		manifest_projection(const source_closure_manifest& manifest)
		{
			std::vector<json_value> members;
			members.reserve(manifest.members.size());
			for (const auto& member : manifest.members)
			{
				auto encoded = json_member(member);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				members.push_back(std::move(*encoded));
			}
			std::vector<json_value> blobs;
			blobs.reserve(manifest.blobs.size());
			for (const auto& blob : manifest.blobs)
			{
				auto encoded = json_blob(blob);
				if (!encoded)
					return sdk::unexpected(std::move(encoded.error()));
				blobs.push_back(std::move(*encoded));
			}
			auto schema = json_string(manifest.schema, "manifest.schema");
			auto closure_id = json_string(manifest.closure_id, "manifest.closure_id");
			auto closure_digest = json_string(manifest.closure_digest, "manifest.closure_digest");
			if (!schema || !closure_id || !closure_digest)
				return sdk::unexpected(invalid("manifest", "invalid-utf8"));
			return json_object({
				{"blobs", json_value::array(std::move(blobs))},
				{"closure_digest", std::move(*closure_digest)},
				{"closure_id", std::move(*closure_id)},
				{"members", json_value::array(std::move(members))},
				{"schema", std::move(*schema)},
			});
		}

		[[nodiscard]] sdk::result<std::string> semantic_json_digest(const std::string_view domain,
																	const json_value& projection)
		{
			const auto encoded = materialization::canonical_json(projection);
			return sdk::semantic_digest(domain, encoded);
		}

		[[nodiscard]] sdk::result<std::string>
		canonical_bytes_digest(const std::string_view domain,
							   const sdk::canonical_value& projection)
		{
			auto encoded = sdk::canonical_binary(projection);
			if (!encoded)
				return sdk::unexpected(std::move(encoded.error()));
			std::string bytes;
			bytes.reserve(encoded->size());
			for (const auto byte : *encoded)
				bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
			return sdk::semantic_digest(domain, bytes);
		}

		[[nodiscard]] sdk::canonical_value
		member_tuple(const source_closure_manifest_member& member)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(member.file_id),
				sdk::canonical_value::from_string(member.logical_path),
				sdk::canonical_value::from_string(member.role),
				sdk::canonical_value::from_string(member.encoding),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(member.size_bytes)),
				sdk::canonical_value::from_string(member.content_digest),
				sdk::canonical_value::from_boolean(member.read_only),
			});
		}

		[[nodiscard]] sdk::canonical_value blob_tuple(const source_closure_manifest_blob& blob)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(blob.content_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(blob.size_bytes)),
			});
		}

		/**
		 * The task-v4 semantic identity uses the same closure reference as the
		 * source-closure transport codec.  Member/blob counts remain validated
		 * transport metadata on source_closure_summary; they are redundant with
		 * the authenticated manifest digest and must not create a second task
		 * identity projection.
		 */
		[[nodiscard]] sdk::result<json_value>
		closure_reference_projection(const source_closure_summary& summary)
		{
			auto id = json_string(summary.source_closure_id, "source_closure.id");
			auto digest = json_string(summary.source_closure_digest, "source_closure.digest");
			auto manifest = json_string(summary.manifest_digest, "source_closure.manifest_digest");
			if (!id || !digest || !manifest)
				return sdk::unexpected(invalid("source_closure", "invalid-utf8"));
			return json_object({
				{"digest", std::move(*digest)},
				{"id", std::move(*id)},
				{"manifest_digest", std::move(*manifest)},
			});
		}

		[[nodiscard]] sdk::result<json_value>
		open_task_projection(const provider_task_v4_open_task& open)
		{
			auto task_input = json_string(open.task_input_digest, "open_task.task_input_digest");
			auto invocation = json_string(open.normalized_invocation_digest,
										  "open_task.normalized_invocation_digest");
			auto toolchain = json_string(open.toolchain_digest, "open_task.toolchain_digest");
			auto environment = json_string(open.environment_digest, "open_task.environment_digest");
			if (!task_input || !invocation || !toolchain || !environment)
				return sdk::unexpected(invalid("open_task", "invalid-utf8"));
			return json_object({
				{"environment_digest", std::move(*environment)},
				{"normalized_invocation_digest", std::move(*invocation)},
				{"task_input_digest", std::move(*task_input)},
				{"toolchain_digest", std::move(*toolchain)},
			});
		}

		[[nodiscard]] sdk::result<json_value> task_projection(const provider_task_v4& task)
		{
			auto schema = json_string(task.schema, "task.schema");
			auto base_provider =
				json_string(task.base_provider_task_id, "task.base_provider_task_id");
			auto base_digest = json_string(task.base_task_digest, "task.base_task_digest");
			auto main_path = json_string(task.main_logical_path, "task.main_logical_path");
			auto workdir =
				json_string(task.logical_working_directory, "task.logical_working_directory");
			if (!schema || !base_provider || !base_digest || !main_path || !workdir)
				return sdk::unexpected(invalid("task", "invalid-utf8"));
			auto open = open_task_projection(task.open_task);
			auto closure = closure_reference_projection(task.source_closure);
			if (!open || !closure)
				return sdk::unexpected(invalid("task", "projection"));
			return json_object({
				{"base_provider_task_id", std::move(*base_provider)},
				{"base_task_index", json_value::unsigned_integer(task.base_task_index)},
				{"base_task_digest", std::move(*base_digest)},
				{"logical_working_directory", std::move(*workdir)},
				{"main_logical_path", std::move(*main_path)},
				{"open_task", std::move(*open)},
				{"schema", std::move(*schema)},
				{"source_closure", std::move(*closure)},
			});
		}

		[[nodiscard]] sdk::result<void>
		validate_manifest_members(const source_closure_manifest& manifest,
								  const provider_task_v4_limits limits)
		{
			if (manifest.members.empty() || manifest.members.size() > limits.maximum_members)
				return sdk::unexpected(invalid("manifest.members", "count"));
			if (manifest.blobs.empty() || manifest.blobs.size() > limits.maximum_unique_blobs)
				return sdk::unexpected(invalid("manifest.blobs", "count"));
			std::set<std::string, std::less<>> paths;
			std::set<std::string, std::less<>> folded_paths;
			std::size_t main_count{};
			std::string previous_path;
			for (std::size_t index{}; index < manifest.members.size(); ++index)
			{
				const auto& member = manifest.members[index];
				if (auto path = validate_project_path(
						member.logical_path, "manifest.member.logical_path", limits);
					!path)
					return path;
				if (index != 0U && !byte_less(previous_path, member.logical_path))
					return sdk::unexpected(invalid("manifest.members", "noncanonical-order"));
				previous_path = member.logical_path;
				if (!paths.insert(member.logical_path).second)
					return sdk::unexpected(invalid("manifest.members", "duplicate-path"));
				auto folded = nfc_casefold_utf8(member.logical_path);
				if (!folded)
					return sdk::unexpected(invalid("manifest.member.logical_path", "casefold"));
				if (!folded_paths.insert(*folded).second)
					return sdk::unexpected(invalid("manifest.members", "casefold-collision"));
				auto expected_file = source_closure_file_id(member.logical_path);
				if (!expected_file || member.file_id != *expected_file ||
					!file_id_grammar(member.file_id))
					return sdk::unexpected(invalid("manifest.member.file_id", "derived-id"));
				if (member.role != "main" && member.role != "header" &&
					member.role != "generated" && member.role != "forced-include" &&
					member.role != "macro-file")
					return sdk::unexpected(invalid("manifest.member.role", "enum"));
				if (member.role == "main")
					++main_count;
				if (member.encoding != "utf8" && member.encoding != "utf16le" &&
					member.encoding != "utf16be" && member.encoding != "locale_dependent" &&
					member.encoding != "binary_or_unknown")
					return sdk::unexpected(invalid("manifest.member.encoding", "enum"));
				if (!member.read_only || member.size_bytes > limits.maximum_blob_bytes ||
					!content_digest_grammar(member.content_digest))
					return sdk::unexpected(invalid("manifest.member", "metadata"));
			}
			if (main_count != 1U)
				return sdk::unexpected(invalid("manifest.members", "exactly-one-main"));

			std::set<std::string, std::less<>> blob_digests;
			std::uint64_t total{};
			for (std::size_t index{}; index < manifest.blobs.size(); ++index)
			{
				const auto& blob = manifest.blobs[index];
				if (index != 0U &&
					!byte_less(manifest.blobs[index - 1U].content_digest, blob.content_digest))
					return sdk::unexpected(invalid("manifest.blobs", "noncanonical-order"));
				if (!blob_digests.insert(blob.content_digest).second ||
					!content_digest_grammar(blob.content_digest) ||
					blob.size_bytes > limits.maximum_blob_bytes)
					return sdk::unexpected(invalid("manifest.blob", "metadata"));
				if (total > limits.maximum_unique_blob_bytes ||
					blob.size_bytes > limits.maximum_unique_blob_bytes - total)
					return sdk::unexpected(invalid("manifest.blobs", "aggregate-size"));
				total += blob.size_bytes;
			}
			std::set<std::string, std::less<>> referenced;
			for (const auto& member : manifest.members)
			{
				auto found =
					std::ranges::find_if(manifest.blobs,
										 [&](const auto& blob)
										 {
											 return blob.content_digest == member.content_digest;
										 });
				if (found == manifest.blobs.end() || found->size_bytes != member.size_bytes)
					return sdk::unexpected(invalid("manifest.member", "orphan-or-size-mismatch"));
				referenced.insert(member.content_digest);
			}
			if (referenced.size() != manifest.blobs.size())
				return sdk::unexpected(invalid("manifest.blobs", "orphan"));
			return {};
		}
	} // namespace

	namespace
	{
		[[nodiscard]] sdk::error authority_invalid(std::string field, std::string detail = {})
		{
			return {
				"materialization.task-v4-authority-invalid", std::move(field), std::move(detail)};
		}

		[[nodiscard]] sdk::error authority_mismatch(std::string field, std::string detail = {})
		{
			return {"materialization.task-v4-authority-binding-mismatch",
					std::move(field),
					std::move(detail)};
		}

		[[nodiscard]] sdk::result<void> authority_string(const std::string_view value,
														 const std::string_view field)
		{
			if (auto valid = sdk::validate_strong_id(value); !valid)
				return sdk::unexpected(authority_invalid(std::string{field}, "strong-id"));
			return {};
		}

		[[nodiscard]] sdk::result<void> authority_content_digest(const std::string_view value,
																 const std::string_view field)
		{
			if (!content_digest_grammar(value))
				return sdk::unexpected(authority_invalid(std::string{field}, "content-digest"));
			return {};
		}

		[[nodiscard]] sdk::result<void> authority_semantic_digest(const std::string_view value,
																  const std::string_view field)
		{
			if (!semantic_digest_grammar(value))
				return sdk::unexpected(authority_invalid(std::string{field}, "semantic-digest"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		authority_content_or_semantic_digest(const std::string_view value,
											 const std::string_view field)
		{
			if (!content_or_semantic_digest_grammar(value))
				return sdk::unexpected(authority_invalid(std::string{field}, "digest"));
			return {};
		}

		[[nodiscard]] sdk::result<void> authority_revision(const std::string_view value,
														   const std::string_view field)
		{
			if (value.size() != 40U || !lower_hex(value))
				return sdk::unexpected(authority_invalid(std::string{field}, "revision"));
			return {};
		}

		[[nodiscard]] sdk::result<void> authority_version(const sdk::semantic_version& actual,
														  const sdk::semantic_version expected,
														  const std::string_view field)
		{
			if (actual != expected)
				return sdk::unexpected(authority_invalid(std::string{field}, "version"));
			return {};
		}

		[[nodiscard]] sdk::result<void>
		authority_budget(const sdk::provider::execution_budget& value)
		{
			if (auto valid = value.validate(); !valid)
				return sdk::unexpected(authority_invalid("budget", "zero"));
			constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
			for (const auto [field, number] : {
					 std::pair{std::string_view{"output_bytes"}, value.output_bytes},
					 std::pair{std::string_view{"rows"}, value.rows},
					 std::pair{std::string_view{"diagnostics"}, value.diagnostics},
					 std::pair{std::string_view{"wall_ms"}, value.wall_ms},
					 std::pair{std::string_view{"cpu_ms"}, value.cpu_ms},
					 std::pair{std::string_view{"address_space_bytes"}, value.address_space_bytes},
					 std::pair{std::string_view{"transport_bytes"}, value.transport_bytes},
					 std::pair{std::string_view{"open_files"}, value.open_files},
					 std::pair{std::string_view{"subprocesses"}, value.subprocesses},
				 })
				if (number > static_cast<std::uint64_t>(maximum))
					return sdk::unexpected(authority_invalid(
						std::string{"budget."} + std::string{field}, "canonical-i64"));
			return {};
		}

		[[nodiscard]] sdk::result<void> authority_logical_path(const std::string_view value,
															   const std::string_view field)
		{
			if (auto valid = validate_project_path(value, field, provider_task_v4_limits{}); !valid)
				return valid;
			return {};
		}

		[[nodiscard]] sdk::canonical_value text(const std::string_view value)
		{
			return sdk::canonical_value::from_string(std::string{value});
		}

		[[nodiscard]] sdk::canonical_value version(const sdk::semantic_version value)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.major)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.minor)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.patch)),
			});
		}

		[[nodiscard]] sdk::canonical_value strings(const std::span<const std::string> values)
		{
			std::vector<sdk::canonical_value> result;
			result.reserve(values.size());
			for (const auto& value : values)
				result.push_back(text(value));
			return sdk::canonical_value::from_tuple(std::move(result));
		}

		[[nodiscard]] sdk::canonical_value optional_text(const std::optional<std::string>& value)
		{
			return value ? text(*value) : sdk::canonical_value::null();
		}

		[[nodiscard]] std::string
		sandbox_assurance_name(const sdk::provider::sandbox_assurance value)
		{
			switch (value)
			{
				case sdk::provider::sandbox_assurance::enforced:
					return "enforced";
				case sdk::provider::sandbox_assurance::certified:
					return "certified";
				default:
					return {};
			}
		}

		[[nodiscard]] sdk::canonical_value
		budget_projection(const sdk::provider::execution_budget& value)
		{
			return sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.output_bytes)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.rows)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.diagnostics)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.wall_ms)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.cpu_ms)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.address_space_bytes)),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.transport_bytes)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.open_files)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.subprocesses)),
			});
		}

		[[nodiscard]] sdk::canonical_value
		sandbox_projection(const sdk::provider::sandbox_requirement& value)
		{
			return sdk::canonical_value::from_tuple(
				{text(sandbox_assurance_name(value.minimum)), text(value.policy_digest)});
		}

		[[nodiscard]] sdk::canonical_value source_projection(const provider_task_v4_source& value)
		{
			return sdk::canonical_value::from_tuple(
				{text(value.source_snapshot_id),
				 text(value.file_id),
				 text(value.logical_path),
				 text(value.content_digest),
				 sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.size_bytes)),
				 text(value.encoding),
				 text(value.line_index_id),
				 sdk::canonical_value::from_boolean(value.read_only)});
		}

		[[nodiscard]] sdk::canonical_value
		input_authority_projection(const provider_task_v4_input_authority& value)
		{
			return sdk::canonical_value::from_tuple({text(value.normalized_invocation_digest),
													 text(value.logical_working_directory),
													 strings(value.effective_arguments),
													 strings(value.qualified_read_roots)});
		}

		[[nodiscard]] sdk::canonical_value
		toolchain_projection(const provider_task_v4_toolchain_authority& value)
		{
			return sdk::canonical_value::from_tuple({text(value.family),
													 text(value.exact_version),
													 text(value.target_triple),
													 text(value.builtin_headers_digest),
													 optional_text(value.sysroot),
													 text(value.abi_digest),
													 text(value.plugin_spec_digest)});
		}

		[[nodiscard]] sdk::canonical_value
		variant_projection(const provider_task_v4_variant_authority& value)
		{
			return sdk::canonical_value::from_tuple({text(value.language),
													 text(value.language_standard),
													 text(value.target_triple),
													 text(value.predefined_macros_digest),
													 text(value.include_search_digest),
													 text(value.semantic_flags_digest)});
		}

		[[nodiscard]] sdk::canonical_value
		base_binding_projection(const provider_task_v4_base_descriptor_binding& value)
		{
			return sdk::canonical_value::from_tuple(
				{text(value.descriptor_id),
				 version(value.descriptor_version),
				 text(value.contract_digest),
				 text(value.runtime_descriptor_digest),
				 sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.stage_order)),
				 text(value.output_stage),
				 text(value.owner)});
		}

		[[nodiscard]] sdk::canonical_value
		output_binding_projection(const provider_task_v4_output_descriptor_binding& value)
		{
			return sdk::canonical_value::from_tuple({text(value.descriptor_id),
													 version(value.descriptor_version),
													 text(value.contract_digest),
													 text(value.runtime_descriptor_digest),
													 text(value.dependency_group_id),
													 text(value.atomic_output_group_id),
													 text(value.batch_id),
													 text(value.output_stage)});
		}

		[[nodiscard]] sdk::canonical_value
		admitted_projection(const provider_task_v4_admitted_descriptor_binding& value)
		{
			return sdk::canonical_value::from_tuple(
				{text(value.descriptor_id), text(value.runtime_descriptor_digest)});
		}

		[[nodiscard]] sdk::canonical_value
		task_projection(const provider_task_v4_task_authority& value)
		{
			return sdk::canonical_value::from_tuple(
				{text(value.provider_task_id),
				 text(value.provider_execution_id),
				 text(value.task_input_digest),
				 text(value.project_id),
				 text(value.catalog_id),
				 text(value.catalog_digest),
				 text(value.selected_catalog_compile_unit_id),
				 text(value.compile_unit_id),
				 text(value.build_variant_id),
				 text(value.toolchain_context_id),
				 text(value.toolchain_digest),
				 toolchain_projection(value.toolchain),
				 variant_projection(value.variant),
				 text(value.normalized_invocation_digest),
				 text(value.environment_digest),
				 text(value.language),
				 text(value.working_directory),
				 text(value.condition_universe_id),
				 text(value.condition_id),
				 text(value.interpretation_domain),
				 source_projection(value.source),
				 input_authority_projection(value.input_authority),
				 strings(value.requested_descriptor_ids),
				 strings(value.dependency_groups),
				 budget_projection(value.budget),
				 sandbox_projection(value.sandbox)});
		}

		[[nodiscard]] sdk::canonical_value
		tool_projection(const provider_task_v4_tool_authority& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.executable),
				text(value.interface_version),
				text(value.distribution_version),
				text(value.source_revision),
				text(value.source_tree),
				text(value.installed_executable_digest),
				text(value.package_configuration),
				text(value.occurrence_manifest_digest),
			});
		}

		[[nodiscard]] sdk::canonical_value
		worker_projection(const provider_task_v4_worker_authority& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.executable),
				text(value.provider_id),
				version(value.provider_version),
				text(value.installed_binary_digest),
				text(value.semantic_contract_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.protocol_major)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.protocol_minor)),
				strings(value.required_features),
				text(value.sandbox_policy_digest),
			});
		}

		[[nodiscard]] sdk::canonical_value
		catalog_projection(const provider_task_v4_catalog_authority& value)
		{
			std::vector<sdk::canonical_value> units;
			units.reserve(value.catalog.compile_units.size());
			for (const auto& unit : value.catalog.compile_units)
				units.push_back(sdk::canonical_value::from_tuple({
					text(unit.compile_unit_id),
					text(unit.effective_invocation_digest),
					text(unit.source_digest),
					text(unit.environment_digest),
				}));
			return sdk::canonical_value::from_tuple({
				text(value.project_id),
				text(value.catalog.catalog_id),
				text(value.catalog.catalog_digest),
				text(value.catalog.logical_root),
				text(value.catalog.environment_digest),
				sdk::canonical_value::from_tuple(std::move(units)),
				text(value.catalog_compile_unit_census_digest),
			});
		}

		[[nodiscard]] sdk::canonical_value
		registry_projection(const provider_task_v4_registry_authority& value)
		{
			std::vector<sdk::canonical_value> base;
			base.reserve(value.base_descriptors.size());
			for (const auto& binding : value.base_descriptors)
				base.push_back(base_binding_projection(binding));
			std::vector<sdk::canonical_value> output;
			output.reserve(value.descriptors.size());
			for (const auto& binding : value.descriptors)
				output.push_back(output_binding_projection(binding));
			return sdk::canonical_value::from_tuple({
				text(value.path),
				text(value.authority_registry_digest),
				sdk::canonical_value::from_tuple(std::move(base)),
				sdk::canonical_value::from_tuple(std::move(output)),
			});
		}

		[[nodiscard]] sdk::canonical_value
		engine_projection(const provider_task_v4_engine_authority& value)
		{
			std::vector<sdk::canonical_value> admitted;
			admitted.reserve(value.admitted_descriptors.size());
			for (const auto& binding : value.admitted_descriptors)
				admitted.push_back(admitted_projection(binding));
			return sdk::canonical_value::from_tuple({
				text(value.generation_contract),
				sdk::canonical_value::from_tuple(std::move(admitted)),
				text(value.engine_registry_digest),
				text(value.engine_generation_id),
			});
		}

		[[nodiscard]] sdk::canonical_value
		interpretation_projection(const provider_task_v4_interpretation_authority& value)
		{
			return sdk::canonical_value::from_tuple({text(value.policy_id),
													 text(value.selected_domain),
													 text(value.interpretation_policy_digest)});
		}

		[[nodiscard]] sdk::canonical_value sandbox_requirements_projection(
			const std::span<const sdk::provider::sandbox_requirement> values)
		{
			std::vector<sdk::canonical_value> result;
			result.reserve(values.size());
			for (const auto& value : values)
				result.push_back(sandbox_projection(value));
			return sdk::canonical_value::from_tuple(std::move(result));
		}

		[[nodiscard]] sdk::canonical_value
		trust_projection(const provider_task_v4_trust_authority& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.policy_id),
				text(value.execution_profile),
				text(value.provider_id),
				version(value.provider_version),
				text(value.semantic_contract_digest),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.protocol_major)),
				sdk::canonical_value::from_integer(static_cast<std::int64_t>(value.protocol_minor)),
				strings(value.required_features),
				text(value.required_qualification),
				text(value.worker_sandbox_policy_digest),
				sandbox_requirements_projection(value.task_sandbox_requirements),
			});
		}

		[[nodiscard]] sdk::canonical_value
		group_projection(const provider_task_v4_group_topology_authority& value)
		{
			return sdk::canonical_value::from_tuple({strings(value.dependency_groups),
													 text(value.atomic_output_group),
													 text(value.partial_policy)});
		}

		[[nodiscard]] sdk::canonical_value
		selector_projection(const sdk::snapshot_series_selector& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.catalog_id),
				text(value.channel_id),
				text(value.engine_generation_id),
				text(value.condition_universe_id),
				text(value.relation_registry_digest),
				text(value.interpretation_policy_digest),
				text(value.trust_policy_digest),
			});
		}

		[[nodiscard]] sdk::canonical_value
		publication_projection(const provider_task_v4_publication_authority& value)
		{
			return sdk::canonical_value::from_tuple({
				text(value.backend),
				selector_projection(value.selector),
				text(value.series_id),
				sdk::canonical_value::from_boolean(value.genesis),
				optional_text(value.expected_parent_publication),
				optional_text(value.sqlite_path),
				text(value.partial_policy),
				sdk::canonical_value::from_integer(
					static_cast<std::int64_t>(value.transaction_count)),
				sdk::canonical_value::from_boolean(value.reopen_before_success),
				text(value.recipe_id),
				text(value.recipe_digest),
				text(value.output_plan_digest),
				text(value.publication_target),
			});
		}

		[[nodiscard]] sdk::canonical_value
		request_projection(const provider_task_v4_request_authority& value)
		{
			std::vector<sdk::canonical_value> tasks;
			tasks.reserve(value.tasks.size());
			for (const auto& task : value.tasks)
				tasks.push_back(task_projection(task));
			return sdk::canonical_value::from_tuple({
				text("cxxlens.clang22.request-authority.v2.2"),
				tool_projection(value.tool),
				worker_projection(value.worker),
				catalog_projection(value.project),
				registry_projection(value.registry),
				engine_projection(value.engine),
				interpretation_projection(value.interpretation_policy),
				trust_projection(value.trust_policy),
				group_projection(value.group_topology),
				sdk::canonical_value::from_tuple(std::move(tasks)),
				publication_projection(value.publication),
			});
		}

		[[nodiscard]] sdk::result<void>
		validate_sorted_unique(const std::vector<std::string>& values, const std::string_view field)
		{
			if (values.empty() || !std::ranges::is_sorted(values, byte_less) ||
				std::ranges::adjacent_find(values) != values.end())
				return sdk::unexpected(authority_invalid(std::string{field}, "canonical-order"));
			for (const auto& value : values)
				if (auto valid = authority_string(value, field); !valid)
					return valid;
			return {};
		}

	} // namespace

	sdk::result<std::string> derive_provider_task_v4_effective_invocation_digest(
		const std::string_view logical_working_directory,
		const std::span<const std::string> effective_arguments)
	{
		return canonical_bytes_digest(
			"cxxlens.clang22.effective-invocation.v1",
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string("cxxlens.clang22.effective-invocation.v1"),
				sdk::canonical_value::from_string(std::string{logical_working_directory}),
				sdk::canonical_value::from_tuple(
					[&]
					{
						std::vector<sdk::canonical_value> values;
						values.reserve(effective_arguments.size());
						for (const auto& argument : effective_arguments)
							values.push_back(sdk::canonical_value::from_string(argument));
						return values;
					}()),
			}));
	}

	sdk::result<void> provider_task_v4_input_authority::validate(
		const std::string_view main_logical_path,
		const std::string_view expected_working_directory,
		const std::string_view expected_invocation_digest) const
	{
		if (logical_working_directory != expected_working_directory)
			return sdk::unexpected(
				mismatch("input-authority.logical_working_directory", logical_working_directory));
		if (effective_arguments.empty())
			return sdk::unexpected(invalid("input-authority.effective_arguments", "empty"));
		if (qualified_read_roots.empty())
			return sdk::unexpected(invalid("input-authority.qualified_read_roots", "empty"));
		if (!semantic_digest_grammar(normalized_invocation_digest) ||
			!semantic_digest_grammar(expected_invocation_digest))
			return sdk::unexpected(
				invalid("input-authority.normalized_invocation_digest", "grammar"));

		// This is the single admission point for path-bearing argv and toolchain roots.  It rejects
		// relative/ambient paths, response files, unsupported overlays/modules, malformed roots,
		// and a main source that is not the final argv item.
		auto prepared = prepare_source_closure_invocation(effective_arguments,
														  main_logical_path,
														  logical_working_directory,
														  qualified_read_roots);
		if (!prepared)
			return sdk::unexpected(std::move(prepared.error()));
		if (prepared->qualified_read_roots != qualified_read_roots)
			return sdk::unexpected(
				invalid("input-authority.qualified_read_roots", "noncanonical-order-or-duplicate"));

		auto expected = derive_provider_task_v4_effective_invocation_digest(
			logical_working_directory, effective_arguments);
		if (!expected || *expected != normalized_invocation_digest ||
			*expected != expected_invocation_digest)
			return sdk::unexpected(digest_mismatch("input-authority.normalized_invocation_digest",
												   normalized_invocation_digest));
		return {};
	}

	sdk::result<void> provider_task_v4_toolchain_authority::validate() const
	{
		for (const auto [field, value] : {
				 std::pair{std::string_view{"family"}, std::string_view{family}},
				 std::pair{std::string_view{"exact_version"}, std::string_view{exact_version}},
				 std::pair{std::string_view{"target_triple"}, std::string_view{target_triple}},
			 })
			if (auto valid = authority_string(value, "toolchain." + std::string{field}); !valid)
				return valid;
		for (const auto [field, value] : {
				 std::pair{std::string_view{"builtin_headers_digest"},
						   std::string_view{builtin_headers_digest}},
				 std::pair{std::string_view{"abi_digest"}, std::string_view{abi_digest}},
				 std::pair{std::string_view{"plugin_spec_digest"},
						   std::string_view{plugin_spec_digest}},
			 })
			if (!content_digest_grammar(value))
				return sdk::unexpected(
					authority_invalid("toolchain." + std::string{field}, "content-digest"));
		if (sysroot && sysroot->empty())
			return sdk::unexpected(authority_invalid("toolchain.sysroot", "empty"));
		if (sysroot && !sdk::validate_strong_id(*sysroot))
			return sdk::unexpected(authority_invalid("toolchain.sysroot", "strong-id"));
		return {};
	}

	sdk::result<void> provider_task_v4_variant_authority::validate() const
	{
		for (const auto [field, value] : {
				 std::pair{std::string_view{"language"}, std::string_view{language}},
				 std::pair{std::string_view{"language_standard"},
						   std::string_view{language_standard}},
				 std::pair{std::string_view{"target_triple"}, std::string_view{target_triple}},
			 })
			if (auto valid = authority_string(value, "variant." + std::string{field}); !valid)
				return valid;
		for (const auto [field, value] : {
				 std::pair{std::string_view{"predefined_macros_digest"},
						   std::string_view{predefined_macros_digest}},
				 std::pair{std::string_view{"include_search_digest"},
						   std::string_view{include_search_digest}},
				 std::pair{std::string_view{"semantic_flags_digest"},
						   std::string_view{semantic_flags_digest}},
			 })
			if (auto valid = authority_content_digest(value, "variant." + std::string{field});
				!valid)
				return valid;
		return {};
	}

	sdk::result<void> provider_task_v4_base_descriptor_binding::validate() const
	{
		if (auto valid = authority_string(descriptor_id, "registry.base.descriptor_id"); !valid)
			return valid;
		if (auto valid = authority_version(
				descriptor_version, {1U, 0U, 0U}, "registry.base.descriptor_version");
			!valid)
			return valid;
		if (auto valid = authority_content_digest(contract_digest, "registry.base.contract_digest");
			!valid)
			return valid;
		if (auto valid = authority_semantic_digest(runtime_descriptor_digest,
												   "registry.base.runtime_descriptor_digest");
			!valid)
			return valid;
		if (output_stage != "canonical_claim" || owner != "installed-tool")
			return sdk::unexpected(authority_invalid("registry.base", "stage-or-owner"));
		if (stage_order >= task_v4_base_descriptor_ids.size())
			return sdk::unexpected(authority_invalid("registry.base.stage_order", "range"));
		return {};
	}

	sdk::result<void> provider_task_v4_output_descriptor_binding::validate() const
	{
		if (auto valid = authority_string(descriptor_id, "registry.descriptor_id"); !valid)
			return valid;
		const auto expected_version = descriptor_id.starts_with("frontend.")
			? sdk::semantic_version{2U, 0U, 0U}
			: sdk::semantic_version{1U, 0U, 0U};
		if (auto valid = authority_version(
				descriptor_version, expected_version, "registry.descriptor_version");
			!valid)
			return valid;
		if (auto valid = authority_content_digest(contract_digest, "registry.contract_digest");
			!valid)
			return valid;
		if (auto valid = authority_semantic_digest(runtime_descriptor_digest,
												   "registry.runtime_descriptor_digest");
			!valid)
			return valid;
		if (dependency_group_id != "canonical" && dependency_group_id != "observation")
			return sdk::unexpected(authority_invalid("registry.dependency_group_id", "enum"));
		if (atomic_output_group_id != "clang22-atomic" || batch_id != descriptor_id + "-batch")
			return sdk::unexpected(authority_invalid("registry.descriptor", "batch-binding"));
		const auto expected_group = descriptor_id.starts_with("cc.") ? "canonical" : "observation";
		const auto expected_stage =
			descriptor_id.starts_with("cc.") ? "canonical_claim" : "assertion";
		if (dependency_group_id != expected_group || output_stage != expected_stage)
			return sdk::unexpected(authority_invalid("registry.descriptor", "stage-binding"));
		return {};
	}

	sdk::result<void> provider_task_v4_admitted_descriptor_binding::validate() const
	{
		if (auto valid = authority_string(descriptor_id, "engine.descriptor_id"); !valid)
			return valid;
		return authority_semantic_digest(runtime_descriptor_digest,
										 "engine.runtime_descriptor_digest");
	}

	sdk::result<void> provider_task_v4_catalog_authority::validate() const
	{
		if (auto valid = authority_string(project_id, "project.project_id"); !valid)
			return valid;
		if (auto valid = catalog.validate(); !valid)
			return sdk::unexpected(authority_invalid("project.catalog", valid.error().detail));
		return authority_semantic_digest(catalog_compile_unit_census_digest,
										 "project.catalog_compile_unit_census_digest");
	}

	sdk::result<void> provider_task_v4_registry_authority::validate() const
	{
		if (path != task_v4_registry_path)
			return sdk::unexpected(authority_invalid("registry.path", "unsupported"));
		if (auto valid = authority_content_digest(authority_registry_digest,
												  "registry.authority_registry_digest");
			!valid)
			return valid;
		if (base_descriptors.size() != task_v4_base_descriptor_ids.size() ||
			descriptors.size() != task_v4_output_descriptor_ids.size())
			return sdk::unexpected(authority_invalid("registry", "descriptor-census"));
		for (std::size_t index{}; index < base_descriptors.size(); ++index)
		{
			if (base_descriptors[index].descriptor_id != task_v4_base_descriptor_ids[index] ||
				base_descriptors[index].stage_order != index)
				return sdk::unexpected(authority_invalid("registry.base_descriptors", "order"));
			if (auto valid = base_descriptors[index].validate(); !valid)
				return valid;
		}
		for (std::size_t index{}; index < descriptors.size(); ++index)
		{
			if (descriptors[index].descriptor_id != task_v4_output_descriptor_ids[index])
				return sdk::unexpected(authority_invalid("registry.descriptors", "order"));
			if (auto valid = descriptors[index].validate(); !valid)
				return valid;
		}
		// The full accepted registry document is an independent content authority.  This
		// value cannot be reconstructed from the twelve descriptors admitted by this
		// materializer, so only its content-digest grammar is checked here.  The engine
		// inventory below has its own semantic digest and must never alias this value.
		return {};
	}

	sdk::result<void> provider_task_v4_engine_authority::validate() const
	{
		if (generation_contract != task_v4_engine_generation_contract ||
			admitted_descriptors.size() != task_v4_engine_descriptor_ids.size())
			return sdk::unexpected(authority_invalid("engine", "shape"));
		if (auto valid =
				authority_semantic_digest(engine_registry_digest, "engine.engine_registry_digest");
			!valid)
			return valid;
		if (engine_generation_id.size() != 89U ||
			!engine_generation_id.starts_with("engine-generation:sha256:") ||
			!lower_hex(engine_generation_id.substr(25U)))
			return sdk::unexpected(authority_invalid("engine.engine_generation_id", "identity"));
		for (std::size_t index{}; index < admitted_descriptors.size(); ++index)
		{
			if (admitted_descriptors[index].descriptor_id != task_v4_engine_descriptor_ids[index])
				return sdk::unexpected(authority_invalid("engine.admitted_descriptors", "order"));
			if (auto valid = admitted_descriptors[index].validate(); !valid)
				return valid;
		}
		std::vector<std::pair<std::string_view, std::string_view>> sorted;
		sorted.reserve(admitted_descriptors.size());
		for (const auto& descriptor : admitted_descriptors)
			sorted.emplace_back(descriptor.descriptor_id, descriptor.runtime_descriptor_digest);
		std::ranges::sort(sorted,
						  [](const auto& left, const auto& right)
						  {
							  return byte_less(left.first, right.first);
						  });
		std::string registry_payload;
		for (const auto& [id, digest] : sorted)
		{
			registry_payload.append(id);
			registry_payload.push_back('=');
			registry_payload.append(digest);
			registry_payload.push_back('\n');
		}
		auto expected_registry =
			sdk::semantic_digest("cxxlens.relation-registry.v1", registry_payload);
		if (!expected_registry || *expected_registry != engine_registry_digest)
			return sdk::unexpected(authority_mismatch("engine.engine_registry_digest"));
		return {};
	}

	sdk::result<void> provider_task_v4_interpretation_authority::validate() const
	{
		if (policy_id != task_v4_interpretation_policy_id ||
			selected_domain != task_v4_interpretation_domain)
			return sdk::unexpected(authority_invalid("interpretation_policy", "contract"));
		if (auto valid = authority_semantic_digest(interpretation_policy_digest,
												   "interpretation_policy.digest");
			!valid)
			return valid;
		auto encoded = sdk::canonical_binary(sdk::canonical_value::from_tuple({
			text(policy_id),
			text(selected_domain),
		}));
		if (!encoded)
			return sdk::unexpected(authority_invalid("interpretation_policy", "projection"));
		std::string bytes;
		bytes.reserve(encoded->size());
		for (const auto byte : *encoded)
			bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		auto expected = sdk::semantic_digest(std::string{task_v4_interpretation_policy_id}, bytes);
		if (!expected || *expected != interpretation_policy_digest)
			return sdk::unexpected(authority_mismatch("interpretation_policy.digest"));
		return {};
	}

	sdk::result<void> provider_task_v4_tool_authority::validate() const
	{
		for (const auto [field, value] : {
				 std::pair{std::string_view{"executable"}, std::string_view{executable}},
				 std::pair{std::string_view{"interface_version"},
						   std::string_view{interface_version}},
				 std::pair{std::string_view{"distribution_version"},
						   std::string_view{distribution_version}},
				 std::pair{std::string_view{"package_configuration"},
						   std::string_view{package_configuration}},
			 })
			if (auto valid = authority_string(value, "tool." + std::string{field}); !valid)
				return valid;
		for (const auto [field, value] : {
				 std::pair{std::string_view{"source_revision"}, std::string_view{source_revision}},
				 std::pair{std::string_view{"source_tree"}, std::string_view{source_tree}},
			 })
			if (auto valid = authority_revision(value, "tool." + std::string{field}); !valid)
				return valid;
		if (auto valid = authority_content_digest(installed_executable_digest,
												  "tool.installed_executable_digest");
			!valid)
			return valid;
		return authority_content_or_semantic_digest(occurrence_manifest_digest,
													"tool.occurrence_manifest_digest");
	}

	sdk::result<void> provider_task_v4_worker_authority::validate() const
	{
		if (auto valid = authority_string(executable, "worker.executable"); !valid)
			return valid;
		if (auto valid = authority_string(provider_id, "worker.provider_id"); !valid)
			return valid;
		if (provider_version == sdk::semantic_version{})
			return sdk::unexpected(authority_invalid("worker.provider_version", "zero"));
		if (auto valid =
				authority_content_digest(installed_binary_digest, "worker.installed_binary_digest");
			!valid)
			return valid;
		if (auto valid = authority_content_or_semantic_digest(semantic_contract_digest,
															  "worker.semantic_contract_digest");
			!valid)
			return valid;
		if (protocol_major != sdk::provider::protocol_v2_major ||
			protocol_minor != sdk::provider::protocol_v2_minor)
			return sdk::unexpected(authority_invalid("worker.protocol", "protocol-2.0-required"));
		if (auto valid = validate_sorted_unique(required_features, "worker.required_features");
			!valid)
			return valid;
		const std::vector<std::string> expected_features{"task-input-chunks-v2",
														 "task-source-closure-v2"};
		if (required_features != expected_features)
			return sdk::unexpected(authority_invalid("worker.required_features", "contract"));
		return authority_content_or_semantic_digest(sandbox_policy_digest,
													"worker.sandbox_policy_digest");
	}

	sdk::result<void> provider_task_v4_group_topology_authority::validate() const
	{
		if (dependency_groups.size() != task_v4_dependency_groups.size() ||
			!std::ranges::equal(dependency_groups, task_v4_dependency_groups))
			return sdk::unexpected(
				authority_invalid("group_topology.dependency_groups", "contract"));
		if (atomic_output_group != "clang22-atomic" || partial_policy != "forbid")
			return sdk::unexpected(authority_invalid("group_topology", "contract"));
		return {};
	}

	sdk::result<void> provider_task_v4_trust_authority::validate() const
	{
		if (policy_id != task_v4_trust_policy_id)
			return sdk::unexpected(authority_invalid("trust_policy.policy_id", "contract"));
		for (const auto [field, value] : {
				 std::pair{std::string_view{"execution_profile"},
						   std::string_view{execution_profile}},
				 std::pair{std::string_view{"provider_id"}, std::string_view{provider_id}},
				 std::pair{std::string_view{"required_qualification"},
						   std::string_view{required_qualification}},
			 })
			if (auto valid = authority_string(value, "trust_policy." + std::string{field}); !valid)
				return valid;
		if (provider_version == sdk::semantic_version{})
			return sdk::unexpected(authority_invalid("trust_policy.provider_version", "zero"));
		if (auto valid = authority_content_or_semantic_digest(
				semantic_contract_digest, "trust_policy.semantic_contract_digest");
			!valid)
			return valid;
		if (protocol_major != sdk::provider::protocol_v2_major ||
			protocol_minor != sdk::provider::protocol_v2_minor)
			return sdk::unexpected(
				authority_invalid("trust_policy.protocol", "protocol-2.0-required"));
		if (auto valid =
				validate_sorted_unique(required_features, "trust_policy.required_features");
			!valid)
			return valid;
		const std::vector<std::string> expected_features{"task-input-chunks-v2",
														 "task-source-closure-v2"};
		if (required_features != expected_features)
			return sdk::unexpected(authority_invalid("trust_policy.required_features", "contract"));
		if (auto valid = authority_content_or_semantic_digest(
				worker_sandbox_policy_digest, "trust_policy.worker_sandbox_policy_digest");
			!valid)
			return valid;
		if (task_sandbox_requirements.empty())
			return sdk::unexpected(
				authority_invalid("trust_policy.task_sandbox_requirements", "empty"));
		for (const auto& requirement : task_sandbox_requirements)
		{
			if (auto valid = requirement.validate(); !valid)
				return sdk::unexpected(authority_invalid("trust_policy.task_sandbox_requirements",
														 valid.error().detail));
			if (requirement.minimum < sdk::provider::sandbox_assurance::enforced)
				return sdk::unexpected(
					authority_invalid("trust_policy.task_sandbox_requirements", "minimum"));
		}
		if (auto valid = authority_semantic_digest(trust_policy_digest, "trust_policy.digest");
			!valid)
			return valid;
		auto expected =
			canonical_bytes_digest(std::string{task_v4_trust_policy_id}, trust_projection(*this));
		if (!expected || *expected != trust_policy_digest)
			return sdk::unexpected(authority_mismatch("trust_policy.digest"));
		return {};
	}

	sdk::result<void> provider_task_v4_publication_authority::validate() const
	{
		if (backend != "memory" && backend != "sqlite")
			return sdk::unexpected(authority_invalid("publication.backend", "enum"));
		if (auto valid = selector.validate(); !valid)
			return sdk::unexpected(authority_invalid("publication.selector", valid.error().detail));
		if (series_id != selector.id())
			return sdk::unexpected(authority_mismatch("publication.series_id"));
		if (genesis == expected_parent_publication.has_value())
			return sdk::unexpected(authority_invalid("publication.parent", "genesis-parent"));
		if (sqlite_path.has_value() != (backend == "sqlite"))
			return sdk::unexpected(authority_invalid("publication.sqlite_path", "backend-binding"));
		if (sqlite_path && sqlite_path->empty())
			return sdk::unexpected(authority_invalid("publication.sqlite_path", "empty"));
		if (expected_parent_publication && expected_parent_publication->empty())
			return sdk::unexpected(
				authority_invalid("publication.expected_parent_publication", "empty"));
		if (partial_policy != "forbid" || transaction_count == 0U || !reopen_before_success)
			return sdk::unexpected(authority_invalid("publication", "effect-safety"));
		if (auto valid = authority_string(recipe_id, "publication.recipe_id"); !valid)
			return valid;
		if (auto valid = authority_semantic_digest(recipe_digest, "publication.recipe_digest");
			!valid)
			return valid;
		if (auto valid =
				authority_semantic_digest(output_plan_digest, "publication.output_plan_digest");
			!valid)
			return valid;
		return authority_string(publication_target, "publication.publication_target");
	}

	sdk::result<void> provider_task_v4_task_authority::validate(
		const provider_task_v4_catalog_authority& catalog_authority,
		const provider_task_v4_group_topology_authority& group_topology) const
	{
		for (const auto [field, value] : {
				 std::pair{std::string_view{"provider_task_id"},
						   std::string_view{provider_task_id}},
				 std::pair{std::string_view{"provider_execution_id"},
						   std::string_view{provider_execution_id}},
				 std::pair{std::string_view{"project_id"}, std::string_view{project_id}},
				 std::pair{std::string_view{"catalog_id"}, std::string_view{catalog_id}},
				 std::pair{std::string_view{"selected_catalog_compile_unit_id"},
						   std::string_view{selected_catalog_compile_unit_id}},
				 std::pair{std::string_view{"compile_unit_id"}, std::string_view{compile_unit_id}},
				 std::pair{std::string_view{"build_variant_id"},
						   std::string_view{build_variant_id}},
				 std::pair{std::string_view{"toolchain_context_id"},
						   std::string_view{toolchain_context_id}},
				 std::pair{std::string_view{"language"}, std::string_view{language}},
				 std::pair{std::string_view{"condition_universe_id"},
						   std::string_view{condition_universe_id}},
				 std::pair{std::string_view{"condition_id"}, std::string_view{condition_id}},
				 std::pair{std::string_view{"interpretation_domain"},
						   std::string_view{interpretation_domain}},
			 })
			if (auto valid = authority_string(value, "task." + std::string{field}); !valid)
				return valid;
		for (const auto [field, value] : {
				 std::pair{std::string_view{"task_input_digest"},
						   std::string_view{task_input_digest}},
				 std::pair{std::string_view{"catalog_digest"}, std::string_view{catalog_digest}},
				 std::pair{std::string_view{"toolchain_digest"},
						   std::string_view{toolchain_digest}},
				 std::pair{std::string_view{"normalized_invocation_digest"},
						   std::string_view{normalized_invocation_digest}},
				 std::pair{std::string_view{"environment_digest"},
						   std::string_view{environment_digest}},
			 })
			if (!content_or_semantic_digest_grammar(value))
				return sdk::unexpected(authority_invalid("task." + std::string{field}, "digest"));
		if (auto valid = toolchain.validate(); !valid)
			return valid;
		if (auto valid = variant.validate(); !valid)
			return valid;
		if (auto valid = authority_logical_path(working_directory, "task.working_directory");
			!valid)
			return valid;
		if (auto valid = source.validate(); !valid)
			return valid;
		if (auto valid = input_authority.validate(
				source.logical_path, working_directory, normalized_invocation_digest);
			!valid)
			return valid;
		if (input_authority.normalized_invocation_digest != normalized_invocation_digest ||
			input_authority.logical_working_directory != working_directory)
			return sdk::unexpected(authority_mismatch("task.input_authority"));
		if (requested_descriptor_ids.size() != task_v4_output_descriptor_ids.size() ||
			!std::ranges::equal(requested_descriptor_ids, task_v4_output_descriptor_ids))
			return sdk::unexpected(authority_invalid("task.requested_descriptor_ids", "contract"));
		if (dependency_groups != group_topology.dependency_groups)
			return sdk::unexpected(authority_mismatch("task.dependency_groups"));
		if (auto valid = authority_budget(budget); !valid)
			return valid;
		if (auto valid = sandbox.validate(); !valid)
			return sdk::unexpected(authority_invalid("task.sandbox", valid.error().detail));
		if (sandbox.minimum < sdk::provider::sandbox_assurance::enforced)
			return sdk::unexpected(authority_invalid("task.sandbox.minimum", "minimum"));
		if (project_id != catalog_authority.project_id ||
			catalog_id != catalog_authority.catalog.catalog_id ||
			catalog_digest != catalog_authority.catalog.catalog_digest)
			return sdk::unexpected(authority_mismatch("task.catalog"));
		const auto unit = std::ranges::find_if(catalog_authority.catalog.compile_units,
											   [&](const auto& candidate)
											   {
												   return candidate.compile_unit_id ==
													   selected_catalog_compile_unit_id;
											   });
		if (unit == catalog_authority.catalog.compile_units.end() ||
			unit->effective_invocation_digest != normalized_invocation_digest ||
			unit->source_digest != source.content_digest ||
			unit->environment_digest != environment_digest)
			return sdk::unexpected(authority_mismatch("task.catalog_compile_unit"));
		return {};
	}

	sdk::result<void> provider_task_v4_request_authority::validate() const
	{
		if (auto valid = tool.validate(); !valid)
			return valid;
		if (auto valid = worker.validate(); !valid)
			return valid;
		if (auto valid = project.validate(); !valid)
			return valid;
		if (auto valid = registry.validate(); !valid)
			return valid;
		if (auto valid = engine.validate(); !valid)
			return valid;
		if (auto valid = interpretation_policy.validate(); !valid)
			return valid;
		if (auto valid = trust_policy.validate(); !valid)
			return valid;
		if (auto valid = group_topology.validate(); !valid)
			return valid;
		if (auto valid = publication.validate(); !valid)
			return valid;
		if (tasks.empty() || tasks.size() > provider_task_v4_limits{}.maximum_members)
			return sdk::unexpected(authority_invalid("tasks", "count"));
		if (worker.provider_id != trust_policy.provider_id ||
			worker.provider_version != trust_policy.provider_version ||
			worker.semantic_contract_digest != trust_policy.semantic_contract_digest ||
			worker.protocol_major != trust_policy.protocol_major ||
			worker.protocol_minor != trust_policy.protocol_minor ||
			worker.required_features != trust_policy.required_features ||
			worker.sandbox_policy_digest != trust_policy.worker_sandbox_policy_digest)
			return sdk::unexpected(authority_mismatch("worker.trust_policy"));
		std::set<std::string, std::less<>> task_ids;
		std::set<std::string, std::less<>> execution_ids;
		for (const auto& task : tasks)
		{
			if (!task_ids.insert(task.provider_task_id).second ||
				!execution_ids.insert(task.provider_execution_id).second)
				return sdk::unexpected(authority_invalid("tasks", "duplicate-id"));
			if (auto valid = task.validate(project, group_topology); !valid)
				return valid;
			if (task.interpretation_domain != interpretation_policy.selected_domain ||
				task.sandbox.policy_digest != worker.sandbox_policy_digest ||
				task.condition_universe_id != tasks.front().condition_universe_id)
				return sdk::unexpected(authority_mismatch("task.interpretation-or-sandbox"));
		}
		const auto& first = tasks.front();
		if (publication.selector.catalog_id != project.catalog.catalog_id ||
			publication.selector.engine_generation_id != engine.engine_generation_id ||
			publication.selector.condition_universe_id != first.condition_universe_id ||
			publication.selector.relation_registry_digest != engine.engine_registry_digest ||
			publication.selector.interpretation_policy_digest !=
				interpretation_policy.interpretation_policy_digest ||
			publication.selector.trust_policy_digest != trust_policy.trust_policy_digest)
			return sdk::unexpected(authority_mismatch("publication.selector"));
		if (publication.partial_policy != group_topology.partial_policy)
			return sdk::unexpected(authority_mismatch("publication.partial_policy"));
		return {};
	}

	sdk::result<std::vector<std::byte>>
	provider_task_v4_request_authority::canonical_projection() const
	{
		if (auto valid = validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto encoded = sdk::canonical_binary(request_projection(*this));
		if (!encoded)
			return sdk::unexpected(std::move(encoded.error()));
		return encoded;
	}

	sdk::result<std::string> provider_task_v4_request_authority::authority_digest() const
	{
		auto projection = canonical_projection();
		if (!projection)
			return sdk::unexpected(std::move(projection.error()));
		std::string bytes;
		bytes.reserve(projection->size());
		for (const auto byte : *projection)
			bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		return sdk::semantic_digest(task_v4_authority_digest_domain, bytes);
	}

	sdk::result<void> source_closure_manifest::validate(const provider_task_v4_limits limits) const
	{
		if (schema != source_closure_manifest_schema)
			return sdk::unexpected(invalid("manifest.schema", "unsupported"));
		if (!source_closure_id_grammar(closure_id) || !semantic_digest_grammar(closure_digest) ||
			!semantic_digest_grammar(manifest_digest) ||
			closure_id != "source-closure:" + closure_digest)
			return sdk::unexpected(invalid("manifest.identity", "grammar"));
		if (auto member_check = validate_manifest_members(*this, limits); !member_check)
			return member_check;
		auto expected_closure = derive_source_closure_digest(*this);
		if (!expected_closure || *expected_closure != closure_digest)
			return sdk::unexpected(digest_mismatch("manifest.closure_digest", closure_digest));
		auto expected_manifest = derive_source_closure_manifest_digest(*this);
		if (!expected_manifest || *expected_manifest != manifest_digest)
			return sdk::unexpected(digest_mismatch("manifest.manifest_digest", manifest_digest));
		return {};
	}

	sdk::result<void> source_closure_summary::validate(const provider_task_v4_limits limits) const
	{
		if (!source_closure_id_grammar(source_closure_id) ||
			!semantic_digest_grammar(source_closure_digest) ||
			!semantic_digest_grammar(manifest_digest) ||
			source_closure_id != "source-closure:" + source_closure_digest)
			return sdk::unexpected(invalid("source_closure", "identity"));
		if (member_count == 0U || member_count > limits.maximum_members || blob_count == 0U ||
			blob_count > limits.maximum_unique_blobs ||
			unique_blob_bytes > limits.maximum_unique_blob_bytes)
			return sdk::unexpected(invalid("source_closure", "bounds"));
		return {};
	}

	sdk::result<void> provider_task_v4_source::validate(const provider_task_v4_limits limits) const
	{
		if (auto strong = validate_strong(source_snapshot_id, "source.source_snapshot_id"); !strong)
			return strong;
		if (!file_id_grammar(file_id) || !line_index_id_grammar(line_index_id))
			return sdk::unexpected(invalid("source", "identity"));
		if (auto path = validate_project_path(logical_path, "source.logical_path", limits); !path)
			return path;
		auto expected_file = source_closure_file_id(logical_path);
		if (!expected_file || file_id != *expected_file)
			return sdk::unexpected(invalid("source.file_id", "derived-id"));
		if (!content_digest_grammar(content_digest) || size_bytes > limits.maximum_blob_bytes ||
			!read_only)
			return sdk::unexpected(invalid("source", "metadata"));
		if (encoding != "utf8" && encoding != "utf16le" && encoding != "utf16be" &&
			encoding != "locale_dependent" && encoding != "binary_or_unknown")
			return sdk::unexpected(invalid("source.encoding", "enum"));
		return {};
	}

	sdk::result<void> provider_task_v4_open_task::validate() const
	{
		if (!content_digest_grammar(task_input_digest) ||
			!semantic_digest_grammar(normalized_invocation_digest) ||
			!semantic_digest_grammar(toolchain_digest) ||
			!content_digest_grammar(environment_digest))
			return sdk::unexpected(invalid("open_task", "digest"));
		return {};
	}

	sdk::result<void>
	provider_task_v4_base_task::validate(const provider_task_v4_limits limits) const
	{
		if (!task_id_grammar(provider_task_id))
			return sdk::unexpected(invalid("base.provider_task_id", "task-id"));
		if (auto execution = validate_strong(provider_execution_id, "base.provider_execution_id");
			!execution)
			return execution;
		if (!content_digest_grammar(canonical_base_task_digest))
			return sdk::unexpected(invalid("base.canonical_base_task_digest", "content-digest"));
		if (!content_or_semantic_digest_grammar(task_input_digest) ||
			!content_or_semantic_digest_grammar(normalized_invocation_digest) ||
			!content_or_semantic_digest_grammar(toolchain_digest) ||
			!content_or_semantic_digest_grammar(environment_digest))
			return sdk::unexpected(invalid("base", "digest"));
		if (auto directory =
				validate_project_path(working_directory, "base.working_directory", limits);
			!directory)
			return directory;
		return source.validate(limits);
	}

	sdk::result<void> provider_task_v4::validate(const provider_task_v4_limits limits) const
	{
		if (schema != provider_task_v4_schema)
			return sdk::unexpected(invalid("task.schema", "unsupported"));
		if (!task_id_grammar(task_id) || !semantic_digest_grammar(task_v4_digest) ||
			!task_id.starts_with("task:" + task_v4_digest) || base_task_index > 4095U ||
			!task_id_grammar(base_provider_task_id) || !content_digest_grammar(base_task_digest))
			return sdk::unexpected(invalid("task.identity", "grammar"));
		if (auto open = open_task.validate(); !open)
			return open;
		if (auto closure = source_closure.validate(limits); !closure)
			return closure;
		if (auto main = validate_project_path(main_logical_path, "task.main_logical_path", limits);
			!main)
			return main;
		return validate_project_path(
			logical_working_directory, "task.logical_working_directory", limits);
	}

	sdk::result<std::string> derive_source_closure_digest(const source_closure_manifest& manifest)
	{
		std::vector<sdk::canonical_value> members;
		members.reserve(manifest.members.size());
		for (const auto& member : manifest.members)
			members.push_back(member_tuple(member));
		std::vector<sdk::canonical_value> blobs;
		blobs.reserve(manifest.blobs.size());
		for (const auto& blob : manifest.blobs)
			blobs.push_back(blob_tuple(blob));
		return canonical_bytes_digest(
			source_closure_digest_domain,
			sdk::canonical_value::from_tuple({
				sdk::canonical_value::from_string(std::string{source_closure_digest_domain}),
				sdk::canonical_value::from_string("unicode-default-casefold-then-nfc"),
				sdk::canonical_value::from_tuple(std::move(members)),
				sdk::canonical_value::from_tuple(std::move(blobs)),
			}));
	}

	sdk::result<std::string>
	derive_source_closure_manifest_digest(const source_closure_manifest& manifest)
	{
		auto projection = manifest_projection(manifest);
		if (!projection)
			return sdk::unexpected(std::move(projection.error()));
		return semantic_json_digest(source_closure_manifest_digest_domain, *projection);
	}

	sdk::result<std::string> derive_provider_task_v4_digest(const provider_task_v4& task)
	{
		auto projection = task_projection(task);
		if (!projection)
			return sdk::unexpected(std::move(projection.error()));
		return semantic_json_digest(task_v4_digest_domain, *projection);
	}

	sdk::result<materialization::json_value>
	provider_task_v4_identity_projection(const provider_task_v4& task)
	{
		return task_projection(task);
	}

	sdk::result<void> validate_provider_task_v4_identity(const provider_task_v4& task)
	{
		if (auto valid = task.validate(); !valid)
			return valid;
		auto expected = derive_provider_task_v4_digest(task);
		if (!expected || task.task_v4_digest != *expected || task.task_id != "task:" + *expected)
			return sdk::unexpected(digest_mismatch("task.task_v4_digest", task.task_id));
		return {};
	}

	sdk::result<void> bind_source_closure_summary(const source_closure_summary& summary,
												  const source_closure_manifest& manifest,
												  const provider_task_v4_limits limits)
	{
		if (auto valid = summary.validate(limits); !valid)
			return valid;
		if (auto valid = manifest.validate(limits); !valid)
			return valid;
		std::uint64_t bytes{};
		for (const auto& blob : manifest.blobs)
		{
			if (bytes > limits.maximum_unique_blob_bytes - blob.size_bytes)
				return sdk::unexpected(invalid("manifest.blobs", "aggregate-size"));
			bytes += blob.size_bytes;
		}
		if (summary.source_closure_id != manifest.closure_id ||
			summary.source_closure_digest != manifest.closure_digest ||
			summary.manifest_digest != manifest.manifest_digest ||
			summary.member_count != manifest.members.size() ||
			summary.blob_count != manifest.blobs.size() || summary.unique_blob_bytes != bytes)
			return sdk::unexpected(mismatch("source_closure", summary.source_closure_id));
		return {};
	}

	sdk::result<void> bind_provider_task_v4_main_member(const provider_task_v4_base_task& base,
														const provider_task_v4& task,
														const source_closure_manifest& manifest,
														const provider_task_v4_limits limits)
	{
		if (auto valid = base.validate(limits); !valid)
			return valid;
		if (auto valid = validate_provider_task_v4_identity(task); !valid)
			return valid;
		if (auto valid = bind_source_closure_summary(task.source_closure, manifest, limits); !valid)
			return valid;
		if (task.base_provider_task_id != base.provider_task_id ||
			task.open_task.task_input_digest != base.task_input_digest ||
			task.open_task.normalized_invocation_digest != base.normalized_invocation_digest ||
			task.open_task.toolchain_digest != base.toolchain_digest ||
			task.open_task.environment_digest != base.environment_digest ||
			task.main_logical_path != base.source.logical_path ||
			task.logical_working_directory != base.working_directory)
			return sdk::unexpected(mismatch("task.base", base.provider_task_id));
		const auto main = std::ranges::find_if(manifest.members,
											   [](const auto& member)
											   {
												   return member.role == "main";
											   });
		if (main == manifest.members.end() ||
			std::ranges::count_if(manifest.members,
								  [](const auto& member)
								  {
									  return member.role == "main";
								  }) != 1U)
			return sdk::unexpected(invalid("manifest.members", "exactly-one-main"));
		if (main->file_id != base.source.file_id ||
			main->logical_path != base.source.logical_path ||
			main->content_digest != base.source.content_digest ||
			main->size_bytes != base.source.size_bytes || main->encoding != base.source.encoding ||
			main->read_only != base.source.read_only)
			return sdk::unexpected(mismatch("manifest.main", base.source.logical_path));
		return {};
	}

	sdk::result<void> bind_provider_task_v4_main_member(const provider_task_v4_base_task& base,
														const provider_task_v4& task,
														const source_closure_manifest& manifest,
														const source_closure_snapshot& snapshot,
														const provider_task_v4_limits limits)
	{
		if (auto valid = bind_provider_task_v4_main_member(base, task, manifest, limits); !valid)
			return valid;
		if (snapshot.closure_digest != manifest.closure_digest ||
			snapshot.snapshot_id != "source-closure:" + snapshot.closure_digest)
			return sdk::unexpected(mismatch("source_closure.snapshot", base.source.logical_path));
		auto line_index = source_closure_main_line_index_id(snapshot);
		if (!line_index)
			return sdk::unexpected(std::move(line_index.error()));
		if (*line_index != base.source.line_index_id)
			return sdk::unexpected(mismatch("source.line_index_id", base.source.logical_path));
		return {};
	}
} // namespace cxxlens::detail::clang22
