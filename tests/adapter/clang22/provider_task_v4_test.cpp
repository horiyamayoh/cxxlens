#include "llvm/clang22/provider_task_v4.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <utility>
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

	void authenticated_main_line_index_binding()
	{
		auto snapshot = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 std::make_shared<const std::string>("int main() { return 0; }\n")},
		});
		assert(snapshot);
		assert(
			snapshot->closure_digest ==
			"semantic-v2:sha256:ea94f38e8a9bdf7250f07769c3b902378c78c3d98dd36858904b3172f94962c9");
		const auto& main = snapshot->members.front();

		source_closure_manifest manifest;
		manifest.members = {{main.file_id,
							 main.logical_path,
							 "main",
							 "utf8",
							 main.size_bytes,
							 main.content_digest,
							 true}};
		manifest.blobs = {{main.content_digest, main.size_bytes}};
		auto closure_digest = derive_source_closure_digest(manifest);
		assert(closure_digest && *closure_digest == snapshot->closure_digest);
		manifest.closure_digest = *closure_digest;
		manifest.closure_id = "source-closure:" + *closure_digest;
		auto manifest_digest = derive_source_closure_manifest_digest(manifest);
		assert(manifest_digest);
		manifest.manifest_digest = *manifest_digest;
		assert(manifest.validate());

		provider_task_v4_base_task base;
		base.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, 'c');
		base.provider_execution_id = "provider-execution:line-index";
		base.canonical_base_task_digest = content('d');
		base.task_input_digest = content('e');
		base.normalized_invocation_digest = semantic('f');
		base.toolchain_digest = semantic('1');
		base.environment_digest = content('2');
		base.working_directory = "project://src";
		auto line_index = source_closure_main_line_index_id(*snapshot);
		assert(line_index);
		base.source = {snapshot->snapshot_id,
					   main.file_id,
					   main.logical_path,
					   main.content_digest,
					   main.size_bytes,
					   "utf8",
					   *line_index,
					   true};
		auto task = make_task(manifest, base);
		assert(bind_provider_task_v4_main_member(base, task, manifest, *snapshot));

		auto stale = base;
		stale.source.line_index_id = "line-index:sha256:" + std::string(64U, '0');
		assert(!bind_provider_task_v4_main_member(stale, task, manifest, *snapshot));
	}

	[[nodiscard]] std::string bytes_digest(const std::string_view domain,
										   const cxxlens::sdk::canonical_value& value)
	{
		auto encoded = cxxlens::sdk::canonical_binary(value);
		assert(encoded);
		std::string bytes;
		bytes.reserve(encoded->size());
		for (const auto byte : *encoded)
			bytes.push_back(static_cast<char>(std::to_integer<unsigned char>(byte)));
		auto digest = cxxlens::sdk::semantic_digest(domain, bytes);
		assert(digest);
		return *digest;
	}

	[[nodiscard]] std::string trust_digest_fixture(const provider_task_v4_trust_authority& trust)
	{
		const auto sandbox_name = [](const auto assurance)
		{
			return assurance == cxxlens::sdk::provider::sandbox_assurance::enforced
				? std::string{"enforced"}
				: std::string{"certified"};
		};
		std::vector<cxxlens::sdk::canonical_value> requirements;
		for (const auto& requirement : trust.task_sandbox_requirements)
			requirements.push_back(cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(sandbox_name(requirement.minimum)),
				cxxlens::sdk::canonical_value::from_string(requirement.policy_digest),
			}));
		return bytes_digest(
			std::string{task_v4_trust_policy_id},
			cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(trust.policy_id),
				cxxlens::sdk::canonical_value::from_string(trust.execution_profile),
				cxxlens::sdk::canonical_value::from_string(trust.provider_id),
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_integer(trust.provider_version.major),
					cxxlens::sdk::canonical_value::from_integer(trust.provider_version.minor),
					cxxlens::sdk::canonical_value::from_integer(trust.provider_version.patch),
				}),
				cxxlens::sdk::canonical_value::from_string(trust.semantic_contract_digest),
				cxxlens::sdk::canonical_value::from_integer(trust.protocol_major),
				cxxlens::sdk::canonical_value::from_integer(trust.protocol_minor),
				cxxlens::sdk::canonical_value::from_tuple({
					cxxlens::sdk::canonical_value::from_string(trust.required_features[0]),
					cxxlens::sdk::canonical_value::from_string(trust.required_features[1]),
				}),
				cxxlens::sdk::canonical_value::from_string(trust.required_qualification),
				cxxlens::sdk::canonical_value::from_string(trust.worker_sandbox_policy_digest),
				cxxlens::sdk::canonical_value::from_tuple(std::move(requirements)),
			}));
	}

	[[nodiscard]] provider_task_v4_request_authority make_request_authority()
	{
		const auto manifest = make_manifest();
		const std::vector<std::string> arguments{
			"/usr/bin/clang++", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
		const auto invocation =
			derive_provider_task_v4_effective_invocation_digest("project://src", arguments);
		assert(invocation);
		const auto environment_digest = content('2');
		const auto catalog_result =
			cxxlens::sdk::project_catalog::make("project://root",
												environment_digest,
												{{"catalog-unit:one",
												  *invocation,
												  manifest.members.front().content_digest,
												  environment_digest}});
		assert(catalog_result);

		provider_task_v4_request_authority request;
		request.tool = {"/opt/cxxlens/bin/materializer",
						"tool-interface-v2.2",
						"distribution-2.0",
						std::string(40U, 'a'),
						std::string(40U, 'b'),
						content('c'),
						"package-config:clang22",
						semantic('d')};
		const auto sandbox_policy = cxxlens::sdk::provider::builtin_sandbox_policies().front();
		request.worker = {"/opt/cxxlens/bin/clang22-worker",
						  "provider:clang22",
						  {1U, 0U, 0U},
						  content('e'),
						  semantic('f'),
						  cxxlens::sdk::provider::protocol_v2_major,
						  cxxlens::sdk::provider::protocol_v2_minor,
						  {"task-input-chunks-v2", "task-source-closure-v2"},
						  sandbox_policy.policy_digest()};
		request.project = {"project:one", *catalog_result, semantic('6')};
		request.registry.path = std::string{task_v4_registry_path};
		request.registry.base_descriptors.reserve(task_v4_base_descriptor_ids.size());
		for (std::size_t index{}; index < task_v4_base_descriptor_ids.size(); ++index)
			request.registry.base_descriptors.push_back(
				{std::string{task_v4_base_descriptor_ids[index]},
				 {1U, 0U, 0U},
				 content(static_cast<char>('a' + index % 6U)),
				 semantic(static_cast<char>('a' + (index + 1U) % 6U)),
				 static_cast<std::uint32_t>(index),
				 "canonical_claim",
				 "installed-tool"});
		request.registry.descriptors.reserve(task_v4_output_descriptor_ids.size());
		for (std::size_t index{}; index < task_v4_output_descriptor_ids.size(); ++index)
		{
			const auto id = std::string{task_v4_output_descriptor_ids[index]};
			request.registry.descriptors.push_back(
				{id,
				 id.starts_with("frontend.") ? cxxlens::sdk::semantic_version{2U, 0U, 0U}
											 : cxxlens::sdk::semantic_version{1U, 0U, 0U},
				 content(static_cast<char>('a' + (index + 2U) % 6U)),
				 semantic(static_cast<char>('a' + (index + 3U) % 6U)),
				 id.starts_with("cc.") ? "canonical" : "observation",
				 "clang22-atomic",
				 id + "-batch",
				 id.starts_with("cc.") ? "canonical_claim" : "assertion"});
		}
		// The registry document authority is a content identity.  The admitted engine
		// inventory has its own semantic registry identity and must not alias it.
		request.registry.authority_registry_digest = content('9');

		request.engine.generation_contract = std::string{task_v4_engine_generation_contract};
		request.engine.engine_generation_id = "engine-generation:sha256:" + std::string(64U, 'c');
		for (const auto id : task_v4_engine_descriptor_ids)
		{
			const auto base = std::ranges::find_if(request.registry.base_descriptors,
												   [&](const auto& descriptor)
												   {
													   return descriptor.descriptor_id == id;
												   });
			if (base != request.registry.base_descriptors.end())
			{
				request.engine.admitted_descriptors.push_back(
					{base->descriptor_id, base->runtime_descriptor_digest});
				continue;
			}
			const auto output = std::ranges::find_if(request.registry.descriptors,
													 [&](const auto& descriptor)
													 {
														 return descriptor.descriptor_id == id;
													 });
			assert(output != request.registry.descriptors.end());
			request.engine.admitted_descriptors.push_back(
				{output->descriptor_id, output->runtime_descriptor_digest});
		}
		// The engine digest is derived from the admitted descriptor inventory, not
		// from the full registry document authority.
		{
			std::vector<std::pair<std::string, std::string>> entries;
			for (const auto& descriptor : request.engine.admitted_descriptors)
				entries.emplace_back(descriptor.descriptor_id,
									 descriptor.runtime_descriptor_digest);
			std::ranges::sort(entries,
							  [](const auto& left, const auto& right)
							  {
								  return left.first < right.first;
							  });
			std::string payload;
			for (const auto& [id, digest] : entries)
				payload += id + "=" + digest + "\n";
			auto engine_digest =
				cxxlens::sdk::semantic_digest("cxxlens.relation-registry.v1", payload);
			assert(engine_digest);
			request.engine.engine_registry_digest = *engine_digest;
		}

		request.interpretation_policy = {std::string{task_v4_interpretation_policy_id},
										 std::string{task_v4_interpretation_domain},
										 {}};
		request.interpretation_policy.interpretation_policy_digest = bytes_digest(
			std::string{task_v4_interpretation_policy_id},
			cxxlens::sdk::canonical_value::from_tuple({
				cxxlens::sdk::canonical_value::from_string(request.interpretation_policy.policy_id),
				cxxlens::sdk::canonical_value::from_string(
					request.interpretation_policy.selected_domain),
			}));
		request.trust_policy = {std::string{task_v4_trust_policy_id},
								"sandboxed-provider-v2",
								request.worker.provider_id,
								request.worker.provider_version,
								request.worker.semantic_contract_digest,
								request.worker.protocol_major,
								request.worker.protocol_minor,
								request.worker.required_features,
								"canonical-semantic-qualified",
								request.worker.sandbox_policy_digest,
								{{cxxlens::sdk::provider::sandbox_assurance::enforced,
								  request.worker.sandbox_policy_digest}},
								{}};
		request.trust_policy.trust_policy_digest = trust_digest_fixture(request.trust_policy);
		request.group_topology = {{"canonical", "observation"}, "clang22-atomic", "forbid"};

		provider_task_v4_task_authority task;
		task.provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		task.provider_execution_id = "provider-execution:one";
		task.task_input_digest = content('e');
		task.project_id = request.project.project_id;
		task.catalog_id = request.project.catalog.catalog_id;
		task.catalog_digest = request.project.catalog.catalog_digest;
		task.selected_catalog_compile_unit_id = "catalog-unit:one";
		task.compile_unit_id = "build-compile-unit:one";
		task.build_variant_id = "build-variant:one";
		task.toolchain_context_id = "toolchain-context:one";
		task.toolchain_digest = semantic('1');
		task.toolchain = {"clang",
						  "22.1.0",
						  "x86_64-pc-linux-gnu",
						  content('a'),
						  std::string{"/usr"},
						  content('b'),
						  content('c')};
		task.variant = {
			"c++", "c++23", "x86_64-pc-linux-gnu", content('d'), content('e'), content('f')};
		task.normalized_invocation_digest = *invocation;
		task.environment_digest = environment_digest;
		task.language = "c++";
		task.working_directory = "project://src";
		task.condition_universe_id = "condition-universe:one";
		task.condition_id = "condition:one";
		task.interpretation_domain = request.interpretation_policy.selected_domain;
		task.source = {"source-snapshot:one",
					   manifest.members.front().file_id,
					   manifest.members.front().logical_path,
					   manifest.members.front().content_digest,
					   manifest.members.front().size_bytes,
					   manifest.members.front().encoding,
					   "line-index:sha256:" + std::string(64U, '3'),
					   true};
		task.input_authority = {*invocation, "project://src", arguments, {"/usr"}};
		task.requested_descriptor_ids.assign(task_v4_output_descriptor_ids.begin(),
											 task_v4_output_descriptor_ids.end());
		task.dependency_groups = request.group_topology.dependency_groups;
		task.budget = {};
		task.sandbox = {cxxlens::sdk::provider::sandbox_assurance::enforced,
						request.worker.sandbox_policy_digest};
		request.tasks.push_back(std::move(task));

		request.publication.backend = "memory";
		request.publication.selector = {request.project.catalog.catalog_id,
										"channel:clang22",
										request.engine.engine_generation_id,
										request.tasks.front().condition_universe_id,
										request.engine.engine_registry_digest,
										request.interpretation_policy.interpretation_policy_digest,
										request.trust_policy.trust_policy_digest};
		request.publication.series_id = request.publication.selector.id();
		request.publication.genesis = true;
		request.publication.partial_policy = "forbid";
		request.publication.transaction_count = 1U;
		request.publication.reopen_before_success = true;
		request.publication.recipe_id = "recipe:clang22";
		request.publication.recipe_digest = semantic('1');
		request.publication.output_plan_digest = semantic('2');
		request.publication.publication_target = "publication:memory";
		return request;
	}

	void positive_typed_authority()
	{
		auto request = make_request_authority();
		assert(request.validate());
		auto first_digest = request.authority_digest();
		assert(first_digest);
		auto second_digest = request.authority_digest();
		assert(second_digest && *first_digest == *second_digest);
	}

	void negative_typed_authority()
	{
		auto request = make_request_authority();
		request.tasks.front().input_authority.effective_arguments.back() =
			"project://src/other.cpp";
		assert(!request.validate());

		request = make_request_authority();
		request.publication.series_id = "stale-series";
		assert(!request.validate());

		request = make_request_authority();
		request.engine.admitted_descriptors.front().runtime_descriptor_digest = semantic('a');
		assert(!request.validate());
	}
} // namespace

int main()
{
	positive_manifest_task_binding();
	negative_identity_and_binding();
	closure_and_provider_codecs_share_identity();
	authenticated_main_line_index_binding();
	positive_typed_authority();
	negative_typed_authority();
}
