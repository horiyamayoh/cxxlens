#include "materialization_request_v2_2.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
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
			auto schema = json_text(request.schema, "schema");
			auto version = json_text(request.request_version, "request_version");
			if (!schema || !version)
				return sdk::unexpected(invalid("request", "invalid-utf8"));
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
				{"inherited_authority", request.inherited_authority},
				{"request_version", std::move(*version)},
				{"schema", std::move(*schema)},
				{"required_features", json_value::array(std::move(features))},
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

		[[nodiscard]] sdk::result<void>
		validate_inherited_authority(const materialization_request_v2_2& request)
		{
			const auto* object = request.inherited_authority.as_object();
			if (object == nullptr)
				return sdk::unexpected(invalid("inherited_authority", "object-required"));
			constexpr std::array<std::string_view, 12U> exact{"engine",
															  "group_topology",
															  "interpretation_policy",
															  "materialization_request_id",
															  "publication",
															  "project",
															  "registry",
															  "semantic_request_digest",
															  "tasks",
															  "tool",
															  "trust_policy",
															  "worker"};
			if (!request.inherited_authority.has_exact_members(exact))
				return sdk::unexpected(invalid("inherited_authority", "member-set"));
			if (contains_forbidden_source_bytes(request.inherited_authority))
				return sdk::unexpected(invalid("inherited_authority", "source-bytes-forbidden"));
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
			return {};
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
			if (auto inherited = validate_inherited_authority(request); !inherited)
				return inherited;
			for (const auto& base : request.base_tasks)
				if (auto valid = base.validate(limits.task_limits); !valid)
					return valid;
			return {};
		}
	} // namespace

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
		return validated_materialization_request_v2_2{
			std::move(request), std::move(*negotiated), *total};
	}
} // namespace cxxlens::detail::clang22::materialization
