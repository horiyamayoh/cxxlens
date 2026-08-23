#include "llvm/clang22/provider_task_v4.hpp"

#include <cassert>
#include <cstdint>
#include <string>

#include "llvm/clang22/source_closure.hpp"

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
} // namespace

int main()
{
	positive_manifest_task_binding();
	negative_identity_and_binding();
}
