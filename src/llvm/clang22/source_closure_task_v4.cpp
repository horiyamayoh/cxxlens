#include "source_closure_task_v4.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "materialization_json.hpp"

namespace cxxlens::detail::clang22
{
	namespace
	{
		using materialization::json_value;

		constexpr std::string_view task_v4_schema{"cxxlens.clang22.task.v4"};
		constexpr std::string_view task_v4_domain{"cxxlens.clang22.task.v4"};
		constexpr std::string_view task_id_prefix{"task:semantic-v2:sha256:"};
		constexpr std::string_view semantic_prefix{"semantic-v2:sha256:"};
		constexpr std::string_view content_prefix{"sha256:"};

		[[nodiscard]] sdk::error
		failure(std::string code, std::string field, std::string detail = {})
		{
			return {std::move(code), std::move(field), std::move(detail)};
		}

		[[nodiscard]] bool lower_hex(const std::string_view value) noexcept
		{
			return std::ranges::all_of(value,
									   [](const char byte)
									   {
										   return (byte >= '0' && byte <= '9') ||
											   (byte >= 'a' && byte <= 'f');
									   });
		}

		[[nodiscard]] bool typed_digest(const std::string_view value,
										const std::string_view prefix) noexcept
		{
			return value.size() == prefix.size() + 64U && value.starts_with(prefix) &&
				lower_hex(value.substr(prefix.size()));
		}

		[[nodiscard]] bool typed_task_id(const std::string_view value) noexcept
		{
			return typed_digest(value, task_id_prefix);
		}

		[[nodiscard]] sdk::result<json_value> object(json_value::object_type fields,
													 const std::string_view field)
		{
			auto output = json_value::object(std::move(fields));
			if (!output)
				return sdk::unexpected(failure(
					"source-closure.task-v4-invalid", std::string{field}, output.error().detail));
			return std::move(*output);
		}

		[[nodiscard]] std::string_view role_name(const source_closure_role role) noexcept
		{
			switch (role)
			{
				case source_closure_role::main:
					return "main";
				case source_closure_role::header:
					return "header";
				case source_closure_role::generated:
					return "generated";
				case source_closure_role::forced_include:
					return "forced-include";
				case source_closure_role::macro_file:
					return "macro-file";
			}
			return {};
		}

		[[nodiscard]] std::string_view
		encoding_name(const source_closure_encoding encoding) noexcept
		{
			switch (encoding)
			{
				case source_closure_encoding::utf8:
					return "utf8";
				case source_closure_encoding::utf16le:
					return "utf16le";
				case source_closure_encoding::utf16be:
					return "utf16be";
				case source_closure_encoding::locale_dependent:
					return "locale_dependent";
				case source_closure_encoding::binary_or_unknown:
					return "binary_or_unknown";
			}
			return {};
		}

