#include "llvm/clang22/materialization_request_v2_2.hpp"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <ranges>
#include <string>
#include <utility>
#include <vector>

namespace source_closure = cxxlens::detail::clang22::source_closure;

namespace
{
	[[nodiscard]] std::vector<std::byte> bytes(const std::string& value)
	{
		return {reinterpret_cast<const std::byte*>(value.data()),
			reinterpret_cast<const std::byte*>(value.data() + value.size())};
	}

	[[nodiscard]] source_closure::blob blob(std::string value)
	{
		auto content = bytes(value);
		return {source_closure::sha256_digest(content), std::move(content)};
	}

	[[nodiscard]] std::shared_ptr<const source_closure::validated_snapshot>
	closure(const std::string& main_path, const std::string& main_text,
		const source_closure::blob& shared)
	{
		auto main = blob(main_text);
		const auto provenance =
			source_closure::sha256_digest(bytes("request-v2_2-test"));
		source_closure::snapshot input;
		input.files = {
			{main_path, main.content_digest, main.content.size(),
				source_closure::file_role::main_source, provenance},
			{"project://include/shared.hpp", shared.content_digest,
				shared.content.size(), source_closure::file_role::project_header,
				provenance},
		};
		input.blobs = {main, shared};
		auto result = source_closure::validate(std::move(input));
		assert(result);
		return std::make_shared<const source_closure::validated_snapshot>(
			std::move(*result));
	}

	[[nodiscard]] source_closure::task_v4 task(
		std::shared_ptr<const source_closure::validated_snapshot> closure,
		std::string main)
	{
		source_closure::task_v4 value;
		value.main_logical_path = std::move(main);
		value.logical_working_directory = "project://";
		value.effective_arguments = {
			"clang++", "-Iproject://include", value.main_logical_path};
		value.closure = std::move(closure);
		auto id = source_closure::derive_task_v4_id(value);
		assert(id);
		value.task_id = *id;
		return value;
	}

	[[nodiscard]] source_closure::materialization_request_v2_2 request()
	{
		auto shared = blob("inline int shared(){return 1;}\n");
		auto a = closure("project://src/a.cpp",
			"#include \"shared.hpp\"\nint a(){return shared();}\n", shared);
		auto b = closure("project://src/b.cpp",
			"#include \"shared.hpp\"\nint b(){return shared();}\n", shared);
		source_closure::materialization_request_v2_2 value;
		value.required_features = source_closure::request_v2_2_required_features();
		value.closures = {a, b};
		value.tasks = {
			task(a, "project://src/a.cpp"), task(b, "project://src/b.cpp")};
		auto digest = source_closure::derive_request_v2_2_digest(value);
		assert(digest);
		value.request_digest = *digest;
		value.request_id = "materialization-request:" + *digest;
		return value;
	}

	void expect_error(source_closure::materialization_request_v2_2 value,
		const std::vector<std::string>& advertised, const std::string& code)
	{
		auto result =
			source_closure::validate_request_v2_2(std::move(value), advertised);
		assert(!result);
		assert(result.error().code == code);
	}
} // namespace

int main()
{
	const std::vector<std::string> advertised{
		"other-provider-feature-v1", "task-source-closure-v1",
		"task-input-chunks-v1"};
	auto value = request();
	auto valid = source_closure::validate_request_v2_2(value, advertised);
	assert(valid);
	assert(valid->tasks.size() == 2U);
	assert(valid->closures.size() == 2U);
	const auto naive = value.closures[0]->aggregate_bytes +
		value.closures[1]->aggregate_bytes;
	assert(valid->unique_blob_bytes < naive);

	auto permuted = request();
	std::ranges::reverse(permuted.tasks);
	std::ranges::reverse(permuted.closures);
	auto digest = source_closure::derive_request_v2_2_digest(permuted);
	assert(digest == permuted.request_digest);
	auto permuted_valid =
		source_closure::validate_request_v2_2(std::move(permuted), advertised);
	assert(permuted_valid);
	assert(permuted_valid->request_id == valid->request_id);

	auto missing_feature = request();
	expect_error(std::move(missing_feature), {"task-input-chunks-v1"},
		"source-closure.request-v2_2-feature-missing");

	auto duplicate_feature = request();
	expect_error(std::move(duplicate_feature),
		{"task-input-chunks-v1", "task-input-chunks-v1",
			"task-source-closure-v1"},
		"source-closure.request-v2_2-feature-invalid");

	auto stale_task = request();
	stale_task.tasks[0].task_id = stale_task.tasks[1].task_id;
	expect_error(std::move(stale_task), advertised,
		"source-closure.request-v2_2-task-invalid");

	auto duplicate_closure = request();
	duplicate_closure.closures.push_back(duplicate_closure.closures.front());
	expect_error(std::move(duplicate_closure), advertised,
		"source-closure.request-v2_2-closure-invalid");

	auto stale_request = request();
	stale_request.request_digest =
		source_closure::sha256_digest(bytes("stale"));
	expect_error(std::move(stale_request), advertised,
		"source-closure.request-v2_2-id-mismatch");
	return 0;
}
