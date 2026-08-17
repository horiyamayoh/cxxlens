#include "llvm/clang22/clang_compiler_vfs.hpp"

#include <clang/AST/ASTContext.h>
#include <clang/Basic/SourceManager.h>

#include <cassert>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <memory>
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

	[[nodiscard]] source_closure::blob make_blob(const std::string& value)
	{
		auto content = bytes(value);
		return {source_closure::sha256_digest(content), std::move(content)};
	}

	[[nodiscard]] source_closure::file make_file(std::string path,
		const source_closure::blob& content,
		const source_closure::file_role role)
	{
		const auto provenance_bytes = bytes("clang-vfs-test");
		return {std::move(path), content.content_digest, content.content.size(), role,
			source_closure::sha256_digest(provenance_bytes)};
	}

	[[nodiscard]] std::shared_ptr<const source_closure::validated_snapshot>
	make_snapshot(const bool include_missing)
	{
		const auto include = include_missing ? "ambient-only.hpp" : "shared.hpp";
		auto main = make_blob("#include \"" + std::string{include} +
			"\"\nint main(){return answer();}\n");
		auto shared = make_blob("#include \"config.hpp\"\ninline int answer(){return GENERATED;}\n");
		auto generated = make_blob("#define GENERATED 42\n");
		source_closure::snapshot input;
		input.files = {
			make_file("project://src/main.cpp", main, source_closure::file_role::main_source),
			make_file("project://include/shared.hpp", shared,
				source_closure::file_role::project_header),
			make_file("generated://build/config.hpp", generated,
				source_closure::file_role::generated_header),
		};
		input.blobs = {main, shared, generated};
		auto validated = source_closure::validate(std::move(input));
		assert(validated);
		return std::make_shared<const source_closure::validated_snapshot>(std::move(*validated));
	}
} // namespace

int main()
{
	const std::vector<std::string> arguments{
		"clang++", "-std=c++23", "-Iproject://include", "-Igenerated://build",
		"project://src/main.cpp"};
	bool called{};
	const auto parsed = source_closure::run_with_compiler_vfs(
		make_snapshot(false), arguments, "project://",
		[&called](clang::ASTContext& context, clang::SourceManager&)
			-> std::expected<void, source_closure::clang_run_error>
		{
			called = context.getTranslationUnitDecl() != nullptr;
			return {};
		});
	assert(parsed);
	assert(called);

	{
		std::ofstream ambient{"ambient-only.hpp"};
		ambient << "inline int answer(){return 7;}\n";
	}
	called = false;
	const auto denied = source_closure::run_with_compiler_vfs(
		make_snapshot(true), arguments, "project://",
		[&called](clang::ASTContext&, clang::SourceManager&)
			-> std::expected<void, source_closure::clang_run_error>
		{
			called = true;
			return {};
		});
	std::remove("ambient-only.hpp");
	assert(!denied);
	assert(denied.error().code == "source-closure.clang-parse-failed");
	assert(!called);
	return 0;
}
