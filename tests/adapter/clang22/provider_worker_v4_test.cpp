#include "llvm/clang22/provider_worker_v4.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using namespace cxxlens::detail::clang22;

	void require(const bool condition, const std::string_view message)
	{
		if (!condition)
		{
			std::cerr << message << '\n';
			std::exit(1);
		}
	}

	[[nodiscard]] std::shared_ptr<const std::string> content(std::string value)
	{
		return std::make_shared<const std::string>(std::move(value));
	}

	[[nodiscard]] source_closure_task_v4_input task_input()
	{
		auto closure = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
		});
		require(closure.has_value(), "worker-v4 closure fixture was rejected");
		source_closure_task_v4_input input;
		input.base_task_index = 0U;
		input.base_provider_task_id = "task:semantic-v2:sha256:" + std::string(64U, '1');
		const std::string base_projection{"{\"a\":\"b\",\"schema\":\"base\"}"};
		const auto base_bytes =
			std::as_bytes(std::span{base_projection.data(), base_projection.size()});
		input.base_task_projection = {base_bytes.begin(), base_bytes.end()};
		input.task_input_digest = "sha256:" + std::string(64U, '2');
		input.normalized_invocation_digest = "semantic-v2:sha256:" + std::string(64U, '3');
		input.toolchain_digest = "semantic-v2:sha256:" + std::string(64U, '4');
		input.environment_digest = "sha256:" + std::string(64U, '5');
		input.closure = std::move(*closure);
		input.main_logical_path = "project://src/main.cpp";
		input.logical_working_directory = "project://src";
		return input;
	}

	[[nodiscard]] source_closure_task_v4_decoded decoded_fixture()
	{
		auto input = task_input();
		auto identity = derive_source_closure_task_v4_identity(input);
		require(identity.has_value(), "worker-v4 task identity was rejected");
		return {std::move(input), std::move(*identity)};
	}

	[[nodiscard]] std::vector<std::string> arguments()
	{
		return {"/usr/bin/clang++", "-nostdinc", "-nostdinc++", "project://src/main.cpp"};
	}
} // namespace

int main()
{
	auto metadata = decoded_fixture();
	auto closure = metadata.input.closure;
	auto effective_arguments = arguments();
	const std::vector<std::string> roots{"/usr"};

	// The typed boundary rejects an absent callback before any native invocation.
	auto rejected =
		execute_provider_worker_v4({metadata,
									closure,
									effective_arguments,
									roots,
									cxxlens::provider::clang22::translation_unit_callback{}});
	require(!rejected && rejected.error().code == "provider-worker-v4.input-invalid",
			"worker-v4 accepted a missing callback");

	// A separately supplied but differently authenticated snapshot cannot be substituted.
	auto foreign_input = task_input();
	auto foreign_closure = make_source_closure_snapshot({
		{"project://src/main.cpp",
		 source_closure_role::main,
		 source_closure_encoding::utf8,
		 content("int main() { return 1; }\n")},
	});
	require(foreign_closure.has_value(), "foreign closure fixture was rejected");
	auto foreign_metadata = metadata;
	foreign_metadata.input.closure = std::move(foreign_input.closure);
	rejected =
		execute_provider_worker_v4({std::move(foreign_metadata),
									std::move(*foreign_closure),
									effective_arguments,
									roots,
									cxxlens::provider::clang22::translation_unit_callback{}});
	require(!rejected && rejected.error().code == "source-closure.task-v4-binding-mismatch",
			"worker-v4 accepted a foreign source closure");

	// A receipt or identity altered after decoding is rejected before the compiler callback gate.
	auto tampered = metadata;
	tampered.identity.task_v4_digest.back() =
		tampered.identity.task_v4_digest.back() == '0' ? '1' : '0';
	rejected =
		execute_provider_worker_v4({std::move(tampered),
									closure,
									effective_arguments,
									roots,
									cxxlens::provider::clang22::translation_unit_callback{}});
	require(!rejected && rejected.error().code == "source-closure.task-v4-binding-mismatch",
			"worker-v4 accepted tampered decoded identity");

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	// The production positive path is enabled by the exact packaged Clang-22 test fixture.
	// Source bytes are served only from the authenticated closure; the qualified root is the
	// explicit toolchain input used by the native VFS.
	const auto expected_task_id = metadata.identity.task_id;
	const auto expected_closure_id = closure.snapshot_id;
	auto exact_arguments = std::vector<std::string>{
		CXXLENS_TEST_CLANGXX22_PATH,
		"-std=c++23",
		"-nostdinc",
		"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
		"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
		"project://src/main.cpp",
	};
	bool callback_ran = false;
	auto exact = execute_provider_worker_v4(
		{std::move(metadata),
		 std::move(closure),
		 exact_arguments,
		 std::vector<std::string>{CXXLENS_TEST_CLANG22_ROOT},
		 [&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit& unit)
			 -> cxxlens::sdk::result<void>
		 {
			 callback_ran = true;
			 (void)unit.ast();
			 (void)unit.source_manager();
			 return {};
		 }});
	if (!exact)
		std::cerr << "worker-v4 exact Clang execution failed: " << exact.error().code << " / "
				  << exact.error().detail << '\n';
	require(exact.has_value(), "worker-v4 did not execute exact Clang 22");
	require(callback_ran, "worker-v4 callback did not run");
	require(exact->translation_unit_executed, "worker-v4 receipt did not attest execution");
	require(exact->task_id == expected_task_id, "worker-v4 receipt task identity drifted");
	require(exact->source_closure_id == expected_closure_id,
			"worker-v4 receipt closure identity drifted");
	require(exact->missing_output.size() == 3U,
			"worker-v4 receipt omitted exact missing output data");
	require(exact->missing_output[0].field == "provider-output.analysis-recipe" &&
				exact->missing_output[1].field == "provider-output.output-plan" &&
				exact->missing_output[2].field == "provider-output.publication-target",
			"worker-v4 receipt missing-data fields were not canonical");
#endif

	return 0;
}
