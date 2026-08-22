#include "llvm/clang22/source_closure_task_v4.hpp"

#include <algorithm>
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

	[[nodiscard]] source_closure_file_input
	file(std::string path, source_closure_role role, std::string bytes)
	{
		return {std::move(path), role, source_closure_encoding::utf8, content(std::move(bytes))};
	}

	[[nodiscard]] source_closure_snapshot closure_fixture()
	{
		auto result = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 "#include \"include/value.hpp\"\nint main() { return value(); }\n"),
			file("project://src/include/value.hpp",
				 source_closure_role::header,
				 "#pragma once\ninline int value() { return 42; }\n"),
		});
		require(result.has_value(), "task-v4 closure fixture was rejected");
		return std::move(*result);
	}

	[[nodiscard]] std::vector<std::byte> bytes(const std::string_view value)
	{
		const auto raw = std::as_bytes(std::span{value.data(), value.size()});
		return {raw.begin(), raw.end()};
	}

	[[nodiscard]] std::string typed(const std::string_view prefix, const char digit)
	{
		return std::string{prefix} + std::string(64U, digit);
	}

	[[nodiscard]] source_closure_task_v4_input input_fixture(source_closure_snapshot closure)
	{
		source_closure_task_v4_input input;
		input.base_task_index = 3U;
		input.base_provider_task_id = typed("task:semantic-v2:sha256:", '1');
		input.base_task_projection = bytes(R"({"a":"b","schema":"base"})");
		input.task_input_digest = typed("sha256:", '2');
		input.normalized_invocation_digest = typed("semantic-v2:sha256:", '3');
		input.toolchain_digest = typed("semantic-v2:sha256:", '4');
		input.environment_digest = typed("sha256:", '5');
		input.closure = std::move(closure);
		input.main_logical_path = "project://src/main.cpp";
		input.logical_working_directory = "project://src";
		return input;
	}

	void expect_failure(const cxxlens::sdk::result<source_closure_task_v4_decoded>& result,
						const std::string_view message)
	{
		require(!result, message);
	}
} // namespace

int main()
{
	auto input = input_fixture(closure_fixture());
	auto identity = derive_source_closure_task_v4_identity(input);
	require(identity.has_value(), "valid task-v4 identity was rejected");
	require(identity->task_id == "task:" + identity->task_v4_digest,
			"task-v4 task_id was not derived from task_v4_digest");
	require(identity->task_v4_input_digest == cxxlens::sdk::content_digest(identity->input_payload),
			"task-v4 input digest was not bound to exact payload bytes");
	require(identity->semantic_projection.size() < identity->input_payload.size(),
			"semantic projection unexpectedly includes transport-only payload fields");
	const auto projection =
		std::string{reinterpret_cast<const char*>(identity->semantic_projection.data()),
					identity->semantic_projection.size()};
	const auto payload = std::string{reinterpret_cast<const char*>(identity->input_payload.data()),
									 identity->input_payload.size()};
	require(!projection.contains("\"task_id\"") && !projection.contains("\"task_v4_digest\"") &&
				!projection.contains("\"task_v4_input_digest\""),
			"semantic identity projection contains a recursive transport field");
	require(payload.contains("\"task_id\"") && payload.contains("\"task_v4_digest\"") &&
				!payload.contains("task_v4_input_digest"),
			"input payload does not have the exact recursion-free field boundary");

	const auto base_digest = identity->base_task_v3_digest;
	auto decoded = decode_source_closure_task_v4_input(
		identity->input_payload, input.closure, base_digest, identity->task_v4_input_digest);
	require(decoded.has_value(), "valid task-v4 input payload was rejected");
	require(decoded->identity == *identity,
			"task-v4 decode did not reproduce the independently-derived identity");
	require(validate_source_closure_task_v4_input_digest(identity->input_payload,
														 identity->task_v4_input_digest)
				.has_value(),
			"valid task-v4 input digest was rejected");

	auto reordered = make_source_closure_snapshot({
		file("project://src/include/value.hpp",
			 source_closure_role::header,
			 "#pragma once\ninline int value() { return 42; }\n"),
		file("project://src/main.cpp",
			 source_closure_role::main,
			 "#include \"include/value.hpp\"\nint main() { return value(); }\n"),
	});
	require(reordered.has_value(), "reordered task-v4 closure fixture was rejected");
	auto reordered_identity = derive_source_closure_task_v4_identity(input_fixture(*reordered));
	require(reordered_identity.has_value() && *reordered_identity == *identity,
			"closure transfer order changed task-v4 semantic or input identity");

	auto wrong_input_digest = identity->task_v4_input_digest;
	wrong_input_digest.back() = wrong_input_digest.back() == '0' ? '1' : '0';
	require(
		!validate_source_closure_task_v4_input_digest(identity->input_payload, wrong_input_digest),
		"tampered task-v4 input digest was accepted");

	auto tampered_payload = identity->input_payload;
	const std::string task_digest_marker{"\"task_v4_digest\":\"semantic-v2:sha256:"};
	const auto marker = std::search(
		tampered_payload.begin(),
		tampered_payload.end(),
		std::as_bytes(std::span{task_digest_marker.data(), task_digest_marker.size()}).begin(),
		std::as_bytes(std::span{task_digest_marker.data(), task_digest_marker.size()}).end());
	require(marker != tampered_payload.end(), "task-v4 digest marker was not encoded");
	const auto digest_offset =
		static_cast<std::size_t>(marker - tampered_payload.begin()) + task_digest_marker.size();
	tampered_payload[digest_offset] =
		tampered_payload[digest_offset] == std::byte{'0'} ? std::byte{'1'} : std::byte{'0'};
	expect_failure(
		decode_source_closure_task_v4_input(
			tampered_payload, input.closure, base_digest, identity->task_v4_input_digest),
		"tampered task-v4 digest was accepted");

	auto wrong_base_digest = typed("sha256:", '9');
	expect_failure(decode_source_closure_task_v4_input(identity->input_payload,
													   input.closure,
													   wrong_base_digest,
													   identity->task_v4_input_digest),
				   "wrong inherited base-task digest was accepted");

	auto noncanonical_payload = identity->input_payload;
	noncanonical_payload.insert(noncanonical_payload.begin(), std::byte{' '});
	expect_failure(
		decode_source_closure_task_v4_input(
			noncanonical_payload, input.closure, base_digest, identity->task_v4_input_digest),
		"noncanonical task-v4 input payload was accepted");

	auto different_closure = closure_fixture();
	different_closure.blobs.front().content = content("different bytes\n");
	expect_failure(decode_source_closure_task_v4_input(identity->input_payload,
													   different_closure,
													   base_digest,
													   identity->task_v4_input_digest),
				   "task-v4 payload rebound to a different closure");

	return 0;
}