		[[nodiscard]] sdk::result<std::vector<std::byte>>
		canonical_json_bytes(const json_value& value, const std::string_view field)
		{
			const auto encoded = materialization::canonical_json(value);
			if (encoded.size() > source_closure_task_v4_maximum_payload_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", std::string{field}, "payload"));
			const auto bytes = std::as_bytes(std::span{encoded.data(), encoded.size()});
			return std::vector<std::byte>{bytes.begin(), bytes.end()};
		}

		[[nodiscard]] sdk::result<std::uint64_t> unsigned_integer(const json_value& value,
																  const std::string_view field)
		{
			if (const auto* unsigned_value = value.as_unsigned_integer())
				return *unsigned_value;
			if (const auto* signed_value = value.as_signed_integer();
				signed_value != nullptr && *signed_value >= 0)
				return static_cast<std::uint64_t>(*signed_value);
			return sdk::unexpected(
				failure("source-closure.task-v4-invalid", std::string{field}, "unsigned-integer"));
		}

		[[nodiscard]] sdk::result<std::string> string_value(const json_value& value,
															const std::string_view field)
		{
			if (const auto* output = value.as_string())
				return *output;
			return sdk::unexpected(
				failure("source-closure.task-v4-invalid", std::string{field}, "string"));
		}

		[[nodiscard]] sdk::result<const json_value*>
		member(const json_value& value, const std::string_view name, const std::string_view field)
		{
			const auto* object_value = value.as_object();
			if (object_value == nullptr)
				return sdk::unexpected(
					failure("source-closure.task-v4-invalid", std::string{field}, "object"));
			const auto* found = value.member(name);
			if (found == nullptr)
				return sdk::unexpected(failure("source-closure.task-v4-invalid",
											   std::string{field},
											   "missing:" + std::string{name}));
			return found;
		}

		[[nodiscard]] sdk::result<json_value> manifest_value(const source_closure_snapshot& closure)
		{
			std::vector<json_value> members;
			members.reserve(closure.members.size());
			for (const auto& item : closure.members)
			{
				std::array fields{
					std::pair<std::string, json_value>{"content_digest", json_value::null()},
					std::pair<std::string, json_value>{"encoding", json_value::null()},
					std::pair<std::string, json_value>{"file_id", json_value::null()},
					std::pair<std::string, json_value>{"logical_path", json_value::null()},
					std::pair<std::string, json_value>{"read_only",
													   json_value::boolean(item.read_only)},
					std::pair<std::string, json_value>{"role", json_value::null()},
					std::pair<std::string, json_value>{
						"size_bytes", json_value::unsigned_integer(item.size_bytes)},
				};
				for (auto& [key, value] : fields)
				{
					if (key == "content_digest")
						value = json_value::string(item.content_digest).value();
					else if (key == "encoding")
						value =
							json_value::string(std::string{encoding_name(item.encoding)}).value();
					else if (key == "file_id")
						value = json_value::string(item.file_id).value();
					else if (key == "logical_path")
						value = json_value::string(item.logical_path).value();
					else if (key == "role")
						value = json_value::string(std::string{role_name(item.role)}).value();
				}
				auto entry = json_value::object({fields.begin(), fields.end()});
				if (!entry)
					return sdk::unexpected(failure("source-closure.manifest-invalid", "member"));
				members.push_back(std::move(*entry));
			}

			std::vector<json_value> blobs;
			blobs.reserve(closure.blobs.size());
			for (const auto& item : closure.blobs)
			{
				json_value::object_type fields;
				fields.emplace("content_digest", json_value::string(item.content_digest).value());
				fields.emplace("size_bytes", json_value::unsigned_integer(item.size_bytes));
				auto entry = json_value::object(std::move(fields));
				if (!entry)
					return sdk::unexpected(failure("source-closure.manifest-invalid", "blob"));
				blobs.push_back(std::move(*entry));
			}

			json_value::object_type root;
			root.emplace("blobs", json_value::array(std::move(blobs)));
			root.emplace("closure_digest", json_value::string(closure.closure_digest).value());
			root.emplace("closure_id", json_value::string(closure.snapshot_id).value());
			root.emplace("members", json_value::array(std::move(members)));
			root.emplace("schema",
						 json_value::string(std::string{source_closure_manifest_schema}).value());
			auto output = json_value::object(std::move(root));
			if (!output)
				return sdk::unexpected(failure("source-closure.manifest-invalid", "manifest"));
			return std::move(*output);
		}

		[[nodiscard]] sdk::result<json_value>
		open_task_value(const source_closure_task_v4_input& input)
		{
			json_value::object_type fields;
			fields.emplace("environment_digest",
						   json_value::string(input.environment_digest).value());
			fields.emplace("normalized_invocation_digest",
						   json_value::string(input.normalized_invocation_digest).value());
			fields.emplace("task_input_digest",
						   json_value::string(input.task_input_digest).value());
			fields.emplace("toolchain_digest", json_value::string(input.toolchain_digest).value());
			return object(std::move(fields), "open_task");
		}

		[[nodiscard]] sdk::result<json_value>
		closure_reference_value(const source_closure_task_v4_input& input,
								const std::string_view manifest_digest)
		{
			json_value::object_type fields;
			fields.emplace("digest", json_value::string(input.closure.closure_digest).value());
			fields.emplace("id", json_value::string(input.closure.snapshot_id).value());
			fields.emplace("manifest_digest",
						   json_value::string(std::string{manifest_digest}).value());
			return object(std::move(fields), "source_closure");
		}

		[[nodiscard]] sdk::result<json_value>
		projection_value(const source_closure_task_v4_input& input,
						 const std::string_view base_digest,
						 const std::string_view manifest_digest)
		{
			auto open = open_task_value(input);
			if (!open)
				return sdk::unexpected(std::move(open.error()));
			auto closure = closure_reference_value(input, manifest_digest);
			if (!closure)
				return sdk::unexpected(std::move(closure.error()));
			json_value::object_type fields;
			fields.emplace("base_provider_task_id",
						   json_value::string(input.base_provider_task_id).value());
			fields.emplace("base_task_index", json_value::unsigned_integer(input.base_task_index));
			fields.emplace("base_task_digest",
						   json_value::string(std::string{base_digest}).value());
			fields.emplace("logical_working_directory",
						   json_value::string(input.logical_working_directory).value());
			fields.emplace("main_logical_path",
						   json_value::string(input.main_logical_path).value());
			fields.emplace("open_task", std::move(*open));
			fields.emplace("schema", json_value::string(std::string{task_v4_schema}).value());
			fields.emplace("source_closure", std::move(*closure));
			return object(std::move(fields), "task_v4_projection");
		}

		[[nodiscard]] sdk::result<json_value>
		payload_value(const source_closure_task_v4_input& input,
					  const std::string_view base_digest,
					  const std::string_view manifest_digest,
					  const std::string_view task_id,
					  const std::string_view task_digest)
		{
			auto projection = projection_value(input, base_digest, manifest_digest);
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			auto fields = *projection->as_object();
			fields.emplace("task_id", json_value::string(std::string{task_id}).value());
			fields.emplace("task_v4_digest", json_value::string(std::string{task_digest}).value());
			return object(std::move(fields), "task_v4_payload");
		}

		[[nodiscard]] sdk::result<std::string>
		canonical_base_digest(const std::vector<std::byte>& projection)
		{
			if (projection.empty() ||
				projection.size() > source_closure_task_v4_maximum_payload_bytes)
				return sdk::unexpected(
					failure("source-closure.limit-exceeded", "base-task-projection"));
			const auto bytes =
				std::string{reinterpret_cast<const char*>(projection.data()), projection.size()};
			auto document = materialization::parse_json_object(
				bytes, materialization::json_limits{source_closure_task_v4_maximum_payload_bytes});
			if (!document)
				return sdk::unexpected(failure("source-closure.task-v4-invalid",
											   "base-task-projection",
											   document.error().detail));
			if (materialization::canonical_json(document->root()) != bytes)
				return sdk::unexpected(failure(
					"source-closure.task-v4-invalid", "base-task-projection", "noncanonical"));
			return sdk::content_digest(std::as_bytes(std::span{bytes.data(), bytes.size()}));
		}

		[[nodiscard]] sdk::result<source_closure_task_v4_identity>
		build_identity(const source_closure_task_v4_input& input,
					   const std::string_view base_digest,
					   const std::string_view manifest_digest,
					   const std::optional<std::string_view> expected_task_id = std::nullopt,
					   const std::optional<std::string_view> expected_task_digest = std::nullopt)
		{
			if (!typed_digest(base_digest, content_prefix) ||
				!typed_task_id(input.base_provider_task_id) ||
				!typed_digest(input.task_input_digest, content_prefix) ||
				!typed_digest(input.normalized_invocation_digest, semantic_prefix) ||
				!typed_digest(input.toolchain_digest, semantic_prefix) ||
				!typed_digest(input.environment_digest, content_prefix) ||
				input.base_task_index > 4095U)
				return sdk::unexpected(failure("source-closure.task-v4-invalid", "authority"));
			if (auto valid = input.closure.validate(); !valid)
				return sdk::unexpected(std::move(valid.error()));
			if (!source_closure_relative_path(input.main_logical_path) ||
				!source_closure_relative_path(input.logical_working_directory))
				return sdk::unexpected(failure("source-closure.path-invalid", "task-v4-path"));
			const auto* main = input.closure.find_member(input.main_logical_path);
			if (main == nullptr || main->role != source_closure_role::main)
				return sdk::unexpected(failure("source-closure.main-invalid", "main-logical-path"));

			auto projection = projection_value(input, base_digest, manifest_digest);
			if (!projection)
				return sdk::unexpected(std::move(projection.error()));
			auto projection_bytes = canonical_json_bytes(*projection, "task-v4-projection");
			if (!projection_bytes)
				return sdk::unexpected(std::move(projection_bytes.error()));
			const auto projection_text = std::string{
				reinterpret_cast<const char*>(projection_bytes->data()), projection_bytes->size()};
			auto task_digest = sdk::semantic_digest(task_v4_domain, projection_text);
			if (!task_digest)
				return sdk::unexpected(std::move(task_digest.error()));
			const auto task_id = "task:" + *task_digest;
			if (expected_task_digest && *expected_task_digest != *task_digest)
				return sdk::unexpected(
					failure("source-closure.task-v4-digest-mismatch", "task_v4_digest"));
			if (expected_task_id && *expected_task_id != task_id)
				return sdk::unexpected(
					failure("source-closure.task-v4-digest-mismatch", "task_id"));

			auto payload =
				payload_value(input, base_digest, manifest_digest, task_id, *task_digest);
			if (!payload)
				return sdk::unexpected(std::move(payload.error()));
			auto payload_bytes = canonical_json_bytes(*payload, "task-v4-input-payload");
			if (!payload_bytes)
				return sdk::unexpected(std::move(payload_bytes.error()));
			const auto input_digest = sdk::content_digest(*payload_bytes);
			return source_closure_task_v4_identity{task_id,
												   *task_digest,
												   std::string{base_digest},
												   std::string{manifest_digest},
												   input_digest,
												   std::move(*projection_bytes),
												   std::move(*payload_bytes)};
		}

		[[nodiscard]] sdk::result<std::string> expected_string(const json_value& root,
															   const std::string_view name,
															   const std::string_view field)
		{
			auto value = member(root, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			return string_value(**value, field);
		}

		[[nodiscard]] sdk::result<const json_value*> expected_object_member(
			const json_value& root, const std::string_view name, const std::string_view field)
		{
			auto value = member(root, name, field);
			if (!value)
				return sdk::unexpected(std::move(value.error()));
			if ((*value)->as_object() == nullptr)
				return sdk::unexpected(
					failure("source-closure.task-v4-invalid", std::string{field}, "object"));
			return *value;
		}

		[[nodiscard]] sdk::result<void>
		require_exact_object(const json_value& value,
							 const std::span<const std::string_view> names,
							 const std::string_view field)
		{
			if (!value.has_exact_members(names))
				return sdk::unexpected(
					failure("source-closure.task-v4-invalid", std::string{field}, "field-census"));
			return {};
		}

	} // namespace

	sdk::result<std::string> source_closure_manifest_digest(const source_closure_snapshot& closure)
	{
		if (auto valid = closure.validate(); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto manifest = manifest_value(closure);
		if (!manifest)
			return sdk::unexpected(std::move(manifest.error()));
		return sdk::semantic_digest(source_closure_manifest_digest_domain,
									materialization::canonical_json(*manifest));
	}

	sdk::result<source_closure_task_v4_identity>
	derive_source_closure_task_v4_identity(const source_closure_task_v4_input& input)
	{
		auto base_digest = canonical_base_digest(input.base_task_projection);
		if (!base_digest)
			return sdk::unexpected(std::move(base_digest.error()));
		auto manifest_digest = source_closure_manifest_digest(input.closure);
		if (!manifest_digest)
			return sdk::unexpected(std::move(manifest_digest.error()));
		return build_identity(input, *base_digest, *manifest_digest);
	}

	sdk::result<void>
	validate_source_closure_task_v4_input_digest(const std::span<const std::byte> payload,
												 const std::string_view expected_input_digest)
	{
		if (!typed_digest(expected_input_digest, content_prefix))
			return sdk::unexpected(failure("source-closure.task-v4-input-digest-mismatch",
										   "task_v4_input_digest",
										   "typed-digest"));
		if (payload.size() > source_closure_task_v4_maximum_payload_bytes)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "task-v4-input-payload"));
		const auto observed = sdk::content_digest(payload);
		if (observed != expected_input_digest)
			return sdk::unexpected(failure(
				"source-closure.task-v4-input-digest-mismatch", "task_v4_input_digest", "payload"));
		return {};
	}

