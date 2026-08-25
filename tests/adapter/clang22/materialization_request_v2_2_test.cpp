#include "llvm/clang22/materialization_request_v2_2.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstdint>
#include <map>
#include <ranges>
#include <span>
#include <string>
#include <string_view>
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

	[[nodiscard]] json_value json_text_value(const std::string_view value)
	{
		auto encoded = json_value::string(std::string{value});
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value
	json_object_value(std::map<std::string, json_value, utf8_byte_less> fields)
	{
		auto encoded = json_value::object(std::move(fields));
		assert(encoded);
		return *encoded;
	}

	[[nodiscard]] json_value json_array_value(std::vector<json_value> values)
	{
		return json_value::array(std::move(values));
	}

	[[nodiscard]] json_value json_version(const cxxlens::sdk::semantic_version value)
	{
		return json_text_value(std::to_string(value.major) + "." + std::to_string(value.minor) +
							   "." + std::to_string(value.patch));
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
		auto digest = cxxlens::sdk::semantic_digest(std::string{domain}, bytes);
		assert(digest);
		return *digest;
	}

	[[nodiscard]] std::string
	engine_registry_digest(const provider_task_v4_request_authority& request)
	{
		std::vector<std::pair<std::string, std::string>> entries;
		for (const auto& descriptor : request.engine.admitted_descriptors)
			entries.emplace_back(descriptor.descriptor_id, descriptor.runtime_descriptor_digest);
		std::ranges::sort(entries,
						  [](const auto& left, const auto& right)
						  {
							  return left.first < right.first;
						  });
		std::string payload;
		for (const auto& [id, digest] : entries)
			payload += id + "=" + digest + "\n";
		auto digest = cxxlens::sdk::semantic_digest("cxxlens.relation-registry.v1", payload);
		assert(digest);
		return *digest;
	}

	[[nodiscard]] std::string trust_digest(const provider_task_v4_trust_authority& trust)
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

	[[nodiscard]] provider_task_v4_request_authority
	make_authority(const source_closure_manifest& manifest)
	{
		const std::vector<std::string> arguments{
			"/usr/bin/clang++", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
		auto invocation =
			derive_provider_task_v4_effective_invocation_digest("project://src", arguments);
		assert(invocation);
		const auto environment_digest = content('2');
		auto catalog =
			cxxlens::sdk::project_catalog::make("project://root",
												environment_digest,
												{{"catalog-unit:one",
												  *invocation,
												  manifest.members.front().content_digest,
												  environment_digest}});
		assert(catalog);

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
		request.project = {"project:one", *catalog, semantic('6')};
		request.registry.path = std::string{task_v4_registry_path};
		for (std::size_t index{}; index < task_v4_base_descriptor_ids.size(); ++index)
			request.registry.base_descriptors.push_back(
				{std::string{task_v4_base_descriptor_ids[index]},
				 {1U, 0U, 0U},
				 content(static_cast<char>('a' + index % 6U)),
				 semantic(static_cast<char>('a' + (index + 1U) % 6U)),
				 static_cast<std::uint32_t>(index),
				 "canonical_claim",
				 "installed-tool"});
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
		request.engine.engine_registry_digest = engine_registry_digest(request);
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
		request.trust_policy.trust_policy_digest = trust_digest(request.trust_policy);
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

	[[nodiscard]] json_value json_sandbox(const cxxlens::sdk::provider::sandbox_requirement& value)
	{
		return json_object_value({
			{"minimum",
			 json_text_value(value.minimum == cxxlens::sdk::provider::sandbox_assurance::enforced
								 ? "enforced"
								 : "certified")},
			{"policy_digest", json_text_value(value.policy_digest)},
		});
	}

	[[nodiscard]] json_value json_source(const provider_task_v4_source& value)
	{
		return json_object_value({
			{"source_snapshot_id", json_text_value(value.source_snapshot_id)},
			{"file_id", json_text_value(value.file_id)},
			{"logical_path", json_text_value(value.logical_path)},
			{"content_digest", json_text_value(value.content_digest)},
			{"size_bytes", json_value::unsigned_integer(value.size_bytes)},
			{"encoding", json_text_value(value.encoding)},
			{"line_index_id", json_text_value(value.line_index_id)},
			{"read_only", json_value::boolean(value.read_only)},
		});
	}

	[[nodiscard]] json_value json_strings(const std::vector<std::string>& values)
	{
		std::vector<json_value> encoded;
		encoded.reserve(values.size());
		for (const auto& value : values)
			encoded.push_back(json_text_value(value));
		return json_array_value(std::move(encoded));
	}

	[[nodiscard]] json_value json_authority(const provider_task_v4_request_authority& request,
											const std::string_view request_id,
											const std::string_view request_digest,
											const bool include_source_bytes = false)
	{
		std::vector<json_value> units;
		for (const auto& unit : request.project.catalog.compile_units)
			units.push_back(json_object_value({
				{"catalog_compile_unit_id", json_text_value(unit.compile_unit_id)},
				{"effective_invocation_digest", json_text_value(unit.effective_invocation_digest)},
				{"source_digest", json_text_value(unit.source_digest)},
				{"environment_digest", json_text_value(unit.environment_digest)},
			}));
		std::vector<json_value> base_descriptors;
		for (const auto& descriptor : request.registry.base_descriptors)
			base_descriptors.push_back(json_object_value({
				{"descriptor_id", json_text_value(descriptor.descriptor_id)},
				{"descriptor_version", json_version(descriptor.descriptor_version)},
				{"contract_digest", json_text_value(descriptor.contract_digest)},
				{"runtime_descriptor_digest",
				 json_text_value(descriptor.runtime_descriptor_digest)},
				{"stage_order", json_value::unsigned_integer(descriptor.stage_order)},
				{"output_stage", json_text_value(descriptor.output_stage)},
				{"owner", json_text_value(descriptor.owner)},
			}));
		std::vector<json_value> descriptors;
		for (const auto& descriptor : request.registry.descriptors)
			descriptors.push_back(json_object_value({
				{"descriptor_id", json_text_value(descriptor.descriptor_id)},
				{"descriptor_version", json_version(descriptor.descriptor_version)},
				{"contract_digest", json_text_value(descriptor.contract_digest)},
				{"runtime_descriptor_digest",
				 json_text_value(descriptor.runtime_descriptor_digest)},
				{"dependency_group_id", json_text_value(descriptor.dependency_group_id)},
				{"atomic_output_group_id", json_text_value(descriptor.atomic_output_group_id)},
				{"batch_id", json_text_value(descriptor.batch_id)},
				{"output_stage", json_text_value(descriptor.output_stage)},
			}));
		std::vector<json_value> admitted;
		for (const auto& descriptor : request.engine.admitted_descriptors)
			admitted.push_back(json_object_value({
				{"descriptor_id", json_text_value(descriptor.descriptor_id)},
				{"runtime_descriptor_digest",
				 json_text_value(descriptor.runtime_descriptor_digest)},
			}));
		std::vector<json_value> sandbox_requirements;
		for (const auto& requirement : request.trust_policy.task_sandbox_requirements)
			sandbox_requirements.push_back(json_sandbox(requirement));
		std::vector<json_value> tasks;
		for (const auto& task : request.tasks)
		{
			auto source = json_source(task.source);
			if (include_source_bytes)
			{
				std::map<std::string, json_value, utf8_byte_less> source_fields{
					{"content_base64", json_text_value("forbidden")}};
				source = json_object_value(std::move(source_fields));
			}
			std::vector<json_value> budget{
				json_value::unsigned_integer(task.budget.output_bytes),
				json_value::unsigned_integer(task.budget.rows),
				json_value::unsigned_integer(task.budget.diagnostics),
				json_value::unsigned_integer(task.budget.wall_ms),
				json_value::unsigned_integer(task.budget.cpu_ms),
				json_value::unsigned_integer(task.budget.address_space_bytes),
				json_value::unsigned_integer(task.budget.transport_bytes),
				json_value::unsigned_integer(task.budget.open_files),
				json_value::unsigned_integer(task.budget.subprocesses)};
			tasks.push_back(json_object_value({
				{"provider_task_id", json_text_value(task.provider_task_id)},
				{"provider_execution_id", json_text_value(task.provider_execution_id)},
				{"task_input_digest", json_text_value(task.task_input_digest)},
				{"project_id", json_text_value(task.project_id)},
				{"catalog_id", json_text_value(task.catalog_id)},
				{"catalog_digest", json_text_value(task.catalog_digest)},
				{"selected_catalog_compile_unit_id",
				 json_text_value(task.selected_catalog_compile_unit_id)},
				{"compile_unit_id", json_text_value(task.compile_unit_id)},
				{"build_variant_id", json_text_value(task.build_variant_id)},
				{"toolchain_context_id", json_text_value(task.toolchain_context_id)},
				{"toolchain_digest", json_text_value(task.toolchain_digest)},
				{"toolchain",
				 json_object_value({
					 {"family", json_text_value(task.toolchain.family)},
					 {"exact_version", json_text_value(task.toolchain.exact_version)},
					 {"target_triple", json_text_value(task.toolchain.target_triple)},
					 {"builtin_headers_digest",
					  json_text_value(task.toolchain.builtin_headers_digest)},
					 {"sysroot",
					  task.toolchain.sysroot ? json_text_value(*task.toolchain.sysroot)
											 : json_value::null()},
					 {"abi_digest", json_text_value(task.toolchain.abi_digest)},
					 {"plugin_spec_digest", json_text_value(task.toolchain.plugin_spec_digest)},
				 })},
				{"variant",
				 json_object_value({
					 {"language", json_text_value(task.variant.language)},
					 {"language_standard", json_text_value(task.variant.language_standard)},
					 {"target_triple", json_text_value(task.variant.target_triple)},
					 {"predefined_macros_digest",
					  json_text_value(task.variant.predefined_macros_digest)},
					 {"include_search_digest", json_text_value(task.variant.include_search_digest)},
					 {"semantic_flags_digest", json_text_value(task.variant.semantic_flags_digest)},
				 })},
				{"normalized_invocation_digest",
				 json_text_value(task.normalized_invocation_digest)},
				{"environment_digest", json_text_value(task.environment_digest)},
				{"language", json_text_value(task.language)},
				{"working_directory", json_text_value(task.working_directory)},
				{"condition_universe_id", json_text_value(task.condition_universe_id)},
				{"condition_id", json_text_value(task.condition_id)},
				{"interpretation_domain", json_text_value(task.interpretation_domain)},
				{"source", std::move(source)},
				{"effective_argv", json_strings(task.input_authority.effective_arguments)},
				{"qualified_read_roots", json_strings(task.input_authority.qualified_read_roots)},
				{"requested_descriptor_ids", json_strings(task.requested_descriptor_ids)},
				{"dependency_groups", json_strings(task.dependency_groups)},
				{"budget",
				 json_object_value({
					 {"output_bytes", budget[0]},
					 {"rows", budget[1]},
					 {"diagnostics", budget[2]},
					 {"wall_ms", budget[3]},
					 {"cpu_ms", budget[4]},
					 {"address_space_bytes", budget[5]},
					 {"transport_bytes", budget[6]},
					 {"open_files", budget[7]},
					 {"subprocesses", budget[8]},
				 })},
				{"sandbox", json_sandbox(task.sandbox)},
			}));
		}
		const auto selector = json_object_value({
			{"catalog_id", json_text_value(request.publication.selector.catalog_id)},
			{"channel_id", json_text_value(request.publication.selector.channel_id)},
			{"engine_generation_id",
			 json_text_value(request.publication.selector.engine_generation_id)},
			{"condition_universe_id",
			 json_text_value(request.publication.selector.condition_universe_id)},
			{"relation_registry_digest",
			 json_text_value(request.publication.selector.relation_registry_digest)},
			{"interpretation_policy_digest",
			 json_text_value(request.publication.selector.interpretation_policy_digest)},
			{"trust_policy_digest",
			 json_text_value(request.publication.selector.trust_policy_digest)},
		});
		return json_object_value({
			{"materialization_request_id", json_text_value(request_id)},
			{"semantic_request_digest", json_text_value(request_digest)},
			{"tool",
			 json_object_value({
				 {"executable", json_text_value(request.tool.executable)},
				 {"interface_version", json_text_value(request.tool.interface_version)},
				 {"distribution_version", json_text_value(request.tool.distribution_version)},
				 {"source_revision", json_text_value(request.tool.source_revision)},
				 {"source_tree", json_text_value(request.tool.source_tree)},
				 {"installed_executable_digest",
				  json_text_value(request.tool.installed_executable_digest)},
				 {"package_configuration", json_text_value(request.tool.package_configuration)},
				 {"occurrence_manifest_digest",
				  json_text_value(request.tool.occurrence_manifest_digest)},
			 })},
			{"worker",
			 json_object_value({
				 {"executable", json_text_value(request.worker.executable)},
				 {"provider_id", json_text_value(request.worker.provider_id)},
				 {"provider_version", json_version(request.worker.provider_version)},
				 {"installed_binary_digest",
				  json_text_value(request.worker.installed_binary_digest)},
				 {"semantic_contract_digest",
				  json_text_value(request.worker.semantic_contract_digest)},
				 {"protocol_major", json_value::unsigned_integer(request.worker.protocol_major)},
				 {"protocol_minor", json_value::unsigned_integer(request.worker.protocol_minor)},
				 {"required_features", json_strings(request.worker.required_features)},
				 {"sandbox_policy_digest", json_text_value(request.worker.sandbox_policy_digest)},
			 })},
			{"project",
			 json_object_value({
				 {"project_id", json_text_value(request.project.project_id)},
				 {"catalog_id", json_text_value(request.project.catalog.catalog_id)},
				 {"catalog_digest", json_text_value(request.project.catalog.catalog_digest)},
				 {"logical_root", json_text_value(request.project.catalog.logical_root)},
				 {"catalog_environment_digest",
				  json_text_value(request.project.catalog.environment_digest)},
				 {"catalog_compile_unit_census_digest",
				  json_text_value(request.project.catalog_compile_unit_census_digest)},
				 {"catalog_compile_units", json_array_value(std::move(units))},
			 })},
			{"registry",
			 json_object_value({
				 {"path", json_text_value(request.registry.path)},
				 {"authority_registry_digest",
				  json_text_value(request.registry.authority_registry_digest)},
				 {"base_descriptors", json_array_value(std::move(base_descriptors))},
				 {"descriptors", json_array_value(std::move(descriptors))},
			 })},
			{"engine",
			 json_object_value({
				 {"generation_contract", json_text_value(request.engine.generation_contract)},
				 {"admitted_descriptors", json_array_value(std::move(admitted))},
				 {"engine_registry_digest", json_text_value(request.engine.engine_registry_digest)},
				 {"engine_generation_id", json_text_value(request.engine.engine_generation_id)},
			 })},
			{"interpretation_policy",
			 json_object_value({
				 {"policy_id", json_text_value(request.interpretation_policy.policy_id)},
				 {"selected_domain",
				  json_text_value(request.interpretation_policy.selected_domain)},
				 {"interpretation_policy_digest",
				  json_text_value(request.interpretation_policy.interpretation_policy_digest)},
			 })},
			{"trust_policy",
			 json_object_value({
				 {"policy_id", json_text_value(request.trust_policy.policy_id)},
				 {"execution_profile", json_text_value(request.trust_policy.execution_profile)},
				 {"provider_id", json_text_value(request.trust_policy.provider_id)},
				 {"provider_version", json_version(request.trust_policy.provider_version)},
				 {"semantic_contract_digest",
				  json_text_value(request.trust_policy.semantic_contract_digest)},
				 {"protocol_major",
				  json_value::unsigned_integer(request.trust_policy.protocol_major)},
				 {"protocol_minor",
				  json_value::unsigned_integer(request.trust_policy.protocol_minor)},
				 {"required_features", json_strings(request.trust_policy.required_features)},
				 {"required_qualification",
				  json_text_value(request.trust_policy.required_qualification)},
				 {"worker_sandbox_policy_digest",
				  json_text_value(request.trust_policy.worker_sandbox_policy_digest)},
				 {"task_sandbox_requirements", json_array_value(std::move(sandbox_requirements))},
				 {"trust_policy_digest", json_text_value(request.trust_policy.trust_policy_digest)},
			 })},
			{"group_topology",
			 json_object_value({
				 {"dependency_groups", json_strings(request.group_topology.dependency_groups)},
				 {"atomic_output_group",
				  json_text_value(request.group_topology.atomic_output_group)},
				 {"partial_policy", json_text_value(request.group_topology.partial_policy)},
			 })},
			{"tasks", json_array_value(std::move(tasks))},
			{"publication",
			 json_object_value({
				 {"backend", json_text_value(request.publication.backend)},
				 {"selector", selector},
				 {"series_id", json_text_value(request.publication.series_id)},
				 {"genesis", json_value::boolean(request.publication.genesis)},
				 {"expected_parent_publication",
				  request.publication.expected_parent_publication
					  ? json_text_value(*request.publication.expected_parent_publication)
					  : json_value::null()},
				 {"sqlite_path",
				  request.publication.sqlite_path
					  ? json_text_value(*request.publication.sqlite_path)
					  : json_value::null()},
				 {"partial_policy", json_text_value(request.publication.partial_policy)},
				 {"transaction_count",
				  json_value::unsigned_integer(request.publication.transaction_count)},
				 {"reopen_before_success",
				  json_value::boolean(request.publication.reopen_before_success)},
				 {"recipe_id", json_text_value(request.publication.recipe_id)},
				 {"recipe_digest", json_text_value(request.publication.recipe_digest)},
				 {"output_plan_digest", json_text_value(request.publication.output_plan_digest)},
				 {"publication_target", json_text_value(request.publication.publication_target)},
			 })},
		});
	}

	[[nodiscard]] json_value inherited_authority(const std::string& request_id,
												 const std::string& semantic_request_digest,
												 const bool include_source_bytes = false)
	{
		auto manifest = make_manifest();
		auto authority = make_authority(manifest);
		return json_authority(authority, request_id, semantic_request_digest, include_source_bytes);
	}

	[[nodiscard]] materialization_request_v2_2 make_request(const source_closure_manifest& manifest,
															const provider_task_v4_base_task& base,
															const provider_task_v4& task)
	{
		static_cast<void>(manifest);
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

	[[nodiscard]] json_value document_shape(const bool source_bytes,
											const bool future_minor,
											const bool complete_authority = false)
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
		if (complete_authority)
		{
			auto authority = json_authority(make_authority(make_manifest()),
											"materialization-authority:v2_2-test",
											semantic('a'));
			assert(authority.as_object() != nullptr);
			for (const auto& [name, value] : *authority.as_object())
				root.insert_or_assign(name, value);
		}
		return object(std::move(root));
	}

	void document_ingress_is_closed_before_transport()
	{
		assert(validate_materialization_request_v2_2_document(document_shape(false, false, true)));
		// A structurally shaped envelope with empty authority objects is rejected at ingress;
		// the typed decoder is the authority gate, not an optional post-parse check.
		auto incomplete =
			validate_materialization_request_v2_2_document(document_shape(false, false));
		assert(!incomplete && incomplete.error().code == "materialization.request-v2_2-invalid");
		auto bytes = validate_materialization_request_v2_2_document(document_shape(true, false));
		assert(!bytes && bytes.error().code == "materialization.request-v2_2-invalid");
		auto future = validate_materialization_request_v2_2_document(document_shape(false, true));
		assert(!future && future.error().code == "materialization.version-unsupported");
	}

	void typed_authority_decoder_is_source_free_and_deterministic()
	{
		auto manifest = make_manifest();
		auto authority = make_authority(manifest);
		const auto request_id = std::string{"materialization-authority:v2_2-test"};
		const auto request_digest = semantic('8');
		auto root = json_authority(authority, request_id, request_digest);
		auto first = decode_provider_task_v4_request_authority(root);
		assert(first);
		assert(first->tasks.size() == 1U);
		assert(first->tasks.front().input_authority.effective_arguments.front() ==
			   "/usr/bin/clang++");
		assert(first->tasks.front().input_authority.qualified_read_roots ==
			   std::vector<std::string>{"/usr"});
		assert(first->publication.recipe_id == "recipe:clang22");
		assert(first->publication.publication_target == "publication:memory");
		auto second = decode_provider_task_v4_request_authority(root);
		assert(second);
		auto first_digest = first->authority_digest();
		auto second_digest = second->authority_digest();
		assert(first_digest && second_digest && *first_digest == *second_digest);

		auto bytes = json_authority(authority, request_id, request_digest, true);
		auto bytes_result = decode_provider_task_v4_request_authority(bytes);
		assert(!bytes_result &&
			   bytes_result.error().code == "materialization.request-v2_2-invalid");

		authority.tasks.front().input_authority.qualified_read_roots.clear();
		auto missing_root = json_authority(authority, request_id, request_digest);
		auto missing_root_result = decode_provider_task_v4_request_authority(missing_root);
		assert(!missing_root_result);

		authority = make_authority(manifest);
		authority.publication.selector.relation_registry_digest =
			authority.registry.authority_registry_digest;
		authority.publication.series_id = authority.publication.selector.id();
		auto wrong_registry = json_authority(authority, request_id, request_digest);
		auto wrong_registry_result = decode_provider_task_v4_request_authority(wrong_registry);
		assert(!wrong_registry_result);
	}
} // namespace

#ifdef CXXLENS_MATERIALIZATION_REQUEST_V2_2_FIXTURE_ONLY
cxxlens::detail::clang22::materialization::json_value
cxxlens_test_materialization_request_v2_2_complete_document()
{
	return document_shape(false, false, true);
}
#else
int main()
{
	positive_and_manifest_cross_binding();
	version_feature_and_payload_rejection();
	identity_binding_and_bounds_rejection();
	document_ingress_is_closed_before_transport();
	typed_authority_decoder_is_source_free_and_deterministic();
}
#endif
