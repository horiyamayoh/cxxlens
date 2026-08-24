#include "llvm/clang22/provider_worker_v4_output_normalizer.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/observation_v2.hpp"
#include "llvm/clang22/provider_task_v4.hpp"
#include "llvm/clang22/provider_worker_v4_ast_observer.hpp"

namespace
{
	using namespace cxxlens;
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(EXIT_FAILURE);
		}
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] std::vector<std::string> arguments()
	{
#if defined(CXXLENS_TEST_CLANGXX22_PATH)
		return {
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			"project://src/main.cpp",
		};
#else
		return {
			"/usr/bin/clang++", "-std=c++23", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
#endif
	}

	struct fixture
	{
		source_closure_task_v4_decoded metadata;
		std::string source;
		std::string file_id;
		std::vector<std::string> effective_arguments;
	};

	[[nodiscard]] fixture make_fixture()
	{
		fixture output;
		output.source = "int leaf(int value) { return value + 1; }\n"
						"int main() { return leaf(41); }\n";
		auto closure = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content(output.source)},
		});
		require(closure.has_value(), "normalizer closure fixture was rejected");
		output.file_id = closure->members.front().file_id;
		output.effective_arguments = arguments();
		source_closure_task_v4_input input;
		input.base_task_index = 0U;
		input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		input.task_input_digest = "sha256:" + std::string(64U, '2');
		input.logical_working_directory = "project://src";
		auto invocation = derive_provider_task_v4_effective_invocation_digest(
			input.logical_working_directory, output.effective_arguments);
		require(invocation.has_value(), "normalizer invocation digest was rejected");
		input.normalized_invocation_digest = std::move(*invocation);
		input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		input.environment_digest = "sha256:" + std::string(64U, '5');
		input.closure = std::move(*closure);
		input.main_logical_path = "project://src/main.cpp";
		auto identity = derive_source_closure_task_v4_identity(input);
		require(identity.has_value(), "normalizer task identity was rejected");
		output.metadata = {std::move(input), std::move(*identity)};
		return output;
	}

	[[nodiscard]] sdk::result<provider_worker_v4_ast_observation_batch>
	run_observer(const fixture& value)
	{
		const auto* main =
			value.metadata.input.closure.find_member(value.metadata.input.main_logical_path);
		require(main != nullptr, "normalizer fixture lost main member");
		provider::clang22::translation_unit_input input{
			value.metadata.input.closure.snapshot_id,
			main->file_id,
			value.metadata.input.main_logical_path,
			value.source,
			value.effective_arguments,
		};
		std::optional<provider_worker_v4_ast_observation_batch> observed;
		auto executed = provider::clang22::with_translation_unit(
			input,
			[&](provider::clang22::borrowed_translation_unit& unit) -> sdk::result<void>
			{
				auto result = observe_provider_worker_v4_ast(
					unit, value.metadata, "compile-unit:v4-output-normalizer");
				if (!result)
					return sdk::unexpected(std::move(result.error()));
				observed.emplace(std::move(*result));
				return {};
			});
		if (!executed)
			return sdk::unexpected(std::move(executed.error()));
		if (!observed)
			return sdk::unexpected(
				sdk::error{"test.normalizer-no-output", "callback", "not-invoked"});
		return std::move(*observed);
	}

	[[nodiscard]] provider_worker_v4_output_normalizer_options options(const fixture& value)
	{
		provider_worker_v4_output_normalizer_options output;
		output.toolchain_context_id =
			*sdk::semantic_digest("toolchain-context", value.metadata.input.toolchain_digest);
		return output;
	}

	[[nodiscard]] sdk::result<provider_worker_v4_ast_observation_batch>
	make_unresolved_call_batch(const provider_worker_v4_ast_observation_batch& source)
	{
		provider_worker_v4_ast_observation observation;
		observation.kind = provider_worker_v4_ast_observation_kind::call;
		observation.compile_unit = source.compile_unit;
		observation.semantic_key = "call:source-unavailable";
		observation.payload.emplace("call.kind", "indirect_function");
		observation.payload.emplace("call.unresolved_reason",
									"function-pointer-target-not-modeled");
		observation.exact_equivalence = false;
		observation.limitation = "source-span-unavailable:test";
		materialization::native_observation_v2 native;
		native.kind = materialization::observation_v2_kind::call;
		native.final_relation_compile_unit_id = source.compile_unit;
		native.semantic_key = observation.semantic_key;
		for (const auto& [key, value] : observation.payload)
			native.payload.push_back({key, value});
		native.exact_equivalence = false;
		native.limitation = observation.limitation;
		const materialization::observation_v2_task_authority authority{
			source.compile_unit, source.source_snapshot, source.source_file, 1024U};
		auto row = materialization::make_observation_v2_row(native, authority);
		if (!row)
			return sdk::unexpected(std::move(row.error()));
		provider_worker_v4_ast_observation_batch output{
			source.task_id,
			source.task_v4_digest,
			source.compile_unit,
			source.source_snapshot,
			source.source_file,
			1024U,
			{std::move(observation)},
			{std::move(*row)},
			0U,
			{},
		};
		return output;
	}
} // namespace