	sdk::result<source_closure_task_v4_decoded>
	decode_source_closure_task_v4_input(const std::span<const std::byte> payload,
										const source_closure_snapshot& closure,
										const std::string_view expected_base_task_digest,
										const std::string_view expected_task_v4_input_digest)
	{
		if (!typed_digest(expected_base_task_digest, content_prefix))
			return sdk::unexpected(failure("source-closure.task-v4-invalid", "base-task-digest"));
		if (payload.empty() || payload.size() > source_closure_task_v4_maximum_payload_bytes)
			return sdk::unexpected(
				failure("source-closure.limit-exceeded", "task-v4-input-payload"));
		std::string raw;
		auto parsed = materialization::parse_json_object(
			std::string{reinterpret_cast<const char*>(payload.data()), payload.size()},
			materialization::json_limits{source_closure_task_v4_maximum_payload_bytes});
		if (!parsed)
			return sdk::unexpected(failure(
				"source-closure.task-v4-invalid", "task-v4-input-payload", parsed.error().detail));
		raw = parsed->raw_bytes();
		if (materialization::canonical_json(parsed->root()) != raw)
			return sdk::unexpected(
				failure("source-closure.task-v4-invalid", "task-v4-input-payload", "noncanonical"));
		if (auto valid = validate_source_closure_task_v4_input_digest(
				payload, expected_task_v4_input_digest);
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		const auto& root = parsed->root();
		constexpr std::array<std::string_view, 10U> root_fields{"base_provider_task_id",
																"base_task_index",
																"base_task_digest",
																"logical_working_directory",
																"main_logical_path",
																"open_task",
																"schema",
																"source_closure",
																"task_id",
																"task_v4_digest"};
		if (auto valid = require_exact_object(root, root_fields, "task-v4-input-payload"); !valid)
			return sdk::unexpected(std::move(valid.error()));
		auto schema = expected_string(root, "schema", "schema");
		if (!schema || *schema != task_v4_schema)
			return sdk::unexpected(failure("source-closure.task-v4-invalid", "schema"));
		auto task_id = expected_string(root, "task_id", "task_id");
		auto task_digest = expected_string(root, "task_v4_digest", "task_v4_digest");
		auto base_provider =
			expected_string(root, "base_provider_task_id", "base_provider_task_id");
		auto base_digest = expected_string(root, "base_task_digest", "base_task_digest");
		auto main_path = expected_string(root, "main_logical_path", "main_logical_path");
		auto working_directory =
			expected_string(root, "logical_working_directory", "logical_working_directory");
		if (!task_id || !task_digest || !base_provider || !base_digest || !main_path ||
			!working_directory)
			return sdk::unexpected(failure("source-closure.task-v4-invalid", "identity"));
		auto index_member = member(root, "base_task_index", "base_task_index");
		if (!index_member)
			return sdk::unexpected(std::move(index_member.error()));
		auto base_index = unsigned_integer(**index_member, "base_task_index");
		if (!base_index)
			return sdk::unexpected(std::move(base_index.error()));
		auto open = expected_object_member(root, "open_task", "open_task");
		auto closure_ref = expected_object_member(root, "source_closure", "source_closure");
		if (!open || !closure_ref)
			return sdk::unexpected(failure("source-closure.task-v4-invalid", "authority"));
		constexpr std::array<std::string_view, 4U> open_fields{"environment_digest",
															   "normalized_invocation_digest",
															   "task_input_digest",
															   "toolchain_digest"};
		if (auto valid = require_exact_object(**open, open_fields, "open_task"); !valid)
			return sdk::unexpected(std::move(valid.error()));
		constexpr std::array<std::string_view, 3U> closure_fields{
			"digest", "id", "manifest_digest"};
		if (auto valid = require_exact_object(**closure_ref, closure_fields, "source_closure");
			!valid)
			return sdk::unexpected(std::move(valid.error()));
		auto task_input_digest =
			expected_string(**open, "task_input_digest", "open_task.task_input_digest");
		auto invocation_digest = expected_string(
			**open, "normalized_invocation_digest", "open_task.normalized_invocation_digest");
		auto toolchain_digest =
			expected_string(**open, "toolchain_digest", "open_task.toolchain_digest");
		auto environment_digest =
			expected_string(**open, "environment_digest", "open_task.environment_digest");
		auto closure_id = expected_string(**closure_ref, "id", "source_closure.id");
		auto closure_digest = expected_string(**closure_ref, "digest", "source_closure.digest");
		auto manifest_digest =
			expected_string(**closure_ref, "manifest_digest", "source_closure.manifest_digest");
		if (!task_input_digest || !invocation_digest || !toolchain_digest ||
			!environment_digest || !closure_id || !closure_digest || !manifest_digest)
			return sdk::unexpected(failure("source-closure.task-v4-invalid", "authority"));
		if (*closure_id != closure.snapshot_id || *closure_digest != closure.closure_digest)
			return sdk::unexpected(
				failure("source-closure.task-binding-mismatch", "source_closure"));
		auto expected_manifest = source_closure_manifest_digest(closure);
		if (!expected_manifest || *expected_manifest != *manifest_digest)
			return sdk::unexpected(failure("source-closure.digest-mismatch", "manifest_digest"));
		source_closure_task_v4_input input;
		input.base_task_index = *base_index;
		input.base_provider_task_id = std::move(*base_provider);
		input.task_input_digest = std::move(*task_input_digest);
		input.normalized_invocation_digest = std::move(*invocation_digest);
		input.toolchain_digest = std::move(*toolchain_digest);
		input.environment_digest = std::move(*environment_digest);
		input.closure = closure;
		input.main_logical_path = std::move(*main_path);
		input.logical_working_directory = std::move(*working_directory);
		auto identity = build_identity(
			input, expected_base_task_digest, *expected_manifest, *task_id, *task_digest);
		if (!identity)
			return sdk::unexpected(std::move(identity.error()));
		if (identity->input_payload != std::vector<std::byte>{payload.begin(), payload.end()})
			return sdk::unexpected(failure(
				"source-closure.task-v4-invalid", "task-v4-input-payload", "projection-reencode"));
		return source_closure_task_v4_decoded{std::move(input), std::move(*identity)};
	}
} // namespace cxxlens::detail::clang22
