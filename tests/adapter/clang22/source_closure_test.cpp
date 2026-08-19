#include "llvm/clang22/source_closure.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
	using cxxlens::detail::clang22::make_source_closure_snapshot;
	using cxxlens::detail::clang22::source_closure_encoding;
	using cxxlens::detail::clang22::source_closure_file_input;
	using cxxlens::detail::clang22::source_closure_role;
	using cxxlens::detail::clang22::source_closure_snapshot;

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

	[[nodiscard]] source_closure_file_input file(std::string logical_path,
												 source_closure_role role,
												 std::shared_ptr<const std::string> bytes)
	{
		return {std::move(logical_path), role, source_closure_encoding::utf8, std::move(bytes)};
	}

	[[nodiscard]] source_closure_snapshot valid_snapshot()
	{
		auto header = content("#pragma once\ninline int answer() { return 42; }\n");
		auto closure = make_source_closure_snapshot({
			file("project://src/main.cpp",
				 source_closure_role::main,
				 content("#include \"include/answer.hpp\"\nint main() { return answer(); }\n")),
			file("project://src/include/answer.hpp", source_closure_role::header, header),
			file("project://generated/answer.hpp", source_closure_role::generated, header),
		});
		require(closure.has_value(), "valid source closure was rejected");
		return std::move(*closure);
	}

	void expect_failure(std::vector<source_closure_file_input> files, const std::string_view code)
	{
		auto result = make_source_closure_snapshot(std::move(files));
		require(!result, "invalid source closure was accepted");
		require(result.error().code == code, "source closure failed with the wrong typed code");
	}
} // namespace

int main()
{
	auto closure = valid_snapshot();
	require(closure.validate().has_value(), "constructed source closure did not revalidate");
	require(closure.snapshot_id == "source-closure:" + closure.closure_digest,
			"source closure snapshot identity was not digest-addressed");
	require(closure.members.size() == 3U, "source closure member census was wrong");
	require(closure.blobs.size() == 2U, "identical immutable content was not deduplicated");
	require(closure.find_member("project://src/main.cpp") != nullptr,
			"main source was not addressable by canonical logical path");
	require(closure.find_member("project://missing.hpp") == nullptr,
			"missing logical path unexpectedly resolved");

	auto mutable_bytes = std::make_shared<std::string>("int main() { return 0; }");
	std::shared_ptr<const std::string> borrowed_mutable = mutable_bytes;
	auto sealed_copy = make_source_closure_snapshot(
		{file("project://src/sealed.cpp", source_closure_role::main, borrowed_mutable)});
	require(sealed_copy.has_value(), "mutable-alias fixture was rejected");
	(*mutable_bytes)[0] = 'x';
	require(sealed_copy->validate().has_value() &&
				sealed_copy->blobs.front().content->front() == 'i',
			"source closure retained a caller-mutable byte alias");

	auto reversed = make_source_closure_snapshot({
		file("project://generated/answer.hpp",
			 source_closure_role::generated,
			 content("#pragma once\ninline int answer() { return 42; }\n")),
		file("project://src/include/answer.hpp",
			 source_closure_role::header,
			 content("#pragma once\ninline int answer() { return 42; }\n")),
		file("project://src/main.cpp",
			 source_closure_role::main,
			 content("#include \"include/answer.hpp\"\nint main() { return answer(); }\n")),
	});
	require(reversed.has_value(), "order-permuted source closure was rejected");
	require(reversed->closure_digest == closure.closure_digest,
			"transfer order changed source closure identity");

	expect_failure(
		{file("project://src/main.cpp", source_closure_role::main, content("int main(){}")),
		 file("project://src/main.cpp", source_closure_role::header, content("header"))},
		"source-closure.duplicate-member");
	expect_failure(
		{file("project://src/main.cpp", source_closure_role::main, content("int main(){}")),
		 file("project://src/Foo.hpp", source_closure_role::header, content("A")),
		 file("project://src/foo.hpp", source_closure_role::header, content("B"))},
		"source-closure.case-collision");
	expect_failure(
		{file("project://src/../main.cpp", source_closure_role::main, content("int main(){}"))},
		"source-closure.path-invalid");
	expect_failure({file("project://src/cafe\xcc\x81.cpp",
						 source_closure_role::main,
						 content("int main(){}"))},
				   "source-closure.path-not-nfc");
	expect_failure({file("project://src/main.cpp", source_closure_role::header, content("header"))},
				   "source-closure.main-invalid");
	expect_failure({file("project://src/a.cpp", source_closure_role::main, content("a")),
					file("project://src/b.cpp", source_closure_role::main, content("b"))},
				   "source-closure.main-invalid");

	auto tampered_blob = valid_snapshot();
	auto tampered_content = *tampered_blob.blobs.front().content;
	tampered_content.front() = tampered_content.front() == 'x' ? 'y' : 'x';
	tampered_blob.blobs.front().content = content(std::move(tampered_content));
	auto tampered_result = tampered_blob.validate();
	require(!tampered_result && tampered_result.error().code == "source-closure.digest-mismatch",
			"blob byte mutation was not rejected");

	auto size_mutation = valid_snapshot();
	++size_mutation.blobs.front().size_bytes;
	auto size_result = size_mutation.validate();
	require(!size_result && size_result.error().code == "source-closure.digest-mismatch",
			"blob size mutation was not classified as a digest mismatch");

	auto missing_blob = valid_snapshot();
	missing_blob.blobs.erase(missing_blob.blobs.begin());
	auto missing_result = missing_blob.validate();
	require(!missing_result && missing_result.error().code == "source-closure.blob-missing",
			"missing content-addressed blob was not rejected");

	auto orphan_blob = valid_snapshot();
	orphan_blob.blobs.push_back({
		"sha256:ca978112ca1bbdcafac231b39a23dc4da786eff8147c4e72b9807785afee48bb",
		1U,
		content("a"),
	});
	std::ranges::sort(
		orphan_blob.blobs, {}, &cxxlens::detail::clang22::source_closure_blob::content_digest);
	auto orphan_result = orphan_blob.validate();
	require(!orphan_result && orphan_result.error().code == "source-closure.digest-mismatch",
			"unreferenced authenticated blob was accepted");

	auto identity_mutation = valid_snapshot();
	identity_mutation.closure_digest.back() =
		identity_mutation.closure_digest.back() == '0' ? '1' : '0';
	auto identity_result = identity_mutation.validate();
	require(!identity_result && identity_result.error().code == "source-closure.digest-mismatch",
			"closure digest mutation was not rejected");

	auto order_mutation = valid_snapshot();
	std::swap(order_mutation.members.front(), order_mutation.members.back());
	auto order_result = order_mutation.validate();
	require(!order_result && order_result.error().code == "source-closure.path-collision",
			"noncanonical member order was not rejected");

	return 0;
}
