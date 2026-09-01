#include "sdk/materialization_task_internal.hpp"

#include <cassert>
#include <string>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;

	[[nodiscard]] std::string content(const char digit)
	{
		return "sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] std::string semantic(const char digit)
	{
		return "semantic-v2:sha256:" + std::string(64U, digit);
	}

	[[nodiscard]] relation_descriptor descriptor()
	{
		relation_descriptor output;
		output.id = "test.entity.v1";
		output.name = "test.entity";
		output.version = {1U, 0U, 0U};
		output.semantic_major = 1U;
		output.semantics = "test.entity/1";
		output.owner_namespace = "cxxlens.test";
		output.columns = {{"test.entity.v1.entity",
						   "entity",
						   {scalar_kind::typed_id, "test_entity_id", false},
						   true,
						   column_role::claim_key}};
		output.key_columns = {"test.entity.v1.entity"};
		output.descriptor_digest =
			*semantic_digest("cxxlens.relation-descriptor-binding.v2",
							 output.contract_digest + "\n" + output.canonical_form());
		return output;
	}

	[[nodiscard]] relation_engine engine_for(const relation_descriptor& relation)
	{
		relation_registry registry;
		assert(registry.add(relation));
		auto engine = registry.build("generation:test");
		assert(engine);
		return std::move(*engine);
	}

	[[nodiscard]] project_catalog catalog_for(const std::string& invocation,
											  const std::string& source,
											  const std::string& environment)
	{
		auto value = project_catalog::make(
			"project://root", environment, {{"catalog-unit:one", invocation, source, environment}});
		assert(value);
		return std::move(*value);
	}

	[[nodiscard]] validated_build_capture capture_for()
	{
		build_capture_draft value;
		value.project_id = "project:one";
		value.selected_catalog_compile_unit_id = "catalog-unit:one";
		value.compile_unit_id = "compile-unit:one";
		value.build_variant_id = "build-variant:one";
		value.toolchain_context_id = "toolchain-context:one";
		value.toolchain_digest = semantic('1');
		value.toolchain = {
			"clang",
			"22.1.0",
			"x86_64-pc-linux-gnu",
			content('2'),
			std::string{"/opt/sysroot"},
			content('3'),
			content('4'),
			captured_value<std::string>::unavailable("not-captured", "capture-path"),
			captured_value<std::string>::unavailable("not-captured", "capture-binary")};
		value.variant = {
			"c++", "c++23", "x86_64-pc-linux-gnu", content('5'), content('6'), content('7')};
		value.invocation.original_arguments =
			captured_value<std::vector<std::string>>::unavailable("not-captured", "recapture-argv");
		value.invocation.normalized_semantic_options =
			captured_value<std::vector<normalized_build_option>>::unavailable("not-captured",
																			  "recapture-options");
		value.invocation.effective_replay_arguments =
			captured_value<std::vector<std::string>>::observed(
				{"/opt/bin/clang++", "-std=c++23", "project://src/main.cpp"});
		value.invocation.response_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"not-captured", "recapture-response-files");
		value.invocation.config_files =
			captured_value<std::vector<build_capture_auxiliary_file>>::unavailable(
				"not-captured", "recapture-config-files");
		value.invocation.environment_effects =
			captured_value<std::vector<build_capture_environment_effect>>::unavailable(
				"not-captured", "recapture-environment");
		value.invocation.effective_invocation_digest = semantic('8');
		value.invocation.environment_digest = content('9');
		value.invocation.language = "c++";
		value.invocation.logical_working_directory = "project://src";
		value.invocation.qualified_read_roots = {"/opt"};
		value.source = {"source-snapshot:one",
						"file:one",
						"project://src/main.cpp",
						content('a'),
						7U,
						"utf8",
						"line-index:one",
						true};
		value.source_closure = {"source-closure:one", semantic('b'), semantic('c'), 1U, 1U, 7U};
		value.catalog = catalog_for(value.invocation.effective_invocation_digest,
									value.source.content_digest,
									value.invocation.environment_digest);
		auto validated = validate_build_capture(std::move(value));
		assert(validated);
		return std::move(*validated);
	}

	[[nodiscard]] partition_draft partition_for(const relation_descriptor& relation)
	{
		return {relation.id,
				"project:one",
				{"condition-universe:one", {"condition:true"}},
				"clang.cpp23",
				semantic('d'),
				content('e'),
				"exact",
				"assumption-set:one",
				{},
				{{"test.entity.compile-unit", "compile-unit:one", "covered", {}}},
				{}};
	}

	[[nodiscard]] incremental::input_fingerprint
	fingerprint_for(const relation_descriptor& relation,
					const validated_build_capture& capture,
					const relation_engine& engine)
	{
		return {capture.value().source.content_digest,
				capture.value().source_closure.closure_digest,
				capture.value().invocation.effective_invocation_digest,
				capture.value().toolchain_digest,
				semantic('f'),
				semantic('0'),
				semantic('1'),
				std::string{engine.registry_digest()},
				semantic('2'),
				semantic('3'),
				capture.value().invocation.environment_digest,
				content('4'),
				semantic('5'),
				relation.descriptor_digest,
				"normalizer-v1",
				semantic('6'),
				semantic('7'),
				"exact"};
	}

	[[nodiscard]] materialization_task_draft task_draft(const relation_descriptor& relation,
														const relation_engine& engine)
	{
		auto capture = capture_for();
		provider::provider_session session{"provider.test",
										   {1U, 0U, 0U},
										   semantic('5'),
										   {relation},
										   {},
										   {"clang.cpp23"},
										   "source",
										   "observation"};
		auto portable = provider::task::make(std::move(session),
											 capture.value().catalog,
											 {relation},
											 "condition:true",
											 "clang.cpp23",
											 {"clang-ast"});
		assert(portable);
		auto partition = partition_for(relation);
		auto manifest = make_partition_manifest(engine, partition);
		assert(manifest);
		const auto catalog_digest = capture.value().catalog.catalog_digest;
		materialization_task_draft output{"materialization-request:one",
										  content('d'),
										  std::move(capture),
										  std::move(*portable),
										  {},
										  {"provider.test",
										   {1U, 0U, 0U},
										   content('4'),
										   semantic('5'),
										   "provider.release",
										   semantic('8'),
										   {provider::sandbox_assurance::enforced, semantic('9')},
										   {}},
										  {{{"catalog:series",
											 "channel:main",
											 "engine-generation:one",
											 "condition-universe:one",
											 std::string{engine.registry_digest()},
											 semantic('2'),
											 semantic('8')},
											{1U, 0U, 0U},
											catalog_digest,
											std::nullopt},
										   semantic('a'),
										   semantic('b'),
										   "snapshot:main"}};
		output.partitions.push_back({relation.id,
									 {{manifest->partition_id,
									   fingerprint_for(relation, output.capture, engine),
									   manifest->coverage_digest,
									   semantic('c'),
									   false},
									  std::nullopt}});
		return output;
	}

	[[nodiscard]] materialization_runtime_binding
	runtime_for(const validated_materialization_task& task)
	{
		return {task.value().provider.provider_id,
				task.value().provider.provider_version,
				task.value().provider.provider_binary_digest,
				task.value().provider.provider_semantics_digest,
				task.value().provider_input_digest,
				semantic('d')};
	}

	void task_authority_and_determinism()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto first = validate_materialization_task(task_draft(relation, engine));
		auto second = validate_materialization_task(task_draft(relation, engine));
		assert(first && second);
		assert(first->id() == second->id());
		assert(first->input_binding_digest() == second->input_binding_digest());
		assert(first->plan().frontend_provider_executions == 1U);
		assert(!first->plan().warm_zero);

		auto provider_mismatch = task_draft(relation, engine);
		provider_mismatch.provider.provider_id = "provider.other";
		assert(!validate_materialization_task(std::move(provider_mismatch)));

		auto capture_mismatch = task_draft(relation, engine);
		capture_mismatch.partitions.front().candidate.current.input.source_digest = content('0');
		auto rejected = validate_materialization_task(std::move(capture_mismatch));
		assert(!rejected && rejected.error().detail == "authority-mismatch");
	}

	void result_terminals_and_atomic_rejection()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto task = validate_materialization_task(task_draft(relation, engine));
		assert(task);
		materialization_result_draft complete;
		complete.terminal = materialization_terminal::complete;
		complete.task_id = task->id();
		complete.task_input_digest = task->input_binding_digest();
		complete.runtime = runtime_for(*task);
		complete.partitions = {partition_for(relation)};
		complete.coverage = {{"relation", relation.id, "covered", {}}};
		auto accepted = validate_materialization_result(engine, *task, complete);
		assert(accepted);
		assert(accepted->partitions().size() == 1U);
		assert(!accepted->result_digest().empty());

		auto repeated = validate_materialization_result(engine, *task, std::move(complete));
		assert(repeated && repeated->result_digest() == accepted->result_digest());

		auto wrong_runtime_input = materialization_result_draft{};
		wrong_runtime_input.terminal = materialization_terminal::complete;
		wrong_runtime_input.task_id = task->id();
		wrong_runtime_input.task_input_digest = task->input_binding_digest();
		wrong_runtime_input.runtime = runtime_for(*task);
		wrong_runtime_input.runtime->task_input_digest = content('0');
		wrong_runtime_input.partitions = {partition_for(relation)};
		auto runtime_rejected =
			validate_materialization_result(engine, *task, std::move(wrong_runtime_input));
		assert(!runtime_rejected && runtime_rejected.error().detail == "task-or-provider-mismatch");

		materialization_result_draft partial;
		partial.terminal = materialization_terminal::partial;
		partial.task_id = task->id();
		partial.task_input_digest = task->input_binding_digest();
		partial.runtime = runtime_for(*task);
		partial.unresolved = {{"frontend.missing-input", "compile-unit:one", "recapture-source"}};
		assert(validate_materialization_result(engine, *task, std::move(partial)));

		materialization_result_draft failed;
		failed.terminal = materialization_terminal::failed;
		failed.task_id = task->id();
		failed.task_input_digest = task->input_binding_digest();
		failed.unresolved = {{"provider.failed", "compile-unit:one", "retry-provider"}};
		assert(validate_materialization_result(engine, *task, failed));
		failed.partitions = {partition_for(relation)};
		auto forbidden = validate_materialization_result(engine, *task, std::move(failed));
		assert(!forbidden && forbidden.error().detail == "publication-data-forbidden");

		auto malformed = materialization_result_draft{};
		malformed.terminal = materialization_terminal::complete;
		malformed.task_id = task->id();
		malformed.task_input_digest = task->input_binding_digest();
		malformed.runtime = runtime_for(*task);
		malformed.partitions = {partition_for(relation)};
		malformed.partitions.front().relation_descriptor_id = "unknown.relation.v1";
		auto atomic_reject = validate_materialization_result(engine, *task, std::move(malformed));
		assert(!atomic_reject);
	}
} // namespace

int main()
{
	task_authority_and_determinism();
	result_terminals_and_atomic_rejection();
}
