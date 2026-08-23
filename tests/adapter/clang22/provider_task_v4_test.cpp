#include "llvm/clang22/provider_task_v4.hpp"

#include <cassert>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "llvm/clang22/source_closure.hpp"
#include "llvm/clang22/source_closure_task_v4.hpp"

namespace
{
	using namespace cxxlens::detail::clang22;

	constexpr char semantic_digit = 'a';

	[[nodiscard]] std::string semantic(const char digit = semantic_digit)
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
		auto file_id = source_closure_file_id(value.members.front().logical_path);
		assert(file_id);
		value.members.front().file_id = *file_id;
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
						manifest.members.front().size_bytes,
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
		value.source_closure = {manifest.closure_id,
								manifest.closure_digest,
								manifest.manifest_digest,
								static_cast<std::uint64_t>(manifest.members.size()),
								static_cast<std::uint64_t>(manifest.blobs.size()),
								manifest.blobs.front().size_bytes};
		value.main_logical_path = base.source.logical_path;
		value.logical_working_directory = base.working_directory;
		auto digest = derive_provider_task_v4_digest(value);
		assert(digest);
		value.task_v4_digest = *digest;
		value.task_id = "task:" + *digest;
		return value;
	}

	void positive_manifest_task_binding()
	{
		auto manifest = make_manifest();
		assert(manifest.validate());
		auto base = make_base(manifest);
		assert(base.validate());
		auto task = make_task(manifest, base);
		assert(task.validate());
		assert(validate_provider_task_v4_identity(task));
		assert(bind_provider_task_v4_main_member(base, task, manifest));

		auto changed_chunking = manifest;
		// Manifest identity is independent of transport chunking; metadata remains identical.
		auto changed_digest = derive_source_closure_manifest_digest(changed_chunking);
		assert(changed_digest && *changed_digest == manifest.manifest_digest);
	}

	void negative_identity_and_binding()
	{
		auto manifest = make_manifest();
		auto base = make_base(manifest);
		auto task = make_task(manifest, base);

		auto stale_task = task;
		stale_task.main_logical_path = "project://src/other.cpp";
		assert(!validate_provider_task_v4_identity(stale_task));

		auto stale_summary = task.source_closure;
		stale_summary.manifest_digest = semantic('9');
		assert(!bind_source_closure_summary(stale_summary, manifest));

		auto stale_main = manifest;
		stale_main.members.front().size_bytes = 8U;
		assert(!bind_provider_task_v4_main_member(base, task, stale_main));

		auto too_many = manifest;
		too_many.members.resize(4097U);
		assert(!too_many.validate());
	}

	void closure_and_provider_codecs_share_identity()
	{
		auto snapshot = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 std::make_shared<const std::string>("int main() { return 0; }\n")},
		});
		assert(snapshot);

		const auto base_projection_text =
			std::string{"{\"base\":\"authority\",\"schema\":\"v2.2\"}"};
		const auto base_projection_bytes =
			std::as_bytes(std::span{base_projection_text.data(), base_projection_text.size()});
		const auto base_digest = cxxlens::sdk::content_digest(base_projection_bytes);

		provider_task_v4_base_task base;
		base.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'd');
		base.provider_execution_id = "provider-execution:identity";
		base.canonical_base_task_digest = base_digest;
		base.task_input_digest = content('e');
		base.normalized_invocation_digest = semantic('f');
		base.toolchain_digest = semantic('1');
		base.environment_digest = content('2');
		base.working_directory = "project://src";
		const auto& main = snapshot->members.front();
		base.source = {"source-snapshot:identity",
					   main.file_id,
					   main.logical_path,
					   main.content_digest,
					   main.size_bytes,
					   "utf8",
					   "line-index:sha256:" + std::string(64U, '3'),
					   true};

		provider_task_v4 task;
		task.base_task_index = 0U;
		task.base_provider_task_id = base.provider_task_id;
		task.base_task_digest = base.canonical_base_task_digest;
		task.open_task = {base.task_input_digest,
						  base.normalized_invocation_digest,
						  base.toolchain_digest,
						  base.environment_digest};
		task.source_closure = {snapshot->snapshot_id,
							   snapshot->closure_digest,
							   {},
							   static_cast<std::uint64_t>(snapshot->members.size()),
							   static_cast<std::uint64_t>(snapshot->blobs.size()),
							   snapshot->blobs.front().size_bytes};
		auto manifest_digest = source_closure_manifest_digest(*snapshot);
		assert(manifest_digest);
		task.source_closure.manifest_digest = *manifest_digest;
		task.main_logical_path = base.source.logical_path;
		task.logical_working_directory = base.working_directory;
		auto provider_digest = derive_provider_task_v4_digest(task);
		assert(provider_digest);

		source_closure_task_v4_input input;
		input.base_task_index = task.base_task_index;
		input.base_provider_task_id = task.base_provider_task_id;
		input.base_task_projection.assign(base_projection_bytes.begin(),
										  base_projection_bytes.end());
		input.task_input_digest = base.task_input_digest;
		input.normalized_invocation_digest = base.normalized_invocation_digest;
		input.toolchain_digest = base.toolchain_digest;
		input.environment_digest = base.environment_digest;
		input.closure = *snapshot;
		input.main_logical_path = task.main_logical_path;
		input.logical_working_directory = task.logical_working_directory;
		auto closure_identity = derive_source_closure_task_v4_identity(input);
		assert(closure_identity);
		assert(*provider_digest == closure_identity->task_v4_digest);
	}
} // namespace

int main()
{
	positive_manifest_task_binding();
	negative_identity_and_binding();
	closure_and_provider_codecs_share_identity();
}
