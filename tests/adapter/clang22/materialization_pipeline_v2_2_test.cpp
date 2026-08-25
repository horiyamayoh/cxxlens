#include "llvm/clang22/materialization_pipeline_v2_2.hpp"

#include <array>
#include <cassert>
#include <map>
#include <span>
#include <string>
#include <vector>

#include "llvm/clang22/source_closure.hpp"
#include "materialization_request_v2_2_fixture.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;
	using namespace cxxlens::detail::clang22::materialization;

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] source_closure_manifest make_manifest()
	{
		source_closure_manifest manifest;
		manifest.members = {{"", "project://src/main.cpp", "main", "utf8", 7U, content('a'), true}};
		manifest.blobs = {{content('a'), 7U}};
		auto file_id = source_closure_file_id(manifest.members.front().logical_path);
		assert(file_id);
		manifest.members.front().file_id = *file_id;
		auto closure = derive_source_closure_digest(manifest);
		assert(closure);
		manifest.closure_digest = *closure;
		manifest.closure_id = "source-closure:" + *closure;
		auto digest = derive_source_closure_manifest_digest(manifest);
		assert(digest);
		manifest.manifest_digest = *digest;
		return manifest;
	}

	[[nodiscard]] provider_task_v4_base_task make_base(const source_closure_manifest& manifest)
	{
		provider_task_v4_base_task base;
		base.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'c');
		base.provider_execution_id = "provider-execution:one";
		base.canonical_base_task_digest = content('d');
		base.task_input_digest = content('e');
		base.normalized_invocation_digest = semantic('f');
		base.toolchain_digest = semantic('1');
		base.environment_digest = content('2');
		base.working_directory = "project://src";
		base.source = {"source-snapshot:one",
					   manifest.members.front().file_id,
					   manifest.members.front().logical_path,
					   manifest.members.front().content_digest,
					   manifest.members.front().size_bytes,
					   manifest.members.front().encoding,
					   "line-index:sha256:" + std::string(64U, '3'),
					   true};
		return base;
	}

	[[nodiscard]] provider_task_v4 make_task(const source_closure_manifest& manifest,
											 const provider_task_v4_base_task& base)
	{
		provider_task_v4 task;
		task.base_task_index = 0U;
		task.base_provider_task_id = base.provider_task_id;
		task.base_task_digest = base.canonical_base_task_digest;
		task.open_task = {base.task_input_digest,
						  base.normalized_invocation_digest,
						  base.toolchain_digest,
						  base.environment_digest};
		task.source_closure = {
			manifest.closure_id, manifest.closure_digest, manifest.manifest_digest, 1U, 1U, 7U};
		task.main_logical_path = base.source.logical_path;
		task.logical_working_directory = base.working_directory;
		auto digest = derive_provider_task_v4_digest(task);
		assert(digest);
		task.task_v4_digest = *digest;
		task.task_id = "task:" + *digest;
		return task;
	}

	[[nodiscard]] json_value make_inherited_authority(const std::string& request_id,
													  const std::string& semantic_request_digest)
	{
		auto document = cxxlens_test_materialization_request_v2_2_complete_document();
		auto fields = *document.as_object();
		auto materialization_id = json_value::string(request_id);
		auto semantic_digest = json_value::string(semantic_request_digest);
		assert(materialization_id && semantic_digest);
		fields.insert_or_assign("materialization_request_id", *materialization_id);
		fields.insert_or_assign("semantic_request_digest", *semantic_digest);
		auto object = json_value::object(std::move(fields));
		assert(object);
		return *object;
	}

	[[nodiscard]] materialization_request_v2_2 make_request(const source_closure_manifest& manifest,
															const provider_task_v4_base_task& base,
															const provider_task_v4& task)
	{
		static_cast<void>(manifest);
		materialization_request_v2_2 request;
		request.materialization_request_id = "materialization-authority:v2_2-test";
		request.semantic_request_digest = semantic('8');
		request.inherited_authority = make_inherited_authority(request.materialization_request_id,
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

	void closure_is_required_before_acceptance()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);
		auto request = make_request(manifest, base, task);
		const std::vector<std::string> features{"task-input-chunks-v2", "task-source-closure-v2"};

		auto pretransfer = validate_materialization_request_v2_2(request, features);
		assert(pretransfer);
		auto rejected = admit_materialization_request_v2_2_for_execution(
			request, features, std::span<const source_closure_manifest>{});
		assert(!rejected);
		assert(rejected.error().code == "materialization.source-closure-invalid");

		const std::array manifests{manifest};
		auto admission = admit_materialization_request_v2_2_for_execution(
			std::move(request), features, std::span<const source_closure_manifest>{manifests});
		assert(admission);
		auto accepted = accept_materialization_task_v2_2(*admission, 0U);
		assert(accepted);
		assert(accepted->phase == materialization_v2_2_task_phase::task_accepted);
		assert(begin_materialization_task_v2_2(*accepted));
		assert(accepted->phase == materialization_v2_2_task_phase::materialization_started);
	}

	void stale_manifest_and_legacy_task_are_rejected()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);
		auto request = make_request(manifest, base, task);
		const std::vector<std::string> features{"task-input-chunks-v2", "task-source-closure-v2"};
		const std::array manifests{manifest};

		auto stale = manifest;
		stale.manifest_digest = semantic('9');
		const std::array stale_manifests{stale};
		auto rejected = admit_materialization_request_v2_2_for_execution(
			std::move(request),
			features,
			std::span<const source_closure_manifest>{stale_manifests});
		assert(!rejected);

		auto legacy = make_request(manifest, base, task);
		legacy.request_version = "2.1.0";
		auto legacy_rejected = admit_materialization_request_v2_2_for_execution(
			std::move(legacy), features, std::span<const source_closure_manifest>{manifests});
		assert(!legacy_rejected);
		assert(legacy_rejected.error().code == "materialization.version-unsupported");
	}
} // namespace

int main()
{
	closure_is_required_before_acceptance();
	stale_manifest_and_legacy_task_are_rejected();
}
