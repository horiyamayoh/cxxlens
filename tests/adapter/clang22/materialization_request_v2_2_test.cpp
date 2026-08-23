#include "llvm/clang22/materialization_request_v2_2.hpp"

#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "llvm/clang22/source_closure.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

	[[nodiscard]] std::string semantic(const char digit = 'a')
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string content(const char digit = 'b')
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] source_closure_manifest make_manifest()
	{
		source_closure_manifest value;
		value.members = {{"", "project://src/main.cpp", "main", "utf8", 7U, content('a'), true}};
		value.blobs = {{content('a'), 7U}};
		auto file = source_closure_file_id(value.members.front().logical_path);
		assert(file);
		value.members.front().file_id = *file;
		auto closure = derive_source_closure_digest(value);
		assert(closure);
		value.closure_digest = *closure;
		value.closure_id = "source-closure:" + *closure;
		auto manifest = derive_source_closure_manifest_digest(value);
		assert(manifest);
		value.manifest_digest = *manifest;
		return value;
	}

	[[nodiscard]] provider_task_v4_base_task make_base(const source_closure_manifest& manifest)
	{
		provider_task_v4_base_task value;
		value.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'c');
		value.provider_execution_id = "provider-execution:one";
		value.canonical_base_task_digest = content('d');
		value.task_input_digest = content('e');
		value.normalized_invocation_digest = semantic('f');
		value.toolchain_digest = semantic('1');
		value.environment_digest = content('2');
		value.working_directory = "project://src";
		value.source = {"source-snapshot:one",
						manifest.members.front().file_id,
						manifest.members.front().logical_path,
						manifest.members.front().content_digest,
						7U,
						manifest.members.front().encoding,
						"line-index:sha256:" + std::string(64U, '3'),
						true};
		return value;
	}

	[[nodiscard]] provider_task_v4 make_task(const source_closure_manifest& manifest,
											 const provider_task_v4_base_task& base)
	{
		provider_task_v4 value;
		value.base_task_index = 0U;
		value.base_provider_task_id = base.provider_task_id;
		value.base_task_digest = base.canonical_base_task_digest;
		value.open_task = {base.task_input_digest,
						   base.normalized_invocation_digest,
						   base.toolchain_digest,
						   base.environment_digest};
		value.source_closure = {
			manifest.closure_id, manifest.closure_digest, manifest.manifest_digest, 1U, 1U, 7U};
		value.main_logical_path = base.source.logical_path;
		value.logical_working_directory = base.working_directory;
		auto digest = derive_provider_task_v4_digest(value);
		assert(digest);
		value.task_v4_digest = *digest;
		value.task_id = "task:" + *digest;
		return value;
	}

	[[nodiscard]] json_value inherited_authority(const std::string& request_id,
												 const std::string& semantic_request_digest,
												 const bool include_source_bytes = false)
	{
		std::map<std::string, json_value, utf8_byte_less> fields;
		auto request_id_value = json_value::string(request_id);
		auto digest_value = json_value::string(semantic_request_digest);
		assert(request_id_value && digest_value);
		fields.emplace("materialization_request_id", *request_id_value);
		fields.emplace("semantic_request_digest", *digest_value);
		fields.emplace("engine", json_value::null());
		fields.emplace("group_topology", json_value::null());
		fields.emplace("interpretation_policy", json_value::null());
		fields.emplace("publication", json_value::null());
		fields.emplace("project", json_value::null());
		fields.emplace("registry", json_value::null());
		fields.emplace("tool", json_value::null());
		fields.emplace("trust_policy", json_value::null());
		fields.emplace("worker", json_value::null());
		std::map<std::string, json_value, utf8_byte_less> task_fields;
		if (include_source_bytes)
		{
			auto bytes = json_value::string("not-authority");
			assert(bytes);
			task_fields.emplace("content_base64", *bytes);
		}
		auto task = json_value::object(std::move(task_fields));
		assert(task);
		fields.emplace("tasks", json_value::array({*task}));
		auto object = json_value::object(std::move(fields));
		assert(object);
		return *object;
	}

	[[nodiscard]] materialization_request_v2_2 make_request(const source_closure_manifest& manifest,
															const provider_task_v4_base_task& base,
															const provider_task_v4& task)
	{
		assert(manifest.closure_id == task.source_closure.source_closure_id);
		materialization_request_v2_2 request;
		request.materialization_request_id = "materialization-authority:v2_2-test";
		request.semantic_request_digest = semantic('8');
		request.inherited_authority = inherited_authority(request.materialization_request_id,
														  request.semantic_request_digest);
		request.base_tasks = {base};
		request.source_closures = {task.source_closure};
		request.task_extensions = {task};
		auto digest = derive_materialization_request_v2_2_digest(request);
		assert(digest);
		request.request_digest = *digest;
		request.request_id = "materialization-request:" + *digest;
		return request;
	}

	void positive_and_manifest_cross_binding()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);
		auto request = make_request(manifest, base, task);
		const std::vector<std::string> advertised{
			"task-input-chunks-v2", "task-source-closure-v2", "optional-extension-v1"};
		const std::array manifests{manifest};
		auto validated = validate_materialization_request_v2_2(
			request, advertised, std::span<const source_closure_manifest>{manifests});
		assert(validated);
		assert(validated->negotiated_features == materialization_request_v2_2_required_features());
		assert(validated->unique_blob_bytes == 7U);
		auto same = derive_materialization_request_v2_2_digest(request);
		assert(same && *same == request.request_digest);
	}

	void version_feature_and_payload_rejection()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);
		const std::vector<std::string> advertised{"task-input-chunks-v2", "task-source-closure-v2"};

		auto missing = make_request(manifest, base, task);
		missing.required_features = {"task-input-chunks-v2"};
		assert(!validate_materialization_request_v2_2(missing, advertised));

		auto downgrade = make_request(manifest, base, task);
		downgrade.protocol_major = 1U;
		assert(!validate_materialization_request_v2_2(downgrade, advertised));
		assert(validate_materialization_request_v2_2(downgrade, advertised).error().code ==
			   "materialization.version-unsupported");

		auto legacy = make_request(manifest, base, task);
		legacy.request_version = "2.1.0";
		assert(!validate_materialization_request_v2_2(legacy, advertised));

		auto bytes = make_request(manifest, base, task);
		bytes.inherited_authority = inherited_authority(
			bytes.materialization_request_id, bytes.semantic_request_digest, true);
		assert(!validate_materialization_request_v2_2(bytes, advertised));
	}

	void identity_binding_and_bounds_rejection()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);
		const std::vector<std::string> advertised{"task-input-chunks-v2", "task-source-closure-v2"};

		auto stale_task = make_request(manifest, base, task);
		stale_task.task_extensions.front().task_v4_digest = semantic('9');
		assert(!validate_materialization_request_v2_2(stale_task, advertised));

		auto stale_base = make_request(manifest, base, task);
		stale_base.base_tasks.front().environment_digest = content('0');
		assert(!validate_materialization_request_v2_2(stale_base, advertised));

		auto bound = make_request(manifest, base, task);
		materialization_request_v2_2_limits limits;
		limits.maximum_unique_blob_bytes = 6U;
		assert(!validate_materialization_request_v2_2(bound, advertised, limits));

		auto duplicate = make_request(manifest, base, task);
		duplicate.source_closures.push_back(task.source_closure);
		assert(!validate_materialization_request_v2_2(duplicate, advertised));
	}

	[[nodiscard]] json_value text(const std::string_view value)
	{
		auto encoded = json_value::string(std::string{value});
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value object(std::map<std::string, json_value, utf8_byte_less> fields)
	{
		auto encoded = json_value::object(std::move(fields));
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value document_shape(const bool source_bytes, const bool future_minor)
	{
		std::map<std::string, json_value, utf8_byte_less> source;
		source.emplace("content_digest", text(content('a')));
		source.emplace("encoding", text("utf8"));
		source.emplace("file_id", text("file:sha256:" + std::string(64U, 'b')));
		source.emplace("line_index_id", text("line-index:sha256:" + std::string(64U, 'c')));
		source.emplace("logical_path", text("project://src/main.cpp"));
		source.emplace("read_only", json_value::boolean(true));
		source.emplace("size_bytes", json_value::unsigned_integer(7U));
		source.emplace("source_snapshot_id", text("source-snapshot:one"));
		if (source_bytes)
			source.emplace("content_base64", text("forbidden"));
		auto task = object({{"source", object(std::move(source))}});
		auto open_task = object({{"environment_digest", text(content('d'))},
								 {"normalized_invocation_digest", text(semantic('e'))},
								 {"task_input_digest", text(content('f'))},
								 {"toolchain_digest", text(semantic('1'))}});
		auto closure =
			object({{"digest", text(semantic('2'))},
					{"id", text("source-closure:semantic-v2:sha256:" + std::string(64U, '3'))},
					{"manifest_digest", text(semantic('4'))}});
		auto extension = object({
			{"base_provider_task_id", text("task:semantic-v2:sha256:" + std::string(64U, '5'))},
			{"base_task_digest", text(content('6'))},
			{"base_task_index", json_value::unsigned_integer(0U)},
			{"logical_working_directory", text("project://src")},
			{"main_logical_path", text("project://src/main.cpp")},
			{"open_task", std::move(open_task)},
			{"schema", text("cxxlens.clang22.task.v4")},
			{"source_closure", std::move(closure)},
			{"task_id", text("task:semantic-v2:sha256:" + std::string(64U, '7'))},
			{"task_v4_digest", text(semantic('8'))},
		});
		auto summary = object({
			{"blob_count", json_value::unsigned_integer(1U)},
			{"manifest_digest", text(semantic('4'))},
			{"member_count", json_value::unsigned_integer(1U)},
			{"source_closure_digest", text(semantic('2'))},
			{"source_closure_id",
			 text("source-closure:semantic-v2:sha256:" + std::string(64U, '3'))},
			{"unique_blob_bytes", json_value::unsigned_integer(7U)},
		});
		auto worker = object({
			{"protocol_major", json_value::unsigned_integer(2U)},
			{"protocol_minor", json_value::unsigned_integer(future_minor ? 1U : 0U)},
		});
		std::map<std::string, json_value, utf8_byte_less> root;
		for (const auto name : {"engine",
								"group_topology",
								"interpretation_policy",
								"publication",
								"project",
								"registry",
								"tool",
								"trust_policy"})
			root.emplace(name, object({}));
		root.emplace("worker", std::move(worker));
		root.emplace("request_digest", text(semantic('9')));
		root.emplace("request_id", text("materialization-request:" + semantic('9')));
		root.emplace("request_version", text("2.2.0"));
		root.emplace(
			"required_features",
			json_value::array({text("task-input-chunks-v2"), text("task-source-closure-v2")}));
		root.emplace("schema", text("cxxlens.clang22-materialization-request.v2_2"));
		root.emplace("materialization_request_id", text("materialization-authority:v2_2-test"));
		root.emplace("semantic_request_digest", text(semantic('a')));
		root.emplace("source_closures", json_value::array({std::move(summary)}));
		root.emplace("tasks", json_value::array({std::move(task)}));
		root.emplace("task_extensions", json_value::array({std::move(extension)}));
		return object(std::move(root));
	}

	void document_ingress_is_closed_before_transport()
	{
		assert(validate_materialization_request_v2_2_document(document_shape(false, false)));
		auto bytes = validate_materialization_request_v2_2_document(document_shape(true, false));
		assert(!bytes && bytes.error().code == "materialization.request-v2_2-invalid");
		auto future = validate_materialization_request_v2_2_document(document_shape(false, true));
		assert(!future && future.error().code == "materialization.version-unsupported");
	}
} // namespace

int main()
{
	positive_and_manifest_cross_binding();
	version_feature_and_payload_rejection();
	identity_binding_and_bounds_rejection();
	document_ingress_is_closed_before_transport();
}
