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

		[[nodiscard]] sdk::result<json_value>
		closure_summary_projection(const source_closure_summary& summary)
		{
			auto id = json_string(summary.source_closure_id, "source_closure_id");
			auto digest = json_string(summary.source_closure_digest, "source_closure_digest");
			auto manifest = json_string(summary.manifest_digest, "manifest_digest");
			if (!id || !digest || !manifest)
				return sdk::unexpected(invalid("source_closure", "invalid-utf8"));
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
			auto closure = closure_summary_projection(task.source_closure);
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
				sdk::canonical_value::from_string("cxxlens.clang22.source-closure.v2"),
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
} // namespace cxxlens::detail::clang22
