#include "llvm/clang22/source_closure_task_v4_worker.hpp"

#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "llvm/clang22/source_closure_task_v4.hpp"

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

	[[nodiscard]] source_closure_task_v4_input fixture()
	{
		auto result = make_source_closure_snapshot({
			{"project://src/main.cpp",
			 source_closure_role::main,
			 source_closure_encoding::utf8,
			 content("int main() { return 0; }\n")},
		});
		require(result.has_value(), "worker closure fixture was rejected");
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
		input.closure = std::move(*result);
		input.main_logical_path = "project://src/main.cpp";
		input.logical_working_directory = "project://src";
		return input;
	}

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	[[nodiscard]] std::vector<std::string> exact_clang_arguments()
	{
		std::vector<std::string> arguments{
			CXXLENS_TEST_CLANGXX22_PATH,
			"-std=c++23",
			"-nostdinc",
			"-nostdinc++",
#if defined(CXXLENS_TEST_CLANG22_RESOURCE_DIR)
			"-resource-dir=" CXXLENS_TEST_CLANG22_RESOURCE_DIR,
#endif
			"project://src/main.cpp",
		};
		return arguments;
	}
#endif
} // namespace

int main()
{
	auto input = fixture();
	auto identity = derive_source_closure_task_v4_identity(input);
	require(identity.has_value(), "worker task-v4 fixture could not derive identity");
	const std::vector<std::string> arguments{"clang++", "project://src/main.cpp"};
	const std::vector<std::string> roots{"/usr/include"};

	// A complete, bound payload reaches the candidate's explicit callback gate.  No compiler is
	// launched on this host because the callback is absent; the callback gate remains fail-closed
	// until the production dispatcher supplies a validated compiler callback.
	auto result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		identity->task_v4_input_digest,
		arguments,
		roots,
		cxxlens::provider::clang22::translation_unit_callback{},
	});
	require(!result && result.error().code == "source-closure.worker-input-invalid",
			"bound task-v4 payload did not fail closed at the inactive callback gate");

	auto wrong_input_digest = identity->task_v4_input_digest;
	wrong_input_digest.back() = wrong_input_digest.back() == '0' ? '1' : '0';
	result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		wrong_input_digest,
		arguments,
		roots,
		cxxlens::provider::clang22::translation_unit_callback{},
	});
	require(!result && result.error().code == "source-closure.task-v4-input-digest-mismatch",
			"worker candidate accepted a payload with a foreign outer input digest");

#if defined(CXXLENS_TEST_CLANGXX22_PATH)
	// A complete task-v4 payload must reach the real, exact Clang 22 callback through the
	// closure-exclusive VFS. The callback deliberately only observes the borrowed lifetime;
	// all compiler-owned state must be detached by the worker before this receipt is returned.
	const auto exact_arguments = exact_clang_arguments();
	const std::vector<std::string> qualified_read_roots{CXXLENS_TEST_CLANG22_ROOT};
	bool callback_ran = false;
	result = execute_source_closure_task_v4_candidate({
		identity->input_payload,
		input.closure,
		identity->base_task_digest,
		identity->task_v4_input_digest,
		exact_arguments,
		qualified_read_roots,
		[&callback_ran](cxxlens::provider::clang22::borrowed_translation_unit& unit)
			-> cxxlens::sdk::result<void>
		{
			callback_ran = true;
			(void)unit.ast();
			(void)unit.source_manager();
			return {};
		},
	});
	if (!result)
		std::cerr << "exact Clang 22 task-v4 execution failed: " << result.error().code << " / "
				  << result.error().detail << '\n';
	require(result.has_value(), "exact Clang 22 task-v4 candidate did not execute successfully");
	require(callback_ran, "exact Clang 22 task-v4 callback did not run");
	require(result->task_id == identity->task_id,
			"task-v4 worker receipt returned a foreign task identity");
	require(result->task_v4_digest == identity->task_v4_digest,
			"task-v4 worker receipt returned a foreign task digest");
	require(result->task_v4_input_digest == identity->task_v4_input_digest,
			"task-v4 worker receipt returned a foreign input digest");
	require(result->closure_id == input.closure.snapshot_id,
			"task-v4 worker receipt returned a foreign closure identity");
#endif

	return 0;
}