int main()
{
#if !defined(CXXLENS_TEST_CLANGXX22_PATH)
	return 77;
#else
	const auto fixture = make_fixture();
	auto observed = run_observer(fixture);
	require(observed.has_value(), "normalizer observer failed");
	const auto normalizer_options = options(fixture);
	auto first = normalize_provider_worker_v4_output(*observed, normalizer_options);
	if (!first)
		std::cerr << "normalizer positive failed: " << first.error().code << " / "
				  << first.error().field << " / " << first.error().detail << '\n';
	require(first.has_value(), "normalizer rejected positive AST output");
	require(first->validate().has_value(), "normalizer output failed validation");
	require(first->batches.size() == task_v4_output_descriptor_ids.size(),
			"normalizer did not emit exact six batches");
	for (std::size_t index{}; index < first->batches.size(); ++index)
	{
		require(first->batches[index].descriptor_id == task_v4_output_descriptor_ids[index],
				"normalizer descriptor order changed");
		require(first->batches[index].atomic_output_group_id == "clang22-atomic",
				"normalizer atomic group changed");
	}
	require(!first->batches[0U].rows.empty(), "normalizer emitted no direct-target rows");
	require(!first->batches[1U].rows.empty(), "normalizer emitted no call-site rows");
	require(!first->batches[2U].rows.empty(), "normalizer emitted no entity rows");
	require(!first->batches[3U].rows.empty(), "normalizer emitted no call observations");
	require(!first->batches[4U].rows.empty(), "normalizer emitted no entity observations");
	require(!first->batches[5U].rows.empty(), "normalizer emitted no type observations");

	auto second = normalize_provider_worker_v4_output(*observed, normalizer_options);
	require(second.has_value(), "normalizer repeat execution failed");
	for (std::size_t batch_index{}; batch_index < first->batches.size(); ++batch_index)
	{
		require(first->batches[batch_index].descriptor_id ==
					second->batches[batch_index].descriptor_id,
				"normalizer repeat descriptor changed");
		require(first->batches[batch_index].rows.size() == second->batches[batch_index].rows.size(),
				"normalizer repeat row cardinality changed");
		for (std::size_t row_index{}; row_index < first->batches[batch_index].rows.size();
			 ++row_index)
			require(first->batches[batch_index].rows[row_index].canonical_form() ==
						second->batches[batch_index].rows[row_index].canonical_form(),
					"normalizer output was not deterministic");
	}
	require(first->unresolved == second->unresolved,
			"normalizer unresolved output was not deterministic");

	// A source-less call remains an observation but cannot acquire a hard source-backed
	// cc.call_site/direct-target pair.
	auto unresolved_input = make_unresolved_call_batch(*observed);
	require(unresolved_input.has_value(), "normalizer unresolved fixture was rejected");
	auto unresolved = normalize_provider_worker_v4_output(*unresolved_input, normalizer_options);
	require(unresolved.has_value(), "normalizer rejected source-less observation");
	require(unresolved->batches[0U].rows.empty() && unresolved->batches[1U].rows.empty(),
			"normalizer fabricated canonical rows without source authority");
	require(!unresolved->batches[3U].rows.empty(), "normalizer dropped source-less observation");
	require(!unresolved->unresolved.empty() && !unresolved->exact_equivalence,
			"normalizer did not retain typed unresolved/non-exact state");

	auto limited_options = normalizer_options;
	limited_options.limits.maximum_rows = 1U;
	auto limited = normalize_provider_worker_v4_output(*observed, limited_options);
	require(!limited && limited.error().code == "provider-worker-v4.output-limit",
			"normalizer ignored row resource limit");

	std::stop_source stop;
	stop.request_stop();
	auto cancelled_options = normalizer_options;
	cancelled_options.cancellation = stop.get_token();
	auto cancelled = normalize_provider_worker_v4_output(*observed, cancelled_options);
	require(!cancelled && cancelled.error().code == "provider-worker-v4.output-cancelled",
			"normalizer ignored cancellation");
	return EXIT_SUCCESS;
#endif
}
