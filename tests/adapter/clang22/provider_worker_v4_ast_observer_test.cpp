#include "llvm/clang22/provider_worker_v4_ast_observer.hpp"

#include <cstdlib>
#include <iostream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/provider_task_v4.hpp"

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
		require(closure.has_value(), "ast observer closure fixture was rejected");
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
		require(invocation.has_value(), "ast observer invocation digest was rejected");
		input.normalized_invocation_digest = std::move(*invocation);
		input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		input.environment_digest = "sha256:" + std::string(64U, '5');
		input.closure = std::move(*closure);
		input.main_logical_path = "project://src/main.cpp";
		auto identity = derive_source_closure_task_v4_identity(input);
		require(identity.has_value(), "ast observer task identity was rejected");
		output.metadata = {std::move(input), std::move(*identity)};
		return output;
	}

	[[nodiscard]] sdk::result<provider_worker_v4_ast_observation_batch>
	run_once(const fixture& value)
	{
		const auto* main =
			value.metadata.input.closure.find_member(value.metadata.input.main_logical_path);
		require(main != nullptr, "ast observer fixture lost main member");
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
					unit, value.metadata, "compile-unit:v4-ast-observer");
				if (!result)
					return sdk::unexpected(std::move(result.error()));
				observed.emplace(std::move(*result));
				return {};
			});
		if (!executed)
			return sdk::unexpected(std::move(executed.error()));
		if (!observed)
			return sdk::unexpected(
				sdk::error{"test.ast-observer-no-output", "callback", "not-invoked"});
		return std::move(*observed);
	}
} // namespace

int main()
{
#if !defined(CXXLENS_TEST_CLANGXX22_PATH)
	return 77;
#else
	const auto fixture = make_fixture();
	auto first = run_once(fixture);
	if (!first)
	{
		std::cerr << "ast observer positive failed: " << first.error().code << " / "
				  << first.error().field << " / " << first.error().detail << '\n';
	}
	require(first.has_value(), "ast observer did not detach positive output");
	require(first->validate().has_value(), "ast observer batch failed validation");
	require(!first->observations.empty(), "ast observer produced no observations");
	require(first->observations.size() == first->rows.size(),
			"ast observer observation/row cardinality diverged");
	bool saw_entity = false;
	bool saw_call = false;
	for (const auto& observation : first->observations)
	{
		saw_entity |= observation.kind == provider_worker_v4_ast_observation_kind::entity;
		saw_call |= observation.kind == provider_worker_v4_ast_observation_kind::call;
		require(observation.validate().has_value(), "ast observer emitted invalid observation");
	}
	for (const auto& row : first->rows)
		require(provider::clang22::detect_native_escape(row).has_value(),
				"ast observer row retained native state");
	require(saw_entity, "ast observer missed function declaration");
	require(saw_call, "ast observer missed direct call");

	auto second = run_once(fixture);
	require(second.has_value(), "ast observer repeat execution failed");
	require(first->observations.size() == second->observations.size(),
			"ast observer repeat cardinality changed");
	for (std::size_t index = 0U; index < first->observations.size(); ++index)
		require(first->observations[index] == second->observations[index],
				"ast observer output was not deterministic");

	// A task identity changed after transport admission must not be accepted by the AST seam.
	auto tampered = fixture;
	tampered.metadata.identity.task_v4_digest.back() =
		tampered.metadata.identity.task_v4_digest.back() == '0' ? '1' : '0';
	auto rejected = run_once(tampered);
	require(!rejected && rejected.error().code == "provider-worker-v4.ast-input-invalid",
			"ast observer accepted a tampered task identity");

	return EXIT_SUCCESS;
#endif
}
