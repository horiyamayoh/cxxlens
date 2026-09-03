#include "sdk/materialization_task_internal.hpp"

#include <cstdlib>
#include <iostream>
#include <source_location>
#include <string>
#include <utility>
#include <vector>

#include "sdk/application_materialization_adoption_internal.hpp"
#include "sdk/materialization_writer_internal.hpp"

namespace
{
	using namespace cxxlens::sdk;
	using namespace cxxlens::sdk::detail;

	void require(const bool condition,
				 const std::source_location location = std::source_location::current())
	{
		if (!condition)
		{
			std::cerr << location.file_name() << ':' << location.line() << '\n';
			std::abort();
		}
	}

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
		require(registry.add(relation).has_value());
		auto engine = registry.build("generation:test");
		require(engine.has_value());
		return std::move(*engine);
	}

	[[nodiscard]] project_catalog catalog_for(const std::string& invocation,
											  const std::string& source,
											  const std::string& environment)
	{
		auto value = project_catalog::make(
			"project://root", environment, {{"catalog-unit:one", invocation, source, environment}});
		require(value.has_value());
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
		require(validated.has_value());
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
		require(portable.has_value());
		auto partition = partition_for(relation);
		auto manifest = make_partition_manifest(engine, partition);
		require(manifest.has_value());
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
											 std::string{engine.generation()},
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

	[[nodiscard]] generic_materialization_task_request
	generic_request(const relation_descriptor& relation, const relation_engine& engine)
	{
		auto draft = task_draft(relation, engine);
		return {std::move(draft.materialization_request_id),
				std::move(draft.provider_input_digest),
				std::move(draft.capture),
				std::move(draft.provider_task.session),
				std::move(draft.provider_task.outputs),
				draft.publication.snapshot.series.condition_universe_id,
				std::move(draft.provider_task.condition),
				std::move(draft.provider_task.interpretation),
				std::move(draft.provider_task.dependency_groups),
				"normalizer-v1",
				"assumption-set:one",
				"exact",
				{"compile-unit", "project:one", "covered"},
				{},
				std::move(draft.provider),
				std::move(draft.publication)};
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

	class transcript_sink final : public provider::frame_sink
	{
	  public:
		result<void> write(const std::span<const std::byte> bytes) override
		{
			transcript.insert(transcript.end(), bytes.begin(), bytes.end());
			return {};
		}

		std::vector<std::byte> transcript;
	};

	class application_provider final : public provider::portable_provider
	{
	  public:
		explicit application_provider(relation_descriptor relation) : relation_{std::move(relation)}
		{
		}

		[[nodiscard]] std::string_view id() const noexcept override
		{
			return "provider.test";
		}
		[[nodiscard]] semantic_version version() const noexcept override
		{
			return {1U, 0U, 0U};
		}
		[[nodiscard]] std::string_view semantic_contract_digest() const noexcept override
		{
			return contract_;
		}
		result<void> run(const provider::task& task, provider::context& context) override
		{
			row_builder builder{relation_};
			if (auto set = builder.set({relation_.id,
										relation_.columns.front().id,
										relation_.columns.front().type,
										{}},
									   detached_cell::typed("test_entity_id", "test-entity:one"));
				!set)
				return set;
			auto row = std::move(builder).finish();
			if (!row)
				return unexpected(std::move(row.error()));
			auto output = context.relation(relation_);
			if (auto begun = output.begin("clang-ast", "atomic:one", "batch:one"); !begun)
				return begun;
			if (auto pushed = output.push(*row); !pushed)
				return pushed;
			if (auto ended = output.end(); !ended)
				return ended;
			context.coverage().request("relation", relation_.id);
			if (auto classified = context.coverage().classify(
					{"relation", relation_.id, "covered", "frontend-observed"});
				!classified)
				return classified;
			context.coverage().request("task", task.task_id);
			return context.coverage().classify(
				{"task", task.task_id, "covered", "translation-unit-executed"});
		}

	  private:
		relation_descriptor relation_;
		std::string contract_{semantic('5')};
	};

	void task_authority_and_determinism()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto first = validate_materialization_task(task_draft(relation, engine));
		auto second = validate_materialization_task(task_draft(relation, engine));
		require(first && second);
		require(first->id() == second->id());
		require(first->input_binding_digest() == second->input_binding_digest());
		require(first->plan().frontend_provider_executions == 1U);
		require(!first->plan().warm_zero);

		auto provider_mismatch = task_draft(relation, engine);
		provider_mismatch.provider.provider_id = "provider.other";
		require(!validate_materialization_task(std::move(provider_mismatch)));

		auto capture_mismatch = task_draft(relation, engine);
		capture_mismatch.partitions.front().candidate.current.input.source_digest = content('0');
		auto rejected = validate_materialization_task(std::move(capture_mismatch));
		require(!rejected && rejected.error().detail == "authority-mismatch");
	}

	void generic_builder_is_the_only_incremental_identity_authority()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto first = make_generic_materialization_task(engine, generic_request(relation, engine));
		auto second = make_generic_materialization_task(engine, generic_request(relation, engine));
		require(first && second && first->id() == second->id() &&
				first->input_binding_digest() == second->input_binding_digest());
		require(first->plan().frontend_provider_executions == 1U && !first->plan().warm_zero);

		auto warm = generic_request(relation, engine);
		warm.prior_partitions.push_back(
			{relation.id, first->value().partitions.front().candidate.current});
		auto reused = make_generic_materialization_task(engine, std::move(warm));
		require(reused && reused->plan().warm_zero &&
				reused->plan().frontend_provider_executions == 0U);

		auto changed_provider = generic_request(relation, engine);
		changed_provider.provider.provider_binary_digest = content('0');
		auto invalidated = make_generic_materialization_task(engine, std::move(changed_provider));
		require(invalidated && invalidated->id() != first->id());

		auto omitted = generic_request(relation, engine);
		omitted.output_normalizer_version.clear();
		auto rejected = make_generic_materialization_task(engine, std::move(omitted));
		require(!rejected && rejected.error().detail == "omitted-authority");

		auto unrelated_prior = generic_request(relation, engine);
		unrelated_prior.prior_partitions.push_back(
			{"test.other.v1", first->value().partitions.front().candidate.current});
		auto unrelated = make_generic_materialization_task(engine, std::move(unrelated_prior));
		require(!unrelated && unrelated.error().detail == "unrequested-relation");
	}

	void result_terminals_and_atomic_rejection()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto task = validate_materialization_task(task_draft(relation, engine));
		require(task.has_value());
		materialization_result_draft complete;
		complete.terminal = materialization_terminal::complete;
		complete.task_id = task->id();
		complete.task_input_digest = task->input_binding_digest();
		complete.runtime = runtime_for(*task);
		complete.partitions = {partition_for(relation)};
		complete.coverage = {{"relation", relation.id, "covered", {}}};
		auto accepted = validate_materialization_result(engine, *task, complete);
		require(accepted.has_value());
		require(accepted->partitions().size() == 1U);
		require(!accepted->result_digest().empty());

		auto repeated = validate_materialization_result(engine, *task, std::move(complete));
		require(repeated && repeated->result_digest() == accepted->result_digest());

		auto wrong_runtime_input = materialization_result_draft{};
		wrong_runtime_input.terminal = materialization_terminal::complete;
		wrong_runtime_input.task_id = task->id();
		wrong_runtime_input.task_input_digest = task->input_binding_digest();
		wrong_runtime_input.runtime = runtime_for(*task);
		wrong_runtime_input.runtime->task_input_digest = content('0');
		wrong_runtime_input.partitions = {partition_for(relation)};
		auto runtime_rejected =
			validate_materialization_result(engine, *task, std::move(wrong_runtime_input));
		require(!runtime_rejected &&
				runtime_rejected.error().detail == "task-or-provider-mismatch");

		materialization_result_draft partial;
		partial.terminal = materialization_terminal::partial;
		partial.task_id = task->id();
		partial.task_input_digest = task->input_binding_digest();
		partial.runtime = runtime_for(*task);
		partial.unresolved = {{"frontend.missing-input", "compile-unit:one", "recapture-source"}};
		require(validate_materialization_result(engine, *task, std::move(partial)).has_value());

		materialization_result_draft failed;
		failed.terminal = materialization_terminal::failed;
		failed.task_id = task->id();
		failed.task_input_digest = task->input_binding_digest();
		failed.unresolved = {{"provider.failed", "compile-unit:one", "retry-provider"}};
		require(validate_materialization_result(engine, *task, failed).has_value());
		failed.partitions = {partition_for(relation)};
		auto forbidden = validate_materialization_result(engine, *task, std::move(failed));
		require(!forbidden && forbidden.error().detail == "publication-data-forbidden");

		auto malformed = materialization_result_draft{};
		malformed.terminal = materialization_terminal::complete;
		malformed.task_id = task->id();
		malformed.task_input_digest = task->input_binding_digest();
		malformed.runtime = runtime_for(*task);
		malformed.partitions = {partition_for(relation)};
		malformed.partitions.front().relation_descriptor_id = "unknown.relation.v1";
		auto atomic_reject = validate_materialization_result(engine, *task, std::move(malformed));
		require(!atomic_reject);
	}

	void single_writer_publication_and_rejection()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto task = validate_materialization_task(task_draft(relation, engine));
		require(task.has_value());
		materialization_result_draft complete;
		complete.terminal = materialization_terminal::complete;
		complete.task_id = task->id();
		complete.task_input_digest = task->input_binding_digest();
		complete.runtime = runtime_for(*task);
		complete.partitions = {partition_for(relation)};
		complete.coverage = {{"relation", relation.id, "covered", {}}};
		auto result = validate_materialization_result(engine, *task, std::move(complete));
		require(result.has_value());
		auto source =
			make_materialization_publication_source(engine, *task, *result, {}, semantic('e'));
		if (!source)
			std::cerr << source.error().code << ':' << source.error().field << ':'
					  << source.error().detail << '\n';
		require(source.has_value());
		auto empty_combination = combine_materialization_publication_sources(engine, {});
		require(!empty_combination && empty_combination.error().detail == "empty");
		auto duplicate_combination =
			combine_materialization_publication_sources(engine, {*source, *source});
		require(!duplicate_combination && duplicate_combination.error().detail == "duplicate");

		auto other_task_draft = task_draft(relation, engine);
		other_task_draft.publication.publication_target = "snapshot:other";
		auto other_task = validate_materialization_task(std::move(other_task_draft));
		require(other_task.has_value());
		materialization_result_draft other_complete;
		other_complete.terminal = materialization_terminal::complete;
		other_complete.task_id = other_task->id();
		other_complete.task_input_digest = other_task->input_binding_digest();
		other_complete.runtime = runtime_for(*other_task);
		other_complete.partitions = {partition_for(relation)};
		other_complete.coverage = {{"relation", relation.id, "covered", {}}};
		auto other_result =
			validate_materialization_result(engine, *other_task, std::move(other_complete));
		require(other_result.has_value());
		auto other_source = make_materialization_publication_source(
			engine, *other_task, *other_result, {}, semantic('f'));
		require(other_source.has_value());
		auto authority_mismatch =
			combine_materialization_publication_sources(engine, {*source, *other_source});
		require(!authority_mismatch && authority_mismatch.error().detail == "different");
		auto store = make_in_memory_snapshot_store(engine);
		require(store.has_value());
		auto published = publish_materialization_source(engine, *store, std::move(*source));
		require(published.has_value());
		require(published->publication_verified);
		require(published->snapshot.publication().state == publication_state::committed);
		require(published->task_id == result->task_id());
		require(published->result_digest == result->result_digest());
		require(published->source_receipt_digest == semantic('e'));

		const auto empty_host = partition_for(relation);
		auto rejected_host = make_materialization_publication_source(
			engine, *task, *result, std::span{&empty_host, 1U}, semantic('e'));
		require(!rejected_host && rejected_host.error().detail == "empty");
	}

	void sealed_provider_adoption_is_the_only_application_publication_path()
	{
		const auto relation = descriptor();
		const auto engine = engine_for(relation);
		auto task = validate_materialization_task(task_draft(relation, engine));
		require(task.has_value());

		application_provider provider{relation};
		transcript_sink sink;
		provider::protocol_writer writer{sink};
		const provider::protocol_credit credit{64U * 1024U * 1024U, 65536U};
		writer.grant_credit(credit);
		require(provider::run_worker(provider, task->value().provider_task, writer).has_value());
		auto frames = provider::decode_frame_stream(sink.transcript);
		require(frames.has_value());
		provider::execution_budget budget;
		const provider::detail::transcript_validation_request validation{
			task->value().provider_task.task_id,
			task->value().provider.provider_id,
			task->value().provider.provider_version,
			nullptr,
			task->value().provider_task.outputs,
			credit,
			&budget,
			false,
		};
		auto transcript = provider::detail::validate_provider_transcript(
			validation, *frames, provider::protocol_limits{});
		if (!transcript)
			std::cerr << transcript.error().code << ':' << transcript.error().field << ':'
					  << transcript.error().detail << '\n';
		else if (transcript->sealing_error())
			std::cerr << transcript->sealing_error()->code << ':'
					  << transcript->sealing_error()->field << ':'
					  << transcript->sealing_error()->detail << '\n';
		require(transcript && transcript->sealed() && !transcript->sealing_error());

		auto runtime = runtime_for(*task);
		runtime.runtime_receipt_digest = semantic('e');
		auto prepared = prepare_sealed_application_materialization(
			engine, *task, *transcript->sealed(), runtime, semantic('e'), semantic('f'));
		require(prepared.has_value());
		auto store = make_in_memory_snapshot_store(engine);
		require(store.has_value() && !store->current(task->value().publication.snapshot.series));
		auto adopted =
			publish_prepared_application_materializations(engine, *store, {std::move(*prepared)});
		if (!adopted)
			std::cerr << adopted.error().code << ':' << adopted.error().field << ':'
					  << adopted.error().detail << '\n';
		require(adopted && adopted->publication.publication_verified &&
				adopted->publication.snapshot.manifest().partitions.size() == 1U &&
				adopted->coverage.size() == 2U && adopted->unresolved.empty());
		auto repeated_store = make_in_memory_snapshot_store(engine);
		require(repeated_store.has_value());
		auto repeated_prepared = prepare_sealed_application_materialization(
			engine, *task, *transcript->sealed(), runtime, semantic('e'), semantic('f'));
		require(repeated_prepared.has_value());
		auto repeated = publish_prepared_application_materializations(
			engine, *repeated_store, {std::move(*repeated_prepared)});
		require(repeated &&
				repeated->publication.snapshot.id() == adopted->publication.snapshot.id());

		auto rejected = prepare_sealed_application_materialization(
			engine, *task, *transcript->sealed(), std::move(runtime), semantic('0'), semantic('f'));
		require(!rejected && rejected.error().field == "runtime_receipt");
	}
} // namespace

int main()
{
	task_authority_and_determinism();
	generic_builder_is_the_only_incremental_identity_authority();
	result_terminals_and_atomic_rejection();
	single_writer_publication_and_rejection();
	sealed_provider_adoption_is_the_only_application_publication_path();
}
